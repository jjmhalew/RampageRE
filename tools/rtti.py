"""Find which RTTI classes (vtables) reference a function, and list a class's virtual methods.

Usage: python tools/rtti.py func ADDR      classes whose vtable contains the function owning ADDR
       python tools/rtti.py class NAME     vtable of a class
"""
import csv
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OFF, BASE, FILE_SIZE = 0x1000, 0x100000, 0x34ca80
with open(os.path.join(ROOT, 'build', 'disc', 'RAMPAGE.ELF'), 'rb') as f:
    ELF = f.read()
WORDS = struct.unpack(f'<{FILE_SIZE // 4}I', ELF[OFF:OFF + FILE_SIZE])
with open(os.path.join(ROOT, 'recomp', 'funcmap.csv')) as f:
    FUNCS = sorted((int(r['Start'], 16), int(r['End'], 16)) for r in csv.DictReader(f))


def w(a):
    return WORDS[(a - BASE) // 4]


def is_code(a):
    return a % 4 == 0 and (0x100000 <= a < 0x349910 or 0x435d00 <= a < 0x443b50)


def name_at(a):
    if not BASE <= a < BASE + FILE_SIZE:
        return None
    o = OFF + a - BASE
    t = ELF[o:ELF.find(b'\0', o)]
    return t.decode() if 1 < len(t) < 64 and re.fullmatch(rb'[A-Za-z_]\w*', t) else None


def vtables():
    for i in range(len(WORDS) - 3):
        ti = WORDS[i]
        if 0x349910 <= ti < 0x44ca80 and WORDS[i + 1] == 0 and is_code(WORDS[i + 2]):
            name = name_at(w(ti)) if BASE <= ti < BASE + FILE_SIZE else None
            if name:
                vt = BASE + i * 4
                methods = []
                a = vt + 8
                while a < BASE + FILE_SIZE and is_code(w(a)):
                    methods.append(w(a))
                    a += 4
                yield name, vt, methods


def owner(addr):
    return next((s for s, e in FUNCS if s <= addr < e), addr)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    if sys.argv[1] == 'func':
        f = owner(int(sys.argv[2], 16))
        print(f'function {f:#x}')
        for name, vt, methods in vtables():
            for i, m in enumerate(methods):
                if m == f:
                    print(f'  {name}  vtable {vt:#x}  +{8 + 4 * i:#x}')
    else:
        for name, vt, methods in vtables():
            if name == sys.argv[2]:
                print(f'{name} vtable {vt:#x}')
                for i, m in enumerate(methods):
                    print(f'  +{8 + 4 * i:#04x} {m:#x}')
