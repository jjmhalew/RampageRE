"""Read MONSTER_DATA, a monster's move graph (raw entry in <Monster>.cmp; docs/FORMAT_MONSTER.md).

Loaded by 0x2F3A50 (version 11 or it is refused) and fixed up in place by 0x2F3C10: name indices become strings
(the bundle's name table holds the clip, state, callback and effect names) and offsets pointers. Layout (offsets from
the start of the entry, little endian):
  0x000         u32 version 11
  0x004..0x128  74 event slots: index of a start list (+0xA5C) per event, the defaults for every mode
  0x12C..0x9C0  not decoded
  0x9C4..0xA2C  name indices (-1 = none) and settings (floats 0x9F4..0xA10)
  0xA30 / 0xA34 / 0xA38  skeleton, mesh and cloth model names     0xAA4  the monster's property book (George_prop.prb)
  0xA44 / 0xA48  11 x 24 bytes: collision shapes (3 floats, 2 floats, bone | part << 16)
  0xA4C / 0xA50  state nodes, 0x84 bytes each: +0x78 clip name, +0x7C state name, the rest settings
  0xA60 / 0xA64 / 0xA68  coded items: u16 offset per item (at A68) into a block (at A64), u32 type first
  0xA6C / 0xA70 / 0xA74  body parts (Arm L, Arm R, Body, Head, Leg L, Leg R): data, then a pointer per part
  0xA58 / 0xA5C  start lists, 12 bytes each: u32 ?, offset of a state, u32 ?
  0xA94 / 0xA98  modes, 0x128 bytes each: 74 slots overriding the defaults (-1 = keep)
  0xA78 / 0xA7C / 0xA80  2 items: data, then pointers
  0xA84 / 0xA88  attachment nodes, 0x38 bytes: f32[12] matrix, u32 name, u32 bone | index << 16
  0xA8C / 0xA90  effect names
  0xA54  the move graph: header 0x34 bytes (+0x2C state count, +0x30 offset of the first state), then the states:
         0x2C bytes (+0x0 index, +0x4 offset of its node, +0x8 f32 clip length, +0xE i16 transitions, +0x14 offset,
         +0x18 u8[16], +0x28 offset of the transitions) and 0x74 bytes per transition (+0x4 offset of the target state,
         +0x8 flags, +0x10 / +0x18 f32 time window, +0x1E i16 conditions, +0x20 four 16-byte conditions {u32 kind,
         u32 id, u32 value, u32 ?}, +0x60 callback name)
  then byte tables to the end (not decoded)

Usage: python tools/monster.py --verify VOLDIR          parse every monster, check that the tables follow each other
       python tools/monster.py VOLDIR MONSTER [OUT]     print the move graph
"""
import os
import struct
import sys

import bdp

VERSION = 11
SLOTS = 74


class MonsterData:
    def __init__(self, data, names):
        self.d = data
        self.names = names
        if self.u(0) != VERSION:
            raise ValueError(f'version {self.u(0)}')
        self.slots = [self.i(4 + 4 * k) for k in range(SLOTS)]
        self.parts = []                                  # (name, start, end) of every table, in file order
        n_shapes, shapes = self.u(0xA44), self.u(0xA48)
        self.part('collision shapes', shapes, 24 * n_shapes)
        n_nodes, nodes = self.u(0xA4C), self.u(0xA50)
        self.part('state nodes', nodes, 0x84 * n_nodes)
        self.nodes = [(nodes + 0x84 * k, self.name(self.i(nodes + 0x84 * k + 0x78)),
                       self.name(self.i(nodes + 0x84 * k + 0x7C))) for k in range(n_nodes)]
        n_items, items, item_offsets = self.u(0xA60), self.u(0xA64), self.u(0xA68)
        self.part('coded item offsets', item_offsets, (2 * n_items + 3) & ~3)
        n_bodyparts, bodydata, bodyptrs = self.u(0xA6C), self.u(0xA70), self.u(0xA74)
        self.part('coded items', items, bodydata - items)
        self.item_types = [self.u(items + struct.unpack_from('<H', data, item_offsets + 2 * k)[0])
                           for k in range(n_items)]
        self.part('body part data', bodydata, bodyptrs - bodydata)
        self.part('body part pointers', bodyptrs, 4 * n_bodyparts)
        self.bodyparts = [self.name(self.i(self.u(bodyptrs + 4 * k))) for k in range(n_bodyparts)]
        n_lists, lists = self.u(0xA58), self.u(0xA5C)
        self.part('start lists', lists, 12 * n_lists)
        self.lists = [struct.unpack_from('<III', data, lists + 12 * k) for k in range(n_lists)]
        n_modes, modes = self.u(0xA94), self.u(0xA98)
        self.part('modes', modes, 0x128 * n_modes)
        self.modes = [[self.i(modes + 0x128 * m + 4 * k) for k in range(SLOTS)] for m in range(n_modes)]
        n_x, xdata, xptrs = self.u(0xA78), self.u(0xA7C), self.u(0xA80)
        self.part('items A78', xdata, xptrs - xdata)
        self.part('items A78 pointers', xptrs, 4 * n_x)
        n_att, att = self.u(0xA84), self.u(0xA88)
        self.part('attachment nodes', att, 0x38 * n_att)
        self.attachments = [self.name(self.i(att + 0x38 * k + 0x30)) for k in range(n_att)]
        n_fx, fx = self.u(0xA8C), self.u(0xA90)
        self.part('effect names', fx, 4 * n_fx)
        self.effects = [self.name(self.i(fx + 4 * k)) for k in range(n_fx)]
        graph = self.u(0xA54)
        self.part('move graph header', graph, 0x34)
        n_states, at = self.u(graph + 0x2C), self.u(graph + 0x30)
        first = at
        self.states = []
        for k in range(n_states):
            count = struct.unpack_from('<h', data, at + 0xE)[0]
            if self.u(at) != k or self.u(at + 0x28) != at + 0x2C:
                raise ValueError(f'state {k} at {at:#x}')
            transitions = []
            for t in range(count):
                o = at + 0x2C + 0x74 * t
                nc = struct.unpack_from('<h', data, o + 0x1E)[0]
                conds = [struct.unpack_from('<IIII', data, o + 0x20 + 16 * c) for c in range(nc)]
                transitions.append(dict(target=self.u(o + 4), flags=self.u(o + 8), window=(self.f(o + 0x10),
                                        self.f(o + 0x18)), conditions=conds, call=self.name(self.i(o + 0x60))))
            self.states.append(dict(at=at, node=self.u(at + 4), length=self.f(at + 8), transitions=transitions))
            at += 0x2C + 0x74 * count
        self.part('states', first, at - first)
        self.tail = (at, len(data))
        self.parts.sort(key=lambda p: p[1])
        self.by_offset = {s['at']: k for k, s in enumerate(self.states)}
        self.node_by_offset = {n[0]: n for n in self.nodes}

    def u(self, o):
        return struct.unpack_from('<I', self.d, o)[0]

    def i(self, o):
        return struct.unpack_from('<i', self.d, o)[0]

    def f(self, o):
        return struct.unpack_from('<f', self.d, o)[0]

    def name(self, i):
        if i < 0 or i >= len(self.names):
            return None
        n = self.names[i].lstrip('\x13')
        return n[3:] if n.startswith('000') else n

    def part(self, name, start, size):
        self.parts.append((name, start, start + size))

    def check(self):
        """Raises unless the tables follow each other from the header (0xAA8) to the tail."""
        at = 0xAA8
        for name, start, end in self.parts:
            if start != at:
                raise ValueError(f'{name} at {start:#x}, expected {at:#x}')
            at = end
        if at != self.tail[0]:
            raise ValueError(f'tables end at {at:#x}, the states at {self.tail[0]:#x}')

    def state_name(self, offset):
        k = self.by_offset.get(offset)
        if k is None:
            return f'?{offset:#x}'
        node = self.node_by_offset.get(self.states[k]['node'])
        return f'{k}:{node[2] if node else "?"}'

    def dump(self):
        out = ['// slots (event -> start list -> state) per mode; - = the default']
        for k, l in enumerate(self.slots):
            row = [self.list_state(l)] + [self.list_state(m[k]) if m[k] >= 0 else '-' for m in self.modes]
            out.append(f'slot {k:2d}: ' + ' | '.join(row))
        out.append('')
        out.append(f'// {len(self.states)} states')
        for k, s in enumerate(self.states):
            node = self.node_by_offset.get(s['node'])
            out.append(f'state {self.state_name(s["at"])}  clip {node[1] if node else "?"}  length {s["length"]:.3g}')
            for t in s['transitions']:
                conds = ' '.join(f'{c[0]}/{c[1]}={c[2]}{"!" if c[3] else ""}' for c in t['conditions'])
                call = f' call {t["call"]}' if t['call'] else ''
                out.append(f'    -> {self.state_name(t["target"])}  [{conds}]  window {t["window"][0]:.3g}..'
                           f'{t["window"][1]:.3g}  flags {t["flags"]:#x}{call}')
        return '\n'.join(out) + '\n'

    def list_state(self, index):
        if index < 0 or index >= len(self.lists):
            return str(index)
        return self.state_name(self.lists[index][1])


def monster_data(voldir, name):
    with open(os.path.join(voldir, name + '.cmp'), 'rb') as f:
        b = bdp.Bundle(f.read())
    for e, a, _ in b.items(0):
        if b.name(e.name).lstrip('\x13').endswith('MONSTER_DATA'):
            return MonsterData(a, b.names)
    raise ValueError('no MONSTER_DATA')


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == '--verify':
        monsters = sorted(f[:-10] for f in os.listdir(args[1]) if f.lower().endswith('_anims.cmp'))
        bad = 0
        for m in monsters:
            try:
                md = monster_data(args[1], m)
                md.check()
                print(f'{m}: {len(md.states)} states, {sum(len(s["transitions"]) for s in md.states)} transitions, '
                      f'{len(md.nodes)} nodes, {len(md.item_types)} coded items, tail {md.tail[1] - md.tail[0]} bytes')
            except (ValueError, struct.error) as err:
                bad += 1
                print(f'{m}: {err}')
        print(f'{len(monsters) - bad} of {len(monsters)} parsed, tables in order')
        sys.exit(1 if bad else 0)
    elif len(args) in (2, 3):
        md = monster_data(args[0], args[1])
        text = md.dump()
        if len(args) == 3:
            with open(args[2], 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
        else:
            sys.stdout.write(text)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
