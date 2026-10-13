#include "pokerecomp/services.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

namespace pokerecomp {
namespace {

// ---------------------------------------------------------------------------- APT (applet manager)
class AptService : public Service {
  public:
    AptService(const char *name, Kernel &k) : name_(name), k_(k) {}
    const char *name() const override { return name_; }
    void handle(IpcRequest &r) override {
        switch (r.header()) {
        case 0x00010040: // GetLockHandle(flags) -> attributes, lock mutex
            r.reply(2, 2, kresult::Success);
            r.cmd[2] = 0;
            r.cmd[3] = 0;
            if (!lock_)
                lock_ = k_.createMutex(false);
            r.cmd[4] = lock_;
            break;
        case 0x00020080: // Initialize(appid, attr) -> notification + resume events
            if (!notify_) {
                notify_ = k_.createEvent(false);
                resume_ = k_.createEvent(false);
            }
            r.reply(1, 3, kresult::Success);
            r.cmd[2] = 0x04000000; // two copied handles
            r.cmd[3] = notify_;
            r.cmd[4] = resume_;
            break;
        case 0x00030040: // Enable(attr): the system now delivers the first notification (Wakeup)
            r.reply(1, 0, kresult::Success);
            pending_.push_back(1); // SignalType::Wakeup
            if (notify_)
                k_.scheduleSignal(notify_, 2000000); // arrives ~2 ms later, asynchronously
            break;
        case 0x000B0040: // InquireNotification(appid) -> signal type (0 = none)
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = pending_.empty() ? 0 : pending_.front();
            if (!pending_.empty())
                pending_.erase(pending_.begin());
            break;
        case 0x000D0080: // ReceiveParameter(appid, buffer size)
        case 0x000E0080: // GlanceParameter(appid, buffer size)
            r.reply(4, 4, kresult::Success);
            r.cmd[2] = 0x300;      // sender app id (home menu)
            r.cmd[3] = 1;          // command: Wakeup
            r.cmd[4] = 0;          // parameter size
            r.cmd[5] = 0;          // handle descriptor
            r.cmd[6] = 0;          // no handle
            r.cmd[7] = 0x00000002; // empty static buffer
            r.cmd[8] = 0;
            break;
        case 0x00430040: // NotifyToWait(appid): the app now waits for the resume event
            r.reply(1, 0, kresult::Success);
            if (resume_)
                k_.scheduleSignal(resume_, 2000000);
            break;
        case 0x004B00C2: // AppletUtility(command, in size, out size, in buffer): output stays zero
        case 0x00550040: // SetScreenCapPostPermission(perm)
        case 0x004F0080: // SetApplicationCpuTimeLimit(...)
        case 0x00040040: // Finalize(appid)
            r.reply(1, 0, kresult::Success);
            break;
        default:
            unimpl(r);
            break;
        }
    }

  private:
    void unimpl(IpcRequest &r) {
        if (seen_.insert(r.header()).second)
            std::fprintf(stderr, "[%s] unimplemented command %08x\n", name_, r.header());
        r.reply(1, 0, kresult::InvalidCommand);
    }
    const char *name_;
    Kernel &k_;
    Handle lock_ = 0, notify_ = 0, resume_ = 0;
    std::vector<uint32_t> pending_;
    std::set<uint32_t> seen_;
};

// ---------------------------------------------------------------------------- cfg
class CfgService : public Service {
  public:
    CfgService(const char *name, const ServiceOptions &o, Kernel &k) : name_(name), o_(o), k_(k) {}
    const char *name() const override { return name_; }
    void handle(IpcRequest &r) override {
        switch (r.header()) {
        case 0x00010082: { // GetConfigInfoBlk2(size, blkID, out*)
            const uint32_t size = r.cmd[1], blk = r.cmd[2], out = r.cmd[4];
            std::memset(k_.guestPtr(out), 0, size);
            if (blk == 0xA0002 && size >= 1)
                k_.guestPtr(out)[0] = uint8_t(o_.language); // language
            r.reply(1, 2, kresult::Success);
            r.cmd[2] = (size << 14) | 0x402;
            r.cmd[3] = out;
            break;
        }
        case 0x00020000: // SecureInfoGetRegion -> region
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = o_.region;
            break;
        default:
            r.reply(1, 0, kresult::InvalidCommand);
            std::fprintf(stderr, "[%s] unimplemented command %08x\n", name_, r.header());
            break;
        }
    }

  private:
    const char *name_;
    ServiceOptions o_;
    Kernel &k_;
};

// ---------------------------------------------------------------------------- services that only need
// "success"
class SuccessService : public Service {
  public:
    SuccessService(const char *name, Kernel &k) : name_(name), k_(k) {}
    const char *name() const override { return name_; }
    void handle(IpcRequest &r) override {
        (void)k_;
        r.reply(1, 0, kresult::Success);
    }

  private:
    const char *name_;
    Kernel &k_;
};

// ---------------------------------------------------------------------------- ptm
class PtmService : public Service {
  public:
    explicit PtmService(const char *name) : name_(name) {}
    const char *name() const override { return name_; }
    void handle(IpcRequest &r) override {
        switch (r.header()) {
        case 0x00070000: // GetBatteryLevel -> 5 (full)
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = 5;
            break;
        case 0x00080000: // GetBatteryChargeState -> not charging
        case 0x00050000: // GetAdapterState -> adapter connected
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = r.header() == 0x00050000 ? 1 : 0;
            break;
        default:
            r.reply(1, 0, kresult::InvalidCommand);
            std::fprintf(stderr, "[%s] unimplemented command %08x\n", name_, r.header());
            break;
        }
    }

  private:
    const char *name_;
};

// ---------------------------------------------------------------------------- err:f (fatal error display)
class ErrService : public Service {
  public:
    ErrService(Kernel &k, bool log) : k_(k), log_(log) {}
    const char *name() const override { return "err:f"; }
    void handle(IpcRequest &r) override {
        if (r.header() == 0x00010800) { // Throw(ErrorInfo)
            char msg[200];
            std::snprintf(msg, sizeof msg,
                          "guest raised a fatal error: result %08x at pc %08x (info %08x %08x)", r.cmd[2],
                          r.cmd[3], r.cmd[1], r.cmd[4]);
            if (log_)
                std::fprintf(stderr, "[err:f] %s\n", msg);
            r.reply(1, 0, kresult::Success);
            k_.stopWithReason(0xFE, msg);
            return;
        }
        r.reply(1, 0, kresult::InvalidCommand);
    }

  private:
    Kernel &k_;
    bool log_;
};

} // namespace

void registerDefaultServices(Kernel &k, const ServiceOptions &opt) {
    k.registerService(std::make_unique<AptService>("APT:A", k));
    k.registerService(std::make_unique<AptService>("APT:U", k));
    k.registerService(std::make_unique<AptService>("APT:S", k));
    k.registerService(std::make_unique<CfgService>("cfg:u", opt, k));
    k.registerService(std::make_unique<CfgService>("cfg:i", opt, k));
    k.registerService(std::make_unique<PtmService>("ptm:u"));
    k.registerService(std::make_unique<SuccessService>("ndm:u", k));
    k.registerService(std::make_unique<ErrService>(k, opt.logFatalErrors));
}

} // namespace pokerecomp
