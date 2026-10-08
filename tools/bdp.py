"""List and extract Rampage bundles (.bdp/.cmp, "Pipeworks Bundle File version 1.3").

Read by the game at 0x1A8740 (BundleVolume). See docs/FORMAT_BDP.md.

Layout (little endian):
  0x00  char[0x2C]  "Pipeworks Bundle File version 1.3", zero padded
  0x2C  u32         segments that are absent: bit n set = segment n is empty
  0x30  u32         version, 1300 (0x514; the game refuses anything else)
  0x34  u32         offset of the name table: u32 count, count x u32 name offset (from the table), names
  0x38  u32[10]     offset of each segment header
Segment header: u16 segment number, u16 record count, u32 entry table offset, u32 data offset, u32 data size,
                u32 data2 offset, u32 data2 size, then per record (0x12 bytes, packed):
                u16 entry index, u32 data offset, u32 data size, u32 data2 offset, u32 data2 size
                (record offsets are relative to the segment's data and data2 blocks).
Entry (16 bytes): u16 entry index, u8 type, u8 ?, u32 name index, u32 ? (1), u32 ? (data size for some types)
Segment 0 holds every entry. Another segment replaces some of them (by entry index) when the game loads it.

Usage: python tools/bdp.py FILE                list the entries
       python tools/bdp.py FILE OUTDIR         extract each entry's data (and data2) blocks
       python tools/bdp.py --verify VOLDIR     parse every bundle and check that every byte is accounted for
"""
import collections
import os
import struct
import sys

MAGIC = b'Pipeworks Bundle File version 1.3'
VERSION = 1300
SEGMENTS = 10

Entry = collections.namedtuple('Entry', 'index flags type extra name word8 wordC')
Record = collections.namedtuple('Record', 'entry data_offset data_size data2_offset data2_size')
Segment = collections.namedtuple('Segment', 'number header entries_offset data_offset data_size data2_offset '
                                            'data2_size records entries')


class Bundle:
    def __init__(self, data):
        if not data.startswith(MAGIC):
            raise ValueError('not a Pipeworks bundle')
        self.data = data
        self.absent, version, names_at = struct.unpack_from('<III', data, 0x2C)
        if version != VERSION:
            raise ValueError(f'version {version}, expected {VERSION}')
        self.names_at = names_at
        count = struct.unpack_from('<I', data, names_at)[0]
        self.name_offsets = struct.unpack_from(f'<{count}I', data, names_at + 4)
        self.names = [self.cstr(names_at + o) for o in self.name_offsets]
        self.segment_offsets = struct.unpack_from(f'<{SEGMENTS}I', data, 0x38)
        self.segments = [self.read_segment(n) for n in range(SEGMENTS)]

    def cstr(self, offset):
        return self.data[offset:self.data.index(b'\0', offset)].decode('latin1')

    def read_segment(self, n):
        at = self.segment_offsets[n]
        number, count, entries_offset, d_off, d_size, d2_off, d2_size = struct.unpack_from('<HHIIIII', self.data, at)
        records = [Record(*struct.unpack_from('<HIIII', self.data, at + 0x18 + 0x12 * i)) for i in range(count)]
        entries = []
        for i in range(count):
            flags, etype, extra, name, w8, wc = struct.unpack_from('<HBBIII', self.data, entries_offset + 16 * i)
            entries.append(Entry(records[i].entry if n else i, flags, etype, extra, name, w8, wc))
        return Segment(n, at, entries_offset, d_off, d_size, d2_off, d2_size, records, entries)

    def items(self, segment=0):
        """(entry, data bytes, data2 bytes) of one segment."""
        s = self.segments[segment]
        for e, r in zip(s.entries, s.records):
            at, at2 = s.data_offset + r.data_offset, s.data2_offset + r.data2_offset
            a = self.data[at:at + r.data_size] if r.data_size else b''
            b = self.data[at2:at2 + r.data2_size] if r.data2_size else b''
            yield e, a, b

    def name(self, index):
        return self.names[index] if index < len(self.names) else f'#{index}'


def coverage(bundle):
    """Byte ranges the parser explains, as (start, end, what). Raises if two of them overlap."""
    d = bundle.data
    ranges = [(0, 0x38 + 4 * SEGMENTS, 'file header')]
    names_end = bundle.names_at + 4 + 4 * len(bundle.names)
    if bundle.names:
        names_end = max(bundle.names_at + o + len(n) + 1 for o, n in zip(bundle.name_offsets, bundle.names))
    ranges.append((bundle.names_at, names_end, 'name table'))
    for s in bundle.segments:
        ranges.append((s.header, s.header + 0x18 + 0x12 * len(s.records), f'segment {s.number} header'))
        if s.records:
            ranges.append((s.entries_offset, s.entries_offset + 16 * len(s.records), f'segment {s.number} entries'))
        for e, r in zip(s.entries, s.records):
            if r.data_size:
                start = s.data_offset + r.data_offset
                ranges.append((start, start + r.data_size, f'segment {s.number} data of {bundle.name(e.name)}'))
            if r.data2_size:
                start = s.data2_offset + r.data2_offset
                ranges.append((start, start + r.data2_size, f'segment {s.number} data2 of {bundle.name(e.name)}'))
    ranges.sort()
    for (a0, a1, an), (b0, b1, bn) in zip(ranges, ranges[1:]):
        if b0 < a1:
            raise ValueError(f'{an} {a0:#x}-{a1:#x} overlaps {bn} {b0:#x}-{b1:#x}')
    if ranges[-1][1] > len(d):
        raise ValueError(f'{ranges[-1][2]} ends at {ranges[-1][1]:#x}, past the end of the file {len(d):#x}')
    return ranges


def gaps(bundle):
    """Bytes no structure claims: (start, end, fill bytes)."""
    out = []
    pos = 0
    for start, end, _ in coverage(bundle) + [(len(bundle.data), len(bundle.data), 'end')]:
        if start > pos:
            out.append((pos, start, bundle.data[pos:start]))
        pos = max(pos, end)
    return out


def safe_name(name):
    """Entry name usable as a file name (monster bundles prefix names with "\x13000")."""
    return ''.join('_' if c in '<>:"/\\|?*' or ord(c) < 32 else c for c in name.lstrip('\x13'))


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == '--verify':
        files = sorted(f for f in os.listdir(args[1]) if f.lower().endswith(('.bdp', '.cmp')))
        types = collections.Counter()
        unexplained = 0
        for f in files:
            with open(os.path.join(args[1], f), 'rb') as h:
                b = Bundle(h.read())
            for start, end, fill in gaps(b):
                if fill.strip(b'\0') and fill.strip(b'\xff'):
                    unexplained += 1
                    print(f'{f}: {end - start} unexplained bytes at {start:#x}: {fill[:16].hex(" ")}')
            for s in b.segments:
                for e in s.entries:
                    types[e.type] += 1
        print(f'{len(files)} bundles parsed, {unexplained} unexplained ranges (padding is 0x00 or 0xFF)')
        print('entry types:', ', '.join(f'{t}: {n}' for t, n in sorted(types.items())))
    elif len(args) in (1, 2):
        with open(args[0], 'rb') as h:
            b = Bundle(h.read())
        print(f'absent segments {b.absent:#x}, {len(b.names)} names')
        for s in b.segments:
            if not s.records:
                continue
            print(f'segment {s.number}: {len(s.records)} entries, data {s.data_size:#x} bytes at {s.data_offset:#x}, '
                  f'data2 {s.data2_size:#x} bytes at {s.data2_offset:#x}')
            for e, r in zip(s.entries, s.records):
                print(f'  [{e.index:3}] type {e.type:3} flags {e.flags:#06x} extra {e.extra:#04x} '
                      f'data {r.data_size:#8x} data2 {r.data2_size:#8x} w8 {e.word8:#x} wC {e.wordC:#x}  '
                      f'{b.name(e.name)}')
        if len(args) == 2:
            os.makedirs(args[1], exist_ok=True)
            for s in range(SEGMENTS):
                for e, a, d2 in b.items(s):
                    base = os.path.join(args[1], f'{s}_{e.index:03}_{safe_name(b.name(e.name))}.t{e.type}')
                    if a:
                        open(base, 'wb').write(a)
                    if d2:
                        open(base + '.data2', 'wb').write(d2)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
