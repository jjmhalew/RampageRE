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
  `RBeamWeapon` fires beams, and `Shockwave` is the monsters' roar ("The monster's weapons" below).

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
- **Actors:** they fire while `enableWeapon(1)` is on and `setWeaponTarget` is set. The actor's weapon is updated
  every frame, enabled or not (0x30EC10 ends in the weapon's vtable +0x6C with the enable byte +0x0), so a disabled
  one keeps counting down to its floor and is ready again when re-enabled; it goes with its actor. The punks
  (WalkingAroundPerson.bsf) enable theirs at the end of `Throw` and disable it 0.25 s later (`disableWeaponDelay`
  on the pulse): StreetPunkBottle's countdown starts at 0 (ChargeTime + FiringOffset), so one bottle a throw, a throw
  every `ReloadDelay` (5) at most [R]. Here army.c `army_person_weapon` / `persons_update` (a weapon per person,
  kept until `army_person_forget`; the bikers' too, parade.c's). The weapon is made as any actor's [R 0x30F1F0]: the
  book's group (0x297AA0 at 0x30F594), then the template's own `Weapon|RifleWeapon` over it (0x30F5A8): the Hard
  punks' `ImpactDamage` 8 (the book's 4), the Easy ones' `Velocity` 12, every punk's `ImpactDelay` 0.2, the biker's
  `ImpactDamage` 2. Its shot passes its own actor (a shot's owner is no hit): the biker's bottle left from inside the
  biker's box and burst there before.
- **The actor's weapon** (0x30EC10 each frame; its keys read from the template's own Weapon group by 0x30F1F0:
  `AnimPosMin` +0x18 (0), `AnimPosMax` +0x1C (1), `FromWindow` +0x24, `LimitAngle` +0x28 in degrees, `Direction`)
  [R]: it tries to fire (vtable +0x68, with the target) only while it is enabled, has ammo (+0xEC > 0) and its
  current clip's position (the record's time / its length, 0x30C020; 0 for a length under 0.001) is within
  AnimPosMin .. AnimPosMax; otherwise it holds (+0x7C, 0x297090: the countdown at least ChargeTime + FiringOffset,
  the charge effect stopped). Only `SoldierGrenadierFromWindow` (0.5 .. 0.75 of its 1 s throw) and the `Biker`
  (0.4 .. 1) narrow it: the biker's bottle leaves 0.28 s into its 0.7 s `Throw`. Here `unit_weapons_tick` for
  Swat.bsf and Military_Window.bsf units, `persons_update` for the people and the bikers (their clip's time and
  length handed in by `army_person_moved`).
- **The actor's aim** (0x30ED9C .. 0x30F0E8) [R]: from the node's world (else the actor's) at p, the target point t;
  with gravity (the definition's +0x1C) t is lifted by 0x293050 over |t − p|, that difference's y zeroed first for
  `FromWindow`. The way is t − p, then with `LimitAngle` > 0 0x30E930 keeps it within that angle of the node's aimed
  axis (the row `Direction` picks): farther, the axis is turned by the limit towards the way, about their cross
  product (0x105340 + 0x104BB0). From a window the frame's place is 1.1 lower in y (out from the front, towards the
  street) and the way's y is 0: a window soldier fires along the front, so it reaches a monster on the facade or
  just before it, and passes one out on the street. `FromWindow` 1: SoldierRifleman0 (LimitAngle 10),
  SoldierGrenadierFromWindow, SWATWindow0 and the window police (PoliceUSWindow1, PoliceUKWindow: LimitAngle 45).
  Here `weapon_aim` (`ArmyWeapon.from_window`, `limit_angle`).

The cluster (0x299640) [R]: RotationRate and ElevationRate are radians a second as read (the step 0x29BA50 is rate ×
the frame's dt; the angles wrap at 0x298E70 / 0x298F40). The defaults: ElevationRate 0.5, ElevationRange ±40°,
RotationRange ±360°; a negative or missing Range keeps the default, and `Elevation/RotationMin/MaxRange` (degrees)
set each end on its own (the HT turret's −10 … 90). It is on target when the turn is within π/50 and the pitch
within π/100, or when `RequiresRotation/ElevationClearToFire` is 0; a step that close lands on the target.
`ReCenterWhenNoTarget` (flag 0x10, default off) makes the cluster step back towards 0, 0 at the same rates while it
has no target (0x29ADE0: the wanted angles are 0); without it the turn stays where it was.

**The turn is drawn** [R]: the cluster keeps the turn at +0x54 and the pitch at +0x3C, and the nodes named by
`RotationControlNode` and `ElevationControlNode` at +0x64 and +0x4C (−1 when absent; looked up through the object's
vtable +0x48, 0x299D90 / 0x299DF0). An embedded `WWeaponProcAnimator` (vtable 0x4499F0, at cluster +0x2C, set up
by the cluster's constructor 0x29A200) is the shape's procedural animator while the weapon system runs: 0x299010
hands it to the object (vtable +0x28) in its states 2, 3, 4 and 7 and takes it back otherwise. Its +0x8 (0x298B40)
names the nodes it touches (a mask of the two), its +0xC (0x298980) poses one of them: the clip's local pose (the
anim's vtable +0x14, made a matrix by 0x104BB0) with a turn put first (row vectors, 0x101A90), about the node's own
z by +0x54 for the rotation node (0x1015B0: rows (c, s, 0), (−s, c, 0), (0, 0, 1)) or about its own x by +0x3C for
the elevation node (0x101730: rows (1, 0, 0), (0, c, s), (0, −s, c)). So what hangs below the nodes turns with them,
and the WeaponNode's world, the firing frame, carries the turn. Here city.c `army_sync` / `pose_instance`
(`Instance.turn`), from `ArmyWeapon.rot_node` / `elev_node` and its `yaw` / `pitch`: the tanks' turrets, the
helicopters' bodies and the PirateShip's cannon (`Cannon_Rotation`, `Cannon_Elevation`).

The turn's targets (0x29ADE0) [R]:
- **The turn** (0x29B1AC): the angle of the aim's x, y less that of the object's +y row, wrapped to ±π.
- **The pitch** (0x29B7F8): atan2 of the rise (the lifted target's z less the weapon's) over the level distance
  (0x29AFF4: √(dx² + dy²)), so the aim's own elevation, plus the object's tilt (0x29B3B8 .. 0x29B7D8): x = the
  angle of its x row above the level (0x105C60, acos of the row against its level part made 1 long, signed by the
  row's z), y = minus that of its y row; y negated while the turn (+0x54, before this frame's step) is beyond ±π/2,
  x while it is below 0; the correction is the larger plus half the smaller. 0 for an upright frame; the rocking
  PirateShip's is up to about 0.1 rad. The atan2s are the VU0 routine at 0xA00 (vcallms; the quadrant fixes around
  it: x < 0 → π − r, y < 0 → −r, both 0 → π/2).
- Here `weapon_aim` (`cluster_tilt`).

The aim [R 0x293050]: actors (0x30EC10) and clusters aim straight at the target lifted by ½ × 9.81 × Gravity ×
(L / V)² (L the straight distance, V the Velocity; no lift without gravity). A monster's target point (its vtable
+0x1C, 0x2DB030) is 6 above its feet, at its `Bip01 Spine1`'s x and y (0x2E6000; "The weapon system" below) [R].
The weapon node comes from the host's `node_world` callback when it knows it, and from the unit's place
otherwise.

**A shot** (RProjectile; init 0x2944F0, update 0x293120, step 0x294300) [R]:
- **Launch:** speed = Velocity × InitialVelocityPercent. Its look is the `FireEffect` (`Tank_Projectile`,
  `Punk_Bottle`, `Handgun_projectile`), which the viewer moves with it.
- **Steering:** towards its target the unit direction changes by at most `Mobility` × dt. That is barely at all at
  0.02 (bullets, shells), and outright homing at 17.5 (`SmartMissile*`).
- **Step:** speed = min(Velocity, speed + Acceleration × dt); the velocity loses 9.81 × Gravity × dt in z; the shot
  moves by velocity × dt.
- **Hit:** after `ImpactDelay` seconds, the step's segment is tested and the first thing met is hit. The
  `ImpactEffect` starts there and the shot is gone. It is also gone at `Duration`.
- **What the ray meets** [R] (0x293CB4, the world's ray 0x283730, collide.c `collide_ray`): the street first, then
  every object of the frame's collision world with a flag of the shot's include mask (+0x134) and none of its
  exclude mask (+0x138) | 8, the nearest. The masks come from the record (0x2953E0: include 4, exclude 0), changed by
  0x33B770: `DamageType` 8 meets only buildings (0x2000), Blunt (4, counted as 0) buildings and monsters (0x2040).
  A hit on the shot's own unit (+0xC4) is no hit at all (0x293CD8). The monsters are their collision spheres, flagged
  0x40; a ray that starts inside a box meets it at t = 0 (0x280780 → 0x100420: the face of least depth, ties to z
  then y, on the start's side), inside a sphere too (0x100900: away from its centre), so a shot whose step starts
  inside its own unit is blind for that step [R]. What the shot hits that is not a monster only stops it: its
  impact on such an object (0x292D20) is not ported.

**A beam** (0x295750) [R]:
- The beam runs from the weapon along its direction and stops at the first thing it meets: the world's ray with
  include −1 and exclude 8 (+ the fire's own, 0 here), its owner flagged 8 around the cast (0x2957E0).
- On the beam's first touch it deals `ImpactDamage`. After that it deals `DamagePerSec` × dt per frame, if that is
  above 0.
- The beam's reach is its stream effect's (0x295750: the ray's maxt is 0x226950's head − tail, then the effect is cut
  at the hit, 0x226DF0) [R]. The stream (0x226CC0) grows its head by its speed (+0x1C) × dt up to its length
  (+0x20); its tail stays at 0 until its cycle runs out or it is shut down, then follows at the same speed [R]. Those
  are the FireEffect's collision rod [R], derived at load by 0x1F9290 only when emitter 0 has AutoStart and
  +ProvidesCollisionRod: speed |em_vel(0)| (+0x50), length speed × p_lifetime.max(0) × destretch_percent_time when
  above 0 (+0x54), radius |start_area(0)| + max(p_size) × size_mul (+0x4C), and the stream handle's cycle the
  emitter's longest range.cycle_length (0x2270A0); the cast runs along the stream's own axis through the effect's
  matrix, from the start offset plus the tail. The stream is state [R 0x226CC0, 0x226DF0]: each frame the clock
  runs on; the tail is 0 while the clock is within the cycle, then grows by speed × dt; the head grows by speed × dt
  up to the length; a tail at the head sets both to 0, which then holds for the rest of the burst (Machinegun_fire,
  cycle 0.5 s, stops hitting about 0.8 s in; Blimp_Laser_Fire, 2 s, at about 2.25 s); a hit at t cuts the head to
  tail + t, from where it grows again. The loop byte +0x42, which would wrap the clock, is never written (no store
  to it in the whole ELF; the emitter's CycleLoops does not reach it): no beam loops. The impact is placed at the
  weapon's place + t (0x2958CC), the tail not added. The start offset is em_pt_offset (none of the five rod effects
  sets it); the direction is rod.dir through the weapon's firing matrix (+0xBC × the WeaponNode's world, 0x2971A0):
  +X for Machinegun_fire / Autocannon_fire / Blimp_Laser_Fire / Cyclone_Lightning, +Y for Roar_Beam. Stopping
  (0x295650) releases the handle at once: the reach ends the frame the firing stops. Machinegun_fire and Autocannon_fire (Rifle, PulseCannon): 150 a
  second, 44.55 long; Blimp_Laser_Fire (BlimpBeam, FireEngineStream) 500, 125; Cyclone_Lightning (LightingZap 0 .. 2)
  2000, 2000; Roar_Beam (BreathWeapon) 25, 8.75. MaxFiringRange only gates the firing. Here as so (fx.c `fx_rod`,
  the host's `rod`, the stream in `ArmyShot.head` / `tail`).
- **The firing frame** [R]: +0xBC is the matrix the weapon is made with (0x2978D0), the identity at gp−0x70F0 from
  the cluster (0x29A020), so an object's firing frame is its WeaponNode's world (0x2971A0, the node from the owner's
  vtable +0x4C). An actor's weapon has no node of its own: the actor's aim (0x30EC10, each frame) takes the node's
  world (else the actor's), points its aimed axis at the target (lifted for a fall, 0x293050) with 0x100F50 and
  writes that to +0xBC. The aimed axis is the Weapon group's `Direction`, its first letter − 'X' (0x30F1F0, default
  X). 0x100F50 sets row k to the aim, row k + 1 to the world's x (its y when the aim's x is above 0.7), row k + 2 =
  row k × row k + 1 made 1 long, then row k + 1 = row k + 2 × row k. The stream's effect is placed in the firing
  frame every frame while it fires (0x2975A4: vtable +0x8C the place, +0x90 the matrix). Rifle and PulseCannon say
  `Direction X`, like their effects' rods, so a soldier's beam runs straight at its target; the helicopters'
  Weapon_0 has +X along the unit's front. Here (`army_frame_point`, `army_beam_axis`): the beam's axis is the rod
  through the firing frame and its effect is placed in that frame (fx_spawn_frame, fx_place), for the army, the
  bosses and the fire truck. An army unit whose shape has its cluster's control nodes is drawn turned ("The turn is
  drawn" above), so its firing frame is the WeaponNode as drawn, a frame behind (the shape is posed after the
  update; the node's world the object hands out, vtable +0x4C, is its last pose), and a shot leaves along that
  frame's `Direction` axis (0x2963C0: row +0x3C of +0xBC × the node's world, negated by +0x40) [R]. Measured: the
  tanks' drawn Weapon_0 +Y and the turn's way agree to 0°, the rocking PirateShip's Weapon_Node_1 +Y within 4°.
  The bosses' and the fire truck's turrets are drawn the same way (boss.c, vehicle.c through `army_cluster_read` /
  `army_cluster_turn`, which carry the turn from the +y row and the tilt-corrected pitch; city.c `instance_turns`).
  For a shape drawn without the control nodes the turn is put on the node's frame about the unit's right and the
  world's z (`army_frame_turn`) and the shot leaves along the turn's way [G: a stand-in for the nodes the game turns;
  every army, boss and fire truck model with a cluster has them]. A boss weapon without a node gets a frame with the
  rod's main axis along its aim.

**The impact on a monster** [R] (0x33BCA0 / 0x33B440): an object flagged 0x40 (a monster) gets a knock. The knock is
the shot's direction in x and y, made 1 long, times `HorizontalKnockback`, with `VerticalKnockback` up.
- With ReactionLevel ≥ 0 the hit goes to 0x2D9650 → 0x2D6A50.
- Otherwise it goes to the monster's TakeDamage (vtable +0x34, 0x2D9680), which picks the level by the amount
  against MONSTER_DATA +0x12C / +0x130 / +0x134 (3, 15, 30 for all 30 monsters): below 3 level 4 (no reaction),
  below 15 level 0, below 30 level 1, else 2, then 0x2D6A50 with the same amount [R]. The weapons without a
  ReactionLevel: BlimpBeam (10 on its first touch: level 0; its DamagePerSec × dt: none), SmartMissile1 (0.5),
  the monsters' own. Here main.c `take_damage_level`.
- The amount is the ImpactDamage as it is (a beam's: × the fire's rate +0x1B0, which the actor (0x30F134) and the
  cluster (+0x6C / +0x70, only written by its constructor 0x2996EC) pass as 1; DamagePerSec × dt × that rate after
  the first touch) [R]: no difficulty, play type, profile, upgrade or armour enters. 0x2D6A50 takes it straight off
  HP (0x2D9364), clamped to MONSTER_DATA +0x138 (0x2D9374); its scaling by the four records at +0xBA8C (+0x8 the
  damage, +0xC the knock, by DamageType masks, 0x2D7138 / 0x2D7C80) is filled only by a clip event no monster has
  (FORMAT_MONSTER.md). No invulnerability window: only +0xAA2 (always set in play), a grab (+0xB9C4) and 0 HP stop a
  hit [R].

This is `AE_HIT`, and `army_reaction_slot` is the monster side:
- **Ignored entirely** while the monster is not interruptible, is in a grab, or has 0 HP. The interruptible byte
  (+0xAA2: set to 1 by every clip's start, 0x2E7F70; cleared only by a clip event no monster has, 0x2E7CC0, or the
  binding `setInterruptabe` (id 88, 0x2AB258), which no script calls [R]: always set in play).
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

## The monster's weapons (the roar)
**The list** [R] (0x2D4C40, from the monster's setup 0x2E86C0): the property book's `WeaponList` (the seven books
the 30 monsters share, Amanda_, Cal_, George_, Jill_, Lizzie_, Ralph_ and Rhett_prop, alike) has four
`Weapon|<key>` groups: `Breath` (BreathWeapon, an RBeamWeapon, on `Lift_Node`),
`Fart` (FartWeapon, RProjectileWeapon, `fxNode_Fart`), `FireBall` (TankCannon, `Lift_Node`) and `Roar` (RoarWeapon,
a `Shockwave`, `fxNode_Belch`, the mouth). Each is made like an army weapon: WeaponName's group of Weapons.bdp read
first and the key's group over it, the type the key's `WeaponType` else the book's; the key is its name (+0x88),
compared without case (0x32C9C8). They are world objects that stay when a block's objects go (flag 0x200000, see
GAME_NOTES.md), kept at +0xC5E4 + 8 i with a state at +0xC604 + 4 i (room for four, no check) and the count +0xC614.

**Who fires** [R]: only Monsters.bsf's `fireWeapon(p0, p1, ...)`, a clip event: `monster.fireWeapon("roar")` when p0
is 0, else `"fart"` when p1 is 1. All 30 monsters call it in their three roar states alone, with (0, 0, 0, 0), beside
`useRoarEnergy` and `faceAnim(12)` (Expression_Roar): `Ground_Roar` (taunt2, at 0.063 s), `Front_Roar` and
`Side_L_Roar` (the face and side Powerup clips, at 0.38 s), all reached by R2 when `hasRoarEnergy`. So the breath,
the fart and the fireball are made and updated but never fired. The binding `fireWeapon` (0x2D4B80) puts each weapon
of that key in state 3; `stopWeapon` (0x2D4A80: state 2 and the weapon held, vtable +0x7C) no script calls. The
other way in, `setWeaponControl(state, weapon)` (0x2E8DB0, every monster's `<Name>::EnableWeapon` clip event asks
for `"Expression_Weapon"` and `"Roar"`): a state of that name would fire the weapon (state 3, 0x2E7248 .. 0x2E72F8,
when the node flag 2 turns on, the weapon's +0x78 allows and +0xA50 ≥ 0.25, which it then loses) while playing it on
the face layer; no monster has an `Expression_Weapon` state, so +0xC620 = 0 and +0xC624 = −1 and it never fires.

**Each frame** [R] (the monster's update 0x2E1400 .. 0x2E14E0, per weapon): the monster's own state (+0x5C, vtable
+0x54) takes the weapon's; a state 3 calls the weapon's tryFire (vtable +0x68: no target, rate 1, 1) and becomes 2;
then the weapon's update (vtable +0x6C with 1).

**The Shockwave** (vtable 0x44B540; its WSDesc 0x44B5E0, 0x54 bytes, made by 0x31B9D0) [R]:
- WSDesc (0x31C1C0, over the base definition 0x297DC0): `Duration` +0x3C (default 1), `DamagePerSec` +0x40 (0),
  `CollisionRadius` +0x44 (10), `HorizontalKnockback` +0x48, `VerticalKnockback` +0x4C (0), `ReactionLevel` +0x50
  (−1). RoarWeapon: Duration 1, DamagePerSec 10, CollisionRadius 10, DamageType Concussive (0), ReactionLevel 1,
  FireEffect `Roar_Weapon`, Ammo 100 (never counted down), no RefireDelay.
- Init (0x31C080): the countdown at ChargeTime + FiringOffset (0), +0x1AC = Duration.
- tryFire (0x31BF20): the charge (0x297760, no ChargeEffect), then only with the countdown at 0, no burst on (+0xF5)
  and the weapon updated once (+0xF4): +0x1AC = Duration, the `FireEffect` started at the weapon's place and frame
  (+0x2C / +0x8: the WeaponNode's world, the weapon's +0xBC being the identity; the effect is not moved after),
  +0xF5 = 1 and +0x1B8 = 0. A roar within a burst's Duration of the last does nothing.
- Update (0x31BE50): +0x1AC falls by dt; at 0 a burst that is on ends, its effect stopped, the countdown RefireDelay
  (0), +0xF5 = 0. Then the common 0x2971A0: the place from the node, and with weapons on (gp−0x772C: set as play
  starts, 0x3243A8 / 0x324494, cleared with the block won, 0x324510; off, every monster weapon is held each frame)
  the countdown falls by the rate × dt, not below 0; then the burst (vtable +0x70).
- **The burst** (0x31BAD0), once a burst (+0x1B8): the RGridMgr's sphere query (vtable +0x3C / +0x40 / +0x44 =
  0x282BC0 / 0x282A40 / 0x282A30, include −1, exclude 0) over the world's objects: each one whose shape's bounding
  sphere (centre +0xC through the object's matrix, radius +0x18) is within CollisionRadius of the weapon's place,
  but the monster itself (+0xB0). Its way from the place, made 1 long ((1, 0, 0) when too near):
  - a monster (flag 0x40): the knock (the way's x, y made 1 long × HorizontalKnockback, VerticalKnockback up: none
    for the roar) to 0x2D9650 → 0x2D6A50 with the DamageType, the roaring monster, no point, the ReactionLevel and
    DamagePerSec as the amount: 10 HP and level 1 (React_Ground_Heavy on the street, slot 1) once a roar. Without a
    point 0x2D6A50 takes the knock against the monster's +y row for "from behind": a zero knock is from the front;
  - anything else: its TakeDamage (vtable +0x34) with the DamageType (Concussive 0, as an armed attack's), the
    monster, no point, the way, and the amount 10: people fly as from a blow, the parked cars, the army's units,
    the bosses' parts, the parade and street objects, the huts and the MonsterUpgrade take it as from an attack;
  - a building (0x2BC150): a hit of type 0 from a monster with a way and no point takes its own path (0x2BC328):
    only a standing building with storeys (+0x1D0 > 0); the monster's shape centre (0x287F00; its place without a
    shape) picks the front cell: column (x − min x) / 5 and storey (z − 6) / 3, each clamped to the building's
    (+0x1C8 columns, +0x1D0 storeys); that cell is hit at its middle (min x + 2.5 + 5 column, the front's y, min z
    + 6 + 3 storey + 1.5) as an ordinary hit (0x2BC4B0 on) whose radius is the amount, 10: 12 dents and the cell's
    2 HP, its Scoring paying the monster (+0x190). The bounding sphere of a building is large (its box's half
    diagonal, about 16 for a 3-wide building of six storeys), so a roar on the street cracks the front-row
    buildings beside it and the back row behind them (New York 3 at x 25: three cells, two of them a row back).
- No sound of its own (the roar's sound is the clip's); the Expression_Roar face is `faceAnim(12)`.
- **One Roar_Weapon a roar** [R]: the roar's look is the Shockwave's own FireEffect (tryFire's 0x208220 on the
  effect manager 0x48A340, released by 0x207A40 when the burst ends). The three roar states have no clip event 12,
  and their face clip (Expression_Roar) starts no effect. Roar_Weapon.edf as a clip event 12 (slot 0, on, then off
  0.43 .. 0.45 s later) is in the other three states of every monster, `Ground_Rampage`, `Front_Rampage` and
  `Side_L_Rampage` (the rage's start, slot 54), which call no fireWeapon: the rage shows the rings and deals nothing.

Here army.c `army_mon_weapons_read` (the list), `army_mon_weapon_fire`, `army_mon_weapon_step` (the update and
tryFire: AMW_START / AMW_STOP / AMW_BURST), `army_monster_shape` (the WCISpheres' bounding sphere), and main.c
`mon_weapons_step` (the effect at the node's world, `fx_spawn_frame`, stopped after Duration) and `roar_burst`: the
other monsters by their spheres' bounding sphere (`monster_hit`), the buildings by their box's (`building_hit` at
the cell above), the rest through the modules' attack entries (`army_hit`, `vehicle_hit`, `rail_hit`, `boss_hit`,
`parade_hit`, `streetobj_hit`, `people_hit`, `scene_huts_hit`, `upgrade_touch`) with a fresh attack record, the
sphere being the roar's [G: those test their own shapes against the sphere, the game the shapes' bounding spheres;
a box's reach is a little shorter here]. The roar feeds no challenge of its own: the things it breaks count by their
scripts' addStats as from a blow (CHALLENGE_NOTES.md); `RoarAttack` is the upgrade that unlocks it (ability 4).

## The units

**Tanks** (`Vehicle_Attack.bsf`, pulse 1/15 s):
- **Arrival** [R]: from x = ±100, y = −7.5, along a LineMover at 12.
- **The AttackMover** [R] (init 0x314B40, update 0x313F80) takes over once the tank is on screen
  (`isSphereVisible` of a point 10 beyond it, radius 1: gWorld id 0x40 → 0x16C0A0 on the camera, a six-plane
  frustum test, four sides n·p + r ≥ d, then the depth along the view within near − r .. far + r [R]; here the host's
  `visible`, the game camera's view at 4:3, main.c `people_visible`):
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
- **The turn onto the target:** every helicopter's WeaponCluster names `Helicopter_Base_Rotate` (RotationRange 360,
  1 rad/s) and `Helicopter_Base_Pitch` (ElevationRange 20, 0.5 rad/s), with `ReCenterWhenNoTarget`. In the shapes'
  trees (Helicopter, NewsChopper) those nodes carry the whole body: Helicopter_base → Helicopter_base_pitch →
  Helicopter_base_rotate → the body, its rotors and Weapon_0. So while the script's `setTarget(monster)` holds
  (states 2 and 1) the helicopter turns its nose onto the monster and dips it by up to 20°, and from state 3
  (`setTarget(nullTarget)`) it levels out again ("The turn is drawn" above).
- **The news helicopters' cameras** [R]: `NewsHelicopter` and its city copies (`NewsHelicopterCG`, `LA`, `LV`, `NY`,
  `SF`) carry the same cluster with one PulseCannon on Weapon_0 at `Ammo 0` (`RefireDelay 1000`). A weapon without
  ammo never fires (0x2978D0, "The countdown" above; `IsHoming` is not set, so the script never gives it any), so
  the cluster does nothing but turn the NewsChopper onto the monster it has spotted: that is the camera, the
  chopper itself pointing at the monster, with its `VoiceSound` (`V_Traffic_Reporter`, `V_Traffic_Chic_Seq` …) when
  it spots one. There is nothing more to it: the ELF has no news, broadcast or render-target strings (`RTTCamera` is
  the play camera, CAMERA.md), no script but Vehicle_Helicopter.bsf names these templates, and neither Hud.bsf nor
  the GUI books (CommonGUI, WorldGui) have a picture-in-picture or a news screen. Their 300 `Points` and the
  addStats on their death are a helicopter's.
- The spline is the SplineMover's (0x33ACA0, BOSS_NOTES.md: a Kochanek-Bartels curve whose place d grows by the
  velocity in the points' own d units, answering 2 every frame past its end) [R], ported as boss.c's (army.c
  `spline_step`); flag 2 turns the helicopter along the tangent (yaw only here; the game sets its whole frame, from
  the tangent and z up, 0x33AD58: here the tilt goes to 0).
- **The wobble** [R] (the AttackObject's, BOSS_NOTES.md "The AttackObject's oscillation"; 0x315330, 0x314EF0): every
  helicopter template, the news helicopters too, has `OscillationScale` 0.1 and no offset, so the phase starts at 0
  and grows by the world's step, a second a second. After the update the facing gets (sin 2φ, cos 3φ, 0) × 0.1 less
  the last one: on the path (flag 2, the frame made afresh each frame) only that frame's change, at most 0.3 × the
  step (0.01 rad at 1/30);
  once flag 2 is off (a monster spotted: states 2, 1, 3) it keeps its frame and rocks ±0.1 rad (5.7°) about x and
  y, and state 2's `facing.x × 0.9` each pulse damps the x part. The wreck's flight gets the change onto the
  SpherePhysicsMover's frame. Here `army_osc_*` in `unit_update`, the keys in `unit_new` (the instance book the
  AIInstance).

**Trucks and police cars** (`CombatTruck.bsf`) [R]:
- **Arrival:** they come from x = ±100 to 8 past the nearest monster (within ±40), at 20. getNearestMonster finds
  any monster in the block (below: no limit short of 1000), so the end is 8 past it.
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
- `getMonsterDirection` (Actor id 0x3F → 0x2B2850) [R]: of the player monsters, the nearest (d² < 1e5); the eighth of
  the way to it 3 above its feet, in the front's plane (atan2(dz, dx), dy unused).

**StationaryAttackObject** (the PirateShip) [R]:
- **Made by the block** (0x304450, the current block's objects): its only user, the PirateShip, is an
  `AttackObjectInstance|` of LV05_JollyRogers at (−3.4, 10, 0), ZRotation 70.69°, in the lagoon. That list is made
  AttackObjects (the pool 0x4D5EB0, 0x283DA0) unless the instance or the template has `NoLoadOnActiveBlock` or a
  `SpawnDelay` (gAIMgr makes those later, 0x334A20: only the bikers' actors have one). Its init (vtable +0x14,
  0x304FA0 → 0x315330, f12 0) runs the world object's 0x287AB0 (Location, ZRotation degrees about z, the group kept
  as the instance book +0x7C), the script's Construct, the WeaponSystem (0x29C020) and the wobble's keys; then
  gWorld vtable +0x3C adds it; onInit at the block's start. Its Process runs every frame from the load, without
  gAIMgr's activation. Here `army_load_block` (`block_attack_objects`) makes it a unit; city.c keeps the still model
  until the unit is drawn, then carries the flag's cloth (`Flagpole_Pirate`) on it (`Scene.army_obj`).
- **Aim:** `selectTarget` aims the cluster at a random monster (its target point), and a point (`lTarget`, a
  DummyObject: its target point is its place, vtable +0x1C 0x285AC0) follows it; once the monster has stayed within
  0.5 of the point for 0.55 s the point locks onto it 11 up (`TargetOffsetZ`) and the cluster aims there, until the
  monster is 3 away for 1.05 s. The cluster: `Cannon_Rotation` ±180° at 0.5 rad/s, `Cannon_Elevation` ±160° at 1,
  neither required on target, so it fires while turning.
- **Firing:** `TankCannon` over the book's (Velocity 20, Gravity 1, ImpactDamage 5, Blunt, RefireDelay 0) from
  `Weapon_Node_1` along its +Y. Construct leaves it without ammo; every `WeaponRefireDelay` (8, the first after 2 s)
  Process gives it one round (setWeaponAmmo, ammo = min(1, 100)) and plays `FireSound` 0.5 s before; the countdown
  (FiringOffset 3 + ChargeTime 1 the first time, then max(0, 1)) fires it 4 s, then 1 s, later. Aimed at a point 11
  above the monster's feet with Velocity 20 the ball goes over a monster on the street and comes down far behind it;
  it hits one wading in the lagoon (its feet at −5).
- **Death:** 3 hits from a monster (TakeDamage ignores anything else); each stops the last damage effect and
  starts `HitEffect<n>` attached (`Tank_Smoke_Big`, `_Med`) and `HitEffect` (`Tank_Spank`). The hop (flag 4) is
  for the unsinkable only. At 0: no `DestShape` (the book has only `xDestShape`), flags 8, no longer
  PushOutMonsters, weapons off, `IsSinkable` so a LineMover 10 down at `SinkRate` (4), `HitEffect0`
  (`Tank_Explode`) where it is, addStats type 0 (1000 points). 1.5 s later `HitEffect` and flags 16; 1 s after that
  removeAIObject and it is gone (2.5 s, as long as the sinking). The script has no onMoverCallback.
- **The wobble:** `OscillationScale` 0.1, no offset: it rocks ±0.1 rad about x and y all its life (the facing is
  its own; "The wobble" of the helicopters above).

**getNearestMonster** [R]: the Actor binding 0x42 (0x2A8FA0 -> 0x2B26F0) and the RScriptObject one 0x37 (0x2AA080 ->
0x2B2CC0; the AttackObject's lookup 0x2A9F00 falls back to it) are the same code: the nearest player monster by
3D distance (0x2B27E8 is `sqrt.s`), starting at 1000, so none farther than 1000: in a block, any (army.c
`nearest_monster`) [G: with none (no monster in the world) the
index is −1 and the script is handed gWorld +0x3C (0x4D17DC, the monsters' count in 0x2BD940) through the result's
setter 0x27C8D0; what it then reads is not followed, here none]. The movers' own target (flag 8, LineMover.getTarget:
the SWAT's and troops' aim, the facing) is not that: 0x316060 takes the nearest from FLT_MAX, no limit
(`mover_target`).

**Bombers** (`Bomber.bsf`) [R]:
- **Path:** across at 20, at `Height` (from the AIInstance; 42 by default), at y = 5 (row 0) or 15 (row 1).
- **Bombing:** `Carpet` bombers drop every pulse (1 s). `Targeted` ones drop when within 4.8 along x. Both only with
  a monster found by getNearestMonster (updateBombStatus): any in the block.
- **Death:** one hit makes it fall (addStats type 2).
- `reloadWeapon` (Actor id 0x3D; the AttackObject's id 0x3A takes cluster and weapon, −1 all) is the weapon's vtable
  +0x80 (0x296F70) [R]: ammo = Ammo (0 stays 0), the countdown at least ChargeTime + FiringOffset, the charge effect
  restarted (not here). `setWeaponAmmo` (+0x84, 0x296F50) is ammo = min(n, Ammo).

## Droppables
`Droppable.bsf` [R], `army_drop` for people.c's `PE_DROP`:
- **The path:** a SplineMover at 12 from the window, through a point 0.6 of the way across, 3 out and 1.5 up, to 5
  in front of the wall at z = 1. If a building stands there, the end is on its roof (storeys × 3 + 1.5 + 6).
  The building there is findBuildingByPosition's (0x2A3D20, no direction): the last in the world's list whose
  footprint holds the point, rubble too; its place is its box's middle (the box is laid about it) [R].
- **The landing x:** the column edge at or right of the drop (getCellCenter), ± 2.5 towards the monster below.
- **The turn:** a quarter turn about y over the fall.
- **On the ground:** the `GroundEffect`, and it is gone.
- **On the monster:** when the monster takes it (within 2.5 along x) it plays `HitEffect` and `HitMonsterSound`,
  calls `setReaction(19)` (an overhead hit), and is gone. No damage.
- The game calls this TakeDamage from the monster's object test (0x2E22B0) [R]: a droppable has neither 0x8000 nor
  0x2040, so every frame a monster's sphere touches its shape it is TakeDamage(type 0 and the attack's damage when the
  sphere is armed, else type 1 and 2.0). Here it fires when one of the monster's spheres meets its shape, or when an
  attack reaches it; the shape is 0x28FD70's, and the Droppable_* books (Objects1_Props; their parent DefObjectProps
  has only Gravity 35 and Points 10) set no CollisionBox, CollisionRadius or AutoCollisionBox, so it is the model's
  sphere (0x184CB0, through the model's vtable +0x14 at 0x28FEC4) [R].

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
- **Random numbers:** `frand` is the world's one Park-Miller state (gWorld +0x828, 0x106BD0, seeded from GameStatus seed[0]), shared with every other system as in the game (wrand.c) [R]; the frame step is the world's, 1/30
  (0x2B7800) [R].
- **The collision world** (collide.c, the frame's world the viewer builds, `army_set_collide` /
  `army_collide_add`): the shots' and beams' rays and the flights are the game's. Left:
  - flag 4 is every world object's, VolumeTriggers and held objects too (addObject 0x284CD0, COLLIDE_NOTES.md) [R],
    so a shell stops at a soldier, a passing car or a trigger's box; a shot's damage to what it hits other than a
    monster (0x292D20: a thunk to the hit object's TakeDamage, vtable +0x34, with the shot's DamageType, its owner
    unit, ImpactDamage, the point and the shot's direction, no knock [R]) is not ported: the scripts' TakeDamage
    begin with coerceToMonster and ignore it, but Fountain.scr (ungated) and DamagingEffect.scr would not;
  - a unit has one shape by 0x28FD70 (`collide_shape_pick`: CollisionBox, CollisionRadius, AutoCollisionBox, else
    the sphere the model keeps (0x184CB0, stored apart from its box, 0x184940) [R]; here the union of its meshes'
    header spheres, city.c `city_shape_sphere`), and the monster's spheres meet that one shape (0x2E22B0 ->
    0x280470) [R], in the collision world, for the attacks (`army_hit`) and for a droppable's touch alike: each
    collision sphere of the monster, armed or not, against the droppable (the Droppable_* books set no shape, so it
    is the model's sphere) (`unit_meets`, `touches`).
- **Movers:** the SplineMover's curve and the HeldObjectMover's axis turn (the unit takes the hand's matrix). A flight
  (`mv.kind` 2: a wreck knocked away, a smashed actor, a thrown vehicle, a falling bomber) is the game's
  SpherePhysicsMover (collide.c `collide_mover_step`, 0x28E440) with the template's `Gravity` (DefObjectProps 35,
  else 9.81), `Mass`, `Restitution`, `SlidingFriction`, `InertiaSolidity` and the shape's radius; it has no rest
  (it rolls on until the script ends it). OnHitGround is sent once a frame it touches; the game sends it every
  sub-step (0x28E440, COLLIDE_NOTES.md) [R], the same here: the truck's only sets state 6 and the bomber's is
  guarded. TakeImpact on both what it meets (0x287430: Bomber.bsf's is its onHitGround; another module's object
  through `AE_IMPACT`). Its spin: smashObject (0x2B2F50) hands its rates to setRotation (0x28DD80) as they are, an
  angular velocity about a world axis (Bomber's setRotation(π/4, 0, 0): π/4 a second about world x) [R]; the facing
  follows the mover's x axis, the tilt stays.
- **The weapon system:** the weapon node without the host's `node_world`. The monster's target point (0x2DB030) [R]
  is 0x2E6000's world place of `Bip01 Spine1` (node index +0x7DB0, its model place +0xB18 + 0x30 × node, scaled by
  +0xA78 when byte +0xA74 is set, through the monster's matrix) plus M × the offset +0xBD54 (set only by the script
  binding `setTargetOffset`, id 76, which no script calls; zeroed at 0x2DA6CC, 0x2E805C, 0x2E9A34), its z then the
  feet's + 6: here Spine1's x and y (`ArmyMonster.target`). A shot's homing aims at the target's shape centre (+0x74,
  +0xC, through its matrix: 0x293260 → 0x287F00; no offset, no lead) [R]. A monster's shape is its own WCISpheres
  (+0x68C, set up by the ctor 0x2EB6E0, +0x74 = it at 0x2EB8AC): 0x2DA9E0 places the collision spheres from the
  posed bones (lazily, byte +0x92C) and 0x29A880 merges them in order (0x103770: a centre within 0.001 keeps the
  larger, one inside the other the outer, else the sphere round both), so the centre is the spheres' bounding
  sphere's and moves with the pose. As here (army.c `monster_centre`, from `ArmyMonster.spheres`). Both the projectile's wobble (definition flag 8) and its spread (record flag 8) come from flags no book
  sets.
- **The camera's place** [R]: Bomber's `getCameraPos().x` (id 0x41, 0x2AF220) is gWorld +0x684, the eye the
  objects' render distance is taken from (0x290FBC); the host sets the view about its camera's x
  (`army_set_view`). The x range is also what is seen for a host without `visible`.
- **The hold** [R] (the HeldObjectMover, init 0x310ED0, held 0x3109E0, as people.c's): `MajorAxis` (default 2)
  along the hand's z, the next along its y, the last their cross product, at the hand plus `HoldOffset`
  (`MirroredHoldOffset` while the monster plays mirrored) in the hand's frame (`army_hold`). Its blend in from
  where the unit was (+0x84, 0x310B50, 0.1 s from doGrabObject) is not ported [G: three frames].
- **Other:** the impact's visibility test (ImpactVisRange) and the ImpactEffect's orientation (the surface's
  normal) are not traced.
- **Elsewhere:** the bosses are boss.c's ([BOSS_NOTES.md](BOSS_NOTES.md)); `Vehicle.bsf` (the `Humvee` and the other
  street vehicles, `AK_OTHER` here) and Rocket / RocketBase are vehicle.c's ([VEHICLE_NOTES.md](VEHICLE_NOTES.md)).
  The `BradleyTroopCarrier` is a CombatTruck and is covered.

## Driving it from the viewer
1. **Setup:** `army_new` with a host whose `find(book, name)` returns `book_group` of `<book>.bdp`. Use the same
   resolver as `prb_set_resolver`. Then `army_load_block(BlockDef group)`, and `army_activate(1)` when play starts.
2. **Each frame:** call `army_update(dt, monsters, buildings)`, where the monsters carry their collision spheres in
   the world. Then:
   - draw each live unit's `shape` at `world`; actors play `clip` at `anim_time` (one play is `anim_len`);
   - turn each weapon's `rot_node` by its `yaw` about the node's own z and its `elev_node` by its `pitch` about the
     node's own x, before the node's own pose (once per node; city.c `army_sync`), and give `node_world` the nodes
     as drawn;
   - draw each live shot by its `effect` at `pos` along `dir` (a beam spans `length`);
   - apply the events: start and stop effects by name (attached ones follow `unit` / `shot`), play sounds, route
     `AE_HIT` through `army_reaction_slot` to the monster's slots and HP, `AE_REACTION` straight to the slot,
     `AE_STATS_*` to HP and energy, `AE_PUSH` to the monster's place, and `AE_IMPACT` (a flight met another
     module's object, `id` its collision tag) to that object's TakeImpact (a building: `building_hit` with radius
     `amount` / 1000).
   - before the update, the frame's collision world: `army_set_collide` once, then each frame `army_collide_add`
     between `collide_begin` and `collide_finish`, with the monster's spheres added as `COLLIDE_F_MONSTER` objects
     tagged `COLLIDE_TAG(COLLIDE_OWN_MONSTER, slot)` (main.c `world_build`).
3. **Monster attacks:** an armed attack sphere calls `army_hit`. The grab uses `army_nearest_pickup`, `army_grab`,
   `army_hold` each frame and `army_throw`.
4. **People:** `PE_DROP` goes to `army_drop` with the block's `DroppableDef` template, and `PE_WEAPON` goes to
   `army_person_weapon` (on with its name, off without: both), with `army_person_moved` while the person is on and
   `army_person_forget` when it is gone.
5. **Buildings:** building damage calls `army_score_building`.
