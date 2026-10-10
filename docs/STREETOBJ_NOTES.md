# Street and roof objects (StreetObject.bsf, RoofItem.bsf)

The lamps, benches, mailboxes, trees, fences and parked cars along the street, the gongs of the Hong Kong shrines,
and everything that stands on a roof: chimneys, smoke stacks, antennas, fans, the London and Las Vegas trims, the
pagoda huts, the palace ornaments, the skywalks between two buildings. Two scripts drive them all; `src/streetobj.c`
ports both, with the engine pieces around them. [R] read in the scripts (`build/scripts/StreetObject.scr`,
`RoofItem.scr`) or the ELF, [G] guessed.

| What | Where |
|---|---|
| StreetObject.bsf, RoofItem.bsf | the scripts (Construct, onPulse, TakeDamage, TakeImpact, onPickup, onThrow, OnHitGround; RoofItem's onBuildingDestruction, fallOff, forceDamage) |
| The script slots | 0x3F3C30 (the world object's: 0 GetTypeName … 4 TakeDamage … 8 onPickup, 9 onThrow, 10 TakeImpact, 11 OnHitGround), 0x3F5F00 (12 onInit, 13 onPulse, 14 onMoverCallback, 15 onBuildingShake, 16 onBuildingDestruction), bound by 0x30A060; called through 0x27DE00 (slot, no arguments) |
| The world object's vtable | 0x44B1D0: +0x10 its update 0x309AC0, +0x30 its matrix 0x3098E0, +0x34 TakeDamage 0x309A80, +0x38 TakeImpact 0x28FA30, +0x6C getBuildingHeight 0x309890 |
| TakeDamage's call | 0x28FBB0 (obj, type a1, who a2, amount f12, direction t1), behind 0x309A80 |
| The monster against the objects | 0x2E22B0 (from 0x2E2870, 0x2E2980, 0x2E2B28, 0x2E2B58) |
| An attack's hit list | 0x2E33A0 |
| The attachment to a building | 0x2B6580 (the block's start) → 0x309E20 |
| findBuildingByPosition | 0x2A3D20 (with no direction) |
| Building.isFalling | 0x2ADE10 (Building binding 0x45 in 0x2AD610) |

## The templates
Every template whose `Script|ScriptName` (through `ParentPropertySet`) is one of the two, all in `Objects1_Props`
(the city books only place them, as `ObjectInstance|` entries of a `BlockDef` or of a `BuildingInstance|`). All
inherit `DefObjectProps` (`Gravity` 35, `Points` 10).

**StreetObject.bsf, 75 templates.** The street furniture (Bench1, Trashcan1, Busstop1, Fan1-4, PlanterPlant/Tree,
Rocks1, Swings1, PortaPotty1, DumpsterGreen0, Cart1, CartSmall1, Crate1, Shoeshine, VendingMachine1, NewsPaper*, the
stands FishBin, CrabBin, Flowerstand, Fruitstand1), the parked vehicles (BusParked, CarParkedCoup/Sedan/Taxi with
`Pickupable` and `HitSound`/`PickupSound` "Car_Alarm", StaticTruckBlue1, TankStatic, the Train*Parked), the poles
that fall (`FallOnHit`: StreetLamp2, StreetLampChina, TelephonePole1, MailboxBlue/Brown, BasketballHoop1, the palm
trees, Tree1 with `IsSwaying`, the flagpoles Flagpole_Red/US_StreetObject), the fences and gates that break when
walked into (`IsDamagedOnCollision`: CentralParkFence, CentralParkGate, ChainLinkFence1, GraffitiWall0-3,
WroughtIronArch), the phone booths with their `HitSound`, the Hong Kong shrines (HKPagodaGongAdorn_Street with
`AttachEffect` "Gong", `HitEffect` "Gong_hit" not attached; the hut and legs HKPagodaFront*_Street with `NumHits`
2), SampanBoatParked (rocking: `Oscillate`, `IsCustomOscillation`, `NumHits` 100000, `PlayHitEffectOnCollision`),
the spotlights (`AttachEffect` "searchlight" at their node `spotlight_attach`, Spotlight2 after 2 s), the Nero
fire pillars, the Bridge, Pagoda, the gates, and MonsterUpgrade (`IsMonsterUpgrade`: challenge.c's).

**RoofItem.bsf, 205 templates.** Most are a model and a `HitEffect` ("Building_Collapse_parts" 80 of them,
"object_destroyed_generic" 58, `Points` 10). Special ones:

| Templates | Keys |
|---|---|
| ChimneyLondon, ChimneyLosAngeles, SmokeStack0-3 | `AttachEffect` "smoke_stack" at `AttachEffectZOffset` (7.5, 4, 15, 20), `HitEffect` "Smoke_stack_hit" at `HitZOffset`, `DeleteOnHit`, `FallLatency` 0, `AttachToMonsterMouthEffect` / `ShoulderEffect` "Soot_Attached", `OnlyHitWithEffNodes+PushOutMonsters`; `Points` 50 (the London and LA chimneys), 25 / 10 the stacks |
| HKPagodaGongAdorn_Roof | `AttachEffect` "Gong", `HitEffect` "Gong_hit" (`DestFallEffectAttached` 0), `OnlyHitWithEffNodes+PushOutMonsters` |
| HKPagodaFront*_Roof | `NumHits` 2, `PlayHitEffectOnFirstHit` 0, `OnlyHitWithEffNodes+PushOutMonsters` |
| HKPalaceTopAdorn1L/1R/2 | `HitRadius` 5 / 9 (boss.c: the Leeper landing breaks them, forceDamage) |
| ElevatedWalkway, JollyRogersSign(Support), WelcomeToVegasSign(Support) | `SpansTwoBuildings`, `BuildingDistance` (10, 15); the skywalk `DestFallEffectAttached` and `DamageShape` "Skywalk_Damaged" |
| RadioTower, Torch, SlotMachineHandle, WarehouseFan4, WarehouseVent0-2 | `OnlyHitWithEffNodes` |
| CarParkedCoupRoof, CarParkedSedanRoof | `HitSound` "Car_Alarm" |
| WaterTowerTop1 | `TakeDamage` 0: WaterTower.bsf's attachment, never hit through this script |
| 18 with a `ClothBit|` | a flag (the castle pillars, the London tops "…WithFlag", TribuneCap, …): it falls with them |

`Radius` (7 on many), `HitRadius`, `GroundEffect` and `AutoCollisionBox` are not read by RoofItem.bsf.

The block challenges count four of them (CHALLENGE_NOTES.md): ChimneyLondon (GB02_Mayfair, 5 of 6), StreetLamp2
(SF06_Tenderloin_District, 4), HKPagodaGongAdorn_Street and _Roof (HK04_Shrines, 6: three of each).

## The engine around them
**Flags** [R]: a template's `Flags` names set the object flags (the table 0x3F5F20: 0x2000 IsBuilding, 0x4000
PushOutMonsters, 0x8000 OnlyHitWithEffNodes, 0x10000 Pickupable, 0x20000 BeingHeld, 0x40000 IsObstacle).
StreetObject's Construct adds 0x8000 unless `OnlyHitWithEffNodes` is 0 (default 1) and clears it for `IsSwaying`;
RoofItem leaves the template's.

**The monster against the objects** (0x2E22B0) [R]: for each object in the list it is given (skipping 0x2040), when
the monster's bound meets the object's, each of the monster's collision spheres is tried:
- A sphere whose bone is armed by an attack (the bone's record +0x18 & 0x10) looks for the object in that attack's
  hit list (8 ids at +0x6): already there, it does not count; else it takes the first free slot.
- A sphere not armed, or armed but already listed, does nothing on an OnlyHitWithEffNodes (0x8000) object.
- Otherwise the sphere meeting the object's shape (0x280470) is a touch; an armed, unlisted one is a blow and ends
  the search.
- Touched: **TakeDamage(who the monster, amount, type, direction)**, type **0** for a blow with the attack's damage
  (+0x2C: 5 a punch, 20 a kick), type **1** otherwise with 2.0; direction the way from the monster, made 1 long.
- After a blow on an 0x8000 object the object goes on the attack's list (0x2E33A0, for each body part of the
  attack; after the 8th id the bone is disarmed). The list is emptied when the attack is armed.

So an OnlyHitWithEffNodes object takes one blow an attack and nothing else; any other object takes TakeDamage
**every frame** something of the monster touches it, of type 0 while an armed sphere does. 0x309A80 drops it while
the object is held (0x20000).

**The building it is on** [R]: at the block's start every world object of the block gets onInit and then 0x309E20:
its building is the one that made it (+0x104 already set), else the one its `BuildingAttachment` names, else
findBuildingByPosition at its place, which with no direction is the **last building whose box holds the point along
x and y, both ends in; z is not looked at** (0x2A3D20). With one, the object has flag 0x4000000 and +0x104 / +0x108
(the building and its id). Then:
- each frame (0x309AC0, after its pulse) while the flag is set and the building is falling (its collapsing storey
  G+0x1CC is not INT_MAX, as `isFalling` 0x2ADE10 tests), the script's onBuildingDestruction runs and the flag goes;
- while the flag is set its matrix (0x3098E0) has the building's shake added (0x2BAD50);
- getBuildingHeight (0x309890) is that building's +0x1F0, its box's top (G+0x38) now, or 0.

## StreetObject.bsf [R]
**Construct.** Flags as above. With an `AttachEffect`: the pulse after `AttachEffectDelay` (0) + a frame. With
`Oscillate`: `OscillateScale` (1) as the base and the scale, the clock at 5 × rand. The last setPulseDelay is a
frame + the delay. `IsMonsterUpgrade` sets nFlags 2 (challenge.c).

**onPulse.**
- State 0 (the first pulse): the `AttachEffect` once its delay is over, and turned off when the object dies: at
  `Offset` (a Point3, unless (-1000, -1000, -1000)) on the object, else on its model node `NodeId` (> 0), else where
  it stands (`Attached` 0), else at its node `NodeName`, else on it. Then no more pulses, or every frame with
  `IsCustomOscillation`, and state 1.
- State 1 with `IsCustomOscillation`: setFacing(sin 2t, cos 3t, its turn) × 0.0625 × the scale (the boat rocks); a
  raised scale falls back to the base by 1 a second.
- State 2, the sway (every frame): with the nearest monster within `SwayRestitutionDist2` (49: 7 away), on the side
  it leans to and past the top of the swing (cos(π t / 1.2) ≤ 0), it is held where it is. Otherwise, while the angle
  is ≥ 0, t grows by a frame and it turns by intensity × 0.025 / 1.44 × cos(π t / 1.2) about the axis; once the
  angle has come back below 0 it is set upright and the sway ends (state 1, no pulses). At intensity 1 that is about
  0.4 rad (23°) over 0.6 s and back.
- State 3, the fall: it turns 1.4 rad a second about the axis; past π/2 it is set to π/2 exactly, state 4.
- State 4: its `HitEffect` (attached unless `HitEffectAttached` 0), then `DieDuration` (1 s) later it is deleted.

**TakeDamage**, from a monster only:
- Type 0, or any type with `IsDamagedOnCollision`: one more hit. At `NumHits` (0) or more: addStats (type 0, once
  an object), `HitSound` (add3DSfx(…, 1)), its `HitEffect` (unless `PlayHitEffectOnlyOnce` and played; attached
  unless `HitEffectAttached` 0; off with the object when `TurnOffHitEffectOnDestroy`), its `DamageShape`; then with
  `OscillateWithDamage` the scale goes to 6, with `FallOnHit` it starts falling (state 3, unless already) about the
  horizontal axis square to the way to the monster, so **it falls away from the monster**; anything else is deleted
  at once. Fewer hits: with `PlayHitEffectOnFirstHit` (1) the `HitEffect` attached.
- Type 1: with `PlayHitEffectOnCollision` (and not already with `PlayHitEffectOnlyOnce`) its `DamageShape` and
  `HitEffect`. Then with `IsSwaying`, below state 2 and the monster moved since its last touch: the sway starts, its
  intensity (tangent · the way to the monster)⁴ with the tangent (−cos(f + π/2), −sin(f + π/2)) of the monster's
  facing f: walking straight at the tree, 1. Else with `IsCustomOscillation` the scale goes to
  `CustomOscillationScale` × 2 (the boat rocks harder while touched).
- The monster's place is kept for the next touch.

**TakeImpact** (a thrown object): its `HitEffect` on it, deleted; no score. Its caller is the SpherePhysicsMover of a
flying object that meets the street object in the collision world (0x287430, docs/COLLIDE_NOTES.md): the viewer
passes the flying module's impact on to `streetobj_take_impact`.

**onPickup**: `DamageShape`, `PickupSound` (add3DSfx(…, 0)), `HitEffect`, no longer PushOutMonsters. **onThrow**:
addStats, no longer Pickupable. **OnHitGround**: `HitEffect` where it is, deleted.

## RoofItem.bsf [R]
**Construct.** State 1; the `AttachEffect` at (0, 0, `AttachEffectZOffset`) on it, off when it dies;
`SpansTwoBuildings`: a pulse every 5 frames.

**TakeDamage**, from a monster or forceDamage's none (nFlags 2), with `TakeDamage` (1):
- one more hit; zDist = its z − the monster's, or 0 for an OnlyHitWithEffNodes object (and without a monster);
- the reach: |zDist| < 2 (**the monster on its roof**) or its z below getBuildingHeight (the roof has come down
  below it);
- standing (state 1), at `NumHits` (0) and in reach: it falls to the side away from the monster in x (ydir 1 when
  the monster's x is less), the pulse every frame, `FallLatency` (0.4 s), state 2, its `DamageShape`; the monster
  gets `AttachToMonsterMouthEffect` on its `fxNode_Belch` and, with an `AttachToMonsterShoulderEffect`, the mouth
  effect again (sic) on `fxNode_Shoulder1`, `2`, … up to 20 until one is missing; addStats (type 0) when a monster
  did it;
- else, standing and in reach, with `PlayHitEffectOnFirstHit` (1): its `HitEffect` (attached at (0, 0,
  `HitZOffset`) with `DestFallEffectAttached`, else at its place + `HitZOffset`).

**onBuildingDestruction**: standing, it falls at once (latency 0) to a random side.

**forceDamage**: nFlags 2, 99 hits, TakeDamage(none, 1000, 0): it falls without a score.

**onPulse.**
- State 2: the first time its `HitEffect` (as above; off with the object when `TurnOffHitEffectOnDestroy`) and,
  when it has one and `DeleteOnHit` (the chimneys and stacks), deleted. Then the latency runs down a pulse; below 0:
  fallOff (and the `HitEffect` stopped with `TurnOffHitEffectOnDestroy`).
- State 3 (the same pulse when fallOff just ran): down 6 a second; for the first 0.5 s also 2.5 along +y in all;
  turned by rotationPerFrame; at z ≤ 0 it returns 4|8|16: deleted.
- State 1 with `SpansTwoBuildings`: the building at its place − (`BuildingDistance`, 0, 0): falling, it falls; else
  the one at + `BuildingDistance`: falling, it falls; either missing: no more pulses.

**fallOff**: `HitSound` (add3DSfx(…, 1)), `DamageShape`, state 3, a pulse every frame; n = the frames a fall to the
street at 6 takes (its z / 6 / a frame; 0.01 for 0, after a dbgAssert); a frame's turn (π/2 / n, ydir π/2 / n, ±π/3
/ n) (the z sign at random); the y move 2.5 / (0.5 s of frames).

## The port (src/streetobj.c)
- `streetobj_add(tmpl, inst, world, building)` for each object the city placed (city.c records them in
  `Scene.street`: the block's own and its buildings' `ObjectInstance|` entries whose script is one of the two, with
  the instances of its model and its flags' cloths), then `streetobj_start(buildings)` attaches the others by the
  footprint.
- `streetobj_update(dt, monsters, buildings)` runs the pulses at the world's step, 1/30 s (0x2B7800), and
  onBuildingDestruction; `streetobj_hit` (an armed sphere: type 0, once per attack record for an
  0x8000 object) and `streetobj_touch` (the monster's box: type 1, not on 0x8000 objects) call TakeDamage, at most one
  an object a frame; `streetobj_impact` is TakeImpact; `streetobj_force_damage(name, pos)` is boss.c's
  BE_FORCE_DAMAGE.
- Events: effects (with a handle, on the object at an offset or a node, or where they start), their stops, sounds,
  the model changes, the score (route it through `army_add_stats(army, monster, tmpl, 0)`: its AE_SCORE counts the
  template for the block's challenge), the effects on the monster's nodes, the deletions.
- city.c's `scene_street_place(k, world, hidden)` moves an object's model and its flags' cloths together;
  `scene_street_shape(k, shape)` puts a new model in place of the old; `scene_instance_node` finds the node an
  attached effect sits on.

Checked in the viewer: in HK04_Shrines a kick deletes a street gong in its `Gong_hit` (its ring stops) and the
challenge counts 1/6; collapsing a pagoda drops its roof gong, hut and legs as its first storey falls, tumbling to
the street; in SF06 a punch tips a street lamp away from the monster (sparks, 1/4); in SF07 the skywalk falls in its
damaged shape when either of its buildings comes down; in GB06 a roof top falls with its flag. A test program on
the module: a blow on a London chimney (from the roof) scores 50, soots the monster's mouth and shoulders, puts out
the smoke and deletes it in `Smoke_stack_hit`; the same blow from the street does nothing to the roof trims; the
palace's HKPalaceTopAdorn2 under forceDamage falls without a score; a tree swayed by the monster walking into it
leans away, held while the monster stays within 7.

## Not traced (stand-ins in streetobj.c)
- The shape tests are the collision world's (collide.c): a blow is the attack sphere against the object's shape
  (0x280470, `collide_obj_sphere`), and so is a touch, each of the monster's collision spheres armed or not (0x2E22B0;
  main.c `mon_spheres`) [R]; the monster's box is only the push-out's (0x2E2C50). The shape is 0x28FD70's
  (`collide_shape_pick`: the template's then the instance's `CollisionBox`, `CollisionRadius` or `AutoCollisionBox`
  with the model's box; else the sphere the model keeps (0x184CB0) [R], here the union of its meshes' header spheres, city.c `city_shape_sphere`), read when the object is
  added; `box_min` / `box_max` hold
  its box. The hit lists are the game's records' (one per arming, its parts' own; 8 ids at +0x6, 0x2E33A0) kept
  the other way round: each object keeps the last eight (monster, record) that hit it (`CollideHitList`).
- `setPulseDelay` only sets the delay (0x2AA2A0) and the clock runs on: with a delay of 0.01 or more it grows by the
  step and at the delay it is taken off, onPulse once a frame; below 0.01 neither (0x309BA0) [R].
- `rotate(e)` is rot × Rz(e[2]) Rx(e[0]) Ry(e[1]) (case 0xD, 0x278258) and `rotateAA(a, axis)` rot × (I + [a axis]×)
  made square again (case 0xE, 0x278320; 0x101F70, 0x1017F0) [R].
- `requestDelete` (0x285DD0) only marks an object in the world (0x800) for the world's walk to delete at its turn,
  the rest of the script call running on [R]; here it goes at the end of the object's step (the chimney's fallOff
  after DeleteOnHit still runs), a frame earlier than the game.
- `gWorld.findWorldObject(name)` [R]: 0x279E70 → 0x2852A0 → 0x284930, h = h × 131 + c (case folded by 0x32EC08) over
  127 buckets, the first of the chain whose hash (+0x8C) and name (+0x88, 0x32C9C8 case-blind) match; addObject
  (0x284A90) puts a named object (flag 0x1000, from the instance's `Name`, 0x287DF0) at its chain's head and removal
  (0x284A10) unlinks it, so the newest one of that name wins. Here the newest roof object of that name; the three
  HK palace names (Boss_Jumper.scr:132-134, the only callers) are unique.
- `rotateAA` turns about the object's own axis, right-handed (so a pole falls away from the monster); `setFacing` is
  Rz × Rx × Ry (0x277E78 → 0x101330).
- The building's shake on an attached object is not added to its matrix (the viewer's building objects sway with
  their building through `building_place` until they move on their own).
- `gWorld.frand` is the world's one Park-Miller state (gWorld +0x828, 0x106BD0, seeded from GameStatus seed[0]), shared with every other system as in the game (wrand.c) [R]. FollowEffects follow the object's place (and node), not its turn.
- The objects are in the frame's collision world (`streetobj_collide_add`: tag `COLLIDE_TAG(COLLIDE_OWN_STREET, i)`,
  flags 4 and their own): the PushOutMonsters ones (the gongs, the chimneys) push the monster out (collide_monster),
  a flying object's TakeImpact reaches them. The pick-up of the Pickupable parked cars (`streetobj_pickup` /
  `_throw` / `_hit_ground`) is there but no grab reaches it.
