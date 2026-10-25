# Architecture

```
 user's decrypted dump
        │  romtool (tools/romtool, tools/romlib)
        ▼
 exefs/code.bin + exheader + lazy RomFS access          ← implemented (issues #2, #3)
        │  analysis (REA)                                ← planned (#4)
        ▼
 recompiler (ARM/Thumb/VFP → C → AArch64)               ← decoder, interpreter, C emitter, VFP soft-float
                                                          implemented and Unicorn-verified (#5, #6);
                                                          dispatch/CRO (#7, #8) planned
        ▼
 generated sources (local only, never committed)
        │  linked with
        ▼
 runtime: HLE kernel/services, PICA200→Vulkan, DSP audio ← planned (#9–#14)
        ▼
 Android app shell, dual-screen                          ← planned (#15, #16)
```

## Current code
- `tools/romlib`: container parsing (NCSD/NCCH/ExHeader/ExeFS/RomFS), backward-LZ, SHA-256, title
  profiles. All reads go through a `Source` abstraction so multi-GiB dumps are never loaded eagerly.
- `tools/romtool`: CLI over romlib.
- `tools/tests`: unit tests and a CLI end-to-end test, built from synthetic fixtures only.
- `tools/recomp`: A32 + Thumb decoder, reference interpreter, C emitter (explicit NZCV, flag-liveness elimination).
- `runtime`: guest memory (4 GiB reservation, guarded faults, `alias()` second views), CRO loader + `ldr:ro` (docs/ldr-ro.md), a PICA200 software renderer (docs/gpu.md), host-backed save data (docs/save-data.md), VFPv2 soft-float (`vfp.c`), CPU context/ops headers
  used by generated code; the HLE kernel and services (srv, apt, cfg, fs, gsp, hid, ldr, …) run the real game to its first
  interactive screen, with stubs for the rest (docs/boot-status.md).
- Testing approach and its limits: `docs/recompiler-testing.md`.

This document is updated as each roadmap issue lands; sections marked "planned" are not implemented.
