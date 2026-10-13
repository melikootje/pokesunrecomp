#include "pokerecomp/dispatcher.h"

#include <algorithm>
#include <cstring>

#include "recomp/decode.h"
#include "recomp/interp.h"

namespace pokerecomp {
namespace {

// Guest memory as the fallback interpreter sees it: host base + 32-bit guest address.
class HostMemory : public recomp::Memory {
  public:
    explicit HostMemory(uint8_t *base) : base_(base) {}
    uint8_t r8(uint32_t a) override { return base_[a]; }
    uint16_t r16(uint32_t a) override {
        uint16_t v;
        std::memcpy(&v, base_ + a, 2);
        return v;
    }
    uint32_t r32(uint32_t a) override {
        uint32_t v;
        std::memcpy(&v, base_ + a, 4);
        return v;
    }
    void w8(uint32_t a, uint8_t v) override { base_[a] = v; }
    void w16(uint32_t a, uint16_t v) override { std::memcpy(base_ + a, &v, 2); }
    void w32(uint32_t a, uint32_t v) override { std::memcpy(base_ + a, &v, 4); }

  private:
    uint8_t *base_;
};

} // namespace

void Dispatcher::registerBlock(const BlockInfo &b) { blocks_[key(b.addr, b.thumb)] = b; }

void Dispatcher::invalidate(uint32_t addr, uint32_t size) {
    const uint64_t end = uint64_t(addr) + size;
    for (auto it = blocks_.begin(); it != blocks_.end();) {
        const uint64_t bs = it->second.addr, be = bs + it->second.bytes;
        if (bs < end && be > addr) {
            it = blocks_.erase(it);
            invalidated_++;
        } else {
            ++it;
        }
    }
}

void Dispatcher::resetCoverage() {
    cov_ = Coverage();
    fallback_.clear();
}

std::vector<std::pair<uint32_t, uint64_t>> Dispatcher::topFallbacks(size_t n) const {
    std::vector<std::pair<uint32_t, uint64_t>> v(fallback_.begin(), fallback_.end());
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) {
        return a.second > b.second || (a.second == b.second && a.first < b.first);
    });
    if (v.size() > n)
        v.resize(n);
    return v;
}

std::vector<uint32_t> Dispatcher::recentPcs() const {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < ring_.size(); i++) {
        const uint32_t v = ring_[(ringPos_ + i) % ring_.size()];
        if (v)
            out.push_back(v);
    }
    return out;
}

StopReason Dispatcher::run(RcCtx &c, uint64_t maxSteps) {
    HostMemory mem(c.mem);
    for (uint64_t step = 0; step < maxSteps; step++) {
        if (haltSet_ && c.r[15] == halt_)
            return StopReason::Halt;
        const uint32_t k = key(c.r[15], c.thumb != 0);
        if (!ring_.empty()) {
            ring_[ringPos_] = k;
            ringPos_ = (ringPos_ + 1) % ring_.size();
        }
        auto it = blocks_.find(k);
        if (it != blocks_.end()) {
            it->second.fn(&c);
            cov_.recompiledInsns += it->second.insns;
            cov_.blocksRun++;
        } else {
            const uint32_t pc = c.r[15];
            recomp::Insn insn;
            if (c.thumb) {
                uint16_t h = mem.r16(pc), h2 = mem.r16(pc + 2);
                insn = recomp::decodeThumb(h, h2, pc);
            } else {
                insn = recomp::decodeArm(mem.r32(pc), pc);
            }
            recomp::step(c, mem, insn);
            cov_.interpretedInsns++;
            fallback_[k]++;
        }
        if (c.trap)
            return StopReason::Trap;
        if (c.svc_pending)
            return StopReason::Svc;
    }
    return StopReason::StepLimit;
}

} // namespace pokerecomp
