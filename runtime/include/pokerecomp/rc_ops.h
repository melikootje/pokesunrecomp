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
