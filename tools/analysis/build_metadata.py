#!/usr/bin/env python3
"""Derive metadata (addresses/sizes/numbers only) from a locally extracted code image.

Inputs  : code.bin (decompressed .code, base 0x100000), funcmap.tsv (from ExportFunctionMap.java)
Outputs : symbols.json, svc_sites.json, ipc_commands.json  (never any code bytes)
Policy  : docs/legal-policy.md -- do not commit anything but this derived metadata.
"""
import bisect
import collections
import json
import struct
import sys

BASE = 0x100000
TEXT_END = 0x5BA000

# Public SVC names (3dbrew "SVC"). Names are documentation, not recovered from the binary.
SVC_NAMES = {
    0x01: "ControlMemory", 0x02: "QueryMemory", 0x03: "ExitProcess", 0x08: "CreateThread",
    0x09: "ExitThread", 0x0A: "SleepThread", 0x0B: "GetThreadPriority", 0x0C: "SetThreadPriority",
    0x13: "CreateMutex", 0x14: "ReleaseMutex", 0x17: "CreateEvent", 0x18: "SignalEvent",
    0x19: "ClearEvent", 0x1E: "CreateMemoryBlock", 0x1F: "MapMemoryBlock", 0x20: "UnmapMemoryBlock",
    0x21: "CreateAddressArbiter", 0x22: "ArbitrateAddress", 0x23: "CloseHandle",
    0x24: "WaitSynchronization1", 0x25: "WaitSynchronizationN", 0x27: "DuplicateHandle",
    0x28: "GetSystemTick", 0x29: "GetHandleInfo", 0x2B: "GetProcessInfo", 0x2D: "ConnectToPort",
    0x32: "SendSyncRequest", 0x35: "GetProcessId", 0x37: "GetThreadId", 0x38: "GetResourceLimit",
    0x3A: "GetResourceLimitCurrentValues", 0x3C: "Break", 0x3D: "OutputDebugString",
}


def load_funcs(path):
    rows = []
    with open(path) as f:
        next(f)
        for line in f:
            a, size, mode, thunk = line.rstrip("\n").split("\t")
            rows.append({"addr": int(a, 16), "size": int(size), "mode": mode, "thunk": thunk == "1"})
    rows.sort(key=lambda r: r["addr"])
    return rows


def rd(code, addr):
    return struct.unpack_from("<I", code, addr - BASE)[0]


def main(code_path, funcmap, outdir):
    code = open(code_path, "rb").read()
    funcs = load_funcs(funcmap)
    starts = [f["addr"] for f in funcs]

    def owner(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i < 0:
            return None
        f = funcs[i]
        # Bodies can be non-contiguous; accept anything up to the next function start.
        return f

    sites = []
    # ARM: unconditional SVC inside an ARM-mode function.
    for off in range(0, TEXT_END - BASE, 4):
        w = struct.unpack_from("<I", code, off)[0]
        if w >> 24 == 0xEF and (w & 0xFFFFFF) <= 0x7D:
            f = owner(BASE + off)
            if f and f["mode"] == "arm":
                sites.append({"addr": BASE + off, "svc": w & 0xFFFFFF, "mode": "arm", "func": f["addr"]})
    # Thumb: SVC halfwords inside Thumb-mode function bodies.
    for f in funcs:
        if f["mode"] != "thumb":
            continue
        for a in range(f["addr"], f["addr"] + f["size"], 2):
            h = struct.unpack_from("<H", code, a - BASE)[0]
            if h >> 8 == 0xDF and (h & 0xFF) <= 0x7D:
                sites.append({"addr": a, "svc": h & 0xFF, "mode": "thumb", "func": f["addr"]})
    sites.sort(key=lambda s: s["addr"])
    for s in sites:
        s["name"] = SVC_NAMES.get(s["svc"])

    # IPC command headers: look back from each SendSyncRequest for `str rt,[rn,#0x80]` and resolve rt.
    ipc = []
    for s in (x for x in sites if x["svc"] == 0x32 and x["mode"] == "arm"):
        rt = None
        pos = None
        for back in range(1, 41):
            a = s["addr"] - 4 * back
            if a < s["func"]:
                break
            w = rd(code, a)
            if (w & 0xFFF00FFF) in (0xE5800080, 0xE5A00080):
                rt = (w >> 12) & 0xF
                pos = a
                break
        hdr = None
        how = None
        if rt is not None:
            for back in range(1, 41):
                a = pos - 4 * back
                if a < s["func"]:
                    break
                w = rd(code, a)
                if (w & 0xFFFFF000) == (0xE59F0000 | (rt << 12)) and (w & 0x00800000):
                    lit = a + 8 + (w & 0xFFF)
                    if BASE <= lit < BASE + len(code) - 3:
                        hdr, how = rd(code, lit), "literal"
                    break
                if (w & 0xFFFFF000) == (0xE3A00000 | (rt << 12)):
                    imm, rot = w & 0xFF, ((w >> 8) & 0xF) * 2
                    hdr, how = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm, "mov"
                    break
        e = {"svc_addr": s["addr"], "func": s["func"]}
        if hdr is not None:
            e.update(header=hdr, command_id=hdr >> 16, normal_params=(hdr >> 6) & 0x3F,
                     translate_params=hdr & 0x3F, source=how)
        ipc.append(e)

    out = {}
    out["symbols.json"] = {
        "image_base": BASE,
        "functions": [
            {"addr": f["addr"], "size": f["size"], "mode": f["mode"], **({"thunk": True} if f["thunk"] else {})}
            for f in funcs
        ],
    }
    out["svc_sites.json"] = sites
    out["ipc_commands.json"] = ipc
    for name, data in out.items():
        with open(f"{outdir}/{name}", "w") as f:
            json.dump(data, f, separators=(",", ":"))
    by = collections.Counter(s["svc"] for s in sites)
    resolved = sum(1 for e in ipc if "header" in e)
    print(f"functions={len(funcs)} svc_sites={len(sites)} distinct_svcs={len(by)} "
          f"ipc_sites={len(ipc)} ipc_headers_resolved={resolved}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
