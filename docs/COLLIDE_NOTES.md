# The collision world (src/collide.c)

gWorld's collision is two static objects, built before `main` by the static initialisers in the second code segment:

| Object | Where | Class | Its job |
|---|---|---|---|
| **0x4D0900** = `*(gp − 0x6D60)` | 0x441490: `0x283B40` (SpatialDataMgr ctor, vptr at +0xC), then vptr 0x449AE0 | `RGridMgr : SpatialDataMgr` | every query over the world's objects, and the block's buildings in two rows |
| **0x4D0B00** = `*(gp − 0x6D5C)` | 0x441530: vptr 0x449B30 at +0 | `RSceneryMgr : SceneryMgr` | the street: the plane z = 0 |

gp is 0x453B70 (0x100198). The world's objects themselves are RWorld's list (`gp − 0x6CFC` +0x58, linked by +0x68;
WorldBase vtable +0x3C / +0x40 add and remove them, flag 0x200). An object takes part when it has a shape
(+0x74, a `WCollisionInfo`) and passes the caller's flag masks (+0x4C). There is no grid or tree for the objects:
every query walks the whole list [R].

`collide.c` is the port, an engine-free module the viewer fills each frame (`collide_begin`, `collide_add_building`,
`collide_add`, `collide_finish`) and then asks. [R] read in RAMPAGE.ELF, [G] guessed.

## What is in it

**Shapes** [R] — `WCollisionInfo` (vtable 0x449000; WSimpleObject's sits at +0xC4 and +0x74 points to it, 0x2905E8):
+4 type (0 sphere, 1 box), +0xC centre (in the object's frame), +0x18 radius (a box: its bounding sphere |half|),
+0x1C half sizes, +0x28 the values as set. `setSphere` 0x281110 (centre, r²), `setBox` 0x281170 (min, max).
The vtable: +0xC sphere test 0x280FF0, +0x10 box test 0x280D60, +0x18 ray 0x280780. The object's matrix is
+0x8 .. +0x34 (x, y, z axes, position at +0x2C); 0x101190 turns a world vector into it.

**An object's shape** (WSimpleObject +0x14, 0x28FD70) [R]: the template's book (+0x80) and then the instance's
(+0x7C): `CollisionBox` (min, max) if given, else `CollisionRadius` (a sphere about the model's bounding sphere's
centre), else `AutoCollisionBox` with the model's box (model vtable +0x18); with none of them, the model's bounding
sphere (no model: radius 1 at the origin). `RenderRadius` only sets the drawing sphere. `collide_shape_pick`.

**Buildings** [R]: `Building` is a WorldObject (vtable 0x44A8F0). At its init (+0x14, 0x2BC920) and every frame of
its update while a storey or the whole is falling (+0x10, 0x2BDC70) its shape (+0x98) is set to the grid's box
(+0x1DC .. +0x1F0) less its place (+0x2C: x the middle, y 2.5 Depth + row × BackRowOffset): the box's top comes down as it
falls. `RGridMgr` keeps them too: all of them at +0x5C (count +0x58), and by row at +0x18 + row × 0x20 (count
+0x10 + row × 4), added by 0x2A5110 in the block's order and sorted by x (+0x2C) by 0x2A51B0. The row's fronts are
at y = row × `BackRowOffset` (the int at 0x4D502C, set from the block). `collide_add_building`.

**The street** [R]: RSceneryMgr inherits SceneryMgr's plane: the ray (+0x24, 0x282460) meets z = 0 only going down
(dir.z ≤ −0.001) from above it, t = −z / dir.z; the sphere (+0x2C, 0x2823F0) is in it when r > z, normal (0, 0, 1),
out by r − z + 0.001 r; the box (+0x30, 0x282330) likewise; the ground height (+0x20, 0x282500) is 0 everywhere.
The water's lower floor is the monster's own business (0x337E00, main.c), not the collision world's.

**Flags** the queries use [R]: 4 in the world, 8 left out of the ray cast for the shooter (0x295750 sets it on the
weapon's owner around its own cast), 0x40 a monster, 0x2000 IsBuilding, 0x4000 PushOutMonsters, 0x40000
IsObstacle, **0x400000 a VolumeTrigger** (its init 0x3360F4 sets 0x400000, | 0x4000 with `DoPushout`, | 0x40000
with its +0xA2).

## The queries (RGridMgr vtable 0x449AE0) [R]

- **+0x18 the ray** (0x283730): `(start, dir, &t, &normal, &material, &object, include, maxt, exclude, any)`. The
  street first (maxt becomes its t); then every object with flags & include and none of exclude, its shape's ray
  (0x27FFF0: start and dir into the object's frame, the shape's +0x18, the normal back out). The nearest (or with
  `any` the first). Shots: include −1 (beams) or the weapon's +0x134 (projectiles, 0x293CB4), exclude the
  weapon's +0x138 | 8. `collide_ray`.
- **+0x20 the sphere query** (0x2835B0): every object whose shape's bounding sphere reaches the sphere.
  `collide_sphere_query`.
- **+0x30 / +0x34 / +0x38 the box query** (0x282FB0, next 0x282D70, end 0x282D60): an iterator over the objects whose
  bounding sphere reaches the box's and whose box, turned into the world (0x101C20: |M| × half), overlaps it.
  `collide_box_query`.
- **One shape against a box** (0x2801A0, the monster's): the box's centre into the object's frame, its sizes as they
  are (taken as lying along the object's axes); a box shape: per axis the overlap a + b − |d|, none below 0, out
  along the smallest one (0x281800, ties x then y), its sign d's (0x281230); a sphere shape: the sphere against the
  box (0x2815F0 / 0x281940: inside on all three axes out by the least depth, outside on one straight out of that
  face, else from the nearest edge or corner), the push turned round. The push, normal and point come back into
  the world. `collide_obj_box`.
- **One shape against a sphere** (0x280470 → +0xC, 0x280FF0: sphere–sphere 0x2813F0, sphere–box 0x2815F0).
  `collide_obj_sphere` (streetobj.c's 0x280470).
- **Two shapes** (0x27F620 → 0x27FD70): the one of the higher type is tested in its own frame (its matrix inverted;
  the push turned round when it was the second): a sphere against a box (0x2815F0), or two spheres (0x2813F0); two
  boxes never meet (0x27FF18 → 0x27FFC0 returns 0), and a type 3 (the monsters' WCISpheres) meets nothing [R].
  `collide_pair`.
- **The buildings by place** (0x2A3D20, `gWorld.findBuildingByPosition`): with the direction (0, 0) the last building
  in the list whose footprint (x, y, edges included) holds the point, whatever its state or height; with a direction
  the nearest one that way beyond the building at the point (or the point), wholly past it, across the point's row
  (an x direction: the point's y within it) or column. `collide_building_at`.
- **hitBuildingPt** (0x2D54F0): the building at the point, unless there is none or the point is more than 1 behind
  its front, then the nearest that way (side 0 +x, 1 −x, 2 +y, 5 down, which is the footprint again); not above its
  top (but the roof); the point onto its face (side 0 its min x, 1 its max x, 2 its front, 5 its top); a hit
  (vtable +0x34, 0x2BC150) only within 5. `collide_hit_point`; the hit stays building.c's `building_hit`.

## The monster (0x2E2C50, from its update) [R]

**The other monsters** are not in the box query (exclude 0x40). Just before it (0x2E11A4) 0x2E4660 pushes a player's
monster (its slot under the players' count, not in mode 3 or 5) off each other player's monster it overlaps, not one
hidden (+0x109) nor one it holds or is held by: each a cylinder about its reference point (0x2DA900: its place +
(+0x190 + the type-15 record's +0xC, +0x194 + its +0x10) × its scale along its axes), MONSTER_DATA +0x188 + the
record's +0x8 across, 2 × +0x18C up from the feet (in mode 1 or 2 about +0xB20 instead), × the scale. Overlapping
in z and nearer in x, y than the two radii, it moves away along x, y by half of the overlap, the overlap at most 50 ×
the frame's time (0x4D17A4), and with foot IK its feet let go (0x33BE90). main.c `push_apart` (up to v0.11.4 nothing
kept them apart: in a game of two or more they walked into each other).

The box: around its feet ±(MONSTER_DATA +0x198) across, ±(+0x19C) up, both × the scale (+0xA78) when +0xA74;
its centre +0x19C up. On a building (mode 5), or in the air (mode 1) more than 3 up, the box and centre are 3
higher (0x2E2E18: `c.le.s z, 3` then `bc1f`, the compiler's z > 3; the jump clips carry the body about 3 above the
place, and the landing, 0x2E29A4, and the water test, 0x2D603C, add the same 3). The box query: include 0x404000 (VolumeTrigger | PushOutMonsters), exclude 0x40 (monsters). For each object
(on a building only VolumeTriggers), 0x2801A0; when they meet:
- a VolumeTrigger: 0x335BF0 fires it (when enabled, +0xA0, with a FireFunc, +0xA4, and its refire clock +0x98 run
  down: the script's FireFunc(monster, normal), the clock set to MinRefireTime +0x9C; 0x3361A0 runs the clock), and
  with DoPushout the monster moves by the push's x and y, unless it is on a building;
- otherwise (PushOutMonsters, 0x2E2F88): nothing when the monster's +0x108 (setIsAI, 0x2E35C0: the AI's monsters)
  is set; else the push's x and y, then, with foot IK (+0x258C), 0x33BE90(+0xC81C) lets both feet go (each record
  +0x24 &= ~3, +0 = 0; the anchor −1) [R].
  The pushes are each from the box as it was at the start.

Then the buildings, from its body's centre (0x2E4D30: the feet + MONSTER_DATA +0x1A0 along its x axis and +0x1A4
along its y, × scale, plus the move's offset at +0xBAA0 +0xC / +0x10):
- **On the ground** (mode 0) over a building (0x2D6150: the one at the body's centre, if its top is no more than 5
  above the feet; none once +0xA4C ≤ 0; cached for the frame) whose top is less than 1 above the centre, and the
  building's +0x38C clear (0x2E3020; the building's grid's +0x1D4: a gone building's, below): **the roof** (0x2A49C0): the centre kept within the roof's rectangle (+0x1DC .. +0x1EC),
  0.1 × half width (0.3) in from an edge whose value is not 0 (a free edge, 2: it walks to the brink), half width (3)
  from a higher neighbour's wall (0) (0x2E30D8 passes f12 = 0.1 × f13, f13 = +0x198 × scale) [R]; no limit on an edge
  where the neighbour is flush and as high. The neighbours (0x2A4C70, built once): in a row, standing ones
  (+0x14C 1 .. 3, not falling) at most 2 apart: as high and touching → both 1 (walk across); the higher one's
  wall → 0 on the lower; front against back: touching (|back min y − front max y| < 0.001), the same x span and
  height → 1, else the higher one's wall. Initially 2 (no neighbour).
- **Otherwise, unless on a building** (mode 5): 0x2A35A0 with r = half width and the alleys' r2 = half width (+5
  when +0xB9C0), the centre 3 higher in mode 1 more than 3 up (0x2E3198); the change in x and y moves the feet, and the velocity (+0xBD30)
  and the stick's (+0xA68) are zeroed along an axis it was pushed against.

**0x2A35A0, the rows** (0x2A36F0 for row 1, then row 0, on the same point): a row only matters once p.y + r is past
its fronts (y = row × BackRowOffset). R = the first standing building whose middle x is not left of p, L = the last
whose middle is not right of it; a wall is R's min x (L's max x), or none (±1000) when the building is no higher
than p.z. Inside one's span and behind the front within 3 below its top: p.y ≤ rowY + 10 − r (kept on a roof 10 deep)
and that wall dropped. Then: in front of a wall (inside a building's span, or a gap narrower than 0.1 + 2 r2) p.y
goes to rowY − r − 0.001 (×1.001); in a gap and still in front of the row, out of the corners' circles; in the
alley, off its walls. Pushed by both rows: p.y ≤ BackRowOffset − r. Last, **the street's limits**: x within ±55, y
within −10 .. 21 (the body's centre). `collide_buildings_push`, `collide_roof_clamp`, `collide_building_below`,
`collide_monster`.
While an NI runs there is no push-out at all [R 0x2E2C84, the world state's +0x1A: below]: Levels.scr's ResultsNI
puts the winners on the block left (13, −42) and EndBlockNI walks them from there to (0.7 x, −13.2) and on to
(x, −5.2), through the fronts and the limits (main.c `push_out`, game.c `game_ni_on`).

## The SpherePhysicsMover (vtable 0x449500) [R]

**Its numbers** (0x28F220, 0x28DE70): Gravity +0x8 (the template's, else the world book's, else 9.81), Mass +0xC (1),
Restitution +0x1C (0.8), SlidingFriction +0x20 (0.4), the radius +0x38 = √(shape +0x34): a sphere's radius, but
for a box √(max x) as the game reads it; the inertia +0x24 .. +0x2C = (0.67 − 0.27 InertiaSolidity) × Mass × r²
(InertiaSolidity 1 unless set); velocity +0x10; the spin as a world axis +0x3C and rate +0x48 (0x28DD80).

**A frame** (+0x30, 0x28E440): the speed kept under 50 × 0.75 r / dt; sub-steps of h = min(0.75 r / speed, the time
left) until the frame's time is used. Each: the matrix turned about the spin's axis by rate × h (when rate > 0.001);
the position by v h − ½ g h² in z, v.z −= g h; then the **street**: when the sphere (centred on the object's
position) reaches below z = 0, it is lifted out, the contact (0x28EB30) is made at its lowest point, and the
object's vtable +0x40 runs (WSimpleObject 0x28F9B0: the script's **OnHitGround**, event 11, with the ground's
material) — every sub-step it touches; the matrix is written back (vtable +0x24) and the **objects** checked
(vtable +0x3C with 1, 0x287430): the sphere query about its shape (all flags), each other object's shape against
its own (0x27F620); when they meet (push² > 1e‑5) the response 0x285E90 and then **both** objects' vtable +0x38 —
WSimpleObject's 0x28FA30 sends the script's **TakeImpact** (event 10) with the other object; Building's 0x2BC0F0
hits the building (vtable +0x34, kind 1) at the contact with radius Mass / 1000 of the other's mover (10 without
one). **This is TakeImpact's caller**: the only movers that call +0x3C are the SpherePhysicsMover's (the Line,
Spline, Rotation, Attack and Held movers do not), so thrown and smashed things are what meet, and the meeting
repeats each sub-step while the shapes overlap.

**The contact** (0x28EB30) in the object's frame: the velocity at the point u = v + ω × r; parting (u·n ≥ 0): nothing;
else j = −(1 + e) u·n / (1/m + n·((I⁻¹(r × n)) × r)), the tangents' impulses cancelling u's sliding, held to
μ j (Coulomb); the impulse (0x28E0D0: v += J/m, ω += M I⁻¹ Mᵀ (r × J)). Objects meeting (0x285E90): one with a
mover and one without — the same contact against the other's surface with its own restitution and friction (no
pushing out); both with movers — an exchange between them (0x285E90) [R]. A is the one met, B the one moving
(0x2875F4), N the push that takes A out of B, P1 the contact on A's surface, P2 on B's:
- u = (vA − vB) + ωA × (P1 − A) − ωB × (P2 − B) (each mover's velocity and spin, vtable +0x10); nothing unless u·N < 0;
- e and μ each 0.7 × the lesser + 0.3 × the greater;
- n = −N/|N|; the tangents t1 = c × n made unit, t2 = n × t1, c from 0x103E90 (of (−n.x, n.y, n.z), (n.y, n.x, n.z),
  (n.z, n.y, n.x) the one with the least (c·n)², the first on a tie);
- K(d) = 1/mA + 1/mB + d·((I⁻¹(rA × d)) × rA + (I⁻¹(rB × d)) × rB) with **A's world inverse inertia for both** (B's
  own is made from A's and never read, 0x286530); jn = (1 + e) u·n / K(n), nothing if jn < 0; j1 = u·t1 / K(t1),
  j2 = u·t2 / K(t2) (with the first u: the spins the normal impulse changes are dead stores);
- the friction held per axis: S = max(|j1|, |j2|) + ½ min(...), scaled by μ|jn| / S when S > μ|jn|;
- J = jn n + j1 t1 + j2 t2; the push mB / (mA + mB) N to A and −mA / (mA + mB) N to B, each replaced by a lift
  (0, 0, d + 0.001 r) when its shape's sphere would be d under the street where it is pushed to (0x282330);
- A takes −J at P1, B +J at P2 (0x28E0D0: v += J / m, the push, then ω += R I⁻¹ Rᵀ (r × J) about its own inertia).
Every other mover class answers no spin and ignores the impulse (0x2873A0, 0x287390), held objects included. Here
(collide.c `exchange`) for two flying objects, each module naming its flying ones' movers (`collide_set_mover`), and
for a flying one meeting a driven one (a Line or Spline mover: a car driving; a held object), which the modules name
with `collide_mover_kinematic`: velocity 0, no spin, inertia FLT_MAX, no push or impulse for it (below); P2 is taken
as P1 − n × depth (0x27FD70 leaves the second point of a sphere against a box unwritten).

There is no rest and no rolling resistance in the mover: friction makes it roll, and it rolls on (the scripts stop
or delete what they throw). The street test (0x28E6F4) takes the sphere about the mover's position, with no offset
for the shape's centre, and the models' origins are at their base (the taxi's body centre 0.90 up, the bench's 0.37):
a thrown object rests r above the street [R].

## Settled since, and what is left
- The gate at the top of 0x2E2C50 [R 0x2E2C6C .. 0x2E2C88]: it returns at once when the monster's +0xFC (its slot)
  is not below 0x4D7E28 (the players' count: the revealed monster's slot, main.c `push_out`) or the byte 0x4D6A9A
  is set: the world state's +0x1A (0x4D6A80), "an NI runs", written through the state's pointer, so no absolute
  address finds the writes (0x324340 sets it at each NI's start, 0x324870 clears it on the first tick without one;
  game.c `game_ni_on`). So no push-out at all while BeginLevelNI, ResultsNI, EndBlockNI and the others run: the
  winners stand on the block left and walk through its fronts and the street's limits. The
  fields it reads: +0x108 setIsAI (0x2E35C0), +0xB9C0 (`Monster.isClimbing` tests it ≠ 0, BOSS_NOTES.md; the held
  monster's slot word, MULTIPLAYER_NOTES.md), +0x258C the foot IK (FOOTSTEP_NOTES.md), +0xBAA0 the kept type-16 move
  record (its +0xC / +0x10, main.c `Mon.ev16x` / `ev16`) [R].
- The building's +0x38C [R]: the byte at its grid's +0x1D4 (the grid at +0x1B8), cleared at the set-up (0x2D2848)
  and set when the sinking collapse ends (0x2D2D58, with the box's top +0x1F0 set to 0, +0x38 of the grid): a gone
  building. 0x2E3020 then skips the roof, and the monster takes the street's push as if no building were below; the
  rows' push leaves out the ones not standing (+0x14C 0), so the rubble's footprint is walked onto and off
  (collide.c `collide_monster`: the roof only for `state != 0`; before, the roof held the body's centre 3 inside the
  footprint's edges).
- The order of 0x101A90 [R]: a × b of the game's 3 × 4 row matrices (rows the axes, v' = v a b), called by the
  SpherePhysicsMover as (the object's matrix, R(axis, angle)) (0x28E610 .. 0x28E618): the spin turns about a world
  axis, as collide.c has it; the HeldObjectMover's blend as (the rest, the hand's place): the rest in the hand's frame
  (0x310D04).
- WCISpheres (vtable 0x449A20, the monster's spheres as one shape for rays) [R]: a type 3 shape, which 0x27FD70 never
  meets (0x27F620 passes the higher type first), so a thrown object does not meet its thrower or any monster: the
  viewer's movers leave the monsters out (`collide_mover_step_ex`'s exclude COLLIDE_F_MONSTER), as the game.
  Its entries (+0x40, count +0x44, 0x14 each: the centre in the monster's frame, r² at +0xC, r at +0x10) are filled
  once a frame by 0x2DA9E0: the shape's offset through its bone's matrix, × the dynamic scale (+0xA78 while +0xA74),
  the radius × the same scale (0x2DAB60 .. 0x2DAB84); then 0x29A880 sets the bound (sphere 0 merged with the rest by
  0x103770) [R]. The touch test 0x2E22B0 takes the graph's radius unscaled with the world centres +0x6D4 [R].
- WCISpheres' ray (vtable +0x18, 0x29A300; reached only through 0x27FFF0 from the world's ray 0x283730, whose
  callers are the shots 0x293CB4, the beams 0x295800, the effects' FxWorldAdapter +0x4C 0x282210 and the debug
  overlay 0x288B90; 0x31FC40's direct 0x27FFF0 calls are the buildings' ground-shadow grid) [R]: no hit when the start
  is farther than the bound's radius + maxt from its centre (0x102DA0) or the line misses the bound; then each sphere
  by 0x100900 (the unit ray: 0 none, 1 touching at t = -b/2, 2 two roots): a start inside one (t0 < 0 ≤ t1) is a hit
  at t = 0 at once, its normal away from that sphere's centre (its frame's x below 1e-6 squared), its index 0; else
  t0 ≤ maxt is kept and maxt becomes t0, the normal (the hit − the centre) made 1 long, the index the sphere's. The
  base WCollisionInfo's sphere (0x280780) is the same test with the same fallback. The port adds each sphere as its
  own object (main.c `world_build`, radius × the scale), which the world's ray takes the nearest of: the same t and
  normal [R]. Left as it is: an inside hit's index is the sphere's, not 0, and a fallback normal is the world's x,
  not the monster's (the spheres' objects are world-aligned), and a ray with `any` stops at the first sphere it
  meets, not the monster's nearest (only the FxWorldAdapter's ray passes its caller's flag); nothing in the port
  reads the index or asks with `any`.
- The monster's touch damage (TakeDamage of type Collision, 2.0, every frame, to an object without
  OnlyHitWithEffNodes; boss.c, rail.c; not the guards' huts, which set it) [R]: the monster's object test 0x2E22B0, its unarmed spheres
  against each object's shape (0x280470), not 0x2E2C50.
- `isSphereVisible` is the camera's, not the collision world's.
- The other mover classes in the exchange (0x285E90) [R]: LineMover, SplineMover, RotationMover, HelicopterMover,
  AttackMover and HeldObjectMover all have 0x28F220 as vtable +0x1C (the base init: velocity +0x10 zeroed, inertia
  +0x24 .. +0x2C FLT_MAX, Mass, Restitution and SlidingFriction read), their spin getter (+0x10) 0x2873A0 gives the
  zero vector at 0x409FF8 and their impulse (+0x24) 0x287390 does nothing. The exchange reads each mover's velocity
  at +0x10 directly (0x2860F0, 0x286148) and hands each its push with its impulse (0x287308, 0x287324): a driven
  object meeting a flying one is not moved, the flying one's push, street lift and impulse are as for two flying
  ones, with the driven one's restitution, friction and mass in the blend and the levers from its tensor (FLT_MAX:
  none). Its +0x10 is 0: only the setters (+0x18: the LineMover's 0x315BD0, the SplineMover's 0x2A7EF0, the
  AttackMover's 0x313D90; the others' write elsewhere or nothing) write it, and the one caller of a mover's +0x18
  found, 0x2B71A8, is the world's unused debug ball (COVERAGE.md). collide.c `collide_mover_kinematic`; vehicle.c,
  rail.c and parade.c give one to an object on a LineMover, a SplineMover (rail.c) or held.
- A thrown or smashed object becomes a sphere [R]: throwObject (0x2DA088 .. 0x2DA1E8, unless the object has flag
  0x80000) and smashObject (0x2B1EC0) set its shape, before its SpherePhysicsMover, to a sphere at its origin of radius
  min(|centre + half|, |centre − half|) / 2 (a sphere at its origin stays). So a thrown car (`AutoCollisionBox`)
  meets the buildings' boxes and hits them. `collide_shape_flight` (vehicle.c, streetobj.c, army.c, parade.c); the
  port had kept the box, and a thrown car flew through the buildings.
- Two boxes never meet [R]: 0x27F620 passes the higher type first and 0x27FF14 returns 0 for a box against a box
  (0x27FFC0); collide_pair the same.
- A gone building keeps its box [R]: the game never removes a building's object (removeObject's only callers are
  the block's unload 0x2B66C0, the monster 0x2EACA0 and 0x29A0F0), and the collapse's last frame (0x2D2D00) sets
  the grid box's top to 0, which the update (0x2BDCE0 -> 0x2BDDB8) still takes the shape from: a flat box at the
  street (collide_add_building adds it for every state). A mover meets it only past depth² 1e-5, and a ray's tie at
  z = 0 gives the street's normal, so nothing changes in play.

## Wired in (main.c and the modules)

Each frame, before anything moves, main.c's `world_build` fills the Viewer's `CollideWorld`:
1. `collide_begin(w, -1)`; the block's buildings (`collide_add_building`, tag `COLLIDE_TAG(COLLIDE_OWN_BUILDING, i)`);
2. the monster's collision spheres (flags 4 | `COLLIDE_F_MONSTER`, tag `COLLIDE_TAG(COLLIDE_OWN_MONSTER, slot)`, kind
   the sphere's number), moved again with the monster before the modules update (`world_monster_moved`);
3. each module's objects with their shapes (0x28FD70, `collide_shape_pick`, read once per object) and their own
   flags | 4: `army_collide_add`, `vehicle_collide_add`, `rail_collide_add`, `parade_collide_add`,
   `streetobj_collide_add`, `scene_huts_collide_add` (the guards' huts, their own sphere: a flying hut meets the
   buildings and objects), `people_collide_add` (every person, the actors' shared sphere: PEOPLE_NOTES.md),
   `boss_collide_add` (its parts kind 0, its VolumeTriggers kind 1 flagged 4 |
   `COLLIDE_F_TRIGGER`, | `COLLIDE_F_PUSHOUT` with DoPushout). Flag 4 is "in the world's object list": World::addObject
   (0x284CD0, gWorld vtable +0x3C) sets it on every object it adds, VolumeTriggers too (0x2A6710 adds them at 0x2A67C0
   / 0x2A68D4), and removeObject (0x284B60, +0x40) clears it [R]. A held object stays in, as in the game: the grab
   (0x2DA310: 0x2DA3F0 .. 0x2DA424) sets BeingHeld 0x20000 and clears Pickupable and PushOutMonsters (0x14000), with
   no removeObject, so the monster's box no longer sees it but flying objects meet it (TakeImpact) and so do shots
   and beams (include 4 / -1; bit 8 is only the beam's owner's during its cast, 0x2957E0 .. 0x295854); only a shot of
   the held unit's own is no hit (its owner, 0x293CD8) [R];
4. `collide_finish`.

The tag (`COLLIDE_TAG(owner, index)`, `COLLIDE_TAG_OWNER`, `COLLIDE_TAG_INDEX`) names the module and its index; each
module keeps its object's index in the world (`cobj`, checked with `collide_check`) for its mover's `self`.

Who asks it:
- **The monster** (`push_out`, every mode: on a face only the triggers): `collide_monster`; the PushOutMonsters
  objects (traffic, trains, carts, balloons, chimneys and gongs, the bosses' tanks and jumpers) and DoPushout triggers
  push it, and `out.touch` goes to `boss_trigger_touch` (the FireFunc at the boss's next update, 0x335BF0).
- **The shots** (army.c `ray_hit`): `collide_ray` with the shot's masks (include 4, 0x2000 for DamageType 8, 0x2040
  for Blunt; exclude 8; a hit on its owner is none) or a beam's (include −1, its owner flagged 8). A monster's sphere
  is a hit on that monster; anything else stops the shot.
- **The flights** (army.c, vehicle.c, rail.c, parade.c `sphere_step`, people.c): one `CollideMover` per flying object from the
  template's Gravity, Mass, Restitution, SlidingFriction and InertiaSolidity and `collide_mover_radius` of its shape,
  stepped by `collide_mover_step_ex` (the monsters left out). The −1 contact is OnHitGround; an object contact is
  TakeImpact on both: within the module directly, else an `AE_IMPACT` / `VE_IMPACT` / `RE_IMPACT` / `PAE_IMPACT` / `PE_IMPACT` event
  that main.c's `impact_route` gives to the owner (`building_hit` with radius Mass / 1000, `vehicle_impact`,
  `rail_impact`, `streetobj_take_impact`, `army_impact`).
- **The street objects' tests** (streetobj.c): a blow `collide_obj_sphere`, a touch `collide_obj_box`, against the
  object's shape.
- The world is built from where everything was at the end of the last frame; what moves this frame is seen by the
  others next frame. The game has no such picture: each query walks RWorld's list (+0x58, next +0x68) and reads every
  object's matrix and shape as it is then (0x2835B0), so those updated earlier in the frame are seen where they are
  now (its objects move one after another within its frame) [R]. Here movers write their entry each sub-step.

## The API (collide.h)

```c
collide_begin(&w, back_row);                 /* each frame; back_row < 0: from the back row's fronts */
collide_add_building(&w, min, max, row, state, falling, tag);
collide_add(&w, &shape, m, flags, tag, kind);
collide_finish(&w);
collide_monster(&w, &monster, &out);         /* 0x2E2C50 */
collide_ray(&w, start, dir, maxt, include, exclude, any, &hit);
collide_mover_step(&w, &mover, m, self, dt, on_contact, user);   /* 0x28E440 */
```
plus the single tests (`collide_obj_box`, `collide_obj_sphere`, `collide_obj_ray`, `collide_pair`), the queries
(`collide_sphere_query`, `collide_box_query`), the building searches (`collide_building_at`, `collide_hit_point`,
`collide_buildings_push`, `collide_roof_clamp`, `collide_building_below`) and the mover's pieces
(`collide_mover_init`, `collide_mover_spin`, `collide_mover_impulse`, `collide_mover_contact`), and for the viewer:
`collide_mover_step_ex` (with an exclude mask), the tags (`COLLIDE_TAG`, `collide_check`), `collide_matrix_from16` /
and `collide_matrix_to16` (the modules' column-major 4 x 4).
