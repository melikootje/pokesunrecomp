#include "recomp/blocks.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "recomp/emit_c.h"

namespace recomp {
namespace {

uint32_t key(uint32_t addr, bool thumb) { return addr | (thumb ? 1u : 0u); }

bool decodeAt(const ImageReader &img, uint32_t addr, bool thumb, Insn *out) {
    if (thumb) {
        uint16_t h[2] = {0, 0};
        if (!img.read(addr, &h[0], 2))
            return false;
        if (!img.read(addr + 2, &h[1], 2))
            h[1] = 0;
        *out = decodeThumb(h[0], h[1], addr);
        return true;
    }
    uint32_t w;
    if (!img.read(addr, &w, 4))
        return false;
    *out = decodeArm(w, addr);
    return true;
}

bool isJumpTableDispatch(const Insn &i) {
    if (i.thumb || i.rd != 15 || i.rn != 15)
        return false;
    if (i.op == Op::Ldr)
        return i.pre && i.up && !i.wb && i.regOffset && i.shift == Shift::Lsl && i.shiftAmt == 2;
    if (i.op == Op::Add)
        return !i.immOperand && !i.shiftByReg && i.shift == Shift::Lsl && i.shiftAmt == 2 && !i.setFlags;
    return false;
}

} // namespace

bool recoverJumpTable(const ImageReader &img, const Insn &d, const std::vector<Insn> &before,
                      std::vector<uint32_t> *targets) {
    if (!isJumpTableDispatch(d))
        return false;
    // The bound comes from a preceding `cmp rX, #N` on the index register (valid for indices 0..N).
    uint32_t count = 0;
    for (size_t k = before.size(), seen = 0; k-- > 0 && seen < 6; seen++) {
        const Insn &c = before[k];
        if (c.op == Op::Cmp && c.immOperand && c.rn == d.rm && !c.thumb) {
            // The dispatch only runs when the index is in range: LS -> 0..N, CC (lower) -> 0..N-1.
            if (d.cond == 9)
                count = c.imm + 1;
            else if (d.cond == 3)
                count = c.imm;
            break;
        }
    }
    if (count == 0 || count > 1024)
        return false;
    const uint32_t table = d.pc + 8;
    std::vector<uint32_t> out;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t w;
        if (!img.read(table + 4 * k, &w, 4))
            return false;
        uint32_t t;
        if (d.op == Op::Ldr) {
            t = w; // absolute address (ARM, bit 0 clear)
            if (t & 3)
                return false;
        } else {
            const Insn b = decodeArm(w, table + 4 * k); // slot holds a `b target`
            if (b.op != Op::B || b.cond != 14)
                return false;
            t = b.target;
        }
        uint32_t probe;
        if (!img.read(t, &probe, 4))
            return false;
        out.push_back(t);
    }
    *targets = out;
    return true;
}

std::vector<Block> discoverBlocks(const ImageReader &img, const std::vector<Entry> &entries,
                                  const DiscoverOptions &opt, DiscoverStats *stats) {
    std::map<uint32_t, Insn> decoded; // key -> instruction
    std::set<uint32_t> leaders;
    std::vector<uint32_t> work;
    auto addLeader = [&](uint32_t addr, bool thumb) {
        if (leaders.insert(key(addr, thumb)).second)
            work.push_back(key(addr, thumb));
    };
    DiscoverStats st;
    for (const Entry &e : entries)
        addLeader(e.addr, e.thumb);

    while (!work.empty()) {
        const uint32_t k = work.back();
        work.pop_back();
        const bool thumb = k & 1;
        uint32_t addr = k & ~1u;
        std::vector<Insn> recent;
        for (size_t n = 0;; n++) {
            const uint32_t ik = key(addr, thumb);
            if (n > 0 && (decoded.count(ik) || leaders.count(ik)))
                break; // already analysed from here on
            if (opt.isData && opt.isData(addr))
                break;
            Insn insn;
            if (!decodeAt(img, addr, thumb, &insn))
                break;
            decoded[ik] = insn;
            recent.push_back(insn);
            if (insn.op == Op::Undef)
                break;
            if (!insn.endsBlock()) {
                addr = insn.next();
                continue;
            }
            // Successors
            const bool cond = insn.cond != 14;
            switch (insn.op) {
            case Op::B:
                addLeader(insn.target, insn.thumb);
                break;
            case Op::Bl:
                addLeader(insn.target, insn.thumb);
                addLeader(insn.next(), insn.thumb);
                break;
            case Op::BlxImm:
                addLeader(insn.target, insn.targetThumb);
                addLeader(insn.next(), insn.thumb);
                break;
            case Op::BlxReg:
                addLeader(insn.next(), insn.thumb);
                st.unresolvedIndirect++;
                break;
            case Op::Svc:
                addLeader(insn.next(), insn.thumb);
                break;
            default: {
                std::vector<uint32_t> targets;
                std::vector<Insn> before(recent.begin(), recent.end() - 1);
                if (recoverJumpTable(img, insn, before, &targets)) {
                    if (opt.onJumpTable)
                        opt.onJumpTable(insn.pc, targets);
                    st.jumpTables++;
                    st.jumpTableTargets += targets.size();
                    for (uint32_t t : targets)
                        addLeader(t, false);
                } else if (insn.op != Op::Undef) {
                    st.unresolvedIndirect++; // bx / pop {pc} / ldr pc / mov pc: resolved at run time
                }
                break;
            }
            }
            if (cond && insn.op != Op::Undef)
                addLeader(insn.next(), insn.thumb);
            break;
        }
    }

    std::vector<Block> blocks;
    for (uint32_t k : leaders) {
        Block b;
        b.thumb = k & 1;
        b.addr = k & ~1u;
        uint32_t addr = b.addr;
        for (size_t n = 0; n < opt.maxBlockInsns; n++) {
            const uint32_t ik = key(addr, b.thumb);
            auto it = decoded.find(ik);
            if (it == decoded.end())
                break;
            if (n > 0 && leaders.count(ik))
                break;
            b.insns.push_back(it->second);
            addr = it->second.next();
            if (it->second.endsBlock() || it->second.op == Op::Undef)
                break;
        }
        if (!b.insns.empty())
            blocks.push_back(std::move(b));
    }
    std::sort(blocks.begin(), blocks.end(), [](const Block &a, const Block &b) {
        return a.addr < b.addr || (a.addr == b.addr && a.thumb < b.thumb);
    });
    st.blocks = blocks.size();
    for (const auto &b : blocks)
        st.insns += b.insns.size();
    if (stats)
        *stats = st;
    return blocks;
}

std::string blockSymbol(const Block &b) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "rc_blk_%08x_%c", b.addr, b.thumb ? 't' : 'a');
    return buf;
}

std::string emitBlockFunctionsC(const std::vector<Block> &blocks, size_t begin, size_t end) {
    std::string out = emitPrelude();
    for (size_t k = begin; k < end && k < blocks.size(); k++)
        out += emitBlockC(blocks[k].insns, blockSymbol(blocks[k]));
    return out;
}

std::string emitBlockTableC(const std::vector<Block> &blocks) {
    std::string out = "#include \"pokerecomp/cpu.h\"\n";
    char buf[192];
    for (const Block &b : blocks)
        out += "void " + blockSymbol(b) + "(RcCtx*);\n";
    out += "typedef void (*rc_fn)(RcCtx*);\n";
    out += "typedef struct { rc_fn fn; uint32_t addr; uint32_t bytes; uint32_t insns; uint32_t thumb; } "
           "rc_block_t;\n";
    out += "const rc_block_t rc_blocks[] = {\n";
    for (const Block &b : blocks) {
        std::snprintf(buf, sizeof buf, "    {%s, 0x%08xu, %uu, %zuu, %uu},\n", blockSymbol(b).c_str(), b.addr,
                      b.bytes(), b.insns.size(), b.thumb ? 1u : 0u);
        out += buf;
    }
    out += "    {0, 0, 0, 0, 0}};\n";
    std::snprintf(buf, sizeof buf, "const unsigned rc_block_count = %zu;\n", blocks.size());
    out += buf;
    return out;
}

std::string emitBlocksC(const std::vector<Block> &blocks) {
    // Single translation unit: functions first, then the table without extern declarations.
    std::string out = emitBlockFunctionsC(blocks, 0, blocks.size());
    std::string table = emitBlockTableC(blocks);
    // drop the include line and extern prototypes (the definitions above already declare the functions)
    const size_t k = table.find("typedef void (*rc_fn)");
    return out + table.substr(k);
}

} // namespace recomp
