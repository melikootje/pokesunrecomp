#pragma once
#include "pokerecomp/cpu.h"
#include "recomp/decode.h"

namespace recomp {

// Guest memory as seen by the reference interpreter (little endian). Implementations may throw on faults.
class Memory {
  public:
    virtual ~Memory() = default;
    virtual uint8_t r8(uint32_t a) = 0;
    virtual uint16_t r16(uint32_t a) = 0;
    virtual uint32_t r32(uint32_t a) = 0;
    virtual void w8(uint32_t a, uint8_t v) = 0;
    virtual void w16(uint32_t a, uint16_t v) = 0;
    virtual void w32(uint32_t a, uint32_t v) = 0;
};

// Executes one decoded instruction with straightforward (non-optimised) semantics. On return
// ctx.r[15] is the next pc. Undefined/unsupported encodings set ctx.trap and leave r15 at the instruction.
void step(RcCtx &ctx, Memory &mem, const Insn &insn);

} // namespace recomp
