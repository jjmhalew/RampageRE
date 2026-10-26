"""Read Rampage string tables (.loc, bundle entry type 23): menu, block and challenge texts in every language.

Layout (little endian, big endian in the Wii's .bdg; offsets from the start of the entry):
  0x00  0x1C bytes  runtime object (u32 0x00612D3C "<-a", 1, 1, zeros)
  0x1C  u32  4 (meaning unknown)
  0x20  u32  9, the number of languages
  0x24  u32  string count n
  0x28  u32[11] offset of each column: the keys, one column per language, and an unused one whose string
             offsets are all 0
  then the columns, n x u32 string offsets each, then the strings (zero terminated, each padded to 4 bytes)
Column order: keys, English, English (82 strings differ, mostly memory-card wording), Japanese (Shift-JIS, cp932),
French, German, Italian, Spanish, Dutch, Portuguese; the Western texts are Windows-1252. (The Wii's Japanese
column is English but for 7 memory-card strings.)

Usage: python tools/loc.py --verify VOLDIR           check every .loc for layout and full coverage
       python tools/loc.py VOLDIR OUTDIR             write OUTDIR/<bundle>_<name>.tsv (key, then one column per language)
"""
import os
import struct
import sys

import bdp

LANGUAGES = ['English', 'English 2', 'Japanese', 'French', 'German', 'Italian', 'Spanish', 'Dutch', 'Portuguese']
ENCODINGS = ['cp1252', 'cp1252', 'cp932', 'cp1252', 'cp1252', 'cp1252', 'cp1252', 'cp1252', 'cp1252']
COLUMNS = 11


def align4(n):
    return (n + 3) & ~3


class Loc:
    def __init__(self, data, endian='<'):
        self.data = data
        self.e = endian
        self.unknown, self.languages, self.count = struct.unpack_from(endian + '3I', data, 0x1C)
        self.columns = struct.unpack_from(f'{endian}{COLUMNS}I', data, 0x28)
        self.rows = []
        for k in range(self.count):
            row = []
            for c, at in enumerate(self.columns[:1 + len(LANGUAGES)]):
                o = struct.unpack_from(endian + 'I', data, at + 4 * k)[0]
                raw = data[o:data.index(b'\0', o)]
                enc = 'ascii' if c == 0 else ENCODINGS[c - 1]
                row.append(raw.decode(enc, errors='replace'))
            self.rows.append(row)

    def check(self):
        """Raises unless the columns and strings follow each other to the last byte."""
        used = [at for at in self.columns if at]
        at = 0x28 + 4 * COLUMNS
        for c in used:
            if c != at:
                raise ValueError(f'column at {c:#x}, expected {at:#x}')
            at += 4 * self.count
        offsets = sorted({struct.unpack_from(self.e + 'I', self.data, c + 4 * k)[0] for c in used
                          for k in range(self.count)} - {0})
        for o in offsets:
            if o != at:
                raise ValueError(f'string at {o:#x}, expected {at:#x}')
            at = align4(self.data.index(b'\0', o) + 1)
        if at != len(self.data):
            raise ValueError(f'strings end at {at:#x}, entry is {len(self.data):#x}')
        if self.languages != len(LANGUAGES) or len(used) != COLUMNS or any(
                struct.unpack_from(self.e + 'I', self.data, self.columns[-1] + 4 * k)[0] for k in range(self.count)):
            raise ValueError(f'{self.languages} languages, {len(used)} columns')


def tables(voldir):
    for f in sorted(os.listdir(voldir)):
        if not f.lower().endswith(bdp.EXTENSIONS):
            continue
        with open(os.path.join(voldir, f), 'rb') as h:
            b = bdp.Bundle(h.read())
        for e, a, _ in b.items(0):
            if e.type == 23:
                yield f, b.name(e.name), a, b.endian


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == '--verify':
        n = strings = bad = 0
        for f, name, data, endian in tables(args[1]):
            try:
                loc = Loc(data, endian)
                loc.check()
                n += 1
                strings += loc.count
            except (ValueError, struct.error, UnicodeDecodeError) as err:
                bad += 1
                print(f'{f} {name}: {err}')
        print(f'{n} string tables, {strings} keys x {len(LANGUAGES)} languages, every byte accounted for; {bad} bad')
        sys.exit(1 if bad else 0)
    elif len(args) == 2:
        os.makedirs(args[1], exist_ok=True)
        for f, name, data, endian in tables(args[0]):
            loc = Loc(data, endian)
            path = os.path.join(args[1], f'{os.path.splitext(f)[0]}_{os.path.splitext(name)[0]}.tsv')
            with open(path, 'w', encoding='utf-8', newline='\n') as out:
                out.write('\t'.join(['key'] + LANGUAGES) + '\n')
                for row in loc.rows:
                    out.write('\t'.join(c.replace('\n', '\\n').replace('\t', '\\t') for c in row) + '\n')
            print(f'{path}: {loc.count} strings')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
