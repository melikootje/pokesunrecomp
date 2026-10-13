# Recompiler testing and its limits

## What is tested
`tools/recomp/tests` replays **reference vectors** against two independent paths:
1. the reference interpreter (`tools/recomp/src/interp.cpp`), and
2. C code emitted by the recompiler (`emit_c.cpp`) — compiled with `-Wall -Wextra -Werror` — in two variants:
   all flags kept, and with dead-flag elimination (`liveOut = 0`, so only registers/memory/PC must match).

The vectors (`vectors/arm.vec`) come from **Unicorn with the ARM11 MPCore CPU model** (the 3DS CPU). They are
short random instruction sequences (ARM, Thumb, VFP) with random initial state; the expected state is whatever
Unicorn produced. They contain no game code. Regenerate with the venv Python:

    python -I tools/recomp/tests/gen_vectors.py <count> <seed> <out> [thumb|vfpdata|vfpxfer|<media kind>]

Mutation checks (deliberately breaking ADC carry-in, flag liveness, Thumb BL target, link register) are caught.
Sanitizer builds (ASan/UBSan) are clean on the full set. CI runs the vectors with gcc (Linux) and clang (macOS).

## Known gaps (deliberately not generated, or unverified)
- **Unaligned word access.** ARM11 behaviour depends on SCTLR.U and is not established for the 3DS; the
  interpreter/emitter implement the legacy rotate for `LDR`/`SWP` and align-down for `STR`. Vectors stay aligned.
- **PC with register-controlled shifts, STM of PC, LDRD/STRD pairs touching PC/LR, Rd==Rn writeback:**
  UNPREDICTABLE in the ARM ARM; not generated.
- **VFP short vectors.** The reference model mis-implements stride 2 (it writes two elements, spaced 4 apart) and
  double-precision operations whose source is in a vector bank. Those shapes are implemented from the ARM
  Architecture Reference Manual but **unverified**: vectors use stride 1, and double vector ops only with a
  scalar (bank 0) source. Single-precision vector mode with stride 1 matches the reference.
- **FPSCR trap-enable bits and FPEXC/FPSID**: only the bits the reference exposes are modelled (`RC_FPSCR_WMASK`).
- **Reference ≠ hardware.** Unicorn is QEMU's model of the ARM11, not a 3DS. Agreement with it is strong
  evidence of ARM-ARM conformance, not proof of bit-identical hardware behaviour (e.g. VFP11 corner cases).
- **Not covered yet:** Thumb-2 (not used by ARMv6K), CRO-relative code, indirect-branch dispatch (#7).
