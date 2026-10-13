# Ultra Sun (USA) `code.bin` analysis — derived metadata only

Produced with the REA MCP server (Ghidra 12.1.4 provider) on a locally extracted, decompressed `.code`
image. **Only addresses, sizes, modes, numbers and hashes are committed; no code bytes.**

- `code.bin` SHA-256: `b5388f7500d91be01499a99ca007c98212068608ed7c83c43952e1d5148e9e09` (5,914,624 bytes)
- ELF wrapper (local only, via `romtool elf`) SHA-256: `bc860d48bdb72a885b39fdd3c754a906bd469941d84bb1dd997ab57e175b54a2`
- Segments (observed, matches ExHeader): text `0x100000–0x5ba000`, ro `0x5ba000–0x667000`, data+bss `0x667000–0x6d41bc`

## REA evidence
| What | Evidence ID |
|---|---|
| `list_segments` | `ev_e80d69d66549da7c0df115cc6cf488d1a7c81311c4f408e92568ec4f38196309` |
| `binary_overview` (17,448 procedures, 12,300 strings) | `ev_0776f818342a6689a21f43fa17bb627ccf1ca69270e42c483260c818ea879b6f` |
| entry stub (tail `swi 0x3`) | `ev_f88231b40982d71c14a2014c393cdd29a6ac44768fb644eda690ee41037ca5b4` |
| IPC stub @0x1050c4 (header `0x10002`, svc 0x32) | `ev_cdd0279a14e71d5c1e33a5e211e823231e2e9f628b89b943e9ae8b59b0ff809c` |
| IPC stub @0x105cf0 (header `0x20000`) | `ev_205f271d38609c258206768e6a2f7b7f9b82df01fd3a428a297e5cd338a55e96` |
| IPC stub @0x106078 (header via literal, 2 normal params) | `ev_c8c148aeac861911dd48474bddeaad250c500378aa6734b0a086733abf5db94b` |
| Thumb function @0x100ef0 (static-init loop) | `ev_649ee573e25c50e8265c15d4f4f0255d6d6b042f0d1c72eb0aa69612d26c43c8` |

## Method and results
- `symbols.json`: 17,448 functions from Ghidra (count equals REA's `binary_overview`): 17,242 ARM, 206 Thumb,
  321 thunks. Mode/size come from `tools/analysis/ghidra_scripts/ExportFunctionMap.java` run headless on the
  same ELF (REA does not expose per-function mode in bulk). Names are not recovered yet.
- `svc_sites.json`: 734 SVC sites (32 distinct numbers). Found by scanning for unconditional `svc`
  words inside Ghidra ARM functions (`tools/analysis/build_metadata.py`); samples were confirmed as decoded
  instructions with REA. No Thumb `svc` was found. SVC names are from public 3dbrew documentation.
- `ipc_commands.json`: 339 `SendSyncRequest` sites; 324 command headers recovered from the stub that writes
  TLS+0x80 (command id = header>>16, normal/translate counts from the low bits). Three decoded results were
  cross-checked against REA's disassembly. Which *service* each targets is not yet established.
- `services.txt`: ExHeader service access list (declared access, not proof of use).

| SVC | Name (documented) | Sites |
|---|---|---|
| 0x01 | ControlMemory | 1 |
| 0x02 | QueryMemory | 1 |
| 0x03 | ExitProcess | 3 |
| 0x08 | CreateThread | 1 |
| 0x09 | ExitThread | 1 |
| 0x0a | SleepThread | 2 |
| 0x0b | GetThreadPriority | 1 |
| 0x0c | SetThreadPriority | 12 |
| 0x13 | CreateMutex | 1 |
| 0x14 | ReleaseMutex | 2 |
| 0x17 | CreateEvent | 1 |
| 0x18 | SignalEvent | 14 |
| 0x19 | ClearEvent | 29 |
| 0x1e | CreateMemoryBlock | 1 |
| 0x1f | MapMemoryBlock | 2 |
| 0x20 | UnmapMemoryBlock | 2 |
| 0x21 | CreateAddressArbiter | 1 |
| 0x22 | ArbitrateAddress | 1 |
| 0x23 | CloseHandle | 197 |
| 0x24 | WaitSynchronization1 | 49 |
| 0x25 | WaitSynchronizationN | 1 |
| 0x27 | DuplicateHandle | 1 |
| 0x28 | GetSystemTick | 63 |
| 0x29 | GetHandleInfo | 1 |
| 0x2b | GetProcessInfo | 1 |
| 0x2d | ConnectToPort | 1 |
| 0x32 | SendSyncRequest | 339 |
| 0x35 | GetProcessId | 1 |
| 0x37 | GetThreadId | 1 |
| 0x38 | GetResourceLimit | 1 |
| 0x3a | GetResourceLimitCurrentValues | 1 |
| 0x3c | Break | 1 |

## Second pass (scripts: `tools/analysis/attribute_services.py`, `find_tables.py`, `classify.py`)
- **IPC → service:** 155 stubs load their session handle from a global; 93 of those
  are attributed to a service by matching the global to the init function that references the service-name
  string (cam:u 34, y2r:u 21, ac:u 14, ir:USER 13, srv: 5, ndm:u 5, http:C 1). The attributed `srv:` headers (`0x10002`, `0x20000`, `0x90040`, `0xb0000`)
  match the public srv command list. The remaining stubs receive the handle as an argument (class methods)
  and need caller analysis; 5 handle globals have no matching init string.
- **Vtables:** 578 vtables / 1,044 virtual slots recovered from C++ RTTI (`vtables.json`, mangled class names
  from the binary). Spot-checked with REA (`ev_4a5cb0aef16c7cd3a77321a46376beb62478d9dde6a5a2b571aa07cb8143def5`).
- **Jump tables:** 511 dispatch sites found by instruction scan (`jump_tables.json`); one confirmed by REA with
  7 cases + default (`ev_11cba90f3201f2fc8f4f537191034e05e9dbc7614694fbddb7c9304fcfc6d49b`). Per-site
  case targets/bounds are not yet extracted for all (issue #7).
- **Literal pools:** 8,676 ranges / 73,044 bytes referenced by PC-relative loads in ARM functions (`classification.json`).
- **HLE vs recompile (mechanical rule, see script):** {'recompile': 16514, 'svc_stub': 92, 'ipc_stub': 300, 'sdk_virtual': 542}. `svc_stub`/`ipc_stub` are replaced by runtime HLE;
  `sdk_virtual` (virtuals of `nn::` classes) is decided per service later; everything else is recompiled.

## Not done (issue #4 remains open)
Name recovery for non-virtual functions, caller-based attribution for the remaining IPC stubs, per-site
jump-table targets, and the `build_reconstruction_obligation_ledger` / `evaluate_reconstruction_coverage`
workflow (it requires owner modules, verifier contracts and fixtures that exist only once the recompiler
does). Open questions are tracked in REA's unknown ledger (domains `hle-services`, `recompiler-analysis`).

## Third pass: block discovery and dispatch (issue #7)
- `jump_table_targets.json`: 434 bounded jump tables recovered by `recomp::recoverJumpTable` (4,726 targets),
  from the 511 dispatch sites found by the instruction scan. Pattern: `cmp rX,#N` then `ldrls/ldrcc pc,[pc,rX,lsl #2]`
  (LS: N+1 entries, CC: N entries) with the table at site+8. Cross-checked against REA's independent jump-table
  analysis: sites 0x10576c (`ev_11cba90f3201f2fc8f4f537191034e05e9dbc7614694fbddb7c9304fcfc6d49b`), 0x259ae4
  (`ev_fa30d15d9630e3f499f2fe2a27cbd878777720f413ad0702a70394b32de1b5c8`) and 0x4c4b64
  (`ev_170c4c0e39644ce4f7d4ef6a718ee6faabe963e9a38a97b7a1f2fcd31f7b51ea`) match exactly. At 0x4356ec
  (`ev_7858cec1fe975d6c54913b241db340cbeb15e7cac53dfafcaec7a8190a84eba6`) the table words read from the image
  (5 entries) agree with this recovery, while REA's medium-confidence list adds one extra target that appears in neither
  raw table (REA reports it cannot separate two switches there). The other 77 scan hits are data/false positives or use other patterns; they run through the interpreter
  fallback if ever reached.
- Whole-image block discovery (`recomp_discover`, local): 210,622 blocks / 947,956 instructions from the 17,448
  function entries plus recovered jump-table targets (2,509 Thumb blocks). Emitting C for every block (`recomp_translate`,
  local output only) takes ~5 s and compiles cleanly (clang -O1, 107 translation units, ~42 s on 8 cores).
- Running it (`boot_probe`, local): the recompiled image starts at its entry point, executes ~148k guest instructions
  (100% recompiled, 0 interpreted) and reaches the CTR SDK start-up SVCs (CreateAddressArbiter, QueryMemory,
  GetProcessInfo, ConnectToPort "srv:", SendSyncRequest with IPC header 0x10002 = srv:RegisterClient, GetResourceLimit,
  CloseHandle) before spinning in a loop that waits for another thread to publish a function pointer (needs the
  HLE scheduler, issue #9).
