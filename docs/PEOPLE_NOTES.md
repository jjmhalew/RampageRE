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

**`TakeDamage`** (an armed attack sphere reaching a person) smashes that person, except:
- a window person anywhere but state 7;
- a kid tourist;
- anyone already flying.

The window and walker scripts call `onPickup` after `smash`, which sets the state back to 20 or 2. Read literally,
a person punched this way is never deleted. The port keeps this behaviour.

## Window occupants
- **Spawning** (0x318650, from the world update 0x2B6CE0): on a timer, with a chance that ramps between two rates
  (fields +0x44 … +0x50 of the manager at 0x4D5F00; where these are set was not found), the game picks a random
  free slot whose building is in view.
- **Placement** (0x2BE660): the actor stands at the window's bottom middle:
  - x: the grid's min x + 5 × column + the middle of the column's `Outline`;
  - y: the front's y;
  - z: the storey's height + the `Outline`'s bottom.
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
- **Random numbers**: the exact `frand` sequence (an LCG stands in) and `getTimeStep` (taken as 1/60).
- **The physics mover's collision response**: a vertical bounce with restitution 0.8 on z = 0 stands in.
- **Blending and the hit test**:
  - the shape of the hold blend (taken as linear);
  - the attack-sphere test against people (0x2D4390): `CollisionRadius` 2 about the person, 1 up.
- **Occupant spawning**: the timing and rates (+0x44 … +0x50), the view check (0x318390) and the cell check
  (0x31AD00).
- **The Actor bindings' bodies**: `startAnim`'s second argument, `onPickup` (taken as sending script event 8), and
  `getStringProp` versus `getTemplateStringProp` (only the template is read).
- **Scripts not ported**: `Mime`, `BandMember`, `RoyalGuard` and the naked streakers. They play their `DefaultAnim`,
  and if smashed they are deleted after 3 s.
- **Drawing**: `RenderScale` (`DefActorProps`: 0.65) is for the renderer to apply.
