# The Wii version's code: unlocks and controls (phase 0 of WII_PLAN.md)

Read in the Wii disc's `Rampage.elf` (RPGP5D, `DATA/files/Rampage.elf`), which keeps its symbol table (10,233
functions by name). `tools/wiidis.py ELF NAME` disassembles a function with its calls, strings and small-data
values. Addresses are the Wii ELF's. **[R]** read in the code, **[G]** inferred.

## How the ten Wii monsters are unlocked

| Monster | Unlocked | Where |
|---|---|---|
| Boris, Ruby | from the start | `Profile::reset` 0x8008FB58 [R] |
| Plucky, Fifi, Cyril | revealed in Dallas | `Dallas_Props` WorldData `MonsterRevealDef` [R] |
| Squirmy, Flea, Sarah, Brian, VERN | 1, 3, 6, 12 and 20 monsters with all five upgrades | `RWorldStateMgr::prepareForResults` 0x80081970 [R] |

**The start** [R]. `Profile::reset` writes version 8 and size 0x150 (the PS2's 0x128 and 0x28: ten more monster
words), clears the status record (0xE0 bytes at +0x20; the PS2's 0xB8 at +0x28), the city flags (8 words at +0x100)
and start blocks (8 at +0x120), gives city 0 0x80, and gives 0x80 to monsters 0..7 (+0x54..+0x70, status +0x34 + 4m
as on the PS2). The PS2 gives 0..5. In the Wii's order (`ShellData` `MonsterData`) 6 and 7 are Boris and Ruby. The
players' monsters start as 0, 1, 2, 3 (status +0x14).

**The reveals** [R]. `MonsterReveal::setupExtraMonsters` 0x800837AC is the PS2's cryotube reveal: the city's
WorldData `MonsterRevealDef`, each `RevealDef|` a `Monster` and the monsters (or "everyone") that reveal it, then
`RevealLoc` (`Block`, `Building`, `floor`, `cell`). It reveals three monsters per city: Dallas's are Plucky (by
everyone), Fifi (Rojo, Kingston, Dick, Squirmy, Flea) and Cyril (Furby, Sarah). The other seven cities reveal the
same three each as on the PS2; the Wii adds its monsters to some of the lists of who reveals them.

**The city bonuses** [R]. `RCommon::updateProfile` 0x800911A4 runs its table `bonuses` (0x802F1640) of three, as
the PS2's 0x3F6770: Philbert (every city's flags have 2), Eyegore (4), Boosy (0xF), once every city has 0x80.

**The five-star unlocks** [R], new on the Wii. At the end of a block that was finished or reached a checkpoint
(`prepareForResults` +0x18, `BlockMgr::isAtCheckpoint`), after the PS2's "Boosy at the block's end" check
(0x80081CC4, `MonsterReveal::areAllMonstersUnlocked`):
1. count the monsters, all 40 (gCommon +0x284 + 4m, the current record's flags), whose flags & 0x1F == 0x1F: every
   upgrade, the four of the PS2 and SuperJump (16);
2. walk `n5StarUnlock` (0x802F1030: 1, 3, 6, 12, 20) and `n5StarUnlockNames` (0x802F1044: Squirmy, Flea, Sarah,
   Brian, Vern): stop at the first threshold above the count; when the count **equals** a threshold, that monster
   gets 0x80 and the walk stops.

So one monster per block at most, and only on a block where the count equals a threshold. If the count jumps past
a threshold (two monsters completed in one block), that monster is never unlocked by this rule, as the count only
grows [R; whether a game can do that, [G]]. The rule needs SuperJump: without it no monster reaches 0x1F.

## The Wii's controls

**Input ids** (`PControllerIds`). The move graphs' condition variables are these ids:
`MonsterActionTree::queryEvents` 0x8001CC94 passes a condition's variable to `RController::getState` 0x800608A4
(held values) or `getMakeEvent` 0x80060540 (new this frame), and treats 4..21 and 58..67 as booleans [R].

| Wii id | Value [R] | PS2 id |
|---|---|---|
| 0, 1 | left stick x, y (y negated): the Nunchuk's, or the Classic Controller's left | 0, 1 |
| 2, 3 | the Classic Controller's right stick | 2, 3 |
| 4..21 | buttons (below) | 4..21 |
| 22..29 | (as the PS2's pressures; not traced) | 22..29 |
| 30..34 | the Wii remote's accelerometer: x, y, z, magnitude, speed | — |
| 35..39 | the Nunchuk's accelerometer: x, y, z, magnitude, speed | — |
| 40 | \|R\|: the larger of \|id 2\|, \|id 3\| | 30 |
| 41 | \|L\|: from ids 0 / 1, or with `RController` +0x460 = 0 from ids 30 / 32, with dead bands (a control mode that walks by tilting the remote [G]) | 31 |
| 42..57 | the stick's direction in the monster's frame (bits of +0x454 from `_calcDirectionFlags`; left and right swapped when the monster is mirrored) | 32..47 |
| 58..65 | the stick's direction on screen (`_calcProfileDirection`, +0x458; mirrored too) | 48..55 |
| 66, 67 | true, false | 56, 57 |

The Wii inserted its ten motion inputs at 30..39, so every id from the PS2's 30 on is 10 higher.

**Buttons** [R]. `GamecubeControllerPack::getEvents` 0x801480DC reads `KPADRead` (the status record copied at
+0x2C8: buttons, then the accelerometer at +0x0C.., the Nunchuk at +0x60..) and turns button bits into ids by
`rev_digital_button_order` (0x8030D7C8):

| Wii remote | D-pad left, right, down, up | A | B | 1 | 2 | + | − | Home |
|---|---|---|---|---|---|---|---|---|
| id | 6, 7, 5, 4 | 14 | 15 | 16 and 10 | 17 and 11 | 18 | 19 | 20 |

`gc_digital_button_order` (0x8030D830, the GameCube pad) keeps the PS2's ids: A 14, B 15, X 16, Y 17, Z 12, R 10,
L 11, Start 8. The Wii's graphs also test id 12, which no entry of the Wii remote's table gives: probably a
Nunchuk button (C or Z) [G].

**The Wii graph against the PS2 graph** (George, `tools/monster.py` on both). 941 (state, target) pairs are in
both, 937 with as many transitions; 56 are the Wii's only (its five new states), 4 the PS2's only. Condition by
condition [R, counted]:

| Wii | PS2 | |
|---|---|---|
| v14 (A) | Square | punch (193) |
| v15 (B) | Cross | jump (131) |
| v17 (2) | Circle (16) | |
| v12 | Triangle (18) | |
| v41, v42.., v58, v59, v60, v63, v66, v67 | \|L\|, M.front.., L.up, L.down, L.left, L.right, true, false | the +10 shift |

Where the counts differ, the Wii swings the remote where the PS2 presses a button:

| PS2 | Wii | times |
|---|---|---|
| `Circle==1(new)` | `v33>2 v30<-1.5 v32>1.5 v31<1.5`: a hard swing (magnitude > 2), sideways | 37 |
| `Triangle==1(new)` | `v33>2 v31>1.5 v32>1.5`: a hard swing, down | 33 |
| `Square==1(new) L.down==1` | `v14==1(new) v59==1 v41>0`: A with the stick down | 35 |
| `Square==1(new) L.up==1` | `v14==1(new) v58==1 v41>0.5` | 34 |

So a Wii graph can be run on the PS2's pad by renumbering (Wii id − 10 from 40 on; A → Square, B → Cross, 2 →
Circle, 12 → Triangle) and by turning the two swings back into Circle and Triangle presses. The Wii's new
transitions (`Ground_Walk_Slow` / `Medium` by how far the stick is pushed, `Jump_Super`) need no motion.
