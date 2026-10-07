"""Build a PS2Recomp function map (Ghidra CSV format) for RAMPAGE.ELF, without Ghidra.

PS2Recomp's own analyzer only follows jal targets, which misses everything the game reaches
through pointers: C++ constructors, vtables, callbacks. A missing function stops the game at
run time ("guest-branch:missing-target"), so this collects function starts from:
  1. jal targets
  2. code pointers stored in data (vtables, callback tables)
  3. every entry of the C++ static constructor table
  4. code addresses built with lui + addiu/ori (thread entries, handlers)
  5. functions that open a stack frame right after a jr $ra or tail-call j
  6. every address the analyzer bound in config.toml (stubs, entry_points)

recomp/funcmap.csv is the map the current state was tested with (this script plus the functions
PS2Recomp's analyzer found on its own); regenerate it only when the rules here change.

Usage: python funcmap.py RAMPAGE.ELF config.toml funcmap.csv
"""
import collections
import re
import struct
import sys

OFF, BASE, FILE_SIZE = 0x1000, 0x100000, 0x34ca80   # layout written by split_elf.py
CODE = [(0x100000, 0x349910), (0x435d00, 0x443b50)]
DATA = [(0x349910, 0x435d00), (0x443b50, 0x44ca80)]
CTOR_TABLE = (0x443b50, 0x443fb0)
JR_RA = 0x03E00008


def build(elf, config_text):
    words = struct.unpack(f'<{FILE_SIZE // 4}I', elf[OFF:OFF + FILE_SIZE])

    def w(a):
        return words[(a - BASE) // 4]

    def in_code(a):
        return a % 4 == 0 and any(lo <= a < hi for lo, hi in CODE)

    def is_prologue(x):  # addiu $sp, $sp, -N
        return (x >> 16) == 0x27BD and (x & 0x8000)

    def at_boundary(a):
        if any(a == lo for lo, _ in CODE):
            return True
        before, delay = w(a - 8), w(a - 4)
        # the previous function ended with jr $ra, a tail-call j or jr, or padding
        return (before == JR_RA or (before >> 26) == 2 or ((before >> 26) == 0 and (before & 0x3f) == 8)
                or delay == 0 or is_prologue(w(a)))

    starts = {lo for lo, _ in CODE} | {struct.unpack('<I', elf[24:28])[0]}  # plus the ELF entry point
    found = collections.Counter()

    for lo, hi in CODE:
        for a in range(lo, hi, 4):
            x = w(a)
            if (x >> 26) == 3:
                t = (a & 0xF0000000) | ((x & 0x3FFFFFF) << 2)
                if in_code(t):
                    starts.add(t)
                    found['jal'] += 1

    for lo, hi in DATA:
        for a in range(lo, hi, 4):
            t = w(a)
            if in_code(t) and at_boundary(t):
                starts.add(t)
                found['data pointer'] += 1

    for a in range(*CTOR_TABLE, 4):
        if in_code(w(a)):
            starts.add(w(a))
            found['constructor'] += 1

    for lo, hi in CODE:
        for a in range(lo, hi, 4):
            x = w(a)
            if (x >> 26) != 0x0f:  # lui
                continue
            rt, upper = (x >> 16) & 31, (x & 0xffff) << 16
            for b in range(a + 4, min(a + 40, hi), 4):
                y = w(b)
                op = y >> 26
                if op in (0x09, 0x0d) and (y >> 21) & 31 == rt:  # addiu / ori on the same register
                    low = y & 0xffff
                    if op == 0x09 and low & 0x8000:
                        low -= 0x10000
                    t = (upper + low) & 0xffffffff
                    if in_code(t) and at_boundary(t):
                        starts.add(t)
                        found['lui address'] += 1
                    break

    for lo, hi in CODE:
        for a in range(lo, hi - 8, 4):
            if w(a) == JR_RA or (w(a) >> 26) == 2:
                b = a + 8
                while b < hi and w(b) == 0:
                    b += 4
                if b < hi and is_prologue(w(b)):
                    starts.add(b)
                    found['after return'] += 1

    for m in re.finditer(r'"[^"@]+@0x([0-9A-Fa-f]{8})"', config_text):
        a = int(m.group(1), 16)
        if in_code(a):
            starts.add(a)
            found['config'] += 1

    starts = sorted(starts)
    rows = []
    for i, s in enumerate(starts):
        end = next(hi for lo, hi in CODE if lo <= s < hi)
        if i + 1 < len(starts) and starts[i + 1] < end:
            end = starts[i + 1]
        rows.append((s, end))
    return rows, found


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    with open(sys.argv[1], 'rb') as f:
        elf = f.read()
    with open(sys.argv[2], encoding='utf-8') as f:
        config_text = f.read()
    rows, found = build(elf, config_text)
    with open(sys.argv[3], 'w', newline='\n') as o:
        o.write('Name,Start,End,Size\n')
        for s, e in rows:
            o.write(f'sub_{s:08X},0x{s:08X},0x{e:08X},{e - s}\n')
    print(f'{len(rows)} functions', dict(found))


if __name__ == '__main__':
    main()
