#!/usr/bin/env python3
"""Attribute IPC stubs to services (derived metadata only).

For each SendSyncRequest site, find the global that holds the session handle (a literal-pool address
loaded just before the svc). For each service-name string, find functions whose literal pool references the
string; the session-handle globals referenced by the same function are attributed to that service.
Usage: attribute_services.py code.bin analysis_dir
"""
import bisect
import collections
import json
import re
import struct
import sys

BASE, TEXT_END, RO_START, RO_END = 0x100000, 0x5BA000, 0x5BA000, 0x667000


def main(code_path, adir):
    code = open(code_path, "rb").read()
    syms = json.load(open(f"{adir}/symbols.json"))
    starts = [f["addr"] for f in syms["functions"]]
    ipc = json.load(open(f"{adir}/ipc_commands.json"))
    w = lambda a: struct.unpack_from("<I", code, a - BASE)[0]

    def owner(a):
        i = bisect.bisect_right(starts, a) - 1
        return starts[i] if i >= 0 else None

    # 1. handle global per stub: a `ldr rX,[pc,#imm]` literal later dereferenced by `ldr rX,[rX,#0]`
    #    (possibly a few instructions apart) shortly before the svc.
    for e in ipc:
        e["handle_global"] = None
        for back in range(1, 14):
            a = e["svc_addr"] - 4 * back
            if a < e["func"]:
                break
            x = w(a)
            if (x & 0xFFFF0000) == 0xE59F0000 and x & 0x00800000:
                rd = (x >> 12) & 0xF
                deref = 0xE5900000 | (rd << 16) | (rd << 12)
                if any((w(b) & 0xFFFFF000) == deref and (w(b) & 0xFFF) == 0
                       for b in range(a + 4, e["svc_addr"], 4)):
                    e["handle_global"] = w(a + 8 + (x & 0xFFF))
                    break

    # 2. service-name strings (C strings in rodata) that look like service ports.
    ro = code[RO_START - BASE:RO_END - BASE]
    names = {}
    for m in re.finditer(rb"(?<![\x20-\x7e])([A-Za-z0-9_$]{2,10}:[A-Za-z0-9_:]{0,8})\x00", ro):
        names[RO_START + m.start(1)] = m.group(1).decode()

    # 3. literal-pool references to those strings, by function.
    func_svc = collections.defaultdict(set)
    for a in range(BASE, TEXT_END, 4):
        v = w(a)
        if v in names:
            func_svc[owner(a)].add(names[v])
    # 4. per function, collect literal words that equal known handle globals.
    globals_ = {e["handle_global"] for e in ipc if e["handle_global"]}
    attribution = collections.defaultdict(collections.Counter)
    for f, svcs in func_svc.items():
        i = starts.index(f)
        end = starts[i + 1] if i + 1 < len(starts) else TEXT_END
        for a in range(f, end, 4):
            if w(a) in globals_:
                for s in svcs:
                    attribution[w(a)][s] += 1
    result = {}
    for g, c in attribution.items():
        result[g] = c.most_common(1)[0][0] if len(c) == 1 else None
    for e in ipc:
        g = e["handle_global"]
        e["service"] = result.get(g)
        e["handle_global"] = g
    json.dump(ipc, open(f"{adir}/ipc_commands.json", "w"), separators=(",", ":"))
    n = len(ipc)
    with_g = sum(1 for e in ipc if e["handle_global"])
    attributed = sum(1 for e in ipc if e["service"])
    print(f"ipc={n} with_handle_global={with_g} attributed={attributed} globals={len(globals_)} "
          f"attributed_globals={sum(1 for v in result.values() if v)} ambiguous={sum(1 for v in result.values() if not v)}")
    c = collections.Counter(e["service"] for e in ipc)
    for k, v in c.most_common(30):
        print(f"  {k}: {v}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
