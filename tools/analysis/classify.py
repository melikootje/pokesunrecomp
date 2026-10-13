#!/usr/bin/env python3
"""Literal pools and the HLE-vs-recompile classification (derived metadata only).

Classification rules (documented, mechanical):
  svc_stub     : function that contains an SVC site and is <= 0x60 bytes  -> replaced by runtime SVC HLE
  ipc_stub     : function that contains a SendSyncRequest site            -> replaced by runtime service HLE
  sdk_virtual  : virtual function of a class whose RTTI name is in namespace nn:: -> decided per service later
  recompile    : everything else
Usage: classify.py code.bin analysis_dir
"""
import bisect
import collections
import json
import struct
import sys

BASE, TEXT_END = 0x100000, 0x5BA000


def main(code_path, adir):
    code = open(code_path, "rb").read()
    syms = json.load(open(f"{adir}/symbols.json"))
    funcs = syms["functions"]
    starts = [f["addr"] for f in funcs]
    sites = json.load(open(f"{adir}/svc_sites.json"))
    vt = json.load(open(f"{adir}/vtables.json"))
    w = lambda a: struct.unpack_from("<I", code, a - BASE)[0]

    # Literal pool words: targets of PC-relative `ldr rX,[pc,#imm]` (ARM) in ARM-mode functions.
    pool = set()
    for f in funcs:
        if f["mode"] != "arm":
            continue
        end = f["addr"] + f["size"]
        for a in range(f["addr"], min(end, TEXT_END), 4):
            x = w(a)
            if (x & 0x0F7F0000) == 0x051F0000 and x >> 28 != 0xF:  # LDR (literal), P=1,B=0
                lit = a + 8 + (x & 0xFFF) if x & 0x00800000 else a + 8 - (x & 0xFFF)
                if BASE <= lit < BASE + len(code) - 3:
                    pool.add(lit)
    ranges = []
    for a in sorted(pool):
        if ranges and a == ranges[-1][1]:
            ranges[-1][1] = a + 4
        else:
            ranges.append([a, a + 4])

    cls = {}
    bysvc = collections.defaultdict(list)
    for s in sites:
        bysvc[s["func"]].append(s)
    fsize = {f["addr"]: f["size"] for f in funcs}
    for fa, ss in bysvc.items():
        if any(s["svc"] == 0x32 for s in ss):
            cls[fa] = "ipc_stub"
        elif fsize.get(fa, 1 << 30) <= 0x60:
            cls[fa] = "svc_stub"
    for v in vt:
        if v["class"].startswith("N2nn") or v["class"].startswith("N3nn"):
            for fp in v["virtuals"]:
                cls.setdefault(fp & ~1, "sdk_virtual")
    out = {f["addr"]: cls.get(f["addr"], "recompile") for f in funcs}
    counts = collections.Counter(out.values())
    json.dump({"literal_pool_ranges": ranges,
               "classification": {hex(k): v for k, v in out.items() if v != "recompile"},
               "counts": counts}, open(f"{adir}/classification.json", "w"), separators=(",", ":"))
    print(dict(counts), "pool_ranges=", len(ranges), "pool_bytes=", sum(b - a for a, b in ranges))


if __name__ == "__main__":
    main(*sys.argv[1:3])
