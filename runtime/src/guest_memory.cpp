#include "pokerecomp/guest_memory.h"

#include <csetjmp>
#include <csignal>
#include <mutex>

#include <cstdlib>
#include <cstring>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

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

bool GuestMemory::alias(uint32_t dst, uint32_t src, uint32_t size) {
    const uint64_t pg = hostPageSize();
    if (!base_ || size == 0 || uint64_t(dst) + size > kReserve || uint64_t(src) + size > kReserve)
        return false;
    const int64_t delta = int64_t(dst) - int64_t(src);
    if (delta % int64_t(pg) != 0 || delta == 0)
        return false;
    const uint64_t s0 = uint64_t(src) & ~(pg - 1), s1 = (uint64_t(src) + size + pg - 1) & ~(pg - 1);
    const uint64_t d0 = uint64_t(int64_t(s0) + delta), len = s1 - s0;
    if (s1 > kReserve || d0 + len > kReserve || (d0 < s1 && s0 < d0 + len))
        return false; // out of range or overlapping views
#ifdef __APPLE__
    mach_vm_address_t target = reinterpret_cast<mach_vm_address_t>(base_ + d0);
    vm_prot_t cur, max;
    const kern_return_t kr = mach_vm_remap(
        mach_task_self(), &target, len, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
        reinterpret_cast<mach_vm_address_t>(base_ + s0), FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS)
        return false;
    return mprotect(base_ + d0, len, PROT_READ | PROT_WRITE) == 0;
#else
    // Linux/Android: mremap can only duplicate a MAP_SHARED mapping, so first move the source pages onto an
    // anonymous shared mapping (contents preserved), then duplicate that.
    std::vector<uint8_t> keep(len);
    std::memcpy(keep.data(), base_ + s0, len);
    if (mmap(base_ + s0, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
        return false;
    std::memcpy(base_ + s0, keep.data(), len);
    void *p = mremap(base_ + s0, 0, len, MREMAP_MAYMOVE | MREMAP_FIXED, base_ + d0);
    return p != MAP_FAILED;
#endif
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
