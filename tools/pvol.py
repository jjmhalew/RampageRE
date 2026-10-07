"""List or extract RAMPAGE.VOL (PVOL archive) from Rampage: Total Destruction (PS2, SLES-53800).

Layout (little endian):
  0x00  char[4] "PVOL"
  0x04  u32     version (0x1001)
  0x08  u32     file count N
  0x0C  u32     name block size
  0x10  N x {u32 offset, u32 size, u32 index}   offsets are absolute
  then  name block: u32 count, N x u32 name offset (relative to the block), NUL-terminated names

Usage:
  python pvol.py RAMPAGE.VOL            list files
  python pvol.py RAMPAGE.VOL outdir     extract all files
"""
import os
import struct
import sys


def read_index(f):
    magic, version, count, names_size = struct.unpack('<4sIII', f.read(16))
    if magic != b'PVOL':
        raise ValueError(f'not a PVOL archive (magic {magic!r})')
    entries = [struct.unpack('<III', f.read(12)) for _ in range(count)]
    block = f.read(names_size)
    name_offsets = struct.unpack(f'<{count}I', block[4:4 + 4 * count])
    names = [block[o:block.index(b'\0', o)].decode('latin1') for o in name_offsets]
    return [(name, offset, size) for name, (offset, size, _) in zip(names, entries)]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    with open(sys.argv[1], 'rb') as f:
        files = read_index(f)
        if len(sys.argv) < 3:
            for name, offset, size in files:
                print(f'{offset:#010x} {size:>10}  {name}')
            print(f'{len(files)} files')
            return
        out = sys.argv[2]
        os.makedirs(out, exist_ok=True)
        for name, offset, size in files:
            f.seek(offset)
            with open(os.path.join(out, name), 'wb') as o:
                left = size
                while left:
                    chunk = f.read(min(left, 8 << 20))
                    o.write(chunk)
                    left -= len(chunk)
        print(f'extracted {len(files)} files to {out}')


if __name__ == '__main__':
    main()
