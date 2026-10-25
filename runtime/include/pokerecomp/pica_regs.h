#pragma once
// PICA200 internal register ids and field decoders (layouts: public 3dbrew GPU documentation; each decoder
// used by the renderer is checked against the real game's command lists, see docs/gpu.md).
#include <cstdint>

namespace pokerecomp {
namespace pica {

constexpr uint32_t kNumRegs = 0x300;

enum Reg : uint32_t {
    FINALIZE = 0x010,
    FACECULLING = 0x040,
    VIEWPORT_WIDTH = 0x041,
    VIEWPORT_INVW = 0x042,
    VIEWPORT_HEIGHT = 0x043,
    VIEWPORT_INVH = 0x044,
    DEPTHMAP_ENABLE = 0x04C,
    DEPTHMAP_SCALE = 0x04D,
    DEPTHMAP_OFFSET = 0x04E,
    OUTMAP_TOTAL = 0x04F,
    OUTMAP_O0 = 0x050, // .. 0x056
    SCISSOR_MODE = 0x065,
    SCISSOR_POS = 0x066,
    SCISSOR_DIM = 0x067,
    VIEWPORT_XY = 0x068,
    DEPTHMAP_MODE = 0x06D,
    RENDERBUF_DIM = 0x06E,
    TEXUNIT_CONFIG = 0x080,
    TEXUNIT0_BORDER = 0x081,
    TEXUNIT0_DIM = 0x082,
    TEXUNIT0_PARAM = 0x083,
    TEXUNIT0_LOD = 0x084,
    TEXUNIT0_ADDR = 0x085,
    TEXUNIT0_TYPE = 0x08E,
    TEXUNIT1_BORDER = 0x091,
    TEXUNIT1_DIM = 0x092,
    TEXUNIT1_PARAM = 0x093,
    TEXUNIT1_LOD = 0x094,
    TEXUNIT1_ADDR = 0x095,
    TEXUNIT1_TYPE = 0x096,
    TEXUNIT2_BORDER = 0x099,
    TEXUNIT2_DIM = 0x09A,
    TEXUNIT2_PARAM = 0x09B,
    TEXUNIT2_LOD = 0x09C,
    TEXUNIT2_ADDR = 0x09D,
    TEXUNIT2_TYPE = 0x09E,
    TEXENV0 = 0x0C0, // SOURCE, OPERAND, COMBINE, COLOR, SCALE at +0..+4
    TEXENV1 = 0x0C8,
    TEXENV2 = 0x0D0,
    TEXENV3 = 0x0D8,
    TEXENV_UPDATE_BUFFER = 0x0E0,
    TEXENV4 = 0x0F0,
    TEXENV5 = 0x0F8,
    TEXENV_BUFFER_COLOR = 0x0FD,
    COLOR_OPERATION = 0x100,
    BLEND_FUNC = 0x101,
    LOGIC_OP = 0x102,
    BLEND_COLOR = 0x103,
    ALPHA_TEST = 0x104,
    STENCIL_TEST = 0x105,
    STENCIL_OP = 0x106,
    DEPTH_COLOR_MASK = 0x107,
    FB_INVALIDATE = 0x110,
    FB_FLUSH = 0x111,
    COLORBUF_READ = 0x112,
    COLORBUF_WRITE = 0x113,
    DEPTHBUF_READ = 0x114,
    DEPTHBUF_WRITE = 0x115,
    DEPTHBUF_FORMAT = 0x116,
    COLORBUF_FORMAT = 0x117,
    FB_BLOCK32 = 0x11B,
    DEPTHBUF_LOC = 0x11C,
    COLORBUF_LOC = 0x11D,
    FB_DIM = 0x11E,
    ATTRIBBUF_LOC = 0x200,
    ATTRIBBUF_FORMAT_LOW = 0x201,
    ATTRIBBUF_FORMAT_HIGH = 0x202,
    ATTRIBBUF0_OFFSET = 0x203, // 12 buffers x {OFFSET, CONFIG1, CONFIG2}
    INDEXBUF_CONFIG = 0x227,
    NUMVERTICES = 0x228,
    GEOSTAGE_CONFIG = 0x229,
    VERTEX_OFFSET = 0x22A,
    DRAWARRAYS = 0x22E,
    DRAWELEMENTS = 0x22F,
    CMDBUF_SIZE0 = 0x238, // size >> 3 (bits 0-20)
    CMDBUF_ADDR0 = 0x23A, // physical address >> 3
    CMDBUF_JUMP0 = 0x23C, // writing runs the buffer described by SIZE0/ADDR0; the 1 registers do the same
    FIXEDATTRIB_INDEX = 0x232,
    FIXEDATTRIB_DATA0 = 0x233,
    VSH_NUM_ATTR = 0x242,
    PRIMITIVE_CONFIG = 0x25E,
    RESTART_PRIMITIVE = 0x25F,
    VSH_BOOLUNIFORM = 0x2B0,
    VSH_INTUNIFORM0 = 0x2B1,
    VSH_INPUTBUF_CONFIG = 0x2B9,
    VSH_ENTRYPOINT = 0x2BA,
    VSH_ATTR_PERM_LOW = 0x2BB,
    VSH_ATTR_PERM_HIGH = 0x2BC,
    VSH_OUTMAP_MASK = 0x2BD,
    VSH_CODETRANSFER_END = 0x2BF,
    VSH_FLOATUNIFORM_CONFIG = 0x2C0,
    VSH_FLOATUNIFORM_DATA = 0x2C1, // .. 0x2C8
    VSH_CODETRANSFER_CONFIG = 0x2CB,
    VSH_CODETRANSFER_DATA = 0x2CC, // .. 0x2D3
    VSH_OPDESCS_CONFIG = 0x2D5,
    VSH_OPDESCS_DATA = 0x2D6, // .. 0x2DD
};

// float24 (1 sign, 7 exponent, 16 mantissa; bias 63) <-> float32.
float f24ToF32(uint32_t v24);
uint32_t f32ToF24(float f);
// float32 variants used by the lighting/texture registers: float1.7.16 in the low 24 bits.
inline float regF24(uint32_t v) { return f24ToF32(v & 0xFFFFFF); }

} // namespace pica
} // namespace pokerecomp
