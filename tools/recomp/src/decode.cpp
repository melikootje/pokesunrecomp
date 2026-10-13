#include "recomp/decode.h"

#include <cstdio>

namespace recomp {
namespace {

constexpr uint32_t ror32(uint32_t v, unsigned n) { return n ? (v >> n) | (v << (32 - n)) : v; }

bool isLogical(Op op) {
    switch (op) {
    case Op::And:
    case Op::Eor:
    case Op::Tst:
    case Op::Teq:
    case Op::Orr:
    case Op::Mov:
    case Op::Bic:
    case Op::Mvn:
        return true;
    default:
        return false;
    }
}

bool isCompare(Op op) { return op == Op::Tst || op == Op::Teq || op == Op::Cmp || op == Op::Cmn; }

void finishFlags(Insn &i) {
    i.readsFlags = condFlagsRead(i.cond);
    i.writesFlags = 0;
    if (i.op >= Op::And && i.op <= Op::Mvn) {
        if (i.op == Op::Adc || i.op == Op::Sbc || i.op == Op::Rsc)
            i.readsFlags |= FlagC;
        if (i.setFlags) {
            if (isLogical(i.op)) {
                i.writesFlags = FlagN | FlagZ | FlagC;
                // Carry comes from the shifter; if the shifter can leave C unchanged, C is also read.
                bool carryDefined = i.immOperand
                                        ? i.immCarryDefined
                                        : (!i.shiftByReg && !(i.shift == Shift::Lsl && i.shiftAmt == 0));
                if (!carryDefined || (!i.immOperand && i.shift == Shift::Rrx))
                    i.readsFlags |= FlagC;
                if (i.shiftByReg)
                    i.readsFlags |= FlagC; // amount 0 leaves C unchanged
            } else {
                i.writesFlags = FlagAll;
            }
        }
        if (!i.immOperand && i.shift == Shift::Rrx)
            i.readsFlags |= FlagC;
    } else if (i.setFlags && (i.op == Op::Mul || i.op == Op::Mla || i.op == Op::Umull || i.op == Op::Umlal ||
                              i.op == Op::Smull || i.op == Op::Smlal)) {
        i.writesFlags = FlagN | FlagZ;
    } else if (i.op == Op::Mrs) {
        i.readsFlags |= FlagAll;
    } else if (i.op == Op::Msr) {
        i.writesFlags = FlagAll; // conservative; mask refinement is not needed for liveness
    }
}

Insn undef(uint32_t w, uint32_t pc) {
    Insn i;
    i.word = w;
    i.pc = pc;
    i.cond = uint8_t(w >> 28) == 15 ? 14 : uint8_t(w >> 28);
    i.op = Op::Undef;
    finishFlags(i);
    return i;
}

} // namespace

uint8_t condFlagsRead(uint8_t cond) {
    switch (cond) {
    case 0:
    case 1:
        return FlagZ;
    case 2:
    case 3:
        return FlagC;
    case 4:
    case 5:
        return FlagN;
    case 6:
    case 7:
        return FlagV;
    case 8:
    case 9:
        return FlagC | FlagZ;
    case 10:
    case 11:
        return FlagN | FlagV;
    case 12:
    case 13:
        return FlagZ | FlagN | FlagV;
    default:
        return 0;
    }
}

bool Insn::endsBlock() const {
    switch (op) {
    case Op::B:
    case Op::Bl:
    case Op::Bx:
    case Op::BlxReg:
    case Op::BlxImm:
    case Op::Svc:
    case Op::Undef:
    case Op::Bkpt:
        return true;
    case Op::Ldr:
    case Op::Ldm:
        return (op == Op::Ldr && rd == 15) || (op == Op::Ldm && (regList & 0x8000));
    case Op::Swp:
    case Op::Swpb:
        return rd == 15;
    default:
        break;
    }
    if (op >= Op::And && op <= Op::Mvn && !isCompare(op))
        return rd == 15;
    if (op == Op::Mul || op == Op::Mla || op == Op::Clz || op == Op::Rev || op == Op::Rev16 ||
        op == Op::Revsh || (op >= Op::Sxtb && op <= Op::Uxtah) || op == Op::MrcTls || op == Op::Mrs)
        return rd == 15;
    return false;
}

Insn decodeArm(uint32_t w, uint32_t pc) {
    Insn i;
    i.word = w;
    i.pc = pc;
    const uint32_t c = w >> 28;

    if (c == 0xF) {
        if ((w & 0xFE000000) == 0xFA000000) { // BLX imm
            int32_t off = int32_t(w << 8) >> 6;
            i.cond = 14;
            i.op = Op::BlxImm;
            i.target = pc + 8 + uint32_t(off) + (((w >> 24) & 1) << 1);
            i.targetThumb = true;
            finishFlags(i);
            return i;
        }
        if ((w & 0xFF30F000) == 0xF510F000 || (w & 0xFF30F010) == 0xF710F000) { // PLD
            i.cond = 14;
            i.op = Op::Pld;
            finishFlags(i);
            return i;
        }
        if (w == 0xF57FF01F) { // CLREX
            i.cond = 14;
            i.op = Op::Clrex;
            finishFlags(i);
            return i;
        }
        return undef(w, pc);
    }
    i.cond = uint8_t(c);
    i.rd = (w >> 12) & 0xF;
    i.rn = (w >> 16) & 0xF;
    i.rm = w & 0xF;
    i.rs = (w >> 8) & 0xF;

    const uint32_t cls = (w >> 25) & 7;
    switch (cls) {
    case 0:
    case 1: {
        if ((w & 0x0FFFFFF0) == 0x012FFF10) {
            i.op = Op::Bx;
            break;
        }
        if ((w & 0x0FFFFFF0) == 0x012FFF30) {
            i.op = Op::BlxReg;
            break;
        }
        if ((w & 0x0FFF0FF0) == 0x016F0F10) {
            i.op = Op::Clz;
            break;
        }
        if ((w & 0x0FF000F0) == 0x01200070) {
            i.op = Op::Bkpt;
            i.imm = ((w >> 4) & 0xFFF0) | (w & 0xF);
            break;
        }
        if ((w & 0x0FBF0FFF) == 0x010F0000) {
            i.op = Op::Mrs;
            if (w & (1 << 22))
                i.op = Op::Undef;
            break;
        }
        if ((w & 0x0FB0FFF0) == 0x0120F000) { // MSR reg
            if (w & (1 << 22)) {
                i.op = Op::Undef;
                break;
            }
            i.op = Op::Msr;
            i.immOperand = false;
            break;
        }
        if ((w & 0x0FB0F000) == 0x0320F000 && ((w >> 16) & 0xF) != 0) { // MSR imm
            if (w & (1 << 22)) {
                i.op = Op::Undef;
                break;
            }
            i.op = Op::Msr;
            i.immOperand = true;
            i.imm = ror32(w & 0xFF, 2 * ((w >> 8) & 0xF));
            break;
        }
        if ((w & 0x0FFFFFF8) == 0x0320F000) {
            i.op = Op::Nop;
            break;
        }                                     // NOP, YIELD, WFE, WFI, SEV hints
        if (cls == 0 && (w & 0x90) == 0x90) { // extension space
            const uint32_t sh = (w >> 5) & 3;
            if (sh == 0) {
                const uint32_t b = (w >> 20) & 0xFF;
                i.setFlags = b & 1;
                switch (b & 0xFE) {
                case 0x00:
                    i.op = Op::Mul;
                    i.rd = i.rn;
                    i.rn = (w >> 12) & 0xF;
                    break;
                case 0x02:
                    i.op = Op::Mla;
                    i.rd = i.rn;
                    i.rn = (w >> 12) & 0xF;
                    break;
                case 0x04:
                    i.op = (b & 1) ? Op::Undef : Op::Umaal;
                    i.setFlags = false;
                    break;
                case 0x08:
                    i.op = Op::Umull;
                    break;
                case 0x0A:
                    i.op = Op::Umlal;
                    break;
                case 0x0C:
                    i.op = Op::Smull;
                    break;
                case 0x0E:
                    i.op = Op::Smlal;
                    break;
                default:
                    i.setFlags = false;
                    switch (b) {
                    case 0x10:
                        i.op = Op::Swp;
                        break;
                    case 0x14:
                        i.op = Op::Swpb;
                        break;
                    case 0x18:
                        i.op = Op::Strex;
                        break;
                    case 0x19:
                        i.op = Op::Ldrex;
                        break;
                    case 0x1A:
                        i.op = Op::Strexd;
                        break;
                    case 0x1B:
                        i.op = Op::Ldrexd;
                        break;
                    case 0x1C:
                        i.op = Op::Strexb;
                        break;
                    case 0x1D:
                        i.op = Op::Ldrexb;
                        break;
                    case 0x1E:
                        i.op = Op::Strexh;
                        break;
                    case 0x1F:
                        i.op = Op::Ldrexh;
                        break;
                    default:
                        i.op = Op::Undef;
                        break;
                    }
                }
                // long multiplies keep the raw fields: rd = RdLo (15:12), rn = RdHi (19:16)
                break;
            }
            // halfword / signed / doubleword transfers
            const bool L = (w >> 20) & 1;
            i.pre = (w >> 24) & 1;
            i.up = (w >> 23) & 1;
            i.wb = (w >> 21) & 1;
            const bool immOff = (w >> 22) & 1;
            if (!i.pre && i.wb) {
                i.op = Op::Undef;
                break;
            }
            if (immOff) {
                i.imm = ((w >> 4) & 0xF0) | (w & 0xF);
                i.regOffset = false;
            } else {
                i.regOffset = true;
                if (w & 0xF00) {
                    i.op = Op::Undef;
                    break;
                }
            }
            switch (sh) {
            case 1:
                i.op = L ? Op::Ldrh : Op::Strh;
                break;
            case 2:
                i.op = L ? Op::Ldrsb : Op::Ldrd;
                break;
            default:
                i.op = L ? Op::Ldrsh : Op::Strd;
                break;
            }
            if ((i.op == Op::Ldrd || i.op == Op::Strd) && (((w >> 12) & 1) || (w >> 12 & 0xF) == 14))
                i.op = Op::Undef;
            break;
        }
        // data processing
        const uint32_t opc = (w >> 21) & 0xF;
        i.setFlags = (w >> 20) & 1;
        if (opc >= 8 && opc <= 11 && !i.setFlags) {
            i.op = Op::Undef;
            break;
        } // misc space not modelled
        i.op = Op(uint8_t(Op::And) + opc);
        if (cls == 1) {
            const unsigned rot = 2 * ((w >> 8) & 0xF);
            i.immOperand = true;
            i.imm = ror32(w & 0xFF, rot);
            i.immCarryDefined = rot != 0;
        } else {
            i.immOperand = false;
            i.shift = Shift(uint8_t((w >> 5) & 3));
            if (w & 0x10) {
                if (w & 0x80) {
                    i.op = Op::Undef;
                    break;
                }
                i.shiftByReg = true;
            } else {
                i.shiftAmt = (w >> 7) & 31;
                if (i.shiftAmt == 0) {
                    if (i.shift == Shift::Lsr || i.shift == Shift::Asr)
                        i.shiftAmt = 32;
                    else if (i.shift == Shift::Ror)
                        i.shift = Shift::Rrx;
                }
            }
        }
        break;
    }
    case 2:
    case 3: {
        if (cls == 3 && (w & 0x10)) { // media instructions
            if ((w & 0x0FFF0FF0) == 0x06BF0F30) {
                i.op = Op::Rev;
                break;
            }
            if ((w & 0x0FFF0FF0) == 0x06BF0FB0) {
                i.op = Op::Rev16;
                break;
            }
            if ((w & 0x0FFF0FF0) == 0x06FF0FB0) {
                i.op = Op::Revsh;
                break;
            }
            if ((w & 0x0F8003F0) == 0x06800070) {
                const uint32_t kind = (w >> 20) & 7; // 010 sxtb, 011 sxth, 110 uxtb, 111 uxth
                const bool add = ((w >> 16) & 0xF) != 15;
                i.rot = uint8_t(((w >> 10) & 3) * 8);
                switch (kind) {
                case 2:
                    i.op = add ? Op::Sxtab : Op::Sxtb;
                    break;
                case 3:
                    i.op = add ? Op::Sxtah : Op::Sxth;
                    break;
                case 6:
                    i.op = add ? Op::Uxtab : Op::Uxtb;
                    break;
                case 7:
                    i.op = add ? Op::Uxtah : Op::Uxth;
                    break;
                default:
                    i.op = Op::Undef;
                    break;
                }
                break;
            }
            i.op = Op::Undef;
            break;
        }
        const bool L = (w >> 20) & 1;
        const bool B = (w >> 22) & 1;
        i.pre = (w >> 24) & 1;
        i.up = (w >> 23) & 1;
        i.wb = (w >> 21) & 1;
        if (cls == 3) {
            i.regOffset = true;
            i.shift = Shift(uint8_t((w >> 5) & 3));
            i.shiftAmt = (w >> 7) & 31;
            if (i.shiftAmt == 0) {
                if (i.shift == Shift::Lsr || i.shift == Shift::Asr)
                    i.shiftAmt = 32;
                else if (i.shift == Shift::Ror)
                    i.shift = Shift::Rrx;
            }
        } else {
            i.imm = w & 0xFFF;
        }
        if (!i.pre && i.wb)
            i.wb = true; // LDRT/STRT: treated as user-mode access (same as normal here)
        i.op = L ? (B ? Op::Ldrb : Op::Ldr) : (B ? Op::Strb : Op::Str);
        break;
    }
    case 4: {
        if ((w >> 22) & 1) {
            i.op = Op::Undef;
            break;
        } // user-bank / SPSR forms are privileged
        i.pre = (w >> 24) & 1;
        i.up = (w >> 23) & 1;
        i.wb = (w >> 21) & 1;
        i.regList = uint16_t(w & 0xFFFF);
        i.op = ((w >> 20) & 1) ? Op::Ldm : Op::Stm;
        if (i.regList == 0)
            i.op = Op::Undef;
        break;
    }
    case 5: {
        const int32_t off = int32_t(w << 8) >> 6;
        i.op = ((w >> 24) & 1) ? Op::Bl : Op::B;
        i.target = pc + 8 + uint32_t(off);
        break;
    }
    case 7: {
        if (w & (1u << 24)) {
            i.op = Op::Svc;
            i.imm = w & 0xFFFFFF;
            break;
        }
        if ((w & 0x0FFF0FFF) == 0x0E1D0F70) {
            i.op = Op::MrcTls;
            break;
        } // mrc p15,0,Rd,c13,c0,3
        // CP15 barriers usable from user code: c7,c10,4 (DSB), c7,c10,5 (DMB), c7,c5,4 (flush prefetch)
        if ((w & 0x0FFF0FFF) == 0x0E070F9A || (w & 0x0FFF0FFF) == 0x0E070FBA ||
            (w & 0x0FFF0FFF) == 0x0E070F95) {
            i.op = Op::Barrier;
            break;
        }
        i.op = Op::Undef;
        break;
    }
    default:
        i.op = Op::Undef;
        break;
    }

    // S-bit with Rd == PC is the privileged exception-return form; not executable from user code.
    if (i.op >= Op::And && i.op <= Op::Mvn && i.setFlags && !isCompare(i.op) && i.rd == 15)
        i.op = Op::Undef;
    finishFlags(i);
    return i;
}

const char *opName(Op op) {
    static const char *names[] = {
        "undef",  "and",   "eor",   "sub",   "rsb",     "add",     "adc",    "sbc",    "rsc",
        "tst",    "teq",   "cmp",   "cmn",   "orr",     "mov",     "bic",    "mvn",    "mul",
        "mla",    "umull", "umlal", "smull", "smlal",   "umaal",   "ldr",    "str",    "ldrb",
        "strb",   "ldrh",  "strh",  "ldrsb", "ldrsh",   "ldrd",    "strd",   "ldm",    "stm",
        "swp",    "swpb",  "ldrex", "strex", "ldrexb",  "strexb",  "ldrexh", "strexh", "ldrexd",
        "strexd", "clrex", "b",     "bl",    "bx",      "blx",     "blx",    "clz",    "rev",
        "rev16",  "revsh", "sxtb",  "sxth",  "uxtb",    "uxth",    "sxtab",  "sxtah",  "uxtab",
        "uxtah",  "svc",   "nop",   "pld",   "barrier", "mrc-tls", "mrs",    "msr",    "bkpt"};
    return names[uint8_t(op)];
}

std::string disasm(const Insn &i) {
    static const char *cc[] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                               "hi", "ls", "ge", "lt", "gt", "le", "",   "nv"};
    char b[160];
    std::snprintf(b, sizeof b, "%08x: %s%s%s rd=r%u rn=r%u rm=r%u rs=r%u imm=%#x", i.pc, opName(i.op),
                  cc[i.cond], i.setFlags ? "s" : "", i.rd, i.rn, i.rm, i.rs, i.imm);
    return b;
}

} // namespace recomp
