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
4. **Unlocks: the Wii's.** Plucky, Fifi and Cyril are revealed in Dallas (its `MonsterRevealDef`, which the port
   already reads by name). The other seven (Boris, Brian, Ruby, Sarah, Squirmy, Flea, VERN) are revealed nowhere in
   the books: the Wii's rule is in code, `MonsterReveal::setupExtraMonsters` (0x800837AC in the Wii's `Rampage.elf`,
   0x534 bytes, symbols kept). Read it first (phase 0). If it cannot be read, they are available from the start.
5. **Controls of the Wii monsters' moves: map the Wii's inputs onto the pad.** Their move graphs number the inputs
   otherwise: 31..57 move up by 10 (George walks on v41, the PS2's `|L|` 31), the buttons follow the Wii remote's
   ids (`rev_digital_button_order`, 0x8030D7C8: A 14, B 15, 1 16, 2 17, ...), and v30..v33 are probably the remote's
   motion (punch down: `v33>2 v31>1.5 v32>1`) [G]. Plan: a table per Wii input, built by lining up the Wii and PS2
   graphs of the same monster (George, Lizzie, Ralph, Boosy; the Wii's 181 states are the PS2's 176 and five new),
   so every Wii condition becomes the PS2 condition it stands for; the motion inputs become the PS2 buttons the same
   transitions use. How far the transitions match one to one is the first thing to check. The 30 PS2 monsters keep
   their PS2 graphs.
6. **The fifth upgrade, SuperJump: later, Wii monsters first.** It needs a move bit (16), a fifth star, a fifth
   `Upgrade|` group per block and an answer for `Monsters::hasSuperJump` (Wii `Monsters.bsf:846`); the PS2
   monsters' graphs have no `Jump_Super` state. Phase 5, optional.
7. **Four players:** the Wii drops `DisableFor4Player` (its monster select has no such test). Keep the port's
   setting (`four_players=`) deciding as now; the Wii monsters have no such flag.

## Phases (one commit per step, each checked like the tools were)

**Phase 0, reading the Wii's code (no port changes).** `MonsterReveal::setupExtraMonsters` (decision 4); the Wii's
pad-to-input code (`RController::getState`, `_calcDirectionFlags`, `getAccelerometerVal`) for decision 5's table;
which shell defaults differ (the new profile's starting monsters). The ELF is PowerPC with symbols; Ghidra reads it.
Output: notes in `docs/`, marked [R] / [G].

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
Check: `rampage.exe Dallas 4` draws the block; `--monster Boris` walks in a PS2 city (with George's controls until
phase 2); `--shot` images of each of the 10 against the viewer's glTF.

**Phase 2, the Wii monsters playing.**
1. The input table of decision 5 applied to Wii graphs at load (`moves_read`).
2. The Wii sound bank as a second bank: its 153 events not in the PS2's, DSP-ADPCM (`sound.c` `sample_get`).
3. Effects and GUI names from the overlay (`fx.c`, `tex_find` in hud.c and shell.c).
4. The 10 monsters' script calls (`BORIS.bsf` ... `FABIO.bsf`, `VERN.bsf`) checked against `answer()`
   (`main.c:1634`) and the clip events; any new call carried into C.
Check: each of the 10 in `--autoplay` through a Las Vegas block: punches, climbs, eats, roars, dies and respawns;
the event log shows no unanswered call.

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

**Phase 5 (optional): SuperJump** (decision 6), and the other platforms (Android, Switch, Vita: where the Wii
folder goes).

## Risks and unknowns
- The input table (decision 5) is the least certain part: if the Wii's transitions do not line up one to one with
  the PS2's, the Wii monsters' controls need their own design.
- The seven unlocks without a book entry (decision 4) depend on reading PowerPC code.
- Dallas's second boss bar and its two-boss finish are new C, not data.
- Size: phase 1 is the biggest (the GX mesh reader and the byte swapping); phases 3 and 4 touch many files but are
  mostly widening arrays.
