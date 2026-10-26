/* profile.h - the player profile and its save: gCommon's game status object (0x4D7990), its three status records
 * (the profile's, the checkpoint, the current game), the saved block, the settings, the cheats, and what updates
 * them (createProfile, loadProfile / restoreProfile, the checkpoint, updateProfile, saveProfile). docs/PROFILE_NOTES.md.
 *
 * [R] What the game has, and what this is the port of:
 *   - the object at 0x4D7990 (gCommon's getProfile / getCheckpoint / getGameStatus answer +0x20 / +0x148 / +0x200):
 *     +0x0C the cities (7), +0x10 / +0x18 the monster roster (30, ShellData's MonsterData), +0x20 the saved block
 *     (0x128 bytes, ProfileBlock), +0x148 the checkpoint and +0x200 the current game (ProfileStatus, 0xB8 bytes each),
 *     +0x299 + 31 p the other players' own monster flags, +0x318 the players, +0x498 the players' count, +0x4B4 the
 *     play type, +0x4BC isCityDestroyed, +0x4D4 the last city, +0x4D8 / +0x4DC the cheat flags and parameter;
 *   - the Common bindings (dispatcher 0x2C2A00, table 0x430450), the Profile bindings (0x2C4640) and the GameStatus
 *     bindings (0x2C48D0, table 0x430B00);
 *   - the new profile 0x33F630, createProfile 0x33E480, restoreProfile 0x33D910, restoreCheckpoint 0x33D820, the
 *     checkpoint 0x33E290, the city's start 0x33DB90, updateProfile 0x33DE10, the status merge 0x33F730, saveProfile
 *     0x33E2D0 and its callback 0x33F230, loadProfile 0x33E3C0 and its callbacks 0x33F0F0 / 0x33F120, the block's end
 *     (0x3255B0: flag 8 and Boosy), the roster loader 0x33E850.
 *
 * Engine-free: the host sets the roster (or keeps the built-in ShellData one), tells the module where the game is
 * (the city's start, each block won, the Continue, the results), applies the settings itself, and saves the block
 * with profile_write / profile_read (the memory card's data file: a 0x50-byte header and the game's 0x128 bytes,
 * little endian). */
#ifndef RAMPAGE_PROFILE_H
#define RAMPAGE_PROFILE_H
#include <stdint.h>

#define PROFILE_VERSION 8                            /* [R] 0x33F630: the block's +0x00 */
#define PROFILE_SIZE 0x128                           /* [R] its +0x04: the bytes saved (0x4D79B0 .. 0x4D7AD8) */
#define PROFILE_PS2_CITIES 7                         /* [R] 0x33E850: CityData City0 .. City6 (at most 7); the file's */
#define PROFILE_CITIES 8                             /* the room: the Wii's CityData has 8 (Dallas the fourth) */
#define PROFILE_PS2_MONSTERS 30                      /* [R] 0x33E850: MonsterData Monster0 .. 29 (at most 30); the
                                                      * records keep 30 flags (0x33D820 copies 15 x 8 bytes), and so
                                                      * does the file */
#define PROFILE_MONSTERS 40                          /* the room: the Wii's ShellData has 40 (docs/WII_PLAN.md); the
                                                      * PS2's roster fills the first 30 */
#define PROFILE_PLAYERS 4

/* ---- the monsters' flags (ProfileStatus.monster_flags, getMonsterFlags) [R] */
enum {
    PM_CHARGE_PUNCH = 1,     /* the upgrades (challenge.h CH_*; Monsters.bsf tests them, MonsterSelect shows them) */
    PM_SPINS = 2,
    PM_ROAR = 4,
    PM_JUMP_SMASH = 8,
    PM_SUPER_JUMP = 0x10,    /* the Wii's fifth upgrade (its Monsters.bsf hasSuperJump): kept in the side file */
    PM_UNLOCKED = 0x80       /* MonsterSelect.isMonsterAvailable: flags & 128 */
};
/* ---- the cities' flags (ProfileStatus.city_flags = GameStatus +0x2AC; ProfileBlock.city_flags[]) [R] */
enum {
    PC_ALL_BLOCKS = 1,       /* the city destroyed (LevelResults' first star) */
    PC_PAR = 2,              /* the city's total reached ParScore */
    PC_CHALLENGES = 4,       /* every block's challenge met (a new city starts with it) */
    PC_REVEAL = 8,           /* every monster of the city's MonsterRevealDef unlocked */
    PC_AVAILABLE = 0x80      /* the city can be picked (Profile.isCityAvailable); a new city's status has it too */
};
/* ---- the cheat flags (gCommon.getCheatFlags, 0x4D7E68; MainMenu.activateCheat sets them) [R the users named] */
#define PCH_MONSTERS 0x1u        /* "141421" (with 2): MonsterSelect / MonsterSheet: every finished monster */
#define PCH_CITIES 0x2u          /* "271828": CitySelect: every city open, its stars shown */
#define PCH_4 0x4u               /* "986960": no monster can be hit (0x2D6A50 answers 0; the reaction bindings
                                  * 0x2ABB18 / 0x2ABB70 do nothing) */
#define PCH_MOVES 0x8u           /* "011235": Monsters.bsf, Eyegore, Furby, Leon: every upgrade move usable */
#define PCH_VIDEO 0x10u          /* "66730n": setCheatFlagsI(16, n), a movie (the parameter, read by the shell
                                  * 0x308390) */
#define PCH_10000 0x10000u       /* "082864": no vibration (setEnableRumble), the status's seeds for the world
                                  * (0x2B6580); the Pause menu's Quit clears it */
#define PCH_20000 0x20000u       /* "874098" (with 0x10000): 0x324930 skips a block */
#define PCH_20000000 0x20000000u /* "000020": dead (0x308340 calls the empty 0x1A61F0) */
#define PCH_40000000 0x40000000u /* "071767": a standing building falls at its first hit (0x2BC1F0) */
#define PCH_BLOCK 0x80000000u    /* "24902n": setCheatFlagsI(0x80000000, n), a start block; but that binding stores
                                  * nothing for it (0x2C3350) and getCheatParam answers -1 (0x2C33B8): CitySelect's
                                  * cheat block is never above 0 */

/* ---- a status record (0xB8 bytes): the profile's (+0x48), the checkpoint (+0x148), the current game (+0x200) */
typedef struct {
    uint32_t seed[2];                                /* +0x00, +0x04: the world's random seeds (gWorld +0x828 and
                                                      * +0x82C, used with cheat 0x10000; the shell draws them at each
                                                      * world's start, 0x308890) */
    uint8_t night;                                   /* +0x08: getNight / setNight (CitySelect) */
    int32_t city;                                    /* +0x0C: getCityIdx / setCityIdx */
    int32_t start_block;                             /* +0x10: getStartBlock / setStartBlock; activateNextBlock
                                                      * (0x2B6600) sets the current block's BlockList index */
    int32_t monster[PROFILE_PLAYERS];                /* +0x14: getMonsterIdx(i) / setMonsterIdx */
    int32_t lives[PROFILE_PLAYERS];                  /* +0x24: getNumLives(i) / setNumLives */
    uint32_t monster_flags[PROFILE_MONSTERS];        /* +0x34: PM_* (getMonsterFlags reads the current one's); the
                                                      * file keeps the first 30 */
    uint32_t city_flags;                             /* +0xAC: getCityFlags / setCityFlags: PC_* of this city */
    int32_t city_score;                              /* +0xB0: getCityScore: the city's total (against ParScore) */
    uint8_t changed;                                 /* +0xB4: set by the merge (0x33F730) when anything changed */
} ProfileStatus;

/* ---- the saved block (0x4D79B0, 0x128 bytes): what goes to the memory card */
typedef struct {
    uint32_t version, size;                          /* +0x00 8, +0x04 0x128 */
    char name[24];                                   /* +0x08: the profile's name (a new profile clears 16 [R];
                                                      * +0x18 .. +0x1F seen neither written nor read [G]) */
    uint64_t save_time;                              /* +0x20: stamped by the SaveGame popup before each card write
                                                      * (0x344434, 0x10D140: about ms since 2000) */
    ProfileStatus status;                            /* +0x28: where the profile is (Profile.getGameStatus) */
    uint32_t city_flags[PROFILE_CITIES];             /* +0xE0: Profile.getCityFlags(i): PC_* saved per city (the
                                                      * file keeps the first 7) */
    int32_t start_block[PROFILE_CITIES];             /* +0xFC: Profile.getStartBlock(i): where the city resumes */
    uint8_t changed;                                 /* +0x118 (0x4D7AC8): to be saved */
    uint8_t victory_shown;                           /* +0x119: getVictoryShown / setVictoryShown */
    float sound, music;                              /* +0x11C (0x4D7ACC), +0x120: 0 .. 0.75 (the sliders' full scale) */
    uint8_t rumble;                                  /* +0x124 (0x4D7AD4): getEnableRumble */
} ProfileBlock;

/* ---- gCommon's game status object (0x4D7990) */
typedef struct {
    int ncities, nmonsters;                          /* +0x0C, +0x18 */
    const char *city_name[PROFILE_CITIES];           /* +0x04: CityData City<i>.City ("LasVegas" ...) */
    const char *monster_name[PROFILE_MONSTERS];      /* +0x10: MonsterData Monster<i>.Name ("George" ...) */
    ProfileBlock block;                              /* +0x20 */
    ProfileStatus checkpoint;                        /* +0x148 */
    ProfileStatus current;                           /* +0x200 */
    uint8_t player_flags[PROFILE_PLAYERS][PROFILE_MONSTERS + 1];   /* +0x299 + 31 p: players 1 .. 3's own flags
                                                      * (0x307EE0, a second profile); the last = in use (the game's
                                                      * [30]) */
    int play_type;                                   /* +0x4B4: gCommon.getPlayType (2 the campaign) */
    int city_destroyed;                              /* +0x4BC: isCityDestroyed (the city's last block won) */
    int last_city;                                   /* +0x4D4: getLastCityIdx */
    uint32_t cheats;                                 /* +0x4D8 (0x4D7E68) */
    int cheat_param;                                 /* +0x4DC (0x4D7E6C) */
    int allow_save;                                  /* the SaveGame popup's flags (0x4D7FF8) & 4: getAllowSave;
                                                      * clear at the boot, set by a load picked from the list
                                                      * (0x34559C) and setAllowSave(1) (Results' Save) */
    int was_saving;                                  /* & 8: set with 4 but by setAllowSave, cleared only by
                                                      * warn-continue (0x343B00) */
    int profile_shown;                               /* & 0x200: a popup closed with a result but 3 */
    int loaded;                                      /* the popup's +0x25C: hasLoadedProfile (a profile loaded or
                                                      * created on a card) */
} Profile;

/* ShellData's rosters as built in (MonsterData Monster0 .. 29, CityData City0 .. 6) */
extern const char *const PROFILE_MONSTER_NAMES[PROFILE_PS2_MONSTERS];
extern const char *const PROFILE_CITY_NAMES[PROFILE_PS2_CITIES];

/* everything 0 (the bss at boot), the built-in rosters, then createProfile's new profile; campaign play type */
void profile_init(Profile *p);
/* 0x33E850's rosters (the names are kept, not copied); n above the room is cut (40 / 8; the game cuts at 30 / 7) */
void profile_set_roster(Profile *p, const char *const *monsters, int nmonsters, const char *const *cities, int ncities);
int profile_find_monster(const Profile *p, const char *name);   /* 0x33DAF0: any case, -1 */
int profile_find_city(const Profile *p, const char *name);      /* the CityData index, any case, -1 */

/* 0x33F630: a new profile's block. version 8, size 0x128; the name cleared; the status: seeds 0xA0A0A0A0 / 0x1111,
 * city 0, start block 0, monsters 0 1 2 3, lives 0, George, Lizzie, Ralph, Ramsey, Gilman and Rhett (0 .. 5) 0x80
 * and no upgrade; the cities: Las Vegas 0x80, the others 0; every start block 0; not shown the victory; sound 0.5,
 * music 0.25, vibration on. The changed byte is not touched. On the Wii's roster profile_create and profile_read
 * also open its 6 and 7, Boris and Ruby, as the Wii's Profile::reset does (0 .. 7) [R Wii 0x8008FB58] */
void profile_block_new(ProfileBlock *b);

/* ---- what the shell calls */
/* createProfile (0x33E480): the new block (name: the profile's, may be NULL), the checkpoint and the current game
 * from its status (no lives set). Returns 1 when it would save (the block or its status changed) */
int profile_create(Profile *p, const char *name);
/* restoreProfile (0x33D910; also the load's success, 0x33F120): checkpoint = the block's status, current = the
 * checkpoint, three lives for each player (changed when not 3) */
void profile_restore(Profile *p);
/* restoreCheckpoint (0x33D820): current = checkpoint, three lives each (MainMenu's campaign start; the Continue) */
void profile_restore_checkpoint(Profile *p);

/* ---- the game */
/* CitySelect's pick and the world's start, in order: current.city / night; the start block (the campaign:
 * profile.start_block[city]; otherwise 0; cheat_block > 0 wins); the shell's checkpoint (0x308710 -> 0x33E290, the
 * whole record); then 0x33DB90 for a start at block 0: the campaign sets the city's flags to 0x84, its score to 0 and
 * three lives (changed when they were not); the King modes and the Timed Run 0 lives. Returns the start block */
int profile_city_start(Profile *p, int city, int night, int cheat_block);
/* 0x33DB90's status part alone (the world's start, and the GameOver's Continue after restoreCheckpoint, 0x3250B0):
 * nothing unless current.start_block is 0; then by play type as above */
void profile_new_city(Profile *p);
/* the checkpoint (0x33E290): the campaign only. Merges the current game into it (0x33F730: keep_head keeps its
 * seeds, night, city and start block unless the current game is further on; the monster flags are ORed), then
 * current.changed = 0 */
void profile_save_checkpoint(Profile *p, int keep_head);
/* the block won (0x3255B0, the profile's part), after the game set its flags: current.city_flags = flags (the
 * game's 1 / 2 / 4 / 8 and 0x80), city_score, start_block (the BlockList index of the next block, or of the last when
 * the city is done), isCityDestroyed = city_done; with flag 8 Boosy's rule (profile_reveal_boosy); the checkpoint.
 * Returns 1 when Boosy was unlocked now */
int profile_block_won(Profile *p, uint32_t city_flags, int city_score, int start_block, int city_done);
/* 0x3259E4: every city's saved flags (the current city's | 8) AND to 1 | 2 | 4 | 8 -> Boosy's current flags |= 0x80.
 * Every city is the roster's: with the Wii's 8 Dallas's flags count too (the Wii runs the same rules, its
 * updateProfile 0x800911A4 [R]; over its 8 cities [G]). Returns 1 when he was unlocked now (the game says nothing
 * either) */
int profile_reveal_boosy(Profile *p);
/* the Wii's five-star unlocks [R Wii RWorldStateMgr::prepareForResults 0x80081970, docs/WII_NOTES.md], at the same
 * block's end (profile_block_won calls it after Boosy's): the monsters of the roster whose current flags have every
 * upgrade are counted; n5StarUnlock (1, 3, 6, 12, 20) walked with its names (Squirmy, Flea, Sarah, Brian, Vern),
 * stopping at the first above the count; the one equal to it gets PM_UNLOCKED. Every upgrade is the Wii's 0x1F;
 * until the port has the fifth (SuperJump, docs/WII_PLAN.md decision 6) the four of the PS2 count (0xF) [G].
 * Without those names in the roster (the PS2's) nothing. Returns the monster unlocked now, -1 none */
int profile_reveal_five_star(Profile *p);
/* the PS2's roster index of a monster's name (any case), -1 when it is one of the Wii's own */
int profile_ps2_monster(const char *name);
/* updateProfile (0x33DE10; Results.bsf 1.5 s into each block's results), the campaign only:
 *   - when the checkpoint changed or the city is destroyed: the checkpoint is merged into the block's status (its
 *     city and start block kept); the city's start block becomes the checkpoint's (0 when the city is destroyed or was
 *     beaten before: PC_ALL_BLOCKS), the city's flag 0x80 with it;
 *   - the city destroyed: the city's saved flags |= the checkpoint's; the next city is the first that is not both
 *     available and destroyed (all done: the one after it, wrapping); it gets 0x80, the status points at it (its
 *     start block); then, with every city available: Philbert unlocks when all have flag 2, Eyegore when all have 4,
 *     Boosy when all have 1 | 2 | 4 | 8 (table 0x3F6770; the status and the checkpoint), all being the roster's
 *     cities (Dallas among them on the Wii's);
 *   - not destroyed: only the city's flag 8 is saved at once;
 *   - in every case current = checkpoint (the lives kept) */
void profile_update(Profile *p);
/* saveProfile's test (0x33E2D0): the block or its status changed. The game then hands the block to the SaveGame
 * popup, which writes only while autosave is on (getAllowSave) [R the popup's +0x178 & 4 test] */
int profile_needs_save(const Profile *p);
/* the save's success (0x33F230): both changed bytes cleared */
void profile_saved(Profile *p);

/* ---- the file: the memory card's data file /BESLES-53800<crc32 of the name, %08X>/<name> as the game writes it
 * (PS2MemoryCardManager 0x1A1AE0): a 0x50-byte header, then the block (0x128 bytes, little endian, floats IEEE).
 * Header [R 0x1A3214 .. 0x1A3238, 0x1A1C38]: +0x00 u32 the data's size 0x128, +0x04 u32 the header's size 0x50,
 * +0x08 u32 the magic 0x0BADFEE5, +0x0C u32 zlib crc32 of the data, +0x10 char[32] "Rampage: TD", +0x30 char[32]
 * the profile's name */
#define PROFILE_FILE_HEADER 0x50
#define PROFILE_FILE_SIZE (PROFILE_FILE_HEADER + PROFILE_SIZE)   /* 0x178 */
#define PROFILE_MAGIC 0x0BADFEE5u
/* saveProfile: writes when profile_needs_save and allow_save (force: always); 1 written, 0 nothing to do, -1 the
 * file could not be written, -2 no room on its drive (ENOSPC). now_ms: the block's +0x20 stamp (0: left as it is) */
int profile_write(Profile *p, const char *path, int force, uint64_t now_ms);
/* the side file's name for a profile file's: "x.bin" -> "x.wii.bin", else + ".wii" */
void profile_side_path(const char *path, char *out, int size);
/* The file holds the PS2's rosters (PROFILE_MONSTER_NAMES, PROFILE_CITY_NAMES): with others set (the Wii's,
 * docs/WII_PLAN.md) each monster's flags, each player's pick, each city's flags and start block and the status's city
 * go by name; a pick the PS2 has not got is saved as that player's start pick (George, Lizzie, Ralph, Ramsey), a city
 * it has not got (Dallas) as the first of its cities not both available and destroyed (its start block, its flags).
 * With the PS2's rosters the file is the block as it is.
 * What the PS2's file has no room for goes to a side file beside it (profile_side_path: rampage_profile.wii.bin),
 * written with the file whenever the roster has monsters the PS2's has not, and read after it: the flags of those
 * monsters and every monster's PM_SUPER_JUMP (ORed in: flags only grow), the cities the PS2's has not got (their
 * flags ORed, their start block), and the players' picks of those monsters and the status's city when it is one of
 * those (taken only while the PS2's file is the one the side file was written with: its data's CRC). Little endian:
 * "RPWI", u32 version 1, u32 that CRC, u32 the count, then 32-byte records: char[16] a name, u32 the kind (1 a
 * monster's flags: a; 2 a pick: a the player; 3 a city: a its flags, b its start block; 4 the status's city: a its
 * start block, b its flags, c its score), u32 a, b, c. A profile used without the Wii folder leaves the side file as
 * it is.
 * loadProfile: reads the file; a missing one (-1) or a bad one (0: shorter than its sizes, not the magic, a CRC that
 * is not 0 and does not match [R 0x1A2D08, 0x1A19B0], a data size not 0x128 [R 0x34462C]; the version is not checked
 * [R 0x3445E0]) gives a
 * new profile (0x33F0F0); read: restoreProfile, both changed bytes cleared (0x33F120 with 1), loaded = 1, 1 */
int profile_read(Profile *p, const char *path);
void profile_pack(const ProfileBlock *b, uint8_t out[PROFILE_SIZE]);
int profile_unpack(ProfileBlock *b, const uint8_t in[PROFILE_SIZE]);   /* 1 (no field is checked) */
/* the whole file (header + block); 0: not valid */
void profile_pack_file(const ProfileBlock *b, uint8_t out[PROFILE_FILE_SIZE]);
int profile_unpack_file(ProfileBlock *b, const uint8_t *in, long size);
uint32_t profile_crc32(uint32_t crc, const uint8_t *data, unsigned long n);   /* 0x106780: zlib's crc32 */
/* the card's directory name for a profile name: "BESLES-53800" + crc32(name) as %08X (0x1A2F90) */
void profile_card_dir(const char *name, char out[24]);

/* ---- gCommon's getters and setters (Common binding ids) */
/* getMonsterFlags (0x19, 0x33D760): monster 0 .. the roster's count (30 on the PS2; else 0); players 1 .. 3 with
 * their own flags in use: theirs; otherwise the current game's, the count itself its city flags */
uint32_t profile_monster_flags(const Profile *p, int player, int monster);
/* setMonsterFlags (0x1A, 0x33D7C0): players 1 .. 3 their own (a byte); otherwise current (flags & 0xFF) */
void profile_set_monster_flags(Profile *p, int player, int monster, uint32_t flags);
int profile_monster_available(const Profile *p, int player, int monster);   /* MonsterSelect: & 0x80, or cheat 1 */
uint32_t profile_moves(const Profile *p, int player, int monster);         /* the upgrades: & 0x1F, all with cheat 8 */
float profile_sound_volume(const Profile *p);        /* 0x1E */
float profile_music_volume(const Profile *p);        /* 0x20 */
/* setSoundVolume (0x1D, 0x2C2F68) / setMusicVolume (0x1F): a new value is stored and raises the changed byte; the
 * game sets the SoundDevice groups (0, 9, 10, 11 / 3) at once: the host does that. Returns 1 if it changed */
int profile_set_sound_volume(Profile *p, float v);
int profile_set_music_volume(Profile *p, float v);
int profile_rumble(const Profile *p);                /* getEnableRumble (0x32) */
/* setEnableRumble(on, save) (0x33, 0x2C3440): save stores it (changed when new); returns whether the pad vibrates:
 * the stored value (save) or on, and never with cheat 0x10000 */
int profile_set_rumble(Profile *p, int on, int save);
uint32_t profile_cheat_flags(const Profile *p);      /* getCheatFlags (0x2D) */
void profile_set_cheat_flags(Profile *p, uint32_t f);            /* setCheatFlags (0x2E): |= */
void profile_set_cheat_flags_i(Profile *p, uint32_t f, int param);   /* setCheatFlagsI (0x2F): 0x10 stores the
                                                                   * parameter; 0x80000000 does nothing; |= f */
void profile_clear_cheat_flags(Profile *p, uint32_t f);          /* clearCheatFlags (0x30): &= ~f */
int profile_cheat_param(const Profile *p, uint32_t f);           /* getCheatParam (0x31): -1 for 0x80000000, else 0 */
/* MainMenu.activateCheat: the six digits; 1 valid ("Cheat_valid"), 0 not. "314159" (the version) is valid and does
 * nothing here */
int profile_cheat_code(Profile *p, const char *code);
int profile_victory_shown(const Profile *p);         /* getVictoryShown (0x39) */
void profile_set_victory_shown(Profile *p);          /* setVictoryShown (0x3A): 1, changed */
/* the Profile object (getProfile): getCityFlags(i), getStartBlock(i), isCityAvailable(i) (flags & 0x80) */
uint32_t profile_city_flags(const Profile *p, int city);
int profile_start_block(const Profile *p, int city);
/* CitySelect's lock: the campaign needs & 0x80 (isCityAvailable); cheat 2 and the other play types open every city */
int profile_city_available(const Profile *p, int city);

#endif
