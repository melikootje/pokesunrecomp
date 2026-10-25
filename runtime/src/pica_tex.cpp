// PICA200 texture decoding and sampling (formats: public 3dbrew documentation).
#include <algorithm>
#include <cmath>
#include <cstring>

#include "pokerecomp/gpu.h"
#include "pokerecomp/pica.h"

namespace pokerecomp {
namespace pica {

namespace {

float to01(unsigned v) { return float(v) / 255.0f; }
unsigned expand4(unsigned v) { return v * 17; }
unsigned expand5(unsigned v) { return (v << 3) | (v >> 2); }

void setRgba(float *o, unsigned r, unsigned g, unsigned b, unsigned a) {
    o[0] = to01(r);
    o[1] = to01(g);
    o[2] = to01(b);
    o[3] = to01(a);
}

// ETC1: the PICA stores each 64-bit block byte-reversed relative to the standard (big-endian) description.
struct EtcBlock {
    uint8_t rgb[16][3]; // indexed by x * 4 + y
};

int sign3(unsigned v) { return v >= 4 ? int(v) - 8 : int(v); }

void decodeEtcBlock(const uint8_t *mem, EtcBlock *out) {
    uint8_t s[8];
    for (int i = 0; i < 8; i++)
        s[i] = mem[7 - i];
    static const int kTable[8][2] = {{2, 8},   {5, 17},  {9, 29},   {13, 42},
                                     {18, 60}, {24, 80}, {33, 106}, {47, 183}};
    const bool diff = (s[3] >> 1) & 1, flip = s[3] & 1;
    const unsigned t1 = (s[3] >> 5) & 7, t2 = (s[3] >> 2) & 7;
    int base[2][3];
    if (diff) {
        for (int c = 0; c < 3; c++) {
            const int a = s[c] >> 3, b = a + sign3(s[c] & 7);
            base[0][c] = int(expand5(unsigned(a)));
            base[1][c] = int(expand5(unsigned(std::clamp(b, 0, 31))));
        }
    } else {
        for (int c = 0; c < 3; c++) {
            base[0][c] = int(expand4(s[c] >> 4));
            base[1][c] = int(expand4(s[c] & 15));
        }
    }
    const uint32_t idx = uint32_t(s[4]) << 24 | uint32_t(s[5]) << 16 | uint32_t(s[6]) << 8 | s[7];
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
            const int k = x * 4 + y;
            const unsigned msb = (idx >> (k + 16)) & 1, lsb = (idx >> k) & 1;
            const int sub = flip ? (y >= 2) : (x >= 2);
            const int *tab = kTable[sub ? t2 : t1];
            const int mod = (msb ? -1 : 1) * tab[lsb];
            for (int c = 0; c < 3; c++)
                out->rgb[k][c] = uint8_t(std::clamp(base[sub][c] + mod, 0, 255));
        }
}

} // namespace

bool fetchTexel(const TexInfo &t, int x, int y, float *o) {
    const unsigned f = t.format;
    if (f == 12 || f == 13) { // ETC1 / ETC1A4
        const unsigned blockBytes = f == 12 ? 8 : 16;
        const uint64_t tile = uint64_t(y / 8) * (t.width / 8) + x / 8;
        const uint64_t block = tile * 4 + ((y % 8) / 4) * 2 + (x % 8) / 4;
        const uint8_t *p = t.data + block * blockBytes;
        EtcBlock b;
        decodeEtcBlock(p + (f == 13 ? 8 : 0), &b);
        const int k = (x & 3) * 4 + (y & 3);
        unsigned a = 255;
        if (f == 13) {
            const unsigned pix = unsigned(k); // alpha nibbles, little-endian, same column-major order
            a = expand4((p[pix / 2] >> (4 * (pix & 1))) & 0xF);
        }
        setRgba(o, b.rgb[k][0], b.rgb[k][1], b.rgb[k][2], a);
        return true;
    }
    const unsigned pix = unsigned(tiledOffset(unsigned(x), unsigned(y), t.width, 1)); // pixel index in memory
    switch (f) {
    case 0: {
        const Rgba c = decodePixel(t.data + pix * 4, PixFmt::RGBA8);
        setRgba(o, c.r, c.g, c.b, c.a);
        return true;
    }
    case 1: {
        const Rgba c = decodePixel(t.data + pix * 3, PixFmt::RGB8);
        setRgba(o, c.r, c.g, c.b, 255);
        return true;
    }
    case 2:
    case 3:
    case 4: {
        const Rgba c = decodePixel(t.data + pix * 2, f == 2   ? PixFmt::RGB5A1
                                                     : f == 3 ? PixFmt::RGB565
                                                              : PixFmt::RGBA4);
        setRgba(o, c.r, c.g, c.b, c.a);
        return true;
    }
    case 5: { // IA8: byte 1 intensity, byte 0 alpha
        const uint8_t *p = t.data + pix * 2;
        setRgba(o, p[1], p[1], p[1], p[0]);
        return true;
    }
    case 6: { // HILO8: two 8-bit channels (normal maps)
        const uint8_t *p = t.data + pix * 2;
        setRgba(o, p[1], p[0], 0, 255);
        return true;
    }
    case 7:
        setRgba(o, t.data[pix], t.data[pix], t.data[pix], 255);
        return true;
    case 8:
        setRgba(o, 0, 0, 0, t.data[pix]);
        return true;
    case 9: { // IA44: intensity in the high nibble, alpha in the low nibble
        const unsigned v = t.data[pix], i = expand4(v >> 4), a = expand4(v & 15);
        setRgba(o, i, i, i, a);
        return true;
    }
    case 10: { // I4
        const unsigned i = expand4((t.data[pix / 2] >> (4 * (pix & 1))) & 15);
        setRgba(o, i, i, i, 255);
        return true;
    }
    case 11: { // A4
        const unsigned a = expand4((t.data[pix / 2] >> (4 * (pix & 1))) & 15);
        setRgba(o, 0, 0, 0, a);
        return true;
    }
    default:
        return false;
    }
}

namespace {

// Returns the coordinate to read, or -1 to read the border colour.
int wrapCoord(int c, int size, unsigned mode) {
    switch (mode) {
    case 0: // clamp to edge
        return std::clamp(c, 0, size - 1);
    case 1: // clamp to border
        return (c < 0 || c >= size) ? -1 : c;
    case 3: { // mirrored repeat
        const int period = 2 * size;
        int m = ((c % period) + period) % period;
        return m < size ? m : period - 1 - m;
    }
    default: // repeat (2, and undocumented values)
        return ((c % size) + size) % size;
    }
}

} // namespace

bool sampleTexture(const TexInfo &t, float u, float v, float *out) {
    if (!t.data || !t.width || !t.height)
        return false;
    const int w = int(t.width), h = int(t.height);
    // t = 0 is the bottom of the image and memory stores the image top row first (verified on the game's font
    // atlas: its glyph rows sit at v ~ 1 while the glyphs are in the first rows of memory), so row = height -
    // 1 - t.
    auto tap = [&](int x, int y, float *o) {
        const int wx = wrapCoord(x, w, t.wrapS), wy = wrapCoord(y, h, t.wrapT);
        if (wx < 0 || wy < 0) {
            std::memcpy(o, t.border, 4 * sizeof(float));
            return true;
        }
        return fetchTexel(t, wx, h - 1 - wy, o);
    };
    if (!t.magLinear) {
        return tap(int(std::floor(u * float(w))), int(std::floor(v * float(h))), out);
    }
    const float fx = u * float(w) - 0.5f, fy = v * float(h) - 0.5f;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float ax = fx - float(x0), ay = fy - float(y0);
    float c[4][4];
    if (!tap(x0, y0, c[0]) || !tap(x0 + 1, y0, c[1]) || !tap(x0, y0 + 1, c[2]) || !tap(x0 + 1, y0 + 1, c[3]))
        return false;
    for (int k = 0; k < 4; k++)
        out[k] = (c[0][k] * (1 - ax) + c[1][k] * ax) * (1 - ay) + (c[2][k] * (1 - ax) + c[3][k] * ax) * ay;
    return true;
}

} // namespace pica
} // namespace pokerecomp
