# Particle effects: findings (to fold into FORMAT_EDF.md)

What the game does with an effect file (`type = particle`, `composite`; `camerashake` is in [CAMERA.md](CAMERA.md)
"Shake"), traced from the PS2 code for `src/fx.c`. [R] read in the code, [G] guessed.

## Classes and where they are
| What | Where |
|---|---|
| `ParticleSystemFx` (the live effect) | vtable 0x446AE0; advance (+0x3C) 0x1F2900, start (+0x28) 0x1F3570 |
| `ParticleSystemDef` (the parsed file) | vtable 0x446BB0; load (+0x10) 0x1F95E0: `int.emitter_count`, then one emitter definition (0x18C bytes, constructor 0x200690) per `{ }` |
| one emitter's keys | 0x1FA4D0 (a 21 KB function: a key table on the stack, then the lines) |
| derived at load | 0x1F97D0 (graph scaling, rotation and tile flags), 0x1F9290 |
| the coded text | 0x1ED400 (one line), 0x1ED060 |
| graphs | factory 0x20D630; `GraphConstant` 0x447180, `GraphLinear` 0x447140 … `GraphQuintic`, `GraphLinearInterpolation` 0x447000, `GraphSineTable` 0x446F80 |
| an emitter's frame | a member pointer at definition +0x180, picked by 0x21AD40 from 3 specialisations (0x20EFD0, 0x213F30, 0x216470); the default lives in BSS (0x450938) |
| drawing | member pointer at +0x174 (0x2213F0, 4 specialisations): billboards 0x21D798 / 0x220768 → 0x2214D0 (VU1 packets), streaks 0x21C060 (0x1F6CA0 / 0x1F7500), shapes (XForm) 0x21B330 / 0x21DC20, streams 0x1F4B90 |
| emitter start | 0x1F7F90; shutdown 0x1F7CD0 |
| `CompositeFx` | vtable 0x446830: `int.num_edfs`, `edf.name.N` (the parts by name, ".edf" optional), all started together |

The specialisations are C++ template instances on the definition's features (OneTimeEmission, normalisation,
textures, …); they compute the same thing.

## The text
Each line is decoded to words separated by one space, `=` stays [R 0x1ED400]: `?text?` and `?text` are literals
(a literal may hold spaces), two capitals a dictionary word. Keys are compared to the key table; a flag line is a
list of tokens. `src/fx.c` decodes all 287 effects of fx.bdp to exactly the text `tools/edf.py` prints.

## Graphs (0x20D630)
The value of a `graph.*` key; the factory looks for the words with `strstr` in this order:

| Word | Numbers | Value at t |
|---|---|---|
| `const` | v | v |
| `reciprocal`, `reci` | 8 floats | (no effect uses it) |
| `polynomial`, `poly` | n, then c0 … cn (n ≤ 5; n = 0 is a constant) | c0 + c1 t + … + cn tⁿ (0x20EDC0 …) |
| `linearinterpolation`, `lininterp` | n, then n pairs x y | y held before the first and after the last x, straight lines between (0x20B980). A missing x is the last + 0.1, a missing y the last y; each point keeps 1 / (x − previous x), 1/1000 when 0 (0x20BA80) |
| `sine`, `sin` | a f p o | a sin(f t + p) + o through a 128-step table (0x20B1E0); `sin` alone is sin t |

Emitter graphs run over the emitter's cycle (t = age / cycle length), particle graphs over the particle's life
(t = age / lifetime): the "normalized" masks (+0x8 = 0x3FF, +0xD4 = 0x3F) are on by default and every file writes
`flags.*.normalized = NormalizedEmAll` *without* a sign, which the flag reader skips, so they stay on.

## Keys (emitter definition offsets)
| Key | Off | |
|---|---|---|
| `graph.gen_rate` | +0x24 | particles a second; with +OneTimeEmission: gen_rate(0) particles on the first frame |
| `graph.start_area.x/y/z` | +0x28 | half extents of the start box / ellipsoid |
| `graph.vel_from_start` | +0x34 | speed away from the area's centre (along the start offset, normalised) |
| `graph.em_vel.x/y/z` | +0x38 | velocity |
| `graph.em_vel_r.x/y/z` | +0x44 | ± random velocity per axis (× (2r − 1)) |
| `graph.em_pt_offset.x/y/z` | +0x50 | start offset (default `vector.em_pt_offset`, else 0) |
| `graph.p_lifetime.min/max` | +0x5C | lifetime, at least 0.04 s each, uniform between |
| `graph.p_size.min/max` | +0x64 | size; with +LifetimeAndSizeLinked the same random number as the lifetime |
| `graph.p_vel_mult.x/y/z` | +0x6C | multiplies the per-frame move over the life |
| `range.cycle_length` | +0x78 | "lo hi": the cycle length, picked once per emitter |
| `val.cycle_shutdown_time` | +0x80 | share of the cycle that runs on after a shutdown (1.0) |
| `graph.em_start_time` | +0x84 | start age = cycle × em_start_time(random) |
| `graph.orbit_accel_in/along` | +0x88/+0x8C | pull to the emitter / along the velocity (unused by the files) |
| `val.stretchlength`, `val.stretch_percent_time`, `val.destretch_percent_time` | +0x90…0x98 | streaks (1, 0.3, 0.7) |
| `int.forward_axis` | +0xC8 | the normal of a +DontBillboard quad in the emitter's frame (2 = Z) |
| `int.num_tiles.u/v`, `int.start_tile_step`, `int.multiple_render` | +0xE0…0xEC | tile sheet, start-tile step, draw count |
| `graph.tile_advance` | +0xF8 | tiles advanced (floor) over the life |
| `graph.rotation`, `graph.start_rot_distribution` | +0x108, +0x10C | angle over the life (radians; scaled by 4/π at load to octants), spin: the distribution evaluated at a uniform random number (rad/s) |
| `val.grav_multiplier`, `val.air_resistance` | +0x110, +0x114 | see "A particle's frame" |
| `textures.name` | +0x118/+0x11C | a list; each particle picks one at random |
| `shapes.name` | +0x120 | meshes for XForm particles |
| `graph.emvc.r/g/b/a` | +0xC…0x18 | the emitter colour (default 1) |
| `enum.emvc.color_blend`, `alpha_blend` | +0x1C, +0x20 | Normal 0, Lighten 1, Darken 2, Difference 3, Multiply 4, None 5 |
| `graph.vertex_color.r/g/b/a` | +0x128…0x134 | the particle colour over its life |
| `graph.size_mul` | +0x138 | size factor over the life |
| `enum.obj/gnd/con_col_resp` | +0x13C…0x144 | collision response (index into 0x3B2170) |
| `int.emitter_to_spawn` | +0x164 | the emitter a particle carries (+EmitEmitters), −1 none |
| `enum.particle_type` | +0xD8 | Normal 0, Stretch 1, XForm 2 (Shape: as XForm) |
| `enum.particle_mode` | +0xDC | normal 0, RotatesOnly 2, TilesNoRotation 4, Tiles 6, StreamsNoRotation 8, Streams 10 |

Enum and flag names are compared over the name's length, in the table's order (so `HalfBounceAgeHalf` reads as
`HalfBounce`); case does not matter (strncasecmp 0x32D1B0 over the table name's length, keys strcasecmp) [R]. Also read: `enum.sound_2d`,
`enum.sound_loop_2d` (sounds), `val.cost_*`, `int.num_emitters_reserved`, `graph.texture_center/radius` (+0xF8…
+0x104; centre 0.5, radius 1 by default; no file uses them), the old `rgb.vertex_color` (0…255) and
`graph.vertex_alpha`.

## Flags
`flags.emitter.def` and `flags.particle.def` go through one reader: `+Name` sets, `-Name` clears, a token without
the sign is skipped (so `"AutoStart UsesPercentStretchLength BreaksRelativityUponShutdown"` and `TestZBuffer
UseDiscreteRotation` change nothing: those are the defaults).

Emitter (+0x0, default 0x221): AutoStart 0x1, CycleLoops 0x2, LifetimeAndSizeLinked 0x4, Stationary 0x8
(particles do not take the carrier's speed), EmitterRelativeParticles 0x10, BreaksRelativityUponShutdown 0x20,
EndOfLifeUponShutdown 0x40, EmitEmitters 0x80, RelativeDistanceLimit 0x100, UsesPercentStretchLength 0x200,
ProvidesCollisionRod 0x400, PredictableEmissionRate 0x800, RandomizeEmitterXF 0x1000, OneTimeEmission 0x2000,
RenderAsDatapoints 0x4000, EllipsoidalStartArea 0x8000, SurfaceStart 0x10000, UseWorldMatrix 0x20000 (the axes
are the world's, not the effect's), UsesDatasetGenerator 0x40000, XF_AgeNotDistribution 0x800000,
XF_RotatesAroundX/Y/Z 0x1000000/2000000/4000000. StretchEmissions, UsesStretchInformation: particle type Stretch.

Particle (+0xD0, default 0x4001): TestZBuffer 0x1, WriteZBuffer 0x2, AdditiveAlpha 0x4, LoopingTextureAnimation
0x8, CollidesWithGround 0x10, CollidesWithBuildings 0x20, CollidesWithObjects 0x40, UsesTinyParticles 0x80,
NoRender 0x100, DontBillboard 0x200, CollidesWithWater 0x400, RandomStartTile 0x1000, RandomStartRotation and
RandomizeInitialRotation 0x2000, UseDiscreteRotation 0x4000, CycleWithinStartArea 0x20000 (tiles cycle within
the start tile's step), LinearAirResistance 0x40000, StretchAlternateAlignment 0x80000,
StretchCrosshatchRendering 0x100000, ParticlesDieWithEmitter 0x200000, StretchRealignDirection 0x1000000.
Derived at load: 0x10000 "does not rotate" (mode without bit 2, or rotation and spin all 0), 0x8000, 0x400000 /
0x800000 (the colour / alpha graphs are constants).

## An emitter (start 0x1F7F90, frame 0x20EFD0)
- **Start**: cycle = lo + r (hi − lo); age = cycle × em_start_time(r) − dt; a random rotation with
  +RandomizeEmitterXF; a sound. Only +AutoStart emitters start with the effect; the others are carried
  (+EmitEmitters).
- **Frame** (dt = the effects world's +0x64 × +0x10: 1 / the display rate 0x4CB550 and 1.0, set at its init 0x309210
  [R]; the rate is 60, or 50 in PAL mode without option flag 0x80 (0x25E1A0): this disc is PAL, so a 50 Hz game steps
  its effects at 1/50 s, gravity still per frame; the port takes 60): age += dt; past the cycle with +CycleLoops (and not
  shut down) age −= cycle; past it with +ParticlesDieWithEmitter all its particles go. A carried emitter sits on
  its particle.
- **How many**: +OneTimeEmission: n = gen_rate(0) on the frame age ≤ dt, else 0. Otherwise rate = gen_rate(age /
  cycle) while age ≤ cycle and not shut down; +PredictableEmissionRate keeps a running count (n += rate dt),
  otherwise n = rate dt + r (a random rounding). Particles are made while n ≥ 1.
- **A particle** (once per frame the graphs above are read at age / cycle): p0 in the start area (0x212A20: a
  box (2r − 1) per axis; +EllipsoidalStartArea up to 16 tries for a point in the unit ball, else (0, 0, 0.2);
  +SurfaceStart on its surface; on a box a face picked by area, a segment its ends); position = p0 + em_pt_offset
  through the emitter's axes; velocity = normalise(p0) × vel_from_start + em_vel + em_vel_r (2r − 1), through the
  axes, plus the carrier's velocity (unless Stationary / EmitterRelative). Unless Stretch or Predictable it starts
  −r dt old and that much back along its velocity (spread over the frame). Lifetime, size, a texture (r mod
  count), a start tile (RandomStartTile: (r mod ⌊u v / step⌋) × step), with RandomStartRotation and a rotating
  mode a start angle (byte, 2π / 256) and a spin (start_rot_distribution(r)), the emitter colour emvc(age /
  cycle). Inserted in the list before the first particle of its texture or a later one (0x211970).
- **Done** when it has no particles and is shut down, or does not loop and is past its cycle.
- **Shutdown** (0x1F7CD0): no more particles; +EndOfLifeUponShutdown ends the live ones.

## A particle's frame (0x20F9B8…)
1. age += dt; gone when age > lifetime + 0.0001.
2. Move (0x211110): pos += dt × velocity (× p_vel_mult(t) per axis). Stretch: while t < stretch_percent_time the
   head runs ahead (length += dt |v|) and the tail (pos) stays; after it the tail moves; past
   destretch_percent_time the length shrinks by dt |v| (the head stops). +StretchRealignDirection: direction =
   velocity.
3. The ground (0x2106B0, +CollidesWithGround): height h above the ground, radius r = size × size_mul / 2. With the
   response's row (c0 … c5) of the table at 0x3B2170: when h − c0 r < 0 and the particle moves down (v·n ≤ 0.01):
   v = −c1 (v·n) n + c2 (v − (v·n) n), it is put back to h = c0 r, and its age = c3 + c4 age + c5 lifetime (dead when
   past the lifetime).

   | Response | c0 … c5 |
   |---|---|
   | Nothing 0 | 0 −1 1 0 1 0 |
   | Bounce 1, HalfBounce 2, QuarterBounce 3 | 1 1/.5/.25 1 0 1 0 |
   | Slide 4, Stop 5 | 1 0 1 0 1 0, 1 0 0 0 1 0 |
   | Delete 6, DeleteCheckCenter 7, DeleteCheckEntire 8 | 1/0/−1 0 0 0 0 2 |
   | AgeHalf 9, AgeQuarter 10, AgeHalfEnd 11, AgeQuarterEnd 12 | 1 −1 1 0 1 .5, …, 1 −1 1 0 .5 .5, 1 −1 1 0 .75 .25 |
   | (Bounce, HalfBounce, QuarterBounce)(AgeHalf, AgeQuarter, AgeHalfEnd, AgeQuarterEnd) 13 … 24 | the same with c1 1 / .5 / .25 |
4. Gravity (0x2105E0): v.z += −196.2 × grav_multiplier, once per frame (no dt).
5. Air (0x2118E0, 0x210450): v ×= max(0, 1 − dt × air × |v|), or with +LinearAirResistance max(0, 1 − dt × air).

## Drawing (0x2214D0, 0x21C060)
- t = age / lifetime. Size = size × size_mul(t), the quad's full width: its two axes are size × size_mul(t) times the
  current camera's right (+0x30) and up (+0x3C) (0x22233C … 0x2223D8), the corners (±0.5, ±0.5) (0x3B2480) [R]. The
  effect's matrix never reaches these axes, so a scaled effect's billboards keep their size (only their places
  scale). A streak (0x1F7500) [R]: tail and head (+ length × direction) go through the matrix, so its length scales,
  but its width is size × size_mul (0x1F769C), laid in camera space after the transform, so it does not. The ground test and the bounding box use half the size as the radius.
- **DontBillboard** (0x222C08) [R]: not relative, the quad lies in the world's x y plane (the draw's model matrix ×
  size), forward_axis unused (Barf_pool, chocolate_pool, the Carbinator's glows); relative, rows 0 and 1 of (emitter
  × effect) × B, B by forward_axis: 2 identity, 1 rows (1, 0, 0), (0, 0, 1), (0, −1, 0), 0 rows (0, 1, 0), (−1, 0,
  0), (0, 0, 1); then the spin (0x223344). The relative ones scale with the effect.
- Colour: rgb = vertex_color(t), alpha = vertex_color.a(t), combined with the emitter colour the particle got:
  Normal / None: as is; Lighten v + e; Darken e − v; Difference v − e; Multiply v × e; then clamped to 0 … 1. None
  with constant graphs gives the emitter colour (0x213420). Sent as 128 × colour (128 = 1.0: the texture as is).
- Angle (modes RotatesOnly, Tiles; 0x2225E0): rotation(t) + spin × age + start angle.
- Texture: the particle's texture; uv 0 … 1, or with Tiles / TilesNoRotation the tile start + ⌊tile_advance(t)⌋
  (with +CycleWithinStartArea modulo the step) of a u × v sheet, read row by row.
- AdditiveAlpha: additive (dst + src × a), else alpha blended; TestZBuffer (default) tests depth, WriteZBuffer
  (never set) writes it. NoRender: not drawn (the sound emitters). int.multiple_render: drawn that many times.
  DontBillboard: the quad lies in the emitter's frame, normal = `int.forward_axis`. Stretch: a streak from the tail
  to the head, size wide.
- Textures are the bundle's type-9 entries of those names (fx.bdp holds all of them).

## XForm particles (meshes)
`enum.particle_type = XForm` (11 emitters in 6 effects: `Debris_sodamachine`, `Debris_Crate`, `Bean_debris`,
`Debris_megapunch_part2`, `Midway_Sign_explode`, `smokestack_pieces`) draw a shape instead of a quad.
- **Shapes**: `shapes.name` is a list like `textures.name`; at load each name becomes a refcounted SimpleShape
  (def +0x120 count, +0x124 array) [R 0x1F91F8]. All ten are models of fx.bdp: `<name>.pwk` (type 16) and one mesh
  `<name>|<object>` (type 0) each, no node tree (`BeanCan`, `ScumSodaCan`, `girder`, `Midway_M/I/D/W/A/Y`,
  `SmokeStackRing_02`). Sizes are the meshes' own (a can is 0.5 tall, a girder 2 long).
- **A particle** (0x2119D0, called for every new particle; it returns unless the type is XForm) [R]: shape = r mod
  count (byte +0x29, 0x212250). The axes are the `XF_RotatesAroundX/Y/Z` flags (a mask; n of them). Start turn: with
  +RandomStartRotation and n > 0, 2π r about an axis among them (n = 1: that axis; else each enabled component
  2r − 1, the third random number on x and the first on z, the mask if that is near 0, normalised), else none; it
  is taken into the world through the emitter's matrix (and the effect's unless Stationary; EmitterRelative keeps
  it in the emitter's frame and multiplies at draw), stored as a quaternion (+0x4C). Tumble: none when n = 0 or the
  `start_rot_distribution` graph is the constant 0; else an axis picked the same way (+0x5C) and a speed
  start_rot_distribution(r) (+0x68; at the emitter's cycle point with +XF_AgeNotDistribution).
- **Drawing** (0x21B330 / 0x21DC20, the shape branch) [R]: per particle s = size_mul(t); s ≤ 0 is skipped; the
  renderer state at 0x4CB52C gets {flags, 1, s} (0x21B740; renderer +0xAC: u16 flags, bit 0 "clip needed", u16, float,
  copied to 0x4CC040) [R]: s only scales the shape's culling sphere (0x184BB0 -> 0x16DC90 -> 0x16DD30: state +0xAE = 1
  from 0x21B74C multiplies the radius by +0xB0, 0x16DDA8) and skips s ≤ 0; 0x4CC040 is written and never read (the
  matrix upload 0x263210 sends 0x4CBFC0 .. 0x4CC03F) and the particle's matrix carries no s; the
  matrix is quat(+0x4C) × quat(axis +0x5C, speed × age) with the particle's place (× the emitter's matrix when
  relative): B × RowRot(axis, rate × age), the tumble after the start turn about an axis fixed in the world
  (0x21B8EC … 0x21B914; the axis kept raw, 0x2119D0); then the shape's vtable +0x10 draws it (SimpleShape 0x184BB0: culled by its sphere, drawn like a
  prop). No particle colour or alpha is applied, so the debris does not fade. The particle size is only the ground
  test's radius.
- `src/fx.c`: `fx_shape_*`, `fx_meshes` (a column-major model-to-world matrix per particle): the tumble × the start
  turn; a relative emitter's axis is taken into the world when the particle starts.

## Sounds
`enum.sound_loop_2d` (def +0xA4) and `enum.sound_2d` (+0xB0) keep the name after the `|` [R 0x1FE1D4]. When an
emitter starts (0x1F7F90) each that is set and not "No Sound" is started with add3DSfx (0x26D560) at the effect's
place, owned by the emitter (+0x0 / +0x1) [R 0x1F84C0]; both are killed (0x26D530) when the emitter shuts down
(0x1F7CD0) or ends (0x1F7E40). Whether a sound loops is the sound event's own business. `src/fx.c`: `fx_on_sound`.

## Collisions
ParticleSystemFx::advance (0x1F2A48) builds a mask per emitter from the particle flags: CollidesWithGround 1,
CollidesWithWater 2, CollidesWithObjects 8 (CollidesWithBuildings is not asked for), and asks the world
(gp −0x6E8C → +0x6C, vtable +0x2C) for the surfaces within 5 + 1.3 × the effect's radius of it, kept at effect +0x74;
the ground test (0x2106B0 → 0x206140) checks a particle against up to two planes of that set. No effect sets
anything but CollidesWithGround, and every `obj_col_resp` / `con_col_resp` is `nothing`, so a ground plane is all
the effects need.

## Ground-level quads and the depth test (the Las Vegas tank's "square" smoke)
`Carbinator_Shockwave` (the tank boss's blast, at the tank's place on the street, z = 0) has 100–500 `Suds_3`
billboards a second centred on the ground (start_area.z 0, size up to 6) and a `basic` glow lying flat on the
ground (+DontBillboard, forward axis Z, size 10 × (1 + 10t)); `Carbinator_Run` / `_Charge` add `smoke_white` puffs at
0.25–0.6 above it. Depth tested (+TestZBuffer, the default), the lower half of each billboard is behind the street
and the flat glow z-fights with it, so the street's and the curb's straight edges cut them into steps. The textures
are right (tex.c: `Suds_3` border alpha ≤ 30/255, `smoke_white` ≤ 26/255, `basic` is black at its corners and drawn
additive); without the depth test the foam is round. How the PS2 avoids it (the VU1 billboard code, or how the
street writes Z) was not read.

## Not done / not known
- Streams modes (no effect uses them), RenderAsDatapoints, UsesTinyParticles (drawn as ordinary quads here),
  `RelativeDistanceLimit`, the orbit graphs.
- The VU1 microcode: the billboard's exact corner layout, uv orientation, UseDiscreteRotation, the depth it tests.
- Which of the two update variants (0x21AD40's template match against 0x4CAE10) the XForm emitters get: only the
  order of random draws could differ (meshes use neither the angle nor the spin).
