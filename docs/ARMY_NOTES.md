# The army, its weapons, the droppables and the score

What `src/army.c` ports, and from where. [R] means read from the game's code or scripts, [G] means a guess or a
stand-in. The attackers are AttackObjects (vtable 0x44B470) and Actors (0x44B300) run by their templates' scripts
(decompiled in `build/scripts/*.scr`). The engine supplies the spawner, the movers, the weapons and the monster's
side of a hit.

## The spawner: gAIMgr (0x4D7800)
**Loading** [R] (0x334C40, called from the block load 0x302DF0):
- Only the current block's `AIDef` is used. The two blocks behind it get traffic only.
- Each `AIInstance|` child becomes a 0x24-byte record (constructor 0x335320, reader 0x3350F0):

| Offset | Field | Default |
|---|---|---|
| +4 | `OTRef\|Name` | |
| +8 | `MinDelay` | 10 |
| +C | `RespawnDelay` | 10 |
| +10 | `OffsetDelay` | 5 |
| +14 | `MaxOccurances`: the spawns left | 9999 |
| +18 | the timer, starting at MinDelay + OffsetDelay × rand | |
| +1C | alive | 0 |
| +20 | enabled | 1 |

- The native code never reads `FriendChance`, `Height` or `CompletionCallback`. The scripts read them from the
  object's instance book (obj +0x7C), which is this AIInstance group. `Vehicle_Helicopter` reads FriendChance and
  `Bomber` reads Height.

**The update** [R] (0x3333F0, called from the world update 0x2B6D6C). It runs only after `gAIMgr.activate(1)`
(0x2A6208), which `Levels.scr` BeginBlockNI calls once after the block's intro. For each record:
1. A record with something alive, or no spawns left, is skipped. Its timer is frozen.
2. The timer counts down even while the record is disabled.
3. When the record is enabled and the timer reaches 0:
   - the timer restarts at RespawnDelay + OffsetDelay × rand;
   - alive goes up by 1 and the spawns left go down by 1;
   - the template is made as an AttackObject (an Actor only with `IsActor`, which no book sets) and gets `onInit`.
     No position is passed, so the script places the object itself.
4. If the template is not found, alive stays at 1 and the record never spawns again.

What this means:
- MinDelay counts from the activation, not from the block load.
- At most one of each record is alive at a time.
- MaxOccurances counts all the spawns over the block.
- Difficulty, the number of players and the destruction do not enter. The difficulty is in the data instead
  (`HelicopterEasy` against `HelicopterHard`).

**The bindings** [R]:
- `removeAIObject(obj)` (0x332A50) takes 1 off alive for **every** record whose name matches the template's
  (case-insensitive). It does not reset the timer.
- `enableRespawn(name, on)` (0x3329B0) switches every record of that name on or off.
- `setSavedObjectName` / `getSavedObjectName` (0x2A6338 / 0x2A6420) hand the truck's name to the troops it makes.
  CombatTruck switches its own respawn off when it is made. Its troops (Swat.bsf `notifyGroup`) switch it back on
  once their group is gone.

Example (Chicago CG01_Block1): `Tank11sec` 100/5/100/100, `SWAT5` 30/2/45/100, `HelicopterMedium` 20/2/30/100 with
FriendChance 0.75, `ArmyTruck03` 60/2/50/100 and `NewsHelicopterCG` 5/5/120/2. The figures are
Min/Offset/Respawn/Max.

## The weapons
A template's weapon group (`Weapon|<name>` on an actor, or `WeaponSystem { WeaponCluster { ... Weapon| } }` on an
object) names `WeaponName`, a group of Weapons.bdp.
- **Read order** [R] (0x299EC8): the definition reads the book's group first and then the template's own group over
  it. So `Tank11sec` keeps TankWeapon but with `RefireDelay 11`, and the SWAT rifle does `ImpactDamage 2.5`.
- **The type** comes from the template's `WeaponType`, else the book's. `RProjectileWeapon` fires shots,
  `RBeamWeapon` fires beams, and `Shockwave` (the roar) is not fired here.

**The definition** (0x297DC0, then projectiles 0x296830 + 0x33B860, beams 0x2960D0 + 0x33B5F0). Defaults [R]
(0x296BC0, 0x296230, 0x33B3E0): `MaxFiringRange` 10, `DamageType` 11 (Unresistable), `StartAmmo` −1 (use Ammo),
`Acceleration` 1e8, `InitialVelocityPercent` 1, `Direction` Z, `ImpactVisRange` 1, `ReactionLevel` −1, everything
else 0.
- `Direction` is "X", "Y", "Z" or "-Z": which axis of the weapon node the shot leaves along.
- `DamageType` names are looked up in the table at 0x3F5E40: Concussive 0 … Heat 10, Unresistable 11. The books'
  `wd_Concussive` matches none of them.
- A weapon with `Ammo` ≠ 0 counts its shots (definition flag 2).
- Projectiles also read `Duration`, `ImpactDamage`, `ImpactDelay`, `Mobility` and `ImpactVisRange`. Beams also read
  `Duration`, `ImpactDamage`, `DamagePerSec` and `CollisionRadius`. The R classes add `HorizontalKnockback`,
  `VerticalKnockback` and `ReactionLevel`.
- `CollisionRadius` is not read for projectiles: a shot is a ray.

**The countdown** [R] (0x2978D0, 0x2971A0, 0x297760):
- **Start values:** ammo = StartAmmo, or Ammo when StartAmmo is −1. The countdown starts at
  ChargeTime + FiringOffset.
- **Each frame:** the countdown falls by dt while the weapon has ammo, or does not count its ammo.
- **While the weapon may not fire**, the countdown stops at ChargeTime + FiringOffset. So the time from permission to
  the shot is FiringOffset, then ChargeTime with the `ChargeEffect` showing.
- **At 0, with ammo:** one shot, or a beam for `Duration`. Ammo goes down by 1 and the countdown restarts at
  max(RefireDelay, ChargeTime). A beam restarts it when its Duration is over (0x295C20).

**Who may fire** [R] (0x29ADE0, 0x299010):
- **Objects:** the WeaponCluster turns towards the target's aim point, lifted for the shot's fall (0x293050:
  Velocity, Gravity). Rotation and elevation step at Rate × dt, each within ±Range in degrees (0x298E70 / 0x298F40).
  The weapon fires when the turret is on target, unless `RequiresRotation/ElevationClearToFire` is 0, and the target
  is within MaxFiringRange. `FireOnceCommited` latches the fire.
- **Actors:** they fire while `enableWeapon(1)` is on and `setWeaponTarget` is set.

[G] The rates are taken as radians per second. Actors and people aim straight at the monster's middle, or lob shots
that fall (the lower ballistic angle). The weapon node comes from the host's `node_world` callback when it knows it,
and from the unit's place otherwise.

**A shot** (RProjectile; init 0x2944F0, update 0x293120, step 0x294300) [R]:
- **Launch:** speed = Velocity × InitialVelocityPercent. Its look is the `FireEffect` (`Tank_Projectile`,
  `Punk_Bottle`, `Handgun_projectile`), which the viewer moves with it.
- **Steering:** towards its target the unit direction changes by at most `Mobility` × dt. That is barely at all at
  0.02 (bullets, shells), and outright homing at 17.5 (`SmartMissile*`).
- **Step:** speed = min(Velocity, speed + Acceleration × dt); the velocity loses 9.81 × Gravity × dt in z; the shot
  moves by velocity × dt.
- **Hit:** after `ImpactDelay` seconds, the step's segment is tested and the first thing met is hit. The
  `ImpactEffect` starts there and the shot is gone. It is also gone at `Duration`.
- [G] What the ray meets: the monster's collision spheres (or its box), a standing building's box (not one the shot
  starts in), and the street. The game asks its collision world (gp−0x6D60), which is not read.

**A beam** (0x295750) [R]:
- The beam runs from the weapon along its direction and stops at the first thing it meets.
- On the beam's first touch it deals `ImpactDamage`. After that it deals `DamagePerSec` × dt per frame, if that is
  above 0.
- [G] The beam's reach is its stream effect's, which is not read. It is taken as growing at Velocity per second up to
  MaxFiringRange.

**The impact on a monster** [R] (0x33BCA0 / 0x33B440): an object flagged 0x40 (a monster) gets a knock. The knock is
the shot's direction in x and y, made 1 long, times `HorizontalKnockback`, with `VerticalKnockback` up.
- With ReactionLevel ≥ 0 the hit goes to 0x2D9650 → 0x2D6A50.
- Otherwise it goes to the monster's TakeDamage.

This is `AE_HIT`, and `army_reaction_slot` is the monster side:
- **Ignored entirely** while the monster is not interruptible (+0xAA2), is in a grab, or has 0 HP.
- **Level 4 starts no reaction** (pistols, rifles, the pulse cannon). The damage still counts.
- **In the air:** mode 2 starts slot 42 (44 from behind) and mode 1 starts slot 43 (45).
- **On the ground or a building** (modes 0 and 5), by the knock made 1 long:

| Knock | Slot | From behind |
|---|---|---|
| z > 0.707 | 24 + level | 27 + level |
| z < −0.707 | 18 + level | 21 + level |
| otherwise | 0 + level | 3 + level |
| otherwise, level 2 with knock z ≤ 0.001 | 71 | 72 |

- **After the slot:** the column override applies; then HP −= damage (0x2D9364); at 0 HP, slot 67 (React_Defeated).
- **Examples:** the tank shell (30, 35 up; level 2) starts George's slot 26 `React_Ground_Knockdowns`, and a bomber's
  bomb (−15 down; level 1) slot 19 `React_Ground_Overhead`.

The script calls go straight to slots: `setReaction(n)` (0x2E1AD0) is slot n. 2 is a knock-down (the priest),
19 an overhead hit (a droppable), 54 the rampage. `addHitPoints` (0x2DA530) clamps HP to 0 … most and does nothing
at 0.

## The units

**Tanks** (`Vehicle_Attack.bsf`, pulse 1/15 s):
- **Arrival** [R]: from x = ±100, y = −7.5, along a LineMover at 12.
- **The AttackMover** [R] (init 0x314B40, update 0x313F80) takes over once the tank is on screen
  (`isSphereVisible`; [G] the view's x range, `army_set_view`):
  - **Target:** the monster ahead nearest along x. It is the weapons' target too.
  - **Distance:** by dx² alone. Beyond `ApproachRange` (20) the tank speeds up at 8/s to `Velocity` (8) after a 2 s
    wait. Inside `FleeRange` (15) it backs off. In between it brakes.
  - **Turning:** a monster behind makes it drive on 10 and then turn on the spot at π/2 per second.
  - **Blocking:** a step towards a monster within 10 (and 5 in y) is not taken.
  - **Off the street:** it does nothing while z ≥ 0.1 (the hop when hit).
- **Damage** [R]:
  - Each hit hops it: (∓5, 0, 20), two bounces, 50/s² down.
  - `HitPoints` (51) − amount per hit.
  - At 0: `DestShape`, a SphereMover away from the monster ×(12, 5, 1.5), weapons off, addStats type 2, then after
    1.5 s the `HitEffect` and after 1 s more, gone.
  - The hop sets the pulse to every frame and dying keeps it, so those 2.5 s pass four times faster, as the script
    does.
- **Lifespan** [R]: past `Lifespan` the tank turns round and drives off.

**Helicopters** (`Vehicle_Helicopter.bsf`) [R]:
- **Path:** a SplineMover through 8 points at y = −10, 12 … 18 high (or above the highest monster), x from ±100.
- **Spotting:** a monster within `TargetRange` (or `TargetXRange` along x) becomes the target.
- **Attacking:** closer than 0.6 of those ranges it slows down by `Acceleration` and attacks for `AttackDuration`,
  then leaves. Homing helicopters get their missiles' ammo (1 and 2) only then.
- **Wingman:** with `FriendChance` (from the AIInstance) a wingman is made and follows the leader's states
  (`stopFlight`).
- **Death:** `HitPoints` hits make it fall on a SphereMover. At z ≤ 4 it explodes (`HitEffect0`, `ExplodeEffect`).
- [G] The spline is taken as straight runs, facing the way the helicopter goes.

**Trucks and police cars** (`CombatTruck.bsf`) [R]:
- **Arrival:** they come from x = ±100 to 8 past the nearest monster (within ±40), at 20.
- **Troops:** they drop their `TroopType<n>` × `numTroop<n>Spawned`, one every 0.5 s, in a staggered line
  (`ExitTruck` from a truck), then drive off at 15.
- **The police car** skids to a stop and becomes pickable.
- **Pushing:** while moving, a monster on the ground within `MonsterResponseDist` is pushed along (`AE_PUSH`).
- **Death:** at 0 `HitPoints` it flies 15 away from the monster (addStats type 2) and explodes 2 s later.
- **Grabbing:** a pickable car can be grabbed and thrown (onPickup, onThrow: addStats type 2).

**Troops and cops** (`Swat.bsf`) [R]:
- **Firing:** each one runs to its place, then fires its weapon while the monster is within `ShootingRange` (20)
  along x and below 10. Cops get 1 to 3 bullets every `CopReloadDelay`.
- **Following:** `FollowMonster` troops stay `IdealShootingRange` from the monster. Grenadiers throw
  (`StandingGroundFiringAngleU`, then `GroundThrowRecover`, with a 3 s reload).
- **Fleeing:** grabbing or hitting one makes its whole `GroupIndex` fall back. The last one of a group re-enables
  its truck's respawn.
- **Eating:** an eaten troop gives addStats type 1 (its Health, Points 100) and a fart one time in five.
- **Smashing:** a smashed troop flies off (smashObject 0x2B2F50). The script's TakeDamage then calls onPickup, so a
  punched troop is never deleted. This quirk is kept.

**Window soldiers** (`Military_Window.bsf`) [R]:
- **Firing:** they emerge, look, and while the monster's y is within 6 of the building's front they play
  `WindowFiring<RM|RU|MU|LU|LM|LD|MD|RD>`. Each firing reloads the weapon, with a 2 … 4 s gap between reloads.
  After 5 s, a 50 % chance makes them go back in.
- **Hits:** they have no TakeDamage, so attacks pass them by.
- [G] `getMonsterDirection` is taken as the eighth of the way to the monster in the front's plane.

**StationaryAttackObject** (the pirate ship) [R]:
- **Aim:** it aims at a point that locks onto the monster after 0.55 s still (+`TargetOffsetZ`) and re-picks when
  the monster moves 3 away for 1.05 s.
- **Firing:** one round every `WeaponRefireDelay`.
- **Death:** 3 hits; `IsSinkable` ones sink.

**Bombers** (`Bomber.bsf`) [R]:
- **Path:** across at 20, at `Height` (from the AIInstance; 42 by default), at y = 5 (row 0) or 15 (row 1).
- **Bombing:** `Carpet` bombers drop every pulse (1 s). `Targeted` ones drop when within 4.8 along x.
- **Death:** one hit makes it fall (addStats type 2).
- [G] `reloadWeapon` is taken as refilling the ammo.

## Droppables
`Droppable.bsf` [R], `army_drop` for people.c's `PE_DROP`:
- **The path:** a SplineMover at 12 from the window, through a point 0.6 of the way across, 3 out and 1.5 up, to 5
  in front of the wall at z = 1. If a building stands there, the end is on its roof (storeys × 3 + 1.5 + 6).
- **The landing x:** the column edge at or right of the drop (getCellCenter), ± 2.5 towards the monster below.
- **The turn:** a quarter turn about y over the fall.
- **On the ground:** the `GroundEffect`, and it is gone.
- **On the monster:** when the monster takes it (within 2.5 along x) it plays `HitEffect` and `HitMonsterSound`,
  calls `setReaction(19)` (an overhead hit), and is gone. No damage.
- [G] The game calls this TakeDamage from its collision, which was not read. Here it fires when the object touches
  the monster's spheres, or when an attack reaches it.

## Scoring
**The records** [R] (`Player.addPoints` 0x33F530): each player's record holds `points[7]` and `kills[7]`. The
categories are 0 destruction, 1 civilian, 2 vehicle, 3 bonus, 4 time bonus, 5 challenge, and 6 the total. Every
award also adds to 6. There are no multipliers.

**gCommon.addStats(obj, slot, type)** [R] (0x2C51A0):
- It pays once per object (flag 0x40000000).
- The monster's HP becomes max(1, HP + `Health`), up to its most, only while HP > 0.
- Its energy becomes energy + `Energy`, within 0 … 100. At 100 it starts the rage.
- Category `type` gets the object's `Points`.
- **Types:** 0 a stationary attack object or sign, 1 an eaten person, 2 a destroyed vehicle, 3 an edible.
- **Points:** soldiers, SWAT and cops 100; SWAT van and army truck 500; tanks 1000; helicopters 300; bombers 1500.

**Buildings** [R] (`Scoring` of DefBuildingProps, 0x2BA4B0): the points go to the monster that hit the building
(+0x190), into category 0. The book is the same in every city:

```
Building { Base 100, None 0, Light -25, Medium -50, Heavy -50 }
cell     { None 10, Light 10, Medium 25, Heavy 50 }
floor    { Base 50, Collapse 1000 }
MultiFloor { Base 500, ..., Collapse 2500 }
Floating = 100
```

- **A cell hit:** cell[its state after the hit].
- **The first falling floor:** floor.Base × the front's width. It also starts a chain step of MultiFloor's entry
  for the monster's state, which in practice is Base 500. The hit that dropped the floor pays this instead of its
  cell.
- **Each next floor of the chain:** the chain sum so far, which then grows by the step. The second floor says
  `v_combo`.
- **The building:** Base × width × storeys standing, plus the Building entry for each front cell's state. If a floor
  fell just before, it adds MultiFloor.Collapse (or floor.Collapse) and `v_super`.
- **Display:** an award of `Floating` (100) or more shows a floating number.

`army_score_building` keeps the chain per building.

## Not traced (stand-ins in army.c)
- **Random numbers:** the sequence (an LCG stands in for Park-Miller 0x106BD0) and the frame step (1/60).
- **The collision world:**
  - what a shot's or beam's ray meets;
  - the beam's reach;
  - when a droppable meets the monster;
  - the units' shapes for the monster's attacks (actors: a sphere of `CollisionRadius` 2; objects: the host's model
    box, else 4 × 6 × 3).
- **Movers:** the SplineMover's curve, the SphereMover's collision response (a bounce on z = 0 with `Restitution`,
  coming to rest), and the HeldObjectMover's axis turn (the unit takes the hand's matrix).
- **The weapon system:** the turret's rate units, the aim of actors and people, `reloadWeapon`, and the weapon node
  without the host's `node_world`. Both the projectile's wobble (definition flag 8) and its spread (record flag 8)
  come from flags no book sets.
- **Other:** `isSphereVisible` and the camera's place (`army_set_view`), the AttackObject hit wobble (+0x18C), the
  impact's visibility test (ImpactVisRange), and the ImpactEffect's orientation (the surface's normal).
- **Not ported:** bosses, `Vehicle.bsf` (the `Humvee` and other street vehicles, plain `AK_OTHER` here), the news
  helicopters' cameras, Rocket / RocketBase. The `BradleyTroopCarrier` is a CombatTruck and is covered.

## Driving it from the viewer
1. **Setup:** `army_new` with a host whose `find(book, name)` returns `book_group` of `<book>.bdp`. Use the same
   resolver as `prb_set_resolver`. Then `army_load_block(BlockDef group)`, and `army_activate(1)` when play starts.
2. **Each frame:** call `army_update(dt, monsters, buildings)`, where the monsters carry their collision spheres in
   the world. Then:
   - draw each live unit's `shape` at `world`; actors play `clip` at `anim_time` (one play is `anim_len`);
   - draw each live shot by its `effect` at `pos` along `dir` (a beam spans `length`);
   - apply the events: start and stop effects by name (attached ones follow `unit` / `shot`), play sounds, route
     `AE_HIT` through `army_reaction_slot` to the monster's slots and HP, `AE_REACTION` straight to the slot,
     `AE_STATS_*` to HP and energy, and `AE_PUSH` to the monster's place.
3. **Monster attacks:** an armed attack sphere calls `army_hit`. The grab uses `army_nearest_pickup`, `army_grab`,
   `army_hold` each frame and `army_throw`.
4. **People:** `PE_DROP` goes to `army_drop` with the block's `DroppableDef` template, and `PE_WEAPON` goes to
   `army_person_weapon`, with `army_person_moved` while the person is on.
5. **Buildings:** building damage calls `army_score_building`.
