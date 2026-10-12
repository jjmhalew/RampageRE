# Street and roof objects (StreetObject.bsf, RoofItem.bsf, MerryGoRound.bsf, Sign.bsf)

The lamps, benches, mailboxes, trees, fences and parked cars along the street, the gongs of the Hong Kong shrines,
and everything that stands on a roof: chimneys, smoke stacks, antennas, fans, the London and Las Vegas trims, the
pagoda huts, the palace ornaments, the skywalks between two buildings. Two scripts drive them all; `src/streetobj.c`
ports both, with the engine pieces around them, and a third, the playground's merry-go-round (below). [R] read in the
scripts (`build/scripts/StreetObject.scr`, `RoofItem.scr`, `MerryGoRound.scr`) or the ELF, [G] guessed.

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
| MerryGoRound.bsf | Construct, TakeDamage, onPulse (no onInit, onPickup, onThrow, TakeImpact or OnHitGround) |
| Sign.bsf | Construct, TakeDamage, TakeImpact, onPulse, onBuildingDestruction |
| RotationMover | its step 0x31C540; the angles from a matrix 0x101220, the matrix from them 0x101330 |
| Monster queueFacingZRequest / queuePushRequest | the junction's cases 0x2AB370 / 0x2AB350 → 0x2E97D0 / 0x2E9860; taken by the monster's update at 0x2DEB04 |

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

**The AttackObjectInstance| ones** [R]: a city book placing a StreetObject.bsf template as an
`AttackObjectInstance|` (not `ObjectInstance|`) makes it an AttackObject (0x3050F0's second list → 0x315330), which
rocks after each update (0x314EF0, BOSS_NOTES.md "The AttackObject's oscillation"): Tree1 (template offset −1, the
instance's `OscillationScale`: 0.035 in Chicago, 0.01 in the other cities), PalmTreeShort / PalmTreeTall (the
template's 0.02, offset −1) in Las Vegas and Los Angeles. The facing (taken apart from the turn it has: the sway's,
the fall's) gets (sin 2φ, cos 3φ, 0) × the scale less the last one, φ starting at frand × 2π: a tree rocks by ± the
scale in radians, on top of its sway. Here `streetobj_add(..., attack)` (city.c's `SceneStreetObj.attack`) reads the
keys, `step` adds it (army.h `ArmyOsc`).

**TakeImpact** (a thrown object): its `HitEffect` on it, deleted; no score. Its caller is the SpherePhysicsMover of a
flying object that meets the street object in the collision world (0x287430, docs/COLLIDE_NOTES.md): the viewer
passes the flying module's impact on to `streetobj_take_impact`.

**onPickup**: `DamageShape`, `PickupSound` (add3DSfx(…, 0)), `HitEffect`, no longer PushOutMonsters. **onThrow**:
addStats, no longer Pickupable. **OnHitGround**: `HitEffect` where it is, deleted.

**The pick-up** [R]: the Pickupable ones (0x10000: CarParkedCoup / Sedan / Taxi, with their `Car_Alarm`, and the
roof cars CarParkedCoupRoof / SedanRoof, whose RoofItem.bsf has no onPickup, onThrow or OnHitGround) are found by
doGrabObject's search (0x2D99C0: within 15 of the hand, in front of the root, not 0x20840) like the people. The hold
(0x2DA310) makes one BeingHeld, neither Pickupable nor PushOutMonsters, then onPickup; the HeldObjectMover (0x310ED0)
keeps it in the hand's MajorAxis frame (the parked cars set none: 2) at its HoldOffset. throwObject (0x2D9DC0, from
Monsters.bsf's `throwObject(vel, 0, eat)`): with its second argument 0 the clip's velocity is in the monster's frame,
turned by its matrix (and by its +0x2560 angle) into the world; the object gets a SpherePhysicsMover with it, no
spin, then onThrow. It flies (0x28E440) in the frame's collision world: the street calls OnHitGround (the car breaks
in its `HitEffect` and goes), the objects it meets get TakeImpact (a building is hit at radius Mass / 1000, main.c
`impact_route`) and so does it (its `HitEffect`, deleted). `isHoldlingEdible` (hasGrabbedObject == 2, an actor) is 0
for it, so a car is thrown, never eaten.

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

## MerryGoRound.bsf [R]
One template, `MerryGoRound` in `Objects1_Props` (`Shape|Shape` "MerryGoRound_base", `Effect|AttachedEffect`
"MerryGoRound", `Effect|HitEffect` "object_destroyed_generic", `CollisionRadius` 0.7, `HitPoints` 5 and `Points` 100,
which the script never reads; no `Flags`), placed once in each of three playgrounds: NY01_School (New York block 1,
at (−18, 8) by the swings and the basketball hoops), GB04_BakerSt (London block 4, (10, 8) between two benches) and
SF04_HaightSt (San Francisco block 4, (5.09, 6.43) by the swings). The three cities' `MerryGoRound.bsf` are the same
script. Its shape is a sphere of 0.7 about its model's sphere's centre (0x28FD70): the monster walks onto the ride (no
PushOutMonsters) and touches it at the middle.

**Construct.** State 1, a pulse every 0.2 s (`currPulseTime` 0.2), the object flag 0x2000000 (the trains' too; no
code tests it, RAIL_NOTES.md), a RotationMover of (0, 0, 0) as its mover.

**The RotationMover** (0x31C540): each frame its rate × the world's step (gp−0x6CFC +4, 1/30) is added to the angles
read back from the matrix (0x101220: z an atan2, within −π .. π, which is also `getFacing().z`) and the matrix made
again from them (0x101330, Rz Rx Ry). `setRotation(v)` sets the rate. So the ride turns about its centre at the rate
`angleVel3.z`, in radians a second.

**onPulse.**
- State 1 (idle): below π/2 the rate gets π/2 more: the ride turns at a quarter turn a second from its first pulse.
- State 2 (slowing): at π/2 or less, state 1; else the rate loses 30% a second (rate × 0.3 × `currPulseTime`).
- State 3 (a monster on it, every frame): the monster gone (`isValid`), state 2. Else `durationGrab` runs down;
  while it lasts the rate grows 2% a frame (`scaleMerryGoRoundRotation(1.02)`: × 2.16 over 1.3 s at 1/30) and the
  monster gets `queueFacingZRequest(the ride's facing z)` and `queuePushRequest(the ride's place)`; at the end it is
  flung: `setVel((cos a, sin a, 1.2) × 1.2 × the rate, 0)` with a the ride's facing z (a world velocity: 0x2E74F0
  turns it by the monster's matrix only for a nonzero second argument), `playReaction("React_Ground_Slip_Heavy")`,
  `queueFacingZRequest(its facing z + π)` (turned round), the rate × 0.3, the pulse back to 0.2 s,
  `durationNoHit` 2, state 2.

**TakeDamage(who, amount, type, direction)**, any type (a blow or the every-frame touch), from anything:
`durationNoHit` loses `currPulseTime`; while it is above 0, or with a monster on the ride (state 3), nothing. Else
`durationNoHit` is 2 again and, from a monster: if the rate is under 6π and its move's name starts with `Ground`
(`getMoveName().fFind("Ground") == 0`), the rate gets 4π × r more (r = frand, 0.5 added below 0.5: 0.5 .. 1), its
`AttachedEffect` (getStringProp: the instance, then the template) starts where it stands (the "MerryGoRound" twirl:
twirl particles round a 5-high column, its `twirl loop` sound) and its `HitEffect` (getTemplateStringProp) on it.
Then state 1 becomes 2, and in state 2 a monster in a `Ground` move is taken on: `durationGrab` 1.3, `currPulseTime`
and the pulse the world's step (a pulse every frame), state 3, onPulse at once.

So a monster walking onto the ride (`Ground_Walk`, `Ground_Idle`) is taken at its first touch: the ride spins up to
about 3 turns a second with the twirl, the monster stands at its middle turning with it for 1.3 s, then slides off
along the turn at 1.2 × the rate (30 or so a second), turned round, and `React_Ground_Slip_Heavy` (mode 2 at 0.104,
mode 1 and `addVelocity(0, 0, −20, 15)` at 0.438, then `Jump_Air_Borne_Fall`) throws it into the air. After a ride
each touch takes 0.2 off the 2 s: the eleventh touching frame starts the next. No score, no damage, no sound of its
own (the twirl's loop is the effect's), no pick-up, no TakeImpact.

**The monster's requests** (0x2E97D0, 0x2E9860) [R]: queueFacingZRequest(z) makes Rz(z) (0x1015B0) and stores it at
monster +0xC754 with the flag +0xC750; queuePushRequest(pos) stores pos at +0xC744 with the flag +0xC740; both then,
with foot IK (+0x258C), let both feet go (0x33BE90(+0xC81C), as the push-out). The monster's update (0x2DEB04 ..
0x2DEBAC) takes them into the matrix it starts its frame from: the place replaced (x, y and z), the turn replaced,
the flags cleared. Only MerryGoRound.bsf calls queueFacingZRequest; queuePushRequest also the freighter and the army's
pushMonster (rail.c, army.c).

## Sign.bsf [R]
The signs hung on the buildings' fronts: 65 templates (CasinoVerticalSign, CasinoHorizontalSign, SignParking1,
GoldrushSign, LuckysSign, OasisSign, ...; parents DefSignProps, DefElectricSideSignProps with
`OnlyHitWithEffNodes+IsObstacle`, DefFaceSignProps, DefLogoSign*Props with `Hits` 4). Made by their building's
instance, so attached to it (0x4000000) and swayed and sunk with it until they come loose.
- **Construct**: flags 0, no hits; one `gWorld.frand()` for a `fallRot` that onPulse never reads. No pulse.
- **TakeDamage**: only from a monster, any type. The hits count up; when it is not loose yet, `TakeDamage` (1) is set
  and the count reaches `Hits` (0): loose (flags 1, a pulse every step), its `HitEffect` attached, IsObstacle and
  OnlyHitWithEffNodes cleared (0x40000 | 0x8000), `addStats(object, slot, 0)`.
- **TakeImpact**: loose, its HitEffect attached; no score, the flags kept.
- **onBuildingDestruction** (0x309AC0: the first storey of its building to fall): loose, its `DetachEffect` (the
  electric signs' sparks) where it is, the two flags cleared.
- **onPulse**: z − 10 and y − 3 a second (towards the street), turned (π / 10, 3π / 20, 0) a second about its own
  axes; at z ≤ −0.25 its `GroundEffect` there, and it returns 4 | 8 | 16: deleted.
- A loose sign's IsObstacle climbing box goes (city.c `scene_street_place`).
- The templates' `CollisionBox` is keyed `CollisionBox|CollisionBox` in the books; `book_box` reads that first (it
  read only `CollisionBox`, so every street, roof and sign object fell back on its model's sphere).

## The port (src/streetobj.c)
- `streetobj_add(tmpl, inst, world, building, attack)` for each object the city placed (city.c records them in
  `Scene.street`: the block's own and its buildings' `ObjectInstance|` entries whose script is one of the three, with
  the instances of its model and its flags' cloths; attack for the block's `AttackObjectInstance|` ones), then
  `streetobj_start(buildings)` attaches the others by the footprint.
- `streetobj_update(dt, monsters, buildings)` runs the pulses at the world's step, 1/30 s (0x2B7800), and
  onBuildingDestruction; `streetobj_hit` (an armed sphere: type 0, once per attack record for an
  0x8000 object) and `streetobj_touch` (the monster's box: type 1, not on 0x8000 objects) call TakeDamage, at most one
  an object a frame; `streetobj_impact` is TakeImpact; `streetobj_force_damage(name, pos)` is boss.c's
  BE_FORCE_DAMAGE.
- Events: effects (with a handle, on the object at an offset or a node, or where they start), their stops, sounds,
  the model changes, the score (route it through `army_add_stats(army, monster, tmpl, 0)`: its AE_SCORE counts the
  template for the block's challenge), the effects on the monster's nodes, the deletions.
- The MerryGoRound (`SK_MERRY`): its RotationMover runs each step before its pulse (as rail.c's movers); the monster's
  move name comes in `StreetMonster.move` (getMoveName). Its calls on the monster are events: `SE_MONSTER_FACING`
  (queueFacingZRequest: main.c sets the heading, z × 57.2957795) and `SE_MONSTER_PUSH` (queuePushRequest: the place),
  each letting the feet go (`foot_step`, no anchor); `SE_MONSTER_VEL` (setVel(v, 0): `Mon.vel`) and
  `SE_MONSTER_REACTION` (playReaction: that move state now, while alive). main.c applies them after the objects' step,
  the game at the monster's next update: the same frame's place and turn, the clip's travel of the frame added after.
- city.c's `scene_street_place(k, world, hidden)` moves an object's model and its flags' cloths together;
  `scene_street_shape(k, shape)` puts a new model in place of the old; `scene_instance_node` finds the node an
  attached effect sits on.

Checked in the viewer: in Las Vegas block 2 George picks a parked sedan up from the lot (its `Car_Alarm`), throws
it (25 points) and it breaks where it lands; in HK04_Shrines a kick deletes a street gong in its `Gong_hit` (its ring stops) and the
challenge counts 1/6; collapsing a pagoda drops its roof gong, hut and legs as its first storey falls, tumbling to
the street; in SF06 a punch tips a street lamp away from the monster (sparks, 1/4); in SF07 the skywalk falls in its
damaged shape when either of its buildings comes down; in GB06 a roof top falls with its flag. A test program on
the module: a blow on a London chimney (from the roof) scores 50, soots the monster's mouth and shoulders, puts out
the smoke and deletes it in `Smoke_stack_hit`; the same blow from the street does nothing to the roof trims; the
palace's HKPalaceTopAdorn2 under forceDamage falls without a score; a tree swayed by the monster walking into it
leans away, held while the monster stays within 7. In NY01_School the merry-go-round turns a quarter turn a second
on its own (45 degrees between shots 0.5 s apart); George put on it (`--do "at -18 8"`) is taken at once in the twirl,
turns with it at its middle for 1.3 s and is flung off towards the street, sliding and then thrown up by
React_Ground_Slip_Heavy, his heading turned round; the same in GB04_BakerSt.

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
- `rotateAA` turns about the object's own axis, right-handed (so a pole falls away from the monster; 0x278320, above);
  `setFacing` is Rz × Rx × Ry (0x277E78 → 0x101330) [R].
- The building's shake on an attached object is not added to its matrix (the viewer's building objects sway with
  their building through `building_place` until they move on their own).
- `gWorld.frand` is the world's one Park-Miller state (gWorld +0x828, 0x106BD0, seeded from GameStatus seed[0]), shared with every other system as in the game (wrand.c) [R]. FollowEffects follow the object's place (and node), not its turn.
- The objects are in the frame's collision world (`streetobj_collide_add`: tag `COLLIDE_TAG(COLLIDE_OWN_STREET, i)`,
  flags 4 and their own): the PushOutMonsters ones (the gongs, the chimneys) push the monster out (collide_monster),
  a flying object's TakeImpact reaches them, and a thrown one flies in it (`streetobj_nearest_pickup`, `_grab`,
  `_hold`, `_fly`, main.c's doGrabObject and throwObject). The hold blends in from where it was over doGrabObject's
  0.1 s (0x310B50, army.c's `army_hold_blend`, ARMY_NOTES "The hold") [R]; the throw's +0x2560 angle is taken as 0.

## Billboard.bsf, Billboard_2Part.bsf [R]
The billboards on the roofs and the Las Vegas marquees: 32 templates (ScumSodaBillboard, BillboardBeans and its
Back, the BillboardBreakaway* with their fronts, MidwayBillboard, BillboardHotel, StompBillboard1, WesternBackdrop,
BillboardKenoMarquee, BillboardPrimeRibMarquee, the invisible BirdsOnlyInvisBillboard1, ...), in every city.
- **Construct**: `Type` (getStringProp, "Generic"), `useBirdFx` (getIntProp, 1; the books write `useBirdFX`: the
  key is found in any case, 0x1B4490), requiredHits 3. No pulse.
- **onInit**: one `gWorld.frand()` (always above −0.33): with useBirdFx 1 the `Pigeon_walk` effect on its top, 0.5
  behind its place (z + getShapeBounds' max z [G: the model's box]). Then for N = 1 .. 5, while the object has an
  `AttachmentN` (an `OTRef|`) and its model a node N (`getNodePosition`: the tree's node N), an object of that template
  (`gWorld.createScriptObject`) put at the node, 0.5 nearer the street (setPoint alone: unturned). Here city.c makes
  them with the billboard (`place_attachments`, the node at the tree's rest pose); they are drawn with the building
  until they move.
- **TakeDamage**, only from a monster (the templates are `OnlyHitWithEffNodes`: a blow, once an attack):
  - state 0: its `SoundDestroy`, `DestShape`, `DestEffect` on it (not `BirdsOnly`); each attachment's
    `Atch<i>DestFx1` on it, and then state 2 (the next blow takes them); otherwise, and always for a `2Part`, the
    attachments go (`AtchDestFx` where each is; deleted, or a 2Part's front gets `onStartFall`). The birds fly
    (`Pigeon_fly` on its top, birdEffect stopped). State 1.
  - state 2: SoundDestroy again, `Atch<i>DestFx2` on them, they go; state 1.
  - state 1: three more blows, each 0.5 s more shaking, a pulse every frame, `addStats` (counted once: 0x2C51A0);
    the third starts the fall (fFalling 4) in `Collapse_Floor_redbrick_comp` (not BirdsOnly).
- **onPulse**: no hits left, it turns over about its x at `DestFallDir` × π a second (1: its top to the street); at
  ±π/2 its `DestFallEffect` (`building_collapse_comp`) on it, state 5, and it goes (4|8|16) unless fFalling still
  runs: then it sits at its building's top (getBuildingHeight, going down with it) until 4 s from the start.
  While shaking, each frame a step of −1/16 .. 1/16 added to x and the same to z, not undone (it wanders).
- **onBuildingDestruction**: before it has fallen: the attachments go when it is still whole or a `Midway` one; the
  birds fly from where they sat; it falls (fFalling 4). The marquees stand on the street in no building's
  footprint, so their buildings never take them.
- **Billboard_2Part.bsf** (the breakaway fronts): Construct picks fRotDir ±1 (+1 when `gWorld.frand() < 0.5`).
  `onStartFall`: a pulse every frame and a SplineMover at 10 a second from where it is to 3 out (−y) and 3 up, then
  down to z −3 [G: straight between the points], deleted at its end (`onMoverCallback(2)`). onPulse: turning about
  its own y at 2π a second.
- The attachments without a script (the Midway logo and name) have no TakeDamage: blows pass them.
- Here streetobj.c `SK_BILLBOARD`, `SK_2PART`, `SK_ATTACH`; `--do "strike k"` is a blow on street object k.

## Billboard_George.bsf [R]
GeorgeBillboard (New York, NY09): George pours Scum Soda into his mouth. Its jaw (`Attachment1`, node 1, 0.5 out) and
the bottle in its hand (`Attachment2`, node 2, 0.25 out) turn every frame (Process):
- **Construct**: `Type` (the template's), requiredHits 3, `setFlags(0x8000)`: OnlyHitWithEffNodes alone.
- **onInit**: the pigeons when `gWorld.frand() > 0.33`, on its top.
- **Undamaged**: the hand up to π/3.5 at that a second (`soda_pour` at its top, 1 s still), back down (0.75 s still);
  the jaw with it to π/4, set at that − π/10.
- **TakeDamage** (from anything; the score only from a monster; nothing once its building has gone): the first blow
  `DestShape`, `DestEffect` on it, the birds fly, state 1: the hand up to 0.62π at 2π a second, 3 out and 1 down over
  0.25 s, the jaw 1.85 down at 5.5 a second, `soda_pour` while the hand is up; all three there, the bubbles
  (`soda_george_mouth`). Then two blows shake it, the third counts and the hand and jaw go (`AtchDestFx`): it falls as
  a billboard does (at ±π/2 set to π × DestFallDir).
- **onBuildingDestruction**: it falls (fFalling 4), the hand and jaw go (their effect only while it was whole), the
  birds fly.
- `soda_pour` and the bubbles are at fixed places, (22.25, 18.75, 42.95) and (21.5, 18.35, 39.75), 54 to the right of
  where NY09 puts the billboard (−32.3, 9.7, 25.5): as the game has them.

## AnimatedSign.bsf [R]
VegasCowboySign (Las Vegas block 1, on the Gold Rush building's roof): an `ActorInstance|` of its building, the only
actor any building places; city.c places it with the building's objects. Its own clip (Sign_Cowboy's arm, the
`Sign` ActorAnimShape with no Anim) loops.
- **Construct**: flags 0, a fallRot onPulse never reads (one `gWorld.frand()`).
- **TakeDamage** (a monster's blow: it is `IsObstacle+OnlyHitWithEffNodes`), **TakeImpact** (a thrown object) or
  **onBuildingDestruction** set it loose once: its `HitEffect` (sparks) on it (a blow counts too, `Points` 100), or
  its `DetachEffect` where it is; a pulse every frame.
- **onPulse**: down 10 a second, turned (π/10, 3π/20, 0) a second about its own axes; at z ≤ −0.25 its
  `GroundEffect` (Debris_Generic) there and it goes. Unlike Sign.bsf no drift to the street, and its flags stay.
- Here streetobj.c `SK_ANIMSIGN`.

## Fountain.bsf, DamagingEffect.bsf [R]
- **Fountain.bsf** (`Fountain1`, the fountains' sculptures, in six cities; `FireHydrant1`, the hydrants of Chicago,
  New York and Los Angeles): Construct `setFlags(0x8000)` (only an armed blow, once an attack); onInit with
  `AttachFountainFX` its `fountain` effect at its model's nodes 1 .. 4. The first TakeDamage (from anything) or
  TakeImpact: its `Geyser` (`OTRef|`, `Fountain3`) made where it stood (`gWorld.createScriptObject`); it is put 3 up
  and off on a SphereMover at the blow's way × 10 along x and y (a thrown object's: none) and 25 up, a pulse a second;
  a monster's blow counts. At the fourth pulse `Impact_concrete_Big` where it is, and it goes. The blow's way is the
  monster's facing here [G].
- **DamagingEffect.bsf** (the geysers: Las Vegas' `Fountain2`, `Fountain2_silent` in Nero's pool, and `Fountain3`;
  the puddles `PukePuddle`, `SoapPuddle`, `ChocolatePuddle`): Construct a pulse after `DeleteSelfTime` (none: it
  stays), which deletes it, and its `Effect` (the `Geyser` jet) on it. TakeDamage, any type (the touch every frame),
  from a monster within `HitDistance` (1.5) along x and along y: its `PlayReaction` (`React_Geyser`) while the
  monster's move is a Ground one, its `ReactionSound` (`Monster_slip`), and with `SetMonsterVelocity` setVel(10 away
  along x, −5 × frand, `MonsterZVelocity` 20: the books' `MonsterZVel` is not that key). `React_Geyser`'s own events at
  0 (mode 1, `addVelocity(0, 0, 18)`) throw the monster up.
- A reaction a module plays (playReaction: the geyser, the merry-go-round, the bosses) starts at the monster's next
  step as a slot's start does, so the state's events at its time 0 fire (main.c `force_state`, `Mon.force_to`);
  started between the steps they were passed over and `React_Geyser` left the monster on the ground.
- The geysers are InvisShape objects: city.c lists them with no instances; streetobj.c makes `Fountain3` itself (the
  host's `find`), `StreetObj.made`.
- The bridges: the Las Vegas lagoon's wooden `Bridge` (StreetObject.bsf, its blow's `Building_Collapse_parts`, then
  gone) and Chicago's `ChicagoBridgeSupport` (StreetObject.bsf, OnlyHitWithEffNodes) break at a blow; the elevated
  train's `TrainBridge1`, `TrainBridge2` are ScriptObject.bsf: they stand.
