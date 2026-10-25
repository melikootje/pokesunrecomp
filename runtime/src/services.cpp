#include "pokerecomp/services.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
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
        case 0x00030040: { // GenHashConsoleUnique(appIdSalt) -> 64-bit value (deterministic, not a real
                           // console)
            uint64_t x = uint64_t(r.cmd[1]) * 0x9E3779B97F4A7C15ull + 0x243F6A8885A308D3ull;
            x ^= x >> 29;
            x *= 0xBF58476D1CE4E5B9ull;
            x ^= x >> 32;
            r.reply(3, 0, kresult::Success);
            r.cmd[2] = uint32_t(x);
            r.cmd[3] = uint32_t(x >> 32);
            break;
        }
        case 0x00040000: // GetRegionCanadaUSA -> 0 (not the Canada/USA special case)
        case 0x00050000: // GetSystemModel -> 0 (Old 3DS)
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = 0;
            break;
        case 0x00060000: // GetModelNintendo2DS -> 1 (not a 2DS)
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = 1;
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

// ---------------------------------------------------------------------------- dsp::DSP (stub until the audio
// HLE, #13)
class DspService : public Service {
  public:
    explicit DspService(Kernel &k) : k_(k) {
        // DSP shared memory (256 KiB) as the application sees it
        k_.memory().map(kDspRam, 0x40000, GuestMemory::Read | GuestMemory::Write);
    }
    const char *name() const override { return "dsp::DSP"; }
    void handle(IpcRequest &r) override {
        switch (r.header()) {
        case 0x001100C2: { // LoadComponent(size, prog mask, data mask, buffer) -> loaded, buffer
            const uint32_t desc = r.cmd[4], ptr = r.cmd[5];
            r.reply(2, 2, kresult::Success);
            r.cmd[2] = 1;
            r.cmd[3] = desc;
            r.cmd[4] = ptr;
            break;
        }
        case 0x00120000: // UnloadComponent
            r.reply(1, 0, kresult::Success);
            break;
        case 0x00150082: // RegisterInterruptEvents(interrupt, channel, event handle)
            events_[(r.cmd[1] << 8) | r.cmd[2]] = r.cmd[4];
            r.reply(1, 0, kresult::Success);
            break;
        case 0x00160000: // GetSemaphoreEventHandle -> event handle
            if (!sem_)
                sem_ = k_.createEvent(false);
            r.reply(1, 2, kresult::Success);
            r.cmd[2] = 0;
            r.cmd[3] = sem_;
            break;
        case 0x00170040: // SetSemaphoreMask(mask)
            r.reply(1, 0, kresult::Success);
            break;
        case 0x00070040: // SetSemaphore(value): the DSP acknowledges immediately
            if (sem_)
                k_.signalEvent(sem_);
            r.reply(1, 0, kresult::Success);
            break;
        case 0x000C0040: // ConvertProcessAddressFromDspDram(dsp word address) -> process address
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = kDspRam + (r.cmd[1] << 1);
            break;
        case 0x000D0082: { // WriteProcessPipe(channel, size, buffer)
            const uint32_t channel = r.cmd[1], size = r.cmd[2], ptr = r.cmd[4];
            if (channel == 2 && size >= 4)
                onAudioPipeWrite(k_.guestPtr(ptr), size);
            r.reply(1, 0, kresult::Success);
            break;
        }
        case 0x000F0080: // GetPipeReadableSize(channel, peer)
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = r.cmd[1] == 2 ? uint32_t(pipe_.size()) : 0;
            break;
        case 0x001000C0:   // ReadPipeIfPossible(channel, peer, size): data goes to the thread's static buffer
        case 0x000E00C0: { // ReadPipe(channel, peer, size)
            const uint32_t channel = r.cmd[1], size = r.cmd[3];
            const uint32_t ptr = r.cmd[65]; // static buffer pointer at TLS + 0x184
            uint32_t n = 0;
            if (channel == 2 && ptr) {
                n = std::min<uint32_t>(size, uint32_t(pipe_.size()));
                std::memcpy(k_.guestPtr(ptr), pipe_.data(), n);
                pipe_.erase(pipe_.begin(), pipe_.begin() + n);
            }
            r.reply(2, 2, kresult::Success);
            r.cmd[2] = n;
            r.cmd[3] = (n << 14) | 2;
            r.cmd[4] = ptr;
            break;
        }
        case 0x00020040: // RecvDataIsReady(register) -> ready
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = 1;
            break;
        case 0x00010040: // RecvData(register) -> value
        case 0x001F0000: // GetHeadphoneStatus -> not connected
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = 0;
            break;
        case 0x00130082: // InvalidateDataCache(addr, size, process handle)
        case 0x00140082: // FlushDataCache(addr, size, process handle)
            r.reply(1, 0, kresult::Success);
            break;
        default:
            if (seen_.insert(r.header()).second)
                std::fprintf(stderr, "[dsp::DSP] stub for command %08x\n", r.header());
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = 0;
            break;
        }
    }

  private:
    static constexpr uint32_t kDspRam = 0x1FF00000;
    // Audio pipe: state-change commands from the application. Initialize answers with the addresses of the
    // shared-memory structures (approximate layout; the real mixer arrives with issue #13) and starts the
    // frame interrupt (every 4.096 ms of virtual time).
    void onAudioPipeWrite(const uint8_t *data, uint32_t size) {
        (void)size;
        const uint16_t cmd = uint16_t(data[0] | data[1] << 8);
        if (cmd == 0) { // Initialize
            const std::vector<uint16_t> resp = {0x000F, 0x8000, 0x8040, 0x8080, 0x80C0, 0x8100,
                                                0x8140, 0x8180, 0x81C0, 0x8200, 0x8240, 0x8280,
                                                0x82C0, 0x8300, 0x8340, 0x8380};
            pipe_.assign(reinterpret_cast<const uint8_t *>(resp.data()),
                         reinterpret_cast<const uint8_t *>(resp.data()) + resp.size() * 2);
            auto it = events_.find((2u << 8) | 2u);
            if (it != events_.end()) {
                k_.signalEvent(it->second);
                if (!framesStarted_) {
                    framesStarted_ = true;
                    k_.scheduleSignal(it->second, 4096000, 4096000);
                }
            }
        }
    }
    Kernel &k_;
    Handle sem_ = 0;
    bool framesStarted_ = false;
    std::map<uint32_t, Handle> events_;
    std::vector<uint8_t> pipe_;
    std::set<uint32_t> seen_;
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

// Peripheral/online services that are not implemented yet: every command answers with `result`, and each
// distinct command is logged once so real needs can be found and implemented properly.
class LoggedStubService : public Service {
  public:
    LoggedStubService(const char *name, uint32_t result) : name_(name), result_(result) {}
    const char *name() const override { return name_; }
    void handle(IpcRequest &r) override {
        if (seen_.insert(r.header()).second) {
            std::fprintf(stderr, "[%s] stub command %08x:", name_, r.header());
            for (unsigned i = 1; i <= r.normalParams() + r.translateParams() && i < 10; i++)
                std::fprintf(stderr, " %08x", r.cmd[i]);
            std::fprintf(stderr, "\n");
        }
        r.reply(1, 0, result_);
    }

  private:
    const char *name_;
    uint32_t result_;
    std::set<uint32_t> seen_;
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
    k.registerService(std::make_unique<DspService>(k));
    k.registerService(std::make_unique<ErrService>(k, opt.logFatalErrors));
    // Online/peripheral services: stubbed (success) until each is implemented for real.
    for (const char *n : {"ac:u",     "act:u",    "am:app", "boss:U",  "cam:u", "cecd:u", "dlp:FKCL",
                          "dlp:SRVR", "frd:u",    "http:C", "ir:USER", "mic:u", "news:u", "nfc:u",
                          "nim:aoc",  "nwm::UDS", "qtm:u",  "soc:U",   "ssl:C", "y2r:u"})
        k.registerService(std::make_unique<LoggedStubService>(n, kresult::Success));
}

} // namespace pokerecomp
