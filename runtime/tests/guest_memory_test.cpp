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
    // alias(): two guest addresses share pages; unmapping the alias leaves the source intact.
    {
        const uint32_t src = 0x08000000 + 2 * page, dst = 0x006D4000 + 2 * page; // same delta the SDK uses
        CHECK(m.map(src, 2 * page, GuestMemory::Read | GuestMemory::Write));
        uint32_t pat = 0x11223344;
        std::memcpy(m.base() + src + 4, &pat, 4);
        CHECK(m.alias(dst, src, 2 * page));
        std::memcpy(&r, m.base() + dst + 4, 4);
        CHECK(r == pat);  // existing contents visible through the alias
        pat = 0x55667788; // write through the alias, read through the source
        std::memcpy(m.base() + dst + page + 8, &pat, 4);
        std::memcpy(&r, m.base() + src + page + 8, 4);
        CHECK(r == pat);
        pat = 0x99AABBCC; // and the other way round
        std::memcpy(m.base() + src + 12, &pat, 4);
        std::memcpy(&r, m.base() + dst + 12, 4);
        CHECK(r == pat);
        CHECK(!m.alias(dst + 1, src, page)); // delta not a host-page multiple
        CHECK(!m.alias(src, src, page));     // same range
        CHECK(m.unmap(dst, 2 * page));
        ok = m.runGuarded([&] { sink = m.base()[dst + 4]; }, &f);
        CHECK(!ok && f.address == dst + 4); // alias gone
        std::memcpy(&r, m.base() + src + 12, 4);
        CHECK(r == 0x99AABBCC); // source untouched
    }
    std::printf(g_fail ? "FAIL\n" : "guest_memory ok\n");
    return g_fail ? 1 : 0;
}
