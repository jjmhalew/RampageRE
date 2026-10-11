/* game.h - the game flow of a city: the world's state machine (blocks begin, are played, won, lost), the monsters'
 * lives and respawns, the block clock and its time bonus, the score kept per block and per city, the results, and
 * the play modes' rules (campaign, King of the City / World, Timed Run). docs/GAME_NOTES.md has the details; game.c
 * cites the game code ([R] read in the code or the files, [G] a guess or a stand-in).
 *
 * [R] What the game has, and what this is the port of:
 *   - the world state, the word at 0x4D6A80 (gWorld.getState, World binding 58): set by 0x325BF0, run every frame by
 *     0x324340 (entering a state: table 0x434040; the state's end: table 0x434020 for 6 .. 11, otherwise + 1), the
 *     state object's +0x8 / +0xC / +0x10 the running Levels.bsf "NI" function, +0x14 IsBlockFinished, +0x18 the
 *     city is done, +0x19 game over;
 *   - the block list (the manager 0x4D4E80: WorldData's BlockData.BlockList, BlockStart, BlockEnd, read by 0x303240),
 *     activateNextBlock (0x2B6580);
 *   - the block clock (0x4D5190: 0x347ED0 from the block's TimeBonus, 0x347EA0 start, 0x347E10 stop and the time
 *     bonus, 0x347DC0 the time left; isGameClockVisible 0x2AF880);
 *   - the game status (0x4D7990: lives +0x224, the start block +0x210, the checkpoint copy +0x148, the players at
 *     +0x318 + 0x60 each: points[7] +0x4, kills[7] +0x20, cities won +0x3C, blocks won +0x40, city score +0x44), its
 *     reset 0x33DB90, the checkpoint 0x33E290 / 0x33D820, Player.addPoints 0x33F530;
 *   - the Monster bindings becomeHuman (0x2D3ED0), canRespawn (0x2D3D80), startRespawn (0x2D3930), cancelRespawn
 *     (0x2D36A0) and the block-end reset 0x2DCB40;
 *   - the scripts Levels.bsf (BeginLevelNI, ContinueLevelNI, BeginBlockNI, IsBlockFinished, ResultsNI,
 *     SkipResultsNI, EndBlockNI, EndLevelNI, EndWorldNI), Monsters.bsf (becomeHuman, canRespawn, startRespawn),
 *     WorldGuiDriver.bsf (startResults, setResultState, endResults), GameOver.bsf, Results.bsf, Hud.bsf, Clock.bsf and
 *     the cities' isBossBlockFinished / setBossBlockFinished.
 *
 * Engine-agnostic: nothing here draws, loads or plays. The caller feeds each frame what it knows (the buildings
 * standing, each monster's HP), answers the monster graph's script calls through the respawn functions, forwards
 * the score, and gets back events (which block to build, where to put the monsters, which moves they play, which
 * results screen to show, game over, leaving the city). Positions are the game's: z up, the street at z = 0, the
 * buildings' fronts towards +y, the camera at -y; facings in radians about z as the game keeps them (0 faces +y,
 * pi faces -y, towards the camera). */
#ifndef RAMPAGE_GAME_H
#define RAMPAGE_GAME_H
#include <stdint.h>
#include "data.h"

#define GAME_PLAYERS 4

/* gWorld.getState() [R 0x4D6A80; who sets it: 0x2B7D40 1 then 4, 0x2B8040 2, 0x2B7F50 3, restartShell 12,
 * 0x2B7EA0 13, 0x2B7FF0 14, 0x2B7CB0 15, the rest the update 0x324340] */
enum {
    GS_NONE = 0,
    GS_STARTING = 1,        /* the world starts: SinglePlayer.bsf loaded (0x2B7D40), then 4 */
    GS_2 = 2,               /* 0x2B8040 [G: not traced further] */
    GS_LOADING = 3,         /* the city is loading (0x2B7F50); waits for the start */
    GS_BEGIN_LEVEL = 4,     /* Levels::BeginLevelNI (the city's first block) or ContinueLevelNI: the intro */
    GS_BEGIN_BLOCK = 5,     /* Levels::BeginBlockNI: the HUD shows, the army is let loose */
    GS_PLAY = 6,            /* a block is played: IsBlockFinished, the clock, the lives */
    GS_BLOCK_WON = 7,       /* the block's results (Results / KingBlockResults / TimedRunResults, ResultsNI) */
    GS_GAME_OVER = 8,       /* the GameOver screen */
    GS_END_BLOCK = 9,       /* Levels::EndBlockNI: the monsters walk on to the next block */
    GS_END_LEVEL = 10,      /* the city is destroyed: LevelResults / KingCityResults, EndLevelNI */
    GS_END_WORLD = 11,      /* the last city: WorldResults / KingWorldResults, EndWorldNI */
    GS_EXIT = 12,           /* back to the shell (restartShell, the Pause menu's quit) */
    GS_13 = 13, GS_14 = 14, /* 0x2B7EA0, 0x2B7FF0: the world shutting down [G] */
    GS_SHUTDOWN = 15        /* 0x2B7CB0: WorldGuiDriver::endResults */
};

/* gCommon.getPlayType() [R MainMenu.scr: playModeTitles 2 .. 5] */
enum { GP_ATTRACT = 0, GP_CAMPAIGN = 2, GP_KING_CITY = 3, GP_KING_WORLD = 4, GP_TIMED_RUN = 5 };

/* WorldGuiDriver::startResults(n): the results screen [R WorldGuiDriver.scr] */
enum {
    GR_SCREEN_GAME_OVER = 0, GR_SCREEN_RESULTS = 1, GR_SCREEN_LEVEL = 2, GR_SCREEN_WORLD = 3,
    GR_SCREEN_KING_BLOCK = 4, GR_SCREEN_KING_CITY = 5, GR_SCREEN_KING_WORLD = 6, GR_SCREEN_TIMED_RUN = 7
};
/* WorldGuiDriver::setResultState(n), what a results screen answers [R 0x324BF0 and the screens' scripts] */
enum {
    GR_NONE = 0,            /* still showing */
    GR_DONE = 1,            /* Continue (Results, LevelResults, WorldResults ...: after the walkout) */
    GR_CONTINUE = 2,        /* GameOver's first choice: play on from the checkpoint */
    GR_SELECT = 3,          /* GameOver's second choice, Results' "select": leave the city (0x4D7E4E) */
    GR_QUIT = 4             /* quit, confirmed (0x4D7E4D) */
};

/* the player's point categories (Player.addPoints, 0x33F530) [R Results.scr's list, army.h ArmyStats] */
enum { GPT_DESTRUCTION = 0, GPT_CIVILIAN = 1, GPT_VEHICLE = 2, GPT_BONUS = 3, GPT_TIME_BONUS = 4, GPT_CHALLENGE = 5,
       GPT_TOTAL = 6 };

/* ---- what the module asks of its host */
typedef struct {
    /* a property group by book and name: book "<City>_Props", name "WorldData" (and "DefBlockProps", a block's
     * ParentPropertySet). Group.at = 0 when absent */
    Group (*find)(void *ctx, const char *book, const char *name);
    /* optional: a script function the port does not have (a block's IsBlockFinished / NI override by name, e.g.
     * "NewYork::BossResultsNI"): NI functions return their next call's delay (0: done), IsBlockFinished non-zero
     * when finished. Return -1 when unknown: the Levels.bsf version is used instead [G] */
    float (*script)(void *ctx, const char *function, float last_freq);
    /* 0x323240 at the block's end: every monster of the city's MonsterRevealDef unlocked (city flag 8, "All Monsters
     * Revealed"); NULL: never */
    int (*all_revealed)(void *ctx);
    void *ctx;
} GameHost;

typedef struct {
    int play_type;                                   /* GP_* (default campaign) */
    int num_players, num_bots;                       /* 1 .. 4 (default 1); the last num_bots of them are AI (the
                                                      * demo, GP_ATTRACT: all of them, three lives each) */
    int city_index;                                  /* ShellData CityData.City<n>: 0 LasVegas .. 6 NewYork */
    int num_cities;                                  /* 7 (CityData City0 .. City6) */
    int start_block;                                 /* GameStatus +0x210 (0: the city's BlockStart) */
    uint32_t seed;                                   /* gWorld +0x828: GameStatus seed[0] (RWorld reset 0x2B74D0) */
    uint32_t cheats;                                 /* gCommon.getCheatFlags (0x4D7E68): 0x10000 | 0x20000 skip a
                                                      * block (0x324930); 0x10000 reseeds the world each block */
    uint32_t seed1;                                  /* gWorld +0x82C's seed: GameStatus seed[1] */
} GameConfig;

/* ---- what the module is told each frame */
typedef struct {
    float hp[GAME_PLAYERS];                          /* each monster's hit points (+0xA4C): 0 or less = defeated */
    int buildings;                                   /* gWorld.getCurrentBuildingCount(): standing, not falling */
    int centre_down;                                 /* New York: the building at (0, 10, 1) is falling or destroyed */
} GameInput;

/* ---- what happened (events) */
enum {
    GE_STATE,          /* the world entered state a (GS_*) */
    GE_BLOCK,          /* activateNextBlock: block a of the BlockList (name its BlockDef name; b 1 in the game, 0 the
                        * world's reset) becomes the current
                        * one; build it (BlockDef|<name>), load its AIDef (army_load_block) but do not activate the AI
                        * yet (GE_AI_ACTIVATE). The block just left lies 41 towards -y now (0x302AF0) */
    GE_RESET,          /* monster slot: resetMonsterState + clearAllGoals; a = 1: also put it at pos facing f */
    GE_PLACE,          /* monster slot: setPosition pos (a = 1: pos is added to where it is); no reset, its facing kept */
    GE_AI,             /* monster slot: setIsAI(a); b = 1: and destroyBuildings (a bot in BeginBlockNI) */
    GE_MOVE,           /* monster slot plays the move `name` (playMove); a = 1: tell game_move_done(slot) when it ends
                        * (Levels::moveComplete), the NI waits for it */
    GE_WALK,           /* monster slot: walkTo(pos, radius f), then addWayPoint(v, f) (f: the arrival radius,
                        * 0x2F20E0); its next GE_MOVEs follow the walk */
    GE_CAMERA,         /* gCameraMgr.setCamMode(name, blend f); name "Script": the script camera at pos looking at v,
                        * a = 1 cut */
    GE_HUD,            /* the players' HUDs (and ChallengeStatus) fade to a (1 shown, 0 hidden) */
    GE_GUI,            /* screen.loadInterface(name) ("ChallengeText") */
    GE_AI_ACTIVATE,    /* gAIMgr.activate(1): the block's attackers start (army_activate) */
    GE_RUBBLE_FX,      /* ResultsNI: effect `name` at a point found in the rubble within f of pos (findPointInRubble),
                        * 1.5 above it; a = the index (0 .. 2). The game uses Fire_Loop instead of Rubble_Smolder when the
                        * point found has y <= -23 */
    GE_RUBBLE_FX_STOP, /* BeginBlockNI: the three effects stop */
    GE_RESULTS,        /* WorldGuiDriver::startResults(a): show results screen a (GR_SCREEN_*), answer with
                        * game_set_result; GR_SCREEN_GAME_OVER is the game over */
    GE_REVIVE,         /* monster slot comes back where it is (cancelRespawn, 0x2D36A0): full HP (MONSTER_DATA +0x138),
                        * scale 1, its streaker deleted; a = 1: also energy 0 and the rage off (the continue, 0x2E5730 [G]) */
    GE_TIME_UP,        /* the clock ran out (state 6): a = 1 game over, 0 the block simply ends */
    GE_EXIT            /* the world is left for the shell: a = 0 the menus, 1 the next city (King of the World: city
                        * b), 2 the attract mode's demo is over (b = 1: a pad's button ended it, gShell's idle timer
                        * is 120 again, 0x324CC8; 0: its minute is up or the block finished) */
};
typedef struct { int type, slot, a, b; const char *name; float pos[3], v[3], f; } GameEvent;

typedef struct Game Game;
Game *game_new(const GameHost *host, const GameConfig *cfg);   /* cfg may be NULL: one player, campaign, Las Vegas */
void game_free(Game *g);

/* the city: its WorldData (<city>_Props), "LasVegas" ...; the state becomes GS_LOADING. 0 = ok */
int game_load_city(Game *g, const char *city);
/* the world starts (0x2B7D40): the status reset (lives 3 in the campaign, the points), the first block (max of
 * BlockStart and the start block) activated (GE_BLOCK), then GS_BEGIN_LEVEL */
void game_start(Game *g);
void game_update(Game *g, float dt, const GameInput *in);
int game_events(Game *g, GameEvent *out, int max);  /* takes the events since the last call */

/* ---- the host's answers */
void game_move_done(Game *g, int slot);              /* a GE_MOVE with a = 1 ended (Levels::moveComplete) */
void game_set_result(Game *g, int result);           /* WorldGuiDriver::setResultState(GR_*) */
/* stand-in for the screens: answer GR_DONE by itself this many seconds after a results screen opens (the GameOver
 * screen gets GR_CONTINUE); 0 = never (default) */
void game_auto_results(Game *g, float seconds);
void game_restart_shell(Game *g);                    /* gWorld.restartShell (the Pause menu's quit): GS_EXIT */
/* a pad's button went down (RControllerMgr's input 0x2C1B80: any control from 4 on, the d-pad and every button, not
 * the sticks, on any pad, sets gWorld +0x1B): in the attract mode's demo the world is left at the next update
 * (0x324C80), GE_EXIT a = 2, b = 1 */
void game_attract_button(Game *g);
/* Player.addPoints(cat, amount) (0x33F530): forward the army's AE_SCORE (monster, level = cat, amount) here */
void game_add_points(Game *g, int slot, int cat, int amount);
/* <City>::setBossBlockFinished, the boss AIInstance's CompletionCallback; centre_down: New York's centre building
 * (0, 10, 1) is falling or destroyed */
void game_set_boss_finished(Game *g, int centre_down);
/* gWorld +0x828, the state frand and findPointInRubble share (0x106BD0), and a draw of frand from it */
uint32_t *game_rng(Game *g);
float game_frand(Game *g);

/* ---- the monster graph's script calls (Monsters.bsf) */
/* Monsters::becomeHuman: 1 = make the streaker (createStreaker) and hide the monster; only while a block is played
 * (state 6), otherwise nothing happens and the monster lies there */
int game_become_human(Game *g, int slot);
/* Monsters::canRespawn (0x2D3D80): it became human, its streaker is not being held (held: object flag 0x20000), and
 * in the campaign the players have a life left between them */
int game_can_respawn(const Game *g, int slot, int streaker_held);
typedef struct {
    int streaker;                                    /* the streaker still exists (not deleted: it ran out of view) */
    int on_line;                                     /* it still moves on its LineMover (not grabbed, not flying) */
    float streaker_pos[3], streaker_facing;          /* its place and facing */
    float cam_pos[3], cam_dir[3];                    /* the camera: its place and the way it looks */
    float cam_half_fov;                              /* half its vertical field of view, radians (0: unknown) */
} GameRespawnIn;
typedef struct {
    float pos[3], facing;                            /* where the monster comes back */
    int fall;                                        /* 1: in the air (mode 1), playing Jump_Butt_Slam_fall; 0: where the
                                                      * streaker was, as the clip goes on (Faceup_Idle_Respawn grows it) */
    int delete_streaker;                             /* delete the streaker actor */
} GameRespawn;
/* Monsters::startRespawn (0x2D3930): a life is taken (campaign), HP back to the most (MONSTER_DATA +0x138) */
void game_start_respawn(Game *g, int slot, const GameRespawnIn *in, GameRespawn *out);

/* ---- queries */
int game_state(const Game *g);                       /* gWorld.getState() */
int game_left(const Game *g);                        /* a GE_EXIT was given: nothing runs until game_load_city */
int game_play_type(const Game *g);
int game_lives(const Game *g, int slot);             /* GameStatus.getNumLives (the HUD's "xN") */
/* gWorld.getGameClockTime (seconds left, 0x347DC0) and isGameClockVisible (ClockVisible, states 5 and 6) */
float game_clock(const Game *g, int *visible);
/* gWorld.getGameClockLimit (World binding 81, 0x2AF818): the clock's +0x30, the current block's TimeLimit (the Timed
 * Run's 180 a block; TimedRunResults' "+ Bonus Time") */
float game_clock_limit(const Game *g);
int game_city_index(const Game *g);                  /* gCommon.getGameStatus().getCityIdx(): CityData.City<n> */
int game_points(const Game *g, int slot, int cat);  /* Player.getPoints(cat): this block's (the HUD shows GPT_TOTAL) */
int game_kills(const Game *g, int slot, int cat);
int game_city_score(const Game *g, int slot);        /* Player.getCityScore: the blocks' totals so far */
int game_city_total(const Game *g);                  /* all players' blocks so far, against the ParScore */
int game_blocks_won(const Game *g, int slot);        /* King modes */
int game_cities_won(const Game *g, int slot);
int game_blocks_to_win(const Game *g);               /* gCommon.getNumBlocksToWin: 6 - players */
int game_cities_to_win(const Game *g);               /* gCommon.getNumCitiesToWin: (cities - 1) / players + 1 */
int game_king(const Game *g, int which);             /* 0 of the block, 1 of the city, 2 of the world; -1 none */
int game_city_destroyed(const Game *g);              /* gCommon.isCityDestroyed */
int game_par_score(const Game *g);                   /* gWorld.getParScore (WorldData ParScore) */
uint32_t game_city_flags(const Game *g);             /* 1 destroyed, 2 par beaten, 8 (all monsters ...) [R 0x4D7C3C] */
int game_block(const Game *g, const char **name);    /* the current block's index in the BlockList, its name */
const char *game_block_name(const Game *g, int which);   /* gWorld.getBlockName(which): 0 the block played, 1 the
                                                          * current (they differ once the next one is activated) */
int game_num_blocks(const Game *g);                  /* gWorld.getNumBlocks: BlockEnd - BlockStart + 1 */
int game_result_state(const Game *g);                /* WorldGuiDriver::getResultState */

#endif
