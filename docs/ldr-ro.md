# ldr:ro and the CRO loader (issue #8)

`runtime/src/ro_loader.cpp` (`pokerecomp::ro::Loader`) rebases, relocates and links CRS/CRO images in guest memory;
`runtime/src/ldr_service.cpp` is the `ldr:ro` service around it. Everything below was established against the
Ultra Sun (USA) image whose `code.bin` SHA-256 is `b5388f75…9e09` (see `analysis/ultrasun-usa/README.md`).

## How the game uses it (verified from the binary)
The SDK's `nn::ro` client has exactly four IPC stubs. They load the session handle from the global at `0x69cae8`.

| Stub | Header | Role | Evidence |
|---|---|---|---|
| `0x17368c` | `0x000100C2` | Initialize(crsBuffer, size, mappedAddr) + process handle | args stored to cmd[1..3]; matches the runtime log |
| `0x173584` | `0x00020082` | LoadCRR(crrBuffer, size) + process handle | called from `0x173094` |
| `0x1735c8` | `0x000902C2` | **LoadCRO_New**, 11 normal words + process handle | packing at `0x1735dc-0x173614`, caller `0x172f38` |
| `0x173648` | `0x000500C2` | module unload (cmd[1] = module address) | only caller `0x1730e8` reads the module magic, then the data size, before it |

LoadCRO_New words: `[1] heap buffer, [2] mapped address, [3] size, [4] data buffer, [5] 0, [6] aligned data size,
[7] bss buffer, [8] bss size, [9] auto-link (byte), [10] fix level, [11] CRR address`. Words 1–8 follow from the arithmetic
in the caller (`0x172f88-0x172fc8`: data size is `header+0xBC` rounded up to 4, bss starts at `dataBuf + dataSize`).
Words 9–11 are named from the stub's packing order only (medium confidence).

**Mapped window.** `Initialize(crs=0x081de000, mapped=0x008b2000)` and the client code at `0x172f6c-0x172f84`
(`mapped = heap − 0x08000000 + 0x006d4000`, where `0x6d4000` is the exact end of the process image from the ExHeader)
show that the service must make the heap buffer appear a second time in a window after the image. Both views must be
coherent: after `LoadCRO` the client copies the module's `.data` from the image (`0x172ff0-0x173044`, part from the
mapped view, the rest from the heap view) into its own data buffer and zeroes bss itself. Hence:
- `GuestMemory::alias()` makes a real second view of the pages (tested; the Linux/Android `mremap` path is only
  exercised by CI, not on the development Mac);
- relocations that target `.data` are written **into the image** until the guest has copied it out
  (`Module::dataLive`, set when the next `ldr:ro` request arrives), then into the live data buffer.

## Format facts (verified against all 133 modules in the dump)
- All 133 files parse. Only relocation type `2` (absolute 32-bit: `symbol + addend`) occurs, in the internal (56 581)
  and external (45 892) tables; the static table is empty. Other types are refused, not guessed.
- The CRS has `CRO0` magic. Its segment table holds absolute main-executable addresses; game CROs use image offsets.
- `fixed_size` is 0 in the files. `OnUnresolved` (`+0xAC`) is set only in the CRS (`0x548bd4`).
- Internal relocation entries carry the symbol segment in byte 5; external entries carry "last of batch" there.

`build/tools/recomp/ro_check <dump>` loads the CRS and all 132 CROs through the loader and audits the result:
132 loaded, 0 failed, 56 581 + 45 892 relocations applied (equal to the totals in the files), 21 297 import batches
resolved, 0 words left at the unresolved handler, 0 relocated words outside every segment, 0 unresolved named imports.

## Not verified / known limits
- Result codes returned on failure are placeholders (the client only tests the sign bit).
- LoadCRR does not verify signatures; `fix level` is ignored and the "fixed size" returned is the whole buffer.
- `OnLoad`/`OnExit` are not called by the service (assumed to be the client's job; not yet observed).
- CRO code is not recompiled (its address is only known at run time): it runs in the dispatcher's interpreter
  fallback, which was ~12 % of executed instructions (247M of 2.0G) at the language-select screen.
- Unload re-points other modules' imports at `OnUnresolved`; reloading at a different address is covered by unit
  tests only, the game has not been seen to do it.

## Status at the end of this milestone
The recompiled image boots, loads the CRS and `LangSelect.cro` and runs its main loop (2 × 10⁹ guest instructions
without fault or deadlock). It then waits for input at the language-selection scene; nothing is rendered yet.
