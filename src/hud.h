/* hud.h - the game's 2D interface over the world: the in-play HUD of one player (HudN.ifc and Clock.ifc of
 * WorldGui.bdp, run by the logic of Hud.bsf / Clock.bsf), the floating score numbers (PointsBanner, 0x331D50), the
 * text banners (PointsBanner.createAndStartText: Rage.ifc + Rage.bsf), the challenge / boss status (ChallengeStatus),
 * the challenge card (ChallengeText), the results screens (WorldGuiDriver::startResults: Results, LevelResults,
 * WorldResults, the King screens, TimedRunResults, GameOver) and the Pause menu (Pause.ifc + Pause.bsf), drawn with
 * the game's bitmap fonts (.fdf + texture) and texts (.loc).
 * docs/FORMAT_GUI.md has the formats, docs/SCREENS_NOTES.md the banners and screens, docs/PAUSE_NOTES.md the Pause
 * menu; hud.c cites the game code ([R] read in the code or the files, [G] a guess where it matters).
 *
 * Use: load WorldGui.bdp and CommonGUI.bdp (vol_read + bdp_load), hud_open() once per player, hud_set() whenever the
 * monster's numbers change (or every frame), hud_float() for a score award the game shows (army AE_FLOAT),
 * hud_update() every frame, hud_quads() for what to draw. The banners, the challenge status and the results screens
 * belong to the screen, not to a player: use player 0's Hud for them. Nothing here touches OpenGL or plays sounds
 * (hud_sounds() says which to play).
 *
 * Drawing (the viewer): the quads are in the GUI's reference screen, 640 x 480 pixels, y down (the layouts place
 * full-screen items at 320 240 with size 640 480 [R]). Scale x by window_width / 640 and y by window_height / 480 (or
 * keep 4:3 and centre). Draw them in the order given, after the 3D scene, with no depth test and normal alpha blending
 * (src.a, 1 - src.a); the texture (tex_decode of bdp_entry_of(b, tex, ET_TEXTURE)) times col; b == NULL: just col. */
#ifndef RAMPAGE_HUD_H
#define RAMPAGE_HUD_H
#include <stdint.h>
#include "data.h"

#define HUD_SCREEN_W 640.0f
#define HUD_SCREEN_H 480.0f
#define HUD_TEX_VIDEO 0xFFFFFFFFu   /* HudQuad.tex with no bundle: the movie's picture (rnd_video) */
#define HUD_TEX_3D 0xFFFFFFFEu      /* HudQuad.tex with no bundle: the next 3D layer is drawn here (the shell's globe and
                                     * effects, shell_3d; its corners mean nothing) */

/* gCommon.getPlayType() as the scripts test it [R Hud.scr, ChallengeStatus.scr, KingCityResults.scr] (game.h GP_*) */
enum {
    HUD_PLAY_ATTRACT = 0,    /* the demo: player 1's screen loads AttractMode.ifc, "Press START button" blinking */
    HUD_PLAY_CAMPAIGN = 2,   /* the lives counter "xN" by the head */
    HUD_PLAY_KING_CITY = 3,  /* King of the City / World: the blocks icon "xN" instead of the head */
    HUD_PLAY_KING_WORLD = 4,
    HUD_PLAY_TIMED = 5
};

typedef struct {
    int player;              /* 0 .. 3: the layout Hud<player>.ifc (its group at x 48, 158, 392 or 502) */
    int num_players;         /* 2 and player 1: the group moves to 502 60 [R Hud.scr] */
    int play_type;           /* HUD_PLAY_* */
    int blocks_to_win;       /* King modes: gCommon.getNumBlocksToWin() (the one block icon shows when > 0) */
    const char *monster;     /* "George", ...: the head texture Hud_<monster>_Head */
} HudConfig;

typedef struct Hud Hud;
/* worldgui: WorldGui.bdp (layouts, heads, slider mask, fonts, Game.loc / Common.loc); common: CommonGUI.bdp (the
 * ArialBlack24 and Atrox60 fonts, frame and button pictures). The bundles must outlive the Hud. NULL when a layout or
 * a font is missing. Player 0's Hud also loads ChallengeStatus.ifc (the city scripts load it with the HUDs [R]). */
Hud *hud_open(const Bundle *worldgui, const Bundle *common, const HudConfig *cfg);
void hud_close(Hud *h);
/* one more bundle to look in for pictures and texts (.loc): the city's (<City>.bdp holds <City>.loc: the block names
 * the results show, the challenge texts) and any other; up to 6. It must outlive the Hud */
void hud_add_bundle(Hud *h, const Bundle *b);
/* gApp.localizeString(key): the text of a .loc key in the bundles (keys ignore case), NULL when there is none.
 * Language 0 is English (tools/loc.py's columns: 0 English, 1 English 2, 2 Japanese, 3 French, 4 German, 5 Italian,
 * 6 Spanish, 7 Dutch, 8 Portuguese) */
const char *hud_string(const Hud *h, const char *key);
void hud_set_language(Hud *h, int lang);

/* the monster's numbers: hit points of most hp_max (MONSTER_DATA +0x138, 0 .. 200 for most), energy (rage) of most
 * energy_max (the script's maxEnergy is 100), score = player.getPoints(6) (ArmyStats points[6]); lives (campaign) and
 * blocks won (King modes); negative: unchanged */
void hud_set(Hud *h, float hp, float hp_max, float energy, float energy_max, int score, int lives, int blocks);
/* the game clock (gWorld.isGameClockVisible / getGameClockTime): mm:ss at the top centre, fading in */
void hud_set_clock(Hud *h, int visible, float seconds);
/* game.h GE_HUD (a): the HUD's and the ChallengeStatus' fade, 1 shown, 0 hidden (BeginBlockNI / ResultsNI set it at
 * once [R Levels.scr]); the Hud starts shown */
void hud_show(Hud *h, float fade);

/* where a world point lands: screen in the reference 640 x 480 pixels, *px_per_unit the pixels one world unit spans
 * there (for a perspective camera: focal_px / depth). Return 0 when it is behind the camera. */
typedef int (*HudProject)(void *ctx, const float world[3], float screen[2], float *px_per_unit);
void hud_set_project(Hud *h, HudProject fn, void *ctx);
/* a floating score number (a PointsBanner): amount at pos (world, Z up; army AE_FLOAT's pos, already 5 towards the
 * street), building = 1 for a whole building's award (yellow), 0 for the rest (white); scale 0.8 for the army's
 * awards [R 0x2BAB2C], 1.0 for an edible's [R Edible.scr]. Without a projection, pos[0], pos[1] are a screen point
 * and the number's motion and size are drawn at 8 pixels per world unit (a viewer's fallback: the game always has
 * its camera). */
void hud_float(Hud *h, int amount, const float pos[3], int building, float scale);

/* ---- the text banners: PointsBanner.createAndStartText (0x2B1640) loads the interface Rage and hands it the text
 * (Rage::setPointsBannerData) [R]. Each is its own copy of Rage.ifc: 15 ghosts swirl in to the centre (0.75 s), the
 * text holds (hold s, shaking), fades out from half (0.5 s), then is gone. */
/* createAndStartText(name, colour, scale, sound): name is a .loc key ("InstantRampage", "Challenge_Met"), or a text
 * when it begins with '#'; rgba 0 .. 1; scale the text object's (0.5 edibles, 1.0 challenges); sound played when it
 * starts, "" / NULL none, "Default" keeps "v_rampage". Edible.scr's "\n" in the name (two characters) is made a line
 * break here. Use it for edible.h EE_MESSAGE (name, v, amount, name2). */
void hud_banner_text(Hud *h, const char *name, const float rgba[4], float scale, const char *sound);
/* createAndStartText2 (0x2B14E0): a second line (Rage.ifc's line2, ArialBlack24) and the hold time */
void hud_banner_text2(Hud *h, const char *name, const char *name2, const float rgba[4], float scale, float hold,
                      const char *sound);
/* Monsters::startRage's screen.loadInterface("Rage"): the RAMPAGE banner as the layout has it ("Rampage", red,
 * scale 1, "v_rampage", no hold: the swirl, then the text at half fading out) [R Monsters.scr, Rage.scr]. A text
 * banner asked for while another is up gives its data to the oldest one and shows this one [R 0x231220] */
void hud_banner_rage(Hud *h);
/* Cryotube.scr: the monster reveal (edible.h EE_REVEAL): "#" + "%s Unlocked" of the monster's localised name, colour
 * 220 0 0, scale 0.5, sound "V_Monster_Reveal_Seq" [R]. monster: gCommon.getMonsterName(gWorld.startReveal(pos)),
 * e.g. "Boosy" */
void hud_banner_reveal(Hud *h, const char *monster);
/* gWorld.hideChallengeCompletionUI (World binding 102, 0x2B01C0) [R]: every interface on the screen named
 * "Removeable" gets flag 0x1 (hidden): those are the text banners, which Rage::setPointsBannerData renames ("CHALLENGE
 * MET!", "5,000 PTS", the edibles' and the upgrades' texts, the monster reveal), not the plain RAMPAGE banner. They
 * run on unseen and unload as usual. StreetObject.bsf calls it when a monster takes the challenge's upgrade, before
 * its own banner */
void hud_hide_challenge_completion(Hud *h);

/* ---- the challenge: ChallengeStatus (the item count or the boss's health bar, bottom left) and ChallengeText (the
 * card at the block's start). What the game's gWorld knows about the current block's challenge: */
typedef struct {
    int last_block;          /* gWorld.getCurrentBlockNum() == getNumBlocks(): the boss block */
    const char *identifier;  /* getChallengeIdentifier(): the .loc key of the challenge's text ("Challenge01"), NULL */
    const char *icon;        /* getChallengeIcon(): a texture name, NULL / "" none */
    float icon_scale;        /* getChallengeIconScale(0) */
    float icon_shift;        /* getChallengeIconShift() */
    int upgrade;             /* getChallengeUpgrade(): 0, or the upgrade (1 2 4 8: "Upgrade<n>") */
    int required;            /* getChallengeItemRequired(): -1 no challenge */
    float pause_scale;       /* getChallengeIconScale(1): the ChallengeDef's PauseScale, else its Scale [R 0x2AF488]
                              * (the Pause menu's icon) */
} HudChallenge;
/* a block was activated (Levels::onActivateBlock -> ChallengeStatus::updateBossStatus, setupUI) [R]: the boss bar
 * shows on the last block in the campaign and the Timed Run, the item count on the others */
void hud_challenge(Hud *h, const HudChallenge *c);
/* the items collected (getChallengeItemCollected) and required, every frame or when they change; the count shows
 * "n/m", hidden when required is -1; meeting it starts the "CHALLENGE MET!" banner and returns 1 (the caller adds
 * ChallengeDef.Points (5000) / players to each player's GPT_CHALLENGE [R ChallengeStatus.scr]) */
int hud_challenge_items(Hud *h, int collected, int required);
/* boss.h BE_BOSS_HEALTH: gCommon.updateChallengeHud(fraction) -> ChallengeStatus::updateBossHealth [R]: the bar at
 * 100 x fraction; at 0 or less the "CHALLENGE MET!" banner (campaign), the bar fades out (0.5 a second) and it
 * returns 1: the caller adds ChallengeDef.Points (5000) / players to each player's GPT_CHALLENGE (the script does
 * this on every call at 0) */
int hud_boss_health(Hud *h, float fraction);
/* game.h GE_GUI "ChallengeText" (screen.loadInterface, BeginBlockNI): the challenge card for 3 s (the boss's for
 * 8 s), with the data of the last hud_challenge(); it unloads at once unless there is an identifier, items required
 * and the campaign [R ChallengeText.scr]. Any other name loads nothing (returns 0). */
int hud_load_interface(Hud *h, const char *name);

/* ---- the results screens: WorldGuiDriver::startResults(screen) (game.h GE_RESULTS a, GR_SCREEN_*). The screen
 * answers through WorldGuiDriver::setResultState: hud_result() hands that over once (game_set_result). */
enum {
    HUD_SCREEN_GAME_OVER = 0, HUD_SCREEN_RESULTS = 1, HUD_SCREEN_LEVEL = 2, HUD_SCREEN_WORLD = 3,
    HUD_SCREEN_KING_BLOCK = 4, HUD_SCREEN_KING_CITY = 5, HUD_SCREEN_KING_WORLD = 6, HUD_SCREEN_TIMED_RUN = 7
};
enum { HUD_RESULT_NONE = 0, HUD_RESULT_DONE = 1, HUD_RESULT_CONTINUE = 2, HUD_RESULT_SELECT = 3, HUD_RESULT_QUIT = 4 };
typedef struct {
    int play_type;           /* HUD_PLAY_* */
    int num_players;
    int points[4][7];        /* Player.getPoints(cat) per player, game.h GPT_* (game_points): this block's */
    int kills[4][7];         /* Player.getKills(cat) (game_kills) */
    const char *block_played;    /* gWorld.getBlockName(0) (game_block_name(g, 0)): a .loc key, "LV01_Casino1" */
    const char *block_current;   /* getBlockName(1): the next block; the same on the city's last block */
    const char *results_text;    /* the current block's "ResultsNIText" (a .loc key), NULL none */
    int buildings;           /* gWorld.getCurrentBuildingCount() */
    int city_destroyed;      /* gCommon.isCityDestroyed() (game_city_destroyed) */
    int monster_revealed;    /* gWorld.wasMonsterRevealed() */
    /* LevelResults (the city's end) */
    const char *city;        /* gCommon.getCityName(getLastCityIdx()): a .loc key, "LasVegas" (KingCityResults asks
                              * getGameStatus().getCityName(): the same city) */
    int city_score;          /* gCommon.getGameStatus().getCityScore() (game_city_total) */
    int par_score;           /* gWorld.getParScore() (game_par_score) */
    unsigned city_flags;     /* getCityFlags() (game_city_flags): a star each for 1 all blocks, 2 par, 4 challenges,
                              * 8 monsters revealed */
    int show_victory;        /* gCommon.getShowVictory(): no "Change Monster" / "Quit" */
    /* the King screens (KingBlockResults, KingCityResults, KingWorldResults), per player slot 0 .. 3 */
    const char *monsters[4]; /* gCommon.getGameStatus().getMonsterName(i): a .loc key ("George", "Boosy") */
    const char *monster_none;    /* getMonsterName(-1), asked for a King that is none (a tie): the status's +0x14 - 4,
                                  * its start block (+0x10, game_start_block), taken as a roster index [R 0x2C4B18] */
    int king_block, king_city, king_world;   /* gCommon.getKingOfBlock / City / World (game_king(g, 0 / 1 / 2)): a
                                              * player slot, -1 none (a tie) */
    int blocks_won[4];       /* Player.getNumBlocksWon (game_blocks_won) */
    int cities_won[4];       /* Player.getNumCitiesWon (game_cities_won) */
    int blocks_to_win;       /* gCommon.getNumBlocksToWin (game_blocks_to_win) */
    int cities_to_win;       /* gCommon.getNumCitiesToWin (game_cities_to_win) */
    const char *next_city;   /* gCommon.getCityName(getGameStatus().getCityIdx() + 1): the King of the World's next
                              * city, a .loc key (CityData.City<game_city_index + 1>) */
    /* TimedRunResults */
    float clock_time;        /* gWorld.getGameClockTime (game_clock): the seconds left, this block's added */
    float clock_limit;       /* gWorld.getGameClockLimit (game_clock_limit): the seconds this block added */
    /* Results' Save button */
    int allow_save;          /* gCommon.getAllowSave() as the screen opens */
    int loaded;              /* gCommon.hasLoadedProfile() */
} HudResults;
/* show results screen `screen` (any open one is unloaded): GameOver, Results (the block's: the score lines counting up,
 * the time bonus among them), LevelResults (the city's: its score, the par, the stars), WorldResults (the campaign's
 * end), KingBlockResults (the block's winner, the points, the standings), KingCityResults (the King of the City, the
 * standings), KingWorldResults (the King of the World) and TimedRunResults (the time left and added). r may be NULL
 * for GameOver. A screen whose layout is missing (a viewer's fallback; the game's files have them all) draws nothing
 * and answers DONE after 3 s or on HUD_EV_ACCEPT */
void hud_results(Hud *h, int screen, const HudResults *r);
/* the result the screen gave since the last call (HUD_RESULT_*, the numbers of game.h GR_*), HUD_RESULT_NONE none */
int hud_result(Hud *h);
int hud_results_showing(const Hud *h);   /* a results screen is up (it has the pad: route hud_input to it) */
int hud_paused(const Hud *h);            /* gApp.pauseGame(1): the GameOver screen or the Pause menu pauses the game
                                          * until a choice */
/* gCommon.isSaveGameActive() (shell.h shell_popup_active): Results holds its answer (Continue, Change Monster, the
 * quit) while the SaveGame popup is up [R Results.scr:200] */
void hud_set_save_active(Hud *h, int on);
/* gCommon.getAllowSave() changed under the screen (the popup's warn-continue turned autosave off): Results' Save button
 * shows again [R Results.scr:217, read each frame] */
void hud_set_allow_save(Hud *h, int on);
/* a test aid in place of a player: press ACCEPT for them this many seconds after a screen takes buttons (Results'
 * Continue, GameOver's Retry); 0 = never (default) */
void hud_results_auto(Hud *h, float seconds);

/* the pad, as GuiEvent ids [R the screens' scripts]. Which direction is which [R by the scripts' use]: the Pause
 * menu's options page picks its rows (stacked down the page) with 8 / 16 while the held left / right move the
 * sliders and 4 / 2 turn the vibration's ON / OFF (side by side); ConfirmQuit's No / Yes (side by side) take 4 / 2;
 * the main menu's cheat code moves along its six digits with 4 / 2 and changes one with 8 (+1) / 16 (-1). So 2 is
 * right, 4 left, 8 up, 16 down. HUD_EV_BUTTON with button 0 is the Results' "Change Monster" (Controller_B0), 1 its
 * "Autosave" (Controller_B1) */
enum {
    HUD_EV_RIGHT = 2, HUD_EV_LEFT = 4, HUD_EV_UP = 8, HUD_EV_DOWN = 16, HUD_EV_BACK = 32, HUD_EV_ACCEPT = 64,
    HUD_EV_START = 128, HUD_EV_BUTTON = 256
};
/* to the interface that has the pad: the Pause menu when it shows, else a results screen */
void hud_input(Hud *h, int ev, int button);
/* the soak test's world half (0x2B6350; cheat 082864, flag 0x10000): call each world frame, running or paused, with
 * the flag set. popup: the SaveGame popup is up (1 shown in full, -1 still fading in, 0 not up); block: the world's
 * block index (game_block). It presses on the results screens itself; the event to give the popup, 0 none */
int hud_soak(Hud *h, int popup, int block);
/* the same from pad `pad` (GuiEvent.getController(): 0 = player 1); hud_input is pad 0. The Pause menu's Options
 * page answers pad 0 only [R Pause.scr] */
void hud_input_pad(Hud *h, int ev, int button, int pad);
/* pad 0 held, every frame (gGuiMgr.getController(0): getX(0), getButtons() & 4 / 8): the stick across (-1 .. 1) and
 * the d-pad's left and right; the Options page's sliders move while they are held */
void hud_pad_held(Hud *h, float stick_x, int left, int right);

/* ---- the Pause menu (docs/PAUSE_NOTES.md): Start reaches WorldGuiDriver::process (event 128) when no other
 * interface has the pad, and outside the attract mode it loads the interface Pause (Pause.ifc + Pause.bsf) [R]. It
 * takes the pad, pauses the game (gApp.pauseGame(1): hud_paused()), turns the music down to 0 in 1 s and the
 * vibration off. "Game Paused", "<city> : <block>", the block's challenge (or the boss's card), the list Continue /
 * Options / Quit and the controller's picture (not on the boss block). Continue resumes; Options (pad 0 only) shows
 * the sound and music sliders and the vibration's ON / OFF; Quit asks ConfirmQuit, Yes leaves for the shell
 * (gWorld.restartShell). */
typedef struct {
    const char *city;        /* gCommon.getGameStatus().getCityName(): a .loc key ("LasVegas") */
    const char *block;       /* gWorld.getBlockName(1) (game_block_name(g, 1)): the current block's .loc key; the
                              * title reads "<city> : <block>" localised */
} HudPause;
/* Start pressed by pad `pad` (0 = player 1): shows the menu and returns 1; 0 when it cannot (attract mode, a results
 * screen has the pad, already showing). The challenge box shows the last hud_challenge() / hud_challenge_items(). */
int hud_pause_open(Hud *h, int pad, const HudPause *p);
int hud_pause_showing(const Hud *h);     /* it has the pad: route hud_input to it */
enum {
    HUD_PAUSE_NONE = 0,
    HUD_PAUSE_RESUME = 1,    /* Continue: the menu is gone, the game runs on (gApp.pauseGame(0)) */
    HUD_PAUSE_QUIT = 2,      /* ConfirmQuit's Yes: Pause::onQuit -> gWorld.restartShell (game_restart_shell) */
    HUD_PAUSE_SETTINGS = 3   /* the Options page's Accept: new settings (hud_settings) for the profile; the menu
                              * stays */
};
int hud_pause_result(Hud *h);            /* since the last call, HUD_PAUSE_NONE none */
/* the settings the game keeps (gCommon.getSoundVolume / getMusicVolume, 0 .. 0.75 = the sliders' 0 .. 100, and
 * getEnableRumble; the profile's 0x4D7ACC / 0x4D7AD0 / 0x4D7AD4 [R 0x2C2F68..0x2C3480]): set them from the profile
 * (default a new profile's 0.5, 0.25, on [R 0x33F630]); the Options page's Accept changes them */
void hud_set_settings(Hud *h, float sound, float music, int rumble);
void hud_settings(const Hud *h, float *sound, float *music, int *rumble);
/* what the Pause menu asks of gSndDev / gCommon since the last call (the same call twice in a row: the last kept) */
enum {
    HUD_CALL_MUSIC_VOLUME,   /* gSndDev.setMusicVolume(value, arg ms) */
    HUD_CALL_SFX_VOLUME,     /* gSndDev.setSfxVolume(value, arg ms) */
    HUD_CALL_RUMBLE,         /* gCommon.setEnableRumble(value != 0, arg: 1 = into the profile) */
    HUD_CALL_RUMBLE_PULSE,   /* gCommon.rumble(arg ms): a test buzz when the vibration is turned ON */
    HUD_CALL_PLAY_MUSIC,     /* the music slider went up: if no music plays, start the block's "Music" */
    HUD_CALL_SAVE_SETTINGS,  /* gCommon.setSoundVolume / setMusicVolume / setEnableRumble(.., 1): hud_settings */
    HUD_CALL_CLEAR_CHEATS,   /* gCommon.clearCheatFlags(arg): 65536 when quitting (the cheat 082864's flag) */
    HUD_CALL_SAVE_PROFILE,   /* Results: gCommon.saveProfile() (it writes only with autosave on); arg 1: 1.5 s in,
                              * gCommon.updateProfile() first */
    HUD_CALL_ALLOW_SAVE      /* Results' Save button: gCommon.setAllowSave(1) */
};
typedef struct { int call; float value; int arg; } HudCall;
int hud_calls(Hud *h, HudCall *out, int max);

/* the sounds the interfaces start (gSndDev.playSound) and stop (killSound) since the last call */
typedef struct { char name[32]; int kill; } HudSound;
int hud_sounds(Hud *h, HudSound *out, int max);

void hud_update(Hud *h, float dt);   /* seconds */

typedef struct {
    float x0, y0, x1, y1;    /* corners in the reference screen (x0, y0 top-left) */
    float u0, v0, u1, v1;    /* texture coordinates at those corners, over the whole decoded texture, v = 0 first row */
    float col[4];            /* rgba 0 .. 1, multiplies the texture */
    const Bundle *b;         /* the texture's bundle, NULL: an untextured quad */
    uint32_t tex;            /* name index of the texture in b */
    float skew;              /* the top edge moved this far along x (a slanted glyph), 0 upright */
    int additive;            /* M 0x80: the additive shader (GS state 0x17, ALPHA 0x48: Cs As + Cd) [R 0x17D100] */
    int corners;             /* set: drawn from cx / cy / cu / cv instead (a turned picture: the shell's side frost) */
    float cx[4], cy[4], cu[4], cv[4];   /* its top-left, top-right, bottom-right, bottom-left corners and their uv */
} HudQuad;
/* the quads back to front: the floating numbers (they are in the 3D world, under the GUI), then the interfaces in the
 * order they were loaded (the HUD, the clock, ChallengeStatus, ChallengeText, the banners, the results screen, the
 * Pause menu);
 * returns the count (at most max) */
int hud_quads(const Hud *h, HudQuad *out, int max);
#endif
