# Footsteps, landings and the foot IK

The dust (or splash) under a monster's foot and the bigger one when it lands, and the foot lock and leg IK that hold
a planted foot on the ground, traced from the ELF; `src/footstep.c` ports them, `src/ik.c` bends the legs. [R] read
in the code, [G] guessed.

## The names (MonsterFx)
All seven property books (`Amanda_prop`, `Cal_prop`, `George_prop`, `Jill_prop`, `Lizzie_prop`, `Ralph_prop`,
`Rhett_prop`) give the same four, and the same `HandFootList` (`Bip01 L/R Hand`, `Bip01 L/R Foot`):

| Key | Name | Record +off | Started by |
|---|---|---|---|
| `GroundFootStepFx` | `footfall.edf` | +0x140 | footPlant, on the ground |
| `WaterFootStepFx` | `footfall_water.edf` | +0x144 | footPlant, in a water |
| `GroundImpactFx` | `footfall_big.edf` | +0x150 | the landing, on the ground |
| `WaterImpactFx` | `footfall_big_water.edf` | +0x154 | the landing, in a water |
| `RageFx` | `Rage.edf` | +0x164 | (the rage; not followed) |

The reader is 0x2F5D80 (the part from 0x2F6168) [R]: the book's `MonsterFx` group (0x1B0210 on the setup record's
+0xAD0), each key's name (the key table 0x3F5BC4 .. 0x3F5BD8, read with 0x1B4490) loaded as an effect definition
(0x198DC0, library 0x4861C0) and kept refcounted in the monster's effect record at **monster +0x7DFC**. A missing key
leaves NULL there (0x2F65D8 zeroes them) and every starter below returns at once on NULL. The same function loads
`camshake.edf` (+0x15C, its live effect +0x160) and the effect list of MONSTER_DATA +0xA8C / +0xA90.

The effects themselves carry the sounds (`enum.sound_2d`): `Rampage|Footfall` (footfall), `Rampage|Splash`
(footfall_water), `Rampage|Ground Impact` (both big ones) and `Rampage|Geyser` (footfall_big_water). All are
`+OneTimeEmission`: one puff per start.

## The script methods
`Monsters.bsf`'s `footPlant`, `footStep`, `cancelFootStep` (clip events of type 21, `(foot, 0, 0, 0)`) call the
Monster methods of the same names [R]:

| Method | Descriptor | Id | Dispatcher case | Calls |
|---|---|---|---|---|
| `footPlant` | 0x42BB40 | 0x87 | 0x2ABD68 | 0x33CCC0 |
| `footStep` | 0x42BB50 | 0x88 | 0x2ABD88 | 0x33CDA0 |
| `cancelFootStep` | 0x42BB60 | 0x89 | 0x2ABDA8 | 0x33CA10 |

(names matched in 0x2AC3B0 at 0x2AD254 .. 0x2AD2A4; the jump table is at 0x42CEE4, 4 bytes per id.) Each passes the
int argument and the monster's **foot object at +0xC81C**.

### The foot object (monster +0xC81C) [R]
Set up by 0x33D4E0 (from the monster setup 0x2E98C0 at 0x2EAB48):

| Off | |
|---|---|
| +0x00, +0x28 | two foot records of 0x28 bytes (below) |
| +0x50 | the ankle: added to the ground's height for a planted foot. −(row z of the inverse bind rotation · its translation) of the node at monster +0x7DAC (`Bip01 R Foot`) in the skeleton's bind pose (MONSTER_DATA +0xC8A0 → +0xA30 → +0x3C, 0x50 bytes a node, the matrix from +0x4; 0x174B20 transposes, 0x174A90 turns back): **the right foot's height in the bind pose**, George 0.528, Rhett 0.648, Ralph and Lizzie 0.514 (the bosses without feet: whatever node −1 reads) |
| +0x54, +0x58 | 0.5, 0.5: the share a planted foot keeps a frame of its target (+0x18) and of its IK share (+0x4) |
| +0x5C, +0x60 | 0.7, 0.7: the same for a foot that is not planted |
| +0x64 | `MiscData|RTWSmoothFactor` (0.7 until read; Lizzie and Ralph 0.85, a float key only): the share of the anchor's slip the root does not take up a frame |
| +0x68 | a byte: a footfall waiting for 0x33BF10 (see below); only ever cleared |
| +0x69 | `MiscData|DisableFootDrag` (an int key; Lizzie and Ralph 1) |
| +0x6C | the anchor: the planted foot the root follows, −1 none |
| +0x70 | the monster |
| +0x74 | **the state node the monster is leaving**: 0x33CCA0 is the first thing the state command 0x2E6580 does (with a new node), +0x74 = monster +0x10C, and +0x10C only takes the new node at 0x2E6848; the new clip's events (0x2F45D0 at 0x2E6858) come after |

A foot record (the records are indexed by the foot as drawn: 0 left, 1 right, see the mirror swap below):

| Off | |
|---|---|
| +0x0 | weight: 1 held (planted or dragged), 0 free |
| +0x4 | the IK's share, eased towards the weight (flag 0x10) |
| +0x8 | a dragged foot's arc height (0x33C5A0) |
| +0xC | the place (world): planted, where (z the ground + the ankle, every frame); else the foot as posed |
| +0x18 | the IK's target, eased towards +0xC (flag 4) |
| +0x24 | flags: 1 planted, 2 dragged, 4 +0x18 has started, 0x10 +0x4 has started (both set by 0x33CDE0 the first time, never cleared) |

The records are indexed by the foot **after the mirror swap**: when the monster is mirrored (+0x7DA1) each method
first turns the foot into (foot + 1) & 1.

### The bones [R]
The animation player's setup 0x301AE0 keeps the node named `Bip01 L Foot` (gp−0x71E8, the name table 0x44C970) at
player +0x72C0 (monster +0x7DA8), `Bip01 R Foot` (gp−0x71EC) at +0x72C4 (+0x7DAC), `Bip01 Spine1` (gp−0x71E4) at
+0x72C8 (+0x7DB0) and `Lift_Node` at +0x72CC (+0x7DB4); gp = 0x453B70 (set at 0x1001C0). `HandFootList` is not
how the feet are found: the setup reads it only to mark the collision shapes on those four nodes (+0xC618).

0x2E5C40(monster, foot, out): the node +0x7DA8 (foot 0) or +0x7DAC (otherwise), its model-space place (animation
player +0x30 + node × 0x30) times the dynamic scale +0xA78 (when +0xA74), through the monster's matrix (rows +0x8 ..,
place +0x2C): **the foot bone's place in the world**. With the swap, a clip's foot 0 is its left foot: drawn on the
left, or on the right when the monster plays mirrored. (In the viewer, which poses the clip unmirrored and reflects
the drawing, that is `scene_monster_node("Bip01 L Foot")` unmirrored and `"Bip01 R Foot"` mirrored: `foot_bone`.)

### footStep (0x33CDA0) [R]
The foot lifts: record +0x24 &= ~3, +0x0 = 0. Nothing else: no effect, the anchor stays.

### footPlant (0x33CCC0) [R]
1. Nothing while the debug byte 0x4D6A9A is set (only ever read: 0x2E2C84, 0x2E3620, 0x2EDC94, here, 0x33CA38).
2. Nothing unless the monster is still: |getVelocity|² < 1e−6 (0x2E7460: the velocity +0xBD30 plus the stick's own
   +0xA68). So a monster walking on its root motion (Ground_Walk, flags 0x288, has no stick speed) steps; one
   sliding from a knock, flying, or carried along by the stick does not.
3. p = the foot's place (0x2E5C40); p.z = the monster's ground (0x2E8530: cached once a frame at +0xA48: the roof's
   top (+0xD4) when it stands on a building (0x2D6150), the water's floor (0x337E00) in a water, the world's ground
   otherwise, all under its centre node (0x282B90), **not under the foot**).
4. Record 1 → 0x2DC350, record 0 → 0x2DC3D0: the water it stands in (0x2D5F00; its box against the waters'): with
   one, p = (p.x, p.y, the water's +0x34, its Origin z: the surface) and the water flag is set.
5. 0x2F4A20 (record 1) / 0x2F4B70 (record 0) on the effect record: the definition +0x144 (water) or +0x140
   (ground), none: return. The foot's own live effect (+0x14C for record 1, +0x148 for record 0) is made anew
   (0x208220 on the effect manager 0x48A340, the old one freed with 0x207A40) only when it holds another
   definition; then it is placed (vtable +0x8C, 0x1F2500: +0x50), given the frame at gp−0x70F0 (+0x90, 0x1F1D60: the
   identity 0x40A040, set by the static initializer 0x435D80 [R]) and restarted (+0x2C, 0x1F2260).

The record itself is not touched: footPlant only makes the footfall. No sound but the effect's, no camera shake, no
damage to people, vehicles or roofs (the roof stomp is the separate event `Monsters::damageRoof` →
`hitBuildingRoof("Bip01 R Foot", 1, 2)`, [FORMAT_BDP.md](FORMAT_BDP.md) "The roof stomp"), no scale (the monster's size moves the foot, not the puff), and the water
is not pressed (the wading does that every frame on its own, WATER_NOTES.md).

### cancelFootStep (0x33CA10) [R]
Every standing clip sends it for both feet at its start (Ground_Idle, the reactions, the jumps' starts ...), but
**only foot 0 does anything** (`bnez $s2` on the argument before the swap). Then, unless the debug byte, with a state
node at +0x74 — **the state just left** — whose flags have 0x50 or 0x1080 (NODE_AIR_X | NODE_ROOT_X, NODE_AIR_Y |
NODE_ROOT_Y: a state that travels; Ground_Walk has 0x80) and the monster still:
- s2 = record 0 (1 mirrored), s1 the other; both feet's places, less the monster's place, into its frame (0x101190:
  the dots with its axes); if s2's y (forward) is less than s1's they swap: **s2 is the foot further forward**;
- s2: weight 1, flags |= 1 (planted), its place (0x2E5C40), z = the ground (0x2E8530) + the ankle (+0x50);
- s1: with DisableFootDrag weight 0 (its flags kept); otherwise weight 1, flags |= 2, 0x33C5A0 sets the drag up,
  flags &= ~1;
- +0x6C = s2.

No effect. So: **coming to a stop** (a walk into an idle, a reaction ...), the forward foot is planted where it stands
and becomes the anchor, the other one is dragged into the new clip's stance. A standing clip entered from a standing
state plants nothing (Ground_Idle's own flags are 0).

**The drag (0x33C5A0, s3 the dragged record, s2 the planted)**: the planted foot as the clip now started poses it
**at its time 0** (0x33C290 mirrored, else the clip's own query, animation player +0x2A24 — the clip 0x2FFA00 started
— vtable +0x34, f12 = 0: the clip's own time, as 0x2FFA00 and 0x2FFF10 hand its +0x14 / +0x48 the player's +0x2A28),
through the monster's matrix, is short of where it was just planted by d (x, y; z 0); the dragged foot as the clip
poses it, through the matrix moved by d, is a distance (x, y) from where it is now (0x2E5C40). 0.333 (0x3EAA7EFA)
of that, at most 3 × the ankle (worked out the first time any monster drags and kept for good: gp−0x6C68, flag
gp−0x6C64), × `FootIKDragHeightScale` (monster +0xAC4: 1 until read; Lizzie and Ralph 0.5), is the arc's height
(record +0x8). So the arc scales with how far the foot will be dragged.

### The clip query (+0x2A24 vtable +0x34, 0x33C290) [R]
The clip object a state node holds (+0x78, handed to 0x2FFA00 at 0x2E6820) is a `KeyInterpAnim` (vtable 0x444C30) or
`RawAnim` (0x444DF0); their +0x34 is the base `NodeTreeAnim`'s 0x196D20: the node's own key (+0x14: node, time,
out: a rotation at +0x0, a place at +0x10) and then each parent's (the skeleton's +0x2C table, +0x4 the parent, up to
−1) put before it (0x196F30 the rotations, 0x196E70 + 0x1859F0 the place). So it is **the clip's keys alone, the root
as the clip has it**: the motion a monster takes over (0x301080) is taken out only in the animation player's own
frame, not here. 0x33C290 (mirrored) does the same from its own key array of the requested node and its parents
(the others left at the identity), mirrored first by 0x194BA0 (with `Lift_Node`, monster +0x7DB4), and composes from
the node of the other side (0x33C418 .. 0x33C424): the node as the mirrored pose draws it. Both the drag and the
foot lock ask at time 0.

## The foot lock and the foot IK

### The frame [R]
The monster update (vtable +0x10, 0x2DE530) runs the clip events (0x2F46D0, 0x2DE96C / 0x2DE9B0: cancelFootStep
among them), the animation player's frame (0x2FFF10 at 0x2DE9BC: the clocks, the blend, the pose), then the mode's
case (jump table 0x431290: 0x2DF6F0, 0x2DEBD8, 0x2DF300, 0x2E03A8) and the shared tail from 0x2E0738 (every case
branches there). In the tail: the frame's new matrix is made (the move, the turn, 0x2E0E40 ..), **the foot lock**
(0x33BF10 at 0x2E0EFC) may move its place, 0x2DCCE0 sets it, the arm IK (+0x2588: 0x323130, vtable +0x8 at 0x2E1310),
then **the foot object's update** (0x33D450 at 0x2E1324) and **the foot IK** (+0x258C, vtable +0x8 at 0x2E1338). So
at cancelFootStep the pose array still holds last frame's pose, IK and all; at 0x33D450 it holds this frame's pose,
before the IK.

### The foot lock (0x33BF10: a1 the frame's new matrix, a2 the matrix out, 1 = take it) [R]
Nothing under the debug byte. With an anchor: if the +0x68 byte is set, a footfall at the anchor's record place (z the
ground; record 1 → 0x2DC350, else 0x2DC3D0) and the byte cleared. **No code sets +0x68** (every `sb` to +0x68 /
+0xC884 in the monster and foot code stores zero), so this footfall never happens: the footfalls are footPlant's
alone. Then the anchor is dropped (−1) **when the state now (monster +0x10C) travels** (0x50 / 0x1080: 0x33BFE8 ..
0x33C000 branch to the drop at 0x33C018) **or its record is no longer planted** (footStep). Otherwise the anchor's
foot as the clip alone poses it **at its time 0** (0x33C290 mirrored, else the clip query above on node +0x72C0 /
+0x72C4, swapped when mirrored; f12 = 0 at 0x33C04C / 0x33C080; the place at +0x10 of the result, 0x33C098),
through the new matrix (rows +0x0 / +0xC / +0x18, place +0x24), against the record's place, (x, y): within 1e−6
squared (0x358637BE) the anchor is dropped and nothing moves; else the matrix is copied out with its place moved by
**(1 − RTWSmoothFactor) of the slip**, its rotation kept, and 1 returned. The root eases after the planted foot until
the clip's first frame stands on it; the IK holds the foot meanwhile. Walking into Ground_Idle (George, 0.7) that is
the half stride: the walk's forward foot, 2.56 ahead of the place, against the idle's 0.27, so the monster eases 2.3
forward over its planted foot and stays there.

### The foot object's update (0x33D450 = 0x33CF90 for 0 and 1, then 0x33D490 = 0x33CDE0 for 0 and 1) [R]
Run only when the monster has a foot IK. 0x33CF90, per record:
- once flag 0x10: +0x4 = weight × (1 − k) + +0x4 × k, k = +0x58 planted, else +0x60; then 0 within 0.001 of 0
  (0x3A83126F) and 1 within 0.001 of 1;
- planted: z = the ground (0x2E8530) + the ankle; the leg's first node (0x2F8F50: chain record, link 0, the thigh)
  through the monster's matrix; a place farther from it than the leg's length (0x2F9060: the links' lengths as posed)
  is brought back onto that sphere;
- else the place is the foot as posed (0x2E5C40, record 0 node +0x7DA8, 1 +0x7DAC: as drawn); dragged: once the
  player's blend is over (+0x4E58, the time left of the clip blend 0x2FFA00 set, ≤ 0) the drag ends (flags &= ~2,
  weight 0), else z is raised to at least ground + ankle + sin(π (1 − left × +0x4E5C)) × the arc (+0x8; +0x4E5C is
  1 / the blend time): **the dragged foot follows its clip, lifted on an arc over the blend**;
- once flag 4: +0x18 = place + k (+0x18 − place), k = +0x54 planted, else +0x5C.

0x33CDE0, per record (also run by the state command, 0x2E67DC): the first time +0x4 = weight (flags |= 0x10) and
+0x18 = the place (flags |= 4); the target is +0x18 in the monster's frame (0x101190) with the rotation of the posed
foot (animation player +0xC + node × 0x30, 0x24 bytes), handed to the IK's chain of that record (0x2F8FF0: chain
+0x18; 0x2F8FE0: the share +0x4 to chain +0x48).

### The foot IK (monster +0x258C = animation player +0x1AA4) [R]
Made by 0x2E5640 from the player's setup 0x301AE0 (0x302458) **only when `Bip01 L Foot` and `Bip01 R Foot` both exist**
(+0x72C0 ≠ +0x72C4; the five bosses Cal, Eyegore, Jill, Natalie, Venus have no feet) and the book has `MiscData`
(no group: NULL, no foot IK, no 0x33D450). `MiscData|Use3LinkFootIK` (an int) ≠ 0: the 3-link (0x2F7D70, 0xB0
bytes, vtable 0x44AF60); 0 or absent: the 2-link, which is **StdIKPairAnimator**, the arms' class (0x2F8ED0, vtable
0x44AFA0). 0x2F9720 (vtable +0x1C, with the two foot nodes) points it at the player's pose (+0xAE8, the pose at +0xC)
and sets chains 0 and 1 up (vtable +0x14): the 2-link's 0x2F8D20 takes the foot, its parent and grandparent (thigh,
calf, foot), the 3-link's 0x2F7BC0 one more (thigh, calf, horselink, foot); each starts with the identity target and
the share 0. A chain (0x40 bytes from +0x18): the target matrix (0x30), the share +0x48, the node list +0x4C, its
count +0x54.

The update (vtable +0x8, 0x2F9560): each chain of share > 0 is solved and blended (vtable +0x18: 2-link 0x2F7E10, the
arms' — ik_arm ports it; 3-link 0x2F66D0), then 0x2F9320 makes the nodes below follow (the toes).

**The 3-link solve (0x2FCA70)**: the thigh is first aimed at the target, its x turned the shortest way onto the
target's direction in its own frame (0x101190, 0x104E90 from (1, 0, 0) at 0x40A008, 0x104BB0). The thigh and the
horselink are kept parallel, so the chain is the triangle (D, L2, L1 + L3); the game scales it by k = L3 / (L1 + L3)
(0x2FCC98: 1 / (1 + L1 / L3)): cos a = (D'² + L3² − L2'²) / (2 D' L3), cos g = (L2'² + L3² − D'²) / (2 L2' L3),
each clamped to −1 .. 1. The thigh turns by a about its own z (0x101570), the calf by g + π under it (0x101570 with
−sin g, −cos g, the bone's place the thigh's plus L1 along the thigh's x), the horselink back by the transpose of
that turn (parallel to the thigh again, L2 along the calf's x). Reached: D ≤ L1 + L2 + L3 and no clamp. 0x2F66D0 at
the full share puts the solution in, the foot at the target when reached (else riding on the horselink: 0x2F79A0)
with the target's rotation, and 0x1017F0 on each node.

**The settings** (`MiscData` of the seven books; Ralph's book also serves Dick, Jack, Leon, Rojo and Wally — the
seven with `Bip01 L/R HorseLink`):

| Book | Use3LinkFootIK | FootIKDragHeightScale | RTWSmoothFactor | DisableFootDrag | ankle |
|---|---|---|---|---|---|
| Amanda, Cal, George, Jill, Rhett | 0 | 1 | 0.7 | — | by skeleton: 0.648 (Amanda, Batty, Crock, Kingston, Philbert, Rhett), else 0.528 |
| Lizzie, Ralph | 1 | 0.5 | 0.85 | 1 | 0.514 |

`LiftNodeName` (Cal, Jill: `Helper R Hand`) is not the feet's: the setup reads it to monster +0x100, the node a lifted
person hangs from (PEOPLE_NOTES.md).

## The landing (0x2DC450) [R]
Every landing from the air (from 0x2E2980), whatever the fall speed: p = the place of `Bip01 Spine1` (0x2E6000:
node +0x7DB0, through the matrix as above); p.z = the water's +0x34 with a water (0x2D5F00), else the ground
(0x2E8530); 0x2F48D0 starts WaterImpactFx (+0x154) or GroundImpactFx (+0x150) in the landing's own live effect
(+0x158), the same way as a step. In the same function: mode 4, the landing slot, the water pressed 1.25 × the radius
across (0x2DCA94 → 0x2E2BD0, WATER_NOTES.md), the fall's damage (+0xBA88), and, coming from modes 1 / 2 and unless
+0x108, a call on the object at gp−0x6F5C (vtable +0x28, with +0xFC and 400): the pad's rumble, a
PS2ControllerPack (0x488700, static initializer 0x4372C0; 0x1A5020 → scePadSetActDirect) [R]; the port rumbles the
player's controller there (main.c `mon_rumble`, NATIVE.md "Input").

## Where there are none
- Climbing (mode 5): the climb clips send no footPlant. The mantle onto a roof (`Side_L_Transition_Mantle` at 0.625
  in 25 of the 30 monsters) starts `footfall_Big.edf` through MONSTER_DATA's own effect list (a type-12 clip event,
  slot 8), not through MonsterFx.
- On a roof: footPlant runs as on the street; the ground is the roof's top, the effect the ground one.
- In the air: the velocity test holds it off (a jump's first frames still send footStep, which only lifts).

Of the clip events of the 30 monsters: 1,254 footStep, 1,091 footPlant, 598 cancelFootStep (299 for each foot).

## The port
`src/footstep.c`: `foot_props_read` (MonsterFx), `foot_bone` (the bone per clip foot and mirror), `foot_plant`
(the footfall: when, where, which), `foot_step`, `foot_command` (+0x74: the state left) and `foot_cancel` (the
records, the anchor, the drag's arc), `foot_ik_props` / `foot_ankle` (MiscData, +0x50), `foot_lock` (0x33BF10),
`foot_update` (0x33D450: the records and the IK's shares and targets), `foot_impact` (the landing).
`src/ik.c`: `ik_leg3` (the 3-link: 0x2FCA70 in 0x2F66D0), `ik_feet` (the foot IK object: the chains, the posed feet
noted, the 2-link through `ik_arm`, the 3-link through `ik_leg3`), `ik_clip_node` (the player's clip query).

The viewer drives it like the game's frame: the state command calls `foot_command` with the old state's flags before
the new clip's events; at the end of the frame's move (before the place is clamped and pushed out) `foot_lock` with
each foot as the clip alone poses it (`ik_clip_node`); the city's posing calls `ik_feet` after the arm IK, and
`ik_feet` calls back into the viewer (`IkFeet.update`) between noting the feet and bending the legs, where
`foot_update` runs once a frame on this very pose — so there is no frame of lag between the pose, the targets and the
IK. Checked in a scratch build: walking into Ground_Idle plants the forward foot on the ground (z = ankle) while the
root eases back by 0.3 of the slip a frame until it is within 1e−3; the dragged foot rises on its arc (0.359 for
George; capped at 1.584 = 3 × 0.528 after a long stride) and lets go at the blend's end; the drawn foot sits on its
target to 1e−3 for the 2-link (George) and the 3-link (Ralph, Lizzie), mirrored or not; walking off drops the anchor
at once and the walk is as fast as without the IK.
- Faithful [R]: the four names and their absence, the stillness test, the foot's (x, y), the ground or water z, the
  mirror swap, the per-foot slot, the forward foot as the anchor, Spine1 for the landing; the state left as the
  cancel's test, the drop rule, the slip's share, the easing shares and snaps, the planted z and reach clamp, the
  drag's arc and its end, the two IK classes and their chains, the 3-link solve at the full share, the ankle; the
  clip query (`ik_clip_node`: the clip's keys alone, the root as the clip has it, at time 0, 0x196D20 / 0x33C290).
- The side convention [R]: the game's foot record is (f + 1) & 1 when mirrored, and the mirrored foot query
  0x33C290 keys the node asked for and its parents, mirrors the array (0x194BA0: the pairs swap slots) and composes
  from the other side's slot (0x33C418 .. 0x33C424): asked for +0x72C4 (R), as record 0 of a mirrored monster asks,
  it gives the L slot, the clip's R chain reflected, which is the drawn left foot (the node 0x2E5C40 takes for record
  0 from the pose MonsterXFormTree 0x301610 builds out of the mirrored keys); `ik_clip_node("Bip01 L Foot")` mirrored
  is the same, reflect(clip R). Likewise the viewer's `foot_bone(0, 1)` ("Bip01 R Foot") through
  `scene_monster_node` (city.c) is reflect(node[mirror(R)]), the clip's L reflected: the drawn right foot, record 1,
  the script's foot 0. The reflection [R 0x194BA0, from 0x2FFF10 when
  +0x72B9 is set]: the bone-local transforms are reflected (a pair swapped, each (−qx, −qy, qz, qw; tx, ty, −tz),
  L' = Sz L Sz; Bip01 Sx L Sy through the constants 0x3B1790 .. 0x3B17B0; Pelvis Sy L Sz; Lift_Node Sy L Sy), so in
  the model M'_n = Sx M[mirror(n)] S_n, S_n the bone's y for Bip01 and Lift_Node, its z for the rest. The place is
  Sx's (the same as the viewer's old S M S), and so is the skin; the rotations differ by a half turn about the
  bone's axis, which the viewer now has (city.c `reflect_node`): the mirrored IK hand's target is Sx r Sz, the root's
  turn Sx R Sy (main.c `seen_root`). The root motion is mirrored with Bip01's rule (0x194A60 on the clip's start
  root, from 0x2FFA00 / 0x2E6580 / 0x2DB130); 0x196F30 / 0x196E70 / 0x1859F0 are the composition (the rotations,
  the place turned and added), in 0x196D20 and 0x33C290 alike;
  restarting a foot's effect (the game) versus starting a new one (the
  viewer) is the same for these one-shot effects; the 3-link's blend below the full share [R]: 0x2F66D0 runs
  0x2F7E10's blocks with one bone more (0x2F6E04 the thigh in the model's frame, 0x2F6FCC and 0x2F726C the calf and
  the horselink each after a 0x101A90 into its parent's, 0x2F75D4 the foot as 0x2F87B8), as ik_arm ports them;
  0x1017F0 is a row Gram-Schmidt, a no-op on rigid matrices [R]; the arc cap set once for the game by the first
  monster to drag (gp−0x6C68 / gp−0x6C64), a static in `drag_setup` [R]; the shares applied per world step, 1/30 s
  (RWorld 0x2B7800) [R]: k^(30 dt) a viewer frame (`FootFrame.steps`); the mirrored 3-link solved on the other side's
  nodes with the target reflected (as the arms), where the game solves the mirrored pose itself — the same up to the
  bend's sense, which both put forward.
- Fixed: the stop that slid the monster on (up to 17 units for George, walking into Ground_Idle on NewYork 3:
  −12.2 → −29.4). Two causes: the foot lock asked the clip at its time now, with the root held and turned as the
  posing does (the game: time 0, the root as the clip has it); and the posing (city.c `pose_instance`) kept the pose
  a blend starts from with the root as the clip has it, so a walk's root (13.6 ahead at its end) came back into the
  drawn pose over the walk → idle blend and the planted foot's reach clamp dragged the anchor's record along after the
  hip, which the lock then followed. The game takes the root's motion out of the clip's pose (0x301080, the root
  rewritten at 0x3003C8) before the blend towards the kept pose (0x30057C ..), the kept one being last frame's final
  pose; the posing now does the same (the hold and the turn on the root before the blend, and so in the kept pose).
  Now: −12.24 → −14.52 (0.3 of the slip a frame), the record stays at its planted place; mirrored the same on record
  1; Ralph −11.6 → −12.7, Rhett −14.4 → −15.7 (were −17.1, −27.8).
- Not ported: the debug byte; the dynamic scale in 0x2E5C40 (+0xA78; the viewer draws at 1); the state command's own
  0x33D490 refresh (the next frame's update does the same).
