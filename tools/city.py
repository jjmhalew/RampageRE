"""Assemble Rampage city blocks from their property books into glTF scenes.

A city is played block by block. `<City>_Props.prb` (WorldData) lists the blocks; each block definition gives
its ground shape, the buildings (template, left edge X, front or back row) and the street objects (template,
location, rotation). Building templates are in `<City>_buildings.prb`: a ground-floor shape, a top shape, a
texture sheet and one `Floor|` per storey with a facade tile per cell. The meshes are in `<City>.bdp`. The Wii's
cities (the PS2's seven and Dallas) are read the same way from its .bdg files.
See docs/FORMAT_BDP.md ("City layout").

Building geometry (game units; one cell is 5 x 5, Z up, the street is towards -Y):
  ground-floor shape at z 0 (6 high), then one storey 3 high per Floor| with tiles, then the top shape (1.5 high).
  The upper storeys have no mesh of their own: the game builds the facade from the texture sheet, a 4 x 4
  grid of tiles (column = window style, row = damage state). A storey lists width + 2 * depth tile columns:
  left side (back to front), front (left to right), right side (front to back).

People and traffic (the blocks' ActorInstance| and TrafficDef, see "Spawns" in docs/FORMAT_BDP.md):
  an actor stands at its Location in the state its template's DefaultAnim names (ActorAnimShape|<state>: model
  and clip), playing that clip in a loop. Traffic runs on two lanes, as 0x332D90 spawns it: front y = -12.5
  towards +x, rear y = -7.5 towards -x, x from -75 to 75 at 15 units/s; the vehicles are a fixed sample of the
  block's VehicleDef mix (the game picks them at random), so the loop shows the kind of traffic, not a recording.

Lighting (WorldData.LightingData, or LightingDataNight with --night): the backdrop and sky models, fog and a light
  file (common.bdp: an ambient colour and directional lights) become glTF lights (KHR_lights_punctual) and scene
  extras ("ambient", "fog"); the cloud band material scrolls its texture (material extras "uvRate", taken as
  texture widths per second).

Usage: python tools/city.py VOLDIR CITY OUTDIR [--night]   e.g. tools/city.py build/vol Chicago build/gltf
       writes OUTDIR/CITY/<block>.gltf (or <block>_night.gltf) for every block, the textures in OUTDIR/CITY/textures
"""
import collections
import math
import os
import random
import sys

import anim
import bdp
import mesh
import prb

CELL = 5.0
GROUND_FLOOR = 6.0
STOREY = 3.0
SHEET_TILE = 0.25
# traffic (0x332D90): lanes, direction (+1 = towards +x), turn of the model in degrees
LANES = (('front', -12.5, 1, -90.0, 1), ('rear', -7.5, -1, 90.0, 2))  # last: the NoFront/NoRearTraffic bit
LANE_END = 75.0
TRAFFIC_SPEED = 15.0
SPAWN_INTERVAL = 3.5  # the release timers start at 2 + 3 * rand seconds


class Books:
    """Property books by name ("Chicago_Props" is the .prb in Chicago_Props.bdp), with inheritance."""

    def __init__(self, voldir):
        self.voldir = voldir
        self.files = {f.lower(): f for f in os.listdir(voldir)}
        self.cache = {}

    def file(self, name):
        """The bundle `name` (the PS2's .bdp or the Wii's .bdg), or None."""
        return self.files.get(name.lower() + '.bdp') or self.files.get(name.lower() + '.bdg')

    def book(self, name):
        key = name.lower()
        if key not in self.cache:
            f = self.file(key)
            if f is None:
                self.cache[key] = None
            else:
                with open(os.path.join(self.voldir, f), 'rb') as h:
                    b = bdp.Bundle(h.read())
                entry = next(((e, a) for e, a, _ in b.items(0) if e.type == 22), None)
                self.cache[key] = prb.Book(entry[1], b.names, b.endian) if entry else None
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

    def prefixed(self, prefix):
        """Own records whose name starts with prefix: list elements "Type|" and typed keys "Type|Name"."""
        return [self.convert(kind, value) for kind, name, value in self.records if name.startswith(prefix)]

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


def read_lights(text):
    """(ambient (r, g, b), [((r, g, b), direction)]) of a light file: BEGIN_AMBIENT / BEGIN_LIGHT blocks of
    "KEY value" lines, // comments."""
    ambient, lights, block, values = (0.0, 0.0, 0.0), [], None, {}
    for line in text.splitlines():
        words = line.split('//')[0].split()
        if not words:
            continue
        if words[0] in ('BEGIN_AMBIENT', 'BEGIN_LIGHT'):
            block, values = words[0], {}
        elif words[0] == 'END_AMBIENT':
            ambient = tuple(float(values.get(k, 0)) for k in ('RED', 'GREEN', 'BLUE'))
        elif words[0] == 'END_LIGHT':
            if values.get('LIGHT_TYPE', 'DIRECTIONAL') == 'DIRECTIONAL':
                lights.append((tuple(float(values.get(f'DIFFUSE_{k}', 0)) for k in 'RGB'),
                               tuple(float(values.get(f'DIRECTION_{k}', 0)) for k in 'XYZ')))
        elif len(words) > 1:
            values[words[0]] = words[1]
    return ambient, lights


def rotation_from_minus_z(d):
    """Quaternion (x, y, z, w) turning (0, 0, -1), the way a glTF directional light shines, onto d."""
    n = math.sqrt(sum(c * c for c in d))
    dx, dy, dz = (c / n for c in d)
    # a = (0, 0, -1): a x d = (dy, -dx, 0), a . d = -dz
    x, y, z, w = dy, -dx, 0.0, 1.0 - dz
    if w < 1e-6:  # d = (0, 0, 1): half a turn about x
        return [1.0, 0.0, 0.0, 0.0]
    n = math.sqrt(x * x + y * y + z * z + w * w)
    return [x / n, y / n, z / n, w / n]


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
    def __init__(self, voldir, city, out, night=False):
        self.voldir = voldir
        self.night = night
        self.books = Books(voldir)
        self.city = city
        self.out = os.path.join(out, city)
        with open(os.path.join(voldir, self.books.file(city)), 'rb') as f:
            self.bundle = bdp.Bundle(f.read())
        self.name_index = {n.lower(): i for i, n in enumerate(self.bundle.names)}
        self.models = anim.Models(self.bundle)
        self.world = self.books.group(city + '_Props', 'WorldData')
        self.lighting = self.world.get('LightingDataNight' if night else 'LightingData')
        self.template_books = [self.world.get('ObjectTemplateBook'), self.world.get('WeaponsTemplateBook'),
                               city + '_Props', city + '_buildings']
        self.missing = collections.Counter()
        self.actors = self.vehicles = 0

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

    def place_actor(self, assets, g, inst, parent):
        """A person standing in the block (ActorInstance|), in its template's default state, its clip looping."""
        name = inst.get('Name')
        t = self.template(name) if name else None
        if t is None:
            self.missing[f'actor {name}'] += 1
            return
        state = t.get('DefaultAnim')
        look = t.get(f'ActorAnimShape|{state}') or t.get('ActorAnimShape|Idle')
        clip_name = look.get('Anim|AnimName') if look else None
        clip = self.models.clip_by_name.get(clip_name.lower()) if clip_name else None
        shape = (look.get('Shape|ShapeName') if look else None) or t.get('Shape|ShapeName') or t.get('Shape|Shape')
        if not shape and clip:
            shape = clip.skeleton_name  # the clip's own node tree
        loc = inst.get('Location')
        xyz = [float(v) for v in loc.items()] if isinstance(loc, Group) else [0.0, 0.0, 0.0]
        placed = self.models.add(g, assets, shape, parent, xyz, inst.get('ZRotation')) if shape else None
        if placed is None:
            self.missing[f'actor {name} ({shape})'] += 1
            return
        _, joints = placed
        if clip and clip.bones == len(joints):
            anim.add_animation(g, f'{name} {clip_name} loop', clip.duration, anim.keyframe_channels(clip, joints))
        else:
            self.models.loop(g, shape, joints)
        self.actors += 1

    def traffic(self, assets, g, block, block_name, parent):
        """Vehicles driving along the two lanes, as one looping clip."""
        td = block.get('TrafficDef')
        if td is None:
            return
        mix = [(v.get('Frequency', 0.0), v.get('OTRef|Name')) for v in td.values('VehicleDef|')]
        mix = [(f, n) for f, n in mix if f > 0 and n]
        if not mix:
            return
        rng = random.Random(block_name)
        period = 2 * LANE_END / TRAFFIC_SPEED  # one crossing, 10 s
        per_lane = max(1, min(round(period / SPAWN_INTERVAL), td.get('MaxVehicles', 7) // 2))
        channels = []
        off = (1 if td.get('NoFrontTraffic') else 0) | (2 if td.get('NoRearTraffic') else 0)
        for lane, y, direction, turn, bit in LANES:
            if off & bit:
                continue
            for k in range(per_lane):
                vehicle = rng.choices([n for _, n in mix], [f for f, _ in mix])[0]
                t = self.template(vehicle)
                shape = t.get('Shape|Shape') if t else None
                placed = self.models.add(g, assets, shape, parent, (0.0, y, 0.0), turn) if shape else None
                if placed is None:
                    self.missing[f'vehicle {vehicle} ({shape})'] += 1
                    continue
                root, _ = placed
                # starts k / per_lane of the way along, wraps around at the end
                f0 = k / per_lane
                x = lambda f: direction * (-LANE_END + 2 * LANE_END * f)
                times = [0.0, 1.0 - f0, min(1.0 - f0 + 1e-4, 1.0), 1.0] if f0 else [0.0, 1.0]
                values = [x(f0), x(1.0), x(0.0), x(f0)] if f0 else [x(0.0), x(1.0)]
                if f0 and times[2] >= 1.0:
                    times, values = times[:2], values[:2]
                channels.append((root, 'translation', times, [(v, y, 0.0) for v in values]))
                self.vehicles += 1
        if channels:
            anim.add_animation(g, 'traffic loop', period, channels)

    def building(self, assets, g, inst, row_offset, parent):
        t = self.template(inst.get('TemplateName'))
        if t is None:
            self.missing[f'building {inst.get("TemplateName")}'] += 1
            return
        width, depth = t.get('width', 1), t.get('depth', 2)
        x = float(inst.get('LeftEdgeXPos', 0)) + width * CELL / 2
        y = 5 + (row_offset if inst.get('Row', 0) else 0)   # 0x305E78: the fronts behind the far sidewalk
        node = g.node(f'{inst.get("TemplateName")} @ {x:g}', None, (x, y, 0), None, parent)
        ground = t.get('ShapeSet|GroundFloorShapes')
        if ground and ground.items():
            self.place_model(assets, g, ground.items()[0], node)
        floors = [f for f in t.values('Floor|') if f.records]
        storeys = [(GROUND_FLOOR + STOREY * i, f.items()) for i, f in enumerate(floors)]   # every tiled Floor|
        sheet = t.get('TextureSheet')
        if storeys and sheet:
            png = assets.texture(self.name_index.get(sheet.lower(), -1))
            mat = g.material(('sheet', sheet), sheet, png)
            m = g.mesh(f'{inst.get("TemplateName")} facade', [(mat, facade(width, depth, storeys))])
            if m is not None:
                g.node('facade', m, parent=node)
        top = t.get('ShapeSet|TopFloorShapes')
        if top and top.items():
            self.place_model(assets, g, top.items()[0], node, (0, 0, GROUND_FLOOR + STOREY * len(floors)))
        for obj in inst.values('ObjectInstance|'):
            self.place_object(assets, g, obj, node)

    def block(self, block_name):
        block = self.world.get('BlockData').get('BlockDef|' + block_name)
        g = mesh.Gltf()
        assets = mesh.BundleAssets(self.bundle, g, self.out, 'textures')
        root = g.node(block_name)
        sky = [self.lighting.get('Shape|Backdrop'), self.lighting.get('Shape|SkyBox')] if self.lighting else []
        if not all(sky):  # by name: "Chicago_Backdrop", "SanFran_Skydome_Main", ...
            sky = [m for m in sorted(assets.models) if m.endswith(('_backdrop', '_skydome_main'))]
        for model in sky:
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
        # people there from the start (SpawnedItem = 1: one that comes after an earlier one is gone)
        for inst in block.prefixed('ActorInstance|'):
            if not inst.get('SpawnedItem'):
                self.place_actor(assets, g, inst, root)
        self.traffic(assets, g, block, block_name, root)
        if self.lighting:
            self.light(g)
        os.makedirs(self.out, exist_ok=True)
        g.save(os.path.join(self.out, block_name + ('_night' if self.night else '') + '.gltf'))
        return len(block.values('BuildingInstance|'))

    def light_file(self, name):
        with open(os.path.join(self.voldir, self.books.file('common')), 'rb') as f:
            common = bdp.Bundle(f.read())
        for e, a, _ in common.items(0):
            if e.type == 2 and common.name(e.name).lower() == name.lower():
                return a.decode('latin1')
        return None

    def light(self, g):
        """The block's lights, fog and scrolling clouds from LightingData."""
        ld = self.lighting
        extras = g.doc['scenes'][0].setdefault('extras', {})
        text = self.light_file(ld.get('text|LightFile', 'default_lights.txt'))
        if text:
            ambient, lights = read_lights(text)
            extras['ambient'] = list(ambient)
            out = []
            for i, (colour, direction) in enumerate(lights):
                peak = max(colour) or 1.0
                # intensity in glTF's units: a surface facing the light gets colour x texture, as in the game
                out.append({'type': 'directional', 'name': f'light {i}', 'color': [c / peak for c in colour],
                            'intensity': peak * math.pi})
                n = g.node(f'light {i}')
                g.doc['nodes'][n]['rotation'] = rotation_from_minus_z(direction)
                g.doc['nodes'][n]['extensions'] = {'KHR_lights_punctual': {'light': i}}
            g.doc.setdefault('extensions', {})['KHR_lights_punctual'] = {'lights': out}
            g.doc.setdefault('extensionsUsed', []).append('KHR_lights_punctual')
        if ld.get('FogEnd') is not None:
            extras['fog'] = {'color': [ld.get(f'Fog{k}', 0) / 255 for k in 'RGB'],
                             'start': ld.get('FogStart', 0.0), 'end': ld.get('FogEnd')}
        sky = ld.get('AnimatingSkyData')
        if sky and sky.get('MaterialName'):
            for m in g.doc['materials']:
                if m['name'].lower() == sky.get('MaterialName').lower():
                    m['extras'] = {'uvRate': [sky.get('UAnimRate', 0.0), sky.get('VAnimRate', 0.0)]}

    def blocks(self):
        seen = []
        for b in self.world.get('BlockData').get('BlockList').items():
            if b not in seen:
                seen.append(b)
        return seen


def main():
    args = [a for a in sys.argv[1:] if a != '--night']
    if len(args) != 3:
        sys.exit(__doc__)
    voldir, city, out = args
    x = CityExporter(voldir, city, out, night='--night' in sys.argv)
    for block in x.blocks():
        actors, vehicles = x.actors, x.vehicles
        n = x.block(block)
        print(f'{block}: {n} buildings, {x.actors - actors} people, {x.vehicles - vehicles} vehicles')
    if x.missing:
        print('not placed (no model found):', ', '.join(f'{k} x{v}' for k, v in x.missing.most_common(12)))


if __name__ == '__main__':
    main()
