"""Find code that builds an address with lui + addiu/ori/load/store (string and data references).

Usage: python tools/refs.py ADDR [ADDR ...]
"""
import csv
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OFF, BASE, TEXT_END = 0x1000, 0x100000, 0x349910
with open(os.path.join(ROOT, 'build', 'disc', 'RAMPAGE.ELF'), 'rb') as f:
    ELF = f.read()
WORDS = struct.unpack(f'<{(TEXT_END - BASE) // 4}I', ELF[OFF:OFF + TEXT_END - BASE])
with open(os.path.join(ROOT, 'recomp', 'funcmap.csv')) as f:
    FUNCS = sorted((int(r['Start'], 16), int(r['End'], 16)) for r in csv.DictReader(f))


def owner(addr):
    best = None
    for s, e in FUNCS:
        if s <= addr < e:
            best = s
    return best


def refs(target):
    hits = []
    upper = {}
    for i, x in enumerate(WORDS):
        a = BASE + i * 4
        op = x >> 26
        if op == 0x0f:
            upper[(x >> 16) & 31] = ((x & 0xffff) << 16, a)
        elif op in (0x09, 0x0d, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2b, 0x31, 0x39, 0x1e, 0x1f, 0x37, 0x3f):
            rs = (x >> 21) & 31
            if rs in upper and a - upper[rs][1] < 0x80:
                low = x & 0xffff
                if op != 0x0d and low & 0x8000:
                    low -= 0x10000
                if (upper[rs][0] + low) & 0xffffffff == target:
                    hits.append(a)
    return hits


for arg in sys.argv[1:]:
    t = int(arg, 16)
    for a in refs(t):
        o = owner(a)
        print(f'{t:#x} <- {a:#x} in {o:#x}' if o else f'{t:#x} <- {a:#x}')
