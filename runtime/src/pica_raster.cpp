// PICA200 clipping, rasterization and the fragment pipeline (texturing, TEV, tests, blending) in software.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "pokerecomp/gpu.h"
#include "pokerecomp/pica.h"

namespace pokerecomp {
namespace pica {

namespace {

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
};

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

Color fromRgba8(uint32_t v) { // bytes: R in bits 0-7 ... A in bits 24-31
    return {float(v & 0xFF) / 255.f, float((v >> 8) & 0xFF) / 255.f, float((v >> 16) & 0xFF) / 255.f,
            float(v >> 24) / 255.f};
}

bool compare(unsigned func, int a, int b) { // a = fragment/reference value, b = stored value
    switch (func) {
    case 0:
        return false;
    case 1:
        return true;
    case 2:
        return a == b;
    case 3:
        return a != b;
    case 4:
        return a < b;
    case 5:
        return a <= b;
    case 6:
        return a > b;
    default:
        return a >= b;
    }
}

PixFmt colorFmt(unsigned f) {
    switch (f) {
    case 2:
        return PixFmt::RGB5A1;
    case 3:
        return PixFmt::RGB565;
    case 4:
        return PixFmt::RGBA4;
    default:
        return PixFmt::RGBA8;
    }
}

unsigned texBytes(unsigned fmt, uint32_t w, uint32_t h) {
    const uint64_t px = uint64_t(w) * h;
    switch (fmt) {
    case 0:
        return unsigned(px * 4);
    case 1:
        return unsigned(px * 3);
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
        return unsigned(px * 2);
    case 7:
    case 8:
    case 9:
    case 12:
        return unsigned(fmt == 12 ? px / 2 : px);
    case 10:
    case 11:
        return unsigned(px / 2);
    case 13:
        return unsigned(px);
    default:
        return 0;
    }
}

} // namespace

bool Pica::prepareTarget() {
    const uint32_t dim = regs_[FB_DIM];
    fb_ = Fb();
    fb_.width = dim & 0x7FF;
    fb_.height = ((dim >> 12) & 0x3FF) + 1;
    fb_.colorFmt = (regs_[COLORBUF_FORMAT] >> 16) & 7;
    fb_.colorBytes = fb_.colorFmt == 0 ? 4 : 2;
    fb_.block32 = regs_[FB_BLOCK32] & 1;
    fb_.flip = (dim >> 24) & 1;
    if (!fb_.width || !fb_.height || (fb_.colorFmt != 0 && fb_.colorFmt < 2) || fb_.colorFmt > 4)
        return false;
    const uint32_t stride = (fb_.width + 7) & ~7u, rows = (fb_.height + 7) & ~7u;
    fb_.color = physPtr(regs_[COLORBUF_LOC] * 8, stride * rows * fb_.colorBytes);
    if (!fb_.color) {
        stats_.badAddress++;
        return false;
    }
    const unsigned df = regs_[DEPTHBUF_FORMAT] & 3;
    fb_.depthBytes = df == 0 ? 2 : df == 2 ? 3 : 4;
    if (regs_[DEPTHBUF_LOC])
        fb_.depth = physPtr(regs_[DEPTHBUF_LOC] * 8, stride * rows * fb_.depthBytes); // may be absent
    return true;
}

void Pica::prepareTextures() {
    // A TEV stage that replaces the result with the previous result (and does not feed the combiner buffer)
    // changes nothing; most draws use one or two stages, so skipping the rest saves most of the per-fragment
    // work.
    static const uint32_t kStageRegs[6] = {TEXENV0, TEXENV1, TEXENV2, TEXENV3, TEXENV4, TEXENV5};
    for (int s = 0; s < 6; s++) {
        const uint32_t src = regs_[kStageRegs[s]], ops = regs_[kStageRegs[s] + 1],
                       comb = regs_[kStageRegs[s] + 2];
        const uint32_t scale = regs_[kStageRegs[s] + 4];
        const bool passthrough =
            (src & 0x000F000F) == 0x000F000F && (ops & 0x0000700F) == 0 && comb == 0 && scale == 0;
        const bool feedsBuffer = s < 4 && (((regs_[TEXENV_UPDATE_BUFFER] >> (8 + s)) & 1) ||
                                           ((regs_[TEXENV_UPDATE_BUFFER] >> (12 + s)) & 1));
        stageActive_[s] = !(passthrough && !feedsBuffer);
    }
    static const uint32_t kBase[3][5] = {
        {TEXUNIT0_BORDER, TEXUNIT0_DIM, TEXUNIT0_PARAM, TEXUNIT0_ADDR, TEXUNIT0_TYPE},
        {TEXUNIT1_BORDER, TEXUNIT1_DIM, TEXUNIT1_PARAM, TEXUNIT1_ADDR, TEXUNIT1_TYPE},
        {TEXUNIT2_BORDER, TEXUNIT2_DIM, TEXUNIT2_PARAM, TEXUNIT2_ADDR, TEXUNIT2_TYPE}};
    for (int u = 0; u < 3; u++) {
        texEnabled_[u] = (regs_[TEXUNIT_CONFIG] >> u) & 1;
        tex_[u] = TexInfo();
        if (!texEnabled_[u])
            continue;
        const uint32_t dim = regs_[kBase[u][1]], param = regs_[kBase[u][2]];
        TexInfo &t = tex_[u];
        t.width = (dim >> 16) & 0x7FF;
        t.height = dim & 0x7FF;
        t.format = regs_[kBase[u][4]] & 0xF;
        t.wrapT = (param >> 8) & 7;
        t.wrapS = (param >> 12) & 7;
        t.magLinear = (param >> 1) & 1;
        t.minLinear = (param >> 2) & 1;
        const Color bc = fromRgba8(regs_[kBase[u][0]]);
        t.border[0] = bc.r;
        t.border[1] = bc.g;
        t.border[2] = bc.b;
        t.border[3] = bc.a;
        if (u == 0 && ((param >> 28) & 7) != 0) { // cube maps, projection, shadow textures
            stats_.unsupportedCube++;
            texEnabled_[u] = false;
            continue;
        }
        const unsigned bytes = texBytes(t.format, t.width, t.height);
        if (!bytes || t.width % 8 || t.height % 8) {
            stats_.unsupportedTexFormat++;
            texEnabled_[u] = false;
            continue;
        }
        t.data = physPtr(regs_[kBase[u][3]] * 8, bytes);
        if (!t.data) {
            stats_.badAddress++;
            texEnabled_[u] = false;
        }
    }
}

// ---------------------------------------------------------------------------------------------- triangles

namespace {
ClipVertex lerp(const ClipVertex &a, const ClipVertex &b, float t) {
    ClipVertex o;
    for (int i = 0; i < 4; i++)
        o.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
    for (int i = 0; i < kNumSlots; i++)
        o.a[i] = a.a[i] + (b.a[i] - a.a[i]) * t;
    return o;
}
} // namespace

void Pica::drawTriangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c) {
    stats_.triangles++;
    // Clip against the near plane (z >= -w); the screen edges are handled by the bounding box.
    ClipVertex in[3] = {a, b, c}, poly[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const ClipVertex &p = in[i], &q = in[(i + 1) % 3];
        const float dp = p.pos[2] + p.pos[3], dq = q.pos[2] + q.pos[3];
        if (dp >= 0)
            poly[n++] = p;
        if ((dp >= 0) != (dq >= 0)) {
            if (n >= 4)
                break;
            poly[n++] = lerp(p, q, dp / (dp - dq));
        }
    }
    if (n < 3)
        return;
    if (n != 3)
        stats_.clipped++;
    for (int i = 1; i + 1 < n; i++)
        rasterTriangle(poly[0], poly[i], poly[i + 1]);
}

void Pica::rasterTriangle(const ClipVertex &va, const ClipVertex &vb, const ClipVertex &vc) {
    if (!rasterEnabled_)
        return;
    const ClipVertex *v[3] = {&va, &vb, &vc};
    float sx[3], sy[3], sz[3], iw[3];
    for (int i = 0; i < 3; i++) {
        const float w = v[i]->pos[3];
        if (w == 0)
            return;
        iw[i] = 1.0f / w;
        sx[i] = (v[i]->pos[0] * iw[i] + 1.0f) * halfW_ + vpX_;
        sy[i] = (v[i]->pos[1] * iw[i] + 1.0f) * halfH_ + vpY_;
        sz[i] = v[i]->pos[2] * iw[i];
    }
    for (int i = 0; i < 3; i++) {
        stats_.bboxMin[0] = std::min(stats_.bboxMin[0], sx[i]);
        stats_.bboxMin[1] = std::min(stats_.bboxMin[1], sy[i]);
        stats_.bboxMax[0] = std::max(stats_.bboxMax[0], sx[i]);
        stats_.bboxMax[1] = std::max(stats_.bboxMax[1], sy[i]);
    }
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
    if (area == 0 || std::isnan(area))
        return;
    const unsigned cull = regs_[FACECULLING] & 3;
    if ((cull == 1 && area > 0) || (cull == 2 && area < 0)) { // 1: keep clockwise, 2: keep counter-clockwise
        stats_.culled++;
        return;
    }
    int order[3] = {0, 1, 2};
    if (area < 0) {
        std::swap(order[1], order[2]);
        area = -area;
    }
    const int i0 = order[0], i1 = order[1], i2 = order[2];
    // Bounding box clipped to the target and the scissor rectangle.
    float minx = std::min({sx[0], sx[1], sx[2]}), maxx = std::max({sx[0], sx[1], sx[2]});
    float miny = std::min({sy[0], sy[1], sy[2]}), maxy = std::max({sy[0], sy[1], sy[2]});
    int x0 = std::max(0, int(std::floor(minx))), x1 = std::min(int(fb_.width) - 1, int(std::ceil(maxx)));
    int y0 = std::max(0, int(std::floor(miny))), y1 = std::min(int(fb_.height) - 1, int(std::ceil(maxy)));
    if ((regs_[SCISSOR_MODE] & 3) == 3) {
        const int sx1 = regs_[SCISSOR_POS] & 0x3FF, sy1 = (regs_[SCISSOR_POS] >> 16) & 0x3FF;
        const int sx2 = regs_[SCISSOR_DIM] & 0x3FF, sy2 = (regs_[SCISSOR_DIM] >> 16) & 0x3FF;
        x0 = std::max(x0, sx1);
        y0 = std::max(y0, sy1);
        x1 = std::min(x1, sx2);
        y1 = std::min(y1, sy2);
    }
    // Top-left fill rule (counter-clockwise in y-up screen space).
    auto topLeft = [&](int a, int b) {
        const float dx = sx[b] - sx[a], dy = sy[b] - sy[a];
        return (dy == 0 && dx < 0) || dy < 0;
    };
    const bool tl0 = topLeft(i1, i2), tl1 = topLeft(i2, i0), tl2 = topLeft(i0, i1);
    const float inv = 1.0f / area;
    float attr[kNumSlots];
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            // edge functions (weight of vertex i0, i1, i2)
            const float w0 = (sx[i2] - sx[i1]) * (py - sy[i1]) - (sy[i2] - sy[i1]) * (px - sx[i1]);
            const float w1 = (sx[i0] - sx[i2]) * (py - sy[i2]) - (sy[i0] - sy[i2]) * (px - sx[i2]);
            const float w2 = (sx[i1] - sx[i0]) * (py - sy[i0]) - (sy[i1] - sy[i0]) * (px - sx[i0]);
            if (!(w0 > 0 || (w0 == 0 && tl0)) || !(w1 > 0 || (w1 == 0 && tl1)) ||
                !(w2 > 0 || (w2 == 0 && tl2)))
                continue;
            const float l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv;
            const float p0 = l0 * iw[i0], p1 = l1 * iw[i1], p2 = l2 * iw[i2];
            const float denom = p0 + p1 + p2;
            if (denom == 0)
                continue;
            const float id = 1.0f / denom;
            for (int k = 0; k < kNumSlots; k++)
                attr[k] = (p0 * v[i0]->a[k] + p1 * v[i1]->a[k] + p2 * v[i2]->a[k]) * id;
            const float z = l0 * sz[i0] + l1 * sz[i1] + l2 * sz[i2];
            float d = (regs_[DEPTHMAP_ENABLE] & 1) ? z * depthScale_ + depthOffset_ : -z;
            fragment(x, y, clamp01(d), attr);
        }
    }
}

// ---------------------------------------------------------------------------------------------- fragments

namespace {

struct Rgb {
    float r, g, b;
};

float blendFactor(unsigned f, const Color &src, const Color &dst, const Color &cst, int comp,
                  bool alphaChannel) {
    auto ch = [&](const Color &c) { return comp == 0 ? c.r : comp == 1 ? c.g : comp == 2 ? c.b : c.a; };
    switch (f) {
    case 0:
        return 0;
    case 1:
        return 1;
    case 2:
        return ch(src);
    case 3:
        return 1 - ch(src);
    case 4:
        return ch(dst);
    case 5:
        return 1 - ch(dst);
    case 6:
        return src.a;
    case 7:
        return 1 - src.a;
    case 8:
        return dst.a;
    case 9:
        return 1 - dst.a;
    case 10:
        return ch(cst);
    case 11:
        return 1 - ch(cst);
    case 12:
        return cst.a;
    case 13:
        return 1 - cst.a;
    case 14:
        return alphaChannel ? 1.0f : std::min(src.a, 1 - dst.a);
    default:
        return 1;
    }
}

float blendEq(unsigned eq, float s, float d) {
    switch (eq) {
    case 1:
        return s - d;
    case 2:
        return d - s;
    case 3:
        return std::min(s, d);
    case 4:
        return std::max(s, d);
    default:
        return s + d;
    }
}

} // namespace

void Pica::fragment(int x, int y, float depth, const float *attr) {
    stats_.fragments++;
    const uint32_t stride = (fb_.width + 7) & ~7u;
    // Rows are stored upside down in the buffer (the game's display transfers read from the bottom-aligned
    // area).
    const unsigned row = unsigned(fb_.flip ? int(fb_.height) - 1 - y : y);
    uint8_t *cpix = fb_.color + tiledOffset(unsigned(x), row, stride, fb_.colorBytes, fb_.block32);
    uint8_t *dpix =
        fb_.depth ? fb_.depth + tiledOffset(unsigned(x), row, stride, fb_.depthBytes, fb_.block32) : nullptr;

    // --- texturing and the TEV combiner chain ---
    Color primary = {clamp01(attr[kColor]), clamp01(attr[kColor + 1]), clamp01(attr[kColor + 2]),
                     clamp01(attr[kColor + 3])};
    Color tex[4];
    for (int u = 0; u < 3; u++) {
        if (!texEnabled_[u])
            continue;
        float uv[2];
        if (u == 0) {
            uv[0] = attr[kTex0];
            uv[1] = attr[kTex0 + 1];
        } else if (u == 1) {
            uv[0] = attr[kTex1];
            uv[1] = attr[kTex1 + 1];
        } else {
            const bool fromTex1 = (regs_[TEXUNIT_CONFIG] >> 13) & 1;
            uv[0] = fromTex1 ? attr[kTex1] : attr[kTex2];
            uv[1] = fromTex1 ? attr[kTex1 + 1] : attr[kTex2 + 1];
        }
        float c[4] = {1, 1, 1, 1};
        if (sampleTexture(tex_[u], uv[0], uv[1], c))
            tex[u] = {c[0], c[1], c[2], c[3]};
    }
    static const uint32_t kStage[6] = {TEXENV0, TEXENV1, TEXENV2, TEXENV3, TEXENV4, TEXENV5};
    Color prev = primary, buffer = fromRgba8(regs_[TEXENV_BUFFER_COLOR]);
    const uint32_t upd = regs_[TEXENV_UPDATE_BUFFER];
    for (int s = 0; s < 6; s++) {
        if (!stageActive_[s])
            continue;
        const uint32_t src = regs_[kStage[s]], ops = regs_[kStage[s] + 1], comb = regs_[kStage[s] + 2];
        const Color konst = fromRgba8(regs_[kStage[s] + 3]);
        const uint32_t scale = regs_[kStage[s] + 4];
        auto source = [&](unsigned id) -> Color {
            switch (id) {
            case 0:
            case 1:
                return primary;
            case 2:
                return Color{0, 0, 0, 1};
            case 3:
            case 4:
            case 5:
                return tex[id - 3];
            case 6:
                return Color{0, 0, 0, 0};
            case 13:
                return buffer;
            case 14:
                return konst;
            default:
                return prev;
            }
        };
        Rgb ca[3];
        float aa[3];
        for (int i = 0; i < 3; i++) {
            const Color c = source((src >> (4 * i)) & 0xF);
            const unsigned op = (ops >> (4 * i)) & 0xF;
            switch (op) {
            case 0:
                ca[i] = {c.r, c.g, c.b};
                break;
            case 1:
                ca[i] = {1 - c.r, 1 - c.g, 1 - c.b};
                break;
            case 2:
                ca[i] = {c.a, c.a, c.a};
                break;
            case 3:
                ca[i] = {1 - c.a, 1 - c.a, 1 - c.a};
                break;
            case 4:
                ca[i] = {c.r, c.r, c.r};
                break;
            case 5:
                ca[i] = {1 - c.r, 1 - c.r, 1 - c.r};
                break;
            case 8:
                ca[i] = {c.g, c.g, c.g};
                break;
            case 9:
                ca[i] = {1 - c.g, 1 - c.g, 1 - c.g};
                break;
            case 12:
                ca[i] = {c.b, c.b, c.b};
                break;
            case 13:
                ca[i] = {1 - c.b, 1 - c.b, 1 - c.b};
                break;
            default:
                ca[i] = {c.r, c.g, c.b};
                break;
            }
            const Color d = source((src >> (16 + 4 * i)) & 0xF);
            switch ((ops >> (12 + 4 * i)) & 7) {
            case 0:
                aa[i] = d.a;
                break;
            case 1:
                aa[i] = 1 - d.a;
                break;
            case 2:
                aa[i] = d.r;
                break;
            case 3:
                aa[i] = 1 - d.r;
                break;
            case 4:
                aa[i] = d.g;
                break;
            case 5:
                aa[i] = 1 - d.g;
                break;
            case 6:
                aa[i] = d.b;
                break;
            default:
                aa[i] = 1 - d.b;
                break;
            }
        }
        const unsigned cm = comb & 0xF, am = (comb >> 16) & 0xF;
        auto comb1 = [](unsigned mode, float a0, float a1, float a2) {
            switch (mode) {
            case 0:
                return a0;
            case 1:
                return a0 * a1;
            case 2:
                return a0 + a1;
            case 3:
                return a0 + a1 - 0.5f;
            case 4:
                return a0 * a2 + a1 * (1 - a2);
            case 5:
                return a0 - a1;
            case 8:
                return a0 * a1 + a2;
            case 9:
                return (a0 + a1) * a2;
            default:
                return a0;
            }
        };
        Color out;
        float dot = 0;
        const bool dot3 = cm == 6 || cm == 7;
        if (dot3)
            dot = clamp01(4.0f * ((ca[0].r - 0.5f) * (ca[1].r - 0.5f) + (ca[0].g - 0.5f) * (ca[1].g - 0.5f) +
                                  (ca[0].b - 0.5f) * (ca[1].b - 0.5f)));
        const float rs = float(1 << std::min<uint32_t>(scale & 3, 2)),
                    as = float(1 << std::min<uint32_t>((scale >> 16) & 3, 2));
        if (dot3) {
            out.r = out.g = out.b = dot;
        } else {
            out.r = clamp01(comb1(cm, ca[0].r, ca[1].r, ca[2].r) * rs);
            out.g = clamp01(comb1(cm, ca[0].g, ca[1].g, ca[2].g) * rs);
            out.b = clamp01(comb1(cm, ca[0].b, ca[1].b, ca[2].b) * rs);
        }
        out.a = cm == 7 ? dot : clamp01(comb1(am, aa[0], aa[1], aa[2]) * as);
        if (s < 4) { // stage s decides whether the combiner buffer takes this stage's output
            if ((upd >> (8 + s)) & 1) {
                buffer.r = out.r;
                buffer.g = out.g;
                buffer.b = out.b;
            }
            if ((upd >> (12 + s)) & 1)
                buffer.a = out.a;
        }
        prev = out;
    }
    Color frag = prev;

    // --- alpha test ---
    const uint32_t at = regs_[ALPHA_TEST];
    if (at & 1)
        if (!compare((at >> 4) & 7, int(std::lround(frag.a * 255.0f)), int((at >> 8) & 0xFF)))
            return;

    // --- stencil and depth ---
    const bool hasStencil = dpix && (regs_[DEPTHBUF_FORMAT] & 3) == 3;
    uint32_t stored = 0;
    const uint32_t dmax = fb_.depthBytes == 2 ? 0xFFFF : 0xFFFFFF;
    uint8_t stencil = 0;
    if (dpix) {
        if (fb_.depthBytes == 2)
            stored = dpix[0] | dpix[1] << 8;
        else
            stored = (dpix[0] | dpix[1] << 8 | dpix[2] << 16);
        if (hasStencil)
            stencil = dpix[3];
    }
    const uint32_t st = regs_[STENCIL_TEST], sop = regs_[STENCIL_OP];
    const bool stencilOn = hasStencil && (st & 1);
    auto stencilApply = [&](unsigned op) {
        int v = stencil;
        switch (op) {
        case 1:
            v = 0;
            break;
        case 2:
            v = int8_t((st >> 16) & 0xFF) & 0xFF;
            break;
        case 3:
            v = std::min(255, v + 1);
            break;
        case 4:
            v = std::max(0, v - 1);
            break;
        case 5:
            v = ~v & 0xFF;
            break;
        case 6:
            v = (v + 1) & 0xFF;
            break;
        case 7:
            v = (v - 1) & 0xFF;
            break;
        default:
            break;
        }
        const uint8_t wm = uint8_t((st >> 8) & 0xFF);
        stencil = uint8_t((stencil & ~wm) | (v & wm));
    };
    if (stencilOn) {
        const int ref = (st >> 16) & 0xFF, mask = (st >> 24) & 0xFF;
        if (!compare((st >> 4) & 7, ref & mask, stencil & mask)) {
            stencilApply(sop & 7);
            dpix[3] = stencil;
            return;
        }
    }
    const uint32_t dcm = regs_[DEPTH_COLOR_MASK];
    const uint32_t newDepth = uint32_t(std::lround(depth * float(dmax)));
    bool depthPass = true;
    if (dpix && (dcm & 1))
        depthPass = compare((dcm >> 4) & 7, int(newDepth), int(stored));
    if (!depthPass) {
        if (stencilOn) {
            stencilApply((sop >> 4) & 7);
            dpix[3] = stencil;
        }
        return;
    }
    if (stencilOn)
        stencilApply((sop >> 8) & 7);
    if (dpix && ((dcm >> 12) & 1) && (regs_[DEPTHBUF_WRITE] & 2)) {
        dpix[0] = uint8_t(newDepth);
        dpix[1] = uint8_t(newDepth >> 8);
        if (fb_.depthBytes >= 3)
            dpix[2] = uint8_t(newDepth >> 16);
    }
    if (hasStencil)
        dpix[3] = stencil;

    // --- blending / logic op and the colour write ---
    if (!(regs_[COLORBUF_WRITE] & 0xF))
        return;
    const PixFmt pf = colorFmt(fb_.colorFmt);
    Color dst = {0, 0, 0, 1};
    if (regs_[COLORBUF_READ] & 0xF) {
        const Rgba d = decodePixel(cpix, pf);
        dst = {float(d.r) / 255.f, float(d.g) / 255.f, float(d.b) / 255.f, float(d.a) / 255.f};
    }
    Color res = frag;
    if ((regs_[COLOR_OPERATION] >> 8) & 1) {
        const uint32_t bf = regs_[BLEND_FUNC];
        const Color cst = fromRgba8(regs_[BLEND_COLOR]);
        const unsigned rgbEq = bf & 7, aEq = (bf >> 8) & 7;
        const unsigned rs = (bf >> 16) & 0xF, rd = (bf >> 20) & 0xF, as = (bf >> 24) & 0xF,
                       ad = (bf >> 28) & 0xF;
        float s[4] = {frag.r, frag.g, frag.b, frag.a}, d[4] = {dst.r, dst.g, dst.b, dst.a}, o[4];
        for (int k = 0; k < 3; k++)
            o[k] = clamp01(blendEq(rgbEq, s[k] * blendFactor(rs, frag, dst, cst, k, false),
                                   d[k] * blendFactor(rd, frag, dst, cst, k, false)));
        o[3] = clamp01(blendEq(aEq, s[3] * blendFactor(as, frag, dst, cst, 3, true),
                               d[3] * blendFactor(ad, frag, dst, cst, 3, true)));
        res = {o[0], o[1], o[2], o[3]};
    } else if ((regs_[LOGIC_OP] & 0xF) != 3) {
        stats_.unsupportedLogicOp++;
    }
    Rgba old = decodePixel(cpix, pf);
    auto q = [](float v) { return uint8_t(std::lround(clamp01(v) * 255.0f)); };
    Rgba nw = {q(res.r), q(res.g), q(res.b), q(res.a)};
    if (!((dcm >> 8) & 1))
        nw.r = old.r;
    if (!((dcm >> 9) & 1))
        nw.g = old.g;
    if (!((dcm >> 10) & 1))
        nw.b = old.b;
    if (!((dcm >> 11) & 1))
        nw.a = old.a;
    encodePixel(cpix, pf, nw);
}

} // namespace pica
} // namespace pokerecomp
