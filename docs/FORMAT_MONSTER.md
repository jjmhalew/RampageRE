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
| 0x12C..0x9C0 | | | not decoded |
| 0x9C4..0xA2C | | | name indices (−1 = none), settings (f32 0x9F4..0xA10) |
| 0xA30, 0xA34, 0xA38 | | | names of the skeleton, the mesh and the cloth model |
| 0xA44 / 0xA48 | collision spheres | 11 × 24 at 0xAA8 | f32[3] centre in the bone's frame, f32 (not traced), f32 radius, i16 bone, i16 body part; 0x2DA9E0 places them: bone matrix, the monster's scale (+0xA78), its matrix |
| 0xA4C / 0xA50 | state nodes | 186 × 0x84 at 0xBB0 | the clip and how it plays, see "State nodes" |
| 0xA60 / 0xA68 | coded item offsets | 1,325 × u16 at 0x6B98 | into the block at +0xA64 |
| 0xA64 | clip events | at 0x75F4 | {u32 type, f32 time, ...}, see "Clip events" |
| 0xA6C / 0xA70 / 0xA74 | body parts | 6 at 0xF678, pointers at 0xF748 | `Arm L`, `Arm R`, `Body`, `Head`, `Leg L`, `Leg R`: {name, u32, u32, u16 count at +0xC, i16 bones from +0xE} (George's arms bones 28 … 37 and 38 … 47) |
| 0xA58 / 0xA5C | start lists | 29 × 12 at 0xF760 | u32 ?, offset of a state, u32 flags (8: keep a time) |
| 0xA94 / 0xA98 | slot columns | 6 × 0x128 at 0xF8BC | 74 slots each overriding the defaults (−1 = keep), see "Slots" |
| 0xA78 / 0xA7C / 0xA80 | | 2 at 0xFFAC, pointers at 0x1022C | |
| 0xA84 / 0xA88 | attachment nodes | 15 × 0x38 at 0x10234 | f32[12] matrix, name (`fxNode_Belch`, `Helper L Hand`, `Tracking_Node`, ...), bone \| index << 16 |
| 0xA8C / 0xA90 | effect names | 12 at 0x1057C | `Monster_Belch.edf`, `Debris_mega_punch.edf`, ... |
| 0xA54 | the move graph | header 0x34 bytes at 0x105AC | +0x2C state count, +0x30 offset of the first state |
| | states | 176 from 0x105E0 | see below |
| | | states² bytes at the end | a state × state byte matrix (30,976 = 176² for George; 177² for Marco, 179² for Rojo) |
| 0xAA4 | | | name of the monster's property book (`George_prop.prb`: hand and foot bones, foot IK, effects, weapons) |

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
| +0x00 | f32 rate | the clip plays at this speed: the state's length is the clip's duration / rate (all 176 of George's states; `Ground_Idle` plays its 1 s clip at 0.15, 6.67 s) |
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
| 0x40000 | when the next clip starts, the root's place and turn in this clip become the monster's (0x2E6580, before it starts the next); the climbs, mounts, knock-downs |
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
| 32 … 47 | the left stick's sector in the monster's own frame (0x2C03E0; two sets of eight bits, the stick size where set) | 48 … 55 | the left stick's eighth: up, down, left, up-left, down-left, right, up-right, down-right (0x2C0870: (x, −y) turned by π/8, none under 0.2) |
| 56, 57 | true, false | | |

Ids 4 … 21 and 48 … 57 are booleans, the rest floats. When the monster is mirrored (+0x7DA1 of the monster) the
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
| 0 | {i16 part[4], u16 count, u32 flags, ...}, 0x80 bytes | an attack: the bones of the listed body parts (`Arm R` for a punch, `Leg R` for the kick, both arms for `Ground_Punch_Down`) take this record as theirs (+0xBD60 of the monster, 0x1C per bone, through the mirror map at +0xC4D0 when mirrored); flags 7 arms them, 0 disarms them, so an attack is a pair: on, then off, and a collision sphere on an armed bone can hit (George's punch: Arm R's two spheres for 0.07 s; starting a clip disarms all, 0x2E7F70). After the flags: u32 +0x18 (0xF, 0x1F), +0x20 (1 or 3), +0x24, +0x28 (4), f32 +0x2C (5 a punch, 20 the kick, 10 or 20), +0x30 (15, 10), +0x34 (0.2 … 0.4), +0x38 (0 or 1), +0x40 (1); their use (0x2D4390 tests the shapes) is not traced |
| 6 | {u16, u16, name} | a sound: +0xA set, a world sound (0x26B960), otherwise one of the monster's own (+0x8, 0x287860) |
| 7 | {mode} | the movement mode (+0xAD8 of the monster): 0 on the ground, 2 taking off, 1 in the air (`isAirborne`: 1 or 2), 4 down or landing, 5 on a building |
| 8 | | the queued mirror state flips (+0xAA1, taken at the next clip's start); the climbs flip at each hand, so one clip climbs with both |
| 9 | {f32} | a camera shake of that strength in a random direction (0x2F5AC0 → 0x1E7ED0, the camera-shake effect's own start); 3 or 4 on the heavy punches and the butt slam |
| 12 | {u16 slot, u16 effect, u32 on} | an effect of MONSTER_DATA's list (+0xA90) starts (1) or stops (0) in that slot: `Monster_Belch.edf` on the burp, `Charged_Punch.edf` on the charge punch, `Collapse_Floor_concrete_comp.edf` on the butt slam |
| 23 | {column} | the slot column (+0xBAB4), see "Slots" |
| 15, 16 | {..., f32 +0x10} | the record is kept at +0xBA9C / +0xBAA0 (on a building, −5.5; leaving it, −5 … −6.25); not traced |
| 1, 3, 4, 5, 10, 11, 13, 14, 17, 19, 20, 22 | | handled (a list of four at +0xBA8C, disarming shapes, a throw, +0xAA2, +0xAA8, +0xBD00, ...) but used by no monster |

Of the 38,002 events of all 30 monsters: 16,879 script calls, 7,612 sounds, 4,607 modes, 3,751 columns, 2,431 of
type 15, 1,370 attacks, 862 effects, 310 flips, 90 shakes, 90 of type 16.

`tools/monster.py` prints them under each state (`0.136 Monsters::addVelocity(1, 0, 0, 16); 0.136 mode 1`).

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

## Slots
The game's own events (hits, landing, rampage) start states through the 74 slots: slot k is a start list index at
+0x4 + 4k, or, when the monster's column (+0xBAB4) is 0 … 5, that column's override if not −1. The code puts the
start list entry at +0x110 of the monster, and the controller (0x2C0AD0) switches to its state next frame
(0x2EBF50). The columns are set by the type-23 events of the state the monster is in: −1 the default, 0 butt slam,
1 drop, 2 on a building's front, 3 on its side, 4 knocked down, 5 hit in the air. Starting any state's clip (Monster
vtable +0x60, 0x2E6580) first resets the column to −1 and gravity to on (0x2E7F70).

Landing (0x2DC450) sets mode 4 and fires slot 56, or 57 when the ground's normal points against the monster's up
axis: for George `Jump_Finish` by default, `Jump_Butt_Slam_impact` in the butt slam's column, `Jump_Fall_Finish2`
when knocked down. Hits (0x2D8078) appear to pick their slot from the direction and strength of the blow. That is why
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
by dt × the monster's speed, and every bone of the new clip's pose goes towards the kept one by the share left
(time left / blend time, so from 1 to 0 linearly): the translation by a lerp, the rotation (turned to the kept one's
side first) by 0x104590. A start the game forces through a slot takes no transition (0x2EBF50 clears the runner's
+0x8); which way it comes in is not traced.
