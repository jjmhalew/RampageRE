"""Disassemble a range of RAMPAGE.ELF with call names and string references.

Usage: python tools/dis.py START [END]      (END defaults to the end of the function)
"""
import csv
import os
import re
import struct
import sys

import rabbitizer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OFF, BASE, FILE_SIZE = 0x1000, 0x100000, 0x34ca80
with open(os.path.join(ROOT, 'build', 'disc', 'RAMPAGE.ELF'), 'rb') as f:
    ELF = f.read()
with open(os.path.join(ROOT, 'recomp', 'config.toml'), encoding='utf-8') as f:
    NAMES = {int(a, 16): n for n, a in re.findall(r'"([^"@]+)@0x([0-9A-F]{8})"', f.read())}
with open(os.path.join(ROOT, 'recomp', 'funcmap.csv')) as f:
    FUNCS = {int(r['Start'], 16): int(r['End'], 16) for r in csv.DictReader(f)}


def word(a):
    return struct.unpack('<I', ELF[OFF + a - BASE:OFF + a - BASE + 4])[0]


def string_at(a):
    if not BASE <= a < BASE + FILE_SIZE:
        return None
    o = OFF + a - BASE
    t = ELF[o:ELF.find(b'\0', o)]
    return t.decode() if 2 < len(t) < 80 and all(32 <= c < 127 for c in t) else None


def main():
    start = int(sys.argv[1], 16)
    end = int(sys.argv[2], 16) if len(sys.argv) > 2 else FUNCS.get(start, start + 0x100)
    upper = {}
    for a in range(start, end, 4):
        x = word(a)
        ins = rabbitizer.Instruction(x, vram=a, category=rabbitizer.InstrCategory.R5900)
        text = ins.disassemble()
        if x & 0xFFE0003F == 0x46000004:             # the EE's sqrt.s fd, ft (fs unused), which rabbitizer leaves "c1"
            text = f'{"sqrt.s":11s} $f{(x >> 6) & 31}, $f{(x >> 16) & 31}'
        note = ''
        op = x >> 26
        if op == 0x0f:
            upper[(x >> 16) & 31] = (x & 0xffff) << 16
        elif op in (0x09, 0x0d, 0x23, 0x2b, 0x31, 0x39) and (x >> 21) & 31 in upper:
            low = x & 0xffff
            if op != 0x0d and low & 0x8000:
                low -= 0x10000
            v = (upper[(x >> 21) & 31] + low) & 0xffffffff
            s = string_at(v)
            note = f'{v:#x}' + (f' "{s}"' if s else '')
        if op in (2, 3):
            t = (x & 0x3FFFFFF) << 2
            note = NAMES.get(t, '')
        if a in FUNCS and a != start:
            print(f'--- {a:#x}')
        print(f'{a:#08x}  {text:45s} {("; " + note) if note else ""}')


if __name__ == '__main__':
    main()
