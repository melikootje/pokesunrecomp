#include "pokerecomp/guest_memory.h"

#include <csetjmp>
#include <csignal>
#include <mutex>

#include <sys/mman.h>
#include <unistd.h>

namespace pokerecomp {
namespace {

constexpr uint64_t kReserve = 1ull << 32;

struct Guard {
    uint8_t *base = nullptr;
    sigjmp_buf env;
    GuestFault fault;
    bool active = false;
};
thread_local Guard g_guard;

struct sigaction g_oldSegv, g_oldBus;
std::once_flag g_once;

void handler(int sig, siginfo_t *info, void *ctx) {
    Guard &g = g_guard;
    const uint8_t *a = static_cast<const uint8_t *>(info->si_addr);
    if (g.active && g.base && a >= g.base && a < g.base + kReserve) {
        g.fault.address = uint32_t(a - g.base);
        g.fault.write = false;
        siglongjmp(g.env, 1);
    }
    // Not ours: restore the previous action and re-raise.
    const struct sigaction &old = sig == SIGSEGV ? g_oldSegv : g_oldBus;
    if (old.sa_flags & SA_SIGINFO) {
        old.sa_sigaction(sig, info, ctx);
    } else if (old.sa_handler == SIG_DFL || old.sa_handler == SIG_IGN) {
        signal(sig, SIG_DFL);
        raise(sig);
    } else {
        old.sa_handler(sig);
    }
}

void installHandlers() {
    struct sigaction sa;
    sa.sa_sigaction = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, &g_oldSegv);
    sigaction(SIGBUS, &sa, &g_oldBus);
}

int toHost(int p) {
    int r = PROT_NONE;
    if (p & GuestMemory::Read)
        r |= PROT_READ;
    if (p & GuestMemory::Write)
        r |= PROT_WRITE;
    if (p & GuestMemory::Exec)
        r |= PROT_EXEC;
    return r;
}

} // namespace

size_t GuestMemory::hostPageSize() { return size_t(sysconf(_SC_PAGESIZE)); }

GuestMemory::GuestMemory() {
    void *p = mmap(nullptr, kReserve, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p != MAP_FAILED)
        base_ = static_cast<uint8_t *>(p);
    std::call_once(g_once, installHandlers);
}

GuestMemory::~GuestMemory() {
    if (base_)
        munmap(base_, kReserve);
}

bool GuestMemory::map(uint32_t addr, uint32_t size, int prot) {
    if (!base_ || size == 0 || uint64_t(addr) + size > kReserve)
        return false;
    return mprotect(base_ + addr, size, toHost(prot)) == 0;
}

bool GuestMemory::protect(uint32_t addr, uint32_t size, int prot) { return map(addr, size, prot); }

bool GuestMemory::unmap(uint32_t addr, uint32_t size) {
    if (!base_ || size == 0 || uint64_t(addr) + size > kReserve)
        return false;
    // Drop the pages' contents and make them inaccessible again.
    return mmap(base_ + addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0) !=
           MAP_FAILED;
}

bool GuestMemory::runGuarded(const std::function<void()> &fn, GuestFault *fault) {
    Guard &g = g_guard;
    g.base = base_;
    g.active = true;
    if (sigsetjmp(g.env, 1) == 0) {
        fn();
        g.active = false;
        return true;
    }
    g.active = false;
    if (fault)
        *fault = g.fault;
    return false;
}

} // namespace pokerecomp
