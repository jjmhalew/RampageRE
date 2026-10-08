"""Read Rampage skeletons (bundle entry type 3) and animations (type 4); export animated monsters as glTF.

Both are used in place by the game, like the property books: offsets in the data become pointers at load time.
See docs/FORMAT_BDP.md ("Skeletons" and "Animations").

Skeleton / node tree (type 3; 0x1A8110 makes a NodeTree, vtable 0x444D30, or a Skeleton, vtable 0x444E60, when the
entry's extra byte is 1; 0x195730 and 0x19E070 fix up the offsets):
  0x00  0x1C bytes  runtime object (vtable, reference count, ...)
  0x1C  u32  offset of the first node (0x3C, or 0x40 for a skeleton)
  0x20  u32  node count
  0x24  u32  tree depth (levels; one more on skeletons), the size of the walk stack
  0x28  u32  size of the entry
  0x2C  u32  offset of the node table: one u32 offset per node, in node order
  0x30  u32  offset of the same nodes sorted by name index (0x19A270 binary searches it)
  0x34  u32  mirror pair count
  0x38  u32  offset of the mirror table (0 = none): u16 per node, first the left/right pairs, then the centre nodes
  0x3C  u32  (skeleton) offset of the bind pose: 0x50 bytes per node
Node: i32 index, i32 parent (-1 = root), u32 child count, u32 name index, u32 child offsets.
Bind pose record: u32 0, f32[12] inverse bind matrix (x, y, z axis rows and translation; row vectors),
  f32[3] local translation, f32[4] local rotation x, y, z, w (row-vector convention, i.e. the conjugate of the
  usual column-vector quaternion).

Animation (type 4; 0x196730 makes a KeyInterpAnim, vtable 0x444C30, when the kind is 2, or a RawAnim, vtable
0x444DF0, when it is 1):
  0x1C  f32  duration in seconds        0x20  u32  kind        0x24  u32  size        0x28  u16  skeleton name index
  KeyInterpAnim (every monster and prop animation; fixed up by 0x190720):
  0x2C  u32  bone count  0x30  u32  bone count again  0x34  u32  offset of the channel mask (2 bits per bone,
  always all set)  0x38  u32  offset of the translation tracks  0x3C  u32  offset of the rotation tracks
  0x40  u32  runtime size (136 bytes per bone)
  Translation track (0x191860), one per bone in bone order: f32[3] scale, u8 bone, u8 key count, u8 key count - 1,
    u8 0, then per
    key u16 time, s16 x, y, z; value = s16 * scale * f32(0x38000100) (~1/32768).
  Rotation track (0x191C50), one per bone: u8 bone, u8 key count, then per key u16 time, s16 a, b, c (0x192010):
    x, y, z = (a >> 3, b >> 3, c >> 3) * f32(0x39800801) (~1/4096); w = sqrt(1 - (x^2 + y^2 + z^2 + r * k)) with the
    correction r = (a & 3) << 6 | (b & 7) << 3 | (c & 7), negative when a & 4, k = f32(0x3B808081) * f32(0x39DD7E7C);
    w is negated when bit 0 of the time is set.
  Key times are (time & 0xFFFE) / 65534 of the duration. A track without keys uses the bind pose; between keys
  translations are interpolated linearly and rotations spherically.
  RawAnim (one per node tree, same name; on skeletons the first key is the bind pose; fixed up by 0x19C700):
  0x2C  u32  record count  0x30  u32  offset of the record table (sorted by bone)  0x34  u32  first record
  Record: u32 bone, f32 ?, u32 translation keys, u32 rotation keys, f32 0, f32 1, f32 1, u32 offset of the
    translation keys (6 x f32: translation, then 3 not decoded), u32 offset of the rotation keys (9 x f32:
    quaternion, then 5 not decoded); the next record follows the rotation keys.

Usage: python tools/anim.py --verify VOLDIR                check every skeleton and animation
       python tools/anim.py VOLDIR MONSTER OUTDIR            write OUTDIR/MONSTER.gltf: mesh, skeleton, animations
       python tools/anim.py VOLDIR --all OUTDIR              every monster (each NAME with NAME.cmp + NAME_anims.cmp)
"""
import collections
import math
import os
import struct
import sys

import bdp
import mesh

Node = collections.namedtuple('Node', 'index parent name children')
Pose = collections.namedtuple('Pose', 'inverse_bind translation rotation')
Track = collections.namedtuple('Track', 'bone times values')  # times 0..1 of the duration


def f32(bits):
    return struct.unpack('<f', struct.pack('<I', bits))[0]


TRANSLATION_UNIT = f32(0x38000100)
ROTATION_UNIT = f32(0x39800801)
ROTATION_CORRECTION = f32(0x3B808081) * f32(0x39DD7E7C)
TIME_SCALE = 65534.0
KEY_INTERP, RAW = 2, 1


def align4(n):
    return (n + 3) & ~3


class Skeleton:
    """A node tree (type 3), with the bind pose when it is a skeleton."""

    def __init__(self, data, names):
        (self.first, n, self.depth, self.size, table, by_name, self.mirror_pairs,
         mirror) = struct.unpack_from('<8I', data, 0x1C)
        self.nodes = []
        offsets = struct.unpack_from(f'<{n}I', data, table)
        for at in offsets:
            index, parent, count, name = struct.unpack_from('<iiII', data, at)
            self.nodes.append(Node(index, parent, names[name].lstrip('\x13'),
                                   [offsets.index(c) for c in struct.unpack_from(f'<{count}I', data, at + 16)]))
        self.node_offsets = offsets
        self.by_name = [offsets.index(o) for o in struct.unpack_from(f'<{n}I', data, by_name)]
        self.name_indices = [struct.unpack_from('<I', data, o + 12)[0] for o in offsets]
        self.mirror = list(struct.unpack_from(f'<{n}H', data, mirror)) if mirror else None
        self.poses = None
        if self.first == 0x40:
            at = struct.unpack_from('<I', data, 0x3C)[0]
            self.poses_at = at
            self.poses = []
            for i in range(n):
                r = struct.unpack_from('<I19f', data, at + 0x50 * i)
                if r[0]:
                    raise ValueError(f'bind pose {i} starts with {r[0]:#x}')
                self.poses.append(Pose(r[1:13], r[13:16], r[16:20]))
        self.layout_end = self.check_layout(data, table, by_name, mirror)

    def check_layout(self, data, table, by_name, mirror):
        """Offset just past the last part, when every part follows the one before it (else raises)."""
        n = len(self.nodes)
        at = self.first
        for i, o in enumerate(self.node_offsets):
            if o != at or self.nodes[i].index != i:
                raise ValueError(f'node {i} at {o:#x}, expected {at:#x}')
            at += 16 + 4 * len(self.nodes[i].children)
        for part, size in ((table, 4 * n), (by_name, 4 * n), (mirror, align4(2 * n)),
                           (self.poses_at if self.poses else 0, 0x50 * n)):
            if not part:
                continue
            if part != at:
                raise ValueError(f'part at {part:#x}, expected {at:#x}')
            at += size
        return at

    def local_matrix(self, i):
        """Column-vector 4x4 (row lists) of node i's bind transform relative to its parent."""
        t, (x, y, z, w) = self.poses[i].translation, self.poses[i].rotation
        x, y, z = -x, -y, -z  # stored for row vectors
        return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), t[0]],
                [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), t[1]],
                [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), t[2]],
                [0.0, 0.0, 0.0, 1.0]]

    def inverse_bind(self, i):
        """Column-vector 4x4 (row lists) of the stored inverse bind matrix."""
        m = self.poses[i].inverse_bind
        return [[m[0], m[3], m[6], m[9]], [m[1], m[4], m[7], m[10]], [m[2], m[5], m[8], m[11]], [0.0, 0.0, 0.0, 1.0]]

    def world(self):
        out = []
        for i, node in enumerate(self.nodes):
            local = self.local_matrix(i)
            out.append(local if node.parent < 0 else matmul(out[node.parent], local))
        return out


def matmul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def decode_rotation(data, at):
    """(time, (x, y, z, w)) of the compressed rotation key at `at`, as 0x192010 does it."""
    time, a, b, c = struct.unpack_from('<Hhhh', data, at)
    x, y, z = (a >> 3) * ROTATION_UNIT, (b >> 3) * ROTATION_UNIT, (c >> 3) * ROTATION_UNIT
    s = min(x * x + y * y + z * z, 1.0)
    r = (a & 3) << 6 | (b & 7) << 3 | (c & 7)
    s += (-r if a & 4 else r) * ROTATION_CORRECTION
    if s <= 1.0:
        w = math.sqrt(1.0 - s)
    else:
        norm = math.sqrt(x * x + y * y + z * z)
        x, y, z, w = x / norm, y / norm, z / norm, 0.0
    if time & 1:
        w = -w
    return time & 0xFFFE, (x, y, z, w)


class Animation:
    """A KeyInterpAnim: per bone a translation and a rotation track."""

    def __init__(self, data, names):
        (self.duration, self.kind, self.size, self.skeleton, pad, self.bones, bones2, mask, translations,
         rotations, self.work_size) = struct.unpack_from('<fIIHHIIIIII', data, 0x1C)
        if self.kind != KEY_INTERP or pad or bones2 != self.bones:
            raise ValueError(f'not a KeyInterpAnim header (kind {self.kind})')
        self.skeleton_name = names[self.skeleton].lstrip('\x13')
        self.mask = int.from_bytes(data[mask:translations], 'little')
        self.translations, self.rotations = [], []
        at = translations
        for i in range(self.bones):
            sx, sy, sz, bone, count, last, zero = struct.unpack_from('<3fBBBB', data, at)
            if bone != i or last != count - 1 or zero:
                raise ValueError(f'translation track {i} at {at:#x} is for bone {bone}')
            keys = [struct.unpack_from('<Hhhh', data, at + 0x10 + 8 * k) for k in range(count)]
            self.translations.append(Track(bone, [(t & 0xFFFE) / TIME_SCALE for t, *_ in keys],
                                           [(x * sx * TRANSLATION_UNIT, y * sy * TRANSLATION_UNIT,
                                             z * sz * TRANSLATION_UNIT) for _, x, y, z in keys]))
            at += 0x10 + 8 * count
        if at != rotations:
            raise ValueError(f'translation tracks end at {at:#x}, rotation tracks start at {rotations:#x}')
        for i in range(self.bones):
            bone, count = data[at], data[at + 1]
            if bone != i:
                raise ValueError(f'rotation track {i} at {at:#x} is for bone {bone}')
            keys = [decode_rotation(data, at + 2 + 8 * k) for k in range(count)]
            self.rotations.append(Track(bone, [t / TIME_SCALE for t, _ in keys], [q for _, q in keys]))
            at += 2 + 8 * count
        self.end = at


class RawAnim:
    """A RawAnim: per node the first translation and rotation key (on skeletons, the bind pose)."""

    def __init__(self, data, names):
        self.duration, self.kind, self.size, self.skeleton, count, table, first = struct.unpack_from(
            '<fIIHxxIII', data, 0x1C)
        if self.kind != RAW:
            raise ValueError(f'not a RawAnim header (kind {self.kind})')
        self.skeleton_name = names[self.skeleton].lstrip('\x13')
        self.records = {}
        offsets = struct.unpack_from(f'<{count}I', data, table)
        if table + 4 * count != first:
            raise ValueError('RawAnim records do not follow the table')
        at = first
        for o in offsets:
            if o != at:
                raise ValueError(f'RawAnim record at {o:#x}, expected {at:#x}')
            bone, length, nt, nr, zero, one, one2, t_at, r_at = struct.unpack_from('<IfIIfffII', data, at)
            if t_at != 0x24 or r_at != 0x24 + 24 * nt:
                raise ValueError(f'RawAnim record at {at:#x}: keys at {t_at:#x} / {r_at:#x}')
            t = struct.unpack_from('<6f', data, at + t_at)
            q = struct.unpack_from('<9f', data, at + r_at)
            self.records[bone] = (length, nt, nr, (zero, one, one2), t, q)
            at += r_at + 36 * nr
        self.end = at


def bundle_items(path):
    with open(path, 'rb') as f:
        b = bdp.Bundle(f.read())
    return b, list(b.items(0))


def verify(voldir):
    counts = collections.Counter()
    worst_bind = worst_raw = worst_norm = 0.0
    problems = []
    for f in sorted(os.listdir(voldir)):
        if not f.lower().endswith(('.bdp', '.cmp')):
            continue
        b, items = bundle_items(os.path.join(voldir, f))
        skeletons = {}
        for e, a, _ in items:
            if e.type != 3:
                continue
            try:
                s = Skeleton(a, b.names)
                if s.layout_end != len(a) or s.size != len(a):
                    raise ValueError(f'parts end at {s.layout_end:#x}, entry is {len(a):#x}')
                if (e.extra == 1) != (s.poses is not None):
                    raise ValueError('bind pose present/absent against the entry flag')
                if s.mirror and sorted(s.mirror) != list(range(len(s.nodes))):
                    raise ValueError('mirror table is not a permutation of the nodes')
                if [s.name_indices[i] for i in s.by_name] != sorted(s.name_indices):
                    raise ValueError('name table is not sorted by name index')
                if s.poses:
                    for i, w in enumerate(s.world()):
                        p = matmul(s.inverse_bind(i), w)
                        worst_bind = max(worst_bind, max(abs(p[r][c] - (r == c)) for r in range(4) for c in range(4)))
                skeletons[e.name] = s
                counts['skeletons' if s.poses else 'node trees'] += 1
            except (ValueError, struct.error) as err:
                problems.append(f'{f} {b.name(e.name)!r}: {err}')
        for e, a, _ in items:
            if e.type != 4:
                continue
            try:
                kind = struct.unpack_from('<I', a, 0x20)[0]
                anim = Animation(a, b.names) if kind == KEY_INTERP else RawAnim(a, b.names)
                if anim.end != len(a) or anim.size != len(a):
                    raise ValueError(f'tracks end at {anim.end:#x}, entry is {len(a):#x}')
                s = skeletons.get(anim.skeleton)
                if s is None:
                    raise ValueError(f'skeleton {anim.skeleton_name!r} is not in the bundle')
                if kind == KEY_INTERP:
                    if anim.bones != len(s.nodes) or anim.work_size != 136 * anim.bones:
                        raise ValueError(f'{anim.bones} bones, skeleton has {len(s.nodes)}')
                    if anim.mask != (1 << 2 * anim.bones) - 1:
                        raise ValueError(f'channel mask {anim.mask:#x}')
                    for tr in anim.translations + anim.rotations:
                        if not tr.times or any(b2 <= a2 for a2, b2 in zip(tr.times, tr.times[1:])):
                            raise ValueError(f'bone {tr.bone}: key times {tr.times[:6]} are not increasing')
                    for tr in anim.rotations:
                        for q in tr.values:
                            worst_norm = max(worst_norm, abs(math.sqrt(sum(c * c for c in q)) - 1))
                    counts['keyframed animations'] += 1
                    counts['keys'] += sum(len(t.times) for t in anim.translations + anim.rotations)
                else:
                    if sorted(anim.records) != list(range(len(s.nodes))):
                        raise ValueError('RawAnim does not cover every node')
                    if s.poses:  # the bind pose a second time: compare
                        for i, (length, nt, nr, consts, t, q) in anim.records.items():
                            p = s.poses[i]
                            worst_raw = max(worst_raw, max(abs(u - v) for u, v in zip(t[:3], p.translation)),
                                            min(max(abs(u - v) for u, v in zip(q[:4], p.rotation)),
                                                max(abs(u + v) for u, v in zip(q[:4], p.rotation))))
                    counts['raw animations'] += 1
            except (ValueError, struct.error) as err:
                problems.append(f'{f} {b.name(e.name)!r}: {err}')
    for p in problems[:20]:
        print(p)
    print(', '.join(f'{v} {k}' for k, v in counts.items()) + f'; {len(problems)} problems')
    print(f'bind pose chain vs stored inverse bind: max error {worst_bind:.2e}; RawAnim vs skeleton bind pose: '
          f'{worst_raw:.2e}; rotation key |q| - 1: {worst_norm:.2e}')
    return not problems


# ---------------------------------------------------------------- glTF

def column_major(m):
    return [m[r][c] for c in range(4) for r in range(4)]


def gltf_rotation(q):
    x, y, z, w = q
    return [-x, -y, -z, w]


def scalar_accessor(g, values):
    i = g.accessor(values, 'SCALAR', target=None)
    g.doc['accessors'][i]['min'] = [min(values)]
    g.doc['accessors'][i]['max'] = [max(values)]
    return i


def nearest_bone(skel, world, p):
    """Node whose bone (the segment from it to a child, or just its joint) passes closest to p."""
    best, best_d = 0, None
    for i, node in enumerate(skel.nodes):
        a = [world[i][k][3] for k in range(3)]
        for end in ([world[c] for c in node.children] or [world[i]]):
            b = [end[k][3] for k in range(3)]
            ab = [b[k] - a[k] for k in range(3)]
            ll = sum(c * c for c in ab)
            t = 0.0 if ll == 0 else max(0.0, min(1.0, sum((p[k] - a[k]) * ab[k] for k in range(3)) / ll))
            d = sum((p[k] - a[k] - t * ab[k]) ** 2 for k in range(3))
            if best_d is None or d < best_d:
                best, best_d = i, d
    return best


def anim_name(name):
    name = name.lstrip('\x13')
    return name[3:] if name.startswith('000') else name


def export_monster(voldir, monster, out):
    files = {f.lower(): f for f in os.listdir(voldir)}
    b, items = bundle_items(os.path.join(voldir, files[monster.lower() + '.cmp']))
    g = mesh.Gltf()
    assets = mesh.BundleAssets(b, g, out, monster + '_textures')
    skel = next(Skeleton(a, b.names) for e, a, _ in items if e.type == 3 and e.extra == 1)
    joints = []
    for i, node in enumerate(skel.nodes):
        j = g.node(node.name, parent=0 if node.parent < 0 else joints[node.parent])
        g.doc['nodes'][j]['translation'] = list(skel.poses[i].translation)
        g.doc['nodes'][j]['rotation'] = gltf_rotation(skel.poses[i].rotation)
        joints.append(j)
    ibm = g.accessor([column_major(skel.inverse_bind(i)) for i in range(len(joints))], 'MAT4', target=None)
    g.doc['skins'] = [{'joints': joints, 'inverseBindMatrices': ibm, 'skeleton': joints[0], 'name': 'skeleton'}]
    world = skel.world()
    for entry in items:
        e = entry[0]
        if e.type == 17:
            m = assets.mesh(entry, skinned=True)
            if m is not None:
                n = g.node(assets.clean(e.name), m)
                g.doc['nodes'][n]['skin'] = 0
        elif e.type == 0:
            # cloth (hair, whiskers, ...): the game simulates it (type 25); here each vertex rides on the bone
            # nearest to it in the bind pose
            prims = []
            for part in mesh.read_mesh(0, entry[1], entry[2]):
                for s in part.sections:
                    bones = [nearest_bone(skel, world, p) for p in s.positions]
                    prims.append((assets.material(part.material),
                                  s._replace(bones=[(j, j) for j in bones], weights=[1.0] * len(bones))))
            m = g.mesh(assets.clean(e.name), prims, skinned=True)
            if m is not None:
                n = g.node(assets.clean(e.name), m)
                g.doc['nodes'][n]['skin'] = 0
    anims_file = files.get(monster.lower() + '_anims.cmp')
    count = 0
    if anims_file:
        ab, aitems = bundle_items(os.path.join(voldir, anims_file))
        g.doc['animations'] = []
        for e, a, _ in aitems:
            if e.type != 4 or struct.unpack_from('<I', a, 0x20)[0] != KEY_INTERP:
                continue
            anim = Animation(a, ab.names)
            if anim.bones != len(joints):
                continue
            samplers, channels = [], []
            for tracks, path, convert in ((anim.translations, 'translation', list),
                                          (anim.rotations, 'rotation', gltf_rotation)):
                for tr in tracks:
                    times = [t * anim.duration for t in tr.times]
                    samplers.append({'input': scalar_accessor(g, times),
                                     'output': g.accessor([convert(v) for v in tr.values],
                                                          'VEC3' if path == 'translation' else 'VEC4', target=None),
                                     'interpolation': 'LINEAR'})
                    channels.append({'sampler': len(samplers) - 1, 'target': {'node': joints[tr.bone], 'path': path}})
            g.doc['animations'].append({'name': anim_name(ab.name(e.name)), 'samplers': samplers,
                                        'channels': channels})
            count += 1
        g.doc['animations'].sort(key=lambda x: x['name'])
    os.makedirs(out, exist_ok=True)
    g.save(os.path.join(out, monster + '.gltf'))
    return count


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == '--verify':
        sys.exit(0 if verify(args[1]) else 1)
    elif len(args) == 3:
        voldir, monster, out = args
        if monster == '--all':
            names = sorted(f[:-len('_anims.cmp')] for f in os.listdir(voldir) if f.lower().endswith('_anims.cmp'))
        else:
            names = [monster]
        for name in names:
            print(f'{name}: {export_monster(voldir, name, out)} animations')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
