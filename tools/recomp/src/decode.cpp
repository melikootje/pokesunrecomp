#include "recomp/decode.h"

#include "pokerecomp/vfp.h"

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
    } else if (i.op == Op::VfpMrs && i.rd == 15) {
        i.writesFlags = FlagAll;
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

// Decodes VFPv2 (coprocessors 10/11). Returns false for anything that is not a modelled VFP encoding.
bool decodeVfp(Insn &i, uint32_t w) {
    const uint32_t cp = (w >> 8) & 0xF;
    if (cp != 10 && cp != 11)
        return false;
    const bool dbl = cp == 11;
    const uint32_t D = (w >> 22) & 1, N = (w >> 7) & 1, M = (w >> 5) & 1;
    const uint32_t Vd = (w >> 12) & 0xF, Vn = (w >> 16) & 0xF, Vm = w & 0xF;
    auto sreg = [](uint32_t v, uint32_t b) { return uint8_t((v << 1) | b); };
    i.dbl = dbl;
    const uint32_t cls = (w >> 25) & 7;
    if (cls == 6) {
        if ((w & 0x0FE00000) == 0x0C400000) { // VMOV Rt,Rt2 <-> two S / one D
            if ((w & 0xD0) != 0x10)
                return false;
            i.op = Op::VfpMovRR;
            i.fx = (w >> 20) & 1;
            i.rd = (w >> 12) & 0xF;
            i.rn = (w >> 16) & 0xF;
            if (dbl) {
                if (M)
                    return false;
                i.vm = uint8_t(Vm);
            } else {
                i.vm = sreg(Vm, M);
                if (i.vm == 31)
                    return false;
            }
            return true;
        }
        if ((w & 0x0F200000) == 0x0D000000) { // VLDR / VSTR
            i.op = Op::VfpLdSt;
            i.fx = (w >> 20) & 1;
            i.up = (w >> 23) & 1;
            i.rn = (w >> 16) & 0xF;
            i.imm = (w & 0xFF) * 4;
            if (dbl) {
                if (D)
                    return false;
                i.vd = uint8_t(Vd);
            } else {
                i.vd = sreg(Vd, D);
            }
            return true;
        }
        const uint32_t P = (w >> 24) & 1, U = (w >> 23) & 1, W = (w >> 21) & 1;
        const bool ia = !P && U, db = P && !U && W;
        if (!ia && !db)
            return false;
        i.op = Op::VfpLdmStm;
        i.fx = (w >> 20) & 1;
        i.pre = P;
        i.up = U;
        i.wb = W;
        i.rn = (w >> 16) & 0xF;
        i.imm = w & 0xFF; // words
        if (i.imm == 0)
            return false;
        if (dbl) {
            if (D || (i.imm & 1) || Vd + i.imm / 2 > 16)
                return false; // FLDMX/FSTMX (odd count) not modelled
            i.vd = uint8_t(Vd);
        } else {
            i.vd = sreg(Vd, D);
            if (i.vd + i.imm > 32)
                return false;
        }
        return true;
    }
    if (cls != 7 || (w & (1u << 24)))
        return false;
    if ((w & 0x10) == 0) { // data processing
        const uint32_t pqrs =
            (((w >> 23) & 1) << 3) | (((w >> 21) & 1) << 2) | (((w >> 20) & 1) << 1) | ((w >> 6) & 1);
        auto regs = [&]() {
            if (dbl) {
                // in the extension space (pqrs == 15) bit 7 is part of the opcode, not a register bit
                if (D || (N && pqrs != 15) || M)
                    return false;
                i.vd = uint8_t(Vd);
                i.vn = uint8_t(Vn);
                i.vm = uint8_t(Vm);
            } else {
                i.vd = sreg(Vd, D);
                i.vn = sreg(Vn, N);
                i.vm = sreg(Vm, M);
            }
            return true;
        };
        i.op = Op::Vfp;
        if (pqrs != 15) {
            static const uint8_t map[9] = {RC_V_MLA,  RC_V_MLS, RC_V_NMLS, RC_V_NMLA, RC_V_MUL,
                                           RC_V_NMUL, RC_V_ADD, RC_V_SUB,  RC_V_DIV};
            // pqrs: 0000 MLA, 0001 MLS, 0010 NMLS, 0011 NMLA, 0100 MUL, 0101 NMUL, 0110 ADD, 0111 SUB, 1000
            // DIV
            if (pqrs > 8)
                return false;
            i.sub = map[pqrs];
            return regs();
        }
        const uint32_t opc2 = Vn, opc3 = (w >> 6) & 3;
        switch (opc2) {
        case 0:
            i.sub = opc3 == 1 ? RC_V_MOV : opc3 == 3 ? RC_V_ABS : 99;
            break;
        case 1:
            i.sub = opc3 == 1 ? RC_V_NEG : opc3 == 3 ? RC_V_SQRT : 99;
            break;
        case 4:
            i.sub = opc3 == 1 ? RC_V_CMP : opc3 == 3 ? RC_V_CMPE : 99;
            break;
        case 5:
            if (Vm != 0 || M)
                return false;
            i.sub = opc3 == 1 ? RC_V_CMPZ : opc3 == 3 ? RC_V_CMPEZ : 99;
            break;
        case 7:
            if (opc3 != 3)
                return false;
            i.sub = RC_V_CVT_DS;
            if (dbl) { // double -> single
                if (M)
                    return false;
                i.vd = sreg(Vd, D);
                i.vm = uint8_t(Vm);
            } else { // single -> double
                if (D)
                    return false;
                i.vd = uint8_t(Vd);
                i.vm = sreg(Vm, M);
            }
            return true;
        case 8:
            if (!(opc3 & 1))
                return false;
            i.sub = (opc3 & 2) ? RC_V_CVT_S2F : RC_V_CVT_U2F;
            if (dbl) {
                if (D)
                    return false;
                i.vd = uint8_t(Vd);
            } else {
                i.vd = sreg(Vd, D);
            }
            i.vm = sreg(Vm, M);
            return true;
        case 12:
        case 13: {
            if (!(opc3 & 1))
                return false;
            const bool rz = opc3 & 2;
            i.sub =
                opc2 == 12 ? (rz ? RC_V_CVT_F2U_RZ : RC_V_CVT_F2U) : (rz ? RC_V_CVT_F2S_RZ : RC_V_CVT_F2S);
            i.vd = sreg(Vd, D);
            if (dbl) {
                if (M)
                    return false;
                i.vm = uint8_t(Vm);
            } else {
                i.vm = sreg(Vm, M);
            }
            return true;
        }
        default:
            return false;
        }
        if (i.sub == 99)
            return false;
        if (opc2 == 5) { // compare with zero: only Fd is used
            if (dbl) {
                if (D)
                    return false;
                i.vd = uint8_t(Vd);
            } else
                i.vd = sreg(Vd, D);
            return true;
        }
        if (opc2 == 4) { // compare: Fd vs Fm
            return regs();
        }
        return regs();
    }
    // register transfers (bit 4 set)
    if ((w & 0x0FE00F7F) == 0x0E000A10 && !dbl) { // VMOV Sn <-> Rt
        i.op = Op::VfpMovRS;
        i.fx = (w >> 20) & 1;
        i.rd = (w >> 12) & 0xF;
        i.vn = sreg(Vn, N);
        return true;
    }
    if ((w & 0x0FF00FFF) == 0x0EF00A10) {
        i.op = Op::VfpMrs;
        i.rd = (w >> 12) & 0xF;
        i.imm = Vn;
        return true;
    }
    if ((w & 0x0FF00FFF) == 0x0EE00A10) {
        i.op = Op::VfpMsr;
        i.rd = (w >> 12) & 0xF;
        i.imm = Vn;
        return true;
    }
    return false;
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
    if ((op >= Op::Par && op <= Op::Smlal16) || op == Op::Mul || op == Op::Mla || op == Op::Clz ||
        op == Op::Rev || op == Op::Rev16 || op == Op::Revsh || (op >= Op::Sxtb && op <= Op::Uxtah) ||
        op == Op::MrcTls || op == Op::Mrs)
        return rd == 15;
    return false;
}

// ---- Thumb (ARMv6K Thumb-1 + BL/BLX pair + ARMv6 extend/reverse) mapped onto the common Insn model ----
Insn decodeThumb(uint16_t h, uint16_t h2, uint32_t pc) {
    Insn i;
    i.thumb = true;
    i.size = 2;
    i.word = h;
    i.pc = pc;
    i.cond = 14;
    i.up = true;
    const unsigned rd3 = h & 7, rm3 = (h >> 3) & 7, rn3 = (h >> 6) & 7;
    // Loads/stores use a pre-indexed immediate/register offset without writeback.
    auto xfer = [&](Op op, unsigned rt, unsigned rn, uint32_t immOff) {
        i.op = op;
        i.rd = uint8_t(rt);
        i.rn = uint8_t(rn);
        i.pre = true;
        i.up = true;
        i.wb = false;
        i.imm = immOff;
    };
    auto dp = [&](Op op, unsigned rd, unsigned rn, bool s) {
        i.op = op;
        i.rd = uint8_t(rd);
        i.rn = uint8_t(rn);
        i.setFlags = s;
        i.immOperand = false;
        i.shift = Shift::Lsl;
        i.shiftAmt = 0;
    };
    auto dpImm = [&](Op op, unsigned rd, unsigned rn, uint32_t imm, bool s) {
        dp(op, rd, rn, s);
        i.immOperand = true;
        i.imm = imm;
        i.immCarryDefined = false;
    };
    switch (h >> 11) {
    case 0:   // LSL imm
    case 1:   // LSR imm
    case 2: { // ASR imm
        dp(Op::Mov, rd3, 0, true);
        i.rm = uint8_t(rm3);
        const unsigned amt = (h >> 6) & 31;
        i.shift = (h >> 11) == 0 ? Shift::Lsl : (h >> 11) == 1 ? Shift::Lsr : Shift::Asr;
        i.shiftAmt = uint8_t(amt == 0 && i.shift != Shift::Lsl ? 32 : amt);
        break;
    }
    case 3: { // ADD/SUB reg or imm3
        const bool sub = (h >> 9) & 1, imm = (h >> 10) & 1;
        if (imm)
            dpImm(sub ? Op::Sub : Op::Add, rd3, rm3, rn3, true);
        else {
            dp(sub ? Op::Sub : Op::Add, rd3, rm3, true);
            i.rm = uint8_t(rn3);
        }
        break;
    }
    case 4:
        dpImm(Op::Mov, (h >> 8) & 7, 0, h & 0xFF, true);
        break;
    case 5:
        dpImm(Op::Cmp, (h >> 8) & 7, (h >> 8) & 7, h & 0xFF, true);
        break;
    case 6:
        dpImm(Op::Add, (h >> 8) & 7, (h >> 8) & 7, h & 0xFF, true);
        break;
    case 7:
        dpImm(Op::Sub, (h >> 8) & 7, (h >> 8) & 7, h & 0xFF, true);
        break;
    case 8: {
        if ((h & 0xFC00) == 0x4000) { // ALU operations
            const unsigned op = (h >> 6) & 15;
            switch (op) {
            case 0:
                dp(Op::And, rd3, rd3, true);
                i.rm = uint8_t(rm3);
                break;
            case 1:
                dp(Op::Eor, rd3, rd3, true);
                i.rm = uint8_t(rm3);
                break;
            case 2:
            case 3:
            case 4:
            case 7: { // shifts by register: MOVS Rd, Rd, <shift> Rs
                dp(Op::Mov, rd3, 0, true);
                i.rm = uint8_t(rd3);
                i.rs = uint8_t(rm3);
                i.shiftByReg = true;
                i.shift = op == 2 ? Shift::Lsl : op == 3 ? Shift::Lsr : op == 4 ? Shift::Asr : Shift::Ror;
                break;
            }
            case 5:
                dp(Op::Adc, rd3, rd3, true);
                i.rm = uint8_t(rm3);
                break;
            case 6:
                dp(Op::Sbc, rd3, rd3, true);
                i.rm = uint8_t(rm3);
                break;
            case 8:
                dp(Op::Tst, 0, rd3, true);
                i.rm = uint8_t(rm3);
                break;
            case 9:
                dpImm(Op::Rsb, rd3, rm3, 0, true);
                break; // NEG
            case 10:
                dp(Op::Cmp, 0, rd3, true);
                i.rm = uint8_t(rm3);
                break;
            case 11:
                dp(Op::Cmn, 0, rd3, true);
                i.rm = uint8_t(rm3);
                break;
            case 12:
                dp(Op::Orr, rd3, rd3, true);
                i.rm = uint8_t(rm3);
                break;
            case 13: // MULS Rd, Rm, Rd
                i.op = Op::Mul;
                i.rd = uint8_t(rd3);
                i.rm = uint8_t(rm3);
                i.rs = uint8_t(rd3);
                i.setFlags = true;
                break;
            case 14:
                dp(Op::Bic, rd3, rd3, true);
                i.rm = uint8_t(rm3);
                break;
            default:
                dp(Op::Mvn, rd3, 0, true);
                i.rm = uint8_t(rm3);
                break;
            }
            break;
        }
        // 0100 01xx: high register operations / BX / BLX
        const unsigned rd = rd3 | ((h >> 4) & 8), rm = (h >> 3) & 15;
        switch ((h >> 8) & 3) {
        case 0:
            dp(Op::Add, rd, rd, false);
            i.rm = uint8_t(rm);
            break;
        case 1:
            dp(Op::Cmp, 0, rd, true);
            i.rm = uint8_t(rm);
            break;
        case 2:
            dp(Op::Mov, rd, 0, false);
            i.rm = uint8_t(rm);
            break;
        default:
            if (h & 7) {
                i.op = Op::Undef;
                break;
            }
            if (rm == 15) {
                i.op = Op::Undef;
                break;
            }
            i.op = (h & 0x80) ? Op::BlxReg : Op::Bx;
            i.rm = uint8_t(rm);
            break;
        }
        break;
    }
    case 9: { // LDR literal: address = Align(pc + 4, 4) + imm8 * 4, expressed relative to the raw pc + 4 read
        const uint32_t adj = ((pc + 4) & ~3u) - (pc + 4);
        xfer(Op::Ldr, (h >> 8) & 7, 15, (h & 0xFF) * 4 + adj);
        break;
    }
    case 10:
    case 11: { // load/store register offset
        static const Op ops[8] = {Op::Str, Op::Strh, Op::Strb, Op::Ldrsb,
                                  Op::Ldr, Op::Ldrh, Op::Ldrb, Op::Ldrsh};
        xfer(ops[(h >> 9) & 7], rd3, rm3, 0);
        i.regOffset = true;
        i.rm = uint8_t(rn3);
        i.shift = Shift::Lsl;
        i.shiftAmt = 0;
        break;
    }
    case 12:
        xfer(Op::Str, rd3, rm3, ((h >> 6) & 31) * 4);
        break;
    case 13:
        xfer(Op::Ldr, rd3, rm3, ((h >> 6) & 31) * 4);
        break;
    case 14:
        xfer(Op::Strb, rd3, rm3, (h >> 6) & 31);
        break;
    case 15:
        xfer(Op::Ldrb, rd3, rm3, (h >> 6) & 31);
        break;
    case 16:
        xfer(Op::Strh, rd3, rm3, ((h >> 6) & 31) * 2);
        break;
    case 17:
        xfer(Op::Ldrh, rd3, rm3, ((h >> 6) & 31) * 2);
        break;
    case 18:
        xfer(Op::Str, (h >> 8) & 7, 13, (h & 0xFF) * 4);
        break;
    case 19:
        xfer(Op::Ldr, (h >> 8) & 7, 13, (h & 0xFF) * 4);
        break;
    case 20: { // ADD Rd, PC, #imm8*4 (ADR)
        const uint32_t adj = ((pc + 4) & ~3u) - (pc + 4);
        dpImm(Op::Add, (h >> 8) & 7, 15, (h & 0xFF) * 4 + adj, false);
        break;
    }
    case 21:
        dpImm(Op::Add, (h >> 8) & 7, 13, (h & 0xFF) * 4, false);
        break;
    case 22:
    case 23: {
        if ((h & 0xFF00) == 0xB000) { // ADD/SUB SP, #imm7*4
            dpImm((h & 0x80) ? Op::Sub : Op::Add, 13, 13, (h & 0x7F) * 4, false);
        } else if ((h & 0xFE00) == 0xB400 || (h & 0xFE00) == 0xBC00) { // PUSH / POP
            const bool pop = (h & 0x0800) != 0;
            i.regList = uint16_t(h & 0xFF);
            if (h & 0x100)
                i.regList |= pop ? 0x8000 : 0x4000;
            if (i.regList == 0) {
                i.op = Op::Undef;
                break;
            }
            i.op = pop ? Op::Ldm : Op::Stm;
            i.rn = 13;
            i.pre = !pop;
            i.up = pop;
            i.wb = true;
        } else if ((h & 0xFF00) == 0xB200) { // SXTH / SXTB / UXTH / UXTB
            static const Op ops[4] = {Op::Sxth, Op::Sxtb, Op::Uxth, Op::Uxtb};
            i.op = ops[(h >> 6) & 3];
            i.rd = uint8_t(rd3);
            i.rm = uint8_t(rm3);
            i.rot = 0;
        } else if ((h & 0xFF00) == 0xBA00 && ((h >> 6) & 3) != 2) { // REV / REV16 / REVSH
            const unsigned k = (h >> 6) & 3;
            i.op = k == 0 ? Op::Rev : k == 1 ? Op::Rev16 : Op::Revsh;
            i.rd = uint8_t(rd3);
            i.rm = uint8_t(rm3);
        } else {
            i.op = Op::Undef;
        }
        break;
    }
    case 24:
    case 25: { // STMIA / LDMIA Rn!, {list}
        const bool load = (h >> 11) & 1;
        i.regList = uint16_t(h & 0xFF);
        if (i.regList == 0) {
            i.op = Op::Undef;
            break;
        }
        i.op = load ? Op::Ldm : Op::Stm;
        i.rn = uint8_t((h >> 8) & 7);
        i.pre = false;
        i.up = true;
        i.wb = !(load && (i.regList & (1u << i.rn)));
        break;
    }
    case 26:
    case 27: {
        const unsigned cond = (h >> 8) & 15;
        if (cond == 15) {
            i.op = Op::Svc;
            i.imm = h & 0xFF;
        } else if (cond == 14) {
            i.op = Op::Undef;
        } else {
            i.op = Op::B;
            i.cond = uint8_t(cond);
            i.target = pc + 4 + uint32_t(int32_t(int8_t(h & 0xFF))) * 2;
        }
        break;
    }
    case 28: // B
        i.op = Op::B;
        i.target = pc + 4 + uint32_t(int32_t(int16_t((h & 0x7FF) << 5) >> 4));
        break;
    case 30: { // BL / BLX prefix + suffix
        const bool bl = (h2 >> 11) == 0x1F, blx = (h2 >> 11) == 0x1D && !(h2 & 1);
        if (!bl && !blx) {
            i.op = Op::Undef;
            break;
        }
        const int32_t hi = int32_t(int16_t((h & 0x7FF) << 5) >> 5); // sign-extended imm11
        const uint32_t off = uint32_t(hi) * 4096u + (uint32_t(h2 & 0x7FF) << 1);
        i.size = 4;
        i.word = (uint32_t(h) << 16) | h2;
        if (bl) {
            i.op = Op::Bl;
            i.target = pc + 4 + off;
            i.targetThumb = true;
        } else {
            i.op = Op::BlxImm;
            i.target = (pc + 4 + off) & ~3u;
            i.targetThumb = false;
        }
        break;
    }
    default:
        i.op = Op::Undef;
        break; // 11101 (BLX suffix alone) / 11111 (BL suffix alone)
    }
    finishFlags(i);
    return i;
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
        } // NOP, YIELD, WFE, WFI, SEV hints
        if ((w & 0x0F900FF0) == 0x01000050) { // QADD / QSUB / QDADD / QDSUB (Rd, Rm, Rn)
            i.op = Op(uint8_t(Op::Qadd) + ((w >> 21) & 3));
            break;
        }
        if ((w & 0x0F900090) == 0x01000080) { // SMLAxy / SMLAWy / SMULWy / SMLALxy / SMULxy
            const uint32_t k = (w >> 21) & 3;
            i.fx = (w & 0x20) != 0;
            i.fy = (w & 0x40) != 0;
            switch (k) {
            case 0:
                i.op = Op::Smla;
                i.acc = true;
                break;
            case 1:
                i.op = Op::Smlaw;
                i.acc = !(w & 0x20);
                i.fx = false;
                break;
            case 2:
                i.op = Op::Smlal16;
                i.acc = true;
                break;
            default:
                i.op = Op::Smla;
                i.acc = false;
                break;
            }
            if (k != 2) {
                i.rd = i.rn;
                i.rn = (w >> 12) & 0xF;
            } // rd = Rd(19:16), rn = Ra
            break;
        }
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
            const uint32_t top = (w >> 20) & 0xFF;
            const uint32_t op2 = (w >> 5) & 7;
            if ((top & 0xF8) == 0x60 && (top & 7) != 0 && (top & 7) != 4 && ((w >> 8) & 0xF) == 0xF) {
                static const uint8_t kind[8] = {9, 0, 1, 2, 9, 3, 4, 5};
                static const uint8_t pop[8] = {0, 1, 2, 3, 4, 9, 9, 5};
                if (kind[top & 7] == 9 || pop[op2] == 9) {
                    i.op = Op::Undef;
                    break;
                }
                i.op = Op::Par;
                i.sub = kind[top & 7];
                i.sub2 = pop[op2];
                break;
            }
            if ((w & 0x0FF00FF0) == 0x06800FB0) {
                i.op = Op::Sel;
                break;
            }
            if ((w & 0x0FF00030) == 0x06800010 && (w & 0x40) == 0) {
                i.op = Op::Pkh;
                i.fx = false;
                i.shiftAmt = (w >> 7) & 31;
                break;
            }
            if ((w & 0x0FF00070) == 0x06800050) { // PKHTB: ASR, #0 encodes 32
                i.op = Op::Pkh;
                i.fx = true;
                i.shiftAmt = (w >> 7) & 31;
                if (!i.shiftAmt)
                    i.shiftAmt = 32;
                break;
            }
            if ((w & 0x0FA00030) == 0x06A00010) { // SSAT / USAT
                i.op = (w & (1u << 22)) ? Op::Usat : Op::Ssat;
                i.imm = (w >> 16) & 31; // sat_imm (SSAT adds 1 below)
                i.shiftAmt = (w >> 7) & 31;
                i.shift = (w & 0x40) ? Shift::Asr : Shift::Lsl;
                if (i.shift == Shift::Asr && !i.shiftAmt)
                    i.shiftAmt = 32;
                if (i.op == Op::Ssat)
                    i.imm += 1;
                break;
            }
            if ((w & 0x0FB00FF0) == 0x06A00F30) {
                i.op = (w & (1u << 22)) ? Op::Usat16 : Op::Ssat16;
                i.imm = (w >> 16) & 15;
                if (i.op == Op::Ssat16)
                    i.imm += 1;
                break;
            }
            if ((w & 0x0FF000F0) == 0x07800010) {
                i.op = ((w >> 12) & 0xF) == 15 ? Op::Usad8 : Op::Usada8;
                i.rd = i.rn;
                i.rn = (w >> 12) & 0xF; // rd = Rd(19:16), rn = Ra, rs = Rm(11:8), rm = Rn(3:0)
                break;
            }
            if ((w & 0x0FF000D0) == 0x07000010 ||
                (w & 0x0FF000D0) == 0x07000050) { // SMLAD / SMUAD / SMLSD / SMUSD
                const bool sub_ = (w & 0x40) != 0;
                i.fx = (w & 0x20) != 0;
                i.acc = ((w >> 12) & 0xF) != 15;
                i.op = sub_ ? Op::Smlsd : Op::Smlad;
                i.rd = i.rn;
                i.rn = (w >> 12) & 0xF;
                break;
            }
            if ((w & 0x0FF000D0) == 0x07400010 || (w & 0x0FF000D0) == 0x07400050) { // SMLALD / SMLSLD
                i.op = (w & 0x40) ? Op::Smlsld : Op::Smlald;
                i.fx = (w & 0x20) != 0;
                break; // rd = RdLo (15:12), rn = RdHi (19:16)
            }
            if ((w & 0x0FF000D0) == 0x07500010 || (w & 0x0FF000D0) == 0x075000D0) { // SMMLA/SMMUL, SMMLS
                i.op = (w & 0x80) ? Op::Smmls : Op::Smmla;
                i.fx = (w & 0x20) != 0; // rounding
                i.acc = ((w >> 12) & 0xF) != 15 || i.op == Op::Smmls;
                i.rd = i.rn;
                i.rn = (w >> 12) & 0xF;
                break;
            }
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
                const uint32_t kind =
                    (w >> 20) & 7; // 000 sxtb16, 010 sxtb, 011 sxth, 100 uxtb16, 110 uxtb, 111 uxth
                const bool add = ((w >> 16) & 0xF) != 15;
                i.rot = uint8_t(((w >> 10) & 3) * 8);
                switch (kind) {
                case 0:
                    i.op = add ? Op::Sxtab16 : Op::Sxtb16;
                    break;
                case 2:
                    i.op = add ? Op::Sxtab : Op::Sxtb;
                    break;
                case 3:
                    i.op = add ? Op::Sxtah : Op::Sxth;
                    break;
                case 4:
                    i.op = add ? Op::Uxtab16 : Op::Uxtb16;
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
    case 6:
        if (!decodeVfp(i, w))
            i.op = Op::Undef;
        break;
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
        if (decodeVfp(i, w))
            break;
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
        "undef",   "and",     "eor",       "sub",    "rsb",     "add",     "adc",      "sbc",      "rsc",
        "tst",     "teq",     "cmp",       "cmn",    "orr",     "mov",     "bic",      "mvn",      "mul",
        "mla",     "umull",   "umlal",     "smull",  "smlal",   "umaal",   "ldr",      "str",      "ldrb",
        "strb",    "ldrh",    "strh",      "ldrsb",  "ldrsh",   "ldrd",    "strd",     "ldm",      "stm",
        "swp",     "swpb",    "ldrex",     "strex",  "ldrexb",  "strexb",  "ldrexh",   "strexh",   "ldrexd",
        "strexd",  "clrex",   "b",         "bl",     "bx",      "blx",     "blx",      "clz",      "rev",
        "rev16",   "revsh",   "sxtb",      "sxth",   "uxtb",    "uxth",    "sxtab",    "sxtah",    "uxtab",
        "uxtah",   "svc",     "nop",       "pld",    "barrier", "mrc-tls", "mrs",      "msr",      "bkpt",
        "par",     "sel",     "pkh",       "ssat",   "usat",    "ssat16",  "usat16",   "sxtb16",   "uxtb16",
        "sxtab16", "uxtab16", "usad8",     "usada8", "smlad",   "smlsd",   "smlald",   "smlsld",   "smmla",
        "smmls",   "qadd",    "qsub",      "qdadd",  "qdsub",   "vfp",     "vfpmovrs", "vfpmovrr", "vfpmrs",
        "vfpmsr",  "vfpldst", "vfpldmstm", "smla",   "smlaw",   "smlal16"};
    return names[uint8_t(op)];
}

std::string disasm(const Insn &i) {
    static const char *cc[] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                               "hi", "ls", "ge", "lt", "gt", "le", "",   "nv"};
    char b[160];
    std::snprintf(b, sizeof b, "%08x: [%08x] %s%s%s rd=r%u rn=r%u rm=r%u rs=r%u imm=%#x", i.pc, i.word,
                  opName(i.op), cc[i.cond], i.setFlags ? "s" : "", i.rd, i.rn, i.rm, i.rs, i.imm);
    return b;
}

} // namespace recomp
