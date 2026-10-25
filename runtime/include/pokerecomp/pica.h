#pragma once
// PICA200 3D pipeline: command lists, register state, vertex processing and a software rasterizer (the
// reference renderer; a GPU backend can reuse the decoded state later). See docs/gpu.md.
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pokerecomp/pica_regs.h"
#include "pokerecomp/pica_shader.h"

namespace pokerecomp {
namespace pica {

struct PicaStats {
    uint64_t commandLists = 0, regWrites = 0, draws = 0, triangles = 0, culled = 0, clipped = 0,
             fragments = 0, shaderRuns = 0, shaderFailures = 0, drawsSkipped = 0;
    // Features the renderer does not implement, counted when a draw used them (so they show up in reports).
    uint64_t unsupportedLighting = 0, unsupportedProcTex = 0, unsupportedFog = 0, unsupportedShadow = 0,
             unsupportedGeometryShader = 0, unsupportedTexFormat = 0, unsupportedCube = 0, badAddress = 0,
             unsupportedImmediate = 0, unsupportedLogicOp = 0;
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
    const PicaStats &stats() const { return stats_; }
    ShaderUnit &shader() { return shader_; }
    // Per-draw log hook for diagnostics (receives one line per draw).
    void setDrawLog(void (*fn)(void *, const std::string &), void *user) {
        drawLog_ = fn;
        drawLogUser_ = user;
    }

  private:
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
    ShaderUnit shader_;
    PicaStats stats_;
    void (*drawLog_)(void *, const std::string &) = nullptr;
    void *drawLogUser_ = nullptr;

    // FIFO state
    uint32_t uniformIndex_ = 0, uniformWords_[4] = {}, uniformCount_ = 0;
    uint32_t codeIndex_ = 0, descIndex_ = 0;
    uint32_t fixedWords_[3] = {}, fixedCount_ = 0;
    Vec4 fixedAttr_[12];
    bool finalized_ = false;

    // state resolved at the start of a draw
    struct Fb {
        uint8_t *color = nullptr, *depth = nullptr;
        uint32_t width = 0, height = 0;
        unsigned colorBytes = 4, depthBytes = 0, colorFmt = 0;
        bool block32 = false;
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
    float halfW_ = 0, halfH_ = 0, vpX_ = 0, vpY_ = 0, depthScale_ = 0, depthOffset_ = 0;
    bool unsupportedTexThisDraw_ = false;
};

} // namespace pica
} // namespace pokerecomp
