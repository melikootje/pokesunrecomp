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

## Not done (issue #4 remains open)
Vtables/jump tables/literal pools, name recovery from RTTI, service attribution for IPC sites, HLE-vs-recompile
split of CTR-SDK code, `symbols.json` names, and the obligation-ledger/coverage workflow. Open questions are
tracked in REA's unknown ledger (domain `hle-services`, `recompiler-analysis`).
