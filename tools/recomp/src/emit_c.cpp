#include "recomp/emit_c.h"

#include <cstdarg>
#include <cstdio>

namespace recomp {
namespace {

std::string fmt(const char *f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string hex(uint32_t v) { return fmt("0x%08xu", v); }

std::string condExpr(unsigned c) {
    switch (c) {
    case 0:
        return "fz";
    case 1:
        return "!fz";
    case 2:
        return "fc";
    case 3:
        return "!fc";
    case 4:
        return "fn";
    case 5:
        return "!fn";
    case 6:
        return "fv";
    case 7:
        return "!fv";
    case 8:
        return "(fc && !fz)";
    case 9:
        return "(!fc || fz)";
    case 10:
        return "fn == fv";
    case 11:
        return "fn != fv";
    case 12:
        return "(!fz && fn == fv)";
    case 13:
        return "(fz || fn != fv)";
    default:
        return "1";
    }
}

struct Emitter {
    std::string out;
    void line(const std::string &s) { out += "    " + s + "\n"; }

    std::string reg(const Insn &i, unsigned r) const { return r == 15 ? hex(i.pc + 8) : fmt("r%u", r); }
    std::string regPc12(const Insn &i, unsigned r) const { return r == 15 ? hex(i.pc + 12) : fmt("r%u", r); }

    void jump(const std::string &v, bool interwork) {
        if (interwork) {
            line("{ uint32_t j = " + v + "; c->thumb = j & 1u; npc = j & ((j & 1u) ? ~1u : ~3u); }");
        } else {
            line("npc = (" + v + ") & ~3u;");
        }
    }

    // Writes a destination register (branching when it is the PC).
    void writeRd(unsigned r, const std::string &v, bool interwork = false) {
        if (r == 15)
            jump(v, interwork);
        else
            line(fmt("r%u = ", r) + v + ";");
    }

    void operand2(const Insn &i, bool needCarry) {
        // Defines `b` (value) and `sc` (shifter carry, when needCarry) as locals.
        if (i.immOperand) {
            line("const uint32_t b = " + hex(i.imm) + ";");
            if (needCarry)
                line(i.immCarryDefined ? fmt("const uint32_t sc = %uu;", i.imm >> 31)
                                       : "const uint32_t sc = fc;");
            return;
        }
        if (i.shiftByReg) {
            line("uint32_t sc = fc;");
            const char *fn = i.shift == Shift::Lsl   ? "rc_shl_reg"
                             : i.shift == Shift::Lsr ? "rc_shr_reg"
                             : i.shift == Shift::Asr ? "rc_asr_reg"
                                                     : "rc_ror_reg";
            line(fmt("const uint32_t b = %s(", fn) + regPc12(i, i.rm) + ", r" + std::to_string(i.rs) +
                 " & 0xFFu, &sc);");
            if (!needCarry)
                line("(void)sc;");
            return;
        }
        const std::string v = reg(i, i.rm);
        const unsigned a = i.shiftAmt;
        switch (i.shift) {
        case Shift::Lsl:
            if (a == 0) {
                line("const uint32_t b = " + v + ";");
                if (needCarry)
                    line("const uint32_t sc = fc;");
            } else {
                line("const uint32_t b = " + v + fmt(" << %u;", a));
                if (needCarry)
                    line("const uint32_t sc = (" + v + fmt(" >> %u) & 1u;", 32 - a));
            }
            break;
        case Shift::Lsr:
            if (a == 32) {
                line("const uint32_t b = 0;");
                if (needCarry)
                    line("const uint32_t sc = " + v + " >> 31;");
            } else {
                line("const uint32_t b = " + v + fmt(" >> %u;", a));
                if (needCarry)
                    line("const uint32_t sc = (" + v + fmt(" >> %u) & 1u;", a - 1));
            }
            break;
        case Shift::Asr:
            if (a == 32) {
                line("const uint32_t b = (uint32_t)((int32_t)" + v + " >> 31);");
                if (needCarry)
                    line("const uint32_t sc = " + v + " >> 31;");
            } else {
                line("const uint32_t b = (uint32_t)((int32_t)" + v + fmt(" >> %u);", a));
                if (needCarry)
                    line("const uint32_t sc = (" + v + fmt(" >> %u) & 1u;", a - 1));
            }
            break;
        case Shift::Ror:
            line("const uint32_t b = " + fmt("rc_ror(%s, %u);", v.c_str(), a));
            if (needCarry)
                line("const uint32_t sc = (" + v + fmt(" >> %u) & 1u;", a - 1));
            break;
        case Shift::Rrx:
            line("const uint32_t b = (fc << 31) | (" + v + " >> 1);");
            if (needCarry)
                line("const uint32_t sc = " + v + " & 1u;");
            break;
        }
    }

    void dataProcessing(const Insn &i, uint8_t need) {
        const bool logical = i.op == Op::And || i.op == Op::Eor || i.op == Op::Orr || i.op == Op::Bic ||
                             i.op == Op::Mov || i.op == Op::Mvn || i.op == Op::Tst || i.op == Op::Teq;
        const bool compare = i.op == Op::Tst || i.op == Op::Teq || i.op == Op::Cmp || i.op == Op::Cmn;
        const bool wantFlags = i.setFlags && need != 0;
        operand2(i, wantFlags && logical && (need & FlagC));
        const bool regShiftPc = i.shiftByReg;
        std::string a = (regShiftPc ? regPc12(i, i.rn) : reg(i, i.rn));
        std::string expr;
        switch (i.op) {
        case Op::And:
        case Op::Tst:
            expr = a + " & b";
            break;
        case Op::Eor:
        case Op::Teq:
            expr = a + " ^ b";
            break;
        case Op::Orr:
            expr = a + " | b";
            break;
        case Op::Bic:
            expr = a + " & ~b";
            break;
        case Op::Mov:
            expr = "b";
            break;
        case Op::Mvn:
            expr = "~b";
            break;
        default:
            break;
        }
        if (logical) {
            line("const uint32_t res = " + expr + ";");
            if (wantFlags) {
                if (need & FlagN)
                    line("fn = res >> 31;");
                if (need & FlagZ)
                    line("fz = res == 0;");
                if (need & FlagC)
                    line("fc = sc;");
            }
        } else {
            std::string x = a, y = "b";
            std::string cin = "0";
            bool sub = false;
            switch (i.op) {
            case Op::Add:
            case Op::Cmn:
                break;
            case Op::Adc:
                cin = "fc";
                break;
            case Op::Sub:
            case Op::Cmp:
                sub = true;
                cin = "1";
                break;
            case Op::Sbc:
                sub = true;
                cin = "fc";
                break;
            case Op::Rsb:
                sub = true;
                cin = "1";
                x = "b";
                y = a;
                break;
            case Op::Rsc:
                sub = true;
                cin = "fc";
                x = "b";
                y = a;
                break;
            default:
                break;
            }
            line("const uint32_t x = " + x + ";");
            line("const uint32_t y = " + std::string(sub ? "~(" : "(") + y + ");");
            line("const uint64_t w = (uint64_t)x + y + " + cin + ";");
            line("const uint32_t res = (uint32_t)w;");
            if (wantFlags) {
                if (need & FlagN)
                    line("fn = res >> 31;");
                if (need & FlagZ)
                    line("fz = res == 0;");
                if (need & FlagC)
                    line("fc = (uint32_t)(w >> 32) & 1u;");
                if (need & FlagV)
                    line("fv = ((~(x ^ y) & (x ^ res)) >> 31) & 1u;");
            }
        }
        if (!compare)
            writeRd(i.rd, "res");
        else
            line("(void)res;");
    }

    void singleTransfer(const Insn &i) {
        const bool half = i.op == Op::Ldrh || i.op == Op::Strh || i.op == Op::Ldrsb || i.op == Op::Ldrsh ||
                          i.op == Op::Ldrd || i.op == Op::Strd;
        const bool isLoad = i.op == Op::Ldr || i.op == Op::Ldrb || i.op == Op::Ldrh || i.op == Op::Ldrsb ||
                            i.op == Op::Ldrsh || i.op == Op::Ldrd;
        std::string off;
        if (half) {
            off = i.regOffset ? reg(i, i.rm) : hex(i.imm);
        } else if (!i.regOffset) {
            off = hex(i.imm);
        } else {
            // register offset with immediate shift (carry not needed)
            const std::string v = reg(i, i.rm);
            const unsigned a = i.shiftAmt;
            switch (i.shift) {
            case Shift::Lsl:
                off = a ? "(" + v + fmt(" << %u)", a) : v;
                break;
            case Shift::Lsr:
                off = a == 32 ? "0u" : "(" + v + fmt(" >> %u)", a);
                break;
            case Shift::Asr:
                off = a == 32 ? "(uint32_t)((int32_t)" + v + " >> 31)"
                              : "(uint32_t)((int32_t)" + v + fmt(" >> %u)", a);
                break;
            case Shift::Ror:
                off = fmt("rc_ror(%s, %u)", v.c_str(), a);
                break;
            case Shift::Rrx:
                off = "((fc << 31) | (" + v + " >> 1))";
                break;
            }
        }
        line("const uint32_t base = " + reg(i, i.rn) + ";");
        line("const uint32_t moved = base " + std::string(i.up ? "+" : "-") + " " + off + ";");
        line(std::string("const uint32_t addr = ") + (i.pre ? "moved" : "base") + ";");
        const bool wb = i.pre ? i.wb : true;
        if (isLoad) {
            switch (i.op) {
            case Op::Ldr:
                line("const uint32_t v = rc_ror(RC_LD32(c, addr & ~3u), (addr & 3u) * 8u);");
                break;
            case Op::Ldrb:
                line("const uint32_t v = RC_LD8(c, addr);");
                break;
            case Op::Ldrh:
                line("const uint32_t v = RC_LD16(c, addr);");
                break;
            case Op::Ldrsb:
                line("const uint32_t v = (uint32_t)(int32_t)(int8_t)RC_LD8(c, addr);");
                break;
            case Op::Ldrsh:
                line("const uint32_t v = (uint32_t)(int32_t)(int16_t)RC_LD16(c, addr);");
                break;
            default:
                line("const uint32_t v = RC_LD32(c, addr); const uint32_t v2 = RC_LD32(c, addr + 4u);");
                break;
            }
            if (wb)
                line(fmt("r%u = moved;", i.rn));
            writeRd(i.rd, "v", i.op == Op::Ldr);
            if (i.op == Op::Ldrd)
                line(fmt("r%u = v2;", i.rd + 1));
            else if (false)
                line("(void)0;");
        } else {
            const std::string sv = i.rd == 15 ? hex(i.pc + 8) : fmt("r%u", i.rd);
            switch (i.op) {
            case Op::Str:
                line("RC_ST32(c, addr & ~3u, " + sv + ");");
                break;
            case Op::Strb:
                line("RC_ST8(c, addr, (uint8_t)" + sv + ");");
                break;
            case Op::Strh:
                line("RC_ST16(c, addr, (uint16_t)" + sv + ");");
                break;
            default:
                line("RC_ST32(c, addr, " + sv + "); RC_ST32(c, addr + 4u, " + fmt("r%u", i.rd + 1) + ");");
                break;
            }
            if (wb)
                line(fmt("r%u = moved;", i.rn));
        }
    }

    void blockTransfer(const Insn &i) {
        const unsigned n = unsigned(__builtin_popcount(i.regList));
        line(fmt("const uint32_t base = r%u;", i.rn));
        std::string start, nb;
        if (i.up) {
            start = i.pre ? "base + 4u" : "base";
            nb = fmt("base + %uu", 4 * n);
        } else {
            start = i.pre ? fmt("base - %uu", 4 * n) : fmt("base - %uu", 4 * n - 4);
            nb = fmt("base - %uu", 4 * n);
        }
        line("const uint32_t a0 = " + start + ";");
        unsigned k = 0;
        std::string pcv;
        for (unsigned r = 0; r < 16; r++) {
            if (!(i.regList & (1u << r)))
                continue;
            const std::string ad = fmt("a0 + %uu", 4 * k);
            if (i.op == Op::Ldm) {
                if (r == 15)
                    line("const uint32_t pcv = RC_LD32(c, " + ad + ");");
                else
                    line(fmt("r%u = RC_LD32(c, ", r) + ad + ");");
            } else {
                line("RC_ST32(c, " + ad + ", " + (r == 15 ? hex(i.pc + 8) : fmt("r%u", r)) + ");");
            }
            k++;
        }
        if (i.wb)
            line(fmt("r%u = ", i.rn) + nb + ";");
        if (i.op == Op::Ldm && (i.regList & 0x8000))
            jump("pcv", true);
    }

    void exclusive(const Insn &i) {
        line(fmt("const uint32_t addr = r%u;", i.rn));
        switch (i.op) {
        case Op::Ldrex:
            line(fmt("r%u = RC_LD32(c, addr);", i.rd));
            break;
        case Op::Ldrexb:
            line(fmt("r%u = RC_LD8(c, addr);", i.rd));
            break;
        case Op::Ldrexh:
            line(fmt("r%u = RC_LD16(c, addr);", i.rd));
            break;
        case Op::Ldrexd:
            line(fmt("r%u = RC_LD32(c, addr); r%u = RC_LD32(c, addr + 4u);", i.rd, i.rd + 1));
            break;
        default: {
            std::string st;
            switch (i.op) {
            case Op::Strex:
                st = fmt("RC_ST32(c, addr, r%u);", i.rm);
                break;
            case Op::Strexb:
                st = fmt("RC_ST8(c, addr, (uint8_t)r%u);", i.rm);
                break;
            case Op::Strexh:
                st = fmt("RC_ST16(c, addr, (uint16_t)r%u);", i.rm);
                break;
            default:
                st = fmt("RC_ST32(c, addr, r%u); RC_ST32(c, addr + 4u, r%u);", i.rm, i.rm + 1);
                break;
            }
            line("const uint32_t ok = c->excl_valid && c->excl_addr == addr;");
            line("if (ok) { " + st + " }");
            line("c->excl_valid = 0;");
            line(fmt("r%u = ok ? 0u : 1u;", i.rd));
            return;
        }
        }
        line("c->excl_valid = 1; c->excl_addr = addr;");
    }

    void multiply(const Insn &i, uint8_t need) {
        const bool flags = i.setFlags && (need & (FlagN | FlagZ));
        if (i.op == Op::Mul || i.op == Op::Mla) {
            std::string e = fmt("r%u * r%u", i.rm, i.rs);
            if (i.op == Op::Mla)
                e += fmt(" + r%u", i.rn);
            line("const uint32_t res = " + e + ";");
            line(fmt("r%u = res;", i.rd));
            if (flags) {
                if (need & FlagN)
                    line("fn = res >> 31;");
                if (need & FlagZ)
                    line("fz = res == 0;");
            }
            return;
        }
        std::string p;
        if (i.op == Op::Smull || i.op == Op::Smlal)
            p = fmt("(uint64_t)((int64_t)(int32_t)r%u * (int64_t)(int32_t)r%u)", i.rm, i.rs);
        else
            p = fmt("((uint64_t)r%u * r%u)", i.rm, i.rs);
        line("uint64_t p = " + p + ";");
        if (i.op == Op::Umlal || i.op == Op::Smlal)
            line(fmt("p += ((uint64_t)r%u << 32) | r%u;", i.rn, i.rd));
        if (i.op == Op::Umaal)
            line(fmt("p += (uint64_t)r%u + r%u;", i.rn, i.rd));
        line(fmt("r%u = (uint32_t)p; r%u = (uint32_t)(p >> 32);", i.rd, i.rn));
        if (flags) {
            if (need & FlagN)
                line("fn = (uint32_t)(p >> 63);");
            if (need & FlagZ)
                line("fz = p == 0;");
        }
    }

    void extend(const Insn &i) {
        const std::string v = i.rot ? fmt("rc_ror(r%u, %u)", i.rm, i.rot) : fmt("r%u", i.rm);
        std::string e;
        switch (i.op) {
        case Op::Sxtb:
        case Op::Sxtab:
            e = "(uint32_t)(int32_t)(int8_t)" + v;
            break;
        case Op::Sxth:
        case Op::Sxtah:
            e = "(uint32_t)(int32_t)(int16_t)" + v;
            break;
        case Op::Uxtb:
        case Op::Uxtab:
            e = "(" + v + " & 0xFFu)";
            break;
        default:
            e = "(" + v + " & 0xFFFFu)";
            break;
        }
        if (i.op >= Op::Sxtab)
            e = fmt("r%u + ", i.rn) + e;
        line(fmt("r%u = ", i.rd) + e + ";");
    }

    void insn(const Insn &i, uint8_t need) {
        const bool cond = i.cond != 14;
        line(fmt("/* %08x: %08x %s */", i.pc, i.word, opName(i.op)));
        if (cond)
            line("if (" + condExpr(i.cond) + ") {");
        else
            line("{");
        switch (i.op) {
        case Op::And:
        case Op::Eor:
        case Op::Sub:
        case Op::Rsb:
        case Op::Add:
        case Op::Adc:
        case Op::Sbc:
        case Op::Rsc:
        case Op::Tst:
        case Op::Teq:
        case Op::Cmp:
        case Op::Cmn:
        case Op::Orr:
        case Op::Mov:
        case Op::Bic:
        case Op::Mvn:
            dataProcessing(i, need);
            break;
        case Op::Mul:
        case Op::Mla:
        case Op::Umull:
        case Op::Umlal:
        case Op::Smull:
        case Op::Smlal:
        case Op::Umaal:
            multiply(i, need);
            break;
        case Op::Ldr:
        case Op::Str:
        case Op::Ldrb:
        case Op::Strb:
        case Op::Ldrh:
        case Op::Strh:
        case Op::Ldrsb:
        case Op::Ldrsh:
        case Op::Ldrd:
        case Op::Strd:
            singleTransfer(i);
            break;
        case Op::Ldm:
        case Op::Stm:
            blockTransfer(i);
            break;
        case Op::Swp:
            line(fmt("const uint32_t a = r%u; const uint32_t o = rc_ror(RC_LD32(c, a & ~3u), (a & 3u) * 8u);",
                     i.rn));
            line(fmt("RC_ST32(c, a & ~3u, r%u); r%u = o;", i.rm, i.rd));
            break;
        case Op::Swpb:
            line(fmt("const uint32_t a = r%u; const uint32_t o = RC_LD8(c, a);", i.rn));
            line(fmt("RC_ST8(c, a, (uint8_t)r%u); r%u = o;", i.rm, i.rd));
            break;
        case Op::Ldrex:
        case Op::Strex:
        case Op::Ldrexb:
        case Op::Strexb:
        case Op::Ldrexh:
        case Op::Strexh:
        case Op::Ldrexd:
        case Op::Strexd:
            exclusive(i);
            break;
        case Op::Clrex:
            line("c->excl_valid = 0;");
            break;
        case Op::B:
            line("npc = " + hex(i.target) + ";");
            break;
        case Op::Bl:
            line("r14 = " + hex(i.pc + 4) + "; npc = " + hex(i.target) + ";");
            break;
        case Op::Bx:
            jump(reg(i, i.rm), true);
            break;
        case Op::BlxReg:
            line("const uint32_t t = " + reg(i, i.rm) + ";");
            line("r14 = " + hex(i.pc + 4) + ";");
            jump("t", true);
            break;
        case Op::BlxImm:
            line("r14 = " + hex(i.pc + 4) + "; c->thumb = 1; npc = " + hex(i.target) + ";");
            break;
        case Op::Clz:
            line(fmt("r%u = r%u ? (uint32_t)__builtin_clz(r%u) : 32u;", i.rd, i.rm, i.rm));
            break;
        case Op::Rev:
            line(fmt("r%u = __builtin_bswap32(r%u);", i.rd, i.rm));
            break;
        case Op::Rev16:
            line(fmt("r%u = ((r%u & 0x00FF00FFu) << 8) | ((r%u >> 8) & 0x00FF00FFu);", i.rd, i.rm, i.rm));
            break;
        case Op::Revsh:
            line(fmt("r%u = (uint32_t)(int32_t)(int16_t)(((r%u & 0xFFu) << 8) | ((r%u >> 8) & 0xFFu));", i.rd,
                     i.rm, i.rm));
            break;
        case Op::Sxtb:
        case Op::Sxth:
        case Op::Uxtb:
        case Op::Uxth:
        case Op::Sxtab:
        case Op::Sxtah:
        case Op::Uxtab:
        case Op::Uxtah:
            extend(i);
            break;
        case Op::Par:
            line(fmt("{ uint32_t ge = c->ge; r%u = rc_par(%u, %u, r%u, r%u, &ge); c->ge = ge; }", i.rd, i.sub,
                     i.sub2, i.rn, i.rm));
            break;
        case Op::Sel:
            line(fmt("r%u = rc_sel(r%u, r%u, c->ge);", i.rd, i.rn, i.rm));
            break;
        case Op::Pkh:
            if (!i.fx)
                line(fmt("r%u = (r%u & 0xFFFFu) | ((r%u << %u) & 0xFFFF0000u);", i.rd, i.rn, i.rm,
                         i.shiftAmt));
            else
                line(fmt("r%u = (r%u & 0xFFFF0000u) | ((uint32_t)((int32_t)r%u >> %u) & 0xFFFFu);", i.rd,
                         i.rn, i.rm, i.shiftAmt == 32 ? 31 : i.shiftAmt));
            break;
        case Op::Ssat:
        case Op::Usat: {
            std::string v;
            if (i.shift == Shift::Asr)
                v = fmt("(int64_t)((int32_t)r%u >> %u)", i.rm, i.shiftAmt == 32 ? 31 : i.shiftAmt);
            else
                v = fmt("(int64_t)(int32_t)(r%u << %u)", i.rm, i.shiftAmt);
            line(fmt("r%u = %s(", i.rd, i.op == Op::Ssat ? "rc_ssat" : "rc_usat") + v +
                 fmt(", %u, &c->q);", i.imm));
            break;
        }
        case Op::Ssat16:
        case Op::Usat16: {
            const char *f = i.op == Op::Ssat16 ? "rc_ssat" : "rc_usat";
            line(fmt("r%u = (%s(rc_sx(r%u & 0xFFFFu, 16), %u, &c->q) & 0xFFFFu) | ((%s(rc_sx(r%u >> 16, 16), "
                     "%u, &c->q) & "
                     "0xFFFFu) << 16);",
                     i.rd, f, i.rm, i.imm, f, i.rm, i.imm));
            break;
        }
        case Op::Sxtb16:
        case Op::Uxtb16:
        case Op::Sxtab16:
        case Op::Uxtab16: {
            const bool sg = i.op == Op::Sxtb16 || i.op == Op::Sxtab16,
                       add = i.op == Op::Sxtab16 || i.op == Op::Uxtab16;
            const std::string src = i.rot ? fmt("rc_ror(r%u, %u)", i.rm, i.rot) : fmt("r%u", i.rm);
            line(fmt("r%u = rc_ext16(", i.rd) + src + fmt(", %d, %d, ", sg ? 1 : 0, add ? 1 : 0) +
                 (add ? fmt("r%u", i.rn) : std::string("0u")) + ");");
            break;
        }
        case Op::Usad8:
        case Op::Usada8:
            line(fmt("r%u = rc_usad(r%u, r%u, ", i.rd, i.rm, i.rs) +
                 (i.op == Op::Usada8 ? fmt("r%u", i.rn) : std::string("0u")) + ");");
            break;
        case Op::Smlad:
        case Op::Smlsd: {
            const std::string m = i.fx ? fmt("rc_ror(r%u, 16)", i.rs) : fmt("r%u", i.rs);
            line(fmt("{ const uint32_t n = r%u, m = ", i.rm) + m + ";");
            line("  int64_t p1 = rc_sx(n & 0xFFFFu, 16) * rc_sx(m & 0xFFFFu, 16), p2 = rc_sx(n >> 16, 16) * "
                 "rc_sx(m >> 16, 16);");
            line(std::string("  int64_t r = ") + (i.op == Op::Smlad ? "p1 + p2;" : "p1 - p2;"));
            if (i.acc)
                line(fmt("  r += (int32_t)r%u;", i.rn));
            line(fmt("  if (r != (int32_t)r) c->q = 1; r%u = (uint32_t)r; }", i.rd));
            break;
        }
        case Op::Smlald:
        case Op::Smlsld: {
            const std::string m = i.fx ? fmt("rc_ror(r%u, 16)", i.rs) : fmt("r%u", i.rs);
            line(fmt("{ const uint32_t n = r%u, m = ", i.rm) + m + ";");
            line("  int64_t p1 = rc_sx(n & 0xFFFFu, 16) * rc_sx(m & 0xFFFFu, 16), p2 = rc_sx(n >> 16, 16) * "
                 "rc_sx(m >> 16, 16);");
            line(fmt("  int64_t r = (int64_t)(((uint64_t)r%u << 32) | r%u) + ", i.rn, i.rd) +
                 (i.op == Op::Smlald ? "(p1 + p2);" : "(p1 - p2);"));
            line(fmt("  r%u = (uint32_t)r; r%u = (uint32_t)((uint64_t)r >> 32); }", i.rd, i.rn));
            break;
        }
        case Op::Smmla:
        case Op::Smmls: {
            line(fmt("{ const int64_t prod = (int64_t)(int32_t)r%u * (int64_t)(int32_t)r%u;", i.rm, i.rs));
            line(std::string("  __int128 acc = ") +
                 (i.acc ? fmt("(__int128)(int32_t)r%u << 32;", i.rn) : std::string("0;")));
            line(std::string("  acc = acc ") + (i.op == Op::Smmls ? "- prod;" : "+ prod;"));
            if (i.fx)
                line("  acc += 0x80000000ll;");
            line(fmt("  r%u = (uint32_t)(uint64_t)(acc >> 32); }", i.rd));
            break;
        }
        case Op::Qadd:
        case Op::Qsub:
        case Op::Qdadd:
        case Op::Qdsub:
            line(fmt("r%u = rc_qop(%d, %d, r%u, r%u, &c->q);", i.rd,
                     (i.op == Op::Qsub || i.op == Op::Qdsub) ? 1 : 0,
                     (i.op == Op::Qdadd || i.op == Op::Qdsub) ? 1 : 0, i.rm, i.rn));
            break;
        case Op::Smla:
        case Op::Smlaw:
        case Op::Smlal16: {
            const std::string b = fmt("rc_sx(r%u %s, 16)", i.rs, i.fy ? ">> 16" : "& 0xFFFFu");
            if (i.op == Op::Smlaw) {
                line("{ int64_t r = (((int64_t)(int32_t)" + fmt("r%u", i.rm) + ") * " + b + ") >> 16;");
                if (i.acc)
                    line(fmt("  r += (int32_t)r%u; if (r != (int32_t)r) c->q = 1;", i.rn));
                line(fmt("  r%u = (uint32_t)r; }", i.rd));
                break;
            }
            const std::string a = fmt("rc_sx(r%u %s, 16)", i.rm, i.fx ? ">> 16" : "& 0xFFFFu");
            if (i.op == Op::Smlal16) {
                line("{ int64_t r = (int64_t)(((uint64_t)" + fmt("r%u", i.rn) + " << 32) | " +
                     fmt("r%u", i.rd) + ") + " + a + " * " + b + ";");
                line(fmt("  r%u = (uint32_t)r; r%u = (uint32_t)((uint64_t)r >> 32); }", i.rd, i.rn));
                break;
            }
            line("{ int64_t r = " + a + " * " + b + ";");
            if (i.acc)
                line(fmt("  r += (int32_t)r%u; if (r != (int32_t)r) c->q = 1;", i.rn));
            line(fmt("  r%u = (uint32_t)r; }", i.rd));
            break;
        }
        case Op::Svc:
            line(fmt("c->svc = %uu;", i.imm));
            break;
        case Op::Nop:
        case Op::Pld:
        case Op::Barrier:
            line("(void)0;");
            break;
        case Op::MrcTls:
            writeRd(i.rd, "c->tls");
            break;
        case Op::Mrs:
            line(
                fmt("r%u = (fn << 31) | (fz << 30) | (fc << 29) | (fv << 28) | (c->q << 27) | (c->ge << 16);",
                    i.rd));
            break;
        case Op::Msr: {
            const std::string v = i.immOperand ? hex(i.imm) : fmt("r%u", i.rm);
            const unsigned mask = (i.word >> 16) & 0xF;
            if (mask & 8)
                line("{ const uint32_t v = " + v +
                     "; fn = v >> 31; fz = (v >> 30) & 1u; fc = (v >> 29) & 1u; fv = (v >> 28) & 1u; c->q = "
                     "(v >> 27) & 1u; }");
            if (mask & 4)
                line("c->ge = ((" + v + ") >> 16) & 0xFu;");
            break;
        }
        default:
            line(fmt("c->trap = 1; c->trap_pc = %s; npc = %s;", hex(i.pc).c_str(), hex(i.pc).c_str()));
            break;
        }
        line("}");
    }
};

} // namespace

std::string emitPrelude() {
    return "#include \"pokerecomp/cpu.h\"\n#include \"pokerecomp/rc_ops.h\"\n#include "
           "\"pokerecomp/mem_fast.h\"\n\n";
}

std::string emitBlockC(const std::vector<Insn> &insns, const std::string &name, const EmitOptions &opt) {
    // Backward flag liveness. A conditional instruction cannot kill flags (it may not execute).
    std::vector<uint8_t> liveAfter(insns.size());
    uint8_t live = opt.eliminateDeadFlags ? opt.liveOut : FlagAll;
    for (size_t k = insns.size(); k-- > 0;) {
        liveAfter[k] = opt.eliminateDeadFlags ? live : FlagAll;
        const Insn &i = insns[k];
        const uint8_t kills = i.cond == 14 ? i.writesFlags : 0;
        live = uint8_t((live & ~kills) | i.readsFlags);
    }
    Emitter e;
    e.out += "void " + name + "(RcCtx* c) {\n";
    for (int r = 0; r < 15; r++)
        e.line(fmt("uint32_t r%d = c->r[%d];", r, r));
    e.line("uint32_t fn = c->n, fz = c->z, fc = c->c, fv = c->v;");
    e.line(fmt("uint32_t npc = %s;", hex(insns.empty() ? 0 : insns.back().pc + 4).c_str()));
    for (size_t k = 0; k < insns.size(); k++)
        e.insn(insns[k], liveAfter[k]);
    for (int r = 0; r < 15; r++)
        e.line(fmt("c->r[%d] = r%d;", r, r));
    e.line("c->n = fn; c->z = fz; c->c = fc; c->v = fv;");
    e.line("c->r[15] = npc;");
    e.out += "}\n";
    return e.out;
}

} // namespace recomp
