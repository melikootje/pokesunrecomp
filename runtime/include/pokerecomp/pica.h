#pragma once
// PICA200 3D pipeline: command lists, register state, vertex processing and a software rasterizer (the
// reference renderer; a GPU backend can reuse the decoded state later). See docs/gpu.md.
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "pokerecomp/pica_regs.h"
#include "pokerecomp/pica_shader.h"

namespace pokerecomp {
namespace pica {

struct PicaStats {
    float bboxMin[2] = {1e30f, 1e30f}, bboxMax[2] = {-1e30f, -1e30f}; // screen extent of rasterized triangles
    uint64_t commandLists = 0, regWrites = 0, draws = 0, triangles = 0, culled = 0, clipped = 0,
             fragments = 0, shaderRuns = 0, shaderFailures = 0, drawsSkipped = 0;
    // Features the renderer does not implement, counted when a draw used them (so they show up in reports).
    uint64_t unsupportedLighting = 0, unsupportedProcTex = 0, unsupportedFog = 0, unsupportedShadow = 0,
             unsupportedGeometryShader = 0, unsupportedTexFormat = 0, unsupportedCube = 0, badAddress = 0,
             unsupportedImmediate = 0, unsupportedLogicOp = 0;
    uint64_t bufferJumps = 0, jumpsRejected = 0; // nested command buffers executed / refused
    // Float uniforms uploaded as NaN (a sign that the guest computed a bad matrix); where the first one came from.
    uint64_t nanUniforms = 0;
    uint64_t texFormatDraws[3][16] = {}; // draws that sampled each texture format, per unit
    uint32_t firstNanList = 0, firstNanWord = 0, firstNanReg = 0;
    uint32_t lastNanList = 0, lastNanWord = 0, lastNanReg = 0;
};

// Interpolated vertex output slots.
enum Slot : int {
    kColor = 0,     // 4
    kTex0 = 4,      // 2
    kTex1 = 6,      // 2
    kTex2 = 8,      // 2
    kTex0W = 10,    // 1
    kView = 11,     // 3
    kNormQuat = 14, // 4
    kNumSlots = 18,
};

struct ClipVertex {
    float pos[4] = {0, 0, 0, 1};
    float a[kNumSlots] = {};
};

// A texture unit as the sampler sees it.
struct TexInfo {
    uint8_t *data = nullptr;
    uint32_t width = 0, height = 0;
    unsigned format = 0, wrapS = 0, wrapT = 0;
    bool magLinear = false, minLinear = false;
    float border[4] = {0, 0, 0, 0};
};
// Samples a texture at normalised coordinates; returns false for formats that are not implemented.
bool sampleTexture(const TexInfo &t, float u, float v, float rgba[4]);
// Decodes one texel (x, y, with y counted from the first row stored) to RGBA in [0, 1].
bool fetchTexel(const TexInfo &t, int x, int y, float rgba[4]);

class Pica {
  public:
    // `mem` is the host address of guest address 0.
    explicit Pica(uint8_t *mem) : mem_(mem) { regs_.fill(0); }

    // Executes a command list at guest virtual address `vaddr`.
    void processCommandList(uint32_t vaddr, uint32_t bytes);
    // One register write with the byte-enable mask of the command header.
    void writeRegister(uint32_t id, uint32_t value, uint32_t mask = 0xF);

    uint32_t reg(uint32_t id) const { return regs_[id]; }
    // Fragment lighting for one fragment from interpolated attributes: out = primary rgb, secondary rgb.
    // Public so tests can check it without rasterizing. See docs/gpu.md for what is and is not modelled.
    void shadeLighting(const float *attr, float out[6]) const;
    // Register file as it was at the latest draw that used fragment lighting (diagnostics).
    const std::array<uint32_t, kNumRegs> &litSnapshot() const { return litSnapshot_; }
    const PicaStats &stats() const { return stats_; }
    PicaStats &mutableStats() { return stats_; }
    // When false, draws run the vertex stage and are counted but nothing is rasterized (fast-forwarding a
    // run).
    void setRasterEnabled(bool on) { rasterEnabled_ = on; }
    ShaderUnit &shader() { return shader_; }
    // Per-draw log hook for diagnostics (receives one line per draw).
    // Called when a draw finishes: vertex count, fragments produced and the triangle extent in screen space.
    void setDrawEndLog(void (*fn)(void *, uint32_t, uint64_t, const float *, const float *)) {
        drawEndLog_ = fn;
    }
    // Diagnostics: called for every vertex the shader processes, with (draw vertex count, index, inputs,
    // output).
    // Diagnostics: called for every enabled texture unit at the start of each draw (unit, texture, draw's TEV
    // stage 0 source word).
    void setTextureHook(std::function<void(int, const TexInfo &, uint32_t)> fn) { texHook_ = std::move(fn); }
    void setVertexHook(std::function<void(uint32_t, uint32_t, const Vec4 *, const ClipVertex &)> fn) {
        vertexHook_ = std::move(fn);
    }
    // Any number of observers, called after each draw with (vertex count, fragments, screen-space min, max).
    void addDrawEndHook(std::function<void(uint32_t, uint64_t, const float *, const float *)> fn) {
        drawEndHooks_.push_back(std::move(fn));
    }
    void setDrawLog(void (*fn)(void *, const std::string &), void *user) {
        drawLog_ = fn;
        drawLogUser_ = user;
    }

  private:
    void runWords(const uint32_t *w, uint32_t n, uint32_t srcVaddr);
    bool jumpPending_ = false; // a CMDBUF_JUMP trigger was written: continue in the target buffer
    uint32_t jumpVaddr_ = 0, jumpBytes_ = 0;
    uint32_t listVaddr_ = 0, listWord_ = 0; // position of the command being executed (diagnostics)
    void draw(bool indexed);
    bool prepareTarget();
    bool fetchVertex(uint32_t index, Vec4 in[16]);
    bool runVertex(const Vec4 in[16], ClipVertex *out);
    void drawTriangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c);
    void rasterTriangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c);
    void fragment(int x, int y, float depth, const float *attr);
    void prepareTextures();
    uint8_t *physPtr(uint32_t paddr, uint32_t size) const;

    uint8_t *mem_;
    std::array<uint32_t, kNumRegs> regs_;
    std::array<uint32_t, kNumRegs> litSnapshot_{};
    std::function<void(int, const TexInfo &, uint32_t)> texHook_;
    ShaderUnit shader_;
    PicaStats stats_;
    void (*drawLog_)(void *, const std::string &) = nullptr;
    void *drawLogUser_ = nullptr;
    void (*drawEndLog_)(void *, uint32_t, uint64_t, const float *, const float *) = nullptr;
    std::vector<std::function<void(uint32_t, uint64_t, const float *, const float *)>> drawEndHooks_;
    std::function<void(uint32_t, uint32_t, const Vec4 *, const ClipVertex &)> vertexHook_;

    // FIFO state
    uint32_t uniformIndex_ = 0, uniformWords_[4] = {}, uniformCount_ = 0;
    uint32_t codeIndex_ = 0, descIndex_ = 0;
    uint32_t fixedWords_[3] = {}, fixedCount_ = 0;
    Vec4 fixedAttr_[12];
    bool finalized_ = false;
    bool rasterEnabled_ = true;

    // state resolved at the start of a draw
    struct Fb {
        uint8_t *color = nullptr, *depth = nullptr;
        uint32_t width = 0, height = 0;
        unsigned colorBytes = 4, depthBytes = 0, colorFmt = 0;
        bool block32 = false;
        bool flip = true; // FB_DIM bit 24 set (what the game uses): row y is stored at height - 1 - y
    } fb_;
    struct Attr {
        int buffer = -1;
        uint32_t byteOffset = 0;
        unsigned type = 0, count = 0;
        bool fixed = false;
    } attrs_[12];
    struct Buffer {
        uint32_t base = 0, stride = 0;
    } buffers_[12];
    uint32_t numAttrs_ = 0, perm_[16] = {};
    uint32_t outRegs_[7] = {}, numOut_ = 0;
    TexInfo tex_[3];
    bool texEnabled_[3] = {false, false, false};
    bool stageActive_[6] = {true, true, true, true, true, true}; // pass-through TEV stages are skipped
    float halfW_ = 0, halfH_ = 0, vpX_ = 0, vpY_ = 0, depthScale_ = 0, depthOffset_ = 0;
    bool unsupportedTexThisDraw_ = false;
};

} // namespace pica
} // namespace pokerecomp
