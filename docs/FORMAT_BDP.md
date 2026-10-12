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
| 3 | 615 | node tree; with a bind pose (byte 3 of the entry = 1) a skeleton, see below | |
| 4 | 5,379 | animation: 4,764 keyframed, 615 sampled (one per node tree), see below | |
| 6 | 1,893 | material, see below | |
| 9 | 1,428 | texture | header / pixels (see below) |
| 13 | 1,275 | palette (`.pal`) | header / 256 colours |
| 16 | 2,119 | `.pwk`: model description; holds u16 name indices of the model's materials, 0xFFFF terminated (probably) | |
| 17 | 30 | skinned mesh (monster bodies), see below | header / vertices |
| 20 | 297 | effect ("Text EDF file": particles, composites, camera shakes), see [FORMAT_EDF.md](FORMAT_EDF.md) | |
| 22 | 28 | property book (`.prb`), see below | |
| 23 | 13 | string table (`.loc`): block names, menus, challenge texts in 9 languages, see [FORMAT_EDF.md](FORMAT_EDF.md) | |
| 24 | 43 | GUI layout (`.ifc`, XML), see [FORMAT_EDF.md](FORMAT_EDF.md) | |
| 25 | 44 | cloth: particles, springs and triangles of a monster's hair or a flag, see below | |

Most `.ifc` tags are not decoded yet.

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
| (always) | position | 4 × f32: x, y, z, w. Rigid: w = 1.0 if the vertex closes a triangle, 0.0 if it starts a strip. Skinned (0x08): w is u16 weight of the first bone (4.12), u8 bone 0 × 4, u8 bone 1 × 4 (skeleton node indices) |
| 0x010 | normal | 3 × s8, u8: skinned 0x7F = closes a triangle, 0 = starts a strip |
| 0x040 | colour | 4 × u8 RGBA, 0x80 = 1.0 |
| 0x100 | texture coordinates | 2 × s16, 4.12 fixed point |

Formats in use: 0x112 (most sections), 0x102, 0x152, 0x142, 0x12 and 0x118 (skinned). 0x02 is set on almost every
section; its meaning is not known. Triangles follow the PS2 strip rule: every vertex flagged "closes" makes a
triangle with the two before it, winding alternating along the strip. Rigid meshes are often preceded in data2
by a u16 list ended by 0xFFFF (probably edge or shadow data). Positions are in the model's space: monster bodies
are in their bind pose (George in a T-pose), city pieces each at their own origin, so placing them needs the
level layout from the property books (below).

## Skeletons (type 3) and animations (type 4)
Both are used in place, like the property books. `tools/anim.py --verify` parses all 615 node trees and 5,379
animations and checks that their parts follow each other to the last byte, that every keyframed animation has
one translation and one rotation track per bone in bone order, and that the bind pose chained from the root
gives back the stored inverse bind matrices (largest difference 2.7e-6).

**Node tree / skeleton** (0x1A8110: NodeTree, vtable 0x444D30, or Skeleton, vtable 0x444E60; offsets fixed up
by 0x195730 and 0x19E070). The 60 skeletons are the 30 monsters' (in `<Monster>.cmp` and `<Monster>_anims.cmp`);
the other 555 trees are the node hierarchies of city models.

| Offset | Type | |
|---|---|---|
| 0x00 | 0x1C bytes | runtime object (vtable, reference count, ...) |
| 0x1C | u32 | offset of the first node: 0x3C, or 0x40 on a skeleton |
| 0x20 | u32 | node count |
| 0x24 | u32 | tree depth in levels (one more on skeletons): the size of the walk stack |
| 0x28 | u32 | entry size |
| 0x2C | u32 | offset of the node table, one u32 offset per node |
| 0x30 | u32 | offset of the nodes sorted by name index (0x19A270 binary searches it by name) |
| 0x34 | u32 | mirror pair count |
| 0x38 | u32 | offset of the mirror table, 0 = none: u16 per node, the left/right pairs first, then the centre nodes |
| 0x3C | u32 | skeleton only: offset of the bind pose, 0x50 bytes per node |

Node: i32 index, i32 parent (−1 = root), u32 child count, u32 name index, u32 offset of every child. Nodes are
in depth-first order, so a parent always comes before its children.

Bind pose record: u32 0, f32[12] inverse bind matrix (rows: x, y, z axis and translation; row vectors), f32[3]
translation relative to the parent, f32[4] rotation relative to the parent as x, y, z, w. The rotation is for
row vectors: for column vectors (glTF) use its conjugate (−x, −y, −z, w).

The monsters are 3ds Max Biped rigs (`Bip01 Pelvis`, `Bip01 L Thigh`, ...; George has 56 nodes including 20
face bones and a `Lift_Node`). The bones of the skinned mesh's vertices are indices into this node list, not
into the mesh's own bone list.

**Animation** header (0x196730 picks the class by the kind):

| Offset | Type | |
|---|---|---|
| 0x1C | f32 | duration in seconds |
| 0x20 | u32 | kind: 2 = KeyInterpAnim (vtable 0x444C30), 1 = RawAnim (vtable 0x444DF0) |
| 0x24 | u32 | entry size |
| 0x28 | u16, u16 | name index of the skeleton (replaced by a pointer at load), 0 |

*KeyInterpAnim* (every monster and prop animation; fixed up by 0x190720):

| Offset | Type | |
|---|---|---|
| 0x2C | u32 | bone count (= the skeleton's nodes) |
| 0x30 | u32 | bone count again |
| 0x34 | u32 | offset of the channel mask, 2 bits per bone padded to 4 bytes; always all set |
| 0x38 | u32 | offset of the translation tracks |
| 0x3C | u32 | offset of the rotation tracks |
| 0x40 | u32 | runtime work size, 136 bytes per bone |

Translation track (read by 0x191860), one per bone in bone order: f32[3] scale, u8 bone, u8 key count, u8 key
count − 1, u8 0, then 8 bytes per key: u16 time, s16 x, y, z. Value = s16 × scale × f32 0x38000100 (about 1/32768).

Rotation track (0x191C50), one per bone, not aligned: u8 bone, u8 key count, then 8 bytes per key: u16 time,
s16 a, b, c. Decoded by 0x192010:

- x, y, z = (a >> 3, b >> 3, c >> 3) × f32 0x39800801 (about 1/4096);
- r = (a & 3) << 6 | (b & 7) << 3 | (c & 7), negative if a & 4: a small correction to w;
- w = √(1 − (x² + y² + z² + r × f32 0x3B808081 × f32 0x39DD7E7C)), or 0 with x, y, z normalised if that is
  negative;
- w is negated if bit 0 of the time is set.

The quaternion is in the same row-vector convention as the bind pose. Key times are (time & 0xFFFE) / 65534 of
the duration. A track without keys falls back to the bind pose (there are none in the shipped data); between
keys translations are interpolated linearly and rotations spherically. In a body animation about a quarter of
the tracks have a single key.

*RawAnim* (exactly one per node tree, with the same name; fixed up by 0x19C700): 0x2C u32 record count,
0x30 offset of the record table (one offset per record, sorted by bone), 0x34 offset of the first record.
Record: u32 bone, f32 ? (not decoded), u32 translation segment count, u32 rotation segment count, f32 start 0,
f32 end 1, f32 1 / (end − start) = 1 (on all 8,173 records), u32 offset of the translation segments (from the
record), u32 offset of the rotation segments; the next record follows the rotation segments.

The n segments of a track split the clip evenly: at time t (0..1) segment k = ⌊t × n⌋ runs with
f = t × n − k (0x19D020). A translation segment (0x19CEC0) is f32[3] p, f32[3] d: value p + d × f. A rotation
segment (0x19D0A0, 0x167DE0) is f32[4] q0, f32[4] q1, f32 angle a: value normalise(q0 × cos(a f) + q1 × sin(a f)),
or normalise(q0 + q1 × f) when a is 0, a slerp worked out in advance. Every segment ends where the next one
starts (largest difference 5.9e-7 over all 85,801), so sampling the segment ends and slerping between them gives
the same motion. On the 60 monster skeletons the first segment starts at the bind pose exactly.

**Node-tree models.** Every tree has a model of the same name, and the model's mesh objects (`MODEL|object`)
are named after its nodes (on 84 of Chicago's 90 trees all of them; the rest go on the root). Their vertices
are in their node's space, so a model is drawn by placing each node at its RawAnim pose; the city export does
that for every placed model. In the cities the trees are people (`Naked_Male_Held`, `Police_SWAT_WindowEmerge`,
... with their keyframed clips `Running`, `Falling`, `WindowLook`), vehicles (`Helicopter`, `FireTruck`, `Tank_Sherman`),
bosses (`SaucerBoss`, `Blimp`), the fountain and the sky. A tree named after a pose (`Naked_Male_Held`) has
that pose as its RawAnim.

**Monster names.** Animations are shared by skeleton type and prefixed with it (`SKELETON_1_WALK2` for George's
ape skeleton, `SKELETON_2_...` for Ralph). `<MONSTER>_HEAD_*` move the face and head bones (87 of the 330 also
the spine) and leave the rest in the bind pose: they play on the face layer over the body clip
([FORMAT_MONSTER.md](FORMAT_MONSTER.md) "The face"). A KeyInterpAnim's key times are 0 .. 1 of its duration. From the
names (not checked in the code): `FACE_*` are climbing on the front of a building, `SIDE_L_*` on its side,
`GROUND_*`, `IDLE` and `WALK2` on the street.

`tools/anim.py VOLDIR --all OUTDIR` writes every monster as one glTF with its skinned mesh, the skeleton as
joints and all its animations. The cloth meshes (hair, whiskers: type 0 meshes in the monster bundle) are
skinned to the bones and weights of their cloth particles (below); the game simulates them on top of that.

## Cloth (type 25)
`MODEL|cloth` entries: the hair, fur or whiskers of 12 monsters and 32 flags in the cities. Used in place
(vtable 0x446500, offsets fixed up by 0x1DB840); `tools/anim.py --verify` parses all 44 to the last byte.

| Offset | Type | |
|---|---|---|
| 0x1C | f32[3], f32, f32 | bounding sphere: centre, radius squared, radius |
| 0x30 | u32 | entry size |
| 0x34 | u32, u32 | part count, offset of the parts (0x8C bytes each) |
| 0x3C | u32, u32 | bone count, offset of the bone list (u32 skeleton node indices; George: Spine2, Head, both upper arms and forearms) |
| 0x44 | u32, u32 | 0, an offset |

A part: f32[19] simulation settings (gravity 9.8 on the flags, 0 on George; timesteps 1/60 and 1/30), u32 2 or 3,
u8[4] flags, u8[4], f32 100, u32 0, then count and offset of five arrays, which follow each other:

| Array | Item | |
|---|---|---|
| pins | u32 | particles held in place |
| springs | 24 bytes | u32 a, u32 b, f32 rest length, its square, f32 (5 or 4), f32 (200 or 100) |
| bends | 32 bytes | u32[4] particles, f32[3] 0, f32 20 (flags only) |
| triangles | 12 bytes | u32[3] particles |
| particles | 64 bytes (count stored twice) | f32[3] position (bind pose, model space), f32[3] normal, f32 ?, f32[4] bone weights, u16[4] skeleton nodes (0xFFFF = none), u8 bone count, u8 0xEA, u8 1 = held by its bones, u8 0, 8 × 0xFF |

Every vertex of the cloth mesh sits exactly on a particle (checked on five monsters), so the particles' bones and
weights skin the mesh at rest. A flag's particles have no bones.

## Materials (type 6)
0x90 bytes: u16 own name index, u8 kind, u8 ?, u32 flags, ..., f32 colours from 0x08 (diffuse, ambient,
specular, ...), u16 name index of the texture at 0x38 (0 = no texture). 1,412 of the 1,695 materials name a
texture; the rest have none or name an `.ifl` animation or another material. An `.ifl` is a text entry (type 2):
one line per frame, its image's name (`Parking00.bmp`: the texture `Parking00`) and how many frames it shows
(taken as 3ds Max's 30 per second; 20 of them, 9 in Las Vegas: neon signs, fans).

The colour at 0x08 (diffuse rgba) is all a material without a texture has: 218 of them (the saucer's orange
`saucer_blades`, its green `Saucer_glass` at alpha 0.38, flag poles, bumpers, cloth); every textured material's is
white. Alpha below 1 is see-through glass (9 materials: the saucer's, the hover tank's, the helicopter's canopy).
The byte at +0x2 is the material's kind (how many textures it binds, below): 1 none (222 materials), 2 one
(1,621), 5 and 8 a second layer named by the u16 at +0x3A (the byte at +0x3 is 0, or 128 on 90 and 150 on one of
kind 2): 5 a reflection, the sphere map `cubemap` (12 materials: the saucer's top `SB_shinyNOTreflective`, the
trash cans, the Jumper's shiny body; below), 8 a damage blend `DestroyedGeneric` (38 vehicle materials, below).

**How a material is built** [R]: the material library's loader 0x16F540 picks the shader with 0x16F030 from the kind
(+0x2) and the u32 at +0x4 (bits 0, 1, 0x100000, 0x200000, 0x2000000, 0x8000000), builds the material object
(0x25C4F0: 0x90 bytes, vtable 0x4488D0; +0x2C the shader, +0x30 its state) and fills it with 0x25F960: by kind (jump
table 0x426940) no texture (kinds 0, 1: the untextured materials), one (2: +0x38), two (3, 4, 5, 7, 8: +0x38 into
slot 0, +0x3A into slot 1) or three (6: also +0x3C); with flag 0x100 the 16 bytes right after those indices (+0x3A,
+0x3C or +0x3E) go into the first texture's region; the colours at +0x18 and +0x28 as bytes (x 255 + 0.5). A shader
is a 0xD8-byte descriptor (0x40F910 + id x 0xD8) compiled offline: its record (0xB8 bytes at 0x3B4960 + n x 0xB8) is
found by the sum of the descriptor's words mod 0x329 in the table 0x3C73B0 (0xE0 bytes per entry: the sum, n, the
descriptor). A record is u32 passes, then 0x24 bytes per pass: texture slot, GS state (0x30 bytes at 0x3B4240 + k x
0x30: ALPHA_1, TEST_1, PRMODE), ..., the colour mode (0x265840: 0 a constant 255 = the GS's 1.0), the GIF registers
(0x412: STQ, RGBAQ, XYZ2), the VU1 program and its clipping twin.

**The street** [R]: the block grounds' parts (`*Blocks_NN` models: sidewalks, crossings, alleys, grass) are kind 2
materials; 162 of 167 have flags 0xC, which 0x16F030's kind 2 table (0x3B1680: six flag bytes and the shader per
12-byte row, in the order bit 0, bit 1, 0x100000, 0x200000, 0x2000000, 0x8000000) gives shader 1: one pass, GS state
1 (ALPHA 0x44, TEST 0x50002, PRMODE 0x38): opaque, Z tested (GEQUAL) and written. The other five are shader 2
(flags 0x100100: state 2, TEST 0x5000D, the alpha test GREATER 0, Z written) and 0xC (kind 1). So the street writes
depth, and an effect on it is cut by it (FX_NOTES.md "Ground-level quads").

**The damage blend (kind 8)** [R]. All 38 materials have flags 0x0800000C or 0x08000000 (bit 27, not bits 0 / 1), so
0x16F030 gives shader 0x7A (descriptor 0x416000, record 122 at 0x3BA110; kind 8 with bit 27 gives 0x7A, 0x60
see-through (bit 1), 0x7B two-sided (bit 0), 0x61 both; without bit 27 0x5E, 0x5C, 0x5F, 0x5D). Its record: two
passes, vertex format 0x152 (position, normal, colour, uv), VU1 program 0x37A2A0 (0x384668 clipping) for both:
- pass 0, slot 0 (the vehicle's own texture: `ArmyVan`, `Citybus`, ...), state 1: no blend (PRMODE 0x38), Z test
  and write (TEST 0x50002). The colour is written whole, its alpha too: the texture's (opaque in all of them) x the
  vertex's.
- pass 1, slot 1 (`DestroyedGeneric`: 128 x 128 crumpled metal, opaque), state 0x14: ALPHA 0x54 =
  (Cs − Cd) x **Ad** + Cd, the destination alpha pass 0 wrote; PRMODE 0x78 (blended, fogged); TEST 0x50002.

The program lights each vertex (its normal against three light directions, their colours and the ambient at VU
rows 13 .. 16) and multiplies the vertex colour's rgb by that light; the alpha is the vertex's x the constant
colour's (0x80): so the share of `DestroyedGeneric` at a vertex is **its vertex alpha** (0x80 = all of it, 0 = the
vehicle's own texture), and the rgb darkens both layers. Nothing at run time sets it: no material parameter, no
hit points. The 38 materials are only used by the vehicles' damaged meshes (one part each: `citybus_Damaged`,
`Army_Van_Destroyed`, `Car_MinivanRed_Damaged`, `DeliveryVan_Destroyed`, `Garbage_Truck_Damaged`,
`Bus_HongKong_Damaged`, `Bus_UK_Damaged`, the Mini, Sedan, SportsCoupe, Hummer, tour tram, cable car and New
York's train cars), the `DamageShape` a vehicle's `setShape` swaps in (docs/VEHICLE_NOTES.md). Their alphas: the
Army van 355 of 394 vertices 0, 30 near 1; the red minivan 256 of 280 at 0; the delivery van 259 of 419 near 1, the
tanker car 275 of 432. On some the rgb is the alpha again (the Army van, the city bus, the Sedan, the Hummer, the
red and blue Minis: black where the layer is 0, a burnt shell), on the others white (the minivan stays red with
metal on its dents). The viewer draws it so (src/render_gl.c draw_mesh: the part opaque without the alpha test, then
`material_damage_layer`'s texture over it by the vertex alpha). Record 0x7A [R]: pass 0 state 1 (no blend: it writes
the destination alpha, the texture's × the vertex's), pass 1 state 0x14 (ALPHA 0x54, × Ad). GL's source alpha stands
for that destination alpha, the same while both textures are opaque (the frame buffer is PSMCT32 with FBMSK 0, so
Ad is what pass 0 wrote: 0x25E370 -> 0x263380 PSM 0, ZPSM 0x31; FRAME_1 built by 0x264440 [R]).

**The reflections (kind 5) and the cube-map materials** [R]. Kind 5 with flags 0xC (bit 1 clear, as on all 12) is
shader 0x1A (0x19 with bit 1; record 26 at 0x3B5C10), two passes, vertex format 0x112:
- pass 0, slot 0 (the material's own texture), state 1 (opaque, fogged, TEST 0x50002: **no alpha test**), TFX 0
  MODULATE with TCC 1, lit (colour mode 3): it writes the texture's alpha x the vertex's into the frame's alpha. That
  alpha is the reflection's share: `Blimp` has 0 on the gondola and the lettering, 1 on the envelope; `Shiny` a
  gradient; the trash cans, `Train_Tanker` and `Concrete_Grey` 1 all over.
- pass 1, slot 1 (+0x3A: `cubemap`), state 10: ALPHA 0x58 = Cs x **Ad** + Cd (added, by that alpha), PRMODE 0x58
  (blended, **not fogged**), TFX 1 DECAL with TCC 0 and colour mode 0: the texture's colour as it is. VU1 program
  0x34F1C8 (flag 8 of the pass, 0x265500, uploads the world x view matrix): the view ray V (the vertex in view space)
  reflected on the normal N (rotated, not renormalised), R = (V - 2 (N.V) N) / |V|, u = 0.5 + 0.3394 Rx /
  sqrt(1 - Rz), v = 0.5 - 0.3394 Ry / sqrt(1 - Rz) (z into the screen, y up): a sphere map with the reflection back
  at the eye in the centre, up at the top, reaching 0.48 from the centre. The pictures (`cubemap`, the cities'
  `<City>_Cubemap`, 128 x 128, New York's 64 x 64) are mirror balls of radius 0.48, black and alpha 0 outside.

The world's renderer swaps in the block's own picture (0x2B92F0): its material filter RWorldMaterialFilter (vtable
0x44AAC0) gives a material named `BUILDING_CUBEMAP` shader 0x1B instead of the one 0x16F030 picks (+0x8, 0x2C2680),
and lists it and every `RCUBEMAP*` one (case-insensitive, +0xC, 0x2C26E0, into 0x4D2918); at the block's start the
LightingData key `EnvMap` (`CubeMap|EnvMap`: `<City>_Cubemap`, London `NewYork_Cubemap`) is loaded through the
texture manager (0x171C10) and set into texture slot 0 of `BUILDING_CUBEMAP` and slot 1 of the `RCUBEMAP*` ones
(0x260040, 0x2B96E4); +0x24 (0x2B9150) clears them at the end. Only the city bundles' materials go through the filter.
In the data: `BUILDING_CUBEMAP` (kind 2, its own texture `Concrete_Grey`) on `FOUNTAIN_Sculpture` (the fountain
sculpture: New York, Chicago, Hong Kong, London, Los Angeles, San Francisco), and `rcubemap_blimp` (kind 5) on the
New York blimp (`Blimp|Blimp`, its engines, `Blimp_Rotate`, `Blimp_Elevate`); `Rcube_tanker`, the trash cans and the
rest keep their `cubemap`. Shader 0x1B (record 27 at 0x3B5CC8) is one pass: slot 0 at the same sphere-map coordinates
(VU1 0x34FB78), TFX 1 DECAL, TCC 0, colour mode 0, state 1 (opaque, fogged, no alpha test): a chrome sculpture
mirroring the block's skyline. The port: render_gl.c `draw_part` (`cube_material`, `sphere_coords`: GL's sphere map,
whose eye space has z out of the screen, scaled 0.96 about the centre and turned upside down, which is the same
formula), city.c `lighting` (Scene.env_map); the reflection added by GL_DST_ALPHA [G: x taken as right in the
game's view space; on a frame without alpha bits the whole reflection is added].

Material flags (+0x4, u16): bit 0 two-sided (65 of 1,893: flags, girders, trees, cloth, teeth), bit 1 see-through
(fog masks, skylines, tree cards), 0xC the monsters' own. 0x100 (78 materials, all kind 2) is the texture region
(0x16F540 → 0x25F960, above): the 4 floats after the texture index, u0, v0, width, height in the texture, a tile of
a 4 × 4 atlas (`Taxi_normal` (0.5, 0, 0.25, 0.25) of `RampageVehicles_01`, `Taxi_Damaged` (0.75, 0, …); the
backdrops' `stucco`, `solid_wall_block`, `*_redbricksolid`, `*_windows` of their `*Base00` / `Window_sheet`), the
parts' UVs in units of the whole texture but within one tile's size: the taxi's run 0.035 … 0.239 by 0 … 0.249,
the backdrops' −0.83 … 0.83 (they tile). The region sits at +0x3A (kind 2), +0x3C (3, 4, 5, 7) or +0x3E (6, 8),
by 0x25F960's jump table 0x426940. **How the GS reads it** [R]: with a first texture (material +0x70), 0x25FC08 ..
0x25FE50 makes it a CLAMP_1 register at material +0x80: WMS = WMT = 3 (REGION_REPEAT); the size in texels
round(16 × width), down to a power of two (0x25FED0, 0x25FE80 halved; 0x10E0E0 the highest bit), × the texture's
width (+0x20) / 16, MINU = size − 1; MAXU = round(u0 × width) + size / 2 cut to a multiple of the size; the same in V
with height and the texture's +0x24; 10 bits each. The pass setup 0x265840 sends it for a pass on texture slot 0
(to 0x4CBED0; slot 1 or none: 0), and 0x2598A0 writes it over the texture's own CLAMP_1 (+0x90) in the packet. The GS
reads texel u at (u & MINU) | MAXU, so the UVs wrap within the tile, each bilinear tap too. All 78 are on 256 × 256
atlases and snap to quarter tiles (64 × 64: `chicago_windowBrick`'s 0.2549 starts at 64). The viewer
(src/render_gl.c `region_texture`, mesh.c `material_region`) cuts the tile into a texture of its own with GL_REPEAT
and scales the UVs by the texture matrix (texture size / tile size): the same texels at the same UVs. Its mip levels
are the tile's own [G: the GS's REGION_REPEAT on the smaller levels not followed; the viewer draws one level unless
mips= is on].

A mesh part's material name index does not always name a material: 3,768 of the 6,292 parts do, the other 2,524
name a texture entry (type 9) directly (people, the city backdrops, many props).

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

### The blocks behind
The game draws the next two blocks of `BlockData.BlockList` behind the one being played (the block manager
0x4D4E80, 0x302AF0): slot 0 the block at y 0, slot 1 the next at y + 41, slot 2 the one after at y + 82 (both
hard-coded). Slots 1 and 2 get their `Shape|GroundShape`, every `BuildingInstance` (0x3059D0) and only the objects
with `LoadWhenWaiting` (slot 1) or `LoadWhenDistant` (slot 2) set (0x3050F0; a `WaterObject` so marked is the flat
stand-in, WATER_NOTES.md "The blocks behind"), and their traffic; no people or AI.
Three block grounds (y -20 .. 21 each) fill the hole in the backdrop's ground plane exactly (`Chicago_Ground`: x
-108 .. 107, y -17 .. 98, the same in New York and Las Vegas). Every `BlockList` ends in two buffer blocks
(`*_BufferBlock01`, `02`), so the last block has two behind it too; entry 0 repeats entry 1.

The sky (`Shape|SkyBox`) is drawn first around the camera (0x2A53E0): scaled by 0.01 at the camera's (x, y, 0.99 z)
with the depth test off (GS TEST 0x31001) and no fog (PRIM.FGE cleared for its render state 0xB), a background
that hides nothing; the backdrop then at the origin, fogged like the world (FogR/G/B, FogStart / FogEnd, 0x2BFF60).

### Assembling a block (`tools/city.py`)
All seven cities export (Chicago, New York, San Francisco, Los Angeles, Las Vegas, London, Hong Kong: one glTF per
block, every building and object model found). Game units, Z up, the street towards −Y; one cell is 5 × 5:
- the block's ground shape, the city's day backdrop (`*_Backdrop`) and sky (`*_Skydome_Main`) at the origin;
- a building's centre is at x = `LeftEdgeXPos` + width × 2.5, y = 5 + (`Row` ? `BackRowOffset` : 0) (0x305E78 …
  0x305F08): the front row's fronts at y 0, behind the street's far sidewalk (y −5 … 0; the road is y −15 … −5);
- the ground-floor shape (`ShapeSet|GroundFloorShapes`, 6 high) at z 0, then a storey 3 high per `Floor|` entry
  with tiles (the template name is width × storeys: `CG2_2x7` has 7 such entries and an empty eighth, and the
  game's grid counts storeys as entries − 1), then the top shape (`ShapeSet|TopFloorShapes`, 1.5 high: the roof's
  band of the grid's box) at 6 + 3 × storeys;
- upper storeys have no mesh: the facade is built from the `TextureSheet`, a 4 × 4 grid of tiles (column =
  window style, row = damage state, 0 intact). Each `Floor|` lists width + 2 × depth tile columns: left side
  (back to front), front (left to right), right side (front to back); the back is never seen. A floor's
  `Skeleton` (`Damaged10x20_00.pwk`) is the interior shown once the building is damaged;
- objects (`ObjectInstance|`, `AttackObjectInstance|`, ...) at their `Location`, turned by `ZRotation` degrees;
  objects inside a `BuildingInstance|` are relative to the building. `Name` is a template in the
  `ObjectTemplateBook` (`Objects1_Props`), whose `Shape|Shape` names the model; templates inherit through
  `ParentPropertySet = "Group:Book"`.

Models with a node tree are placed node by node (see "Node-tree models"); one whose RawAnim moves (the
helicopter's rotors, the fountain) gets it as a looping clip `<model> loop`, which the viewer plays. The sky's
cloud band does not move in its RawAnim (29 identical segments); its drift must come from elsewhere.
`tools/anim.py VOLDIR --models CITY OUTDIR` exports every animated tree model of a city (people, vehicles,
bosses) with all its clips to `OUTDIR/CITY/models`.

### Damage
A building is a grid of cells (the Building's grid at +0x1B8, built by 0x2D1A30): its box is the template's width × 5
by depth × 5 (depth is 2 in every city) and 6 + 3 × storeys + 1.5 high (the last 1.5 is the roof's band), and every
storey above the ground floor has a cell per facade column: the left side (depth columns, back to front), the right
side (front to back) and the front (width columns), 16 bytes each: damage state 0 … 3 (the `TextureSheet` row), HP
(10 at the start, 0x2CBEB4), the sheet's column (from the `Floor|` list), a tint and flags (+0xE: `FloorFlags`, read
by 0x2CBB50 at 0x2CC014 in the list's order, left, front, right, as the columns; 1 mirrored, 0x400 hidden behind a
front-row building, 0x800 hits ignored, 0x100 / 0x200 its `Openable` window `LRPos` 0 / 1 is open: see "Open
windows"). The books' FloorFlags are all 0 but 22 cells of New York with 1 (`ConcretePink` and `ConcretePinkShort`,
NY05 and the buffer block, `Broadway_3x9`, `Empire_3x18`): the front's right column, a side's front cell [R]. Each floor holds `FloorHP` (8 ×
width unless set), and the building `BuildingHP` (6 × width × storeys unless set). Script bindings read and set them
(`getCellDamage`, `setCellDamage(face, storey, column, state)`, `getDestructionFrac` = HP left / HP at the start,
`disableAutoCollapse`, ...; 0x2ADF00 → 0x2AD610). `setCellDamage` (0x2BB580 → 0x2CFDA0) writes only that cell's state (+0x0; the sheet column +5 when given): its HP stays, no side cell follows [R].

A monster damages a building through its clips' script calls, not its attack spheres: `Monsters.bsf`'s
`hitBuildingFront(kind, x, y, z)` (George's street punches `(2, 0.5, 6, 8.5)`: 6 ahead and 8.5 up, the first
storey) picks the face from `kind` (0, 1, 3, 4 by the mirror state; 2, 5 the front; 6, 7 by the way the monster
faces) and calls the monster's `hitBuildingPt(point, side, 1, 2.0)` (twice in the rage, Monsters.bsf). That turns
the point into the world with the monster's matrix (0x2B3A80) or, on a face (mode 5), adds it to the monster's place
unturned (0x2B3AC4 .. 0x2B3B08: a side's punch, kind 2, lands on the front's corner column), finds the building whose face the side looks at (0x2A3D20 on the world at 0x4D0900;
side 0 its left face, 1 its right, 2 its front, 5 its roof), moves the point onto that face and, if it is within 5
units of where it was, hits the building (vtable +0x34, 0x2BC150) with radius 2.0:

1. Only a standing building (+0x14C = 1) takes hits. The point goes into the box and onto the nearest of roof, front,
   left or right (0x2BB7B0, never the back), then to a face, storey and column (0x2CCCA0): below z 6.01 the ground
   floor, in the roof's band the roof; storey = (z − 6) / 3, column = the 5-unit slot along the face.
2. Every hit on a facade (any of the three faces, whatever its radius) dents its vertices: 3 × round(radius / 2.5)
   times 0x2CC900 raises a vertex level (0 … 3) by 1, at the cell's corner nearest the hit or the first vertex of
   its rings below the ring's cap (rings 0x4310D0 of 1, 8 and 12, (du, row) pairs at 0x4310E0, capped 3, 2, 1,
   starting at a random entry), the faces unwrapped into one row of columns (left 0 … 2, front from 2, right from
   W + 2: the depth taken as 2). A front corner raises its side twin too. 0x2CEAD0 sets a level: a new dent moves
   the vertex 0.5 × level × (0.2 … 0.3) in a random direction in the face's plane (+0x14 along, +0x18 up), a deeper
   one further the same way; the bottom and top rows move only along, the corners only up and together with the
   side's, nothing moves along the normal (+0x1C stays 0). The level is +0x0's alpha (× 85), which fades in the
   crack pass. The cracking (state 4) sets front vertices straight to 1 … 3, a floor's collapse its rows on every
   face to 3.
3. With a radius of 2 or more the front cell takes 2 HP (0x2BB950; a side's front column counts as the front's end
   cell, the other side cells and the roof take none). The HP comes off the floor's and the building's pools too:
   when the building's runs out it falls (state 4), when the floor's does every cell of that storey goes to state 3
   and the floor collapses (0x2BAE70). Otherwise the cell's state follows its HP: any damage → 1 (cracked, glass or
   wall debris), under 5 → 2, 0 → 3 (heavy debris). Crossing below 7 HP the hit carries 3 HP to the cell above (hit
   in its upper half) or below; crossing below 3 HP, 3 HP to a living neighbour left or right at random
   (Park–Miller, 0x106BD0). So five punches on one cell: 10 → 8 (cracked) → 6 (the cell above takes 3) → 4 (broken)
   → 2 (a neighbour takes 3) → 0 (destroyed).
4. The facade shows each cell's state as its tile's row; a floor with any damaged cell shows its `Skeleton`, the
   interior, and its `DamagedSkeleton` once `DamagedSkeletonHP` (half of `FloorHP`) is passed (around 0x2CD898).
   The interiors are prelit: their sections are format 0x142 (colour, no normals; grey shading baked in) and are drawn
   with no light set; nothing changes their colours with damage or storey. The facade itself is lit on the CPU, a
   colour per grid vertex times the storey's `FloorTintR/G/B` (the cells' tint): see
   [BUILDING_LIGHT_NOTES.md](BUILDING_LIGHT_NOTES.md).

**The roof stomp** [R]. `Monsters.bsf`'s `damageRoof` ignores its four arguments and calls
`monster.hitBuildingRoof("Bip01 R Foot", 1, 2.0)` (the monsters with a `damageRoof` of their own, Cal, Eyegore, Jill,
Natalie and Venus, pass `"Bip01 Spine"`). George's graph fires it in `Ground_Punch_Down` (Square with the stick down,
at 0.328 of the clip), `Jump_Finish` (a jump's landing), `Jump_Fall_Finish` / `Jump_Fall_Finish2` (twice) and
`Jump_Butt_Slam_impact` (six times); the debris effect and the camera shake beside them are clip events of their own.
`hitBuildingRoof` is Monster method 0x6F (descriptor 0x42B9C0, the lookup's strcmp at 0x2ACE90; the dispatcher's case
0x2AB620 via the jump table 0x42CEE0):
1. the bone's place in the world: its index by name (vtable +0x48, 0x2E54A0) and its matrix (+0x4C, 0x2E4FF0; the
   translation at +0x24), from the pose as played, mirrored or not (no left/right swap);
2. the building under the monster's body (0x2D6150, as on the ground: the one at its probe point 2.5 ahead, whatever
   its height, unless its top is more than 5 above the feet); none, or a dead monster: 0, nothing happens;
3. 0x2BBE80(building, monster, point, 1, 2.0), only for a standing building (+0x14C = 1):
   - the roof (face 5, a single state at G+0x658, 0x2CF7C0 / 0x2BB580 → 0x2CFDA0) one state worse, up to 3. The roof
     is drawn (0x2CD5E0) as the `ShapeSet|TopFloorShapes` model of its state (G+0x65C + 4 × state, filled by 0x28D890
     in list order; a missing one: the nearest lower state's). Every city lists two (`Bank00_Top10.pwk`,
     `Bank00_TopDmg10.pwk`), so the first stomp shows the damaged roof for good;
   - the column under the point, (x − min x) × 0.2 truncated; on the top storey (G+0x18 − 1) the nearest front cell
     not destroyed (state < 3): searching left from that column and right from the next one, the left when it is
     strictly nearer, else the right; none: 0;
   - Building::hit (0x2BC150) of kind 1 with radius 2.0 at that cell's middle (x = min x + 5 × column + 2.5), on the
     front (y = min y), 0.3 below the top storey's top (z = 6 + 3 × storeys − 0.3): the dents, 2 HP off the cell and
     the spreading, as a punch.
The script ignores the answer: no energy (hitBuildingFront's +2 is the script's own) and no rage; the points are
Building::hit's own (its `Scoring`, paid to the hitter it keeps at +0x190). Because only the top storey's front
cells are hit, stomping drops the top storey once its `FloorHP` is gone (8 × width: about 11 stomps on a 3-wide
building), the roof model comes down a storey with the monster on it (the roof is the building's top, which drops; the
monster lands, `Jump_Finish`, whose own `damageRoof` hits the new top storey), and so on until the BuildingHP runs out
and the building falls (state 4). There is no separate cave-in through the roof.

### Collapse
The building's update (vtable +0x10, 0x2BDC70) runs a falling storey and then its state (jump table 0x42FA00).
`DefBuildingProps` gives the times: `CollapseTime` 0.25, `CrackSpreadTime` 0.05, `HesitationTime` 0.4,
`MinCollapseCrackLevel` 1, a `ShakerDef` and the debris effects (`Floor_Large` `Collapse_Floor_brnbrick_comp.edf`,
`Glass_*` `Window_Shatter.edf`, `Wall_*` `Debris_concrete_big.edf`); each template its `Shape|RubbleShape`
(`GENRUBBLE_10X10` … `10X20`, by footprint).

- **A floor falls** (its FloorHP gone, 0x2BAE70 with k = storey + 1; the building is in state 3 meanwhile and takes no
  hits; only one at a time): its cells go to state 3 and its vertex rows to level 3, and over `CollapseTime` the
  storeys above come down onto it, 3 units (one storey) in all, squashing it (0x2D2D00, 0x2D2A40: the collapsing
  storey G+0x1CC, the progress 0 … 1 at G+0x1D0; G+0x1CC = INT_MAX when none). Then it is gone: the storeys above
  take its place (the storey table G+0x48 shifts down), the building is a storey lower, and the one below is
  crushed (0x2BD3B0): each front cell under it loses the weight table's HP for the cell that came down onto it
  (0x3F5710, by that cell's HP 0 … 10: 0, 0, 1, 1, 2, 2, 2, 6, 6, 6, 6; 2 if the top storey fell), and the cells that
  came down with 5 HP or more lose 2, which may start the next collapse. Scoring `floor.Base` (50).
- **The building is destroyed** (its BuildingHP gone, the `collapseBuilding` binding 0x2BB8B0, or a hit of kind 8;
  scoring `Building.Base`, 100): state 4, the front cracks bottom up, a vertex every `CrackSpreadTime` (0x2BD7D0:
  the lowest row with an intact vertex, from a random column, to level 1 … 3, and the cells beside it to at least
  `MinCollapseCrackLevel`, now and then glass or wall debris); then state 5, it shakes for 2 s
  (`Building_Pre_Collapse.edf`, a rumble for the monsters on it); then state 6 (0x2BD610): every `HesitationTime`
  less up to half of it at random, its lowest storey falls as above (without the crush), until only the ground
  floor is left, which sinks 7 units into the ground over 7/3 × `CollapseTime` while the rubble shows; then only
  the rubble is drawn (0x2CD394) and the building is inert (state 0).
- **The shaker** (+0x8AC, from `ShakerDef`: `MasterAmp` 0.05, `ShakeLength` 0.5, and (period, phase, amp) triples
  (0.05, 1, 3), (0.25, 2, 0.25), (0.33, 3, 0.25), (0.61, 4, 0.25)) sways the building along x: MasterAmp × the sum
  of amp × a parabolic sine of (t + phase) / period (0x2CC450), fading over the last fifth. Every cell hit and every
  frame of a falling storey makes it shake on for ShakeLength × (1 + a random share) from now (0x2CC5E0: a shake
  still going has its length, +0x44, cut to the time it has shaken, clock − +0x40, before that is added; the hits do
  not add up, so a charge punch's many cells leave 0.5 .. 1 s of it, as any one hit). A monster climbing it
  (+0xC640) or, on the ground (+0xAD8 0), standing on it (0x2D6150) is drawn moved by the same offset while it
  shakes (0x2D6680, from the monster's draw 0x2FEFA0): added to the place of the matrix its render instance is drawn
  with and given to its hair as the draw offset (ClothInstance +0x90, 0x1D6780; 0 otherwise); its own matrix, its
  bones and its hair's simulation stay put [R] (city.c `monster_sway`).

`CellDetachDamage` and `VertDetachDamage` belong to a building's attachments (AC units, fire escapes; 0x2C617C), not
to the grid.

### Adornments
A template's and a building instance's `Adornment|` entries (0x2D1A30, the template's first) are the fire escapes,
AC units, awnings, roof trims and stone heads hung on a building: a record of 0x60 bytes each at building +0x250
(count +0x258), filled by 0x2C6120 from the entry and its `TemplateName` template (`Shape|Shape` from the template;
`AdornmentFlags`, `CellDetachDamage`, `VertDetachDamage`, `DislodgedFx` from the entry, else the template). Keys
match in any case (0x1B4490), as 22 entries spell `Cell` and `Floor`.

- **Where** (0x30FAA0, 0x30F710, 0x2CF2F0): on a side (`side` 0 left, 1 right, 2 front), at storey `floor`, `cell`
  (5 wide, the cells' order), `u` along the face from the cell's edge (left to right seen from outside), `v` up from
  the storey's floor; the model's origin on the face plane, turned so its +x runs along the face and +y goes into
  the building (rows at 0x430F10). `side` 4 is the ground floor (v above the base), 5 the roof (1.5 + v above the
  roof band's foot); they are placed by `LocationOrigin`: 0 the left face from its back, 1 the right face from its
  front, 2 the front from its left (v still adds to the height), 4 or 5 (u, v) from the building's middle.
- **Height each frame** (0x30F690, 0x2CD010): 6 + 3 × the slot of its storey, or of the first one above it still
  standing (the roof band's at the top), + its offset; lower by 3 × the falling storey's progress when above it.
- **Falling off**: setting a cell's state (0x2CFDA0) drops the adornments on that cell once the state reaches their
  `CellDetachDamage` (0x2CFC20; the structural ones, flag 4, only once the building's HP is gone), so a collapsing
  storey sheds its own; an attack sphere reaching one at 0.75 of its radius knocks it off (0x2D4390; not flag 4 nor
  2) and spawns its `DislodgedFx` at the cell's middle. `VertDetachDamage` is never read.
- **Flags**: 2 an obstacle for the climbing (−2) and safe from normal attacks, 4 structural, 8 detaching, 0x10 gone.
  The data has only 0 and 4. `DistFadeLevel` (default 2) only keeps an entry for grids of a lower detail level.

### Facade geometry
The grid G builds the walls itself: the grid builder 0x2D1A30 sets up a mesh writer per pass and face (G+0x1FC + pass
× 0x174 + face × 0x7C, 0x2D32D0). The builders sit in two 9-entry tables of member-function pointers, [pass][face]
with left, right, front: 0x3F5818 and 0x3F58A8. Its sheet properties (0x2D1580, names at 0x3F5A2C) are
`ShellTexture` (+0xB0, the 4 × 4 facade sheet), `CrackTexture` (+0xB4), 1 / `CrackScale` (+0xC0), `CrackTint`
(+0xC4 …), `InnerWallColumn` (+0xD4), `RefTexWidth` / 4 and `RefTexHeight` / 4 (+0xDC, +0xE0: a tile's pixels), and
the `ColumnDef|` entries (G+0xEC + column × 0x38, 0x2D0FB0).

- **No back.** The cells and the grid vertices run left (back to front), right (front to back), then front. Nothing
  builds a back wall, a roof or floor slabs. The roof is the `TopFloorShapes` model and the inside is the floors'
  `Skeleton` models. The game's camera stays in front.
- **Pass 1, the facade** (front 0x2C81A0, right 0x2C8760, left 0x2C8D30): one quad per cell in every damage state, a
  sheet of no thickness.
  - u is the cell's column / 4, u0 and u1 swapped for a mirrored cell (flag bit 0: 0x2C82AC, 0x2C886C, 0x2C8E14); v
    runs from state / 4 at the top to (state + 1) / 4 at the bottom.
  - The corners take the dent offsets of their grid vertices: +0x14 along the face, +0x18 up, and on the sides +0x1C
    along the normal.
  - The colour is the vertex's times the cell's tint (u16, 256 = 1).
  - A vertex is emitted at z + 6 + 3 × row (0x2C6790), and each quad is a strip TL, BL, TR, BR.
- **Inner corner walls** (0x2C63A0, before the front): while the first or the last front cell is damaged, a piece 5 deep
  just inside the side wall (0.3 in), textured from column `InnerWallColumn` at the side cell's state, grey.
- **Pass 0, window glass** (front 0x2C92F0): only for cells in state 0 whose column has an `Outline`; it goes once the
  cell is damaged. A mirrored cell's outline is at 5 − x (x0' = 5 − x1, x1' = 5 − x0: 0x2C9470 and the other faces'
  0x2C9910, 0x2C9DB4, 0x2CA278, 0x2CA960, 0x2CAD48); the cracks' pass (0x2C7A90) reads no flag, so their window
  hole stays where the column has it.
  - The quad is the outline grown by 0.3 on each side, 0.03 inside the face, tinted `CubeTint`.
  - It shows one of 16 tiles of `InnerWindow` (the cell's byte +6), or, with no inner window texture, a reflection
    (the 0x3F5818 variant, from the vertex normals). Byte +6 is rand() & 15 (0x2CBAD8) of a Park-Miller state the
    grids keep for themselves (gp − 0x6CC0, which also jitters each grid vertex's normal by two draws × 0.02,
    0x2CB86C), seeded once, at the first grid made, from the EE timer's count (0x2CBB74: the flag gp − 0x7240, then
    0x106C80) [R]; so which tile a window shows is hardware timing [G in city.c: a hash].
  - An `Outline` (x0, z0, x1, z1 in tile pixels) becomes cell units: x = (o & (Wt − 1)) × 5 / Wt and z = (Ht − o) × 3 / Ht.
  - The pass does not look at the open-window flags: an open window keeps its glass, and is covered from the front
    (see "Open windows").
- **Pass 2, cracks** (front 0x2C6A30, right 0x2C7340, left 0x2C7A90; emit 0x2C7020): `CrackTexture` just inside the
  face (0.015: y = 0.015 − D / 2 on the front, 0x2C6A3C), faded in by the dent level.
  - The materials (0x2D0AB0): the glass +0x1F0 shader 0x51 (`InnerWindowTexture`) or 0x22 (the reflection), the
    facade +0x1F4 shader 0x7C (`ShellTexture`), the cracks +0x1F8 shader 0x4C; 0x7C and 0x4C are one pass in GS state
    2 (ALPHA 0x44, TEST 0x5000D: the alpha test GREATER 0, Z test GEQUAL, Z written; PRMODE 0x78), the glass state 1
    (opaque) [R]. With the passes drawn in order the cracks would be behind the facade's depth; how they show over it
    [G: the VU1 programs' depth or the draw order outside the grid; city.c draws them just in front].
  - The fade is the grid vertex's alpha, level × 85; a cell with no level at its corners is skipped.
  - Below state 2 a column with an `Outline` gets 4 bands around the window, otherwise one quad.
  - The texture runs on across the unwrapped faces at 1 / `CrackScale` per unit.
- **Which passes**: all three; a low-detail grid (G+0x1C) only the facade, or the facade and glass when it has an
  inner window texture.

### Open windows
A window occupant opens a window; nothing else does (no script calls `openWindow`, and the edibles' slots never open
one: an edible stays behind the glass until its cell breaks). [R]
- **Which window.** A sheet's `ColumnDef|` has, besides its `Outline|`, one or two `Openable|` entries (0x2D0FB0, names
  at 0x3F5AB4 …): `LRPos` (0 or 1, which of the two), `OpensH` (1: it opens sideways, a casement; 0: upwards, a lower
  sash) and four numbers in tile pixels, left, bottom, right, top, made cell units as the `Outline` (x0 = ((p0 & (Wt −
  1)) + 1) × 5 / Wt, z0 = (Ht − (p1 & (Ht − 1))) × 3 / Ht, x1 = (p2 & (Wt − 1)) × 5 / Wt, z1 = (Ht − 1 − (p3 & (Ht −
  1))) × 3 / Ht), kept per `LRPos` at ColumnDef +0x14 (there), +0x16 (`OpensH`), +0x18 (the rect). It is usually the
  lower part of the outline (Chicago's `BrownBrick_Sheet` column 0: outline pixels 45, 102, 78, 31, openable 45, 102,
  78, 74). 6 of the 183 column definitions have none, 39 have two; LRPos 1 is often the right half with `OpensH` 1.
- **The building's list** (+0xFC, capacity +0x100, count +0x104), 0x24 bytes a window: +0x0 column, +0x4 storey (the
  storey table's id, +0x200[storey], so it survives storeys falling), +0x8 `LRPos`, +0x9 `OpensH`, +0xC its left
  edge in the world (grid min x + 5 × column + x0), +0x10 width, +0x14 its bottom above the storey's floor (z0),
  +0x18 height, +0x1C how open (0 … 1), +0x20 its rate per second.
- **`openWindow(storey, column, LRPos, open, seconds)`** (Building binding 0x38 → 0x2BE8B0, from 0x2ADAE8): a window
  in the list just runs to open or shut, `rate` = ±1 / seconds (seconds ≤ 0.001: `open` = 1 or 0 at once, rate 0). A new
  one only to open, and only while the front cell is intact (state 0, 0x2CF7C0) and the column has that `Openable`
  (0x2CFF30): appended shut with rate 1 / seconds (open at once for ≤ 0.001), the cell's flag 0x100 << LRPos set
  (0x2CF980; only flag bits 0 … 1 make the grid's meshes again, so this changes no mesh), and the building put on the
  active list (0x4D17F0 / 0x4D17F4, its flag 0x200). `isWindowOpen(storey, column, LRPos)` (0x39, 0x2BE7C0): in the
  list and `open` ≥ 1. `doesWindowOpen` (0x33, 0x2BE8A0 → 0x2D0460) and `getWindowPosition(storey, column,
  LRPos)` (0x3A, 0x2BE660: x = grid min x + 5 × column + (x0 + x1) / 2, y the front's (+0x1E0), z = the storey's
  height (0x2CD010) + z0: the openable's bottom middle) are the rest.
- **The update** (0x2BDC70, from 0x2BDDD0): every window with a rate moves `open` by rate × the frame time (0x4D17A4);
  opening it stops at 1 (rate 0); shutting, at 0 or below it leaves the list (the last one copied over it) and its
  cell's flag goes (0x2CF890).
- **Gone at once**: a front cell set to a damaged state (0x2BB580, the setter under 0x2BB410 / 0x2BD7D0 and
  `setCellDamage`) drops both its windows and their flags before the state changes; a storey that falls (0x2BAE70)
  drops its windows (the flags stay, the cells are at state 3 anyway).
- **Drawn** (0x2BECC0, per building from 0x320280): the first 40 windows of the list, each a flat quad 0.015 in front
  of the facade (y = +0x1E0 − 0.5 × 0.03 (0x430F80)), in a material of its own (0x25C4F0 shader 0x11 on 0x4CB480, no
  texture, colour 0xFF101010: near black, opaque), corners (x, z0), (x1, z0), (x1, z1), (x, z1) from the bottom left:
  with `OpensH` x1 = x + open × width and z1 = z0 + height (it opens from the left), else x1 = x + width and z1 = z0 +
  open × height (it opens upwards); z0 = the storey's height now + the bottom. The occupant stands at the front's y,
  so what of it is behind that plane is hidden and the rest leans out of the dark.
- **Who opens and shuts** (the window occupants, 0x4D5F00; docs/PEOPLE_NOTES.md "Window occupants"): the spawn
  (0x318B04) opens the slot's window over 0.5 s as it places the occupant; the actor binding `closeWindow` (0x30C3F0)
  shuts it over 0.5 s (openWindow(…, 0, 0.5) on the slot's storey, column and `LRPos`), which `Person_Window` and
  `Military_Window` call as their `WindowReEnter` ends. `leaveBuilding` (0x30C3D0 → 0x318140 with 0), a grab
  (`grabBuildingOccupant` 0x2B3350 → 0x318140 with 1), the fall (0x30C510) and the actor's end (0x30DF80) only free the
  slot: the window stays open (until its cell is damaged, or a new occupant of that slot shuts it).

The port: `building_window_open` / `building_window_is_open` and the list in building.c (by template storey, the rect
in cell units), the `Openable`s in city.c's `sheet_columns`, the drawing as part 4 of the facade mesh (city.c
`open_windows`, every frame: in the facade's frame, so it sways with it; the game's 0x2BECC0 takes the per-building
vector the facade draws get too (0x320280), which holds its shake (0x2BAD50) [R]),
and `scene_window_close` / `scene_window_close_at` for the occupants' `closeWindow`. A mirrored cell (flag bit 0)
mirrors the slot's `Openable` rect (0x2CFF30 -> 0x2CC420: x0' = 5 − x1, x1' = 5 − x0), for the occupant's place and
its window [R]: city.c `cell_rect` (the facade's u and the glass too).

### Spawns
A block definition also says who is in the street (names found through the property name table at 0x3F5990;
the block loader is 0x302AF0):

| Group | | Read by |
|---|---|---|
| `ActorInstance\|SP_n` (typed) or `ActorInstance\|` | a person at `Location` (template `Name`). `SpawnsItem0 = "SP_m"` names the one that takes its place when it is gone; those have `SpawnedItem = 1` and are not there at the start | 0x304450 |
| `TrafficDef` | `MaxVehicles` (7 if absent), `MinReleaseTime`, `NoFrontTraffic`, `NoRearTraffic`, and one `VehicleDef\|` per vehicle type with `Frequency` and `OTRef\|Name` (template) | 0x334240 into the AI manager at 0x4D7800 (`gAIMgr`) |
| `AIDef` | `AIInstance\|`: attackers (tanks, SWAT, helicopters) with `MinDelay`, `OffsetDelay`, `RespawnDelay`, `MaxOccurances`, `FriendChance` | 0x3350F0 |
| `Occupants` | `FillRate`, `NonMilitaryRate`, `OccupantInstance\|` (template, `Frequency`): the people who appear in windows | 0x3196F0 |

An actor template (in `Objects1_Props`) gives its states as `ActorAnimShape|<state>` groups, each with the model
(`Shape|ShapeName`, else the template's own `Shape|ShapeName`) and the clip (`Anim|AnimName`, a keyframed
animation of that model's node tree); `DefaultAnim` names the state it starts in (`Idle`, `Walk`, `WindowEmerge`).

Traffic (0x332D90, called from the AI manager's update 0x3333F0): a vehicle drives a straight line at 15 units/s,
either from x = 75 to −75 at y = 41 × slot − 7.5 (the rear lane, turned +90°) or from −75 to 75 at
y = 41 × slot − 12.5 (the front lane, turned −90°); slot is the block's place among the loaded blocks.
`NoFrontTraffic` / `NoRearTraffic` switch a lane off; two release timers start at 2 + 3 × rand seconds (probably
one per lane), and the type is picked at random by `Frequency`. `Vehicle.bsf` takes a vehicle off at the end of its line.
`Traffic.bsf` adds two crowds when a block becomes active: 4–8 people of the block's occupant types
(`<Occupant>Runner`) around (−14, 0.5) and (8, −3), running 50 units towards −y, and plays `Crowd_Panic` at the
block's origin (a `SoundEffect`, FORMAT_SOUND.md). Only once a world (`bCreateCrowd`, set by its `onInit`): on the
first block with occupants whose `CreateCrowd` (1 unless set) is not 0; `HK01_WaterfrontA`, `HK02_WaterfrontB` and
`SF10_Presidio` set it to 0.

`tools/city.py` places the actors that are there from the start, each looping the clip of its default state, and
a looping traffic clip: per open lane, round(10 s / 3.5 s) = 3 vehicles (at most `MaxVehicles` / 2) spread evenly
along the line, their types drawn by `Frequency` with a fixed seed per block. All 7 cities: 379 people, 375
vehicles, every template resolved. The traffic is a fair sample of what the game spawns, not a recording of it.

### Lighting
`WorldData.LightingData` (day) and `LightingDataNight` (read at 0x2A5750). The fog, light files and model naming
below are the same in all seven cities; Las Vegas has no `AnimatingSkyData`:

| Key | Day | Night | |
|---|---|---|---|
| `Shape\|Backdrop`, `Shape\|SkyBox` | `Chicago_Backdrop`, `Chicago_Skydome_Main` | `..._Backdrop_Night`, `..._Skydome_Night` | the skyline plane and the sky around the block |
| `text\|LightFile` | `default_lights.txt` | `default_night_lights.txt` | in `common.bdp` (type 2) |
| `FogR/G/B`, `FogStart`, `FogEnd` | 120, 152, 226; −2 … 200.5 | 50, 37, 122; 5 … 200.5 | |
| `AnimatingSkyData` | `MaterialName` (the cloud band), `UAnimRate` 0.009, `VAnimRate` 0 | – | scrolls the material's texture; given to a material animator |
| `CubeMap\|EnvMap`, `CubeTintR/G/B` | `Chicago_Cubemap`, 192 | 255 | the cube-map materials' sphere map ("The reflections"); the window glass's tint |
| `GroundShadowIntensity`, `MonsterShadowIntensity`, `MShadow*`, `TShadow*`, `Texture\|MonsterBlob` | | | shadows ([SHADOW_NOTES.md](SHADOW_NOTES.md)) |
| `RampStart`, `RampEnd`, `RampIntensity` | 6, 35, 0.35 | 6, 35, −0.5 | the buildings' light brightens (night: darkens) with height ([BUILDING_LIGHT_NOTES.md](BUILDING_LIGHT_NOTES.md)) |

A light file is text: a `BEGIN_AMBIENT` block (`RED`, `GREEN`, `BLUE`) and `BEGIN_LIGHT` blocks (`LIGHT_TYPE
DIRECTIONAL`, `DIFFUSE_R/G/B`, `DIRECTION_X/Y/Z`: the way the light travels, in game space), `//` comments.
Day: ambient 0.1 grey, a white key light (0.85) from above and in front (0, 0.5, −1), a warm orange one (0.65,
0.35, 0.1) from the right (−1, 0.25, 0) and a blue fill (0.2, 0.2, 0.4) from the left (1, 0, 0). Only 21 sections
of city meshes carry vertex colours (the damaged interiors), so the city is lit by these at run time; the buildings'
facades are lit by them on the CPU, with the height ramp (`Ramp*`) and the neighbours' shadows taken off
([BUILDING_LIGHT_NOTES.md](BUILDING_LIGHT_NOTES.md)).

The light file is parsed by 0x16E6B0 (keywords compared after `toupper`, numbers read by 0x106DF0 as doubles and
narrowed with `dptofp` 0x1450A8, which `recomp/config.toml` lists as a stub with no runtime handler, so the
game's own code runs). For the recompiled game's black ground (docs/NOTES.md): if the lights reach VU1 intact,
its light block should hold ambient 0.1, 0.1, 0.1 and the three colours 0.85 / (0.65, 0.35, 0.1) / (0.2, 0.2,
0.4) with directions (0, 0.5, −1), (−1, 0.25, 0), (1, 0, 0) normalised (Las Vegas uses the same day file).

`tools/city.py` (and `--night`) writes the lights as `KHR_lights_punctual` directional lights (intensity × π, so
that a surface facing a light gets colour × texture), the ambient colour and the fog as scene extras, and the cloud
band's rate as material extras (`uvRate`). The rate is taken as texture widths per second (one drift across in
about 110 s); the animator that applies it is not decoded, so per second is an assumption. `FogStart` and
`FogEnd` are not distances but world y [R]: each frame the camera manager (RCameraMgr 0x2BFDA0) makes each the eye
depth of the street point (camera x, y, 0), forward.y × (y − camera y) − forward.z × camera z, and the VU1 programs
fog linearly between those depths (0x25A510: F = 255 (far − w) / (far − near), w the eye-plane depth; 0x37A2A0). So
the street, the monsters and the fronts get no fog, the blocks behind and the backdrop do (`render_gl.c` rnd_frame;
a glTF viewer's distance fog from 5 to 200.5 is far too strong).

Not reproduced yet: the facade layout is inferred (it looks right in all cities, not checked against the game),
shadows and reflections, the timed and random spawns (attackers, window occupants, crowds), and the
people's walking routes (`WalkingAroundPerson.bsf`).
