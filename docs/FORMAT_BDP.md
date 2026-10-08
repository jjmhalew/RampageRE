# .bdp / .cmp – Pipeworks bundles

Everything that is not a script, a video or a sound stream is in a bundle: the cities (`Chicago.bdp`, 7 MB), their
props, buildings and scripts, the monsters (`George.cmp` with model and textures, `George_anims.cmp` with the
animations), the menus (`Shell.bdp`, `CommonGUI.bdp`, `WorldGui.bdp`), loading screens and effects (`fx.bdp`).
`.bdp` and `.cmp` are the same format (117 files). The 60 `.zip` files in `RAMPAGE.VOL` each hold a byte-identical
copy of the `.cmp` of the same name.

```
python tools/bdp.py build/vol/George.cmp              # list the entries
python tools/bdp.py build/vol/George.cmp outdir       # extract each entry's data blocks
python tools/bdp.py --verify build/vol                # parse all 117: every byte is a known structure or padding
python tools/tex.py --all build/vol build/textures    # all 1,428 textures as PNG
python tools/mesh.py --verify build/vol               # all 5,577 meshes: every part agrees with its header
python tools/mesh.py build/vol/George.cmp build/gltf  # glTF with materials and textures (viewer/index.html)
python tools/prb.py build/vol/Chicago_Props.bdp       # a property book as a readable tree
python tools/city.py build/vol Chicago build/gltf     # every block of a city as a glTF scene
```

## In the game
| Address | |
|---|---|
| 0x1A8740 | `BundleVolume` setup: checks the version (1300), resolves names, entries and data pointers, applies a segment |
| 0x4451C0 | `BundleVolume` vtable; `BundleReaderSynchronous` 0x445210, `BundleReaderZippedMemory` 0x4451F0 (the `.zip` copies) |

## File layout (little endian)
| Offset | Type | |
|---|---|---|
| 0x00 | char[0x2C] | `"Pipeworks Bundle File version 1.3"`, zero padded |
| 0x2C | u32 | absent segments: bit n set = segment n is empty (`0x3FE` in 116 files: only segment 0) |
| 0x30 | u32 | version: 1300 (0x514); the game refuses other values |
| 0x34 | u32 | offset of the name table |
| 0x38 | u32[10] | offset of each segment header |

**Name table:** u32 count, then count × u32 offset of each name (from the start of the table), then the
NUL-terminated names. Entries refer to names by index. Names inside monster bundles start with `"\x13000"`.

**Segment header** (0x18 bytes, then the records):
| Offset | Type | |
|---|---|---|
| 0x00 | u16 | segment number |
| 0x02 | u16 | number of records |
| 0x04 | u32 | offset of the entry table (16 bytes per entry) |
| 0x08 | u32, u32 | offset and size of the segment's **data** block |
| 0x10 | u32, u32 | offset and size of the segment's **data2** block |
| 0x18 | 0x12 bytes each, packed | record: u16 entry index, u32 data offset, u32 data size, u32 data2 offset, u32 data2 size (relative to the blocks) |

**Entry** (16 bytes): u16 entry index, u8 type, u8 (0, 1 on 60 skeletons), u32 name index, u32 (1 in all but
3 of the 19,033 entries), u32 (data size for some types). At load time the game replaces the last two words with pointers to the
entry's data and data2.

**Segments.** Segment 0 holds every entry. The others replace entries of segment 0 (by entry index) when the game
asks for that segment. Only `Shell.bdp` has any: segments 1, 2 and 7 hold other versions of `MonsterSelect.ifc`,
`MonsterSheet.ifc` and `MainMenu.ifc` (probably language or video-mode variants of these menu layouts).

## Entry types
| Type | Count | What | data / data2 |
|---|---|---|---|
| 0 | 5,547 | mesh (`MODEL\|object`), see below | header / vertices |
| 2 | 327 | raw file: `.bsf` scripts (269, all identical to the ones in `RAMPAGE.VOL`), `.ifl`, `.fdf` (font metrics), `MONSTER_DATA` | file / – |
| 3 | 615 | skeleton | |
| 4 | 5,379 | animation | |
| 6 | 1,893 | material, see below | |
| 9 | 1,428 | texture | header / pixels (see below) |
| 13 | 1,275 | palette (`.pal`) | header / 256 colours |
| 16 | 2,119 | `.pwk`: model description; holds u16 name indices of the model's materials, 0xFFFF terminated (probably) | |
| 17 | 30 | skinned mesh (monster bodies), see below | header / vertices |
| 20 | 297 | GUI image description (text) | |
| 22 | 28 | property book (`.prb`), see below | |
| 23 | 13 | text (`.loc`): names of city blocks, challenge texts | |
| 24 | 43 | GUI layout (`.ifc`) | |
| 25 | 44 | cloth | |

Types 3, 4, 20, 24 and 25 are named from their entry names and sizes; their contents are not decoded yet.

## Textures (type 9) and palettes (type 13)
Texture data, 0x104 bytes:
| Offset | Type | |
|---|---|---|
| 0x00 | u32 | kind: 3 with PSMT8, 1 with PSMCT32, 9 with PSMCT16 |
| 0x04 | u8 | GS pixel format: 0x13 PSMT8 (1,291 textures), 0x00 PSMCT32 (71), 0x02 PSMCT16 (66) |
| 0x07 | u8 | 0x40: pixels stored in GS memory order (PSMT8 written as PSMCT32) |
| 0x08 | u16, u16 | width, height (powers of two) |
| 0x0C | u32 | mip levels (1–4) |
| 0x10 | u32 | size of the pixels in data2 (all mip levels, largest first) |
| 0x14 | u16, u16 | used width and height (fonts and GUI art do not fill the texture) |
| 0x18 | f32, f32 | used / full size, as texture coordinate scales |
| 0x20 | u32 | name index of the palette (0xFFFF: none) |

Palette data2: 256 RGBA colours in the GS CSM1 order (entries 8–15 and 16–23 of each group of 32 swapped); alpha
0x80 is opaque. Fonts and some effects name `_Alpha8` (white, alpha 0–255) or `_Intensity8` (grey ramp), palettes
the engine makes itself; they are not stored in any bundle. PSMCT32 alpha is also 0x80 = opaque; PSMCT16 is
5:5:5:1.

## Meshes (types 0 and 17)
A mesh is one or more parts; each part has one material and one strip of vertices (a *section*).

Type 0 data: u32 part count, 7 × f32 bounds, then 0x50 bytes per part from 0x20. Type 17 data: u16 part count,
u16 bone count, u32 name index of every bone the mesh uses, 3 × u32, then 0x58 bytes per part.

| Part record | Type 0 | Type 17 |
|---|---|---|
| u32 strip length − 2 (vertex count − 2) | +0x00 | +0x00 |
| u16 material name index | +0x0A | +0x38 |
| f32 bounding box (min, max) | +0x14 | +0x08 |
| u32 offset of the section block, from the record | +0x3C | +0x40 |
| u32 offset of the section descriptor, from the record | +0x40 | +0x44 |
| u32 offset of the bone palette, from the record | | +0x48 |

Section descriptor: u32 vertex count, u32 ?, u32 vertex format, u32 0, u32 offset in data2, u32 size. In data2
the vertices are separate streams, each starting on a 16-byte boundary, in this order:

| Format bit | Stream | Per vertex |
|---|---|---|
| (always) | position | 4 × f32: x, y, z, w. Rigid: w = 1.0 if the vertex closes a triangle, 0.0 if it starts a strip. Skinned (0x08): w is u16 weight of the first bone (4.12), u8 bone 0 × 4, u8 bone 1 × 4 |
| 0x010 | normal | 3 × s8, u8: skinned 0x7F = closes a triangle, 0 = starts a strip |
| 0x040 | colour | 4 × u8 RGBA, 0x80 = 1.0 |
| 0x100 | texture coordinates | 2 × s16, 4.12 fixed point |

Formats in use: 0x112 (most sections), 0x102, 0x152, 0x142, 0x12 and 0x118 (skinned). 0x02 is set on almost every
section; its meaning is not known. Triangles follow the PS2 strip rule: every vertex flagged "closes" makes a
triangle with the two before it, winding alternating along the strip. Rigid meshes are often preceded in data2
by a u16 list ended by 0xFFFF (probably edge or shadow data). Positions are in the model's space: monster bodies
are in their bind pose (George in a T-pose), city pieces each at their own origin, so placing them needs the
level layout from the property books (below).

## Materials (type 6)
0x90 bytes: u16 own name index, u16 layer count?, u16 flags, ..., f32 colours from 0x08 (diffuse, ambient,
specular, ...), u16 name index of the texture at 0x38 (0 = no texture). 1,412 of the 1,695 materials name a
texture; the rest have none or name an `.ifl` animation (a text list of frame images and durations) or another
material.

## Property books (type 22)
Most of the game's design data: the city layout, building templates, AI spawn rules, challenges, monster
upgrades, camera settings, object templates. All 28 books decode (224,218 lines with `tools/prb.py`).

The game uses a book in place: 0x1B00E0 puts a `PropertyBook` object (vtable 0x445380) over the entry data and
0x1B0170 turns the offsets into pointers. 0x1AFC80 builds the same structure from a *text* book (it skips
whitespace and `//` comments), so the books were written as text.

| Offset | Type | |
|---|---|---|
| 0x00 | 0x1C bytes | room for the runtime object (vtable, ...) |
| 0x1C | u32 | number of top-level groups |
| 0x20 | u32 | size |
| 0x24 | u32 | offset of the group table: a u32 offset per group |

**Group:** u32 0, u32 name index, u32 record count, u32 ?, u32 header size (0x14), then 12-byte records
`{u32 kind, u32 name index, u32 value}`.

| `kind & 0xFF` | Value |
|---|---|
| 0 | int |
| 1 | float |
| 2 | name index (strings, references to other entries such as `"ChicagoBlocks_01.pwk"`) |
| 3 | group, at this offset from the start of the containing group |

| `(kind >> 8) & 0xFF` | Record |
|---|---|
| 0 | plain (`Row = 1`) |
| 1 | typed key `Type\|Name` (`BlockDef\|CG01_Block1`); `kind >> 16` = length of `Type` |
| 2 | list element without a name (the x, y, z of a `Location`); the name index is a placeholder |
| 3 | list element of a type `Type\|` (`BuildingInstance\|`); `kind >> 16` = length of `Type` |

### City layout
`<City>_Props.prb` has a `WorldData` group: `BlockData.BlockList` names the city blocks in order (`CG01_Block1`
...), and each `BlockDef|<block>` holds the ground shape (`Shape|GroundShape = "ChicagoBlocks_01.pwk"`),
`BackRowOffset`, the spawn rules (`AIDef`: tanks, SWAT, helicopters with delays), the block's challenge, and the
buildings: `BuildingInstance| { LeftEdgeXPos = -42  Row = 1  TemplateName = "CG1_4x1" }`, with signs and other
objects at a `Location` (x, y, z) relative to the building. The buildings are placed side-scroller style along
X, in a front row (0) and a back row (1). `<City>_buildings.prb` defines the templates (`CG1_4x1`): width, depth,
floors (`Floor|` with ground, middle and top floor shapes, damaged variants), texture sheet, debris effects
(`.edf`) and the script (`DefaultBuilding.bsf`).

### Assembling a block (`tools/city.py`)
All seven cities export (Chicago, New York, San Francisco, Los Angeles, Las Vegas, London, Hong Kong: one glTF per
block, every building and object model found). Game units, Z up, the street towards −Y; one cell is 5 × 5:
- the block's ground shape, the city's day backdrop (`*_Backdrop`) and sky (`*_Skydome_Main`) at the origin;
- a building's centre is at x = `LeftEdgeXPos` + width × 2.5, y = `Row` × `BackRowOffset`;
- the ground-floor shape (`ShapeSet|GroundFloorShapes`, 6 high) at z 0, then storeys 2..n (`Floor|` entries; the
  template name is width × storeys, `CG2_2x7`) 3 high each, then the top shape (`ShapeSet|TopFloorShapes`);
- upper storeys have no mesh: the facade is built from the `TextureSheet`, a 4 × 4 grid of tiles (column =
  window style, row = damage state, 0 intact). Each `Floor|` lists width + 2 × depth tile columns: left side
  (back to front), front (left to right), right side (front to back); the back is never seen. A floor's
  `Skeleton` (`Damaged10x20_00.pwk`) is the interior shown once the building is damaged;
- objects (`ObjectInstance|`, `AttackObjectInstance|`, ...) at their `Location`, turned by `ZRotation` degrees;
  objects inside a `BuildingInstance|` are relative to the building. `Name` is a template in the
  `ObjectTemplateBook` (`Objects1_Props`), whose `Shape|Shape` names the model; templates inherit through
  `ParentPropertySet = "Group:Book"`.

Not reproduced yet: the facade layout is inferred (it looks right in all cities, not checked against the game),
vertex lighting (`LightingData`), people, vehicles and AI spawns, and some backdrop planes come out white.
