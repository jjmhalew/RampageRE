"""Dump Rampage property books (.prb, bundle entry type 22) as an indented tree.

A property book is used in place by the game: 0x1B00E0 puts a PropertyBook object (vtable 0x445380) over the
entry's data and 0x1B0170 turns its offsets into pointers. (0x1AFC80 builds the same thing from a text book.)

Layout (little endian; offsets are from the start of the entry data unless noted):
  0x00  0x1C bytes  room for the runtime object (vtable, ...); zero apart from two 1s
  0x1C  u32         number of top-level groups
  0x20  u32         size
  0x24  u32         offset of the group table: one u32 offset per group
Group:   u32 0, u32 name index, u32 record count, u32 ?, u32 header size (0x14), then the records
Record:  u32 kind, u32 name index, u32 value
  kind & 0xFF: 0 int, 1 float, 2 name (value = name index), 3 group (value = offset from the containing group)
  (kind >> 8) & 0xFF: 0 plain, 1 typed key "Type|Name" (kind >> 16 = length of "Type"), 2 list element without
  a name (the name index is 0, a placeholder), 3 list element of a type "Type|" (kind >> 16 = length of "Type")

Usage: python tools/prb.py BUNDLE [GROUP]       print every book in the bundle (or one top-level group)
"""
import struct
import sys

import bdp

INT, FLOAT, NAME, GROUP = 0, 1, 2, 3


class Book:
    def __init__(self, data, names):
        self.data = data
        self.names = names
        count, self.size, table = struct.unpack_from('<III', data, 0x1C)
        self.groups = [struct.unpack_from('<I', data, table + 4 * i)[0] for i in range(count)]

    def name(self, i):
        return self.names[i] if i < len(self.names) else f'#{i}'

    def group(self, at):
        """(name, [(kind, name, value)]) of the group at `at`; group values are absolute offsets."""
        zero, name, count, _, header = struct.unpack_from('<5I', self.data, at)
        if zero or header != 0x14:
            raise ValueError(f'no group at {at:#x}')
        records = []
        for i in range(count):
            kind, rname, value = struct.unpack_from('<III', self.data, at + header + 12 * i)
            t = kind & 0xFF
            if t == FLOAT:
                value = struct.unpack_from('<f', self.data, at + header + 12 * i + 8)[0]
            elif t == GROUP:
                value = at + value
            elif t == INT:
                value = struct.unpack_from('<i', self.data, at + header + 12 * i + 8)[0]
            records.append((kind, self.name(rname), value))
        return self.name(name), records

    def tree(self, at, depth=0, out=None, seen=None):
        out = [] if out is None else out
        seen = set() if seen is None else seen
        name, records = self.group(at)
        if depth == 0:
            out.append(f'{name}')
        for kind, rname, value in records:
            t = kind & 0xFF
            pad = '    ' * (depth + 1)
            if (kind >> 8) & 0xFF == 2:  # unnamed list element
                rname = '-'
            if t == GROUP:
                if value in seen:
                    out.append(f'{pad}{rname} = <group at {value:#x}>')
                    continue
                seen.add(value)
                sub, _ = self.group(value)
                label = '' if sub in (rname, rname.split('|')[-1]) or sub.startswith('#') else f'  // {sub}'
                out.append(f'{pad}{rname} {{{label}')
                self.tree(value, depth + 1, out, seen)
                out.append(f'{pad}}}')
            elif t == NAME:
                out.append(f'{pad}{rname} "{self.name(value)}"' if rname == '-' else f'{pad}{rname} = "{self.name(value)}"')
            elif t == FLOAT:
                out.append(f'{pad}{rname} {value:g}' if rname == '-' else f'{pad}{rname} = {value:g}')
            elif t == INT:
                out.append(f'{pad}{rname} {value}' if rname == '-' else f'{pad}{rname} = {value}')
            else:
                out.append(f'{pad}{rname} = ?{kind:#x}:{value:#x}')
        return out


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    with open(sys.argv[1], 'rb') as f:
        b = bdp.Bundle(f.read())
    for e, a, _ in b.items(0):
        if e.type != 22:
            continue
        book = Book(a, b.names)
        print(f'// {b.name(e.name)}: {len(book.groups)} groups')
        for g in book.groups:
            if len(sys.argv) == 3 and book.group(g)[0] != sys.argv[2]:
                continue
            print('\n'.join(book.tree(g)))


if __name__ == '__main__':
    main()
