# Verified boot status (Ultra Sun USA, `code.bin` SHA-256 `b5388f75…9e09`)

What actually runs, measured with the local `boot_probe` on a user-supplied dump (never in CI). "Reached" means
observed in the kernel/IPC trace, not inferred.

| Stage | Status |
|---|---|
| Entry point, SDK start-up, `srv:` registration, TLS/threads/arbiters | reached |
| `APT`, `cfg:u`, `ptm:u`, `ndm:u`, `fs:USER` (RomFS), `dsp::DSP`, `frd:u`/`boss:U` (stubs) | reached |
| `gsp::Gpu` (shared memory, interrupt queue, GX commands) | reached; **no rendering** (commands are acknowledged) |
| `hid:USER` shared memory | mapped and polled by the game (see below) |
| `ldr:ro`: CRS + `LangSelect.cro` | reached, loaded and relocated (docs/ldr-ro.md) |
| Language-selection scene / first frames | running; **both screens render** (software renderer, docs/gpu.md) |
| First-boot data | `GetFormatInfo` → `FormatSaveData` → `CreateExtSaveData` flow completes (docs/save-data.md) |
| **Language-selection screen** | **reached and rendered**: "Play the game in" English / Español / Français / Deutsch over the Alola sea backdrop; bottom screen shows the instructions (`PROBE_SHOT`) |
| Language choice → "Begin game" → Kukui's message → **name entry** (touch keyboard) → hero portrait → move-in cutscene | **reached with scripted input** (`PROBE_SCRIPT`); modules loaded along the way: `LangSelect`, `AppDemo`, `SequencePlayer`, `Object`, `StrInput`, `HeroSelect` |
| Cutscene content | **not rendered**: the video-message box interior stays at the clear colour and the move-in scene's top screen has no draws at all |

### The cutscene gap (open)
Evidence gathered, not yet explained: (1) the backdrop draw is a 256x512 ETC1A4 texture with a transparent window, so the
window's content must come from earlier draws that never occur (draw logs after 2.3e9 and 3.35e9 guest instructions show only
backdrop, icons, dialog and glyph draws); (2) `PicaStats` reports no unsupported feature and no skipped draw; (3) no `fs:`
read or open fails (every failing request is logged); (4) every thread is parked at ordinary wait points or runs
intermittently (`PROBE_PCSAMPLE`), so it is not a spin or deadlock; (5) the game never requests `y2r`/`mvd`.
Candidates still open: audio-clocked video/sequence playback (the DSP HLE only fires a frame interrupt and does not maintain
the shared-memory source/status structures), and 3D passes gated on a state the HLE does not provide.

The game is observed with `PROBE_SHOT`. It used to stop at a *save data is corrupted* screen: the FS HLE answered
`GetFormatInfo` wrongly and had no writable archives. With the save-data archive in place it runs the genuine first-boot
sequence and stops, waiting for input, at the language selection.

## Bugs found by this work (all fixed, with tests)
- **16 KiB host pages vs 4 KiB guest pages.** The kernel rounded every allocation to the *host* page, so the game's
  second shared-memory mapping (`MapMemoryBlock` at `0x10002000`, 8 KiB after the first) failed with
  `0xE0E01BF5`. The game therefore never had HID memory. Allocation is now tracked per guest page
  (`kernel_test`: "4 KiB guest pages on a larger host page").
- **HID pad layout.** The service wrote pad entries at `+0x20`; the game's reader (`0x28242c`) reads entry
  `index % 8` at `+0x28 + 16·i` (held, pressed, released, circle pad as two halfwords).
- **Linear-heap physical addresses.** `svcGetProcessInfo` type 20 returned 0, so every GPU address the game wrote was
  virtual; it must return `0x0C000000` (docs/gpu.md).
- **fs:USER** answered `GetFormatInfo` with a single zero (it returns size, directories, files and a flag) and had no
  save archive; the game concluded its save was corrupted (docs/save-data.md). `OpenLinkFile` was missing and ended the
  run in `svcBreak`. Services now run under the fault guard so a bad guest pointer cannot crash the host.
- **`ldr:ro`** answered success for everything without mapping anything (docs/ldr-ro.md).

## HID shared memory (evidence from the game's own readers)
| Block | Evidence |
|---|---|
| Pad: header at `+0x00` (u64 tick, u64 previous tick, u32 index at `+0x10`), entries at `+0x28`, 16 bytes each | loop at `0x28242c` |
| Touch: block at `+0xA8` with the same header shape, entries at `+0xA8+0x20`, 8 bytes each: `u16 x, u16 y, u8 valid` | loop at `0x2825d0` |
The game maps the GPU block at `0x10000000` and HID at `0x10002000`.

## Probe knobs (local)
`PROBE_TRACE=file` kernel trace (first 200k lines); `PROBE_SCRIPT=file` timed input (`press`, `touch`, `repeat`; see
`boot_probe.cpp`), `PROBE_PRESS` / `PROBE_TOUCH` periodic input; `PROBE_SAVEDIR` writable archives (default `./savedata`);
`PROBE_SHOT=dir:every` PNG screenshots with `PROBE_RENDER_AFTER` / `PROBE_RENDER_WINDOW` (rasterize only when a frame is
needed: the vertex stage always runs); diagnostics: `PROBE_DRAWLOG`, `PROBE_DRAWSUM[_AFTER]`, `PROBE_DRAWEND[_AFTER]`,
`PROBE_RT_SEQ`, `PROBE_RT_DRAW`, `PROBE_DUMP_RT`, `PROBE_DUMPMEM=addr:size:file`, `PROBE_PCSAMPLE=fromInsns` (per-thread pc
histogram), `PROBE_PROTECT=addr:size:afterInsns` (make a range fault to find who touches it), `PROBE_WORDS=addr,addr`.

## Resolved: 3D model scenes (stereo camera calibration)

The move-in cutscene's 3D content (offscreen model draws composited into the video-message box) rendered
nothing because every model draw produced NaN clip positions. Root cause, verified on the real dump:

- The game builds its stereo cameras from the `cfg` config block `0x00050005` (StereoCameraSettings, 32 bytes,
  requested with `GetConfigInfoBlk2`). Our `cfg` returned zeros for every block.
- The camera constructor at `0x278c00` copies fields into the camera and a following function computes
  `s5 = a / (b * field[+12])` then `s1 = s5 * field[+16]`; with zeros that is `inf * 0 = NaN`, which flows into the
  view/projection uniforms (`c87`, `c90..c92`) and into every clip position.
- Method: `Pica` counts NaN uniform uploads and records the command-list position (`PicaStats::nanUniforms`,
  `firstNan*`, `lastNan*`); an instrumented `rc_vfp_data` showed the first NaN-producing VFP op (`vmul inf*0`) at
  `0x278e4c`; the `inf` came from a `vdiv` by a zero read from a `.bss` singleton filled by the config IPC.

Fix: `cfg` now returns finite, non-zero stereo settings for that block. **Provenance caveat:** the eight floats
are generic defaults used by other emulators, not values read from a console; the hypothesis tested is that any
finite non-zero data removes the NaN (confirmed: NaN uniform uploads fell from ~100k to 21 and the scene
renders), and real calibration would only change the parallax. Regression test: `cfg_service_test`.

Result: Kukui's video message now renders with the 3D Pokémon in the frame (verified with `PROBE_SHOT`).

Remaining gaps: fragment lighting is not implemented (a large share of draws use it), buffer-jump chains hit the
depth limit (256) about 1,400 times, DSP HLE only provides the frame counter, software rendering is slow.

## Resolved: fatal error after "Three months later" (CreateMemoryBlock over owned memory)

After the intro the game raised `ErrDisp` with `0xE0E01BF5`. That value is our own kernel's `InvalidAddress`: the
nwm::UDS library (local wireless) calls `svcCreateMemoryBlock(addr != 0, ...)` over a buffer in its own heap.
On hardware a non-zero address designates memory the process already owns; our kernel tried to allocate it and
refused because the pages were committed. Fix: reuse the existing pages (`kernel_test` covers it). The game now
reaches the player's bedroom (Mom's "first day under Alola's sun" dialogue).

Method worth keeping: `PROBE_RECENT_IPC=1` prints the last 48 service requests (with results and file paths) when
the run stops, and the `ErrorInfo` words of an `err:f` throw are logged.

Open observations: the bedroom scene renders with a strong magenta tint (suspect: fragment lighting is not
implemented, or a combiner source); `OpenFileDirectly` on archive `0x2345678A` (binary 16-byte archive path
`00010302 000400db ffffff00 00000001`, 20-byte zero file path) is answered "file not found" and the game carries
on; the display transfer `1f180000 -> 1f480000, 128x256, flags 0x02000020` (2x2 downscale with equal in/out
dimensions) is rejected about 2,000 times.

## Resolved: magenta 3D scenes (downscaling display transfer)

The bedroom scene was tinted magenta. Per-draw render-target dumps (`PROBE_RT_SEQ`) showed the picture was right
until the last three additive full-screen passes of the game's bloom chain. Those sample a 64x128 texture at VRAM
`0x18480000` that the game produces with a display transfer (`1f180000 -> 1f480000`, input `128x256`, flags
`0x02000020` = 2x2 downscale) whose *output dimensions equal the input dimensions*. We rejected such transfers
(output too large for the scaled source), so the texture kept stale memory, which was added three times.
Fix: when the output size cannot fit the scaled source it is taken as the source footprint (so the result is the
half-size image). Test: `gpu_test`. The scene now renders in natural colours.

Diagnostics added on the way: `PROBE_TEXDUMP=dir:after:count` (decoded textures of draws as PNG),
`PROBE_LITREGS` (register file at the latest lit draw), a texture-format histogram, `PROBE_RECENT_IPC`, and combiner
constants in the draw summary.
