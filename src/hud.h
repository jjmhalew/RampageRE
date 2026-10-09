/* hud.h - the in-play HUD of one player: the game's own layout (HudN.ifc and Clock.ifc of WorldGui.bdp, run by the
 * script logic of Hud.bsf / Clock.bsf) drawn with the game's bitmap fonts (.fdf + texture), and the floating score
 * numbers (PointsBanner, 0x331D50). docs/FORMAT_GUI.md has the formats; hud.c cites the game code ([R] read in the
 * code or the files, [G] a guess where it matters).
 *
 * Use: load WorldGui.bdp and CommonGUI.bdp (vol_read + bdp_load), hud_open() once per player, hud_set() whenever the
 * monster's numbers change (or every frame), hud_float() for a score award the game shows (army AE_FLOAT),
 * hud_update() every frame, hud_quads() for what to draw. Nothing here touches OpenGL.
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

/* gCommon.getPlayType() as the scripts test it [R Hud.scr, ChallengeStatus.scr, KingCityResults.scr] */
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
/* worldgui: WorldGui.bdp (layouts, heads, slider mask, PointsBanner font); common: CommonGUI.bdp (ArialBlack24). The
 * bundles must outlive the Hud. NULL when a layout or a font is missing. */
Hud *hud_open(const Bundle *worldgui, const Bundle *common, const HudConfig *cfg);
void hud_close(Hud *h);

/* the monster's numbers: hit points of most hp_max (MONSTER_DATA +0x138, 0 .. 200 for most), energy (rage) of most
 * energy_max (the script's maxEnergy is 100), score = player.getPoints(6) (ArmyStats points[6]); lives (campaign) and
 * blocks won (King modes); negative: unchanged */
void hud_set(Hud *h, float hp, float hp_max, float energy, float energy_max, int score, int lives, int blocks);
/* the game clock (gWorld.isGameClockVisible / getGameClockTime): mm:ss at the top centre, fading in */
void hud_set_clock(Hud *h, int visible, float seconds);

/* where a world point lands: screen in the reference 640 x 480 pixels, *px_per_unit the pixels one world unit spans
 * there (for a perspective camera: focal_px / depth). Return 0 when it is behind the camera. */
typedef int (*HudProject)(void *ctx, const float world[3], float screen[2], float *px_per_unit);
void hud_set_project(Hud *h, HudProject fn, void *ctx);
/* a floating score number (a PointsBanner): amount at pos (world, Z up; army AE_FLOAT's pos, already 5 towards the
 * street), building = 1 for a whole building's award (yellow), 0 for the rest (white); scale 0.8 for the army's
 * awards [R 0x2BAB2C], 1.0 for an edible's [R Edible.scr]. Without a projection, pos[0], pos[1] are a screen point
 * and the number's motion and size are drawn at 8 pixels per world unit [G]. */
void hud_float(Hud *h, int amount, const float pos[3], int building, float scale);

void hud_update(Hud *h, float dt);   /* seconds */

typedef struct {
    float x0, y0, x1, y1;    /* corners in the reference screen (x0, y0 top-left) */
    float u0, v0, u1, v1;    /* texture coordinates at those corners, over the whole decoded texture, v = 0 first row */
    float col[4];            /* rgba 0 .. 1, multiplies the texture */
    const Bundle *b;         /* the texture's bundle, NULL: an untextured quad */
    uint32_t tex;            /* name index of the texture in b */
} HudQuad;
/* the quads back to front: the floating numbers (they are in the 3D world, under the GUI), then the HUD; returns the
 * count (at most max) */
int hud_quads(const Hud *h, HudQuad *out, int max);
#endif
