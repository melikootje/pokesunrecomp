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

## Open blocker: offscreen 3D model draws produce no fragments (investigated)

Verified on the real dump (move-in cutscene, ~2.3 G instructions with `PROBE_SCRIPT`):

- The game issues its 3D scenes from **nested command buffers** (`CMDBUF_JUMP`), now executed. An offscreen
  256x512 render target at phys `0x18100000` is later sampled by 12 composite draws.
- The ~120 offscreen model draws yield zero fragments because the vertex shader outputs **NaN clip positions**.
  Vertex inputs are sane. The NaN comes from the vertex-shader float uniforms `c90..c92` (view/model rows) and
  `c87` (projection row 0), which the game itself uploads as NaN (`0x7fc00000` / `0xffc00000`, float32 upload
  mode). The shader interpreter is not the cause (instruction trace: `DP3 ... c90` consumes the NaN).
- The NaN is **not** created by any VFP data-processing op: instrumenting `rc_vfp_data` showed no op that turns
  non-NaN inputs into NaN (and no IOC exceptions). So it enters through memory (a load / HLE-provided value /
  file data) before the matrix is built. The producing guest code is still unidentified.
- Next step: find the first guest store of a NaN word into the matrix source (write-watch on the command-list
  words, or NaN check in store paths) and trace the value back; suspects are an HLE-provided value (shared
  page, tick/time based animation delta) or file data read incorrectly.

Other known gaps: fragment lighting is not implemented (30k draws use it), buffer-jump chains hit the depth
limit (256) 1,825 times (a 400/1056-byte pair repeating), DSP HLE only provides the frame counter.
