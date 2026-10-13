/* ARM11 VFPv2 semantics (FPSCR flags, flush-to-zero, default NaN, rounding modes, short vectors) implemented
 * with integer arithmetic so results are bit-exact on every host. Plain C. */
#pragma once
#include "pokerecomp/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FPSCR bits */
#define RC_FPSCR_N (1u << 31)
#define RC_FPSCR_Z (1u << 30)
#define RC_FPSCR_C (1u << 29)
#define RC_FPSCR_V (1u << 28)
#define RC_FPSCR_DN (1u << 25)
#define RC_FPSCR_FZ (1u << 24)
#define RC_FPSCR_IDC (1u << 7)
#define RC_FPSCR_IXC (1u << 4)
#define RC_FPSCR_UFC (1u << 3)
#define RC_FPSCR_OFC (1u << 2)
#define RC_FPSCR_DZC (1u << 1)
#define RC_FPSCR_IOC (1u << 0)
/* Bits software may write (matches the reference model; the trap-enable bits are not implemented). */
#define RC_FPSCR_WMASK 0xFFF7009Fu

enum RcVfpOp {
    RC_V_MLA, RC_V_MLS, RC_V_NMLS, RC_V_NMLA, RC_V_MUL, RC_V_NMUL, RC_V_ADD, RC_V_SUB, RC_V_DIV,
    RC_V_MOV, RC_V_ABS, RC_V_NEG, RC_V_SQRT,
    RC_V_CMP, RC_V_CMPE, RC_V_CMPZ, RC_V_CMPEZ,
    RC_V_CVT_DS,                       /* single<->double: dbl selects the *source* size */
    RC_V_CVT_U2F, RC_V_CVT_S2F,        /* integer in Sm -> float (dbl selects destination size) */
    RC_V_CVT_F2U, RC_V_CVT_F2S,        /* float -> integer in Sd, FPSCR rounding (VCVTR) */
    RC_V_CVT_F2U_RZ, RC_V_CVT_F2S_RZ   /* float -> integer, round toward zero */
};

/* Executes one VFP data-processing instruction including short-vector expansion.
 * vd/vn/vm are S register numbers (0-31) or D register numbers (0-15) when dbl != 0. */
void rc_vfp_data(RcCtx* c, unsigned op, unsigned dbl, unsigned vd, unsigned vn, unsigned vm);

#ifdef __cplusplus
}
#endif
