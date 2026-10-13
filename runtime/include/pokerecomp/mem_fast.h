/* Default guest memory accessors: fast path = host base + 32-bit guest address.
 * A harness may define its own RC_LD/RC_ST macros before including generated code. */
#pragma once
#include <stdint.h>
#include <string.h>

#ifndef RC_LD8
static inline uint8_t rc_ld8(const uint8_t *m, uint32_t a) { return m[a]; }
static inline uint16_t rc_ld16(const uint8_t *m, uint32_t a) {
    uint16_t v;
    memcpy(&v, m + a, 2);
    return v;
}
static inline uint32_t rc_ld32(const uint8_t *m, uint32_t a) {
    uint32_t v;
    memcpy(&v, m + a, 4);
    return v;
}
static inline void rc_st8(uint8_t *m, uint32_t a, uint8_t v) { m[a] = v; }
static inline void rc_st16(uint8_t *m, uint32_t a, uint16_t v) { memcpy(m + a, &v, 2); }
static inline void rc_st32(uint8_t *m, uint32_t a, uint32_t v) { memcpy(m + a, &v, 4); }
#define RC_LD8(c, a) rc_ld8((c)->mem, (a))
#define RC_LD16(c, a) rc_ld16((c)->mem, (a))
#define RC_LD32(c, a) rc_ld32((c)->mem, (a))
#define RC_ST8(c, a, v) rc_st8((c)->mem, (a), (v))
#define RC_ST16(c, a, v) rc_st16((c)->mem, (a), (v))
#define RC_ST32(c, a, v) rc_st32((c)->mem, (a), (v))
#endif
