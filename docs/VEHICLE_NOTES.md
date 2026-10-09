# The street traffic, the fire truck and the rockets

What `src/vehicle.c` ports, and from where. [R] means read from the game's code or scripts, [G] means a guess or a
stand-in. The tanks (`Vehicle_Attack.bsf`) and the helicopters (`Vehicle_Helicopter.bsf`, army and news ones) are
army.c's: see [ARMY_NOTES.md](ARMY_NOTES.md). Everything here is a script object (class 0x4D5BD0) run by its
template's script.

| Script | Templates |
|---|---|
| `Vehicle.bsf` | every traffic car (`Sedan2D`, `Sedan4D`, `Taxi1`, `Citybus`, `Limo`, `Humvee`, `PoliceCar`, `FireTruckVehicle`, `Bicycle`, ...), the `Boulder` a monster tears out of the street (`Monsters::grabBoulder`), and `HiddenVehicle` |
| `Vehicle_FireTruck.bsf` | `FireTruck`: an AttackObject with a water beam. No block of the shipped cities spawns it |
| `Rocket.bsf`, `RocketBase.bsf` | `Rocket` (a Hong Kong roof object, a challenge goal), its `RocketBase` and `RocketPipe` |

## The traffic: gAIMgr (0x4D7800)
**Loading** [R] (0x334240, per loaded block slot: 0 the current block, 1 and 2 the two behind it): the block's
`TrafficDef` becomes a 0x28-byte record.

| Offset | Field | Default |
|---|---|---|
| +0 | `MaxVehicles` − 2 × slot: only the first fill uses it | 7 |
| +4 | `MinReleaseTime`: read, never used | 1 |
| +C, +10 | the rear and the front lane's timers | 2 + 3 × rand |
| +14 | the types: `VehicleDef\|` { `Name` (OTRef), alive, `MaxCount` (10000), `Frequency` (1) }, 0x10 bytes each | |
| +24 | 1 `NoFrontTraffic`, 2 `NoRearTraffic` | |

**The release** [R] (0x332D90, share s):
- **The type:** weights are Frequency × (min(MaxCount, 5) − alive). A slot never has more than 5 of a type alive,
  and that is the only limit after the first fill. A type is picked by a random number (0x106950, gWorld +0x82C)
  times the sum, among the weights above 0.001. Nothing is released when the sum is 0.
- **The lane:** s ≥ 1 the rear lane, s < −1 either at random, otherwise the front lane. A lane switched off releases
  nothing.
  - rear: from x = 75 to −75 at y = 41 × slot − 7.5, turned +π/2 (facing −x);
  - front: from −75 to 75 at y = 41 × slot − 12.5, turned −π/2.
- **The mover:** a LineMover at 15 (0x317650). A share strictly between −1 and 1 is where on the line it starts.
- **Counting:** the type's alive goes up by 1, and so does slot 0's count (gp − 0x6C78).

**The update** [R] (0x3333F0; it runs before `gAIMgr.activate`):
- **The first time:** each slot fills its front lane. The share starts at 0 and grows by max(0.1, rand × 2 / n),
  with n = MaxVehicles − 2 × slot; a vehicle starts at each share while the share is below 1 and fewer than n were
  made. The last one can get a share of 1 or more, and so lands at the rear lane's start. Nothing else happens that
  frame.
- **After that,** for each slot in turn both timers fall by dt:
  - the front timer at 0 starts again at 2 + 3 × rand and releases on the front lane (share −1);
  - else the rear timer at 0 does the same on the rear lane (share 1);
  - one release, or a timer that ran out on a switched-off lane, ends the whole AI update for that frame. The later
    slots' timers and the AIInstance spawner wait a frame.
- `MaxVehicles` 0 (9 blocks) only skips the first fill: the timed traffic still comes.

**removeVehicle** [R] (0x332BA0):
- **The slot** comes from where the vehicle is now (y > 20, y > 60), not where it was released.
- **The list:** the vehicle leaves the manager's list if it is in it.
- **The count:** every type of its name in that slot gets one fewer alive, even when the vehicle was not in the
  list. Slot 0's count goes down with them.

## The LineMover
[R] (init 0x3173D0 / 0x317650, update 0x315E10; people.c has the same):
- **Fields:** +0x54 the speed, +0x58 the share of the way done, +0x5C `MonsterResponseDist`² of the template (or −1).
- **The callbacks:**
  - 2 at the end;
  - 4 every frame a monster is nearer than that (3D distance) while it simply moves.

[G] Only these reach the script: `Vehicle_FireTruck.bsf` deletes its truck on any callback, so "moving" cannot be
one of them. The speed is constant: a car never brakes for a monster.

## Vehicle.bsf
`this.flags` [R]:

| Flag | Meaning |
|---|---|
| 8 | wrecked: it explodes on its next pulse |
| 16 | picked up |
| 32 | thrown or knocked away |
| 64 | touched the street since the last pulse |
| 128 | spent a whole pulse in the air |
| 1024 | hidden |
| 4056 | exactly: an edible that was smashed |

**Construct** [R]:
- **Pulse:** every 0.5 + rand seconds.
- **Object flags:**
  - OnlyHitWithEffNodes (0x8000: only attack spheres hit it) and Pickupable (0x10000);
  - plus 0x80000 for an `Edible` (the bicycle);
  - plus 0x8000000 | 0x80000 for an `Animates` one (no book sets that).
- **On it:**
  - the `AmbientEffect` (the police lights; it stops when the object dies);
  - the `EntranceSound` (the bicycle's bell).
- **IsHidden** (the HiddenVehicle): flag 1024, not pickable, pulse every 1 s.

**The mover's callback** [R]:
- **At the end (2):** a hidden one turns round (a new LineMover from where it is to −x, at 8). Any other leaves the
  manager and is deleted.
- **A monster near (4):** with its horn count at 0 it plays its `HornSound`. The count then waits 3 pulses, 20 for a
  hidden one, so the loudspeaker speaks at most once in 20 s.

**TakeDamage** (the monster's blow) [R]. Nothing happens if the vehicle is wrecked, hidden or thrown. Otherwise:
- **The look:** flag 8, neither hittable nor pickable, its `DamageShape`; the ambient effect stops; `DamageSound` and
  `DamageEffect` (both effect names) start on it.
- **The flight:** a SphereMover at 25 along the way from the monster's place, z made positive. From the street that
  is mostly sideways: the car skids off.
- **onThrow(monster, not eaten):** a car counts (addStats type 2) and is "thrown" (32). Flag 8 is already set, so it
  explodes on its next pulse, half a second later: its `ExplodeEffect` (`Car_explode`) where it is, then it is gone.
- **A bicycle** is smashed instead (below).

**onThrow(monster, eaten)** [R]:
- **An Edible** (the bicycle) leaves the manager:
  - eaten, it counts (addStats type 1: Health 20, Points 25), with a fart one time in five 2 s later;
  - not eaten, it is smashed away (smashObject 0x2B2F50: the street-object cone at 15) with its `FlyingSound`. Its
    flags become 4056 and it pulses every 0.25 s. It is deleted when its 3 s run below 0, after 3.25 s.
- **Anything else** counts as a destroyed vehicle (addStats type 2, `Points` 25, the police car 100, the Boulder 10
  from DefObjectProps). It is no longer pickable, gets flag 32, and has 5 s: it pulses every 0.5 s.

**onPulse** [R]:
- 4056: −0.25 from its 3 s; deleted below 0.
- 8: explode (as above), leave the manager, deleted.
- 32: −0.5 from its 5 s; at 0 it leaves the manager and is deleted, without an explosion. Otherwise, if 64 is set,
  64 and 128 are cleared; if not, 128 is set.
- The horn count falls by 1.

**OnHitGround** [R]:
- Each touch adds 5 to the bounce count, or 1 if 64 is already set. Over 40, flag 8: it explodes on the next pulse.
- Otherwise flag 64 is set. A touch with 128 set (and not 8) plays the `DamageEffect` again and clears 128.

**onPickup** [R]: the `DamageShape`, `PickupSound` and `DamageEffect` on it, flag 16.
- An `IsShaker` (buses, the tram, the garbage truck) also plays `PourEffect`, `ScreamSound` (Crowd_Panic) and
  `ShakerSound`.
- The books hold the pour (`People_pour`, `Garbage_pour`) as an unnamed `Effect|` list element, which the lookup by
  name does not find. So no pour plays, and none here.

**TakeImpact** [R]: flag 8. It explodes on its next pulse.

**What this means:**
- A punched car skids and explodes half a second later.
- A thrown car flies. Landing after a whole pulse in the air plays `Car_Hit`, and it explodes after about 0.6 s of
  sliding or bouncing on the street.
- A car still on the street when 5 s are over just goes.

**The hold** [R] (0x2DA310): BeingHeld, neither Pickupable nor PushOutMonsters. The HeldObjectMover (init 0x310ED0)
replaces the LineMover, so the end never comes: a held car stays counted in its slot. `HoldOffset` (or
`MirroredHoldOffset` while the monster is mirrored, +0x7DA1) is taken along the hand's axes.

## Vehicle_FireTruck.bsf
[R] Not spawned by any shipped block; ported from the script.
- **onInit:** it pulses every frame; it picks a random building; `TargetDistance` 5, `FireDuration` 5, `MaxVelocity`
  8 (`EntryDelay` is read but unused).
- **The drive:** from x = ±75 (a coin) at y = −15 to the other side, on a LineMover at 8, facing the way it goes.
- **Stopping:** within TargetDistance of its building along x it stops (setVelocity 0) and targets a DummyObject. The
  target is 10 in front of the building (+y), at the roof's height (storeys × 3 + 1.5 + 6; above 15 a random height
  between 15 and that).
- **The stream:** while stopped, FireDuration falls by the frame. At 0 the target is let go, the truck drives on at
  MaxVelocity, and `setWeaponFlags(-1, 16)` holds its weapons. It is deleted at the end of its line.
- **The weapon:** `FireEngineStream` (RBeamWeapon on `WeaponNode_Y`; Ammo 1, Duration 4, ImpactDamage 5,
  DamagePerSec 10, Velocity 275, ReactionLevel 1, FireEffect `Blimp_laser_fire`), on a WeaponCluster (rotation 360°
  at 2.5, elevation −10 … 90° at 2.5). With 1 ammo a truck makes one 4 s stream. A monster in its way takes 5, then
  10 a second, at reaction level 1, as army.c's beams do.
- **TakeDamage:** it counts (addStats type 2: Points 250) and drives on. It cannot be wrecked.

## Rocket.bsf and RocketBase.bsf
**Construct** [R]: OnlyHitWithEffNodes and PushOutMonsters; it pulses every frame. Its `Base` (RocketBase) and
`BasePipe` (RocketPipe) are made where it stands; only their point is set, so they face 0.

**TakeDamage** [R]: a monster's blow on a waiting rocket (state 0) starts it, and it counts (addStats type 0, Points
100). In state 2:
- `ChargeEffect` (`Skyrocket_Buildup`) where it stands;
- a 2 s shake, every frame. The step grows by 50 dt and the kept point moves by 0.7 × (sin step, 0, sin step). The
  rocket is put at that point − (0.5, 0.5, 0.5), so it also sits half a unit off while it shakes.

**flyAway** [R], after the shake, or at once when its building falls (onBuildingDestruction, state 0 only, no score):
- **The flight:** state 1, a LineMover 50 up at 25, `TrailingSmokeEffect` (`Skyrocket_Launch`, a composite with
  `skyrocket_explode`) on it. It is deleted at the top.
- **The parts:** the Base and the Pipe `fallOff`.
- **The monsters:** each player's monster within 10 of the start along x and along z is thrown with setVel. The
  velocity is (−10 if the monster is on the −x side, else 0; −5; 30). It gets reaction 2 (a knock-down) and
  addHitPoints(−`Damage`), which is −20.

**fallOff** (RocketBase.bsf) [R], for 2 s, every frame:
- **The move:** down 6 a second; along x 10 / n × `FallDir` (−1 for the base, 1 for the pipe); along y −20 / n.
- **The turn:** by (−π/2, FallDir × π/2, ±π/3) / n, the sign of z at random. n is the number of frames a fall to the
  street at 6 would take (height / 6 / the frame).
- **The end:** deleted after the 2 s. The time does not depend on the height, so a part from a roof lower than 12
  ends below the street.

## Scoring
Through `gCommon.addStats` [R] (0x2C51A0; see ARMY_NOTES "Scoring"): once per object. When an Army is bound,
`army_add_stats` applies it, and its events (AE_STATS_HEALTH / AE_STATS_ENERGY / AE_SCORE) carry it. VE_STATS
reports every award with its template name, so the viewer can count challenge goals such as Hong Kong's `Rocket`
(Required 2). How the game counts a goal was not traced.

| What | Type | Points |
|---|---|---|
| a car wrecked or thrown | 2 (vehicle) | 25, police cars 100 |
| a bicycle eaten | 1 (civilian) | 25, Health +20 |
| the fire truck hit | 2 | 250 |
| a rocket started | 0 (destruction) | 100 |

## Not traced (stand-ins in vehicle.c)
- **Random numbers and time:** an LCG for both of the game's generators. The scripts run at a fixed 1/60 frame
  (gWorld.getTimeStep).
- **Pulses:** `setPulseDelay` is taken to restart the count from the call.
- **The SphereMover:**
  - `Gravity` (35 from DefObjectProps) down; a bounce on z = 0 with `Restitution`; a slide slowed by
    `SlidingFriction` × Gravity; at rest when still;
  - OnHitGround every frame it touches the street while moving;
  - a standing building's box stops it and counts as TakeImpact.
  The game's collision world (gp − 0x6D60) is not read, nor whether the mover sends a callback when it comes to rest.
- **TakeImpact's callers** (the virtual +0x38 on WSimpleObject 0x28FA30) were not found. The viewer can call
  `vehicle_impact` when its own collision sees something hit a vehicle.
- **A blow's shape:** the model's box (AutoCollisionBox) from the host, else the `ShadowRect` 2.5 high.
- **The hold:** the HeldObjectMover's `MajorAxis` frame is not ported; the object takes the hand's turn. A thrown
  object keeps the hand's turn and does not spin.
- **The fire truck:**
  - getRandomBuilding: any standing building;
  - Building.getPosition: the middle of its footprint;
  - the turret's units (radians a second);
  - firing when within 0.02 rad of the target;
  - the stream stops at its target; `setWeaponFlags(-1, 16)` taken as all weapons off;
  - the weapon node: the host's `node_world`, else 3 above the truck.
- **WorldObject.rotate:** turns about the object's own x, then y, then z.
- **The first fill** is done again after `vehicle_clear`. Whether a later block load resets it was not traced.
- **Not ported:** the news helicopters' cameras (army), and what the 0x80000 / 0x8000000 object flags do in the engine.

## Driving it from the viewer
1. **Setup:**
   - `vehicle_new` with a host whose `find(book, name)` is the army's (city_book_group) and `shape_box` the city's;
   - `vehicle_bind_army(v, army)`;
   - per block slot, `vehicle_load_traffic(v, slot, BlockDef group)`: slot 0 the current block, 1 and 2 the ones at
     y + 41 and y + 82;
   - `vehicle_level_start` once (the HiddenVehicle);
   - a roof `ObjectInstance` whose template is ours (`vehicle_kind_of` ≥ 0: the rockets) goes to `vehicle_spawn` with
     its building's index instead of being placed as a still model.
2. **Each frame:** `vehicle_update(dt, monsters, boxes)` with the same ArmyMonster and ArmyBox arrays as
   army_update. Then:
   - draw each object that is not VP_GONE and not `hidden`: its `shape` at `world`;
   - apply the events (see vehicle.h).
3. **Monster attacks:**
   - every armed attack sphere also calls `vehicle_hit`;
   - the grab tries `vehicle_nearest_pickup` and then `vehicle_grab`; `vehicle_hold` each frame with the hand's
     matrix; `throwObject` calls `vehicle_throw`;
   - `Monsters::grabBoulder`: `vehicle_spawn("Boulder", monster + 6.5 up)` and then the grab.
4. **Buildings:** a building that falls calls `vehicle_building_gone`.
