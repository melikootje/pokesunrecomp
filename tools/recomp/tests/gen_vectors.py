#!/usr/bin/env python3
"""Generate reference vectors for the ARM recompiler tests using Unicorn (ARM11 MPCore model, i.e. the 3DS CPU).

Each vector is a short straight-line instruction sequence with a random initial state and the state Unicorn
produces. Vectors contain only synthetic instructions and random data (never game code).
Usage: gen_vectors.py <count> <seed> <out>
"""
import random
import sys

from unicorn import *
from unicorn.arm_const import *

CODE = 0x00100000
WIN = 0x20000000
WIN_SIZE = 0x10000
REGS = [UC_ARM_REG_R0 + i for i in range(13)] + [UC_ARM_REG_SP, UC_ARM_REG_LR]
PTR = (8, 9, 10)                       # registers reserved as memory base pointers
OFF = (11, 12)                         # registers reserved for memory offsets (never destinations)
DATA = [0, 1, 2, 3, 4, 5, 6, 7, 13, 14]  # destination registers that never clobber pointers/offsets


def memf(a):
    return ((a * 2654435761) & 0xFFFFFFFF) >> 24


rnd = random.Random()


def cond(force_al=False):
    return 14 if force_al or rnd.random() < 0.6 else rnd.randrange(0, 14)


def src():
    return rnd.choice([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]) if rnd.random() < 0.05 else rnd.choice(
        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14])


def dp():
    c = cond()
    opc = rnd.randrange(16)
    s = 1 if opc in (8, 9, 10, 11) else rnd.randrange(2)
    rd = rnd.choice(DATA) if opc not in (8, 9, 10, 11) else 0
    rn = src()
    kind = rnd.randrange(3)
    if kind == 0:
        op2 = (1 << 25) | (rnd.randrange(16) << 8) | rnd.randrange(256)
    elif kind == 1:
        op2 = (rnd.randrange(32) << 7) | (rnd.randrange(4) << 5) | src()
    else:
        # PC with a register-controlled shift is UNPREDICTABLE on ARM11: keep PC out of Rn/Rm/Rs
        rm_ = rnd.randrange(15)
        rn = rnd.randrange(15)
        op2 = (rnd.randrange(15) << 8) | (rnd.randrange(4) << 5) | (1 << 4) | rm_
    return (c << 28) | (opc << 21) | (s << 20) | (rn << 16) | (rd << 12) | op2, []


def mul():
    c = cond()
    kind = rnd.choice(["mul", "mla", "umull", "umlal", "smull", "smlal", "umaal"])
    s = rnd.randrange(2) if kind != "umaal" else 0
    rm, rs = rnd.randrange(15), rnd.randrange(15)
    if kind in ("mul", "mla"):
        rd = rnd.choice(DATA)
        if rd in (rm,):  # ARMv6 allows rd==rm but keep to the unambiguous cases
            rm = (rm + 1) % 15
        ra = rnd.randrange(15)
        w = (c << 28) | ((1 if kind == "mla" else 0) << 21) | (s << 20) | (rd << 16) | (ra << 12) | (rs << 8) | 0x90 | rm
        return w, []
    lo, hi = rnd.sample(DATA, 2)
    if lo in (rm,) or hi in (rm,):
        rm = rnd.choice([r for r in range(15) if r not in (lo, hi)])
    base = {"umaal": 0x00400090, "umull": 0x00800090, "umlal": 0x00A00090, "smull": 0x00C00090, "smlal": 0x00E00090}[kind]
    return (c << 28) | base | (s << 20) | (hi << 16) | (lo << 12) | (rs << 8) | rm, []


def ptrsetup(rn, extra=0):
    return {rn: WIN + 0x8000 + rnd.randrange(0, 0x200) * 8 + extra}


def ldst():
    c = cond()
    load = rnd.randrange(2)
    byte = rnd.randrange(2)
    pre = rnd.randrange(2)
    up = rnd.randrange(2)
    wb = rnd.randrange(2) if pre else 0
    rn = rnd.choice(PTR)
    rd = rnd.choice(DATA)
    setup = ptrsetup(rn)
    moves = wb or not pre          # base register is updated: keep it 8-byte aligned afterwards
    if rnd.random() < 0.5:
        off = rnd.randrange(0x80) * 8 if (moves or not byte) else rnd.randrange(0x400)
        if not byte and not moves:
            off = rnd.randrange(0x100) * 4
        w = (c << 28) | (0b010 << 25) | (pre << 24) | (up << 23) | (byte << 22) | (wb << 21) | (load << 20) | (rn << 16) | (rd << 12) | off
    else:
        rm = rnd.choice(OFF)
        shtype = rnd.randrange(3)          # LSL / LSR / ASR on small positive values (no ROR/RRX: unaligned risk)
        amt = rnd.randrange(0, 4) if shtype == 0 else rnd.randrange(1, 4)
        setup[rm] = 64 * rnd.randrange(0, 0x10)   # multiples of 64 stay multiples of 8 after LSR by <= 3
        w = (c << 28) | (0b011 << 25) | (pre << 24) | (up << 23) | (byte << 22) | (wb << 21) | (load << 20) | (rn << 16) | (rd << 12) | (amt << 7) | (shtype << 5) | rm
    if (wb or not pre) and rd == rn:
        return None
    return w, [setup]


def ldsth():
    c = cond()
    sh = rnd.choice([1, 2, 3])
    load = rnd.randrange(2)
    dword = sh in (2, 3) and not (sh == 2 and load) and not (sh == 3 and load)  # LDRD (sh=2,L=0) / STRD (sh=3,L=0)
    if sh == 2:
        dword = not load
    if sh == 3:
        dword = not load
    pre = rnd.randrange(2)
    up = rnd.randrange(2)
    wb = rnd.randrange(2) if pre else 0
    if dword and (wb or not pre):
        return None
    rn = rnd.choice(PTR)
    if dword:
        rd = rnd.choice([r for r in DATA if r % 2 == 0 and r != 14 and r + 1 in DATA])
    else:
        rd = rnd.choice(DATA)
    setup = ptrsetup(rn)
    moves = wb or not pre
    if rnd.randrange(2):
        mult = 8 if (moves or dword) else (2 if sh != 2 else 1)
        off = rnd.randrange(0, 0x20) * mult
        w = (c << 28) | (pre << 24) | (up << 23) | (1 << 22) | (wb << 21) | (load << 20) | (rn << 16) | (rd << 12) | ((off >> 4) << 8) | (sh << 5) | 0x90 | (off & 0xF)
    else:
        rm = rnd.choice(OFF)
        setup[rm] = 64 * rnd.randrange(0, 0x10)
        w = (c << 28) | (pre << 24) | (up << 23) | (wb << 21) | (load << 20) | (rn << 16) | (rd << 12) | (sh << 5) | 0x90 | rm
    if (wb or not pre) and rd == rn:
        return None
    if dword and rn in (rd, rd + 1):
        return None
    return w, [setup]


def ldmstm():
    c = cond()
    load = rnd.randrange(2)
    pre = rnd.randrange(2)
    up = rnd.randrange(2)
    wb = rnd.randrange(2)
    rn = rnd.choice(PTR)
    regs = sorted(rnd.sample(DATA, rnd.randrange(1, 6)))
    if wb and len(regs) % 2:
        regs = regs[:-1] or None
        if regs is None:
            return None
    if load:
        if wb and rn in regs:
            return None
    else:
        if wb and rn in regs:
            return None
    lst = 0
    for r in regs:
        lst |= 1 << r
    w = (c << 28) | (0b100 << 25) | (pre << 24) | (up << 23) | (wb << 21) | (load << 20) | (rn << 16) | lst
    return w, [ptrsetup(rn)]


def swp():
    c = cond()
    byte = rnd.randrange(2)
    rn = rnd.choice(PTR)
    rd, rm = rnd.choice(DATA), rnd.choice(DATA)
    w = (c << 28) | (0x01000090) | (byte << 22) | (rn << 16) | (rd << 12) | rm
    return w, [ptrsetup(rn)]


def excl_pair():
    c = cond(True)
    kind = rnd.choice(["w", "b", "h", "d"])
    rn = rnd.choice(PTR)
    rt = rnd.choice([r for r in DATA if r % 2 == 0 and r != 14] if kind == "d" else DATA)
    rs = rnd.choice([r for r in DATA if r not in (rt, rt + 1)])
    rv = rnd.choice([r for r in DATA if r % 2 == 0 and r != 14 and r not in (rs, rt)] if kind == "d" else [r for r in DATA if r != rs])
    ld = {"w": 0x01900F9F, "b": 0x01D00F9F, "h": 0x01F00F9F, "d": 0x01B00F9F}[kind]
    st = {"w": 0x01800F90, "b": 0x01C00F90, "h": 0x01E00F90, "d": 0x01A00F90}[kind]
    setup = ptrsetup(rn)
    if kind == "d":
        setup = {rn: WIN + 0x8000 + rnd.randrange(0x100) * 8}
    w1 = (c << 28) | ld | (rn << 16) | (rt << 12)
    w2 = (c << 28) | st | (rn << 16) | (rs << 12) | rv
    if rn in (rt, rs, rv):
        return None
    if kind == "d" and (rt + 1 == rs or rv + 1 == rs):
        return None
    if rnd.random() < 0.15:
        return [(w1, [setup]), (0xF57FF01F, []), (w2, [])]  # CLREX in between: STREX must fail
    return [(w1, [setup]), (w2, [])]


def misc():
    c = cond()
    k = rnd.choice(["clz", "rev", "rev16", "revsh", "sxtb", "sxth", "uxtb", "uxth", "sxtab", "sxtah", "uxtab", "uxtah", "tls"])
    rd, rm = rnd.choice(DATA), rnd.randrange(15)
    if k == "clz":
        return (c << 28) | 0x016F0F10 | (rd << 12) | rm, []
    if k == "rev":
        return (c << 28) | 0x06BF0F30 | (rd << 12) | rm, []
    if k == "rev16":
        return (c << 28) | 0x06BF0FB0 | (rd << 12) | rm, []
    if k == "revsh":
        return (c << 28) | 0x06FF0FB0 | (rd << 12) | rm, []
    if k == "tls":
        return (c << 28) | 0x0E1D0F70 | (rd << 12), []
    rot = rnd.randrange(4)
    base = {"sxtb": 0x06AF0070, "sxth": 0x06BF0070, "uxtb": 0x06EF0070, "uxth": 0x06FF0070,
            "sxtab": 0x06A00070, "sxtah": 0x06B00070, "uxtab": 0x06E00070, "uxtah": 0x06F00070}[k]
    rn = rnd.randrange(15) if k.endswith("ab") or k.endswith("ah") else 15
    if (k.endswith("ab") or k.endswith("ah")) and rn == 15:
        rn = 0
    return (c << 28) | (base & 0xFFF0FFFF) | (rn << 16) | (rd << 12) | (rot << 10) | rm, []


def branch():
    c = cond()
    k = rnd.choice(["b", "bl", "bx", "blx"])
    if k in ("b", "bl"):
        off = rnd.randrange(-0x1000, 0x1000) & 0xFFFFFF
        return (c << 28) | (0b101 << 25) | ((1 if k == "bl" else 0) << 24) | off, []
    rm = rnd.randrange(15)
    return (c << 28) | (0x012FFF10 if k == "bx" else 0x012FFF30) | rm, [{"align": rm}]


GENS = [(dp, 40), (mul, 8), (ldst, 12), (ldsth, 8), (ldmstm, 6), (swp, 2), (excl_pair, 3), (misc, 8)]


def one_insn():
    gens, weights = zip(*GENS)
    while True:
        g = rnd.choices(gens, weights)[0]
        r = g()
        if r is None:
            continue
        return r


def make_vector():
    n = rnd.randrange(1, 6)
    seq = []
    setups = {}
    while len(seq) < n:
        r = one_insn()
        items = r if isinstance(r, list) else [r]
        for w, sl in items:
            seq.append(w)
            for s in sl:
                setups.update(s)
    if rnd.random() < 0.15:
        w, sl = branch()
        seq.append(w)
        for s in sl:
            if "align" in s:
                # make the target aligned/interworking-safe: low two bits 00 or bit0 = 1 with 2-byte alignment
                setups["bxreg"] = s["align"]
            else:
                setups.update(s)
    return seq, setups


def run(seq, setups):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.ctl_set_cpu_model(UC_CPU_ARM_11MPCORE)
    uc.mem_map(CODE, 0x1000)
    uc.mem_map(WIN, WIN_SIZE)
    uc.mem_write(WIN, bytes(memf(WIN + i) for i in range(WIN_SIZE)))
    init = [rnd.getrandbits(32) for _ in range(15)]
    for k, v in setups.items():
        if k == "bxreg":
            continue
        init[k] = v & 0xFFFFFFFF
    if "bxreg" in setups:
        t = setups["bxreg"]
        tgt = rnd.choice([0x00400000, 0x00400001, 0x00800004, 0x00800005])
        init[t] = tgt
    flags = rnd.getrandbits(4)
    tls = rnd.getrandbits(32) & ~3
    for i, r in enumerate(REGS):
        uc.reg_write(r, init[i])
    cpsr = uc.reg_read(UC_ARM_REG_CPSR)
    cpsr = (cpsr & 0x0FFFFFFF) | (flags << 28)
    uc.reg_write(UC_ARM_REG_CPSR, cpsr)
    try:
        uc.reg_write(UC_ARM_REG_C13_C0_3, tls)
    except Exception:
        pass
    code = b"".join(w.to_bytes(4, "little") for w in seq)
    uc.mem_write(CODE, code)
    try:
        uc.emu_start(CODE, CODE + 4 * len(seq), count=len(seq))
    except UcError:
        return None
    out = [uc.reg_read(r) for r in REGS]
    pc = uc.reg_read(UC_ARM_REG_PC)
    cpsr = uc.reg_read(UC_ARM_REG_CPSR)
    after = bytes(uc.mem_read(WIN, WIN_SIZE))
    diffs = [(WIN + i, b) for i, b in enumerate(after) if b != memf(WIN + i)]
    return init, flags, tls, out, pc, (cpsr >> 28) & 0xF, (cpsr >> 5) & 1, diffs


def main():
    count, seed, path = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    rnd.seed(seed)
    done = 0
    with open(path, "w") as f:
        f.write("# vectors: N words | init r0-r14 | nzcv tls | final r0-r14 pc nzcv thumb | ndiffs (addr:byte)*\n")
        while done < count:
            seq, setups = make_vector()
            res = run(seq, setups)
            if res is None:
                continue
            init, flags, tls, out, pc, nzcv, thumb, diffs = res
            h = lambda xs: " ".join("%08x" % x for x in xs)
            f.write("V %d %s | %s %x %08x | %s %08x %x %d | %d %s\n" % (
                len(seq), h(seq), h(init), flags, tls, h(out), pc, nzcv, thumb, len(diffs),
                " ".join("%08x:%02x" % d for d in diffs)))
            done += 1


if __name__ == "__main__":
    main()
