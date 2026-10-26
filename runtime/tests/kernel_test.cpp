// HLE kernel behaviour, driven by tiny hand-assembled ARM guest programs run in the interpreter fallback.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pokerecomp/dispatcher.h"
#include "pokerecomp/guest_memory.h"
#include "pokerecomp/kernel.h"

using namespace pokerecomp;

static int g_fail = 0;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c);                        \
            g_fail++;                                                                                        \
        }                                                                                                    \
    } while (0)

// ---------------------------------------------------------------- mini assembler (A32)
struct Asm {
    uint32_t base;
    std::vector<uint32_t> w;
    std::map<std::string, uint32_t> labels;
    struct Fix {
        size_t at;
        std::string label;
        unsigned cond;
        bool link;
    };
    std::vector<Fix> fixes;
    struct Lit {
        size_t at;
        uint32_t value;
    };
    std::vector<Lit> lits;
    explicit Asm(uint32_t b) : base(b) {}
    uint32_t here() const { return base + 4 * uint32_t(w.size()); }
    void label(const std::string &n) { labels[n] = here(); }
    void emit(uint32_t x) { w.push_back(x); }
    void mov(unsigned rd, unsigned imm) { emit(0xE3A00000u | rd << 12 | imm); }
    void mvn0(unsigned rd) { emit(0xE3E00000u | rd << 12); } // rd = 0xFFFFFFFF
    void movr(unsigned rd, unsigned rm) { emit(0xE1A00000u | rd << 12 | rm); }
    void add(unsigned rd, unsigned rn, unsigned imm) { emit(0xE2800000u | rn << 16 | rd << 12 | imm); }
    void sub(unsigned rd, unsigned rn, unsigned imm) { emit(0xE2400000u | rn << 16 | rd << 12 | imm); }
    void ldr(unsigned rd, unsigned rn, unsigned off) { emit(0xE5900000u | rn << 16 | rd << 12 | off); }
    void str(unsigned rd, unsigned rn, unsigned off) { emit(0xE5800000u | rn << 16 | rd << 12 | off); }
    void cmp(unsigned rn, unsigned imm) { emit(0xE3500000u | rn << 16 | imm); }
    void svc(unsigned n) { emit(0xEF000000u | n); }
    void bx(unsigned rm) { emit(0xE12FFF10u | rm); }
    void b(unsigned cond, const std::string &l) {
        fixes.push_back({w.size(), l, cond, false});
        emit(0);
    }
    void bl(const std::string &l) {
        fixes.push_back({w.size(), l, 14, true});
        emit(0);
    }
    void ldrc(unsigned rd, uint32_t value) {
        lits.push_back({w.size(), value});
        emit(0xE59F0000u | rd << 12);
    }
    void finish() {
        std::vector<uint32_t> litAddr;
        for (auto &l : lits) {
            litAddr.push_back(here());
            emit(l.value);
        }
        for (size_t k = 0; k < lits.size(); k++) {
            const uint32_t pc = base + 4 * uint32_t(lits[k].at) + 8;
            w[lits[k].at] |= litAddr[k] - pc;
        }
        for (auto &f : fixes) {
            const uint32_t from = base + 4 * uint32_t(f.at) + 8;
            const int32_t off = (int32_t(labels.at(f.label)) - int32_t(from)) / 4;
            w[f.at] = f.cond << 28 | (f.link ? 0x0B000000u : 0x0A000000u) | (uint32_t(off) & 0xFFFFFF);
        }
    }
};

constexpr uint32_t kCode = 0x00100000, kData = 0x00200000, kStackTop = 0x08100000;
constexpr uint32_t kChildStack = 0x080F0000;

struct Env {
    GuestMemory gm;
    Dispatcher disp;
    Kernel kernel;
    std::vector<std::string> logs;
    Env() : kernel(gm, disp) {
        const uint32_t pg = uint32_t(GuestMemory::hostPageSize());
        const int rw = GuestMemory::Read | GuestMemory::Write;
        gm.map(kCode, 0x10000, rw);
        gm.map(kData, 0x10000, rw);
        gm.map((kStackTop - 0x20000) & ~(pg - 1), 0x20000, rw);
        kernel.setLog([this](const std::string &s) { logs.push_back(s); });
    }
    void load(Asm &a) {
        a.finish();
        std::memcpy(gm.base() + a.base, a.w.data(), a.w.size() * 4);
    }
    uint32_t rd(uint32_t addr) const {
        uint32_t v;
        std::memcpy(&v, gm.base() + addr, 4);
        return v;
    }
    void wr(uint32_t addr, uint32_t v) { std::memcpy(gm.base() + addr, &v, 4); }
    RunResult runMain(Asm &a, uint64_t limit = 5000000) {
        load(a);
        kernel.createMainThread(a.base, kStackTop, 0);
        return kernel.run(limit);
    }
};

// Data slots
constexpr uint32_t kEvt = kData + 0, kRes = kData + 4, kLogN = kData + 8, kFlag = kData + 12,
                   kHandleA = kData + 16, kHandleB = kData + 20, kCount = kData + 24, kLog = kData + 0x100,
                   kIpcName = kData + 0x400;

struct Prog {
    Asm a{kCode};
    std::map<size_t, std::string> entryLits; // literal slot index (in w) -> label
    // CreateThread(prio, entry=label, arg, stackTop, processor=-2)
    void createThread(const std::string &entry, unsigned prio, uint32_t stackTop, uint32_t arg = 0) {
        a.mov(0, prio);
        entryLits[a.w.size()] = entry;
        a.ldrc(1, 0);
        a.ldrc(2, arg);
        a.ldrc(3, stackTop);
        a.mvn0(4);
        a.sub(4, 4, 1);
        a.svc(0x08);
    }
    // Appends `value` to the log array (r5-r8 clobbered)
    void appendLog(unsigned value) {
        a.ldrc(5, kLogN);
        a.ldr(6, 5, 0);
        a.ldrc(7, kLog);
        a.emit(0xE0877106u); // add r7, r7, r6, lsl #2
        a.mov(8, value);
        a.str(8, 7, 0);
        a.add(6, 6, 1);
        a.str(6, 5, 0);
    }
    void storeR0(uint32_t addr) { // [addr] = r0   (clobbers r9)
        a.ldrc(9, addr);
        a.str(0, 9, 0);
    }
    void saveHandle(uint32_t slot) { storeHandleFromR1(slot); }
    void storeHandleFromR1(uint32_t slot) {
        a.ldrc(9, slot);
        a.str(1, 9, 0);
    }
    void loadR0(uint32_t slot) {
        a.ldrc(9, slot);
        a.ldr(0, 9, 0);
    }
    void waitForever() {
        a.mvn0(1);
        a.mvn0(2);
        a.svc(0x24);
    }
    void exitThread() {
        a.mov(0, 0);
        a.svc(0x09);
    }
    void finish(Env &e) {
        a.finish();
        for (auto &kv : entryLits) { // fix up the literal that holds a label address
            const uint32_t litAddr = (a.w[kv.first] & 0xFFF) + a.base + 4 * uint32_t(kv.first) + 8;
            a.w[(litAddr - a.base) / 4] = a.labels.at(kv.second);
        }
        std::memcpy(e.gm.base() + a.base, a.w.data(), a.w.size() * 4);
    }
};

static RunResult runProg(Env &e, Prog &p, uint64_t limit = 5000000) {
    p.finish(e);
    e.kernel.createMainThread(p.a.base, kStackTop, 0);
    return e.kernel.run(limit);
}

static Env *g_env = nullptr;

static void testEventHandoff(std::vector<std::string> *traceOut = nullptr) {
    Env e;
    Prog p;
    p.a.mov(1, 0);
    p.a.svc(0x17); // CreateEvent(oneshot)
    p.storeHandleFromR1(kEvt);
    p.createThread("child", 0x30, kChildStack);
    p.loadR0(kEvt);
    p.waitForever();
    p.storeR0(kRes);
    p.a.bx(14);
    p.a.label("child");
    p.loadR0(kEvt);
    p.a.svc(0x18); // SignalEvent
    p.exitThread();
    CHECK(runProg(e, p) == RunResult::AllExited);
    CHECK(e.rd(kRes) == 0);
    CHECK(e.kernel.stats().svcs >= 5);
    if (traceOut)
        *traceOut = e.kernel.trace();
}

int main() {
    // 1. thread creation + event signalling + WaitSynchronization1 with an infinite timeout
    testEventHandoff();
    // 2. deterministic: the scheduling/SVC trace is identical across runs
    {
        std::vector<std::string> t1, t2;
        testEventHandoff(&t1);
        testEventHandoff(&t2);
        CHECK(!t1.empty() && t1 == t2);
    }
    // CreateMemoryBlock over memory the process already owns (a heap buffer passed to a service as shared
    // memory) succeeds without allocating anything.
    {
        Env e;
        Prog p;
        p.a.mov(0, 3); // COMMIT
        p.a.mov(1, 0);
        p.a.mov(2, 0);
        p.a.ldrc(3, 0x8000);
        p.a.mov(4, 3);
        p.a.svc(0x01); // -> r1 address
        p.a.movr(5, 1);
        p.a.add(0, 1, 0); // r0 = addr
        p.a.ldrc(1, 0x1000);
        p.a.mov(2, 3);
        p.a.mov(3, 3);
        p.a.svc(0x1E);
        p.storeR0(kRes);
        p.a.bx(14);
        CHECK(runProg(e, p) == RunResult::AllExited);
        CHECK(e.rd(kRes) == kresult::Success);
    }
    // The shared page carries the console clock (2024-01-02 03:04:05.678 local) and the 3D slider.
    {
        Env e;
        KernelConfig cfg;
        cfg.startTimeMs = 1704164645678ull;
        e.kernel.configure(cfg);
        uint64_t ms, ticksPerSec, tickBase;
        float slider;
        std::memcpy(&ms, e.gm.base() + 0x1FF81020, 8);
        std::memcpy(&tickBase, e.gm.base() + 0x1FF81028, 8);
        std::memcpy(&ticksPerSec, e.gm.base() + 0x1FF81030, 8);
        std::memcpy(&slider, e.gm.base() + 0x1FF81080, 4);
        CHECK(ms == 1704164645678ull + 2208988800000ull);
        CHECK(tickBase == 0 && ticksPerSec == kTicksPerSecond && slider == 0.0f);
        uint64_t ms2;
        std::memcpy(&ms2, e.gm.base() + 0x1FF81040, 8); // the alternate record is identical
        CHECK(ms2 == ms);
    }
    // 3. timeout on the virtual clock
    {
        Env e;
        Prog p;
        p.a.mov(1, 0);
        p.a.svc(0x17);
        p.a.movr(0, 1);
        p.a.ldrc(1, 1000000); // 1 ms
        p.a.mov(2, 0);
        p.a.svc(0x24);
        p.storeR0(kRes);
        p.a.bx(14);
        CHECK(runProg(e, p) == RunResult::AllExited);
        CHECK(e.rd(kRes) == kresult::Timeout);
        CHECK(e.kernel.ticks() >= kTicksPerSecond / 1000); // at least 1 ms of virtual time passed
    }
    // 4. sleeping threads wake in the order of their deadlines
    {
        Env e;
        Prog p;
        p.createThread("slow", 0x30, kChildStack);
        p.createThread("fast", 0x30, kChildStack - 0x2000);
        p.a.ldrc(0, 5000000); // main sleeps 5 ms so both have finished
        p.a.mov(1, 0);
        p.a.svc(0x0A);
        p.a.bx(14);
        p.a.label("slow");
        p.a.ldrc(0, 2000000);
        p.a.mov(1, 0);
        p.a.svc(0x0A);
        p.appendLog(2);
        p.exitThread();
        p.a.label("fast");
        p.a.ldrc(0, 1000000);
        p.a.mov(1, 0);
        p.a.svc(0x0A);
        p.appendLog(1);
        p.exitThread();
        CHECK(runProg(e, p) == RunResult::AllExited);
        CHECK(e.rd(kLogN) == 2);
        CHECK(e.rd(kLog) == 1 && e.rd(kLog + 4) == 2);
    }
    // 5. deadlock: the only thread waits forever for an event nobody signals
    {
        Env e;
        Prog p;
        p.a.mov(1, 0);
        p.a.svc(0x17);
        p.a.movr(0, 1);
        p.waitForever();
        p.a.bx(14);
        CHECK(runProg(e, p) == RunResult::Deadlock);
    }
    // 6. spin-wait on memory needs time slicing: the child runs and sets the flag
    {
        Env e;
        Prog p;
        p.createThread("child", 0x30, kChildStack);
        p.a.ldrc(5, kFlag);
        p.a.label("spin");
        p.a.ldr(0, 5, 0);
        p.a.cmp(0, 0);
        p.a.b(0 /*eq*/, "spin");
        p.a.mov(0, 77);
        p.storeR0(kRes);
        p.a.bx(14);
        p.a.label("child");
        p.a.ldrc(5, kFlag);
        p.a.mov(0, 1);
        p.a.str(0, 5, 0);
        p.exitThread();
        CHECK(runProg(e, p) == RunResult::AllExited);
        CHECK(e.rd(kRes) == 77);
        CHECK(e.kernel.stats().switches >= 3);
    }
    // 7. mutex hand-off: the child blocks on a mutex held by main and gets it after ReleaseMutex
    {
        Env e;
        Prog p;
        p.a.mov(1, 1);
        p.a.svc(0x13); // CreateMutex(initially locked) -> r1
        p.storeHandleFromR1(kHandleA);
        p.createThread("child", 0x30, kChildStack);
        p.a.mov(0, 0);
        p.a.mov(1, 0);
        p.a.ldrc(1, 100000);
        p.a.mov(2, 0);
        p.a.mov(0, 0);
        // yield so the child runs and blocks on the mutex: SleepThread(1 ms)
        p.a.ldrc(0, 1000000);
        p.a.mov(1, 0);
        p.a.svc(0x0A);
        p.appendLog(1); // main logs "released" before releasing
        p.loadR0(kHandleA);
        p.a.svc(0x14); // ReleaseMutex
        p.a.ldrc(0, 1000000);
        p.a.mov(1, 0);
        p.a.svc(0x0A);
        p.a.bx(14);
        p.a.label("child");
        p.loadR0(kHandleA);
        p.waitForever(); // blocks until main releases
        p.appendLog(2);
        p.exitThread();
        CHECK(runProg(e, p) == RunResult::AllExited);
        CHECK(e.rd(kLogN) == 2 && e.rd(kLog) == 1 && e.rd(kLog + 4) == 2);
    }
    // 8. semaphore: each ReleaseSemaphore(1) lets exactly one waiter through
    {
        Env e;
        Prog p;
        p.a.mov(1, 0);
        p.a.mov(2, 2);
        p.a.svc(0x15); // CreateSemaphore(initial 0, max 2) -> r1
        p.storeHandleFromR1(kHandleA);
        p.createThread("waiter", 0x30, kChildStack, 1);
        p.createThread("waiter", 0x30, kChildStack - 0x2000, 2);
        p.a.ldrc(0, 1000000);
        p.a.mov(1, 0);
        p.a.svc(0x0A); // let both block
        p.a.ldrc(9, kHandleA);
        p.a.ldr(1, 9, 0);
        p.a.mov(2, 1);
        p.a.svc(0x16); // ReleaseSemaphore(handle in r1, count 1 in r2)
        p.a.ldrc(0, 1000000);
        p.a.mov(1, 0);
        p.a.svc(0x0A);
        p.storeR0(kRes);
        p.a.bx(14);
        p.a.label("waiter");
        p.loadR0(kHandleA);
        p.waitForever();
        p.appendLog(5);
        p.exitThread();
        CHECK(runProg(e, p, 20000000) == RunResult::AllExited || e.rd(kLogN) == 1);
        CHECK(
            e.rd(kLogN) ==
            1); // only one waiter passed; the other is still blocked (deadlock/all-exited depending on main)
    }
    // 9. ArbitrateAddress: wait-if-less-than blocks until another thread signals the address
    {
        Env e;
        Prog p;
        p.a.svc(0x21); // CreateAddressArbiter -> r1
        p.storeHandleFromR1(kHandleA);
        p.createThread("waiter", 0x30, kChildStack);
        p.a.ldrc(0, 1000000);
        p.a.mov(1, 0);
        p.a.svc(0x0A);
        p.appendLog(1); // "about to signal"
        p.loadR0(kHandleA);
        p.a.ldrc(1, kCount);
        p.a.mov(2, 0); // type SIGNAL
        p.a.mov(3, 1); // wake one
        p.a.svc(0x22);
        p.a.ldrc(0, 1000000);
        p.a.mov(1, 0);
        p.a.svc(0x0A);
        p.a.bx(14);
        p.a.label("waiter");
        p.loadR0(kHandleA);
        p.a.ldrc(1, kCount); // *kCount == 0 < 1 -> wait
        p.a.mov(2, 1);       // WAIT_IF_LESS_THAN
        p.a.mov(3, 1);
        p.a.svc(0x22);
        p.appendLog(2);
        p.exitThread();
        CHECK(runProg(e, p) == RunResult::AllExited);
        CHECK(e.rd(kLogN) == 2 && e.rd(kLog) == 1 && e.rd(kLog + 4) == 2);
    }
    // 10. memory SVCs: commit heap, use it, query it, free it
    {
        Env e;
        Prog p;
        p.a.mov(0, 3); // COMMIT
        p.a.mov(1, 0); // any address
        p.a.mov(2, 0);
        p.a.ldrc(3, 0x8000);
        p.a.mov(4, 3);
        p.a.svc(0x01); // -> r1 address
        p.storeHandleFromR1(kHandleA);
        p.a.mov(2, 0x5A);
        p.a.str(2, 1, 4);           // write to the new memory
        p.a.ldrc(0, kData + 0x200); // QueryMemory(meminfo*, pageinfo*, addr)
        p.a.ldrc(1, kData + 0x240);
        p.a.ldrc(9, kHandleA);
        p.a.ldr(2, 9, 0);
        p.a.svc(0x02);
        p.a.mov(0, 1); // FREE
        p.a.ldrc(9, kHandleA);
        p.a.ldr(1, 9, 0);
        p.a.mov(2, 0);
        p.a.ldrc(3, 0x8000);
        p.a.svc(0x01);
        p.storeR0(kRes);
        p.a.bx(14);
        CHECK(runProg(e, p) == RunResult::AllExited);
        const uint32_t addr = e.rd(kHandleA);
        CHECK(addr >= 0x08000000 && addr < 0x10000000);
        CHECK(e.rd(kData + 0x200) == addr);   // meminfo.base_addr
        CHECK(e.rd(kData + 0x204) >= 0x8000); // meminfo.size
        CHECK(e.rd(kRes) == 0);
    }
    // 11. IPC through srv: (RegisterClient, GetServiceHandle) and an unimplemented service
    {
        Env e;
        std::memcpy(e.gm.base() + kIpcName, "srv:\0\0\0\0\0\0\0", 11);
        Prog p;
        p.a.ldrc(1, kIpcName);
        p.a.svc(0x2D); // ConnectToPort -> r1
        p.storeHandleFromR1(kHandleA);
        // tls = r? : use mrc p15,0,r4,c13,c0,3 to find the command buffer
        p.a.emit(0xEE1D4F70u);   // mrc p15, 0, r4, c13, c0, 3
        p.a.ldrc(5, 0x00010002); // RegisterClient header
        p.a.str(5, 4, 0x80);
        p.a.mov(5, 0);
        p.a.str(5, 4, 0x84);
        p.a.ldrc(9, kHandleA);
        p.a.ldr(0, 9, 0);
        p.a.svc(0x32);
        p.a.ldr(0, 4, 0x80);
        p.storeR0(kRes); // reply header
        // GetServiceHandle("fs:USER")
        p.a.ldrc(5, 0x00050100);
        p.a.str(5, 4, 0x80);
        p.a.ldrc(5, 0x553A7366); // "fs:U"
        p.a.str(5, 4, 0x84);
        p.a.ldrc(5, 0x00524553); // "SER\0"
        p.a.str(5, 4, 0x88);
        p.a.mov(5, 7);
        p.a.str(5, 4, 0x8C);
        p.a.mov(5, 0);
        p.a.str(5, 4, 0x90);
        p.a.ldrc(9, kHandleA);
        p.a.ldr(0, 9, 0);
        p.a.svc(0x32);
        p.a.ldr(1, 4, 0x8C); // service session handle
        p.storeHandleFromR1(kHandleB);
        // send an arbitrary command to the fs: session
        p.a.ldrc(5, 0x08010002);
        p.a.str(5, 4, 0x80);
        p.a.ldrc(9, kHandleB);
        p.a.ldr(0, 9, 0);
        p.a.svc(0x32);
        p.a.ldr(0, 4, 0x84);
        p.storeR0(kData + 0x30);
        p.a.bx(14);
        e.kernel.setUnimplementedPolicy(UnimplementedPolicy::ReturnError);
        const RunResult rr = runProg(e, p);
        if (rr != RunResult::AllExited) {
            std::fprintf(stderr, "IPC test run result %d fault=%08x\n", int(rr),
                         e.kernel.lastFault().address);
            for (auto &t : e.kernel.trace())
                std::fprintf(stderr, "  %s\n", t.c_str());
            for (auto &l : e.logs)
                std::fprintf(stderr, "  log: %s\n", l.c_str());
        }
        CHECK(rr == RunResult::AllExited);
        CHECK(e.rd(kRes) == 0x00010040); // RegisterClient reply: 1 normal parameter
        CHECK(e.rd(kHandleB) != 0);
        CHECK(e.rd(kData + 0x30) == kresult::InvalidCommand); // fs:USER is not implemented yet
        const auto un = e.kernel.unimplementedCommands();
        CHECK(un.size() == 1 && un[0].first == "fs:USER" && un[0].second == 0x08010002);
        CHECK(!e.logs.empty());
    }
    // 11b. configuration page and resource limits (the real game panics at start-up without sane values)
    {
        Env e;
        KernelConfig cfg;
        cfg.appMemType = 1;
        e.kernel.configure(cfg);
        Prog p;
        p.a.ldrc(5, kConfigPage);
        p.a.ldr(0, 5, 0x40); // APPMEMALLOC
        p.storeR0(kRes);
        p.a.ldr(0, 5, 0x20); // APPMEMTYPE
        p.storeR0(kHandleA);
        // ControlMemory commit 0x10000, then GetResourceLimitCurrentValues(commit)
        p.a.mov(0, 3);
        p.a.mov(1, 0);
        p.a.mov(2, 0);
        p.a.ldrc(3, 0x10000);
        p.a.mov(4, 3);
        p.a.svc(0x01);
        p.a.ldrc(5, kData + 0x300); // names[0] = 1 (commit)
        p.a.mov(6, 1);
        p.a.str(6, 5, 0);
        p.a.ldrc(0, kData + 0x310); // values*
        p.a.mov(1, 0);
        p.a.ldrc(2, kData + 0x300);
        p.a.mov(3, 1);
        p.a.svc(0x3A);
        p.a.ldrc(0, kData + 0x320); // limit values
        p.a.mov(1, 0);
        p.a.ldrc(2, kData + 0x300);
        p.a.mov(3, 1);
        p.a.svc(0x39);
        p.a.bx(14);
        CHECK(runProg(e, p) == RunResult::AllExited);
        CHECK(e.rd(kRes) == 0x04800000);
        CHECK(e.rd(kHandleA) == 1);
        CHECK(e.rd(kData + 0x310) >= 0x10000);    // committed memory is accounted
        CHECK(e.rd(kData + 0x320) == 0x04800000); // commit limit = application memory
    }
    // 11b. configuration page and resource limits (the real game panics at start-up without sane values)
    {
        Env e;
        KernelConfig cfg;
        cfg.appMemType = 1;
        e.kernel.configure(cfg);
        Prog p;
        p.a.ldrc(5, kConfigPage);
        p.a.ldr(0, 5, 0x40); // APPMEMALLOC
        p.storeR0(kRes);
        p.a.ldr(0, 5, 0x20); // APPMEMTYPE
        p.storeR0(kHandleA);
        // ControlMemory commit 0x10000, then GetResourceLimitCurrentValues(commit)
        p.a.mov(0, 3);
        p.a.mov(1, 0);
        p.a.mov(2, 0);
        p.a.ldrc(3, 0x10000);
        p.a.mov(4, 3);
        p.a.svc(0x01);
        p.a.ldrc(5, kData + 0x300); // names[0] = 1 (commit)
        p.a.mov(6, 1);
        p.a.str(6, 5, 0);
        p.a.ldrc(0, kData + 0x310); // values*
        p.a.mov(1, 0);
        p.a.ldrc(2, kData + 0x300);
        p.a.mov(3, 1);
        p.a.svc(0x3A);
        p.a.ldrc(0, kData + 0x320); // limit values
        p.a.mov(1, 0);
        p.a.ldrc(2, kData + 0x300);
        p.a.mov(3, 1);
        p.a.svc(0x39);
        p.a.bx(14);
        CHECK(runProg(e, p) == RunResult::AllExited);
        CHECK(e.rd(kRes) == 0x04800000);
        CHECK(e.rd(kHandleA) == 1);
        CHECK(e.rd(kData + 0x310) >= 0x10000);    // committed memory is accounted
        CHECK(e.rd(kData + 0x320) == 0x04800000); // commit limit = application memory
    }
    // 12. Break and ExitProcess
    {
        Env e;
        Prog p;
        p.a.mov(0, 1);
        p.a.svc(0x3C);
        CHECK(runProg(e, p) == RunResult::Break);
        CHECK(e.kernel.breakReason() == 1);
        Env e2;
        Prog p2;
        p2.a.svc(0x03);
        CHECK(runProg(e2, p2) == RunResult::ProcessExit);
    }
    // 13. a guest access to unmapped memory is reported as a fault with its address
    {
        Env e;
        Prog p;
        p.a.ldrc(5, 0x30000000);
        p.a.ldr(0, 5, 4);
        p.a.bx(14);
        CHECK(runProg(e, p) == RunResult::Fault);
        CHECK(e.kernel.lastFault().address == 0x30000004);
    }
    // The guest allocates in 4 KiB pages even when the host uses bigger ones: two blocks 8 KiB apart must
    // both map (they share a 16 KiB host page on Apple silicon), freeing one keeps its neighbour and zeroes
    // itself.
    {
        Env e;
        Prog p;
        auto commit = [&](uint32_t addr, uint32_t op) {
            p.a.mov(0, op);
            p.a.ldrc(1, addr);
            p.a.mov(2, 0);
            p.a.ldrc(3, 0x1000);
            p.a.mov(4, 3);
            p.a.svc(0x01);
        };
        auto poke = [&](uint32_t addr, uint32_t v) {
            p.a.ldrc(5, addr);
            p.a.ldrc(6, v);
            p.a.str(6, 5, 0);
        };
        auto peek = [&](uint32_t addr, uint32_t slot) {
            p.a.ldrc(5, addr);
            p.a.ldr(0, 5, 0);
            p.storeR0(slot);
        };
        commit(0x10000000, 3);
        p.storeR0(kRes);
        commit(0x10002000, 3);
        p.storeR0(kHandleA);
        poke(0x10000010, 0xAAAA);
        poke(0x10002010, 0xBBBB);
        commit(0x10000000, 1); // free the first block
        p.storeR0(kHandleB);
        commit(0x10000000, 3); // and map it again
        p.storeR0(kCount);
        peek(0x10000010, kFlag);
        peek(0x10002010, kEvt);
        p.a.bx(14);
        CHECK(runProg(e, p) == RunResult::AllExited);
        CHECK(e.rd(kRes) == 0 && e.rd(kHandleA) == 0); // both commits succeed
        CHECK(e.rd(kHandleB) == 0 && e.rd(kCount) == 0);
        CHECK(e.rd(kFlag) == 0);     // the freed block reads as zero pages again
        CHECK(e.rd(kEvt) == 0xBBBB); // the neighbour in the same host page was not touched
    }
    // GetProcessInfo(self, 20) reports physical - virtual of the linear heap; other types read as 0.
    {
        Env e;
        Prog p;
        p.a.ldrc(1, kCurrentProcess);
        p.a.mov(2, 20);
        p.a.svc(0x2B); // r1:r2 = value
        p.a.movr(8, 2);
        p.a.movr(0, 1);
        p.storeR0(kRes);
        p.a.movr(0, 8);
        p.storeR0(kHandleA);
        p.a.ldrc(1, kCurrentProcess);
        p.a.mov(2, 3);
        p.a.svc(0x2B);
        p.a.movr(0, 1);
        p.storeR0(kHandleB);
        p.a.bx(14);
        CHECK(runProg(e, p) == RunResult::AllExited);
        CHECK(e.rd(kRes) == 0x0C000000 && e.rd(kHandleA) == 0);
        CHECK(e.rd(kHandleB) == 0);
    }
    (void)g_env;
    std::printf(g_fail ? "FAIL\n" : "kernel_test ok\n");
    return g_fail ? 1 : 0;
}
