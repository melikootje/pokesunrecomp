// PICA200 command processing and the vertex stage (register layouts: public 3dbrew GPU documentation).
#include "pokerecomp/pica.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pokerecomp/gpu.h"

namespace pokerecomp {
namespace pica {

float f24ToF32(uint32_t v) {
    v &= 0xFFFFFF;
    const uint32_t sign = (v >> 23) & 1, exp = (v >> 16) & 0x7F, man = v & 0xFFFF;
    uint32_t bits;
    if (exp == 0 && man == 0)
        bits = sign << 31; // zero (the hardware has no negative zero; harmless here)
    else if (exp == 0x7F)
        bits = sign << 31 | 0x7F800000u | (man << 7);
    else if (exp == 0)
        bits = sign << 31; // subnormals flush to zero
    else
        bits = sign << 31 | (exp + 127 - 63) << 23 | man << 7;
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint32_t f32ToF24(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    const uint32_t sign = b >> 31;
    const int exp = int((b >> 23) & 0xFF);
    if (exp == 0xFF)
        return sign << 23 | 0x7Fu << 16 | ((b >> 7) & 0xFFFF);
    const int e = exp - 127 + 63;
    if (exp == 0 || e <= 0)
        return sign << 23;
    if (e >= 0x7F)
        return sign << 23 | 0x7Fu << 16;
    return sign << 23 | uint32_t(e) << 16 | ((b >> 7) & 0xFFFF);
}

namespace {
// Command buffers can chain (each ends by jumping to the next), so the call depth needs headroom; it is still
// bounded.
constexpr unsigned kMaxBufferDepth = 256;
// Unpacks the 3-word float24 layout (also used by the fixed-attribute registers) into x, y, z, w.
void unpackF24(const uint32_t w[3], Vec4 *out) {
    const uint32_t x = w[2] & 0xFFFFFF;
    const uint32_t y = ((w[1] & 0xFFFF) << 8) | (w[2] >> 24);
    const uint32_t z = ((w[0] & 0xFF) << 16) | (w[1] >> 16);
    const uint32_t ww = w[0] >> 8;
    (*out)[0] = f24ToF32(x);
    (*out)[1] = f24ToF32(y);
    (*out)[2] = f24ToF32(z);
    (*out)[3] = f24ToF32(ww);
}
float asFloat(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
} // namespace

uint8_t *Pica::physPtr(uint32_t paddr, uint32_t size) const {
    uint32_t v;
    if (!Gpu::physToVirt(paddr, &v) || !Gpu::accessible(v, size))
        return nullptr;
    return mem_ + v;
}

void Pica::writeRegister(uint32_t id, uint32_t value, uint32_t mask) {
    if (id >= kNumRegs || !mask)
        return;
    stats_.regWrites++;
    uint32_t bm = 0;
    for (int i = 0; i < 4; i++)
        if (mask & (1u << i))
            bm |= 0xFFu << (8 * i);
    regs_[id] = (regs_[id] & ~bm) | (value & bm);
    const uint32_t v = regs_[id];

    if (id == CMDBUF_JUMP0 || id == CMDBUF_JUMP0 + 1) {
        const uint32_t k = id - CMDBUF_JUMP0;
        const uint32_t bytes = (regs_[CMDBUF_SIZE0 + k] & 0x1FFFFF) * 8;
        uint32_t vaddr = 0;
        if (value && depth_ < kMaxBufferDepth && bytes >= 8 &&
            Gpu::physToVirt(regs_[CMDBUF_ADDR0 + k] * 8, &vaddr) && Gpu::accessible(vaddr, bytes)) {
            stats_.bufferJumps++;
            std::vector<uint32_t> w(bytes / 4);
            std::memcpy(w.data(), mem_ + vaddr, bytes);
            const bool outerFinalized = finalized_;
            finalized_ = false;
            runWords(w.data(), uint32_t(w.size()), depth_ + 1, vaddr);
            finalized_ = outerFinalized; // a finalize inside the nested buffer ends only that buffer
        } else if (value) {
            if (stats_.jumpsRejected++ < 5)
                std::fprintf(stderr, "[pica] rejected buffer jump %u: size=%u bytes addr=%08x (depth %u)\n",
                             k, bytes, regs_[CMDBUF_ADDR0 + k] * 8, depth_);
        }
    } else if (id == FINALIZE) {
        finalized_ = true;
    } else if (id == DRAWARRAYS || id == DRAWELEMENTS) {
        if (value)
            draw(id == DRAWELEMENTS);
    } else if (id >= VSH_FLOATUNIFORM_DATA && id <= VSH_FLOATUNIFORM_DATA + 7) {
        const bool f32 = regs_[VSH_FLOATUNIFORM_CONFIG] >> 31;
        if (uniformCount_ == 0)
            uniformIndex_ = regs_[VSH_FLOATUNIFORM_CONFIG] & 0x7F;
        uniformWords_[uniformCount_++] = value;
        if (uniformCount_ == (f32 ? 4u : 3u)) {
            if (f32) {
                for (int k = 0; k < 4; k++)
                    if (std::isnan(asFloat(uniformWords_[k]))) {
                        stats_.lastNanList = listVaddr_;
                        stats_.lastNanWord = listWord_;
                        stats_.lastNanReg = uniformIndex_;
                        if (!stats_.nanUniforms++) {
                            stats_.firstNanList = listVaddr_;
                            stats_.firstNanWord = listWord_;
                            stats_.firstNanReg = uniformIndex_;
                        }
                        break;
                    }
            }
            if (uniformIndex_ < 96) {
                Vec4 &u = shader_.fconst[uniformIndex_];
                if (f32) { // words arrive as w, z, y, x
                    u[3] = asFloat(uniformWords_[0]);
                    u[2] = asFloat(uniformWords_[1]);
                    u[1] = asFloat(uniformWords_[2]);
                    u[0] = asFloat(uniformWords_[3]);
                } else {
                    unpackF24(uniformWords_, &u);
                }
            }
            uniformIndex_++;
            uniformCount_ = 0;
            // keep the config register in step so a new burst continues where this one ended
            regs_[VSH_FLOATUNIFORM_CONFIG] =
                (regs_[VSH_FLOATUNIFORM_CONFIG] & 0x80000000u) | (uniformIndex_ & 0x7F);
        }
    } else if (id == VSH_FLOATUNIFORM_CONFIG) {
        uniformCount_ = 0;
    } else if (id == VSH_CODETRANSFER_CONFIG) {
        codeIndex_ = v & 0xFFF;
    } else if (id >= VSH_CODETRANSFER_DATA && id <= VSH_CODETRANSFER_DATA + 7) {
        if (codeIndex_ < kShaderCodeSize)
            shader_.code[codeIndex_] = value;
        codeIndex_++;
    } else if (id == VSH_OPDESCS_CONFIG) {
        descIndex_ = v & 0x7F;
    } else if (id >= VSH_OPDESCS_DATA && id <= VSH_OPDESCS_DATA + 7) {
        if (descIndex_ < kOpDescSize)
            shader_.opdesc[descIndex_] = value;
        descIndex_++;
    } else if (id == FIXEDATTRIB_INDEX) {
        fixedCount_ = 0;
    } else if (id >= FIXEDATTRIB_DATA0 && id <= FIXEDATTRIB_DATA0 + 2) {
        fixedWords_[fixedCount_++] = value;
        if (fixedCount_ == 3) {
            fixedCount_ = 0;
            const uint32_t idx = regs_[FIXEDATTRIB_INDEX] & 0xF;
            if (idx < 12)
                unpackF24(fixedWords_, &fixedAttr_[idx]);
            else
                stats_.unsupportedImmediate++; // 0xF: immediate-mode vertex submission
        }
    }
}

void Pica::processCommandList(uint32_t vaddr, uint32_t bytes) {
    stats_.commandLists++;
    if (bytes < 8 || !Gpu::accessible(vaddr, bytes))
        return;
    const uint32_t n = bytes / 4;
    std::vector<uint32_t> w(n);
    std::memcpy(w.data(), mem_ + vaddr, n * 4);
    finalized_ = false;
    runWords(w.data(), n, 0, vaddr);
}

// Executes (value, header) command pairs. Nested buffers (CMDBUF_JUMP) run to their end before the caller
// continues.
void Pica::runWords(const uint32_t *w, uint32_t n, unsigned depth, uint32_t srcVaddr) {
    const unsigned outerDepth = depth_;
    const uint32_t outerList = listVaddr_;
    listVaddr_ = srcVaddr;
    depth_ = depth;
    for (uint32_t i = 0; i + 1 < n && !finalized_;) {
        const uint32_t param = w[i], header = w[i + 1];
        const uint32_t id = header & 0xFFFF, mask = (header >> 16) & 0xF, extra = (header >> 20) & 0xFF;
        const bool consecutive = header >> 31;
        listWord_ = i;
        writeRegister(id, param, mask);
        for (uint32_t e = 0; e < extra && i + 2 + e < n && !finalized_; e++)
            writeRegister(consecutive ? id + 1 + e : id, w[i + 2 + e], mask);
        uint32_t used = 2 + extra;
        if (used & 1)
            used++; // commands are padded to 8 bytes
        i += used;
    }
    depth_ = outerDepth;
    listVaddr_ = outerList;
}

// ---------------------------------------------------------------------------------------------- vertex stage

bool Pica::fetchVertex(uint32_t index, Vec4 in[16]) {
    for (int k = 0; k < 16; k++)
        in[k] = Vec4{{0, 0, 0, 1}};
    for (uint32_t k = 0; k < numAttrs_ && k < 12; k++) {
        Vec4 v{{0, 0, 0, 1}};
        const Attr &a = attrs_[k];
        if (a.fixed || a.buffer < 0) {
            v = fixedAttr_[k];
        } else {
            const Buffer &b = buffers_[a.buffer];
            static const unsigned kSize[4] = {1, 1, 2, 4};
            const unsigned csz = kSize[a.type];
            const uint32_t addr = b.base + index * b.stride + a.byteOffset;
            const uint8_t *p = physPtr(addr, csz * a.count);
            if (!p) {
                stats_.badAddress++;
                return false;
            }
            for (unsigned c = 0; c < a.count; c++) {
                switch (a.type) {
                case 0:
                    v[int(c)] = float(int8_t(p[c]));
                    break;
                case 1:
                    v[int(c)] = float(p[c]);
                    break;
                case 2: {
                    int16_t s;
                    std::memcpy(&s, p + 2 * c, 2);
                    v[int(c)] = float(s);
                    break;
                }
                default:
                    std::memcpy(&v[int(c)], p + 4 * c, 4);
                    break;
                }
            }
        }
        in[perm_[k] & 15] = v;
    }
    return true;
}

bool Pica::runVertex(const Vec4 in[16], ClipVertex *out) {
    Vec4 o[16];
    stats_.shaderRuns++;
    if (!shader_.run(in, o)) {
        stats_.shaderFailures++;
        return false;
    }
    float outv[kNumSlots + 8] = {};
    bool hasPos = false;
    // Semantic ids of the output map (libctru/3dbrew naming).
    for (uint32_t i = 0; i < numOut_; i++) {
        const uint32_t m = regs_[OUTMAP_O0 + i];
        for (int c = 0; c < 4; c++) {
            const uint32_t sem = (m >> (8 * c)) & 0x1F;
            const float val = o[outRegs_[i]][c];
            switch (sem) {
            case 0:
            case 1:
            case 2:
            case 3:
                out->pos[sem] = val;
                hasPos = true;
                break;
            case 4:
            case 5:
            case 6:
            case 7:
                out->a[kNormQuat + (sem - 4)] = val;
                break;
            case 8:
            case 9:
            case 10:
            case 11:
                out->a[kColor + (sem - 8)] = val;
                break;
            case 12:
                out->a[kTex0] = val;
                break;
            case 13:
                out->a[kTex0 + 1] = val;
                break;
            case 14:
                out->a[kTex1] = val;
                break;
            case 15:
                out->a[kTex1 + 1] = val;
                break;
            case 16:
                out->a[kTex0W] = val;
                break;
            case 18:
            case 19:
            case 20:
                out->a[kView + (sem - 18)] = val;
                break;
            case 22:
                out->a[kTex2] = val;
                break;
            case 23:
                out->a[kTex2 + 1] = val;
                break;
            default:
                break;
            }
        }
    }
    (void)outv;
    return hasPos;
}

std::string drawSummary(const uint32_t *regs);

void Pica::draw(bool indexed) {
    stats_.draws++;
    // Primitive mode 3 with the geometry shader off (seen in the game: lists of triangles) is drawn as a
    // plain list.
    if ((regs_[GEOSTAGE_CONFIG] & 3) == 2) {
        stats_.unsupportedGeometryShader++;
        if (drawLog_) {
            char gb[200];
            std::snprintf(
                gb, sizeof gb,
                "[geometry-shader draw skipped] geostage=%08x prim=%08x n=%u gsh_entry=%08x gsh_in=%08x "
                "gsh_mask=%08x geo2=%08x misc0=%08x misc1=%08x",
                regs_[GEOSTAGE_CONFIG], regs_[PRIMITIVE_CONFIG], regs_[NUMVERTICES], regs_[0x28A],
                regs_[0x289], regs_[0x28D], regs_[0x253], regs_[0x252], regs_[0x254]);
            drawLog_(drawLogUser_, std::string(gb));
        }
        stats_.drawsSkipped++;
        return;
    }
    if (!prepareTarget()) {
        stats_.drawsSkipped++;
        return;
    }
    // Shader constants that live in plain registers.
    shader_.entry = regs_[VSH_ENTRYPOINT] & 0xFFFF;
    for (int i = 0; i < 16; i++)
        shader_.bconst[i] = (regs_[VSH_BOOLUNIFORM] >> i) & 1;
    for (int i = 0; i < 4; i++)
        for (int c = 0; c < 4; c++)
            shader_.iconst[i][c] = uint8_t(regs_[VSH_INTUNIFORM0 + i] >> (8 * c));

    // Attribute layout.
    const uint32_t loc = (regs_[ATTRIBBUF_LOC] & 0x0FFFFFFF) * 8; // physical address of the vertex arrays
    const uint32_t fmtLow = regs_[ATTRIBBUF_FORMAT_LOW], fmtHigh = regs_[ATTRIBBUF_FORMAT_HIGH];
    numAttrs_ = (fmtHigh >> 28) + 1;
    const uint32_t fixedMask = (fmtHigh >> 16) & 0xFFF;
    for (int k = 0; k < 12; k++) {
        const uint32_t nib = k < 8 ? (fmtLow >> (4 * k)) & 0xF : (fmtHigh >> (4 * (k - 8))) & 0xF;
        attrs_[k] = {};
        attrs_[k].type = nib & 3;
        attrs_[k].count = (nib >> 2) + 1;
        attrs_[k].fixed = (fixedMask >> k) & 1;
    }
    for (int b = 0; b < 12; b++) {
        const uint32_t off = regs_[ATTRIBBUF0_OFFSET + 3 * b] & 0x0FFFFFFF;
        const uint32_t c1 = regs_[ATTRIBBUF0_OFFSET + 3 * b + 1], c2 = regs_[ATTRIBBUF0_OFFSET + 3 * b + 2];
        buffers_[b].base = loc + off;
        buffers_[b].stride = (c2 >> 16) & 0xFF;
        const uint32_t ncomp = c2 >> 28;
        uint32_t byteOff = 0;
        for (uint32_t c = 0; c < ncomp && c < 12; c++) {
            const uint32_t sel = c < 8 ? (c1 >> (4 * c)) & 0xF : (c2 >> (4 * (c - 8))) & 0xF;
            if (sel < 12) {
                static const unsigned kSize[4] = {1, 1, 2, 4};
                attrs_[sel].buffer = b;
                attrs_[sel].byteOffset = byteOff;
                byteOff += kSize[attrs_[sel].type] * attrs_[sel].count;
            } else {
                byteOff += 4 * (sel - 11); // padding: 4, 8, 12 or 16 bytes
            }
        }
    }
    for (int k = 0; k < 8; k++)
        perm_[k] = (regs_[VSH_ATTR_PERM_LOW] >> (4 * k)) & 0xF;
    for (int k = 0; k < 8; k++)
        perm_[8 + k] = (regs_[VSH_ATTR_PERM_HIGH] >> (4 * k)) & 0xF;
    // Output registers in use, in increasing order, are what the output map entries describe.
    numOut_ = 0;
    for (int i = 0; i < 16 && numOut_ < 7; i++)
        if ((regs_[VSH_OUTMAP_MASK] >> i) & 1)
            outRegs_[numOut_++] = uint32_t(i);
    if ((regs_[OUTMAP_TOTAL] & 7) < numOut_)
        numOut_ = regs_[OUTMAP_TOTAL] & 7;

    // Viewport and depth mapping.
    halfW_ = regF24(regs_[VIEWPORT_WIDTH]);
    halfH_ = regF24(regs_[VIEWPORT_HEIGHT]);
    vpX_ = float(int16_t((regs_[VIEWPORT_XY] & 0x3FF) << 6) >> 6);
    vpY_ = float(int16_t(((regs_[VIEWPORT_XY] >> 16) & 0x3FF) << 6) >> 6);
    depthScale_ = regF24(regs_[DEPTHMAP_SCALE]);
    depthOffset_ = regF24(regs_[DEPTHMAP_OFFSET]);
    prepareTextures();
    if (regs_[0x08F] & 1) {
        stats_.unsupportedLighting++;
        litSnapshot_ = regs_;
    }
    if (regs_[TEXENV_UPDATE_BUFFER] & 7)
        stats_.unsupportedFog++;
    if ((regs_[COLOR_OPERATION] & 3) == 3)
        stats_.unsupportedShadow++;
    if (drawLog_)
        drawLog_(drawLogUser_, drawSummary(regs_.data()));

    // Vertex indices.
    const uint32_t count = regs_[NUMVERTICES];
    const uint32_t first = regs_[VERTEX_OFFSET];
    std::vector<uint32_t> idx(count);
    if (indexed) {
        const uint32_t cfg = regs_[INDEXBUF_CONFIG];
        const bool u16 = cfg >> 31;
        const uint32_t addr = loc + (cfg & 0x0FFFFFFF);
        const uint8_t *p = physPtr(addr, count * (u16 ? 2 : 1));
        if (!p) {
            stats_.badAddress++;
            stats_.drawsSkipped++;
            return;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (u16) {
                uint16_t v;
                std::memcpy(&v, p + 2 * i, 2);
                idx[i] = v;
            } else {
                idx[i] = p[i];
            }
        }
    } else {
        for (uint32_t i = 0; i < count; i++)
            idx[i] = first + i;
    }
    std::map<uint32_t, ClipVertex> cache;
    std::vector<ClipVertex> verts(count);
    std::vector<bool> okv(count, false);
    for (uint32_t i = 0; i < count; i++) {
        auto it = cache.find(idx[i]);
        if (it == cache.end()) {
            Vec4 in[16];
            ClipVertex cv;
            if (fetchVertex(idx[i], in) && runVertex(in, &cv)) {
                it = cache.emplace(idx[i], cv).first;
                if (vertexHook_)
                    vertexHook_(count, i, in, cv);
            }
        }
        if (it != cache.end()) {
            verts[i] = it->second;
            okv[i] = true;
        }
    }
    uint32_t mode = (regs_[PRIMITIVE_CONFIG] >> 8) & 3;
    if (mode == 3)
        mode = 0;
    const uint64_t fragsBefore = stats_.fragments;
    const float bbBefore[4] = {stats_.bboxMin[0], stats_.bboxMin[1], stats_.bboxMax[0], stats_.bboxMax[1]};
    stats_.bboxMin[0] = stats_.bboxMin[1] = 1e30f;
    stats_.bboxMax[0] = stats_.bboxMax[1] = -1e30f;
    auto finish = [&] {
        if (drawEndLog_)
            drawEndLog_(drawLogUser_, count, stats_.fragments - fragsBefore, stats_.bboxMin, stats_.bboxMax);
        for (const auto &h : drawEndHooks_)
            h(count, stats_.fragments - fragsBefore, stats_.bboxMin, stats_.bboxMax);
        stats_.bboxMin[0] = std::min(stats_.bboxMin[0], bbBefore[0]);
        stats_.bboxMin[1] = std::min(stats_.bboxMin[1], bbBefore[1]);
        stats_.bboxMax[0] = std::max(stats_.bboxMax[0], bbBefore[2]);
        stats_.bboxMax[1] = std::max(stats_.bboxMax[1], bbBefore[3]);
    };
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (okv[a] && okv[b] && okv[c])
            drawTriangle(verts[a], verts[b], verts[c]);
    };
    if (mode == 0) {
        for (uint32_t i = 0; i + 2 < count; i += 3)
            tri(i, i + 1, i + 2);
    } else if (mode == 1) {
        for (uint32_t i = 0; i + 2 < count; i++)
            (i & 1) ? tri(i + 1, i, i + 2) : tri(i, i + 1, i + 2);
    } else {
        for (uint32_t i = 1; i + 1 < count; i++)
            tri(0, i, i + 1);
    }
    finish();
}

std::string drawSummary(const uint32_t *regs) {
    char b[400];
    std::snprintf(b, sizeof b,
                  "draw n=%u prim=%u fbdim=%08x color=%08x(%ux%u fmt %u) depth=%08x fmt=%u tex=%x "
                  "env0=%08x/%08x/%08x blend=%08x "
                  "func=%08x dmask=%08x alpha=%08x",
                  regs[NUMVERTICES], (regs[PRIMITIVE_CONFIG] >> 8) & 3, regs[FB_DIM], regs[COLORBUF_LOC] * 8,
                  regs[FB_DIM] & 0x7FF, ((regs[FB_DIM] >> 12) & 0x3FF) + 1, (regs[COLORBUF_FORMAT] >> 16) & 7,
                  regs[DEPTHBUF_LOC] * 8, regs[DEPTHBUF_FORMAT] & 3, regs[TEXUNIT_CONFIG] & 7, regs[TEXENV0],
                  regs[TEXENV0 + 1], regs[TEXENV0 + 2], regs[COLOR_OPERATION], regs[BLEND_FUNC],
                  regs[DEPTH_COLOR_MASK], regs[ALPHA_TEST]);
    char b3[200];
    std::snprintf(b3, sizeof b3, " | vp=%g,%g xy=%08x scissor=%x/%08x/%08x cull=%x depthmap=%x/%g/%g",
                  regF24(regs[VIEWPORT_WIDTH]), regF24(regs[VIEWPORT_HEIGHT]), regs[VIEWPORT_XY],
                  regs[SCISSOR_MODE] & 3, regs[SCISSOR_POS], regs[SCISSOR_DIM], regs[FACECULLING] & 3,
                  regs[DEPTHMAP_ENABLE] & 1, regF24(regs[DEPTHMAP_SCALE]), regF24(regs[DEPTHMAP_OFFSET]));
    char b2[400];
    std::snprintf(
        b2, sizeof b2,
        " | loc=%08x fmt=%08x/%08x buf0=%08x/%08x/%08x idx=%08x vsattr=%08x perm=%08x/%08x outmask=%08x "
        "outtot=%08x o0=%08x o1=%08x o2=%08x entry=%08x tex0=%08x/%08x/%08x/%08x",
        regs[ATTRIBBUF_LOC], regs[ATTRIBBUF_FORMAT_LOW], regs[ATTRIBBUF_FORMAT_HIGH], regs[ATTRIBBUF0_OFFSET],
        regs[ATTRIBBUF0_OFFSET + 1], regs[ATTRIBBUF0_OFFSET + 2], regs[INDEXBUF_CONFIG], regs[VSH_NUM_ATTR],
        regs[VSH_ATTR_PERM_LOW], regs[VSH_ATTR_PERM_HIGH], regs[VSH_OUTMAP_MASK], regs[OUTMAP_TOTAL],
        regs[OUTMAP_O0], regs[OUTMAP_O0 + 1], regs[OUTMAP_O0 + 2], regs[VSH_ENTRYPOINT], regs[TEXUNIT0_DIM],
        regs[TEXUNIT0_PARAM], regs[TEXUNIT0_ADDR], regs[TEXUNIT0_TYPE]);
    return std::string(b) + b3 + b2;
}

} // namespace pica
} // namespace pokerecomp
