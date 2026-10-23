# The Pause menu

The in-game Pause menu as `src/hud.c` ports it (`hud_pause_open` and the calls after it in `src/hud.h`): the layout
`Pause.ifc` of `WorldGui.bdp`, run by its script `PAUSE.BSF` (`build/scripts/Pause.scr`, with `ObjectFader.h` and
`ConfirmQuit.h`), and the engine calls it makes. The formats are in [FORMAT_GUI.md](FORMAT_GUI.md), the other screens
in [SCREENS_NOTES.md](SCREENS_NOTES.md). **[R]** read in the code or the files, **[G]** a guess.
Addresses are in `RAMPAGE.ELF`.

## What opens it [R WorldGuiDriver.scr]
- GUI events go to the interface with the focus. In play that is `WorldGuiDriver`, unless a results screen
  (`startResults`) has taken it. The GameOver screen gets Start as its own "accept".
- `WorldGuiDriver::process`, event 128 (Start), outside the attract mode (`getPlayType() != 0`): once its first frame
  event has come (`bOkayToPause`), and unless the debug setting `App/bUseStartButtonExtensions` is set, it calls
  `screen.loadInterface("Pause")`.
- The driver does not check `event.getController()`. Either player's Start pauses.
- No native code loads it: the ELF has no "Pause" string besides `pauseGame` and `PauseScale`.

## What stops [R]
- `gApp.pauseGame(on)` is the script binding with id 0x12 (0x28B5C0, jump table 0x428EF0, index = id + 1). It calls
  RApp's vtable +0x50, 0x28B620. That function toggles the pause through 0x28BC90 when `isPaused()` (+0x3C, 0x28B690)
  differs from `on`.
- 0x28BC90:
  - flips bit 0x10 of App +0xA0 (the paused flag) and copies it into the byte 0x45ACAC;
  - pauses the sound events: SoundDevice 0x26B7F0 with mask 3 → the event player 0x4CFAB0, 0x272780, which sets each
    playing instance's +0x1D and pauses its voices;
  - calls App +0xA4's vtable +0x24: the active world's pause (RWorld 0x2B7A40), which clears its tick accumulator
    (+0x608), so pausing or resuming drops the leftover time [R];
  - sets App +0x96 when +0x95 is set.
- World code tests the flag (App +0xA0 & 0x10, or the vtable +0x3C), for example 0x2B6AF0 and 0x2C1CE0. The world and
  its clock stop.
- The GUI does not stop. The menu's fades, the banners and the other interfaces still get their frame event (2048)
  with the real frame time. The GameOver screen relies on this too.
- The music is not paused. The script fades it to volume 0 over 1000 ms (`gSndDev.setMusicVolume(0.0, 1000)`) and
  turns the pad's vibration off (`gCommon.setEnableRumble(0, 0)`), after saving both
  (`prevMusic = gCommon.getMusicVolume()`, `prevRumble = getEnableRumble()`). Continue and Quit restore them.

## The layout (Pause.ifc) [R]
- A full-screen veil (black, alpha 0x7F).
- `pause_group`: the frame `boss_rect`, a GuiRect at 130 40, 380 × 400, with a red `FrameLine` / `FrameCorner` frame
  and a black 0xA4 fill. Its `center` group holds:
  - `title_group`: the red band `BAND_RED` with "GAME PAUSED" (`Pause_GamePaused`, Atrox60, maxwidth 370), a rule, and
    `city_location` (ArialBlack24 × 0.6, maxwidth 360).
  - `challenge_group` (hidden at the start), at 220 30:
    - `icon` (F 0x20, Q 6, m 1.25);
    - `completed`: "COMPLETED", green, wrapped at 300, placed by a full matrix `<x>` (scale 0.8, turned 28° up to the right);
    - `challenge`: "Bonus Challenge" (`CT_Challenge`), orange, m 0.86;
    - `goal`: the challenge's text (s 0.8, wrapped at 270).
  - `boss_group` (hidden) at 220 40: `boss_icon`, `boss_completed`, `boss_title` (maxwidth 300), and `boss_challenge`,
    `boss_objectives`, `boss_hint` (s 0.65, m 0.85, wrapped at 400).
  - `boss_group_foreign` (hidden): the same at 190 40, with x −220 and a 520 wrap for the longer languages.
  - `selection_group` at 0 160: a rule (child 0), the list box `choices` at 136 153 (child 1), and a rule at 4 235
    (child 2). The list has "CONTINUE" / "OPTIONS" / "QUIT" (`Button_Continue`, `Button_Options`, `Button_Quit`;
    Atrox60 × 0.5), yellow when selected and grey 0x7F otherwise, the selected one grown to 1.2 (`<growth>`), with the
    pulsing highlight bar (below, "The list box").
  - `controller_group` (hidden) at 0 45: the picture `controller_map` (centred at 210 285, m 1.2) with green labels
    ROAR (with the `Trigger_Right` icon), KICK, PUNCH, GRAB, JUMP and MOVE (`Pause_*`, ArialBlack24 × 0.4).
- `confirmQuit` (hidden), inside `pause_group`: ConfirmQuit.h's dialog (veil, `cq_rect`, `cq_title`, No / Yes).
- `options_group` (hidden). Its `options_rect` sits at 100 120, 440 × 250, with a dark green 0x7F fill. It holds:
  - "OPTIONS" (`Options_Options`, × 1.2);
  - a line `<l>` (`<ep>2 0 0 0 435 0 0</ep>`);
  - the sliders `sound` (282 100) and `music` (282 140): 200 × 20, a green bar under a black half mask, titled
    "Sound:" and "Music:" (right-justified at −120);
  - "OFF" and "MAX" at 182 70 and 382 70 over the sliders' ends, each with an `Arrow_Sm` mirrored by `<x>`;
  - the list `rumble` at 220 180 with "ON" and "OFF", titled "Vibration:" (`Options_Rumble:`);
  - in a sizer `sz`: "Cancel" (Controller_Back) and "Accept" (Controller_Select).
  - There is a second group named `options_group` inside it. `findObject` finds the outer one first.

## The script (Pause.scr) [R]
**Start (event 8192)**:
1. Take the focus and set `nPauseState = 0`. Save the music volume and vibration and turn them off, as above.
2. Select item 0 of `choices` and make its highlight pulse.
3. `bIsBoss = getCurrentBlockNum() == getNumBlocks()`. Show `challenge_group` when not the boss and `boss_group` when
   the boss.
4. Then one of these:
   - **Not the campaign** (`setupNonCampaign`): both groups hidden; `selection_group` to 0 −20 with its top rule hidden
     and its lower rule moved to 4 255; the controller picture shown.
   - **Campaign, not the boss** (`setupChallenge`):
     - the title: `CT_Challenge_Upgrade` if `ChallengeStatus::isChallengeUpgrade()` (`getChallengeUpgrade() != 0`),
       else `CT_Challenge`;
     - `goal`: `localizeString(getChallengeIdentifier())` ("Eat 2 Ice Cream Cones");
     - the icon: `resetTexture(getChallengeIcon())`, `setScale(getChallengeIconScale(1))`, `showObject`, x plus
       `getChallengeIconShift()`;
     - "COMPLETED" shown when `min(collected, required) == required`, with `setFlags(1024)`;
     - `selection_group` at 0 20, the controller picture shown.
   - **Campaign, the boss block** (`setupBoss`):
     - "Foreign" means `getLanguage() > 2` and `getPlatform() == 2`. The foreign group is then used with padding 0,
       the frame moves to 80 40 at width 480, and its `center` to 50 0. Otherwise `boss_group` with padding 8.
     - The four texts are `ChallengeBossTitle`, `ChallengeBoss`, `ChallengeBossObjectives` and `ChallengeBossHint` (in
       the city's `.loc`). The objectives go `height() + padding` below the message, and the hint as far below the
       objectives.
     - The icon is set as above, without the shift; `boss_completed` as above.
     - `selection_group` at 0 160, the controller picture hidden.
5. `city_location` = `localizeString(getGameStatus().getCityName()) + " : " + localizeString(getBlockName(1))`
   ("Las Vegas : Harry's").
6. `CQInit("Pause::onQuit")`; faders: `pause_group` shown, `options_group` hidden.
7. `gApp.pauseGame(1)`.

**`getChallengeIconScale(1)`**: World binding 0x4C, 0x2AF488. It reads the ChallengeDef's `PauseScale` property and
falls back to its `Scale`. With 0 (ChallengeStatus, ChallengeText) it reads `Scale` only.

**The menu's events** (`nPauseState == 0`):
- 2048: the faders. ConfirmQuit, when it shows, takes every other event (No / Yes, X, Back).
- Up or left (8, 4): previous item, with `Rollover` when the selection moved. Down or right (16, 2): next item.
- Start or X (128, 64): `Click`, then by the selected item:
  - **Continue**: `unloadInterface(this)`, the focus back, `pauseGame(0)`, `setMusicVolume(prevMusic, 1000)`,
    `setEnableRumble(prevRumble, 0)`.
  - **Options**: from pad 0, `activateOptions`. From any other pad, the sound `Cheat_back` and nothing else.
  - **Quit**: the highlight stops pulsing; `CQShowConfirmDialog(0)` ("Unsaved progress and option settings will be
    lost if you quit now. Are you sure you want to quit?"). Yes calls `Pause::onQuit`: `pauseGame(0)`,
    `gCommon.clearCheatFlags(65536)`, the music and vibration restored, `gWorld.restartShell()` (the world's state
    12, `GS_EXIT`; game.h `game_restart_shell`). The interface is not unloaded; the world's end takes it.
- Back (32): no case. It does not resume.

**The Options page** (`nPauseState == 1`):
- **Opening** (`activateOptions`): `pause_group` fades out and `options_group` in (ObjectFader, 2 a second). Then
  `setupOptions`:
  - row 0;
  - the sliders at `100 × volume / 0.75` (sound from `getSoundVolume()`, music from `prevMusic`);
  - the rumble list at `!prevRumble` (0 ON, 1 OFF);
  - the three row titles at text scale 1.
  - In Dutch (language 7) the list moves to 185 180, `setExtraSpacing(30)`, and ON / OFF are at scale 0.6.

  Then `setMusicVolume(fMusic, 1000)`, so the music can be heard while it is set.
- **Each frame (2048)**:
  - the faders;
  - the selected row's title pulses at `1 + 0.1 sin(4t)`;
  - on rows 0 and 1, the row's slider moves while pad 0 is held: `getController(0)`, `getX(0) ≥ 0.05` or
    `getButtons() & 8` gives `increment()`, `≤ −0.05` or `& 4` gives `decrement()`. GuiSlider binding 0x3B / 0x3C
    (0x250EE0 / 0x250F00) is `setValue(value ± 1)`, clamped to 0..100 (0x24ABA0): one step a frame.
  - The sound volume is set (`setSfxVolume(v / 100 × 0.75, 0)`, `Click`) once the slider has moved and then stopped
    for a frame.
  - The music's is set at every step, and the block's `Music` (`getCurrentBlockStringProp("Music")`) is started if
    no music plays and the value is above 0.
- **Events**: those of pads other than 0 are dropped.
  - Up / down (8 / 16): the row, wrapping over the three, with `Rollover`. The old row's title goes back to scale 1.
  - Left / right (4 / 2), on the rumble row: `incrementSelection(dir, 1)` with `Rollover`: left ON, right OFF, no
    wrap (0x23E360; the 1 plays the list's end sound once at an end, which this list has none of). Going from OFF to
    ON calls `setEnableRumble(1, 0)` and `gCommon.rumble(250)`.
  - X (64; Start does nothing here): `Click`, then `saveSettings`. It sets `prevMusic` and `prevRumble` to the sliders'
    values and calls `gCommon.setSoundVolume`, `setMusicVolume` and `setEnableRumble(b, 1)`. Then `deactivateOptions`:
    the faders back, and `setMusicVolume(0, 1000)`.
  - Back: `Cancel`, `setSfxVolume(fSound, 0)` and `setEnableRumble(bRumble, 0)`, then deactivate. The music is not
    set back; it is faded to 0 and `prevMusic` is unchanged.

**The settings in gCommon** [R the Common bindings: table 0x430450]:

| Binding | Id | Address | What it does |
|---|---|---|---|
| `setSoundVolume` | 0x1D | 0x2C2F68 | stores 0x4D7ACC and sets the SoundDevice volume groups 0, 9, 10 and 11 (0x26B5E0) |
| `setMusicVolume` | 0x1F | 0x2C3048 | stores 0x4D7AD0 and sets group 3 |
| `setEnableRumble(on, save)` | 0x33 | 0x2C3440 | stores the byte 0x4D7AD4 |

- `getEnableRumble` is 0x32. `clearCheatFlags` (0x30, 0x2C3390) clears bits of 0x4D7E68.
- The setters raise the profile's "changed" byte 0x4D7AC8. The profile's load (0x33F120) applies them again.
- Flag 65536 is the main menu's cheat code "082864" (`MainMenu::activateCheat`): the soak test of fake GUI presses
  (0x33D740; the shell's 0x307920, the world's 0x2B6350; [SHELL_NOTES.md](SHELL_NOTES.md) "The soak test") [R].
  Quitting clears it, which is the only way out of it.

## Two players [R]
- Either pad opens the menu, and either pad's events move its list, pick Continue / Quit and answer ConfirmQuit.
- Only pad 0 (player 1) opens Options or uses that page: the others hear `Cheat_back`.
- The sliders read pad 0's held state.
- The game pauses for both players.

## The pad's event ids [R by the scripts]
- The pause's Options page is one piece of evidence. Its rows are stacked down the page and change with 8 / 16. Its
  sliders move with the held left / right. The ON / OFF pair (side by side) changes with 4 / 2.
- The same reading fits ConfirmQuit (No / Yes side by side: 4 / 2, with 8 / 16 folded into them), the main menu's
  cheat code (6 digits side by side: 4 / 2; a digit up with 8) and CitySelect.
- So 2 is right, 4 left, 8 up, 16 down (getButtons' names, 0x3F67C0) [R]. An earlier `hud.h` had them the other
  way; its `HUD_EV_*` values are corrected. The screens that only treat up = left and down = right behave as before.

## The port (hud.c) [R unless marked]
- `hud_pause_open(h, pad, &{city, block})` refuses in the attract mode, while a results screen is up, or while the
  menu already shows. It loads its own copy of `Pause.ifc` (drawn last, over everything) and runs the 8192 setup.
  - The challenge data come from the last `hud_challenge()` (`last_block`, `identifier`, `icon`, `icon_scale`,
    `pause_scale`, `icon_shift`, `upgrade`) and `hud_challenge_items()`.
  - The icon is a texture by name: the city's bundle has the `Pause_*` pictures, fx.bdp the powerups' and the other
    20 blocks' (`Chocolate_bar`, `Powerup_HotDog`, `moneybag`, `Ace_Sp`, `King_Card`, `Gong` ...). The viewer gives the
    HUD both (`hud_add_bundle`); up to v0.11.1 it had only the city's, so those 20 blocks showed no icon here, in
    ChallengeText or in ChallengeStatus.
  - The language comes from `hud_set_language` (> 2: the foreign boss card; 7: the Dutch rumble list), the viewer's
    `--lang`.
- `hud_input` / `hud_input_pad` go to the menu while it shows. `hud_pad_held` gives pad 0's held stick x and left /
  right.
- The script's engine calls come out of `hud_calls`:
  - `HUD_CALL_MUSIC_VOLUME`, `SFX_VOLUME`: `setMusicVolume` / `setSfxVolume` (value, ms);
  - `RUMBLE`: `setEnableRumble`; `RUMBLE_PULSE`: `rumble(250)`;
  - `PLAY_MUSIC`: start the block's music if none plays;
  - `SAVE_SETTINGS`: `gCommon.set*Volume` / `setEnableRumble(.., 1)`, read with `hud_settings`;
  - `CLEAR_CHEATS`: 65536;
  - `GRAPHICS` (port extra): the Graphics page's values changed, read with `hud_graphics` (arg 1: its Accept);
  - `LANGUAGE` (port extra): the Options' Accept with another language in the Language row (arg, settings.h's).
- **Port extra:** the Options page has a third button, Triangle **Graphics** (a copy of Cancel with CommonGUI's
  `Controller_B1`; on the page Square **Defaults**, `Controller_B0`), which opens the front end's Graphics page in the same frame (`pause_graphics_*`, NATIVE.md
  "Graphics settings"), and a fourth row under Vibration, Language (`pause_language_build`); `hud_set_graphics` gives
  them the settings in use. The game's Options have no such button or row.
- The sounds come out of `hud_sounds`.
- `hud_pause_result` gives `HUD_PAUSE_RESUME` (Continue), `HUD_PAUSE_QUIT` (call `game_restart_shell`) or
  `HUD_PAUSE_SETTINGS` (the profile's settings changed).
- `hud_paused()` is true while the menu shows.
- `hud_set_settings` gives the menu the profile's volumes and vibration. Default a new profile's 0.5, 0.25, on
  (0x33F630) [R]; the slider's full scale is 0.75.

## The list box (GuiListBox) [R]
The three lists (`choices`, ConfirmQuit's `cq_list`, the Options page's `rumble`) work as GuiListBox does (ctor
0x23E280 over 0x23EF70, vtable 0x447850: draw 0x23C250, layout 0x23CF60; the script calls in 0x253730):
- **Selection** (`selectItem` / `setSelection`: 0x23E480): at or below 0 the first item shown, at or past the last
  the last one shown, else the nearest shown one from there, searching away from the old selection. No wrap.
  `incrementSelection(delta, bump)` (0x23E360) sets the selection + delta; when it moved the list's move sound
  (+0xF0) plays, when it did not and `bump` is 1 its end sound (+0x108), once, flag 0x10. No layout here names those
  sounds.
- **Colours**, every frame in the layout (0x23D8A4 .. 0x23DB2C): with the highlight on (+0x10C & 2, set by the
  constructor) the selected item takes `<c highlight>`, the other items shown `<c normal>`, hidden ones
  `<c disabled>`.
- **Growth** (each item, 0x23CB20 from the layout): `<growth>3 1.2 4 16` is a count and +0x13C, +0x14C, +0x150
  (0x22A640; defaults 1, 4, 16). The selected item (shown, highlight on) grows towards +0x13C, the others back to 1:
  up `g += 4 dt (target − g)` (reached within 0.0001), down at 16, not past the target. The item's growth record
  (+0x50) holds g and an offset that keeps it in place by its justify and origin: across 0.5 (w / g_old)(1 − g),
  w the text's width (0x234BA0), minus for left-justified, plus for right, none centred; the same down by its height.
  GuiText's draw (0x234D60) scales the text (not its children) by g and moves it by the offset when its parent is a
  list. So CONTINUE grows to 1.2, ConfirmQuit's No / Yes to 1.15 (about their middles), ON / OFF to 1.2.
- **Highlight** (draw 0x23C250): `<b highlight>` (0x23BE10) and `<b endcap>` (0x23BD40) make a GuiLine (+0x138) with
  the segment picture (sprite +0x16C) and the cap picture at both ends (+0x6C, +0xEC). For the selected item, shown
  with a text: s its frame's scale times its growth; its box (0x234BA0 × s) placed by its justify and origin from its
  place plus the growth's offset; for each line (the line's measure, held to maxwidth, × s) the line runs from ext
  before the line's start to ext past its end at the line's middle, ext = max(dh₀ s, s × cap − h / 2), the band
  h + dh₁ s across (`<d highlight>` dh, cap the cap picture's width: HighlightEndcap 64), in `<c selector>`, times the
  GUI's pulse with `setHighlightPulse(1)` (+0x10C & 8). The GuiLine (0x23A8E0) puts a cap of the picture's width
  inside each end (u 0 at the end, the far one mirrored; HighlightEndcap fades out towards u 0) and the segment
  between. The segment's uv on that path is left as the sprite has them [G: taken 0 .. 1]; without caps the segment
  runs end to end, u 1 .. 0. For CONTINUE: s 1.2, h ≈ 36, ext 58.8, the band 69.6 (the pictures are clear at their
  top and bottom).

## Not ported, or guessed
- The `<x>` matrix (0x22DEC0: the 12 floats into +0x8 .. +0x34 as written) [R]: a point goes through it as a row
  vector, x' = x m00 + y m10 (0x101CD0), so "COMPLETED" (0.7055 −0.3771 / 0.3771 0.7055) is scale 0.8 turned 28° up
  to the right, the arrows mirrored up-down. The port draws the object and its children turned about its origin
  (hud.c `turn_quads`, corner quads the renderer already draws for the shell's turned pictures).
- `setFlags(1024)` on "COMPLETED" is the GUI's pulse: object flag 0x400 multiplies the alpha by 1 − (0.2 + 0.2 sin
  phase), the phase 2π a second (0x242950): 0.6 … 1 at 1 Hz [R].
- The list's `setExtraSpacing` is GuiListBox id 0x4D (dispatcher 0x253730, case 0x253D48): it replaces the list's
  `<sp>` (+0x130, the `sp` tag 0x22AA90's) [R] (0x254868 is GuiText's setLineSpacing, OFont +0x11). The list lays
  its items out every frame (0x23CF60, vtable 0x447850 +0x10; window 0x23BF10) [R]: each shown item (F & 0x41 clear)
  at a running place from 0, horizontal (`<a>` & 8) step its width + sp, vertical its height + its line spacing + sp;
  x by its justify (0, ½, 1 of its width, or of the list's when vertical), y by the origin (0, ½, 1 of the list's
  height, or of the item's when vertical), 0x20 centred; items outside the window get F 0x200. Checked: ON / OFF
  (ArialBlack24 × 0.8: 30.4 and 40 wide) come at 15.2 and 110.4, the layout's, and ConfirmQuit's No / Yes at 0 and
  131.2. The port lays out the lists across every `hud_update` (hud.c `layouts_lay`); down a list the steps
  are heights no language changes, so those keep the layout's places. Dutch sets the rumble list's spacing to 30.
- The sizer [R] (GuiSizer type 0x12, ctor 0x249160, vtable 0x447B20, update 0x248B80 with F 0x100): each child's box
  (its bounds with children, joined with the origin, through its 3 × 3) laid along x (flag 1) or y: place_i =
  place_i−1 + max_i−1 + after_i−1 + before_i − min_i, `<si>` before (u16 +0), after (+2), stretch (byte +6); flag 4
  ends at 0, 8 centres; across 0x20 / 0x40 / 0x10 (horizontal) or 2 / 8 / 4; 0x100 spreads (stretch − total) / Σ(s +
  1) below `<stretch>` (0x2490D0); hidden children count. Checked: Cancel / Accept come at −161.7125 / −61.8375, the
  layout's. In full [R 0x248B80]: sflags at +0x78, `<stretch>` at +0x7C; a child's `<si>` before / after are s16
  (+0, +2), its stretch byte +6 (`s=`), its box (0x2491D0) its bounds joined with the origin through its matrix.
  The first item sits at 0, each next at the last's origin + max + after + its before − its min. With 0x100 and the
  boxes' total below S, f = (S − total) / Σ(s + 1) (never negative) and each item's f (s + 1) goes between the
  items: the first's after it, the last's before it, a middle one's split half and half. Then, with any of 0x7E, 4
  puts the last box's max at 0 (its after not counted) and 8 (over 4) centres the row. Down (0x248FA0) the row is
  not shifted; 4 / 8 / 2 align across. Port: `sizer_row` / `sizer_lay` (hud.c, and the same in shell.c), across
  only (no layout lays one down). French's "Accepter" then ends at the frame's edge as Accept does.
- The line [R]: GuiLine (ctor 0x23BA10) is 3 wide by default (+0x228), drawn as a band ± width / 2 across its points
  (0x23A8E0); `<ep>` (0x22C0E0) copies two points, floats 0 .. 2 and 3 .. 5 after the count. As here (`<width>`, 3).
- The slider steps: one per script frame, the world's step 1/30 s (the GuiManager's update in the tick 0x288340,
  paused or not, with gWorld +4 as dt) [R]: a full sweep in about 3.3 s.
- `getPlatform()` answers 2 in this build (App case 0x28B1A0) [R], so the foreign boss card is used from French on.
- After Quit the menu goes on the next `hud_update` (in the game it stays up while the world shuts down; the viewer
  leaves the world at once).
- `pauseGame`'s gate is the tick's (0x288340: App +0xA0 & 0x10 skips the world's update; the GUI and the drawing
  run on) [R]. 0x2B6AF0 (from 0x2B8F80) runs each monster's 0x2D6650 and then, only with the app paused (+0xA0 &
  0x10) and the cheat flag 0x10000, the soak test's world half (SHELL_NOTES.md "The soak test") [R]; 0x2C1CE0's test
  was not traced.
- The music fade, the pausing of the sound events and the vibration belong to the viewer.
