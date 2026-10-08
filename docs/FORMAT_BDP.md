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
| 0 | 5,547 | sub-mesh (`MESH\|material`) | header / geometry |
| 2 | 327 | raw file: `.bsf` scripts (269, all identical to the ones in `RAMPAGE.VOL`), `.ifl`, `.fdf` (font metrics), `MONSTER_DATA` | file / – |
| 3 | 615 | skeleton | |
| 4 | 5,379 | animation | |
| 6 | 1,893 | material | |
| 9 | 1,428 | texture | header / pixels (see below) |
| 13 | 1,275 | palette (`.pal`) | header / 256 colours |
| 16 | 2,119 | `.pwk` | |
| 17 | 30 | skinned mesh section | header / geometry |
| 20 | 297 | GUI image description (text) | |
| 22 | 28 | properties (`.prb`: `camera.prb`, `<City>_Props.prb`) | |
| 23 | 13 | localised text (`.loc`) | |
| 24 | 43 | GUI layout (`.ifc`) | |
| 25 | 44 | cloth | |

Types 0, 3, 4, 6, 16, 17, 20, 22–25 are named from their entry names and sizes; their contents are not decoded yet.

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
