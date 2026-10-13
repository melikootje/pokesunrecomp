#pragma once
#include <string>
#include <vector>

#include "recomp/decode.h"

namespace recomp {

struct EmitOptions {
    // Flags that must be correct when the block exits. Anything else may be eliminated.
    uint8_t liveOut = FlagAll;
    bool eliminateDeadFlags = true;
};

// Emits `void <name>(RcCtx* c)` that executes `insns` in order (only the last may end the block).
// On exit c->r[15] holds the next pc (and c->thumb the instruction set).
std::string emitBlockC(const std::vector<Insn> &insns, const std::string &name, const EmitOptions &opt = {});

// Standard prelude (includes) for a generated translation unit.
std::string emitPrelude();

} // namespace recomp
