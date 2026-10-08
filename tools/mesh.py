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
                  Skinned: w is u16 weight (4.12, of the first bone), u8 bone 0 x 4, u8 bone 1 x 4.
  0x010 normal    3 x s8, u8 (skinned: 0x7F = closes a triangle, 0 = starts a strip)
  0x040 colour    4 x u8 RGBA, 0x80 = 1.0
  0x100 uv        2 x s16, 4.12 fixed point
  (0x008 = skinned vertices; 0x002 is set on almost every mesh, meaning unknown)
Triangles are strips: every vertex flagged "closes" makes a triangle with the two before it, alternating winding.

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

Section = collections.namedtuple('Section', 'count format positions normals colours uvs closes weights bones')
Part = collections.namedtuple('Part', 'material triangles_expected sections')

NORMAL, COLOUR, UV, SKINNED = 0x010, 0x040, 0x100, 0x008


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


def read_mesh(etype, data, data2):
    """[Part] of a type 0 or type 17 entry."""
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
        width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[kind]
        code = {5126: 'f', 5125: 'I', 5123: 'H'}[ctype]
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

    def mesh(self, name, prims):
        """prims: [(material index, section)]. Returns the mesh index, or None if nothing is drawn."""
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
            json.dump(self.doc, f, indent=1)


class BundleAssets:
    """Materials, textures and models of one bundle, written into a Gltf on first use."""

    def __init__(self, bundle, gltf, out, texdir):
        self.b = bundle
        self.g = gltf
        self.items = list(bundle.items(0))
        self.by_name = {e.name: (e, a, d2) for e, a, d2 in self.items}
        self.palettes = {e.name: (e, a, d2) for e, a, d2 in self.items if e.type == 13}
        self.palettes.update({i: n for i, n in enumerate(bundle.names) if n in tex.BUILTIN})
        self.out, self.texdir = out, texdir
        self.models = collections.defaultdict(list)  # lower-case model name -> mesh entries ("MODEL|object")
        for e, a, d2 in self.items:
            if e.type in (0, 17):
                self.models[self.clean(e.name).split('|')[0].lower()].append((e, a, d2))
        self.model_meshes = {}
        self.textures = {}

    def clean(self, name_index):
        return self.b.name(name_index).lstrip('\x13')

    def texture(self, name_index):
        """Relative PNG path of a texture entry (written once), or None."""
        if name_index not in self.textures:
            png = None
            entry = self.by_name.get(name_index)
            if entry and entry[0].type == 9:
                os.makedirs(os.path.join(self.out, self.texdir), exist_ok=True)
                file = bdp.safe_name(self.b.name(name_index)) + '.png'
                path = os.path.join(self.out, self.texdir, file)
                if not os.path.exists(path):
                    tex.image(entry[1], entry[2], self.palettes).save(path)
                png = f'{self.texdir}/{file}'
            self.textures[name_index] = png
        return self.textures[name_index]

    def material(self, name_index):
        png = None
        mat = self.by_name.get(name_index)
        if mat and mat[0].type == 6:
            png = self.texture(struct.unpack_from('<H', mat[1], 0x38)[0])
        return self.g.material(name_index, self.clean(name_index), png)

    def mesh(self, entry):
        e, a, d2 = entry
        parts = read_mesh(e.type, a, d2)
        return self.g.mesh(self.clean(e.name), [(self.material(p.material), s) for p in parts for s in p.sections])

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
            if not f.lower().endswith(('.bdp', '.cmp')):
                continue
            with open(os.path.join(args[1], f), 'rb') as h:
                b = bdp.Bundle(h.read())
            for e, a, d2 in b.items(0):
                if e.type not in (0, 17):
                    continue
                meshes += 1
                try:
                    for p in read_mesh(e.type, a, d2):
                        parts += 1
                        n = sum(s.count for s in p.sections)
                        verts += n
                        tris += sum(len(triangles(s)) for s in p.sections)
                        if n - 2 != p.triangles_expected:
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
