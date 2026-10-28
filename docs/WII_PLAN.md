# Plan: the Wii's monsters and Dallas in the port (issue 3)

A port extra, off unless the player points the port at their own extracted Wii disc. With `wii_data=` empty the
port must behave and save exactly as it does now. Nothing from the Wii disc ships with the port.

The Wii's formats are worked out and read by the tools (`docs/FORMAT_BDP.md` "The Wii's bundles", FORMAT_EDF.md,
FORMAT_GUI.md, FORMAT_SOUND.md). This plan is about the C port (`src/`).

## What the Wii disc adds, and what the port lacks for it

| Wii content | Where (DATA/files) | The port today |
|---|---|---|
| 10 monsters: mesh, textures, skeleton, clips, cloth, MONSTER_DATA | `Game/<Name>.cmg`, `<Name>_anims.cmg` | `bundle.c:17` takes only 1.3 / little endian; no GX textures or meshes |
| Dallas: 11 blocks, props, buildings, string table, loading screen | `Game/Dallas*.bdg`, `Shell/Loading_Dallas.bdg` | `.bdp` hardcoded (`city.c:155`, `shell.c:2993`); 7 cities hardcoded (below) |
| The roster: 40 monsters, 8 cities, stats, power-ups per city | `Shell/ShellData.bdg` (`MonsterData`, `CityData`) | 30 / 7 everywhere (below); the shell reads the PS2's ShellData |
| Cards, heads, HUD heads, names, mutation texts | `Shell/Shell.bdg`, `Game/WorldGui.bdg`, `.loc` | looked up by name, so they only need to be found |
| 153 sound events (the new monsters' voices) | `Audio/Rampage.uber/.samp`, DSP-ADPCM | `sound.c` reads one PS-ADPCM bank |
| 4 effects (`brain_attack`, `plucky_feathers`, `debris_boots`, `oilwell`) | `Game/fx.bdg` | `fx.c` reads `fx.bdp` |
| The fifth upgrade, SuperJump (flag 16), and the moves that use it | property books, MONSTER_DATA | four upgrades (`challenge.c:53`, `profile.c:572`) |
| Music and movies | Bink (`Video/*.bik`) | not needed: Dallas plays `bbop`, `hoodia`, `jerrys_labotomy`, `killer`, `level_02`, `level_03`, `pump_it`, all in the PS2's `.mic` |

The port does not run `.bsf` scripts: their logic is carried by hand into C (`docs/COVERAGE.md`). So whatever the
Wii's scripts add (Dallas's two bosses, the new monsters' calls, SuperJump) has to be written in C.

## Decisions (each with the recommendation first)

1. **How the Wii data comes in: an overlay.** The PS2 disc stays the base; the Wii folder only supplies files the
   PS2 has not got (`Boris.cmg`, `Dallas*.bdg`, ...) and, for things looked up by name (a texture, a string, a
   sound event, an effect), answers when the PS2's data has no such name. PS2 content keeps the PS2's data, so the 30
   monsters and 7 cities look, sound and play as now. The alternative, running everything from the Wii data, would
   make the port a Wii port (its controls, its art) and needs Bink.
2. **The roster and city order: the Wii's, when `wii_data` is set.** Its `ShellData` lists the 40 monsters (the new
   ones mixed in: Boris is 6, Cal moves from 6 to 8) and 8 cities (Dallas fourth: Las Vegas, San Francisco, London,
   Dallas, Los Angeles, Chicago, Hong Kong, New York). The port keeps its own order in memory and saves by name
   (decision 3), so either order is safe. The alternative is to append: the PS2's 30 then the 10, Dallas eighth
   (but then "New York is the last city" in `game.c:837` and King of the World's chain need care).
3. **The save: the PS2 profile untouched, plus a side file.** `rampage_profile.bin` stays the PS2 card's 0x178
   bytes (its size is checked, `profile.c:490`; there is no room: monster 30's slot is the city flags,
   `profile.c:544`). A second file next to it, e.g. `rampage_profile.wii.bin`, keeps by name the 10 monsters'
   flags, Dallas's city flags and start block, the SuperJump bits (16) of all 40 and the players' picks when they
   are Wii monsters. Loading merges by name; saving splits. A profile used without the Wii data loses nothing.
4. **Unlocks: the Wii's** (read in phase 0, [WII_NOTES.md](WII_NOTES.md)). Boris and Ruby from the start (the
   Wii starts monsters 0..7, the PS2 0..5); Plucky, Fifi and Cyril revealed in Dallas (its `MonsterRevealDef`, which
   the port already reads by name); Squirmy, Flea, Sarah, Brian and VERN when 1, 3, 6, 12 and 20 monsters have all
   five upgrades (`n5StarUnlock`). That last rule counts SuperJump: it needs decision 6.
5. **Controls of the Wii monsters' moves: map the Wii's inputs onto the pad** (read in phase 0,
   [WII_NOTES.md](WII_NOTES.md)). The Wii inserted ten motion inputs at 30..39 (the remote's and the Nunchuk's
   accelerometers), so its ids from 40 on are the PS2's + 10; its buttons are A 14 (the PS2's Square moves), B 15
   (Cross), 2 17 (Circle), 12 (Triangle). Two gestures stand for button presses: a hard sideways swing for Circle,
   a hard downward swing for Triangle. George's graphs line up: 937 of the 941 shared (state, target) pairs have as
   many transitions. So at load a Wii graph's conditions are renumbered, A / B / 2 / 12 become Square / Cross /
   Circle / Triangle, and the two swings become Circle and Triangle presses. The 30 PS2 monsters keep their PS2
   graphs.
6. **The fifth upgrade, SuperJump: later, Wii monsters first.** It needs a move bit (16), a fifth star, a fifth
   `Upgrade|` group per block and an answer for `Monsters::hasSuperJump` (Wii `Monsters.bsf:846`); the PS2
   monsters' graphs have no `Jump_Super` state. Phase 5. Without it the five-star unlocks of decision 4 cannot
   happen (no monster reaches all five upgrades), so until then those five need a stand-in rule (open: four upgrades
   counted instead of five, or available from the start).
7. **Four players:** the Wii drops `DisableFor4Player` (its monster select has no such test). Keep the port's
   setting (`four_players=`) deciding as now; the Wii monsters have no such flag.

## Phases (one commit per step, each checked like the tools were)

**Phase 0, reading the Wii's code (no port changes). Done:** [WII_NOTES.md](WII_NOTES.md) (the unlocks, the
input ids and buttons, the Wii graph against the PS2's), `tools/wiidis.py` (the Wii ELF disassembled with its
symbols).

**Phase 1, reading Wii files in C.** Each its own commit, each checked against the tools' numbers:
1. `wii_data=` in `rampage.cfg` (`settings.c` `set()`, `Settings`), the command line `--wii DIR`.
2. `Vol` gets an overlay: a scan of `DATA/files/{Game,Shell,Audio}` into a second file table, each file marked Wii;
   `vol_find` / `vol_read` look in the PS2's first, then the overlay; `.bdp` / `.cmp` asked for also finds `.bdg` /
   `.cmg`. `monster_names` (`city.c:3853`) lists both.
3. `bdp_load` (`bundle.c`) takes 1.4 / 1400; a `Bundle.be` flag; read helpers that follow it (`rd32b(b, p)`), or the
   big-endian entries swapped once at load into the PS2's layout. Swapping at load keeps every decoder as it is
   (moves.c alone has 85 reads) and is the recommended way; the swap is per entry type, from the tools' layouts.
4. Textures: a GX decoder (`tex.c`), CMPR, RGB5A3, IA8, I8, RGBA8, I4, to RGBA, as `tools/tex.py`.
5. Meshes: the display-list reader into `MeshPart` (`mesh.c`), as `tools/mesh.py`; `MeshPart` gets 4 bones and 4
   weights (the skinning already loops over 4, `city.c:3328`).
6. Cloth: 72-byte particles (`tree.c:65`, `cloth.c:256`), the held flag a u16.
7. Property books, string tables (the kind bytes; `prb.c`, `loc_find` in hud.c and shell.c).
Check: `RampageRE-dev.exe Dallas 4` draws the block; `--monster Boris` walks in a PS2 city (with George's controls until
phase 2); `--shot` images of each of the 10 against the viewer's glTF.

**Phase 1 done** (92fe225 .. adff860): `RampageRE-dev.exe build\disc\RAMPAGE.VOL Dallas 4 --wii DIR` draws Dallas's blocks
(the viewer's eighth city, key 8) and `--monster Boris` (any of the ten) plays a Wii monster in any city; the computer
drives Boris through three Dallas blocks (`--autoplay`). Every reader was checked against the tools on every Wii file
(textures, meshes, cloth, books, string tables, MONSTER_DATA by George's PS2 and Wii records), and the PS2 game
stays byte for byte the same with and without `--wii`. Found on the way:
- `MONSTER_DATA` has 16-bit fields the PS2's little endian hid (docs/FORMAT_BDP.md);
- a Wii cloth mesh has no vertex -> particle table: its vertices lie on their particles;
- shared books (`Objects1_props`) come from the PS2, so Dallas's templates the PS2's lacks (cows, oil towers, its
  people) are taken from the Wii's copy for the groups the PS2's has not got (city.c `wii_twin`);
- the human player's pad drives a Wii monster's graph with the PS2's input numbers until phase 2 maps them.

**Phase 2, the Wii monsters playing.**
1. The input table of decision 5 applied to Wii graphs at load (`moves_read`).
2. The Wii sound bank as a second bank: its 153 events not in the PS2's, DSP-ADPCM (`sound.c` `sample_get`).
3. Effects and GUI names from the overlay (`fx.c`, `tex_find` in hud.c and shell.c).
4. The 10 monsters' script calls (`BORIS.bsf` ... `FABIO.bsf`, `VERN.bsf`) checked against `answer()`
   (`main.c:1634`) and the clip events; any new call carried into C.
Check: each of the 10 in `--autoplay` through a Las Vegas block: punches, climbs, eats, roars, dies and respawns;
the event log shows no unanswered call.

**Phase 2 done** (397cadd .. 9beca6d): a Wii monster plays on the PS2's pad, with its own voice, effects, HUD head and
script calls. In a Wii graph, 23,664 of the 32,337 transitions the 30 shared monsters have now carry the PS2's exact
conditions. The rest differ by the Wii's own thresholds. The Wii's sound bank decodes all 696 samples, and its
fx.bdg adds the four effects only it has. Each of the ten autoplays a Las Vegas block with every call answered, and
the PS2 game stays byte for byte the same with `--wii`. Found on the way:
- on the ground the Wii swapped punch down and kick; the PS2's inputs are put back (`moves.c` `wii_ground`);
- `fx_open` kept its caller's bundle list, which was on the stack; it is now copied;
- the HUD looked the monster's head up before any bundle was added; it now looks again (`find_head`);
- a Wii monster's property book is the Wii's copy. Brian has Rhett's, where the Wii has `Weapon|Brain` in place of
  the PS2's Breath (`mon_props`, `city_book_group_wii`);
- the ten's own calls: `Brian::fireWeapon` ("brain"), `Cyril::crazyCyril` (setSpeedScale 1.5 for 35 s, on his
  Expression_Zap), Cyril's and Fifi's swaps (Hazard_Cyril_Shock, Powerup_Crock_Cat), `Monsters::hasSuperJump`
  (upgrade 16, cheat 8 or the rage: the upgrade waits for phase 5); `<Name>::EnableWeapon` stays without effect,
  as on the PS2 (no `Expression_Weapon` node);
- Sarah's `aqua_aha_2` and `aqua_growl_8` are in neither platform's bank.

**Phase 3, the 40-monster roster and the save.**
1. Lists sized by the data, not 30: `PROFILE_MONSTERS`, `SHELL_MONSTERS`, `MsPlayer.monsters`,
   `ShellProfile.monster_flags`, the reveal glue (`main.c:1377`, `4258`), `gs_from` (`shell.c:3620`), the attract
   mode's draw (`shell.c:3156`).
2. The roster from the Wii's ShellData when `wii_data` is set (decision 2); the profile mapped by name.
3. The side file (decision 3), with its own magic and version.
4. The monster select and card sheet for 40 (cards in pairs, `right_bounds`, the node budget `shell.c:5446`).
5. The unlocks of decision 4.
Check: a PS2 profile loaded and saved with and without `wii_data` stays byte for byte the same; a Wii monster
unlocked, saved, quit, loaded; the monster select scrolls through 40.

**Phase 3 done** (4a766b8 .. 2416753): with a Wii folder the shell, the profile and the reveals use the Wii's roster
of 40 in its order. The monster select and the card sheet show all 40, the card sheet in 20 columns of pairs. The
PS2's `rampage_profile.bin` stays its 0x178 bytes, its monsters mapped by name. What it has no room for goes to
`rampage_profile.wii.bin`: the Wii's monsters' flags, every monster's SuperJump bit and the players' picks of Wii
monsters. Boris and Ruby are open from the start, and the five-star rule unlocks Squirmy, Flea, Sarah, Brian and
Vern. Without the folder nothing changes: pictures, logs and the profile file match byte for byte, apart from the
save's time stamp and CRC. Decided or found on the way:
- decision 6's stand-in: until SuperJump (phase 5), the five-star rule counts the PS2's four upgrades (0xF);
- the Wii changed the cities' reveal lists, dropping some PS2 monsters and adding its own. A Wii monster playing
  uses the Wii's list, and a PS2 monster keeps the PS2's (`reveal_world_data`);
- the Wii's MonsterData has no `DisableFor4Player`, so the shell reads it from the PS2's group of the same name;
- a picked Wii monster is saved in the PS2 file as that player's start pick, and the side file keeps the real pick
  while the PS2 file is still the one it was written with (its CRC);
- the shell's `--do` gets `until NAME`, because the boot's movies run on the clock.
The cities stay the PS2's seven in the PS2's order until phase 4.

**Phase 4, Dallas in the game.**
1. Cities sized by the data: `CITIES` / `CAMPAIGN` / `NCITIES` (`main.c:61-71`), `PROFILE_CITIES`, `SHELL_CITIES`,
   `num_cities` (`main.c:1387`, `game.c:1099`), the city select (`shell.c:5177`, `5185`: the Wii's `< 4` and 8
   rings), King of the World's chain (`main.c:3959`), the last-city rule (`game.c:837`).
2. Dallas's boss block, DA09 Fort Worth: a tank and a helicopter, both to finish (`Dallas::setBossTankFinished`,
   `setBossHawkFinished`), and a second boss bar (`ChallengeBoss2.ifc`; `boss.c` reads `HudId`).
3. The unlock rules that need every city (Philbert, Eyegore, Boosy, `profile.c:285-321`) over 8 cities when Dallas
   is in.
Check: the campaign through Dallas with `--autoplay`; Dallas's results; the PS2 campaign unchanged without
`wii_data`.

**Phase 4 done** (ae034ba .. 29f00ae). With a Wii folder the campaign has the Wii's eight cities in its order,
Dallas the fourth. The city select shows them all, using the Wii's own `CitySelect.ifc`, and its globe turns to
Texas. The save keeps the PS2's seven cities by name, and Dallas's flags and start block go to the side file. The
autoplay played Dallas from a saved block, won DA09 Fort Worth and went on to Los Angeles. DA09 shows the Wii's
second boss bar for the tank. Philbert, Eyegore and Boosy then need Dallas as well, and Dallas reveals Plucky,
Fifi and Cyril as the Wii's data says. Without the folder the PS2 game is unchanged. Found on the way:
- Dallas also spells its key `xxIsBlockFinished`, so as in the other cities its block ends when the buildings are
  down, and `setBossTankFinished` / `setBossHawkFinished` are never read;
- the Wii's two boss bars count as met only once both are beaten: each bar's `updateBossHealth` asks the other's
  `bossDefeated`;
- the Wii's CitySelect script also hides the globe while unfocused and stops only rings below 7, which leaves New
  York's ring on; the port takes neither;
- a status in Dallas is saved in the PS2 file as the first PS2 city not both open and destroyed;
- `--do "bosshit n part"` hits one part of a boss block, such as Dallas's tank.
Left for phase 5: SuperJump and the fifth star.

**Phase 5 (optional): SuperJump** (decision 6), and the other platforms (Android, Switch, Vita: where the Wii
folder goes).

**Phase 5's SuperJump done** (c220768 .. b005948). With a Wii folder the
block's upgrade comes from the Wii's lists, for all 40 monsters (chosen: the Wii's ten were in no PS2 list, and the
Wii moved some PS2 monsters to make room for SuperJump). Taking SuperJump sets bit 16, which is saved in the side
file. The Wii's ten can do the move: a fresh Cross press while landing (the Wii's B) goes into `Jump_Super`. The PS2's
thirty have no such state in their graphs, so for them it is the flag and its star only. The monster select and card
sheet use the Wii's layouts with five stars, and the five-star rule counts 0x1F, so Squirmy, Flea, Sarah, Brian and
Vern can all be reached. Found on the way:
- the Wii's monsters never got an upgrade from the PS2's lists, which don't name them; phase 3 had missed this;
- the Wii's card layouts need their own pictures first (its star bar has five slots, the PS2's of the same name
  four), so a Wii layout now looks in its own bundle first;
- the Wii's card sheet stamps locked monsters with its padlock and keeps black for another player's pick; in four-
  player games it puts the mutation text on the card itself;
- SuperJump's instruction text reads "Press and Hold 1.25Controller_Back while Landing": a Wii button name inside
  the text, shown as it is.

**Phase 5's other platforms done** (12d8940 .. 45889e3). On the Switch, the Vita and Android a `wii` folder in the home
folder (`sdmc:/switch/rampagere`, `ux0:data/rampagere`, the app folder) holding the Wii's Game, Shell and Audio is
used on its own, as the game's data there is; `wii_data=` still points elsewhere and `wii_data=none` leaves it out. The
PC builds stay explicit. On Android the app also copies it: its picker takes the extracted disc, its `DATA/files` or
the folder with `Game` in it, from the launcher's shortcut "Add the Wii's data" (before the data is read on a fresh
start, at once while running) and once after the first start's copy. Found on the way:
- nothing in the Wii code was Windows-only: `vol_add_wii` already read folders with opendir / stat elsewhere;
- the port reads Game, Shell and Audio alone, 317 MB of the disc's 716 MB (88 MB of it the .zip files, which it does
  not read either), so only those go onto a card or a phone;
- on the emulator, folders that `adb shell` makes in the app folder before the app's first start are not the app's to
  open (the files folder is then the shell's): the data goes in after a first start, or through the picker.
Untested: a real Switch, Vita or phone (no hardware here), a USB cable's (MTP) copy on Android. The builds are:
rampage.nro, rampage.vpk and the APK, and the Android flows on the emulator (NATIVE.md "Android").

**The Android bundle** (afterwards): `make_android_bundle.bat` takes the Wii folder too (dragged on, else
`rampage.cfg`'s `wii_data=` or a `wii` folder beside it) and adds its Game, Shell and Audio as `assets/wii/...`, stored
and aligned as the disc's files (about 1.6 GB in all, still under 2 GB); `vol_add_wii` reads a ZIP's `assets/wii` as it
does a folder, which Android uses when nothing else names the Wii's data.

**The Windows standalone exe** (afterwards): `make_standalone.bat` takes the Wii folder the same way (the file list is
`tools/native/wiifiles.h`, shared with the Android bundle); `pack.c` adds Game, Shell and Audio as `wii/...`, the
launcher unpacks them into `%LOCALAPPDATA%\RampageRE\wii` and hands `RampageRE.exe` that folder through
`RAMPAGE_WII_DATA` when neither that variable nor the folder's `rampage.cfg` names another.

**Asked once on Windows** (afterwards): `RampageRE.exe` asks for the extracted Wii disc at a start when nothing names
it, as it asks for the PS2 disc at the first: Yes, the folder dialog (checked for Game); No or Cancel, `wii_data=none`.
The answer goes into `rampage.cfg`, so it is asked once, existing setups included; `wii_data=none` is now silent.

**Dallas for a PS2 save** (afterwards): a PS2 profile loaded with the Wii's cities opens the city `updateProfile` would
have opened at the last city destroyed, the first not both open and destroyed, when the PS2 file has not got it. So a
PS2 game past London (or finished) finds Dallas open at block 1, as a Wii game would; one that has not destroyed
London yet opens it the Wii's way, after London (`profile.c` `wii_next_city`, at `profile_read`).

## Risks and unknowns
- The input table (decision 5): George's graphs line up (937 of 941); the Wii-only monsters have no PS2 graph to
  check against, so their translated graphs are checked by playing them. Id 12's Wii button is not known [G].
- The five-star unlocks need SuperJump (decision 6) or a stand-in rule: SuperJump came in phase 5.
- Dallas's second boss bar and its two-boss finish are new C, not data.
- Size: phase 1 is the biggest (the GX mesh reader and the byte swapping); phases 3 and 4 touch many files but are
  mostly widening arrays.
