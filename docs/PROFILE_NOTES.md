# The player profile and its save

This covers what the game keeps about a player and when it saves it:
- the monsters unlocked and their upgrade moves;
- the cities' stars and where each city resumes;
- the checkpoint;
- the settings;
- the cheats.

It also covers the memory card file the game writes. The port is `src/profile.c` / `src/profile.h`. Addresses are in
`RAMPAGE.ELF`. **[R]** means read in the code or the scripts; **[G]** means a guess or a stand-in.

## The object (0x4D7990) [R]
gCommon's state is one object at 0x4D7990. The bindings `getProfile`, `getCheckpoint` and `getGameStatus` answer
three parts of it: +0x20, +0x148 and +0x200.

| Offset | Address | What |
|---|---|---|
| +0x04 / +0x0C | | the cities: CityData `City<n>.City`; at most 7 (0x33EED0) |
| +0x10 / +0x18 | | the monster roster: 12-byte records, the name at +8; MonsterData `Monster<n>.Name` at index n (atoi of the key, 0x33EC30); at most 30 (0x33EDD8) |
| +0x20 | 0x4D79B0 | **the saved block** (0x128 bytes, below) |
| +0x148 | 0x4D7AD8 | **the checkpoint**, a status record |
| +0x200 | 0x4D7B90 | **the current game**, a status record (`getGameStatus`) |
| +0x299 + 31 p | | players 1 .. 3's own monster flags, one byte each; [30] = in use (0x307EE0 fills them from a second profile) |
| +0x318 + 0x60 p | | the players (points, kills, ...: game.c) |
| +0x498 / +0x49C | 0x4D7E28 | players, bots |
| +0x4B4 | 0x4D7E44 | the play type (2 = the campaign) |
| +0x4BC | 0x4D7E4C | `isCityDestroyed` |
| +0x4D4 | 0x4D7E64 | `getLastCityIdx` |
| +0x4D8 / +0x4DC | 0x4D7E68 / 6C | the cheat flags, the cheat parameter |

The SaveGame popup is at 0x4D7E80. Its flags at +0x178 (0x4D7FF8) hold:
- 2: `getSaveFilesExist`;
- 4: `getAllowSave`, the autosave;
- 0x4000: set when a save starts in shell state 5.

Its +0x25C (0x4D80DC) is `hasLoadedProfile`, the name of the last profile saved or loaded.

### A status record (0xB8 bytes) [R 0x33D820, 0x33F730, the GameStatus bindings 0x2C48D0]
| Offset | What | Binding |
|---|---|---|
| +0x00, +0x04 | the world's random seeds; the shell draws them at each world's start (0x308890; not for play type 0: the attract mode's demo draws its own, 0x308460, SHELL_NOTES.md), and the world uses them with cheat 0x10000 (0x2B6580) | |
| +0x08 | byte: night | getNight / setNight |
| +0x0C | the city | getCityIdx / setCityIdx / getCityName |
| +0x10 | the start block: activateNextBlock stores the current BlockList index (0x2B6600) | getStartBlock / setStartBlock |
| +0x14 | the players' monsters [4] | getMonsterIdx / setMonsterIdx / getMonsterName |
| +0x24 | the lives [4] | getNumLives / setNumLives |
| +0x34 | the monster flags [30] | gCommon.getMonsterFlags |
| +0xAC | the city's flags | getCityFlags / setCityFlags |
| +0xB0 | the city's total (against ParScore) | getCityScore |
| +0xB4 | byte: changed | |

The record names one city. The profile's copy says where the campaign stands; the current one is the game being
played.

### The saved block (0x4D79B0, 0x128 bytes) [R 0x33F630, the Profile bindings 0x2C4640]
| Offset | What |
|---|---|
| +0x00 | u32 version 8 |
| +0x04 | u32 size 0x128 (the popup copies this many bytes) |
| +0x08 | the name, 16 bytes (a new profile clears 16; the popup writes it with strncpy 16, 0x344114 / 0x343DA8; the keyboard takes 15, 0x3408D0); +0x18 … +0x1F is padding **[R]** |
| +0x20 | u64 a time stamp, set by the popup before each card operation (0x344434; 0x10D140, about ms since 2000) |
| +0x28 | the profile's status record (`Profile.getGameStatus`) |
| +0xE0 | u32 the cities' saved flags [7] (`Profile.getCityFlags(i)`; `isCityAvailable(i)` = & 0x80) |
| +0xFC | the cities' start blocks [7] (`Profile.getStartBlock(i)`) |
| +0x118 | byte: changed (0x4D7AC8); every setter raises it |
| +0x119 | byte: `getVictoryShown` (`setVictoryShown` sets it and changed: Victory.scr, as its first movie starts; no script and no engine code reads it, so only the saved byte and the changed mark show it) |
| +0x11C | float: the sound volume (0x4D7ACC) |
| +0x120 | float: the music volume |
| +0x124 | byte: the vibration (0x4D7AD4) |

### The flags [R]
**The monster flags**:
- 1 Charge Punch, 2 more spins, 4 Roar, 8 Jump Smash: the upgrades; Monsters.bsf and the others test them through
  `getMonsterFlags`;
- 0x80: the monster can be picked (MonsterSelect: `flags & 128`).

`getMonsterFlags(player, m)` (0x33D760) works so:
- it accepts m from 0 to 30;
- players 1 .. 3 get their own byte when theirs is in use;
- everyone else gets the current record's flags.

m = 30 reads past the 30 flags into the city's flags. `setMonsterFlags` (0x33D7C0) stores `flags & 0xFF` the same way.

**The city flags**:
- 1 destroyed, 2 par, 4 challenges, 8 reveal: LevelResults' four stars;
- 0x80: the city is available.

A new city's current flags are 0x84 (0x33DB90).

## A new profile (0x33F630) [R]
- version 8, size 0x128; the name cleared;
- the seeds 0xA0A0A0A0 / 0x1111; city 0, start block 0; monsters 0, 1, 2, 3; lives 0;
- **monsters 0 .. 5 have 0x80 and nothing else**: George, Lizzie, Ralph, Ramsey, Gilman, Rhett (ShellData's order).
  So **the four moves start locked**; only the upgrade challenges teach them. The viewer's `abilities = 0xF` and
  "every move" are cheat 8's behaviour, not a new profile's;
- the cities: Las Vegas 0x80, the others 0; every start block 0;
- the victory not shown;
- **sound 0.5, music 0.25, vibration on**. On the Pause and Options sliders (100 × v / 0.75) that is 66 and 33.

The changed byte is left as it was. `createProfile` (0x33E480) runs this, copies the profile's record into the
checkpoint and that into the current game, and saves if anything is marked changed. The name comes from the shell's
name screen; the popup keeps its own copy at +0x25C, which replaces the descriptor's name (0x345B24).

## How the records move [R]
- **The merge 0x33F730(dst, src, keep_head).** It raises dst's changed byte when anything differs:
  - without keep_head: the seeds, night, city and start block are copied;
  - the monsters and lives are copied;
  - the monster flags are **ORed**: they only grow;
  - the city's flags and total are copied;
  - with keep_head, a later city, or a later block of the same city, still moves dst on.
- **`restoreProfile`** (0x33D910; also the load's success): checkpoint = the profile's record, current = the
  checkpoint, three lives each.
- **`restoreCheckpoint`** (0x33D820): current = checkpoint, three lives each. MainMenu calls it when the campaign is
  picked (`processNumPlayerSelect`); the GameOver's Continue (0x3250B0) does the same.
- **The checkpoint** (0x33E290, campaign only): merge(checkpoint, current, keep_head), then current.changed = 0. It
  runs:
  - when the shell starts the world (0x3088F0, keep_head 0), after drawing the seeds;
  - at each block won (0x325AC8), with keep_head = the city is **not** done (`xori 1` in the call's delay slot,
    0x325ACC): within a city the head moves only forwards; at the city's end it is copied.
- **A city's start** (0x33DB90, from the world's start 0x2B7D40 and the Continue). It does nothing unless the start
  block is 0. Then:
  - the campaign: flags 0x84 and total 0 (changed when they were not), three lives;
  - King of the City / World and the Timed Run: 0 lives.

  A city resumed at a later block therefore keeps the checkpointed flags: a challenge missed earlier stays missed.
- **CitySelect** sets the city, the night and the start block. The start block is:
  - the campaign: `Profile.getStartBlock(city)`;
  - other play types: 0;
  - the cheat block when above 0, which it never is (below).

## updateProfile (0x33DE10) [R]
**When.** Results.bsf calls `updateProfile(); saveProfile()` 1.5 s into each block's Results screen. The other
results screens do not call it. It does nothing outside the campaign.

**What it does.** When the checkpoint changed or the city is destroyed:
1. The checkpoint is merged into the profile's record (keep_head). The checkpoint's changed byte is cleared; the
   profile is marked changed if its record changed.
2. **The city's start block** becomes:
   - 0 if the city is destroyed now or was beaten before (saved flag 1);
   - otherwise the checkpoint's: the block after the last one won.

   When it differs, the city also gets 0x80 and the profile is marked changed.
3. **The city destroyed**:
   - its saved flags |= the checkpoint's: the stars are saved only at the city's end;
   - `getLastCityIdx` = the city;
   - the next city is the first that is not both 0x80 and 1. If every city is done, it is the one after this city,
     wrapping. It gets 0x80, and the profile's record and the checkpoint point at it, with its saved start block.
4. **Not destroyed**: only flag 8 (the reveal) is saved at once.
5. **Unlocks** (city destroyed; table 0x3F6770). The saved flags of all cities are ANDed. If the result has 0x80
   (every city available):
   - **Philbert** unlocks when all have 2 (par);
   - **Eyegore** when all have 4 (challenges);
   - **Boosy** when all have 1 | 2 | 4 | 8.

   Each gets 0x80 in the profile's record and in the checkpoint.

Always, at the end: current = checkpoint, with the lives as they are.

**Boosy at the block's end** (0x3255B0, 0x3259E4). Separately from updateProfile, when 0x323240 says every monster
of the city's MonsterRevealDef is unlocked:
- the current flags |= 8;
- the saved flags of every city are ANDed, the current city counting its saved flags | 8;
- a result of 0xF gives Boosy (0x434098) 0x80 in the current record.

`profile_reveal_boosy` is this rule. It replaces `reveal_boosy` (reveal.c), which takes the city flags from the
host: same logic, but the profile now holds those flags.

## Saving [R]
- **`saveProfile`** (0x33E2D0) does nothing unless the profile's changed byte or its record's changed byte is set.
  Then it builds a 0xC8-byte descriptor:
  - +0x00 the name;
  - +0x40 "Rampage: TD";
  - +0x80 the name;
  - +0xC0 the magic 0x0BADFEE5;
  - +0xC4 −1 (the default card).

  It calls the popup 0x345950 with the block and the callback 0x33F230, which clears both changed bytes on success.
  The popup writes only with its flag 4 (autosave) set; otherwise 0x345540(3): no message, the popup closed with
  result 3 (no show-profile change), the profile not written and its changed bytes kept **[R 0x345950]**. With flag
  4 it runs state 7: the SaveIcon alone, then the write (SHELL_NOTES.md "The memory card's states"). The port: the
  viewer's `save_profile` (Results, HUD_CALL_SAVE_PROFILE) and the shell's Options (`op_save`, a new value marks
  the profile changed) test the changed bytes, then the popup asks for the write (`SHELL_EVT_PROFILE_SAVE`) and the
  viewer writes with `profile_write(.., force 1)`.
- **Where it is called**:
  - Results.bsf, every block won: `updateProfile` and `saveProfile` 1.5 s into the screen;
  - Results.bsf's **save button**: it shows when a profile is loaded and autosave is off. Pressing it calls
    `setAllowSave(1)`, which turns autosave on; the button hides, and its hiding calls `saveProfile`;
  - the main menu's Options page `saveSettings`: `setSoundVolume`, `setMusicVolume`, `setEnableRumble(b, 1)`,
    `saveProfile`.
- **The Pause menu's Options** set the same three values without saving. They are written by the next block's
  Results.
- **`loadProfile`** (0x33E3C0): the popup 0x345700, with 0x33F0F0 as the "new profile" hook (0x33F630 on the
  block). On success it calls 0x33F120:
  - 1 or 2: `restoreProfile`;
  - 1: both changed bytes cleared;
  - then the settings are applied: sound volume groups 1, 9, 10, 11, music group 3 over 500 ms, and the vibration.
- **MainMenu's start** (`getShowProfile`): `loadProfile` with autosave on, `restoreProfile` otherwise. Back on the
  main menu also calls `loadProfile`.

### The settings' setters [R]
- `setSoundVolume` (0x2C2F68): a new value is stored, raises changed, and sets the SoundDevice groups 0, 9, 10 and 11.
- `setMusicVolume`: the same with group 3.
- `setEnableRumble(on, save)` (0x2C3440):
  - with save, a new value is stored and raises changed;
  - the pad vibrates when the value (or on, without save) is set and cheat 0x10000 is not.

## The memory card file [R, from PS2MemoryCardManager, ps2_MemoryCard.cpp, vtable 0x444EE0]
**The directory.** `"BESLES-53800"` + `%08X` of zlib's crc32 of the profile name (0x106780, table 0x40A190):
- the region prefix comes from the product code: "BE" for SLES / SCES (0x1A4200);
- the product code comes from SYSTEM.CNF's BOOT2 line (0x1A75A0).

For "AA" the directory is `BESLES-53800A9601DBD`.

**The files**:
| File | What |
|---|---|
| `<dir>/<dir>` | an empty marker (created and closed, 0x1A30AC) |
| `<dir>/<name>` | the data |
| `<dir>/icon.sys` | 0x3C4 bytes (0x1A38D0), below |
| `<dir>/Rampage.icn` | the disc's RAMPAGE.ICN (136880 bytes) copied as it is (0x1A3C00; the name set at 0x2A3418) |

icon.sys and Rampage.icn are written only when icon.sys is not there yet.

**icon.sys**:
| Offset | What |
|---|---|
| +0x00 | "PS2D" |
| +0x06 | 22: the first line's SJIS length |
| +0x0C | 0x60 |
| +0x10 | the corner colours (128,0,0) (0,128,0) (0,0,128) (128,128,128) |
| +0x50 | the light directions (0.5,0.5,0.5) (0,−0.4,−0.1) (−0.5,−0.5,0.5) |
| +0x80 | the light colours (0.67,0.67,0.04) (0.70,0.46,0.28) (0.20,0.20,0.54) |
| +0xB0 | the ambient 0.6 |
| +0xC0 | the title in full-width SJIS: "Rampage: TD", then the profile name on the second line |
| +0x104, +0x144, +0x184 | "Rampage.icn" |

**The data file** (0x178 bytes):
| Offset | What |
|---|---|
| +0x00 | u32 the data's size, 0x128 |
| +0x04 | u32 the header's size, 0x50 |
| +0x08 | u32 0x0BADFEE5 |
| +0x0C | u32 crc32 of the data |
| +0x10 | char[32] "Rampage: TD" |
| +0x30 | char[32] the name (the bytes after each NUL are heap leftovers) |
| +0x50 | the 0x128-byte block, as in memory |

**The load's checks**:
- the file is at least the header's sizes long (0x1A2D08);
- the magic matches;
- the CRC matches, unless it is 0 (0x1A19B0);
- the data's size is the 0x128 the caller asks for (0x34462C).

The version word 8 is not checked (0x3445E0 .. 0x3446B4 test the CRC and the data size alone) [R].

## The cheats (MainMenu.activateCheat) [R the codes; what they do where the users were read]
| Code | Flags | What |
|---|---|---|
| 141421 | 1, 2 | every finished monster (MonsterSelect / MonsterSheet) and every city |
| 271828 | 2 | every city open and its stars shown (CitySelect) |
| 011235 | 8 | every upgrade move (Monsters.bsf, Eyegore, Furby, Leon) |
| 986960 | 4 | no monster can be hit (0x2D6A50 answers 0; below) |
| 082864 | 0x10000 | the soak test (SHELL_NOTES.md): the screens pressed through, every player a bot; no vibration; the world takes the status's seeds; the Pause menu's Quit clears it |
| 874098 | 0x10000, 0x20000 | 0x324930 skips a block |
| 071767 | 0x40000000 | one-hit buildings (Building::hit 0x2BC1F0; below) |
| 000020 | 0x20000000 | nothing: its user 0x1A61F0 is empty (below) |
| 66730n | 0x10 | movie n (the parameter; the shell reads it at 0x308390) |
| 24902n | | `setCheatFlagsI(0x80000000, n)` stores nothing (0x2C3350), and `getCheatParam(0x80000000)` answers −1 (0x2C33B8), so CitySelect's cheat block is always 0: dead in this build |
| 000000 | | clears every flag |
| 314159 | | shows the version |

The cheats are not in the saved block.

## The port (src/profile.c)
- **Setup**:
  - `profile_init`: zeros, the built-in ShellData rosters, then a new profile; the campaign; autosave off. The
    game's flag 4 [R] starts clear (.bss, no constructor store found; the popup's init 0x345C90 and unload 0x345D00
    clear only flag 1) and is set by setAllowSave(1) (0x2C3568; only Results.scr:358's Save button), a close(1) of loadProfile (flag 0x10)
    in mode 3, 6 or 7 (0x34559C: a load picked from the list, or a save; a create closes in mode 5 and does not),
    the card watcher in mode 1 with the status now 0 and a name held (0x3454E0), and saveProfile when the card id
    changed and a name is held (0x3459D8, its entry 0x345950); cleared by setAllowSave(0) and warn-continue's Don't
    Save / Continue (0x343B00). It gates getAllowSave (MainMenu.scr:209 loadProfile vs restoreProfile, Results'
    Save button and its 3 s), saveProfile (clear: close(3), nothing written) and warn-continue (only with 4 set and
    0x20 clear). So the boot's card check (close(1) in mode 12) leaves it clear and MainMenu restores the profile
    without opening the list; the list waits for Back ("Profiles"). Flag 8 is set with 4 (0xC) at the same three
    places but not by setAllowSave, and cleared only by warn-continue; flag 0x20 is flag 8 and a name held when
    loadProfile opens (0x345760). The port keeps 4 as `allow_save`, 8 as `was_saving` and 0x200 as
    `profile_shown`: a load from the list sets 4 and 8, a create neither, Results' Save 4, warn-continue clears
    both. A viewer started straight in a game with a profile file sets 4 and 8 (as a load). The card watcher's and
    the card-id case need a card that comes and goes and are not ported;
  - `profile_set_roster`: the rosters from the books.
- **The shell**: `profile_create`, `profile_restore`, `profile_restore_checkpoint`.
- **The game**:
  - `profile_city_start(p, city, night, cheat_block)`: CitySelect, the checkpoint at the world's start, 0x33DB90;
  - `profile_new_city`: 0x33DB90 alone (the Continue);
  - `profile_block_won(p, flags, score, start_block, city_done)`: 0x3255B0's part, with Boosy;
  - `profile_save_checkpoint`, `profile_update`, `profile_needs_save`.
- **The file**:
  - `profile_write(p, path, force, now_ms)` and `profile_read(p, path)` write and read the card's data file (header
    and block), e.g. `rampage_profile.bin` next to the viewer. The write answers -1 when the file cannot be
    written and -2 for a full drive (ENOSPC), the read -1 when there is no file and 0 for one that is not a
    profile: the popup's statuses 7, 11, 6 and 18 (main.c `card_job`; SHELL_NOTES.md);
  - `profile_pack` / `profile_unpack` handle the bare block, `profile_pack_file` / `profile_unpack_file` the whole
    file;
  - `profile_crc32`, `profile_card_dir`.
- **gCommon's getters and setters**:
  - `profile_monster_flags` / `profile_set_monster_flags`, `profile_monster_available`, and `profile_moves` (the
    upgrades, all four with cheat 8);
  - the volumes, `profile_set_rumble`;
  - the cheats (`profile_cheat_code` takes the six digits);
  - the victory shown;
  - `profile_city_flags`, `profile_start_block`, and `profile_city_available` (CitySelect's lock).

## Not traced, or guessed
- The load checks the CRC and the file's data size, not the block's version or size (0x3445E0 … 0x3446B4) [R]; the
  port the same.
- The time stamp (0x10D140) [R]: (u64) difftime(now, epoch) × 1000, the epoch mktime of 1 Jan 2000 (tm_year 2000
  itself), now sceCdReadClock's raw BCD bytes in a tm (the month without − 1, the year + 2000). The port stores what
  the host passes.
- The header's leftover bytes are heap leftovers in the game [no host meaning]; the viewer's file leaves them zero.
- Cheats [R]: 4 (986960) no monster can be hit (0x2D6A50 answers 0; the reaction bindings 0x2ABB18 / 0x2ABB70 do
  nothing); 0x20000000 (000020) dead (0x1A61F0 is empty); 0x40000000 (071767) one-hit buildings (Building::hit
  0x2BC1F0: a standing building collapses at its first hit). 4 and 0x40000000 are ported (main.c
  `monster_hit`, building.c `building_set_one_hit`).
- Players 2 .. 4's own profiles [R]: gShell.loadBuddyProfile (0x3077B0) loads one into a block of its own (0x4D5218,
  no autosave) and 0x307EE0 copies its 30 monster flags (& 0xFF) into the player's bytes, [30] = 1. Kept as
  `player_flags`; the viewer has one profile file, so nothing fills them.
