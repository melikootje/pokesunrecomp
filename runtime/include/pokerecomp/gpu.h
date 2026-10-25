#pragma once
// PICA200 / GX engine (issue #8, GPU). This file covers the fixed-function transfer engine driven by GX
// commands: memory fills, display transfers (tiling, format conversion, scaling), texture copies, DMA and
// capture of the screens the LCD controllers scan out. Addresses are guest virtual addresses; every access is
// range-checked before it touches host memory.
#include <cstdint>
#include <vector>

#include "pokerecomp/pica.h"

namespace pokerecomp {

enum class PixFmt : uint8_t { RGBA8 = 0, RGB8 = 1, RGB565 = 2, RGB5A1 = 3, RGBA4 = 4 };
constexpr unsigned bytesPerPixel(PixFmt f) { return f == PixFmt::RGBA8 ? 4 : f == PixFmt::RGB8 ? 3 : 2; }
struct Rgba {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};
Rgba decodePixel(const uint8_t *p, PixFmt f);
void encodePixel(uint8_t *p, PixFmt f, Rgba c);
// Offset (in pixels) of (x, y) inside an 8x8 Morton-ordered tile, x in the even bits.
unsigned mortonOffset(unsigned x, unsigned y);

// Byte offset of pixel (x, y) in a GPU-tiled surface `w` pixels wide (8x8 tiles in Morton order, tiles
// row-major; with `block32` the tiles are grouped in row-major 32x32 blocks).
inline uint64_t tiledOffset(unsigned x, unsigned y, unsigned w, unsigned bpp, bool block32 = false) {
    uint64_t tile;
    if (block32) {
        tile = (uint64_t(y / 32) * (w / 32) + x / 32) * 16 + ((y % 32) / 8) * 4 + (x % 32) / 8;
    } else {
        tile = uint64_t(y / 8) * (w / 8) + x / 8;
    }
    return (tile * 64 + mortonOffset(x & 7, y & 7)) * bpp;
}

constexpr uint32_t kVramBase = 0x1F000000, kVramSize = 0x600000;
constexpr int kTopScreen = 0, kBottomScreen = 1;

struct Image {
    unsigned width = 0, height = 0;
    std::vector<uint8_t> rgba; // width * height * 4, row-major from the top-left
};

struct GxStats {
    uint64_t fills = 0, transfers = 0, copies = 0, dmas = 0, rejected = 0;
};

class Gpu {
  public:
    // `mem` is the host address of guest address 0.
    explicit Gpu(uint8_t *mem) : mem_(mem), pica_(mem) {}

    // GX SetMemoryFill: one unit. control = the 16-bit control word (bit 0 start, bits 9:8 width:
    // 0 = 16-bit, 1/3 = 24-bit, 2 = 32-bit). Fills [start, end).
    bool memoryFill(uint32_t start, uint32_t end, uint32_t value, uint32_t control);
    // GX SetDisplayTransfer. Dimensions are (height << 16 | width). See docs/gpu.md for the flag bits.
    bool displayTransfer(uint32_t src, uint32_t dst, uint32_t inDim, uint32_t outDim, uint32_t flags);
    // GX SetTextureCopy: copies `size` bytes in lines of `lineWidth` bytes, skipping `gap` bytes after every
    // line on the side that has one (both given in bytes here; the caller converts the 16-byte units).
    bool textureCopy(uint32_t src, uint32_t dst, uint32_t size, uint32_t inLine, uint32_t inGap,
                     uint32_t outLine, uint32_t outGap);
    bool dma(uint32_t src, uint32_t dst, uint32_t size);

    struct Screen {
        uint32_t addr = 0, stride = 0;
        PixFmt fmt = PixFmt::RGB8;
        bool valid = false;
    };
    // The framebuffer the LCD controller scans out (from the GSP framebuffer-info structure).
    void setScreen(int screen, const Screen &s) { screens_[screen & 1] = s; }
    const Screen &screen(int s) const { return screens_[s & 1]; }
    // Reads the scanned-out framebuffer as an upright image (400x240 top, 320x240 bottom). The panels are
    // mounted rotated: memory holds columns, `stride` bytes apart, each 240 pixels from the bottom up.
    bool capture(int screen, Image *out) const;

    const GxStats &stats() const { return stats_; }
    pica::Pica &pica() { return pica_; }
    // True if [addr, addr + size) lies in memory the GPU may touch (application memory or VRAM).
    static bool accessible(uint32_t addr, uint32_t size);
    // The 3D engine addresses memory physically: VRAM at 0x18000000 and FCRAM at 0x20000000 appear to the
    // process at 0x1F000000 and (the linear heap) 0x14000000. Returns false for any other physical address.
    static bool physToVirt(uint32_t paddr, uint32_t *vaddr);

  private:
    uint8_t *mem_;
    pica::Pica pica_;
    Screen screens_[2];
    GxStats stats_;
};

} // namespace pokerecomp
