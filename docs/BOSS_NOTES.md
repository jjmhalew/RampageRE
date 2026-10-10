# The bosses

What `src/boss.c` ports, and from where. [R] means read from the game's code or scripts, [G] means a guess or a
stand-in. Every boss is a script (decompiled in `build/scripts/Boss_*.scr`) on an AttackObject (the Jumper on an
Actor). The engine supplies the movers, the weapons, the building finders and the monster's side.

## Which block has which boss
The last block of each city has a boss [R]. Its `AIDef` holds one `AIInstance` with `MaxOccurances 1`. The
`ChallengeDef` there is `ChallengeBoss`, with the boss's template as its one `Goal`.

| City | Block | Template (script) | MinDelay | Name on the pause screen |
|---|---|---|---|---|
| Chicago | `CG10_Block10` | `BossSaucer` (Boss_Chicago_Saucer) | 20 | Pause_Saucer |
| London | `GB08_BigBen` | `BossSaucer` | 20 | Pause_Saucer |
| Las Vegas | `LV09_Neros` | `BossTank` (Boss_Tank) | 20 | Pause_Carbonator |
| Los Angeles | `LA08_Hollywood_Blv` | `BossTank` | 20 | Pause_Carbonator |
| San Francisco | `SF10_Presidio` | `BossHelicopter` (Boss_Helicopter) | 20 | Pause_Scumhawk |
| Hong Kong | `HK10_Fortress` | `BossJumper` (Boss_Jumper, `IsActor 1`) | 15 | Pause_Leeper |
| New York | `NY10_Empire_State` | `NYBlimp` (Boss_NY_Blimp) | 1 | Pause_Blimp |

- **Spawning:** the boss comes from gAIMgr like any other unit: its timer starts at `boss_activate`, at
  MinDelay + OffsetDelay × rand.
- **Extra block data:**
  - SF10's AIInstance also carries the helicopter's flight paths: `NumPaths 4`, `Path<n> { NumPoints, Point<i> }`.
  - HK10's AIInstance carries the Jumper's `Location (-50, -10, 0)` and its `FleePoint1..5`.
  - NY10 adds two `NYGenerator` ObjectInstances at (−17, 9.5, 22) and (18, 9.5, 22).
  - NY10's Empire buildings carry `VolumeTrigger|Barrier` / `VolumeTrigger|BarrierTop` groups.
  - LA08 and CG10 keep army records beside the boss: `HelicopterHomingMedium` and `Tank11sec`.
- **Ambient bosses** [R]:
  - Some earlier blocks hold an `AttackObjectInstance|` at (100, 5.2, 28) with `LoadWhenWaiting 1` and
    `NoLoadOnActiveBlock 1`. It is made only while that block waits behind the one being played (slot 1, 41 further
    back), and it crosses that block.
  - `BossSaucerAmbient` is in CG02, CG04, CG06 and CG07_Block8, and in GB02, GB04 and GB06.
  - `BossBlimpAmbient` is in NY02, NY04, NY06 and NY08.
  - `BossHelicopterAmbient` (Boss_Blimp_Ambient with Animate) is in SF02, SF04, SF06 and SF08.

## The block
**Callbacks** [R]: each boss calls its AIInstance's `CompletionCallback` (`<City>::setBossBlockFinished`) when it is
beaten (`BE_DEFEATED`). The NY blimp calls `NewYork::setBossBlockFinished` itself.

**New York** [R]:
- The block's `IsBlockFinished` is `NewYork::isBossBlockFinished`. It is 1 once the blimp is beaten, and 2 when no
  building is left. If the middle building at (0, 10) has fallen when the blimp is beaten, it is 2 at once.
- A non-zero value ends the block: the world state's update (0x324888) asks the BlockDef's IsBlockFinished in state
  6. So in New York, beating the blimp ends the block.
- `ResultsNI = NewYork::BossResultsNI`: on 1 the monster stands on top of the Empire building at (0, 18, 61.5) for
  the results; on 2 the usual results run.

**Every other city** [R]:
- The data spells the key `xxIsBlockFinished`, which turns it off. The flag their `setBossBlockFinished` sets is
  therefore never read.
- The block ends by the engine's own rule: no script, so 0x324888 goes on to its other checks. [G] Those are taken as
  every building down, like `Levels::IsBlockFinished`; 0x33D740 was not read.
- So the boss is a fight inside the block, worth its `Points` (1500, category 2 "vehicle", through addStats type 2)
  and the challenge's Goal. The block still has to be flattened.
- When the city's last block is finished, `gCommon.isCityDestroyed` (0x4D7E4F, set by 0x325AD0 in state 9 when the
  current block is the last) becomes 1, and a boss still around leaves for good.

**The HUD** [R]: `gCommon.updateChallengeHud(frac)` drives `ChallengeStatus::updateBossHealth` (the boss bar,
100 × frac). The boss group shows only on the last block.

**The camera** [R]: every boss's onInit calls `Levels::changeTwoTiltCameraRMH(35)`. One frame (1/20 s) later the
TwoTilts camera's RepMonsterHeight is 35, so the camera frames higher. It is never set back.

## The natives they use
**SplineMover** (vtable 0x44B800, update 0x33ACA0) [R]:
- **Points:** each is a place and its distance along the path (`addPoint(p, d)`). The velocity is in those distance
  units per second. The Saucer's paths run d = 0 … 1 at 0.125, so one takes 8 s.
- **Each frame:** flag 4 holds it still. Otherwise d grows by velocity × dt. Past the last point it loops (flag 1),
  or it stays there and answers 2 (`onMoverCallback(2)`) every frame until the script gives it a new path.
- **The curve** (0x13B780, tangents 0x13B2F0) is Kochanek-Bartels / Hermite between the points that hold d:
  - each point's tangent is ½(1−T)(1+B)(1+C)(p−prev) + ½(1−T)(1−B)(1−C)(next−p);
  - an end takes itself as its missing neighbour; a closed path wraps round.
- **Flag 2** sets the object's whole turn from the tangent: Y along it, X = tangent × Z, Z = X × tangent.
- [G] `addPointWithVel(p, d, k)` scales that point's tangent by k. The Jumper's 0.05 gives a sharp take-off and
  landing.
- [G] `clearPath` puts d back to 0.

**LineMover** (as people.c): from point to point. Flag 1 faces the way it goes, and flag 2 loops. [G] Flag 4 is
taken to round the corners, and is not done. `initWithPath("Path_NYEmpire1", 15, 1)` takes the path's points from
the Objects1_Props group of that name (x y z triples). [G] It starts at the first point.

**Building finders** (World bindings, dispatcher 0x2AE3D0, table 0x42DD70) [R]:
- `findBuildingNearMonster(d)` (0x318240):
  - **The span:** the monsters' least and most x (0x318390). If it is under 20 wide, it becomes 30 wide around its
    middle. Then it is widened by 10, and by d (d < 0 narrows it).
  - **The pick:** from a random building, going a random way round the list, the first standing one whose middle x
    lies in (lo, hi].
- `findBuildingNearPosition(p, r)` (0x2B4D50): of the buildings whose box meets the box p ± r (z 0 … 1000), the one
  nearest along x.
- `getRandomBuilding` (0x2AEC40): from a random one on, the first standing one.
- `findBuildingByPosition` (0x2A3D20): [G] the standing building whose footprint holds the point.
- The scripts' height of a building is always storeys × 3 + 1.5 + 6.

**Other natives** [R]:
- `gWorld.getPlayValue / setPlayValue`: a table of ints. The generators write it and the blimp reads it.
- `Monster.isClimbing`: +0xB9C0 ≠ 0.
- `Monster.playReaction(name)`: the move state of that name (0x2D9890), started through 0x2D9710.

**Object flags** (0x3F5F70) [R]:
- 0x4000 `PushOutMonsters` (Tank, Jumper): a monster is pushed out of the part's box by the collision world
  (collide_monster, 0x2E2C50 [R]: the box query with include VolumeTrigger | PushOutMonsters).
- 0x8000 `OnlyHitWithEffNodes`: only attacks reach TakeDamage. It is on the Tank, Heli, Jumper, Saucer, Blimp and
  Generator.
- 0x40000 `IsObstacle` (Whirlwind).
- 8 (Whirlwind, the Saucer's rotor) has no name.
- 0x8000000 (Heli, ambients with `Animate`) has no name. [G] It plays the model's own animation.

[G] Without 0x8000, touching the object calls its TakeDamage(monster, 0, Collision) every frame. That is how the
Whirlwind catches a monster, how the Jumper's goo burns one, and how the wrecked Tank shoves one. The game's
collision for this was not read.

## The bosses
### BossTank ("Carbonator", Boss_Tank)
**The template:** HitPoints 76, Acceleration 25, MaxVelocity 17.5, ShockwaveDamage 50, BombFrequency 10,
LeaveDuration 20, ReturnDuration 20. Its weapon is `SudsGun` (charge 3 s + offset 3 s, refire 8 s, a lobbed
projectile, Gravity 0.25, 5 damage, ReactionLevel 1).

**Entry** [R]: at (−100, −10, 0), facing +x, with `Carbinator_Run` attached and the `Carbinator_Charge` glow.
Every 5 s, half the time (or when its monster is gone), it picks a random monster.

**Movement** [R]:
- Along x only. Beyond 11.5 from its monster it accelerates towards it at 25/s², up to 17.5.
- Within that it brakes (× 0.9 a frame). Once stopped it waits 3 s.
- It wobbles (facing x, y = sin 2t, cos 3t × 0.0625).

**The shockwave** [R]:
- **Trigger:** a monster within 15 along x, 10 along y and below z 3 starts a 1.25 s countdown. Every hit on the
  tank sets the countdown back to 1.25.
- **The blast** (`Carbinator_Shockwave`) reaches monsters below z 9. The strength is 1 within 7.5 and falls to 0 at
  17.5. Each is thrown outwards at 25 × strength and up at 20 + 25 × strength, with React_Airborne, and loses
  50 × strength HP.
- **After the blast:** the tank hops (12 up, 24/s² down) and backs off for 1.55 s. It recharges in 2.66 s.
- **No monster near:** the gun gets one round every frame, so it fires on its own 8 s cycle.

**Bombs** [R]:
- Every BombFrequency (then every 10 s), while it is not blasting or backing off, it launches a `BossTankBomb`.
- **The arc** (Boss_Tank_Bomb, a SplineMover at 20): to the roof of the building a monster stands on (at that
  monster's x), else `findBuildingNearMonster(10)`, else a random building. Its top is 1.75 × the roof height.
- **The impact** (`Explosion_Fireball`): within 7.5, strength 1 − d/7.5, and within 6 up or down. The throw is
  15 × s out and 10 + 15 × s up, with React_Airborne and −20 × s HP.

**Damage** [R] (any attack):
- **A quarter crossed:** a hit that crosses a quarter of its HP (76 → 57 → 38 → 19) makes it back away for 1.25 s
  and leave. Its weapons go off and it says `V_Boss_Damaged`. Below the last quarter, any hit of 1 or more does
  this.
- **Each hit:** `Blimp_Explode`, and a damage effect `HitEffect<int(hp / (76/3))>`: HitEffect2 from 2/3 down,
  HitEffect0 below 1/3. The HUD gets hp / 76.

**Leaving** [R]:
- **When:** after LeaveDuration (20 s) of fighting, or at a quarter crossed.
- **Where:** it drives off to the side away from its monster, turning to face −x at π/3 a second when that is left.
  It stops at x ±95.
- **Return:** it comes back after ReturnDuration (20 s), with LeaveDuration + 6 to fight before the next leave.

**Defeat** [R]:
- **At HP ≤ 0:** addStats type 2 (1500), `V_Boss_Defeated`, weapons off, 0x8000 cleared, the CompletionCallback.
- **Then:** it drives off and is deleted past ±95.
- **On the way:** a monster on the street that touches it (on its leading side, within 3) is shoved at (0, 15, 0).
- **City destroyed:** if the city is destroyed, it is deleted the next time it is off screen.

### BossHelicopter ("Scumhawk", Boss_Helicopter)
**The template:** HitPoints 3, Velocity 25, MaxMissiles 2, FiringDelay 3. Its weapons are `SmartMissile0` (15
damage, homing, ReactionLevel 1) and `SmartMissile1` (0.5 damage), both from `Weapon_0`. `Scumhawk_Engine` plays on
both rotors.

**Entry** [R]: at (±75, −15 or 25, 25). At its first pulse it takes one of its AIInstance's 4 paths:
- path points within x ±40 are lifted 3 over the nearest building (within 20);
- a climb of over 12 within 50 is eased in from the points either side;
- coming from the left, it flies the path backwards.
- It enters at 60 (its mover's first velocity).

**Attack** [R] (pulse 0.25 s):
- A monster within 50 becomes the target.
- SmartMissile0 gets 1 + int(rand × 2) rounds and SmartMissile1 gets 1 or 2. It brakes at 40/s² to a hover.
- After FiringDelay (3 s) it drops the target and speeds back up to 25. At the path's end it takes another path.

**Damage** [R] (any attack, each one of its 3 HitPoints):
- It tumbles away (spinning two turns a second, pulse every frame) along a 7-step path away from and above the
  monster, which bounces off tall buildings.
- Its ammo goes to 0, and HitEffect4 (or at the last point, ExplodeEffect) plays on both rotors.

**Defeat** [R]: at 0, addStats type 2 (1500) and the callback. At the end of the tumble it flies the flee path at 25
(round to (0, −40, 75), then down to (−4, 165, 0) behind the city), burning, and is deleted.

### BossJumper ("Leeper", Boss_Jumper, an Actor)
**The template:** HitPoints 51, Damage 10, JumpVelocity 30, JumpDamageDuration 2, LeaveDelay 30, ReturnDuration 15,
RenderScale 1.2. Its clips are `JumperBoss_Idle` / `_Launch` / `_Land` on the `JumperBoss_Idle` model.

**Jumping** [R]:
- **Target:** it jumps onto a roof picked by `findBuildingNearMonster(−5)` (3 tries for one other than its own),
  then `(15)`, then a random one. It lands at a random x along a roof of 3 or more cells.
- **The arc:** a 5-point spline at 30 from its place, 9 up, over the arc (25 × sin 60° over the higher end, raised in
  3s while it would cut a building), 9 over the end, onto the roof (storeys × 3 + 7.5).
- **Landing:** at the end the mover holds and it settles onto the roof's current height at 24/s, plays `Land` (then
  `Idle`), and drops a `BossJumperBomb`. On the street the bomb's damage waits 2 s.
- **The next jump:** after 2 s it plays `Jump`. 0.25 s later it leaves, with `Leeper_Steam` at its nodes 2, 5 and 8.
  Monsters on the ground within 8 are thrown (0, −20, 20) with React_Airborne and lose 10 HP.
- **Falling roof:** if its roof starts to fall, it jumps at once.

**The goo bomb** (Boss_Jumper_Bomb) [R]:
- **Path:** from 2 below the Jumper, 9 out towards the street, down to the street (or onto a lower roof there), then
  out to y = −20 at 10.
- **Burning:** a monster touching its box (10 wide, 10 deep behind it, 1 high) after DamageDelay is blown out once
  at (0, −20, 5) with `Jump_Air_Borne_Fall` and loses 15 HP. Not while jumping, not on a building's side, and not
  climbing a front below it.

**The roof ornaments** [R]: landing within HitRadius of the palace's `HKPalaceTopAdorn1R` / `2` / `1L` breaks them
(`forceDamage`).

**Leaving** [R]:
- **When:** its LeaveDelay runs down twice a frame while it is not in the air, so 30 counts as 15 s.
- **Where:** it jumps out to (0, −30, 75) and waits there for ReturnDuration (15 s), then jumps back in.

**Damage** [R]:
- Only attacks of DamageType 0 hurt it. [G] Monster attacks are taken as 0, and touching as 1.
- **A hit:** it is knocked slowly away (2.25 points a second, 15 steps of 10, at most 60 up), spinning. Then it
  jumps to any roof.
- **Effects:** HitEffect, and HitEffect2 / 1 / 0 above ½, above 0.15, below.

**Defeat** [R]:
- **At 0:** addStats type 2 (1500) and the callback. After its knock it flies through the AIInstance's FleePoints at
  45.
- **The script's bug, kept:** `doFleeReaction` does `nFlags & 4` where `| 4` was meant. So the flag that would delete
  it at the end is never set. It flies the flee points again and again (behind the city), and its LeaveDelay still
  makes it leave and come back.

### BossSaucer (Boss_Chicago_Saucer)
**The template:** HitPoints 51, WhirlwindLaunchDelay 4, WhirlwindChance 0.4, ReturnDuration 15. LeaveDelay is not in
the template, so it is 20. Its attachment is the `BossSaucerRotor` model. Its weapons are `LightingZap` 0 / 1 / 2
(beams of 5.5 / 3 / 1.5, FiringOffset 0.01 / 0.25 / 0.5). Its effects are `Cyclo_Barrier` (the shield) and
`Cyclone_chargeup`.

**Entry** [R]: across the front from (−50, −5, 22) to (50, −5, 22) in 8 s, shielded.

**Paths** [R]: `createNewPath` goes round to a random monster's side or the front, 3 or 4 points, 10 … 16 above the
monster and 6 … 12 over buildings near the points. Its velocity is 0.125 d/s and grows by 0.0625 per hit, up to 0.6.

**At a path's end** [R]:
- The shield drops, it taunts and charges for WhirlwindLaunchDelay.
- **Whirlwind** (chance 0.4): a `BossSaucerWhirlwind` at its place, at least 12 up.
- **Lightning** (otherwise): after half the delay, a bolt at the monster's place + 6 up (taken then), with a 60 %
  chance of a second and 60 % of those a third.
- **After:** half the delay more, then shield up and a new path.
- The rotor spins 1.5 turns a second, faster while it charges.

**The whirlwind** (Boss_Chicago_Whirlwind) [R]:
- **Path:** 5 steps of about 10 towards the last monster's place (the script computes the monster to pick and then
  uses the loop's last `mpos`), sinking to 12, 1.5 s a step.
- **Lifespan:** 6 s. It is gone after half a second inside a building.
- **Lightning:** every 2.5 s it may zap a monster 20 … 50 away (the first time 50 %, then always).
- **Catching:** a monster touching it is caught: Damage 10 × the life left / 6 HP, React_Tornado, its facing turned
  π/8 every frame of touch. The tornado follows it 9 above.
- **Release:** at 6 s it throws the monster outwards (25 … 35 from the street's middle, 15 … 25 up) with
  `Jump_Air_Borne_Fall`.

**Damage** [R]:
- **Shielded:** sparks (`sparks`, every 2 s at most).
- **Unshielded and not already knocked:** knocked away (15 steps at 2.5 a second), HitEffect, HitEffect<int(hp/17)>,
  then shield up and a new path.

**Leaving** [R]: LeaveDelay counts down. When it is at a path's end it leaves to (0, −40, 100) at 25, then comes back
after ReturnDuration.

**Defeat** [R]: at 0, addStats type 2 (1500) and the callback. After the knock it flies the flee path at 25 and is
deleted with its rotor.

### NYBlimp (Boss_NY_Blimp) with its generators and barriers
**The template:** its weapons are `BlimpBeam` (a beam: charge 3 + offset 3, Duration 3.5, 10 then 10/s) from
`Fire_effects` and `BlimpBomb` (falls from `Blimp_Rotate`, 15 damage, ReactionLevel 2, StartAmmo 0). It has 3
hit points.

**The five Empire buildings** [R]:
- They are found at (−17.5, 1), (17.5, 1), (1, 15), (−17.5, 15) and (17.5, 15). They get `disableAutoCollapse`.
- **Barriers:** each gets a `Barrier_Front` (the middle one `Barrier_Top`) glow. Its `VolumeTrigger|Barrier`
  becomes a trigger. The back three also get the `BarrierTop` one.
- **The triggers** (Center, Extents; DoPushout, MinRefireTime 2.5, FireFunc `NewYork::BlimpBarrierFire`):
  - a monster not climbing is thrown (0, −100, 30) with React_Airborne;
  - a climbing one plays React_Front_Zap (React_Side_L_Zap on a side);
  - `Hazard_Electricity_Explode` plays on it either way.
  - The test and the push are the monster's own (0x2E2C50, collide_monster) [R]: the trigger is a box in the
    collision world flagged 0x400000 (| PushOutMonsters with DoPushout, 0x3360F4); the monster's box meeting it
    reports a touch (`boss_trigger_touch`), which fires the FireFunc at the next update once MinRefireTime has run
    (0x335BF0); with DoPushout it is pushed out along the least overlap (x and y only), unless climbing. [G] Center
    is taken in the world and Extents as half sizes.
- **Openings:** the back-left and back-right buildings' front cells 1–2 on storeys 4–6 are set to state 3, which
  opens the walls before the generators.

**The front two buildings** [R]: they collapse when their HP falls to a quarter. Their barriers go with them, and a
falling back one loses its barriers too.

**Generators** (NYGenerator, InvisShape with the `Generator` effect, 3 attacks) [R]:
- A generator is also destroyed when the building behind it (5 back) falls. Its building is behind a front one, so
  that front building has to come down first.
- Destroyed, it sets its play value: `Fire_Big` plus 4 × `sparks`, addStats type 0 (200).
- The blimp picks at random which generator lowers the top barriers. The other lowers the lower barriers of the back
  three and sends the blimp to the top.

**The blimp** [R]:
- **State 0:** it circles `Path_NYEmpire1` at 15 (z 42 … 50).
- **The beam:**
  - Every 6.5 s (only while it is in front), a monster gets a `Blimp_Reticle` (`_Ground` on the street): 3 over its
    place, 3 more on a wall, 3 out on a front.
  - The beam is reloaded at that point, which the weapon reaches 6 s later.
  - On the top: every 13 s while the monsters are below 35. Otherwise 6.5 s, half the time at the highest monster.
- **Bombs:**
  - While circling: one every 2 … 3 s over x −27.5 … 28.
  - On the top: up to 4 per reload over a monster within 6 along x, faster while the monsters are low.
- **Taunts:** `V_Boss_Taunt` / `V_Boss_Damaged`, at most every 3 s.

**Damage** [R]:
- **When:** in states 0 and 3 or above, each attack is a hit point.
- **Hit 1:** knocked 25 away (never into the buildings), then up to the top: line to (∓25, −40, 38), then
  `Path_NYEmpire3L/R` up to the Empire top (z 68).
- **On the top:** it crosses back and forth over the top (state 3) at 15, turning round in 0.75 s at each end
  (state 5).
- **Hit 2:** knocked, then back up by `Path_NYEmpire4L/R`.
- **Hit 3:** it falls nose-down at 12.5 to (−12, −10, 0), with three `Blimp_Explode`, addStats type 2 (1500) and
  `NewYork::setBossBlockFinished`.

### Ambient bosses (Boss_Blimp_Ambient, Boss_Saucer_Ambient) [R]
- **The crossing:** from its Location (x ≥ 0: from +150), 8 points 37.5 apart across at its height + 0 … 5.
  `NumTrips` (3) crossings, then 50 % more each time, then deleted.
- **The blimp:** at `Velocity` (12, the SF helicopter 18), facing along the path.
- **The saucer:** at 24. It slows to a hover at a random x in −30 … 30 for 5 … 8 s, then speeds off at 40/s².
  Its rotor is not deleted with it (kept).

## Not traced (stand-ins in boss.c)
- **Collision:**
  - the monster touching an object (TakeDamage of type Collision every frame: where the game's touch comes from is
    not traced; here the monster's spheres within the part's box). The PushOutMonsters push and the VolumeTriggers'
    test and push-out are the collision world's (`boss_collide_add`, collide_monster);
  - the parts' shapes (0x28FD70, `collide_shape_pick`: CollisionBox (the generators'), CollisionRadius or
    AutoCollisionBox (the blimp) with the model's box, else the model's bounding sphere [G: the box's]);
  - which DamageType a monster's attack has (taken as 0);
  - `findBuildingByPosition` (taken as the footprint).
- **Movers:**
  - addPointWithVel's per-point value (taken as the tangent's scale);
  - whether clearPath restarts the distance (taken as yes);
  - the LineMover's flag 4;
  - where `initWithPath` starts.
- **Weapons:**
  - the turrets' rates (radians a second, as army.c);
  - `setWeaponAmmo` / `reloadWeapon` (the weapon's ammo, as army.c);
  - `setWeaponFlags(-1, 16)` (Boss_Helicopter: not done);
  - AttackObject weapons being on from the start (the helicopter and blimp never call enableWeapons, yet fire).
- **Odds and ends:**
  - `isSphereVisible` (the view's x range, `boss_set_view`);
  - `adjustZRot` (taken as adding to the turn);
  - getFacing().z read back in (−π, π];
  - the numGenerators global and the play values (taken as starting again per block);
  - `gWorld.frand` (an LCG).
- **Not ported:**
  - the movement sounds (`MovementSound`: `Tank Treads`, `Copter Loop`: native, not in the scripts);
  - the bosses' `OscillationScale`;
  - the challenge's Goal count;
  - the engine's default block-finished check (0x33D740);
  - NewYork's `BossResultsNI` scene;
  - RoofItem.bsf (what `BE_FORCE_DAMAGE` does to the palace ornaments).

## Driving it from the viewer
1. **Setup:**
   - `boss_new` with a host:
     - `find(book, name)`: `city_book_group`;
     - `shape_box`;
     - `clip_seconds`;
     - optional `node_world`;
     - `weapon_make` / `weapon_tick`, which run army.c's `weapon_make` and `weapon_tick` with no unit.
   - On a block: `boss_load_block(BlockDef)`, then `boss_load_waiting(next BlockDef, 41, 1)` (and 82, 2).
   - `boss_activate(1)` when gAIMgr is activated.
   - The army must not spawn the boss's AIInstance (`boss_is_boss_template`). city.c must not place the ambient
     bosses' or generators' AttackObjectInstance / ObjectInstance as still models.
2. **Each frame:** `boss_collide_add` into the frame's collision world (between `collide_begin` and
   `collide_finish`, before anything moves), each touch of a kind-1 object (a trigger) that collide_monster reports
   to `boss_trigger_touch`; `boss_update(dt, monsters, buildings)`, then draw each alive part: `shape` at `world` ×
   `scale`; `animate`: its own RawAnim; the Jumper: `clip` at `anim_time`. Then apply the events (`BE_PUSH` no
   longer comes: the pushes are collide_monster's).
3. **Monster attacks:** an armed attack sphere calls `boss_hit`.
4. **The city's end:** `boss_set_city_destroyed(1)` once the city's last block is finished.
