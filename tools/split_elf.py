"""Rewrite SLES_538.00 with separate code and data sections.

The retail ELF has one RWX segment that holds code and data together, so PS2Recomp would
translate data as code. This writes a copy with proper .text/.data sections and segments.

Usage: python split_elf.py SLES_538.00 RAMPAGE.ELF
"""
import struct
import sys

# SLES-53800 (Europe), the only release checked so far
OFF, BASE, FILE_SIZE, MEM_SIZE = 0x80, 0x100000, 3459712, 4032000
REGIONS = [
    ('.text', 0x100000, 0x349910, 'X'),        # game, SCE libs, newlib, MW runtime
    ('.data', 0x349910, 0x435d00, 'W'),
    ('.text.init', 0x435d00, 0x443b50, 'X'),   # C++ static constructors
    ('.data2', 0x443b50, BASE + FILE_SIZE, 'W'),  # starts with the constructor table
]


def split(src):
    if src[:4] != b'\x7fELF':
        raise ValueError('not an ELF file')
    image = src[OFF:OFF + FILE_SIZE]
    if len(image) != FILE_SIZE:
        raise ValueError('unexpected ELF size: is this SLES_538.00?')
    entry = struct.unpack('<I', src[24:28])[0]
    bss = (BASE + FILE_SIZE, MEM_SIZE - FILE_SIZE)

    ehsize, phnum, data_off = 52, len(REGIONS) + 1, 0x1000
    shstr = b'\0' + b'\0'.join(n.encode() for n, *_ in REGIONS) + b'\0.bss\0.shstrtab\0'

    def name_index(name):
        return shstr.index(b'\0' + name.encode() + b'\0') + 1

    shstr_off = data_off + FILE_SIZE
    sh_off = (shstr_off + len(shstr) + 3) & ~3
    shnum = len(REGIONS) + 3

    header = bytearray(src[:ehsize])
    flags = struct.unpack('<I', src[36:40])[0]
    struct.pack_into('<IIIIHHHHHH', header, 24, entry, ehsize, sh_off, flags, ehsize, 32, phnum, 40, shnum, shnum - 1)

    phdrs = b''
    for _, lo, hi, kind in REGIONS:
        phdrs += struct.pack('<8I', 1, data_off + lo - BASE, lo, lo, hi - lo, hi - lo, 5 if kind == 'X' else 6, 0x10)
    phdrs += struct.pack('<8I', 1, data_off + FILE_SIZE, bss[0], bss[0], 0, bss[1], 6, 0x10)

    shdrs = b'\0' * 40
    for name, lo, hi, kind in REGIONS:
        shdrs += struct.pack('<10I', name_index(name), 1, 6 if kind == 'X' else 3, lo, data_off + lo - BASE, hi - lo, 0, 0, 16, 0)
    shdrs += struct.pack('<10I', name_index('.bss'), 8, 3, bss[0], data_off + FILE_SIZE, bss[1], 0, 0, 16, 0)
    shdrs += struct.pack('<10I', name_index('.shstrtab'), 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)

    out = bytearray(header) + phdrs
    out += b'\0' * (data_off - len(out))
    out += image + shstr
    out += b'\0' * (sh_off - len(out))
    out += shdrs
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], 'rb') as f:
        out = split(f.read())
    with open(sys.argv[2], 'wb') as f:
        f.write(out)
    print(f'wrote {sys.argv[2]} ({len(out)} bytes)')


if __name__ == '__main__':
    main()
