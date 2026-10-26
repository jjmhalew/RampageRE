"""Read the meshes in Rampage bundles (entry type 0 = mesh, 17 = skinned mesh) and export them as glTF.

Mesh entry data (little endian, see docs/FORMAT_BDP.md):
  type 0:  u32 part count, 7 x f32 bounds, then a 0x50-byte record per part
  type 17: u16 part count, u16 bone count, u32[bone count] bone name indices, 3 x u32, then a 0x58-byte record per
           part
  record:  u32 strip length - 2 (vertex count - 2), u32 ?, ... u16 material name index, and offsets (from the record) of the part's section
           descriptor: type 0 at +0x40 (material at +0x0A), type 17 at +0x44 (material at +0x38)
  section descriptor: u32 vertex count, u32 ?, u32 vertex format, u32 0, u32 offset in data2, u32 size
The vertices are in data2 as separate streams, each starting on a 16-byte boundary, in this order:
  position        4 x f32: x, y, z, w (always present) Rigid: w is 1.0 = this vertex closes a triangle, 0.0 = it starts a strip.
                  Skinned: w is u16 weight (4.12, of the first bone), u8 bone 0 x 4, u8 bone 1 x 4; the bones are
                  node indices of the skeleton (tools/anim.py) and the positions are in the bind pose's model space.
  0x010 normal    3 x s8, u8 (skinned: 0x7F = closes a triangle, 0 = starts a strip)
  0x040 colour    4 x u8 RGBA, 0x80 = 1.0
  0x100 uv        2 x s16, 4.12 fixed point
  (0x008 = skinned vertices; 0x002 is set on almost every mesh, meaning unknown)
Triangles are strips: every vertex flagged "closes" makes a triangle with the two before it, alternating winding.
The Wii's meshes (.bdg / .cmg) have the same records, big endian, pointing at a GX display list and interleaved
vertices instead: see read_wii_section and docs/FORMAT_BDP.md.

Usage: python tools/mesh.py --verify VOLDIR                check every mesh's triangle count
       python tools/mesh.py BUNDLE OUTDIR                   write OUTDIR/<bundle>.gltf (+ .bin and textures)
"""
import base64
import collections
import json
import math
import os
import struct
import sys

import bdp
import tex

Section = collections.namedtuple('Section', 'count format positions normals colours uvs closes weights bones tris',
                                 defaults=(None,))     # tris: the Wii's triangles (the PS2's come from the strip flags)
Part = collections.namedtuple('Part', 'material triangles_expected sections')

NORMAL, COLOUR, UV, SKINNED = 0x010, 0x040, 0x100, 0x008
FOUR_BONES, UV4, NBT = 0x004, 0x200, 0x10000          # Wii only


def align16(n):
    return (n + 15) & ~15


def read_section(data, at, data2, skinned):
    count, _, fmt, zero, offset, size = struct.unpack_from('<6I', data, at)
    if zero or offset + size > len(data2):
        raise ValueError(f'bad section descriptor at {at:#x}')
    need = align16(16 * count) + sum(align16(4 * count) for bit in (NORMAL, COLOUR, UV) if fmt & bit)
    if need > size:
        raise ValueError(f'format {fmt:#x} with {count} vertices needs {need} bytes, section has {size}')
    p = offset
    positions, closes, weights, bones = [], [], [], []
    for i in range(count):
        x, y, z = struct.unpack_from('<3f', data2, p + 16 * i)
        if skinned:
            w, b0, b1 = struct.unpack_from('<HBB', data2, p + 16 * i + 12)
            weights.append(w / 4096)
            bones.append((b0 // 4, b1 // 4))
        else:
            closes.append(struct.unpack_from('<f', data2, p + 16 * i + 12)[0] != 0.0)
        positions.append((x, y, z))
    p += align16(16 * count)
    normals = colours = uvs = None
    if fmt & NORMAL:
        normals = []
        for i in range(count):
            nx, ny, nz, flag = struct.unpack_from('<bbbB', data2, p + 4 * i)
            normals.append((nx / 127, ny / 127, nz / 127))
            if skinned:
                closes.append(flag != 0)
        p += align16(4 * count)
    if fmt & COLOUR:
        colours = [tuple(min(1.0, c / 128) for c in data2[p + 4 * i:p + 4 * i + 4]) for i in range(count)]
        p += align16(4 * count)
    if fmt & UV:
        uvs = [tuple(c / 4096 for c in struct.unpack_from('<hh', data2, p + 4 * i)) for i in range(count)]
        p += align16(4 * count)
    if p > offset + size:
        raise ValueError(f'streams of the section at {at:#x} (format {fmt:#x}, {count} vertices) run past its size')
    if skinned and not normals:
        raise ValueError('skinned section without normals has no strip flags')
    return Section(count, fmt, positions, normals, colours, uvs, closes, weights or None, bones or None)


def wii_layout(skinned, fmt):
    """[(attribute, bytes)] of one Wii vertex, in order."""
    out = [('position', 12)]
    if skinned:
        out += [('weights', 12 if fmt & FOUR_BONES else 4), ('bones', 8 if fmt & FOUR_BONES else 4)]
    if fmt & NBT:                                    # bump mapped: uv first, then tangent (along u), binormal, normal
        return out + [('uv', 8), ('tangent', 12), ('binormal', 12), ('normal', 12)]
    if fmt & NORMAL:
        out.append(('normal', 12))
    if fmt & COLOUR:
        out.append(('colour', 4))
    if fmt & UV:
        out.append(('uv', 8))
    if fmt & UV4:
        out.append(('uv4', 16))
    return out


def wii_triangles(data2, at, size, attrs, wide, count):
    """Triangles of a GX display list: per primitive u8 command, u16 vertex count, then per vertex one index per
    attribute (u8, or u16 when `wide`); the attributes are interleaved, so every index of a vertex is the same."""
    out = []
    p, end = at, at + size
    code = '>' + ('H' if wide else 'B') * attrs
    while p < end and data2[p]:
        op, n = data2[p], struct.unpack_from('>H', data2, p + 1)[0]
        p += 3
        idx = []
        for _ in range(n):
            v = struct.unpack_from(code, data2, p)
            if len(set(v)) != 1 or v[0] >= count:
                raise ValueError(f'display list vertex {v} at {p:#x}')
            idx.append(v[0])
            p += struct.calcsize(code)
        if op == 0x80:                               # quads
            for k in range(0, n - 3, 4):
                out += [(idx[k], idx[k + 1], idx[k + 2]), (idx[k], idx[k + 2], idx[k + 3])]
        elif op == 0x90:                             # triangles
            out += [tuple(idx[k:k + 3]) for k in range(0, n - 2, 3)]
        elif op == 0x98:                             # strip
            out += [(idx[k], idx[k + 1], idx[k + 2]) if k % 2 == 0 else (idx[k + 1], idx[k], idx[k + 2])
                    for k in range(n - 2)]
        elif op == 0xA0:                             # fan
            out += [(idx[0], idx[k], idx[k + 1]) for k in range(1, n - 1)]
        else:
            raise ValueError(f'display list command {op:#x} at {p - 3:#x}')
    if p > end or any(data2[p:end]):
        raise ValueError(f'display list at {at:#x} does not end at {end:#x}')
    return out


def read_wii_section(data, block, desc, data2, skinned):
    """A Wii part: the display list block (u32 vertices, u32 1 = u8 indices, u32 offset in data2, u32 size, u32
    indices per vertex) and the vertex descriptor (u32 count, u32 stride, u32 format, u32 0, u32 offset, u32 size)."""
    dl_count, narrow, dl_at, dl_size, attrs = struct.unpack_from('>5I', data, block)
    count, stride, fmt, zero, offset, size = struct.unpack_from('>6I', data, desc)
    layout = wii_layout(skinned, fmt)
    if zero or size != count * stride or sum(n for _, n in layout) != stride or offset + size > len(data2):
        raise ValueError(f'vertex descriptor at {desc:#x} (format {fmt:#x}, stride {stride})')
    tris = wii_triangles(data2, dl_at, dl_size, attrs, not narrow, count)
    streams = collections.defaultdict(list)
    for i in range(count):
        o = offset + stride * i
        for name, n in layout:
            if name == 'colour':
                streams[name].append(tuple(c / 255 for c in data2[o:o + 4]))
            elif name == 'bones':
                streams[name].append(struct.unpack_from(f'>{n // 2}H', data2, o))
            else:
                streams[name].append(struct.unpack_from(f'>{n // 4}f', data2, o))
            o += n
    weights = [w + (1.0 - sum(w),) for w in streams['weights']] or None   # the last bone's weight is the rest
    uvs = streams['uv'] or [u[:2] for u in streams['uv4']] or None
    return Section(count, fmt, streams['position'], streams['normal'] or None, streams['colour'] or None, uvs, None,
                   weights, streams['bones'] or None, tris)


def read_wii_mesh(etype, data, data2):
    """[Part] of a Wii type 0 or 17 entry: the PS2's records, big endian, pointing at display lists."""
    if etype == 0:
        count = struct.unpack_from('>I', data, 0)[0]
        records = [(0x20 + 0x50 * i, 0x0A, 0x3C, 0x40) for i in range(count)]
    elif etype == 17:
        count, nbones = struct.unpack_from('>HH', data, 0)
        first = 4 + 4 * nbones + 12
        records = [(first + 0x58 * i, 0x38, 0x40, 0x44) for i in range(count)]
    else:
        raise ValueError(f'entry type {etype} is not a mesh')
    parts = []
    for rec, mat, block, desc in records:
        material = struct.unpack_from('>H', data, rec + mat)[0]
        block, desc = (rec + struct.unpack_from('>I', data, rec + o)[0] for o in (block, desc))
        parts.append(Part(material, None, [read_wii_section(data, block, desc, data2, etype == 17)]))
    return parts


def read_mesh(etype, data, data2, endian='<'):
    """[Part] of a type 0 or type 17 entry."""
    if endian == '>':
        return read_wii_mesh(etype, data, data2)
    parts = []
    if etype == 0:
        count = struct.unpack_from('<I', data, 0)[0]
        for i in range(count):
            rec = 0x20 + 0x50 * i
            tris = struct.unpack_from('<I', data, rec)[0]
            material = struct.unpack_from('<H', data, rec + 0x0A)[0]
            desc = rec + struct.unpack_from('<I', data, rec + 0x40)[0]
            parts.append(Part(material, tris, [read_section(data, desc, data2, False)]))
    elif etype == 17:
        count, nbones = struct.unpack_from('<HH', data, 0)
        first = 4 + 4 * nbones + 12
        for i in range(count):
            rec = first + 0x58 * i
            tris = struct.unpack_from('<I', data, rec)[0]
            material = struct.unpack_from('<H', data, rec + 0x38)[0]
            desc = rec + struct.unpack_from('<I', data, rec + 0x44)[0]
            parts.append(Part(material, tris, [read_section(data, desc, data2, True)]))
    else:
        raise ValueError(f'entry type {etype} is not a mesh')
    return parts


def triangles(section):
    """Triangle list from the strip flags, winding alternating within a strip."""
    if section.tris is not None:
        return section.tris
    out = []
    run = 0
    for i, closes in enumerate(section.closes):
        if not closes:
            run = 0
            continue
        run += 1
        if i >= 2:
            a, b, c = i - 2, i - 1, i
            out.append((a, b, c) if run % 2 else (b, a, c))
    return out


# ---------------------------------------------------------------- glTF

class Gltf:
    """glTF 2.0 writer. The game is Z-up; everything hangs under a root node that turns it Y-up."""

    def __init__(self):
        self.doc = {'asset': {'version': '2.0', 'generator': 'RampageRE tools'}, 'scene': 0,
                    'scenes': [{'nodes': [0]}],
                    'nodes': [{'name': 'Z up', 'rotation': [-0.70710678, 0.0, 0.0, 0.70710678]}],
                    'meshes': [], 'materials': [], 'textures': [],
                    'images': [], 'samplers': [{'wrapS': 10497, 'wrapT': 10497}], 'accessors': [],
                    'bufferViews': [], 'buffers': []}
        self.bin = bytearray()
        self.material_index = {}

    def blob(self, raw, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        view = {'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': len(raw)}
        if target:
            view['target'] = target
        self.bin += raw
        self.doc['bufferViews'].append(view)
        return len(self.doc['bufferViews']) - 1

    def accessor(self, values, kind, ctype=5126, target=34962, minmax=False):
        width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[kind]
        code = {5126: 'f', 5125: 'I', 5123: 'H', 5121: 'B'}[ctype]
        flat = [c for v in values for c in (v if width > 1 else (v,))]
        acc = {'bufferView': self.blob(struct.pack(f'<{len(flat)}{code}', *flat), target), 'componentType': ctype,
               'count': len(values), 'type': kind}
        if minmax:
            acc['min'] = [min(v[k] for v in values) for k in range(width)]
            acc['max'] = [max(v[k] for v in values) for k in range(width)]
        self.doc['accessors'].append(acc)
        return len(self.doc['accessors']) - 1

    def material(self, key, name, png):
        if key in self.material_index:
            return self.material_index[key]
        mat = {'name': name, 'doubleSided': True, 'pbrMetallicRoughness': {'metallicFactor': 0.0,
                                                                            'roughnessFactor': 1.0}}
        if png is not None:
            self.doc['images'].append({'uri': png})
            self.doc['textures'].append({'source': len(self.doc['images']) - 1, 'sampler': 0})
            mat['pbrMetallicRoughness']['baseColorTexture'] = {'index': len(self.doc['textures']) - 1}
            mat['alphaMode'] = 'MASK'
        self.doc['materials'].append(mat)
        self.material_index[key] = len(self.doc['materials']) - 1
        return self.material_index[key]

    def mesh(self, name, prims, skinned=False):
        """prims: [(material index, section)]. Returns the mesh index, or None if nothing is drawn.
        skinned: write the vertex bones (skeleton node indices) and weights as JOINTS_0 / WEIGHTS_0."""
        out = []
        for mat, s in prims:
            tris = triangles(s)
            if not tris:
                continue
            attrs = {'POSITION': self.accessor(s.positions, 'VEC3', minmax=True)}
            if s.normals:
                attrs['NORMAL'] = self.accessor(s.normals, 'VEC3')
            if s.uvs:
                attrs['TEXCOORD_0'] = self.accessor(s.uvs, 'VEC2')
            if s.colours:
                attrs['COLOR_0'] = self.accessor(s.colours, 'VEC4')
            if skinned and s.bones:
                if isinstance(s.weights[0], tuple):  # up to 4 bones per vertex, weights given per bone
                    joints = [tuple(b) + (0,) * (4 - len(b)) for b in s.bones]
                    weights = [tuple(w) + (0.0,) * (4 - len(w)) for w in s.weights]
                else:  # skinned mesh: 2 bones, the weight of the first
                    joints = [(b0, b1, 0, 0) for b0, b1 in s.bones]
                    weights = [(min(w, 1.0), 1.0 - min(w, 1.0), 0.0, 0.0) for w in s.weights]
                attrs['JOINTS_0'] = self.accessor(joints, 'VEC4', 5121)
                attrs['WEIGHTS_0'] = self.accessor(weights, 'VEC4')
            idx = self.accessor([i for t in tris for i in t], 'SCALAR', 5125, 34963)
            out.append({'attributes': attrs, 'indices': idx, 'material': mat})
        if not out:
            return None
        self.doc['meshes'].append({'name': name, 'primitives': out})
        return len(self.doc['meshes']) - 1

    def node(self, name, mesh=None, translation=None, rotation_z=None, parent=0):
        """A node under `parent` (the root by default); rotation_z in degrees about the game's Z axis."""
        n = {'name': name}
        if mesh is not None:
            n['mesh'] = mesh
        if translation and any(translation):
            n['translation'] = [float(c) for c in translation]
        if rotation_z:
            half = math.radians(rotation_z) / 2
            n['rotation'] = [0.0, 0.0, math.sin(half), math.cos(half)]
        self.doc['nodes'].append(n)
        index = len(self.doc['nodes']) - 1
        self.doc['nodes'][parent].setdefault('children', []).append(index)
        return index

    def save(self, path):
        base = os.path.splitext(os.path.basename(path))[0]
        self.doc['buffers'] = [{'uri': base + '.bin', 'byteLength': len(self.bin)}]
        for k in ('materials', 'textures', 'images'):
            if not self.doc[k]:
                del self.doc[k]
        with open(os.path.splitext(path)[0] + '.bin', 'wb') as f:
            f.write(self.bin)
        with open(path, 'w') as f:
            if len(self.doc['accessors']) < 5000:
                json.dump(self.doc, f, indent=1)
            else:  # animated models: tens of thousands of accessors
                json.dump(self.doc, f, separators=(',', ':'))


class BundleAssets:
    """Materials, textures and models of one bundle, written into a Gltf on first use."""

    def __init__(self, bundle, gltf, out, texdir):
        self.b = bundle
        self.g = gltf
        self.items = list(bundle.items(0))
        # by (name, type): a name can be shared (Chicago's Citybus is a texture, a node tree and its animation)
        self.by_name = {(e.name, e.type): (e, a, d2) for e, a, d2 in self.items}
        self.palettes = {e.name: (e, a, d2) for e, a, d2 in self.items if e.type == 13}
        self.palettes.update({i: n for i, n in enumerate(bundle.names) if n in tex.BUILTIN})
        self.out, self.texdir = out, texdir
        self.models = collections.defaultdict(list)  # lower-case model name -> mesh entries ("MODEL|object")
        for e, a, d2 in self.items:
            if e.type in (0, 17):
                self.models[self.clean(e.name).split('|')[0].lower()].append((e, a, d2))
        self.model_meshes = {}
        self.meshes = {}
        self.textures = {}

    def clean(self, name_index):
        return self.b.name(name_index).lstrip('\x13')

    def texture(self, name_index):
        """Relative PNG path of a texture entry (written once), or None."""
        if name_index not in self.textures:
            png = None
            entry = self.by_name.get((name_index, 9))
            if entry:
                os.makedirs(os.path.join(self.out, self.texdir), exist_ok=True)
                file = bdp.safe_name(self.b.name(name_index)) + '.png'
                path = os.path.join(self.out, self.texdir, file)
                if not os.path.exists(path):
                    if self.b.endian == '>':
                        tex.wii_image(entry[1], entry[2]).save(path)
                    else:
                        tex.image(entry[1], entry[2], self.palettes).save(path)
                png = f'{self.texdir}/{file}'
            self.textures[name_index] = png
        return self.textures[name_index]

    def material(self, name_index):
        """A part names a material (type 6, 3,768 parts) or directly a texture (type 9, 2,524 parts)."""
        png = None
        mat = self.by_name.get((name_index, 6))
        if mat:
            png = self.texture(struct.unpack_from(self.b.endian + 'H', mat[1], 0x38)[0])
        elif (name_index, 9) in self.by_name:
            png = self.texture(name_index)
        return self.g.material(name_index, self.clean(name_index), png)

    def mesh(self, entry, skinned=False):
        """glTF mesh index of a mesh entry (written once), or None if it draws nothing."""
        e, a, d2 = entry
        if (e.index, skinned) not in self.meshes:
            parts = read_mesh(e.type, a, d2, self.b.endian)
            self.meshes[e.index, skinned] = self.g.mesh(
                self.clean(e.name), [(self.material(p.material), s) for p in parts for s in p.sections], skinned)
        return self.meshes[e.index, skinned]

    def model(self, name):
        """glTF mesh indices of a model (all its "MODEL|object" meshes), by name with or without .pwk."""
        key = name.lower()
        if key.endswith('.pwk'):
            key = key[:-4]
        if key not in self.model_meshes:
            self.model_meshes[key] = [m for m in (self.mesh(x) for x in self.models.get(key, [])) if m is not None]
        return self.model_meshes[key]


def export(bundle_path, out):
    with open(bundle_path, 'rb') as f:
        b = bdp.Bundle(f.read())
    stem = os.path.splitext(os.path.basename(bundle_path))[0]
    g = Gltf()
    assets = BundleAssets(b, g, out, stem + '_textures')
    count = 0
    for entry in assets.items:
        if entry[0].type in (0, 17):
            m = assets.mesh(entry)
            if m is not None:
                g.node(assets.clean(entry[0].name), m)
            count += 1
    os.makedirs(out, exist_ok=True)
    g.save(os.path.join(out, stem + '.gltf'))
    return count


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == '--verify':
        meshes = parts = bad = verts = tris = 0
        for f in sorted(os.listdir(args[1])):
            if not f.lower().endswith(bdp.EXTENSIONS):
                continue
            with open(os.path.join(args[1], f), 'rb') as h:
                b = bdp.Bundle(h.read())
            for e, a, d2 in b.items(0):
                if e.type not in (0, 17):
                    continue
                meshes += 1
                try:
                    for p in read_mesh(e.type, a, d2, b.endian):
                        parts += 1
                        n = sum(s.count for s in p.sections)
                        verts += n
                        tris += sum(len(triangles(s)) for s in p.sections)
                        if p.triangles_expected is not None and n - 2 != p.triangles_expected:
                            bad += 1
                            if bad <= 10:
                                print(f'{f} {b.name(e.name)!r}: {n} vertices, header says {p.triangles_expected} + 2')
                except ValueError as err:
                    bad += 1
                    if bad <= 10:
                        print(f'{f} {b.name(e.name)!r}: {err}')
        print(f'{meshes} meshes, {parts} parts, {verts} vertices, {tris} triangles drawn; {bad} parts disagree with their header')
    elif len(args) == 2:
        print(f'wrote {export(args[0], args[1])} meshes to {args[1]}')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
