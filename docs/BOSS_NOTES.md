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
  the results; on 2 the usual results run. It is game.c's `ny_boss_results_ni` (NewYork.scr), as the other results
  scenes are: SetupNI(0.25); the script camera cut to (4, −55, 78) looking at (−3, 10, 57); one player on (0, 18,
  61.5) facing π, two on (±3, 15, 61.5) facing ∓¾π; the Hud and ChallengeStatus faded out; player 1 taunts
  (`Ground_Taunt`), then each now and then a taunt, a grab or a punch down at the roof; at NITime 1.25 the camera
  moves in to (−2.61, −13.75, 74.4) looking at (−3.08, 11.93, 66.46); while `bWatchForFall` they are held on the
  roof, and once the centre building falls the TwoTilts camera comes back in 1.5 s. It never returns 0.

**Every other city** [R]:
- The data spells the key `xxIsBlockFinished`, which turns it off. The flag their `setBossBlockFinished` sets is
  therefore never read.
- The block ends by the usual rule. At state 6, 0x3244A0 looks up "IsBlockFinished" through 0x324180 (the block's
  property, else WorldData's, else the name itself; then `IsBlockFinished`, `<City>::IsBlockFinished`,
  `Levels::IsBlockFinished`). No city script defines `IsBlockFinished`, so it is Levels.bsf's
  `getCurrentBuildingCount() <= 0`: every building down [R]. 0x324888's other tests are the clock (0x347DC0) and a
  cheat: 0x33D740 is `*(0x4D7E68) & 0x10000`, the cheat flags, which with 0x20000 (0x324930) skip a block [R].
- So the boss is a fight inside the block, worth its `Points` (1500, category 2 "vehicle", through addStats type 2)
  and the challenge's Goal. The block still has to be flattened.

**The Goal** [R]: the boss's addStats (0x2C51A0) awards its template's name once (0x346780), and `ChallengeBoss` has
that template as its one Goal (`Required` 1), so beating the boss meets it. In the port the boss's `BE_STATS` goes
through `army_add_stats`, whose `AE_SCORE` carries the template's name to `challenge_add_stats` (main.c
`challenge_stats`, challenge.c); the HUD shows the boss bar instead of the count (hud.c `hud_boss_health`: the 5,000 at
0 health), the Pause card its `boss_completed` tick once the count is met, and the Results the challenge line.
Checked: NY10 with the blimp beaten traces `challenge: NYBlimp counted, 1/1`, then `challenge met: 1/1` as the block
is won.
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
- `addPointWithVel(p, d, k)` is 0x33B010(mover, p, d, k), and `addPoint` the same with k = 0 (dispatcher 0x2A8050);
  0x13B660 keeps per point d (+0x18), T = k (+0x1C), B = C = 0. So k is the point's tension: its tangent is scaled
  by 1 - k. The Jumper's 0.05 leaves an almost ordinary tangent (0.95) [R].
- `clearPath` (0x33B0B0) frees the points (0x13BB40) and puts the place along the path (+0x48) back to 0 [R].

**LineMover** (as people.c; the update 0x315E10, flags +0x60): from point to point. Flag 1 faces the way it goes,
and flag 2 loops. Flag 4 (with flag 8 off) turns the heading into the next leg over each segment's second half
(t > +0x68 = 0.5): a slerp by w = s²(3 − 2s), s = (t − 0.5) / 0.5, towards the next segment's direction (after the
last, the first's with flag 2, else its own); the place stays on the straight segment [R]. The frame is the leg's
in 3D (0x316E60): X = |d × (0, 0, 1)| (none: (1, 0, 0), the up at 0x40A028), Y = |d|, Z = X × Y, as a quaternion at
+0x70 (0x105090; a leg with |d|² < 1e-6 the identity) put on the object's matrix (0x316A24, 0x104BB0), the next
leg's built the same way for flag 4's slerp (0x3164E8 .. 0x316590) [R]: so it pitches along the leg (Path_NYEmpire1's
z runs 42.5 .. 50.5, some 20° on one leg), as here. `initWithPath("Path_NYEmpire1", 15, 1)` (descriptor 7 →
0x3173D0) takes the path's points from the Objects1_Props group of that name (x y z triples), starts at segment 0,
t 0, and places the object at the first point at once [R]. With flag 1 off the update writes no turn (0x316A18 skips
0x104BB0), so the last leg's frame stays on the matrix: the knocked blimp and the one crossing the top keep their
last leg's pitch, and `setFacing` / `setFacingZ` change only their part (boss.c reads the facing back from the frame).

**Building finders** (World bindings, dispatcher 0x2AE3D0, table 0x42DD70) [R]:
- `findBuildingNearMonster(d)` (0x318240):
  - **The span:** the monsters' least and most x (0x318390). If it is under 20 wide, it becomes 30 wide around its
    middle. Then it is widened by 10, and by d (d < 0 narrows it).
  - **The pick:** from a random building, going a random way round the list, the first standing one whose middle x
    lies in (lo, hi].
- `findBuildingNearPosition(p, r)` (0x2B4D50): of the buildings whose box meets the box p ± r (z 0 … 1000), the one
  nearest along x.
- `getRandomBuilding` (0x2AEC40): from a random one on, the first standing one.
- `findBuildingByPosition` (0x2A3D20; the binding 0x2AEAA0 passes a zero direction): of every building in the
  world's list, the last whose box (shape box + place) holds the point in x and y, ends included; z and the state
  are not tested, so rubble still counts [R]. `b.isValid()` is only a handle check (0x2795E8), and `getSizeZ`
  (0x2AD818) reads the fixed storey count, so a flattened footprint keeps its full height for the path checks.
- The scripts' height of a building is always storeys × 3 + 1.5 + 6.

**Other natives** [R]:
- `gWorld.getPlayValue / setPlayValue` (World cases 61 / 62, 0x2AF108 / 0x2AF148): eight ints at 0x4D201C, indices 0
  … 7. Nothing clears them: they last the whole run. The generators write theirs (genId) and the blimp reads 0 and 1,
  and the airstrike keeps its bombers' height in 0 (0x332930, `FinalBomber`'s `getPlayValue(0)`). So after any
  airstrike, earlier or in NY10 itself, the blimp takes the first generator as gone and its barriers come down (main.c
  `play_values`).
- `numGenerators`, Boss_NY_Generator.bsf's global: a script's globals last as long as its bundle,
  NewYork_scripts, which NewYork's `onLoadScript` loads (`loadCityData`, 0x2B5000: `_buildings`, `_scripts`,
  `_props`) and `onShutdown` unloads. A continue plays NY10 again in the same world, so its generators count on
  (2 and 3) and theirs are not the values the blimp reads (main.c `ny_generators`, from 0 at `open_city`).
- `Monster.isClimbing` (0x2AAAD8): mode 5 (+0xAD8) and mounted (+0xC644). A monster only getting on (its mount
  prepared, +0xC640 set) is not climbing.
- `Building.isFalling` (0x2ADE10 → 0x2CE230): a storey of it coming down, G+0x1CC set (a floor dropping off a
  standing building too; not while it cracks or shakes, nor between its storeys). `isDestroyed` (0x2AD780): +0x14C 0,
  gone. `findBuildingNearMonster` (0x318320) and `getRandomBuilding` (0x2AECF8) take any building with +0x14C not 0,
  one cracking, shaking or falling too [R]. Up to v0.11.4 the port had isFalling and isDestroyed from the cracking
  on, so HK10's Leeper on a roof did not leap off as a storey went from under it, NY10's generator died and the back
  barriers dropped as the centre building began to crack (not as its first storey fell), and the Leeper never
  picked a building coming down.
- `Monster.playReaction(name)`: the move state of that name (0x2D9890), started through 0x2D9710.

**The AttackObject's oscillation** (vtable 0x44B470; its init 0x304FA0 → 0x315330, its update 0x314EF0) [R]:
- **The keys** (0x315810 .. 0x3159F0): +0x190 = `OscillationScale`, the template's (obj +0x80) float, else (0 or
  less) the instance book's (obj +0x7C); +0x18C = `OscillationOffset`, the template's, read twice: the second read
  (0x315934) is made only when there is an instance book, but from +0x80 again, so the instance's offset is never
  read; below 0 gWorld.frand × 2π (0x106BD0 on gWorld +0x828, × 2⁻³¹ × π × 2). Only float records count (the type
  byte is tested for 1). The constructor (0x315A90) zeroes both and the last wobble (+0x180). 0x315330 runs from the
  vtable's init (0x304FA0, f12 = 0: the spawner's and createAttackObject's objects) and from the block's load
  (0x3050F0: its second list, the `AttackObjectInstance|` entries, made from the pool 0x4D5EB0 by 0x283DA0 and given
  their instance group; the first list, `ObjectInstance|`, makes no AttackObjects).
- **Each frame**, after the world object's update (0x309AC0: the mover, the pulse, the script), while +0x190 > 0: the
  phase grows by the world's step (gWorld +0x4); the wobble is (sin 2φ, cos 3φ, 0) × the scale; the object's turn
  is taken apart into its facing (0x101220), gets the new wobble less the last one, and is made again (0x101330),
  then its matrix (vtable +0x24). So a facing the script leaves alone rocks by ± the scale (in radians) about x and
  y, and a mover's frame made afresh each frame only gets the frame's change.
- **Which:** of the bosses only `BossSaucerAmbient` has one (0.075: ±4.3°, the tilt and its rotor's, which copies the
  saucer's facing in its Process). The keys (0x3F5DB4, 0x3F5DB8) are read nowhere else, so the others that have them
  rock only as AttackObjects: the army's helicopters and news helicopters (0.1; the SplineMover with flag 2 makes
  their frame afresh each frame, 0x33AD58, so on their path they get only the frame's change, and once a monster is
  spotted, flag 2 off, the whole ±0.1), the PirateShip (0.1), and the StreetObject.bsf trees the city books place as
  `AttackObjectInstance|`: Tree1 (offset −1, the scale the instance's: 0.035 in Chicago, 0.01 elsewhere; 119 of
  them), PalmTreeShort / PalmTreeTall (0.02, offset −1; 92 in Las Vegas and Los Angeles). The same templates placed
  as `ObjectInstance|` do not rock.
- Ported, one code for all (army.h `ArmyOsc`: `army_osc_init`, `army_osc_step`, `army_osc_turn`, and
  `army_facing_turn` / `army_turn_facing` for 0x101330 / 0x101220): boss.c `obj_update` (the keys in `obj_new`; the
  facing carries it, a part whose turn is its mover's, `oriented`, gets the change on its matrix), army.c
  `unit_update` (the keys in `unit_new`), streetobj.c `step` (city.c marks the `AttackObjectInstance|` ones,
  `SceneStreetObj.attack`). The PirateShip has no module: LV05_JollyRogers places it as an `AttackObjectInstance|`
  and city.c draws it as a still model (army.c's StationaryAttackObject is never spawned), so it does not rock here.

**Sounds** [R]:
- `add3DSfx(name, keep)` (the binding 0x278810) is 0x287860(object, name, keep != 0): one of the object's 16 sound
  slots (2 bits each in +0x50: in use, keep), a slot whose sound has ended taken again (0x274CF0), none left: none.
  It starts the 3D sound through 0x26D560 with the slot's id and the object's own place (+0x2C) as its position, so
  the sound follows the object. A deleted object (0x290480 → 0x288050) kills the sounds of its slots without keep
  (0x274710) and leaves those with keep where they are. The bosses' scripts all pass keep 0.
- In the port each `BE_SOUND` is a `sound_add3d` moved with its part every frame (main.c `bsnd`), at most 16 a
  part, and killed with it (`BE_SOUND` level 0, and a new block). The other objects' add3DSfx go the same way
  through main.c `obj_sound` (the army's units, people and ActorSwarm, vehicles, street objects, the parade,
  edibles, the monsters' own clip sounds): at most 16 an object, moved with it each frame (`obj_sounds_step`),
  stopped by its remove3DSfx (the army's level 0, the people's `on` 0), and on its deletion killed without keep, left
  where it was with keep.
- **MovementSound** (`Tank Treads` on BossTank, BossSaucer and NYBlimp, `Copter Loop` on BossHelicopter) is never
  played for the bosses: the key (0x3F5D9C) is read only by the AttackMover's init (0x314B40, 0x314CE4) and the
  HelicopterMover's (0x31EE20, 0x31F094, which starts it at once through 0x287860), and by the scripts
  Vehicle_Attack.bsf and Vehicle_Helicopter.bsf. No boss has either mover (the SplineMover's init is empty,
  0x33AC90, and the LineMover's 0x317650 does not read it), the Tank moves itself, and no Boss_*.bsf asks for the
  key. What the bosses sound like is their scripts' own: `Cyclo_Hum_Loop` (the Saucer), `Cyclo_Wind_Loop` (the
  whirlwind), `Leeper_Hum_Loop`, `Leeper_Servo`, `Leeper_Jump`, `Tim_Impact_Test` (the Jumper), the voices.

**The turrets** [R]: a part's `WeaponCluster`s are army.c's (ARMY_NOTES.md "The turn is drawn"): the keys
(0x299640, `army_cluster_read`: the rates in radians a second, the ranges in degrees, RequiresRotation /
ElevationClearToFire, ReCenterWhenNoTarget, the control nodes); each frame the turn steps at the rates onto the target
lifted for the shot's fall, from the heading of the part's +y row, within the ends, on target within π/50 and π/100
(0x29ADE0, 0x29BA50, `army_cluster_turn`); the target point is the monster's (0x2DB030: its `Bip01 Spine1`'s x and y,
its feet + 6; `BossMonster.target`). The WWeaponProcAnimator (0x298980) draws the turn on the control nodes (city.c
`boss_sync` → `instance_turns`), so the firing frame, the WeaponNode as drawn (main.c `boss_node_world`, a frame
behind), carries it. Every boss cluster names its nodes, and every model has them:
- BossTank: `HT_Rotation` / `HT_Elevation`, the SudsGun's dome turned onto the monster (−10 … 90° at 2.5);
- BossHelicopter: `Helicopter_Base_Rotate` / `_Pitch`, which carry the whole helicopter (as the news helicopters'),
  ReCenterWhenNoTarget;
- BossSaucer: `RotationControl` / `ElevationControl` (31.45 a second: at once); the whirlwind's `Rotation` /
  `Elevation` are on an InvisShape, so its zaps leave from its place;
- NYBlimp: `Blimp_Rotate` / `Blimp_Elevate`, the gondola gun that hangs under the blimp and turns down onto the
  monster (0 … −70°); the BlimpBomb, in a cluster of its own, falls from `Blimp_Rotate` (its node is the BlimpBeam
  cluster's turned node, so a bomb leaves from the turned gun).

**Object flags** (0x3F5F70) [R]:
- 0x4000 `PushOutMonsters` (Tank, Jumper): a monster is pushed out of the part's box by the collision world
  (collide_monster, 0x2E2C50 [R]: the box query with include VolumeTrigger | PushOutMonsters).
- 0x8000 `OnlyHitWithEffNodes`: only attacks reach TakeDamage. It is on the Tank, Heli, Jumper, Saucer, Blimp and
  Generator.
- 0x40000 `IsObstacle` (Whirlwind).
- 8 (Whirlwind, the Saucer's rotor) has no name.
- 0x8000000 (Heli, ambients with `Animate`) has no name. It plays the model's own animation: the frame 0x309AC0
  (0x309CE0) advances the model's animation state by dt only with it [R]; without it the model holds its start.

The monster's update (0x2E22B0) tests its spheres against the objects within 20 (up to 64, flags 0x2040 skipped)
and calls TakeDamage(monster, amount, type): an armed sphere gives the attack's damage (+0x2C) with type 0, and an
object with 0x8000 goes on the attack record's hit list (0x2E33A0; a record per arming, its parts' own), so it
takes one blow a record (here each part keeps the records that hit it, `CollideHitList`). An unarmed sphere gives 2.0 (0x2E2680) with type 1 (Collision), every frame, and only to objects without
0x8000 [R]. That is how the Whirlwind catches a monster, how the Jumper's goo burns one, and how the wrecked Tank
shoves one.

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
- **Where:** on its timer it drives off to the side away from its monster; when that is left (the hit retreat), it
  turns to face −x at π/3 a second. A quarter crossed (and nearly always the killing blow) has set the hit retreat
  already, and the fleeing never clears it, so then it always leaves to the left, and it comes back still facing −x,
  rear first. It stops at x ±95.
- **Return:** it comes back after ReturnDuration (20 s), with LeaveDuration + 6 to fight before the next leave.

**Defeat** [R]:
- **At HP ≤ 0:** addStats type 2 (1500), `V_Boss_Defeated`, weapons off, 0x8000 cleared, the CompletionCallback.
- **Then:** it drives off and is deleted past ±95.
- **On the way:** a monster on the street that touches it (on its leading side, within 3) is shoved at (0, 15, 0).
- **City destroyed:** if the city is destroyed, it is deleted the next time it is off screen.

### BossHelicopter ("Scumhawk", Boss_Helicopter)
**The template:** HitPoints 3, Velocity 25, MaxMissiles 2, FiringDelay 3. Its weapons are `SmartMissile0` (15
damage, homing, ReactionLevel 1) and `SmartMissile1` (0.5 damage), both from `Weapon_0`. A boss's shots pass its own body, as a shot's owner is no hit (0x293CD8) [R]: the weapons are the army's, run with the part's collision tag as their owner (`army_weapon_tick`'s `owner_tag`); up to v0.11.1 they had none, and a missile fired from inside the helicopter's own box could end there at once, at its muzzle. `Scumhawk_Engine` plays on
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
(`forceDamage`, `BE_FORCE_DAMAGE`). That is RoofItem.bsf's forceDamage, streetobj.c's `streetobj_force_damage`
(STREETOBJ_NOTES.md): nFlags 2, 99 hits, TakeDamage(none, 1000, 0), so the ornament falls off its roof (the fall
latency, then down 6 a second, turning) with no score. Checked in HK10: about 17 s in the Leeper lands on the palace
and the top tower `HKPalaceTopAdorn2` (HitRadius 9) tips off and falls to the street; the side ornaments (HitRadius
5) stay.

**Leaving** [R]:
- **When:** its LeaveDelay runs down twice a frame while it is not in the air, so 30 counts as 15 s.
- **Where:** it jumps out to (0, −30, 75) and waits there for ReturnDuration (15 s), then jumps back in.

**Damage** [R]:
- Only attacks of DamageType 0 hurt it. A monster's attack is type 0 and a touch type 1 (0x2E25F4) [R].
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
    (0x335BF0); with DoPushout it is pushed out along the least overlap (x and y only), unless climbing.
    `initByObject` (0x2A6710 case 0x2C) gives the trigger's init (0x335ED0) the group alone, so Center is in the
    world, and the box is −Extents … +Extents about it [R]. Its CanFireFunc (+0xA8) is not ported: NewYork's
    `BlimpBarrierCanFire` always answers 1.
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

## The engine's side, as read
- **Collision** [R]: a monster's touch and an armed attack sphere are each of its spheres against the part's
  collision shape (0x2E22B0 → 0x280470, collide.c `collide_obj_sphere`; `sphere_meets`). The shapes are 0x28FD70's
  (`collide_shape_pick`: CollisionBox (the generators'), CollisionRadius or AutoCollisionBox (the blimp) with the
  model's box, else the sphere the model gives (its vtable +0x14, at 0x290384): SimpleShape's 0x184CB0, the union of
  its parts' spheres (0x185AA0)); here the union of its meshes' header spheres, city.c `city_shape_sphere`. The
  damage type is 0 for an armed sphere, 1 for an unarmed touch (0x2E25F4).
- **The world's step** [R]: the game runs at 30 steps a second (FLOW_NOTES.md), and what the scripts do "a frame" is
  a step: the Tank's × 0.9 braking, the ambient saucer's × 0.95, the whirlwind's pulse at `getTimeStep()` (10 % of
  the gap to its monster a step) and its π/8 turn a touch. boss.c runs each frame with that frame's dt, so these are
  taken as so much a 1/30 s step (`powf(k, dt / TIMESTEP)`, the pulse at `TIMESTEP`), and the monsters' touches
  (`contacts`) are tested once a step, from the frame's time left over (at most 4 a frame).
- **Weapons** [R]:
  - `setWeaponAmmo(n, cluster, weapon)` is the weapon's vtable +0x84 (0x296F50, through 0x29AC90 / 0x298D20):
    ammo = min(n, Ammo); `reloadWeapon(cluster, weapon)` its +0x80 (0x296F70): ammo = Ammo, the countdown at least
    ChargeTime + FiringOffset (the charge effect restarted: not here);
  - `setWeaponFlags` / `clearWeaponFlags(-1, f)` (0x29AB60 / 0x29AAE0) set or clear f in every cluster's flags
    (+0x38). The flag names (0x3F3EA0): 1 RequiresElevClearToFire, 2 RequiresRotClearToFire, 4 DeactivateReload,
    8 WeaponsDisabled, 16 ReCenterWhenNoTarget, 32 AdjustWeaponNTP, 64 FireOnceCommited. So Boss_Helicopter's
    `setWeaponFlags(-1, 16)` (createFlightPath, createFleePath) and `clearWeaponFlags(-1, 16)` (createKnockedAwayPath)
    switch its ReCenterWhenNoTarget: on its paths it levels out without a target, while tumbling it keeps its turn;
  - `enableWeapons(on)` (id 0x38, 0x3151C0) is the AttackObject's +0x17C, set when its weapon system is made
    (0x3157DC): the weapons are on from the start (the helicopter and the blimp never call it). The system (the
    turn, the countdowns, the fire) runs only while it is set (0x315170); turned off, each weapon stops (0x29AA40 →
    0x298B90 → the weapon's vtable +0x7C; a beam's 0x295650 ends the beam and the charge and holds the countdown at
    ChargeTime + FiringOffset), so the beaten blimp's beam goes out and the leaving Carbonator's gun stops turning.
- **Odds and ends** [R]:
  - `adjustZRot(a)` (id 0x36, 0x2AA4E0): the turn becomes Rz(a) × the turn (0x1015B0, 0x101A90, row vectors), a turn
    about the part's own z after its facing (`Obj.zrot`, the rotors);
  - `getFacing()` (WorldObject id 9, 0x277E10) is read back from the matrix (0x101220), so its z is in (−π, π];
  - the facing the oscillation takes apart (0x101220) is `army_turn_facing`'s asin / atan2, read for the mover-turned
    parts only: the others carry their facing as the script set it, plus the wobble;
  - `findBuildingNearPosition` (0x2B4D50) tests every building of the world's list, fallen or not;
  - a key with a leading dot (".Path0.NumPoints") is cut at its dots by the book's reader (0x1B43C0), others are
    looked up whole; a typed key is found by its bare name (0x1B4300).

## Not traced (stand-ins in boss.c)
- **TakeDamage's direction** (the blimp's knock follows it): here from the monster to the part; the game's is
  0x280470's third result (0x2E2598), made 1 long (0x2E2654), not followed into the shape's test.
- **An invalid monster's place** reads as 0: the WorldObject cases test the handle and then write nothing
  (0x277E30), so the script reads what its Point3 held.
- **Never reached with the shipped data:** Boss_Helicopter without paths (the script asserts; SF10 has 4), and its
  knock's `dir.z` doubling loop at exactly 0, which would never end (a hang); here 5.
- **Sounds:** a `--shot` run moves no sound on (`sound_update(0)`), so there a part's 16 slots fill up and later
  sounds are refused (the trace says so); in play an ended sound frees its slot, as 0x274CF0 does.

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
   - `--do "bosshit n"` (main.c) is one blow of n on the block's boss by the monster, as an armed attack sphere at
     its middle with a record of its own: `wait 8, bosshit, wait 20, bosshit, wait 12, bosshit` beats the NY blimp.
2. **Each frame:** `boss_collide_add` into the frame's collision world (between `collide_begin` and
   `collide_finish`, before anything moves), each touch of a kind-1 object (a trigger) that collide_monster reports
   to `boss_trigger_touch`; `boss_update(dt, monsters, buildings)`, then draw each alive part: `shape` at `world` ×
   `scale`; `animate`: its own RawAnim; the Jumper: `clip` at `anim_time`. Then apply the events (`BE_PUSH` no
   longer comes: the pushes are collide_monster's; `BE_SOUND` level 1 starts a 3D sound that follows part `obj`,
   level 0 kills that part's).
3. **Monster attacks:** an armed attack sphere calls `boss_hit`.
4. **The city's end:** `boss_set_city_destroyed(1)` once the city's last block is finished.
