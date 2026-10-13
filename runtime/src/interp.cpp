#include "recomp/interp.h"

#include "pokerecomp/vfp.h"

namespace recomp {
namespace {

inline uint32_t ror(uint32_t v, unsigned n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

bool condPass(const RcCtx &c, unsigned cond) {
    switch (cond) {
    case 0:
        return c.z;
    case 1:
        return !c.z;
    case 2:
        return c.c;
    case 3:
        return !c.c;
    case 4:
        return c.n;
    case 5:
        return !c.n;
    case 6:
        return c.v;
    case 7:
        return !c.v;
    case 8:
        return c.c && !c.z;
    case 9:
        return !c.c || c.z;
    case 10:
        return c.n == c.v;
    case 11:
        return c.n != c.v;
    case 12:
        return !c.z && c.n == c.v;
    case 13:
        return c.z || c.n != c.v;
    default:
        return true;
    }
}

struct Shifted {
    uint32_t value;
    uint32_t carry;
};

Shifted shiftImm(Shift t, uint32_t v, unsigned amt, uint32_t cIn) {
    switch (t) {
    case Shift::Lsl:
        if (amt == 0)
            return {v, cIn};
        return {v << amt, (v >> (32 - amt)) & 1};
    case Shift::Lsr:
        if (amt == 32)
            return {0, v >> 31};
        return {v >> amt, (v >> (amt - 1)) & 1};
    case Shift::Asr:
        if (amt == 32)
            return {uint32_t(int32_t(v) >> 31), v >> 31};
        return {uint32_t(int32_t(v) >> amt), (v >> (amt - 1)) & 1};
    case Shift::Ror:
        return {ror(v, amt), (v >> (amt - 1)) & 1};
    case Shift::Rrx:
        return {(cIn << 31) | (v >> 1), v & 1};
    }
    return {v, cIn};
}

Shifted shiftReg(Shift t, uint32_t v, uint32_t rsVal, uint32_t cIn) {
    const unsigned amt = rsVal & 0xFF;
    if (amt == 0)
        return {v, cIn};
    switch (t) {
    case Shift::Lsl:
        if (amt < 32)
            return {v << amt, (v >> (32 - amt)) & 1};
        if (amt == 32)
            return {0, v & 1};
        return {0, 0};
    case Shift::Lsr:
        if (amt < 32)
            return {v >> amt, (v >> (amt - 1)) & 1};
        if (amt == 32)
            return {0, v >> 31};
        return {0, 0};
    case Shift::Asr:
        if (amt < 32)
            return {uint32_t(int32_t(v) >> amt), (v >> (amt - 1)) & 1};
        return {uint32_t(int32_t(v) >> 31), v >> 31};
    default: {
        if ((amt & 31) == 0)
            return {v, v >> 31};
        const uint32_t r = ror(v, amt & 31);
        return {r, r >> 31};
    }
    }
}

// ---- ARMv6 media / DSP helpers (written lane-wise; independent of the generated-code helpers) ----
int64_t sext(uint32_t v, unsigned bits) { return int64_t(int32_t(v << (32 - bits))) >> (32 - bits); }

uint32_t satSigned(int64_t v, unsigned n, uint32_t &q) {
    const int64_t hi = (int64_t(1) << (n - 1)) - 1, lo = -(int64_t(1) << (n - 1));
    if (v > hi) {
        q = 1;
        return uint32_t(hi);
    }
    if (v < lo) {
        q = 1;
        return uint32_t(lo);
    }
    return uint32_t(v);
}

uint32_t satUnsigned(int64_t v, unsigned n, uint32_t &q) {
    const int64_t hi = (int64_t(1) << n) - 1;
    if (v > hi) {
        q = 1;
        return uint32_t(hi);
    }
    if (v < 0) {
        q = 1;
        return 0;
    }
    return uint32_t(v);
}

uint32_t parallel(unsigned kind, unsigned pop, uint32_t a, uint32_t b, uint32_t &ge) {
    const bool isSigned = kind <= 2;
    const unsigned bits = pop >= 4 ? 8 : 16;
    const unsigned lanes = 32 / bits;
    const uint32_t mask = (1u << bits) - 1;
    uint32_t out = 0, geOut = 0;
    for (unsigned l = 0; l < lanes; l++) {
        unsigned bl = l; // lane of b
        bool add;
        switch (pop) {
        case 0:
        case 4:
            add = true;
            break;
        case 3:
        case 5:
            add = false;
            break;
        case 1: // ASX: lo = a.lo - b.hi, hi = a.hi + b.lo
            add = l == 1;
            bl = 1 - l;
            break;
        default: // SAX: lo = a.lo + b.hi, hi = a.hi - b.lo
            add = l == 0;
            bl = 1 - l;
            break;
        }
        const uint32_t ax = (a >> (l * bits)) & mask, bx = (b >> (bl * bits)) & mask;
        const int64_t x = isSigned ? sext(ax, bits) : int64_t(ax);
        const int64_t y = isSigned ? sext(bx, bits) : int64_t(bx);
        const int64_t r = add ? x + y : x - y;
        uint32_t lane;
        uint32_t dummy = 0;
        switch (kind) {
        case 0:
        case 3:
            lane = uint32_t(r) & mask;
            if (isSigned ? r >= 0 : (add ? r >= (int64_t(1) << bits) : r >= 0))
                geOut |= (bits == 16 ? 3u : 1u) << (l * (bits == 16 ? 2 : 1));
            break;
        case 1:
            lane = satSigned(r, bits, dummy) & mask;
            break;
        case 4:
            lane = satUnsigned(r, bits, dummy) & mask;
            break;
        default:
            lane = uint32_t(r >> 1) & mask;
            break;
        }
        out |= lane << (l * bits);
    }
    if (kind == 0 || kind == 3)
        ge = geOut;
    return out;
}

void setNZ(RcCtx &c, uint32_t r) {
    c.n = r >> 31;
    c.z = r == 0;
}

struct Exec {
    RcCtx &c;
    Memory &m;
    const Insn &i;

    uint32_t pcRead() const { return i.pcRead(); }
    uint32_t reg(unsigned r) const { return r == 15 ? pcRead() : c.r[r]; }

    void jump(uint32_t v, bool interwork) {
        if (interwork) {
            c.thumb = v & 1;
            c.r[15] = v & (c.thumb ? ~1u : ~3u);
        } else {
            c.r[15] = v & (i.thumb ? ~1u : ~3u);
        }
    }

    // Writes a result register; PC writes branch.
    void writeRd(unsigned r, uint32_t v, bool interwork = false) {
        if (r == 15)
            jump(v, interwork);
        else
            c.r[r] = v;
    }

    uint32_t load32(uint32_t a) {
        const uint32_t v = m.r32(a & ~3u);
        return ror(v, (a & 3) * 8);
    }

    void dataProcessing() {
        const uint32_t cIn = c.c;
        uint32_t b, shCarry = cIn;
        uint32_t a;
        if (i.immOperand) {
            b = i.imm;
            if (i.immCarryDefined)
                shCarry = b >> 31;
            a = reg(i.rn);
        } else if (i.shiftByReg) {
            // Register-specified shifts read PC as pc+12.
            const uint32_t rmv = i.rm == 15 ? i.pc + 12 : c.r[i.rm];
            a = i.rn == 15 ? i.pc + 12 : c.r[i.rn];
            Shifted s = shiftReg(i.shift, rmv, c.r[i.rs], cIn);
            b = s.value;
            shCarry = s.carry;
        } else {
            Shifted s = shiftImm(i.shift, reg(i.rm), i.shiftAmt, cIn);
            b = s.value;
            shCarry = s.carry;
            a = reg(i.rn);
        }

        uint32_t res = 0;
        uint64_t wide = 0;
        bool arith = false, sub = false;
        uint32_t x = a, y = b, carryIn = 0;
        switch (i.op) {
        case Op::And:
        case Op::Tst:
            res = a & b;
            break;
        case Op::Eor:
        case Op::Teq:
            res = a ^ b;
            break;
        case Op::Orr:
            res = a | b;
            break;
        case Op::Bic:
            res = a & ~b;
            break;
        case Op::Mov:
            res = b;
            break;
        case Op::Mvn:
            res = ~b;
            break;
        case Op::Add:
        case Op::Cmn:
            arith = true;
            break;
        case Op::Adc:
            arith = true;
            carryIn = cIn;
            break;
        case Op::Sub:
        case Op::Cmp:
            arith = sub = true;
            carryIn = 1;
            break;
        case Op::Sbc:
            arith = sub = true;
            carryIn = cIn;
            break;
        case Op::Rsb:
            arith = sub = true;
            carryIn = 1;
            x = b;
            y = a;
            break;
        case Op::Rsc:
            arith = sub = true;
            carryIn = cIn;
            x = b;
            y = a;
            break;
        default:
            break;
        }
        if (arith) {
            const uint32_t yy = sub ? ~y : y;
            wide = uint64_t(x) + yy + carryIn;
            res = uint32_t(wide);
        }
        if (i.setFlags) {
            setNZ(c, res);
            if (arith) {
                c.c = uint32_t(wide >> 32) & 1;
                const uint32_t yy = sub ? ~y : y;
                c.v = ((~(x ^ yy) & (x ^ res)) >> 31) & 1;
            } else {
                c.c = shCarry;
            }
        }
        if (i.op < Op::Tst || i.op > Op::Cmn)
            writeRd(i.rd, res);
    }

    uint32_t offsetValue() {
        if (!i.regOffset)
            return i.imm;
        return shiftImm(i.shift, reg(i.rm), i.shiftAmt, c.c).value;
    }

    void singleTransfer() {
        const uint32_t base = reg(i.rn);
        const uint32_t off = (i.op == Op::Ldrh || i.op == Op::Strh || i.op == Op::Ldrsb ||
                              i.op == Op::Ldrsh || i.op == Op::Ldrd || i.op == Op::Strd)
                                 ? (i.regOffset ? reg(i.rm) : i.imm)
                                 : offsetValue();
        const uint32_t moved = i.up ? base + off : base - off;
        const uint32_t addr = i.pre ? moved : base;
        const bool writeback = i.pre ? i.wb : true;
        const bool isLoad = i.op == Op::Ldr || i.op == Op::Ldrb || i.op == Op::Ldrh || i.op == Op::Ldrsb ||
                            i.op == Op::Ldrsh || i.op == Op::Ldrd;
        uint32_t v = 0, v2 = 0;
        if (isLoad) {
            switch (i.op) {
            case Op::Ldr:
                v = m.r32(addr); // SCTLR.U=1: true unaligned access
                break;
            case Op::Ldrb:
                v = m.r8(addr);
                break;
            case Op::Ldrh:
                v = m.r16(addr);
                break;
            case Op::Ldrsb:
                v = uint32_t(int32_t(int8_t(m.r8(addr))));
                break;
            case Op::Ldrsh:
                v = uint32_t(int32_t(int16_t(m.r16(addr))));
                break;
            default:
                v = m.r32(addr);
                v2 = m.r32(addr + 4);
                break;
            }
        } else {
            const uint32_t sv = i.rd == 15 ? i.pcRead() : c.r[i.rd];
            switch (i.op) {
            case Op::Str:
                m.w32(addr, sv);
                break;
            case Op::Strb:
                m.w8(addr, uint8_t(sv));
                break;
            case Op::Strh:
                m.w16(addr, uint16_t(sv));
                break;
            default:
                m.w32(addr, sv);
                m.w32(addr + 4, c.r[i.rd + 1]);
                break;
            }
        }
        if (writeback)
            c.r[i.rn] = moved;
        if (isLoad) {
            writeRd(i.rd, v, i.op == Op::Ldr);
            if (i.op == Op::Ldrd)
                c.r[i.rd + 1] = v2;
        }
    }

    void blockTransfer() {
        const unsigned n = unsigned(__builtin_popcount(i.regList));
        const uint32_t base = c.r[i.rn];
        uint32_t start;
        uint32_t newBase;
        if (i.up) {
            start = i.pre ? base + 4 : base;
            newBase = base + 4 * n;
        } else {
            start = i.pre ? base - 4 * n : base - 4 * n + 4;
            newBase = base - 4 * n;
        }
        const bool load = i.op == Op::Ldm;
        uint32_t a = start;
        uint32_t pcVal = 0;
        bool loadsPc = false;
        for (unsigned r = 0; r < 16; r++) {
            if (!(i.regList & (1u << r)))
                continue;
            if (load) {
                const uint32_t v = m.r32(a);
                if (r == 15) {
                    pcVal = v;
                    loadsPc = true;
                } else
                    c.r[r] = v;
            } else {
                m.w32(a, r == 15 ? i.pcRead() : c.r[r]);
            }
            a += 4;
        }
        if (i.wb)
            c.r[i.rn] = newBase;
        if (loadsPc)
            jump(pcVal, true);
    }

    void exclusive() {
        const uint32_t addr = c.r[i.rn];
        switch (i.op) {
        case Op::Ldrex:
            c.r[i.rd] = m.r32(addr);
            break;
        case Op::Ldrexb:
            c.r[i.rd] = m.r8(addr);
            break;
        case Op::Ldrexh:
            c.r[i.rd] = m.r16(addr);
            break;
        case Op::Ldrexd:
            c.r[i.rd] = m.r32(addr);
            c.r[i.rd + 1] = m.r32(addr + 4);
            break;
        default: {
            const bool ok = c.excl_valid && c.excl_addr == addr;
            if (ok) {
                switch (i.op) {
                case Op::Strex:
                    m.w32(addr, c.r[i.rm]);
                    break;
                case Op::Strexb:
                    m.w8(addr, uint8_t(c.r[i.rm]));
                    break;
                case Op::Strexh:
                    m.w16(addr, uint16_t(c.r[i.rm]));
                    break;
                default:
                    m.w32(addr, c.r[i.rm]);
                    m.w32(addr + 4, c.r[i.rm + 1]);
                    break;
                }
            }
            c.excl_valid = 0;
            c.r[i.rd] = ok ? 0 : 1;
            return;
        }
        }
        c.excl_valid = 1;
        c.excl_addr = addr;
    }

    void multiply() {
        const uint32_t m_ = c.r[i.rm], s_ = c.r[i.rs];
        switch (i.op) {
        case Op::Mul:
        case Op::Mla: {
            uint32_t r = m_ * s_;
            if (i.op == Op::Mla)
                r += c.r[i.rn];
            c.r[i.rd] = r;
            if (i.setFlags)
                setNZ(c, r);
            break;
        }
        default: {
            uint64_t p;
            if (i.op == Op::Smull || i.op == Op::Smlal)
                p = uint64_t(int64_t(int32_t(m_)) * int64_t(int32_t(s_)));
            else
                p = uint64_t(m_) * s_;
            const uint64_t acc = (uint64_t(c.r[i.rn]) << 32) | c.r[i.rd];
            if (i.op == Op::Umlal || i.op == Op::Smlal)
                p += acc;
            if (i.op == Op::Umaal)
                p += uint64_t(c.r[i.rn]) + c.r[i.rd];
            c.r[i.rd] = uint32_t(p);
            c.r[i.rn] = uint32_t(p >> 32);
            if (i.setFlags) {
                c.n = uint32_t(p >> 63);
                c.z = p == 0;
            }
            break;
        }
        }
    }

    void extend() {
        const uint32_t v = ror(c.r[i.rm], i.rot);
        uint32_t e = 0;
        switch (i.op) {
        case Op::Sxtb:
        case Op::Sxtab:
            e = uint32_t(int32_t(int8_t(v)));
            break;
        case Op::Sxth:
        case Op::Sxtah:
            e = uint32_t(int32_t(int16_t(v)));
            break;
        case Op::Uxtb:
        case Op::Uxtab:
            e = v & 0xFF;
            break;
        default:
            e = v & 0xFFFF;
            break;
        }
        if (i.op >= Op::Sxtab)
            e += c.r[i.rn];
        c.r[i.rd] = e;
    }
};

} // namespace

void step(RcCtx &c, Memory &m, const Insn &i) {
    c.r[15] = i.next();
    if (i.op == Op::Undef) {
        c.trap = 1;
        c.trap_pc = i.pc;
        c.r[15] = i.pc;
        return;
    }
    if (!condPass(c, i.cond))
        return;
    Exec x{c, m, i};
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
        x.dataProcessing();
        break;
    case Op::Mul:
    case Op::Mla:
    case Op::Umull:
    case Op::Umlal:
    case Op::Smull:
    case Op::Smlal:
    case Op::Umaal:
        x.multiply();
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
        x.singleTransfer();
        break;
    case Op::Ldm:
    case Op::Stm:
        x.blockTransfer();
        break;
    case Op::Swp:
    case Op::Swpb: {
        const uint32_t a = c.r[i.rn];
        const uint32_t nv = c.r[i.rm];
        if (i.op == Op::Swp) {
            const uint32_t o = x.load32(a);
            m.w32(a & ~3u, nv);
            c.r[i.rd] = o;
        } else {
            const uint32_t o = m.r8(a);
            m.w8(a, uint8_t(nv));
            c.r[i.rd] = o;
        }
        break;
    }
    case Op::Ldrex:
    case Op::Strex:
    case Op::Ldrexb:
    case Op::Strexb:
    case Op::Ldrexh:
    case Op::Strexh:
    case Op::Ldrexd:
    case Op::Strexd:
        x.exclusive();
        break;
    case Op::Clrex:
        c.excl_valid = 0;
        break;
    case Op::B:
        c.r[15] = i.target;
        break;
    case Op::Bl:
        c.r[14] = i.link();
        c.r[15] = i.target;
        break;
    case Op::Bx:
        x.jump(x.reg(i.rm), true);
        break;
    case Op::BlxReg: {
        const uint32_t t = x.reg(i.rm);
        c.r[14] = i.link();
        x.jump(t, true);
        break;
    }
    case Op::BlxImm:
        c.r[14] = i.link();
        c.thumb = i.targetThumb;
        c.r[15] = i.target;
        break;
    case Op::Clz: {
        const uint32_t v = c.r[i.rm];
        c.r[i.rd] = v ? uint32_t(__builtin_clz(v)) : 32;
        break;
    }
    case Op::Rev:
        c.r[i.rd] = __builtin_bswap32(c.r[i.rm]);
        break;
    case Op::Rev16: {
        const uint32_t v = c.r[i.rm];
        c.r[i.rd] = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
        break;
    }
    case Op::Revsh: {
        const uint32_t v = c.r[i.rm];
        c.r[i.rd] = uint32_t(int32_t(int16_t(((v & 0xFF) << 8) | ((v >> 8) & 0xFF))));
        break;
    }
    case Op::Sxtb:
    case Op::Sxth:
    case Op::Uxtb:
    case Op::Uxth:
    case Op::Sxtab:
    case Op::Sxtah:
    case Op::Uxtab:
    case Op::Uxtah:
        x.extend();
        break;
    case Op::Par:
        c.r[i.rd] = parallel(i.sub, i.sub2, c.r[i.rn], c.r[i.rm], c.ge);
        break;
    case Op::Sel: {
        uint32_t r = 0;
        for (unsigned l = 0; l < 4; l++)
            r |= ((c.ge >> l) & 1 ? c.r[i.rn] : c.r[i.rm]) & (0xFFu << (8 * l));
        c.r[i.rd] = r;
        break;
    }
    case Op::Pkh: {
        const uint32_t n = c.r[i.rn], m = c.r[i.rm];
        if (!i.fx)
            c.r[i.rd] = (n & 0xFFFF) | ((m << i.shiftAmt) & 0xFFFF0000u);
        else
            c.r[i.rd] =
                (n & 0xFFFF0000u) | (uint32_t(int32_t(m) >> (i.shiftAmt == 32 ? 31 : i.shiftAmt)) & 0xFFFF);
        break;
    }
    case Op::Ssat:
    case Op::Usat: {
        int64_t v = i.shift == Shift::Asr
                        ? int64_t(int32_t(c.r[i.rm])) >> (i.shiftAmt == 32 ? 31 : i.shiftAmt)
                        : int64_t(int32_t(c.r[i.rm] << i.shiftAmt));
        c.r[i.rd] = i.op == Op::Ssat ? satSigned(v, i.imm, c.q) : satUnsigned(v, i.imm, c.q);
        break;
    }
    case Op::Ssat16:
    case Op::Usat16: {
        uint32_t r = 0;
        for (unsigned h = 0; h < 2; h++) {
            const int64_t v = sext((c.r[i.rm] >> (16 * h)) & 0xFFFF, 16);
            const uint32_t s = i.op == Op::Ssat16 ? satSigned(v, i.imm, c.q) : satUnsigned(v, i.imm, c.q);
            r |= (s & 0xFFFF) << (16 * h);
        }
        c.r[i.rd] = r;
        break;
    }
    case Op::Sxtb16:
    case Op::Uxtb16:
    case Op::Sxtab16:
    case Op::Uxtab16: {
        const uint32_t v = ror(c.r[i.rm], i.rot);
        const bool sg = i.op == Op::Sxtb16 || i.op == Op::Sxtab16;
        const bool add = i.op == Op::Sxtab16 || i.op == Op::Uxtab16;
        uint32_t r = 0;
        for (unsigned h = 0; h < 2; h++) {
            const uint32_t b = (v >> (16 * h)) & 0xFF;
            uint32_t e = sg ? uint32_t(int32_t(int8_t(b))) & 0xFFFF : b;
            if (add)
                e = (e + (c.r[i.rn] >> (16 * h))) & 0xFFFF;
            r |= e << (16 * h);
        }
        c.r[i.rd] = r;
        break;
    }
    case Op::Usad8:
    case Op::Usada8: {
        uint32_t sum = i.op == Op::Usada8 ? c.r[i.rn] : 0;
        for (unsigned l = 0; l < 4; l++) {
            const int a = (c.r[i.rm] >> (8 * l)) & 0xFF, b = (c.r[i.rs] >> (8 * l)) & 0xFF;
            sum += uint32_t(a > b ? a - b : b - a);
        }
        c.r[i.rd] = sum;
        break;
    }
    case Op::Smlad:
    case Op::Smlsd: {
        const uint32_t n = c.r[i.rm], m = i.fx ? ror(c.r[i.rs], 16) : c.r[i.rs];
        const int64_t p1 = sext(n & 0xFFFF, 16) * sext(m & 0xFFFF, 16);
        const int64_t p2 = sext(n >> 16, 16) * sext(m >> 16, 16);
        int64_t r = i.op == Op::Smlad ? p1 + p2 : p1 - p2;
        if (i.acc)
            r += int32_t(c.r[i.rn]);
        if (r != int32_t(r))
            c.q = 1;
        c.r[i.rd] = uint32_t(r);
        break;
    }
    case Op::Smlald:
    case Op::Smlsld: {
        const uint32_t n = c.r[i.rm], m = i.fx ? ror(c.r[i.rs], 16) : c.r[i.rs];
        const int64_t p1 = sext(n & 0xFFFF, 16) * sext(m & 0xFFFF, 16);
        const int64_t p2 = sext(n >> 16, 16) * sext(m >> 16, 16);
        const int64_t r =
            int64_t((uint64_t(c.r[i.rn]) << 32) | c.r[i.rd]) + (i.op == Op::Smlald ? p1 + p2 : p1 - p2);
        c.r[i.rd] = uint32_t(r);
        c.r[i.rn] = uint32_t(uint64_t(r) >> 32);
        break;
    }
    case Op::Smmla:
    case Op::Smmls: {
        const int64_t prod = int64_t(int32_t(c.r[i.rm])) * int64_t(int32_t(c.r[i.rs]));
        __int128 acc = i.acc ? __int128(int32_t(c.r[i.rn])) * (__int128(1) << 32) : 0;
        acc = i.op == Op::Smmls ? acc - prod : acc + prod;
        if (i.fx)
            acc += 0x80000000ll;
        c.r[i.rd] = uint32_t(uint64_t(acc >> 32));
        break;
    }
    case Op::Qadd:
    case Op::Qsub:
    case Op::Qdadd:
    case Op::Qdsub: {
        const int64_t m = int32_t(c.r[i.rm]);
        int64_t n = int32_t(c.r[i.rn]);
        if (i.op == Op::Qdadd || i.op == Op::Qdsub)
            n = int32_t(satSigned(n * 2, 32, c.q));
        const bool sub = i.op == Op::Qsub || i.op == Op::Qdsub;
        c.r[i.rd] = satSigned(sub ? m - n : m + n, 32, c.q);
        break;
    }
    case Op::Smla:
    case Op::Smlaw:
    case Op::Smlal16: {
        const uint32_t n = c.r[i.rm], m = c.r[i.rs];
        const int64_t b = sext(i.fy ? m >> 16 : m & 0xFFFF, 16);
        if (i.op == Op::Smlaw) {
            const int64_t r = ((int64_t(int32_t(n)) * b) >> 16) + (i.acc ? int32_t(c.r[i.rn]) : 0);
            if (i.acc && r != int32_t(r))
                c.q = 1;
            c.r[i.rd] = uint32_t(r);
            break;
        }
        const int64_t p = sext(i.fx ? n >> 16 : n & 0xFFFF, 16) * b;
        if (i.op == Op::Smlal16) {
            const int64_t r = int64_t((uint64_t(c.r[i.rn]) << 32) | c.r[i.rd]) + p;
            c.r[i.rd] = uint32_t(r);
            c.r[i.rn] = uint32_t(uint64_t(r) >> 32);
            break;
        }
        const int64_t r = p + (i.acc ? int32_t(c.r[i.rn]) : 0);
        if (i.acc && r != int32_t(r))
            c.q = 1;
        c.r[i.rd] = uint32_t(r);
        break;
    }
    case Op::Vfp:
        rc_vfp_data(&c, i.sub, i.dbl, i.vd, i.vn, i.vm);
        break;
    case Op::VfpMovRS:
        if (i.fx)
            c.r[i.rd] = c.s[i.vn];
        else
            c.s[i.vn] = c.r[i.rd];
        break;
    case Op::VfpMovRR: {
        const unsigned idx = i.dbl ? 2u * i.vm : i.vm;
        if (i.fx) {
            c.r[i.rd] = c.s[idx];
            c.r[i.rn] = c.s[idx + 1];
        } else {
            c.s[idx] = c.r[i.rd];
            c.s[idx + 1] = c.r[i.rn];
        }
        break;
    }
    case Op::VfpMrs: {
        const uint32_t v = i.imm == 1 ? c.fpscr : i.imm == 0 ? 0x410120b4u : i.imm == 8 ? 0x40000000u : 0;
        if (i.rd == 15) {
            c.n = v >> 31;
            c.z = (v >> 30) & 1;
            c.c = (v >> 29) & 1;
            c.v = (v >> 28) & 1;
        } else {
            c.r[i.rd] = v;
        }
        break;
    }
    case Op::VfpMsr:
        if (i.imm == 1)
            c.fpscr = (c.fpscr & ~RC_FPSCR_WMASK) | (c.r[i.rd] & RC_FPSCR_WMASK);
        break;
    case Op::VfpLdSt: {
        const uint32_t base = i.rn == 15 ? (i.pc + 8) & ~3u : c.r[i.rn];
        const uint32_t a = i.up ? base + i.imm : base - i.imm;
        const unsigned idx = i.dbl ? 2u * i.vd : i.vd;
        const unsigned words = i.dbl ? 2 : 1;
        for (unsigned k = 0; k < words; k++) {
            if (i.fx)
                c.s[idx + k] = m.r32(a + 4 * k);
            else
                m.w32(a + 4 * k, c.s[idx + k]);
        }
        break;
    }
    case Op::VfpLdmStm: {
        const uint32_t base = c.r[i.rn];
        const uint32_t start = i.up ? base : base - 4 * i.imm;
        const unsigned idx = i.dbl ? 2u * i.vd : i.vd;
        for (unsigned k = 0; k < i.imm; k++) {
            if (i.fx)
                c.s[idx + k] = m.r32(start + 4 * k);
            else
                m.w32(start + 4 * k, c.s[idx + k]);
        }
        if (i.wb)
            c.r[i.rn] = i.up ? base + 4 * i.imm : base - 4 * i.imm;
        break;
    }
    case Op::Svc:
        c.svc = i.imm;
        c.svc_pending = 1;
        break;
    case Op::Nop:
    case Op::Pld:
    case Op::Barrier:
        break;
    case Op::MrcTls:
        x.writeRd(i.rd, c.tls);
        break;
    case Op::Mrs:
        c.r[i.rd] = (c.n << 31) | (c.z << 30) | (c.c << 29) | (c.v << 28) | (c.q << 27) | (c.ge << 16);
        break;
    case Op::Msr: {
        const uint32_t v = i.immOperand ? i.imm : c.r[i.rm];
        const uint32_t mask = (i.word >> 16) & 0xF;
        if (mask & 8) {
            c.n = v >> 31;
            c.z = (v >> 30) & 1;
            c.c = (v >> 29) & 1;
            c.v = (v >> 28) & 1;
            c.q = (v >> 27) & 1;
        }
        if (mask & 4)
            c.ge = (v >> 16) & 0xF;
        break;
    }
    case Op::Bkpt:
    default:
        c.trap = 1;
        c.trap_pc = i.pc;
        c.r[15] = i.pc;
        break;
    }
}

} // namespace recomp
