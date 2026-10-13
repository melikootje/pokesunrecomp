#pragma once
/* Test harness memory: a 64 KiB window at 0x20000000; any other access is a recorded fault. */
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t rc_test_window[0x10000];
extern unsigned rc_test_faults;
extern uint32_t rc_test_last_fault;
extern uint8_t rc_test_scratch[16];
extern uint8_t rc_test_code[0x1000]; /* instruction bytes at 0x00100000 (read-only) */
#ifdef __cplusplus
}
#endif

static inline uint8_t *rc_t(uint32_t a, uint32_t n) {
    if (a >= 0x00100000u && (uint64_t)a + n <= 0x00101000u)
        return rc_test_code + (a - 0x00100000u);
    if (a < 0x20000000u || (uint64_t)a + n > 0x20010000u) {
        rc_test_faults++;
        rc_test_last_fault = a;
        return rc_test_scratch;
    }
    return rc_test_window + (a - 0x20000000u);
}
static inline uint8_t rc_t_ld8(uint32_t a) { return *rc_t(a, 1); }
static inline uint16_t rc_t_ld16(uint32_t a) {
    uint16_t v;
    memcpy(&v, rc_t(a, 2), 2);
    return v;
}
static inline uint32_t rc_t_ld32(uint32_t a) {
    uint32_t v;
    memcpy(&v, rc_t(a, 4), 4);
    return v;
}
static inline void rc_t_st8(uint32_t a, uint8_t v) { *rc_t(a, 1) = v; }
static inline void rc_t_st16(uint32_t a, uint16_t v) { memcpy(rc_t(a, 2), &v, 2); }
static inline void rc_t_st32(uint32_t a, uint32_t v) { memcpy(rc_t(a, 4), &v, 4); }
#define RC_LD8(c, a) rc_t_ld8(a)
#define RC_LD16(c, a) rc_t_ld16(a)
#define RC_LD32(c, a) rc_t_ld32(a)
#define RC_ST8(c, a, v) rc_t_st8((a), (v))
#define RC_ST16(c, a, v) rc_t_st16((a), (v))
#define RC_ST32(c, a, v) rc_t_st32((a), (v))
