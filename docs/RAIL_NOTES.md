# The trains, the cable cars and the freighters

What `src/rail.c` ports, and from where. [R] means read from the game's code or scripts, [G] means a guess or a
stand-in. Every object here is a script object (class 0x4D5BD0) run by its template's script; the templates are in
`Objects1_Props`.

| Script | Templates | Where |
|---|---|---|
| `ElevatedTrainController.bsf` | `ElevatedTrainController1` (AvgSpawnTrainLatency 22, NumChainedTrain 3) | an ObjectInstance at (0, 0, 0) of Chicago's CG03_Block3 and CG06_Block6 |
| `ElevatedTrain.bsf` | `SlowCommuterTrain1` (HitPoints 1), `SpeedingCommuterTrain1` (HitPoints 1000, KnockDownMonster 1) | made by the controller |
| `NewYorkTrainController.bsf` | `NewYorkTrainController1` (FreqFastTrain 0.75, NumChainedTrain 6) | an ObjectInstance of NY02_Industrial |
| `NewYorkTrain.bsf` | `TrainNYEngine`, `TrainNYFlat`, `TrainNYTanker`, `TrainNYFreight` | made by the controller and the engine |
| `CableCarController.bsf` | `NoTurnaroundCableCarController` (CarInterval 15, CarType `CableCar`), `TurnaroundCableCarController` (20, `CableCarForTurnaround`) | ObjectInstances at (100, 0, 0) of SF01 (no turnaround), SF02 (turnaround, LoadWhenWaiting) and SF08 (no turnaround, LoadWhenWaiting) |
| `CableCar.bsf` | `CableCar` (Turnaround 0), `CableCarForTurnaround` (Turnaround 1) | made by the controllers |
| `freighter.bsf` | `ScumFreighter` | an AIInstance of HK01_WaterfrontA and HK02_WaterfrontB (MaxOccurances 5, MinDelay 5, OffsetDelay 5, RespawnDelay 15) |
| `Train.bsf` | `Train1` | no block places it |

`TrainBridge1/2` (ScriptObject.bsf) are still props; `TrainEngineParked`, `TrainFlatBedParked` and
`TrainFreightParked` are `StreetObject.bsf` props. Neither is here. The elevated track is the block's front row of
`RR_Bldg01_Front` (4 wide) and `RR_Bldg03_Front` (3 wide) buildings: 2 storeys, y 0 … 10, the box's top at 10.5. The
trains run on it at z 10.

## The challenge goals
Each object counts once through `gCommon.addStats` (0x2C51A0), which also awards its template name to the block's
challenge (0x346780, [CHALLENGE_NOTES.md](CHALLENGE_NOTES.md)). All are type 2 (vehicle), `Points` 25.

| Block | Goal | What counts [R] |
|---|---|---|
| CG03_Block3 (4), CG06_Block6 (6) | SlowCommuterTrain1, SpeedingCommuterTrain1 | a car with fewer than 2 HP left that a monster's blow reaches, or a thrown car; once per side's chain (below). A speeding train (1000 HP, not Pickupable) never counts |
| NY02_Industrial (3) | TrainNYFlat | the first blow on a flatcar (a freight car counts too, but is no goal; a tanker never counts) |
| SF02_The_Park (5) | CableCarForTurnaround | a blow, or a throw |
| HK02_WaterfrontB (5) | ScumFreighter | a blow: it sinks. With MaxOccurances 5 and one alive at a time, every freighter must be sunk |

## The natives
**LineMover** [R] (init 0x3173D0 / 0x317650, update 0x315E10, as in vehicle.c): from a to b at a speed, starting at a
share of the way; callback 2 at the end, 4 while a monster is within `MonsterResponseDist` (3D) as it simply moves.
`setFlags(1)` faces it the way it goes.

**SplineMover** [R] (vtable 0x44B800, update 0x33ACA0; boss.c has the same): points with their distance along the
path (`addPoint(p, d)`; `addPointWithVel(p, d, k)`: k is the point's tension, its tangent scaled by 1 − k [R
0x33B010, BOSS_NOTES.md]), the place along it grows by
velocity × dt; past the last point it stays there and answers 2 every frame until the script gives it a new path.
Hermite between the points with Kochanek-Bartels tangents (0x13B780, 0x13B2F0). Flag 2 turns the object along the
tangent (+Y along it).

**SphereMover**: the game's SpherePhysicsMover (collide.c `collide_mover_step`, 0x28E440, docs/COLLIDE_NOTES.md) in
the frame's collision world (`rail_set_collide`, `rail_collide_add`), with the template's `Gravity` (35 from
DefObjectProps), `Mass` (12000), `Restitution` (0.2), `SlidingFriction` (0.3), `InertiaSolidity` and the radius of
its shape (√ of the box's max x, as the game reads it). OnHitGround every sub-step it touches the street; a building
(a track building's roof or front, the back row's fronts) or another object it meets is a contact with the
restitution and friction, and TakeImpact on both (0x287430): ours on our own objects, `RE_IMPACT` for another
module's (a building is hit with radius Mass / 1000 = 12, every sub-step it touches it). No rest: it rolls on until
its script ends it. `setRotation(r)` (SphereMover id 8 → 0x28DD80) is an angular velocity in the world: |r| radians
a second about the world axis r / |r| (axis +0x3C, rate +0x48; each sub-step 0x28E440 turns the object's axes by
R(axis, rate × h)) [R]. The monsters are left out of what it meets: a monster's shape is its WCISpheres (type 3,
0x29A9F0), which 0x27FD70 never meets (0x27F620 passes the higher type first) [R].

**The hold** [R] (0x2DA310, HeldObjectMover 0x310ED0, as vehicle.c): BeingHeld, neither Pickupable nor
PushOutMonsters, `HoldOffset` (0, 1, 0) or `MirroredHoldOffset` (0, −0.5, 0) along the hand's axes. Its turn is fixed
in the hand by `MajorAxis` (0x310ED0): that row is the hand's z, the next the hand's y, the third their cross; the
trains and cable cars have MajorAxis 1, so x = −hand x, y = hand z, z = hand y [R]. The throw (0x2D9DC0) makes a
SphereMover with only init (spin axis z, rate 0) and the throw's velocity (Monsters.bsf's throwObject(vel, 0, eat): the 0 turns it by the monster's matrix, main's turn as for the other modules): it keeps the held turn [R].

**Buildings** [R]: `Building.isDestroyed` is +0x14C = 0 (case 0x2AD780: BUILDING_GONE, only the rubble left);
`Building.isFalling` is grid +0x1CC ≠ 0x7FFFFFFF (0x2CE230: building.h's `collapse` ≠ −1, a storey falling or the
whole collapse). `gWorld.findBuildingByPosition` is case 0x2AEAA0 of the World dispatcher: 0x2A3D20 on the world's
tree (0x4D0900) with the Building class and a zero direction: the last building in the world's list whose box holds
the point in x and y, ends included, whatever its height and state [R] (the trains ask at their own height, z 10,
above the box, and need a destroyed building found).

**Script calls** [R]: the ScriptObject frame (0x309AC0) runs the mover (its answers 2, 3, 4 to onMoverCallback), then
`Process` every frame (0x309B94), then the pulse: with a delay of at least 0.01 its clock grows by dt and at the delay
the delay is taken off and `onPulse` runs, once a frame; below 0.01 neither (setPulseDelay(0) stops the pulses;
0x309BA0). `setPulseDelay` (id 0x2F) only sets the delay, `setPulseTime` (0x30) the clock. A handler returning
4 | 8 | 16 deletes its object. `getStringProp` / `getFloatProp` / `getIntProp` read the instance
book (the ObjectInstance or AIInstance group) first, then the template; `getTemplate*Prop` the template only.
`isValid` of a deleted object is false.

## ElevatedTrainController.bsf
**Construct** [R]: SlowTrainType, SpeedingTrainType (getStringProp), NumChainedTrain; flags 4 | 8 (both tracks run),
and 32 on a coin (the tracks' ways swapped); it pulses every 0.4 s. Each track's first train comes in
(1 + rand) × latency / 4: 5.5 … 11 s with the book's 22.

**onPulse** [R], every 0.4 s:
- **Deleted** once both tracks are off (16).
- **The clocks:** a track's clock runs only while none of the trains it made is left (the stacks `trainsFront` /
  `trainsBack`, filled by the trains' `regWithController`).
- **isTrackBroken:** 16 points from (−48, 2, 0) to (48, 2, 0). A point with no building under it answers "not
  broken" at once; a destroyed one (not a falling one) within x −49 … 49 answers "broken" (flag 2).
- **A train:** a track whose clock is out gets a speeding or a slow train (a coin) when none of its trains is left:
  front track at y 3, back track at y 7.5, z 10, from x −100 (facing +x) or 100 (facing −x). Without 32 the front
  track runs to −x and the back one to +x; with it, the other way. The clock starts again at (1 + rand) × latency / 2
  (11 … 22 s). With the track broken that was the track's last train.
- **createTrain:** setTrainType; the speeding one gets flags 64 | 2048, the slow one 2048 without 64; 32 when it faces
  left (π/2); then `spawnTrainChain(NumChainedTrain − 1)`.

## ElevatedTrain.bsf
`this.flags` [R]: 1 the crash sound played, 2 the stopping sound on, 4 the explosion played, 8 the fire on, 16 it pulls
a next car, 32 it runs to −x, 64 speeding, 128 it knocks monsters down (the lead car of a speeding chain), 256 its
slow-down path made, 2048 the lead car, 4096 it has touched the street, 16384 the sparks on. `this.state`: 0 new,
1 running, 2 braking, 3 stopped, 5 crashing, 6 held, 8 exploding (7 is tested, never set).

**Construct** [R]: pulses every 0.15 s; object flags 0x2000000 | PushOutMonsters | 0x10000000, and Pickupable unless
KnockDownMonster (the speeding train is not); HitPoints; 2 s to die once crashed; 128 when KnockDownMonster.
0x10000000 [R 0x2BC1D0]: Building::hit (0x2BC150, from the building's TakeImpact 0x2BC0F0 with the object as the
attacker) returns at once for an attacker so flagged, so a car thrown or falling onto a building never damages it
(rail.c sends no `RE_IMPACT` for it). 0x2000000 (the NY train, MerryGoRound too; CombatTruck clears it) [G: no code
tests it with an immediate: no `lui 0x200` mask and no shift by 25 of +0x4C anywhere; a mask read from data was not
followed].

**spawnTrainChain(n)** [R]: the next car 14 behind (x + 14 when it runs to −x), facing the same way, in the same state,
with the same flags but neither 2048 nor 128; it makes its own next car. Making a chain clears its side's "stats
given" (the script's globals `hasGivenStatsLeftGroup` / `RightGroup`).

**onPulse** [R]:
- **0:** a LineMover to x = −x at 18 (speeding) or 10, and the `AmbientSound` (Train Rolling Loop).
- **1:** the lead car near a monster (within 60) blows its `HornSound` (train_horn) once, a pulse after it first comes
  near. It looks 20 ahead (5 points, 4 apart, within x −49 … 49) for a destroyed or falling building under the track;
  finding one it brakes its whole chain (eventSlowdownChain: state 2, the sparks, the slow-down path).
- **2:** the `StoppingSound` (MetalCreak_RND) once; the sparks and the slow-down path if it has none.
- **3** (stopped at the path's end): the stopping and the rolling sound end, the sparks stop. The train stays there,
  and its track gets no other train while it does.
- **6** (held): the sparks stop.
- **5:** the `CrashingSound` (Train_Crash) once. Still on a SplineMover it crashes again; otherwise its `FireEffect`
  (Fire_Big) goes on it. After its 2 s, state 8.
- **8:** its `ExplodeEffect` (Tank_Explode), then deleted at the next pulse 0.25 s later.

**The slow-down path** [R] (createPathSlowDown): a SplineMover from where it is to 50 ahead (speeding) or 15 (slow),
the points at 0 and 3, at 0.08 × its speed: 2.1 s or 3.75 s. A speeding train runs on into the gap.

**The sparks** [R]: the template's `SparksEffect` (Train_Sparks), 0.6 up and 2 along the train's own x (the offset
is in the object's frame: createAndStartAttachedWithOffset 0x27A990, the follow 0x291440 = offset × the object's
matrix), turned off when the train goes.

**Process** [R], every frame: running, braking or stopped, a car checks 3 behind and 3 ahead along x; the first
point within x −49 … 49 decides: no building is good, a destroyed or falling one is not. Then it crashes.

**crashTrain** [R]: a SphereMover at (10 or 18 the way it runs, −7.5 … −15 towards the front, 0), turning π/8 … π/4
about x and π/24 … π/12 about y (both negative when it runs to −x); no longer Pickupable; state 5.

**onMoverCallback** [R]: 2 while braking: stopped (3); 2 while running: the end of the line, deleted.

**TakeDamage(who, amount, type, dir)** [R], from a monster only:
- type 0: the `DamageEffect` (Car_Hit) on it;
- running, the lead car of a speeding chain (128 and 64) knocks a monster that stands above z 2 (on the track) in a
  move whose name has `Ground` or `Faceup`: setVel (±10 the way it runs, −20, 30), setReaction(2),
  addHitPoints(−`Damage` (20)). The damage's type does not matter, so a touch (type 1) does it;
- type 0: a car with fewer than 2 HP left counts (its side's stats, once per chain); then this car and those behind it
  lose 1 HP each (eventDamage), and a car at 0 crashes with the cars behind it (eventCrashChain: their DamageEffect,
  crashTrain). A slow car (1 HP) crashes at the first blow, with the cars behind it; the cars in front keep going. A
  crashed car still takes blows and crashes again (a new random flight).

**onThrow** [R]: not Pickupable; its side's stats; state 5 (it burns and explodes 2 s on).

**onPickup** [R]: the `DamageShape` (Train_commuter_01_Damaged), the `PickupSound` and `DamageEffect` effects; no
longer a knocker; the cars behind it crash; state 6.

**OnHitGround** [R]: the script's `this.hitDelay - pulse` assigns nothing, so hitDelay stays 0.25 after the first touch
and only that one acts: the DamageShape, PickupSound and DamageEffect, the bounce count, 4096, state 5.

**TakeImpact** [R]: it crashes (unless crashing or exploding).

## NewYorkTrainController.bsf and NewYorkTrain.bsf
**The controller** [R]: its first pulse (a frame after it is made) makes a slow train at (−100, −12.5, 0) running to
+x; after that, every second while that train's engine is gone, a fast one (FreqFastTrain 0.75) or a slow one, from
either end (a coin). The trains do not depend on the controller's place.

**The engine** [R] (`TrainNYEngine`, flag 256) pulls `NumChainedTrain` − 1 (5) cars, the first 14.5 behind it, the
others 12 behind each other, each with its flags (32 runs to −x, 64 fast) but 256:
- slow (0): flatcars (`TrainNYFlat`) with a tanker (`TrainNYTanker`, flag 4) every second car: flat, tanker, flat,
  tanker, flat;
- fast (1): `TrainNYFreight` cars.

`this.flags` [R]: 2 its hit effect played, 4 a tanker, 32, 64, 256 the engine, 512 the light on, 1024 hit. State 0 new,
1 running, 2 a tanker's blast due.

**Construct** [R]: pulses every 0.75 s; 0x2000000 | PushOutMonsters; the `AmbientSound` (the engine's Train Rolling
Loop).

**onPulse** [R]:
- **0:** a LineMover to x = −x at 20 (fast) or 10; the engine's `LightEffect` (Train_light) on it.
- **1:** the engine near a monster (60) blows its horn once.
- **2:** a hit tanker blows up: its `ExplodeEffect` (Tank_Explode) where it is, and the nearest monster, wherever it
  is, is thrown at 30 away from it (up twice its distance along the street, then normalized): setVel, setReaction(2),
  addHitPoints(−`DamageExplode` (20)). Back to 1: it rolls on, damaged.

**onMoverCallback** [R]: at the end of its run, deleted.

**TakeDamage** [R], from a monster only: running, a fast engine knocks a monster in a `Ground` move (setVel (±10, −20,
30), setReaction(2), addHitPoints(−`DamageCollide` (5))), whatever the type, so its touch does it. The engine's
`CollisionBox` is a thin box 6 … 8 ahead of it (its cow-catcher). A blow (type 0) on a car that is neither the engine
nor hit before (doHitEffect): its `DamageShape`, once its `HitEffect` (the flatcar's Debris_Crate) and its stats (not
a tanker's); a tanker blows up on its next pulse. The engine cannot be damaged.

## CableCarController.bsf and CableCar.bsf
**The controller** [R]: onInit a car at (45 + 10 rand, y − 7.2, 0), part way along; then one every `CarInterval` at
(100, y − 7.2, 0). y is the controller's (0 in its own block, 41 in the block behind).

**The car** [R]: `this.state` holds a stage and the bits 128 dying, 256 thrown, 512 touched the street, 1024 (never
set), 2048 held.
- **Construct:** PushOutMonsters, OnlyHitWithEffNodes, Pickupable; its `AmbientEffect` (none in the books); facing −x;
  stage 1, pulsing every frame.
- **1:** a LineMover at 6, facing its way: without `Turnaround` across to x = −x (stage 7, deleted at its end);
  with it to (30, y), stage 2.
- **2 → 3:** a SplineMover turning the car along its way through (30, y), (18, y), (8, y), (−2.8, y + 1.7),
  (−3.8, y + 16.6): along the street, then 16.6 back up the turntable's track. Each point at
  `addPointWithVel(p, knot / total, 0.2)`, the knot being the length up to that point and total the square root of
  the knots' sum; at 0.5 it takes 8.9 s.
- **3 → 4:** no mover; it turns on the turntable at π/4 a second through π (4 s), every frame.
- **5 → 6:** the second path, (−3.8, y + 16.6), (−4.8, y + 1.7), (−15.6, y), (−25.6, y), (−35.6, y): back down and on.
- **6 → 7:** a LineMover from there to x −100 at 6; deleted at its end.
- **A monster near** (callback 4, MonsterResponseDist 10, LineMovers only): its `HornSound` (cablecar_bell), at most
  every 2 s.

**TakeDamage** [R], a monster's blow (any type) on a car neither dying nor thrown: neither OnlyHitWithEffNodes nor
Pickupable, the `DamageShape` (cablecar_Damaged), its ambient effect stopped, `DamageSound` and `DamageEffect`
(Car_Hit) on it; a SphereMover at 25 away from the monster (the height difference a fifth, made upwards); onThrow.

**onThrow** [R]: not Pickupable, thrown (256), pulses every 0.5 s, it counts. The next pulse makes it dying (128): its
`HitEffect1` (Tank_Smoke_Med) and 1.5 s, then its `ExplodeEffect` (Car_explode), deleted a pulse later.

**onPickup** [R]: the DamageShape, `PickupSound` (cablecar_bell), DamageEffect; an `IsShaker` (both) also `PourEffect`
(not in the books), `ScreamSound` (Crowd_Panic), `ShakerSound` (none); held (2048).

**OnHitGround** [R]: 5 for the first touch, 1 after; over 40 dying. **TakeImpact** [R]: dying.

## freighter.bsf
**Construct** [R]: pulses every frame; OnlyHitWithEffNodes (not Pickupable); its `AmbientEffect` and `EntranceSound`
(none in the book); from x 100 or −100 (a coin) at y −7.2, z −0.5 across to the other side on a LineMover at 6,
facing its way; `Oscillate` (1, getIntProp) starts its sway at 5 rand.

**onMoverCallback** [R], while sailing: at its line's end it turns round (its facing's z negated) and sails back, for
ever. A monster near (4: `MonsterResponseDist` 12) hears its `HornSound` (none), and each monster within that distance
in a `Ground` move is pushed (pushMonster): one within 15° of its course ahead or behind 0.5 further along it, one
within 4 across 0.6 to +y (the quay), by `queuePushRequest`.

**onPulse** [R]: sailing, it sways: (sin 2t, cos 3t) × 0.21 × 0.2 about x and y, its heading kept. Sinking (3): a
LineMover 30 down and 30 along −x (the script's float π makes cos(±π/2) just below 0 either way) at 6, swaying
0.21 × min(t, 2), for 2.5 s; then (4) its `DamageEffect` (Debris_Crate) where it is, `gAIMgr.removeAIObject`, deleted.

**TakeDamage** [R]: a monster's blow on a sailing freighter: no longer hittable, the `DamageShape`
(ScumLabsFreighter_Damaged), the ambient effect stopped, `DamageSound` on it, `DamageEffect` where it is; it counts
and sinks (3).

**The spawner** [R] (gAIMgr 0x334C40 / 0x3350F0 / 0x3333F0, as army.c's): the AIInstance's first freighter in MinDelay +
OffsetDelay × rand (5 … 10 s), the next RespawnDelay + OffsetDelay × rand (15 … 20 s) after one sank; one alive at a
time, MaxOccurances (5) in all. The freighter has the AIInstance as its instance book.

## Train.bsf
[R] Construct: `Direction` (−1) faces it left or right; a LineMover from x 150 × Direction to the other side at 30,
starting at a random share; at its end it starts again from that side. No block places `Train1`.

## The engine's side (as rail.c has it)
- **Random numbers and time:** `frand` is the world's one Park-Miller state (gWorld +0x828, 0x106BD0, seeded from GameStatus seed[0]), shared with every other system as in the game (wrand.c) [R]; the scripts at the world's fixed
  step, 1/30 (0x2B7800, FLOW_NOTES.md) [R].
- **The contacts** [R]: the monster's (0x2E22B0): its spheres against the object's shape (0x280470, collide.c
  `collide_obj_sphere`); an unarmed one gives TakeDamage(monster, 2.0, Collision (1)) every frame to an object without
  OnlyHitWithEffNodes, an armed one the attack's damage, type 0 (Concussive), the sphere search ending at the first
  blow. As here: rail_update tests the monster's spheres (ArmyMonster.spheres; its box only for a monster without
  any), rail_hit each armed sphere against the shape. The knock-downs repeat every frame while the monster's move still says
  `Ground`: the viewer starts a reaction at its next clip event, not at once. A PushOutMonsters object's push is the
  game's (collide_monster, 0x2E2C50: out of the object's box along its least overlap; a train pushes a monster on
  its track ahead of it).
- **An object's shape** (0x28FD70, `collide_shape_pick`): its `CollisionBox` (the New York engine's), else
  `CollisionRadius`, else `AutoCollisionBox` (every car here) with the model's box from the host, then the
  instance's; else the sphere the model keeps (0x184CB0: centre +0x38, radius +0x44, made at load as the union of
  its parts' spheres from the file, 0x185AA0) [R]; here the union of its meshes' header spheres, city.c `city_shape_sphere` (the freighter's hull
  keeps (0, −0.53, 1.69) r 8.03, where its box's would be (0, −0.44, 2.68) r 8.75). A blow is tested on the shape (0x280470).
- **Attached effects** [R 0x27A990, 0x291440, 0x291ED0]: in the object's matrix x the offset, its whole frame: main
  spawns them in the object's frame (fx_spawn_frame) and places them there every frame (fx_place). Its 3D sounds end
  when it goes [R: a deleted object (0x290480 -> 0x288050) kills its sound slots added without keep, and these
  scripts all pass add3DSfx(..., 0)].
- **getNearestMonster** [R 0x2B26F0, the binding 0x42 of 0x2A8FA0]: the nearest of the players' monsters (gWorld
  +0x40, the count 0x4D7E28) by squared 3D distance, starting at 1000: none farther than about 31.6, so the lead
  car's isNearMonster (60) is in fact 31.6, and a tanker's blast throws no monster beyond it.
- **The freighter's sway:** setFacing(x, y, z) is Rz(z) × Rx(x) × Ry(y) (0x277E78 → 0x101330(m, z, x, y)) [R].
- **The freighter's flag** [R]: every object's setup (0x28FD70 -> 0x29C490) makes a cloth for each of its template's
  first four `ClothBit|` groups (`ClothRef|Name`, the `ClothOffset` matrix at +0x10 of the 0x54-byte record, identity
  without one, the wind keys at +0x44 ..). ScumFreighter has one: `flagpole_red` at (0, −5, 4.5) in its frame, turned
  up. city.c `rail_flags` makes it when the freighter is first drawn and carries it at the freighter x ClothOffset,
  in its wind; it goes with the freighter.

## Driving it from the viewer
1. **Setup** (each block): `rail_new` with a host whose `find` is the army's (city_book_group) and `shape_box` the
   city's; `rail_bind_army(r, army)`; `rail_load_block(r, BlockDef)` and `rail_activate(r, 1)` (the AIDef's
   freighters); every ObjectInstance whose template is ours (`rail_kind_of` ≥ 0, or `rail_script_is_ours(script)` in
   the block loader) goes to `rail_spawn(name, its place in the world, ZRotation, the ObjectInstance group)`: the
   block's own, and those of the blocks behind with LoadWhenWaiting (slot 1, y + 41) or LoadWhenDistant (slot 2,
   y + 82). army.c must skip the AIInstances whose template's script is `freighter.bsf`, as it skips the bosses.
2. **Each frame:** `rail_update(dt, monsters, n, boxes, nb)` with the ArmyMonster the army gets and a RailBox per
   building (destroyed = BUILDING_GONE, falling = collapse ≠ −1). Draw each object that is not RP_GONE and not
   `hidden` (its `shape` at `world`, the template's ShadowData), and apply the events (rail.h; `RE_IMPACT` is
   TakeImpact on the object with collision tag `id`). Before the update: `rail_collide_add` into the frame's
   collision world between `collide_begin` and `collide_finish` (`rail_set_collide` once).
3. **Monster attacks:** every armed attack sphere also calls `rail_hit`; the grab tries `rail_nearest_pickup` then
   `rail_grab`, `rail_hold` each frame with the hand's matrix, `throwObject` calls `rail_throw`.
