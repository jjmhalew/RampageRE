"""Disassemble PS2 VU micro code (upper/lower instruction pairs).

Usage: python tools/vudis.py CODE.bin [START [END]]     addresses in bytes
       python tools/vudis.py --trace vutrace.txt         decode a runtime VU trace (PS2X_VUTRACE=1)
"""
import struct
import sys

DEST = lambda w: ''.join(c for c, b in zip('xyzw', (24, 23, 22, 21)) if (w >> b) & 1)
BC = 'xyzw'


def upper(w):
    flags = ''.join(f for f, b in zip('IEMDT', (31, 30, 29, 28, 27)) if (w >> b) & 1)
    d, ft, fs, fd, op = DEST(w), (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 0x3F
    names = {0x00: 'ADD', 0x04: 'SUB', 0x08: 'MADD', 0x0C: 'MSUB', 0x10: 'MAX', 0x14: 'MINI', 0x18: 'MUL'}
    if op < 0x1C:
        base = names[op & 0x3C]
        s = f'{base}{BC[op & 3]}.{d} vf{fd}, vf{fs}, vf{ft}{BC[op & 3]}'
    elif op < 0x30:
        tbl = {0x1C: ('MULq', 'q'), 0x1D: ('MAXi', 'i'), 0x1E: ('MULi', 'i'), 0x1F: ('MINIi', 'i'), 0x20: ('ADDq', 'q'),
               0x21: ('MADDq', 'q'), 0x22: ('ADDi', 'i'), 0x23: ('MADDi', 'i'), 0x24: ('SUBq', 'q'), 0x25: ('MSUBq', 'q'),
               0x26: ('SUBi', 'i'), 0x27: ('MSUBi', 'i')}
        if op in tbl:
            s = f'{tbl[op][0]}.{d} vf{fd}, vf{fs}, {tbl[op][1]}'
        else:
            nm = {0x28: 'ADD', 0x29: 'MADD', 0x2A: 'MUL', 0x2B: 'MAX', 0x2C: 'SUB', 0x2D: 'MSUB', 0x2E: 'OPMSUB', 0x2F: 'MINI'}[op]
            s = f'{nm}.{d} vf{fd}, vf{fs}, vf{ft}'
    elif op >= 0x3C:
        sp = (w & 3) | ((w >> 4) & 0x7C)
        acc = {0x00: 'ADDA', 0x04: 'SUBA', 0x08: 'MADDA', 0x0C: 'MSUBA', 0x18: 'MULA'}
        if sp < 0x10 or 0x18 <= sp < 0x1C:
            s = f'{acc[sp & 0x3C]}{BC[sp & 3]}.{d} ACC, vf{fs}, vf{ft}{BC[sp & 3]}'
        elif 0x10 <= sp < 0x18:
            s = f'{"ITOF" if sp < 0x14 else "FTOI"}{(0, 4, 12, 15)[sp & 3]}.{d} vf{ft}, vf{fs}'
        else:
            tbl = {0x1C: 'MULAq.{d} ACC, vf{fs}, q', 0x1D: 'ABS.{d} vf{ft}, vf{fs}', 0x1E: 'MULAi.{d} ACC, vf{fs}, i',
                   0x1F: 'CLIP vf{fs}xyz, vf{ft}w', 0x20: 'ADDAq.{d} ACC, vf{fs}, q', 0x21: 'MADDAq.{d} ACC, vf{fs}, q',
                   0x22: 'ADDAi.{d} ACC, vf{fs}, i', 0x23: 'MADDAi.{d} ACC, vf{fs}, i', 0x24: 'SUBAq.{d} ACC, vf{fs}, q',
                   0x25: 'MSUBAq.{d} ACC, vf{fs}, q', 0x26: 'SUBAi.{d} ACC, vf{fs}, i', 0x27: 'MSUBAi.{d} ACC, vf{fs}, i',
                   0x28: 'ADDA.{d} ACC, vf{fs}, vf{ft}', 0x29: 'MADDA.{d} ACC, vf{fs}, vf{ft}', 0x2A: 'MULA.{d} ACC, vf{fs}, vf{ft}',
                   0x2C: 'SUBA.{d} ACC, vf{fs}, vf{ft}', 0x2D: 'MSUBA.{d} ACC, vf{fs}, vf{ft}', 0x2E: 'OPMULA.{d} ACC, vf{fs}, vf{ft}',
                   0x2F: 'NOP', 0x30: 'NOP'}
            s = tbl.get(sp, f'upper?{sp:#x}').format(d=d, fs=fs, ft=ft)
    else:
        s = f'upper?{op:#x}'
    return (f'[{flags}] ' if flags else '') + s


def simm(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def lower(w, pc):
    d, ft, fs, fd = DEST(w), (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    if w >> 25 == 0x40:
        op = w & 0x3F
        if op < 0x3C:
            return {0x30: f'IADD vi{fd}, vi{fs}, vi{ft}', 0x31: f'ISUB vi{fd}, vi{fs}, vi{ft}',
                    0x32: f'IADDI vi{ft}, vi{fs}, {simm(fd, 5)}', 0x34: f'IAND vi{fd}, vi{fs}, vi{ft}',
                    0x35: f'IOR vi{fd}, vi{fs}, vi{ft}'}.get(op, f'lower?{op:#x}')
        sp = (w & 3) | ((w >> 4) & 0x7C)
        fsf, ftf = BC[(w >> 21) & 3], BC[(w >> 23) & 3]
        return {0x30: f'MOVE.{d} vf{ft}, vf{fs}', 0x31: f'MR32.{d} vf{ft}, vf{fs}', 0x34: f'LQI.{d} vf{ft}, (vi{fs}++)',
                0x35: f'SQI.{d} vf{fs}, (vi{ft}++)', 0x36: f'LQD.{d} vf{ft}, (--vi{fs})', 0x37: f'SQD.{d} vf{fs}, (--vi{ft})',
                0x38: f'DIV Q, vf{fs}{fsf}, vf{ft}{ftf}', 0x39: f'SQRT Q, vf{ft}{ftf}', 0x3A: f'RSQRT Q, vf{fs}{fsf}, vf{ft}{ftf}',
                0x3B: 'WAITQ', 0x3C: f'MTIR vi{ft}, vf{fs}{fsf}', 0x3D: f'MFIR.{d} vf{ft}, vi{fs}',
                0x3E: f'ILWR.{d} vi{ft}, (vi{fs})', 0x3F: f'ISWR.{d} vi{ft}, (vi{fs})', 0x40: f'RNEXT.{d} vf{ft}, R',
                0x41: f'RGET.{d} vf{ft}, R', 0x42: f'RINIT R, vf{fs}{fsf}', 0x43: f'RXOR R, vf{fs}{fsf}',
                0x64: f'MFP.{d} vf{ft}, P', 0x68: f'XTOP vi{ft}', 0x69: f'XITOP vi{ft}', 0x6C: f'XGKICK vi{fs}',
                0x70: f'ESADD P, vf{fs}', 0x71: f'ERSADD P, vf{fs}', 0x72: f'ELENG P, vf{fs}', 0x73: f'ERLENG P, vf{fs}',
                0x76: f'ESUM P, vf{fs}', 0x78: f'ESQRT P, vf{fs}{fsf}', 0x79: f'ERSQRT P, vf{fs}{fsf}',
                0x7A: f'ERCPR P, vf{fs}{fsf}', 0x7B: 'WAITP', 0x7C: f'ESIN P, vf{fs}{fsf}', 0x7D: f'EATAN P, vf{fs}{fsf}',
                0x7E: f'EEXP P, vf{fs}{fsf}'}.get(sp, f'lowersp?{sp:#x}')
    op = w >> 25
    imm11 = simm(w & 0x7FF, 11)
    target = pc + 8 + imm11 * 8
    imm15 = ((w >> 10) & 0x7800) | (w & 0x7FF)
    tbl = {0x00: f'LQ.{d} vf{ft}, {imm11}(vi{fs})', 0x01: f'SQ.{d} vf{fs}, {imm11}(vi{ft})',
           0x04: f'ILW.{d} vi{ft}, {imm11}(vi{fs})', 0x05: f'ISW.{d} vi{ft}, {imm11}(vi{fs})',
           0x08: f'IADDIU vi{ft}, vi{fs}, {imm15:#x}', 0x09: f'ISUBIU vi{ft}, vi{fs}, {imm15:#x}',
           0x10: f'FCEQ vi1, {w & 0xFFFFFF:#x}', 0x11: f'FCSET {w & 0xFFFFFF:#x}', 0x12: f'FCAND vi1, {w & 0xFFFFFF:#x}',
           0x13: f'FCOR vi1, {w & 0xFFFFFF:#x}', 0x14: f'FSEQ vi{ft}, {w & 0xFFF:#x}', 0x15: f'FSSET {w & 0xFFF:#x}',
           0x16: f'FSAND vi{ft}, {w & 0xFFF:#x}', 0x17: f'FSOR vi{ft}, {w & 0xFFF:#x}', 0x18: f'FMEQ vi{ft}, vi{fs}',
           0x1A: f'FMAND vi{ft}, vi{fs}', 0x1B: f'FMOR vi{ft}, vi{fs}', 0x1C: f'FCGET vi{ft}',
           0x20: f'B {target:#x}', 0x21: f'BAL vi{ft}, {target:#x}', 0x24: f'JR vi{fs}', 0x25: f'JALR vi{ft}, vi{fs}',
           0x28: f'IBEQ vi{ft}, vi{fs}, {target:#x}', 0x29: f'IBNE vi{ft}, vi{fs}, {target:#x}',
           0x2C: f'IBLTZ vi{fs}, {target:#x}', 0x2D: f'IBGTZ vi{fs}, {target:#x}', 0x2E: f'IBLEZ vi{fs}, {target:#x}',
           0x2F: f'IBGEZ vi{fs}, {target:#x}'}
    return tbl.get(op, f'lower?{op:#x}')


def pair(up, lo, pc):
    u = upper(up)
    if (up >> 31) & 1:
        f = struct.unpack('<f', struct.pack('<I', lo))[0]
        return f'{u:40} LOI {f}'
    return f'{u:40} {lower(lo, pc)}'


def main():
    if sys.argv[1] == '--trace':
        for line in open(sys.argv[2]):
            parts = line.split('|')
            f = parts[0].split()
            if len(f) == 3 and len(f[0]) == 4:
                pc, up, lo = int(f[0], 16), int(f[1], 16), int(f[2], 16)
                print(f'{pc:04x} {pair(up, lo, pc):80} |{parts[1].rstrip()}')
            else:
                print(line.rstrip())
        return
    code = open(sys.argv[1], 'rb').read()
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    end = int(sys.argv[3], 16) if len(sys.argv) > 3 else len(code)
    for pc in range(start, end, 8):
        lo, up = struct.unpack_from('<II', code, pc)
        print(f'{pc:04x} {pair(up, lo, pc)}')


if __name__ == '__main__':
    main()
