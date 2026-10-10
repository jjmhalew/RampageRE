# The parade balloons, the bikers and the food carts

What `src/parade.c` ports, and from where. [R] means read from the game's code or scripts, [G] means a guess or a
stand-in. All of them are placed by the block itself (no traffic, AI or people code makes them), and three of them
are block challenge goals ([CHALLENGE_NOTES.md](CHALLENGE_NOTES.md)): `ParadeBalloon*`, `Biker`, `HotdogCart1`.

| Script | Templates (Objects1_Props) | Where |
|---|---|---|
| `ParadeBalloon.bsf` | `ParadeBalloonGeorge`, `ParadeBalloonLizzie`, `ParadeBalloonRalph` | `ObjectInstance\|`: NY06 (all three, 14 to 16 up over the street), GB06 (Ralph, 35.7 up), LA02 (Lizzie, 27), LA08 (George and Lizzie, 22), HK05 (Lizzie, 49) |
| `Biker.bsf` | `Biker` (an actor) | `ActorInstance\|Biker_n`: SF04 (6, the "Eat 6 Bikers" block), SF06, LV03, LV07, LV08 (17 each) |
| `Bike.bsf` | `Bike` (the biker's `OTRef\|Attachment`) | made by the biker for each ride |
| `GroundCart.bsf` | `HotdogCart1`, `BurgerCart1`, `IceCreamStand1` | `ObjectInstance\|`: Chicago (CG02_Block2; a pair at about ±x in each of CG06_Block6, CG07_Block7, CG07_Block8, CG09_Block9), NY04, NY05 (all three kinds), NY06 (hot dog and burger), NY09, LA04, SF07, SF08, HK07 |

Before the port the viewer placed the balloons and the carts' food as still models (the carts without their
stands), and the bikers as people.c's `PK_OTHER` actors standing in their `Ride` pose at x = ±1, without a bike.
The `ParadeBalloonHolder1` clowns are `StationaryPerson.bsf` people (people.c), not part of this.

## The engine's pieces
**The ScriptObject's frame** [R] (0x309AC0, the update in the vtable at 0x44B1E0), in this order:
1. its mover's update; the mover's answer 2, 3 or 4 is sent as `onMoverCallback` (event 14);
2. `Process` (event 3) **every frame**: that is what drives the balloons;
3. the pulse: +0x118 grows by the frame's step and at +0x11C (the pulse delay) `onPulse` (event 13) runs; a delay
   below 0.01 never pulses;
4. a building link (+0x104, its id +0x108, set by 0x309DF0 only from the building's own object loader 0x2BC920):
   when that building's +0x1CC is no longer 0x7FFFFFFF, `onBuildingDestruction` (event 16) once (flag 0x4000000).
   The balloons are block objects, never a building's, so their `onBuildingDestruction` (float 80 up at 4, then
   deleted) never runs.

**The events' numbers** [R]: the base table at 0x3F3C30 (0 `GetTypeName`, 1 `Construct`, 2 `Destroy`, 3 `Process`,
4 `TakeDamage`, 5 `Destroyed`, 6 `Delete`, 7 `Hit_While_Invulnerable`, 8 `onPickup`, 9 `onThrow`, 10 `TakeImpact`,
11 `OnHitGround`); a ScriptObject's own from 12 (0x3F5F00: `onInit`, `onPulse`, `onMoverCallback`,
`onBuildingShake`, `onBuildingDestruction`), an Actor's from 12 (0x3F5F50: `onAnimComplete`, `onPulse`,
`onMoverCallback`, `onCellDamage`, `onFallFromWindow`, `onWalkIn`). Events go out through 0x27E3D0 (no arguments) and
0x27D850 (with arguments: `TakeDamage` from 0x28FBB0 for objects and 0x30C1C0 for actors, `onThrow` from 0x2D9F78).

**The block's actors** [R] (0x304450, the block loader's ActorInstance part):
- `SpawnedItem` ≠ 0: not made; it waits to be named by another one's `SpawnsItem<n>`.
- `SpawnDelay` ≠ 0: handed to gAIMgr (0x334A20 appends {instance, delay} at +0x34 / +0x3C). gAIMgr's update makes it
  later and sends it `onWalkIn` (event 17, 0x334194).
- otherwise made at once, without `onWalkIn`.
- **The chain** (the Actor destructor 0x30DF80): when an actor whose instance has `SpawnsItem0`, `SpawnsItem1`, ...
  goes (requestDelete, or eaten) while the world is in state 6 (a block in play), each named `ActorInstance|<name>`
  of the current block is made and sent `onWalkIn` (0x30E114). The street punks, mimes and rollerbladers of other
  blocks use the same chains (people.c does not).

## ParadeBalloon.bsf
**Construct** [R]: `PushOutMonsters | OnlyHitWithEffNodes`; `HitPoints` from the template (5; the script's
default 3); state 1; its starting place kept; facing (π/3, 0, −π/2). `Process` (every frame): in state 1 it moves,
then it wobbles.

**moveBalloon** [R] (not in state 9): a point up to 5 off along each axis of where it is now. Each axis takes its
own `frand` r: the offset is 5 r, negative when r > 0.5. The point is then kept near the start: more than 10 off in x
becomes 8 off, more than 5 in y becomes 3, more than 2.5 in z becomes 1. A LineMover there at 1 a second, state 2.

**rotateBalloon** [R], every frame: the wobble's scale (1 at rest) falls by 4 a second while above 1; the clock t
grows by the frame's step; facing = (π/3 + 0.0625 s sin 2t, 0.0625 s cos 3t, −π/2). The models face −y, so with
R = Rz Ry Rx it flies on its face, head first towards −x [G: the order of setFacing's angles].

**onMoverCallback(2)** [R]: states 1, 2 and 8 move on; 4 (knocked) goes back to its start at 4 (state 8); 9
deletes it.

**TakeDamage(monster)** [R]:
- `HitSound` (`Balloon_punch`), the wobble's scale to 20;
- knockBackBalloon (not in state 9): a LineMover at 20 to 5 along x away from the monster (−5 when the monster's x is
  greater) and 7 up, state 4;
- one hit point off. At 0: `PopSound` (`Balloon_pop`), its `HitEffect` attached (`George_Balloon_Pop`,
  `Lizzie_Balloon_Pop`, `Ralph_Balloon_Pop`), `Monsters::doChuckle` (`add3DSfx("APE_CHUCKLE_HELIUM", 0)` on the
  monster), `gCommon.addStats(balloon, slot, 0)` (200 points, category 0, the `ParadeBalloon*` goal), deleted.

There is no state guard: each TakeDamage takes a hit point, also while it is knocked back.

## Biker.bsf and Bike.bsf
**Construct** [R]: `OnlyHitWithEffNodes | Pickupable` (and the Actor's 0x80000); `Ride` at a random point of its
clip; `HasThrow` 1, `WalkSpeed` 10, `ThrowingRange` 15 (the script's defaults 1, 4, 5); its weapon off;
`flyingDuration` 3; its x becomes −80 (x < 0) or 80; pulsing every frame; state 0.

**onWalkIn** [R]: the pulse delay is the instance's `DelayedSpawn` (30 on the spawned ones), else a frame; state 0.
So a biker made by a chain waits 30 s at its end of the street (x = ±80) before it rides.

**walk** [R]: from x = 80 (if it is at x > 0) or −80, that x × (1 + frand / 40), to the other end (−80 or 80) at
`WalkSpeed`, facing that way (FACE_LEFT π/2 or FACE_RIGHT −π/2). `createBike`: the bike of the ride before is deleted
and a new `Bike` (its `Attachment`) made where the ride starts, facing the same way, on its own LineMover to the same
end at the same speed. State 1. At the end of a ride (`onMoverCallback(2)`, not in states 2 or 5) it rides back.
The bike deletes itself at the end of its line (`Bike.bsf onMoverCallback`, any state).

**onPulse** [R]:
- state 0: walk, pulse 0.5, state 1 (and on into the state-1 part below);
- `HasThrow` 0 or state 2 (held): weapon off, no more pulses;
- state 5 (flying): weapon off; `flyingDuration` less the pulse delay, deleted below 0;
- state 4 (it threw): weapon off, pulse 2, state 1;
- state 1: pulse 0.5. If the nearest monster is ahead of it (facing +x and the monster's x greater, or facing −x and
  smaller) and nearer than `ThrowingRange` (3D): `Throw`, its weapon on at that monster (`Weapon|RifleWeapon`:
  `StreetPunkBottle` from `MuzzleFlash_Small`, RefireDelay 1), state 4. Within 10 of the camera's x it plays its
  `VisibleSound` (`Harley`, level 1), every pulse.

**onAnimComplete** [R]: in states 4 and 1 `Ride` again, in state 2 `PickedUp` again.

**TakeDamage(monster)** [R], not in state 5: `smash` (smashObject, `FlyingSound` `V_Man_Falling`, state 5), then
`this.actor.onPickup()` (taken as its script's onPickup [G]: state 2, `PickedUp`, no pulses, `Voice`
`V_Biker_grabbed`), `PickedUp`, and its bike's `eventMonsterHit`: the nearest monster smashes the bike, which is
deleted 1.5 s later (counted down by its 1 s pulses). In state 2 nothing deletes the biker: it flies, lands and
lies there, still Pickupable.

**The hold** [R] (0x2DA310, then its onPickup): `PickedUp` (the `Biker_Held` model), no pulses, `Voice`. The bike
rides on alone to the end of its line.

**onThrow(monster, eaten)** [R]: eaten, `gCommon.addStats(biker, slot, 1)` (Health 20, 50 points, category 1, the
`Biker` goal) and one time in five `Monsters::timedDoFart` 2 s later; the engine deletes the eaten actor (and its
`SpawnsItem` comes). Not eaten: `smash` (state 5): `OnHitGround` in state 5 deletes it when it lands.

**The chains in the data** [R]: SF04 has Biker_0 (−1, −6) → Biker_2 → Biker_4 and Biker_1 (1, −6) → Biker_3 →
Biker_5, none delayed: two ride at once, and each one eaten (or thrown) brings the next at once, six in all. SF06,
LV03, LV07 and LV08 have Biker_0 (`SpawnDelay` 2), Biker_1 (5) and an unnamed one (3) heading chains of 14 more
(`DelayedSpawn` 30 each; the unnamed one and Biker_0 both name Biker_2).

## GroundCart.bsf
**Construct** [R]: `Edible (0x80000) | PushOutMonsters | OnlyHitWithEffNodes | Pickupable`; this.flags 0;
flyDuration 3. Its `OTRef|Attachment` (`HotdogCart1Base` → `FoodCart_Hotdog`, `BurgerCart1Base` →
`Food_Cart_Burger`, `IceCreamStand1Base` → `Icecream_Stand`: plain objects without a script) is made where it
stands, facing 0. The food itself (`Hotdog00`, `Burger`, `Icecream_Cone`) moves to the stand's node 1, or 3.6 up if
there is none. Node 1 at rest [R, the stands' RawAnims]: `Point01` (0, 0.417, 4.778), `Point02_burg` (0, 0,
5.074), `icecream_Point02` (0, 0, 4.005).

**onPickup** [R] (the hold): no longer PushOutMonsters nor Pickupable; `HitEffectBase` (`Debris_Generic`) on the
stand, the stand deleted.

**onThrow(monster, eaten)** [R]: `HitEffect` (`Debris_Generic`) on it. Eaten: `Monsters::timedDoFart` 2 s later
(always) and `gCommon.addStats(food, slot, 0)` (Health 30, 50 points into category 0, the `HotdogCart1` goal); [G]
the engine takes it away as an eaten actor. Not eaten: smashObject, this.flags 2, no longer Pickupable, pulsing
every 0.25 s, and `gCommon.addPoints(slot, 0, Points)`: 50 points without the challenge's count. `Reaction`
(`Burp`, HotdogCart1) is not read by this script.

**onPulse** [R], smashed (flags 2): 0.25 off its 3 s per pulse; below 0 its `HitEffect` and it is deleted (3.25 s
after the throw).

**TakeDamage(monster)** [R], only while the stand is there: `HitEffect` on it, `HitEffectBase` on the stand, the stand
deleted, then onThrow(monster, not eaten): smashed away for 50 points. So only an eaten cart counts for "Eat 5 Hot
Dogs" (CG07, with `Powerup_HotDog`).

## Scoring
Through `gCommon.addStats` [R] (0x2C51A0; ARMY_NOTES "Scoring"): once per object (flag 0x40000000); its award of the
template's name is what the challenge counts (0x346780).

| What | addStats type | Points | Health |
|---|---|---|---|
| a balloon popped | 0 (destruction) | 200 | |
| a biker eaten | 1 (civilian) | 50 | +20 |
| a cart's food eaten | 0 | 50 | +30 |
| a cart's food hit or thrown | addPoints, category 0 only | 50 | |

## What the port does (src/parade.c)
- `parade_load_block(p, BlockDef)`: the block's ObjectInstance| balloons and carts, its ActorInstance| bikers
  (delayed ones waiting, spawned ones only through their chain); `parade_spawn` one by template name;
  `parade_script_ours(script)` tells the block's placement which objects to leave out.
- `parade_update(p, dt, monsters, n, camera_x)`: the scripts, their movers and pulses, the delayed bikers, the
  fart timers. `parade_obj` gives each object's look (`shape`, `clip`, `anim_time` / `anim_len`, `scale` =
  RenderScale) at `world`.
- The monster: `parade_hit` (TakeDamage of the balloons, the riding or lying bikers and the carts with their stand),
  `parade_nearest_pickup` / `parade_grab` / `parade_hold` / `parade_throw`, `parade_is_edible`.
- Events (`parade_events`): effects (attached ones follow their object), sounds, the monster's chuckle, addStats,
  addPoints, the fart, the biker's weapon on and off, removals.

## Not traced / guessed
- **[G] One TakeDamage per blow.** The sender (0x28FBB0 / 0x30C1C0) has no guard, and who calls it per frame (the
  monster's attack against objects) was not traced. The port lets each armed attack (`blow`, counted by the caller)
  take one hit point of a balloon, so five blows pop one.
- **[G] The hit tests**: a balloon's box (`AutoCollisionBox`: the model's bounds, turned with it; without them its
  ShadowRect and ±3 in z), a biker's `CollisionRadius` (2) 1 up, a cart's food's `CollisionRadius` (3, 2.5) about its
  place.
- **[G] setFacing's order** (Rz Ry Rx, as army.c and boss.c take it) decides which way the balloons lean.
- **[G] `this.actor.onPickup()`** in Biker.bsf's TakeDamage is taken as the script's own onPickup (as people.c).
- **The SpherePhysicsMover** is the game's (collide.c `collide_mover_step`, 0x28E440) in the frame's collision world
  (`parade_set_collide`, `parade_collide_add`): `Gravity` (9.81 for the biker, DefObjectProps' 35 for the bike and
  the food), `Mass` (1), `Restitution` (0.8), `SlidingFriction` (0.4), `InertiaSolidity` and the shape's radius;
  smashObject's turn rates as its spin about the object's axes [G]. `OnHitGround` every sub-step it touches (the
  biker in state 5 is deleted at the first); what it meets (a building's front or side, a car) is a contact, and its
  TakeImpact (none of these scripts has one) goes to another module's object through `PAE_IMPACT`. No rest: it
  rolls on until its script deletes it. [G] The monsters are left out of what it meets.
- **The shapes in the world** (0x28FD70, `collide_shape_pick`): the template's CollisionBox, CollisionRadius (the
  bikers' 2, the carts' food 3 and 2.5, about the model's middle) or AutoCollisionBox (the balloons) with the model's
  box, else the model's bounding sphere [G: the box's], read once. The carts and balloons are PushOutMonsters: the
  monster is pushed out of them (collide_monster).
- **[G] Pulses** restart their count when set (as vehicle.c); `getTimeStep` is 1/60; `frand` is an LCG.
- **[G] The world's state 6** is taken as always true for the chains; gAIMgr's delays count from the block's load.
- **[G] Clip lengths**: from the host, else the bikers' as read from SanFrancisco.bdp (`Biker_Riding_Throw` 0.7 s,
  `Biker_Held` 1.0 s), else 1 s.
- **Not ported**: the riderless bike's `onMoverCallback` from a SphereMover (taken to come only from its line), `onBuildingDestruction` (never sent
  to these), which monster's slot gets the score with two players (the port says monster 0 / the one given).

## Driving it from the viewer
1. **Setup** (per block): `parade_new` with a host whose `find` is the army's (city_book_group), `shape_box`
   city_shape_box, optionally `clip_seconds` and `shape_node`; `parade_load_block(p, the BlockDef)`. The block's own
   placement leaves out the objects and actors `parade_script_ours` claims (only the play block's: the blocks behind
   keep their still models).
2. **Each frame**: `parade_update` with the monster (army.h's ArmyMonster) and the camera's x; draw each object not
   PAP_GONE: its `shape` (and `clip` at `anim_time / anim_len`) at `world` scaled by `scale`; apply the events
   (`PAE_IMPACT`: TakeImpact on the object with collision tag `id`). Before the update: `parade_collide_add` into
   the frame's collision world between `collide_begin` and `collide_finish` (`parade_set_collide` once).
3. **The monster**: every armed attack sphere calls `parade_hit` with the number of the blow; the ground grab tries
   `parade_nearest_pickup` (before the street's vehicles: the bikers and carts are on the sidewalk) and
   `parade_grab`; `parade_hold` each frame with the hand's matrix; `throwObject` calls `parade_throw`; a held object
   is edible (`isHoldlingEdible` 1).
