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
| Shell object, `gShell` | 0x4D51F0: +0x0 state, +0x8 the script, +0xC the ShellData book, +0x10 play videos, +0x14 attract state, +0x18 the idle timer, +0x20 / +0x21 attract game / boot completed |
| `gShell` bindings | lookup 0x306BF0 (records 0x432CE0), dispatcher 0x3062B0, jump table 0x432F50 (index = id + 1) |
| `gCommon` bindings | lookup 0x2C3840, table 0x430450. GameStatus 0x4D7990, its lookup 0x2C4C30 |
| Profile popup (SaveGame) | 0x4D7E80. Interface 0x345D60. Load 0x345700. Boot check 0x345BC0. Event split 0x341880. Profile list 0x340B40 / 0x3410E0 / 0x341250. Name entry 0x340980 / 0x340320. Messages 0x3401E0 / 0x33FBB0 / 0x33FD10 |
| Loading screen | 0x4D8250: showLoading 0x3485E0, the dots and percentage 0x348B80 |
| Layouts | `Shell.bdp`: ControlRoom, AttractMode, MainMenu, MonsterSelect, MonsterSheet, CitySelect, Options. Segments 1, 2 and 7 have other MainMenu / MonsterSelect / MonsterSheet versions; the port uses segment 0. `MemoryCard.bdp`: MemoryCard. `Loading_<City>.bdp`: Loading. `Loading.bdp`: Loading_Shell. `VShell.bdp`: Victory |
| Texts | `Shell.loc`, `MemoryCard.loc`, and `Common.loc` (the monster and city names, in `VShell.bdp` and `Loading.bdp`). Fonts and frames are in `CommonGUI.bdp`; `TitleFont24` and `Lemiesz36` are in `Shell.bdp` |
| Data | `ShellData.bdp`'s book: `MonsterData.Monster0..29` (Name, Climb, Jump, Run, DisableFor4Player, NotFinished) and `CityData.City0..6` (City, Name, CampaignNight, GlobeLocation, NumBlocks, Powerup0..3) |

## The shell's states [R 0x308950, 0x308710, the bindings]

| State | Meaning |
|---|---|
| 1 | The boot: the memory card check, then the attract mode's first run |
| 2 | The attract mode after 120 s idle in the menus (0x308390) |
| 3 | The menus |
| 4 | `gShell.startGame()`: the update starts the world package (0x4D2050) and sets state 5 |
| 6 | The Victory movies |

- **On activation:** state 0 becomes 1, state 7 becomes 3 (boot completed), and state 6 stays. Otherwise the state is
  3 when the boot is completed, else 1.
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
- **Idle timer (+0x18):** it starts at 120 s and counts down. It is reset while Options has the focus or the popup is
  up [R 0x308390]. When it runs out (outside state 4) the shell clears the quit, choose and destroyed flags, sets
  state 2 and attract state −1, and shows AttractMode. The cheat video (0x4D7E6C ≥ 0) makes it 0x80 + the video. Any
  pad input resets it [G: 0x33D740 taken to be that].

## The attract mode, title and movies (AttractMode.scr) [R]

**The states:** BootCheck, ShortDelay (115 frames of black), MidwayVideo, PipeworksVideo, DigitalVideo, BinkStill
(only on platform 4), IntroVideo, PlayRandomGame, PostRandomGame, CheatVideo.

**What each one does:**
- The video states play `midway`, `pipeworks`, `digital` and `intro` (.pss) through the full-screen `Video` quad.
- Start or Cross skips a video when save files exist; the intro can always be skipped. In state 2, Start leaves for
  the main menu (`exitAttractMode`: state 3, the "Oingo" music).
- PlayRandomGame:
  - In the boot it sets "boot completed" and goes to the main menu.
  - In state 2 it calls `gShell.startAttractGame` (0x308460: a random demo game).
- PostRandomGame starts over.

**The title screen:** there is no separate title screen. In state 2, "Press START button" blinks at 587 418 over the
movies (`setFlags(1024)`). The main menu with the `TITLE_BASE` logo is the title.

**The port:**
- Each movie becomes `SHELL_EVT_MOVIE` with its name. The viewer plays it (movie.c,
  [MOVIE_NOTES.md](MOVIE_NOTES.md)): `shell_movie_hold`, the picture in the `Video` quad by `shell_movie_clock`, and
  `shell_movie_end` at its end, so the shell goes on as at `Video.status()` 3.
- A movie nobody holds lasts `shell_set_movie_time` seconds: 0 by default, so it is skipped at once. In state 2 it
  lasts at least 5 s of black under "Press START" **[G: a stand-in]**. Without that, Start could only arrive during
  ShortDelay, which ignores it.
- The demo game becomes `SHELL_EVT_ATTRACT_GAME` and is not played.

## The profile popup (SaveGame, MemoryCard.ifc) [R unless marked]

**Boot check (BootCheck → `gShell.doBootCheck`, 0x306300):** once, in shell state 1. The port shows the autosave
notice (`autosave_rect`, mcWarnAutosave, Continue) and the boot goes on once it is accepted. **[G: the memory card's
own checks (no card, unformatted, corrupt) have nothing to check here.]**

**Profile list (`gCommon.loadProfile` → 0x345700 with "create allowed", flag 0x10):**
- When it opens:
  - from Back on the main menu ("Profiles");
  - from the main menu's focus while `gShell.getShowProfile()` (0x4D7FF8 & 0x200) and `getAllowSave()` hold.
    **[G: the port sets show-profile at the boot and clears it once a profile is chosen, created or declined.]**
- Contents (0x341250): item 0 "Create New Profile", then the saved profiles. The layout has six items; copies of item 0
  make more. The "loc" (card slot) child stays hidden.
- Display (0x340B40):
  - the bar under the selected item, at the item's y + 2;
  - `profile_name` shows the selected item's text;
  - the up and down arrows show when the list scrolls (six visible).
- The pad (0x3410E0):
  - Up and down move with `Rollover`.
  - Cross picks: item 0 opens the name entry (state 7), any other loads that profile (state 8). Both play `Click`.
  - Back is state 9 (Cancel).
- The port's events: `SHELL_EVT_PROFILE_LOAD` (the index into `ShellProfile.profiles`). On Back it shows the
  warn-continue message.

**Warn-continue message (0x3439E8):**
- Text: "WARNING: No Profile" or "Autosave Turned Off", with Continue / Don't Save and Back.
- Continue gives `SHELL_EVT_PROFILE_NONE` (nothing is saved) and Back returns to the list. **[G: that the list's
  cancel leads there.]**

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
- Back is state 14. The port goes back to the list **[G]**.
- A name already saved shows "The Profile name already exists." **[G: when it is checked.]**
- The port's event: `SHELL_EVT_PROFILE_CREATE` with the name.
- `shell_type` is not in the game: it types a character straight in.

**Messages panel (0x33FBB0):**
- Cross gives 0x12 while the autosave notice or `btn_select` shows.
- Back with `btn_cancel` gives 0x13.
- Button 0 with `btn_button0` gives 0x14.

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
  `MainMenu_<n>Player`. Its list items all stand at one place, so only the selected one is drawn **[G]**.

**Back:** state 0 calls `loadProfile`. In the boxes it goes one box back.

**Options (button 1):** `ControlRoom::fadeInInterface("Options")`.

**Bonus Content (button 0):** the "Midway Classics" box, with Rampage and Rampage World Tour. Ballers is in the NTSC
English version only. Cross starts `gShell.startEmulator(n)` after 0.25 s:
- It is 0x3063A0: `desi/m1` or `desi/jarvos` through `LoadExecPS2`, with `-game rampage` / `rwt`.
- The port gives `SHELL_EVT_EMULATOR` and returns to the menu.

**Cheat entry:**
- It opens when pad 0 holds exactly Start + Cross (`getButtons() == 128 | 64`). **[G: the getButtons bits:
  `SHELL_HELD_*`.]**
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

The port reports the cheats as `SHELL_EVT_CHEATS`. What the flags do in the game is not traced.

## The options (Options.scr) [R]

**Layout:** the same as the Pause menu's options page: Sound, Music, Vibration ON / OFF, and Cancel / Accept.

**Controls:**
- Up and down choose the row. The chosen row's title pulses at 1 + 0.1 sin 4t.
- The held left / right (or the stick) move the slider one step per frame.
- On the vibration row, left and right turn it ON or OFF. Going from OFF to ON buzzes the pad (`rumble(250)`).

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

**[G]:** the script leaves `nMonstersInUse` at 0 until the first focus, which would mark George taken. The port starts
it at −1.

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
- The grey box `basic` (M 0x90) under the globe stays hidden **[G]**.

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
| Play type | `gCommon.setPlayType`, the main menu's mode: 2 campaign, 3 King of the City, 4 King of the World, 5 Timed Run (game.h `GP_*`) |
| Players and bots | `setNumPlayersAndBots`: Common 0x4D7E28 / 0x4D7E2C |
| Monsters | `GameStatus.setMonsterIdx(i, m)`: `MonsterData.Monster<m>` and its Name |
| City | `setCityIdx`: `CityData.City<i>` and its City |
| Start block | `setStartBlock`: GameStatus +0x210, 0 = the city's BlockStart |
| Night | `setNight` |

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
- **Affine frames:** each object's frame is a full 2D affine map. `<x>` turns and mirrors draw, as do `yaw` and
  `setScaleX`.
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
  picture]**. The effect's size follows its frame's scale **[G]**: GuiEffect's draw (0x2376E0) hands the instance its
  whole matrix, scale and all (+0x70 to method +0x6C, kept as is at ParticleSystemFx +0x2C, 0x1F1D60; CompositeFx
  0x1EA1A0 keeps it and passes it on), but whether the billboards' sizes take that scale was not read. Every menu
  effect's scale is 1 but the city ring's (0.971) and the card glow's (1.32), so only the glow could differ.
- **The 2D path:** an object outside every `<DD>` has its position in screen widths about the centre, x right and y
  down, and a `<DD>` inside it draws about (320, 240), moved there and scaled by it: the GUI camera at depth 1
  **[R]**. The 1 / (1 + z) of a deeper `<o>` is left out: no shell `<DD>` lies in one.
- **No depth sort:** the layouts draw in tree order **[R 0x241770]**. `<z>` is the fade **[R 0x22C450]**:
  MonsterSelect's title (0.388) and frost border (0.58) are partly see-through, MonsterSheet's `blowup` (0) starts
  unseen.

**Quads:**
- `shell_quads_turned` gives four corners per picture.
- `shell_quads` gives hud.h's `HudQuad`: each picture's bounds, with the texture coordinates of its corners. A turned
  picture is drawn upright, and the 90° frost tiles come out transposed.

**Pad events:**
- Pad events go to the popup when it is up, else to the focused interface if `setupControllerTable` lets the pad
  talk.
- The frame event goes to the focused interface only **[G; R in effect: Options runs MainMenu's fader itself]**.

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
  float at 0x4CB54C (renderer 0x4CB480 + 0xCC, zero in the file, so set at run time; read at 0x16B768)
  **[not traced; 4:3 taken]**. The port keeps the vertical field and widens the view with the window, as the 2D.
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
  out on the centre line, a few degrees above the middle **[G: the latitudes hand-tuned]**. `getLookAt` (0x37,
  0x101220) inverts it.

**Lights:** `<ls>`'s own `<A>` is the set's ambient (0x16E1F0). `<lt>` keeps diffuse (+0x158, default 128 grey),
specular (+0x168) and ambient (+0x178); the colours read RRGGBBAA, as the script's night values are the file's. Its
direction is row 2 of its full world (0x239D60), into the screen and to the right for CitySelect's **[G: the
renderer's sign: the direction the light travels]**. `setAmbient` / `setDiffuse` / `setSpecular` go through
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
fade × z ≤ 0.01, is skipped; F 0x40 draws its children alone. F 0x1 is hidden, 0x80 no frame event, 0x100 an update
after the children; **F 0x4 is never tested**. The GUI's begin (0x231900) clears the screen (0x25C610, flags 7, depth
1.0), turns the lights off (0x16E320), sets the draw scale state 0x4CB52C to 1 and switches to its view (0x16DEE0)
**[R]**; it sets no depth test or write of its own. Gui2D's draw (0x2361E0) only sets its pixel view (0x2363F0 /
0x22F780).

## Not ported, or guessed [G]

- **Not drawn:**
  - the emulators and the demo game (the movies are played: [MOVIE_NOTES.md](MOVIE_NOTES.md));
  - the turn of HudQuads;
  - the main menu's two searchlights: `auto="false"`, and no script starts them **[R]**;
  - the GUI effects' sounds: none to play. No emitter of the menus' effects (the fires, the lava lamp, the rings, the
    card glow) names a sound, so fx.c's sound start never fires for them (traced through the main menu, MonsterSelect
    and CitySelect);
  - the frame's scale on particle sizes (taken to scale them; it is not 1 only for the city ring and the card
    glow) **[G]**;
  - the globe's brightness step at Greenwich, where its two halves meet: it stays with the lighting off, so it is in
    the halves' own art, not the port's lights.
- **Popup:**
  - the memory card's own states (checking, saving, errors, formatting);
  - how the selection starts (item 0);
  - the list scrolling beyond six profiles (counted, not shown);
  - the duplicate-name check;
  - Back from the name entry.
- **Bits and flags:**
  - the `getButtons` bits for the held pad;
  - flag 0x400 as blinking, at 1.25 Hz between 0.2 and 1;
  - B = 5 for a rect without `<B>` (GuiRect constructor 0x245650 stores 5.0 at +0x180);
  - `<sh>`'s colour order.
- **Hidden or masked:** untextured 0xFFFFFE quads without M 0x200 are taken as masks and not drawn (the title boxes of
  MonsterSelect and MonsterSheet).
- **Show-profile:** the show-profile flag's life.
- **Idle and focus:**
  - which inputs reset the idle timer;
  - the order of the focus events (old 1024, then new 512).
- **Attract mode:** its 5 s stand-in per movie in state 2, and `startAttractGame`'s demo.
- **Segments:** the layouts' segments 1, 2 and 7 (other languages or video modes); segment 0 is used.
- **Engine calls:** `restoreCheckpoint` and `restoreProfile` are left to the viewer.
