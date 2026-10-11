# Water

Twelve `WaterObject|` groups in the cities' `BlockDef`s are the water surfaces: Hong Kong's harbour and a shrine
pond, Las Vegas' pools and fountains, Chicago's river. `src/water.c` ports the game's code; this file is what was
read to write it. [R] = read in the code, [G] = guessed.

| City | Block | NumCols x NumRows | Origin | MinGroundHeight (X, Y scale) | EnvMap | Waves | WaveSpeed |
|---|---|---|---|---|---|---|---|
| Chicago | CG07_Block8 | 100 x 21 | 0, 10.5, 0 | -5 (0.9, 0.7) | water_reflection_map | 5 CylWave | 5 |
| HongKong | HK01_WaterfrontA | 140 x 15 | 0, -12.5, 0 | -5 (0.9, 0.6) | water_reflection_hk | 5 | 5 |
| HongKong | HK02_WaterfrontB | 140 x 15 | 0, -12.5, 0 | -5 (0.9, 0.6) | water_reflection_hk | 5 | 5 |
| HongKong | HK02_WaterfrontB | 23 x 12 | 12.175, 1, 0 | -2 (0.9, 0.6) | water_reflection_hk | 5 | 5 |
| HongKong | HK04_Shrines | 16 x 15 | 17.5, 9, 0 | -2 (0.9, 0.6) | water_reflection_hk | 5 | 2 |
| LasVegas | LV04_Lancelots | 100 x 20 | 0, 10, 0 | -5 (0.9, 0.6) | water_reflection_map | 5 | 5 |
| LasVegas | LV05_JollyRogers | 50 x 20 | -5.4, 10, 0 | -5 (0.9, 0.6) | water_reflection_map | 2 | 5 |
| LasVegas | LV06_NoName | 10 x 20, 10 x 10, 10 x 20, 10 x 10 | -11, 10 / -21, 5 / 9, 10 / 19, 5 | -5 (0.9, 0.6) | water_reflection_map | 1 each | 5 |
| LasVegas | LV09_Neros | 48 x 15 | -0.5, 7.5, 0 | -5 (0.9, 0.6) | water_reflection_map | 1 | 5 |

All have `TileSize = 1`, `Damp = 0.98`, `Name = "FooWater"`, `LoadWhenWaiting = 1`, `LoadWhenDistant = 0` (1 on
Chicago's). HK04_Shrines also has `Texture|BaseMap = "blimp"`, which nothing reads [R: the reader asks for EnvMap only].
No `BoxWave` is used.

## The code

| Address | |
|---|---|
| 0x339A10 | constructor: WaterObject (vtable 0x44B778), its draw object ROWater at +0x98 (vtable 0x44B7D8, typeinfo 0x434EC0) |
| 0x338CC0 | **the reader** (object, property group, f12 = an offset added to Origin y) |
| 0x338510 | the grid, its vertex and index buffers; 0x3381B0 the flat quad instead (when that offset is > 0) |
| 0x338180 | vtable +0x10, each frame: 0x336DC0 unless flat |
| 0x336DC0 | **a frame**: the waves, the wave equation, the vertex buffer's z, the normals, the swap |
| 0x337C40 | the waves: every CylWave, then every BoxWave (0x339BB0 evaluates one) |
| 0x336960, 0x3368F0 | press a disc / a box into the water; 0x336C30 its grid rectangle, 0x336B40 / 0x336A30 the loops |
| 0x3369E0 | press a disc at a world position (minus the translation): what the monster calls |
| 0x2D5F00 | the monster's water test: its box against each water's +0x104 (0x2801A0), once a frame |
| 0x337E00 | the floor under the water at a world (x, y) |
| 0x339D00 | ROWater draw (vtable +0x1C); 0x339DC0 its bounding sphere; 0x339840 / 0x339AE0 destructors |
| 0x37C120, 0x3906B0 | the VU1 program of shader 0x7E (and its clipping twin) |
| 0x304450, 0x3050F0 | the block's object factories that call the reader (f12 = 0; f12 = the slot's y behind, 41 / 82: the flat quad) |

The key names are pointers in the table at 0x3F5C64 .. 0x3F5C9C ("WaterObject", "Origin", "MinGroundHeight",
"MinGroundHeightXScale", "MinGroundHeightYScale", "NumRows", "NumCols", "TileSize", "Damp", "WaveSpeed", "EnvMap",
"CircleRadius" (not read by the reader), "WaveList", "CylWave", "BoxWave"). There is no "SineWave" string: the
reader takes the CylWave's four groups in order, whatever they are called, and checks their counts only.

## The object (0x1B4 bytes)

| Offset | |
|---|---|
| +0x08 | 4x3 matrix: identity, translation +0x2C = Origin + (0, f12, 0) |
| +0x38, +0x44, +0x48 | (0x409FF8's vector), the bounding radius² and radius √(hx² + hy²) |
| +0x104 | its box: (-hx, -hy, -Origin z) .. (hx, hy, 0) (0x281170); the monster's overlap test uses it |
| +0x144 | flat (f12 > 0) |
| +0x148 | the grid's local x, y per vertex |
| +0x14C, +0x150 | the two height buffers; +0x170 / +0x174 point at them: older / newer, swapped every frame |
| +0x154 / +0x158 / +0x15C | CylWaves: array (64 bytes each: 4 sines x 4 floats), capacity, count |
| +0x160 / +0x164 / +0x168 | BoxWaves (80 bytes each: 5 sines) |
| +0x16C | the material: shader 0x7E (0x25C4F0), EnvMap in texture slot 0 (0x260040), colour 0xFFFFFFFF (0x2603F0) |
| +0x178, +0x17C | vertices per row / per column: NumCols + 1, NumRows + 1 |
| +0x180 | NumRows x 2 (NumCols + 1) - 2: the strip's triangles |
| +0x184, +0x188 | half sizes hx, hy = NumCols x TileSize / 2, NumRows x TileSize / 2 |
| +0x18C | TileSize |
| +0x194, +0x198 | the floor's inner half sizes: XScale x hx, YScale x hy (hx, hy without MinGroundHeight) |
| +0x19C | MinGroundHeight (0 without) |
| +0x1A0, +0x1A4 | 1 / (hx - inner x), 1 / (hy - inner y); 0 when that is ≤ 0.001 |
| +0x1A8 | +0x1A0 x (hy - inner y) |
| +0x1AC, +0x1B0 | Damp, WaveSpeed |
| +0xF8 | the normals, (0, 0, 1) at first |
| +0xFC, +0x100 | the vertex buffer (the strip unrolled, format 0x112), the index buffer (the strip's grid indices) |

## The waves

A `SineWave| { - a - b - c - d }` is `c sin(a t + b) + d`, t = the game clock (+0x30 of the clock at gp−0x6CFC), the sine
being the VU0 micro routine at 0xE00 (the one sincos 0x106730 uses). A **CylWave** is four: [0] depth, [1] radius,
[2] centre x, [3] centre y; the reader multiplies the amplitude of [2] by hx and of [3] by hy, so the centre wanders
over the whole pool. Every frame each presses the water under the disc down to −depth: every inner vertex within the
radius of the centre, both height buffers, `h = min(h, −depth)`. A **BoxWave** is five: [0] depth, [1] half width,
[2] half length, [3] centre x (x hx), [4] centre y (x hy); the same over a rectangle.

The common set: `[6.28 0 .125 .125] [3 0 2 2] [.314 0 1 0] [1 1 1 0]`: a disc of radius 0 .. 4 pulsing 3 rad/s,
pressing 0 .. 0.25 deep once a second, its centre swinging along x over the whole pool every 20 s and along y every
6.3 s; a second (radius 0 .. 2, 0 .. 0.5 deep) swings along x every 16 s at y = 2.5; three more (radius 0 .. 2,
0 .. 0.25 deep, twice a second) sit at x = −30, 30 and 0 and swing along y every 3.1 s.

The grid rectangle (0x336C30): x0 = cols/2 + trunc((cx − ex) / tile) − 1 (cols = NumCols + 1, integer halving), none
if x0 ≥ cols − 1, raised to 1; x1 = cols/2 + trunc((cx + ex) / tile) + 1, none if ≤ 0, lowered to cols − 2; the same
in y. The loops run x0 ≤ x < x1 and y0 ≤ y ≤ y1. The edge vertices are never touched.

## A frame (0x336DC0)

1. The waves (above).
2. k = WaveSpeed² dt² / TileSize² (dt: the clock's +4); for the inner vertices
   `older = Damp (k (newer[x−1] + newer[x+1] + newer[y−1] + newer[y+1]) + (2 − 4k) newer − older)`: the damped wave
   equation, leapfrog, the result into the older buffer.
3. The vertex buffer's z = those new heights (through the index buffer, u8 or u16).
4. The inner normals from the new heights: (h[x−1] − h[x+1], h[y−1] − h[y+1], 2 TileSize), normalised; (1, 0, 0) if
   its length² < 1e−6. Into the vertex colours as signed bytes, n x 127 (alpha stays 0x7F).
5. Swap: the new heights become the newer buffer.

There is no other input: no rain, no current; the whole look is the CylWaves and the monster.

## The monster

**The water test** (0x2D5F00, returns the water or 0) [R]:

- Once a frame: the answer is kept at monster +0xC700 with the flag +0xC704, which the update clears at its top
  (0x2DE5BC; also 0x2DCC04 and the setup's 0x2E9988). Whatever asks first in a frame decides.
- On a building's face (+0xC644) only a state whose node's action code (+0x3C) is 7 (climbing down) is tested; off
  it, a state of action 1 (the transitions onto a building) is not. Otherwise none.
- The box: the **body centre** (0x2E4D30 on the matrix 0x282B90 returns, +0x8: the feet + MONSTER_DATA +0x1A0 along
  its x axis and +0x1A4 along its y, each plus the kept type-16 record's +0xC / +0x10, x the dynamic scale), raised
  by the half height +0x19C x scale, and 3 more while airborne (mode 1) more than 3 up (0x2D603C, the same 3 the
  air case adds to its landing test, 0x2E29A4 .. 0x2E2A9C); half sizes +0x198 x scale twice and +0x19C x scale.
  George: 2.5 in front of the feet, from the feet to 8.5 up, 3 each way.
- Against each water of the list at 0x4D0AE0 (count 0x4D0AE8) whose +0x74 (its box +0x104) is set, 0x2801A0:
  the centre into the water's frame, the box shape's test (vtable 0x449000 +0x10, 0x280D60): per axis
  a + b − |d|, none below 0. The water's box is (−hx, −hy, −z) .. (hx, hy, 0) about its place (0x339158), i.e. its
  rectangle from the street (z 0) to the surface, flat for all twelve (Origin z 0). The last that touches.

So a monster is in a water when its feet are at or below the surface (and no more than its height under it) and its
body centre is over the pool or within its radius of the edge: on the street at the edge of the harbour it already
splashes, and its ground there is the floor's value outside the rectangle, the surface (0). In the air it is in a
water only once its feet are under the surface: until then its ground is the street (z 0), after that the floor, so
a jump into a pool falls on through the surface and lands on the floor.

The users:

- **its ground** (0x2E8530, kept at +0xA48 for the frame): the roof under it (0x2D6150, +0xD4) if any, else in a
  water the floor (0x337E00) under the body centre (0x2E859C), else the street (gp−0x6D5C vtable +0x34) [R];
- **wading** (0x2E28A0 .. 0x2E2974, the case of modes 0 and 5 in the monster's ground function 0x2E2770, jump table
  0x4312B0: 0x2E2870 for 0 and 5, 0x2E2980 for 1, 0x2E2B28 for 2, 0x2E2B58 for 4, nothing for 3): when the state
  (+0x10C) has a node whose +0x2C has 0x1080, 0x50 or 0x80002 (it travels: the stick or the root along x, y or z),
  0x2E2BD0 → 0x3369E0 at the body centre, radius 0.5 x +0x198 x scale, depth −0.9 x clamp(surface (+0x34) − the
  ground (0x2E8530, f20 = the frame's ground at 0x2E27EC), 0, h) / h, h = +0x19C x scale. George in the harbour's
  middle (floor −5): h 4.25, so 1, pressed 0.9 down. Standing still, prone, in the air: nothing [R];
- **landing** (0x2DC450 from the mode-1 case at 0x2E2B18): mode 4 first, then the impact's place p = `Bip01 Spine1`
  (0x2E6000), z the water's +0x34 or the ground; WaterImpactFx or GroundImpactFx (0x2F48D0); then 0x2E2BD0 at the
  body centre, radius 1.25 x +0x198 x scale, depth −(2 s + 0.5 (1 − s)), s = clamp(−vz / 50, 0, 1) (+0xBD38, f20 =
  0 at 0x2DC508) [R];
- **footPlant** (0x2DC350 / 0x2DC3D0): WaterFootStepFx at the foot's (x, y), z the surface
  ([FOOTSTEP_NOTES.md](FOOTSTEP_NOTES.md)) [R];
- **climbing** (0x2E1618, mounted in mode 5): the place check (0x2DDC40) is asked as ever, and in a water its good
  answers (1, −9) no longer hold the monster on the face: −8 still gives slot 73 (+0x124), anything else slot 31
  (+0x7C), as a failed check does (0x2E1660 .. 0x2E1680, 0x2E1780). So climbing down into a water lets go of the
  wall [R].

The props name the splashes: `WaterFootStepFx = "footfall_water.edf"`, `WaterImpactFx = "footfall_big_water.edf"`
(George_prop and the others, read by 0x2F5D80 into +0x144 / +0x154 of the props record); they carry the sounds
(`Rampage|Splash`, `Rampage|Geyser`). Nothing else spawns a splash: no effect sets `CollidesWithWater` (0x1FCE9C,
particle flag 0x400; FX_NOTES.md), no other object asks for the water (0x2D5F00's callers are the monster's alone:
0x2DC364, 0x2DC3E4, 0x2DC63C, 0x2E1618, 0x2E28F0, 0x2E2BF0, 0x2E855C), and the string "Water" at 0x4180A8 is only a
name 0x1AC480 compares (0x1AC558, with a texture's mip setup after it) [R; its purpose not followed]. No wading sound: the effects' own.

## The blocks behind

The factory of the two blocks behind (0x3050F0, called by 0x302AF0 for slots 1 and 2) makes a WaterObject when the
group has `LoadWhenWaiting` (key 0x3F5DA0) on slot 1 or `LoadWhenDistant` (0x3F5DA4) on slot 2 set
(0x305888 .. 0x305960) and reads it with f12 = the slot's y (its argument's +0x4: 41, 82), so it is the flat
stand-in (0x3381B0: one still quad over the pool at the surface, no waves) [R]. All twelve have LoadWhenWaiting 1, so
the block before each water block shows its pools flat in the gap behind its back row (without it that gap is the
basin, open to the fog colour); only Chicago's river has LoadWhenDistant 1 and shows two blocks ahead too. The block
being played gets the full grid from 0x304450 (f12 = 0) [R].

## The floor (0x337E00)

0 (the water's z) outside the water's rectangle, MinGroundHeight inside the inner rectangle (hx x XScale, hy x YScale),
and between them MinGroundHeight x (1 − max(dx / (hx − inner x), dy / (hy − inner y))), dx, dy how far past the inner
rectangle: the eight regions and corner tests of the code come to that. A pool 5 deep that shelves up over the last
10 % of its length and 30 or 40 % of its width. It shapes nothing that is drawn.

## Drawing

ROWater 0x339D00: the world matrix = +0x08 (0x25B950), the material bound (0x25FF30), the translation minus the
camera's position (+0x54 of the camera at 0x4CB4A0) into 0x4CC030, render flag 0x4CBEF4 |= 8, the vertex buffer
(0x4CBAB4), a triangle strip of +0x180 triangles (0x259E50, primitive 4).

Shader 0x7E (the descriptor at 0x40F910 + 0x7E x 0xD8 = 0x416360, its compiled record 0x3BA3F0 found through the hash
table 0x3C73B0): one pass, texture slot 0, colour mode 0 = constant 255 (0x265A20) → x 0.5 → 127.5 = the GS's 1.0,
alpha 128; vertex format 0x112; VU1 program 0x37C120 (0x3906B0 when clipping). Flag 8 makes the header upload 8
quadwords to VU1 0x89 instead of 4 (0x263210): the 4x4 transform, then 0x4CC000's 4 rows, of which the program reads
only the last, the camera-relative translation (at 0x90).

The program, per vertex (input: position, then the colour as integers):

    N = colour.xyz / 127                       the normal from step 4
    V = position + (translation − camera)      the view ray, world space
    R = (V − 2 (N·V) N) / |V|                  reflected, unit
    s = |Rx| + |Ry| + 1e−9
    a = Ry ≤ 0 ? 1 + (|Rx| + Ry) / s : 3 − (|Rx| − Ry) / s;   a = Rx > 0 ? 8 − a : a
    S = a / 8,  T = 1 − Rz,  Q = 1 / w

so S is the reflection's azimuth (a "diamond angle": −Y 0, −X 0.25, +Y 0.5, +X 0.75, round to 1) and T its height
(1 at the horizon, 0 straight up). The texture is a skyline panorama (128 x 64), sky at the top, so the water mirrors
the city's skyline and sky; the seam behind the camera smears as on the PS2. The vertex colour is only the normal's
carrier: the drawn colour is the constant white. The grid's own UVs in the vertex buffer are not read. Fog: the usual
F = clamp(a + b w, 0, 255) of the other programs. The strip's face is tested with the screen-space cross product
and the strip's parity and the back faces get ADC (culled) [R, loosely]. Neither the block's CubeMap nor its CubeTint
plays a part [R: nothing in 0x3368C0 .. 0x339DF0 reads them; they are read by 0x2B92F0].

## The port

- Faithful [R]: the reader, the grid, the waves and their scaling, the pressing rectangles and loops, the wave
  equation and its order, the normals and their byte quantising for the reflection, the swap, the monster's push
  (water_disturb), the floor, the flat far-off quad, the env-map texture coordinates, white x texture.
- The step: the water's update (0x338180 → 0x336DC0) runs in the world's object update (0x284E40) once a world step,
  its dt gWorld +4 = 1/30 s (0x336DE4) [R]; the port steps it at 1/30 too, so k is about 0.028.
- The state [R]: shader 0x7E's pass is state 1 of 0x3B4240 (0x265840): TEST 0x50002 (no alpha test, Z test, Z
  written), PRMODE 0x38 (shaded, textured, fogged, no blend): drawn opaque, depth tested and written, fogged, as here.
- The port's own: the strip's joins between rows are left out (the game's rows run on in one strip with no
  degenerate triangles), two triangles per cell instead; k is held at 0.5 for a very long frame (the game has no
  guard).
- The pass [R]: slot 0, state 1, TCC 0, TFX 1 (DECAL: the pixel is the texture's colour and the vertex alpha, no
  alpha test), texture flags 0x11: bit 0 makes CLAMP_1 REPEAT on S and T (0x265840 -> 0x26AA20); the material is built
  in code, so no region override (material +0x80 zero). Here GL_REPEAT both ways, the alpha test off.
- The monster [R]: the water test (main.c `mon_water`: the gating by the face and the action codes, the body centre
  `mon_body_centre`, the box with the airborne 3, `water_box_touches` for 0x2801A0, the last one, once a frame), the
  ground under the body centre, the wading's states, modes, place, radius and depth, the landing's press at the body
  centre, the footfalls' and landing's choice of effect, letting go of the face in a water (climb.c `climb_check`,
  `Climber.in_water`). The type-16 record's +0xC (added to +0x1A0) is not kept by the viewer: 0 there [G] (George's
  +0x1A0 is 0 anyway). The water list holds the flat stand-ins of the blocks behind as well [G: which list the game
  adds them to was not followed; a monster cannot reach them].
- The blocks behind [R]: city.c makes the flat quads for the waters with LoadWhenWaiting (slot 1) / LoadWhenDistant
  (slot 2), at y + 41 / + 82.
