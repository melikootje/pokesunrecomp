// GX engine: memory fill, display transfer (tiling, formats, crop, flip, downscale), texture copy, capture.
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "pokerecomp/gpu.h"
#include "pokerecomp/guest_memory.h"

using namespace pokerecomp;

static int g_fail = 0;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c);                        \
            g_fail++;                                                                                        \
        }                                                                                                    \
    } while (0)

namespace {
constexpr uint32_t kHeap = 0x14000000, kVram = kVramBase;

struct Fx {
    GuestMemory gm;
    Gpu gpu{nullptr};
    Fx() : gpu(nullptr) {
        gm.map(kHeap, 0x400000, GuestMemory::Read | GuestMemory::Write);
        gm.map(kVram, 0x100000, GuestMemory::Read | GuestMemory::Write);
        gpu = Gpu(gm.base());
    }
    uint8_t *at(uint32_t a) { return gm.base() + a; }
    uint32_t w32(uint32_t a) {
        uint32_t v;
        std::memcpy(&v, at(a), 4);
        return v;
    }
};
void put32(uint8_t *p, uint32_t v) { std::memcpy(p, &v, 4); }
} // namespace

int main() {
    // --- Morton order inside a tile: x in the even bits, y in the odd bits ---
    CHECK(mortonOffset(0, 0) == 0 && mortonOffset(1, 0) == 1 && mortonOffset(0, 1) == 2 &&
          mortonOffset(1, 1) == 3 && mortonOffset(2, 0) == 4 && mortonOffset(0, 2) == 8 &&
          mortonOffset(4, 0) == 16 && mortonOffset(0, 4) == 32 && mortonOffset(7, 7) == 63);
    {
        std::set<unsigned> seen;
        for (unsigned y = 0; y < 8; y++)
            for (unsigned x = 0; x < 8; x++)
                seen.insert(mortonOffset(x, y));
        CHECK(seen.size() == 64 && *seen.rbegin() == 63);
    }

    // --- pixel formats ---
    {
        uint8_t b[4];
        encodePixel(b, PixFmt::RGBA8, Rgba{0x11, 0x22, 0x33, 0x44});
        CHECK(b[0] == 0x44 && b[1] == 0x33 && b[2] == 0x22 && b[3] == 0x11); // u32 = RRGGBBAA little-endian
        Rgba c = decodePixel(b, PixFmt::RGBA8);
        CHECK(c.r == 0x11 && c.g == 0x22 && c.b == 0x33 && c.a == 0x44);
        encodePixel(b, PixFmt::RGB8, Rgba{1, 2, 3, 255});
        CHECK(b[0] == 3 && b[1] == 2 && b[2] == 1); // stored B, G, R
        encodePixel(b, PixFmt::RGB565, Rgba{255, 0, 255, 255});
        CHECK((b[0] | b[1] << 8) == 0xF81F);
        c = decodePixel(b, PixFmt::RGB565);
        CHECK(c.r == 255 && c.g == 0 && c.b == 255);
        encodePixel(b, PixFmt::RGB5A1, Rgba{255, 255, 255, 255});
        CHECK((b[0] | b[1] << 8) == 0xFFFF);
        encodePixel(b, PixFmt::RGBA4, Rgba{255, 0, 0, 255});
        CHECK((b[0] | b[1] << 8) == 0xF00F);
        for (PixFmt f : {PixFmt::RGB565, PixFmt::RGB5A1, PixFmt::RGBA4}) { // quantisation is idempotent
            encodePixel(b, f, Rgba{100, 150, 200, 255});
            const Rgba once = decodePixel(b, f);
            uint8_t b2[4];
            encodePixel(b2, f, once);
            CHECK(std::memcmp(b, b2, 2) == 0);
        }
    }

    // --- memory fill ---
    {
        Fx f;
        CHECK(f.gpu.memoryFill(kVram, kVram + 16, 0x44332211, 0x0201)); // 32-bit
        CHECK(f.w32(kVram) == 0x44332211 && f.w32(kVram + 12) == 0x44332211);
        CHECK(f.gpu.memoryFill(kVram + 0x100, kVram + 0x106, 0x00CCBBAA, 0x0101)); // 24-bit, two pixels
        CHECK(f.at(kVram + 0x100)[0] == 0xAA && f.at(kVram + 0x100)[2] == 0xCC &&
              f.at(kVram + 0x100)[3] == 0xAA);
        CHECK(f.gpu.memoryFill(kVram + 0x200, kVram + 0x204, 0xBEEF, 0x0001)); // 16-bit
        CHECK(f.at(kVram + 0x200)[0] == 0xEF && f.at(kVram + 0x200)[1] == 0xBE &&
              f.at(kVram + 0x200)[2] == 0xEF);
        CHECK(f.gpu.memoryFill(kVram + 0x300, kVram + 0x310, 0xFFFFFFFF, 0x0200)); // start bit clear: no-op
        CHECK(f.w32(kVram + 0x300) == 0);
        CHECK(!f.gpu.memoryFill(0x00000010, 0x00000020, 1, 1));                  // below application memory
        CHECK(!f.gpu.memoryFill(kVram + 0x5FFFF0, kVram + 0x600010, 1, 0x0201)); // past the end of VRAM
        CHECK(f.gpu.stats().fills == 3 && f.gpu.stats().rejected == 2);
    }

    // --- display transfer ---
    {
        Fx f;
        // Linear 16x8 RGBA8 source, pixel (x, y) = r:x g:y b:0 a:255.
        for (unsigned y = 0; y < 8; y++)
            for (unsigned x = 0; x < 16; x++)
                encodePixel(f.at(kHeap + (y * 16 + x) * 4), PixFmt::RGBA8,
                            Rgba{uint8_t(x), uint8_t(y), 0, 255});
        // linear -> tiled, same format. Tile 0 holds x 0..7, tile 1 holds x 8..15; 64 pixels each.
        CHECK(f.gpu.displayTransfer(kHeap, kHeap + 0x1000, 8 << 16 | 16, 8 << 16 | 16, 2));
        auto tiled = [&](unsigned x, unsigned y) {
            return decodePixel(f.at(kHeap + 0x1000 + ((x / 8) * 64 + mortonOffset(x % 8, y % 8)) * 4),
                               PixFmt::RGBA8);
        };
        CHECK(tiled(1, 0).r == 1 && tiled(1, 0).g == 0);
        // pixel index 2 of tile 0 is (0, 1); pixel index 64 is (8, 0)
        CHECK(decodePixel(f.at(kHeap + 0x1000 + 2 * 4), PixFmt::RGBA8).g == 1);
        CHECK(decodePixel(f.at(kHeap + 0x1000 + 64 * 4), PixFmt::RGBA8).r == 8);
        CHECK(decodePixel(f.at(kHeap + 0x1000 + 63 * 4), PixFmt::RGBA8).r == 7 &&
              decodePixel(f.at(kHeap + 0x1000 + 63 * 4), PixFmt::RGBA8).g == 7);
        // tiled -> linear (the default mode) restores the source exactly.
        CHECK(f.gpu.displayTransfer(kHeap + 0x1000, kHeap + 0x2000, 8 << 16 | 16, 8 << 16 | 16, 0));
        CHECK(std::memcmp(f.at(kHeap), f.at(kHeap + 0x2000), 16 * 8 * 4) == 0);
        // RGBA8 -> RGB8 with crop (output 8 wide from a 16 wide input) and vertical flip.
        CHECK(f.gpu.displayTransfer(kHeap + 0x1000, kHeap + 0x3000, 8 << 16 | 16, 8 << 16 | 8,
                                    1 | 4 | 1 << 12));
        for (unsigned y = 0; y < 8; y++)
            for (unsigned x = 0; x < 8; x++) {
                const Rgba c = decodePixel(f.at(kHeap + 0x3000 + (y * 8 + x) * 3), PixFmt::RGB8);
                CHECK(c.r == x && c.g == 7 - y);
            }
        // 2x2 downscale averages: 16x8 -> 8x4.
        CHECK(f.gpu.displayTransfer(kHeap + 0x1000, kHeap + 0x4000, 8 << 16 | 16, 4 << 16 | 8, 2 << 24));
        {
            // output is linear (tiled->linear): pixel (0,0) averages (0,0),(1,0),(0,1),(1,1): r=(0+1+0+1)/4=0
            const Rgba c = decodePixel(f.at(kHeap + 0x4000), PixFmt::RGBA8);
            CHECK(c.r == 0 && c.g == 0);
            const Rgba d = decodePixel(f.at(kHeap + 0x4000 + (1 * 8 + 3) * 4), PixFmt::RGBA8);
            CHECK(d.r == (6 + 7 + 6 + 7) / 4 && d.g == (2 + 2 + 3 + 3) / 4);
        }
        // The game programs a downscale with output dimensions equal to the input's (bloom chain): the
        // result is the same half-size image.
        CHECK(f.gpu.displayTransfer(kHeap + 0x1000, kHeap + 0x5000, 8 << 16 | 16, 8 << 16 | 16, 2 << 24));
        CHECK(std::memcmp(f.at(kHeap + 0x4000), f.at(kHeap + 0x5000), 8 * 4 * 4) == 0);
        // 32x32 block layout round-trips too.
        std::vector<uint8_t> big(32 * 32 * 4);
        for (unsigned i = 0; i < 32 * 32; i++)
            put32(&big[i * 4], 0x01010101u * (i & 0xFF) ^ i);
        std::memcpy(f.at(kHeap + 0x10000), big.data(), big.size());
        CHECK(f.gpu.displayTransfer(kHeap + 0x10000, kHeap + 0x11000, 32 << 16 | 32, 32 << 16 | 32,
                                    2 | 0x10000));
        CHECK(f.gpu.displayTransfer(kHeap + 0x11000, kHeap + 0x12000, 32 << 16 | 32, 32 << 16 | 32, 0x10000));
        CHECK(std::memcmp(f.at(kHeap + 0x12000), big.data(), big.size()) == 0);
        // rejected: bad format, out-of-range memory, tiled size not a multiple of 8, upscaling
        CHECK(!f.gpu.displayTransfer(kHeap, kHeap + 0x100, 8 << 16 | 8, 8 << 16 | 8, 5 << 8));
        CHECK(!f.gpu.displayTransfer(0x30000000, kHeap, 8 << 16 | 8, 8 << 16 | 8, 0));
        CHECK(!f.gpu.displayTransfer(kHeap, kHeap + 0x100, 9 << 16 | 9, 9 << 16 | 9, 0));
        CHECK(!f.gpu.displayTransfer(kHeap, kHeap + 0x100, 8 << 16 | 8, 16 << 16 | 16, 0));
    }

    // --- texture copy and DMA ---
    {
        Fx f;
        for (unsigned i = 0; i < 64; i++)
            f.at(kHeap)[i] = uint8_t(i);
        // 4 lines of 8 bytes -> lines of 8 bytes with a 8-byte gap after each on the destination
        CHECK(f.gpu.textureCopy(kHeap, kHeap + 0x100, 32, 8, 0, 8, 8));
        CHECK(f.at(kHeap + 0x100)[0] == 0 && f.at(kHeap + 0x100)[7] == 7);
        CHECK(f.at(kHeap + 0x100)[16] == 8 && f.at(kHeap + 0x100)[23] == 15);
        CHECK(f.at(kHeap + 0x100)[8] == 0); // the gap is untouched
        CHECK(f.at(kHeap + 0x100)[48] == 24);
        CHECK(f.gpu.dma(kHeap, kHeap + 0x400, 64) && f.at(kHeap + 0x400)[63] == 63);
        CHECK(!f.gpu.dma(kHeap, 0x00000100, 64));
    }

    // --- screen capture: columns of 240 pixels, bottom pixel first ---
    {
        Fx f;
        Gpu::Screen s;
        s.addr = kHeap;
        s.stride = 240 * 3;
        s.fmt = PixFmt::RGB8;
        s.valid = true;
        f.gpu.setScreen(kTopScreen, s);
        // screen pixel (x=5, y=0) is the LAST pixel of column 5; (x=5, y=239) the first
        encodePixel(f.at(kHeap + 5 * 720 + 239 * 3), PixFmt::RGB8, Rgba{10, 20, 30, 255});
        encodePixel(f.at(kHeap + 5 * 720 + 0), PixFmt::RGB8, Rgba{40, 50, 60, 255});
        Image img;
        CHECK(f.gpu.capture(kTopScreen, &img) && img.width == 400 && img.height == 240);
        CHECK(img.rgba[(0 * 400 + 5) * 4] == 10 && img.rgba[(0 * 400 + 5) * 4 + 2] == 30);
        CHECK(img.rgba[(239 * 400 + 5) * 4] == 40 && img.rgba[(239 * 400 + 5) * 4 + 1] == 50);
        CHECK(!f.gpu.capture(kBottomScreen, &img)); // never configured
        s.stride = 100;                             // too small for 240 pixels
        f.gpu.setScreen(kBottomScreen, s);
        CHECK(!f.gpu.capture(kBottomScreen, &img));
    }

    std::printf(g_fail ? "FAIL\n" : "gpu ok\n");
    return g_fail ? 1 : 0;
}
