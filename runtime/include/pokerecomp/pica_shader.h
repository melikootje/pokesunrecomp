#pragma once
// PICA200 vertex shader interpreter (ISA: public 3dbrew documentation). Arithmetic is done in IEEE float32
// with the documented PICA quirks that matter in practice (0 * inf = 0, GLSL-style min/max); the 16-bit
// mantissa of the hardware's float24 is not reproduced.
#include <cstdint>

namespace pokerecomp {
namespace pica {

struct Vec4 {
    float v[4] = {0, 0, 0, 0};
    float &operator[](int i) { return v[i]; }
    float operator[](int i) const { return v[i]; }
};

constexpr uint32_t kShaderCodeSize = 512, kOpDescSize = 128;

struct ShaderUnit {
    uint32_t code[kShaderCodeSize] = {};
    uint32_t opdesc[kOpDescSize] = {};
    Vec4 fconst[96];
    uint8_t iconst[4][4] = {}; // i0..i3 = {x, y, z, w}
    bool bconst[16] = {};
    uint32_t entry = 0;

    uint64_t executed = 0; // instructions run since the last reset (statistics)

    // Runs the program from `entry` with the given input registers; `out` receives o0-o15 (registers the
    // program does not write are left untouched). Returns false when the program is malformed or runs away.
    bool run(const Vec4 in[16], Vec4 out[16]);
};

} // namespace pica
} // namespace pokerecomp
