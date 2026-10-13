#pragma once
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "pokerecomp/cpu.h"

namespace pokerecomp {

// A recompiled basic block: executes guest code starting at `addr` and leaves the next pc in ctx->r[15]
// (and the instruction set in ctx->thumb).
using BlockFn = void (*)(RcCtx *);

struct BlockInfo {
    BlockFn fn = nullptr;
    uint32_t addr = 0;  // guest address of the first instruction
    uint32_t bytes = 0; // guest code bytes covered (for invalidation)
    uint32_t insns = 0; // guest instructions executed by one pass through the block (for coverage)
    bool thumb = false;
};

enum class StopReason { Svc, Trap, StepLimit, Halt };

struct Coverage {
    uint64_t recompiledInsns = 0;  // guest instructions executed inside recompiled blocks
    uint64_t interpretedInsns = 0; // guest instructions executed by the fallback interpreter
    uint64_t blocksRun = 0;
    // Share of executed guest instructions served by recompiled code (100 when nothing ran).
    double percentRecompiled() const {
        const uint64_t total = recompiledInsns + interpretedInsns;
        return total ? 100.0 * double(recompiledInsns) / double(total) : 100.0;
    }
};

// Address -> recompiled block lookup with an interpreter fallback for anything not statically recovered
// (indirect-branch targets, runtime-generated code). Guest memory is `ctx.mem` (host base + guest address).
class Dispatcher {
  public:
    void registerBlock(const BlockInfo &b);
    size_t blockCount() const { return blocks_.size(); }
    bool hasBlock(uint32_t addr, bool thumb) const { return blocks_.count(key(addr, thumb)) != 0; }

    // Drops every block overlapping [addr, addr + size): used when guest memory that held code is rewritten
    // (CRO load/unload, ControlMemory remaps, self-modifying code). Their pcs then run in the interpreter
    // until they are re-registered.
    void invalidate(uint32_t addr, uint32_t size);
    uint64_t invalidatedBlocks() const { return invalidated_; }

    // Stop (StopReason::Halt) when control reaches this address, e.g. the return address of the entry point.
    void setHaltAddress(uint32_t addr) {
        halt_ = addr;
        haltSet_ = true;
    }

    // Runs until an SVC is pending, a trap occurs, the halt address is reached, or maxSteps dispatches
    // (one block or one interpreted instruction per step) have been executed. A pending SVC is left in
    // ctx.svc/ctx.svc_pending for the caller (HLE) to service and clear.
    StopReason run(RcCtx &ctx, uint64_t maxSteps);

    const Coverage &coverage() const { return cov_; }
    void resetCoverage();
    // Guest pcs (bit 0 = Thumb) executed by the fallback interpreter and how often.
    const std::unordered_map<uint32_t, uint64_t> &fallbackHits() const { return fallback_; }
    // Most-executed fallback pcs first.
    std::vector<std::pair<uint32_t, uint64_t>> topFallbacks(size_t n) const;

  private:
    static uint32_t key(uint32_t addr, bool thumb) { return addr | (thumb ? 1u : 0u); }
    std::map<uint32_t, BlockInfo> blocks_; // ordered by key: invalidation scans a range
    std::unordered_map<uint32_t, uint64_t> fallback_;
    Coverage cov_;
    uint64_t invalidated_ = 0;
    uint32_t halt_ = 0;
    bool haltSet_ = false;
};

} // namespace pokerecomp
