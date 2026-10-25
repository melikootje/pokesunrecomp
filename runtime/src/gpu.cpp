#include "pokerecomp/gpu.h"

#include <algorithm>
#include <cstring>

namespace pokerecomp {

namespace {
uint8_t expand5(unsigned v) { return uint8_t((v << 3) | (v >> 2)); }
uint8_t expand6(unsigned v) { return uint8_t((v << 2) | (v >> 4)); }
uint8_t expand4(unsigned v) { return uint8_t(v * 17); }
} // namespace

Rgba decodePixel(const uint8_t *p, PixFmt f) {
    Rgba c;
    switch (f) {
    case PixFmt::RGBA8: // little-endian u32 = R<<24 | G<<16 | B<<8 | A
        c.a = p[0];
        c.b = p[1];
        c.g = p[2];
        c.r = p[3];
        break;
    case PixFmt::RGB8:
        c.b = p[0];
        c.g = p[1];
        c.r = p[2];
        break;
    case PixFmt::RGB565: {
        const unsigned v = p[0] | p[1] << 8;
        c.r = expand5(v >> 11);
        c.g = expand6((v >> 5) & 0x3F);
        c.b = expand5(v & 0x1F);
        break;
    }
    case PixFmt::RGB5A1: {
        const unsigned v = p[0] | p[1] << 8;
        c.r = expand5(v >> 11);
        c.g = expand5((v >> 6) & 0x1F);
        c.b = expand5((v >> 1) & 0x1F);
        c.a = (v & 1) ? 255 : 0;
        break;
    }
    case PixFmt::RGBA4: {
        const unsigned v = p[0] | p[1] << 8;
        c.r = expand4(v >> 12);
        c.g = expand4((v >> 8) & 0xF);
        c.b = expand4((v >> 4) & 0xF);
        c.a = expand4(v & 0xF);
        break;
    }
    }
    return c;
}

void encodePixel(uint8_t *p, PixFmt f, Rgba c) {
    switch (f) {
    case PixFmt::RGBA8:
        p[0] = c.a;
        p[1] = c.b;
        p[2] = c.g;
        p[3] = c.r;
        break;
    case PixFmt::RGB8:
        p[0] = c.b;
        p[1] = c.g;
        p[2] = c.r;
        break;
    case PixFmt::RGB565: {
        const unsigned v = (c.r >> 3) << 11 | (c.g >> 2) << 5 | (c.b >> 3);
        p[0] = uint8_t(v);
        p[1] = uint8_t(v >> 8);
        break;
    }
    case PixFmt::RGB5A1: {
        const unsigned v = (c.r >> 3) << 11 | (c.g >> 3) << 6 | (c.b >> 3) << 1 | (c.a >= 128 ? 1u : 0u);
        p[0] = uint8_t(v);
        p[1] = uint8_t(v >> 8);
        break;
    }
    case PixFmt::RGBA4: {
        const unsigned v = (c.r >> 4) << 12 | (c.g >> 4) << 8 | (c.b >> 4) << 4 | (c.a >> 4);
        p[0] = uint8_t(v);
        p[1] = uint8_t(v >> 8);
        break;
    }
    }
}

unsigned mortonOffset(unsigned x, unsigned y) {
    return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
}

bool Gpu::accessible(uint32_t addr, uint32_t size) {
    const uint64_t end = uint64_t(addr) + size;
    const bool app = addr >= 0x00100000 && end <= 0x1E000000;
    const bool vram = addr >= kVramBase && end <= uint64_t(kVramBase) + kVramSize;
    return app || vram;
}

bool Gpu::physToVirt(uint32_t paddr, uint32_t *vaddr) {
    if (paddr >= 0x18000000 && paddr < 0x18000000 + kVramSize) {
        *vaddr = paddr + (kVramBase - 0x18000000);
        return true;
    }
    if (paddr >= 0x20000000 && paddr < 0x28000000) {
        *vaddr = paddr - 0x0C000000;
        return true;
    }
    return false;
}

bool Gpu::memoryFill(uint32_t start, uint32_t end, uint32_t value, uint32_t control) {
    if (!(control & 1))
        return true; // not started
    if (end < start || !accessible(start, end - start)) {
        stats_.rejected++;
        return false;
    }
    const unsigned width = (control >> 8) & 3; // 0: 16-bit, 1/3: 24-bit, 2: 32-bit
    const unsigned step = width == 0 ? 2 : width == 2 ? 4 : 3;
    uint8_t pat[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
    uint8_t *p = mem_ + start;
    const uint32_t n = end - start;
    for (uint32_t i = 0; i + step <= n; i += step)
        std::memcpy(p + i, pat, step);
    stats_.fills++;
    return true;
}

bool Gpu::displayTransfer(uint32_t src, uint32_t dst, uint32_t inDim, uint32_t outDim, uint32_t flags) {
    const bool flip = flags & 1, linearToTiled = flags & 2, tiledToTiled = flags & 0x20;
    const bool block32 = flags & 0x10000;
    const unsigned inFmtV = (flags >> 8) & 7, outFmtV = (flags >> 12) & 7, scale = (flags >> 24) & 3;
    unsigned inW = inDim & 0xFFFF, inH = inDim >> 16, outW = outDim & 0xFFFF, outH = outDim >> 16;
    if (inFmtV > 4 || outFmtV > 4 || scale == 3 || !inW || !inH || !outW || !outH) {
        stats_.rejected++;
        return false;
    }
    const PixFmt inFmt = PixFmt(inFmtV), outFmt = PixFmt(outFmtV);
    const unsigned sx = scale ? 2 : 1, sy = scale == 2 ? 2 : 1; // source pixels per output pixel
    // Both sides are laid out in `width` pixels per row (tiled sides in 8x8 or 32x32 blocks of that width).
    const bool inTiled = !linearToTiled || tiledToTiled, outTiled = linearToTiled || tiledToTiled;
    if (outW * sx > inW || outH * sy > inH) {
        stats_.rejected++;
        return false;
    }
    const unsigned ib = bytesPerPixel(inFmt), ob = bytesPerPixel(outFmt);
    const uint64_t inBytes = uint64_t(inW) * inH * ib, outBytes = uint64_t(outW) * outH * ob;
    if (!accessible(src, uint32_t(inBytes)) || !accessible(dst, uint32_t(outBytes)) || inBytes > 0x1000000 ||
        outBytes > 0x1000000) {
        stats_.rejected++;
        return false;
    }
    // A 32x32-block layout is 32x32 blocks, row-major, of 8x8 tiles in row-major order inside the block.
    auto offset = [&](unsigned x, unsigned y, unsigned w, unsigned bpp, bool tiled) -> uint64_t {
        if (!tiled)
            return (uint64_t(y) * w + x) * bpp;
        uint64_t tile;
        if (block32) {
            const uint64_t block = uint64_t(y / 32) * (w / 32) + x / 32;
            tile = block * 16 + ((y % 32) / 8) * 4 + (x % 32) / 8;
        } else {
            tile = uint64_t(y / 8) * (w / 8) + x / 8;
        }
        return (tile * 64 + mortonOffset(x & 7, y & 7)) * bpp;
    };
    if ((inTiled && (inW % 8 || inH % 8)) || (outTiled && (outW % 8 || outH % 8))) {
        stats_.rejected++;
        return false;
    }
    for (unsigned y = 0; y < outH; y++) {
        const unsigned srcRow = flip ? outH * sy - 1 - y * sy : y * sy;
        for (unsigned x = 0; x < outW; x++) {
            unsigned r = 0, g = 0, b = 0, a = 0;
            for (unsigned j = 0; j < sy; j++)
                for (unsigned i = 0; i < sx; i++) {
                    const unsigned yy = flip ? srcRow - j : srcRow + j;
                    const Rgba c = decodePixel(mem_ + src + offset(x * sx + i, yy, inW, ib, inTiled), inFmt);
                    r += c.r;
                    g += c.g;
                    b += c.b;
                    a += c.a;
                }
            const unsigned n = sx * sy;
            encodePixel(mem_ + dst + offset(x, y, outW, ob, outTiled), outFmt,
                        Rgba{uint8_t(r / n), uint8_t(g / n), uint8_t(b / n), uint8_t(a / n)});
        }
    }
    stats_.transfers++;
    return true;
}

bool Gpu::textureCopy(uint32_t src, uint32_t dst, uint32_t size, uint32_t inLine, uint32_t inGap,
                      uint32_t outLine, uint32_t outGap) {
    if (!size || !inLine || !outLine || size > 0x1000000) {
        stats_.rejected++;
        return false;
    }
    // Number of lines is implied by the size and the line width; the gaps stretch the span on each side.
    const uint32_t lines = (size + inLine - 1) / inLine;
    const uint64_t inSpan = uint64_t(lines) * (inLine + inGap),
                   outSpan = uint64_t(lines) * (outLine + outGap);
    if (inSpan > 0x2000000 || outSpan > 0x2000000 || !accessible(src, uint32_t(inSpan)) ||
        !accessible(dst, uint32_t(outSpan))) {
        stats_.rejected++;
        return false;
    }
    // Copy as a byte stream: read `inLine` bytes then skip `inGap`; write `outLine` then skip `outGap`.
    uint32_t remaining = size, ip = 0, op = 0, inLeft = inLine, outLeft = outLine;
    while (remaining) {
        const uint32_t n = std::min({remaining, inLeft, outLeft});
        std::memmove(mem_ + dst + op, mem_ + src + ip, n);
        remaining -= n;
        ip += n;
        op += n;
        inLeft -= n;
        outLeft -= n;
        if (!inLeft) {
            ip += inGap;
            inLeft = inLine;
        }
        if (!outLeft) {
            op += outGap;
            outLeft = outLine;
        }
    }
    stats_.copies++;
    return true;
}

bool Gpu::dma(uint32_t src, uint32_t dst, uint32_t size) {
    if (!size || !accessible(src, size) || !accessible(dst, size)) {
        stats_.rejected++;
        return false;
    }
    std::memmove(mem_ + dst, mem_ + src, size);
    stats_.dmas++;
    return true;
}

bool Gpu::capture(int screenIndex, Image *out) const {
    const Screen &s = screens_[screenIndex & 1];
    const unsigned w = screenIndex == kTopScreen ? 400 : 320, h = 240;
    const unsigned bpp = bytesPerPixel(s.fmt);
    if (!s.valid || s.stride < h * bpp || !accessible(s.addr, uint32_t(uint64_t(s.stride) * w)))
        return false;
    out->width = w;
    out->height = h;
    out->rgba.assign(size_t(w) * h * 4, 0);
    for (unsigned x = 0; x < w; x++)
        for (unsigned y = 0; y < h; y++) {
            const Rgba c =
                decodePixel(mem_ + s.addr + uint64_t(x) * s.stride + uint64_t(h - 1 - y) * bpp, s.fmt);
            uint8_t *d = &out->rgba[(size_t(y) * w + x) * 4];
            d[0] = c.r;
            d[1] = c.g;
            d[2] = c.b;
            d[3] = 255;
        }
    return true;
}

} // namespace pokerecomp
