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
| drawing | member pointer at +0x174 (0x2213F0, 4 specialisations): billboards 0x21D798 / 0x220768 → 0x2214D0 (VU1 packets), streaks 0x21C7E0 / 0x21F7B0 (0x1F6CA0 / 0x1F7500), shapes (XForm) 0x21B330 / 0x21DC20, streams 0x21C060 (0x1F4B90) |
| emitter start | 0x1F7F90; shutdown 0x1F7CD0 |
| `CompositeFx` | vtable 0x446830: `int.num_edfs`, `edf.name.N` (the parts by name, ".edf" optional), all started together |

The specialisations are C++ template instances on the definition's features (OneTimeEmission, normalisation,
textures, …); they compute the same thing.

## The text
Each line is decoded to words separated by one space, `=` stays [R 0x1ED400]: `?text?` and `?text` are literals
(a literal may hold spaces), two capitals a dictionary word. Keys are compared to the key table; a flag line is a
list of tokens. `src/fx.c` decodes all 287 effects of fx.bdp to exactly the text `tools/edf.py` prints.

## The header: priority, flags and the effects budget
The keys before the emitters go to the definition (FxLightWeightBase::+0x10, 0x1EC960) [R]: `enum.fxPriority`
High 0 / Medium 1 / Low 2 (+0x20; 1 when absent, the constructor 0x1E7988), `flags.fxFlags` (+0x24: `+` sets, `-`
clears; AdvanceOnlyWhenVisible 1, DeleteWhenNotVisible 2, SupposedToBeLarge 4; another name is "Unrecognized
flag"), `val.fxRadius` (+0x28) and `val.fxCullDist` (+0x2C). In the data: fx.bdp's 287 effects are 86 High, 171
Medium, 29 Low and one without (Chocolate_tower_explode: Medium); one flag each on Clouds_NYC
(AdvanceOnlyWhenVisible), smokestack_small (DeleteWhenNotVisible), Powerup_good_Bling (SupposedToBeLarge); no file has
fxRadius or fxCullDist.

**The budget** (the object at 0x4D0740) [R]: +0x0 the Medium limit, +0x4 the Low limit, +0x8 / +0xC / +0x10 the live
effects by priority. RWorld +0x34 (0x2B72B0, the world's start) sets the limits (0x292760): with two monsters or more
in the world (the players and bots 0x4D7E28, plus the one to reveal) Medium 40 and Low 30, else 50 and 35. canStart
(0x2926D0): High always; Medium while High + Medium <= the Medium limit; Low while High + Medium + Low <= the Low
limit. The callers ask it before they start an effect and count it while it lives (0x2926A0 add, 0x292670 remove):
FollowEffect's create (0x292050, every script's createAndStart*, the world's own) and update (0x290F60, the counts
0x291920 / 0x291180), the projectiles (0x293120, 0x2944F0), the weapons (0x295750 .. 0x2978D0), the building cells'
debris (0x30FD40, 0x310260), the shockwave (0x31BF20). Refused, nothing is made (a FollowEffect not even made: the
script gets no effect). The monster's own (its foot plants 0x2F4A20 / 0x2F4B70, landings 0x2F48D0, the rage
0x2F4710), a composite's parts (0x1EB0A0) and the GuiEffects (0x2377E0) start without asking and are not counted.

**The flags**: FollowEffect's init (0x291AF8) makes AdvanceOnlyWhenVisible its +0xF4 0x40 and DeleteWhenNotVisible
0x80. Its update (0x290F60) then tests the sphere at its place (radius fxRadius, else the effect's own bounds +0x24:
for a ParticleSystemFx half the diagonal of its particles' box, 0x1F2F68) against the camera frustum (0x16C0A0: four
side planes, near and far) and, with fxCullDist, the camera's distance; out of view a 0x40 one is neither advanced
nor drawn (+0xA8 &= ~0x78), a 0x80 one is released and made again (within the budget) once in view, and is made
only in view to begin with (0x2920A0 skips the budget and the effect). SupposedToBeLarge is read by nothing.

The port (src/fx.c): the keys are read (`Effect.prio`, `.hflags`, `.radius`, `.cull_dist`); `fx_budget` (main.c's
play_step, by the monsters in the world) turns the budget on and `fx_spawn` asks it and counts what it starts while it
lives; `fx_spawn_free` is for the monster's own (main.c's foot plants and landings). The visibility flags are not
ported: nothing ever starts Clouds_NYC, and smokestack_small is only the `Effect|AttachEffect` of the template
`ExampleShape` (Objects1_props), which no block places.

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
| `val.cycle_shutdown_time` | +0x80 | at a shutdown the age is stepped on to cycle × this (1.0): 1 ends the cycle at once, 0 lets it run out |
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
RandomizeInitialRotation 0x2000, UseDiscreteRotation 0x4000 (no code reads it: the angle is always the octant
sine series below), CycleWithinStartArea 0x20000 (tiles cycle within
the start tile's step), LinearAirResistance 0x40000, StretchAlternateAlignment 0x80000,
StretchCrosshatchRendering 0x100000, ParticlesDieWithEmitter 0x200000, StretchRealignDirection 0x1000000.
Derived at load: 0x10000 "does not rotate" (mode without bit 2, or rotation and spin all 0), 0x8000, 0x400000 /
0x800000 (the colour / alpha graphs are constants); UsesTinyParticles is cleared (0x1F9800: flags & ~0x80), so the
31 emitters that set it draw as the others [R].

## The step [R]
An effect is advanced once per step of what owns it, by the step of the effects adapter (gp −0x6E8C: +0x64 the step,
+0x10 a factor 1.0, +0x60 the rate). Two set it:
- **In play** the world's start (RWorld +0x2C 0x2B74D0 → 0x2852E0) hands the world's adapter its rate +8 and step +4
  (vtable +0x54, 0x282260), 30 and 1/30 (25 and 1/25 below a 51 Hz display) from RWorld's init 0x2B7800, and makes it
  the one at gp −0x6E8C. A FollowEffect is a world object: its update (secondary vtable +0x10, 0x290F60) advances its
  effect (+0x3C, 0x2913E8) in the world's object pass (0x284E40, from RWorld's update 0x2B6A30), once a tick; RWorld's
  frame (0x2B7A70) runs the ticks by the real time in whole 1/30 s steps (+0x608), two at most a drawn frame. The
  GuiEffects (0x237590) take the same adapter's step. So every effect in play moves 1/30 s a step, 30 steps a second.
- **In the front end** 0x309210 (from 0x309500) gives its adapter 1 / the display rate (0x4CB550: 60, or 50 for a PAL
  game without option flag 0x80, 0x25E1A0) and the front end's loop (0x3093B0) runs at that rate: 1/60 s a step.
Per step and not per second: gravity (below), the random rounding of the emission (rate × step + r), the spread of new
particles back over the step (−r × step), the start age (− a step) and the shutdown's steps. The port: `fx_set_step`
(main.c: the world's effects and the popup's GuiEffects 1/30, the front end's 1/60); fx_update runs whole steps from
the frame's time and draws each particle between its last two (`Part.prev`: its place, streak length and age eased
by the leftover time), at most a step behind, as the port's other 30 Hz objects (FLOW_NOTES.md "The world's step").
Up to v0.10.18 the port stepped every effect at 1/60: the particles fell (or rose) under twice the game's gravity
(`car_hit` started 10 above the street, 1 s in: its sparks' mean height 1.5, against 10.8 at the game's step: on
the ground instead of in their arc).

## When the object goes [R]
A FollowEffect started on an object (createAndStartAttached*, 0x291ED0 → 0x292050 → 0x291A20) follows it while the
object is valid (its serial, 0x291440). When the object is gone: with setTurnOffWhenObjectDies(1) (+0xF4 bit 8) the
effect is shut down (vtable +0x30, 0x291238 .. 0x291264: no more particles, the live ones finish), without it the
effect keeps its last matrix and runs on (a looping one for good). An Actor's destructor (0x30DF80) shuts down its
AmbientEffect (+0x108) and ChallengeEffect (+0x110) itself (0x30E128 .. 0x30E1DC), the people's and the army units'
(people.c `gone`, army.c `die_with`). A CycleLoops emitter never ends until shut down; another ends when it has no
particles and is past its cycle (0x20EFD0's answer), and the effect with its last emitter (fx.c `fx_update`).

At a block's end activateNextBlock's sweep (0x2B66C0) deletes every object of the world's list but the monsters'
weapons (flag 0x200000), and a FollowEffect is one (the world's add, vtable +0x3C 0x284CD0, puts it on both lists,
+0x58 and +0x50): its destructor (0x292270) takes it off the budget and hands its effect back to the FxManager
(0x207A40), which destroys a ParticleSystemFx and puts it on the free list there and then (type 7, 0x207AB8). The
sweep's 0x302800 deletes the block's buildings too (0x4D0958 / 0x4D095C, destructor 0x2BF490), and a building's
destructor runs its spawner's (0x310810), which releases every effect the spawner holds the same way. So the old
block's effects are gone at once, particles and all, as the results start: the scripts', the objects', the monsters'
clip events' and the buildings' (their cells' debris, the floors' and the pre-collapse dust). What the game starts
straight from the FxManager outside a building (the monster's foot plants, landings and rage, the weapons') is not
swept. The port: fx.c `Fx.of_block` (fx_spawn's; not fx_spawn_free's, the monsters' weapons', the weather, the bosses',
which the port keeps), main.c `block_left` calls `fx_drop_block`; it had shut down only the effects it tracks, and
those (and every building effect) faded out over the Results.

**The buildings' spawner** (+0x8F4; 0x3101C0 by key → 0x310260, 0x30FD40 by name at a point) [R]: a record (0x58
bytes, a list at +0x0 / +0x4, next +0x4C) per effect: the flags (+0x0), the key (three words, +0x4: face, storey,
column), the cell's place (+0x10, 0x30F710: the face's rotation, the place +0x34, 1.5 up the storey +0x40, the storey
+0x44), the effect (+0x54); the building goes on the world's update list (0x4D17F0 / 0x4D17F4) and its update
(0x2BDC70 → 0x310660) advances them, releasing each that is done. Every building caller passes flags 0: the cells'
hits (0x2BBBA4, 0x2BBCB0, 0x2BBE3C: `daddu a3, zero, zero` in the delay slot, which 0x3101C0 hands on as 0x310260's
a3, stored at +0x0 by 0x3104F8; up to v0.12.1 read as −1), the front's crack (0x2BDB08, 0x2BDBC8), the floor's and
the pre-collapse dust (0x2BD784, 0x2BD8EC); the hit debris (0x30FD40) makes its record itself with flags 0
(0x31007C). So the two flags below are never set on a building's records: each debris effect plays out where it was
made. Flag 2: a new effect, once made (0x208220),
shuts down (vtable +0x30) the flag-2 ones of the same key still on the list (0x3103D0 .. 0x31044C), so one hit's
first crack, Wall_Small and Wall_Large leave the last one going. Flag 1: while the building is not falling (grid
+0x1CC = INT_MAX, 0x2CE230) the effect is put back on its storey each update (0x30F690, z alone: the record's storey,
or, that one fallen, the next one still standing up, kept; at that storey's place now, 0x2CD010: 6 + 3 x its live place,
less 3 x the progress at and above a falling one; the effect's setPosition, vtable +0x68: the emitters move, the
particles out stay), so a cell's debris comes down with its storey once a floor below it has fallen (not during the
fall: +0x1CC is the falling storey then). The spawner's start places every record so (0x3104BC). The port:
`Building.debris` carries the flags (`src/building.c` `debris`), `src/city.c` asks for the cells' effects with their key
(`fx_request`), main.c's `fx_step` hands each id back (`scene_fx_started`: the flag-2 stops, the records of flags 1 and
2, `SceneBuilding.spawn`) and `scene_fx_follow` is the update (`fx_move`). Checked: NY03, building 1, a hit on storey
2 then storey 0's cells till the floor falls: the storey-2 debris 13.5 → 10.5 as the fall ends, the storey-1 cracks
10.5 → 7.5; each repeat hit's Wall_Large shut down the cell's crack.

## An emitter (start 0x1F7F90, frame 0x20EFD0)
- **Start**: cycle = lo + r (hi − lo); age = cycle × em_start_time(r) − dt; a random rotation with
  +RandomizeEmitterXF; a sound. Only +AutoStart emitters start with the effect; the others are carried
  (+EmitEmitters).
- **Frame** (dt = the effects adapter's +0x64 × +0x10, 0x1F2938: "The step" above): age += dt; past the cycle with
  +CycleLoops (and not shut down) age −= cycle; past it with +ParticlesDieWithEmitter all its particles go. A carried emitter sits on
  its particle.
- **How many**: +OneTimeEmission: n = gen_rate(0) on the frame age ≤ dt, else 0. Otherwise rate = gen_rate(age /
  cycle) while age ≤ cycle and not shut down; +PredictableEmissionRate keeps a running count (n += rate dt),
  otherwise n = rate dt + r (a random rounding). Particles are made while n ≥ 1.
- **A particle** (once per step the graphs above are read at age / cycle): p0 in the start area (0x212A20: a
  box (2r − 1) per axis; +EllipsoidalStartArea up to 16 tries for a point in the unit ball, else (0, 0, 0.2);
  +SurfaceStart on its surface; on a box a face picked by area, a segment its ends); position = p0 + em_pt_offset
  through the emitter's axes; velocity = normalise(p0) × vel_from_start + em_vel + em_vel_r (2r − 1), through the
  axes, plus the carrier's velocity (unless Stationary / EmitterRelative). Unless Stretch or Predictable it starts
  −r dt old and that much back along its velocity (spread over the frame). Lifetime, size, a texture (r mod
  count), a start tile (RandomStartTile: (r mod ⌊u v / step⌋) × step), with RandomStartRotation and a rotating
  mode a start angle (byte, 2π / 256) and a spin (start_rot_distribution(r)), the emitter colour emvc(age /
  cycle). Inserted in the list before the first particle of its texture or a later one (0x211970).
- **Done** when it has no particles and is shut down, or does not loop and is past its cycle.
- **Shutdown** (0x1F7CD0): no more particles; +EndOfLifeUponShutdown ends the live ones. The emitter's age is then
  stepped on by the frame's step to cycle × `cycle_shutdown_time` (0x1F7D40 .. 0x1F7D80), 1.0 unless set (97 + 52
  emitters set 1, 9 set 0): by default the cycle is over at once, so +ParticlesDieWithEmitter particles go the next
  frame. The edibles' looks (`Powerup_*`, `Hazard_Fire`) are such, so an edible deleted with its floor vanishes; the
  port ran the cycle out (up to 2 s) and the food and fire hung where the window had been.

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
4. Gravity (0x2105E0): v.z += −196.2 × grav_multiplier, once per step (no dt): in play −5886 × grav_multiplier a
   second², in the front end twice that.
5. Air (0x2118E0, 0x210450): v ×= max(0, 1 − dt × air × |v|), or with +LinearAirResistance max(0, 1 − dt × air).

## Drawing (0x2214D0, 0x21C7E0)
- A particle's texture is **clamped** at its edges [R]: the emitter's material (0x1F8870) is shader 0x57 or 0x55,
  whose first pass's repeat (descriptor +0x8D) is 0 and record wrap 0x10, so the pass setup (0x265840 → 0x26AA20)
  makes the texture's CLAMP_1 CLAMP (FORMAT_BDP.md "Repeat or clamp"; render_gl.c `draw_fx`).
- t = age / lifetime. Size = size × size_mul(t), the quad's full width: its two axes are size × size_mul(t) times the
  current camera's right (+0x30) and up (+0x3C) (0x22233C … 0x2223D8), the corners (±0.5, ±0.5) (0x3B2480) [R]. The
  effect's matrix never reaches these axes, so a scaled effect's billboards keep their size (only their places
  scale). A streak (0x1F7500) [R]: tail and head (+ length × direction) go through the matrix, so its length scales,
  but its width is size × size_mul (0x1F769C), laid in camera space after the transform, so it does not. There d is
  the head's direction from the tail in the camera's x y ((−1, 0) when that is no longer than √0.001), n = (−d.y, d.x),
  w half the width; the corners tail + w(n − d), tail − w(n + d), head + w(n + d), head + w(d − n): the quad runs w
  past each end. Their uv (0x3B2550, u in the low half): u 1 at the tail and 0 at the head, v 0 on the n side, so the
  texture's width lies along the streak (the `bomb`, its nose at the picture's right, falls nose first) [R]. The ground test and the bounding box use half the size as the radius.
- **DontBillboard** (0x222C08) [R]: not relative, the quad lies in the world's x y plane (the draw's model matrix ×
  size), forward_axis unused (Barf_pool, chocolate_pool, the Carbinator's glows); relative, rows 0 and 1 of (emitter
  × effect) × B, B by forward_axis: 2 identity, 1 rows (1, 0, 0), (0, 0, 1), (0, −1, 0), 0 rows (0, 1, 0), (−1, 0,
  0), (0, 0, 1); then the spin (0x223344). The relative ones scale with the effect.
- Colour: rgb = vertex_color(t), alpha = vertex_color.a(t), combined with the emitter colour the particle got:
  Normal / None: as is; Lighten v + e; Darken e − v; Difference v − e; Multiply v × e; then clamped to 0 … 1. None
  with constant graphs gives the emitter colour (0x213420). Sent as 128 × colour (128 = 1.0: the texture as is).
- Angle (modes RotatesOnly, Tiles; 0x2225E0): rotation(t) + spin × age + start angle, in octants (× 4/π; the start
  byte × 1/32), its whole part & 7 and the rest into 0x105E30's series: the sine at +0x478, the cosine at +0x47C. The
  quad's two axes A (right × size, or the DontBillboard axes) and B (up × size) become c A + s B and −s A + c B
  (0x223344: rows (c, s, 0), (−s, c, 0), (0, 0, 1) times them, 0x101A90), a turn counter-clockwise on screen [R].
- Corners and uv [R 0x221E50, 0x3B2480, 0x3B24D0]: the four corners (0.5, −0.5), (0.5, 0.5), (−0.5, −0.5), (−0.5, 0.5)
  along A and B, their uv (4.12 fixed point) texture_center ± texture_radius / 2 so that u = 0.5 + x, v = 0.5 − y
  (centre 0.5, radius 1 as no file sets them): v 0 at the top, the texture's first row.
- Texture: the particle's texture; uv 0 … 1, or with Tiles / TilesNoRotation the tile start + ⌊tile_advance(t)⌋
  (with +CycleWithinStartArea modulo the step) of a u × v sheet, read row by row.
- AdditiveAlpha: additive (dst + src × a), else alpha blended; TestZBuffer (default) tests depth, WriteZBuffer
  (never set) writes it. The GS state [R 0x221700..0x221790]: the shader 0x3B2660 (vertex format 0x412: STQ, RGBAQ,
  XYZ2; VU1 program 0x3A75C8 .. 0x3A7D78 for both its plain and clipping slots), the registers 0x3B26C0 (additive:
  ALPHA 0x48, Cs × As + Cd) or 0x3B2700 (ALPHA 0x44), both TEST 0x51001 (depth test GEQUAL; the alpha test fails every
  pixel with AFAIL frame buffer only, so no depth is written whatever WriteZBuffer says), PRMODE 0x58 (Gouraud,
  textured, blended, no fog); -TestZBuffer switches the depth test off (0x25A900).
  `render_gl.c` draws such a particle in the world tested at its depth pulled towards the eye by its size (+ 1):
  90 of the 287 effects have one (Car_explode's flash and fire among them) and the game shows none of them through
  the buildings [G: its effects drawn in the world's order; that draw is not traced], while what it starts in (the
  car, the street) does not cut it. The front end's 3D effects keep the test off.
- The VU1 program (0x3A75C8, `tools/vudis.py`) [R]: the CPU builds every corner (0x2214D0); the program only takes them
  through the matrix at VU1 0x89, divides by w, packs uv (ITOF12) and multiplies each vertex's alpha by the world's
  fog factor: alpha × min(127.5, F × 0.5) / 128, F = a + b w the 0 … 255 fog value the world's programs (0x37A2A0)
  send the GS, from the same VU1 constants (+0, +21). The GS draws the particles unfogged, so a fogged particle fades
  out instead of taking the fog colour. No depth bias, no special depth: the corners' own depth. A triangle with a
  corner outside the clip volume gets ADC (not drawn). `render_gl.c` draw_fx: the alpha times the linear fog's
  factor at each corner's eye-plane depth, GL fog off. NoRender: not drawn (the sound emitters). int.multiple_render: drawn that many times.
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

## The collision rod and RelativeDistanceLimit (beams)
An effect whose emitter 0 has AutoStart and +ProvidesCollisionRod (fx_rod) keeps at ParticleSystemFx +0x9C that
emitter's instance, at +0xA0 the rod's head and at +0xA4 its tail, both 0 at its start (0x1F2750) [R]:
- each frame after the emitters (0x1F2E60): head += rod speed × dt, at most the rod's length; tail = 0 while the rod
  emitter is neither shut down nor past its cycle (a CycleLoops one never is until shut down), else tail += speed × dt;
- the rod emitter stays alive while tail < head (0x1F2CE8); when it goes, +0x9C, the head and the tail are 0;
- a beam's hit at t along it calls the effect's vtable +0x80 (0x1F2520, from 0x295938, before the stream's own cut
  0x226DF0): head = t, and t below the tail drops the rod (the head then stays at t);
- vtable +0xB8 (0x1F1CF0) answers the head: the limit of the +RelativeDistanceLimit emitters (`Autocannon_fire`,
  `Machinegun_fire`, `Blimp_Laser_Charge`, `Blimp_Laser_Fire`, `Cyclone_Lightning`). At the end of the move
  (0x211670, 0x2117D4) a particle goes when, for a relative streak, its tail is the limit or more from its emitter,
  and its length is cut so that its head stops at the limit; for another streak, when age × speed (its head's run;
  compared squared) reaches it; for the rest, when its distance does (the game takes |pos| in the emitter's frame;
  no effect has a non-relative one). So a beam is drawn up to what it hit. `src/fx.c`: `Fx.rod`, `head`, `tail`,
  `fx_cut`; main.c calls `fx_cut` when the army beam's stream was cut this frame (its head short of its growth), with
  t = head − tail.

## Moving effects
Only the +EmitterRelativeParticles emitters' particles are drawn through the emitter's matrix (0x222400); the others
are in the world [R]. `fx_move` moves those back by the effect's move, so they stay (smoke trails stay behind), and
`fx_place` turns the relative ones (their place, velocity, streak direction, mesh turn) with the new frame. An
effect also has a velocity of its own (+0x5C, 0 from the constructor; set through vtable +0x94, 0x1F24E0) that the
emitters add to their non-Stationary, non-relative particles' (0x20F064, 0x212310; a carried emitter adds its
carrier's too) [R]. A CompositeFx hands it to its parts (0x1E9B28: the parts whose +0xE has bit 8, which a
ParticleSystemFx has, 0xB from 0x1F4608). Only a shot sets it, on its FireEffect (Projectile +0xD8): its velocity
(direction +0xB0 x speed +0xBC) at the launch (0x294D24) and after each step (0x293B80); no FollowEffect or other
caller does (the other +0x94 calls are on other classes). So a `Tank_Projectile`'s puffs (em_vel.y -50) travel on with
the shell and fall back from it at 50, not at 50 through the world. `src/fx.c`: `fx_vel`, main.c's shot effects.

## Effects on a monster's bones
The clip events of type 12 start and stop the effects of MONSTER_DATA's list in the monster's 32 effect slots, each a
FollowEffect on a node of the monster's skeleton (0x2F5800, 0x292010 → 0x291A20; FORMAT_MONSTER.md "Effect slots")
[R]. Stopping one is its effect's shutdown (vtable +0x30, 0x1F1C60 → 0x1F2410 → 0x1F7CD0 on every emitter): no more
particles, the live ones finish (`fx_stop`). main.c starts them in the node's whole frame (`fx_spawn_frame`) and
moves them with the bone each frame (`fx_place`).

## Building debris
The building's cells start their props' effects through the building's spawner (+0x8F4: 0x3101C0 → 0x310260 with the
key's index into the table 0x3F5990; `Glass_Small` 0x33 is never asked for) [R]:
- a cell hit (0x2BB950): its first crack (from state 0) `Glass_Large` (0x34) when its sheet column has an Outline (the
  column's +0xEC, 0x2D0500; the window glass pass's test), else `Wall_Small` (0x35); below 5 HP `Wall_Small`; at 0
  `Wall_Large` (0x36). A cell flagged 0x400 (FloorFlags, "hidden behind a front-row building") gets none at the first
  crack and at 0 (not kept in building.c: Chicago's 447 FloorFlags lists are all 0). One hit can give two or three.
- the destroyed front's crack (0x2BD7D0): a cell it raises to MinCollapseCrackLevel gets the first crack's effect when
  the world's Park-Miller number (gWorld +0x828, 0x106BD0) mod (gWorld +0x28 + 1) is 0 (0x2BDAA8, 0x2BDB68): that
  member is 1 (the world's init 0x285A20, from RWorld's 0x2B7800; its setter, vtable +0x14 0x284AF0, has no caller),
  so a cell gets it half the time [R] (`src/building.c` `crack_step`);
- a floor's collapse (0x2BAE70) none per cell, and none at all when its FloorHP ran out (0x2BB950 → 0x2BAE70):
  `Floor_Large` comes only from the destroyed building, at every other column's edge along its foot on the ground
  (key 0x38, the point {4, storey 0, along 5 × 1, 3, …}), once as the front has cracked (0x2BD7D0 → 0x2BD8EC) and
  again after every storey it drops, its sink too (0x2BD610 → 0x2BD784) [R] (`Building.pre_collapse`, `drop_dust`);
- a hit on the ground floor (face 4) of radius 2 or more by an attacker not an Actor: `Wall_Small` at the hit's own
  point (0x2BC840 .. 0x2BC890, through 0x30FCB0 / 0x30FD40) [R] (`Building.hit_debris`; a monster's kick at z 3).
They are placed in the cell's face frame (0x30F710: the matrix of the face from 0x430F10, the front's the identity) at
2.5 along and 1.5 up the cell. `src/building.c` queues them (`Building.debris`), `src/city.c` plays them.

## Sounds
`enum.sound_loop_2d` (def +0xA4) and `enum.sound_2d` (+0xB0) keep the name after the `|` [R 0x1FE1D4]. When an
emitter starts (0x1F7F90) each that is set and not "No Sound" is started with add3DSfx (0x26D560) at the effect's
place, owned by the emitter (+0x0 / +0x1) [R 0x1F84C0]; both are killed (0x26D530) when the emitter shuts down
(0x1F7CD0) or ends (0x1F7E40). Whether a sound loops is the sound event's own business. `src/fx.c`: `fx_on_sound`.

## Collisions
ParticleSystemFx::advance (0x1F2A48) builds a mask per emitter from the particle flags: CollidesWithGround 1,
CollidesWithWater 2, CollidesWithObjects 8 (CollidesWithBuildings is not asked for), and asks the world
(the FxWorldAdapter at gp −0x6E8C, its vtable at +0x6C, slot +0x2C: 0x281FC0) for the surfaces within 5 + 1.3 × the
effect's radius of it, kept at effect +0x74; the ground test (0x2106B0 → 0x206140) checks a particle against up to
two planes of that set. 0x281FC0 does not look at the mask: it empties the set (0x2060C0) and adds one triangle
around the effect's place at the street's height (SceneryMgr vtable +0x34, 0x282320: 0), so the street plane z = 0
is all a particle meets, whatever its flags and responses [R]. The adapter's world ray (+0x4C, 0x282210: 0x283730,
include −1, exclude 0) and its two height queries (+0x20 0x282130, +0x24 0x2820C0) have no caller: nothing in the
ELF calls its slots +0x1C .. +0x24 or +0x4C (+0x34 .. +0x48, which only the unused DynLightFx / ProjectedTexFx call,
are empty stubs) [R]. In the data no effect sets anything but CollidesWithGround,
and every `obj_col_resp` / `con_col_resp` is `nothing`. `src/fx.c`: the plane `fx_update`'s ground_z (main.c: 0).

## Ground-level quads and the depth test (the Las Vegas tank's "square" smoke)
`Carbinator_Shockwave` (the tank boss's blast, at the tank's place on the street, z = 0) has 100–500 `Suds_3`
billboards a second centred on the ground (start_area.z 0, size up to 6) and a `basic` glow lying flat on the
ground (+DontBillboard, forward axis Z, size 10 × (1 + 10t)); `Carbinator_Run` / `_Charge` add `smoke_white` puffs at
0.25–0.6 above it. Depth tested (+TestZBuffer, the default), the lower half of each billboard is behind the street
and the flat glow z-fights with it, so the street's and the curb's straight edges cut them into steps. The textures
are right (tex.c: `Suds_3` border alpha ≤ 30/255, `smoke_white` ≤ 26/255, `basic` is black at its corners and drawn
additive); without the depth test the foam is round. The particles' VU1 program has no depth trick (the corners' own
depth, GEQUAL: an equal depth passes) [R 0x3A75C8, 0x3B26C0]. The street writes that depth [R]: the block grounds'
parts are kind 2 materials (162 of 167 with flags 0xC: shader 1, one pass, GS state 1, TEST 0x50002, Z test and
write; the rest shader 2 and 0xC, Z written too; FORMAT_BDP.md "How a material is built"), so the billboards' lower
halves are behind it on the PS2 as well. `render_gl.c` pulls the flat quads in front of the street with a polygon
offset [G: for GEQUAL's equal-depth pass; how two coplanar draws round on the GS is the hardware's; seen only as
whether a flat glow flickers against the street].

## Not done / not known
- Not ported because no effect uses them: the Streams modes, RenderAsDatapoints (emitter 0x4000), the orbit graphs
  (fx.bdp's 287 effects and the menus' have none of them).
- Which update an emitter gets (0x21AD40 matches a descriptor of its features against the 3 templates at 0x4CAE10,
  the draw 0x2213F0 against the 4 at 0x4CAF80; the counts 3 and 4 are .data, gp −0x7DFC / −0x7DF8): both tables are
  bss, and nothing in the ELF writes them (no address into 0x4CAE10 .. 0x4CB140 but the two matchers' and the draws'
  reads); the update's pointers to member are constants at 0x3B2630 / 0x3B2640 / 0x3B2650 (0x20EFD0, 0x213F30,
  0x216470), which nothing reads either, so the descriptor that goes with each is not in the code. The update variants compute the same; they differ in the draws: 0x216470 picks the texture,
  tile and angle (0x2193A0, 0x219410, 0x219530: two randoms for a RandomStartRotation emitter, XForm or not), 0x20EFD0
  and 0x213F30 none of them (0x2122D0, 0x2119C0, 0x212240). The XForm emitters are taken as the latter [G: a player cannot tell; with 0x216470 only which random numbers the
  next particles draw would move].
