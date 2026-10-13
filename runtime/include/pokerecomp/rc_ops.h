/* Helpers used by generated (recompiled) code. Plain C. Semantics mirror ARM11 (ARMv6K). */
#pragma once
#include <stdint.h>

static inline uint32_t rc_ror(uint32_t v, uint32_t n) {
    n &= 31u;
    return n ? (v >> n) | (v << (32u - n)) : v;
}

/* Register-specified shifts: `amt` is Rs & 0xFF. *carry is updated only when amt != 0 (else unchanged). */
static inline uint32_t rc_shl_reg(uint32_t v, uint32_t amt, uint32_t *carry) {
    if (amt == 0)
        return v;
    if (amt < 32) {
        *carry = (v >> (32u - amt)) & 1u;
        return v << amt;
    }
    *carry = (amt == 32) ? (v & 1u) : 0u;
    return 0;
}
static inline uint32_t rc_shr_reg(uint32_t v, uint32_t amt, uint32_t *carry) {
    if (amt == 0)
        return v;
    if (amt < 32) {
        *carry = (v >> (amt - 1u)) & 1u;
        return v >> amt;
    }
    *carry = (amt == 32) ? (v >> 31) : 0u;
    return 0;
}
static inline uint32_t rc_asr_reg(uint32_t v, uint32_t amt, uint32_t *carry) {
    if (amt == 0)
        return v;
    if (amt < 32) {
        *carry = (v >> (amt - 1u)) & 1u;
        return (uint32_t)((int32_t)v >> amt);
    }
    *carry = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t rc_ror_reg(uint32_t v, uint32_t amt, uint32_t *carry) {
    if (amt == 0)
        return v;
    if ((amt & 31u) == 0) {
        *carry = v >> 31;
        return v;
    }
    v = rc_ror(v, amt & 31u);
    *carry = v >> 31;
    return v;
}

/* ---- ARMv6 media / ARMv5E DSP helpers (lane-wise, constant-folded when kind/op are constants) ---- */
static inline int64_t rc_sx(uint32_t v, unsigned bits) {
    return (int64_t)(int32_t)(v << (32u - bits)) >> (32u - bits);
}

static inline uint32_t rc_ssat(int64_t v, unsigned n, uint32_t *q) {
    const int64_t hi = ((int64_t)1 << (n - 1)) - 1, lo = -((int64_t)1 << (n - 1));
    if (v > hi) {
        *q = 1;
        return (uint32_t)hi;
    }
    if (v < lo) {
        *q = 1;
        return (uint32_t)lo;
    }
    return (uint32_t)v;
}
static inline uint32_t rc_usat(int64_t v, unsigned n, uint32_t *q) {
    const int64_t hi = ((int64_t)1 << n) - 1;
    if (v > hi) {
        *q = 1;
        return (uint32_t)hi;
    }
    if (v < 0) {
        *q = 1;
        return 0;
    }
    return (uint32_t)v;
}

/* kind: 0 S, 1 Q, 2 SH, 3 U, 4 UQ, 5 UH.  op: 0 ADD16, 1 ASX, 2 SAX, 3 SUB16, 4 ADD8, 5 SUB8. */
static inline uint32_t rc_par(unsigned kind, unsigned op, uint32_t a, uint32_t b, uint32_t *ge) {
    const int sgn = kind <= 2;
    const unsigned bits = op >= 4 ? 8u : 16u;
    const unsigned lanes = 32u / bits;
    const uint32_t mask = (1u << bits) - 1u;
    uint32_t out = 0, g = 0, dummy = 0;
    for (unsigned l = 0; l < lanes; l++) {
        unsigned bl = l;
        int add = 1;
        if (op == 3 || op == 5)
            add = 0;
        else if (op == 1) {
            add = (l == 1);
            bl = 1u - l;
        } else if (op == 2) {
            add = (l == 0);
            bl = 1u - l;
        }
        const uint32_t xa = (a >> (l * bits)) & mask, xb = (b >> (bl * bits)) & mask;
        const int64_t x = sgn ? rc_sx(xa, bits) : (int64_t)xa, y = sgn ? rc_sx(xb, bits) : (int64_t)xb;
        const int64_t r = add ? x + y : x - y;
        uint32_t lane;
        if (kind == 0 || kind == 3) {
            lane = (uint32_t)r & mask;
            const int ge_lane = (sgn || !add) ? (r >= 0) : (r >= ((int64_t)1 << bits));
            if (ge_lane)
                g |= (bits == 16u ? 3u : 1u) << (l * (bits == 16u ? 2u : 1u));
        } else if (kind == 1)
            lane = rc_ssat(r, bits, &dummy) & mask;
        else if (kind == 4)
            lane = rc_usat(r, bits, &dummy) & mask;
        else
            lane = (uint32_t)(r >> 1) & mask;
        out |= lane << (l * bits);
    }
    if (kind == 0 || kind == 3)
        *ge = g;
    return out;
}

static inline uint32_t rc_sel(uint32_t a, uint32_t b, uint32_t ge) {
    uint32_t m = 0;
    for (unsigned l = 0; l < 4; l++)
        if ((ge >> l) & 1u)
            m |= 0xFFu << (8u * l);
    return (a & m) | (b & ~m);
}

static inline uint32_t rc_ext16(uint32_t v, int is_signed, int add, uint32_t acc) {
    uint32_t r = 0;
    for (unsigned h = 0; h < 2; h++) {
        const uint32_t b = (v >> (16u * h)) & 0xFFu;
        uint32_t e = is_signed ? ((uint32_t)(int32_t)(int8_t)b & 0xFFFFu) : b;
        if (add)
            e = (e + (acc >> (16u * h))) & 0xFFFFu;
        r |= e << (16u * h);
    }
    return r;
}

static inline uint32_t rc_usad(uint32_t a, uint32_t b, uint32_t acc) {
    for (unsigned l = 0; l < 4; l++) {
        const int x = (int)((a >> (8u * l)) & 0xFFu), y = (int)((b >> (8u * l)) & 0xFFu);
        acc += (uint32_t)(x > y ? x - y : y - x);
    }
    return acc;
}

static inline uint32_t rc_qop(int sub, int dbl, uint32_t m, uint32_t n, uint32_t *q) {
    int64_t nn = (int32_t)n;
    if (dbl)
        nn = (int32_t)rc_ssat(nn * 2, 32, q);
    return rc_ssat(sub ? (int64_t)(int32_t)m - nn : (int64_t)(int32_t)m + nn, 32, q);
}
