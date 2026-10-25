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
| What the game shows | its **save-data error screen** ("The saved game data is corrupted. Please go to the HOME Menu…"): the FS HLE provides no save archive |
| Anything past language selection (title, new game, overworld) | **not reached** |

The game can now be observed (`PROBE_SHOT`). It stops at the save-corrupted screen, so the next milestone is the
save-data archive (`fs:USER` OpenArchive for save data / extdata, plus `fs:File` writes).

## Bugs found by this work (all fixed, with tests)
- **16 KiB host pages vs 4 KiB guest pages.** The kernel rounded every allocation to the *host* page, so the game's
  second shared-memory mapping (`MapMemoryBlock` at `0x10002000`, 8 KiB after the first) failed with
  `0xE0E01BF5`. The game therefore never had HID memory. Allocation is now tracked per guest page
  (`kernel_test`: "4 KiB guest pages on a larger host page").
- **HID pad layout.** The service wrote pad entries at `+0x20`; the game's reader (`0x28242c`) reads entry
  `index % 8` at `+0x28 + 16·i` (held, pressed, released, circle pad as two halfwords).
- **Linear-heap physical addresses.** `svcGetProcessInfo` type 20 returned 0, so every GPU address the game wrote was
  virtual; it must return `0x0C000000` (docs/gpu.md).
- **`ldr:ro`** answered success for everything without mapping anything (docs/ldr-ro.md).

## HID shared memory (evidence from the game's own readers)
| Block | Evidence |
|---|---|
| Pad: header at `+0x00` (u64 tick, u64 previous tick, u32 index at `+0x10`), entries at `+0x28`, 16 bytes each | loop at `0x28242c` |
| Touch: block at `+0xA8` with the same header shape, entries at `+0xA8+0x20`, 8 bytes each: `u16 x, u16 y, u8 valid` | loop at `0x2825d0` |
The game maps the GPU block at `0x10000000` and HID at `0x10002000`.

## Probe knobs (local)
`PROBE_TRACE=file` kernel trace; `PROBE_PRESS=period:hold:hexKeys` and `PROBE_TOUCH=period:hold:x,y[;x,y…]` scripted
input (in guest instructions); `PROBE_PROTECT=addr:size:afterInsns` makes a range fault to find who touches it;
`PROBE_WORDS=addr,addr` prints guest words after the run.
