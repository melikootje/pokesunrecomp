// cfg:u config blocks, driven directly through the service (no game data).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "pokerecomp/dispatcher.h"
#include "pokerecomp/guest_memory.h"
#include "pokerecomp/kernel.h"
#include "pokerecomp/services.h"

using namespace pokerecomp;

static int g_fail = 0;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c);                        \
            g_fail++;                                                                                        \
        }                                                                                                    \
    } while (0)

int main() {
    constexpr uint32_t kOut = 0x00200000;
    GuestMemory gm;
    Dispatcher disp;
    Kernel kernel{gm, disp};
    gm.map(kOut, 0x1000, GuestMemory::Read | GuestMemory::Write);
    ServiceOptions opt;
    opt.language = 1;
    auto cfg = makeCfgService("cfg:u", kernel, opt);

    auto getBlock = [&](uint32_t blk, uint32_t size) {
        std::memset(gm.base() + kOut, 0xCC, 0x100);
        uint32_t w[64] = {0x00010082, size, blk, 0, kOut};
        IpcRequest req;
        req.kernel = &kernel;
        req.cmd = w;
        cfg->handle(req);
        return w[1];
    };

    // Language block: one byte.
    CHECK(getBlock(0xA0002, 1) == 0);
    CHECK(gm.base()[kOut] == 1);

    // Stereo camera settings: the game divides by two of the floats, so they must be finite and non-zero
    // (all-zero data produced NaN view matrices and an empty 3D scene).
    CHECK(getBlock(0x50005, 32) == 0);
    float f[8];
    std::memcpy(f, gm.base() + kOut, sizeof f);
    for (float v : f)
        CHECK(std::isfinite(v) && v != 0.0f);

    // Unknown blocks are zero-filled.
    CHECK(getBlock(0x12345, 16) == 0);
    for (int i = 0; i < 16; i++)
        CHECK(gm.base()[kOut + i] == 0);
    return g_fail ? 1 : 0;
}
