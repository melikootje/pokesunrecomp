#include "pokerecomp/kernel.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <set>

namespace pokerecomp {
namespace {

enum class T { Thread, Mutex, Semaphore, Event, Timer, Arbiter, MemBlock, Session, Process, ResLimit };

struct Obj {
    explicit Obj(T t) : type(t) {}
    virtual ~Obj() = default;
    T type;
};

enum class TState { Ready, Waiting, Dead };
enum class WaitKind { None, Sync, Sleep, Arbiter };

struct Thread : Obj {
    Thread() : Obj(T::Thread) { std::memset(&ctx, 0, sizeof ctx); }
    uint32_t id = 0;
    int prio = 0x30;
    int processor = 0;
    RcCtx ctx;
    TState state = TState::Ready;
    uint64_t readySeq = 0;
    // wait state
    WaitKind waitKind = WaitKind::None;
    std::vector<std::shared_ptr<Obj>> waitObjs;
    bool waitAll = false;
    uint64_t wakeAt = UINT64_MAX;
    uint32_t arbAddr = 0;
    const Obj *arbiter = nullptr;
    uint32_t tls = 0;
};

struct Mutex : Obj {
    Mutex() : Obj(T::Mutex) {}
    Thread *owner = nullptr;
    unsigned count = 0;
};
struct Semaphore : Obj {
    Semaphore() : Obj(T::Semaphore) {}
    int32_t count = 0, max = 0;
};
struct Event : Obj {
    Event() : Obj(T::Event) {}
    int kind = 0; // 0 oneshot, 1 sticky, 2 pulse
    bool signaled = false;
};
struct Timer : Obj {
    Timer() : Obj(T::Timer) {}
    int kind = 0;
    bool signaled = false, active = false;
    uint64_t fireAt = 0, interval = 0;
};
struct Arbiter : Obj {
    Arbiter() : Obj(T::Arbiter) {}
};
struct MemBlock : Obj {
    MemBlock() : Obj(T::MemBlock) {}
    uint32_t addr = 0, size = 0;
    bool mapped = false;
};
struct Session : Obj {
    Session() : Obj(T::Session) {}
    Service *service = nullptr;
};
struct Process : Obj {
    Process() : Obj(T::Process) {}
};
struct ResLimit : Obj {
    ResLimit() : Obj(T::ResLimit) {}
};

uint64_t nsToTicks(uint64_t ns) { return uint64_t((unsigned __int128)ns * kTicksPerSecond / 1000000000ull); }

constexpr uint32_t kSliceSteps = 4000;
constexpr size_t kTraceLimit = 200000;
constexpr uint32_t kHeapBase = 0x08000000, kHeapEnd = 0x10000000, kLinearBase = 0x14000000,
                   kLinearEnd = 0x1C000000;

class UnimplementedService : public Service {
  public:
    UnimplementedService(std::string n, UnimplementedPolicy *p,
                         std::set<std::pair<std::string, uint32_t>> *seen,
                         std::function<void(const std::string &)> *log)
        : name_(std::move(n)), policy_(p), seen_(seen), log_(log) {}
    const char *name() const override { return name_.c_str(); }
    void handle(IpcRequest &r) override {
        if (seen_->insert({name_, r.header()}).second && *log_) {
            std::string words;
            const unsigned n = std::min(1u + r.normalParams() + r.translateParams(), 40u);
            char b[16];
            for (unsigned k = 0; k < n; k++) {
                std::snprintf(b, sizeof b, " %08x", r.cmd[k]);
                words += b;
            }
            (*log_)("unimplemented IPC: service '" + name_ + "' request:" + words);
        }
        r.reply(1, 0,
                *policy_ == UnimplementedPolicy::ReturnSuccess ? kresult::Success : kresult::InvalidCommand);
    }

  private:
    std::string name_;
    UnimplementedPolicy *policy_;
    std::set<std::pair<std::string, uint32_t>> *seen_;
    std::function<void(const std::string &)> *log_;
};

} // namespace

struct Kernel::Impl {
    Kernel &k;
    std::map<Handle, std::shared_ptr<Obj>> handles;
    Handle nextHandle = 0x100;
    std::vector<std::shared_ptr<Thread>> threads;
    Thread *current = nullptr;
    uint32_t nextThreadId = 1;
    uint64_t readyCounter = 0;
    std::map<std::string, std::unique_ptr<Service>> services;
    std::vector<std::unique_ptr<Service>> adhoc;
    std::vector<std::unique_ptr<Service>> owned; // services created through createSession
    std::set<std::pair<std::string, uint32_t>> seenUnimpl;
    std::shared_ptr<Process> process = std::make_shared<Process>();
    std::shared_ptr<ResLimit> resLimit = std::make_shared<ResLimit>();
    bool processExited = false, breakHit = false;
    // memory allocator state: start -> size
    std::map<uint32_t, uint32_t> allocs;
    uint32_t tlsMapped = 0;
    KernelConfig cfg;
    struct Delayed {
        uint64_t fire;
        Handle handle;
        uint64_t period;
        std::function<void()> cb;
    };
    std::vector<Delayed> delayedSignals;

    explicit Impl(Kernel &kk) : k(kk) {}
    bool handleSvc(Thread *t); // runs the pending SVC; false when the thread blocked/exited

    // ------------------------------------------------------------------ helpers
    void trace(const std::string &s) {
        if (k.trace_.size() < kTraceLimit)
            k.trace_.push_back(s);
    }
    std::string tname(const Thread *t) const { return "T" + std::to_string(t ? t->id : 0); }

    Handle add(std::shared_ptr<Obj> o) {
        const Handle h = nextHandle;
        nextHandle += 4;
        handles[h] = std::move(o);
        return h;
    }
    std::shared_ptr<Obj> get(Handle h) {
        if (h == kCurrentThread) {
            for (auto &t : threads)
                if (t.get() == current)
                    return t;
        }
        if (h == kCurrentProcess)
            return process;
        auto it = handles.find(h);
        return it == handles.end() ? nullptr : it->second;
    }
    uint8_t *mem() { return k.gm_.base(); }
    uint32_t rd32(uint32_t a) {
        uint32_t v;
        std::memcpy(&v, mem() + a, 4);
        return v;
    }
    void wr32(uint32_t a, uint32_t v) { std::memcpy(mem() + a, &v, 4); }
    void wr64(uint32_t a, uint64_t v) { std::memcpy(mem() + a, &v, 8); }

    static void setRes(Thread *t, uint32_t r0) { t->ctx.r[0] = r0; }
    static void setRes(Thread *t, uint32_t r0, uint32_t r1) {
        t->ctx.r[0] = r0;
        t->ctx.r[1] = r1;
    }

    // ------------------------------------------------------------------ synchronisation
    bool isSignaled(Obj &o, Thread *t) {
        switch (o.type) {
        case T::Thread:
            return static_cast<Thread &>(o).state == TState::Dead;
        case T::Mutex: {
            auto &m = static_cast<Mutex &>(o);
            return !m.owner || m.owner == t;
        }
        case T::Semaphore:
            return static_cast<Semaphore &>(o).count > 0;
        case T::Event:
            return static_cast<Event &>(o).signaled;
        case T::Timer:
            return static_cast<Timer &>(o).signaled;
        default:
            return false;
        }
    }
    void acquire(Obj &o, Thread *t) {
        switch (o.type) {
        case T::Mutex: {
            auto &m = static_cast<Mutex &>(o);
            m.owner = t;
            m.count++;
            break;
        }
        case T::Semaphore:
            static_cast<Semaphore &>(o).count--;
            break;
        case T::Event: {
            auto &e = static_cast<Event &>(o);
            if (e.kind == 0)
                e.signaled = false;
            break;
        }
        case T::Timer: {
            auto &tm = static_cast<Timer &>(o);
            if (tm.kind == 0)
                tm.signaled = false;
            break;
        }
        default:
            break;
        }
    }

    // Tries to satisfy a Sync wait; returns true and sets result registers when done.
    bool trySatisfy(Thread *t, int *index) {
        if (t->waitAll) {
            for (auto &o : t->waitObjs)
                if (!isSignaled(*o, t))
                    return false;
            for (auto &o : t->waitObjs)
                acquire(*o, t);
            *index = 0;
            return true;
        }
        for (size_t i = 0; i < t->waitObjs.size(); i++) {
            if (isSignaled(*t->waitObjs[i], t)) {
                acquire(*t->waitObjs[i], t);
                *index = int(i);
                return true;
            }
        }
        return false;
    }

    void makeReady(Thread *t) {
        t->state = TState::Ready;
        t->waitKind = WaitKind::None;
        t->waitObjs.clear();
        t->wakeAt = UINT64_MAX;
        t->arbiter = nullptr;
        t->readySeq = ++readyCounter;
    }

    // Wakes every waiter that can now proceed (highest priority first); repeated until stable.
    void wakeAll() {
        for (bool changed = true; changed;) {
            changed = false;
            std::vector<Thread *> waiting;
            for (auto &t : threads)
                if (t->state == TState::Waiting)
                    waiting.push_back(t.get());
            std::stable_sort(waiting.begin(), waiting.end(),
                             [](Thread *a, Thread *b) { return a->prio < b->prio; });
            for (Thread *t : waiting) {
                if (t->state != TState::Waiting)
                    continue;
                if (t->waitKind == WaitKind::Sync) {
                    int idx = 0;
                    if (trySatisfy(t, &idx)) {
                        setRes(t, kresult::Success, uint32_t(idx));
                        makeReady(t);
                        changed = true;
                    } else if (t->wakeAt <= k.now_) {
                        setRes(t, kresult::Timeout, 0xFFFFFFFFu);
                        makeReady(t);
                        changed = true;
                    }
                } else if (t->wakeAt <= k.now_) {
                    setRes(t, t->waitKind == WaitKind::Sleep ? kresult::Success : kresult::Timeout);
                    makeReady(t);
                    changed = true;
                }
            }
        }
    }

    void processTimers() {
        for (size_t i = 0; i < delayedSignals.size();) {
            if (delayedSignals[i].fire <= k.now_) {
                if (delayedSignals[i].cb) {
                    auto cb = delayedSignals[i].cb; // the callback may schedule more work
                    cb();
                }
                auto o = get(delayedSignals[i].handle);
                if (o && o->type == T::Event)
                    static_cast<Event &>(*o).signaled = true;
                if (delayedSignals[i].period) { // periodic: skip missed periods instead of firing a burst
                    Delayed &d = delayedSignals[i];
                    d.fire += d.period * (1 + (k.now_ - d.fire) / d.period);
                    i++;
                } else {
                    delayedSignals.erase(delayedSignals.begin() + long(i));
                }
            } else {
                i++;
            }
        }
        for (auto &kv : handles) {
            if (kv.second->type != T::Timer)
                continue;
            auto &tm = static_cast<Timer &>(*kv.second);
            while (tm.active && tm.fireAt <= k.now_) {
                tm.signaled = true;
                if (tm.interval)
                    tm.fireAt += tm.interval;
                else
                    tm.active = false;
            }
        }
    }

    uint64_t earliestEvent() {
        uint64_t best = UINT64_MAX;
        for (auto &d : delayedSignals)
            best = std::min(best, d.fire);
        for (auto &t : threads)
            if (t->state == TState::Waiting)
                best = std::min(best, t->wakeAt);
        for (auto &kv : handles)
            if (kv.second->type == T::Timer) {
                auto &tm = static_cast<Timer &>(*kv.second);
                if (tm.active)
                    best = std::min(best, tm.fireAt);
            }
        return best;
    }

    Thread *pickNext() {
        Thread *best = nullptr;
        for (auto &t : threads) {
            if (t->state != TState::Ready)
                continue;
            if (!best || t->prio < best->prio || (t->prio == best->prio && t->readySeq < best->readySeq))
                best = t.get();
        }
        return best;
    }

    void killThread(Thread *t) {
        t->state = TState::Dead;
        t->waitObjs.clear();
        for (auto &kv : handles) { // owned mutexes are released when the owner dies
            if (kv.second->type == T::Mutex) {
                auto &m = static_cast<Mutex &>(*kv.second);
                if (m.owner == t) {
                    m.owner = nullptr;
                    m.count = 0;
                }
            }
        }
        trace("exit " + tname(t));
    }

    // ------------------------------------------------------------------ memory
    // The guest allocates in 4 KiB pages. The host may use bigger ones (16 KiB on Apple silicon), so all
    // bookkeeping here is per guest page and only mapping/unmapping widens to whole host pages.
    static constexpr uint32_t kGuestPage = 0x1000;
    static uint32_t pageUp(uint32_t v) { return (v + kGuestPage - 1) & ~(kGuestPage - 1); }
    bool mapGuest(uint32_t addr, uint32_t size) {
        const uint64_t hp = GuestMemory::hostPageSize();
        const uint64_t lo = addr & ~(hp - 1),
                       hi = std::min<uint64_t>((uint64_t(addr) + size + hp - 1) & ~(hp - 1), 1ull << 32);
        return k.gm_.map(uint32_t(lo), uint32_t(hi - lo), GuestMemory::Read | GuestMemory::Write);
    }
    // Gives back [addr, addr + size) (already removed from `allocs`): the bytes read as zero again
    // afterwards. Host pages that still back another allocation stay mapped and are only cleared.
    void releaseGuest(uint32_t addr, uint32_t size) {
        const uint64_t hp = GuestMemory::hostPageSize(), end = uint64_t(addr) + size;
        const uint64_t lo = addr & ~(hp - 1), hi = std::min<uint64_t>((end + hp - 1) & ~(hp - 1), 1ull << 32);
        uint64_t runStart = 0, runLen = 0;
        auto flush = [&] {
            if (runLen)
                k.gm_.unmap(uint32_t(runStart), uint32_t(runLen));
            runLen = 0;
        };
        for (uint64_t p = lo; p < hi; p += hp) {
            auto it = allocs.lower_bound(uint32_t(std::min<uint64_t>(p + hp, 0xFFFFFFFFull)));
            bool used = false;
            if (it != allocs.begin()) {
                --it;
                used = uint64_t(it->first) + it->second > p && it->first < p + hp;
            }
            if (used) {
                flush();
                const uint64_t a = std::max<uint64_t>(p, addr), b = std::min<uint64_t>(p + hp, end);
                std::memset(k.gm_.base() + a, 0, size_t(b - a));
            } else {
                if (!runLen)
                    runStart = p;
                runLen += hp;
            }
        }
        flush();
    }
    bool overlaps(uint32_t a, uint32_t size) {
        for (auto &kv : allocs)
            if (uint64_t(a) < uint64_t(kv.first) + kv.second && uint64_t(kv.first) < uint64_t(a) + size)
                return true;
        return false;
    }
    uint32_t findFree(uint32_t lo, uint32_t hi, uint32_t size) {
        uint32_t a = lo;
        for (;;) {
            if (uint64_t(a) + size > hi)
                return 0;
            bool hit = false;
            for (auto &kv : allocs) {
                if (uint64_t(a) < uint64_t(kv.first) + kv.second && uint64_t(kv.first) < uint64_t(a) + size) {
                    a = pageUp(kv.first + kv.second);
                    hit = true;
                    break;
                }
            }
            if (!hit)
                return a;
        }
    }
    uint32_t commit(uint32_t addr, uint32_t size, bool linear, uint32_t *out) {
        size = pageUp(size);
        if (size == 0)
            return kresult::InvalidCombination;
        if (addr == 0)
            addr = findFree(linear ? kLinearBase : kHeapBase, linear ? kLinearEnd : kHeapEnd, size);
        else
            addr &= ~(kGuestPage - 1);
        if (addr == 0 || overlaps(addr, size))
            return kresult::InvalidAddress;
        if (!mapGuest(addr, size))
            return kresult::OutOfMemory;
        allocs[addr] = size;
        *out = addr;
        return kresult::Success;
    }

    // Maps the host pages that back the TLS slots up to kTlsBase + upTo. The guest's 4 KiB page granularity
    // can be finer than the host's (16 KiB on Apple silicon), so the range is widened to whole host pages.
    void ensureTls(uint32_t upTo) {
        const uint32_t pg = uint32_t(GuestMemory::hostPageSize());
        const uint32_t end = (kTlsBase + upTo + pg - 1) & ~(pg - 1);
        if (end <= tlsMapped)
            return;
        const uint32_t start = tlsMapped ? tlsMapped : (kTlsBase & ~(pg - 1));
        k.gm_.map(start, end - start, GuestMemory::Read | GuestMemory::Write);
        tlsMapped = end;
    }

    // ------------------------------------------------------------------ threads
    Thread *spawn(uint32_t entry, uint32_t stackTop, uint32_t arg, int prio, int processor) {
        auto t = std::make_shared<Thread>();
        t->id = nextThreadId++;
        t->prio = prio;
        t->processor = processor;
        t->tls = kTlsBase + (t->id - 1) * kTlsSize;
        ensureTls(t->tls + kTlsSize - kTlsBase);
        t->ctx.mem = mem();
        t->ctx.tls = t->tls;
        t->ctx.r[0] = arg;
        t->ctx.r[13] = stackTop;
        t->ctx.r[14] = kThreadExitAddress;
        t->ctx.r[15] = entry & ~1u;
        t->ctx.thumb = entry & 1;
        t->state = TState::Ready;
        t->readySeq = ++readyCounter;
        threads.push_back(t);
        {
            char eb[48];
            std::snprintf(eb, sizeof eb, " entry %08x arg %08x", entry, arg);
            trace("create " + tname(t.get()) + " prio " + std::to_string(prio) + eb);
        }
        return t.get();
    }

    // ------------------------------------------------------------------ IPC
    Service *serviceFor(const std::string &name) {
        auto it = services.find(name);
        if (it != services.end())
            return it->second.get();
        adhoc.push_back(std::make_unique<UnimplementedService>(name, &k.policy_, &seenUnimpl, &k.log_));
        services[name] = std::move(adhoc.back());
        adhoc.clear();
        return services[name].get();
    }

    Handle newSession(Service *s) {
        auto ses = std::make_shared<Session>();
        ses->service = s;
        return add(ses);
    }
};

// ---------------------------------------------------------------------------- built-in srv:
namespace {
class SrvService : public Service {
  public:
    explicit SrvService(Kernel::Impl *impl) : impl_(impl) {}
    const char *name() const override { return "srv:"; }
    void handle(IpcRequest &r) override {
        switch (r.header()) {
        case 0x00010002: // RegisterClient(process id)
            r.reply(1, 0, kresult::Success);
            break;
        case 0x00020000: { // EnableNotification -> semaphore handle
            if (!notif_) {
                auto s = std::make_shared<Semaphore>();
                s->count = 0;
                s->max = 1;
                notif_ = impl_->add(s);
            }
            r.reply(1, 2, kresult::Success);
            r.cmd[2] = 0;
            r.cmd[3] = notif_;
            break;
        }
        case 0x00050100: { // GetServiceHandle(name[8], len, flags)
            char name[9] = {};
            std::memcpy(name, &r.cmd[1], 8);
            const uint32_t len = r.cmd[3];
            std::string n(name, std::min<size_t>(len, 8));
            std::string shown = n;
            for (char &ch : shown)
                if (ch < 0x20 || ch > 0x7E)
                    ch = '.'; // keep logs printable
            const size_t nul = n.find('\0');
            if (nul != std::string::npos)
                n.resize(nul);
            Service *s = impl_->serviceFor(n);
            char raw[96];
            std::snprintf(raw, sizeof raw, " (len=%u words %08x %08x %08x %08x)", len, r.cmd[1], r.cmd[2],
                          r.cmd[3], r.cmd[4]);
            impl_->trace("srv GetServiceHandle '" + shown + "'" + raw);
            r.reply(1, 2, kresult::Success);
            r.cmd[2] = 0;
            r.cmd[3] = impl_->newSession(s);
            break;
        }
        case 0x000800C2: // Subscribe
        case 0x00090040: // Unsubscribe
            r.reply(1, 0, kresult::Success);
            break;
        case 0x000B0000: // ReceiveNotification
            r.reply(2, 0, kresult::Success);
            r.cmd[2] = 0;
            break;
        case 0x000C0080: // PublishToSubscriber
            r.reply(1, 0, kresult::Success);
            break;
        default:
            r.reply(1, 0, kresult::InvalidCommand);
            break;
        }
    }

  private:
    Kernel::Impl *impl_;
    Handle notif_ = 0;
};
} // namespace

// ---------------------------------------------------------------------------- Kernel
Kernel::Kernel(GuestMemory &gm, Dispatcher &disp) : gm_(gm), disp_(disp), impl_(new Impl(*this)) {
    disp_.setHaltAddress(kThreadExitAddress);
    impl_->services["srv:"] = std::make_unique<SrvService>(impl_.get());
}

Kernel::~Kernel() = default;

void Kernel::registerService(std::unique_ptr<Service> svc) {
    const std::string n = svc->name();
    impl_->services[n] = std::move(svc);
}

uint32_t Kernel::appMemorySize(uint32_t mode) {
    switch (mode) {
    case 0:
        return 0x04000000; // 64 MB
    case 1:
        return 0x04800000; // 72 MB
    case 2:
        return 0x05000000; // 80 MB
    case 3:
        return 0x06000000; // 96 MB
    case 6:
        return 0x07C00000; // 124 MB (New 3DS)
    case 7:
        return 0x0B200000; // 178 MB (New 3DS)
    default:
        return 0x04000000;
    }
}

void Kernel::configure(const KernelConfig &cfg) {
    impl_->cfg = cfg;
    const uint32_t pg = uint32_t(GuestMemory::hostPageSize());
    // The configuration + shared pages share a host page with the TLS slots.
    impl_->ensureTls(0);
    gm_.map(kConfigPage & ~(pg - 1), pg, GuestMemory::Read | GuestMemory::Write);
    uint8_t *page = gm_.base() + kConfigPage;
    auto w32 = [&](uint32_t off, uint32_t v) { std::memcpy(page + off, &v, 4); };
    page[0] = cfg.kernelRevision;
    page[1] = cfg.kernelMinor;
    page[2] = cfg.kernelMajor;
    w32(0x14, 1);                             // ENVINFO: retail
    w32(0x1C, cfg.ctrSdkVersion);             // KERNEL_CTRSDKVERSION
    w32(0x20, cfg.appMemType);                // APPMEMTYPE
    w32(0x40, appMemorySize(cfg.appMemType)); // APPMEMALLOC
    w32(0x44, 0x02C00000);                    // SYSMEMALLOC (approximate)
    w32(0x48, 0x01400000);                    // BASEMEMALLOC (approximate)
    // Shared page (0x1FF81000): a counter selects one of two alternating clock records (0x20 and 0x40)
    // holding {ms since 1900, system tick of that moment, ticks per second, tick offset} (layout as read by
    // the game's clock code); 0x80 is the 3D slider (float), 0x84 the 3D LED.
    {
        uint64_t unixMs = cfg.startTimeMs;
        if (!unixMs) {
            const auto now = std::chrono::system_clock::now();
            unixMs = uint64_t(
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
            const std::time_t tt = std::time_t(unixMs / 1000);
            std::tm lt{};
            localtime_r(&tt, &lt);
            unixMs += uint64_t(int64_t(lt.tm_gmtoff) * 1000);
        }
        uint8_t *shared = gm_.base() + 0x1FF81000;
        auto w64s = [&](uint32_t off, uint64_t v) { std::memcpy(shared + off, &v, 8); };
        const uint32_t zero = 0;
        std::memcpy(shared, &zero, 4); // counter
        for (uint32_t rec = 0x20; rec <= 0x40; rec += 0x20) {
            w64s(rec + 0x00, unixMs + 2208988800000ull);
            w64s(rec + 0x08, 0);
            w64s(rec + 0x10, kTicksPerSecond);
            w64s(rec + 0x18, 0);
        }
        const float slider = 0.0f;
        std::memcpy(shared + 0x80, &slider, 4);
    }
    w32(0x60, uint32_t(cfg.kernelRevision) | uint32_t(cfg.kernelMinor) << 8 |
                  uint32_t(cfg.kernelMajor) << 16); // FIRM_VERSIONREVISION
}

Handle Kernel::createMainThread(uint32_t entry, uint32_t stackTop, uint32_t arg, int priority) {
    Thread *t = impl_->spawn(entry, stackTop, arg, priority, 0);
    for (auto &th : impl_->threads)
        if (th.get() == t)
            return impl_->add(th);
    return 0;
}

Handle Kernel::createEvent(bool sticky) {
    auto e = std::make_shared<Event>();
    e->kind = sticky ? 1 : 0;
    return impl_->add(e);
}

Handle Kernel::createMemoryBlock(uint32_t size) {
    auto b = std::make_shared<MemBlock>();
    b->size = Kernel::Impl::pageUp(size);
    return impl_->add(b);
}

uint32_t Kernel::memoryBlockAddress(Handle h) const {
    auto o = impl_->get(h);
    if (!o || o->type != T::MemBlock)
        return 0;
    const auto &b = static_cast<const MemBlock &>(*o);
    return b.mapped ? b.addr : 0;
}

Service *Kernel::sessionService(Handle h) {
    auto o = impl_->get(h);
    return o && o->type == T::Session ? static_cast<Session &>(*o).service : nullptr;
}

Handle Kernel::createSession(std::unique_ptr<Service> svc) {
    Service *raw = svc.get();
    impl_->owned.push_back(std::move(svc));
    return impl_->newSession(raw);
}

Handle Kernel::createMutex(bool locked) {
    auto m = std::make_shared<Mutex>();
    (void)locked; // services hand out unlocked mutexes; ownership starts with the first guest acquirer
    return impl_->add(m);
}

Handle Kernel::createSemaphore(int32_t initial, int32_t max) {
    auto sm = std::make_shared<Semaphore>();
    sm->count = initial;
    sm->max = max;
    return impl_->add(sm);
}

void Kernel::stopWithReason(uint32_t reason, const std::string &message) {
    breakReason_ = reason;
    stopMessage_ = message;
    impl_->breakHit = true;
}

void Kernel::scheduleSignal(Handle h, uint64_t delayNs, uint64_t periodNs) {
    impl_->delayedSignals.push_back({now_ + nsToTicks(delayNs), h, nsToTicks(periodNs), nullptr});
}

void Kernel::scheduleCallback(uint64_t delayNs, uint64_t periodNs, std::function<void()> cb) {
    impl_->delayedSignals.push_back({now_ + nsToTicks(delayNs), 0, nsToTicks(periodNs), std::move(cb)});
}

void Kernel::signalEvent(Handle h) {
    auto o = impl_->get(h);
    if (o && o->type == T::Event)
        static_cast<Event &>(*o).signaled = true;
    impl_->wakeAll();
}

uint32_t Kernel::currentThreadId() const { return impl_->current ? impl_->current->id : 0; }

std::vector<std::string> Kernel::describeThreads() const {
    static const char *typeNames[] = {"thread",  "mutex",    "semaphore", "event",   "timer",
                                      "arbiter", "memblock", "session",   "process", "reslimit"};
    std::vector<std::string> out;
    for (auto &t : impl_->threads) {
        if (t->state == TState::Dead)
            continue;
        std::string line = "T" + std::to_string(t->id) + " prio " + std::to_string(t->prio) + " ";
        if (t->state == TState::Ready)
            line += "ready";
        else {
            line += t->waitKind == WaitKind::Sleep     ? "sleeping"
                    : t->waitKind == WaitKind::Arbiter ? "arbitrating 0x"
                                                       : "waiting on";
            if (t->waitKind == WaitKind::Arbiter) {
                char b[16];
                std::snprintf(b, sizeof b, "%08x", t->arbAddr);
                line += b;
            }
            for (auto &o : t->waitObjs) {
                for (auto &kv : impl_->handles)
                    if (kv.second == o) {
                        char b[48];
                        std::snprintf(b, sizeof b, " %s#%x", typeNames[int(o->type)], kv.first);
                        line += b;
                    }
            }
            if (t->wakeAt != UINT64_MAX)
                line += " (timeout)";
        }
        out.push_back(line);
    }
    return out;
}

const RcCtx *Kernel::threadContext(uint32_t id) const {
    for (auto &t : impl_->threads)
        if (t->id == id)
            return &t->ctx;
    return nullptr;
}

std::vector<std::pair<std::string, uint32_t>> Kernel::unimplementedCommands() const {
    return std::vector<std::pair<std::string, uint32_t>>(impl_->seenUnimpl.begin(), impl_->seenUnimpl.end());
}

// ---------------------------------------------------------------------------- SVC dispatch
static uint64_t u64(uint32_t lo, uint32_t hi) { return (uint64_t(hi) << 32) | lo; }

// Returns true when the calling thread should keep running (false: it blocked/exited).
bool Kernel::Impl::handleSvc(Thread *t) {
    Impl &I = *this;
    Kernel &K = k;
    uint64_t &now = k.now_;
    uint32_t (&r)[16] = t->ctx.r;
    const uint32_t n = t->ctx.svc;
    t->ctx.svc_pending = 0;
    char tb[96];
    std::snprintf(tb, sizeof tb, "svc %#04x %s", n, I.tname(t).c_str());
    I.trace(tb);

    auto fail = [&](uint32_t code) {
        r[0] = code;
        return true;
    };
    auto ts = [&](uint32_t lo,
                  uint32_t hi) -> uint64_t { // timeout ns -> absolute wake tick (UINT64_MAX = forever)
        const int64_t ns = int64_t(u64(lo, hi));
        if (ns < 0)
            return UINT64_MAX;
        return now + nsToTicks(uint64_t(ns));
    };

    switch (n) {
    case 0x01: { // ControlMemory(op, addr0, addr1, size, perm) -> r1 addr
        const uint32_t op = r[0] & 0xFF;
        const bool linear = (r[0] & 0x10000) != 0;
        uint32_t out = r[1];
        uint32_t res = kresult::Success;
        if (op == 3) {
            res = I.commit(r[1], r[3], linear, &out);
        } else if (op == 1) {
            const uint32_t size = Kernel::Impl::pageUp(r[3]);
            auto it = I.allocs.find(r[1] & ~(Kernel::Impl::kGuestPage - 1));
            if (it != I.allocs.end()) {
                const uint32_t a = it->first, n = it->second;
                I.allocs.erase(it);
                I.releaseGuest(a, n);
            } else {
                (void)size;
                res = kresult::InvalidAddress;
            }
        } else if (op == 6) {
            res = kresult::Success; // permissions are not enforced (host page granularity differs from the
                                    // guest's)
        } else {
            if (K.log_) {
                char b[96];
                std::snprintf(b, sizeof b, "ControlMemory op %u not implemented", op);
                K.log_(b);
            }
            res = kresult::InvalidCombination;
        }
        r[0] = res;
        r[1] = out;
        return true;
    }
    case 0x02: { // QueryMemory(meminfo*, pageinfo*, addr)
        uint32_t base = r[2] & ~0xFFFu, size = 0x1000, perm = 0, state = 0;
        for (auto &kv : I.allocs)
            if (r[2] >= kv.first && r[2] < kv.first + kv.second) {
                base = kv.first;
                size = kv.second;
                perm = 3;
                state = 5;
            }
        I.wr32(r[0], base);
        I.wr32(r[0] + 4, size);
        I.wr32(r[0] + 8, perm);
        I.wr32(r[0] + 12, state);
        I.wr32(r[1], 0);
        r[0] = kresult::Success;
        r[1] = 0;
        return true;
    }
    case 0x03: // ExitProcess
        I.processExited = true;
        return false;
    case 0x08: { // CreateThread(prio, entry, arg, stacktop, processor) -> r1
        if (int32_t(r[0]) < 0 || r[0] > 0x3F)
            return fail(0xE0E01BFD);
        Thread *nt = I.spawn(r[1], r[3], r[2], int(r[0]), int(r[4]));
        for (auto &th : I.threads)
            if (th.get() == nt) {
                r[1] = I.add(th);
                break;
            }
        r[0] = kresult::Success;
        return true;
    }
    case 0x09: // ExitThread
        I.killThread(t);
        I.wakeAll();
        return false;
    case 0x0A: { // SleepThread(ns)
        const int64_t ns = int64_t(u64(r[0], r[1]));
        if (ns <= 0) { // yield
            t->readySeq = ++I.readyCounter;
            return false;
        }
        t->state = TState::Waiting;
        t->waitKind = WaitKind::Sleep;
        t->wakeAt = now + nsToTicks(uint64_t(ns));
        return false;
    }
    case 0x0B:
    case 0x0C: { // Get/SetThreadPriority
        auto o = I.get(n == 0x0B ? r[1] : r[0]);
        if (!o || o->type != T::Thread)
            return fail(kresult::InvalidHandle);
        auto &th = static_cast<Thread &>(*o);
        if (n == 0x0B) {
            r[1] = uint32_t(th.prio);
            r[0] = kresult::Success;
        } else {
            th.prio = int(r[1]);
            r[0] = kresult::Success;
        }
        return true;
    }
    case 0x13: { // CreateMutex(initially_locked in r1) -> r1
        auto m = std::make_shared<Mutex>();
        if (r[1] & 1) {
            m->owner = t;
            m->count = 1;
        }
        r[1] = I.add(m);
        r[0] = kresult::Success;
        return true;
    }
    case 0x14: { // ReleaseMutex(handle)
        auto o = I.get(r[0]);
        if (!o || o->type != T::Mutex)
            return fail(kresult::InvalidHandle);
        auto &m = static_cast<Mutex &>(*o);
        if (m.owner != t)
            return fail(kresult::NotLocked);
        if (--m.count == 0)
            m.owner = nullptr;
        r[0] = kresult::Success;
        I.wakeAll();
        return true;
    }
    case 0x15: { // CreateSemaphore(initial r1, max r2) -> r1
        auto s = std::make_shared<Semaphore>();
        s->count = int32_t(r[1]);
        s->max = int32_t(r[2]);
        r[1] = I.add(s);
        r[0] = kresult::Success;
        return true;
    }
    case 0x16: { // ReleaseSemaphore(handle r1, count r2) -> r1 previous
        auto o = I.get(r[1]);
        if (!o || o->type != T::Semaphore)
            return fail(kresult::InvalidHandle);
        auto &s = static_cast<Semaphore &>(*o);
        if (int64_t(s.count) + int32_t(r[2]) > s.max)
            return fail(kresult::MaxCount);
        r[1] = uint32_t(s.count);
        s.count += int32_t(r[2]);
        r[0] = kresult::Success;
        I.wakeAll();
        return true;
    }
    case 0x17: { // CreateEvent(reset_type r1) -> r1
        auto e = std::make_shared<Event>();
        e->kind = int(r[1]);
        r[1] = I.add(e);
        r[0] = kresult::Success;
        return true;
    }
    case 0x18:
    case 0x19: { // SignalEvent / ClearEvent
        auto o = I.get(r[0]);
        if (!o || o->type != T::Event)
            return fail(kresult::InvalidHandle);
        auto &e = static_cast<Event &>(*o);
        if (n == 0x18) {
            e.signaled = true;
            I.wakeAll();
            if (e.kind == 2)
                e.signaled = false; // pulse: wake current waiters only
        } else {
            e.signaled = false;
        }
        r[0] = kresult::Success;
        return true;
    }
    case 0x1A: { // CreateTimer(reset_type r1) -> r1
        auto tm = std::make_shared<Timer>();
        tm->kind = int(r[1]);
        r[1] = I.add(tm);
        r[0] = kresult::Success;
        return true;
    }
    case 0x1B: { // SetTimer(handle, initial r1:r2, interval r3:r4)
        auto o = I.get(r[0]);
        if (!o || o->type != T::Timer)
            return fail(kresult::InvalidHandle);
        auto &tm = static_cast<Timer &>(*o);
        tm.fireAt = now + nsToTicks(u64(r[1], r[2]));
        tm.interval = nsToTicks(u64(r[3], r[4]));
        tm.active = true;
        tm.signaled = false;
        r[0] = kresult::Success;
        return true;
    }
    case 0x1C:
    case 0x1D: { // CancelTimer / ClearTimer
        auto o = I.get(r[0]);
        if (!o || o->type != T::Timer)
            return fail(kresult::InvalidHandle);
        auto &tm = static_cast<Timer &>(*o);
        if (n == 0x1C)
            tm.active = false;
        else
            tm.signaled = false;
        r[0] = kresult::Success;
        return true;
    }
    case 0x1E: { // CreateMemoryBlock(addr, size, myperm, otherperm) -> r1
        auto b = std::make_shared<MemBlock>();
        b->addr = r[0];
        b->size = Kernel::Impl::pageUp(r[1]);
        if (b->addr) {
            // A non-zero address designates memory the process already owns (e.g. a heap buffer handed to a
            // service as shared memory): nothing is allocated, the block just refers to those pages.
            if (!I.overlaps(b->addr, b->size)) {
                uint32_t out;
                const uint32_t res = I.commit(b->addr, b->size, false, &out);
                if (res != kresult::Success)
                    return fail(res);
            }
            b->mapped = true;
        }
        r[1] = I.add(b);
        r[0] = kresult::Success;
        return true;
    }
    case 0x1F: { // MapMemoryBlock(handle, addr, myperm, otherperm)
        auto o = I.get(r[0]);
        if (!o || o->type != T::MemBlock)
            return fail(kresult::InvalidHandle);
        auto &b = static_cast<MemBlock &>(*o);
        if (b.mapped) { // aliasing the same pages at a second address is not supported
            if (K.log_)
                K.log_("MapMemoryBlock: second mapping of a block is not supported");
            return fail(kresult::InvalidCombination);
        }
        uint32_t out;
        const uint32_t res = I.commit(r[1], b.size, false, &out);
        if (res != kresult::Success) {
            if (K.log_) {
                char lb[96];
                std::snprintf(lb, sizeof lb, "MapMemoryBlock handle %08x at %08x size %x failed: %08x", r[0],
                              r[1], b.size, res);
                K.log_(lb);
            }
            return fail(res);
        }
        b.addr = out;
        b.mapped = true;
        if (K.log_) {
            char lb[80];
            std::snprintf(lb, sizeof lb, "MapMemoryBlock handle %08x -> %08x size %x", r[0], out, b.size);
            K.log_(lb);
        }
        r[0] = kresult::Success;
        return true;
    }
    case 0x20: { // UnmapMemoryBlock(handle, addr)
        auto o = I.get(r[0]);
        if (!o || o->type != T::MemBlock)
            return fail(kresult::InvalidHandle);
        auto &b = static_cast<MemBlock &>(*o);
        auto it = I.allocs.find(b.addr);
        if (it != I.allocs.end()) {
            const uint32_t a = it->first, n = it->second;
            I.allocs.erase(it);
            I.releaseGuest(a, n);
        }
        b.mapped = false;
        r[0] = kresult::Success;
        return true;
    }
    case 0x21: // CreateAddressArbiter -> r1
        r[1] = I.add(std::make_shared<Arbiter>());
        r[0] = kresult::Success;
        return true;
    case 0x22: { // ArbitrateAddress(handle, addr, type, value, timeout r4:r5)
        auto o = I.get(r[0]);
        if (!o || o->type != T::Arbiter)
            return fail(kresult::InvalidHandle);
        const uint32_t addr = r[1], type = r[2];
        const int32_t value = int32_t(r[3]);
        if (type == 0) { // SIGNAL: wake `value` waiters (all when negative)
            {
                char ab[96];
                std::snprintf(ab, sizeof ab, "arbitrate-signal %08x count %d %s", addr, value,
                              I.tname(t).c_str());
                I.trace(ab);
            }
            std::vector<Thread *> w;
            for (auto &th : I.threads)
                if (th->state == TState::Waiting && th->waitKind == WaitKind::Arbiter &&
                    th->arbiter == o.get() && th->arbAddr == addr)
                    w.push_back(th.get());
            std::stable_sort(w.begin(), w.end(), [](Thread *a, Thread *b) {
                return a->prio < b->prio || (a->prio == b->prio && a->readySeq < b->readySeq);
            });
            int32_t left = value < 0 ? int32_t(w.size()) : value;
            for (Thread *th : w) {
                if (left-- <= 0)
                    break;
                Kernel::Impl::setRes(th, kresult::Success);
                I.makeReady(th);
            }
            r[0] = kresult::Success;
            return true;
        }
        if (type > 4)
            return fail(kresult::InvalidCombination);
        const int32_t cur = int32_t(I.rd32(addr));
        {
            char ab[96];
            std::snprintf(ab, sizeof ab, "arbitrate %08x type %u value %d current %d %s", addr, type, value,
                          cur, I.tname(t).c_str());
            I.trace(ab);
        }
        if (!(cur < value)) {
            r[0] = kresult::Success;
            return true;
        }
        if (type == 2 || type == 4)
            I.wr32(addr, uint32_t(cur - 1));
        t->state = TState::Waiting;
        t->waitKind = WaitKind::Arbiter;
        t->arbAddr = addr;
        t->arbiter = o.get();
        t->readySeq = ++I.readyCounter;
        t->wakeAt = (type == 3 || type == 4) ? ts(r[4], r[5]) : UINT64_MAX;
        return false;
    }
    case 0x23: { // CloseHandle
        auto it = I.handles.find(r[0]);
        if (it == I.handles.end())
            return fail(kresult::InvalidHandle);
        I.handles.erase(it);
        r[0] = kresult::Success;
        return true;
    }
    case 0x24:
    case 0x25: {
        std::vector<std::shared_ptr<Obj>> objs;
        uint32_t lo, hi;
        bool all = false;
        if (n == 0x24) { // WaitSynchronization1(handle, timeout r1:r2)
            auto o = I.get(r[0]);
            if (!o)
                return fail(kresult::InvalidHandle);
            objs.push_back(o);
            lo = r[1];
            hi = r[2];
        } else { // WaitSynchronizationN(timeout lo r0, handles* r1, count r2, wait_all r3, timeout hi r4)
            lo = r[0];
            hi = r[4];
            all = r[3] & 1;
            for (uint32_t k = 0; k < r[2]; k++) {
                auto o = I.get(I.rd32(r[1] + 4 * k));
                if (!o)
                    return fail(kresult::InvalidHandle);
                objs.push_back(o);
            }
        }
        t->waitObjs = objs;
        t->waitAll = all;
        int idx = 0;
        if (I.trySatisfy(t, &idx)) {
            t->waitObjs.clear();
            r[0] = kresult::Success;
            r[1] = uint32_t(idx);
            return true;
        }
        const int64_t ns = int64_t(u64(lo, hi));
        if (ns == 0) {
            t->waitObjs.clear();
            r[0] = kresult::Timeout;
            r[1] = 0xFFFFFFFFu;
            return true;
        }
        t->state = TState::Waiting;
        t->waitKind = WaitKind::Sync;
        t->wakeAt = ts(lo, hi);
        return false;
    }
    case 0x27: { // DuplicateHandle(handle r1) -> r1
        auto o = I.get(r[1]);
        if (!o)
            return fail(kresult::InvalidHandle);
        r[1] = I.add(o);
        r[0] = kresult::Success;
        return true;
    }
    case 0x28: // GetSystemTick -> r0:r1
        r[0] = uint32_t(now);
        r[1] = uint32_t(now >> 32);
        return true;
    case 0x29: // GetHandleInfo(handle r1, type r2) -> r1:r2
        r[0] = I.get(r[1]) ? kresult::Success : kresult::InvalidHandle;
        r[1] = 0;
        r[2] = 0;
        return true;
    case 0x2B: // GetProcessInfo(process r1, type r2) -> r1:r2 (64-bit value)
        r[0] = kresult::Success;
        // Type 20: physical address minus virtual address of the linear heap (0x14000000 -> 0x20000000). The
        // SDK adds it to a linear virtual address to build the physical addresses it writes into GPU command
        // lists (read from the guest conversion at 0x172be4, which returns [0x69ca4c] + vaddr).
        r[1] = r[2] == 20 ? 0x0C000000u : 0;
        r[2] = 0;
        return true;
    case 0x2D: { // ConnectToPort(name* r1) -> r1 session
        char name[12] = {};
        std::memcpy(name, K.gm_.base() + r[1], 11);
        Service *s = I.serviceFor(name);
        r[1] = I.newSession(s);
        r[0] = kresult::Success;
        return true;
    }
    case 0x32: { // SendSyncRequest(session)
        auto o = I.get(r[0]);
        if (!o || o->type != T::Session)
            return fail(kresult::InvalidHandle);
        auto &ses = static_cast<Session &>(*o);
        IpcRequest req;
        req.kernel = &K;
        req.cmd = reinterpret_cast<uint32_t *>(K.gm_.base() + t->tls + kCmdBufOffset);
        K.stats_.ipcRequests++;
        K.ipcHist_[{ses.service->name(), req.header()}]++;
        std::snprintf(tb, sizeof tb, "ipc %s %08x", ses.service->name(), req.header());
        I.trace(tb);
        // A service dereferences guest pointers from the request (static buffers, output buffers). A bad one
        // must fail the request, not crash the host: run the handler under the fault guard.
        GuestFault pf;
        if (!K.gm_.runGuarded([&] { ses.service->handle(req); }, &pf)) {
            std::snprintf(tb, sizeof tb, "service %s faulted on guest address %08x (command %08x)",
                          ses.service->name(), pf.address, req.header());
            if (K.log_)
                K.log_(tb);
            req.reply(1, 0, kresult::InvalidAddress);
        }
        {
            const std::string svc = ses.service->name();
            if (svc != "gsp::Gpu" && svc != "hid:USER" && svc != "dsp::DSP" &&
                !(svc == "fs:File" && req.header() == 0x08020082)) {
                char line[96];
                std::snprintf(line, sizeof line, "%s %08x -> %08x", svc.c_str(), req.header(), req.cmd[1]);
                K.recentIpc_.push_back(line);
                if (K.recentIpc_.size() > 48)
                    K.recentIpc_.pop_front();
            }
        }
        r[0] = kresult::Success;
        I.wakeAll();
        return true;
    }
    case 0x35:   // GetProcessId
    case 0x37: { // GetThreadId
        auto o = I.get(r[1]);
        if (!o)
            return fail(kresult::InvalidHandle);
        r[1] = o->type == T::Thread ? static_cast<Thread &>(*o).id : 0x10;
        r[0] = kresult::Success;
        return true;
    }
    case 0x38: // GetResourceLimit(process r1) -> r1
        r[1] = I.add(I.resLimit);
        r[0] = kresult::Success;
        return true;
    case 0x39:
    case 0x3A: { // GetResourceLimitLimitValues / CurrentValues(values*, handle, names*, count)
        for (uint32_t k = 0; k < r[3]; k++) {
            const uint32_t name = I.rd32(r[2] + 4 * k);
            uint64_t v = 0;
            if (n == 0x39) {
                static const uint64_t limits[10] = {0x18, 0, 0x40, 0x100, 0x40, 0x80, 0x20, 0x30, 0x30, 0};
                v = name < 10 ? limits[name] : 0;
                if (name == 1)
                    v = Kernel::appMemorySize(I.cfg.appMemType);
            } else {
                uint64_t counts[10] = {0};
                for (auto &kv : I.handles) {
                    switch (kv.second->type) {
                    case T::Thread:
                        counts[2]++;
                        break;
                    case T::Event:
                        counts[3]++;
                        break;
                    case T::Mutex:
                        counts[4]++;
                        break;
                    case T::Semaphore:
                        counts[5]++;
                        break;
                    case T::Timer:
                        counts[6]++;
                        break;
                    case T::MemBlock:
                        counts[7]++;
                        break;
                    case T::Arbiter:
                        counts[8]++;
                        break;
                    default:
                        break;
                    }
                }
                for (auto &a : I.allocs)
                    counts[1] += a.second; // committed application memory
                v = name < 10 ? counts[name] : 0;
            }
            I.wr64(r[0] + 8 * k, v);
        }
        r[0] = kresult::Success;
        return true;
    }
    case 0x3C: // Break(reason)
        K.breakReason_ = r[0];
        I.breakHit = true;
        return false;
    case 0x3D: { // OutputDebugString(ptr, len)
        if (K.log_)
            K.log_("debug: " + std::string(reinterpret_cast<char *>(K.gm_.base() + r[0]),
                                           std::min<uint32_t>(r[1], 1024)));
        r[0] = kresult::Success;
        return true;
    }
    default: {
        char b[96];
        std::snprintf(b, sizeof b, "unimplemented SVC %#04x (r0=%08x r1=%08x r2=%08x r3=%08x)", n, r[0], r[1],
                      r[2], r[3]);
        if (K.log_)
            K.log_(b);
        r[0] = 0xF8C007F4; // invalid svc
        return true;
    }
    }
}

RunResult Kernel::run(uint64_t maxInsns) {
    Impl &I = *impl_;
    auto insns = [&] { return disp_.coverage().recompiledInsns + disp_.coverage().interpretedInsns; };
    const uint64_t start = insns();
    Thread *last = nullptr;
    for (;;) {
        I.processTimers();
        I.wakeAll();
        if (I.processExited)
            return RunResult::ProcessExit;
        if (I.breakHit)
            return RunResult::Break;
        if (insns() - start >= maxInsns)
            return RunResult::InstructionLimit;
        Thread *t = I.pickNext();
        if (!t) {
            bool anyAlive = false;
            for (auto &th : I.threads)
                anyAlive |= th->state != TState::Dead;
            if (!anyAlive)
                return RunResult::AllExited;
            const uint64_t ev = I.earliestEvent();
            if (ev == UINT64_MAX)
                return RunResult::Deadlock;
            now_ = std::max(now_, ev);
            continue;
        }
        if (t != last) {
            stats_.switches++;
            I.trace("run " + I.tname(t));
            last = t;
        }
        I.current = t;
        const uint64_t before = insns();
        StopReason why = StopReason::StepLimit;
        GuestFault f;
        const bool ok = gm_.runGuarded([&] { why = disp_.run(t->ctx, kSliceSteps); }, &f);
        now_ += insns() - before;
        if (!ok) {
            fault_ = f;
            I.trace("fault " + I.tname(t));
            return RunResult::Fault;
        }
        switch (why) {
        case StopReason::Svc: {
            stats_.svcs++;
            const bool keepRunning = I.handleSvc(t);
            if (!keepRunning && t->state == TState::Ready)
                t->readySeq = I.readyCounter;
            break;
        }
        case StopReason::Halt: // returned to the exit stub
            I.killThread(t);
            I.wakeAll();
            break;
        case StopReason::Trap:
            I.trace("trap " + I.tname(t));
            return RunResult::Trap;
        case StopReason::StepLimit:
            t->readySeq = ++I.readyCounter; // end of slice: round-robin among equals
            break;
        }
    }
}

} // namespace pokerecomp
