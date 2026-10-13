// Emitted code running on a real GuestMemory reservation: normal access works, faults carry the guest
// address.
#include <cstdio>
#include <cstring>

#include "pokerecomp/cpu.h"
#include "pokerecomp/guest_memory.h"

extern "C" void blk_loadadd_store(RcCtx *c);

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
    GuestMemory gm;
    CHECK(gm.ok());
    const uint32_t page = uint32_t(GuestMemory::hostPageSize());
    const uint32_t base = 0x08000000;
    CHECK(gm.map(base, 2 * page, GuestMemory::Read | GuestMemory::Write));
    uint32_t v = 41;
    std::memcpy(gm.base() + base + 4, &v, 4);

    RcCtx c;
    std::memset(&c, 0, sizeof c);
    c.mem = gm.base();
    c.r[1] = base;
    c.r[2] = base + 64;
    GuestFault f;
    CHECK(gm.runGuarded([&] { blk_loadadd_store(&c); }, &f));
    uint32_t out = 0;
    std::memcpy(&out, gm.base() + base + 64, 4);
    CHECK(out == 42);
    CHECK(c.r[0] == 42);

    // load from an unmapped page: the fault reports the guest address of the access
    c.r[1] = 0x30000000;
    CHECK(!gm.runGuarded([&] { blk_loadadd_store(&c); }, &f));
    CHECK(f.address == 0x30000004);

    // store to a read-only page
    c.r[1] = base;
    c.r[2] = base + page;
    CHECK(gm.protect(base + page, page, GuestMemory::Read));
    CHECK(!gm.runGuarded([&] { blk_loadadd_store(&c); }, &f));
    CHECK(f.address == base + page);

    std::printf(g_fail ? "FAIL\n" : "fault_test ok\n");
    return g_fail ? 1 : 0;
}
