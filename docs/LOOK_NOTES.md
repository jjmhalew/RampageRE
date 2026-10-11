# The face queue, the head turn and the velocity's friction

Three small pieces of the monster traced from the ELF; `src/look.c` ports them. [R] read, [G] guessed.

## The face queue
The Monster script methods `setFacialAnim` (descriptor 0x42BAA0, id 0x7D, dispatcher case 0x2ABA88 → 0x2E9200) and
`queueFacialAnim` (0x42BAB0, id 0x7E, case 0x2ABAC0 → 0x2E8FB0) both take (name, head layer, looping, plays) and look
the name up among the state nodes (hash × 131 + lower-case char, % 97, the table at monster +0x124; any case). An
unknown name does nothing.

The faces are records of 0x24 bytes, 8 of them at monster +0xBAB8 (0x2EA0E0): +0 next, +4 previous, +8 the node,
+0xC the mask with the head bone (the head layer), +0x1C looping, +0x20 plays left. Two lists: the free one (+0xBBE0 /
+0xBBE4) and the faces (+0xBBD8 front, +0xBBDC back). The front plays.

- **setFacialAnim** (0x2E9350): takes a free record (none: nothing happens), starts its clip on the face layer at once
  (0x2FF820: the node's clip at its rate, blended over the node's +0x14 when > 0) with its clip events (0x2F45D0,
  the event player at +0x7DE8), then frees the front record and puts the new one at the front. What waits behind
  stays.
- **queueFacialAnim** (0x2E8FB0): the same record, appended at the back. Nothing starts.
- **The clip's end** (0x2E95D0, from the face layer, 0x2FF880): the front plays again while it loops or, counted
  down, has plays left (plays 0 and 1 both mean once); otherwise it is freed and the next one starts, or with none
  the script function `DefaultFacialAnim` runs (`setFacialAnim("Expression_Idle", 0, 1, 0)`).

So a queued face plays only after a face that does not loop. The default face loops, so behind it a queued face waits
until something calls setFacialAnim with a face that ends. No script calls queueFacialAnim (Monsters.scr and every
compiled .bsf call setFacialAnim only); it is an unused binding.

`Monsters::faceAnim(expression, head layer, delay, plays)` (the clip event) never loops (`if (param2) isLooping =
0`, and it starts at 0). A delay above 1 installs a timer (`installObjectTimer("Monsters::appendBelch", monster, delay,
0, 1)`): that many seconds later `setFacialAnim("Expression_Burp", 0, 0, 1)`. George's throws chew, then burp 5 s
later (`faceAnim(10, 0, 5, 1)`).

## The head turn
`MonsterHeadAnimator` (vtable 0x44B020; constructor 0x2FC820, made by the animation player's setup 0x301AE0 for each
skeleton bone named `Bip01 Head`, `Bip01 L Head` or `Bip01 R Head`). Its bones: the head (+0x1C) and, when
MONSTER_DATA +0xA14 is set (all 30), the head's parent (+0x20; `Bip01 Neck`). Its angles:

| | now | target | bone | rate (MONSTER_DATA) | limit |
|---|---|---|---|---|---|
| yaw | +0xBC | +0xC0 | the neck, about the model's up (its 3 × 3 × Rz, 0x2FC7DC) | +0x174: π/2 a second | ±+0xA08 (81°) |
| pitch | +0xE8 | +0xEC | the head, about its own z by −pitch (Rz × its 3 × 3, 0x2FC508) | +0x178: π/2 | −+0xA10 .. +0xA0C (45°) |

(Row vectors. With one bone the head takes both.) Each frame while the pose is built (0x2FBF90) each angle moves to
its target at its rate, half that while the stick holds it (+0x119), never past it. There is no blend weight: the
angles themselves ease in and back to 0.

**Mirrored** [R]: the pose is built by MonsterXFormTree (vtable 0x44B070, +0x14 = 0x301610) from the keys 0x2FFF10
has already mirrored (0x194BA0 at 0x300264 / 0x3008F0), and it runs each node's animators' +0xC (0x30182C) as it
goes, so the turn acts on the mirrored pose: the drawn head turns by the yaw as it is (−rx × 81° whatever the
mirror). The viewer turns its unmirrored pose and reflects it, so it hands a mirrored monster −yaw (main.c); the
pitch, about the head's own z, comes through the reflection (Sz Rz Sz = Rz) unchanged.

**The right stick** [R]: the monster's command (0x2E70C0 in 0x2E6580), in a state whose node has flag 0x200: the
held flag is the right stick off centre, and 0x2FBE30 gets pitch = ry × 45° (+0xA0C for ry > 0, +0xA10 otherwise) and
yaw = −rx × 81°, the sticks being the clip request's +0x24 / +0x28 ÷ 255 (y down). Outside such a state the held flag
is cleared. The update (0x2FBC90) zeroes both targets whenever tracking is off and the stick does not hold them, so
the head eases back. One state has 0x200: `Ground_Walk` (0x288, the same in George, Lizzie and Ralph).

**Tracking** [R]: the update (0x2DE9C4) switches tracking on (+0x118) only when the monster's target handle (+0xADC:
{monster, object, serial}) holds an object (0x2FF760) and the state node has flag 0x20. Then 0x2FB950 aims at the
target's place in the monster's frame (+0xA5C, made at 0x2DE6C0), the neck's and the head's places being the last
built pose's (+0x80 .. and +0xB0 .., stored only by 0x2FBF90 at 0x2FC2A8 .. / 0x2FC36C ..): yaw = heading(target −
neck) − heading(the Tracking_Node's x axis), pitch = elevation(target − head) − elevation(that axis) (heading =
atan2(−x, y), elevation = π/2 − acos(z / |v|)). The pitch target is wrapped and clamped; the yaw clamp lands on the current yaw (+0xBC), not on
its target. The handle is only ever cleared (0x2FF790, from the constructor 0x2EB740) and no node of the 30 monsters
has flag 0x20, so **tracking never runs in this build**: the head turns only with the right stick. The handle is also
the "melee range" test (+0xF9: within MONSTER_DATA +0x184 × scale, 0x2DE650), equally idle.

## The velocity
The monster update (vtable +0x10, 0x2DE530) switches on the movement mode (+0xAD8; jump table 0x431290): 0, 2 and 4
→ 0x2DF6F0, 1 → 0x2DEBD8, 3 → 0x2DF300 (no event sets mode 3), 5 → 0x2E03A8. The friction is the monster's +0xF4.

- **Modes 0, 2, 4** (0x2DF6F0): the up velocity is set to 0 unless a slot's state is waiting to start (+0x110).
  With |v|² ≥ 1e-6: the place moves by v × dt (x, y; z too when the node has 0x80002), then (0x2DFD40 or 0x2E0038)
  d = v / |v| and v − d × F × dt is the new velocity (z only with 0x80002). If its dot with the old velocity is ≤ 0
  it stops (0 from 0x409FF8). Below 1e-6 nothing changes. Then the height eases onto the ground (0x2E8530): at once
  within 0.001, otherwise 0.3 × ground + 0.7 × z per frame (not scaled by dt) unless gravity is off. Mode 2 (taking
  off) is a ground mode here: no gravity, the up velocity dropped, the friction on.
- **Mode 1** (0x2DEBD8): with |v|² ≥ 1e-6, a drag v −= d × 0.002 × |v|² × dt on all three axes; the place moves by v ×
  dt; then gravity, 60 × dt off z, unless `setDisableGravity` (+0xAB8).
- **Mode 5** (0x2E03A8): the velocity is 0 unless a slot is waiting.

F (+0xF4) is 50 at spawn (0x2E98C0). A hit (0x2D6A50) sets it:

| | velocity | F |
|---|---|---|
| a monster's attack, the victim on the ground (0x2D77D8) | dir × 2d / T | 2d / T²: it slides d in T and stops |
| the same in the air, modes 1 and 2 (0x2D7558) | dir × d / T, z = tan(attack +0x38) × that speed (tanf 0x1576D8 of the record's 0 or 1 as radians), a spin | 50 (0x2D77C8) |
| a shot's knock (army, 0x2D7928) | each axis the knock's if larger in size | 50 (0x2D79E0) |

d is the attack record's +0x30 (15, 10; times its modifiers), T its +0x34 (0.2 … 0.4) (FORMAT_MONSTER.md "Clip
events", type 0). `setVel` / `addVel` (0x2E74F0, 0x2E7330) do not touch F, so a knocked monster slides with the F of
its last hit. Walking adds the stick's own velocity (+0xA68) on top and is not subject to this friction.
