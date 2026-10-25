# Save data, extdata and the SD card (fs:USER)

`runtime/src/fs_service.cpp` implements `fs:USER` on top of `runtime/src/save_store.cpp`, a host directory per archive.
RomFS stays read-only and is served from the user's dump through `FsBackend`.

## Command set (verified from the game's own SDK stubs)
The SDK's `fs:USER` client stubs are in `0x16bf10-0x16cbb4` of `code.bin`; each loads a literal command header and packs
its arguments in the command buffer. The ones implemented:

| Header | Command | Stub |
|---|---|---|
| `0x080C00C2` | OpenArchive(id, path type, size, path) | `0x16c084` |
| `0x080E0080` | CloseArchive(handle u64) | `0x16c114` |
| `0x080201C2` / `0x08030204` | OpenFile / OpenFileDirectly | `0x16c854` / `0x16c4d8` |
| `0x08080202` | CreateFile(…, size u64, path) | `0x16bf10` |
| `0x08040142` / `0x08060142` / `0x08070142` | DeleteFile / DeleteDirectory / DeleteDirectoryRecursively | `0x16bf6c` / `0x16c3f8` / `0x16c778` |
| `0x08090182` | CreateDirectory | `0x16c3a4` |
| `0x08050244` / `0x080A0244` | RenameFile / RenameDirectory | `0x16bfbc` / `0x16c448` |
| `0x080B0102` | OpenDirectory | `0x16c20c` |
| `0x080D0144`, `0x08120080` | ControlArchive (no-op), GetFreeBytes | `0x16c270`, `0x16c144` |
| `0x084500C2` | **GetFormatInfo**(id, path type, size, path) → size, directories, files, duplicate flag | `0x16c180` |
| `0x084C0242` | **FormatSaveData**(id, path type, path size, size, dirs, files, dir buckets, file buckets, dup) | `0x16c2dc` |
| `0x08510242` | **CreateExtSaveData**(media, id low, id high, …, dirs, files, size limit, …) | `0x16c578` |

File sessions: Read `0x0802`, Write `0x0803`, GetSize `0x0804`, SetSize `0x0805`, Close `0x0808`, Flush `0x0809`,
**OpenLinkFile `0x080C`** (the game issues it on its save file; without it the game stops with `svcBreak`).
Directory sessions: Read `0x0801` (0x228-byte entries), Close `0x0802`.

`0x084500C2` was previously answered "success, 0" on the assumption that it was an authority check; the stub returns
three words and a byte, so it is `GetFormatInfo`, and the wrong answer made the game conclude its save was corrupted.

## Observed first-boot flow (real game, fresh save directory)
1. `GetFormatInfo(4 = SaveData)` → not formatted → the game calls `FormatSaveData(512, 1 dir, 1 file, duplicate)` and
   creates its save file `savedata/main`.
2. `OpenArchive(6 = ExtSaveData, SD, id 0x1b50)` → not found → `CreateExtSaveData(15 directories, 682 files)`.
3. The game shows *"The game is preparing to load, and additional data is being created. Please do not remove your SD
   Card…"* — the real first-run screen — instead of the former *"The saved game data is corrupted"* error screen.

## Result codes
Built as `description | 17<<10 | summary<<21 | 25<<27`. `0xC8804464` (not found, 100) is the value RomFS lookups already
used and matches this formula. The others use the description numbers I know from other implementations and are
**not confirmed against hardware**: ExtData not found `0xC8804478` (120), file not found (112), path not found (113),
already exists (190), directory not empty (240), not formatted `0xC9004554` (340), invalid path (702). The game accepted
the not-formatted and ext-data-not-found codes (it proceeded to format/create), which is the only confirmation.

## Storage and safety
- One directory per archive under the save root (`PROBE_SAVEDIR`, default `./savedata`, ignored by git):
  `savedata/`, `extdata_<media>_<id>/`, `sharedext_…`, `system_…`, `sdmc/`, plus `meta/<archive>.format`.
- Guest paths are confined: absolute paths, `..`, backslashes, NUL, `:` and over-deep paths are rejected
  (`save_store_test`, `fs_service_test`). Files are capped at 256 MiB.
- Every service call now runs under the kernel's fault guard: a bad guest pointer fails the request instead of crashing
  the host (found when `GetFormatInfo` read its path from the wrong word).

## Not done
Atomic writes and backup rotation, import/export of a real 3DS save, per-archive quotas, shared/system archive formats,
`SDMC` write-only semantics, and the `0x0802` Read of files larger than the guest buffer in one call.
