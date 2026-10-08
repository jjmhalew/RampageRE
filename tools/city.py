"""Assemble Rampage city blocks from their property books into glTF scenes.

A city is played block by block. `<City>_Props.prb` (WorldData) lists the blocks; each block definition gives
its ground shape, the buildings (template, left edge X, front or back row) and the street objects (template,
location, rotation). Building templates are in `<City>_buildings.prb`: a ground-floor shape, a top shape, a
texture sheet and one `Floor|` per storey with a facade tile per cell. The meshes are in `<City>.bdp`.
See docs/FORMAT_BDP.md ("City layout").

Building geometry (game units; one cell is 5 x 5, Z up, the street is towards -Y):
  ground-floor shape at z 0 (6 high), then storeys 2..n are 3 high each, then the top shape.
  The upper storeys have no mesh of their own: the game builds the facade from the texture sheet, a 4 x 4
  grid of tiles (column = window style, row = damage state). A storey lists width + 2 * depth tile columns:
  left side (back to front), front (left to right), right side (front to back).

Usage: python tools/city.py VOLDIR CITY OUTDIR        e.g. tools/city.py build/vol Chicago build/gltf
       writes OUTDIR/CITY/<block>.gltf for every block, with the textures in OUTDIR/CITY/textures
"""
import collections
import os
import sys

import anim
import bdp
import mesh
import prb

CELL = 5.0
GROUND_FLOOR = 6.0
STOREY = 3.0
SHEET_TILE = 0.25


class Books:
    """Property books by name ("Chicago_Props" is the .prb in Chicago_Props.bdp), with inheritance."""

    def __init__(self, voldir):
        self.voldir = voldir
        self.files = {f.lower(): f for f in os.listdir(voldir)}
        self.cache = {}

    def book(self, name):
        key = name.lower()
        if key not in self.cache:
            f = self.files.get(key + '.bdp')
            if f is None:
                self.cache[key] = None
            else:
                with open(os.path.join(self.voldir, f), 'rb') as h:
                    b = bdp.Bundle(h.read())
                entry = next(((e, a) for e, a, _ in b.items(0) if e.type == 22), None)
                self.cache[key] = prb.Book(entry[1], b.names) if entry else None
        return self.cache[key]

    def group(self, book_name, group_name):
        book = self.book(book_name)
        if book is None:
            return None
        for at in book.groups:
            if book.group(at)[0].lower() == group_name.lower():
                return Group(self, book, at)
        return None


class Group:
    """A property group; get() falls back to the ParentPropertySet ("Group:Book") chain."""

    def __init__(self, books, book, at):
        self.books, self.book, self.at = books, book, at
        self.name, self.records = book.group(at)

    def own(self, key):
        return [(kind, value) for kind, name, value in self.records if name == key]

    def parent(self):
        ref = next((v for k, v in self.own('ParentPropertySet')), None)
        if ref is None or ':' not in self.book.name(ref):
            return None
        group_name, book_name = self.book.name(ref).split(':', 1)
        return self.books.group(book_name, group_name)

    def values(self, key):
        """All records named `key` (groups as Group, names as str), from the nearest group that has them."""
        g = self
        while g is not None:
            found = g.own(key)
            if found:
                return [g.convert(kind, value) for kind, value in found]
            g = g.parent()
        return []

    def get(self, key, default=None):
        v = self.values(key)
        return v[0] if v else default

    def items(self):
        """Unnamed list elements of this group (Location's x, y, z; a ShapeSet's names; a Floor's tiles)."""
        return [self.convert(kind, value) for kind, name, value in self.records if (kind >> 8) & 0xFF == 2]

    def convert(self, kind, value):
        t = kind & 0xFF
        if t == prb.GROUP:
            return Group(self.books, self.book, value)
        if t == prb.NAME:
            return self.book.name(value)
        return value


def facade(width, depth, storeys):
    """Section (positions, normals, uvs, strip flags) of the generated upper storeys of a building.
    storeys: [(z, [tile column per facade cell])]"""
    positions, normals, uvs, closes = [], [], [], []
    hw, hd = width * CELL / 2, depth * CELL / 2
    for z, tiles in storeys:
        cells = []
        for j in range(depth):  # left side, back to front, facing -X
            y0 = hd - CELL * j
            cells.append(((-hw, y0), (-hw, y0 - CELL), (-1, 0, 0)))
        for k in range(width):  # front, left to right, facing -Y
            x0 = -hw + CELL * k
            cells.append(((x0, -hd), (x0 + CELL, -hd), (0, -1, 0)))
        for j in range(depth):  # right side, front to back, facing +X
            y0 = -hd + CELL * j
            cells.append(((hw, y0), (hw, y0 + CELL), (1, 0, 0)))
        for (a, b, n), tile in zip(cells, tiles):
            u0 = (int(tile) % 4) * SHEET_TILE
            # a quad as its own strip: bottom-left, top-left, bottom-right, top-right
            for (x, y), zz, u, v, c in (((a, z, u0, SHEET_TILE, False)), ((a, z + STOREY, u0, 0.0, False)),
                                        ((b, z, u0 + SHEET_TILE, SHEET_TILE, True)),
                                        ((b, z + STOREY, u0 + SHEET_TILE, 0.0, True))):
                positions.append((x, y, zz))
                normals.append(n)
                uvs.append((u, v))
                closes.append(c)
    return mesh.Section(len(positions), 0, positions, normals, None, uvs, closes, None, None)


class CityExporter:
    def __init__(self, voldir, city, out):
        self.books = Books(voldir)
        self.city = city
        self.out = os.path.join(out, city)
        with open(os.path.join(voldir, self.books.files[city.lower() + '.bdp']), 'rb') as f:
            self.bundle = bdp.Bundle(f.read())
        self.name_index = {n.lower(): i for i, n in enumerate(self.bundle.names)}
        self.models = anim.Models(self.bundle)
        self.world = self.books.group(city + '_Props', 'WorldData')
        self.template_books = [self.world.get('ObjectTemplateBook'), self.world.get('WeaponsTemplateBook'),
                               city + '_Props', city + '_buildings']
        self.missing = collections.Counter()

    def template(self, name):
        for book in self.template_books:
            if book:
                g = self.books.group(book, name)
                if g is not None:
                    return g
        return None

    def place_model(self, assets, g, name, parent, translation=(0, 0, 0), rotation=None):
        """The model with its node hierarchy (helicopters, the sky's cloud band, ...); a model that moves on its
        own gets its RawAnim as a looping clip."""
        placed = self.models.add(g, assets, name, parent, translation, rotation)
        if placed is None:
            self.missing[name] += 1
            return None
        root, joints = placed
        self.models.loop(g, name, joints)
        return root

    def place_object(self, assets, g, inst, parent, base=(0.0, 0.0, 0.0)):
        name = inst.get('Name')
        loc = inst.get('Location')
        xyz = [float(v) for v in loc.items()] if isinstance(loc, Group) else [0.0, 0.0, 0.0]
        rot = inst.get('ZRotation')
        t = self.template(name) if name else None
        shape = t.get('Shape|Shape') if t else None
        if not shape:
            self.missing[f'{name} (no shape)'] += 1
            return
        self.place_model(assets, g, shape, parent, [base[k] + xyz[k] for k in range(3)], rot)

    def building(self, assets, g, inst, row_offset, parent):
        t = self.template(inst.get('TemplateName'))
        if t is None:
            self.missing[f'building {inst.get("TemplateName")}'] += 1
            return
        width, depth = t.get('width', 1), t.get('depth', 2)
        x = float(inst.get('LeftEdgeXPos', 0)) + width * CELL / 2
        y = row_offset * inst.get('Row', 0)
        node = g.node(f'{inst.get("TemplateName")} @ {x:g}', None, (x, y, 0), None, parent)
        ground = t.get('ShapeSet|GroundFloorShapes')
        if ground and ground.items():
            self.place_model(assets, g, ground.items()[0], node)
        floors = [f for f in t.values('Floor|') if f.records]
        storeys = [(GROUND_FLOOR + STOREY * (i - 1), f.items()) for i, f in enumerate(floors) if i > 0]
        sheet = t.get('TextureSheet')
        if storeys and sheet:
            png = assets.texture(self.name_index.get(sheet.lower(), -1))
            mat = g.material(('sheet', sheet), sheet, png)
            m = g.mesh(f'{inst.get("TemplateName")} facade', [(mat, facade(width, depth, storeys))])
            if m is not None:
                g.node('facade', m, parent=node)
        top = t.get('ShapeSet|TopFloorShapes')
        if top and top.items():
            self.place_model(assets, g, top.items()[0], node, (0, 0, GROUND_FLOOR + STOREY * (len(floors) - 1)))
        for obj in inst.values('ObjectInstance|'):
            self.place_object(assets, g, obj, node)

    def block(self, block_name):
        block = self.world.get('BlockData').get('BlockDef|' + block_name)
        g = mesh.Gltf()
        assets = mesh.BundleAssets(self.bundle, g, self.out, 'textures')
        root = g.node(block_name)
        # the day backdrop and sky ("Chicago_Backdrop", "SanFran_Skydome_Main", ...)
        for model in sorted(assets.models):
            if model.endswith(('_backdrop', '_skydome_main')):
                self.place_model(assets, g, model, root)
        ground = block.get('Shape|GroundShape')
        if ground:
            self.place_model(assets, g, ground, root)
        row_offset = float(block.get('BackRowOffset', 10))
        for inst in block.values('BuildingInstance|'):
            self.building(assets, g, inst, row_offset, root)
        for key in ('ObjectInstance|', 'AttackObjectInstance|', 'XObjectInstance|'):
            for inst in block.values(key):
                self.place_object(assets, g, inst, root)
        os.makedirs(self.out, exist_ok=True)
        g.save(os.path.join(self.out, block_name + '.gltf'))
        return len(block.values('BuildingInstance|'))

    def blocks(self):
        seen = []
        for b in self.world.get('BlockData').get('BlockList').items():
            if b not in seen:
                seen.append(b)
        return seen


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    voldir, city, out = sys.argv[1:]
    x = CityExporter(voldir, city, out)
    for block in x.blocks():
        n = x.block(block)
        print(f'{block}: {n} buildings')
    if x.missing:
        print('not placed (no model found):', ', '.join(f'{k} x{v}' for k, v in x.missing.most_common(12)))


if __name__ == '__main__':
    main()
