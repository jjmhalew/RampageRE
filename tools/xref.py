"""Small helpers for exploring RAMPAGE.ELF: callers of a function, or the call tree below it.

Usage: python tools/xref.py callers ADDR [depth]
       python tools/xref.py callees ADDR [depth]
"""
import csv
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OFF, BASE, TEXT_SIZE = 0x1000, 0x100000, 0x249910
with open(os.path.join(ROOT, 'build', 'disc', 'RAMPAGE.ELF'), 'rb') as f:
    elf = f.read()
WORDS = struct.unpack(f'<{TEXT_SIZE // 4}I', elf[OFF:OFF + TEXT_SIZE])
with open(os.path.join(ROOT, 'recomp', 'config.toml'), encoding='utf-8') as f:
    NAMES = {int(a, 16): n for n, a in re.findall(r'"([^"@]+)@0x([0-9A-F]{8})"', f.read())}
with open(os.path.join(ROOT, 'recomp', 'funcmap.csv')) as f:
    FUNCS = sorted((int(r['Start'], 16), int(r['End'], 16)) for r in csv.DictReader(f))


def owner(addr):
    for s, e in FUNCS:
        if s <= addr < e:
            return s
    return None


def name(addr):
    return f'{addr:#x} {NAMES.get(addr, "")}'.rstrip()


def calls_from(func):
    end = next(e for s, e in FUNCS if s == func)
    for a in range(func, end, 4):
        x = WORDS[(a - BASE) // 4]
        if (x >> 26) in (2, 3):
            yield a, (x & 0x3FFFFFF) << 2


def callers_of(func):
    for i, x in enumerate(WORDS):
        if (x >> 26) in (2, 3) and (x & 0x3FFFFFF) << 2 == func:
            yield BASE + i * 4


def tree(func, depth, up, indent=0, seen=None):
    seen = seen if seen is not None else set()
    print('  ' * indent + name(func))
    if depth == 0 or func in seen:
        return
    seen.add(func)
    nxt = {owner(a) for a in callers_of(func)} if up else {t for _, t in calls_from(func)}
    for f in sorted(x for x in nxt if x is not None and x != func):
        tree(f, depth - 1, up, indent + 1, seen)


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    tree(int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 3, sys.argv[1] == 'callers')
