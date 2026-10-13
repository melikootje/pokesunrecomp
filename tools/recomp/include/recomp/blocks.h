#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "recomp/decode.h"

namespace recomp {

// Read access to a guest code image (host-side, for static analysis).
class ImageReader {
  public:
    virtual ~ImageReader() = default;
    virtual bool read(uint32_t addr, void *dst, size_t n) const = 0;
};

struct Entry {
    uint32_t addr = 0;
    bool thumb = false;
};

struct Block {
    uint32_t addr = 0;
    bool thumb = false;
    std::vector<Insn> insns;
    uint32_t bytes() const {
        uint32_t n = 0;
        for (const auto &i : insns)
            n += i.size;
        return n;
    }
};

struct DiscoverOptions {
    size_t maxBlockInsns = 256;
    // Addresses that are known data (literal pools): never decoded as code.
    std::function<bool(uint32_t)> isData;
    // Called for every jump table whose targets were recovered (site = address of the dispatch instruction).
    std::function<void(uint32_t site, const std::vector<uint32_t> &targets)> onJumpTable;
};

struct DiscoverStats {
    size_t blocks = 0, insns = 0, jumpTables = 0, jumpTableTargets = 0, unresolvedIndirect = 0;
};

// Recognises the compiler's bounded jump-table dispatch (`cmp rX,#N` then `ldrls pc,[pc,rX,lsl #2]` or
// `add pc,pc,rX,lsl #2`) and returns its (ARM-mode) targets. `before` holds the instructions that precede
// `dispatch` in the same block. Returns false when the pattern, the bound or any target is not recognised.
bool recoverJumpTable(const ImageReader &img, const Insn &dispatch, const std::vector<Insn> &before,
                      std::vector<uint32_t> *targets);

// Statically recovers basic blocks reachable from `entries`, splitting at every branch target and
// fallthrough.
std::vector<Block> discoverBlocks(const ImageReader &img, const std::vector<Entry> &entries,
                                  const DiscoverOptions &opt = {}, DiscoverStats *stats = nullptr);

// C source: one function per block plus `rc_blocks[]`/`rc_block_count` describing them
// (fields: fn, addr, bytes, insns, thumb — the layout of pokerecomp::BlockInfo).
std::string emitBlocksC(const std::vector<Block> &blocks);

// Split form for large images: functions for blocks[begin, end) ...
std::string emitBlockFunctionsC(const std::vector<Block> &blocks, size_t begin, size_t end);
// ... and the table (with extern declarations) over all blocks.
std::string emitBlockTableC(const std::vector<Block> &blocks);
std::string blockSymbol(const Block &b);

} // namespace recomp
