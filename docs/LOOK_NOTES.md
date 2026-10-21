# The face queue, the head turn, the velocity's friction and the tail

Four small pieces of the monster traced from the ELF; `src/look.c` ports them. [R] read, [G] guessed.

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

The face's event player (+0x7DE8, beside the body's at +0x7DD4) [R]: 0x2F45D0 at each start and replay fires the
face node's events at its time 0, and each frame the update (0x2DE600) steps it (0x2F4500: the node's rate × dt × the
monster's speed +0xA8C, firing the events it reaches) through the body's handler 0x2E7840. The face clips' events:
sounds (every Expression_ node), `Expression_Burp`'s `Monster_Belch.edf` and `Expression_Spew`'s `Fire_small.edf` /
`Monster_Cough_smoke.edf` (type 12, FORMAT_MONSTER.md "Effect slots"), `Expression_Barf`'s `Monsters::doBarf`. The
port: main.c `face_events` (`Mon.face_ev` the player's time), the node's events from moves.c (`MoveNode.events`).

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

## The tail
`MonsterTailAnimator` (vtable 0x44AFE0: +0x8 the update 0x2FAB70, +0xC the per-node build 0x2F97E0; 0x468 bytes,
two of them in the animation player at +0x113C, counted at +0x1A0C). The player's setup 0x301AE0 makes one for a
skeleton node named `Bip01 Tail` (any case, 0x32C9C8 at 0x301FA0; constructor 0x2FABC0). [R] throughout.

**The chain** (0x2FABC0): from that node, while it has exactly one child (node +0x8) named `Bip01 Tail<k>` or
`Bip01 Tail0<k>` (k = 1, 2 …, compared over that name's length, 0x32D1B0), the child is the next link (+0x1C the
first's index, +0x20 the count; the links' bits in the mask +0x8 .. +0x14, which makes the tree build 0x301610 call
+0xC for them: 0x2F97E0 is the plain build, the parent taken as the node before). 13 skeletons have one: Amanda,
Batty, Crock, Kingston, Philbert, Rhett (`Tail1` .. `Tail3`, nodes 56 .. 58) and Dick, Jack, Leon, Lizzie, Ralph, Rojo,
Wally (`Tail1`, `Tail2`, nodes 58, 59), always right after the base (the pose writes base + k). Per link a turn in
the world (+0x24, 32 quaternions, [0] the base's) and a spin (+0x234, 32), all none at the start, the springs marked
to start over (+0x464 = 1), the weight 0 (+0x454).

**The weight** (0x2FAB00, called by the monster's command 0x2E6580 at 0x2E6884 for each tail as a clip starts): it
goes to the new state's node +0x10 over the request's +0x4, which is always the node's blend time (+0x14, 0x2ECC08),
cut or blend: the rate (target − weight) / time at +0x458, the time left at +0x44C. A time of 0.001 or less sets no
blend at all, so **a cut leaves the weight where it was** (the target is kept but never reached). Node +0x10 is 0 or
1: 1 in `Ground_Walk` (all 30 monsters), in `Jump_Air_Borne_Profile` / `…ProfileR` (23 of them; of the tailed ones
Amanda, Batty, Crock, Kingston, Philbert and Rhett, not Dick, Jack, Leon, Lizzie, Ralph, Rojo, Wally), Rojo's
`Ground_Walk_2` .. `_4` and Marco's `React_Side_L_Eat_Person`; 0 everywhere else: the tail swings while walking
(and in the profile jumps) and is the clip's otherwise.

**Each frame** (vtable +0x8 from the monster update at 0x2DEA78, after the animation player's frame 0x2FFF10 has built
the model pose, before the arms' and feet's IK at 0x2E1304 .., the monster's world matrix +0x8 as it was before the
frame's move): while a blend runs (0x2FA970) its time falls by dt and the weight moves by rate × dt, untested; when
it is out the weight is the target and the frame goes on as 0x2FA820, where a weight of 0 or less only marks the
springs to start over (the clip's tail stays). Otherwise q[0] = the base's model matrix × the world matrix, squared
(0x101A90, 0x1017F0, 0x105090); starting over, every link takes q[0] (the spins are kept). The time adds up (+0x460)
and while more than MONSTER_DATA +0xA04 (1/30 s) is there the springs step (0x2F9970), then the links are posed
(0x2F9E90).

**A step** (0x2F9970; MONSTER_DATA +0x9F4 .. +0xA00, the same in all 30: 0.7071, 0.7071, 0.05, 0.2), link by link
from the base, each against its parent as already stepped (a × b the game's product, rows: a then b):
- d = parent × link⁻¹ (what would line it up), taken 0.05 of the way (0x104C60: towards none by 1 − 0.05);
- the new spin = d × (spin taken 0.2 of the way)⁻¹ × spin: the spin keeps 0.8 of itself and gains d;
- the link turns: spin × link; if it is now more than 90° from its parent (the relative turn's w under +0x9F8 =
  cos 45°) it is set to 90° about the same axis ((+0x9F4 × axis, +0x9F8) × parent);
- both normalised.
So each link is a damped angular spring towards its parent's turn, in the world: as the monster turns or sways the
tail lags behind and swings through, link after link.

**The pose** (0x2F9E90): c starts as none, h_before as q[0], the parent's new turn as the base's model turn (its rows
squared in place first, 0x2FA03C). Per link k (node base + k): c = its bind turn (the skeleton's +0x3C table, 0x50 a
node, +0x40) × c, its rest turn below the base; h = c × q[k]; the spring's relative turn r = h × h_before⁻¹; the
clip's own relative turn is the posed link × the posed parent's rows turned over, squared (0x105090); r is taken on
its side (dot < 0: negated) and the clip's goes towards it by the weight (0x1042F0: a straight mix within |a − b|² <
0.25, else along the arc; under 0.001 the clip's, over 0.999 all of r); that × the parent's new turn is the link's
turn (0x104BB0), and its place the bind place (+0x34) in the parent's new frame (the clip's own place of the link is
not used). With the weight 1 (walking) the tail is its bind shape, bent only by the springs.

**Mirrored**: the springs run on the mirrored pose (the keys are mirrored before the build) with the skeleton's bind
turns and places as they are; the tails' bind turns are about z alone and their places along x, which the reflection
leaves as they are, so nothing changes. The port turns the drawn links (reflected) and reflects them back.

**Port**: `look.c` `TailAnim`, `tail_find` (0x2FABC0), `tail_blend` (0x2FAB00, from main.c's `play_state`, with
MoveState `tail` = node +0x10), `tail_pose` (0x2FAB70 .. 0x2F9E90), run by city.c's `monster_tail` after the pose and
before the IK; main.c hands it the frame's dt. `RAMPAGE_NOTAIL=1` leaves the clip's tail.

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
- **Mode 5** (0x2E03A8): the velocity is 0 unless a slot is waiting; no gravity and no ground follow, mounted or not
  (a knock-off's clip after its `unmountBuilding` holds the monster where it is till its own mode event); only below a
  ground at or under the street (z < ground ≤ 0) z goes 0.4 × ground + 0.6 × z a step (0x2E06E0 .. 0x2E0730) [R].

F (+0xF4) is 50 at spawn (0x2E98C0). A hit (0x2D6A50) sets it:

| | velocity | F |
|---|---|---|
| a monster's blow that slides (0x2D77D8) | dir × 2d / T | 2d / T²: it slides d in T and stops |
| a monster's blow that flies: ReactionLevel 2, level 1 with the record's +0x20 = 2, or the victim in modes 1 and 2 (0x2D7224 .. 0x2D7260, then 0x2D7558) | dir × d / T, z = tan(attack +0x38) × that speed (tanf 0x1576D8 of the record's 0, 1 or 1.8 as radians), a spin of 4π n / t, n the record's +0x4C, t the time to the ground (0x2D76B8 .. 0x2D77B4; n is 0 in all 2014 attack events of the 30 monsters: none) | 50 (0x2D77C8) |
| a monster's blow without a distance (d within 0.001, 0x2D7890) | 0 | kept |
| a shot's knock (army, 0x2D7928, at any ReactionLevel; halved after in mode 4, 0x2D8AA8) | each axis the knock's if larger in size | 50 (0x2D79E0) |

d is the attack record's +0x30 (0, 6 … 16), T its +0x34 (0.2 … 0.8) (FORMAT_MONSTER.md "Clip events", type 0).
A blow replaces the whole velocity, the victim's own (a jump's) included. Its dir is the attacker's front (the +0x14
row of its matrix, the node's +0x20 / +0x24 = (0, 1) in all 5284 states), or the way from the attacker to the
victim when the attacker's state node has +0x62 bits 1 and 2 (0x2D6E18: Side_L_Punch of all 30, Ground_Punch ..
Ground_Punch_3 of some; or the target handle holds the victim, never), made flat and 1 long, then turned about z by
the record's +0x3C (0 in all). In mode 4 (landed, or down) the reaction halves it (0x2D8AA8). The scales of d at
+0xBA8C (+0xC) and +0xBAA8 (+0x8) come from clip events 1 and 19 that no monster has [R]. The rest of the blow (the
air hits, the reactions) is in MULTIPLAYER_NOTES.md "Monster against monster"; here look.c `monster_blow`, from
main.c `monster_blow_hit`. `setVel` / `addVel` (0x2E74F0, 0x2E7330) do not touch F, so a knocked monster slides with the F of
its last hit. Walking adds the stick's own velocity (+0xA68) on top and is not subject to this friction.
