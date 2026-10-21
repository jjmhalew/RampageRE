# MONSTER_DATA: the monster's move graph

Every monster bundle (`<Monster>.cmp`) has a raw entry (type 2) `MONSTER_DATA`, 231 KB for George: how the monster
moves, what each pad input does in each state, its collision shapes, attachment nodes and effects. It is the
compiled form of the monster's move graph: states (each with a clip), transitions between them guarded by
conditions (pad inputs, analog values) and optional script callbacks (`Monsters::canMount`, `Monsters::hasRoarEnergy`
in `Monsters.bsf`, plus per-monster ones such as `George::doGrabPowerup`). `tools/monster.py` parses it
(`--verify`: all 30 monsters, every table where the previous one ends) and prints the graph.

## Loading
0x2F3A50 finds the entry by name, refuses any version but 11 and calls 0x2F3C10, which fixes it up in place:
name indices (into the bundle's name table, which holds every clip, state, callback, sound and effect name) become
string pointers, offsets become pointers. State nodes are found by name through a hash table (hash % 97, 0x2E8DB0).

## Layout
Offsets from the start of the entry. The tables follow each other from 0xAA8 in this order (George's offsets).

| Header | Table | George | |
|---|---|---|---|
| 0x000 | | 11 | version |
| 0x004..0x128 | | 74 × i32 | event slots: the start list (+0xA5C) of each event, the defaults for every mode |
| 0x12C..0x134 | | 3, 15, 30 (all 30) | f32 × 3: TakeDamage's reaction level by the amount (0x2D9680): below the first 4 (none), below the second 0, the third 1, else 2 |
| 0x138 | | 200 | f32 the most HP: 0x2D9374 clamps a hit to it, addStats (0x2C51A0) and the respawn (0x2D3930) fill to it. 200 for most; Amanda and Wally 250, Jack 180, Jill 170, Marco 150, Snapper 320 [R] |
| 0x13C..0x9C0 | | | not decoded (0x13C .. 0x168: twelve f32 1.0, as many as the DamageTypes, read by nothing [R]: no load at those offsets and no address built from +0x13C in the monster's code; the loads elsewhere at such offsets are other structs' (the effects, the camera, the foot objects from 0x33D820 on)) |
| 0x9C4..0xA2C | | | name indices (−1 = none), settings (f32 0x9F4..0xA10) |
| 0xA30, 0xA34, 0xA38 | | | names of the skeleton, the mesh and the cloth model |
| 0xA44 / 0xA48 | collision spheres | 11 × 24 at 0xAA8 | f32[3] centre in the bone's frame, f32 (read by none of the code that loads +0xA48: 0x2DA9E0, 0x2D48BC, 0x2D7F60, 0x2E24EC, 0x2E39FC, 0x2E3CAC take the centre, the radius, the bone or the part [R]), f32 radius, i16 bone, i16 body part; 0x2DA9E0 places them: bone matrix, the monster's scale (+0xA78), its matrix |
| 0xA4C / 0xA50 | state nodes | 186 × 0x84 at 0xBB0 | the clip and how it plays, see "State nodes" |
| 0xA60 / 0xA68 | coded item offsets | 1,325 × u16 at 0x6B98 | into the block at +0xA64 |
| 0xA64 | clip events | at 0x75F4 | {u32 type, f32 time, ...}, see "Clip events" |
| 0xA6C / 0xA70 / 0xA74 | body parts | 6 at 0xF678, pointers at 0xF748 | `Arm L`, `Arm R`, `Body`, `Head`, `Leg L`, `Leg R`: {name, u32, u32, u16 count at +0xC, i16 bones from +0xE} (George's arms bones 28 … 37 and 38 … 47) |
| 0xA58 / 0xA5C | start lists | 29 × 12 at 0xF760 | u32 ?, offset of a state, u32 flags (8: keep a time) |
| 0xA94 / 0xA98 | slot columns | 6 × 0x128 at 0xF8BC | 74 slots each overriding the defaults (−1 = keep), see "Slots" |
| 0xA78 / 0xA7C / 0xA80 | effect slot nodes | 2 at 0xFFAC, pointers at 0x1022C | each {u32 count, offset} then 0x34 bytes a node: f32[12] matrix (rows, then the place), i16 bone, i16 the slot of the other side. +0x9C8 picks the set (0x2F3950; −1 none): 1 in all 30, its 11 nodes (Cal, Eyegore, Jill, Natalie, Venus 12); set 0 is one node. The clip events of type 12 put their effects on them ("Effect slots") |
| 0xA84 / 0xA88 | attachment nodes | 15 × 0x38 at 0x10234 | f32[12] matrix, name (`fxNode_Belch`, `Helper L Hand`, `Tracking_Node`, ...), bone \| index << 16 |
| 0xA8C / 0xA90 | effect names | 12 at 0x1057C | `Monster_Belch.edf`, `Debris_mega_punch.edf`, ... |
| 0xA54 | the move graph | header 0x34 bytes at 0x105AC | +0x2C state count, +0x30 offset of the first state |
| | states | 176 from 0x105E0 | see below |
| | | states² bytes at the end | a state × state byte matrix (30,976 = 176² for George; 177² for Marco, 179² for Rojo) |
| 0xAA4 | | | name of the monster's property book (`George_prop.prb`: hand and foot bones, foot IK, effects, weapons; the book is `George_prop.bdp`). The setup 0x2E98C0 reads its `MiscData` (`IsFemale` to the monster's +0xAB6, which `Monster.isFemale`, script method 0x96 at 0x2AC0E0, returns; `Use3LinkFootIK`, `FootIKDragHeightScale` to +0xAC4, `RTWSmoothFactor`, `DisableFootDrag`, `LiftNodeName`, `MonsterFx`). Seven books serve the 30: Amanda (Amanda, Snapper), Jill (Jill, Natalie, Venus) and Lizzie are female; Cal, George, Ralph, Rhett are not |

A state: 0x2C bytes, then 0x74 bytes per transition.

| State | | Transition | |
|---|---|---|---|
| +0x00 | index | +0x04 | offset of the target state |
| +0x04 | offset of its state node | +0x08 | flags, see below |
| +0x08 | f32 clip length (s) | +0x0C, +0x10 | f32 input window: from, to (s into the state) |
| +0x0E | i16 transition count | +0x14, +0x18 | f32 switch window |
| +0x10 | the "clip ended" transition: | +0x1C | i16 priority |
| +0x14 | its target, the state that follows | +0x1E | i16 condition count |
| +0x18 | its flags, ... | +0x20 | 4 × 16-byte conditions {u32 op, u32 variable, u32 value, u32 flags} |
| +0x28 | offset of the transitions | +0x60 | name of a script check (−1 = none), +0x64..0x70 four floats for it |

A transition's +0x00 (and the "clip ended" one's, at the state's +0x10) says how the next clip comes in: 0 it cuts,
1 it blends in (see "Blending"); 0x2ECBE0 puts it in the clip request's flags (<< 4). All 30 monsters: 32,338
transitions blend and 270 cut; of George's clip ends, 29 of the 30 that loop into their own state cut and 136 of the
other 146 blend.

Transition flags: 0x10 the transition is open to a pad-controlled monster, 0x8 to the other kind (0x2FF750 decides
which applies and returns 0, the pad, in this build; nearly all transitions have both, 0x18); 0x1000 switch at once
(otherwise the switch waits a little); 0x1 the next clip keeps the phase (it starts at the same fraction of its
length); 0x100 and 0x200 are set at run time (in the input / switch window); 0x200 is also stored set on some of
the switched-off `false == 0` transitions (`Ground_TurnAround`, `Reactions`; see "Conditions").

## State nodes
0x84 bytes each; the runner hands the node on with the next clip (0x2ECBE0) and the monster's command (0x2E6580)
plays it:

| Node | | |
|---|---|---|
| +0x00 | f32 rate | the speed of the clip's 0 .. 1 clock (0x2FFF10 clamps it to 1): the state lasts 1 / rate, and is at its clip's duration × time × rate (`Ground_Idle`: 0.15, its 6.67 s `SKELETON_1_IDLE_3` once through; most rates are about 1 / their clip's length) |
| +0x04 | f32 turn | with flag 0x8, how fast it turns towards the stick (6 on the ground, 1.5 in the air, 12 in the flip) |
| +0x08 | f32 speed | with the air flags, the speed the stick drives it at (16 in a jump, 1 on the ground) |
| +0x14 | f32 blend | seconds to blend into the clip (0x2FFA00, +0x4E58 of the animation player and 1 / it) |
| +0x18, +0x1C | f32 x, y | its forward axis in its own frame for the turn (0, 1; x negated when mirrored) |
| +0x28 | u32 | its clip events: first << 16 \| count, see "Clip events" |
| +0x2C | u32 flags | below |
| +0x78, +0x7C | | clip name, state name |

Flags (+0x2C), as the command and the monster update (0x2DE530 ...) test them:

| Flag | |
|---|---|
| 0x2, 0x10, 0x1000 | the stick drives the monster along its own z, x, y axis ("Steering"); the jumps have 0x1012 or 0x1010 |
| 0x4 | the camera frame the stick is read in is kept until the stick moves (otherwise taken every frame, 0x2E0738); climbing |
| 0x8 | it turns towards the stick (0x2E8190); `Ground_Walk` (0x288), carrying (`Ground_Hold_Walk`), the flip |
| 0x200 | the right stick turns its head (0x2FBE30, with the settings at +0xA08..+0xA10 of MONSTER_DATA) |
| 0x40, 0x80, 0x80000 | the root's travel along x, y, z moves the monster ("Root motion"); `Ground_Walk` and the punches y, the kick and the landings x and y, the climbs z |
| 0x100 | the root's turn about Z turns the monster (`Ground_TurnAround` only) |
| 0x40000 | when the next clip starts, the root's place and turn in this clip become the monster's (0x2E6580, before it starts the next): the monster moves and turns so that the next clip's root, where it starts, is where this one's ended (the climbing clips hold the root turned 80° from the standing ones, so it is not the root's change over the clip); the climbs, mounts, knock-downs |
| 0x80002 | either: the friction on its velocity (+0xF4 of the monster × dt, against the velocity's direction, 0x2DFDE8) takes z too, not only x and y |

## Conditions
A condition is `variable OP value`, OP 0 `>`, 1 `>=`, 2 `<`, 3 `<=`, 4 `==` (the function tables at 0x3F5950 for
floats and 0x3F5970 for booleans; the variable is the left operand). Flags bit 0: test the value as it is; clear:
the input must be new, latched this frame (0x2C1A60, 0x2C17C0), so a button press counts once. `tools/monster.py`
prints them as `Square==1(new) L.up==1`.

The variables are a table per player (0x4D3330 + player × 0x3C4, +0x18; 16 bytes each from +0x14), filled each
frame by 0x2C0AD0 from the pad record (gp−0x6FA8 + player × 0x5C) that the pad code (0x1A5220, after scePadRead)
fills through the button table at 0x3B18B0:

| Id | | Id | |
|---|---|---|---|
| 0, 1 | left stick x, y (y down) | 12, 13 | R1, L1 |
| 2, 3 | right stick x, y | 14, 15, 16, 17 | Cross, Circle, Square, Triangle |
| 4, 5, 6, 7 | d-pad up, down, left, right | 20, 21 | R3, L3 |
| 8, 9 | Start, Select | 22 … 29 | pressures (28 R2, 29 L2) |
| 10, 11 | R2, L2 | 30, 31 | right, left stick size: max(\|x\|, \|y\|) |
| 32 … 47 | the left stick in the monster's own frame, its size where it points, else 0 (0x2C03E0; see below) | 48 … 55 | the left stick's eighth: up, down, left, up-left, down-left, right, up-right, down-right (0x2C0870: (x, −y) turned by π/8, none under 0.2) |
| 56, 57 | true, false | | |

The stick in the monster's frame (0x2C0AD0 → 0x2C03E0, bits of the controller's +0x3B4, variable = 30 + bit): the
stick in the world as the monster reads it (0x2E8410, see "Steering") against the monster's right (a) and forward
(b) axes. 32 … 35 the quadrant by the larger of the two: front, left, right, back; 36 … 39 the halves: front, back,
right, left; 40 … 47 the eighth, the same tests with the stick turned by π / 8 first: front, front-right,
front-left, right, left, back, back-right, back-left. None below a stick size of 0.01. `tools/monster.py` names
them `M.front`, `M.front8`, ...; mirrored, the condition reader swaps 33 / 34, 41 / 42, 43 / 44 and 46 / 47.

Ids 4 … 21 and 48 … 57 are booleans, the rest floats. The stick sizes (30, 31) always count as new and 32 … 47 while
they are set (0x2C17C0), so `|L|>0.7(new)` means only "pushed past 0.7". When the monster is mirrored (+0x7DA1 of the monster) the
left / right pairs swap (50 ↔ 53, 51 ↔ 54, 52 ↔ 55, and pairs of the sectors). Variable 57 reads 0 and is never new
(0x2C1838), so a `false == 0` (new) or `false > 0` condition never holds: those transitions are switched off. The
game's own events (landing, hits) come in through the slots instead ("Slots"), and nothing found fires
`Ground_TurnAround` for a pad-controlled monster: it turns with the stick ("Steering").

George's graph reads naturally: Cross jumps (released early, the short hop `Jump_Start_2_copy`), Square punches
(with the stick up or down, `Ground_Punch_Up` / `_Down`), Triangle kicks, Circle grabs, R2 roars
(`Monsters::hasRoarEnergy`), a left stick size above 0 walks and 0 stops; climbing a building needs the stick held
past 0.8 towards it and `Monsters::canMount`.

All 30 monsters share the graph's shape (176 states and 1,085 transitions; Marco 177, Rojo 179) and differ in
clips, settings and coded items.

## Clip events
Each state node owns a run of the items at +0xA64 (+0x28: first << 16 | count), the timed events of its clip, in time
order. Each starts {u32 type, f32 time}, the time a share of the clip's length; 0x2E7840 handles them (a switch on the
type, table 0x4312F0):

| Type | | |
|---|---|---|
| 21 | {name, f32 p[4]} | a script function of `Monsters.bsf` (or the monster's own, `George::EnableWeapon`) with four arguments, via 0x2E75C0: footsteps, `addVelocity`, `jumpNorm`, `damageRoof`, `fireWeapon`, ... |
| 0 | {i16 part[4], u16 count, u32 flags, ...}, 0x80 bytes | an attack: the bones of the listed body parts (`Arm R` for a punch, `Leg R` for the kick, both arms for `Ground_Punch_Down`) take this record as theirs (+0xBD60 of the monster, 0x1C per bone, through the mirror map at +0xC4D0 when mirrored); flags 7 arms them, 0 disarms them, so an attack is a pair: on, then off, and a collision sphere on an armed bone can hit (George's punch: Arm R's two spheres for 0.07 s; starting a clip disarms all, 0x2E7F70). The flags (+0x14): 2 a victim in front may be hit, 4 one behind (either arms it); after them u32 +0x18 the victims (0xF, 0x1F: bits 0 .. 3 the monsters' slots, 4 the objects), +0x1C a side (1 or 2, swapped when the attacker plays mirrored; 0 in all), +0x20 the reaction's kind (1 the street's, 3 from above, 4 from below, 0 and 2 by the side; 535, 60 and 90 of the 685 armed records, none 0 or 2), +0x24 the ReactionLevel, +0x28 the DamageType (4 Blunt, 11 Unresistable), f32 +0x2C the damage (5 a punch, 20 the kick, 1 … 26), +0x30 the knock's distance (0, 6 … 16) and +0x34 its time (0.2 … 0.8), +0x38 the lift of a blow that flies, an angle in radians whose tangent times the speed is the z (0, 1 or 1.8), +0x3C the knock's turn about z (0), +0x40 .. +0x48 the spin's axis (1, 0, 0) and +0x4C its turns (0), +0x50 .. +0x58 the attacker's follow-up slots (−1), +0x64 / +0x68 the victim's slot in front / behind on the street and +0x6C / +0x70 in the air (74 none; 8 and 17, React_Ground_Knockdowns / _Back, in George's Ground_Punch_3 and Ground_Punch_Up and their like): monster against monster 0x2E35D0 → 0x2D6A50 (main.c `monsters_fight`, look.c `monster_blow`, MULTIPLAYER_NOTES.md), the objects through 0x2D4390 (the army's and the bosses' the damage) [R]; +0x5C (−1), +0x60 (10) are not traced |
| 6 | {u16, u16, name} | a sound: +0xA set, a world sound (0x26B960), otherwise one of the monster's own (+0x8, 0x287860) |
| 7 | {mode} | the movement mode (+0xAD8 of the monster): 0 on the ground, 2 taking off, 1 in the air (`isAirborne`: 1 or 2), 4 down or landing, 5 on a building |
| 8 | | the queued mirror state flips (+0xAA1, taken at the next clip's start); the climbs flip at each hand, so one clip climbs with both |
| 9 | {f32} | a camera shake of that strength (0x2F5AC0 → 0x1E7ED0: the monster's own `camshake.edf`; CAMERA.md "Shake"); 3 or 4 on the heavy punches and the butt slam |
| 12 | {i16 slot, i16 effect, u16 on, u16 ground} | an effect of MONSTER_DATA's list (+0xA90) starts (1) or stops (0) in that slot, on the slot's node (0x2E7E90 → 0x2F5800, "Effect slots"): `Monster_Belch.edf` on the burp, `Charged_Punch.edf` on the charge punch, `Collapse_Floor_concrete_comp.edf` on the butt slam; ground (the effect kept on the ground, FollowEffect +0xF0, 0x2918D0) is 0 in all 862 |
| 23 | {column} | the slot column (+0xBAB4), see "Slots" |
| 15, 16 | {..., f32 +0x10} | the record is kept at +0xBA9C / +0xBAA0 till the next clip (on a building, −5.5; leaving it, −5 … −6.25): 16's +0x10 adds to MONSTER_DATA +0x1A4 in 0x2E4E40 (the bots' corner rule) and 0x2E4D30; 15 (+0xBA9C) is read by 0x2E4E90 (the bots' reference point: the place + MONSTER_DATA +0x190 / +0x194 and its +0xC / +0x10 along the x / y axes, x the scale; 0 / 2.5 ahead for most) and 0x2E4FA0 (the bots' radius: MONSTER_DATA +0x188, 3 in all 30, + its +0x8), which the bot walk's "in the way" test 0x2EDF1C keeps apart (main.c bot_views, director.c); 15 on George's front, side, spider and React_ clips, 16 on the Transition_Ground clips and Spider_Jump |
| 1, 3, 4, 5, 10, 11, 13, 14, 17, 19, 20, 22 | | handled (1 a list of four at +0xBA8C that scale a blow's damage and distance by DamageType and side, 19 +0xBAA8 the distance's scale, 20 +0xBAAC a blow's block (0x2D6A50); disarming shapes, a throw, +0xAA2, +0xAA8, +0xBD00, ...) but used by no monster |

Of the 38,002 events of all 30 monsters: 16,879 script calls, 7,612 sounds, 4,607 modes, 3,751 columns, 2,431 of
type 15, 1,370 attacks, 862 effects, 310 flips, 90 shakes, 90 of type 16.

`tools/monster.py` prints them under each state (`0.136 Monsters::addVelocity(1, 0, 0, 16); 0.136 mode 1`).

### Effect slots
The monster's effect record (monster +0x7DFC, FOOTSTEP_NOTES.md) begins with 32 slots of {FollowEffect, its serial}
(+0x0 .. +0xFF); +0x100 holds the definitions of the list +0xA90 (loaded by 0x2F5D80). A type-12 event (0x2F5800) [R]:
1. Whatever slot k holds, if it still lives (the FollowEffect's serial +0x4 is the slot's), shuts down: its effect's
   vtable +0x30 (ParticleSystemFx 0x1F1C60 → 0x1F2410: each emitter's shutdown 0x1F7CD0; the live particles finish),
   and the slot is emptied. Off events stop there.
2. On: the node is 0x2DACA0(monster, k): node k of the set +0xA80[+0x9C8], or, while the monster plays mirrored
   (+0x7DA1), the node its +0x32 names (George's slot 3 on bone 40, the right hand, takes node 4 on bone 30). Its
   place is scaled by the dynamic scale (+0xA78) when +0xA74 is set. 0x292010 → 0x292050 → 0x291A20 makes a
   FollowEffect on the monster with that bone and that matrix as its offset; it follows the bone each frame
   (0x290F60 → 0x291440: the offset × the object's node, vtable +0x4C) and keeps the bone it started on. A missing
   definition leaves the slot empty. The new FollowEffect gets +0x4C |= 0x80 (0x2F5A28) [G: no code tests +0x4C
   against 0x80 and no query's mask found holds it (the shots' 4, 0x2000, 0x2040: 0x2953E0, 0x33B770); only a mask
   from a script could].
A state posted to start shuts them all down: 0x2DA570 (from setReaction, playReaction's 0x2D9710, the blows
0x2D6A50 .. 0x2D89E0, the landing 0x2DC450, the knock-offs 0x2E176C / 0x2E185C / 0x2E19EC and resetMonsterState, wherever
+0x110 is set) calls 0x2F56E0 on the record, which stops each live slot's effect (vtable +0x30) and empties the slot
[R]. It also stops the kept sound (+0xC5E0), throws what is held unless asked to keep it, zeroes the target offset
+0xBD54, lets both feet go with foot IK (0x33BE90), and clears a mount only prepared (+0xC640 set, +0xC644 not) with
its hands (0x322EC0): main.c `let_go`. No clip's own start, death or respawn touches the slots; the record's release
(0x2F5AF0, from the Monster's destructor 0x2EACA0) frees the definitions, not the slots. A one-shot ends by itself; a
looping one (`Charged_Punch`, `Roar_Weapon`) burns until its slot's next event or such a state, past its clip when that
is cut short (`Front_Charge_Punch` turns it
off at 0.592, but the switch to `Front_Double_Punch` at 1.7 s comes first: there `Debris_mega_punch` in the same slot
ends it).

The face clips' events come through the same handler (the face's event player, LOOK_NOTES.md): the belch and the
spew are type-12 events of `Expression_Burp` and `Expression_Spew`.

George's slots: 0, 1, 2 and 7 on `Bip01 Head` (0 the mouth, `fxNode_Belch`'s place: `Monster_Belch`, `Roar_Weapon`;
1 `Fire_small`, 2 `Monster_Cough_smoke`, 7 `Hazard_Electricity_Explode`), 3 / 4 the right / left fist (2.66 along
`Bip01 R Forearm` / `L Forearm`: `Charged_Punch`, `Debris_mega_punch`, `Debris_generic`, the butt slam's
`Collapse_Floor_concrete_comp`), 5 / 9 the shoulders (`Bip01 Neck`, 0.93 to each side: the fall's
`Collapse_Floor_concrete_comp`), 6 `Bip01` (`Monster_soar_butt`), 8 / 10 the right / left foot (2.55 along the calf:
the mantle's `footfall_Big`). Of the 862 events of the 30 monsters 583 are on and 279 off (180 `Charged_Punch`, 90
`Roar_Weapon`, 9 `Debris_megapunch_part2`); the rest are one-shots that no off follows.

The port: moves.c reads the nodes (`MoveGraph.fxnode`, `moves_fx_node`) and the events (the effect's name, the slot,
on); main.c `mon_fx_event` starts and stops them in `Mon.fx_slot` (fx.c `fx_spawn_frame`, `fx_stop`) and
`mon_fx_follow` places them each frame (`fx_place`) on the bone as drawn (a mirrored monster's bone is the other
side's node reflected, `scene_monster_node`). A new monster in the slot stops the old one's, for the viewer's
monster swap; in the game the old Monster goes and its FollowEffects lose their object, keep their own last matrix
(0x290F60 → 0x291440) and run on: they would shut down only with +0xF4 bit 8 (0x291238), which 0x291A20 never sets
[R].

The monster's event player (+0 the monster, +0x4 the node, +0x8 the time, +0xC the next event, +0x10 the count)
starts with the clip (0x2F45D0, from the monster's command): at the clip's first event, the time where the clip
starts, and it fires at once, in order, every event at or before that time, so a state entered part-way (its phase
kept) still gets its set-up (mode, column). Each frame (0x2F4500, from the update) the time grows by the node's rate
× dt × the monster's speed and the events it reaches fire.

## Movement
A jump is the clip's events: `Jump_Start_1` (George) sets mode 2 at 0.102, then at 0.136 adds 16 up
(`addVelocity(relative, x, y, z)`) and mode 1, and from 0.203 to 0.542 adds 3 or 4 up eight times, while the graph
keeps it in that state only as long as Cross is held (released, it goes to `Jump_Start_2_copy`): the longer the
press, the higher the jump. `Monsters.bsf`'s `jumpNorm` / `jumpLow` / `jumpDrop` / `jumpHighMonster` set the velocity
outright (22, 15, −10, 32 up; the x argument mirrored with the monster). The velocity is at +0xBD30 (world; the
relative calls turn it by the monster's matrix, 0x2E7330 / 0x2E74F0). Each frame the monster update (vtable +0x10,
0x2DE530 and on) moves the monster by velocity × dt and then takes 60 × dt off the upward velocity, gravity, unless
`setDisableGravity` (+0xAB8) is set.

## Collision
The monster's box against the world (0x2E2C50, from the update): around its position, MONSTER_DATA +0x198 each way
across (3 for George, scaled by +0xA78) and +0x19C each way up from a centre that high (4.25: the box is 8.5 tall),
3 more each way while on a building (mode 5); the world (0x4D0900, vtable +0x30, mask 0x404000, up to 64) returns
what it touches, and 0x2801A0 works out the push for each.

## Mirroring
A monster plays a clip mirrored (left for right, across its own x) when its queued mirror state (+0xAA1) is set as
the clip starts (0x2E67E8 copies it to +0xAA0, and 0x2FFA00 to the animation player's +0x72B9, the monster's
+0x7DA1). Clip events of type 8 flip the queued state, `Monsters::setMirrorState` sets it, `queueMirrorState` /
`getMirrorState` are the script bindings. The animation player mirrors the pose after evaluating it (0x194BA0): it
swaps the skeleton's mirror pairs (skeleton +0x34 pair count, +0x38 table) and turns each bone's rotation and
translation over, with constant quaternions (0x3B1790 … 0x3B17E0) for the root bones whose axes are not the
model's; a mirrored root's x is negated (0x194A60), so root motion and turns go the other way. The steering negates
the forward axis's x (0x2E8190), conditions with flag 2 swap left and right (0x2C15F8), the attack shapes go through
a mirror map (+0xC4D0), and `Monsters.bsf` negates its x velocities (`jumpNorm`: `if (monster.getMirrorState())`).

## Steering
The stick is read in the camera's frame: x along the camera's right axis, −y along its forward axis, the height
dropped (0x2E8410), using the camera's matrix (gp−0x6CFC +0x660) the monster copies to +0xA18 every frame unless the
state has flag 0x4. Both of the following are in the monster's command (0x2E6580) for the state it is in:

- Flag 0x8, turning (0x2E8190): the stick's direction in the monster's own frame against the state's forward axis
  (+0x18) gives an angle (acos of the dot product); if it is more than turn × dt (node +0x4), the monster turns by
  the angle × turn × dt towards it this frame (+0xA14, a turn about Z the update applies to its matrix), so it swings
  round fast and settles gently, and within one frame's step it does not turn at all. Walking turns this way, at 6:
  pushing the stick back turns the monster round while it walks on.
- Flags 0x10 / 0x1000 / 0x2, the stick's own velocity (+0xA68): the stick's direction × the state's speed (+0x8),
  kept to the monster's x / y / z axes the flags allow, is the goal; each axis moves towards it by 50 × dt (gp−0x722C)
  while speeding up, 100 × dt (gp−0x7230) while slowing down (a state without the flags slows it to 0), and the whole
  is kept to the speed. The update adds it × dt to the monster's x and y in modes 0, 1, 2 and 4 (0x2E0C50), on top
  of the velocity (+0xBD30, `getVelocity` is their sum, 0x2E7460): it is how a jump is steered in the air, at up to 16.

## Root motion
The animation player takes the root's motion out of the pose (0x301080, every frame and at a clip's start): against
the root as it was when the clip started (+0x29E4 rotation, +0x29F4 place), the travel along x, y and z goes to the
monster only for the axes the node's flags name (0x40, 0x80, 0x80000; the others are 0), and the turn only with
0x100 (0x301400, otherwise none). What is not taken stays in the pose: an idle's sway, a jump's crouch, the walk's
side-to-side (a clip entered with its phase kept moves that reference, 0x2FFCC4, not traced). George's 186 nodes:
125 take nothing, 14 y, 9 x and y, 3 x, 14 z, the turn-around its turn; 20 more bake (0x40000), 8 of them with z
and one with y.

## Climbing
The Monster script bindings `canMount` (0x2DDA20), `canClimb` (0x2DD100), `prepareToMount` (0x2D6450),
`mountBuilding` (0x2D63D0), `transitionToMount` (0x2D50C0), `unmountBuilding` (0x2D6380), `changeMountRail`
(0x2D4FF0), `transitionFromBuildingTop` (0x2D50A0), `canTransfer` / `transferMount` (0x2DD8B0, 0x2DD680); the
`Monsters.bsf` checks wrap them (`canMount` = 1, `canHop` = 3, `canSwing` = 4, `canMantle` = 5, `stopClimbUp` /
`stopClimbDown` ≠ 2, `shouldUnmountDown` = −1, `hitElectricSign` = −2; the hop and swing go towards the monster's
right, its left when mirrored). The monster's mount (+0xC628 … +0xC646, +0xC778): the building, a point on its face,
the rail and the storey, whether it is mounted, whether the point came from the roof's edge, whether the rail changed
in this clip, whether it is coming down from the top.

- **Rails**: points 5 apart along the front's plane, x = min x + 5 × rail, y = min y; rail 0 is the left side at the
  front-left corner, rail = width the right side at the front-right corner, the ones between the front.
- **canMount(side, z offset)** (side 0 left, 1 right, 2 front, 5 the roof's edge) looks from the monster's position
  plus MONSTER_DATA +0x1A0 to its right and +0x1A4 ahead (George: 2.5 ahead) for a standing building whose face is
  just there: the front within 1.5 × the box's half-width (4.5) ahead, a side within 1.1 × (5.5 when mounted), the
  roof's edge within 4.5 behind the front edge at the roof's height; the nearest such point (storey by the monster's
  height) wins if the face goes on 11.5 above the monster, else −7. The place is then checked (0x2DDC40, the box
  ± 4.1, 4.1, 4.5) [R], in this order: −5 another building in the way (a neighbour flush with the side), −8 the
  building falling, −6 a front cell the box covers flagged 0x1000 (0x2DDF60 .. 0x2DE0C4: the cell's u16 +0xE, from
  the storey's `FloorFlags` list, 0x2CC0C0; every city's lists hold only 0 or 1 and no script sets a cell's flags to
  it, so this never happens), −2 one of that building's adornments with `AdornmentFlags` bit 2 and not knocked off
  (0x10) that the box reaches (0x2DE120: the adornment's box, its two corners through its matrix), then the objects
  of the grid query (mask 0x40040) the box meets: one flagged 0x40 is a monster (−3: on a face by its mount point,
  +0xC634, otherwise by its place, and that one only while this monster is off a face too; within 8.2 across and 9
  up or down of the point), any other (flag 0x40000) an obstacle when its shape meets the box (0x2801A0: −2), −9 out
  of the camera's reach (not asked of an AI, +0x108). A template's `Flags` with `IsObstacle` sets 0x40000 (0x335ED0),
  as the electric side signs (`DefElectricSideSignProps`) have, so climbing into one is `hitElectricSign` (−2) and
  the clip `React_Side_L_Zap`, whose `faceAnim` plays `Expression_Zap`. climb.c `place_ok`; the other monsters'
  grid objects as the box query (0x282D70) sees a WCISpheres: half sizes r, r, r (its bound's radius, 0x29A918 ..
  0x29A924) turned by |M| (0x101C20) about the bound's centre (main.c `climber`, `mon_shape`) [R]; the query's
  sphere test before the box (0x282EE4) is not made [G: it would need the query's sphere in climb.c; only with more
  than one monster].
- **Getting on**: `prepareToMount` takes the point canMount chose, `mountBuilding` mounts it (and
  `transitionToMount(t)` slides the monster onto it over t seconds, root motion still moving the start).
- **On the face** (each frame in mode 5, 0x2D5870, after the root motion): the monster faces the wall (+y at the
  front, +x on the left side, −x on the right) and stands at its point, its height the clip's (the climbs move it by
  root motion in z, flag 0x80000) kept between the building's foot and its top − 11.5; a clip that bakes is free
  (with 0x120000 at the point, 0x100000 at the top − 11.5), one with 0x400 too until its rail changes. Getting on, it
  is only lined up with the rail. The mount's height is the monster's + 7.5, taken before the clamp (0x2E14F0), so a
  stroke past the limit lets canClimb answer 5. The monster is not put there at once: each frame its matrix goes 0.4
  of the way to the place 0x2D5870 gives (0x2E0D44 .. 0x2E0EE4: the translation by a lerp, the rotation by 0x167DE0),
  so a new place (getting on, coming down off the roof once `transitionFromBuildingTop` puts it at the point) is
  eased into over a few frames [R]. A frame of the monster's update is a world step, 1/30 s (FLOW_NOTES.md): main.c
  takes 1 − 0.6^(30 dt) of the way a drawn frame (taken as 0.4 a drawn frame, a 60 Hz screen eased in twice as fast). The place is re-checked every frame: failing it, the monster is knocked off (slot
  31, or 73 when the building falls).
- **canClimb(dv, dh)**: −4 not mounted. Sideways: −7 off the building, −5 a building close by, else the place 5 along
  is checked; 4 when it turns a corner (to or from a side), 3 a hop along the front. Up: 2 while the mount's height
  + 4 is not above the top, else 5 (mantle) if there is room on the roof. Down: −1 when the feet reach the foot, else 2.
- **Transfers**: `canTransfer(right)` (0x2DD8B0; mirrored the other way) takes the building of the same row (+0x10C,
  the block's `Row`) next along that way (0x2A41F0), standing at least 4 above the mount: from the front onto its
  front's first rail (1 going right, width − 1 going left), from a side onto its facing side (rail 0 going right from
  the right side, width going left from the left side; never back across the building). The new point must be 9.75 …
  10.25 from the mount along the ground, so only fronts that touch, or sides across a 10-unit alley; it keeps the
  height and the storey, and is checked as canMount's is. `transferMount` (0x2DD680), an event of the transfer clips
  (Front_Transition_Hop_Right, flags 0x440: its root carries the monster the 10 units; Spider_Transition_Side_R),
  moves the mount onto it.
- **Spider climbing**: on a side across a 10-unit alley, `canSpiderTransfer` (canTransfer towards the alley, always
  right when mirrored) lets the monster brace itself between the two buildings (Side_L_Transition_To_Spider, then
  Spider_Idle; still mounted on the first). In Spider_Climb_Down, Spider_Climb_Up and Spider_Idle (the names at
  0x3F5938) 0x2DD710 finds the building across the alley, and both the height kept (0x2D5870) and canSpiderClimb's top
  are the lower of the two; Spider_Transition_Side_R transfers to the other side (and flips, so the side idle plays
  mirrored, facing the other way). canSpider asks for 6, which canClimb never answers.
- **The mirror of a hop or swing** [R]: canHop, canSwing and canTransfer test the way of `getMirrorState`, the clip
  now playing (+0xAA0, 0x2AB808), while the clip they lead to starts in the queued state (+0xAA1, 0x2E67E8), and
  changeMountRail goes by that one. The climbs flip the queued state halfway through each cycle (Front_Climb_Up at
  0.405 and 0.929, Front_Climb_Down at 0.17 and 0.787) and let the swing and the hop go at any time, so a swing taken
  in the second half of a cycle at the face's end rail is tested towards the corner and played the other way: the
  rail steps along the front and the side clips play there (Side_L_Idle on a front rail). Nothing in the game
  guards the player's monster (no code stores +0xAA0 or +0xAA1 but the clip start, the flip event, queueMirrorState,
  attachHand's flip and the spawn); the AI tests both bytes before it hops (0x2EF704 .. 0x2EF714). Kept as the game has it.
- **The camera's reach** (−9, 0x336230): for a player, the place may be at most 70 high and 30 along x from the middle
  of the players' x (0x336360, kept at gp − 0x6C70; one player: its own x).

## The face
`setFacialAnim(name, head layer, looping, plays)` (binding 0x2E9200, 0x2E9350) looks the name up among the state
nodes (a hash of their names at monster +0x124): George's nodes 0 .. 11 are `Expression_Barf` … `Expression_Zap`, each
playing a `George_Head_*` clip at the rate 1 / its duration with a blend of 0.2. Its clip runs as a second layer of
the animation player (0x2FFF10) on its own 0 .. 1 clock and replaces the body clip, rotation and place, on the bones
of a mask built at spawn (0x2EA458): the subtree under `Bip01 Head` (George: 8 .. 27, the eyes, lips, cheeks, brows
and jaw), the head itself only with the head layer; for `Eyegore`, `Cal`, `Venus`, `Natilie` and `Jill`'s skeletons
the subtree three nodes above the head, and their brows and eyes by name. At spawn `Monsters::PostInit` starts
`DefaultFacialAnim` (`Expression_Idle`, looping, no head layer); a face clip that ends plays again while it loops or
has plays left (0x2E95D0), else the next queued one (`queueFacialAnim`), else the default again. So the face bones
never show the body clips' own: the shared `SKELETON_1_*` clips turn George's eyes 112 … 176° from his bind pose,
the face clips keep them within 20°. `Monsters::faceAnim(expression, head layer, -, plays)`, a clip event, picks the
name by number (1 Burp, 2 Choke, 3 Yuck, 5 Flex, 8 Spew, 9 Zap, 10 Chew, 11 Smile, 12 Roar, 13 Chomp, 15 Barf, 18
`React_Intro`, 19 `React_Victory`, 20 `Expression_Intro`) and never loops. There is no eye tracking or blinking; a
head animator (`MonsterHeadAnimator`, vtable 0x44B020) turns the head towards a target.

## Slots
The game's own events (hits, landing, rampage) start states through the 74 slots: slot k is a start list index at
+0x4 + 4k, or, when the monster's column (+0xBAB4) is 0 … 5, that column's override if not −1. The code puts the
start list entry at +0x110 of the monster, and the controller (0x2C0AD0) switches to its state next frame
(0x2EBF50). The columns are set by the type-23 events of the state the monster is in: −1 the default, 0 butt slam,
1 drop, 2 on a building's front, 3 on its side, 4 knocked down, 5 hit in the air. Starting any state's clip (Monster
vtable +0x60, 0x2E6580) first resets the column to −1, gravity to on, the movement mode to 0 (so each clip's own
events set the mode it is in), the attacks and the event records (0x2E7F70).

The landing test [R] (the mode-1 case of 0x2E2770, 0x2E29A4 .. 0x2E2A9C; 0x2E2770 runs in the world's pass,
0x2B6D20, after the monsters' own updates with their movement and box test): z is the monster's place (+0x34) after
this frame's move, the ground 0x2E8530's (the roof, +0xD4, of the building 0x2D6150 finds under the body centre: the
last whose footprint holds it, 0x2A3D20, kept while its top, +0x1F0, is no more than 5 above the feet, none at 0 HP;
else in a water its floor; else the street). The body centre (0x2E4D30) is the feet + MONSTER_DATA +0x1A0 plus the
kept type-16 record's +0xC along the monster's x and +0x1A4 plus the record's +0x10 along its y (George: 2.5 ahead),
times the dynamic scale. More than 3 up, the body is
taken as 3 higher (the jump clips hold George's feet 2.5 … 4.4 above his place): it lands when z + 3 ≤ the ground,
and is put at the ground − 3; at 3 or below, when z ≤ the ground, put on it. There is no velocity in the test, and
no landing while that building's state (+0x14C) is 4 or more (cracking, shaking, falling, 0x2E2A2C): it falls on
through. The matrix goes back with the new z (vtable +0x24), then 0x2DC450.

The movement by mode [R] (0x2DEBB0's switch, table 0x431290): modes 0, 2 and 4 the ground's (0x2DF6F0: the friction,
then the ground follow 0x2E0330: z = 0.3 × ground + 0.7 × z a frame, a world step, so 0.7^(30 dt) of the gap kept
a drawn frame here (look.c), the ground within 0.001), 1 the air's
(0x2DEBD8: drag, gravity), 3 0x2DF300, 5 on a face (0x2E03A8). Taking off (mode 2, `Jump_Start_1` from 0.102 to the
velocity event) is thus on the ground, and the world's pass has no landing test for it (0x2E2B28). Nothing in modes
0, 2 and 4 makes the monster fall: +0xAD8 is written only by the clips' mode events (0x2E7DE4), their velocity events
(0x2DB130 sets 1), a clip's start (0 at 0x2E8028) and the landing (4 at 0x2DC5DC). So the follow lifts the landing's
3 back over the next frames while the landing clip crouches (the feet stay about on the roof; on the street the 3
never shows: z + 3 ≤ 0 cannot hold above 3), lifts the monster onto a roof no more than 5 above it where its body
centre goes over one, and eases it down onto a lower roof, or onto the street, where its body centre leaves one (off a
free edge the box test's roof clamp, 0x2A49C0, holds the body centre on the roof). Here main.c `mon_step`
(`ground_under`; the box test of the landing frame sees the monster still in the air, as the game's did,
`Mon.landed`).

Landing (0x2DC450) sets mode 4 and fires slot 56, or 57 when the ground's normal points against the monster's up
axis: for George `Jump_Finish` by default, `Jump_Butt_Slam_impact` in the butt slam's column, `Jump_Fall_Finish2`
when knocked down. Hits pick their slot by the victim's mode (the table 0x431270, from 0x2D8078), the blow's kind
(+0x20, or for a shot the velocity's z after its knock), its ReactionLevel and the side it came from (look.c `monster_blow`, army.c
`army_reaction_slot`). That is why
`Jump_Air_Borne_Descend` has no landing transition: the landing comes from the slot.

## At run time
A runner per monster (`src/moves.c` is its port): +0 the state, +0x4 the next one, +0x8 the transition taken,
+0x10 the time, +0x18 the monster, +0x1E the transition picked.

1. 0x2EBFE0: the time advances by dt × the monster's speed (+0xA8C of the monster). A transition gets 0x100 while
   the time is in its input window [from, to) or jumped over it this frame, 0x200 likewise for its switch window.
2. 0x2EC3D0 (monsters with +0x108 set run 0x2E1AA0 instead): of the transitions with 0x100 and 0x10 whose
   target is not disabled (0x80000000 in the target node's +0x2C), skipping any of lower priority than the one
   already picked, the last one whose conditions all hold and whose script check (if any) says yes is picked.
3. A picked transition that has 0x200 is taken: the next state is its target (0x1000 at once, otherwise after a
   short wait counted at +0x14). With none, a state whose time passed its length takes its "clip ended" transition. A pick whose switch
   window has passed is dropped; windows that passed are cleared.
4. 0x2ECBE0 starts the next clip: at 0, or with flag 0x1 at the same fraction of its length (capped at its end);
   looping into the same state carries the time over (time − length). The request it fills (per player, 0x100
   bytes) holds the new node (+0x0), its blend time (+0x4), the start time (+0x8), the sticks × 255 (+0x1C..+0x28)
   and flags (+0x2C: the transition's blend mode << 4, 0x1 keep the phase); the monster's command (0x2E6580) plays it.

## Blending
0x2FFA00 starts a clip in the monster's animation player: with blend mode 0, or 1 while nothing plays yet, it cuts;
otherwise the two pose buffers swap (+0x29C0 / +0x29C4), so the pose the monster had stays as it was, and the blend
time (the new state's node +0x14) is set, with 1 / it (+0x4E58, +0x4E5C). Each frame (0x2FFF10) the time left falls
by dt × the monster's speed; the root's motion is taken out of the new clip's pose first (0x301080: x with node flag
0x40, y with 0x80, z with 0x80000, the turn with 0x100, against the clip's root where it started, +0x29E4 / +0x29F4;
the root rewritten at 0x3003C8), so the kept pose has none either; the new pose is mirrored (0x194BA0) before the
blend, so the kept one, as it was drawn, needs no turning when a clip starts on the other side; and a bake (0x2E6580)
writes the new clip's root where it starts into the root of both buffers (0x300FE0), so the root does not blend from
where the old clip had it (the monster was just moved so that the new root stands there). Every bone of the new clip's pose goes towards the kept one by the share left
(time left / blend time, so from 1 to 0 linearly): the translation by a lerp, the rotation (turned to the kept one's
side first) by 0x104590. A start the game forces through a slot takes no transition (0x2EBF50 clears the runner's
+0x8); the slot's start (0x2C0DC0 .. 0x2C0E78) fills the request itself: the start list entry's state node, at 0
(the entry's flag 8 would keep the phase: no monster's lists have it), its blend time, and blend mode 1 when that time
is over 0.001, else 0 (a cut) [R].
