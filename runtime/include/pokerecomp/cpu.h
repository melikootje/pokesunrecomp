/* Guest CPU context shared by the interpreter, generated (recompiled) code and the runtime.
 * Plain C so generated C sources can include it. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RcCtx {
    uint32_t r[16];      /* r15 holds the *next pc* between blocks (not pc+8) */
    uint32_t n, z, c, v; /* each 0 or 1 */
    uint32_t q;          /* sticky saturation flag (0/1) */
    uint32_t ge;         /* GE[3:0] */
    uint32_t thumb;      /* 1 when the next pc is Thumb code */
    uint32_t excl_valid; /* local exclusive monitor */
    uint32_t excl_addr;
    uint32_t tls;  /* CP15 c13 c0 3 (user read-only thread pointer) */
    uint32_t svc;  /* pending SVC number (set by emitted code, consumed by dispatcher) */
    uint32_t trap; /* non-zero when emitted code hit something it cannot execute */
    uint32_t trap_pc;
    uint32_t fpscr; /* VFP status (see issue #6) */
    uint32_t s[32]; /* VFP single registers S0-S31 (D<n> = s[2n] low word, s[2n+1] high word) */
    uint8_t *mem;   /* guest address 0 in host memory (base + addr fast path) */
} RcCtx;

#ifdef __cplusplus
}
#endif
