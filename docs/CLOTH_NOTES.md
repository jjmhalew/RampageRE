# Cloth

The hair, fur and whiskers of 16 monsters (`000<NAME>_CLOTH`, one per `.cmp`) and 28 city flags (`Flag_US`,
`FlagPole_red`, `Flag_LaundryLine` ...) are `MODEL|cloth` entries (type 25, [FORMAT_BDP.md](FORMAT_BDP.md) "Cloth"):
44 in all. `src/cloth.c` ports the game's simulation; this file is what was read to write it.

## The code

| Address | |
|---|---|
| 0x1DB840 | ClothEntry (vtable 0x446500) over the entry data: offsets +0x38, +0x40, +0x48 and each part's five array offsets to pointers |
| 0x19AE80, 0x1DAB40 | the `Cloth` resource (vtable 0x444DE0) around the entry, with a mesh per part |
| 0x1DA4B0 | ClothInstance setup: a 0x20-byte runtime record and a ClothRenderer (0x90 bytes, vtable 0x445280) per part, a 0x44-byte state per particle |
| 0x1D90E0 | **one frame** (instance, object matrix, dt) |
| 0x1D9B80 | the rest pose: every particle where its bones put it, still |
| 0x1D8360 | the pins onto their bones; their mean velocity and top speed |
| 0x1D7710 | one substep of a part: forces, semi-implicit Euler |
| 0x1D8CA0 | inverse dynamics: the springs' stretch cut back, velocities re-read |
| 0x1D87F0, 0x1D5E00 | collisions with the entry's spheres (none in the data) |
| 0x1D7490 | the triangles' normals |
| 0x1D6E30 | the particles' normals (when drawn) |
| 0x1D67B0, 0x1D89C0 | stop: snap to rest and blend from the kept positions; resume after |
| 0x1D68F0 | the bounding sphere (not ported) |
| 0x1D6740 | add a force for this frame (instance +0x60) |
| 0x3473B0 | ClothObject (a city flag, vtable 0x44BA50) each frame: its wind, then 0x1D90E0 |
| 0x246B10 ... | a monster's cloth: 0x1D90E0 with the monster's world matrix, no force |

Callers of 0x1D90E0: 0x246B10 (GuiShape), 0x29C0B0 (ClothBlob), 0x2CFB30, 0x2E03A8 (not followed), 0x3473B0 (ClothObject).

## The part record (0x8C bytes)

The names are the text format's keywords (0x41C458 .. 0x41C638, read by 0x1E1750 between `BEGIN_CLOTH_PROPS` and
`END_CLOTH_PROPS`); they are in field order and every value checks against its use.

| Offset | Name | Values | Use |
|---|---|---|---|
| 0x00 | GRAVITY | 9.8 flags, 0 most monsters, 100 Batty, Dick, Rhett | force −GRAVITY × MASS on z |
| 0x04 | TENSION | 100 flags, 200 monsters | baked into each spring's k |
| 0x08 | BEND | 20 | baked into each bend's k |
| 0x0C, 0x10, 0x14 | MIN_STIFFNESS, MAX_STIFFNESS, STIFFNESS_EXPONENT | 100/300, 200..500, 1 | baked into each particle's stiffness (+0x18) |
| 0x18 | DAMP | 4 flags, 5 monsters | baked into each spring's damping |
| 0x1C | DRAG | 0.2 flags, 0.5 monsters | air drag, quadratic |
| 0x20 | PARTICLE_MASS | 1 | |
| 0x24 .. 0x30 | RESTITUTION, STATIC_FRICTION, DYNAMIC_FRICTION, DYNAMIC_FRICTION_VEL | 0.1/0.2, 0.7/0.9, 0.9/0.98, 5 | collisions only |
| 0x34 | MAX_TIME_DELTA | 1/60 | the substep |
| 0x38 | RELATIVE_VEL_SCALE | 0.85 | the air moves with this much of the pins' velocity |
| 0x3C, 0x40 | EXTRA_DAMP, EXTRA_DAMP_VEL | 1, 10 | with USE_EXTRA_DAMP |
| 0x44 | MAX_VEL | 100, 150 | a pin faster than this resets the cloth |
| 0x48 | MAX_VEL_TRANS_TIME | 1/30 | the blend after that reset |
| 0x4C | NUM_INV_DYN_ITERS (u32) | 2, 3 | stretch passes |
| 0x50 | ADJUST_VEL (u8) | 1 | velocities from positions after the passes |
| 0x51 | DO_COLLISIONS (u8) | 0 | |
| 0x52 | CALC_BEND_SPRINGS (u8) | 1 flags | the bends were made |
| 0x53 | IS_STIFF (u8) | 1 monsters | springs to the skinned rest shape instead of the springs' forces |
| 0x54 | USE_EXTRA_DAMP (u8) | 0 | |
| 0x55 | CONNECT_DUPLICATE_VERTS (u8)? | 1 on Batty, Dick, Rhett | not read by 0x1D5920 .. 0x1DAAA0; +0x56, +0x57 0xEA padding |
| 0x58 | f32 | 100 | EXTRA_DAMP_VEL², stored |
| 0x5C | f32 | 0 | runtime: 1 / PARTICLE_MASS (0x1DA79C) |
| 0x60 .. 0x88 | counts and offsets | | pins, springs, bends, triangles, particles (count twice) |

A **spring** (24 bytes): u32 a, u32 b, f32 rest length, its square, f32 damping (DAMP), f32 k (TENSION).
A **bend** (32 bytes, flags only): u32 triangle t0, u32 triangle t1 (of the part), u32 p0, p1, p2, p3, f32 rest
angle (0, a few ±0.0003), f32 k (20). FORMAT_BDP.md's "u32[4] particles, f32[3] 0" is this read off by two words.
A **particle** (64 bytes): +0x18 is its stiffness toward the skinned rest shape (0 on pins and on the flags;
between MIN_STIFFNESS and MAX_STIFFNESS, by its distance from the pins [G]), +0x36 u16 held (bit 0), +0x38 s16[4] collision sphere
indices (−1: the "8 × 0xFF").
The entry's +0x44 / +0x48 are a count and array of 0x18-byte collision spheres {f32[3] offset, f32 radius?, f32, u32
node} carried by skeleton nodes (0x1D9518): the count is 0 on all 44.

## A frame (0x1D90E0)

Matrices are 4x3 row-vector (`p' = p.x r0 + p.y r1 + p.z r2 + t`, the memory layout of a column-major 4x4).

1. **First frame** (+0x81 clear): the rest pose (0x1D9B80) with this frame's object matrix W; keep W at +0x1C. Done.
2. **Not simulating** (+0x83 clear, after a reset): the rest pose again; while the blend time +0x84 runs down by dt,
   each free particle = lerp(rest, kept, time left / length), the kept positions carried along by W × (+0x1C)⁻¹
   (0x1D89C0); when it runs out the simulation resumes if +0x80. Keep W. Done (the added force is *not* cleared).
3. The skin matrix of each node in the entry's bone list: inverse bind × node × W (0x1D93E8, 0x1D9768).
4. **Pins** (0x1D8360): each pin to Σ weight × (bind position × skin matrix) (a pin without bones: × the *last*
   frame's object matrix, +0x1C); per part the pins' mean velocity (Σ moves / pins / dt) and the top speed².
5. If a part's top pin speed > MAX_VEL: 0x1D67B0(0, MAX_VEL_TRANS_TIME) keeps the positions and stops, +0x80 = 1,
   and the frame starts over (taking step 2). This is what hides teleports and cuts.
6. Keep W at +0x1C.
7. Each part in substeps of MAX_TIME_DELTA, the last remainder too if > 1e-4 (0x1D7710). Per free particle:
   * F = added force + (0, 0, −GRAVITY × MASS) − DRAG × |v − 0.85 v̄| (v − 0.85 v̄), v̄ the pins' mean velocity;
   * IS_STIFF (monsters): F += stiffness × (skinned rest place − x); the springs add no force;
   * else (flags) per spring, d = x_b − x_a: F_a += d/|d| (k (|d| − rest) + damping (v_b − v_a)·d), F_b −= that;
   * bends: θ = acos(n0·n1) by the rational fit π/2 − c(−6.9086 + 12.8558c² − 5.9494c⁴)/(−6.9253 + 14.2031c²
     − 8.2793c⁴ + c⁶) (π below −0.999, 0 above 0.999), negative when (n0 + n1)·(x_p0 − x_p2) ≤ 0; torque
     (rest − θ) k, skipped under 0.001, along the unit (n0 + n1): + on p0, p1, − on p2, p3;
   * v += F h / MASS; x += v h (USE_EXTRA_DAMP: v × EXTRA_DAMP when v² ≤ EXTRA_DAMP_VEL²);
   * the triangle normals again if the part has bends.
8. **Inverse dynamics** (0x1D8CA0), per part NUM_INV_DYN_ITERS passes over the springs: a spring longer than its rest
   length moves its ends by d (½ − r²/(d² + r²)) each, the whole of it (×2) on the free end when one end is held
   (the code checks only one: with a held a it moves b regardless). Then (ADJUST_VEL) each free particle's
   v = (x − x at the frame's start) / dt, and the start position = x for all.
9. Collisions (0x1D87F0, none), the bounding sphere (0x1D68F0), the added force cleared.

The particles' normals are made when drawn (0x1D6E30): a free particle's is the normalised sum of its triangles' unit
normals ((p1 − p0) × (p2 − p1)); a pin's is its bind normal turned by its skin matrices (normalised with two bones
or more).

## The flags' wind (0x3473B0)

The ClothObject's `ClothBit` property group (objects1 book: `HorizontalWindDirFreq = 1, HorizontalWindForce = 10,
HorizontalWindForceFreq = 3, HorizontalWindMaxAngle = 6.28, HorizontalWindMinAngle = 0, TimeOffset = 0,
VerticalWindForce = 10, VerticalWindFreq = 2` on the US flag pole) at +0x148 .. +0x168. With t = TimeOffset + the
game clock, a = |N(t DirFreq)|, angle = a Max + (1 − a) Min, the force (−sin angle, cos angle, 0) × HorizontalWindForce
× |N(t ForceFreq)| + (0, 0, VerticalWindForce × |N(t VerticalWindFreq)|) goes to 0x1D6740 before the frame. N
(0x13C160) is 1D value noise (0x13C220: the lattice hash 1 − ((n(n² 15731 + 789221) + 1376312589) & 0x7FFFFFFF) / 2³⁰,
n = k ^ (k << 13), smoothed ¼ ½ ¼ and interpolated linearly) plus 5 octaves (the count at 0x44CA54) of half the
strength at twice the frequency. The vertical force about cancels the gravity of 9.8: the flags fly.
The sincos at 0x106730 returns sin through its first pointer (checked against the slerp at 0x167EE0).

## The meshes

A cloth is drawn from its model's own type 0 mesh. The model of `<Model>|cloth` (all 44 have one) holds a mesh
`<Model>|<Model>` for the flags (`Flag_US|Flag_US`: part 0 the flag, material `Cloth_US_Art` with texture `Flag_US`;
part 1 the pole, `Cloth_US_Pole`, no UVs) or `000GEORGE_CLOTH|George_Hair` and the like for the monsters, with one
mesh part per cloth part, in order. 0x1DAB40 builds the Cloth resource from the model (found by 0x19BA60): the cloth
entry (type 25), then the model's first skinned mesh, or else every type 0 mesh's parts, one record {part, material
slot} each; 0x1DA4B0 hands cloth part k's ClothRenderer record k (vt +0x18 the part, vt +0x10 the material) and part
k's particle states (0x1AAFB0).

A type 0 mesh record (0x50 bytes from 0x20; the game's in-place part object, vtable 0x444970, starts 0xC before it:
0x197690, 0x183E50) has at +0x3C the offset of a **vertex → particle stream** {u32 count, u32 0, u32 offset in data2}
beside the vertex stream's at +0x40 (0x183C80: 0x25BF60, 0x25BEA0). It is a u16 per vertex (count 0 on ordinary
meshes; on the flags it comes before the vertices in data2). ClothRenderer 0x1AB120 keeps it at +0x7C and its draw
0x1AA960 writes particle map[v]'s position (state +0x18) and normal (+0x30, × 127 as s8) into vertex v every frame,
then draws the part's strips. On all 76 mesh parts of the 44 cloth models the table has one entry per vertex, every
vertex sits on its particle's bind position, and the strips make exactly the cloth part's triangle count.
`cloth_mesh_part()` reads it all (material name index, UVs, strip triangles, vertex → particle).

**Names.** A city object has `ClothBit| { ClothRef|Name = "Flagpole_US" ... }`. That names a template group in
Objects1_props (`Flagpole_US { ClothProps = "Cloth_Test_Props.txt"  Shape|Shape = "Flag_US" }`) whose Shape is the
model. Model names match without case (0x19AE80 hashes them through tolower, 0x32EC08): `flagpole_red` →
`FlagPole_red`, `Flag_Yellow_Lion` → `FLAG_Yellow_Lion`, `flag_red` → `FLAG_RED`. `ClothProps` names a text props
file that is not on the disc; the entry's own part settings hold.

(Before this table was found the viewer matched vertices by bind position; `cloth_triangle()` still does that, through
a triangle because a position can hold two or three particles — Amanda: 16 such places.)

## Port status

Faithful: every step above, the reset/blend, the wind and its noise. Stand-ins [G]: the skin matrices come from the
caller (the game builds them from the skeleton for the bone list only); without skin matrices a particle with bones
goes by the object matrix (the game always has a skeleton for those; with a skeleton it puts bone-less particles at
0); dt ≤ 0 is ignored (the
game would zero the velocities); the collisions, the bounding sphere and the renderer's scale (+0x8C, 1) are left out.
