# People and the monster's grab

What `src/people.c` ports, and from where. The people are `Actor`s (vtable 0x44B300, constructor 0x30E3E0), each
driven by its template's `Script|ScriptName`. There is no native "AI" for them: everything they decide is in
their scripts (decompiled in `build/scripts/*.scr`). The engine supplies the pieces the scripts call: the anim
records, the `LineMover`, the window occupants, and the physics that throws them around.

## Object flags (+0x4C)
The names are in the table at 0x3F5F70: 0x2000 `IsBuilding`, 0x4000 `PushOutMonsters`, 0x8000 `OnlyHitWithEffNodes`,
0x10000 `Pickupable`, 0x20000 `BeingHeld`, 0x40000 `IsObstacle`. The Actor constructor also sets 0x80000 (an
actor). 0x800 means "deleting" (`requestDelete`, vtable +0x58, 0x285DD0), and 0x40000000 means "already counted"
(addStats). A template's `Flags` names set the bits, and a script's `setFlags` adds bits (there is a separate
`clearFlags`).

## The anim records (0x30AAD0)
`ActorAnimShape|<state>` has the fields `Shape|ShapeName`, `Anim|AnimName`, `Duration` and `AnimRepeat`:
- **Length of one play** (+0xC): the `Duration` if there is one, otherwise the clip's own length. For example,
  `Female_Blonde_WindowLook` is 1.2 s long but plays in 1.0 s.
- **Looping**: `AnimRepeat` sets flag 2, and the play loops.
- **The script's second argument** to `startAnim` does not control looping. Traffic starts its crowds' looping
  `Running` with 0.
- **End of a play**: a play that does not repeat ends in the script's `onAnimComplete`.

## LineMover (update 0x315E10, vtable 0x44B4E0)
- **Points and progress**: the points are at +0x34 (12 bytes each), the count at +0x3C and the segment at +0x40.
  The share of the segment covered (+0x58) grows by speed × dt / length.
- **Past the end of a segment**: the next segment starts from its beginning (callback 3, "a point reached").
  After the last segment, the person stays at the end (callback 2), unless flag 2 makes it loop.
- **Facing**: flag 1 faces the way it moves (0x316E60), and flag 8 faces its target, the nearest monster.
- **Monster response**: while it simply moves, a monster closer than `MonsterResponseDist` gives callback 4
  (+0x5C holds the distance squared).
- **Callback return**: a script's `onMoverCallback` that returns 4|8|16 deletes the actor.

## The scripts
| Script | Behaviour |
|---|---|
| `Person_Window` | **1** `WindowEmerge`, then **2**: `WindowLook`, with 20 % `WindowScared`, 20 % `WindowLook`, otherwise `WindowIdle` for 0.5–1 s (pulses every 0.25 s count the time). After 7.5 s **3** `WindowReEnter`, which closes the window and deletes the actor. A droppable falls if a monster is within 10 along x and more than 15 below, and the window is at least 18 up (rate: the block's `DroppableDef`). **Falling** (`onFallFromWindow`): **5** `Falling` straight down at 20 to (x, y−1, 0); **6** `Landing`; **7** `Running` at 10, first towards −y to y = −3 ± 1, then 100 off to a side. Held: **20** `PickedUp`. Smashed: **22**, deleted after 3 s |
| `Person_Runner` | Moves along the mover it was given. At the end of the path it is deleted. Held: `PickedUp`, facing 0. Smashed: **16**, deleted 3.5 s later |
| `WalkingAroundPerson` | `Walk` stretches of up to `WalkRange` (default 5) at `WalkSpeed` (default 4), kept within the range around its start. With `HasIdle` it stops in `Idle` and looks at the monster. A grenadier (`IsGrenadier`) faces the monster and, when `dx < 12` (signed: anywhere to its left) and the monster's z < 10 and its reload is done, plays `Throw` then `ThrowRecover`, firing its weapon (`StreetPunkBottle`) for 0.25 s and reloading for `ReloadDelay`. Held: **2** `PickedUp`, plus `Voice`. Smashed: **6**, deleted 4 s later |
| `StationaryPerson` | `Idle` at a random point in the clip, with `IdleEffect`, `Sound`, and `AmbientSound` every 7 s. Eaten: `EatEffect` at the monster's `EatEffectNode` + "1"; a `Priest` throws the monster back after `EatReactionDelay` (velocity (±10, −5, 20), reaction 2, −30 HP) |
| `Tourist` | Every 0.25 s: a monster within 15 along x gets its photo taken (`Idle`, then a flash at node `Camera`). Its mover is fixed in place, facing the monster. Kids (`IsChild`) keep `MonsterResponseDist` (10 or 6): a monster that close makes them run 160 away at 7 (`Running`), and they are deleted at the end. Held: `PickedUp2` |
| `Person_Child` | Runs along its mover. A monster in a `Ground*` move that hits it is thrown back (0, −20, 30) |

| `Mime` | Starts in state 4 on a 0.001-unit mover, whose immediate end picks a random `Anim1`–`Anim8` (state 1). Each anim plays three times, then the next one; after 8 it goes back to 1. Facing: anims 1–3 face −180°, 4 faces +90°, 5 faces 0°, 6 faces −90°, 7 faces +90°, 8 faces −90°. `Anim7` shuffles 2.5 towards −x and `Anim8` 2.5 towards +x, at 50 × the frame step per second. Held: **2**. Smashed: **8**, deleted after 4 s |
| `BandMember` (made by `Band`) | `Band` (an `ObjectInstance`, `MarchingBand1` in New York) creates its `OTRef\|BandMember1..n` 0.25 s after it starts. They form files of three (the leader alone in front), spaced 3.5 along x and 2 along y, and march at 4 from x = 50 to the band's place, facing −x. Once there they play `Playing` facing the street (**2**). The leader's `BandSound` loops, once per band. Grabbing or hitting any member sets the shared `flee`: the music stops, and every member runs at 15 to a random point up to 100 along x and at y = −10 … −60 (**8**), deleted on arrival. Held: **4**. Smashed: **16**, deleted after 3 s |
| `RoyalGuard` (made by `RoyalGuardHut`) | The hut's mover takes the guard 3 towards −y. Then **16**: 5 along `MarchDir`. **512**: `Perform` three times. **32**: `Idle` for 2 s. **64**: 5 back. **128**: 3 back in, then deleted, or, if its hut was destroyed, **256**: 80 away, then deleted. Held: **2**. Smashed: **8**. Its `flyDuration` is never set, so it is deleted at its next 1 s pulse |
| `RoyalGuardHut` | London's `StationRoyalGuardLeft` / `Right` (`MarchDir` 1 / −1). It pulses every 1 s. The first guard comes out after 4 s; after that, if a guard is still out it checks again in 6 s, otherwise it sends a new one 15 s later. Hit by a monster: it flies off on a `SphereMover` at −25 × the hit's direction in x and y, gives `addStats(hut, 0)` (10 points), tells its guard (`eventHutDestroyed`), and is deleted at its next pulse |

**Streakers** (`Monsters::createStreaker`, from `becomeHuman`): `becomeHuman` is a clip event of
`Faceup_Idle_Shrinking` at 0.763, after a defeated monster has shrunk to 0.2 over 0.83 s. It runs only while
`gWorld.getState() == 6`.
- **The actor**: `NakedFemale` if the monster `isFemale`, otherwise `NakedMale` (the script names exactly these).
- **Where it runs**: it starts at the monster's place and runs at 2.241 to y = −dir − 3 (`StreakOut`), then on along
  that line towards x = dir × 100 (`Streaking`). dir = −1 when the camera's x is greater than the monster's,
  otherwise +1.
- **How it looks**: it always faces +x, so on the −x side the scurry plays backwards (`setAnimPlayDir(−1)`).
- **When it goes**: it is deleted once `isSphereVisible(pos, 4)` fails.
- **The monster**: `becomeHuman` (0x2D3ED0) keeps the actor (+0xAAC), sets +0xAB4 and clears its own object flag
  0x20, which presumably stops it being drawn.

## The world state (`gWorld.getState()`)
`getState` (World binding 58, dispatcher 0x2AE3D0, table 0x42DD70) returns the word at 0x4D6A80. That word is the
level / block state machine:
- setState is 0x325BF0, the update is 0x3247A0 (handlers at table 0x434020), and entering a state is 0x324340
  (table 0x434040);
- most states run a `Levels.bsf` / `WorldGuiDriver.bsf` function on entry.

| State | Meaning |
|---|---|
| 1 | starting `SinglePlayer` |
| 4 | a level begins (`Levels::BeginLevelNI` / `ContinueLevelNI`) |
| 5 | a block begins (`BeginBlockNI`) |
| **6** | **a block is being played**: entered from 4 … 11 once the block is ready (it asks `IsBlockFinished`) |
| 8 | game over (`WorldGuiDriver::startResults`, `GameOverNI`) |
| 9 | end of the block (`EndBlockNI`) |
| 10 | end of the level (`EndLevelNI`) |
| 11 | end of the world (`EndWorldNI`) |
| 15 | reset or unload |

The Actor destructor (0x30DF80) also spawns an instance's `SpawnsItem<n>` only in state 6.

**`TakeDamage`** (an armed attack sphere reaching a person) smashes that person, except:
- a window person anywhere but state 7;
- a kid tourist;
- anyone already flying.

The window and walker scripts call `onPickup` after `smash`, which sets the state back to 20 or 2. Read literally,
a person punched this way is never deleted. The port keeps this behaviour.

## Window occupants
- **Spawning** (0x318650, from the world update 0x2B6CE0): the manager at 0x4D5F00 counts up to its interval and
  then places one occupant. Its fields come from the block's `Occupants` group (0x31A540): +0x44 `SpawnRate`
  (the interval, 0.75 s by default), +0x48 `MilitaryStartRate` (0.1), +0x4C `MilitaryEndRate` (0.9) and +0x50
  `MilitaryTimeRange` (240 s); `FillRate`, `NonMilitaryRate` and `DropRate` are read too. On the interval, with the
  chance MilitaryStartRate + min(1, t / MilitaryTimeRange) × (End − Start) (t from 0x347DC0 on 0x4D5190, a game
  clock) it looks among the military slots (count +0x40), otherwise among the civilian ones (+0x3C, after them);
  from a random slot on, the first free one (+0x14 empty, +0xD an occupant) whose building's x lies in the view
  (0x318390) and that 0x31AD00 allows. The slots (0x31A540 calls 0x3196F0 per building with the
  Occupants' `FillRate` and `NonMilitaryRate` and the Edibles' `FillRate`): of the building's n windows (its list
  from 0x2BE750 / 0x2D01F0: the intact front cells on live storeys 1 and up, never the first storey above the ground
  floor, whose column has an `Openable` window; of a column's two, `LRPos` 0 or 1 at random (Park–Miller < 0.5: 0
  first), else the other: the slot's +0xC), round(n × FillRate) in a shuffled order, the
  first total − int(total × NonMilitaryRate) of them military (+0x40), the rest civilian (+0x3C). The building's
  `NumNonMilitaryOccupants` (then all n windows, that many civilians) and `NumMilitaryOccupants` override the
  counts, each capped at n. A slot's kind is a random one of the sort's list (0x318B40 makes it from the
  `MilitaryInstance|` / `OccupantInstance|` records; the viewer weighs them by `Frequency`). A military occupant
  is the army's window soldier (`army_spawn_window`, Military_Window.bsf).
- **Placement** (`getWindowPosition` 0x2BE660, with the slot's `LRPos`): the actor stands at its window's bottom middle:
  - x: the grid's min x + 5 × column + the middle of the column's `Openable` (0x2CFF30; not the `Outline`);
  - y: the front's y;
  - z: the storey's height + the `Openable`'s bottom (and +0xF4, its height above the storey's floor).
- **The window opens** as the occupant comes (0x318B04: `openWindow(storey, column, LRPos, 1, 0.5)` on its building):
  over 0.5 s a near-black quad grows over the window's `Openable` part, just in front of the facade, upwards (a sash)
  or, with `OpensH`, from the left (docs/FORMAT_BDP.md "Open windows"). The occupant leans out of that dark.
- **The window shuts** only through the actor binding `closeWindow` (0x30C3F0: `openWindow(…, 0, 0.5)` on the slot's
  storey, column and `LRPos`, the slot found by the actor's +0xEC), which `Person_Window` and `Military_Window` call
  at the end of `WindowReEnter`, then the actor is deleted: it closes over 0.5 s once the occupant has gone in. Falling
  out (`leaveBuilding` 0x30C3D0 → 0x318140(mgr, actor, 0)), being grabbed (0x2B3350 → 0x318140(…, 1), which also
  retires the slot: its +0xD = 0xFF, never filled again), the fall's 0x30C510 and the actor's end (0x30DF80) only
  empty the slot: that window stays open. Damage to the cell drops it at once (0x2BB580). The edibles' slots (+0x10)
  never open a window.
- **The fall** (0x317DC0, from cell damage 0x2BB580): when a front cell's state is 2 or more, every non-military
  window person in that building within ±1 storey of it falls (`onFallFromWindow`), whatever its column.

## The grab

### canGrabPerson and doGrabPerson (on a building)
The `Monsters.bsf` wrappers call `getBuildingOccupant` (0x2B37C0) with George's arguments (0, ±2.5, −0.5, 8.529).
The monster.py printout shows only three of them; the fourth is in the data.
1. **The probe point**: the monster's place + (x, y, z), with x negated when the monster plays mirrored. The offset
   is not rotated by the monster's facing, and the point's y is then lowered by 1.
2. **The building** (0x2A3D20, looking along +y): among the buildings whose x range holds the point, the one whose
   front lies nearest at or ahead of it.
3. **The cell** (0x2B3900): column = trunc((x − min x) / 5), storey = trunc((z − foot − 6) / 3).
4. **The answer**: the occupant in that cell. An actor answers 1 (`canGrabPerson`). An item answers 2
   (`canGrabPowerup`), but only once the cell is broken (state 2 or more), or 3 if its type is a hazard.

`grabBuildingOccupant(loc, 1)` (0x2B3350) then:
- stops the actor's effects (0x30C5E0);
- holds it with a 0.25 s blend (0x2DA310);
- takes it out of the window (0x318140).

Its `bDoEat` argument only matters for items.

### doGrabObject("Helper R Hand") (on the ground, `Ground_Grab_Down` at 0.317)
The search (0x2D99C0) finds the nearest object to pick up:
- **Where it looks**: within `MONSTER_DATA` +0x1B8 (15 for George), times the monster's scale (×4 when +0xBAB0 is
  set; no monster sets it), of the hand node. Add 10 if an earlier `canGrabObject` search found something.
- **Which objects count**: flagged 0x10000 and not 0x20840.
- **Where it must be**: in front of the root (dot product with the monster's +y axis > 0).

It holds the result with a 0.1 s blend (0x2DA4A0). `pickupSuccess` / `hasGrabbedObject` answers 2 for an actor
(0x2E5590: flag 0x80000), so `isHoldlingEdible` lets every person be eaten.

### The hold
In 0x2DA310 the object becomes `BeingHeld`, loses `Pickupable` and `PushOutMonsters`, gets a `HeldObjectMover`, and
the script's `onPickup` runs (script event 8).

The node is the monster's `LiftNodeName` (`"Helper R Hand"` in the monsters' `MiscData`, monster +0x100). The
`HeldObjectMover` (init 0x310ED0, held 0x3109E0) places the object like this:
- **Orientation**: the object's `MajorAxis` (default 2, z) runs along the hand's z, the next axis along the hand's
  y, and the third is their cross product. For people this gives x = hand y, y = −hand x, z = hand z.
- **Position**: the hand's position + `HoldOffset` in the hand's frame (`MirroredHoldOffset` when the monster is
  mirrored). People have neither.
- **Blend**: over the blend time (+0x84), the object comes in from where it was.

### throwObject(eat, x, y, z) (0x2D9DC0)
The script's `onThrow(monster, eat)` always runs. After that:
- **A non-actor** gets the clip's velocity (turned by the monster's matrix and its +0x2560 angle) on a
  `SpherePhysicsMover`.
- **An actor** gets no velocity at all.
  - If eaten, it is deleted. Its script's `addStats` (0x2C51A0) gives the monster:
    - HP = max(1, HP + `Health`) up to `MONSTER_DATA` +0x138 (200), only while HP > 0. People mostly have
      `Health` 20; `Dragon` has −20.
    - Energy + `Energy`, within 0 … 100 (100 starts `startRage`).
    - `Points`.
    - A 1 in 5 chance of `Monsters::timedDoFart` 2 s later.
  - If not eaten, the script calls `smash` → `smashObject` (0x2B2F50).
- **Any let-go is a throw**: the many `throwObject(0, 0, 2, −2)` at the start of jumps and reactions smash a held
  person.
- **The face**: the eating clips set the expression themselves (`faceAnim` 13 Chomp, then 10 Chew, with a belch 5 s
  later).

### smashObject (0x2B2F50)
This works on an actor, or on an object of class 0x4D0720.
- **The direction**: a yaw from 0x2B4390. It starts from a 120° cone around −y (3π/2, towards the camera) and
  narrows it on the side the monster faces: 20° when the monster faces +y, the full 120° when it faces the camera.
- **The speed**: 15 along that yaw, and 15 × (0.8 + 0.5 r) up.
- **The spin**: ±(2π + π/6 r) about x, and ±(π/16 + π/16 r) about y and z.
- **The flight**: a `SpherePhysicsMover` (0x28DE70, properties from 0x28F220): `Gravity` 9.81, `Mass` 1 and
  `Restitution` 0.8 unless the template sets them. People do not set them.

## Not traced (stand-ins in people.c)
- **Random numbers**: the exact `frand` sequence (an LCG stands in; the game's is Park-Miller on gWorld +0x828).
  `getTimeStep` is the world's step, 1/30 (0x2B7800, FLOW_NOTES.md) [R].
- **The physics mover's collision response**: a vertical bounce with restitution 0.8 on z = 0 stands in.
- **Blending and the hit test**:
  - the shape of the hold blend (taken as linear);
  - the attack-sphere test against people (0x2D4390): `CollisionRadius` 2 about the person, 1 up.
- **Occupant spawning**: the timing and rates (+0x44 … +0x50), the view check (0x318390) and the cell check
  (0x31AD00: the cell not hidden, 0x400 by 0x2BE580, then a building search 0x2A3D20 from a point 10 lower [G]).
  A grabbed occupant's slot is not retired.
- **The windows** (city.c's spawner opens them, `scene_window_close` / `_at` shut them): the port's `closeWindow` is
  for people.c and army.c to call (by the cell people_add_window got, or by the occupant's place); until they do, a
  window stays open after its occupant went back in.
- **The Actor bindings' bodies**: `startAnim`'s second argument, `onPickup` (taken as sending script event 8), and
  `getStringProp` versus `getTemplateStringProp` (only the template is read).
- **Scripts not ported**: any other script plays its `DefaultAnim`, and if smashed is deleted after 3 s.
- **The LineMover's end**: it fires once; the port then stops the mover until a handler starts another.
- **Band timing**: the band's members appear 0.25 s after `people_add_band` (its `onInit` pulse).
- **The flying hut**: its flight is left to the viewer.
- **Drawing**: `RenderScale` (`DefActorProps`: 0.65) is for the renderer to apply.
