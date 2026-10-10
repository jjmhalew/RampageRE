# Building light

The buildings are not lit like the rest of the city. Their facades, and the ground-floor and top models of the
playable ones, are lit on the CPU: the game works out a colour per vertex from the block's lights and a height ramp,
takes away the neighbours' shadows, and draws the vertices with that colour. The damaged interiors carry their light
baked into their meshes. `src/building.c` (the light, the grid's colours), `src/city.c` ("the building light":
neighbours, facade quads, the models' own meshes) and `src/shadow.c` (the shadows' shapes) port it; `src/render_gl.c`
draws what has colours and no normals unlit. [R] read in the code, [G] guessed.

| What | Lit by | Where |
|---|---|---|
| the facade grid (storeys above the ground floor) | the CPU: light at each grid vertex, less the shadows | 0x2CE300 → the vertices' +0x4 and +0x0 |
| the cracks | the same vertices' light × `CrackTint` | the vertices' +0x0 |
| a playable building's ground floor, top, rubble with a shadow on it | the CPU: light at each vertex, less the shadows (format 0x112 models only) | 0x31CCA0 → a 0x142 buffer, shader 0x51 |
| the same with no shadow on it, the blocks behind | the GS's lights, ambient = ambient + ramp at the model's height | 0x2B8AD0 → 0x25B0C0 |
| the adornments | the same | 0x2CD060 |
| the interiors (`Skeleton`, `DamagedSkeleton`) | nothing: their meshes are prelit (format 0x142, no normals) | 0x2CD220 draws them with no light set |
| everything else (props, people, vehicles) | the GS's lights, no ramp | — |

## The lighting object (0x4D2680)

Filled by 0x2B92F0 (RWorldRenderer vtable +0x20) from the block's light file (`text|LightFile`, parsed by 0x16E5A0)
and `WorldData.LightingData` / `LightingDataNight` [R]:

| Offset | | Value |
|---|---|---|
| +0x27C, +0x280, +0x284 | `RampStart`, `RampEnd`, `RampIntensity` | 6, 35, 0.35 by day, −0.5 by night (all seven cities) |
| +0x288 | `RampIntensity` / (`RampEnd` − `RampStart`) | 0x2B9620 |
| +0x2A4, +0x2B0, +0x2BC | the light file's first three lights' `DIRECTION_X/Y/Z` (where the light travels), normalised; (1, 0, 0) if shorter than 1e-3 (0x2B9438) | day: (0, 0.5, −1), (−1, 0.25, 0), (1, 0, 0) |
| +0x2C8, +0x2D4, +0x2E0 | their `DIFFUSE_R/G/B` | 0.85 white, (0.65, 0.35, 0.1), (0.2, 0.2, 0.4) |
| +0x2EC | the file's `BEGIN_AMBIENT` | 0.1 grey |
| +0x308, +0x314, +0x324 | the shadows' direction, `MShadowR/G/B`, 0.2 | [SHADOW_NOTES.md](SHADOW_NOTES.md) |

With fewer than three lights the rest get colour and direction (0, 0, 0) from the shared zero vector at 0x409FF8
(0x2B94B0 … 0x2B950C) [R].

**0x2B8C40, the light at a point** p with a normal n (unit):
- ramp r = 0 while p.z ≤ `RampStart`; `RampIntensity` / (`RampEnd` − `RampStart`) × (p.z − `RampStart`) below
  `RampEnd`; `RampIntensity` from there (so up to +0.35 by day, −0.5 by night, at 35 up and higher);
- colour = ambient + Σ light colour × max(−n · dir, 0) + r on each channel, each clamped 0 … 1; alpha 1.

It is the GS's own lighting plus the ramp: below 6 up it gives what the lights give (the viewer's ground floors come
out identical to the pixel). **0x2B8AD0** gives a model lit by the GS its ambient: the light's ambient + the ramp at
the model's matrix's z, clamped (`building_light_ambient`).

## The facade grid (0x2CE300)

Called with the grid and its building's shadow object (building +0x914, "S"; NULL for the blocks behind: 0x3059D0
lights their grids once, with no shadows) whenever the building is redrawn changed (+0x154) or a neighbour's top has
changed (0x2BF180, from the buildings' draw 0x320280 → 0x320750), when the work queue at 0x4D7870 lets it run
(0x2BF270(Q, item, 4)) [R]: the first request of a frame is granted, a later one once it has been queued 4 frames
(0x2BF360 queues it with its frame; 0x2B8F80 starts each frame with none granted). A grant unqueues it. The
building's own change (+0x154) stays asked until it is served; a neighbour's top change is used up even when
refused (0x31C860). The queue also serves the damage mesh, the models' records and 0x31F8B0. It sets G+0x1E4 = −1 and G+0x670 = 1 (the meshes to be made again).

For every vertex row r (0 … the storeys: z = 6 + 3 × its slot, less 3 × the falling storey's progress above it)
and face (0 left, 1 right, 2 front; normals (−1, 0, 0), (1, 0, 0), (0, −1, 0) from 0x430EE0), every vertex 0 … D
(sides) or 0 … W (front) at its place before the dents (left: x = −2.5 W, y = 2.5 (D − 2i); right: x = 2.5 W,
y = 2.5 (2i − D); front: x = 2.5 (2i − W), y = −2.5 D; plus the grid's matrix's x, y):
- colour = the light at it (0x2B8C40);
- on the front face, less the front's shadow (0x31DA60, S itself); on the right face, less the right face's
  (0x31D7F0, S + 0x88); the left face is never shaded;
- +0x4 = the colour as bytes (× 255 + 0.5; R low, alpha 255);
- +0x0 = the colour × `CrackTint` (+0xC4 … +0xCC, alpha × +0xD0), clamped 0 … 1, as bytes, keeping +0x0's alpha
  (the dent level × 85).

G+0x1EC … +0x1EE (all 0xFF from the grid builder, 0x2D2824) say which faces are tested: each pass tests only the
faces that were shaded on the last one, and clears the flag of a face none of whose vertices was shaded. The
shadows only shrink (a neighbour only gets lower), so once clear a face stays clear.

**The facade quads** (0x2C81A0 front, 0x2C8760 right, 0x2C8D30 left) take each corner's +0x4 × the cell's tint
(cell +0x8 … +0xC, u16, 256 = 1: the storey's `FloorTintR/G/B` × 256 + 0.5, 0x2CBC78; 1 unless set, New York's have
0.67 … 1.5) >> 8, at most 255, and the emit (0x2C6790) halves every byte for the GS: so the vertex colour is
min(light × tint, 1). The cracks' pass the same with +0x0 [R: the emit 0x2C7020 not followed byte by byte].

## The models (0x2CD220, 0x31CFC0, 0x31CB10, 0x31CCA0)

0x2CD220 draws a grid's models: the rubble (G+0x6EC, while G+0x1D4 is set), the ground floor (G+0x66C), the top
(G+0x65C[G+0x658], or the nearest one below it that there is), then the interiors. With a shadow object (the playable buildings) the first three go
through a record each of S (+0x2A8 rubble, +0xD8 ground floor, +0x1C0 top; 0xE8 bytes):
- 0x31CFC0: the model and its place; moved by more than 0.001 (the sum of |Δ|) or newly given: dirty (+0x14). Once
  allowed by the work queue, 0x31D2E0 makes a dynamic buffer of format 0x142 (position, colour, uv) and shader 0x51
  from the model, if its format is 0x112 (position, normal, uv), and 0x31D0F0 copies its positions, uvs and normals
  (s8 / 127) into it;
- 0x31CB10 draws it: dirty → 0x31CCA0 lights it; without a buffer (or +0x17) the model is drawn the usual way, lit by
  the GS with 0x2B8AD0's ambient;
- 0x31CCA0: when the highest a shadow reaches on S (+0x84: the front polygon's top, the neighbour's edge projected
  along the light onto the front, capped at its own top + 2, 0 without one (0x31D6B0, 0x31DD90, 0x31DC00) [R]; the right boxes' tops +0xA0,
  +0xB8) is below the model's z − 3 (and 0x4D203C is clear), +0x17: drawn the usual way. Otherwise every vertex: the light at
  its place (the model's position + the vertex; buildings are not turned) with its normal, less the shadows
  (0x31D560: the front's planes where p.y ≤ min y + 0.1, the right face's boxes where p.x > max x − 1), as bytes
  × 127 (1.0 = 0x7F). If no vertex was shaded and +0x16 is set, the buffer is let go (+0x15): the GS lights it from
  then on.

So a playable building's ground floor and top are lit per vertex where a shadow can reach them, and by the GS (with
the ramp at their foot) where none can; the two agree but for the ramp's change over the model's height (≤ 0.02 for
the 1.5-high tops).

## The interiors

A floor's `Skeleton` and `DamagedSkeleton` (`Damaged10x20_00.pwk`, `Damaged10x15_Maximum.pwk`, …) are the only city
meshes whose sections are format 0x142: position, colour, uv, no normals (57 sections in the seven cities). Their
colours are grey shading baked in (0x0B … 0x7F: dark corners and beams, lit walls); some have a floor section of
format 0x152 (with normals). 0x2CD220 draws them (when G+0x1E0 has 8, for every floor whose +0x4 has 8 and not 2;
the `DamagedSkeleton` once FloorHP (+0x14) is not above `DamagedSkeletonHP` (+0x18, half of FloorHP unless set);
a falling floor's squashed by 1 − its progress) after a plain world matrix (0x25B950) and no light of its own: nothing
in the game darkens, burns or tints them by damage or by storey. Without normals they cannot be lit by the GS; they
show their baked colour × the texture, as the game's own 0x142 buffers (shader 0x51) do: its VU1 program (0x357B50,
record 0x3B8398) transforms the place, makes STQ from the uv and stores the vertex colour straight into RGBAQ, never
reading the light rows 13 … 16 (the lit 0x152 program 0x37A2A0 does); none of the 0x142 shaders' programs reads them
[R]. That the GS multiplies it by the texture (TEX0.TFX modulate) is taken from the 0x80 = 1.0 convention [G].

## The neighbours' shadows (0x31C8C0, 0x31C860, 0x31C7B0)

The shapes are in [SHADOW_NOTES.md](SHADOW_NOTES.md) "Facade darkening". Which buildings cast them [R]:
- **0x31C8C0** (the shadow object's set-up, from the building's creation 0x2BE008): +0xC4 the building, +0xCC and
  +0xD4 (the neighbours' tops) −1e20, +0xC8 = +0xD0 = none. Only a building in the back row (+0x10C = 1) gets
  neighbours: **A** (+0xC8) the first building of the front row's list (0x4D0918, count 0x4D0910) whose min x is
  greater than its own; **B** (+0xD0) the first of its own row's list (0x4D0900 + row × 0x20 + 0x18) whose min x is
  at least its own max x. The row lists are filled by 0x2A5110 in the order the buildings are added (the block's
  `BuildingInstance|` order). Front-row buildings are never shaded.
- **0x31C860** each time the building is drawn: has A's or B's top (+0x1F0) changed since? Then **0x31C7B0**: the tops
  kept, the shape made again (0x31D6B0 from the three grids' boxes), the models' records dirty; if the shape shades
  neither face (S+0 clear and no right box) A and B are dropped.

In Chicago's first block only the back-row 4 × 1 (x −42 … −22) is shaded: the front-row 2 × 4 at x −13 casts on its
right face (all of it but the top: the box reaches 10.5 up) and a sliver of its front. Block 2 has five shaded
back-row buildings; the back-row 3 × 4 at x −15 … 0 has the shadow of the front-row 4 × 4 at x 0 over the right half
of its front.

## The port

`building.h`: `BuildingLight`, `building_light_init` (0x2B92F0's part), `building_light_ramp`, `building_light_at`
(0x2B8C40), `building_light_ambient` (0x2B8AD0), `building_light_grid` (0x2CE300, colours per live vertex row, face,
column). `shadow.h`: `shadow_facade_front` (0x31DA60), `shadow_facade_right` (0x31D7F0), `shadow_facade_darken`
(0x31D560) and `shadow_facade_build` (0x31D6B0). `city.c` keeps per building the light rows, the storeys'
`FloorTint`, the shadow object (neighbours, their tops, the faces' flags) and the ground-floor and top items: those get
meshes of their own at the block's end whose lit parts have colours instead of normals. `city_build` lights every
building at its end (`light_buildings`); `scene_buildings_update` lights a building again when it moves, changes or a
neighbour's top changes, before its facade passes. The facade quads and cracks carry the colours and no normals;
`render_gl.c` draws a part with colours and no normals with the GL lighting off.

Differences (the game's side read: 0x2CD220, 0x31CB10, 0x2CD060, 0x2CD470) [R]:
- the viewer lights the ground floor and top always per vertex (the game: only while a shadow can reach them, else
  the GS with the ramp at their foot, 0x31CC00 → 0x2B8AD0), and the blocks behind's models too (the game: the GS);
- the adornments keep the GL lights without the ramp (the game: the GS lights and the ramp at each one's height,
  0x2CD060); the rubble too (the game: with a shadow object as the ground floor, without one no ambient call of its
  own, so the last one set).

The window glass behind an InnerWindowTexture (one colour G+0x6F0 at every corner, format 0x142: 0x2C92F0, 0x2D2700)
and the inner corner walls (0xFF808080 halved, 0x2C63A0) have no normals, and are drawn unlit here too [R].

## Drawing it (OpenGL 1.1)

The colours are GL scale (the GS's 0x80 = 1.0). A part with colours and no normals: `glDisable(GL_LIGHTING)`, the
colour × the texture (`GL_MODULATE`), fogged as the world. The rest as before.
