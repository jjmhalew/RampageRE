# Several monsters: players, bots, and how they share a city

How the game plays up to four monsters at once, one per player, some of them the computer's (the bots), and how the
native viewer does it (`src/main.c`'s `Mon`, `src/city.c`'s `Scene.mon`, `src/director.c`'s DestroyBuildings goal).
Addresses are in RAMPAGE.ELF. **[R]** means read in the code or the files; **[G]** means a guess or a stand-in.

## Who plays

- The players, bots included, are Common 0x4D7E28 (players) and 0x4D7E2C (bots); the last `bots` of the players are
  the computer's [R, SHELL_NOTES.md]. The shell's `NumPlayers` box offers 1 or 2 players on the PS2 (3 and 4 only on
  other platforms), `NumBots` how many of them are the computer's.
- Each player has a monster slot 0 .. players − 1: its monster (GameStatus +0x14 + 4 i), its HUD (`Hud<i>.ifc`), its
  points and kills (`Player`, +0x318 + 0x60 i), its lives (+0x224 + 4 i) [R, GAME_NOTES.md].
- **BeginLevelNI** puts monster i at x = −17 (n − 1) / 2 + 17 i, y = −5.2, facing π, playing `React_Intro`; it ends
  when every slot's move is done [R Levels.scr].
- **BeginBlockNI** resets every slot; the bots get `setIsAI(1)` and `destroyBuildings`, the players `setIsAI(0)`
  [R]. Every other NI (Results, EndBlock, the level and world ends) makes every slot AI for its script moves.

## The bots: the DestroyBuildings goal (MonsterAI goal type 2)

`destroyBuildings` is Monster binding 157 (case 0x2AC2B0): goal = 0x2ED810(2) (0x88 bytes, constructor 0x2F1CB0,
vtable 0x44AE60), queued by 0x2EE600, its states looked up by 0x2F1C10, the current goal stopped (0x2EE650). Its flags
+0x18 = 0 and its update 0x2F1A60 always returns 1: **it never ends** [R].

| Offset | |
|---|---|
| +0xC | the MonsterAI |
| +0x20 / +0x24 | the target building and its id (building +0x4); the target is valid while the ids match |
| +0x28 | its side: 0 left, 1 right, 2 front, 3 none |
| +0x2C | the walk point |
| +0x38, +0x3C | the last hit's point is known; its offset from the monster (x negated when mirrored) |
| +0x48, +0x4C .. +0x54 | a target cell is set; the cell (face, storey, column) |
| +0x58 .. +0x60 | the current cell, from the mount (+0xC628, +0xC62C, +0xC630) |
| +0x64 | a cooldown (0.2 s, set by 0x2EEBD0 from 0x2E03A8; what calls it is not known) |
| +0x68 / +0x6C | a building it was knocked off, and 3 s of avoiding it |
| +0x70 | seconds on this target |
| +0x74 .. +0x84 | the states Ground_Transition_Front, _Side_L, _Side_R, Faceup_Idle_Respawn, Ground_Punch_Down |

The monster tells the goal through MonsterAI's forwarders (0x2ED680 .. 0x2ED7C0), only while it is AI (+0x108) [R]:
`onHit(pt)` (0x2EED40, from hitBuildingPt 0x2D54F0), canMount again at prepareToMount (0x2EECC0), `avoid(b)`
(0x2EEBE0: knocked off a face 0x2E1868, or canClimb −2: +0x68 = b, 3 s, side 2, the walk point in front of b, the
target and cells cleared), `onMount(b)` (0x2EEC40: target b, the side by the rail: 0 left, width right, else front).

**Each frame (0x2F1A60)** [R]:
1. HP ≤ 0: towards `Faceup_Idle_Respawn` through the next-hop table, else action 15. The respawn's own transitions
   (canRespawn) say when.
2. The target is checked (validate 0x2F15F0: standing, state 1 .. 3; mounted: fine; else its side's mount point
   passes the place check 0x2DDC40 (1 or −2) and approachClear 0x2F1290, and canMount(side, 0) is not −5 or −6); if
   not, a new one (pickBuilding 0x2F0F60).
3. With a target: mounted on it or on nothing, withTarget; mounted elsewhere, jump off. Without: mounted, jump off;
   else huntMonster.

**pickBuilding (0x2F0F60)**: of the standing buildings (count 0x4D0958, list 0x4D095C) the frontmost row first, the
nearest (3D, to the building's +0x2C) with a side to take (pickSide 0x2EF090: of left, right and front, the mount
point (mountPoint 0x2EEDE0: the front-left corner, the front-right corner, or the front's rail nearest the bot)
passing the place check and approachClear, no other monster on that side of it, the nearest). One another monster
climbs counts only when there is nothing else (its distance 1e6), and so does the building it was knocked off, half
the time (`rand01`, the lagged Fibonacci 0x106950 on gWorld +0x82C) [R].

**withTarget (0x2F07F0)** [R]:
- After 60 s on one target: `collapseBuilding` (0x2BB8B0) on it, scored to the bot.
- Mounted: a falling building, jump off. Otherwise the current cell (the mount's, or the last hit's point mapped by
  0x2BB7B0, z at least 6.02, on the front); a cell there not yet destroyed (state < 3) is punched (action 0 with a
  fresh pick, so the punches repeat). Else a target cell: the nearest intact one along the storey left or right
  (distLeft 0x2EF450 / distRight 0x2EF2B0: width + 1 when none, or when another monster is on that rail), a storey
  up when it is more than a column away; equal distances of 2 or more: the first intact cell above; then the first
  intact cell above in the column, else the roof. moveToCell (0x2EF5D0) climbs up (canClimb(+1, 0): 5 mantle, −2
  avoid and jump, else climb up), down, or hops along the face (hops go to the monster's right, or its left when
  mirrored: the other way it first mirrors (action 10); a hop that cannot be made sends it up to the roof).
- On a roof (the building under its body, 0x2D6150): a falling one, off it towards the target (jumping when it faces
  it); standing, it walks to the roof's middle and plays `Ground_Punch_Down`, or with JUMP SMASH (upgrade flag 8) or
  the rage butt-slams it (jumps, then attacks in the air near the middle) (onRoof 0x2F0140).
  The stomp's `damageRoof` (`hitBuildingRoof`, [FORMAT_BDP.md](FORMAT_BDP.md) "The roof stomp") wears the top
  storey down one cell at a time until it drops, then the next, until the building falls: a bot stomping in Chicago 1
  fells a 4-storey building in about 25 s, well before its 60 s guard.
- On the ground: a building of 2 storeys or more is approached and mounted (approachAndMount 0x2EFA90: the walk
  point beside the side (sidePoint 0x2EEF30: the half width out from the side's middle), facing the face ≥ 0.95 and
  on the ground it goes for the side's transition state, mirrored sides swapped; canMount −5 / −6 drops the target;
  −2 / −3, something in the way, it punches from the street); a 1-storey building is punched from the street (the
  nearest front column with an intact ground cell, frontPoint 0x2EF8F0).

**huntMonster (0x2F1730)**, no building left: the nearest other slot's monster (by 2D distance; the game tests its own
HP, not the other's, so a monster down is hunted too). Farther than 15: walk to it; under a climber: back off 10;
not facing it: turn to it; else punch [R].

**walk(pt, mode, building) (0x2EDB40)** [R]: the stick, flat, towards the point. Mode 1 jumps that way. With a
building: a ray (IsBuilding, 0x2ED990) meeting another building steps out into the street (stick (0, −1)), or along it
when already out at y < −5; another monster (not human) standing on the point is punched; one whose body circle
overlaps (0.75 d < r + r') is passed in depth (the stick's y −1 for a player, a mounted one or one further back; else
+1); a front-corner rule nudges it out round a building's front corner (below). Scripted walks (WalkToGoal) pass no building:
they avoid nothing.

The goal does not eat, does not seek the army or the edibles, and has no difficulty constants [R: none of its
functions asks for them].

## Monster against monster (0x2E35D0)

Every frame, for each monster, before its object test 0x2E22B0 (all modes, table 0x4312B0), unless byte 0x4D6A9A is
set [R]:
- the victims: every other slot within 40 that is not the attacker's held / holder monster (+0xB9C0 / +0xB9C4), not
  hidden (+0x109), interruptible (+0xAA2) and not held;
- the attacker's collision spheres on bones of an armed attack record (+0xBD60 + 0x1C × bone; the clip's attack event,
  docs/FORMAT_MONSTER.md): record +0x18 the victims (bits 0 .. 3 the slots, 4 the objects; George 0xF / 0x1F), +0x14
  bit 2 in front, bit 4 behind;
- swept sphere against swept sphere: 0x100230 is the origin's squared distance from the segment between the pair's
  offset last frame (the victim's centres +0x430 when +0x688 is set, else this frame's) and now (+0x6D4), t held to
  0 .. 1; a pair meets within the radii's sum, each × its monster's +0xA78 (else 1; the attacker's last place
  also by its +0x688), and the closest pair is kept (after a bounding test of the two sweeps, 0x102DA0) [R]; as here
  (main.c `monsters_fight`, `seg_dist2`; the radii unscaled);
- the hit: 0x2D6A50(victim, …, level = +0x24, damage = +0x2C (5 a punch, 20 the kick)), the reaction by
  army_reaction_slot's table, the knock: the victim slides +0x30 in +0x34 s on the ground; HP −= damage, at 0 slot 67;
  ignored while the victim is not interruptible, held, at 0 HP, or with cheat flag 4 (0x4D7E68);
- `Monsters::onLightHit` only changes the face; the attacker's follow-up slots (+0x50 / +0x54 / +0x58 by the
  result) when it is interruptible, +0xA59 clear and its scale ≥ 0.3;
- 0x2E33A0 clears the victim's bit: **each attack hits each monster once**. Each bone's 0x1C bytes are the record
  (+0) and its own mask of victims left (+0x18: −1 when armed, 0 when disarmed; +6 … +0x14 eight halfwords −1)
  (0x2E7880); 0x2E33A0(monster, part, victim) clears the bit on every bone of that part (a mask left 0 disarms the
  bone), and the fight calls it for each part of the record that hit (0x2E45A0), so the record's parts share it [R].
  Arming a part again starts its bones afresh. Every monster's `Jump_Butt_Slam_impact` has records of different
  damage armed together (20 on one part with 10 or 16 on others), so the record is the part's, not the monster's.
  The record's +0x50 / +0x54 / +0x58 (the follow-up slots) are −1 in every attack of all 30 monsters.
- **No points or kills** for it, **no teams**, no push-out between monsters (0x2E2C50's box query leaves out flag 0x40,
  0x2E22B0 flags 0x2040), no grabbing another monster (the held / holder fields are only ever cleared) [R].

## The camera and its reach

- **TwoTilts** (0x312B50) frames every slot 0 .. players − 1, bots included, that passes 0x2D69A0: not hidden, not
  dropping in after a respawn (+0xAB5, set by startRespawn, cleared on landing 0x2E1A14 and by cancelRespawn), not
  human (+0xAB4) unless its streaker still exists and is not held. Its point is the monster's matrix root (0x2DAF20).
  A monster holding something switches on HeldObjMinCamDistOffset [R].
- **The middle of the players** (0x336360, each frame in 0x2B6CE0 after gAIMgr 0x3333F0): (min x + max x) / 2 over
  every slot, no filter, kept at gp − 0x6C70 [R].
- **The reach check** 0x336230 (in the place check 0x2DDC40, players only, −9): z ≤ 70 and |x − middle| ≤ 30 [R].
- **The clamp** 0x336290 (in the monster's update, 0x2E1050), for a slot that is not mounted (+0xC644) and not AI: z
  above 70 is put at 70 with its velocities' z kept ≤ 0; x beyond middle ± 30 is put there with its velocities' x 0.
  The bots are never clamped, nor anyone during the NIs (everyone AI then) [R].

## Respawns and lives

- Lives are per slot (campaign: 3). startRespawn (0x2D3930) takes the monster's own, or, with none, one from the
  first slot with the most lives (none taken when that is ≤ 0) [R].
- Every monster drops in at the same point, from the camera alone (0x2D3510(m, −10)) [R].
- **Game over** (state 6, campaign): the lives together fewer than the slots at 0 HP, bots included, so a bot's
  respawns can use up a player's lives [R].
- A bot respawns by itself: its goal heads for `Faceup_Idle_Respawn`, whose transitions ask canRespawn [R; that this
  is the only gate G].

## The block's end, the King modes

- The block's King is the player with the most points alone (game.c, GAME_NOTES.md); in the King modes it gets the
  block, 6 − players blocks make the King of the City, (cities − 1) / players + 1 cities the King of the World [R].
- **ResultsNI** [R]: the winner at (13, −42) facing π; the second (2 players or more) at (5, −38), (3, −38) with 3 or
  more, facing 0.75 π; the third at (−12, −32) and the fourth at (−28, −24), facing 1.25 π; the camera (20, −75, 2) →
  (5, −33, 8), with 3 players (25, −73, 2) → (0, −33, 8), with 4 (28, −73, 2) → (−2.5, −33, 8). Slot 0, if not the
  winner, plays React_Victory; the others taunt at random.
- **EndBlockNI**: each walks to (0.7 x_i, −13.2), then (x_i, −5.2), turns round; after 5 s they are put there [R].

## The rage

One monster rages at a time: setEnergy (0x2E5730) makes a full energy 99.9 while another monster rages or has a
full energy, and the rage's start and the building hits' energy take the same "others full" (edible.c's
`others_full`) [R, EDIBLE_NOTES.md]. The port passes it from the other slots.

## The HUDs

Each city script (Chicago.scr:80 …) loads `Hud<i>.ifc` for every slot, bots included, hidden until BeginBlockNI.
Each shows its slot's points (getPoints(6)), HP, energy with the rage flash, head (`Hud_<monster>_Head`), and lives
(campaign) or blocks won (King modes); with 2 players slot 1's group moves to (502, 60) [R]. hud.c had all of it.

## The score by who did it

`Building::hit` (0x2BC150) keeps the hitter in +0x190 when it is a monster (flag 0x40), else 0; the building's
`Scoring` (0x2BA4B0) pays +0x190 the cell, floor, chain and building points (category 0). With 0, nobody is paid and
the chain resets: so a thrown object's TakeImpact (0x2BC0F0, the object as the attacker) pays nobody. collapseBuilding
pays +0x190 as well. Everything else (army, people, vehicles) goes through addStats with the acting monster's slot
[R].

## The port

- **main.c**: each monster's own state is a `Mon` (`Viewer.mon[4]`, `nmon`, `num_bots`): its place, move graph and
  runner, mount and hands, held objects, power and HP, face, head, feet, AI goals, shadow, HUD, its pad; the world's
  is `Viewer`'s. `setup_players` fills the slots (`--players`, `--bots`, `--monster2` .., or the shell's
  `ShellGame`). A frame (`play_step`): the collision world with every monster's spheres (tag = its slot), the
  players' middle, the bots' view, each monster's step (`mon_step`, from `Mon.pad`), the people, buildings, street,
  army (and through it the vehicles, parade, rail, edibles, bosses and the game) seeing every monster, the HUDs, the
  upgrade object, the camera on every monster it frames. The modules' events go to the monster they name (`ev_mon`;
  an event that names none goes to player 1, as before).
- **city.c**: `Scene.mon[k]` (`SceneMonster`): each monster's instance, clips, head bones and angles, arm IK and foot
  IK; `city_monster` adds the next slot; every `scene_monster_*` takes the slot. A building keeps its `hitter`.
- **Input**: player 1 the keys as before, player 2 the numeric keypad; players 3 and 4 and the bots the computer
  (DRIVE_COMPUTER). `--do` plays player 1, or after `p2` .. `p4` that player.
- **director.c**: `DG_DESTROY`, `dir_ai_destroy`, `dir_ai_on_hit`, `dir_ai_on_mount`, `dir_ai_avoid`; the host fills a
  `DirBotView` each frame (the buildings, the monsters, the bot itself, the climbing code's answers).
- **game.c**: BeginBlockNI's GE_AI says `b = 1` for destroyBuildings.
- **moves.c**: an attack event keeps its record's flags, victims and level for the monsters' fight.
- **people.c**: `people_hit` takes the monster; its events say which monster they are about.

Stand-ins and gaps:
- Players 3 and 4 are the port's own: the PS2's NumPlayers box offers 1 or 2, bots included (SHELL_NOTES.md). Here
  they have no keys and are made AI with the bots' goal.
- Here (main.c `MonAtk`) one record per body part, a bone taking its newest armed part's; the hit marks every part
  of the record. The follow-up slots are not ported (no attack has one). Needing nothing: the interruptible flag (+0xAA2: set to 1 by every clip's start,
  0x2E7F70; cleared only by a clip event no monster has, 0x2E7CC0, or the binding `setInterruptabe` (id 88,
  0x2AB258), which no script calls [R]: always set in play), and held / holder, only ever cleared.
- The bots' rand01 is the lagged Fibonacci 0x106950 on the world's one state (gWorld +0x82C, seeded by 0x1069C0 from
  GameStatus seed[1] at the world's reset) [R], shared with the edibles' rand() (wrand.c); its other users in the
  game (smashObject's 0x2B4390 among them) do not draw from it here. The mount point is at the street, z 0, and min x − 5 on a front under 2 wide (0x2EEDE0) [R]. The
  building's +0x2C is the middle of its foot (0x2BC920: LeftEdgeXPos + 2.5 Width, 2.5 Depth + BackRowOffset, 0) [R].
  The ray (0x2ED990) runs from the bot along the flat way to the point without end, against the buildings (include
  0x2000): a first building other than bld sends it out into the street and walks at once [R]. The cooldown +0x64 is
  set (0x2EEBD0) when the monster's foot lock moved it (0x2E0F54) or its hands lifted it (0x2E1044) [R].
- The front-corner rule of the walk (0x2EE150 … 0x2EE374) [R]: with the point at or behind bld's front (its y ≥ the
  box's +0x1E0) and the monster reaching past it (y + R > the point's y), D = R + √2 A, R = MONSTER_DATA +0x198 and
  A = MONSTER_DATA +0x1A4 plus the kept type-16 event record's +0x10 (+0xBAA0: only the Transition_Ground clips and
  Spider_Jump have one, and a clip's start drops it), both × the scale when +0xA74 is set (0x2E4D00, 0x2E4E40);
  going right (the point's x > the monster's) with the box's left side D or more to its right, or left with the
  right side D or more to its left, the stick less (0, 1, 0) (0x40A018), made 1 long ((1, 0, 0) when < 1e‑6). As
  here (director.c `bot_walk`). The monster loop's tests are 2D here, 3D in the game, with radii from 0x2E4FA0.
- A thrown object hitting a building clears its hitter (Building::hit 0x2BC150 keeps only a monster attacker), so
  nobody is paid for what it fells [R].
- The camera (0x2D69A0) frames no monster that is hidden, dropping in, or human without a running streaker, alone
  too; framing none, TwoTilts (0x312B50) holds its last view [R]. (It also frames the 0x4D3100 tracked objects, not
  passed here.)
- The respawn's lives, game over, the King blocks and the results' places and screens were game.c's and hud.c's
  already; they now get every player.
