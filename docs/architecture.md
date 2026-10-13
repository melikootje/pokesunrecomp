# Architecture

```
 user's decrypted dump
        │  romtool (tools/romtool, tools/romlib)
        ▼
 exefs/code.bin + exheader + lazy RomFS access          ← implemented (issues #2, #3)
        │  analysis (REA)                                ← planned (#4)
        ▼
 recompiler (ARM/Thumb/VFP → AArch64)                    ← planned (#5–#8)
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
- `runtime`: placeholder library.

This document is updated as each roadmap issue lands; sections marked "planned" are not implemented.
