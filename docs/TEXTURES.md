# Texture packs (port extra)

The original has no way to replace its textures; the port lets any texture it uploads be swapped for a PNG of any
size, e.g. for HD packs. Code: `src/texpack.c` (WoodyRE's, with stb_image / stb_image_write in `src/stb/`), hooked
into the places that upload the bundles' textures, `texture()` and `region_texture()` in render_gl.c. That covers the world, the
buildings and their facade sheets, the damage layers, the sphere-map environment textures, water, shadows, particles,
the monsters, the HUD and the front end's interfaces. The movie frames (`video_texture`) stay out.

## 1. Names
A texture is known by a 64-bit FNV-1a hash of a 9-byte header (tag `'T'`, width, height, 32-bit little endian each)
and the RGBA that `tex_decode` makes of its mip 0. The palette is part of that picture and the PSMT8 swizzle and the
CSM1 palette order are already undone, so the same texture in two bundles or two cities has one name, and a texture
drawn with two palettes has two. Every frame of an animated texture (`.ifl`) is its own texture.

File names are `<anything>_<16 hex digits>.png` (or just `<16 hex digits>.png`); only the hex part counts, so a pack
may rename the dump's `256x256_2e86beb7144c05c1.png` to `storefronts_2e86beb7144c05c1.png`.

## 2. Folders
Both live in `mods/` in the current directory, next to `rampage_profile.bin`:
- `mods/textures/` - the replacements, any subfolders (8 deep), read once at the first texture upload. When two files
  carry the same hash, the first one found wins. The console prints `texture pack: N replacement textures`.
- `mods/dump/<scope>/` - written by `--dumptex`: every texture as it is first drawn, as `<w>x<h>_<hash>.png`, exactly
  as the game decodes it (RGBA, alpha 0x80 already 255). A texture is written once: the hashes already anywhere in
  `mods/dump/` are not written again, so it lands in the folder of the first bundle that drew it. Only what is drawn
  is dumped (textures are uploaded when first seen); play through a block, or several, to collect more.

The scope is the bundle's (`Bundle.scope`, set where it is loaded):

| Scope | Bundles |
|---|---|
| the city (`Chicago`, `NewYork`, ...) | `<City>.bdp` and the books it loads (`<City>_Props`, the template books, `camera`) |
| `Monsters` | `<Monster>.cmp` and `<Monster>_anims.cmp` |
| `GUI` | the HUD's `WorldGui.bdp` and `CommonGUI.bdp` |
| `fx` | `fx.bdp`: particles, debris and the shadows |
| `Shell` | the front end's bundles (`Shell.bdp`, `VShell.bdp`, `ShellData.bdp`, `CommonGUI.bdp`, `MemoryCard.bdp`, `Loading*.bdp`) |
| `Common` | anything else |

## 3. Making a pack
1. Dump: `RampageRE-dev.exe build\disc\RAMPAGE.VOL Chicago 1 --monster George --dumptex` (or with `--shot` and `--do`
   for an unattended run), then look through `mods/dump/`.
2. Edit or upscale a PNG; keep its aspect ratio and its alpha where it is see-through.
3. Put it anywhere under `mods/textures/`, named with the same 16 hex digits (renaming the front part is fine).
4. Run the game again: the console prints how many replacements it found.

## 4. How a replacement is drawn
- The game keeps every size and texture coordinate of the original; only the sampled image changes. The wrap mode
  stays the original's (repeat).
- A material with a texture region (an atlas tile: the taxis, the backdrops' bricks and windows; FORMAT_BDP.md
  "Material flags") draws a texture cut from its tile (render_gl.c `region_texture`). A pack still replaces the whole
  atlas, under the whole atlas's hash (the dump writes the whole one too); the tile is cut from the replacement at
  its own size (the same share of it), so the replacement must keep the atlas's layout.
- All mip levels are made with a 2x2 box filter and sampled trilinearly (GL_LINEAR_MIPMAP_LINEAR); the originals stay
  one bilinear level.
- Alpha is the PNG's as it is (`TP_ASIS`): the game drops texels below 0.5 and blends the see-through parts (the
  materials with flag 0x100000: fog masks, skylines, signs, trees, by their alpha) as with the original.
- Without a `mods/` folder (and without `--dumptex`) nothing is hashed or read and the game is drawn as before, so a
  `--shot` stays byte-identical.
