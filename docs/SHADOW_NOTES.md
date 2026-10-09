# Shadows

The game has no projected silhouettes, stencil volumes or shadow maps. Everything is a flat dark shape cast along one
fixed direction, **(−1, 1, −1) / √3** (where the light travels: towards −x, away from the camera, down). It is
hard-coded at 0x2B9928 into +0x308 of the lighting object (0x4D2680 + 0x308 = 0x4D2988); the block's light file
(`default_lights.txt`) is not used. There are four kinds plus a lighting tweak. `src/shadow.c` ports them.

| | what casts | onto | how |
|---|---|---|---|
| object shadows | every object or actor whose template has `ShadowData` | the street, z = 0.01 | textured rectangle, slid along the light |
| monster ground blob | the monster | the street, or the roof it stands on | `shadow_blob` under its body, fades with height |
| monster wall blob | the monster while climbing | the climbed building's front face, and its right side face | `shadow_blob`, no depth test |
| ground grid | the buildings | the play block's ground (the lot) | 23 × 6 vertex-coloured cells, a ray per sample |
| facade darkening | neighbour buildings | a back-row building's front / right face | its vertex light colour minus `MShadowRGB` |

## The keys

`WorldData.LightingData` (and `LightingDataNight`), read by 0x2B92F0 (RWorldRenderer vtable 0x44A870 +0x20),
through the key-name table at 0x3F5990 (indices 0x70–0x7D). All seven cities, day and night, have the same values:

| Key | Value | Where | Used by |
|---|---|---|---|
| `TShadowTintR/G/B/A` | 0, 0, 0, 0.85 | +0x328 … +0x334 (A halved there, 0x2B9D10) | every shadow's colour |
| `MonsterShadowIntensity` | 0.8 | +0x33C | both monster blobs |
| `GroundShadowIntensity` | 0.8 | +0x340 | the ground grid |
| `Texture\|MonsterBlob` | `shadow_blob` | +0x338, a material of shader 0x35 | both monster blobs |
| `MShadowR/G/B` | 0.4, 0.25, 0 | +0x314 … +0x31C | facade darkening; +0x324 = 0.2 is set beside it |
| `RampStart/RampEnd/RampIntensity` | 6, 35, 0.35 (night −0.5) | +0x27C … +0x288 | the building light's height ramp ([BUILDING_LIGHT_NOTES.md](BUILDING_LIGHT_NOTES.md)) |
| `MShadowRampDist` | 5 | — | no key name in the ELF: never read (the code's 0.2 is 1/5) |

0x326AB0 packs the tint into the object shadows' manager (0x4D6AB0): +0xD14 = RGB bytes (×255 + 0.5, R low), +0xD18
= the halved alpha (0.425). The ROMonster constructor (0x2FF510) packs the same RGB into its +0x58.
`BlockDef.EnableGroundShadow` (read by 0x31FC40; on when absent) is 0 in HK01, HK02, HK04, LV04, LV05, LV06, LV09.

The textures are in `fx.bdp`: `shadow_blob` (a soft disc) and `shadow_square` (a soft-edged rectangle), 32 × 32
PSMT8 with the engine's `_Alpha8` palette: white, the shape in the alpha. (`bomber_Shadow*` there are the bomber
boss's own.) The engine also builds four palettes `_Shadow0`…`_Shadow3` (0x25CDE0, from the table 0x3B4110); no
shadow here was seen to use them.

## 1. Object shadows (0x325C90)

**Who.** RWorldRenderer +0x18 (0x2B8A50) hands the frame's drawn objects (+0x218, count +0x220) to 0x325C90 with the
manager 0x4D6AB0. An object's render data +0xC is built once by 0x3265D0 from `ShadowData` in its template's
properties (inherited through `ParentPropertySet`); without one it points at a do-nothing record (gp−0x6C7C,
`BogusRenderShadowData`). `Objects1_props.bdp` has 62: the vehicles, trains, carts and stands (`shadow_square`) and
`DefActorProps` (`shadow_blob`, rect ±0.9 × ±0.7), the parent of every person and soldier.

**RenderShadowData** (`RRenderShadowData`, vtable 0x44B670, 0x34 bytes, pooled at manager +0xD10):
+0x4 its material slot (the manager keeps up to four materials of shader 0x4D, one per texture name; a fifth texture
gets no shadow), +0x8 the node, +0xC `MaxHeight`, +0x10 1 / (`MaxHeight` − `FullHeight`) (1e10 when that is ≤ 0.001),
+0x14 `Intensity`, +0x18 `ExProjHeight` (0 if absent), +0x1C `ShadowRect` x0 y0 x1 y1 ((−1, −1, 1, 1) if absent),
+0x2C the frame's list link.

**Each frame**, per object, z = the node matrix's translation z:
- nothing when `MaxHeight` − z ≤ 0;
- f = max((`MaxHeight` − z) / (`MaxHeight` − `FullHeight`), 1) — **max.s** at 0x325E74 (funct 0x28, the same as the
  max.s at 0x2FDC44), so there is no fade: the shadow is full until z reaches MaxHeight and then gone;
- alpha byte = (255 · f · `Intensity` · TShadowTintA/2 + 0.5) & 0xFF (0.8 · 0.425 → 87); none when 0;
- centre = (x, y) + (z + `ExProjHeight`) · (−1 / d.z) · (d.x, d.y): the node's point slid along the light to z = 0,
  i.e. (z + E) towards −x and +y;
- corners centre + X · (m00, m01) + Y · (m10, m11) for (x0 y0) (x1 y0) (x1 y1) (x0 y1), z = 0.01 (gp−0x712C),
  UVs (0 0) (1 0) (1 1) (0 1); the colour RGBA bytes each halved (87 → 43);
- up to 40 quads per call into the dynamic buffer 0x4CBBB8 (format 0x142, primitive 6: a quad list turned into strips
  through the order 0 1 3 2 at gp−0x78D4), the world matrix the identity (0x25B950), per material.

Typical: `Sedan4D` `MaxHeight` 3, `Intensity` 0.8, `ExProjHeight` 0.25, rect (−1.9, −4, 1.8, 3.3); trucks
`MaxHeight` 6, rect (−4, −5.2, 4, 5.8); stands and carts `Intensity` 1, `ExProjHeight` 0. A car lifted by the monster
keeps its full shadow, sliding left-back as it rises, until it is 3 up; a person on a roof (z > 3) has none.

## 2. The monster's ground blob (ROMonster)

ROMonster (vtable 0x44B040; constructor 0x2FF510): +0x10 the monster, +0x14 its radius, +0x18 / +0x1C ground blob
on / fade, +0x20 its point, +0x2C / +0x30 wall blob on / fade, +0x34 the body point, +0x40 … +0x54 the climbed
building's box, +0x58 the colour. Its +0x18 method (0x2FEEB0) runs 0x2FDA50, draws the ground blob, runs 0x2FDD00,
draws the wall blob.

**0x2FDA50, the ground point.** p = 0x2E4D30: the monster's position + its x axis · (MONSTER_DATA +0x1A0 + a move
offset) · scale + its y axis · (+0x1A4 + …) · scale (George: 2.5 ahead). radius = +0x198 · scale (0x2E4D00; George 3).
- climbing (monster +0xC644): no ground; fade out;
- else the building under it (0x2D6150): if p ± 0.999 · radius lies inside its box in x and y, the ground is its
  roof (+0x1F0), else the street (0);
- 0x2FD870 asks the world for objects (mask 0x800000) in a box over the ground point (radius across, 2 × height
  (+0x19C · scale) up): any hit turns the blob off at once;
- else on, the fade going towards 1 − (p.z − ground) / 5 at 1 / 0.4 s (gp−0x7208 = 5, gp−0x7210 = 0.4, dt 0x4D17A4),
  clamped 0 … 1 (off at 0). Without a ground it fades out at the same rate.

**0x2FEA20, the draw.** The world matrix is the monster's 3 × 3 at (p.x, p.y, ground + 0.01); a strip of 2 triangles
(format 0x102) (−1.2 r, −0.8 r) (1.2 r, −0.8 r) (−1.2 r, 0.8 r) (1.2 r, 0.8 r) at local z 0.01, UVs (0 0) (1 0)
(0 1) (1 1); material `MonsterBlob`, colour +0x58 with alpha byte 255 · fade · `MonsterShadowIntensity` (0x2600E0).
For George: 7.2 across by 4.8 long.

## 3. The monster's wall blob (0x2FDD00, 0x2FDEB0)

0x2FDD00: +0x34 = the body point (0x2E61E0, a point of the body). While mounted (+0xC644) the box = the building's
(+0xC640) +0x1DC … +0x1F0 + its position, max x + 0.1, max y − 0.1; the fade goes up at 1 / 0.4 s to 1, else down
to 0 (off). 0x2FDEB0, with (x, y, z) the body point and the box:
- z0 = max(z − 5, 0), z1 = min(z + 7, top); nothing unless z0 < z1. v = 0.5 − 0.1 (z − z0) … 0.5 + (z1 − z) / 14;
- the **front face** (y = min y): x from max(x − 5, min x) to min(x + 2, max x), u = 0.5 − 0.1 (x − a0) …
  0.5 + 0.25 (a1 − x): the blob is centred on the body point and reaches 5 left, 2 right, 5 down, 7 up;
- the **right side face** (x = max x), only when x > max x: the same with y;
- colour +0x58, alpha byte 255 · fade · `MonsterShadowIntensity`; render state 7 (depth compare) = 0x18 = ALWAYS and
  state 1 = 0 (TEST = 0x31001: alpha test NEVER with FB_ONLY, i.e. no depth write) — **no depth test, no depth
  write** — restored after.

## 4. The ground grid (0x31FC40, 0x31F8B0)

An object at 0x4D5FC0 (RSceneryMgr +0xC) holds a vertex buffer (+0xAA8) of 0x12A vertices, format 0x42 (position,
colour), shader 0x5A (+0xAAC), built by 0x31F340: six strips at z = 0 of 24 vertex pairs, x = −64.5 + 5i (i 0 … 23),
y = −2.5 + 5j (j 0 … 6), joined by two degenerate vertices: 0x128 triangles. Fixed world coordinates: the play block
(slot 0) only.

**At block start (0x31FC40)**: enabled = `EnableGroundShadow` (1 if absent). For each building i (the list
0x4D095C / 0x4D0958): its index into +0xCA4, its top (+0x1F0) into +0xA68[i]. For the 22 × 5 samples
(x = −59.5 + 5c, y = 2.5 + 5r, z = −0.001: the inner vertices) a ray towards the light, (1, −1, 1) / √3, up to 100,
against each building's model (0x27FFF0 on +0x74 with its matrix): up to four hits per sample are kept in its cell
(0x18 bytes: count, lit, indices, the hit's height t · d.z). The dirty rectangle (+0x4) is everything.

**A building's top changes** (0x31F790, from its damage 0x2BDC70): the new top is kept, and the old shadow's reach
(min x − old top, …, max y + old top) is added to the dirty rectangle.

**Each frame (0x31F8B0, from RSceneryMgr's draw 0x2A5390, right after the scenery 0x2825A0)**: the dirty
rectangle in cells (trunc(0.2 (x + 64.5) − 0.001) … trunc(0.2 (x + 64.5) + 0.999), clamped to 1 … 22 and 1 … 5):
each cell is lit unless one of its buildings' top is still ≥ the height its ray met it at (0x31F690); its vertex
(both copies in the strips) gets colour 0 when lit, else +0xAFC (0x31F540); the rectangle is emptied. +0xAFC
(0x320040) = TShadowTint RGB bytes with alpha byte 255 · TShadowTintA/2 · `GroundShadowIntensity` (0.34 → 87),
unhalved. Drawn with the identity world matrix, state 7 = 0x18 (depth ALWAYS), state 9 = 1 and 8 = 0x15 (alpha test
on, GREATER), 6 = 1 (its reference), primitive 4, 0x128 triangles; states restored after.

The render states (0x25ABA0 / 0x25A900 / 0x25A5B0 into GS TEST at 0x4CC140) take D3D compare codes + 0x10:
0x11 never, 0x12 less, 0x13 equal, 0x14 less-equal, 0x15 greater, 0x16 not-equal, 0x17 greater-equal, 0x18 always
(the alpha test through the jump table 0x4268C0; depth maps less/equal/less-equal/not-equal to the GS's GEQUAL).

## Facade darkening (0x31C7B0 → 0x31D6B0; applied by 0x31D560 / 0x31D7F0)

Per building (its shadow object at building +0x914), from its box and two neighbours, A (+0xC8) and B (+0xD0):
only a back-row building has them (0x31C8C0), A the first front-row building whose min x is past its own, B the first
of its own row whose min x is at or past its own max x (its right-hand neighbour); the shape is made again whenever
A's or B's top changes (0x31C860 → 0x31C7B0). Who lights with it and how is in
[BUILDING_LIGHT_NOTES.md](BUILDING_LIGHT_NOTES.md). front_y = min y + 0.1, right_x = max x − 1.
- **front** (0x31DD90, 0x31DC00), from A with its top + 2: A's left edge (x = A's min x) carried along the light onto
  the plane y = own min y from A's front and back corners gives (x0, z0), (x1, z1); three planes: x ≥ x0, x ≤ x1, under
  the line (x0, z0)–(x1, z1). It counts when x1 − x0 ≥ 0.001 and the span meets own min x − 2 … max x + 2. (0x31DC00
  also builds a 4–5 vertex polygon at +0x44 from the clipped span, extrapolating its top the wrong way; who reads it
  was not traced.)
- **right face** (0x31D9A0, at most two): from A if A's min x ≥ own max x, and from B: on the plane x = own max x, y from
  nb min y + d.y · t to nb max y + d.y · t, z from 0 to nb top + d.z · t, t = (own max x − nb min x) / d.x.

A lit vertex colour at p: on the front (p.y ≤ front_y) inside the three planes, depth = min(distance from x0, from the
top line); on the right face (p.x > right_x) inside a box, depth = min(p.y − its min y, its top − p.z); then each
channel −= MShadow · min(0.2 · depth, 1), not below 0 (0x31D560, for the ground-floor and top models, 0x31CCA0). The
facade grid (0x2CE300) calls the two halves itself: 0x31DA60 for its front face's vertices, 0x31D7F0 for its right
face's. `shadow_facade_front`, `shadow_facade_right`, `shadow_facade_darken`; city.c drives them.

## Other strings

`ShadowModifier` (vtable 0x444500, type 0x4000) is a material modifier that sets a texture transform (0x260510); with
the material keyword `shadow` (next to `bbias`, `boxfade`, `alphakill` at 0x4221F0) it is probably how the city
meshes' baked shadow decals (`Object_Shadows`, `hedge_med_shadow*`, `FireEscape_shadow`) are drawn — those are part
of the city meshes. `ShadowEffect1/2` are army script keys, `RampStart/RampEnd/RampIntensity` a height ramp of the
building light, not shadows.

## Drawing it (OpenGL 1.1)

Colours: the GS's 128 is 1.0. Vertex colours reach it unchanged (the grid's alpha 87 → 0.68; the object quads' 43 →
0.34) and the blobs' material colour is halved by its shader (204 → 0.8) [G: the formats and shaders 0x35 / 0x4D /
0x5A were not decoded to the GS registers; the ALPHA blend is taken as the usual (Cs − Cd) · As + Cd]. `src/shadow.c`
gives GL-scale colours.

- **Ground grid** (`SHADOW_PASS_GROUND`): right after the street, before buildings and objects. No texture,
  `glShadeModel(GL_SMOOTH)`, `GL_BLEND` `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`, depth test off, `glDepthMask(GL_FALSE)`,
  `glAlphaFunc(GL_GREATER, 0)`. If the viewer cannot order it that early: depth test `GL_LEQUAL` with
  `glPolygonOffset(-1, -1)` (it lies at z = 0 on the street).
- **Object shadows and the monster's ground blob** (`SHADOW_PASS_DECAL`): after the opaque world. Texture (`fx.bdp`),
  `GL_MODULATE`, the same blend, depth test `GL_LEQUAL`, depth mask off, `glPolygonOffset(-1, -1)` for the 0.01 lift.
- **The wall blob** (`SHADOW_PASS_WALL`): after the buildings and before the monster's mesh, depth test off, depth mask
  off (as the game). If the monster is drawn first, use depth test `GL_LEQUAL` with a polygon offset instead.
- **Facade darkening** has no pass of its own: it is in the buildings' vertex colours, drawn unlit
  ([BUILDING_LIGHT_NOTES.md](BUILDING_LIGHT_NOTES.md)).
- Fog: as the scene's [G].
