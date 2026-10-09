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
  `Monsters::startRage` loads `Rage` with none (its defaults: "Rampage", red, scale 1, `v_rampage`).
- `Rage.ifc`: the text `Rage` at 320 240, Atrox60, centred both ways, s 1.4, charspacing 1, maxwidth 500, with a
  hidden `line2` (ArialBlack24, s 1.2, 30 below), and a quad `Head` (HUD_GEORGE_HEAD, scale 4).
- `setPointsBannerData`: message, colour, the text object's scale (0.5 for edibles and the reveal, 1.0 for challenges),
  hold 0.35 s, the sound unless `"Default"`; the interface is renamed `Removeable` (`gWorld.hideChallengeCompletionUI`,
  0x2B01C0, hides those).
- **The name**: `localizeString(name)`; if it begins with `#` the text is the rest, as is (the reveal builds its
  text itself). The texts are in `Game.loc` (WorldGui.bdp): `InstantRampage` "Instant\nRampage", `SpeedIncrease`
  "Speed\nBoost", `Challenge_Met` "CHALLENGE MET!", `MonsterRevealed` "%s Unlocked", `RAMPAGE`. Keys ignore case
  [G]. `Edible.scr` turns the first `\n` (two characters) of its Message into a line break first.
- **Motion** (`Rage::doInterface`, from its second frame; the first sets up):
  - setup: the text, colour, scale; 15 copies of the text (with `line2`), fades 0, 1/15 .. 14/15; the text hidden;
    the sound played.
  - 0 .. 0.75 s: with p = t / 0.75, copy i has fade √p (15 − i) / 15 and sits at the text's place plus
    (1 − √p) 100 cos(1.3 i) along the angle (i − 4) / 6 + p: they swirl in to the centre.
  - then for `hold` s: the copies hidden, the text at fade 1, shaking at (320 + 3 cos 2a, 240 + 2 sin 3a),
    a = 2π · the hold's progress.
  - then 0.5 s: its fade falls from ½ to 0; then the interface unloads itself.
- **[G]** The `Head` quad is never hidden by the script, but it would put George's head (whatever the monster) at 4×
  in the middle of the screen: it is not drawn.
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
  the block's `ResultsNIText`; two copies 150 apart move left at 150 a second between x 62 and 503 (the layout's
  `M 0x310` boxes mask the ends [G]); "Live" blinks every 0.5 s.
- **Count-up** (from 1.5 s): the lines destruction, people, vehicles, bonus, time bonus, challenge — the point
  categories 0..5 summed over the players — appear one by one, each counting from 0 in 0.5 s, the total with them
  (`%i,%03i` from 1000). People and vehicles show the kills. On the last block the challenge line reads "Boss
  Defeated", and "No" when its points are 0. `Slot_Jackpot` stops at the end.
- "Monster Revealed" fades in between 1 and 5 s after the intro when a monster was revealed (not on the last block).
- **Buttons** (once the intro is done): Continue (X / Start) — "MOVING ON TO <next block>" in the title, then the
  walkout (the field, list, total, band and buttons slide away in 0.5 s); Change Monster (button 0) — result 3; Quit
  (Back) — the ConfirmQuit dialog, Yes → result 4. Change Monster and Quit are hidden once the city is destroyed.
- **The end**: after the walkout, result 1 (DONE), 5 s more on screen, a 0.5 s fade, `endResults`. The game goes on
  to the next block meanwhile (game.c moves on at DONE).
- The script's `processIntro(dt, niFlag)` only works if `niFlag` is passed by reference (by value, `bDoIntro`
  would never clear and no button would be taken) **[G]**.
- Buttons are PS2-coloured by `fixButtonColors` (ButtonSetup.h): (0, 186, 255), (190, 45, 39), (220, 108, 194).

### LevelResults (the campaign's city) [R LevelResults.scr]
The city's name (Atrox60 × 1.5) and "DESTROYED" slide in, "TOTAL DAMAGES:" the city score and "PAR SCORE:" the par,
four goals (All Blocks Destroyed, Par Points Gained, All Challenges Met, All Monsters Revealed) rise from below
(0.5, 0.7, 0.9, 1.1 s); then a star (`Tank_Fire`) every 0.75 s for each of the city flags 1, 2, 4, 8. Continue →
result 1, 5 s, fade, end. No walkout.

### GameOver [R GameOver.scr]
A dark full-screen veil and a framed box: "GAME OVER" on a red band, the list Retry / Choose Monster / Quit (Choose
Monster hidden in the Timed Run). Up / down move the selection (`Rollover`), X: Retry → result 2, Choose Monster → 3
and `endResults` at once; Quit → ConfirmQuit, Yes → 4. `gApp.pauseGame(1)` while it shows.

### ConfirmQuit (ConfirmQuit.h) [R]
A frame 480 wide holding `ConfirmQuit` ("Unsaved progress ... quit?") or `ConfirmQuitShort`, the list No / Yes
(left / right) 15 below the text, centred on the screen, fading in; X answers, Back closes.

### Not ported
WorldResults, KingBlockResults, KingCityResults, KingWorldResults, TimedRunResults: `hud_results` shows nothing and
answers DONE after 3 s or on X **[G stand-in]**. The save button and profile saving, `hideChallengeCompletionUI`, the
Pause menu.
