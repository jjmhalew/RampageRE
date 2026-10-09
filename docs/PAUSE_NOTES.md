# The Pause menu

The in-game Pause menu as `src/hud.c` ports it (`hud_pause_open` and the calls after it in `src/hud.h`): the layout
`Pause.ifc` of `WorldGui.bdp`, run by its script `PAUSE.BSF` (`build/scripts/Pause.scr`, with `ObjectFader.h` and
`ConfirmQuit.h`), and the engine calls it makes. The formats are in [FORMAT_GUI.md](FORMAT_GUI.md), the other screens
in [SCREENS_NOTES.md](SCREENS_NOTES.md). **[R]** read in the code or the files, **[G]** a guess or a stand-in.
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
  - calls App +0xA4's vtable +0x24 (taken to be the game's timer [G]);
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
    - `completed`: "COMPLETED", green, wrapped at 300, placed by a full matrix `<x>` (turned −28°, scale 0.8);
    - `challenge`: "Bonus Challenge" (`CT_Challenge`), orange, m 0.86;
    - `goal`: the challenge's text (s 0.8, wrapped at 270).
  - `boss_group` (hidden) at 220 40: `boss_icon`, `boss_completed`, `boss_title` (maxwidth 300), and `boss_challenge`,
    `boss_objectives`, `boss_hint` (s 0.65, m 0.85, wrapped at 400).
  - `boss_group_foreign` (hidden): the same at 190 40, with x −220 and a 520 wrap for the longer languages.
  - `selection_group` at 0 160: a rule (child 0), the list box `choices` at 136 153 (child 1), and a rule at 4 235
    (child 2). The list has "CONTINUE" / "OPTIONS" / "QUIT" (`Button_Continue`, `Button_Options`, `Button_Quit`;
    Atrox60 × 0.5), yellow when selected and grey 0x7F otherwise, with the pulsing highlight bar.
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
  - Left / right (4 / 2), on the rumble row: `incrementSelection(dir, 1)` with `Rollover`. Going from OFF to ON calls
    `setEnableRumble(1, 0)` and `gCommon.rumble(250)`.
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
- Flag 65536 is the main menu's cheat code "082864" (`MainMenu::activateCheat`). What it does is not traced.

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
- So 2 is right, 4 left, 8 up, 16 down. `hud.h` had taken them the other way [G]. Its `HUD_EV_*` values are now
  corrected. The screens that only treat up = left and down = right behave as before.

## The port (hud.c) [R unless marked]
- `hud_pause_open(h, pad, &{city, block})` refuses in the attract mode, while a results screen is up, or while the
  menu already shows. It loads its own copy of `Pause.ifc` (drawn last, over everything) and runs the 8192 setup.
  - The challenge data come from the last `hud_challenge()` (`last_block`, `identifier`, `icon`, `icon_scale`,
    `pause_scale`, `icon_shift`, `upgrade`) and `hud_challenge_items()`.
  - The language comes from `hud_set_language` (> 2: the foreign boss card; 7: the Dutch rumble list).
- `hud_input` / `hud_input_pad` go to the menu while it shows. `hud_pad_held` gives pad 0's held stick x and left /
  right.
- The script's engine calls come out of `hud_calls`:
  - `HUD_CALL_MUSIC_VOLUME`, `SFX_VOLUME`: `setMusicVolume` / `setSfxVolume` (value, ms);
  - `RUMBLE`: `setEnableRumble`; `RUMBLE_PULSE`: `rumble(250)`;
  - `PLAY_MUSIC`: start the block's music if none plays;
  - `SAVE_SETTINGS`: `gCommon.set*Volume` / `setEnableRumble(.., 1)`, read with `hud_settings`;
  - `CLEAR_CHEATS`: 65536.
- The sounds come out of `hud_sounds`.
- `hud_pause_result` gives `HUD_PAUSE_RESUME` (Continue), `HUD_PAUSE_QUIT` (call `game_restart_shell`) or
  `HUD_PAUSE_SETTINGS` (the profile's settings changed).
- `hud_paused()` is true while the menu shows.
- `hud_set_settings` gives the menu the profile's volumes and vibration. Default 0.75, 0.75, on **[G: the profile's
  defaults are not traced; the slider's full scale is 0.75]**.

## Not ported, or guessed [G]
- The `<x>` matrix's turn: quads cannot turn, so the "COMPLETED" stamp is drawn level at its place and scale.
  Mirrors (the arrows) are kept.
- `setFlags(1024)` on "COMPLETED", the list's `setExtraSpacing`, and the sizer's layout (its items stay where the
  layout puts them).
- The line's width (drawn 1 pixel) and `<ep>`'s layout (taken as a count, then x y z points).
- The slider steps: one per 1/60 s of `hud_update` time (the script steps once a frame).
- `getPlatform() == 2` is taken to be the PS2's, so the foreign boss card is used from French on.
- After Quit the menu goes on the next `hud_update` (in the game it stays up while the world shuts down).
- Which flag the menu's `pauseGame` stops in each world system, and App +0xA4 (taken to be the timer), are not
  traced further.
- The music fade, the pausing of the sound events and the vibration belong to the viewer.
