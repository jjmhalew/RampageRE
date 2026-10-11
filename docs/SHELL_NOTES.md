# The front end (the shell)

The game's front end as `src/shell.c` ports it (`src/shell.h`): the ControlRoom and its six interfaces from
`Shell.bdp`, the profile popup from `MemoryCard.bdp`, the loading screen from `Loading_<City>.bdp`, and the Victory
screen from `VShell.bdp`. Each runs the logic of its script (`build/scripts/*.scr`) or, for the popup and the loading
screen, of the native code behind them. It also covers what the shell hands to the game. The formats are in
[FORMAT_GUI.md](FORMAT_GUI.md), and the in-game screens are in [SCREENS_NOTES.md](SCREENS_NOTES.md) and
[PAUSE_NOTES.md](PAUSE_NOTES.md). **[R]** means read in the code or the files. **[G]** means a guess or a stand-in.
Addresses are in `RAMPAGE.ELF`.

## Where it lives

| What | Where |
|---|---|
| Shell package | `RShellPackage` (vtable 0x44B170). Init 0x309500 creates the SaveGame popup. 0x308950 activates the package and loads `Shell.bsf`, then calls `Shell::onInit`. Update 0x308710 |
| Shell object, `gShell` | 0x4D51F0: +0x0 state, +0x8 the script, +0xC the ShellData book, +0x10 play videos, +0x14 attract state, +0x18 the idle timer, +0x1C the world seeds' random state, +0x20 / +0x21 attract game / boot completed. The demo's number: gp-0x71C0 (0x44C9B0) |
| `gShell` bindings | lookup 0x306BF0 (records 0x432CE0), dispatcher 0x3062B0, jump table 0x432F50 (index = id + 1) |
| `gCommon` bindings | lookup 0x2C3840, table 0x430450. GameStatus 0x4D7990, its lookup 0x2C4C30 |
| Profile popup (SaveGame) | 0x4D7E80. Interface 0x345D60. Load 0x345700. Save 0x345950. Boot check 0x345BC0. Close 0x345540. Update 0x341940 (panels), states 0x342F30 (table 0x4358F0). Errors 0x341DF0 (table 0x435640), their answers 0x342680. Event split 0x341880. Profile list 0x340B40 / 0x3410E0 / 0x341250. Name entry 0x340980 / 0x340320. Messages 0x3401E0 / 0x33FBB0 / 0x33FD10 |
| Loading screen | 0x4D8250: showLoading 0x3485E0, the dots and percentage 0x348B80 |
| Layouts | `Shell.bdp`: ControlRoom, AttractMode, MainMenu, MonsterSelect, MonsterSheet, CitySelect, Options. Segments 1, 2 and 7 have other MainMenu / MonsterSelect / MonsterSheet versions; the port uses segment 0. `MemoryCard.bdp`: MemoryCard. `Loading_<City>.bdp`: Loading. `Loading.bdp`: Loading_Shell. `VShell.bdp`: Victory |
| Texts | `Shell.loc`, `MemoryCard.loc`, and `Common.loc` (the monster and city names, in `VShell.bdp` and `Loading.bdp`). Fonts and frames are in `CommonGUI.bdp`; `TitleFont24` and `Lemiesz36` are in `Shell.bdp` |
| Data | `ShellData.bdp`'s book: `MonsterData.Monster0..29` (Name, Climb, Jump, Run, DisableFor4Player, NotFinished) and `CityData.City0..6` (City, Name, CampaignNight, GlobeLocation, NumBlocks, Powerup0..3) |

## The shell's states [R 0x308950, 0x308710, the bindings]

| State | Meaning |
|---|---|
| 1 | The boot: the memory card check, then the attract mode's first run |
| 2 | The attract mode after 120 s idle in the menus (0x308390), or back from a game with the idle timer out (a demo's) |
| 3 | The menus |
| 4 | `gShell.startGame()`: the update starts the world package (0x4D2050) and sets state 5; for any play type but 0 it first draws the world's seeds (0x308890, below) |
| 6 | The Victory movies |

- **On activation:** state 0 becomes 1, state 7 becomes 3 (boot completed), and state 6 stays. Otherwise the state is
  2 when the idle timer is 0 or below (0x3089AC), else 3 when the boot is completed, else 1. Then, with +0x20 set (the
  game was the attract mode's demo), +0x20 is cleared and `restoreProfile` (0x33D910) runs (0x308A00).
- **`Shell::onInit`:**
  - In a King of the World game whose city is destroyed, with no King and no quit, it goes straight to `showLoading`:
    the next city.
  - In state 6 it loads Victory.
  - Otherwise it loads the ControlRoom (`VShell`, `Shell`, `ShellData`, the ShellData book).
- **ControlRoom's first interface:**
  - AttractMode in states 1 and 2.
  - Otherwise, in this order of checks: MainMenu after a quit, MonsterSelect after "choose monster", CitySelect after
    a destroyed city, else MainMenu.
- **Interfaces and focus:** all six interfaces are loaded at once. ObjectFader fades them (2 a second), and
  `showInterface` fades the focused one out, the target in, and moves the focus.
- **Idle timer (+0x18):** it starts at 120 s and counts down (0x308710 / 0x308390) [R]:
  - It holds at 120 in states 6 and 7, in state 1 unless the memory card popup is up, while Options has the focus,
    and in the campaign while the monster is to be chosen or the city is destroyed and not quit.
  - A cheat video set (`setCheatFlagsI(16, n)`, 0x4D7E6C ≥ 0) puts it to 0 (0x3087D8), so the movie starts on the
    next frame; the attract state is then 0x80 + the video.
  - When it runs out (outside state 4) the shell clears the quit, choose and destroyed flags, sets state 2 and
    attract state −1, and shows AttractMode. With the popup up it asks 0x341790 first: an interruptible popup (the
    boot's list, before a profile is chosen) is closed with result 3 and the attract mode starts; any other popup
    waits at 0.
  - 0x308390 stores no time: the timer stays at 0 in the attract mode, and in states 2 and 4 it only runs down to 0
    (0x308390 returns at once in state 2 and is not called in 4). The shell's update does not run while a world plays,
    so a demo game starts with it at 0, unless the pad was touched in the last 120 s, and the shell comes back to the
    attract mode after it (above). The port counts the loading screen's half second the same way, without the attract
    mode's start: the same in effect, since the press that started the game reset the timer and a demo's loading
    starts in state 2, where 0x308390 does nothing.
  - It is reset (0x308240) by input the GUI takes: the screen's (RGuiScreen 0x2C5AD0, from GuiManager::input
    0x22F7A0, for a pad the controller table allows: 0x2C5E60) or the popup's (0x2328B0). The script's
    `resetAttractTimer` (0x306B78) is called only by AttractMode.scr. 0x33D740 is not pad input: it is the cheat flag
    0x10000 (cheat 082864), with which 0x307920 runs a soak test of fake GUI presses and the timer stays at 120 ("The
  soak test").

## The attract mode, title and movies (AttractMode.scr) [R]

**The states:** BootCheck, ShortDelay (115 frames of black), MidwayVideo, PipeworksVideo, DigitalVideo, BinkStill
(only on platform 4), IntroVideo, PlayRandomGame, PostRandomGame, CheatVideo.

**What each one does:**
- The video states play `midway`, `pipeworks`, `digital` and `intro` (.pss) through the full-screen `Video` quad.
- Start or Cross skips a video when save files exist; the intro can always be skipped. In state 2, Start leaves for
  the main menu (`exitAttractMode`: state 3, the "Oingo" music).
- PlayRandomGame:
  - In the boot it sets "boot completed" and goes to the main menu.
  - In state 2 it calls `gShell.startAttractGame` (0x308460: a random demo game, below). The attract state stays 7.
- PostRandomGame (in state 2) sets the attract state to 0 and goes on: ShortDelay, the movies, the next demo. It is
  reached when the shell comes back from the demo in state 2: AttractMode's init takes state 7 and its focus
  (`gotoNextState`) moves on to 8.

**The title screen:** there is no separate title screen. In state 2, "Press START button" blinks at 587 418 over the
movies (`setFlags(1024)`). The main menu with the `TITLE_BASE` logo is the title.

**The port:**
- Each movie becomes `SHELL_EVT_MOVIE` with its name. The viewer plays it (movie.c,
  [MOVIE_NOTES.md](MOVIE_NOTES.md)): `shell_movie_hold`, the picture in the `Video` quad by `shell_movie_clock`, and
  `shell_movie_end` at its end, so the shell goes on as at `Video.status()` 3.
- A movie nobody holds lasts `shell_set_movie_time` seconds: 0 by default, so it is skipped at once. In state 2 it
  lasts at least 5 s of black under "Press START", or Start could only arrive during ShortDelay, which ignores it.
  That is the port's: in the game `Video.status()` (GuiQuad id 0x4A, 0x253270 -> 0x243130) answers 3 when the quad
  has no player (+0xEC 0, as a failed `setVideo` 0x243370 leaves it), so the next frame goes on [R].
- The demo game is played (below).

## The demo game (gShell.startAttractGame, 0x308460) [R unless marked]

**The draw.** The demo has a Park-Miller state of its own (0x106BD0 on a local; the world's is not touched):
1. gShell +0x20 = 1.
2. The demo's number (gp-0x71C0, the word 0x44C9B0, 3 in the file) goes on by one; 65 becomes 0. So the 65 demos come
   in turn, the first one after the power-on is number 4.
3. The seed: 0x106C80 seeds it from the clock, but it is overwritten at once by number × 7 + 1; one draw, and the
   seed becomes that draw's (s >> 8) & 0xFFFF (0x4B27 for 0).
4. Then each value one draw, in this order, into GameStatus (0x4D7B90; PROFILE_NOTES.md "A status record"):

| Field | Value |
|---|---|
| +0xB4 changed | 1 (no draw) |
| +0x8 night | bit 0 of (s >> 8) & 0xFFFF |
| +0xC city | s / 2³¹ × the cities' count (0x4D799C, 7), cut to an int, at most the count |
| +0x10 start block | s / 2³¹ × 6, cut: 0 .. 5, a BlockList index as CitySelect's `setStartBlock` (the world starts at max(BlockStart, it)) |
| +0x0, +0x4 the world's seeds | (s >> 8) & 0xFFFF each |
| 0x4D7E44 play type | 0 (no draw) |
| 0x4D7E28 players, 0x4D7E2C bots | 1 + bit 0 of (s >> 8) & 0xFFFF, both: every player is the computer's |
| +0x14 + 4 i the monster, each player | s / 2³¹ × the roster's count (0x4D79A8, 30), at most the count |
| +0x24 + 4 i | 3: the lives (`getNumLives`), not a controller type |

Any of the 30 monsters can come, whatever the profile has unlocked, and nothing keeps two players apart (demo 30:
Natalie twice). The scaling multiplies (float) s by 2⁻³¹, then by the count (0x308518 .. 0x308574).

5. `ControlRoom::startGame` (iCallScriptS): the focused interface goes, the loading screen of the city, then
   `gShell.startGame()` (state 4). There 0x308890 draws new world seeds from gShell +0x1C only when the play type is
   not 0: the demo keeps its own, so each demo plays from the same seeds each time it comes.

**The first demos**, and the cycle's 0 and 1 (the draw worked through for each number; the port's shots agree for
4, 5 and 6):

| Number | City | Start block | Time | Players |
|---|---|---|---|---|
| 4 | London | 5 | day | Harry |
| 5 | Hong Kong | 2 | day | Gilman |
| 6 | London | 0 | day | Jack, Venus |
| 7 | Chicago | 5 | day | Icky |
| 8 | San Francisco | 2 | night | Lizzie |
| 9 | Chicago | 1 | night | Natalie, Philbert |
| 0 | Los Angeles | 0 | night | Lizzie |
| 1 | Las Vegas | 4 | night | Rock |

**In the world:**
- `WorldGuiDriver::process` 8192 loads `Clock` and, with `getPlayType() == 0`, `AttractMode` (WorldGui.bdp's
  AttractMode.ifc, script GAttractMode.bsf: "Press START button", `AttractMode_PressStart`, right-aligned at 587 418,
  yellow, blinking with `setFlags(1024)`; its `Video` lookup finds nothing). Start opens no Pause (event 128 needs a
  play type but 0). The pads do not vibrate (0x2B7FA4).
- The clock: 60 s, or the block's TimeLimit when smaller, armed and not shown (GAME_NOTES.md). BeginBlockNI gives
  every bot its AI.

**The end:**
- The minute up or the block finished: the end of state 6 in the demo leaves at once (0x324D08: the app's next package
  is the shell, 0x4D53F0).
- A button: RControllerMgr's input (vtable 0x44A9F0 +0x8, 0x2C1B80) sets gWorld +0x1B (0x4D6A9B) for any event of a
  control from 4 on (the names at 0x3F67C0: 4 the d-pad's up .. 21 the left thumb; 0 .. 3, below them, are taken to be
  the sticks [G: the ELF names none of them; the sticks are read apart, MenuController.getX / getY 0x24EF80 from the
  floats at +0x10]) of any pad. The world's update (0x324C80), in the demo and in states 4 .. 11, then stores state 6
  without entering it, sets gShell's idle timer to 120 (0x308240) and ends the state: the same leave. The world's
  reset (0x325C50) clears the flag.
- Back in the shell (activation, above): after the time up the timer is still 0: state 2, AttractMode, PostRandomGame,
  the movies and the next demo. After a button it is 120: state 3, the main menu. Either way restoreProfile: the
  current game is the profile's record again (its city, night, start block, monsters, seeds; three lives each). The
  play type (0, which MainMenu shows as the campaign: `max(2, getPlayType())`) and the players and bots stay the demo's.

**The port:**
- shell.c `start_attract_game`: the draw (wrand.c's `wrand_pm_next`, the same float steps) into `ShellGame` (its
  `seed[2]` new), `SHELL_EVT_ATTRACT_GAME` with the number, then the loading screen and `SHELL_EVT_START`. gShell's
  timer, attract state and +0x20 live on while the shell is closed through `ShellStart` (`shell_keep` at the start,
  `idle`, `attract_state`, `attract_game` at the return); `shell_open` sets state 2 or 3 from the timer.
- main.c: `shell_start_game` plays it with play type 0 (game.c `GP_ATTRACT`, hud.c `HUD_PLAY_ATTRACT`: the HUD's
  AttractMode.ifc, no Pause), every player a bot (`setup_players`, game.c's `num_bots` may then be all), three lives,
  the seeds into the profile's current record; a pad button (`attract_buttons`) or a `--do` button word calls
  `game_attract_button`; game.c's `GE_EXIT` 2 (b 1 for the button) goes back, `attract_end`: the timer, profile.c's
  `profile_restore` and the `ShellGame` taken from the restored record.
- The keys that count as buttons are the ones that stand for the pad's controls from 4 on (main.c `keys_pad` and the
  menus: Enter, Space, Backspace, P, J, K, L, R, the keypad's 0 1 3 7 9); W A S D, the arrows and the keypad's 8 4 2 6
  5 / * are the sticks, which do not count [R in effect].
- [G]: the GUI's own resets of the timer by pad 0's events while the demo plays (RGuiScreen 0x2C5AD0, e.g. a stick
  pushed past the menus' threshold) are not modelled, only the button's; a draw at the count itself (a float rounded
  up to 1: the game keeps it, past the list, and what its lookups then do was not followed) is taken as count − 1.

## The profile popup (SaveGame, MemoryCard.ifc) [R unless marked]

**Boot check (BootCheck → `gShell.doBootCheck`, 0x306300):** once, in shell state 1, with byte 0x4D5211 clear. It
calls 0x345BC0(popup, 0x128): with flag 0x100 already set nothing (the check reads as done); else the interface,
SaveIcon, request 3, the KB needed, op 12 and popup state 13 [R]. The popup's state is +0x2BC (update 0x342F30, table
0x4358F0: 2 warn-continue, 3 list, 4 name entry, 5 check and write, 6 load, 7 save, 8 an error's answer, 9 format, 10
no card, 11 confirm overwrite, 12 the boot card check, 13 the autosave notice, 15 delete; 14, no data, is never
stored); `isBootCheckDone` (0x306338) is state 1 [R].
- **State 13, the autosave notice, at every power-on** whatever the card holds: `autosave_rect` alone (0.25 s before
  an answer), Cross only (or the attract mode's timeout, result 3), then state 12 [R]. The boot check sets the
  interface's second effect, `AutoSaveIcon`, to `SaveIcon` (0x345C08); the notice starts it (0x3430E8) and the answer
  stops it (0x343118): the memory card icon in the notice's frame, at the top (320, 91) [R; ported].
- **State 12, the card check** (0x3432E0): status 0 with saves, status 20 (unformatted; `mcUnformattedAtBoot` is never
  used), or no saves and room enough: closed silently (close(1) in mode 12 sets only 0x100); no saves and no room:
  mcNoSpace (Retry / Don't Save); any other status: mcNoCard ("At least %dKB") with Retry / Don't Save. Retry waits:
  the card watcher (0x345100) starts state 12 again when the status changes; Don't Save goes to state 2, which with
  flag 4 clear closes at once (close(3)) [R].
- **The room** [R]: a save needs 140 KB, a fixed sum (0x345BC0 → +0x48, 0x1A1120: 1 for icon.sys, 134 for the
  disc's RAMPAGE.ICN, 1 for the 0x128-byte data, + (files + 3) / 2 + 2), kept at popup +0x17C and in 0x4D7FFC, which
  every popup text gets through sprintf (0x33FECC: mcNoCard and mcNoSpace say "At least 140KB"). The card's free KB
  is +0x4C (0x1A11E0, sceMcGetInfo's free; 0 without a card); the saves counted are this game's (+0x8C). State 12:
  status 20, or 0 with saves, closes; else room is 0 < 140 < free (strict), closing, or mcNoSpace with Retry / Don't
  Save; status 5 can never reach mcNoCard (the branch tests 21, which +0x5C never answers). Retry waits for the
  card's status to change (the watcher restarts state 12); Don't Save goes to state 2, which with autosave off
  closes (close(3)). The list's Create New Profile runs the same test first (0x343D38, +0x2C4 = 11): error 11, mcNoSpace
  with Continue alone (op 3), which goes back to the list, the op's state (0x342850). A write fails below 140 free
  (0x1A2E00, error 11): mcNoSpace with Retry (the op's state: the write again) / Don't Save (state 2, warn-continue:
  with autosave off it closes, close(3)).
- The port: the notice, then the room test against `shell_set_card_free` (the viewer: `--card-free-kb N`, else the
  free room of the drive the profile file is on, looked at once a second; only the tests against 140 read the free
  KB, no text shows it [R 0x33FECC: the texts get the 140], so it needs no 8 MB card's cap;
  unset or nothing written: room enough). Retry at the boot runs the test again [no PS2 meaning: the host's room
  comes back without a card status change, which the game's Retry waits for]. No card and unformatted have no host meaning; corruption is checked at the load (error
  18). An autosave from the game (Results) that finds no room is not shown: nothing is written.
- Neither state sets autosave (flag 4), so the main menu that follows restores the profile and does not open the
  list (MainMenu.scr:207 asks `getShowProfile() && getAllowSave()`); the list opens from Back ("Profiles") [R].

**Profile list (`gCommon.loadProfile` → 0x345700 with "create allowed", flag 0x10):**
- When it opens:
  - from Back on the main menu ("Profiles");
  - from the main menu's focus while `gShell.getShowProfile()` (0x4D7FF8 & 0x200) and `getAllowSave()` hold.
    `getShowProfile` (0x30697C) is `!(0x4D7FF8 & 0x200)`, clear in .bss, so it shows at the boot. Only the popup's
    close (0x345540) sets the bit: any result but 3 (the attract mode's interrupt) sets 0x300, the boot's check
    (op 12, the autosave notice) only 0x100 (boot done) [R].
- Contents (0x341250): item 0 "Create New Profile", then the saved profiles. The layout has six items; copies of item 0
  make more. The "loc" (card slot) child stays hidden.
- Display (0x340B40):
  - the bar under the selected item, at the item's y + 2;
  - `profile_name` shows the selected item's text;
  - the up and down arrows show when the list scrolls (six visible).
- The pad (0x3410E0):
  - Up and down move with `Rollover`.
  - Cross picks: item 0 opens the name entry (state 7), any other loads that profile (state 8). Both play `Click`.
    A load closes with 1 in op 3: flags 4 and 8 set (0x34559C).
  - Back is state 9 (Cancel), then state 2, warn-continue.
- A pick is state 6 (0x343D60: the name to +0x188; a save's request picks state 7), LOADING, whose read is the port's
  `SHELL_EVT_PROFILE_LOAD` (the index into `ShellProfile.profiles`); a read that works sets `allow_save` and
  `was_saving` in the viewer (close(1) in op 3 sets 4 and 8).
- The attract mode's interrupt (0x341790) closes the autosave notice, or the list while show-profile (0x200) is
  clear [R].

**Warn-continue message (0x3439E8) [R]:**
- With flag 0x20 set (flag 8 and a name held as the list opened) or flag 4 clear it closes at once, close(3): the
  list just goes.
- Else the text: a name held, "Autosave Turned Off" with Don't Save; none, "WARNING: No Profile" with Continue.
  `btn_select` (Cross) reads Back and returns to the state it came from (op 3, the list); `btn_cancel` (Triangle)
  reads Continue / Don't Save: flags 4 and 8 cleared (0x343B00), close(2). The port gives `SHELL_EVT_PROFILE_NONE`
  there.
- The list's cancel leads there: its panel state 9 goes to 0x343DF8, sub-state 2.

**Name entry (0x340980, 0x340320):**
- `keyboard`: `profile_id`, `profile_name` with a blinking caret, and rows `row0`..`row3` of 11, 11, 10 and 11 keys.
  The colours come from the hidden texts `color_normal` and `color_selected`.
- Left and right move along a row, going round. Up and down go to the next row (round) that has a key in that column,
  then the column is taken modulo the row's count.
- Cross on a key:
  - DONE cuts trailing spaces. An empty name gets `Cancel`; otherwise the entry is done.
  - BACKSPACE rubs one out.
  - SPACE adds a space, but not as the first character.
  - Any other key adds its text, up to 15 characters.
  - Each change plays `Click`, and the caret moves to the text's width.
- Back is state 14: sub-state 3, the list, built again (0x343FE8) [R].
- A name already saved shows "The Profile name already exists." [R]: DONE does not check it (state 4 sub-state 6,
  0x343F90: the text to +0x188, the card id to +0x2CC, state 5). State 5 shows CHECKING (a list request) or SAVING,
  opens the file by name (sub-state 1, 0x3441D8; the directory is crc32 of the name), and a handle back (or error
  18) with op ≠ 9 is error 24 (0x344398; answers 0x342BD0): for the list's request Cancel (Cross: state 3, the list
  again, the name lost) and Overwrite (Back: state 11); for a save Retry (state 5 again), Don't Save (state 2) and
  Overwrite (button0: state 11). State 11 (0x343868) asks "OVERWRITE FILE?" with NO (Cross: state 3 for the list,
  5 for a save) and YES (Back: op 9, state 5, writing into the file). A list request's new-profile hook (+0x2A0 =
  0x33F0F0) runs 0x33F630 on the copy first (0x344444), so the old file gets a fresh profile. A create closes with 1
  in op 5: flags 4 and 8 stay. The port runs state 5 as the game does ("The memory card's states" below): CHECKING,
  then the name looked for among the saved ones (the case counts, as the directory's crc32 does), then SAVING and
  the write (`SHELL_EVT_PROFILE_CREATE`).
- The port's event: `SHELL_EVT_PROFILE_CREATE` with the name.
- `shell_type` is not in the game: it types a character straight in.

**Messages panel (0x33FBB0):**
- Cross gives 0x12 while the autosave notice or `btn_select` shows.
- Back with `btn_cancel` gives 0x13.
- Button 0 with `btn_button0` gives 0x14.

## The memory card's states [R]

**The panel (0x33FD10)**, called with the title, the text, a time, t0 and the three buttons' texts:
- The answer (+0x350) and the answer passed on (+0x354) become 0xF (none); with no button at all the bottom bar is
  hidden and the answer is 0x11, "done". The time goes to +0x358. Each text goes through sprintf with the KB a save
  needs (140).
- t0 (always for the list's request, 0x4D8134 = 1) shows the `messages` group and `background_quad` (0x4D8214), at
  fade 1.0 for the boot's request (3) and 0.6 otherwise (0x33FE30); without it both are hidden: only the SaveIcon
  shows. The group's fade goes to 0 (0x3401A8).
- The rect (0x3400C0) [R]: the buttons' row is b = the largest max(30, a shown button's text height − 5) (0 with
  none); `message_rect` keeps its layout's height (+0x3C, 0x244460 at the setup 0x3402E4: 235) unless the text's y +
  its height + 20 + b is more, then that; `button_group` goes to that height − b − 5 (200 for 235 and one row of
  buttons, the layout's 195 being the authoring tool's). As the port's `mc_panel`.

**The popup's update (0x341940):** the panel's group (+0x2D0: 1 the list, 2 the name entry, 3 the messages) fades
in at 4 a second, the other two out (0x341990 .. 0x341C94); the three start at fade 0 (0x3402F4, 0x340AEC,
0x345F0C). In panel 3 the time runs down and an answer is passed on once it is up (0x341C98). After a close (panel
0) all three fade out and the interface goes when they are gone and the SaveIcon is no longer running (+0xB4 &
0x200, 0x341DA4). `gCommon.isSaveGameActive()` (0x2C2F30) is the state (+0x2BC) not 1: false at the close itself.

**The states** (+0x2BC, sub-state +0x2C0, op +0x2B8, request +0x2B4: 1 loadProfile, 2 saveProfile, 3 the boot).
Every store of state 7 becomes 5 once flag 0x80 (an error was shown) is set; the close clears it (0x345690).

| State | What it does |
|---|---|
| 5, check and write (0x344020) | op 5. Sub 0: flag 0x80 or the list's request: CHECKING (0.25 s, shown); else SAVING (1 s, shown unless flag 8, 0x3440C4) and the SaveIcon started. Sub 1, once the group's fade is 1 (0x344138): the card id (error 22), the card's status (vt +0x60, 0x1A0EE0), the name's file opened (0x3441D8): there (or status 18) without +0x2C4 = 9, error 24 (0x344398); not there: created (0x3442C8). SAVING again (1 s, shown after an error or for the list) at fade 1 (0x344334, 0x344418), the time stamp, the list's new-profile hook, the async write (sub 3, 0x3444A0). The callback (0x3424F0): sub 4, the file closed, the card's status (error), else sub 2 (0x342608). Sub 2: the answer 0x11 (the SAVING time out), then close(1) (0x3444F8) |
| 6, load (0x344510) | op left as it was (3, the list's). Sub 0: LOADING (1 s, shown), flag 0x1000. Sub 1 at fade 1: open, read, the data's CRC and size (0x3445AC .. 0x34469C); bad: error 18 (0x344714); no file: error with the status (0x344728). Sub 2: 0x11, close(1) |
| 7, save (0x344770) | op 7. Sub 0: flag 0x80: CHECKING; else SAVING shown unless flag 4 (0x3447FC): an autosave shows the SaveIcon alone; the SaveIcon started. Sub 1 at fade 1: the card id (error 22), the card's status, open (no file: the status), the file changed since its load (CRC against +0x2A4: error 23, 0x344AA8), the stamp, SAVING again if 0x80, the async write. Sub 2: 0x11, close(1) |
| 8, an error's answer (0x342680) | below |
| 11, overwrite? (0x343868) | NO / YES (0.25 s). In op 5: NO, a save's request state 7, else the list; YES, +0x2C4 = 9 and state 5 |
| 2, warn-continue (0x3439E8) | above; Back is the op's state again (0x343B38) |

**The errors (0x341DF0, table 0x435640):** each stops the SaveIcon (0x341E10), sets flag 0x80 and shows its message in
state 8 (0.25 s; the title mcErrorTitle unless said):

| Error | Message | Buttons (Cross / Back / Square) | Answer (0x342680) |
|---|---|---|---|
| 4 | mcWrongDevice | Retry / Don't Save | Retry: the op's state; Don't Save: state 2 |
| 5 | (no card: state 10) | | |
| 7 | op 6 mcRemovedOnLoad, op 7 with flag 0x40000 mcFailOnSave, op 9 mcFailOnFormat (Continue), else mcGeneralIOError | Retry / Don't Save | as 4 |
| 8, 15, 19 | mcBadEncoding | Retry / Don't Save / Format | (0x342D08) |
| 10, 11 | mcNoSpace | op 3: Continue; else Retry / Don't Save | 11: Retry (Continue) the op's state, Don't Save state 2; 10 none |
| 17, 18 | mcCorruptTitle: mcCorrupt | Retry / Cancel | Retry: state 6; Cancel: the list |
| 20 | mcUnformatted | Retry / Don't Save / Format | (0x342D08) |
| 22 | mcCardChanged, or with 0x4000 mcWarningTitle: mcRemovedBeforeSave | Autosave / Don't Save | (0x342B18) |
| 23 | mcFileChanged | Retry / Don't Save / Overwrite | (0x3429D0) |
| 24 | mcDuplicate | the list's: Cancel / Overwrite; else Retry / Don't Save / Overwrite | the list's: Cancel the list, Overwrite state 11; else Retry state 5, Don't Save state 2, Overwrite state 11 |
| any other | the error becomes 2: mcCorrupt | Retry / Don't Save | as 4 |

The write's callback clears 0x40000 before it raises the card's status (0x3425D8), so a failed write shows
mcGeneralIOError, not mcFailOnSave. After a failed autosave, Retry runs state 5 (0x80 is set): CHECKING, then the
profile's own file is found, error 24, and the save goes on only through Overwrite and YES [R; as the port].

**The card's statuses** (PS2MemoryCardManager +0x4, vt 0x444EE0) from libmc's results (0x1A1550 .. 0x1A1E40, tables
0x417900 .. 0x417990): -3 11 (no room), -2 20 (unformatted), -4 6 (no such file), -5 5, -6 .. -8 1, below -9 7 (an
I/O error); a bad CRC 18 (0x1A19B0); the write's room below 140 KB 11 (0x1A2E00).

**The SaveIcon** (the interface's `<e>` `SaveIcon` at (-16.25, 11.75, 40), the bottom left at (66, 423)):
saveProfile (0x345950) sets it to `SaveIcon` (MemoryCard.bdp's SAVEICON.EDF: three looping emitters, the card
picture `Memcard_icon` with a soft additive copy behind it and the red `Mem_arrow` dropping into it every 0.5 s).
States 5 and 7 start it with SAVING, an error and the close stop it (its particles die with their emitters).
saveProfile loads the interface onto the screen there is (0x345990 -> 0x345D60): over the world's screens too.

**The port** (`mc_update`, `mc_card_update`, `mc_error`, `mc_answered`): the panel, its fades, times and answers,
states 5, 6, 7, 8 and 11 and the errors as above. The card is the viewer's profile file: each read and write is a
request (`SHELL_EVT_PROFILE_LOAD`, `_CREATE`, `_SAVE`) answered by `shell_card_result` with the status:

| The host | Status | The popup shows |
|---|---|---|
| read: no file | 6 | error 2: mcCorrupt, Retry (the list) / Don't Save |
| read: not a profile (its sizes, magic, CRC, data size) | 18 | Corrupt Profile, Retry / Cancel |
| write: cannot open or write the file | 7 | mcGeneralIOError, Retry / Don't Save |
| write: the drive is full (ENOSPC) [no PS2 meaning: the card's no room] | 11 | mcNoSpace |
| the free room below 140 KB (`--card-free-kb`, the drive's) | 11 | mcNoSpace (the room test, as before) |

A file that is there but no profile is still listed (by its header's name), as a card's directory would be.
The game's screens get the popup alone (`shell_open_popup`: MemoryCard.bdp over the viewer's CommonGUI.bdp): the
Results' saveProfile shows the SaveIcon over the screen, an error's message takes the pad, and the Results hold
their answer while it is up (`hud_set_save_active`). With no card meaning: the card id and its watcher (errors 22,
the 0x4000 / 0x80000 flags), the file changed since its load (23), unformatted cards and formatting (state 9,
errors 8, 15, 19, 20; a PC drive has nothing to format and no state the game reaches needs it), the wrong device (4)
and no card (5, state 10).

## The main menu (MainMenu.scr) [R]

**Layout:** the logo `TITLE_BASE`, then the `icons` group. It holds:
- the four play modes: campaign, King of the City, King of the World, Timed Run;
- the play-mode text (Atrox60);
- the buttons Select, Profiles (Back), Options (button 1 / triangle) and Bonus Content (button 0 / square).

**Play mode:**
- Left and right change the mode (2..5, going round).
- The selected icon glows green and grows to 1.4 at 2 a second. Its alpha is 0.75 + (1 + sin 6t) / 8. The others
  are red and shrink to 0.9.

**Cross: the number of players.**
- The box `NumPlayers` lists "1 Player" (shown for the campaign and the Timed Run) and "2 Player". "3 Player" and
  "4 Player" show only on platforms 1, 4 and 8, never on the PS2 (2).
- With one player, or in the campaign, the shell sets the players and bots (bots 0) and goes to MonsterSelect:
  - `restoreCheckpoint`, campaign only (left to the viewer);
  - `setupControllerTable(-1)`;
  - `MonsterSelect::setupFromMainMenu`.
- With two or more in another mode, the box `NumBots` asks for "Human: n / Computer: m". Its title is
  `MainMenu_<n>Player`. Its list box (81 × 46) is a scrolling window (0x23BF10: the items from the top index that
  fit its height, each its font height + `sp`; the update keeps the selection in view), and one LEMIESZ36 item fits,
  so only the selected one shows [R; where each item's y is placed was not read].

**Back:** state 0 calls `loadProfile`. In the boxes it goes one box back.

**Options (button 1):** `ControlRoom::fadeInInterface("Options")`.

**Bonus Content (button 0):** the "Midway Classics" box, with Rampage and Rampage World Tour. Ballers is in the NTSC
English version only: `gApp.getPlatform() == 2 && gApp.getLanguage() == 1 && gApp.getRegion() == 0` [R]. The
language is gApp +0x70 (0x28B268), the .loc column (1 English, 2 English 2, 3 Japanese, 4 French, 5 German, 6
Italian, 7 Spanish, 8 Dutch, 9 Portuguese; the port's `--lang` + 1; `== 7`, Spanish, moves the main menu's buttons
5 down and the Options' rumble list); the region 0x28B200 is SYSTEM.CNF's BOOT2: SLUS 0, SLES 1, else 2
(0x10D290), 1 for this disc, so no Ballers box and cheat video 3 is the credits. Cross starts `gShell.startEmulator(n)` after 0.25 s:
- It is 0x3063A0: `desi/m1` or `desi/jarvos` through `LoadExecPS2`, with `-game rampage` / `rwt`.
- The port gives `SHELL_EVT_EMULATOR` and returns to the menu.

**The pad in the menus** [R] (MenuEventHandler 0x169880 .. 0x16A870, a base of GuiManager / RGuiManager; per pad
0x3C bytes at +0x10, made by 0x16A810): each pad event becomes a code (0x16A430): a button going down by its logical
slot (the table 0x40F130): Up 1, Down 2, Left 3, Right 4, A (Cross) 5, B (Circle) 6, Start 7, X (Square) 8, Y
(Triangle) 9, Z 10, LTrigger (L2) 11, RTrigger (R2) 12, W 13 (Back, the shoulders and the thumbs none); the left stick
by flicks (0x16A5B0, per axis): it rests at 0.15 (inside 0.15 it goes back there and disarms), 0.45 past its rest it
fires the way it went and arms, armed it follows the stick out and disarms 0.45 back from there, disarmed it follows
it in (0.45 out again fires again). GuiManager::input (0x22F7A0, the table 0x422410) turns 1 .. 7 into Up, Down,
Left, Right, Accept, Back, Start and 8 .. 13 into Button 0 .. 5. The code that came last is held (+0x20, from +0x24,
last +0x28); every frame (0x169910's event 2, the clock +0x1F4 += dt) 0x169E90 sends it again once it is 0.5 s
(+0x30) from the press and 0.125 s (+0x34) from the last, while it is held: its d-pad button, or the stick armed that
way (up / down beyond 0.15, left / right beyond 0); only the four directions are. main.c's `pad_menu` is this, over
the pack's axis (`pad_axis`); the keys' menu presses are the port's own.

**Cheat entry:**
- It opens when pad 0 holds exactly L2 + R2 (`getButtons() == 128 | 64`) [R]. getButtons (0x16A180) sets a bit per
  held logical button in the order of the names at 0x3F67C0: Up 1, Down 2, Left 4, Right 8, Start 16, Back 32,
  RTrigger 64, LTrigger 128, RShoulder 256, LShoulder 512, A 1024, B 2048, X 4096, Y 8192, then W, Z and the thumbs;
  the PS2 pack (0x1A5220, table 0x3B18B0) maps Select to Back, L2 / R2 to the triggers, L1 / R1 to the shoulders,
  Cross A, Circle B, Square X, Triangle Y. The viewer's F2 holds L2 + R2.
- The `CheatMenu` box has six digits. Left and right choose a digit; up and down change it. Each digit's monster
  picture changes with it.
- Cross checks the code. Back plays `Cheat_back`.

**Cheat codes:**

| Code | Effect |
|---|---|
| 000000 | Clears the cheats |
| 141421 | Flags 1 \| 2: every monster and every city |
| 271828 | Flag 2: every city |
| 082864 | Flag 65536 |
| 874098 | Flags 65536 \| 131072 |
| 071767 | Flag 1 << 30 |
| 986960 | Flag 4 |
| 011235 | Flag 8 |
| 000020 | Flag 1 << 29 |
| 66730n | Movie n through the attract mode's CheatVideo |
| 24902n | Start at block n + 1 |
| 314159 | Shows the version text |

The port reports the cheats as `SHELL_EVT_CHEATS`. What the flags do: [PROFILE_NOTES.md](PROFILE_NOTES.md) "The
cheats".

### The soak test (082864, flag 0x10000) [R]

With the flag set the game plays itself through the screens, as a test that runs on and on. The shell's update
(0x308710) holds the idle timer at 120 (no attract mode) and calls **0x307920** each frame; the world's update calls
**0x2B6350** (running 0x2B6A30; paused 0x2B6AF0, with the app's pause flag +0xA0 & 0x10). Each counts down its own
global from 5 (gp−0x71BC, gp−0x7334; .data) and acts every sixth frame on the interface on top: the first active
popup (GuiManager +0x3C4, 0x22F6F0 / 0x22F680), else the screen's focused interface (+0x238 → +0x24), and only once
that one's fade (+0x5C) is 1. A press is 0x22FE30(gGui, 0, pad, event, button): the popup's input (vtable +0x14),
else the screen's (+0x5C, 0x232F60 → 0x2385F0: GuiEvent(event, pad, button)).

The shell's half:
- AttractMode: Start. MemoryCard (the popup): Back.
- MainMenu: Common's cheat flags |= 1 (every monster, written straight to 0x4D7E68); then while NumPlayers shows
  (fade above 0, acting at 1): np_selection's selection (+0x6C) 0 → Down, 2 or more → Up, 1 → Accept (2 players);
  else while NumBots shows: nb_selection above 0 → Up, else Accept (no bots); else the `campaign` icon: its colour
  (+0x7C) with G below R (red: not chosen) → Right, else Accept.
- MonsterSelect: on its first frame after any other screen (gp−0x71B8, 1 in .data) the next pair of the roster
  (Common +0x10, 0x4D79A8 = 30 records) into gp−0x6CB8 / −0x6CB4 (0x307C18 .. 0x307D30): first (0, 15), (1, 16) ..
  (14, 29) (gp−0x71B4, 1 in .data, marks that pass), then every pair with player 1's above player 0's in order, then
  every one with it below, and round again. Then for each of "2 Player"'s Player0 / Player1 whose `head` shows the
  body (its texture's name has `_Body` / `_body`: not ready): its `name` text against the pick's localized name:
  the same → Accept, else Right, from that player's pad.
- Any other screen (CitySelect, the loading screen, the Victory movies ...): Accept, and the next MonsterSelect takes
  the next pair.

The world's half: MemoryCard (the SaveGame popup): Back; `Results`: nothing while its `timebonus` line is hidden
(the points' lines show one by one), else with `select` shown and the world's block index (+0x878) + 1 a multiple
of 3 Button 1 (Save), else Start; any other name with `esult` in it (strstr 0x32D818: LevelResults, WorldResults,
the King modes', TimedRunResults) and GameOver: Start. The HUD and the Pause menu take nothing.

With it the world's start makes every player a bot (0x2B7DC0, [GAME_NOTES.md](GAME_NOTES.md)) and reseeds the
world's randoms at each block (0x2B65A4, 0x3244C4): a two-player campaign of the computer's, city after city, each
game a new pair of monsters. The cheat is only entered in the main menu, so the boot's popup (whose notice takes
Cross alone) is never under it. Ported: shell.c `soak` (from `shell_update`, the main shell only), hud.c `hud_soak`
(main.c `soak_step`, every world frame) [G: the port sees MonsterSelect's cards only when two play; the loading and
Victory screens are taken as interfaces of other names: Accept, which the loading screen ignores and which ends a
Victory movie, as the port's pad does].

## The options (Options.scr) [R]

**Layout:** the same as the Pause menu's options page: Sound, Music, Vibration ON / OFF, and Cancel / Accept.

**Controls:**
- Up and down choose the row. The chosen row's title pulses at 1 + 0.1 sin 4t.
- The held left / right (or the stick) move the slider one step per frame.
- On the vibration row, left and right move between ON and OFF, `incrementSelection(dir, 1)`: no wrap (0x23E360 →
  0x23E480), so right on OFF and left on ON stay; the 1 is only the list's end sound played once, and no layout gives
  a list sounds. Going from OFF to ON buzzes the pad (`rumble(250)`).

**Events:**
- `setSfxVolume`: once the sound slider has moved and then stopped.
- `setMusicVolume`: at every step of the music slider.

**Leaving:**
- Accept saves (`gCommon.setSoundVolume / setMusicVolume / setEnableRumble(.., 1)`, `saveProfile`). The port gives
  `SHELL_EVT_SAVE_SETTINGS`, read with `shell_settings`.
- Back restores the old values.
- Either way, it waits until the popup is gone and then calls `fadeOutInterface("MainMenu")`.

## The monster select (MonsterSelect.scr) [R]

**The cards:**
- One card per player, from the interface's group `<n> Player`.
- Player n's card base is `Card_base_<n>`.
- The front:
  - the picture `<Name>_Body`, which becomes `<Name>_Head` once ready;
  - the name;
  - the four upgrade stars (flags 1, 2, 4, 8).
- The back:
  - the name and the head;
  - "MUTATION: <MS_MUTATION_<Name>>";
  - the three bars Climb, Jump and Run;
  - the four upgrades in yellow (had) or grey.
- Each card's highlight border pulses in the player's colour (green, orange, blue, pink). It is grey once the player
  is ready.

**Which monsters are offered:**
- `isMonsterAvailable(player, m)`: `getMonsterFlags(player, m) & 0x80`, or cheat flag 1 for a finished, non-blank
  monster.
- `DisableFor4Player` (Venus, Natalie, Jill, Eyegore, Cal) are left out with four players.
- `gCommon.getMonsterFlags` (0x33D760) returns the profile's flags (GameStatus +0x234 + 4 m). For players 1..3 it
  returns a buddy's flags when one is loaded (+0x2B7 + 31 n set; the byte at +0x299 + 31 n + m).
- Each player's list holds the available monsters.
- The start comes from `GameStatus.getMonsterIdx(i)`. A taken or unavailable start is replaced by the first free one
  of monsters 0..5. This keeps the script's quirks: it compares list positions and tries monsters 0..5 without testing
  availability.

**Controls:**
- Left and right move to the next monster no other player has.
- Cross readies the player. When all are ready, `setMonsterIdx` is set for each player, then:
  - "choose monster" with the city not destroyed: back into the game (`ControlRoom::startGame`);
  - King of the World: city 0, block 0, start;
  - otherwise CitySelect.
- Back un-readies, or (outside "choose monster") sets `setPlayerQuiting(1)` and returns to MainMenu.
- Button 0 flips the card: 0.125 s down to edge-on with `setScaleX`, the other side, 0.125 s back.
- Button 1 opens the card sheet.
- Button 4 (R2, "Load Monsters", two or more players):
  - player 1 sees the P1 dialog;
  - another player gets `gShell.loadBuddyProfile`: the popup for that player, then the port's
    `SHELL_EVT_BUDDY_PROFILE` and `shell_buddy_profile(n)`.

**Bots:** player 1's pad picks for each bot in turn once its own card is ready (`remapP0Controller`). Only
`players − bots` pads talk.

## The card sheet (MonsterSheet.scr) [R]

**Layout:**
- Thirty copies of `card_template` (scale 0.425), 130 apart: row 0 the even monsters, row 1 the odd.
- Four columns show. The sheet scrolls in 130 steps, and the arrows show at the ends.
- The stick moves the sheet 16 per frame times its tilt.
- A dark `black` overlay covers the cards that are not available or that another player uses. The player's "Pn" or
  "Bot n" stamp sits on those (fade 0.35).

**Controls:**
- Cross takes the card (`MonsterSelect::setMonsterFromSheet`).
- Button 1 zooms in to the big card, a 1 s fade. In the zoom, button 0 flips it.
- Back returns.

`nMonstersInUse` starts {0, 0, 0} [R]: an `int[3]`'s elements are made by the array resize (0x1C81A0, loop
0x1C8300) through TypePInt's 0x1CDA40, a PInt of 0. So monster 0 (George) is taken in the 8192 init and again in the
first 512's black pass before the reset to −1: George's card stays dark on the first visit, still selectable
(`validSelection` runs after the reset). As here.

## The city select (CitySelect.scr) [R]

**The city bar:** seven pictures (`<City>` and `<City>_Night`), 128 apart. Four show, and the bar scrolls.

**Each city's flags (`Profile.getCityFlags`):**
- 0x80 unlocked; "LOCKED" otherwise, in the campaign.
- Four stars: all blocks, the par score, the challenges, the monsters revealed.
- Cheat 2 unlocks all.

**The news line:** "...A MONSTER IS HEADING TOWARDS LAS VEGAS..." in capitals (`CitySelect_Message_Single` or
`_Plural`). It shows for 1 s and hides for 0.25 s.

**Controls:**
- Left and right move the frame `city_rect`.
- Outside the campaign, up and down switch day and night. In the campaign, each city's `CampaignNight` decides.
- Button 0 shows the stats box (campaign, unlocked).
- Cross sets the city, the night and the start block, then starts:
  - the start block is the cheat's block, else 0 outside the campaign, else `Profile.getStartBlock(city)`.
- Back returns to MonsterSelect, after `setupFromMainMenu`.

**The globe** (the GuiShape `globe`, `CITYSELECT_GLOBE.PWK`; "The GUI's 3D objects" below):
- At the start, `setIdentity` and shown. Each selection takes the city's `GlobeLocation` (degrees: latitude,
  longitude, 0) as `lookAtTarget` in radians, and `moveDir` is the unit step from the `lookAt` there.
- **`updateGlobe`**, every frame: `lookAt` moves 0.05 along `moveDir` while that brings it nearer, and `setLookAt` turns
  the globe. Within 0.5 of the target, the city's ring (`fx_<City>`, `MAP_HIGHLIGHT.EDF` on the globe's node
  `Node_<City>`) starts. Moving on stops the last city's ring, as does losing the focus.
- **`updateDayNightGlobe`**: the light `lights` gets ambient = diffuse = 255 and specular (0, 55, 0) by day, 100 and
  (0, 5, 0) by night.
- The box `basic` (M 0x90, BASIC: a soft white glow on black, colour 0x494949, 260 × 228 at 105, 102; no script
  touches it) is additive [R]: with M 0x80 the shader pick 0x17D100 takes descriptor 0x415940 / 0x415790 (ids 0x72 /
  0x70), whose records (114 / 112) use GS state 0x17: ALPHA 0x48, Cs × As + Cd, TEST 0x5000D. So a faint grey sheen
  over the globe; Pause's icons and ChallengeText's `icon1` (M 0x90) are additive too (`HudQuad.additive`).

## The loading screen (0x3485E0, Loading.scr) [R]

**`gShell.showLoading()`:**
- Loads `loading_<city>.BDP` with `loading.ifc`, the city from GameStatus. Without it, `loading.BDP` with
  `loading_shell.ifc`.
- Fades the music out over 0.5 s.

**Loading.scr:**
- The picture `LOADSCREEN` comes in over 0.5 s:
  - from (610, 17) at scale 0.1 to (320, 240) at scale 2;
  - turning to −4.15π, that is −27° (`yaw`);
  - while the black fill fades in at 2 a second.
- Then the text fades in over 0.1 s, and `gShell.startGame()` follows: state 4. The port gives `SHELL_EVT_START`.
- `bNoIntro` (Loading_Shell) skips the intro.

**The dots (0x348B80):** one more every 0.25 s, 0..6. `ProgressText` ("%d%%") is hidden by its layout.

## The hand-over to the game (GameStatus) [R the bindings and the scripts]

`shell_game()` holds what the shell set:

| Field | Comes from |
|---|---|
| Play type | `gCommon.setPlayType`, the main menu's mode: 2 campaign, 3 King of the City, 4 King of the World, 5 Timed Run (game.h `GP_*`); 0 the demo |
| Players and bots | `setNumPlayersAndBots`: Common 0x4D7E28 / 0x4D7E2C |
| Monsters | `GameStatus.setMonsterIdx(i, m)`: `MonsterData.Monster<m>` and its Name |
| City | `setCityIdx`: `CityData.City<i>` and its City |
| Start block | `setStartBlock`: GameStatus +0x210, 0 = the city's BlockStart |
| Night | `setNight` |
| The world's seeds | GameStatus +0x0 / +0x4: drawn at the start (0x308890) for every play type but the demo, which keeps its own (`ShellGame.seed`) [R]; main.c `shell_start_game` (GAME_NOTES.md: gShell +0x1C's start is not found) |

A game that ends hands back, through `ShellStart`:
- quit (0x4D7E4D);
- choose monster (0x4D7E4E);
- city destroyed (0x4D7E4C);
- the King of the World;
- the campaign's victory.

These decide the first interface, as above.

## The port (shell.c)

**The GUI core:** it is hud.c's (fonts, texts, layouts, rects, list boxes, faders), copied, because hud.c keeps it
static. It is widened in four ways:
- **Affine frames:** each object's frame is a full 2D affine map. `<x>` turns and mirrors draw, as do the scripts'
  matrix calls (the GuiObject bindings: lookup 0x257940, dispatcher 0x2569B0, table 0x425750) [R]:
  - `setScale(m)` (0x2570B8): the 3×3 becomes m times the identity: the turn and a `setScaleX` go;
  - `setScaleX(v)` (0x2572F0): the first row becomes (v, 0, 0), the others kept: the x axis is v long, not v times
    the scale (MonsterSelect's cards, `m` 1.15 with `setScaleX(1.15)`, and MonsterSheet's `blowup` are 1.15 wide,
    no more);
  - `yaw(a)` (0x2571F0 → 0x240AE0): Rz(a) (0x1015B0) times the 3×3, a turn on top of the one there (Loading.scr calls
    `setScale` first each time);
  - `findObject` (0x242200) is depth first among the descendants, each before its children, not the object itself;
    `copyTree` (0x256BD0) a deep copy with F | 0x10, which `setParent` (0x2426F0 → 0x242540) appends as the last
    child.
- **3D objects:** shapes (`<s>`) and effects (`<e>`) put a quad with tex `HUD_TEX_3D` where they stand in the tree, and
  `shell_3d` lists them in that order: the shape's model, or the effect's edf, with its world in the GUI camera's
  (each object's full 3×3 and place, `<m>`, `setIdentity`, `setLookAt`, the `<DD>`s' pixels in GUI units), a shape's
  light set, an effect's state. Light sets, lights and child interfaces draw nothing. The scripts find all of them by
  name. hud.c's reader gave the properties of an `<e>` to its parent, such as MainMenu's fires' `<x>` to the interface.
- **The viewer draws them** (main.c `gui3d_step`, `gui3d_layers`; render_gl.c `draw_gui3d`): at each marker the GUI
  camera, the shape's model from its interface's bundle (each mesh on its node at the rest pose), lit by its set, and
  the effect's particles (fx.c over that bundle and fx.bdp, `fx_spawn_frame` / `fx_place` in the effect's frame, an
  effect on a node in that node's frame on the drawn model). The depth is cleared once a frame; the 2D quads write
  none: they go through the same camera at depth 1 and CitySelect's full-screen background is drawn before the
  globe at depth 10, which still shows (the GUI's begin, 0x231900, sets no depth state of its own) **[R from the
  picture]**. The frame's scale reaches the particles' places, not their sizes [R]: GuiEffect's draw (0x2376E0)
  hands the instance its whole matrix, scale and all (+0x70 to method +0x6C, kept as is at ParticleSystemFx +0x2C,
  0x1F1D60; CompositeFx 0x1EA1A0 keeps it and passes it on; the emitter copies it, 0x1F831C; the spawn points go
  through it, 0x2128C8 .. 0x2129D0). A billboard's axes are size × size_mul times the camera's rows (0x22233C .. 0x2223D8),
  the matrix only on its place (0x222400 .. 0x222504); a streak's ends go through it, so its length scales, its width
  (size × size_mul, 0x1F769C) not. Every menu effect's scale is 1 but the city ring's (0.971, billboards) and the
  card glow's (1.32, streaks), as fx.c draws them.
- **The 2D path:** an object outside every `<DD>` has its position in screen widths about the centre, x right and y
  down, and a `<DD>` inside it draws about (320, 240), moved there and scaled by it: the GUI camera at depth 1
  **[R]**. The 1 / (1 + z) of a deeper `<o>` is left out: no shell `<DD>` lies in one. No picture is drawn outside a
  `<DD>` either: the only ones there, MonsterSheet's `card_template`, are hidden and drawn as copies inside `cards`.
- **Widescreen:** an untextured quad over the whole 640 × 480 (the popup's `background_quad` dim, the fills) is drawn
  across the viewer's wider window [no PS2 meaning: the game's screen is 4:3].
- **No depth sort:** the layouts draw in tree order **[R 0x241770]**. `<z>` is the fade **[R 0x22C450]**:
  MonsterSelect's title (0.388) and frost border (0.58) are partly see-through, MonsterSheet's `blowup` (0) starts
  unseen.

**Quads:**
- `shell_quads_turned` gives four corners per picture.
- `shell_quads` gives hud.h's `HudQuad`: each picture's bounds, with the texture coordinates of its corners, and its
  four corners themselves (`corners`), which the renderer draws: a turned picture keeps its turn, so MonsterSelect's
  side frost tiles (turned 90°, `<x>`) fade inwards as the top and bottom rows do.

**List boxes** [R; the same as hud.c's, PAUSE_NOTES.md "The list box"]:
- `setSelection(i)` (0x23E480, the script's and `incrementSelection`'s): at or before the first item the first shown
  one, at or past the last the last shown one (none: unchanged); between, item i if it shows, else the nearest shown
  one onwards in the move's direction, then back the other way (none: 0). `incrementSelection(d, once)` (0x23E360)
  is `setSelection(selection + d)`: no wrap.
- `isEnabled(i)` (0x253A18 → 0x23E9F0): item i, or with i < 0 the selected one, is there, shows and is enabled (its
  record's byte, 1 from 0x23BB70; no script calls `enableItem`). So MainMenu's up on the top item asks
  `isEnabled(-1)`, true, and plays Rollover with the selection staying.
- Every frame the list's layout (0x23CF60) gives a hidden selected item's selection to the next shown one after it,
  else the nearest before it; sets the item colours (the selected the highlight one, the others the normal one, the
  hidden the disabled one); grows the selected item to `<growth>` (0x23CB20: MainMenu's player counts 1.2, up at 4,
  down at 16 a second), kept in place by its justify and origin.
- The draw (GuiListBoxControl 0x23C250): the background, then with a `b highlight` / `b endcap` its GuiLine (ctor
  0x23BA10, the pieces 0x23A8E0) across each line of the selected item's text: from pad + max(0, endcap width × s −
  pad − the line's width / 2) before the line to as far past it, pad the `d highlight` x × s, s the item's scale
  times its growth; the band the line's height + the `d highlight` y × s across; the endcap one picture wide at each
  end (u 0 at the outer edge, the right one mirrored), the segment (HighlightEmpty, clear) between; in the
  selector colour, with `setHighlightPulse(1)` times the GUI's pulse (0x242950's, as F 0x400). The profile list
  has no `b`: no line, its bar instead.
- A list whose items stand at one place (MainMenu's `nb_selection`, the human / computer counts) shows the selected
  item alone: in effect the list's window (0x23BF10, the top at +0x124) holds the items that fit from its top,
  scrolled to the selection, and its 46 holds one [R in effect; the window is not ported].
- GuiRect (0x244500, rebuild 0x2449F0): the corners only when the first has a picture; the sides always, GuiLines B
  wide at B / 2 in, the left down, the bottom left to right, the right up, the top right to left (v 0 the inner
  edge); top and bottom from B to W − B, left and right from B to H − B, or the whole height without corner
  pictures (MonsterSelect's card highlights: closed corners).

**Pad events:**
- Pad events go to the popup when it is up, else to the focused interface if `setupControllerTable` lets the pad
  talk.
- The frame event goes to the focused interface only, unless the object has F 0x80 [R]: the tree update (0x2415E0)
  stacks each interface, and GuiObject's update (0x2416C0) sends 0x800 to an object with a script when it has F 0x80
  (0x2416E8), else while the stack's top is the screen's focused interface (GuiManager vtbl +0x30, 0x231440). The
  shell's menus have no 0x80; Loading and ControlRoom (0x82) run alone.

## The GUI's 3D objects [R]

What the front end's GuiShapes, lights and GuiEffects do. Classes (type at +0x64): GuiObject 0x447930 (0), GuiShape
0x447AC0 (3, ctor 0x2489E0), GuiEffect 0x447670 (4, 0x237B30), GuiLightSet 0x447730 (9), GuiLight 0x447790 (0xA),
Gui2D 0x447590 (0xB), GuiCamera 0x447610 (0xC); GuiCamController 0x447300 (0x227DA0) with GuiFixedCam 0x447330 (the
default), GuiFreeCam 0x447350 and GuiTrackCam 0x447310. The tag handlers: 20-byte records at 0x3B3240, the handler of
tag i at 0x3B28A0 + 16 i + 8; short and long names share them (`la`/`lighting` 0x22BDC0, `k`/`attach` 0x22BF20,
`E`/`eflags` 0x22B1B0, `x`/`xform` 0x22DEC0, `z`/`fade` 0x22C450). Children are appended in XML order (0x242540).

**The camera** (0x227AD0, GuiFixedCam 0x227F80): the GUI draws through the GuiCamera's view (controller +0x158,
0x2319DC).
- Vertical field of view 0.71808 rad: tan(fov / 2) = 0.375. Near 1, far 10000. The aspect is the viewport's times the
  float at 0x4CB54C (renderer 0x4CB480 + 0xCC, read at 0x16B768), set by the display's setup to 0.75 × the frame
  buffer's width / height (+0x14 / +0x18, 0x25E6B4): the buffer's own shape taken out, a 4:3 screen **[R]**. The port
  keeps the vertical field and widens the view with the window, as the 2D.
- The eye at (0, −1, 0) looking at the origin, z up.
- Each interface's frame is followed by rows (1, 0, 0), (0, 0, −1), (0, 1, 0) (+0x78, ctor 0x239230): GUI (x, y, z)
  is the world's (x, z, −y). A GUI point lands at px = 320 + 640 x / (1 + z), py = 240 + 640 y / (1 + z).
- A top-level `<DD>` maps its pixels to (X / 640 − 0.5, Y / 640 − 0.375, 0) (0x2363F0): the screen at depth 1. `<a>`
  bit 0 on a `<DD>` gives a pixel view of its own; no shell layout has one.
- Checked: the four players' card glows fall on their cards to a pixel; the globe's `<o>` at (0, 0, 9), m 0.971 is
  at the screen's middle at depth 10, about 220 px across its radius, inside its 474-px shadow.

**Frames:** 3×3 rows (row vectors) at +0x08, the place at +0x2C; the world is local × parent's (0x23FF70). `<x>` is
the rows, then the place; `<p>` the place; `<m>` multiplies the 3×3 as it is when read.

**GuiShape `<s>`** (0x22E4F0: `m`/`mesh` the model, `s`/`skeleton`; load 0x2480E0):
- It draws its whole model (0x246A10): with a node tree of the model's name, each part on its node, posed by the
  model's own animation (the globe's is one key).
- Its draw matrix turns the model's axes into the GUI's (0x240790: rows r0, r2, −r1): model x right, y into the
  screen, z up on screen. With the interface's matrix, a model under identity frames is drawn in its own axes.
- `<la>` names a light set found earlier in the interface; it becomes the renderer's lights (0x2394B0), and without one
  the lights are off (0x16E320). Shapes ignore the fade.
- `setIdentity` (method 0x2B, 0x2573F0): the 3×3 is the identity, the place kept.
- `setLookAt(p)` (0x38, 0x257868 → 0x101330(p.z, p.x, p.y)) replaces the 3×3, no scale, with
  Ry(p.y) · Rx(p.x) · Rz(p.z) in the GUI's axes: a GlobeLocation (lat, lon, 0) turns the globe about the screen's
  vertical (the model's pole) by the longitude, then tilts it about the screen's x by the latitude. Every city comes
  out on the centre line, a few degrees above the middle (so the cities' GlobeLocations fit this turn). `getLookAt` (0x37,
  0x101220) inverts it.

**Lights:** `<ls>`'s own `<A>` is the set's ambient (0x16E1F0). `<lt>` keeps diffuse (+0x158, default 128 grey),
specular (+0x168) and ambient (+0x178); the colours read RRGGBBAA, as the script's night values are the file's. Its
direction is row 2 of its full world (0x239D60), into the screen and to the right for CitySelect's, the direction
the light travels [R]: 0x16DF50 copies it to the renderer's light (+0x40, normalised by 0x259C10), 0x25AEB0 writes
−d into the VU1 light matrix (neg.s at 0x25AFAC .. 0x25AFC4), and the lit program 0x37A2A0 takes max(0, N · (−d))
(MAXx at 0x1A0). Up to three lights (0x25AEB0 / 0x25B200: i < 3). With +0x6C bit 0 the matrix is another object's
(gp − 0x6CC4 → +0x240 → +0xEC), not ported. `setAmbient` / `setDiffuse` / `setSpecular` go through
0x255390. Lights update when hidden.

**GuiEffect `<e>`** (0x22E730: `auto`, then `edf`):
- Its flags (+0xB4): 0x1 manual (`auto="false"`, `<E>` 0x1), 0x2 its matrix pushed in the update, 0x200 running,
  0x400 to start, 0x800 to stop. `<E>` replaces them.
- `setEffect` (0x2377E0) makes the instance on the game's effect manager (0x48A340 via 0x208220: the same
  ParticleSystemFx / CompositeFx as fx.c), owned by the GuiEffect and not in the world's list; it is to start unless
  manual.
- The update (0x237590): one to start that was drawn once starts; one to stop stops emitting; a running one advances,
  and is no longer running when its instance is finished. The draw (0x2376E0) places the instance at its draw matrix
  (the shapes' axis turn too) and draws it while it runs. A hidden effect is never drawn, so it never starts.
- `startEffect` (0x237550) starts an idle one; `stopEffect` (0x237520) stops a pending or running one, whose particles
  play out.
- `<k>` names a node of the parent shape (0x2400F0); the effect then sits in that node's frame on the drawn model
  (0x247280, 0x24D8C0), turning with the globe.

**Drawing:** each screen draws its interfaces in order, each in tree order (0x241770): an object with F & 0x201, or
fade × z ≤ 0.01, is skipped; F 0x40 draws its children alone. F 0x1 is hidden, 0x80 the frame event even unfocused (0x2416E8), 0x100 an update
after the children; **F 0x4 is never tested**. The GUI's begin (0x231900) clears the screen (0x25C610, flags 7, depth
1.0), turns the lights off (0x16E320), sets the draw scale state 0x4CB52C to 1 and switches to its view (0x16DEE0)
**[R]**; it sets no depth test or write of its own. Gui2D's draw (0x2361E0) only sets its pixel view (0x2363F0 /
0x22F780).

## Not ported, or guessed

- **Not drawn:**
  - the emulators, after v1 ([ARCADE_NOTES.md](ARCADE_NOTES.md); the movies are played: [MOVIE_NOTES.md](MOVIE_NOTES.md); the demo game too, above);
  - the main menu's two searchlights: `auto="false"`, and no script starts them **[R]**;
  - the GUI effects' sounds: none to play. No emitter of the menus' effects (the fires, the lava lamp, the rings, the
    card glow) names a sound, so fx.c's sound start never fires for them (traced through the main menu, MonsterSelect
    and CitySelect);
  - the globe's brightness step at Greenwich, where its two halves meet: it stays with the lighting off, so it is in
    the halves' own art, not the port's lights.
- **Popup:** the card's states are ported ("The memory card's states"); not the ones a PC has no meaning for (the
  card id, formatting, unformatted, no card, the file changed). The list scrolling beyond six profiles: the viewer
  has one profile file (`ShellProfile.nprofiles` 0 or 1), so `mc_open_list` keeps the top at 0 and the arrows stay
  hidden. A buddy's load (MonsterSelect's R2, gShell.loadBuddyProfile 0x3077B0 → 0x345700 with no "create
  allowed") [R]: the list without Create New Profile (0x341508: item 0 hidden and disabled without flag 0x10), request 1
  and flag 0x20 as loadProfile's; the pick goes to state 6, LOADING, like any load (0x343D60), and then
  `SHELL_EVT_BUDDY_PROFILE`; its callback (0x307FF0) at the close. The viewer has one profile file, so its answer
  (`shell_buddy_profile(-1)`) loads nothing.
- **Show-profile's life** [R]: bit 0x200 starts clear (.bss) and is set only at 0x34561C, on any close whose result is
  not 3 (mode 12 sets 0x100 only), buddy loads too; nothing clears it. As the port's `mc_close(result)`; the viewer
  keeps it (`ShellProfile.profile_shown`, `shell_profile_shown`) for the next shell. A buddy list's Back is any list's
  (0x343DF8: state 2, warn-continue): with flag 0x20 or autosave off it closes with 3 [R].
- **Bits and flags** [R]: `getButtons` (0x16A180) sets bit i from the pad's button byte i + 4 (names 0x3F67C0): Up 1,
  Down 2, Left 4, Right 8, Start 16, as shell.h; flag 0x400 is 0.6 .. 1 at 1 Hz (0x242950: 1 − (0.2 + 0.2 sin), read
  by the text, quad, line, rect, slider and list draws); a rect without `<B>` is 5 (setBorderWidth 0x244410 from the
  `B` tag 0x22A9E0; 12 rects use it); `<sh>` copies word 0 into OFont +0x18 (the .fdf's shadow colour, AABBGGRR) and
  words 1, 2 to +0x12 / +0x13 (dx, dy) (0x229FF0) — that its leading 3 is a count is taken from `ep`.
- **The white bars** [R]: untextured 0xFFFFFE quads without M 0x200 (the title boxes of MonsterSelect, 419, 48,
  222 × 26 at fade 0.29, and MonsterSheet, 436, 41, 202 × 26) are drawn: neither the colour's handler (0x22C650 →
  0x22B530) nor GuiQuad's draw (0x2439E0) tests that colour.
- **Focus** [R]: setFocus (0x238970) sends the old one 1024 (not when it is the new one), stores the slot, then sends
  the new one 512 even when it had it; as `cr_focus`. The idle timer's inputs are read (`shell_input_pad`).
- **Attract mode:** its 5 s stand-in per movie in state 2 (above); the demo's [G]s ("The demo game").
- **Sizers** [R 0x248B80, PAUSE_NOTES.md]: the button rows (the main menu's, Options', MonsterSelect's,
  MonsterSheet's, CitySelect's, the popup's) are laid out every `shell_update` (`layouts_lay`). English comes out
  at the layouts' places for the main menu, Options, MonsterSheet and CitySelect; MonsterSelect's row and the popup's
  were saved with the French texts (e.g. "Liste Des", "Sélectionner" fit them to 0.0005), so in English they now
  move to where the game puts them: MonsterSelect's four spread evenly over its 550.
- **Segments:** the layouts' segments 1, 2 and 7 (other languages or video modes); segment 0 is used. (The tag `se`,
  0x22C160, is a GuiLine's texture, not a segment.)
- **Engine calls:** `restoreCheckpoint` and `restoreProfile` are left to the viewer.
