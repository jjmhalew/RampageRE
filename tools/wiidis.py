"""Disassemble functions of the Wii version's Rampage.elf (PowerPC, big endian, symbols kept) with capstone.

The ELF is DATA/files/Rampage.elf of the Wii disc (RPGP5D; see docs/WII_NOTES.md). Gekko paired-single
instructions are decoded where capstone can (CS_MODE_PS); the rest print as .long.

Usage: python tools/wiidis.py ELF NAME_OR_ADDR...     functions by (part of) symbol name or by hex address
       python tools/wiidis.py ELF --find TEXT         symbols containing TEXT
       python tools/wiidis.py ELF --xref ADDR         instructions that call or build ADDR

Branch targets get their symbol; addresses built with lis / addi or read off r13 / r2 (small data) get the symbol
there, the string there or behind the pointer there, or the float there.
"""
import bisect
import os
import struct
import sys

# tools/dis.py would stand in for the standard library's dis, which capstone needs
sys.path = [p for p in sys.path if os.path.abspath(p or '.') != os.path.dirname(os.path.abspath(__file__))]
import capstone  # noqa: E402
from capstone import ppc  # noqa: E402

ELF = open(sys.argv[1], 'rb').read()
shoff, = struct.unpack_from('>I', ELF, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from('>HHH', ELF, 0x2E)
SECS = [struct.unpack_from('>10I', ELF, shoff + i * shentsize) for i in range(shnum)]
NUL = bytes(1)


def cstr(off):
    return ELF[off:ELF.index(NUL, off)].decode('latin1')


SECTIONS = {cstr(SECS[shstrndx][4] + s[0]): s for s in SECS}
LOADED = [(s[3], s[3] + s[5], s[4], s[1]) for s in SECS if s[3] and s[5]]   # addr, end, file offset, type


def read(addr, n):
    for a, e, off, t in LOADED:
        if a <= addr and addr + n <= e:
            return None if t == 8 else ELF[off + addr - a:off + addr - a + n]   # .bss has no bytes
    return None


symtab = SECTIONS['.symtab']
strtab = SECS[symtab[6]]
FUNCS = []
for i in range(symtab[5] // 16):
    name, val, size, info, oth, shn = struct.unpack_from('>IIIBBH', ELF, symtab[4] + 16 * i)
    s = cstr(strtab[4] + name)
    if val and s and info & 15 in (0, 1, 2):
        FUNCS.append((val, size, s, info & 15))
FUNCS.sort()
ADDRS = [f[0] for f in FUNCS]


def sym_at(addr):
    k = bisect.bisect_right(ADDRS, addr) - 1
    if k >= 0:
        a, size, s, _ = FUNCS[k]
        if addr < a + max(size, 1):
            return s if addr == a else f'{s}+{addr - a:#x}'
    return None


def string_at(addr):
    raw = read(addr, 80)
    if raw:
        z = raw.split(NUL)[0]
        if len(z) >= 2 and all(32 <= c < 127 for c in z):
            return z.decode()
    return None


def describe(addr):
    s = sym_at(addr)
    out = s or ''
    st = string_at(addr)
    if st:
        out += f' "{st}"'
    else:
        raw = read(addr, 4)
        if raw:
            p, = struct.unpack('>I', raw)
            ps = string_at(p) if 0x80000000 <= p < 0x81800000 else None
            if ps:
                out += f' -> "{ps}"'
            elif not s:
                f, = struct.unpack('>f', raw)
                if 1e-4 < abs(f) < 1e6:
                    out += f' ({f:g}f)'
    return out.strip()


SDA = next((a for a, _, s, _ in FUNCS if s == '_SDA_BASE_'), None)
SDA2 = next((a for a, _, s, _ in FUNCS if s == '_SDA2_BASE_'), None)
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_PS)
md.detail = True
md.skipdata = True
LOADS = ('lwz', 'lfs', 'lbz', 'lhz', 'stw', 'stfs', 'lha', 'lfd', 'stb', 'sth', 'lwzu', 'stwu', 'stfd', 'lmw',
         'stmw', 'psq_l', 'psq_st')


def s16(v):
    return struct.unpack('>h', struct.pack('>H', v & 0xFFFF))[0]


def disasm(addr, size, out=print):
    """Linear: a register's value is known after lis / addi / mr from known values, forgotten when written."""
    code = read(addr, size)
    known = {}
    own = sym_at(addr)
    for ins in md.disasm(code, addr):
        note = ''
        if ins.id == 0:                              # a word capstone cannot decode (skipdata)
            out(f'  {ins.address:08x}  .long    {ins.bytes.hex()}')
            continue
        ops = ins.operands
        m = ins.mnemonic
        if m.startswith('b') and ops and ops[-1].type == ppc.PPC_OP_IMM:
            s = sym_at(ops[-1].imm)
            if s and not s.startswith(own + '+'):
                note = s
        dest = ops[0].reg if ops and ops[0].type == ppc.PPC_OP_REG else None
        if m == 'lis' and len(ops) == 2:
            known[dest] = (ops[1].imm & 0xFFFF) << 16
        elif m == 'addi' and len(ops) == 3 and ops[1].reg in known:
            v = (known[ops[1].reg] + s16(ops[2].imm)) & 0xFFFFFFFF
            known[dest] = v
            note = f'{v:#x} {describe(v)}'
        elif m == 'mr' and len(ops) == 2 and ops[1].reg in known:
            known[dest] = known[ops[1].reg]
        elif m in LOADS and len(ops) >= 2 and ops[1].type == ppc.PPC_OP_MEM:
            base, disp = ops[1].mem.base, ops[1].mem.disp
            a = None
            if base in known:
                a = (known[base] + disp) & 0xFFFFFFFF
            elif base == ppc.PPC_REG_R13 and SDA:
                a = SDA + disp
            elif base == ppc.PPC_REG_R2 and SDA2:
                a = SDA2 + disp
            if a is not None:
                tag = 'sda ' if base == ppc.PPC_REG_R13 else 'sda2 ' if base == ppc.PPC_REG_R2 else ''
                note = f'{tag}{a:#x} {describe(a)}'
            if m.startswith('l') and dest is not None:
                known.pop(dest, None)
        elif dest is not None and not m.startswith(('st', 'cmp', 'b')):
            known.pop(dest, None)
        if m == 'bl':
            for r in range(ppc.PPC_REG_R3, ppc.PPC_REG_R12 + 1):
                known.pop(r, None)
        out(f'  {ins.address:08x}  {m:8} {ins.op_str:28}{"  ; " + note if note else ""}')


def main():
    args = sys.argv[2:]
    if args[0] == '--find':
        for a, size, s, t in FUNCS:
            if args[1].lower() in s.lower():
                print(f'{a:08x} {size:6x} {"F" if t == 2 else "D"} {s}')
        return
    if args[0] == '--xref':
        target = int(args[1], 16)
        for a, size, s, t in FUNCS:
            if t != 2 or not size or not read(a, size):
                continue
            lines = []
            disasm(a, size, lines.append)
            for line in lines:
                parts = line.split()
                if f'{target:#x} ' in line + ' ' or (parts[1] in ('bl', 'b') and parts[2] == f'{target:#x}'):
                    print(f'{s}: {line.strip()}')
        return
    for q in args:
        try:
            a = int(q, 16)
            match = [f for f in FUNCS if f[0] == a]
        except ValueError:
            match = [f for f in FUNCS if q in f[2] and f[3] == 2]
        for a, size, s, _ in match[:6]:
            print(f'== {a:08x} {s} ({size:#x} bytes)')
            disasm(a, size)


main()
