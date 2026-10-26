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
  Record: u32 bone, f32 ?, u32 translation segments, u32 rotation segments, f32 start 0, f32 end 1, f32 1 / (end -
    start) = 1, u32 offset of the translation segments, u32 offset of the rotation segments; the next record
    follows. n segments split the clip evenly: at time t segment k = int(t * n) runs with f = t * n - k.
  Translation segment (0x19CEC0): f32[3] p, f32[3] d; value = p + d * f.
  Rotation segment (0x19D0A0, 0x167DE0): f32[4] q0, f32[4] q1, f32 angle a; value = normalise(q0 cos(a f) +
    q1 sin(a f)), or normalise(q0 + q1 f) when a is 0 (a slerp worked out in advance). Segments join up.

Cloth (type 25, hair, whiskers, ...; used in place, vtable 0x446500, fixed up by 0x1DB840):
  0x1C  f32[3] bounding sphere centre, f32 radius squared, f32 radius
  0x30  u32 size  0x34  u32 part count  0x38  u32 offset of the parts (0x8C bytes each)
  0x3C  u32 bone count  0x40  u32 offset of the bone list (u32 skeleton node indices)  0x44  u32 0  0x48  u32 offset
  Part: f32[19] simulation settings (gravity 9.8 on flags, timesteps 1/60 and 1/30 among them), u32 2 or 3,
    u8[4] flags, u8[4], f32 100, u32 0, then count/offset pairs: pinned particles (u32 index), springs (24 bytes: u32 a,
    u32 b, f32 rest length, its square, f32 stiffness?, f32 ?), bend links (flags only; 32 bytes: u32[4]
    particles, f32[3] 0, f32 20), triangles (3 x u32), and
    particles (count twice, then the offset; 64 bytes (the Wii's 72: 8 bytes 0xEA after the normal): f32[3]
    position in the bind pose's model space, f32[3]
    normal, f32 ?, f32[4] bone weights, u16[4] skeleton node indices (0xFFFF = none; flags have none), u8 bone
    count, u8 0xEA,
    u16 1 = held by its bones, 8 x 0xFF). Every vertex of the cloth mesh sits on one particle.

Usage: python tools/anim.py --verify VOLDIR                check every skeleton and animation
       python tools/anim.py VOLDIR MONSTER OUTDIR            write OUTDIR/MONSTER.gltf: mesh, skeleton, animations
       python tools/anim.py VOLDIR --all OUTDIR              every monster (each NAME with NAME.cmp + NAME_anims.cmp,
                                                             or the Wii's .cmg)
       python tools/anim.py VOLDIR --models BUNDLE OUTDIR    every animated node-tree model of BUNDLE (people,
                                                             vehicles, bosses, ...) to OUTDIR/BUNDLE/models
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
Particle = collections.namedtuple('Particle', 'position normal value weights bones held')
ClothPart = collections.namedtuple('ClothPart', 'settings pins springs bends triangles particles')
Track = collections.namedtuple('Track', 'bone times values')  # times 0..1 of the duration
RawTrack = collections.namedtuple('RawTrack', 'value translations rotations')  # value: the f32 at +4, not decoded


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

    def __init__(self, data, names, endian='<'):
        self.e = endian
        (self.first, n, self.depth, self.size, table, by_name, self.mirror_pairs,
         mirror) = struct.unpack_from(self.e + '8I', data, 0x1C)
        self.nodes = []
        offsets = struct.unpack_from(f'{self.e}{n}I', data, table)
        for at in offsets:
            index, parent, count, name = struct.unpack_from(self.e + 'iiII', data, at)
            self.nodes.append(Node(index, parent, names[name].lstrip('\x13'),
                                   [offsets.index(c) for c in struct.unpack_from(f'{self.e}{count}I', data, at + 16)]))
        self.node_offsets = offsets
        self.by_name = [offsets.index(o) for o in struct.unpack_from(f'{self.e}{n}I', data, by_name)]
        self.name_indices = [struct.unpack_from(self.e + 'I', data, o + 12)[0] for o in offsets]
        self.mirror = list(struct.unpack_from(f'{self.e}{n}H', data, mirror)) if mirror else None
        self.poses = None
        if self.first == 0x40:
            at = struct.unpack_from(self.e + 'I', data, 0x3C)[0]
            self.poses_at = at
            self.poses = []
            for i in range(n):
                r = struct.unpack_from(self.e + 'I19f', data, at + 0x50 * i)
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


def decode_rotation(data, at, endian='<'):
    """(time, (x, y, z, w)) of the compressed rotation key at `at`, as 0x192010 does it."""
    time, a, b, c = struct.unpack_from(endian + 'Hhhh', data, at)
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

    def __init__(self, data, names, endian='<'):
        self.e = endian
        (self.duration, self.kind, self.size, self.skeleton, pad, self.bones, bones2, mask, translations,
         rotations, self.work_size) = struct.unpack_from(self.e + 'fIIHHIIIIII', data, 0x1C)
        if self.kind != KEY_INTERP or pad or bones2 != self.bones:
            raise ValueError(f'not a KeyInterpAnim header (kind {self.kind})')
        self.skeleton_name = names[self.skeleton].lstrip('\x13')
        words = struct.unpack_from(f'{endian}{(translations - mask) // 4}I', data, mask)
        self.mask = sum(w << 32 * i for i, w in enumerate(words))     # u32 words, the first the lowest
        self.translations, self.rotations = [], []
        at = translations
        for i in range(self.bones):
            sx, sy, sz, bone, count, last, zero = struct.unpack_from(self.e + '3fBBBB', data, at)
            if bone != i or last != count - 1 or zero:
                raise ValueError(f'translation track {i} at {at:#x} is for bone {bone}')
            keys = [struct.unpack_from(self.e + 'Hhhh', data, at + 0x10 + 8 * k) for k in range(count)]
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
            keys = [decode_rotation(data, at + 2 + 8 * k, endian) for k in range(count)]
            self.rotations.append(Track(bone, [t / TIME_SCALE for t, _ in keys], [q for _, q in keys]))
            at += 2 + 8 * count
        self.end = at


class RawAnim:
    """A RawAnim: per node, translation and rotation segments spread evenly over the clip."""

    def __init__(self, data, names, endian='<'):
        self.e = endian
        self.duration, self.kind, self.size, self.skeleton, count, table, first = struct.unpack_from(
            self.e + 'fIIHxxIII', data, 0x1C)
        if self.kind != RAW:
            raise ValueError(f'not a RawAnim header (kind {self.kind})')
        self.skeleton_name = names[self.skeleton].lstrip('\x13')
        self.records = {}
        offsets = struct.unpack_from(f'{self.e}{count}I', data, table)
        if table + 4 * count != first:
            raise ValueError('RawAnim records do not follow the table')
        at = first
        for o in offsets:
            if o != at:
                raise ValueError(f'RawAnim record at {o:#x}, expected {at:#x}')
            bone, value, nt, nr, start, stop, scale, t_at, r_at = struct.unpack_from(self.e + 'IfIIfffII', data, at)
            if t_at != 0x24 or r_at != 0x24 + 24 * nt or not nt or not nr:
                raise ValueError(f'RawAnim record at {at:#x}: keys at {t_at:#x} / {r_at:#x}')
            if (start, stop, scale) != (0.0, 1.0, 1.0):
                raise ValueError(f'RawAnim record at {at:#x}: times {start}, {stop}, {scale}')
            translations = []
            for k in range(nt):
                v = struct.unpack_from(self.e + '6f', data, at + t_at + 24 * k)
                translations.append((v[:3], v[3:]))
            rotations = []
            for k in range(nr):
                v = struct.unpack_from(self.e + '9f', data, at + r_at + 36 * k)
                rotations.append((v[:4], v[4:8], v[8]))
            self.records[bone] = RawTrack(value, translations, rotations)
            at += r_at + 36 * nr
        self.end = at

    def samples(self, bone):
        """(times, translations, times, rotations) of a node at its segment ends (times 0..1); long rotation
        segments are split so that a shortest-path slerp between the samples follows the game's arc."""
        track = self.records[bone]
        n = len(track.translations)
        t_times = [k / n for k in range(n + 1)]
        t_values = [p for p, _ in track.translations]
        p, d = track.translations[-1]
        t_values.append(tuple(p[i] + d[i] for i in range(3)))
        n = len(track.rotations)
        r_times, r_values = [], []
        for k, key in enumerate(track.rotations):
            parts = max(1, math.ceil(abs(key[2]) / (math.pi / 4)))
            for j in range(parts):
                r_times.append((k + j / parts) / n)
                r_values.append(raw_rotation(key, j / parts))
        r_times.append(1.0)
        r_values.append(raw_rotation(track.rotations[-1], 1.0))
        return t_times, t_values, r_times, r_values

    def moving(self):
        for bone in self.records:
            _, tv, _, rv = self.samples(bone)
            if any(max(abs(a - b) for a, b in zip(v, tv[0])) > 1e-5 for v in tv) or \
                    any(max(abs(a - b) for a, b in zip(v, rv[0])) > 1e-5 for v in rv):
                return True
        return False


def raw_rotation(key, f):
    """Rotation segment (q0, q1, angle) at fraction f, as 0x167DE0 does it."""
    q0, q1, angle = key
    if f < f32(0x3A83126F):
        return tuple(q0)
    if f > f32(0x3F7FBE77):
        f = 1.0
    if angle == 0.0:
        q = [q0[i] + q1[i] * f for i in range(4)]
    else:
        c, s_ = math.cos(angle * f), math.sin(angle * f)
        q = [q0[i] * c + q1[i] * s_ for i in range(4)]
    norm = math.sqrt(sum(c * c for c in q))
    return tuple(c / norm for c in q)


class Cloth:
    """Cloth settings (type 25): parts of particles held by skeleton bones, springs and triangles."""

    def __init__(self, data, endian='<'):
        self.e = endian
        self.particle_size = 64 if endian == '<' else 72   # the Wii's has 8 bytes 0xEA after the normal
        (*self.sphere, self.size, count, parts, nbones, bones, self.zero, self.offset) = struct.unpack_from(
            self.e + '5f7I', data, 0x1C)
        self.bones = list(struct.unpack_from(f'{self.e}{nbones}I', data, bones))
        self.parts = []
        at = bones + 4 * nbones
        if parts != at:
            raise ValueError(f'parts at {parts:#x}, expected {at:#x}')
        at = parts + 0x8C * count
        for k in range(count):
            r = parts + 0x8C * k
            settings = struct.unpack_from(self.e + '19fI4B4BfI', data, r)
            if settings[-1]:
                raise ValueError(f'part {k}: word at +0x5C is {settings[-1]:#x}')
            (n_pins, pins_at, n_springs, springs_at, n_bends, bends_at, n_tris, tris_at, n_particles, n2,
             particles_at) = struct.unpack_from(self.e + '11I', data, r + 0x60)
            if n2 != n_particles:
                raise ValueError(f'part {k}: particle counts {n_particles} / {n2}')
            for name, offset, size in (('pins', pins_at, 4 * n_pins), ('springs', springs_at, 24 * n_springs),
                                       ('bends', bends_at, 32 * n_bends), ('triangles', tris_at, 12 * n_tris),
                                       ('particles', particles_at, self.particle_size * n_particles)):
                if offset != at:
                    raise ValueError(f'part {k}: {name} at {offset:#x}, expected {at:#x}')
                at += size
            particles = []
            for i in range(n_particles):
                q = particles_at + self.particle_size * i
                v = struct.unpack_from(self.e + ('11f' if endian == '<' else '6f8x5f') + '4HBBH2I', data, q)
                bones_ = [b for b in v[11:15] if b != 0xFFFF]
                if len(bones_) != v[15] or v[16] != 0xEA or v[17] > 1 or v[18:20] != (0xFFFFFFFF, 0xFFFFFFFF):
                    raise ValueError(f'part {k} particle {i}: unexpected fields {v[11:]}')
                particles.append(Particle(v[0:3], v[3:6], v[6], v[7:7 + len(bones_)], bones_, bool(v[17])))
            self.parts.append(ClothPart(
                settings, list(struct.unpack_from(f'{self.e}{n_pins}I', data, pins_at)),
                [struct.unpack_from(self.e + 'IIffff', data, springs_at + 24 * i) for i in range(n_springs)],
                [struct.unpack_from(self.e + '4I4f', data, bends_at + 32 * i) for i in range(n_bends)],
                [struct.unpack_from(self.e + '3I', data, tris_at + 12 * i) for i in range(n_tris)], particles))
        self.end = at

    def skinning(self):
        """{position: (bones, weights)} of every particle (a cloth mesh vertex is found by its position)."""
        return {p.position: (p.bones, p.weights) for part in self.parts for p in part.particles}


def bundle_items(path):
    with open(path, 'rb') as f:
        b = bdp.Bundle(f.read())
    return b, list(b.items(0))


def verify(voldir):
    counts = collections.Counter()
    worst_bind = worst_raw = worst_norm = worst_join = 0.0
    problems = []
    for f in sorted(os.listdir(voldir)):
        if not f.lower().endswith(bdp.EXTENSIONS):
            continue
        b, items = bundle_items(os.path.join(voldir, f))
        e_ = b.endian
        skeletons = {}
        for e, a, _ in items:
            if e.type != 3:
                continue
            try:
                s = Skeleton(a, b.names, e_)
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
            if e.type != 25:
                continue
            try:
                c = Cloth(a, e_)
                if c.end != len(a) or c.size != len(a):
                    raise ValueError(f'parts end at {c.end:#x}, entry is {len(a):#x}')
                for part in c.parts:
                    for p in part.particles:
                        if p.bones and abs(sum(p.weights) - 1) > 1e-3 or not set(p.bones) <= set(c.bones):
                            raise ValueError(f'particle weights {p.weights} / bones {p.bones} against {c.bones}')
                counts['cloths'] += 1
                counts['cloth particles'] += sum(len(part.particles) for part in c.parts)
            except (ValueError, struct.error) as err:
                problems.append(f'{f} {b.name(e.name)!r}: {err}')
        for e, a, _ in items:
            if e.type != 4:
                continue
            try:
                kind = struct.unpack_from(e_ + 'I', a, 0x20)[0]
                anim = Animation(a, b.names, e_) if kind == KEY_INTERP else RawAnim(a, b.names, e_)
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
                    for i, track in anim.records.items():
                        if s.poses:  # the bind pose a second time: compare
                            t, q = track.translations[0][0], track.rotations[0][0]
                            p = s.poses[i]
                            worst_raw = max(worst_raw, max(abs(u - v) for u, v in zip(t, p.translation)),
                                            min(max(abs(u - v) for u, v in zip(q, p.rotation)),
                                                max(abs(u + v) for u, v in zip(q, p.rotation))))
                        for (p, d), (p2, _) in zip(track.translations, track.translations[1:]):
                            worst_join = max(worst_join, max(abs(p[k] + d[k] - p2[k]) for k in range(3)))
                        for key, key2 in zip(track.rotations, track.rotations[1:]):
                            q, q2 = raw_rotation(key, 1.0), key2[0]
                            worst_join = max(worst_join, min(max(abs(u - v) for u, v in zip(q, q2)),
                                                             max(abs(u + v) for u, v in zip(q, q2))))
                    counts['raw animations'] += 1
                    counts['raw segments'] += sum(len(t.translations) + len(t.rotations)
                                                  for t in anim.records.values())
            except (ValueError, struct.error) as err:
                problems.append(f'{f} {b.name(e.name)!r}: {err}')
    for p in problems[:20]:
        print(p)
    print(', '.join(f'{v} {k}' for k, v in counts.items()) + f'; {len(problems)} problems')
    print(f'bind pose chain vs stored inverse bind: max error {worst_bind:.2e}; RawAnim vs skeleton bind pose: '
          f'{worst_raw:.2e}; rotation key |q| - 1: {worst_norm:.2e}; RawAnim segment joins: {worst_join:.2e}')
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


def add_animation(g, name, duration, channels):
    """channels: [(node, 'translation' or 'rotation', times 0..1, values in glTF convention)]"""
    samplers, out = [], []
    for node, path, times, values in channels:
        samplers.append({'input': scalar_accessor(g, [t * duration for t in times]),
                         'output': g.accessor([list(v) for v in values],
                                              'VEC3' if path == 'translation' else 'VEC4', target=None),
                         'interpolation': 'LINEAR'})
        out.append({'sampler': len(samplers) - 1, 'target': {'node': node, 'path': path}})
    g.doc.setdefault('animations', []).append({'name': name, 'samplers': samplers, 'channels': out})


def keyframe_channels(anim, joints):
    return ([(joints[t.bone], 'translation', t.times, t.values) for t in anim.translations] +
            [(joints[t.bone], 'rotation', t.times, [gltf_rotation(q) for q in t.values]) for t in anim.rotations])


def raw_channels(raw, joints):
    out = []
    for bone in sorted(raw.records):
        t_times, t_values, r_times, r_values = raw.samples(bone)
        out.append((joints[bone], 'translation', t_times, t_values))
        out.append((joints[bone], 'rotation', r_times, [gltf_rotation(q) for q in r_values]))
    return out


class Models:
    """The node-tree models of a bundle: hierarchy, rest pose (the RawAnim at time 0) and clips. Every tree
    has a model of the same name whose mesh objects ("MODEL|object") are named after its nodes; their vertices
    are in the node's space."""

    def __init__(self, bundle):
        self.b = bundle
        self.trees, self.raws, self.clips = {}, {}, collections.defaultdict(list)
        self.clip_by_name = {}
        items = list(bundle.items(0))
        for e, a, _ in items:
            if e.type == 3:
                self.trees[self.key(bundle.name(e.name))] = Skeleton(a, bundle.names, bundle.endian)
        for e, a, _ in items:
            if e.type != 4:
                continue
            if struct.unpack_from(bundle.endian + 'I', a, 0x20)[0] == RAW:
                raw = RawAnim(a, bundle.names, bundle.endian)
                self.raws[self.key(raw.skeleton_name)] = raw
            else:
                clip = Animation(a, bundle.names, bundle.endian)
                self.clips[self.key(clip.skeleton_name)].append((anim_name(bundle.name(e.name)), clip))
                self.clip_by_name[anim_name(bundle.name(e.name)).lower()] = clip

    @staticmethod
    def key(name):
        name = name.lstrip('\x13').lower()
        return name[:-4] if name.endswith('.pwk') else name

    def add(self, g, assets, name, parent, translation=None, rotation_z=None):
        """Place model `name` under `parent`: (root node, node per tree node), or None if it has no meshes."""
        key = self.key(name)
        entries = assets.models.get(key, [])
        if not entries:
            return None
        root = g.node(name, None, translation, rotation_z, parent)
        joints = []
        tree = self.trees.get(key)
        if tree:
            raw = self.raws.get(key)
            for i, node in enumerate(tree.nodes):
                j = g.node(node.name, parent=root if node.parent < 0 else joints[node.parent])
                if raw and i in raw.records:
                    t, q = raw.records[i].translations[0][0], raw.records[i].rotations[0][0]
                elif tree.poses:
                    t, q = tree.poses[i].translation, tree.poses[i].rotation
                else:
                    t, q = (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)
                if any(t):
                    g.doc['nodes'][j]['translation'] = list(t)
                if tuple(q) != (0.0, 0.0, 0.0, 1.0):
                    g.doc['nodes'][j]['rotation'] = gltf_rotation(q)
                joints.append(j)
        by_node = {tree.nodes[i].name.lower(): j for i, j in enumerate(joints)} if tree else {}
        for entry in entries:
            m = assets.mesh(entry)
            if m is not None:
                obj = assets.clean(entry[0].name).partition('|')[2].lower()
                g.node(g.doc['meshes'][m]['name'], m, parent=by_node.get(obj, root))
        return root, joints

    def loop(self, g, name, joints):
        """Add the model's own RawAnim as a clip "<name> loop" if it moves; True if it did."""
        raw = self.raws.get(self.key(name))
        if not raw or not joints or not raw.moving():
            return False
        add_animation(g, f'{self.key(name)} loop', raw.duration, raw_channels(raw, joints))
        return True


def export_models(voldir, bundle_name, out):
    """Every node-tree model of a bundle that has more than one node, clips or a moving RawAnim."""
    files = {f.lower(): f for f in os.listdir(voldir)}
    path = os.path.join(voldir, files.get(bundle_name.lower()) or files.get(bundle_name.lower() + '.bdp') or
                        files[bundle_name.lower() + '.bdg'])
    b, _ = bundle_items(path)
    stem = os.path.splitext(os.path.basename(path))[0]
    models = Models(b)
    outdir = os.path.join(out, stem, 'models')
    written = []
    for key, tree in sorted(models.trees.items()):
        raw = models.raws.get(key)
        if len(tree.nodes) < 2 and not models.clips.get(key) and not (raw and raw.moving()):
            continue
        g = mesh.Gltf()
        assets = mesh.BundleAssets(b, g, outdir, '../textures')
        placed = models.add(g, assets, key, 0)
        if placed is None:
            continue
        _, joints = placed
        if raw and raw.moving():
            add_animation(g, key, raw.duration, raw_channels(raw, joints))
        for name, clip in models.clips.get(key, []):
            add_animation(g, name, clip.duration, keyframe_channels(clip, joints))
        os.makedirs(outdir, exist_ok=True)
        file = bdp.safe_name(key)
        g.save(os.path.join(outdir, file + '.gltf'))
        written.append((file, len(g.doc.get('animations', []))))
    return written


def export_monster(voldir, monster, out):
    files = {f.lower(): f for f in os.listdir(voldir)}
    file = files.get(monster.lower() + '.cmp') or files[monster.lower() + '.cmg']
    b, items = bundle_items(os.path.join(voldir, file))
    g = mesh.Gltf()
    assets = mesh.BundleAssets(b, g, out, monster + '_textures')
    skel = next(Skeleton(a, b.names, b.endian) for e, a, _ in items if e.type == 3 and e.extra == 1)
    joints = []
    for i, node in enumerate(skel.nodes):
        j = g.node(node.name, parent=0 if node.parent < 0 else joints[node.parent])
        g.doc['nodes'][j]['translation'] = list(skel.poses[i].translation)
        g.doc['nodes'][j]['rotation'] = gltf_rotation(skel.poses[i].rotation)
        joints.append(j)
    ibm = g.accessor([column_major(skel.inverse_bind(i)) for i in range(len(joints))], 'MAT4', target=None)
    g.doc['skins'] = [{'joints': joints, 'inverseBindMatrices': ibm, 'skeleton': joints[0], 'name': 'skeleton'}]
    world = skel.world()
    cloth_skin = {}
    for e, a, _ in items:
        if e.type == 25:
            cloth_skin.update(Cloth(a, b.endian).skinning())
    for entry in items:
        e = entry[0]
        if e.type == 17:
            m = assets.mesh(entry, skinned=True)
            if m is not None:
                n = g.node(assets.clean(e.name), m)
                g.doc['nodes'][n]['skin'] = 0
        elif e.type == 0:
            # cloth (hair, whiskers, ...): the game simulates it (type 25) from particles held by bones; here each
            # vertex is skinned to its particle's bones and weights (the bone nearest to it if it has none)
            prims = []
            for part in mesh.read_mesh(0, entry[1], entry[2], b.endian):
                for s in part.sections:
                    bones, weights = [], []
                    for p in s.positions:
                        bw = cloth_skin.get(p)
                        if bw:
                            bones.append(tuple(bw[0]))
                            weights.append(tuple(bw[1]))
                        else:
                            bones.append((nearest_bone(skel, world, p),))
                            weights.append((1.0,))
                    prims.append((assets.material(part.material), s._replace(bones=bones, weights=weights)))
            m = g.mesh(assets.clean(e.name), prims, skinned=True)
            if m is not None:
                n = g.node(assets.clean(e.name), m)
                g.doc['nodes'][n]['skin'] = 0
    anims_file = files.get(monster.lower() + '_anims.cmp') or files.get(monster.lower() + '_anims.cmg')
    count = 0
    if anims_file:
        ab, aitems = bundle_items(os.path.join(voldir, anims_file))
        for e, a, _ in aitems:
            if e.type != 4 or struct.unpack_from(ab.endian + 'I', a, 0x20)[0] != KEY_INTERP:
                continue
            anim = Animation(a, ab.names, ab.endian)
            if anim.bones != len(joints):
                continue
            add_animation(g, anim_name(ab.name(e.name)), anim.duration, keyframe_channels(anim, joints))
            count += 1
        g.doc.get('animations', []).sort(key=lambda x: x['name'])
    os.makedirs(out, exist_ok=True)
    g.save(os.path.join(out, monster + '.gltf'))
    return count


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == '--verify':
        sys.exit(0 if verify(args[1]) else 1)
    elif len(args) == 4 and args[1] == '--models':
        for file, clips in export_models(args[0], args[2], args[3]):
            print(f'{file}: {clips} clips')
    elif len(args) == 3:
        voldir, monster, out = args
        if monster == '--all':
            names = sorted(f[:-len('_anims.cmp')] for f in os.listdir(voldir)
                           if f.lower().endswith(('_anims.cmp', '_anims.cmg')))
        else:
            names = [monster]
        for name in names:
            print(f'{name}: {export_monster(voldir, name, out)} animations')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
