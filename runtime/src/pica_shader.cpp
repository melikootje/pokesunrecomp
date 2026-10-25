#include "pokerecomp/pica_shader.h"

#include <cmath>
#include <tuple>
#include <vector>

namespace pokerecomp {
namespace pica {

namespace {

enum Op : uint32_t {
    ADD = 0x00,
    DP3 = 0x01,
    DP4 = 0x02,
    DPH = 0x03,
    DST = 0x04,
    EX2 = 0x05,
    LG2 = 0x06,
    LITP = 0x07,
    MUL = 0x08,
    SGE = 0x09,
    SLT = 0x0A,
    FLR = 0x0B,
    MAX = 0x0C,
    MIN = 0x0D,
    RCP = 0x0E,
    RSQ = 0x0F,
    MOVA = 0x12,
    MOV = 0x13,
    DPHI = 0x18,
    DSTI = 0x19,
    SGEI = 0x1A,
    SLTI = 0x1B,
    BREAK = 0x20,
    NOP = 0x21,
    END = 0x22,
    BREAKC = 0x23,
    CALL = 0x24,
    CALLC = 0x25,
    CALLU = 0x26,
    IFU = 0x27,
    IFC = 0x28,
    LOOP = 0x29,
    JMPC = 0x2C,
    JMPU = 0x2D,
    CMP0 = 0x2E,
    CMP1 = 0x2F,
};

float pmul(float a, float b) { // 0 * inf = 0 on the PICA (NaN * 0 stays NaN)
    if ((a == 0.0f && !std::isnan(b)) || (b == 0.0f && !std::isnan(a)))
        return 0.0f;
    return a * b;
}
float pmax(float a, float b) { return a > b ? a : b; } // GLSL: max(NaN, 0) = 0
float pmin(float a, float b) { return a < b ? a : b; } // min(0, NaN) = NaN

struct Frame {
    uint32_t final, ret, loopStart;
    uint32_t repeat;
    uint8_t incr;
    bool isLoop;
};

struct Machine {
    ShaderUnit &u;
    const Vec4 *in;
    Vec4 *out;
    Vec4 r[16];
    int32_t a0x = 0, a0y = 0, aL = 0;
    bool cmpx = false, cmpy = false;

    const Vec4 &readSrc(uint32_t idx, int offset, bool wide) const {
        static const Vec4 zero, one = {{1, 1, 1, 1}};
        if (idx < 0x10)
            return in[idx];
        if (idx < 0x20)
            return r[idx - 0x10];
        if (!wide)
            return zero;
        // Constant registers: the relative offset is ignored outside [-128, 127], the sum with the constant
        // number wraps at 7 bits and constants above 95 read as (1, 1, 1, 1).
        int32_t ci = int32_t(idx) - 0x20;
        if (offset >= -128 && offset <= 127)
            ci += offset;
        ci &= 0x7F;
        if (ci > 95)
            return one;
        return u.fconst[ci];
    }
    int offsetFor(uint32_t idxSel) const {
        switch (idxSel) {
        case 1:
            return a0x;
        case 2:
            return a0y;
        case 3:
            return aL;
        default:
            return 0;
        }
    }
    Vec4 swz(const Vec4 &s, uint32_t sel, bool neg) const {
        Vec4 o;
        for (int c = 0; c < 4; c++) {
            const uint32_t k = (sel >> (6 - 2 * c)) & 3;
            o[c] = neg ? -s[int(k)] : s[int(k)];
        }
        return o;
    }
    void write(uint32_t dst, uint32_t mask, const Vec4 &v) {
        Vec4 *d = dst < 0x10 ? &out[dst] : &r[dst - 0x10];
        for (int c = 0; c < 4; c++)
            if (mask & (8 >> c))
                (*d)[c] = v[c];
    }
    bool cond(uint32_t condop, bool refx, bool refy) const {
        switch (condop) {
        case 0:
            return cmpx == refx || cmpy == refy;
        case 1:
            return cmpx == refx && cmpy == refy;
        case 2:
            return cmpx == refx;
        default:
            return cmpy == refy;
        }
    }
    static bool compare(uint32_t op, float a, float b) {
        switch (op) {
        case 0:
            return a == b;
        case 1:
            return a != b;
        case 2:
            return a < b;
        case 3:
            return a <= b;
        case 4:
            return a > b;
        case 5:
            return a >= b;
        default:
            return true;
        }
    }
};

} // namespace

bool ShaderUnit::run(const Vec4 in[16], Vec4 out[16]) {
    Machine m{*this, in, out, {}, 0, 0, 0, false, false};
    std::vector<Frame> stack;
    uint32_t pc = entry;
    uint64_t budget = 100000;
    for (;;) {
        if (pc >= kShaderCodeSize || budget-- == 0)
            return false;
        const uint32_t w = code[pc];
        const uint32_t opc = w >> 26;
        executed++;
        uint32_t next = pc + 1;
        bool done = false;

        auto fmt1 = [&](bool inverted, bool unary) {
            const uint32_t desc = opdesc[w & 0x7F];
            const uint32_t s2f = inverted ? ((w >> 7) & 0x7F) : ((w >> 7) & 0x1F);
            const uint32_t s1f = inverted ? ((w >> 14) & 0x1F) : ((w >> 12) & 0x7F);
            const uint32_t idx = (w >> 19) & 3;
            const int off = m.offsetFor(idx);
            // The relative index applies to the operand that is wide enough to address constants.
            const Vec4 a =
                m.swz(m.readSrc(s1f, inverted ? 0 : off, !inverted), (desc >> 5) & 0xFF, (desc >> 4) & 1);
            Vec4 b;
            if (!unary)
                b = m.swz(m.readSrc(s2f, inverted ? off : 0, inverted), (desc >> 14) & 0xFF,
                          (desc >> 13) & 1);
            return std::make_tuple(a, b, desc & 0xF, (w >> 21) & 0x1F);
        };

        switch (opc) {
        case ADD:
        case MUL:
        case MAX:
        case MIN:
        case SGE:
        case SLT:
        case SGEI:
        case SLTI: {
            const bool inv = opc == SGEI || opc == SLTI;
            auto [a, b, mask, dst] = fmt1(inv, false);
            Vec4 r;
            for (int c = 0; c < 4; c++) {
                switch (opc) {
                case ADD:
                    r[c] = a[c] + b[c];
                    break;
                case MUL:
                    r[c] = pmul(a[c], b[c]);
                    break;
                case MAX:
                    r[c] = pmax(a[c], b[c]);
                    break;
                case MIN:
                    r[c] = pmin(a[c], b[c]);
                    break;
                case SGE:
                case SGEI:
                    r[c] = a[c] >= b[c] ? 1.0f : 0.0f;
                    break;
                default:
                    r[c] = a[c] < b[c] ? 1.0f : 0.0f;
                    break;
                }
            }
            m.write(dst, mask, r);
            break;
        }
        case DP3:
        case DP4:
        case DPH:
        case DPHI: {
            auto [a, b, mask, dst] = fmt1(opc == DPHI, false);
            float d = pmul(a[0], b[0]) + pmul(a[1], b[1]) + pmul(a[2], b[2]);
            d += (opc == DP4) ? pmul(a[3], b[3]) : (opc == DP3 ? 0.0f : b[3]);
            m.write(dst, mask, Vec4{{d, d, d, d}});
            break;
        }
        case DST:
        case DSTI: {
            auto [a, b, mask, dst] = fmt1(opc == DSTI, false);
            m.write(dst, mask, Vec4{{1.0f, pmul(a[1], b[1]), a[2], b[3]}});
            break;
        }
        case EX2:
        case LG2:
        case RCP:
        case RSQ: {
            auto [a, b, mask, dst] = fmt1(false, true);
            (void)b;
            float r;
            if (opc == EX2)
                r = std::exp2(a[0]);
            else if (opc == LG2)
                r = std::log2(a[0]);
            else if (opc == RCP)
                r = 1.0f / a[0];
            else
                r = 1.0f / std::sqrt(a[0]);
            m.write(dst, mask, Vec4{{r, r, r, r}});
            break;
        }
        case FLR:
        case MOV: {
            auto [a, b, mask, dst] = fmt1(false, true);
            (void)b;
            Vec4 r = a;
            if (opc == FLR)
                for (int c = 0; c < 4; c++)
                    r[c] = std::floor(a[c]);
            m.write(dst, mask, r);
            break;
        }
        case MOVA: {
            auto [a, b, mask, dst] = fmt1(false, true);
            (void)b;
            (void)dst;
            if (mask & 8)
                m.a0x = int32_t(a[0]);
            if (mask & 4)
                m.a0y = int32_t(a[1]);
            break;
        }
        case LITP: { // partial lighting term (D3D lit without the exponent): not needed by the game so far
            auto [a, b, mask, dst] = fmt1(false, true);
            (void)b;
            Vec4 r = {{a[0] > 0 ? a[0] : 0.0f, a[1] > 0 ? a[1] : 0.0f, 0.0f, 1.0f}};
            m.cmpx = a[0] > 0;
            m.cmpy = a[1] > 0;
            m.write(dst, mask, r);
            break;
        }
        case CMP0:
        case CMP1: {
            const uint32_t desc = opdesc[w & 0x7F];
            const uint32_t s1 = (w >> 12) & 0x7F, s2 = (w >> 7) & 0x1F, idx = (w >> 19) & 3;
            const Vec4 a = m.swz(m.readSrc(s1, m.offsetFor(idx), true), (desc >> 5) & 0xFF, (desc >> 4) & 1);
            const Vec4 b = m.swz(m.readSrc(s2, 0, false), (desc >> 14) & 0xFF, (desc >> 13) & 1);
            m.cmpx = Machine::compare((w >> 24) & 7, a[0], b[0]);
            m.cmpy = Machine::compare((w >> 21) & 7, a[1], b[1]);
            break;
        }
        case NOP:
            break;
        case END:
            done = true;
            break;
        case BREAK:
        case BREAKC: {
            if (opc == BREAKC && !m.cond((w >> 22) & 3, (w >> 25) & 1, (w >> 24) & 1))
                break;
            // leave the innermost loop
            while (!stack.empty() && !stack.back().isLoop)
                stack.pop_back();
            if (stack.empty())
                return false;
            next = stack.back().ret;
            stack.pop_back();
            break;
        }
        case CALL:
        case CALLC:
        case CALLU: {
            const uint32_t dst = (w >> 10) & 0xFFF, num = w & 0xFF;
            bool take = true;
            if (opc == CALLC)
                take = m.cond((w >> 22) & 3, (w >> 25) & 1, (w >> 24) & 1);
            if (opc == CALLU)
                take = bconst[(w >> 22) & 0xF];
            if (take) {
                if (stack.size() >= 16)
                    return false;
                stack.push_back({dst + num, pc + 1, 0, 0, 0, false});
                next = dst;
            }
            break;
        }
        case IFU:
        case IFC: {
            const uint32_t dst = (w >> 10) & 0xFFF, num = w & 0xFF;
            const bool take =
                opc == IFU ? bconst[(w >> 22) & 0xF] : m.cond((w >> 22) & 3, (w >> 25) & 1, (w >> 24) & 1);
            if (stack.size() >= 16)
                return false;
            if (take) {
                stack.push_back(
                    {dst, dst + num, 0, 0, 0, false}); // run the then-block, then skip the else-block
            } else {
                next = dst;
                if (num)
                    stack.push_back({dst + num, dst + num, 0, 0, 0, false});
            }
            break;
        }
        case LOOP: {
            const uint32_t dst = (w >> 10) & 0xFFF, id = (w >> 22) & 3;
            if (stack.size() >= 16)
                return false;
            m.aL = iconst[id][1];
            stack.push_back({dst + 1, dst + 1, pc + 1, iconst[id][0], iconst[id][2], true});
            break;
        }
        case JMPC:
        case JMPU: {
            const uint32_t dst = (w >> 10) & 0xFFF;
            bool take;
            if (opc == JMPC)
                take = m.cond((w >> 22) & 3, (w >> 25) & 1, (w >> 24) & 1);
            else
                take = bconst[(w >> 22) & 0xF] == !(w & 1);
            if (take)
                next = dst;
            break;
        }
        default:
            if (opc >= 0x38 || (opc >= 0x30 && opc < 0x38)) { // MAD / MADI
                const bool inv = opc < 0x38;
                const uint32_t desc = opdesc[w & 0x1F];
                const uint32_t s3 = inv ? ((w >> 5) & 0x7F) : ((w >> 5) & 0x1F);
                const uint32_t s2 = inv ? ((w >> 12) & 0x1F) : ((w >> 10) & 0x7F);
                const uint32_t s1 = (w >> 17) & 0x1F, idx = (w >> 22) & 3, dst = (w >> 24) & 0x1F;
                const int off = m.offsetFor(idx);
                const Vec4 a = m.swz(m.readSrc(s1, 0, false), (desc >> 5) & 0xFF, (desc >> 4) & 1);
                const Vec4 b =
                    m.swz(m.readSrc(s2, inv ? 0 : off, !inv), (desc >> 14) & 0xFF, (desc >> 13) & 1);
                const Vec4 c =
                    m.swz(m.readSrc(s3, inv ? off : 0, inv), (desc >> 23) & 0xFF, (desc >> 22) & 1);
                Vec4 r;
                for (int k = 0; k < 4; k++)
                    r[k] = pmul(a[k], b[k]) + c[k];
                m.write(dst, desc & 0xF, r);
            } else {
                return false; // opcode not implemented
            }
            break;
        }
        if (done)
            return true;
        pc = next;
        // Block ends: pop frames whose final address was reached (loops iterate first).
        while (!stack.empty() && pc == stack.back().final) {
            Frame &f = stack.back();
            if (f.isLoop && f.repeat > 0) {
                f.repeat--;
                m.aL += f.incr;
                pc = f.loopStart;
                break;
            }
            pc = f.ret;
            stack.pop_back();
        }
    }
}

} // namespace pica
} // namespace pokerecomp
