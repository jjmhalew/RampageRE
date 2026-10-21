# The game flow: blocks, lives, results

How a city is played: the world's state machine, when a block is won or lost, the monsters' lives and respawns, the
block clock and its time bonus, the score, the results screens and the play modes. Ported in `src/game.c` /
`src/game.h`. Addresses are in RAMPAGE.ELF. **[R]** means read in the code or the files; **[G]** means a guess or a
stand-in.

## Where it lives

| What | Where |
|---|---|
| World state word (`gWorld.getState`, World binding 58) | 0x4D6A80 |
| setState | 0x325BF0 (states 1, 2, 3 and 15 are entered at once) |
| Update, every frame | 0x324340: entering a state (table 0x434040), the NI function, the checks; a state's end (table 0x434020 for 6 .. 11, otherwise state + 1) |
| Finding a script function | 0x324180 |
| Block manager | 0x4D4E80: +0x0 BlockList, +0x4 the book, +0x8 the current block, +0x10 BlockEnd, +0x14 BlockStart (read by 0x303240) |
| activateNextBlock | 0x2B6580 (the world's +0x878 goes on by one; 0x302AF0 makes it current) |
| Block clock | 0x4D5190: 0x347ED0 init from the block, 0x347EA0 start, 0x347E10 stop (the time bonus), 0x347EC0 reset, 0x347DC0 time left |
| Game status | 0x4D7990: +0x210 start block, +0x224 lives[4], +0x148 the checkpoint copy, +0x318 + 0x60 n the players |
| Player | +0x4 points[7], +0x20 kills[7], +0x3C cities won, +0x40 blocks won, +0x44 city score; addPoints 0x33F530, getPoints 0x33F590, clear 0x33F5F0 |
| Common globals | 0x4D7E28 players, 0x4D7E44 play type, 0x4D7E48 the clock's seconds, 0x4D7E4C city destroyed, 0x4D7E4D quit, 0x4D7E4E select, 0x4D7E4F world done, 0x4D7E50 / 54 / 58 King of the block / city / world |
| Monster bindings | becomeHuman (id 146) 0x2D3ED0, canRespawn (147) 0x2D3D80, startRespawn (148) 0x2D3930, cancelRespawn (149) 0x2D36A0 |
| World bindings | dispatcher 0x2AE3D0, jump table 0x42DD70 (index = id + 1), lookup 0x2B02D0 |
| Common bindings | dispatcher 0x2C2A00, table 0x430450, lookup 0x2C3840; GameStatus lookup 0x2C4C30, Player lookup 0x2C4470 |

## The states

| State | Entered | On entry | Ends when | Then |
|---|---|---|---|---|
| 1 | 0x2B7D40 (world start) | SinglePlayer.bsf | at once | 4 |
| 3 | 0x2B7F50 (load) | clears 0x4D7E4C .. 4F | the world starts | (1, then 4) |
| 4 | the start, a continue, a King tie | `BeginLevelNI` on the city's first block, else `ContinueLevelNI` (the same) | the NI returns 0 | 5 |
| 5 | | `BeginBlockNI` | at once (it returns 0) | 6 |
| **6** | | IsBlockFinished is found; the clock starts | the block is finished, the time is up, or game over | 8 if game over, else 7 (attract: leave) |
| 7 | | the block's end (0x3255B0, below); `startResults` 1 / 4 / 7; `ResultsNI` | the screen's result | see below |
| 8 | | `startResults(0)` (the GameOver screen) | the screen's result | continue: 4; otherwise leave |
| 9 | | `EndBlockNI` | the NI returns 0 | 5 |
| 10 | | `startResults` 2 (LevelResults) or 5 (KingCityResults); `EndLevelNI` | the screen's result | see below |
| 11 | | `startResults` 3 (WorldResults) or 6 (KingWorldResults); `EndWorldNI` | the screen's result | quit to the shell |
| 12 | `restartShell` (Pause's quit) | leave for the shell | | |
| 2, 13, 14, 15 | 0x2B8040, 0x2B7EA0, 0x2B7FF0 (RWorldPackage's vtable 0x44A820 +0x3C, +0x38, +0x40: the package's own life, on the world's managers through 0x2889A0 / 0x2886A0 / 0x2888F0; the port has no packages), 0x2B7CB0 (15: `endResults`) | | | |

**From 7** (0x324DE8): every player's block points and kills are cleared. Then: the city done (`+0x18`) in a Timed
Run: leave; the city done: 10; not a King mode: 9; a King of the City: 10; a King of the block: 9; nobody (a tie):
the block index goes back two and activateNextBlock replays the block, state 4.
**From 10** (0x325360): campaign: 11 after the last city (`0x4D7E4F`), otherwise leave (the city select);
King of the City: leave; King of the World: 11 with a King of the World, otherwise the next city (start block 0, city
index + 1) and leave.
**From 8** (0x3250B0): quit or select: leave (Timed Run's select starts over at block 0, otherwise the checkpoint is
restored). Continue: each player gets one life which `cancelRespawn` spends at once, the clock resets, the checkpoint
is restored (three lives each, its start block), the points are cleared, the block index becomes max(BlockStart,
start block) − 1 (Timed Run: BlockStart − 1) and activateNextBlock makes it current; every monster gets full HP
(min(its most, 100000), 0x2DA510) and `setEnergy(0)` (0x325288 → 0x2E5730; a rage is not stopped); state 4.

**Leaving** [R]: every leave (0x324740 and the ends above) makes the app's next package the shell's (+0xA8 =
0x4D53F0), so main.c goes back to the shell on any `GE_EXIT` (a = 0 the menus, 1 King of the World's next city, whose
index b becomes GameStatus' with start block 0, 2 the demo's end). The shell reads what the world left in gCommon:
`isPlayerQuiting` (0x4D7E4D: the main menu), `isChooseMonster` (0x4D7E4E: MonsterSelect), `isCityDestroyed`
(0x4D7E4C: CitySelect, or in King of the World with no King of the World straight to the next city's loading screen),
`getKingOfWorld` (0x4D7E58) and the campaign won (0x4D7E4F: Shell::onInit 0x309500 clears it and plays the Victory
movies, shell state 6); ControlRoom.scr picks the first interface in that order. The Pause menu's Quit
(`restartShell`, state 12) sets none of them: the main menu by default. game.c `game_quitting`,
`game_choose_monster`, `game_city_destroyed`, `game_king`, `game_world_done`; main.c fills ShellStart from them.

**Each frame** (0x324340), in this order:
1. Enter the state if it changed.
2. The NI function: its timer (`+0xC`) counts down by the frame's dt; at 0 it is called with its last return
   (`+0x8`, 0 the first time); a return of 0.001 or less ends it and ends the state.
3. In state 6: `IsBlockFinished()`; if not finished and the clock is armed (`+0x44`) and shows 0: the time is up.
   That is game over when this is the city's last block or a Timed Run, otherwise just the block's end.
4. In state 6, campaign, not yet game over: **game over when the players' lives together are fewer than the monsters
   at 0 HP**.
5. In states 7, 8, 10, 11: `WorldGuiDriver::getResultState`: 1 or 2 end the state, 3 sets select, 4 sets quit (both
   end it too).
6. The attract mode's demo (play type 0), in states 4 .. 11, with gWorld +0x1B set (0x4D6A9B: a pad's button,
   RControllerMgr's input 0x2C1B80): state 6 is stored without being entered, gShell's idle timer is set to 120
   (0x308240) and the state ends (0x324C80); so the demo leaves for the shell, which then shows the main menu
   (SHELL_NOTES.md "The demo game"). game.c `game_attract_button`, `GE_EXIT` 2 with b = 1.
7. If the state ended: its handler. The end of state 6 in the demo leaves at once (0x324D08), whatever ended it.

**The weapons' switch** (gp−0x772C, a byte; 1 in the ELF's data) [R]: set entering states 3 and 5 (0x3243A8,
0x324494), cleared entering 7 (the block won, 0x324510) and 8 (Game Over, 0x324588); its only other writer is the
World binding `globalWeaponEnable` (59, 0x2AF0C0; `isGlobalWeaponEnabled` 60, 0x2AF0D8), which no script calls. Off,
no weapon fires: RBeamWeapon's and RProjectileWeapon's tryFire refuse (0x295CC0, 0x2963EC), the charge does not start
(0x2977BC), and the common update (0x297398) stops the countdown and, while the owner's state (+0x5C) is 2, 3 or 7,
holds the weapon each frame (vtable +0x7C: the burst off, the countdown at least ChargeTime + FiringOffset; the
Shockwave's 0x31BA50 also ends its burst and its FireEffect). So from the block won to the next BeginBlockNI and on the
Game Over screen the army, the bosses and the fire trucks hold their fire, and a monster's roar starts and loses its
burst the same frame (Shockwave's tryFire does not test the flag): no effect, no damage. game.c `game_weapons_on`;
main.c hands it to army.c (`army_set_weapons`: the units' and the bosses' weapons, vehicle.c's stream) and to
`army_mon_weapon_step` (the roar).

**The 3D sounds at a block's teardown and at Game Over** [R]: the teardown 0x2B66C0 ends with enableSfx(0) (0x2B6760:
SoundDevice +2 = 0, the event player paused, +0x48), 0x26D500 (0x274610: every object of the 3D list, SoundEffects
included, killed through 0x26BB30 → 0x2725C0 and freed), the sound's frame 0x26D960 once, and enableSfx back as it
was; entering state 8 does the same (0x324584 .. 0x3245B0). Every loop and object sound of the block stops: sound.c
`sound_objects_clear`, from main.c at GE_BLOCK with b = 1 and at GE_STATE 8 (the extra sound frame, with the event
player paused, makes nothing else heard). Leaving the world does it too: the world package's end (0x2B7CB0,
RWorldPackage's vtable 0x44A820 +0x18) kills every 3D sound (0x26D500) and runs the sound's frame once (0x26D960)
before setState(15) [R]; main.c clears them as the game goes back to the shell. (Up to v0.10.19 the port left them:
the block's loops, a broken hydrant's `Geyser` or a fire's `Fire Loop`, played on in the menus, and the stale
tracks held 3D slots.)

**NI functions** are looked up by 0x324180: the block's string property of that name (the block just won for
ResultsNI, GameOverNI and EndBlockNI, since the next block is already current), else WorldData's, else the name
itself; then as given, as `<City>::<name>`, as `Levels::<name>`. Only New York's boss block overrides any:
`IsBlockFinished = "NewYork::isBossBlockFinished"` and `ResultsNI = "NewYork::BossResultsNI"`. The other cities'
boss blocks name theirs `xxIsBlockFinished`, which the game never asks for. `GameOverNI` is in no script.

## Levels.bsf's NI functions

All of them call `SetupNI(0.25)` on their first call: they run every 0.25 s.

- **BeginLevelNI / ContinueLevelNI**: each monster is reset at x = −17 (n − 1) / 2 + 17 i, y = −5.2, facing π
  (towards the camera), and plays `React_Intro`. The camera goes `Free`, then `TwoTilts`. It ends when every
  monster's move has ended.
- **BeginBlockNI**: each monster is reset; the bots get AI (and `destroyBuildings`), the players lose it. The HUDs
  fade in, `ChallengeText` loads, `gAIMgr.activate(1)`, the camera goes `TwoTilts` over 0.5 s, the rubble effects
  stop. It ends at once.
- **ResultsNI**: the script camera at (20, −75, 2) looking at (5, −33, 8). The block's winner by points
  (`getWinnersByType(0)`) is put at (13, −42) facing π, the second at (5, −38) facing 0.75 π. Three effects go into
  the rubble around (0, −21): `Rubble_Smolder` or `Fire_Loop`, half and half, but always `Fire_Loop` when the point
  found has y of −23 or less. The HUDs fade out. Player 0 plays `React_Victory`. From then on, any monster whose move
  has ended counts down a random 0 .. 15 (later 0 .. 60) ticks and plays one of `Ground_Taunt`, `React_Victory`,
  `Ground_Punch_Down`. It never ends by itself.
- **SkipResultsNI**: the monsters move 41 towards −y (onto the block left behind); the HUDs fade out. No data uses it.
- **EndBlockNI**: each monster walks to (0.7 x, −13.2), then on to (x, −5.2), at 1.5, then plays `Ground_TurnAround`
  and `Ground_Idle`. The camera goes `TwoTilts` over 1.5 s. It ends when every turn-around has ended; after 5 s they
  are put at (0.7 x, −13.2) facing π. Its `resetMonsterState` first (binding 0x97 → 0x2DCB40) starts the graph's
  first state, `Ground_Idle`, as a slot's start (+0x110), unless the monster is defeated, so a taunt of the results
  stops there and the walk (about 3.5 s, the turn 1 s) begins at once; it also sets the scale to 1, the velocity
  to 0 and the speed scale to 1 with no time left (0x2DCBAC: a rage goes on), lets go of what is held and, with foot IK, of both feet (0x33BE90: no anchor, no planted record), clears the
  mount and, with the arm IK, lets the hands go (0x322EC0) [R]. Up to v0.11.1 the port only cleared the goals: a `React_Victory` (3 s) playing
  when the results were left ran on, the walk ran past the 5 s, and the monster was put there facing π as the next
  block began, a snap (main.c `reset_monster_state`).
- **EndLevelNI / EndWorldNI**: everyone taunts. They never end by themselves.

The blocks of a city lie one behind the other, 41 apart along y: (5 × 2 + 5 × 2) + (5 × 2 × 2 + 1) in the scripts.
activateNextBlock already happened when the results play, so the block just won lies at −41: its rubble around
(0, −21), its street around −46. That is where the winner poses, and the end-of-block walk leads from there to the new
street at −5.2. The block manager lays them out again at each activation (0x302800, 0x302AF0) [R]: the new current
block's slot (+0x120) at y 0, the next (+0x1B0) at 41 (0x302F0C), the one after (+0x1C0) at 82 (0x303044); a
building at x = LeftEdgeXPos + 2.5 × Width, y = 5 + its slot's offset (+ BackRowOffset on the back row; 0x305EDC). The
block left behind only gets its rubble (0x303C90(mgr + 0x18, block, 1)): each BuildingInstance with LeftEdgeXPos in
−50 … 50 its template's RubbleShape at y −36 (5 − 41), and its GroundShape at (0, −41, 0) (0x303BB0). There is no
coordinate shift: the game's places stay block-relative. The port keeps the old scene and moves the game's places
by `block_shift`, which looks the same, but buildings left standing after a time-up stay standing here, where the
game shows only rubble. When the scene is rebuilt (`game_next_block`) every place the cameras keep goes back 41 with
it, the script camera's too (`dircam_shift_y`): ResultsNI puts it at (20, −75, 2) looking at (5, −33, 8), in the
new block's frame; left at +41 it stood 8 in front of the winner, looking away from it.

## When a block is won

`Levels::IsBlockFinished`: `gWorld.getCurrentBuildingCount() <= 0`, checked every frame of state 6. The count
(World binding 0x33, 0x2AEE00) is of the block's list (0x4D095C) whose state (+0x14C) is not 0: a falling building
counts until it is down; the list holds only the instances with LeftEdgeXPos in [-50, 50) (0x3059D0 with a2 = 1:
the play block makes no Building of the others) [R]. Boss blocks:

- Las Vegas, San Francisco, London, Los Angeles, Chicago and Hong Kong: the boss block uses the default as well.
  `<City>::isBossBlockFinished` would ask for the boss's `CompletionCallback` (`setBossBlockFinished`) *and* no
  buildings, but no block uses it.
- New York's last block: `NewYork::isBossBlockFinished` returns the flag `setBossBlockFinished` set: 1 when the
  blimp is beaten, 2 when the buildings are gone or the centre building (the one at (0, 10, 1)) is falling or
  destroyed. Either one wins the block.

## The clock

0x347ED0 reads the new block's `TimeBonus` group (or its `ParentPropertySet`'s, `DefBlockProps`) when the block
becomes current:

- `BonusDef`: up to six pairs {seconds, points}. The largest time counts as the bonus time.
- The play type's group, `Campaign`, `King` or `TimedRun`: `TimeLimit` and `ClockVisible`. The books call the King
  group `KingOfCity`, so the King modes never find theirs **[R 0x3F5D44]**.

Then:

- **Timed Run**: the clock's seconds (0x4D7E48) go up by TimeLimit (180 everywhere), so what is left carries over.
  A city's start clears them (0x33DBE4 in 0x33DB90, from the world's start) [R]; a block's win (0x3256A0) is their
  only other writer.
  The clock is armed.
- **Attract**: 60 s, or TimeLimit if smaller. Armed, not shown.
- **Others**: armed only with a TimeLimit of 1 or more. That is the boss blocks in the campaign, with 600 s. Without
  one, the clock counts down the bonus time (480 s) and is always shown, but nothing happens at 0.

The clock starts when state 6 is entered. The time left is the seconds minus the time since the start, never below
0. `gWorld.getGameClockTime` (World binding 82, 0x2AF848) is that time left; `getGameClockLimit` (binding 81, 0x2AF818)
is the clock's +0x30, the TimeLimit read (`game_clock_limit`). It is drawn in states 5 and 6 when ClockVisible is set (Clock.bsf: m:ss, fading in).

**Time bonus** (0x347E10, when the block is won): of the entries whose time has not been reached, the most points.
The default table is < 120 s 5000, < 150 s 2500, < 180 s 1500, < 240 s 500, < 360 s 200, < 480 s 100. In the
campaign each player gets bonus / players as category 4. In a Timed Run what is left of the clock carries on to the
next block.

## Lives and respawning

- **Starting lives** (0x33DB90, a new city; 0x33D820, a restored checkpoint): **3** per player in the campaign; 0 in
  the other modes, which never run out.
- **Defeat**: at 0 HP the monster takes slot 67 `React_Defeated`: from a blow's TakeDamage (0x2D9500 .. 0x2D95F4),
  and from the monster's update itself (0x2E68D0 .. 0x2E6A4C) once it is at 0 HP with no reaction asked, not in the
  air and not yet defeated (+0xA59), so the HP the scripts take with `addHitPoints` (0x2DA530: the bosses, the trains,
  the rockets; it only clamps) fells it too [R]. (Without the update's test the port's monster stood at 0 HP until
  the watchdog's `never_respawns`, HongKong's boss block.) George's graph runs:
  - `React_Defeated` (0.833 s clip at rate 1.2);
  - `Faceup_Idle_Dead` (0.833 s);
  - `Faceup_Idle_Shrinking` (1 s): `setScale(0.2, 0.833)` at 0, `becomeHuman` at 0.763;
  - `Faceup_Idle_Waiting` (3 s at rate 0.333);
  - `Faceup_Idle_Waiting2`, looping until Cross, R2, Square, Triangle or Circle is pressed while `canRespawn` says
    yes;
  - `Faceup_Idle_Respawn` (2 s at rate 0.5): `startRespawn` at 0.16, mode 0 at 0.504, `setScale(1, 0.833)` at 0.513;
  - `Faceup_Idle_Getup`.
- **becomeHuman** (Monsters.bsf): only in state 6. Then `createStreaker` and the monster becomes human (+0xAB4 set,
  object flag 0x20 cleared). In any other state nothing happens, and the monster cannot respawn: canRespawn needs
  +0xAB4.
- **canRespawn** (0x2D3D80) is no when:
  - in the campaign, the players' lives together are 0;
  - the monster has not become human;
  - its streaker is being held (object flag 0x20000).
- **startRespawn** (0x2D3930):
  - **Life**: in the campaign a life is taken: its own, or, if it has none, from the player with the most.
  - **Place**: if the streaker still exists, the monster takes its place and turn and the streaker is deleted. If the
    streaker is gone (deleted once out of view) or no longer on its LineMover (it was smashed and is flying), the
    monster drops in instead. It falls at the point where the camera's look, flattened onto the ground, meets
    y = −10, at the height where the view's top plane crosses that point (gWorld +0x660 + 0x7C is the frustum's
    fourth side plane, 0x16D150: y = +w in view space; 0x2D3510 takes z there, 0 when |n.z| ≤ 0.001) [R]. It
    faces −y, is in the air (mode 1), and plays `Jump_Butt_Slam_fall`.
  - **Then**: HP to the most (MONSTER_DATA +0x138), human cleared, flag 0x20 set. The scale is set to 1 at once
    (+0xA7C = +0xA78 = 1, its time 0: 0x2D3BDC .. 0x2D3C08) only when it drops in, which leaves
    `Faceup_Idle_Respawn` before its `setScale(1, 0.833)`. Where the streaker ran, the monster comes back the
    human's 0.2 and that `setScale` grows it over 0.83 s (the port had it full size at once either way).
- **cancelRespawn** (0x2D36A0) runs at every block's end (0x2DCB40) and on a continue. A monster that is down or
  human comes back where it is, at full HP, for a life. In the campaign with no lives left at all it stays down and
  hidden.
- **Game over** (state 6, campaign): when the lives together are fewer than the monsters at 0 HP. With one player
  that is the moment the last life's monster hits 0 HP. So three lives are four defeats.
- **GameOver.bsf** pauses the game and offers three choices:
  - 0: `setResultState(2)`, continue;
  - 1: `setResultState(3)`, hidden in a Timed Run;
  - 2: quit, confirmed, `setResultState(4)`.
- **Continue** goes back to the checkpoint's block with three lives and the points cleared. The checkpoint is saved
  at every block won (0x33E290; `isAtCheckpoint` is always 1), after the next block was activated, so a continue
  replays the block you died on.

## The block's end, the score, the results (0x3255B0)

1. **The clock**: it stops (the time bonus, as above), then resets.
2. **The block's King**: `king_block` is the player with the most points (category 6) alone. Equal points, 0 for
   everyone included, leave it at −1. Each player's points go into the city total (0x4D7C40, against `ParScore`) and
   their city score (+0x44). The first and second places are kept as well.
3. **King modes**: the block's King gets a block won. At `getNumBlocksToWin` (**6 − players**) it also gets a city
   (King of the City, 0x4D7E54). At `getNumCitiesToWin` (**(cities − 1) / players + 1**, with 7 cities) it is King
   of the World (0x4D7E58).
4. **Monsters still down** come back (cancelRespawn).
5. **The city is done** (`+0x18`) when the block was the last (BlockEnd); otherwise the next block is activated now.
6. **Flags**:
   - city done and city total ≥ ParScore: city flag 2;
   - city done: city flag 1 and `isCityDestroyed`;
   - 0x323240 (every monster of the city's MonsterRevealDef unlocked) sets flag 8; then, when every city's saved
     flags (this one's | 8) have 1, 2, 4 and 8, Boosy is unlocked (0x3259E4, [PROFILE_NOTES.md](PROFILE_NOTES.md));
   - the checkpoint is saved;
   - the campaign's last city done: world done (0x4D7E4F).
7. **The screen**: Results (campaign), TimedRunResults or KingBlockResults; `ResultsNI` runs.

The block's points are cleared when the results end. The HUD's score is `player.getPoints(6)`, so it shows the
block's points.

**Results.bsf**:
1. The intro (about 1.5 s).
2. The count-up, 0.5 s per category: destruction, civilian, vehicle, bonus, time bonus, challenge.
3. The buttons appear: 2.05 s, plus 3 s if saving is allowed.
4. Cross starts the walkout.
5. When it is over: `setResultState(1)`, and the world moves on. 5 s and a 0.5 s fade later: `endResults`.

"select" answers 3 and a confirmed quit answers 4. LevelResults, WorldResults, KingBlockResults, King of the World's
KingCityResults and TimedRunResults answer 1 the same way (after their walkout); King of the City's KingCityResults and
KingWorldResults answer 4 on Continue, which leaves for the menus (the quit flag) [R the scripts;
docs/SCREENS_NOTES.md]. A Timed Run's block shows TimedRunResults: the time left, the next block's TimeLimit added,
the total.

## The play modes and the campaign

`gCommon.getPlayType()`: 0 attract, 2 campaign, 3 King of the City, 4 King of the World, 5 Timed Run.

**Campaign** order (ShellData `CityData.City0..6`):

| # | City | BlockStart .. BlockEnd | ParScore | Night |
|---|---|---|---|---|
| 0 | Las Vegas | 1 .. 9 | 175000 | yes |
| 1 | San Francisco | 1 .. 10 | 175000 | |
| 2 | London | 1 .. 8 | 195000 | yes |
| 3 | Los Angeles | 1 .. 8 | 135000 | |
| 4 | Chicago | 1 .. 9 | 200000 | |
| 5 | Hong Kong | 1 .. 8 | 180000 | yes |
| 6 | New York | 1 .. 10 | 200000 | |

Notes on the blocks:
- The BlockList's entry 0 repeats entry 1.
- The last two entries are buffer blocks: drawn behind, never played. BlockEnd defaults to the list's count − 3.
- `getNumBlocks` = BlockEnd − BlockStart + 1.
- Each city's last block is its boss block: `ChallengeBoss`, TimeLimit 600.
- A city starts at max(BlockStart, the profile's start block for it), as CitySelect sets it.
- After a city: LevelResults, then back to the city select, except after New York: WorldResults.

**King of the City**: the blocks are played in order; each one's King gets a block; 6 − players blocks make the King.
A tied block is replayed. Lives do not run out. The HUD shows blocks won instead of lives.

**King of the World**: the same, city after city in the campaign order. Cities won are kept from city to city (they
reset only when the first city starts). (cities − 1) / players + 1 cities make the King of the World. They live in
GameStatus (0x4D7990, the player's +0x3C), which outlives the world; the port makes a game.c `Game` per city, so
main.c carries them over (`GameConfig.cities_won`, kept at each `GE_EXIT`).

**Timed Run**: TimeLimit 180 per block, adding up. Running out is game over. There is no Continue choice.

## What the port does not have

- Elsewhere: the challenges (challenge.c), the monster reveal and Boosy (reveal.c), the profile save (profile.c),
  the pause (hud.c, PAUSE_NOTES.md), findPointInRubble (reveal.c, GE_RUBBLE_FX).
- The bots' `destroyBuildings` AI is director.c's (MULTIPLAYER_NOTES.md).
- Cheat 874098 (flags 0x10000 | 0x20000): 0x324930 wins a block at once while it is not the last [R]. 0x10000 alone
  also seeds gWorld +0x828 / +0x82C from GameStatus at each activateNextBlock (0x2B65B4: +0x828 = seed[0],
  0x1069C0(+0x82C, seed[1]); the 0x33F720 after it is empty) [R]; as here (game.c `activate_next_block`), and again
  entering state 6 (0x3244C4) [R]. With it the world's start makes every player a bot (0x2B7DC0: Common 0x4D7E2C =
  0x4D7E28; the world's end 0x2B7D18 puts 0 back) [R]: main.c `setup_players`, game.c's check of the bots. That is the
  soak test of cheat 082864 (SHELL_NOTES.md "The soak test"): the world's half 0x2B6350 is hud.c `hud_soak`.
- `NewYork::BossResultsNI` is ported from NewYork.scr: the monsters taunt on the Empire State's roof until the
  centre building falls (flag 1), or Levels' ResultsNI (flag 2). Any other non-Levels script goes to the host's
  `script` callback, falling back to Levels.bsf's version.
- `gWorld.frand` is Park-Miller (0x106BD0 on gWorld +0x828, seeded from GameStatus seed[0] at the world's reset,
  0x2B74D0) [R]; one state shared by every script, as here (wrand.c). GameStatus' two seeds are drawn anew by the
  shell's startGame for every play type but the demo (0x308890: each (s >> 8) & 0xFFFF of a step of 0x106BD0 on
  gShell +0x1C, again while 0) [R], the demo keeping its own; main.c `shell_start_game` (gShell +0x1C's start is not
  found, .bss and no traced writer: 1 there [G]).
- The block left behind's moving objects [R]: every in-game activateNextBlock passes a1 = 1 (0x2AF260, 0x325054,
  0x325258, 0x325924; only the world's reset 0x2B7568 passes 0), which first runs 0x2B66C0: 0x2A5370, the block
  manager's unload 0x302800 (the buildings), 0x4D265C / 0x4D2660 = -1, then every object of RWorld's list (gWorld
  +0x58, next +0x68) without flag 0x200000 gets the world's vtable +0x40 and 0x283BF0 (its destructor, vtable +8, and
  the slot back to the pool 0x4D0000). 0x200000 is set only at 0x2D4EE8 (0x2D4C40, the monster's WeaponList objects,
  from 0x2E8D8C): every vehicle, person, street, parade and army object goes, the monsters' weapons stay. Then
  0x2B6640 re-attaches the new block's script objects to buildings (0x309E20). The usual path is 0x325924, in
  block_won (0x3255B0) as GS_BLOCK_WON begins, before ResultsNI: the old block is gone for the whole Results screen
  and the EndBlockNI walk; only its rubble (0x303C90, the RubbleShapes) and its GroundShape at (0, −41, 0)
  (0x303BB0) are left, and 0x302AF0 lays the new block out at once in slot 0 (y 0; slot 1 at 41, 0x302F0C; slot 2 at
  82, 0x303044). 0x325054 is a tied King block played again, 0x325258 Game Over's Continue (both the same block,
  loaded fresh), 0x2AF260 the script binding (NewYork's CrossParkNI). 0x302AF0 then builds the whole new block at
  once: its buildings (0x3059D0), all its objects (0x3050F0), its actors (0x304450), edibles and tubes (0x304150),
  the AI manager's load (0x334C40), its traffic (0x334240), the challenge (0x3468F0) and the block clock (0x347ED0);
  the bosses, RWorld objects without 0x200000, went with the rest. A tied King block is two activations in a row:
  the next block (0x325924), then the same one again (0x325054). Here [main.c, at GE_BLOCK with b = 1]: `block_left`
  drops what follows the old block (the monsters' held objects, the effects) and the whole scene is built anew for
  the new block once that frame is over (`game_next_block`: the monsters and the cameras 41 back, so the game's
  frame from then on), with the block left's GroundShape at (0, −41, 0) and its buildings' RubbleShapes
  (city.c `city_build_left`, models only: the rubble's points are reveal.c's). The world's reset (b = 0) still
  builds at GS_BEGIN_BLOCK. The monsters' models and move graphs are made again with the scene [G: the game keeps
  its monsters; their places, goals and AI are kept here].
- The results screens are not drawn here: `src/hud.c` has them (hud_results; docs/SCREENS_NOTES.md), fed by these
  queries (game_points, game_kills, game_king, game_blocks_won, game_cities_won, game_blocks_to_win,
  game_cities_to_win, game_city_index, game_clock, game_clock_limit, game_block_name, game_city_destroyed ...).
  `game_auto_results` answers for them after a delay when nothing draws them: GR_DONE, or GR_CONTINUE for the
  GameOver screen.
