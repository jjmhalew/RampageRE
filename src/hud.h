/* hud.h - the game's 2D interface over the world: the in-play HUD of one player (HudN.ifc and Clock.ifc of
 * WorldGui.bdp, run by the logic of Hud.bsf / Clock.bsf), the floating score numbers (PointsBanner, 0x331D50), the
 * text banners (PointsBanner.createAndStartText: Rage.ifc + Rage.bsf), the challenge / boss status (ChallengeStatus),
 * the challenge card (ChallengeText) and the results screens (WorldGuiDriver::startResults: Results, GameOver),
 * drawn with the game's bitmap fonts (.fdf + texture) and texts (.loc). docs/FORMAT_GUI.md has the formats,
 * docs/SCREENS_NOTES.md the banners and screens; hud.c cites the game code ([R] read in the code or the files, [G] a
 * guess where it matters).
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

/* gCommon.getPlayType() as the scripts test it [R Hud.scr, ChallengeStatus.scr, KingCityResults.scr] (game.h GP_*) */
enum {
    HUD_PLAY_ATTRACT = 0,
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
 * and the number's motion and size are drawn at 8 pixels per world unit [G]. */
void hud_float(Hud *h, int amount, const float pos[3], int building, float scale);

/* ---- the text banners: PointsBanner.createAndStartText (0x2B1640) loads the interface Rage and hands it the text
 * (Rage::setPointsBannerData) [R]. Each is its own copy of Rage.ifc: 15 ghosts swirl in to the centre (0.75 s), the
 * text holds (hold s, shaking), fades out (0.5 s), then is gone. */
/* createAndStartText(name, colour, scale, sound): name is a .loc key ("InstantRampage", "Challenge_Met"), or a text
 * when it begins with '#'; rgba 0 .. 1; scale the text object's (0.5 edibles, 1.0 challenges); sound played when it
 * starts, "" / NULL none, "Default" keeps "v_rampage". Edible.scr's "\n" in the name (two characters) is made a line
 * break here. Use it for edible.h EE_MESSAGE (name, v, amount, name2). */
void hud_banner_text(Hud *h, const char *name, const float rgba[4], float scale, const char *sound);
/* createAndStartText2 (0x2B14E0): a second line (Rage.ifc's line2, ArialBlack24) and the hold time */
void hud_banner_text2(Hud *h, const char *name, const char *name2, const float rgba[4], float scale, float hold,
                      const char *sound);
/* Monsters::startRage's screen.loadInterface("Rage"): the RAMPAGE banner as the layout has it ("Rampage", red,
 * scale 1, "v_rampage", hold 0.35 s) [R Monsters.scr, Rage.scr] */
void hud_banner_rage(Hud *h);
/* Cryotube.scr: the monster reveal (edible.h EE_REVEAL): "#" + "%s Unlocked" of the monster's localised name, colour
 * 220 0 0, scale 0.5, sound "V_Monster_Reveal_Seq" [R]. monster: gCommon.getMonsterName(gWorld.startReveal(pos)),
 * e.g. "Boosy" */
void hud_banner_reveal(Hud *h, const char *monster);

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
    const char *city;        /* gCommon.getCityName(getLastCityIdx()): a .loc key, "LasVegas" */
    int city_score;          /* gCommon.getGameStatus().getCityScore() (game_city_total) */
    int par_score;           /* gWorld.getParScore() (game_par_score) */
    unsigned city_flags;     /* getCityFlags() (game_city_flags): a star each for 1 all blocks, 2 par, 4 challenges,
                              * 8 monsters revealed */
    int show_victory;        /* gCommon.getShowVictory(): no "Change Monster" / "Quit" */
} HudResults;
/* show results screen `screen` (any open one is unloaded). Results (the block's: the score lines counting up, the
 * time bonus among them), LevelResults (the city's: its score, the par, the stars) and GameOver are the game's; the
 * others are stand-ins that draw nothing and answer DONE after 3 s or on HUD_EV_ACCEPT. r may be NULL for
 * GameOver */
void hud_results(Hud *h, int screen, const HudResults *r);
/* the result the screen gave since the last call (HUD_RESULT_*, the numbers of game.h GR_*), HUD_RESULT_NONE none */
int hud_result(Hud *h);
int hud_results_showing(const Hud *h);   /* a results screen is up (it has the pad: route hud_input to it) */
int hud_paused(const Hud *h);            /* gApp.pauseGame(1): the GameOver screen pauses the game until a choice */
/* stand-in for a player: press ACCEPT for them this many seconds after a screen takes buttons (Results' Continue,
 * GameOver's Retry); 0 = never (default) */
void hud_results_auto(Hud *h, float seconds);

/* the pad, as GuiEvent ids [R the screens' scripts; G which button is which]: HUD_EV_BUTTON with button 0 is the
 * Results' "Change Monster" (Controller_B0), 1 its "Autosave" (Controller_B1) */
enum {
    HUD_EV_DOWN = 2, HUD_EV_UP = 4, HUD_EV_LEFT = 8, HUD_EV_RIGHT = 16, HUD_EV_BACK = 32, HUD_EV_ACCEPT = 64,
    HUD_EV_START = 128, HUD_EV_BUTTON = 256
};
void hud_input(Hud *h, int ev, int button);

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
} HudQuad;
/* the quads back to front: the floating numbers (they are in the 3D world, under the GUI), then the interfaces in the
 * order they were loaded (the HUD, the clock, ChallengeStatus, ChallengeText, the banners, the results screen);
 * returns the count (at most max) */
int hud_quads(const Hud *h, HudQuad *out, int max);
#endif
