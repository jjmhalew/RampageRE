# Footsteps and landings

The dust (or splash) under a monster's foot and the bigger one when it lands, traced from the ELF; `src/footstep.c`
ports them. [R] read in the code, [G] guessed.

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
| +0x00, +0x28 | two foot records of 0x28 bytes: +0x0 weight (1 held, 0 free), +0xC place (world; +0x14 its z), +0x18 a second vector, +0x24 flags (1 planted, 2 dragged) |
| +0x50 | added to the ground's height for a planted foot: −(a component of the right foot's bind-pose place, 0x174A90) [G: the ankle's height; sign and meaning not settled] |
| +0x54, +0x58 | 0.5, 0.5; +0x5C, +0x60 0.7 (not followed) |
| +0x64 | `MiscData|RTWSmoothFactor` (0.7; Lizzie and Ralph 0.85), the share the root keeps of the planted foot's slip |
| +0x68 | a byte: a footfall waiting for 0x33BF10 (see below); only ever cleared |
| +0x69 | `MiscData|DisableFootDrag` (Lizzie and Ralph 1) |
| +0x6C | the anchor: the planted foot the root follows, −1 none |
| +0x70 | the monster |
| +0x74 | the current state node (0x33CCA0, from the command 0x2E6580: monster +0x10C) |

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
   definition; then it is placed (vtable +0x8C, 0x1F2500: +0x50), given the frame at gp−0x70F0 (+0x90, 0x1F1D60:
   [G] the identity) and restarted (+0x2C, 0x1F2260).

The record itself is not touched: footPlant only makes the footfall. No sound but the effect's, no camera shake, no
damage to people, vehicles or roofs (the roof stomp is the separate event `Monsters::damageRoof` →
`hitBuildingRoof("Bip01 R Foot", 1, 2)`), no scale (the monster's size moves the foot, not the puff), and the water
is not pressed (the wading does that every frame on its own, WATER_NOTES.md).

### cancelFootStep (0x33CA10) [R]
Every standing clip sends it for both feet at its start (Ground_Idle, the reactions, the jumps' starts ...), but
**only foot 0 does anything** (`bnez $s2` on the argument before the swap). Then, unless the debug byte, with a state
node (+0x74) whose flags have 0x50 or 0x1080 and the monster still:
- s2 = record 0 (1 mirrored), s1 the other; both feet's places, less the monster's place, into its frame (0x101190:
  the dots with its axes); if s2's y (forward) is less than s1's they swap: **s2 is the foot further forward**;
- s2: weight 1, flags |= 1 (planted), its place (0x2E5C40), z = the ground (0x2E8530) + the object's +0x50;
- s1: with DisableFootDrag weight 0 (its flags kept); otherwise weight 1, flags |= 2, 0x33C5A0 sets the drag up,
  flags &= ~1;
- +0x6C = s2.

No effect. The name fits loosely: it ends whatever step was going and sets the feet down where they stand.

### The foot lock (0x33BF10, called at 0x2E0EFC in the monster update 0x2E03A8)
With an anchor: if the +0x68 byte is set, a footfall at the anchor's record place (z the ground; record 1 → 0x2DC350,
else 0x2DC3D0) and the byte cleared. Then the anchor is dropped (−1) unless the state locks the feet (0x50 / 0x1080)
or its record is planted; otherwise the planted foot's posed place (0x33C290 with the own IK, else the animation
player's vtable +0x34 on node +0x72C0 / +0x72C4, swapped when mirrored) is compared with where it was planted, and
the root is moved by (1 − RTWSmoothFactor) of that slip so the foot stays put (gone within 1e−6: the anchor is
dropped). **No code sets +0x68** (every `sb` to +0x68 / +0xC884 in the monster and foot code stores zero), so this
footfall never happens [R]: the footfalls are footPlant's alone. The lock and the drag (0x33C5A0, 0x33CDE0,
0x33CF90) are not ported.

## The landing (0x2DC450) [R]
Every landing from the air (from 0x2E2980), whatever the fall speed: p = the place of `Bip01 Spine1` (0x2E6000:
node +0x7DB0, through the matrix as above); p.z = the water's +0x34 with a water (0x2D5F00), else the ground
(0x2E8530); 0x2F48D0 starts WaterImpactFx (+0x154) or GroundImpactFx (+0x150) in the landing's own live effect
(+0x158), the same way as a step. In the same function: mode 4, the landing slot, the water pressed 1.25 × the radius
across (0x2DCA94 → 0x2E2BD0, WATER_NOTES.md), the fall's damage (+0xBA88), and, coming from modes 1 / 2 and unless
+0x108, a call on the object at gp−0x6F5C (vtable +0x28, with +0xFC and 400) [G: the pad's rumble].

## Where there are none
- Climbing (mode 5): the climb clips send no footPlant. The mantle onto a roof (`Side_L_Transition_Mantle` at 0.625
  in 25 of the 30 monsters) starts `footfall_Big.edf` through MONSTER_DATA's own effect list (a type-12 clip event,
  slot 8), not through MonsterFx.
- On a roof: footPlant runs as on the street; the ground is the roof's top, the effect the ground one.
- In the air: the velocity test holds it off (a jump's first frames still send footStep, which only lifts).

Of the clip events of the 30 monsters: 1,254 footStep, 1,091 footPlant, 598 cancelFootStep (299 for each foot).

## The port
`src/footstep.c`: `foot_props_read` (MonsterFx), `foot_bone` (the bone per clip foot and mirror), `foot_plant`
(the footfall: when, where, which), `foot_step` and `foot_cancel` (the records and the anchor, kept as the game keeps
them for a foot lock to come), `foot_impact` (the landing).
- Faithful [R]: the four names and their absence, the stillness test, the foot's (x, y), the ground or water z, the
  mirror swap, the per-foot slot, the forward foot as the anchor, Spine1 for the landing.
- [G]: that the viewer's `scene_monster_node` side convention equals the game's swap (argued from the two, not run);
  the frame gp−0x70F0 being the identity; restarting a foot's effect (the game) versus starting a new one (the
  viewer) is the same for these one-shot effects; the +0x50 offset (left at 0 by `foot_init`).
- Not ported: the foot lock and drag (0x33BF10, 0x33C290, 0x33C5A0, 0x33CDE0, 0x33CF90), the debug byte.
