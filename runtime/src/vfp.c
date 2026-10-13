/* ARM11 VFPv2 soft-float with FPSCR semantics. Original implementation; see vfp.h. */
#include "pokerecomp/vfp.h"

#include <string.h>

typedef unsigned __int128 u128;

typedef struct {
    int ebits, mbits, bias;
} Fmt;
static const Fmt F32 = {8, 23, 127};
static const Fmt F64 = {11, 52, 1023};

enum { C_ZERO, C_NORM, C_INF, C_QNAN, C_SNAN };

typedef struct {
    int cls;
    uint32_t sign;
    int32_t exp; /* exponent of the LSB of sig: value = sig * 2^exp; sig is normalised (bit mbits set) */
    uint64_t sig;
} Unp;

static int clz128(u128 v) {
    const uint64_t hi = (uint64_t)(v >> 64), lo = (uint64_t)v;
    if (hi)
        return __builtin_clzll(hi);
    return 64 + __builtin_clzll(lo);
}

static uint64_t fmt_bits(const Fmt *f, uint64_t sign, uint64_t be, uint64_t frac) {
    return (sign << (f->ebits + f->mbits)) | (be << f->mbits) | frac;
}
static uint64_t inf_bits(const Fmt *f, uint32_t sign) { return fmt_bits(f, sign, (1u << f->ebits) - 1, 0); }
static uint64_t default_nan(const Fmt *f) {
    return fmt_bits(f, 0, (1u << f->ebits) - 1, 1ull << (f->mbits - 1));
}
static uint64_t sign_bit(const Fmt *f) { return 1ull << (f->ebits + f->mbits); }

static Unp unpack(const Fmt *f, uint64_t x) {
    Unp u;
    memset(&u, 0, sizeof u);
    const uint64_t frac = x & ((1ull << f->mbits) - 1);
    const uint32_t be = (uint32_t)((x >> f->mbits) & ((1u << f->ebits) - 1));
    u.sign = (uint32_t)(x >> (f->ebits + f->mbits)) & 1;
    if (be == (1u << f->ebits) - 1) {
        if (frac == 0)
            u.cls = C_INF;
        else
            u.cls = ((frac >> (f->mbits - 1)) & 1) ? C_QNAN : C_SNAN;
        return u;
    }
    if (be == 0) {
        if (frac == 0) {
            u.cls = C_ZERO;
            return u;
        }
        const int shift = __builtin_clzll(frac) - (63 - f->mbits);
        u.cls = C_NORM;
        u.sig = frac << shift;
        u.exp = (1 - f->bias - f->mbits) - shift;
        return u;
    }
    u.cls = C_NORM;
    u.sig = frac | (1ull << f->mbits);
    u.exp = (int32_t)be - f->bias - f->mbits;
    return u;
}

/* Rounds sig * 2^exp2 (plus a sticky "slightly more" bit) to the format using FPSCR rounding mode, FZ and
 * the cumulative exception flags. Tininess is detected before rounding. */
static uint64_t round_pack(const Fmt *f, uint32_t *fp, uint32_t sign, int32_t exp2, u128 sig, int sticky) {
    if (sig == 0)
        return fmt_bits(f, sign, 0, 0);
    const int mode = (int)((*fp >> 22) & 3); /* 0 RN, 1 RP, 2 RM, 3 RZ */
    const int msb = 127 - clz128(sig);
    const int32_t E = exp2 + msb;
    const int32_t emin = 1 - f->bias, emax = f->bias;
    const int tiny = E < emin;
    if (tiny && (*fp & RC_FPSCR_FZ)) {
        *fp |= RC_FPSCR_UFC;
        return fmt_bits(f, sign, 0, 0);
    }
    const int32_t lsbExp = tiny ? emin - f->mbits : E - f->mbits;
    const int32_t shift = lsbExp - exp2; /* >0: bits dropped */
    u128 q;
    int above = 0, equal = 0, inexact = sticky != 0;
    if (shift <= 0) {
        q = sig << (-shift);
    } else if (shift >= 129) {
        q = 0;
        inexact = 1;
    } else {
        u128 rem;
        u128 half;
        if (shift == 128) {
            q = 0;
            rem = sig;
            half = (u128)1 << 127;
        } else {
            q = sig >> shift;
            rem = sig & (((u128)1 << shift) - 1);
            half = (u128)1 << (shift - 1);
        }
        if (rem != 0)
            inexact = 1;
        above = rem > half || (rem == half && sticky);
        equal = rem == half && !sticky;
    }
    int up = 0;
    switch (mode) {
    case 0:
        up = above || (equal && (q & 1));
        break;
    case 1:
        up = !sign && inexact;
        break;
    case 2:
        up = sign && inexact;
        break;
    default:
        up = 0;
        break;
    }
    q += (u128)up;
    if (inexact)
        *fp |= RC_FPSCR_IXC;
    if (tiny && inexact)
        *fp |= RC_FPSCR_UFC;
    uint64_t qq = (uint64_t)q;
    if (qq == 0)
        return fmt_bits(f, sign, 0, 0);
    const int qmsb = 63 - __builtin_clzll(qq);
    const int32_t Eres = lsbExp + qmsb;
    if (Eres > emax) {
        *fp |= RC_FPSCR_OFC | RC_FPSCR_IXC;
        const int toInf = mode == 0 || (mode == 1 && !sign) || (mode == 2 && sign);
        if (toInf)
            return inf_bits(f, sign);
        return fmt_bits(f, sign, (1u << f->ebits) - 2, (1ull << f->mbits) - 1);
    }
    if (Eres < emin)
        return fmt_bits(f, sign, 0, qq); /* denormal: q counts units of 2^(emin-mbits) */
    const int sh = f->mbits - qmsb;
    qq = sh >= 0 ? (qq << sh) : (qq >> -sh);
    return fmt_bits(f, sign, (uint64_t)(Eres + f->bias), qq & ((1ull << f->mbits) - 1));
}

/* Flushes a denormal input to zero when FZ is set (sets IDC). */
static uint64_t flush_in(const Fmt *f, uint32_t *fp, uint64_t x) {
    if (!(*fp & RC_FPSCR_FZ))
        return x;
    const uint64_t frac = x & ((1ull << f->mbits) - 1);
    const uint32_t be = (uint32_t)((x >> f->mbits) & ((1u << f->ebits) - 1));
    if (be == 0 && frac != 0) {
        *fp |= RC_FPSCR_IDC;
        return x & sign_bit(f);
    }
    return x;
}

/* ARM FPProcessNaNs: returns 1 and the result when a NaN operand decides the outcome. */
static int process_nans(const Fmt *f, uint32_t *fp, uint64_t a, uint64_t b, int two, uint64_t *res) {
    const Unp ua = unpack(f, a);
    Unp ub;
    memset(&ub, 0, sizeof ub);
    if (two)
        ub = unpack(f, b);
    const int sa = ua.cls == C_SNAN, sb = two && ub.cls == C_SNAN;
    const int qa = ua.cls == C_QNAN, qb = two && ub.cls == C_QNAN;
    if (!(sa || sb || qa || qb))
        return 0;
    if (sa || sb)
        *fp |= RC_FPSCR_IOC;
    if (*fp & RC_FPSCR_DN)
        *res = default_nan(f);
    else if (sa)
        *res = a | (1ull << (f->mbits - 1));
    else if (sb)
        *res = b | (1ull << (f->mbits - 1));
    else if (qa)
        *res = a;
    else
        *res = b;
    return 1;
}

static uint64_t zero_bits(const Fmt *f, uint32_t sign) { return fmt_bits(f, sign, 0, 0); }

static uint64_t fp_add(const Fmt *f, uint32_t *fp, uint64_t a, uint64_t b, int negateB) {
    a = flush_in(f, fp, a);
    b = flush_in(f, fp, b);
    uint64_t r;
    if (process_nans(f, fp, a, b, 1, &r))
        return r;
    if (negateB)
        b ^= sign_bit(f);
    const Unp ua = unpack(f, a), ub = unpack(f, b);
    if (ua.cls == C_INF || ub.cls == C_INF) {
        if (ua.cls == C_INF && ub.cls == C_INF && ua.sign != ub.sign) {
            *fp |= RC_FPSCR_IOC;
            return default_nan(f);
        }
        return ua.cls == C_INF ? a : b;
    }
    const int rm = (int)((*fp >> 22) & 3);
    if (ua.cls == C_ZERO && ub.cls == C_ZERO)
        return zero_bits(f, ua.sign == ub.sign ? ua.sign : (rm == 2));
    if (ua.cls == C_ZERO)
        return b;
    if (ub.cls == C_ZERO)
        return a;
    Unp L = ua, S = ub;
    if (ub.exp > ua.exp || (ub.exp == ua.exp && ub.sig > ua.sig)) {
        L = ub;
        S = ua;
    }
    const int32_t d = L.exp - S.exp;
    u128 sigL, sigS = 0;
    int32_t exp2;
    int sticky = 0;
    if (d <= 64) {
        sigL = (u128)L.sig << d;
        sigS = S.sig;
        exp2 = S.exp;
    } else {
        sigL = (u128)L.sig << 64;
        exp2 = L.exp - 64;
        sticky = 1;
    }
    u128 res;
    const int sub = L.sign != S.sign;
    if (!sub)
        res = sigL + sigS;
    else if (sticky)
        res = sigL - 1;
    else
        res = sigL - sigS; /* L has the larger magnitude, so no borrow */
    if (res == 0 && !sticky)
        return zero_bits(f, rm == 2);
    return round_pack(f, fp, L.sign, exp2, res, sticky);
}

static uint64_t fp_mul(const Fmt *f, uint32_t *fp, uint64_t a, uint64_t b) {
    a = flush_in(f, fp, a);
    b = flush_in(f, fp, b);
    uint64_t r;
    if (process_nans(f, fp, a, b, 1, &r))
        return r;
    const Unp ua = unpack(f, a), ub = unpack(f, b);
    const uint32_t sign = ua.sign ^ ub.sign;
    if ((ua.cls == C_INF && ub.cls == C_ZERO) || (ua.cls == C_ZERO && ub.cls == C_INF)) {
        *fp |= RC_FPSCR_IOC;
        return default_nan(f);
    }
    if (ua.cls == C_INF || ub.cls == C_INF)
        return inf_bits(f, sign);
    if (ua.cls == C_ZERO || ub.cls == C_ZERO)
        return zero_bits(f, sign);
    return round_pack(f, fp, sign, ua.exp + ub.exp, (u128)ua.sig * ub.sig, 0);
}

static uint64_t fp_div(const Fmt *f, uint32_t *fp, uint64_t a, uint64_t b) {
    a = flush_in(f, fp, a);
    b = flush_in(f, fp, b);
    uint64_t r;
    if (process_nans(f, fp, a, b, 1, &r))
        return r;
    const Unp ua = unpack(f, a), ub = unpack(f, b);
    const uint32_t sign = ua.sign ^ ub.sign;
    if ((ua.cls == C_ZERO && ub.cls == C_ZERO) || (ua.cls == C_INF && ub.cls == C_INF)) {
        *fp |= RC_FPSCR_IOC;
        return default_nan(f);
    }
    if (ua.cls == C_INF)
        return inf_bits(f, sign);
    if (ub.cls == C_INF)
        return zero_bits(f, sign);
    if (ub.cls == C_ZERO) {
        *fp |= RC_FPSCR_DZC;
        return inf_bits(f, sign);
    }
    if (ua.cls == C_ZERO)
        return zero_bits(f, sign);
    const int k = 62;
    const u128 num = (u128)ua.sig << k;
    const u128 q = num / ub.sig;
    const u128 rem = num % ub.sig;
    return round_pack(f, fp, sign, ua.exp - ub.exp - k, q, rem != 0);
}

static u128 isqrt128(u128 n, int *exact) {
    u128 res = 0, bit = (u128)1 << 126;
    while (bit > n)
        bit >>= 2;
    u128 x = n;
    while (bit) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    *exact = x == 0;
    return res;
}

static uint64_t fp_sqrt(const Fmt *f, uint32_t *fp, uint64_t a) {
    a = flush_in(f, fp, a);
    uint64_t r;
    if (process_nans(f, fp, a, 0, 0, &r))
        return r;
    const Unp ua = unpack(f, a);
    if (ua.cls == C_ZERO)
        return a;
    if (ua.sign) {
        *fp |= RC_FPSCR_IOC;
        return default_nan(f);
    }
    if (ua.cls == C_INF)
        return a;
    u128 m = ua.sig;
    int32_t e = ua.exp;
    if (e & 1) {
        m <<= 1;
        e -= 1;
    }
    const int t = (126 - (f->mbits + 2)) / 2;
    int exact;
    const u128 s = isqrt128(m << (2 * t), &exact);
    return round_pack(f, fp, 0, e / 2 - t, s, !exact);
}

static uint64_t cvt_f32_to_f64(uint32_t *fp, uint64_t x) {
    x = flush_in(&F32, fp, x);
    const Unp u = unpack(&F32, x);
    if (u.cls == C_SNAN || u.cls == C_QNAN) {
        if (u.cls == C_SNAN)
            *fp |= RC_FPSCR_IOC;
        if (*fp & RC_FPSCR_DN)
            return default_nan(&F64);
        return fmt_bits(&F64, u.sign, 0x7FF, (1ull << 51) | ((x & 0x3FFFFF) << 29));
    }
    if (u.cls == C_INF)
        return inf_bits(&F64, u.sign);
    if (u.cls == C_ZERO)
        return zero_bits(&F64, u.sign);
    return round_pack(&F64, fp, u.sign, u.exp, u.sig, 0);
}

static uint64_t cvt_f64_to_f32(uint32_t *fp, uint64_t x) {
    x = flush_in(&F64, fp, x);
    const Unp u = unpack(&F64, x);
    if (u.cls == C_SNAN || u.cls == C_QNAN) {
        if (u.cls == C_SNAN)
            *fp |= RC_FPSCR_IOC;
        if (*fp & RC_FPSCR_DN)
            return default_nan(&F32);
        return fmt_bits(&F32, u.sign, 0xFF, (1ull << 22) | ((x >> 29) & 0x3FFFFF));
    }
    if (u.cls == C_INF)
        return inf_bits(&F32, u.sign);
    if (u.cls == C_ZERO)
        return zero_bits(&F32, u.sign);
    return round_pack(&F32, fp, u.sign, u.exp, u.sig, 0);
}

static uint64_t cvt_int_to_fp(const Fmt *f, uint32_t *fp, uint32_t v, int isSigned) {
    uint32_t sign = 0;
    uint64_t mag = v;
    if (isSigned && (int32_t)v < 0) {
        sign = 1;
        mag = (uint64_t)(-(int64_t)(int32_t)v);
    }
    if (mag == 0)
        return zero_bits(f, 0);
    return round_pack(f, fp, sign, 0, mag, 0);
}

static uint32_t cvt_fp_to_int(const Fmt *f, uint32_t *fp, uint64_t x, int isSigned, int mode) {
    x = flush_in(f, fp, x);
    const Unp u = unpack(f, x);
    if (u.cls == C_QNAN || u.cls == C_SNAN) {
        *fp |= RC_FPSCR_IOC;
        return 0;
    }
    const uint32_t lo = isSigned ? 0x80000000u : 0u, hi = isSigned ? 0x7FFFFFFFu : 0xFFFFFFFFu;
    if (u.cls == C_INF) {
        *fp |= RC_FPSCR_IOC;
        return u.sign ? lo : hi;
    }
    if (u.cls == C_ZERO)
        return 0;
    u128 ip;
    int inexact = 0, up = 0;
    if (u.exp >= 0) {
        if (u.exp > 40) {
            *fp |= RC_FPSCR_IOC;
            return u.sign ? lo : hi;
        }
        ip = (u128)u.sig << u.exp;
    } else {
        const int sh = -u.exp;
        if (sh >= 126) {
            ip = 0;
            inexact = 1;
            up = (mode == 1 && !u.sign) || (mode == 2 && u.sign);
        } else {
            ip = (u128)u.sig >> sh;
            const u128 rem = (u128)u.sig & (((u128)1 << sh) - 1);
            const u128 half = (u128)1 << (sh - 1);
            inexact = rem != 0;
            switch (mode) {
            case 0:
                up = rem > half || (rem == half && (ip & 1));
                break;
            case 1:
                up = !u.sign && inexact;
                break;
            case 2:
                up = u.sign && inexact;
                break;
            default:
                up = 0;
                break;
            }
        }
        ip += (u128)up;
    }
    uint32_t out;
    if (isSigned) {
        const int overflow = u.sign ? ip > 0x80000000u : ip > 0x7FFFFFFFu;
        if (overflow) {
            *fp |= RC_FPSCR_IOC;
            return u.sign ? lo : hi;
        }
        out = u.sign ? (uint32_t)(-(int64_t)ip) : (uint32_t)ip;
    } else {
        if (u.sign && ip != 0) {
            *fp |= RC_FPSCR_IOC;
            return 0;
        }
        if (ip > 0xFFFFFFFFu) {
            *fp |= RC_FPSCR_IOC;
            return hi;
        }
        out = (uint32_t)ip;
    }
    if (inexact)
        *fp |= RC_FPSCR_IXC;
    return out;
}

static void fp_compare(const Fmt *f, uint32_t *fp, uint64_t a, uint64_t b, int quietNan) {
    a = flush_in(f, fp, a);
    b = flush_in(f, fp, b);
    const Unp ua = unpack(f, a), ub = unpack(f, b);
    uint32_t flags;
    if (ua.cls >= C_QNAN || ub.cls >= C_QNAN) {
        flags = RC_FPSCR_C | RC_FPSCR_V;
        if (ua.cls == C_SNAN || ub.cls == C_SNAN || !quietNan)
            *fp |= RC_FPSCR_IOC;
    } else {
        int cmp;
        if (ua.cls == C_ZERO && ub.cls == C_ZERO) {
            cmp = 0;
        } else {
            const uint64_t ma = a & ~sign_bit(f), mb = b & ~sign_bit(f);
            const int64_t ka = ua.cls == C_ZERO ? 0 : (ua.sign ? -(int64_t)ma : (int64_t)ma);
            const int64_t kb = ub.cls == C_ZERO ? 0 : (ub.sign ? -(int64_t)mb : (int64_t)mb);
            cmp = ka < kb ? -1 : (ka > kb ? 1 : 0);
        }
        flags = cmp == 0 ? (RC_FPSCR_Z | RC_FPSCR_C) : (cmp < 0 ? RC_FPSCR_N : RC_FPSCR_C);
    }
    *fp = (*fp & 0x0FFFFFFFu) | flags;
}

static uint64_t get_s(const RcCtx *c, unsigned r) { return c->s[r & 31]; }
static void set_s(RcCtx *c, unsigned r, uint64_t v) { c->s[r & 31] = (uint32_t)v; }
static uint64_t get_d(const RcCtx *c, unsigned r) {
    return ((uint64_t)c->s[(2 * r + 1) & 31] << 32) | c->s[(2 * r) & 31];
}
static void set_d(RcCtx *c, unsigned r, uint64_t v) {
    c->s[(2 * r) & 31] = (uint32_t)v;
    c->s[(2 * r + 1) & 31] = (uint32_t)(v >> 32);
}
static uint64_t rd(const RcCtx *c, unsigned dbl, unsigned r) { return dbl ? get_d(c, r) : get_s(c, r); }
static void wr(RcCtx *c, unsigned dbl, unsigned r, uint64_t v) {
    if (dbl)
        set_d(c, r, v);
    else
        set_s(c, r, v);
}

static uint64_t scalar_op(const Fmt *f, uint32_t *fp, unsigned op, uint64_t d, uint64_t n, uint64_t m) {
    switch (op) {
    case RC_V_MLA:
        return fp_add(f, fp, d, fp_mul(f, fp, n, m), 0);
    case RC_V_MLS:
        return fp_add(f, fp, d, fp_mul(f, fp, n, m) ^ sign_bit(f), 0);
    case RC_V_NMLS:
        return fp_add(f, fp, d ^ sign_bit(f), fp_mul(f, fp, n, m), 0);
    case RC_V_NMLA:
        return fp_add(f, fp, d ^ sign_bit(f), fp_mul(f, fp, n, m) ^ sign_bit(f), 0);
    case RC_V_MUL:
        return fp_mul(f, fp, n, m);
    case RC_V_NMUL:
        return fp_mul(f, fp, n, m) ^ sign_bit(f);
    case RC_V_ADD:
        return fp_add(f, fp, n, m, 0);
    case RC_V_SUB:
        return fp_add(f, fp, n, m, 1);
    case RC_V_DIV:
        return fp_div(f, fp, n, m);
    case RC_V_MOV:
        return m;
    case RC_V_ABS:
        return m & ~sign_bit(f);
    case RC_V_NEG:
        return m ^ sign_bit(f);
    case RC_V_SQRT:
        return fp_sqrt(f, fp, m);
    default:
        return 0;
    }
}

void rc_vfp_data(RcCtx *c, unsigned op, unsigned dbl, unsigned vd, unsigned vn, unsigned vm) {
    uint32_t *fp = &c->fpscr;
    const Fmt *f = dbl ? &F64 : &F32;

    if (op >= RC_V_CMP && op <= RC_V_CMPEZ) {
        const uint64_t a = rd(c, dbl, vd);
        const uint64_t b = (op == RC_V_CMPZ || op == RC_V_CMPEZ) ? 0 : rd(c, dbl, vm);
        fp_compare(f, fp, a, b, op == RC_V_CMP || op == RC_V_CMPZ);
        return;
    }
    if (op >= RC_V_CVT_DS) {
        switch (op) {
        case RC_V_CVT_DS:
            if (dbl)
                set_s(c, vd, cvt_f64_to_f32(fp, get_d(c, vm)));
            else
                set_d(c, vd, cvt_f32_to_f64(fp, get_s(c, vm)));
            break;
        case RC_V_CVT_U2F:
            wr(c, dbl, vd, cvt_int_to_fp(f, fp, c->s[vm & 31], 0));
            break;
        case RC_V_CVT_S2F:
            wr(c, dbl, vd, cvt_int_to_fp(f, fp, c->s[vm & 31], 1));
            break;
        default: {
            const int sg = op == RC_V_CVT_F2S || op == RC_V_CVT_F2S_RZ;
            const int mode = (op == RC_V_CVT_F2U_RZ || op == RC_V_CVT_F2S_RZ) ? 3 : (int)((*fp >> 22) & 3);
            c->s[vd & 31] = cvt_fp_to_int(f, fp, rd(c, dbl, vm), sg, mode);
            break;
        }
        }
        return;
    }

    const unsigned len = (*fp >> 16) & 7;
    const unsigned stride = ((*fp >> 20) & 3) == 3 ? 2 : 1;
    const unsigned bankMask = dbl ? 0xC : 0x18;
    const unsigned bankSize = dbl ? 4 : 8;
    unsigned count = 1, dD = 0, dN = 0, dM = 0;
    const int unary = op >= RC_V_MOV;
    if (len != 0 && (vd & bankMask) != 0) {
        count = len + 1;
        dD = stride;
        dN = unary ? 0 : stride;
        dM = (vm & bankMask) == 0 ? 0 : stride;
    }
    for (unsigned i = 0; i < count; i++) {
        const uint64_t d = rd(c, dbl, vd), n = unary ? 0 : rd(c, dbl, vn), m = rd(c, dbl, vm);
        wr(c, dbl, vd, scalar_op(f, fp, op, d, n, m));
        vd = (vd & ~(bankSize - 1)) | ((vd + dD) & (bankSize - 1));
        vn = (vn & ~(bankSize - 1)) | ((vn + dN) & (bankSize - 1));
        vm = (vm & ~(bankSize - 1)) | ((vm + dM) & (bankSize - 1));
    }
}
