#pragma once
#include <cstdint>
#include <string>

namespace recomp {

enum class Op : uint8_t {
    Undef,
    // data processing (order matches the ARM opcode field)
    And,
    Eor,
    Sub,
    Rsb,
    Add,
    Adc,
    Sbc,
    Rsc,
    Tst,
    Teq,
    Cmp,
    Cmn,
    Orr,
    Mov,
    Bic,
    Mvn,
    Mul,
    Mla,
    Umull,
    Umlal,
    Smull,
    Smlal,
    Umaal,
    Ldr,
    Str,
    Ldrb,
    Strb,
    Ldrh,
    Strh,
    Ldrsb,
    Ldrsh,
    Ldrd,
    Strd,
    Ldm,
    Stm,
    Swp,
    Swpb,
    Ldrex,
    Strex,
    Ldrexb,
    Strexb,
    Ldrexh,
    Strexh,
    Ldrexd,
    Strexd,
    Clrex,
    B,
    Bl,
    Bx,
    BlxReg,
    BlxImm,
    Clz,
    Rev,
    Rev16,
    Revsh,
    Sxtb,
    Sxth,
    Uxtb,
    Uxth,
    Sxtab,
    Sxtah,
    Uxtab,
    Uxtah,
    Svc,
    Nop,
    Pld,
    Barrier,
    MrcTls, // mrc p15,0,Rd,c13,c0,3
    Mrs,
    Msr, // only the user-visible APSR flag view is modelled
    Bkpt,
    // ARMv6 media and ARMv5E DSP
    Par,
    Sel,
    Pkh,
    Ssat,
    Usat,
    Ssat16,
    Usat16,
    Sxtb16,
    Uxtb16,
    Sxtab16,
    Uxtab16,
    Usad8,
    Usada8,
    Smlad,
    Smlsd,
    Smlald,
    Smlsld,
    Smmla,
    Smmls,
    Qadd,
    Qsub,
    Qdadd,
    Qdsub,
    Vfp,       // data processing: sub = RcVfpOp, dbl, vd/vn/vm
    VfpMovRS,  // VMOV Sn<->Rt (fx: to core)
    VfpMovRR,  // VMOV Rt,Rt2 <-> two S / one D (fx: to core)
    VfpMrs,    // VMRS Rt, <imm>
    VfpMsr,    // VMSR <imm>, Rt
    VfpLdSt,   // VLDR / VSTR (fx: load)
    VfpLdmStm, // VLDM / VSTM / VPUSH / VPOP (fx: load)
    Smla,
    Smlaw,
    Smlal16, // SMUL/SMLA xy, SMULW/SMLAW y, SMLAL xy (acc flag distinguishes the SMUL forms)
};

enum class Shift : uint8_t { Lsl, Lsr, Asr, Ror, Rrx };

// Flag bit masks used by liveness analysis.
enum : uint8_t { FlagV = 1, FlagC = 2, FlagZ = 4, FlagN = 8, FlagAll = 15 };

struct Insn {
    uint32_t word = 0;
    uint32_t pc = 0; // address of this instruction
    bool thumb = false;
    uint8_t size = 4; // bytes: 4 for ARM and Thumb BL/BLX pairs, 2 for other Thumb
    Op op = Op::Undef;
    uint8_t cond = 14; // 0..14 (15 never produced for executable instructions)
    bool setFlags = false;
    uint8_t rd = 0, rn = 0, rm = 0, rs = 0; // raw register fields (see decode.cpp for per-op meaning)
    // operand 2 (data processing)
    bool immOperand = false;
    uint32_t imm = 0;             // modified immediate value, or load/store offset, or svc number
    bool immCarryDefined = false; // modified immediate with rotate != 0: shifter carry = imm bit 31
    Shift shift = Shift::Lsl;
    bool shiftByReg = false;
    uint8_t shiftAmt = 0; // immediate shift amount (already normalised: LSR/ASR #0 -> 32, ROR #0 -> RRX)
    // load/store
    bool pre = false, up = false, wb = false, regOffset = false;
    uint16_t regList = 0;
    uint8_t rot = 0;                // extend rotation (0,8,16,24)
    uint8_t sub = 0, sub2 = 0;      // Par: kind (S,Q,SH,U,UQ,UH) / op (ADD16,ASX,SAX,SUB16,ADD8,SUB8)
    uint8_t vd = 0, vn = 0, vm = 0; // VFP register numbers (S or D according to dbl)
    bool dbl = false;
    bool fx = false, fy = false, acc = false; // media flags: exchange/x-half/round, y-half, accumulate
    // control flow
    uint32_t target = 0; // B/BL/BLX imm
    bool targetThumb = false;
    // liveness
    uint8_t readsFlags = 0;                                  // flags read (including by the condition)
    uint8_t writesFlags = 0;                                 // flags written when executed
    uint32_t pcRead() const { return pc + (thumb ? 4 : 8); } // value read for r15
    uint32_t next() const { return pc + size; }
    uint32_t link() const { return next() | (thumb ? 1u : 0u); } // value written to LR by BL/BLX
    // true when the instruction can change control flow (ends a block)
    bool endsBlock() const;
};

// Decodes one A32 word at `pc`. Unsupported/undefined encodings produce Op::Undef.
Insn decodeArm(uint32_t word, uint32_t pc);
// Decodes one Thumb instruction at pc. hw2 is the following halfword (used only by the 32-bit BL/BLX pair);
// the result's size is 2 or 4.
Insn decodeThumb(uint16_t hw, uint16_t hw2, uint32_t pc);

const char *opName(Op op);
std::string disasm(const Insn &i);
uint8_t condFlagsRead(uint8_t cond);

} // namespace recomp
