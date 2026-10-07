"""Extract all files from a PS2 disc image (ISO, or BIN with 2352-byte sectors).

Usage: python iso9660.py IMAGE OUTDIR
"""
import os
import struct
import sys


class Image:
    def __init__(self, path):
        self.f = open(path, 'rb')
        size = os.path.getsize(path)
        raw = path.lower().endswith('.bin') or (size % 2352 == 0 and size % 2048 != 0)
        self.sector_size = 2352 if raw else 2048
        self.data_offset = 24 if raw else 0   # MODE2/2352: 24-byte header before the 2048 bytes of data

    def sectors(self, lba, count):
        out = bytearray()
        for i in range(count):
            self.f.seek((lba + i) * self.sector_size + self.data_offset)
            out += self.f.read(2048)
        return bytes(out)

    def copy_to(self, lba, size, dst):
        with open(dst, 'wb') as o:
            if self.sector_size == 2048:
                self.f.seek(lba * 2048)
                left = size
                while left:
                    chunk = self.f.read(min(left, 8 << 20))
                    o.write(chunk)
                    left -= len(chunk)
            else:
                for i in range((size + 2047) // 2048):
                    o.write(self.sectors(lba + i, 1)[:min(2048, size - i * 2048)])


def walk(img, lba, size, path=''):
    data = img.sectors(lba, (size + 2047) // 2048)
    files, i = [], 0
    while i < len(data):
        length = data[i]
        if length == 0:  # records never cross a sector; skip to the next one
            i = (i // 2048 + 1) * 2048
            continue
        record = data[i:i + length]
        entry_lba, entry_size = struct.unpack('<I', record[2:6])[0], struct.unpack('<I', record[10:14])[0]
        flags, name_len = record[25], record[32]
        name = record[33:33 + name_len]
        if name not in (b'\x00', b'\x01'):
            child = path + '/' + name.decode('latin1').split(';')[0]
            if flags & 2:
                files += walk(img, entry_lba, entry_size, child)
            else:
                files.append((child, entry_lba, entry_size))
        i += length
    return files


def list_files(path):
    img = Image(path)
    pvd = img.sectors(16, 1)
    if pvd[1:6] != b'CD001':
        raise ValueError('no ISO 9660 volume descriptor')
    root = pvd[156:156 + 34]
    return img, walk(img, struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0])


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    img, files = list_files(sys.argv[1])
    for name, lba, size in files:
        dst = os.path.join(sys.argv[2], *name.strip('/').split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        img.copy_to(lba, size, dst)
    print(f'extracted {len(files)} files to {sys.argv[2]}')


if __name__ == '__main__':
    main()
