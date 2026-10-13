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
OFF = (11,)                         # registers reserved for memory offsets (never destinations)
DATA = [0, 1, 2, 3, 4, 5, 6, 7, 13, 14]  # destination registers that never clobber pointers/offsets


def memf(a):
    return ((a * 2654435761) & 0xFFFFFFFF) >> 24


rnd = random.Random()
VFP_GENS = set()
VEC_LEN = 0   # FPSCR.LEN used by the current vector (chosen before instructions are generated)
FORCE_KIND = None
ONLY_MEDIA = False
ONLY_VFP = None
THUMB_FRAC = 0.2


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
            off = rnd.randrange(0x100) * 4 if rnd.random() < 0.6 else rnd.randrange(0x400)   # unaligned: true unaligned access
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
    k = rnd.choice(["b", "bl", "bx", "blx", "svc"])
    if k == "svc":
        return (c << 28) | 0x0F000000 | rnd.randrange(0x1000000), []
    if k in ("b", "bl"):
        off = rnd.randrange(-0x1000, 0x1000) & 0xFFFFFF
        return (c << 28) | (0b101 << 25) | ((1 if k == "bl" else 0) << 24) | off, []
    rm = 12                                   # reserved branch-target register (never a destination or offset)
    return (c << 28) | (0x012FFF10 if k == "bx" else 0x012FFF30) | rm, [{"align": rm}]


def media():
    c = cond()
    k = FORCE_KIND or rnd.choice(["par"] * 6 + ["sel", "pkh", "ssat", "usat", "ssat16", "usat16", "ext16", "usad", "smlad", "smlald", "smmul",
                                 "qop", "smxy", "smw", "smlalxy"])
    rd = rnd.choice(DATA)
    rn, rm, rs = rnd.randrange(15), rnd.randrange(15), rnd.randrange(15)
    if k == "par":
        pfx = rnd.choice([1, 2, 3, 5, 6, 7])
        op2 = rnd.choice([0, 1, 2, 3, 4, 7])
        return (c << 28) | ((0x60 | pfx) << 20) | (rn << 16) | (rd << 12) | 0xF00 | (op2 << 5) | 0x10 | rm, []
    if k == "sel":
        return (c << 28) | 0x06800FB0 | (rn << 16) | (rd << 12) | rm, []
    if k == "pkh":
        return (c << 28) | (0x06800050 if rnd.randrange(2) else 0x06800010) | (rn << 16) | (rd << 12) | (rnd.randrange(32) << 7) | rm, []
    if k in ("ssat", "usat"):
        base = 0x06A00010 if k == "ssat" else 0x06E00010
        return (c << 28) | base | (rnd.randrange(32) << 16) | (rd << 12) | (rnd.randrange(32) << 7) | (rnd.randrange(2) << 6) | rm, []
    if k in ("ssat16", "usat16"):
        base = 0x06A00F30 if k == "ssat16" else 0x06E00F30
        return (c << 28) | base | (rnd.randrange(16) << 16) | (rd << 12) | rm, []
    if k == "ext16":
        base = rnd.choice([0x068F0070, 0x06CF0070, 0x06800070, 0x06C00070])
        if (base & 0xF0000) != 0xF0000 or True:
            rn_ = 15 if (base & 0xF0000) == 0xF0000 else rnd.randrange(15)
        return (c << 28) | (base & 0xFFF0FFFF) | (rn_ << 16) | (rd << 12) | (rnd.randrange(4) << 10) | rm, []
    if k == "usad":
        if rnd.randrange(2):
            return (c << 28) | 0x0780F010 | (rd << 16) | (rs << 8) | rm, []
        return (c << 28) | 0x07800010 | (rd << 16) | (rnd.randrange(15) << 12) | (rs << 8) | rm, []
    if k == "smlad":
        base = rnd.choice([0x07000010, 0x07000050])
        ra = 15 if rnd.randrange(2) else rnd.randrange(15)
        return (c << 28) | base | (rnd.randrange(2) << 5) | (rd << 16) | (ra << 12) | (rs << 8) | rm, []
    if k == "smlald":
        lo, hi = rnd.sample(DATA, 2)
        base = rnd.choice([0x07400010, 0x07400050])
        return (c << 28) | base | (rnd.randrange(2) << 5) | (hi << 16) | (lo << 12) | (rs << 8) | rm, []
    if k == "smmul":
        which = rnd.choice(["mla", "mul", "mls"])
        ra = 15 if which == "mul" else rnd.randrange(15)
        base = 0x075000D0 if which == "mls" else 0x07500010
        return (c << 28) | base | (rnd.randrange(2) << 5) | (rd << 16) | (ra << 12) | (rs << 8) | rm, []
    if k == "qop":
        return (c << 28) | 0x01000050 | (rnd.randrange(4) << 21) | (rn << 16) | (rd << 12) | rm, []
    if k == "smxy":
        x, y = rnd.randrange(2), rnd.randrange(2)
        if rnd.randrange(2):
            return (c << 28) | 0x01000080 | (y << 6) | (x << 5) | (rd << 16) | (rnd.randrange(15) << 12) | (rs << 8) | rm, []
        return (c << 28) | 0x01600080 | (y << 6) | (x << 5) | (rd << 16) | (rs << 8) | rm, []
    if k == "smw":
        y = rnd.randrange(2)
        if rnd.randrange(2):
            return (c << 28) | 0x01200080 | (y << 6) | (rd << 16) | (rnd.randrange(15) << 12) | (rs << 8) | rm, []
        return (c << 28) | 0x012000A0 | (y << 6) | (rd << 16) | (rs << 8) | rm, []
    lo, hi = rnd.sample(DATA, 2)
    return (c << 28) | 0x01400080 | (rnd.randrange(2) << 6) | (rnd.randrange(2) << 5) | (hi << 16) | (lo << 12) | (rs << 8) | rm, []



# ---------------------------------------------------------------- VFP
F32_SPECIAL = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001, 0xFFC00001,
               0x00000001, 0x007FFFFF, 0x00800000, 0x7F7FFFFF, 0x80000001, 0x807FFFFF, 0x40000000, 0x3F000000, 0x4B000000,
               0xCF000000, 0x4F000000, 0x4F800000, 0x3F7FFFFF, 0x7FFFFFFF, 0xFFFFFFFF, 0x40490FDB, 0xC0490FDB]
F64_SPECIAL = [0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x7FF0000000000000,
               0xFFF0000000000000, 0x7FF8000000000000, 0x7FF0000000000001, 0xFFF8000000000001, 0x0000000000000001,
               0x000FFFFFFFFFFFFF, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0x8000000000000001, 0x4000000000000000,
               0x3FE0000000000000, 0x41E0000000000000, 0xC1E0000000000000, 0x41F0000000000000, 0x3810000000000000,
               0x47E0000000000000, 0x36A0000000000000]


def f32_val():
    c = rnd.random()
    if c < 0.30:
        return rnd.choice(F32_SPECIAL)
    if c < 0.75:
        return (rnd.getrandbits(1) << 31) | (rnd.randrange(1, 254) << 23) | rnd.getrandbits(23)
    if c < 0.85:
        return (rnd.getrandbits(1) << 31) | rnd.getrandbits(23)                       # denormal / zero
    if c < 0.93:
        return (rnd.getrandbits(1) << 31) | (rnd.randrange(100, 154) << 23) | rnd.getrandbits(23)  # moderate range
    return rnd.getrandbits(32)


def f64_val():
    c = rnd.random()
    if c < 0.30:
        return rnd.choice(F64_SPECIAL)
    if c < 0.75:
        return (rnd.getrandbits(1) << 63) | (rnd.randrange(1, 2046) << 52) | rnd.getrandbits(52)
    if c < 0.85:
        return (rnd.getrandbits(1) << 63) | rnd.getrandbits(52)
    if c < 0.93:
        return (rnd.getrandbits(1) << 63) | (rnd.randrange(1000, 1046) << 52) | rnd.getrandbits(52)
    return rnd.getrandbits(64)


def vfp_state():
    s = []
    for _ in range(16):
        if rnd.random() < 0.5:
            v = f64_val()
            s += [v & 0xFFFFFFFF, v >> 32]
        else:
            s += [f32_val(), f32_val()]
    fpscr = rnd.getrandbits(4) << 28
    if rnd.random() < 0.5:
        fpscr |= 1 << 25          # DN
    if rnd.random() < 0.5:
        fpscr |= 1 << 24          # FZ
    fpscr |= rnd.choice([0, 0, 0, 1, 2, 3]) << 22
    # STRIDE is always 1 (field 0): the reference model mis-implements stride 2 (see docs/vfp-notes.md)
    fpscr |= VEC_LEN << 16
    fpscr |= rnd.getrandbits(5) | (rnd.getrandbits(1) << 7)
    return s, fpscr


def sreg(n):
    return (n >> 1) & 0xF, n & 1       # (Vx, X-bit) for an S register number


def vfp_data():
    c = cond()
    dbl = rnd.randrange(2)
    rmax = 16 if dbl else 32
    if VEC_LEN > 0 and dbl:
        VEC_LEN_DBL = min(VEC_LEN, 3)
    if rnd.random() < 0.55:
        pqrs = rnd.randrange(9)
        d, n, m = rnd.randrange(rmax), rnd.randrange(rmax), rnd.randrange(rmax)
        if VEC_LEN > 0 and dbl:
            m = rnd.randrange(4)                       # scalar Fm (bank 0)
            n = rnd.randrange(4)
        if dbl:
            vd, D, vn, N, vm, M = d, 0, n, 0, m, 0
        else:
            (vd, D), (vn, N), (vm, M) = sreg(d), sreg(n), sreg(m)
        return (c << 28) | 0x0E000A00 | (dbl << 8) | (((pqrs >> 3) & 1) << 23) | (((pqrs >> 2) & 1) << 21) | (((pqrs >> 1) & 1) << 20) \
            | ((pqrs & 1) << 6) | (D << 22) | (vn << 16) | (vd << 12) | (N << 7) | (M << 5) | vm, []
    op = rnd.choice(["mov", "abs", "neg", "sqrt", "cmp", "cmpe", "cmpz", "cmpez", "cvtds", "i2f", "f2i"])
    base = 0x0EB00A00 | (dbl << 8)
    opc2, opc3 = {"mov": (0, 1), "abs": (0, 3), "neg": (1, 1), "sqrt": (1, 3), "cmp": (4, 1), "cmpe": (4, 3), "cmpz": (5, 1),
                  "cmpez": (5, 3), "cvtds": (7, 3), "i2f": (8, 1 | (rnd.randrange(2) << 1)),
                  "f2i": (12 + rnd.randrange(2), 1 | (rnd.randrange(2) << 1))}[op]
    if op == "cvtds":
        if dbl:
            d, m = rnd.randrange(32), rnd.randrange(16)
            (vd, D), (vm, M) = sreg(d), (m, 0)
        else:
            d, m = rnd.randrange(16), rnd.randrange(32)
            (vd, D), (vm, M) = (d, 0), sreg(m)
    elif op == "i2f":
        d = rnd.randrange(rmax)
        (vd, D) = (d, 0) if dbl else sreg(d)
        (vm, M) = sreg(rnd.randrange(32))
    elif op == "f2i":
        (vd, D) = sreg(rnd.randrange(32))
        m = rnd.randrange(rmax)
        (vm, M) = (m, 0) if dbl else sreg(m)
    elif op in ("cmpz", "cmpez"):
        d = rnd.randrange(rmax)
        (vd, D) = (d, 0) if dbl else sreg(d)
        vm, M = 0, 0
    else:
        d, m = rnd.randrange(rmax), rnd.randrange(rmax)
        if VEC_LEN > 0 and dbl:
            m = rnd.randrange(4)
        if dbl:
            vd, D, vm, M = d, 0, m, 0
        else:
            (vd, D), (vm, M) = sreg(d), sreg(m)
    return (c << 28) | base | (D << 22) | (opc2 << 16) | (vd << 12) | ((opc3 >> 1) << 7) | ((opc3 & 1) << 6) | (M << 5) | vm, []


def vfp_xfer():
    c = cond()
    k = rnd.choice(["rs", "rr", "mrs", "msr", "ldst", "ldm"])
    rt = rnd.choice(DATA)
    if k == "rs":
        n = rnd.randrange(32)
        vn, N = sreg(n)
        return (c << 28) | 0x0E000A10 | (rnd.randrange(2) << 20) | (vn << 16) | (rt << 12) | (N << 7), []
    if k == "rr":
        dbl = rnd.randrange(2)
        rt2 = rnd.choice([r for r in DATA if r != rt])
        L = rnd.randrange(2)
        if dbl:
            return (c << 28) | 0x0C400B10 | (L << 20) | (rt2 << 16) | (rt << 12) | rnd.randrange(16), []
        m = rnd.randrange(31)
        vm, M = sreg(m)
        return (c << 28) | 0x0C400A10 | (L << 20) | (rt2 << 16) | (rt << 12) | (M << 5) | vm, []
    if k == "mrs":
        reg = rnd.choice([1, 1, 1, 0, 8])
        rt_ = 15 if (reg == 1 and rnd.random() < 0.4) else rt
        return (c << 28) | 0x0EF00A10 | (reg << 16) | (rt_ << 12), []
    if k == "msr":
        # source register is reserved and preloaded with a valid FPSCR (stride 1; see gen notes on the reference model)
        rs_ = rnd.choice(OFF)
        val = (rnd.getrandbits(4) << 28) | (rnd.getrandbits(2) << 24) | (rnd.choice([0, 0, 0, 1, 2, 3]) << 22) \
            | (VEC_LEN << 16) | rnd.getrandbits(5) | (rnd.getrandbits(1) << 7)   # LEN stays the vector's LEN
        return (c << 28) | 0x0EE00A10 | (1 << 16) | (rs_ << 12), [{rs_: val}]
    rn = rnd.choice(PTR)
    setup = ptrsetup(rn)
    dbl = rnd.randrange(2)
    L = rnd.randrange(2)
    if k == "ldst":
        n = rnd.randrange(16 if dbl else 32)
        vd, D = (n, 0) if dbl else sreg(n)
        return (c << 28) | 0x0D000A00 | (dbl << 8) | (rnd.randrange(2) << 23) | (D << 22) | (L << 20) | (rn << 16) | (vd << 12) | rnd.randrange(0x40), [setup]
    cnt = rnd.randrange(1, 6)
    if dbl:
        first = rnd.randrange(0, 16 - cnt + 1)
        vd, D, imm = first, 0, 2 * cnt
    else:
        first = rnd.randrange(0, 32 - cnt + 1)
        vd, D = sreg(first)
        imm = cnt
    if rnd.randrange(2):
        base = 0x0C800A00 | (rnd.randrange(2) << 21)          # IA, optional writeback
    else:
        base = 0x0D200A00                                      # DB with writeback (VPUSH-like)
    return (c << 28) | base | (dbl << 8) | (D << 22) | (L << 20) | (rn << 16) | (vd << 12) | imm, [setup]


GENS = [(dp, 40), (mul, 8), (ldst, 12), (ldsth, 8), (ldmstm, 6), (swp, 2), (excl_pair, 3), (misc, 8), (media, 22), (vfp_data, 30), (vfp_xfer, 10)]


def one_insn():
    gens, weights = zip(*GENS)
    while True:
        g = rnd.choices(gens, weights)[0]
        r = g()
        if r is None:
            continue
        if g.__name__.startswith('vfp'):
            r = (r[0], r[1] + [{'__vfp__': 1}])
        return r



# ---------------------------------------------------------------- Thumb
PTR_T = (5, 6, 7)       # base pointers
OFF_T = (3, 4)          # offset registers (never destinations)
DATA_T = (0, 1, 2)      # destinations
HI_T = (8, 9, 10, 11, 12)


def t_pick_src():
    return rnd.randrange(8)


def t_shift_imm():
    return [[(rnd.randrange(3) << 11) | (rnd.randrange(32) << 6) | (t_pick_src() << 3) | rnd.choice(DATA_T)]], {}


def t_addsub():
    sub, imm = rnd.randrange(2), rnd.randrange(2)
    return [[0x1800 | (imm << 10) | (sub << 9) | (rnd.randrange(8) << 6) | (t_pick_src() << 3) | rnd.choice(DATA_T)]], {}


def t_imm8():
    op = rnd.randrange(4)
    rd = rnd.randrange(8) if op == 1 else rnd.choice(DATA_T)
    return [[0x2000 | (op << 11) | (rd << 8) | rnd.randrange(256)]], {}


def t_alu():
    op = rnd.randrange(16)
    rd = rnd.randrange(8) if op in (8, 10, 11) else rnd.choice(DATA_T)
    return [[0x4000 | (op << 6) | (t_pick_src() << 3) | rd]], {}


def t_hi():
    op = rnd.choice([0, 1, 2])                    # ADD, CMP, MOV
    rd = rnd.choice([0, 1, 2, 8, 9, 10, 11, 14]) if op != 1 else rnd.choice([0, 1, 2, 3, 8, 9, 12, 14])   # r12 is reserved for BX targets
    rm = rnd.choice(list(range(15)) + [15])
    if rd < 8 and rm < 8:
        rm = rnd.choice([8, 9, 10, 11, 12, 14, 15])
    if op == 2 and rd == 14 and False:
        pass
    h = 0x4400 | (op << 8) | ((rd >> 3) << 7) | ((rm >> 3) << 6) | ((rm & 7) << 3) | (rd & 7)
    return [[h]], {}


def t_ldr_lit():
    return [[0x4800 | (rnd.choice(DATA_T) << 8) | rnd.randrange(256)]], {}


def t_ldst_reg():
    op = rnd.randrange(8)
    rn, rm = rnd.choice(PTR_T), rnd.choice(OFF_T)
    rd = rnd.choice(DATA_T)
    return [[0x5000 | (op << 9) | (rm << 6) | (rn << 3) | rd]], {rn: ptr_value(), rm: 64 * rnd.randrange(0x10)}


def ptr_value():
    return WIN + 0x8000 + rnd.randrange(0, 0x200) * 8


def t_ldst_imm():
    kind = rnd.randrange(3)       # word, byte, halfword
    L = rnd.randrange(2)
    rn, rd = rnd.choice(PTR_T), rnd.choice(DATA_T)
    imm5 = rnd.randrange(32)
    base = {0: 0x6000, 1: 0x7000, 2: 0x8000}[kind]
    return [[base | (L << 11) | (imm5 << 6) | (rn << 3) | rd]], {rn: ptr_value()}


def t_sp_rel():
    return [[0x9000 | (rnd.randrange(2) << 11) | (rnd.choice(DATA_T) << 8) | rnd.randrange(256)]], {}


def t_addr():
    return [[0xA000 | (rnd.randrange(2) << 11) | (rnd.choice(DATA_T) << 8) | rnd.randrange(256)]], {}


def t_sp_adj():
    return [[0xB000 | (rnd.randrange(2) << 7) | rnd.randrange(0x20)]], {}


def t_push():
    lst = rnd.randrange(1, 256)
    return [[0xB400 | (rnd.randrange(2) << 8) | lst]], {}


def t_pop():
    lst = rnd.randrange(0, 8)          # r0-r2 only: never clobbers pointers
    if lst == 0:
        lst = 1
    return [[0xBC00 | lst]], {}


def t_ext():
    if rnd.randrange(2):
        return [[0xB200 | (rnd.randrange(4) << 6) | (t_pick_src() << 3) | rnd.choice(DATA_T)]], {}
    return [[0xBA00 | (rnd.choice([0, 1, 3]) << 6) | (t_pick_src() << 3) | rnd.choice(DATA_T)]], {}


def t_ldmstm():
    L = rnd.randrange(2)
    rn = rnd.choice(PTR_T)
    lst = rnd.randrange(1, 8) if L else rnd.randrange(1, 256)
    return [[0xC000 | (L << 11) | (rn << 8) | lst]], {rn: ptr_value()}


T_BODY = [(t_shift_imm, 8), (t_addsub, 8), (t_imm8, 8), (t_alu, 16), (t_hi, 8), (t_ldr_lit, 4), (t_ldst_reg, 8),
          (t_ldst_imm, 10), (t_sp_rel, 4), (t_addr, 4), (t_sp_adj, 3), (t_push, 3), (t_pop, 3), (t_ext, 4), (t_ldmstm, 5)]


def t_terminator():
    k = rnd.choice(["bcond", "b", "svc", "bl", "blx", "bx", "blxr", "pop_pc"])
    if k == "bcond":
        return [[0xD000 | (rnd.randrange(14) << 8) | rnd.randrange(256)]], {}
    if k == "svc":
        return [[0xDF00 | rnd.randrange(256)]], {}
    if k == "b":
        return [[0xE000 | rnd.randrange(0x800)]], {}
    if k in ("bl", "blx"):
        hi, lo = rnd.randrange(0x800), rnd.randrange(0x400) * 2
        first = 0xF000 | hi
        second = (0xF800 if k == "bl" else 0xE800) | (lo >> 1)
        if k == "blx":
            second &= ~1
        return [[first, second]], {}
    if k in ("bx", "blxr"):
        rm = 12
        tgt = rnd.choice([0x00400000, 0x00400001, 0x00800004, 0x00800005])
        return [[0x4700 | ((1 if k == "blxr" else 0) << 7) | (rm << 3)]], {"treg": (rm, tgt)}
    rd = rnd.randrange(3)
    return [[0xBD00 | rd]], {"poppc": 1}


def make_thumb_vector():
    n = rnd.randrange(1, 7)
    insns, setups, count = [], {13: ptr_value()}, 0
    for _ in range(n):
        g = rnd.choices([f for f, _ in T_BODY], [w for _, w in T_BODY])[0]
        ins, sl = g()
        for grp in ins:
            insns += grp
            count += 1
        setups.update(sl)
    if rnd.random() < 0.2:
        ins, sl = t_terminator()
        for grp in ins:
            insns += grp
            count += 1
        setups.update(sl)
    return insns, setups, count


def make_vector():
    global VEC_LEN
    VEC_LEN = rnd.choice([0, 0, 0, 0, 1, 2, 3, 4, 7]) if rnd.random() < 0.35 else 0
    if ONLY_VFP:
        w, sl = (vfp_data if ONLY_VFP == 'data' else vfp_xfer)()
        d = {}
        for x in sl:
            d.update(x)
        d['__vfp__'] = 1
        return [w], d
    if ONLY_MEDIA:
        w, sl = media()
        return [w], {k: v for d in sl for k, v in d.items()}
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


def run(seq, setups, thumb=False, ninsn=None):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB if thumb else UC_MODE_ARM)
    uc.ctl_set_cpu_model(UC_CPU_ARM_11MPCORE)
    uc.mem_map(0, CODE)
    uc.mem_map(CODE, 0x1000)
    uc.mem_map(CODE + 0x1000, 0x1000000 - CODE - 0x1000)   # branch targets land in mapped (zero) memory
    uc.hook_add(UC_HOOK_INTR, lambda uc_, intno, ud: None)   # SVC: let the instruction retire
    uc.mem_map(WIN, WIN_SIZE)
    use_vfp = bool(setups.get('__vfp__'))
    if use_vfp:
        uc.reg_write(UC_ARM_REG_C1_C0_2, 0xF00000)
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        s_init, fpscr_init = vfp_state()
        for i in range(32):
            uc.reg_write(UC_ARM_REG_S0 + i, s_init[i])
        uc.reg_write(UC_ARM_REG_FPSCR, fpscr_init)
    uc.mem_write(WIN, bytes(memf(WIN + i) for i in range(WIN_SIZE)))
    init = [rnd.getrandbits(32) for _ in range(15)]
    for k, v in setups.items():
        if k in ("bxreg", "__vfp__", "poppc"):
            continue
        if k == "treg":
            init[v[0]] = v[1]
            continue
        init[k] = v & 0xFFFFFFFF
    if "bxreg" in setups:
        t = setups["bxreg"]
        tgt = rnd.choice([0x00400000, 0x00400001, 0x00800004, 0x00800005])
        init[t] = tgt
    flags = rnd.getrandbits(4)
    q_in, ge_in = rnd.randrange(2), rnd.getrandbits(4)
    tls = rnd.getrandbits(32) & ~3
    for i, r in enumerate(REGS):
        uc.reg_write(r, init[i])
    cpsr = uc.reg_read(UC_ARM_REG_CPSR)
    cpsr = (cpsr & 0x0FF0FFFF & ~(1 << 27)) | (flags << 28) | (q_in << 27) | (ge_in << 16)
    uc.reg_write(UC_ARM_REG_CPSR, cpsr)
    try:
        uc.reg_write(UC_ARM_REG_C13_C0_3, tls)
    except Exception:
        pass
    code = b"".join(w.to_bytes(2 if thumb else 4, "little") for w in seq)
    uc.mem_write(CODE, code)
    try:
        if thumb:
            uc.emu_start(CODE | 1, CODE + 2 * len(seq), count=ninsn)
        else:
            uc.emu_start(CODE, CODE + 4 * len(seq), count=len(seq))
    except UcError:
        return None
    out = [uc.reg_read(r) for r in REGS]
    pc = uc.reg_read(UC_ARM_REG_PC)
    cpsr = uc.reg_read(UC_ARM_REG_CPSR)
    after = bytes(uc.mem_read(WIN, WIN_SIZE))
    diffs = [(WIN + i, b) for i, b in enumerate(after) if b != memf(WIN + i)]
    qge_out = (((cpsr >> 27) & 1) << 4) | ((cpsr >> 16) & 0xF)
    vfp_out = None
    if use_vfp:
        vfp_out = ([uc.reg_read(UC_ARM_REG_S0 + i) for i in range(32)], uc.reg_read(UC_ARM_REG_FPSCR), s_init, fpscr_init)
    return init, flags, tls, (q_in << 4) | ge_in, out, pc, (cpsr >> 28) & 0xF, (cpsr >> 5) & 1, qge_out, diffs, vfp_out


def main():
    global FORCE_KIND, ONLY_MEDIA
    count, seed, path = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    global THUMB_FRAC
    if len(sys.argv) > 4 and sys.argv[4] == 'thumb':
        THUMB_FRAC = 1.0
    elif len(sys.argv) > 4:
        THUMB_FRAC = 0.0
    if len(sys.argv) > 4 and sys.argv[4].startswith('vfp'):
        global ONLY_VFP
        ONLY_VFP = sys.argv[4][3:] or 'data'
    elif len(sys.argv) > 4 and sys.argv[4] != 'thumb':
        ONLY_MEDIA = True
        FORCE_KIND = sys.argv[4] if sys.argv[4] != "any" else None
    rnd.seed(seed)
    done = 0
    thumb_frac = THUMB_FRAC
    with open(path, "w") as f:
        f.write("# vectors: N words | init r0-r14 nzcv tls qge vfpflag [s0-s31 fpscr] | final r0-r14 pc nzcv thumb qge [s0-s31 fpscr] | ndiffs (addr:byte)*\n")
        while done < count:
            is_thumb = rnd.random() < thumb_frac
            if is_thumb:
                seq, setups, ninsn = make_thumb_vector()
                res = run(seq, setups, True, ninsn)
            else:
                seq, setups = make_vector()
                res = run(seq, setups)
            if res is None:
                continue
            init, flags, tls, qge_in, out, pc, nzcv, thumb, qge_out, diffs, vfp_out = res
            h = lambda xs: " ".join("%08x" % x for x in xs)
            vi = ("1 %s %08x" % (h(vfp_out[2]), vfp_out[3])) if vfp_out else "0"
            vo = (" %s %08x" % (h(vfp_out[0]), vfp_out[1])) if vfp_out else ""
            f.write("%s %d %s | %s %x %08x %x %s | %s %08x %x %d %x%s | %d %s\n" % (
                'T' if is_thumb else 'V', len(seq), (' '.join('%04x' % x for x in seq) if is_thumb else h(seq)), h(init), flags, tls, qge_in, vi, h(out), pc, nzcv, thumb, qge_out, vo, len(diffs),
                " ".join("%08x:%02x" % d for d in diffs)))
            done += 1


if __name__ == "__main__":
    main()
