#include <cstdio>
#include <cstring>

#include "pokerecomp/guest_memory.h"

using pokerecomp::GuestFault;
using pokerecomp::GuestMemory;

static int g_fail = 0;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c);                        \
            g_fail++;                                                                                        \
        }                                                                                                    \
    } while (0)

int main() {
    GuestMemory m;
    CHECK(m.ok());
    const uint32_t page = uint32_t(GuestMemory::hostPageSize());
    const uint32_t a = 0x00100000;

    // Mapped memory behaves like normal memory through base + guest address.
    CHECK(m.map(a, 4 * page, GuestMemory::Read | GuestMemory::Write));
    uint32_t v = 0xCAFEF00D;
    std::memcpy(m.base() + a + 8, &v, 4);
    uint32_t r = 0;
    std::memcpy(&r, m.base() + a + 8, 4);
    CHECK(r == v);

    // Highest guest address is reachable (4 GiB reservation, 32-bit wrap-free addressing).
    CHECK(m.map(0xFFFFF000u & ~(page - 1), page, GuestMemory::Read | GuestMemory::Write));
    m.base()[0xFFFFFFFFu] = 0x5A;
    CHECK(m.base()[0xFFFFFFFFu] == 0x5A);

    // Unmapped access is reported with the guest address instead of crashing.
    GuestFault f;
    volatile uint8_t sink = 0;
    bool ok = m.runGuarded([&] { sink = m.base()[0x00800123]; }, &f);
    CHECK(!ok);
    CHECK(f.address == 0x00800123);

    // Write to a read-only page faults too; after protect() it succeeds.
    CHECK(m.protect(a, page, GuestMemory::Read));
    ok = m.runGuarded([&] { m.base()[a + 1] = 1; }, &f);
    CHECK(!ok);
    CHECK(f.address == a + 1);
    CHECK(m.protect(a, page, GuestMemory::Read | GuestMemory::Write));
    ok = m.runGuarded([&] { m.base()[a + 1] = 1; }, &f);
    CHECK(ok);

    // unmap() drops contents and re-arms the fault.
    CHECK(m.unmap(a, 4 * page));
    ok = m.runGuarded([&] { sink = m.base()[a + 8]; }, &f);
    CHECK(!ok);
    CHECK(m.map(a, page, GuestMemory::Read | GuestMemory::Write));
    std::memcpy(&r, m.base() + a + 8, 4);
    CHECK(r == 0); // fresh zero page

    // A guarded region that does not fault returns true; the guard can be reused many times.
    for (int i = 0; i < 1000; i++)
        CHECK(m.runGuarded([&] { sink = m.base()[a]; }, &f));
    (void)sink;
    std::printf(g_fail ? "FAIL\n" : "guest_memory ok\n");
    return g_fail ? 1 : 0;
}
