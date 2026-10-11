# Banners, challenge status and results screens

The 2D interfaces over the world besides the HUD, as `src/hud.c` ports them (the formats are in
[FORMAT_GUI.md](FORMAT_GUI.md)). All of them are layouts of `WorldGui.bdp` run by a script
(`build/scripts/<Name>.scr`); each interface the game loads is its own copy of its layout tree. **[R]** read in the
code or the files, **[G]** a guess or a stand-in.

## Text banners (PointsBanner.createAndStartText) [R]
- The script class `PointsBanner` (binding names at 0x2A6BF0, dispatch 0x2A6A40) has three calls: `createAndStart`
  (id 0x2B: a new PointsBanner, `start` 0x331D50, the floating number), `createAndStartText(name, colour, scale,
  sound)` (id 0x2C, 0x2B1640) and
  `createAndStartText2(name, name2, colour, scale, hold, sound)` (0x2B14E0).
- The text calls load the interface **`Rage`** on the screen (screen +0x238, vtable +0x144 / +0x40) and call the script
  function `Rage::setPointsBannerData` (or `...Data2`) in it. So a text banner *is* the RAMPAGE banner with other data;
  `Monsters::startRage` loads `Rage` with none (its defaults: "Rampage", red, scale 1, `v_rampage`). Nothing sets
  its `fHoldTime`, and each interface has a script program of its own (the `sc` tag 0x22C010 → setScript 0x238A90,
  a new 0x7BC-byte program through gGuiMgr +0x4C, 0x231360), so it is 0.0 as made (TypePFloat 0x1CE830): the
  RAMPAGE banner has no hold, its text shows only at half, fading, after the swirl [R].
- **Which banner gets the data** [R]: the new `Rage` is appended to the screen's list (0x233500), and the function
  lookup (gGuiMgr 0x231220: its own program first, then each screen's interfaces from the first) calls
  `setPointsBannerData` in the first interface whose program has it: the oldest `Rage` still up. While another
  banner shows, the data go to that one (its text, set up on its first frame, stays; its hold and name change) and
  the new banner is a plain RAMPAGE one. (Ported: hud.c `rage_load_data`.)
- `Rage.ifc`: the text `Rage` at 320 240, Atrox60, centred both ways, s 1.4, charspacing 1, maxwidth 500, with a
  hidden `line2` (ArialBlack24, s 1.2, 30 below), and a quad `Head` (HUD_GEORGE_HEAD, scale 4).
- `setPointsBannerData`: message, colour, the text object's scale (0.5 for edibles and the reveal, 1.0 for challenges),
  hold 0.35 s, the sound unless `"Default"`; the interface is renamed `Removeable`. So every text banner (challenge
  met, the reward, edibles, upgrades, the reveal) is `Removeable`; the plain RAMPAGE banner (no data) stays `guiRage`.
- **hideChallengeCompletionUI** (World binding 102, 0x2B01C0): for each interface on the screen (screen +0x238: the
  list at +0x0, its count at +0x8) named (+0x38) `Removeable`, the vtable call +0x44 (0x22A3B0, `flags +0x5A |= arg`)
  with 1. Flag 0x1 is what `GuiObject.isVisible` tests (0x256C64: `!(flags & 1)`): the banners are hidden, not
  unloaded. `StreetObject::TakeDamage` calls it when a monster takes the challenge's upgrade, just before its own
  banner (the upgrade's TextID, white, `v_upgrade`), which is therefore not hidden. The hidden banners' scripts run
  on and unload themselves [R]: the screen's update (0x233850) runs the pending unloads, then every interface's tree
  update (0x2415E0), which never tests F 0x1; Rage.ifc's F 0x80 sends the frame event whether focused or not
  (0x2416E8). Ported as `hud_hide_challenge_completion`.
- **The name**: `localizeString(name)`; if it begins with `#` the text is the rest, as is (the reveal builds its
  text itself). The texts are in `Game.loc` (WorldGui.bdp): `InstantRampage` "Instant\nRampage", `SpeedIncrease`
  "Speed\nBoost", `Challenge_Met` "CHALLENGE MET!", `MonsterRevealed` "%s Unlocked", `RAMPAGE`. Keys ignore case
  [R: LocalizationManager 0x1D4C60, a strcasecmp binary search per table]; `gApp.localizeString` of a missing key
  gives "" (0x28B470) [R]. `Edible.scr` turns the first `\n` (two characters) of its Message into a line
  break first.
- **Pictures in a text**: the upgrade hints (`Inst_Upgrade1` .. `8`, the reward banner's second line) hold
  `\v1.25Controller_Back\v`. The font's special glyph (0x1B7AE0) [R]: a leading number is its scale sc (else 1),
  the rest the texture's name; it is k × the picture's used size, k = sc min(1, the font's average height / its
  height), with no space around it, and counts in the line's height. It is drawn after the text (0x1B74A0), white
  in the text's alpha, at the glyph's place, max(0, average height − picture height) / 2 down: Controller_Back
  (32 × 32) in ArialBlack24 (average height 22.6) 28.3 square, Trigger_Right (32 × 16) 40 × 20.
- **Motion** (`Rage::doInterface`, from its second frame; the first sets up):
  - setup: the text, colour, scale; 15 copies of the text (with `line2`), fades 0, 1/15 .. 14/15; the text hidden;
    the sound played.
  - 0 .. 0.75 s: with p = t / 0.75, copy i has fade √p (15 − i) / 15 and sits at the text's place plus
    (1 − √p) 100 cos(1.3 i) along the angle (i − 4) / 6 + p: they swirl in to the centre.
  - then for `hold` s: the copies hidden, the text at fade 1, shaking at (320 + 3 cos 2a, 240 + 2 sin 3a),
    a = 2π · the hold's progress.
  - then 0.5 s: its fade falls from ½ to 0; then the interface unloads itself.
- The `Head` quad is never shown by the script: its `<z>` 0 is its fade (0x22C450), so it is never seen **[R]**.
- The monster reveal (`Cryotube.scr`): `"#" + format(localize("MonsterRevealed"), localize(getMonsterName(i)))`,
  colour (220, 0, 0), scale 0.5, sound `V_Monster_Reveal_Seq`.

## ChallengeStatus [R ChallengeStatus.scr]
Loaded once per city with the HUDs (`<City>::onInit`: `gCommon.setChallengeHud(screen.loadInterface(...))`); faded
with them (1 in BeginBlockNI, 0 in ResultsNI). At 30..60, 408:
- `challenge_group` (not the boss): "Challenge:" and `status` "n/m" (ArialBlack24 at 0.65). The whole interface hides
  while `getChallengeItemRequired()` is −1. Meeting the count: "CHALLENGE MET!" (green, `v_challenge_complete`),
  ChallengeDef.Points (5000) / players added to each player's category 5; 2 s later the banner "5,000 PTS"
  (`ChallengeReward`, hold 1.5). The icon object under it starts hidden (F 0x1) and nothing shows it.
- `boss_group` (the city's last block, campaign or Timed Run): the boss's icon and the slider `bossHealth` (100 × 8,
  green on dark green). `gCommon.updateChallengeHud(f)` (0x2C41CC) calls `ChallengeStatus::updateBossHealth`: the
  bar at 100 f; at f ≤ 0 "CHALLENGE MET!" (campaign), the points as above (on every such call), and the group fades
  out at 0.5 a second.
- `Levels::onActivateBlock` calls `updateBossStatus` (setupUI) for each new block.

## ChallengeText [R ChallengeText.scr]
Loaded by BeginBlockNI. A framed card (GuiRect `ctRect` at 70 140, 500 wide, green frame) for 3 s (the boss block's
8 s), then it fades out (ObjectFader, 0.5 s) and unloads. It unloads at once unless the challenge has an identifier
(not "Xo oX"), items required > 0 and the play is the campaign.
- Not the boss: the title `CT_Challenge` or `CT_Challenge_Upgrade` (pulsing: scale 1 + k sin(6t) / 4, k × 0.975 a
  frame), the challenge's text (its identifier, a key of `<City>.loc`: "Eat 3 Hamburgers"), "Reward: " + the reward
  (`ChallengeReward` or `Upgrade<n>`); height 115; the icon at 45 60.
- The boss: `ChallengeBossTitle`, `ChallengeBoss`, `ChallengeBossObjectives`, `ChallengeBossHint` of the city's table,
  stacked by their heights (+8), the frame 87 + their heights tall; the icon at 45 35.

## Results screens (WorldGuiDriver) [R WorldGuiDriver.scr]
`startResults(n)` loads `GameOver`, `Results`, `LevelResults`, `WorldResults`, `KingBlockResults`, `KingCityResults`,
`KingWorldResults` or `TimedRunResults` (7, 8, 9), gives it the focus, `nResultState = 0`. The screen answers with
`setResultState` (1 done, 2 continue, 3 select, 4 quit; game.c polls `getResultState`) and asks to go with
`endResults`; the driver unloads it on its next frame.

### Results (the campaign's block) [R Results.scr]
A news flash: "RAMPAGE ALERT", "CHANNEL 3", a grey data field, an orange band, a "Live" box, buttons.
- **Intro** (each item a straight move from/to, scale and fade): title bars from x −640 (0–0.5 s); "Rampage Alert"
  and "CHANNEL" drop in (0.33–0.75); the 3 slides in at scale 3 → 0.85; the grey field and the orange band
  (0.75–1.0); the scrolling title, the point list, the total and "TOTAL DAMAGES:" (1.25–1.5); Continue / Change
  Monster / Quit fade in at 2.05, 2.15, 2.25 (+3 s when saving is allowed). `Slot_Jackpot` plays from the start.
- **Scrolling title** (from 1.5 s): the block's name `format(localize("Results_CompletlyDestroyed"),
  localize(getBlockName(0)))` ("Downtown - COMPLETELY DESTROYED"), or, on the city's last block with buildings left,
  the block's `ResultsNIText`; two copies 150 apart move left at 150 a second, wrapping between x 62 and 503. The
  layout's `M 0x310` boxes at the ends do **not** cut the title [R]: the box's shader is 0x17D100's depth-only one
  (TEST 0x52001: ATST NEVER, AFAIL ZB_ONLY, no colour; M 0x10 depth test, 0x100 depth write, 0x200 the depth-only
  shader and the bias) drawn with proj[3][2] −= −0.001 (0x17C180 .. 0x17C19C → 0x25B4D4): z_ndc + 0.001, farther.
  The GUI's camera (Gui +0x24C, 0x100AD0) is orthographic, near 0 far 1000, the depth range 0 .. 1 in every viewport
  setter (0x16BA78, 0x16B8B0 → 0x25BAF0), Z = 16777210 − 16777205 z_ndc (0x25B500 ..): the glyphs at layout z 0 get
  16777210, the boxes about 16760433, and the glyphs test GEQUAL (OFont +0x5C 0x10 by default, 0x1B72E8; 0x1B6590)
  without writing depth, so they pass over the boxes; a clear (0x231900) or none changes nothing. The port draws
  the title unclipped (but at the PS2 screen's edges, x 0 and 640, which the viewer's wider frame would
  pass);
  "Live" blinks every 0.5 s.
- **Count-up** (from 1.5 s): the lines destruction, people, vehicles, bonus, time bonus, challenge — the point
  categories 0..5 summed over the players — appear one by one, each counting from 0 in 0.5 s, the total with them
  (`%i,%03i` from 1000). People and vehicles show the kills. On the last block the challenge line reads "Boss
  Defeated", and "No" when its points are 0. `Slot_Jackpot` stops at the end.
- "Monster Revealed" fades in between 1 and 5 s after the intro when a monster was revealed (not on the last block).
- **Buttons** (once the intro is done): Continue (X / Start) — "MOVING ON TO <next block>" in the title, then the
  walkout (the field, list, total, band and buttons slide away in 0.5 s); Change Monster (button 0) — result 3; Quit
  (Back) — the ConfirmQuit dialog, Yes → result 4. Change Monster and Quit are hidden once the city is destroyed.
- **Save** (Results.scr:215, 354): at 1.5 s `updateProfile`, `saveProfile` and `bTrackSave`; from then on each frame
  the Save button ("Autosave", button 1) shows while `hasLoadedProfile() && !getAllowSave()` (a profile created
  or loaded, autosave off), and when it goes from shown to hidden `saveProfile` runs. Button 1 while it shows (not in
  the walkout): `Click`, `setAllowSave(1)`, 0.25 s before Continue answers. Continue clears `bTrackSave`. The
  answers (`exitCallFunction`) wait while `gCommon.isSaveGameActive()` (Results.scr:200). The port:
  HUD_CALL_SAVE_PROFILE (arg 1 at 1.5 s: updateProfile first) and HUD_CALL_ALLOW_SAVE; key V, `menu b1`. The
  viewer's saveProfile opens the SaveGame popup over the screen (shell.c `shell_open_popup`): the SaveIcon at the
  bottom left while it writes, an error's message (a file that cannot be written: "... damaged and cannot be
  used.") with the pad, and the held answer waits for it (`hud_set_save_active`); its warn-continue's Don't Save
  shows the Save button again (`hud_set_allow_save`) [SHELL_NOTES.md "The memory card's states"].
- **The end**: after the walkout, result 1 (DONE), 5 s more on screen, a 0.5 s fade, `endResults`. The game goes on
  to the next block meanwhile (game.c moves on at DONE).
- The script's `processIntro(dt, niFlag)` clears `bDoIntro` through its reference: the PP interpreter passes a
  writable variable uncopied (0x1B9870 / 0x1B9A20 copy only constants and temporaries; 0x1C3270), and assignment
  writes through it (0x1BBAC8) [R].
- Buttons are PS2-coloured by `fixButtonColors` (ButtonSetup.h): (0, 186, 255), (190, 45, 39), (220, 108, 194).

### LevelResults (the campaign's city) [R LevelResults.scr]
The city's name (Atrox60 × 1.5) and "DESTROYED" slide in, "TOTAL DAMAGES:" the city score and "PAR SCORE:" the par,
four goals (All Blocks Destroyed, Par Points Gained, All Challenges Met, All Monsters Revealed) rise from below
(0.5, 0.7, 0.9, 1.1 s); then a star (`Tank_Fire`) every 0.75 s for each of the city flags 1, 2, 4, 8. Continue →
result 1, 5 s, fade, end. No walkout.

### GameOver [R GameOver.scr]
A dark full-screen veil and a framed box: "GAME OVER" on a red band, the list Retry / Choose Monster / Quit (Choose
Monster hidden in the Timed Run; the list's highlight and growth as [PAUSE_NOTES.md](PAUSE_NOTES.md) "The list
box"). Up / down move the selection (`Rollover`), X: Retry → result 2, Choose Monster → 3
and `endResults` at once; Quit → ConfirmQuit, Yes → 4. `gApp.pauseGame(1)` while it shows.

### ConfirmQuit (ConfirmQuit.h) [R]
A frame 480 wide holding `ConfirmQuit` ("Unsaved progress ... quit?") or `ConfirmQuitShort`, the list No / Yes
(left / right) 15 below the text, centred on the screen, fading in; the selected one grows to 1.15 about its
middle (`<growth>3 1.15 4 16`); X answers, Back closes.

### The other screens: the common pattern [R]
WorldResults, KingBlockResults, KingCityResults, KingWorldResults and TimedRunResults are written like Results.scr:
`setupIntroItem(name)` takes an object's place, scale and fade as both ends (fade 1), the script then sets the start
and end time and the ends it changes; `setupIntro` puts each intro item at its start. `processIntro` moves the items of
the state's list (`niState` 0 the intro, then the walkout; KingBlockResults has more, below) and, when all are done,
`niState++`. Like Results.scr's its flag is passed by reference (above) [R]. The walkout's items
are set up at the start, so they start where the objects were at the intro's start, not where the intro left them.
The King screens and TimedRunResults reuse Results' news flash: the same first ten intro items (title bars, "Rampage
Alert", "CHANNEL 3", the grey field, the orange band, "Live", the buttons, the scrolling title from 1.25 s), the same
walkout of the field, band, buttons and "Live", the same title scroll and "Live" blink from 1.5 s, buttons at 590 less
the widest shown, the PS2 button colours and (except KingWorldResults) ConfirmQuit for Back. After the walkout: result
1, 5 s more, a 0.5 s fade, `endResults`. "P" + (n + 1) is a key of Game.loc ("P1"); monster names are localized keys
("Boosy" is "Bubba"). The scrolling title runs between 62 and 503 in each (`gtTitleLeftX` / `RightX`) [R].
`getMonsterName(-1)` (a city with no King: KingCity / KingWorld do not test for one) reads the status record's
+0x14 − 4 = +0x10, the start block, as a roster index without a test (0x2C4B18 → 0x2C36A0: gCommon +0x10, 12 bytes
a monster, the name at +8): the tie names the roster's monster of the start block's number [R] (hud.c `kr_monster`,
HudResults.monster_none).

### WorldResults (the campaign's end, state 11) [R WorldResults.scr]
"TOTAL DESTRUCTION!" (Atrox60 × 1.25, from x 960) and "ALL CITIES DESTROYED!" (from −320) between two red bands
(from −320 and 640; the script sets the second's start twice, 960 then 640), all in 0.5 s; Continue fades in at 1.55
s. X: the `fade` quad (black, 649 × 489) is shown and fades in over 0.5 s; then result 1 and `endResults` at once. No
Back, no ConfirmQuit, no dt guard.

### KingBlockResults (a King mode's block, state 7) [R KingBlockResults.scr]
- The block's King (`getKingOfBlock`): "P2: Lizzie" / "Wins this block" slide in at 2.25 s, Continue fades in at
  2.45; with none (a tie) "It Is A Tie!
Block will be replayed" and the Replay button instead.
- The points list: the players by points (category 6, an exchange sort as written, not stable), counting up 0.5 s each
  from 1.5 s (`Slot_Jackpot` until the last); the first one's count is also the big `winnerScore` (in at 2.5 s).
- States: 0 the intro; X → 1 `rankItems` ("CITY STANDINGS" and `br_list` slide in from the left, 0.25..0.75 s: the
  players by blocks won, "blocks/to win"; the points list and score drop, the winner lines fade out); X → 2 the
  walkout ("MOVING ON TO <the next block>"); 3 result 1. With a King of the City (`getKingOfCity() != -1`) the first
  X runs `altWalkoutItems` instead (the field, list, score, band, buttons, "Live" go; the winner lines fade over 1 s,
  the tie text 1..2 s) and the state jumps from 1 to 3.
- As written, the walkout's `br_winner` / `br_list` items start at the intro's start x (−320, −265): the standings
  leave the screen at once instead of dropping; the rank move of the points list starts at y 216, not the 281 the
  intro left it at.
- Quit hidden when the city is destroyed or has a King; Back asks ConfirmQuit then (Yes → 4).

### KingCityResults (state 10, both King modes) [R KingCityResults.scr]
"P2: Lizzie" / "is King of Las Vegas" (`getKingOfCity`, the city's localized name), the title "<city> - COMPLETELY
DESTROYED", the standings: "CITY STANDINGS" (blocks won of `getNumBlocksToWin`) or, in King of the World, "WORLD
STANDINGS" (cities won of `getNumCitiesToWin`), the rows sorted by blocks won in both. X: King of the City → result
4 at once (to the menus; no walkout); King of the World → "MOVING ON TO <getCityName(getCityIdx() + 1)>", the
walkout, result 1. Quit hidden in King of the City or with a King of the World. A city never runs out of blocks
without a King: with 6 − players to win and ties replayed, p players share at most p(5 − p) ≤ 6 decisive blocks,
and every city has 8 to 10 [R].

### KingWorldResults (state 11, King modes) [R KingWorldResults.scr]
"THE WORLD COMPLETELY DESTROYED" scrolling, "P2: Lizzie" (the script asks `getKingOfCity()`, the last city's King,
who is the world's) / "is King of the World", the points list (not moved by the intro) with cities won of those to
win, sorted by blocks won. Quit hidden; OK (X) → result 4 at once. The script never starts its walkout, has no
ConfirmQuit, and adds the Continue fade item twice.

### TimedRunResults (a Timed Run's block, state 7) [R TimedRunResults.scr]
- "Block Destroyed" (fading in 1.0..1.5 s) and, from 1.5 s, counting up 0.5 s each: "Time Remaining:" total − added,
  "+ Bonus Time:" added, "= Total Time:" total, with total `gWorld.getGameClockTime()` (the seconds left: the block's
  rest plus the next block's TimeLimit, the clock being reset and the next block read before the screen) and added
  `gWorld.getGameClockLimit()` (World binding 81, 0x2AF818: the clock's +0x30, the TimeLimit, 180). Times are ":ss"
  under a minute, else "m:ss". (The label's key is `Resullts_TimeRemaining`, which Game.loc has.)
- The city done: "YOU WIN!
City Destroyed" and "Time Remaining:" the clock's seconds; no count-up; Quit hidden.
- X: "MOVING ON TO <the next block>" when the block names differ, the walkout, result 1. Back (Quit shown):
  ConfirmQuit, Yes → 4.

### Not ported
Nothing of these screens' scripts. (The Pause menu is ported: [PAUSE_NOTES.md](PAUSE_NOTES.md); the save button and
the autosave are, above, through the SaveGame popup of [SHELL_NOTES.md](SHELL_NOTES.md).)
