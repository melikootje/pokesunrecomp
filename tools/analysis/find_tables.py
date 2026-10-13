#!/usr/bin/env python3
"""Recover vtables (from C++ RTTI) and jump-table dispatch sites. Output: derived metadata only.
Usage: find_tables.py code.bin analysis_dir
"""
import bisect
import json
import re
import struct
import sys

BASE, TEXT_END, RO_END, DATA_END = 0x100000, 0x5BA000, 0x667000, 0x6D41BC


def main(code_path, adir):
    code = open(code_path, "rb").read()
    syms = json.load(open(f"{adir}/symbols.json"))
    fstarts = {f["addr"] for f in syms["functions"]}
    w = lambda a: struct.unpack_from("<I", code, a - BASE)[0]
    in_img = lambda a: BASE <= a <= BASE + len(code) - 4

    # Class-name strings (Itanium mangled type names) in rodata.
    names = {}
    for m in re.finditer(rb"(?<![\x20-\x7e])(N?[0-9]+[A-Za-z_][A-Za-z0-9_]*[A-Za-z0-9_E])\x00", code[TEXT_END - BASE:RO_END - BASE]):
        names[TEXT_END + m.start(1)] = m.group(1).decode()
    # Word -> addresses holding it, over rodata+data (aligned).
    holders = {}
    for a in range(TEXT_END, min(DATA_END, BASE + len(code)) - 3, 4):
        holders.setdefault(w(a), []).append(a)
    # typeinfo object T has name pointer at T+4.
    typeinfos = {}
    for na, nm in names.items():
        for h in holders.get(na, []):
            typeinfos[h - 4] = nm
    vtables = []
    for t, nm in typeinfos.items():
        for h in holders.get(t, []):
            if h < 4 or not in_img(h - 4) or w(h - 4) != 0:
                continue  # vtable begins with offset-to-top (0 for primary base), then typeinfo ptr
            virt = []
            a = h + 4
            while in_img(a):
                v = w(a)
                if (v & ~1) in fstarts and (v & ~1) >= BASE and (v & ~1) < TEXT_END:
                    virt.append(v)
                    a += 4
                else:
                    break
            if virt:
                vtables.append({"class": nm, "typeinfo": t, "vtable": h - 4, "virtuals": virt})
    vtables.sort(key=lambda v: v["vtable"])

    # Jump-table dispatches: ldr pc,[pc,rm,lsl #2]  /  add pc,pc,rm,lsl #2
    jt = []
    for a in range(BASE, TEXT_END, 4):
        x = w(a)
        if (x & 0x0FFFFFF0) == 0x079FF100 or (x & 0x0FFFFFF0) == 0x008FF100:
            if x >> 28 != 0xF:
                jt.append({"addr": a, "kind": "ldr_pc" if (x & 0x0FF00000) == 0x07900000 else "add_pc"})
    json.dump(vtables, open(f"{adir}/vtables.json", "w"), separators=(",", ":"))
    json.dump(jt, open(f"{adir}/jump_tables.json", "w"), separators=(",", ":"))
    print(f"typeinfo_names={len(names)} typeinfos={len(typeinfos)} vtables={len(vtables)} "
          f"virtual_slots={sum(len(v['virtuals']) for v in vtables)} jump_table_sites={len(jt)}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
