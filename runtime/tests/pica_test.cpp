// PICA200 pipeline: command lists, uniform/code upload, vertex fetch, rasterization, texturing, TEV,
// blending, depth and culling, on synthetic scenes (no game data).
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "pokerecomp/gpu.h"
#include "pokerecomp/guest_memory.h"
#include "pokerecomp/pica.h"

using namespace pokerecomp;
using namespace pokerecomp::pica;

static int g_fail = 0;
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c);                        \
            g_fail++;                                                                                        \
        }                                                                                                    \
    } while (0)

namespace {

constexpr uint32_t kVirtVram = kVramBase, kPhysVram = 0x18000000;
constexpr uint32_t kVirtFcram = 0x14000000, kPhysFcram = 0x20000000;
constexpr uint32_t kListAt = kVirtFcram + 0x100000;

struct List {
    std::vector<uint32_t> w;
    void add(uint32_t id, uint32_t value, uint32_t mask = 0xF) {
        w.push_back(value);
        w.push_back(id | mask << 16);
    }
    void multi(uint32_t id, const std::vector<uint32_t> &vals, bool consecutive) {
        w.push_back(vals[0]);
        w.push_back(id | 0xF << 16 | uint32_t(vals.size() - 1) << 20 | (consecutive ? 1u << 31 : 0));
        for (size_t i = 1; i < vals.size(); i++)
            w.push_back(vals[i]);
        if (vals.size() % 2 == 0) // header + params must be 8-byte padded: (1 + n) words
            w.push_back(0);
    }
};

uint32_t fbits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
uint32_t f24(float f) { return f32ToF24(f); }

// pack four floats into the three-word float24 layout
void packF24(float x, float y, float z, float w, uint32_t out[3]) {
    const uint32_t X = f24(x), Y = f24(y), Z = f24(z), W = f24(w);
    out[0] = W << 8 | (Z >> 16);
    out[1] = (Z & 0xFFFF) << 16 | (Y >> 8);
    out[2] = (Y & 0xFF) << 24 | X;
}

struct Fx {
    GuestMemory gm;
    Gpu gpu{nullptr};
    Fx() {
        gm.map(kVirtFcram, 0x800000, GuestMemory::Read | GuestMemory::Write);
        gm.map(kVirtVram, 0x600000, GuestMemory::Read | GuestMemory::Write);
        gpu = Gpu(gm.base());
    }
    Pica &p() { return gpu.pica(); }
    void run(const List &l) {
        std::memcpy(gm.base() + kListAt, l.w.data(), l.w.size() * 4);
        p().processCommandList(kListAt, uint32_t(l.w.size() * 4));
    }
    // Reads colour buffer pixel (x, y) of a tiled 32-bit buffer `w` pixels wide at virtual `base`.
    Rgba px(uint32_t base, unsigned w, unsigned x, unsigned y) {
        // rows are stored upside down (FB_DIM bit 24, as the game sets it): screen row y is memory row w - 1
        // - y
        return decodePixel(gm.base() + base + tiledOffset(x, w - 1 - y, w, 4), PixFmt::RGBA8);
    }
};

// --- shader assembly (same encodings the interpreter test uses) ---
constexpr uint32_t sel(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    return x << 6 | y << 4 | z << 2 | w;
}
constexpr uint32_t desc(uint32_t mask) {
    return mask | sel(0, 1, 2, 3) << 5 | sel(0, 1, 2, 3) << 14 | sel(0, 1, 2, 3) << 23;
}
constexpr uint32_t mov(uint32_t dst, uint32_t src) { return 0x13u << 26 | dst << 21 | src << 12; }
constexpr uint32_t kEnd = 0x22u << 26;

// Standard scene setup: a W x H RGBA8 target at the start of VRAM, depth in 24+8 right after it.
struct Scene {
    List l;
    uint32_t w, h;
    Scene(uint32_t W, uint32_t H, bool depth = false) : w(W), h(H) {
        l.add(FB_DIM, W | (H - 1) << 12 | 1u << 24);
        l.add(COLORBUF_LOC, kPhysVram >> 3);
        l.add(COLORBUF_FORMAT, 0);
        l.add(COLORBUF_READ, 0xF);
        l.add(COLORBUF_WRITE, 0xF);
        if (depth) {
            l.add(DEPTHBUF_LOC, (kPhysVram + 0x100000) >> 3);
            l.add(DEPTHBUF_FORMAT, 3);
            l.add(DEPTHBUF_WRITE, 3);
            l.add(DEPTH_COLOR_MASK, 0xF00 | 1 | 6 << 4 | 1 << 12); // test GREATER, write colour + depth
        } else {
            l.add(DEPTH_COLOR_MASK, 0xF00);
        }
        l.add(VIEWPORT_WIDTH, f24(W / 2.0f));
        l.add(VIEWPORT_HEIGHT, f24(H / 2.0f));
        l.add(VIEWPORT_XY, 0);
        l.add(DEPTHMAP_ENABLE, 1);
        l.add(DEPTHMAP_SCALE, f24(1.0f));
        l.add(DEPTHMAP_OFFSET, f24(0.0f));
        l.add(SCISSOR_MODE, 0);
        // shader: o0 = v0 (position), o1 = v1 (colour), o2 = v2 (texcoord)
        l.add(VSH_CODETRANSFER_CONFIG, 0);
        l.multi(VSH_CODETRANSFER_DATA, {mov(0, 0), mov(1, 1), mov(2, 2), kEnd}, true);
        l.add(VSH_OPDESCS_CONFIG, 0);
        l.add(VSH_OPDESCS_DATA, desc(0xF));
        l.add(VSH_ENTRYPOINT, 0x7F000000);
        l.add(VSH_OUTMAP_MASK, 7);
        l.add(OUTMAP_TOTAL, 3);
        l.add(OUTMAP_O0, 0x03020100);     // position
        l.add(OUTMAP_O0 + 1, 0x0B0A0908); // colour
        l.add(OUTMAP_O0 + 2, 0x1F1F0D0C); // texcoord0 u, v
        l.add(VSH_ATTR_PERM_LOW, 0x210);
        // vertex layout: v0 position, v1 colour, v2 texcoord (4 floats each, one interleaved buffer)
        l.add(ATTRIBBUF_LOC, (kPhysFcram + 0x200000) >> 3);
        l.add(ATTRIBBUF_FORMAT_LOW, 0xFFF);
        l.add(ATTRIBBUF_FORMAT_HIGH, 2u << 28);
        l.add(ATTRIBBUF0_OFFSET, 0);
        l.add(ATTRIBBUF0_OFFSET + 1, 0x210);
        l.add(ATTRIBBUF0_OFFSET + 2, 48u << 16 | 3u << 28);
        // TEV: stage 0 = replace with primary colour, the rest pass the previous result through
        l.add(TEXENV0, 0);
        l.add(TEXENV0 + 1, 0);
        l.add(TEXENV0 + 2, 0);
        l.add(TEXENV0 + 4, 0);
        for (uint32_t s : {TEXENV1, TEXENV2, TEXENV3, TEXENV4, TEXENV5}) {
            l.add(s, 0x0FFF0FFF); // colour and alpha sources 0-2 = previous (15)
            l.add(s + 1, 0);
            l.add(s + 2, 0); // replace
            l.add(s + 4, 0); // scale 1x
        }
        l.add(COLOR_OPERATION, 0); // logic op (copy), no blending
        l.add(LOGIC_OP, 3);
    }
    void vertices(Fx &f, const std::vector<std::array<float, 12>> &v) {
        for (size_t i = 0; i < v.size(); i++)
            std::memcpy(f.gm.base() + kVirtFcram + 0x200000 + 48 * i, v[i].data(), 48);
    }
    void draw(uint32_t n) {
        l.add(NUMVERTICES, n);
        l.add(VERTEX_OFFSET, 0);
        l.add(PRIMITIVE_CONFIG, 0);
        l.add(DRAWARRAYS, 1);
        l.add(FINALIZE, 1);
    }
};

} // namespace

int main() {
    // --- float24 ---
    CHECK(f24ToF32(0x3F0000) == 1.0f && f24ToF32(0xC00000) == -2.0f && f24ToF32(0) == 0.0f);
    CHECK(f32ToF24(1.0f) == 0x3F0000 && f32ToF24(-2.0f) == 0xC00000 && f32ToF24(0.5f) == 0x3E0000);
    for (float f : {1.5f, -3.25f, 100.0f, 0.001953125f})
        CHECK(f24ToF32(f32ToF24(f)) == f);

    // --- fragment lighting: one directional light, global ambient, quaternion-derived normal ---
    {
        Fx f;
        auto c10 = [](unsigned r, unsigned g, unsigned b) { return uint32_t(r << 20 | g << 10 | b); };
        f.p().writeRegister(0x1C0, c10(26, 26, 26));        // global ambient
        f.p().writeRegister(0x1C2, 0);                      // one light
        f.p().writeRegister(0x140 + 4, 0);                  // light vector x = y = 0
        f.p().writeRegister(0x140 + 5, 0x3C00);             // z = 1.0 (float16)
        f.p().writeRegister(0x140 + 9, 1);                  // directional
        f.p().writeRegister(0x140 + 2, c10(100, 100, 100)); // diffuse
        f.p().writeRegister(0x140 + 3, c10(10, 10, 10));    // ambient
        float attr[kNumSlots] = {};
        float out[6];
        attr[kNormQuat + 3] = 1.0f; // identity quaternion: normal (0, 0, 1), facing the light
        f.p().shadeLighting(attr, out);
        CHECK(std::fabs(out[0] - 136.0f / 255.0f) < 1e-5f && out[0] == out[1] && out[1] == out[2]);
        attr[kNormQuat] = 1.0f; // 180 degrees about x: normal (0, 0, -1), facing away
        attr[kNormQuat + 3] = 0.0f;
        f.p().shadeLighting(attr, out);
        CHECK(std::fabs(out[0] - 36.0f / 255.0f) < 1e-5f); // only ambient terms remain
        f.p().writeRegister(0x140 + 9, 3);                 // two-sided diffuse lights the back as well
        f.p().shadeLighting(attr, out);
        CHECK(std::fabs(out[0] - 136.0f / 255.0f) < 1e-5f);
    }
    // --- command list parsing: masks, consecutive/same-register writes, padding, finalize ---
    {
        Fx f;
        List l;
        l.add(0x100, 0xAABBCCDD);
        l.add(0x100, 0x11223344, 0x5);    // bytes 0 and 2 only
        l.multi(0x110, {1, 2, 3}, true);  // 0x110, 0x111, 0x112
        l.multi(0x120, {7, 8, 9}, false); // all to 0x120
        l.add(FINALIZE, 1);
        l.add(0x130, 5); // after finalize: ignored
        f.run(l);
        CHECK(f.p().reg(0x100) == 0xAA22CC44);
        CHECK(f.p().reg(0x110) == 1 && f.p().reg(0x111) == 2 && f.p().reg(0x112) == 3);
        CHECK(f.p().reg(0x120) == 9);
        CHECK(f.p().reg(0x130) == 0);
        CHECK(f.p().stats().commandLists == 1);
    }
    // --- uniform and code uploads ---
    {
        Fx f;
        List l;
        uint32_t w[3];
        packF24(1, 2, 3, 4, w);
        l.add(VSH_FLOATUNIFORM_CONFIG, 5);
        l.multi(VSH_FLOATUNIFORM_DATA, {w[0], w[1], w[2]}, false);
        packF24(-5, 6.5f, 7, 8, w);
        l.multi(VSH_FLOATUNIFORM_DATA, {w[0], w[1], w[2]}, false); // continues at c6
        l.add(VSH_FLOATUNIFORM_CONFIG, 0x80000000u | 10);          // float32 mode: words w, z, y, x
        l.multi(VSH_FLOATUNIFORM_DATA, {fbits(4), fbits(3), fbits(2), fbits(1)}, false);
        l.add(VSH_CODETRANSFER_CONFIG, 3);
        l.multi(VSH_CODETRANSFER_DATA, {0xAAAA0001, 0xAAAA0002, 0xAAAA0003}, true);
        l.add(VSH_OPDESCS_CONFIG, 2);
        l.multi(VSH_OPDESCS_DATA, {0x1234, 0x5678}, true);
        l.add(FIXEDATTRIB_INDEX, 4);
        packF24(9, 8, 7, 6, w);
        l.multi(FIXEDATTRIB_DATA0, {w[0], w[1], w[2]}, true);
        f.run(l);
        const ShaderUnit &s = f.p().shader();
        CHECK(s.fconst[5][0] == 1 && s.fconst[5][1] == 2 && s.fconst[5][2] == 3 && s.fconst[5][3] == 4);
        CHECK(s.fconst[6][0] == -5 && s.fconst[6][1] == 6.5f && s.fconst[6][2] == 7 && s.fconst[6][3] == 8);
        CHECK(s.fconst[10][0] == 1 && s.fconst[10][1] == 2 && s.fconst[10][2] == 3 && s.fconst[10][3] == 4);
        CHECK(s.code[3] == 0xAAAA0001 && s.code[4] == 0xAAAA0002 && s.code[5] == 0xAAAA0003);
        CHECK(s.opdesc[2] == 0x1234 && s.opdesc[3] == 0x5678);
    }

    // --- one coloured triangle: coverage, colour, tiled write ---
    {
        Fx f;
        Scene sc(16, 16);
        // CCW triangle covering the lower-left half of clip space
        sc.vertices(f, {{-1, -1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0},
                        {1, -1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0},
                        {-1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0}});
        sc.draw(3);
        f.run(sc.l);
        unsigned red = 0;
        for (unsigned y = 0; y < 16; y++)
            for (unsigned x = 0; x < 16; x++) {
                const Rgba c = f.px(kVirtVram, 16, x, y);
                if (c.r == 255 && c.g == 0 && c.b == 0 && c.a == 255)
                    red++;
                else
                    CHECK(c.r == 0 && c.a == 0); // untouched
            }
        CHECK(red >= 120 && red <= 136); // half of 16x16, up to the diagonal
        CHECK(f.px(kVirtVram, 16, 1, 1).r == 255 && f.px(kVirtVram, 16, 14, 14).r == 0);
        CHECK(f.p().stats().draws == 1 && f.p().stats().triangles == 1 && f.p().stats().shaderRuns == 3);
        // Face culling: mode 2 keeps counter-clockwise (what we drew), mode 1 keeps clockwise and drops it.
        Fx g;
        Scene s2(16, 16);
        s2.l.add(FACECULLING, 1);
        s2.vertices(g, {{-1, -1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0},
                        {1, -1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0},
                        {-1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0}});
        s2.draw(3);
        g.run(s2.l);
        CHECK(g.p().stats().culled == 1 && g.px(kVirtVram, 16, 1, 1).a == 0);
    }

    // --- FB_DIM bit 24: set (as the game does) stores row y at height - 1 - y; clear stores it at y ---
    for (int flip = 0; flip < 2; flip++) {
        Fx f;
        Scene sc(16, 16);
        sc.l.add(FB_DIM, 16 | 15u << 12 | (flip ? 1u << 24 : 0));
        sc.vertices(f, {{-1, -1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0},
                        {1, -1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0},
                        {-1, -1 + 0.25f, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0}});
        sc.draw(3); // a thin sliver along screen row 0 (y = 0)
        f.run(sc.l);
        const unsigned memRowOfY0 = flip ? 15 : 0;
        const Rgba at0 =
            decodePixel(f.gm.base() + kVirtVram + tiledOffset(2, memRowOfY0, 16, 4), PixFmt::RGBA8);
        const Rgba other =
            decodePixel(f.gm.base() + kVirtVram + tiledOffset(2, 15 - memRowOfY0, 16, 4), PixFmt::RGBA8);
        CHECK(at0.r == 255 && other.a == 0);
    }

    // --- command buffer jumps (CMDBUF_JUMP): the draw lives in a buffer the main list jumps to; a jump
    // replaces the running list, the processor does not come back ---
    {
        Fx f;
        Scene sc(16, 16);
        sc.vertices(f, {{-1, -1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0},
                        {1, -1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0},
                        {-1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0}});
        List nested; // the draw, ending in its own FINALIZE
        nested.add(NUMVERTICES, 3);
        nested.add(VERTEX_OFFSET, 0);
        nested.add(PRIMITIVE_CONFIG, 0);
        nested.add(DRAWARRAYS, 1);
        nested.add(FINALIZE, 1);
        const uint32_t nestedVirt = kVirtFcram + 0x180000, nestedPhys = kPhysFcram + 0x180000;
        std::memcpy(f.gm.base() + nestedVirt, nested.w.data(), nested.w.size() * 4);
        sc.l.add(CMDBUF_SIZE0, uint32_t(nested.w.size() * 4 / 8));
        sc.l.add(CMDBUF_ADDR0, nestedPhys >> 3);
        sc.l.add(CMDBUF_JUMP0, 1);
        sc.l.add(0x130, 5); // after the jump: never executed
        sc.l.add(FINALIZE, 1);
        f.run(sc.l);
        CHECK(f.p().stats().bufferJumps == 1 && f.p().stats().draws == 1);
        CHECK(f.px(kVirtVram, 16, 1, 1).r == 255);
        CHECK(f.p().reg(0x130) == 0);

        // a buffer address outside memory is refused and counted, not followed
        Fx g;
        List bad;
        bad.add(CMDBUF_SIZE0, 4);
        bad.add(CMDBUF_ADDR0, 0x10000000u >> 3);
        bad.add(CMDBUF_JUMP0, 1);
        bad.add(FINALIZE, 1);
        g.run(bad);
        CHECK(g.p().stats().bufferJumps == 0 && g.p().stats().jumpsRejected == 1);

        // a buffer that jumps to itself is cut off after a bounded number of jumps
        Fx h;
        List loop;
        const uint32_t loopVirt = kVirtFcram + 0x190000, loopPhys = kPhysFcram + 0x190000;
        loop.add(CMDBUF_SIZE0, 4);
        loop.add(CMDBUF_ADDR0, loopPhys >> 3);
        loop.add(CMDBUF_JUMP0, 1);
        std::memcpy(h.gm.base() + loopVirt, loop.w.data(), loop.w.size() * 4);
        h.run(loop);
        CHECK(h.p().stats().bufferJumps == 4096 && h.p().stats().jumpsRejected == 1);

        // a chain of buffers (each jumping to the next) runs to its end however long it is
        Fx k;
        List chain;
        constexpr unsigned kLinks = 1000;
        for (unsigned i = 0; i < kLinks; i++) {
            List link;
            if (i + 1 < kLinks) {
                link.add(CMDBUF_SIZE0, 4);
                link.add(CMDBUF_ADDR0, (kPhysFcram + 0x1A0000 + (i + 1) * 0x40) >> 3);
                link.add(CMDBUF_JUMP0, 1);
            } else {
                link.add(0x130, 0x77);
                link.add(FINALIZE, 1);
            }
            std::memcpy(k.gm.base() + kVirtFcram + 0x1A0000 + i * 0x40, link.w.data(), link.w.size() * 4);
        }
        chain.add(CMDBUF_SIZE0, 4);
        chain.add(CMDBUF_ADDR0, (kPhysFcram + 0x1A0000) >> 3);
        chain.add(CMDBUF_JUMP0, 1);
        k.run(chain);
        CHECK(k.p().reg(0x130) == 0x77 && k.p().stats().bufferJumps == kLinks);
    }

    // --- two triangles sharing an edge fill a quad exactly once (top-left rule, no gaps) ---
    {
        Fx f;
        Scene sc(16, 16);
        sc.l.add(COLOR_OPERATION, 0x100); // additive blending would double-cover shared pixels
        sc.l.add(BLEND_FUNC,
                 0x00000000 | (1u << 16) | (1u << 24) | (1u << 20) | (1u << 28)); // add, src=1, dst=1
        const std::array<float, 12> a = {-1, -1, 0, 1, 0.25f, 0.25f, 0.25f, 0.25f, 0, 0, 0, 0},
                                    b = {1, -1, 0, 1, 0.25f, 0.25f, 0.25f, 0.25f, 0, 0, 0, 0},
                                    c = {1, 1, 0, 1, 0.25f, 0.25f, 0.25f, 0.25f, 0, 0, 0, 0},
                                    d = {-1, 1, 0, 1, 0.25f, 0.25f, 0.25f, 0.25f, 0, 0, 0, 0};
        sc.vertices(f, {a, b, c, a, c, d});
        sc.draw(6);
        f.run(sc.l);
        unsigned wrong = 0;
        for (unsigned y = 0; y < 16; y++)
            for (unsigned x = 0; x < 16; x++)
                if (std::abs(int(f.px(kVirtVram, 16, x, y).r) - 64) > 1)
                    wrong++;
        CHECK(wrong == 0);
    }

    // --- texture: replace, orientation (memory row 0 is v = 1) and alpha blending ---
    {
        Fx f;
        Scene sc(16, 16);
        // 8x8 RGBA8 texture in VRAM after the colour/depth area: pixel (x, y) = (x*32, y*32, 0, 255)
        const uint32_t texVirt = kVirtVram + 0x200000;
        for (unsigned y = 0; y < 8; y++)
            for (unsigned x = 0; x < 8; x++)
                encodePixel(f.gm.base() + texVirt + tiledOffset(x, y, 8, 4), PixFmt::RGBA8,
                            Rgba{uint8_t(x * 32), uint8_t(y * 32), 0, 255});
        sc.l.add(TEXUNIT_CONFIG, 1);
        sc.l.add(TEXUNIT0_DIM, 8 << 16 | 8);
        sc.l.add(TEXUNIT0_PARAM, 0);
        sc.l.add(TEXUNIT0_ADDR, (kPhysVram + 0x200000) >> 3);
        sc.l.add(TEXUNIT0_TYPE, 0);
        sc.l.add(TEXENV0, 0x00030003); // colour and alpha source 0 = texture 0
        // quad (two triangles), uv spans (0,0)..(1,1); screen y grows with v
        auto V = [](float x, float y, float u, float v) {
            return std::array<float, 12>{x, y, 0, 1, 1, 1, 1, 1, u, v, 0, 0};
        };
        sc.vertices(f, {V(-1, -1, 0, 0), V(1, -1, 1, 0), V(1, 1, 1, 1), V(-1, -1, 0, 0), V(1, 1, 1, 1),
                        V(-1, 1, 0, 1)});
        sc.draw(6);
        f.run(sc.l);
        // screen pixel (x, y) in 16x16 -> texel column x/2 and memory row 7 - y/2 (v = 0 is the last stored
        // row)
        for (unsigned y = 0; y < 16; y += 3)
            for (unsigned x = 0; x < 16; x += 3) {
                const Rgba c = f.px(kVirtVram, 16, x, y);
                CHECK(c.r == (x / 2) * 32 && c.g == (7 - y / 2) * 32 &&
                      c.a == 255); // v = 0 samples the last memory row
            }
        CHECK(f.p().stats().unsupportedTexFormat == 0 && f.p().stats().badAddress == 0);
    }
    // --- blending, alpha test, depth test, scissor ---
    {
        Fx f;
        Scene sc(16, 16);
        // clear to blue (opaque), then draw a half-transparent red quad with SRC_ALPHA / ONE_MINUS_SRC_ALPHA
        for (unsigned y = 0; y < 16; y++)
            for (unsigned x = 0; x < 16; x++)
                encodePixel(f.gm.base() + kVirtVram + tiledOffset(x, y, 16, 4), PixFmt::RGBA8,
                            Rgba{0, 0, 255, 255});
        sc.l.add(COLOR_OPERATION, 0x100);
        sc.l.add(BLEND_FUNC, (6u << 16) | (7u << 20) | (6u << 24) | (7u << 28));
        auto Q = [](float a) { return std::array<float, 12>{0, 0, 0, 1, 1, 0, 0, a, 0, 0, 0, 0}; };
        auto at = [&](float x, float y, float a) {
            auto v = Q(a);
            v[0] = x;
            v[1] = y;
            return v;
        };
        sc.vertices(f, {at(-1, -1, 0.5f), at(1, -1, 0.5f), at(1, 1, 0.5f), at(-1, -1, 0.5f), at(1, 1, 0.5f),
                        at(-1, 1, 0.5f)});
        sc.draw(6);
        f.run(sc.l);
        const Rgba c = f.px(kVirtVram, 16, 8, 8);
        CHECK(std::abs(int(c.r) - 128) <= 1 && c.g == 0 && std::abs(int(c.b) - 127) <= 1);

        // alpha test GREATER 100: alpha 0.2 (51) is rejected, alpha 0.8 (204) passes
        for (float a : {0.2f, 0.8f}) {
            Fx g;
            Scene s2(16, 16);
            s2.l.add(ALPHA_TEST, 1 | 6u << 4 | 100u << 8);
            s2.vertices(g,
                        {at(-1, -1, a), at(1, -1, a), at(1, 1, a), at(-1, -1, a), at(1, 1, a), at(-1, 1, a)});
            s2.draw(6);
            g.run(s2.l);
            CHECK(g.px(kVirtVram, 16, 8, 8).a ==
                  (a > 0.5f ? 204 : 0)); // a passing fragment writes its own alpha
        }
        // scissor: only x in [4, 7] and y in [2, 5] are written
        Fx h;
        Scene s3(16, 16);
        s3.l.add(SCISSOR_MODE, 3);
        s3.l.add(SCISSOR_POS, 4 | 2u << 16);
        s3.l.add(SCISSOR_DIM, 7 | 5u << 16);
        s3.vertices(h, {at(-1, -1, 1), at(1, -1, 1), at(1, 1, 1), at(-1, -1, 1), at(1, 1, 1), at(-1, 1, 1)});
        s3.draw(6);
        h.run(s3.l);
        unsigned inside = 0, outside = 0;
        for (unsigned y = 0; y < 16; y++)
            for (unsigned x = 0; x < 16; x++)
                (h.px(kVirtVram, 16, x, y).a ? ((x >= 4 && x <= 7 && y >= 2 && y <= 5) ? inside : outside)
                                             : inside) += h.px(kVirtVram, 16, x, y).a ? 1 : 0;
        CHECK(inside == 16 && outside == 0);
    }
    { // depth: with GREATER the larger depth value wins regardless of draw order
        Fx f;
        Scene sc(16, 16, true);
        // clear the depth buffer to 0
        std::memset(f.gm.base() + kVirtVram + 0x100000, 0, 16 * 16 * 4);
        // depth = z * 1.0 + 0 with z = 0.25 (far, red) then 0.75 (near, green) and a third 0.5 (blue, must
        // lose)
        auto quad = [](float z, float r, float g, float b) {
            auto v = [&](float x, float y) {
                return std::array<float, 12>{x, y, z, 1, r, g, b, 1, 0, 0, 0, 0};
            };
            return std::vector<std::array<float, 12>>{v(-1, -1), v(1, -1), v(1, 1),
                                                      v(-1, -1), v(1, 1),  v(-1, 1)};
        };
        std::vector<std::array<float, 12>> all;
        for (auto &q : {quad(0.25f, 1, 0, 0), quad(0.75f, 0, 1, 0), quad(0.5f, 0, 0, 1)})
            all.insert(all.end(), q.begin(), q.end());
        sc.vertices(f, all);
        sc.draw(18);
        f.run(sc.l);
        const Rgba c = f.px(kVirtVram, 16, 8, 8);
        CHECK(c.g == 255 && c.r == 0 && c.b == 0);
    }
    { // a triangle crossing the near plane is clipped, not dropped
        Fx f;
        Scene sc(16, 16);
        // z = -2, w = 1 is behind the near plane (z >= -w) for the first vertex only
        sc.vertices(f, {{-1, -1, -2, 1, 1, 1, 1, 1, 0, 0, 0, 0},
                        {1, -1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0},
                        {-1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0}});
        sc.draw(3);
        f.run(sc.l);
        CHECK(f.p().stats().clipped == 1);
        CHECK(f.px(kVirtVram, 16, 1, 8).r == 255);
    }
    { // unsupported features are counted, not silently ignored; geometry-shader draws are skipped
        Fx f;
        Scene sc(16, 16);
        sc.l.add(0x08F, 1);                // lighting enable
        sc.l.add(TEXENV_UPDATE_BUFFER, 1); // fog mode
        sc.vertices(f, {{-1, -1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0},
                        {1, -1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0},
                        {-1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0}});
        sc.draw(3);
        f.run(sc.l);
        CHECK(f.p().stats().unsupportedLighting == 1 && f.p().stats().unsupportedFog == 1);
        Fx g;
        Scene s2(16, 16);
        s2.l.add(GEOSTAGE_CONFIG, 2);
        s2.draw(3);
        g.run(s2.l);
        CHECK(g.p().stats().unsupportedGeometryShader == 1 && g.p().stats().drawsSkipped == 1);
        // primitive mode 3 with the geometry shader off is a triangle list (the game does this)
        Fx h;
        Scene s3(16, 16);
        s3.vertices(h, {{-1, -1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0},
                        {1, -1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0},
                        {-1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0}});
        s3.l.add(NUMVERTICES, 3);
        s3.l.add(VERTEX_OFFSET, 0);
        s3.l.add(PRIMITIVE_CONFIG, 3 << 8);
        s3.l.add(DRAWARRAYS, 1);
        s3.l.add(FINALIZE, 1);
        h.run(s3.l);
        CHECK(h.p().stats().unsupportedGeometryShader == 0 && h.p().stats().triangles == 1 &&
              h.px(kVirtVram, 16, 1, 1).r == 255);
    }

    // --- texture formats and wrapping ---
    {
        std::vector<uint8_t> mem(64 * 4, 0);
        TexInfo t;
        t.data = mem.data();
        t.width = 8;
        t.height = 8;
        // IA8: byte0 alpha, byte1 intensity
        t.format = 5;
        mem[tiledOffset(2, 1, 8, 2)] = 100;
        mem[tiledOffset(2, 1, 8, 2) + 1] = 200;
        float c[4];
        CHECK(fetchTexel(t, 2, 1, c) && std::fabs(c[0] - 200 / 255.f) < 1e-4 &&
              std::fabs(c[3] - 100 / 255.f) < 1e-4);
        // I4: low nibble = even pixel
        t.format = 10;
        mem.assign(64 * 4, 0);
        const unsigned p0 = unsigned(tiledOffset(0, 0, 8, 1)), p1 = unsigned(tiledOffset(1, 0, 8, 1));
        mem[p0 / 2] = uint8_t(0x0A | (0x05 << 4));
        CHECK(p0 == 0 && p1 == 1);
        CHECK(fetchTexel(t, 0, 0, c) && std::fabs(c[0] - 170 / 255.f) < 1e-4);
        CHECK(fetchTexel(t, 1, 0, c) && std::fabs(c[0] - 85 / 255.f) < 1e-4);
        // A8
        t.format = 8;
        mem.assign(64 * 4, 0);
        mem[tiledOffset(3, 3, 8, 1)] = 77;
        CHECK(fetchTexel(t, 3, 3, c) && std::fabs(c[3] - 77 / 255.f) < 1e-4 && c[0] == 0);
        // ETC1, individual mode: base colours (255,0,0) and (0,255,0), table 0, all indices 0 (+2), no flip
        t.format = 12;
        mem.assign(64 * 4, 0);
        for (int blk = 0; blk < 4; blk++) {
            uint8_t *b = mem.data() + blk * 8;
            b[7] = 0xF0; // R1 R2
            b[6] = 0x0F; // G1 G2
            b[5] = 0x00; // B1 B2
            b[4] = 0x00; // tables 0/0, diff 0, flip 0
            // bytes 0-3 (index bits) stay 0
        }
        CHECK(fetchTexel(t, 0, 0, c) && std::fabs(c[0] - 1.0f) < 1e-4 && std::fabs(c[1] - 2 / 255.f) < 1e-4);
        CHECK(fetchTexel(t, 3, 2, c) && std::fabs(c[0] - 2 / 255.f) < 1e-4 && std::fabs(c[1] - 1.0f) < 1e-4);
        CHECK(fetchTexel(t, 7, 7, c) && std::fabs(c[1] - 1.0f) < 1e-4); // fourth block of the tile
        // wrapping: repeat vs clamp to edge vs border on an RGBA8 texture
        t.format = 0;
        mem.assign(64 * 4, 0);
        for (unsigned x = 0; x < 8; x++)
            for (unsigned y = 0; y < 8; y++)
                encodePixel(mem.data() + tiledOffset(x, y, 8, 4), PixFmt::RGBA8,
                            Rgba{uint8_t(x * 30), 0, 0, 255});
        t.magLinear = false;
        t.wrapS = 2;
        CHECK(sampleTexture(t, 1.0f + 0.5f / 8, 0.5f, c) &&
              std::fabs(c[0] - 0.0f) < 1e-4); // repeat: back to texel 0
        t.wrapS = 0;
        CHECK(sampleTexture(t, 1.5f, 0.5f, c) && std::fabs(c[0] - 210 / 255.f) < 1e-4); // clamp: last texel
        t.wrapS = 3;
        CHECK(sampleTexture(t, 1.0f + 0.5f / 8, 0.5f, c) && std::fabs(c[0] - 210 / 255.f) < 1e-4); // mirror
        t.wrapS = 1;
        t.border[0] = 0.5f;
        CHECK(sampleTexture(t, 1.5f, 0.5f, c) && c[0] == 0.5f); // border colour
        t.wrapS = 0;
        t.magLinear = true; // bilinear between texel 0 and 1 at the shared edge
        CHECK(sampleTexture(t, 1.0f / 8, 0.5f, c) && std::fabs(c[0] - 15 / 255.f) < 1e-3);
    }

    std::printf(g_fail ? "FAIL\n" : "pica ok\n");
    return g_fail ? 1 : 0;
}
