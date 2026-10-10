/* main.c - RampageRE's native viewer: draws the city blocks straight from RAMPAGE.VOL (the game's own archive),
 * with the game's layout, lights, traffic, people and a monster (city.c, anim.c, render_gl.c).
 *   rampage [RAMPAGE.VOL] [City] [block number] [--night] [--monster NAME] [--clip N] [--shot FILE.png] [--time T]
 * default build\disc\RAMPAGE.VOL, Chicago, 1. --shot draws one frame (at T seconds) as a PNG and quits; with a
 * monster in play the camera settles for 2 s, then the monster plays --do "right 1, cross, wait 1.5, ..." (held
 * stick directions and pressed buttons, each for its seconds; --walk S = "right S").
 * Keys: W A S D move, Q E down and up, Shift faster, right mouse button look, wheel speed; Page Up / Page Down the
 * block, 1-7 the city, N day / night, F fog, M the next monster (none, then the 30), K / L its clips, F12 a screenshot
 * (shot_NNN.png), Esc quit. With a monster, G switches to play: the monster runs its own move graph (MONSTER_DATA,
 * moves.c) from a pad made of keys - W A S D / the arrows the left stick, Space Cross (jump), J Square (punch),
 * K Triangle (kick), L Circle (grab), R R2 (roar) - and the game's own camera (camera.c) follows it. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "render_gl.h"
#include "camera.h"
#include "moves.h"
#include "climb.h"
#include "ik.h"
#include "fx.h"
#include "sound.h"
#include "army.h"
#include "hud.h"
#include "vehicle.h"
#include "rail.h"
#include "edible.h"
#include "game.h"
#include "look.h"
#include "shadow.h"
#include "director.h"
#include "footstep.h"
#include "challenge.h"
#include "reveal.h"
#include "profile.h"
#include "shell.h"
#include "movie.h"
#include <ctype.h>
#include <time.h>
#include "boss.h"
#include "streetobj.h"
#include "parade.h"
#include "collide.h"

static const char *const CITIES[] = { "Chicago", "NewYork", "SanFrancisco", "LosAngeles", "London", "HongKong", "LasVegas" };
/* the campaign's order (ShellData CityData.City0 .. City6, GAME_NOTES.md) */
static const char *const CAMPAIGN[] = { "LasVegas", "SanFrancisco", "London", "LosAngeles", "Chicago", "HongKong", "NewYork" };
static int campaign_index(const char *city)
{
    for (int i = 0; i < 7; i++)
        if (!strcmp(CAMPAIGN[i], city))
            return i;
    return 0;
}
#define NCITIES 7

/* the pad a monster is driven by this frame: the left stick (x right, y down), the right stick, the buttons pressed
 * this frame (MV_CROSS ...) and held */
typedef struct {
    float lx, ly, rx, ry;
    uint8_t pressed[MV_COUNT], held[MV_COUNT];
} Pad;

/* how a monster is driven (docs/MULTIPLAYER_NOTES.md): player 1's keys, player 2's keys, or the computer (the bots,
 * and the players without keys of their own: 3 and 4) */
enum { DRIVE_KEYS1 = 0, DRIVE_KEYS2 = 1, DRIVE_COMPUTER = 2 };

#define MONSTERS 4                                   /* the game's players and bots together (Common 0x4D7E28) */

/* a monster in play, one per player or bot: its slot is the scene's (Scene.mon), the army's, the game's player and
 * the HUD's (Hud<slot>.ifc). The game keeps all of this in the monster object (the MONSTER_DATA offsets below). */
typedef struct Mon {
    struct Viewer *v;                                /* the world it plays in */
    int slot;                                        /* 0 .. 3 */
    int monster;                                     /* which of the 30 (Viewer.monsters), -1 none */
    int bot;                                         /* a bot: the last num_bots of the players (setIsAI in each block) */
    int drive;                                       /* DRIVE_*: whose pad */
    Pad pad;                                         /* this frame's pad (keys, --do, or the computer's) */
    float mx, my, mz, heading;                       /* where the monster stands, its turn about Z (0: facing +Y) */
    float vel[3]; int mode, no_gravity, column;      /* velocity (+0xBD30), movement mode (+0xAD8), no gravity
                                                        (+0xAB8), the slots' column (+0xBAB4) */
    MoveGraph graph; MoveRunner run; MoveInput input; int have_graph;
    int yaw_axis; float entry_yaw, entry_root[3], entry_t;    /* the root's axis that tells its turn; its turn and place at the
                                                        state's start */
    float frame[2][3], stick[2];                     /* the camera's right and forward axes the stick is read in
                                                        (+0xA18, +0xA24) and the stick they were taken for (+0xA3C) */
    float air[3];                                    /* the stick's own velocity (+0xA68), NODE_AIR_* */
    uint32_t armed;                                  /* body parts an attack event armed (bits) */
    int mirror, mirror_queued;                       /* the clip plays mirrored (+0xAA0); for the next clip (+0xAA1) */
    Mount mount;                                     /* on a building's face (climb.c) */
    Hand hand[2];                                    /* its hands on the wall (0 left, 1 right, as seen) */
    int held;                                        /* the person in its hand (people.c), -1 none */
    int held_unit;                                   /* or the army unit (army.c), -1 none */
    int held_vehicle;                                /* or a vehicle (vehicle.c), -1 none */
    int held_parade;                                 /* or a biker or a cart's food (parade.c), -1 none */
    int held_rail, rail_knock;                       /* or a rail object (rail.c), -1 none; a knock taken */
    int blow;                                        /* counts the attacks armed: one TakeDamage per blow (parade.c) */
    int streaker;                                    /* the defeated monster's streaker (people.c), -1 none */
    EdiblePower power;                               /* the monster's energy, rage and speed (energy mirrored in .energy) */
    int pending_slot;                                /* a reaction the army caused: started at the next step, -1 */
    int defeated;                                    /* its HP ran out: React_Defeated .. the respawn */
    float look_yaw, look_pitch;                      /* the head animator's angles now (+0xBC, +0xE8) */
    float scale, scale_from, scale_to, scale_time, scale_left;   /* setDynamicScale */
    DirGraph ag; int have_ag;                        /* the move graph as the AI reads it (director.c) */
    DirAI ai; int is_ai;                             /* the monster's goals while the game drives it (setIsAI) */
    float hp, energy;                                /* its hit points (MONSTER_DATA +0x138: 200) and energy, eating */
    const MoveNode *face;                            /* the face layer's node playing now */
    IkFeet feet_ik; int feet_due;                   /* the legs bent to the held feet (ik.c), once a frame */
    FootProps feet_fx; FootState feet;              /* the footfalls' names and the foot records (footstep.c) */
    FaceQueue faces;                                 /* setFacialAnim / queueFacialAnim's records, the belches (look.c) */
    HeadLook head; LookLimits look_lim;              /* the head animator (look.c) */
    float friction;                                  /* +0xF4: the ground velocity's friction, set by the last knock */
    int msph0, nmsph;                                /* the monster's spheres in the frame's world (objects msph0 ..) */
    ShadowMonster smon;                              /* its blob shadows (shadow.c) */
    Hud *hud;                                        /* its HUD (Hud<slot>.ifc); slot 0's is the screen's (Viewer.hud) */
    int human;                                       /* it became human (+0xAB4): its streaker runs */
    int dropping;                                    /* dropping in after a respawn (+0xAB5), until it lands */
    /* the attack armed last (its record, 0x2E35D0): damage (+0x2C), the knock's distance and time (+0x30, +0x34),
     * ReactionLevel (+0x24), the flags (+0x14: 2 in front, 4 behind) and the monsters it may hit (+0x18), and the
     * ones it hit already (0x2E33A0: each once an attack) */
    struct { float damage, knock, knock_time; int level; uint32_t flags, victims, hit; } atk;
    int sph0, nsph;                                  /* its armed spheres this frame: Scene.spheres sph0 .. */
    int reveal, shown;                               /* the cryotube's monster (Viewer.extra): out of the world
                                                      * (+0x108 / +0x109) until the reveal shows it */
} Mon;

typedef struct Viewer {
    Vol vol;
    City *city; int city_index;
    const char *blocks[64]; uint32_t nblocks, block;
    int night, fog;
    const char *monsters[64]; uint32_t nmonsters; int clip;   /* clip -1 = its idle (player 1's monster) */
    Mon mon[MONSTERS]; int nmon;                     /* the monsters in play: players then bots (Mon.monster of slot
                                                      * 0 -1: none, the free camera only) */
    int num_bots;                                    /* the last num_bots of the nmon are bots */
    Scene scene; int have_scene;
    Renderer *rnd;
    int play; CamSettings cam_settings; GameCamera cam;   /* play: the monsters under control, the game camera */
    int show_hits;                                   /* H draws the attack spheres */
    /* 0: monster 0's own shake (monster +0x7DFC +0x160), 1 .. 7 the effects', 8 .. 10 monsters 1 .. 3's own */
    CamShake shakes[8 + MONSTERS - 1];
    uint32_t shake_seed;
    int last_collapse[64];                           /* per building: its collapse when last looked at */
    Parade *parade;                                  /* the balloons, bikers and food carts (parade.c) */
    struct { int fx, obj; float off[3]; } pfx[32]; int npfx;   /* the parade's effects that follow their object */
    Vehicles *veh;                                   /* the traffic, the fire truck and the rockets (vehicle.c) */
    Rail *rail;                                      /* the trains, cable cars and freighters (rail.c) */
    struct { int id, fx, obj; float off[3]; } rfx[64]; int nrfx;   /* their effects (following them) */
    struct { int id, h, obj; } rsnd[64]; int nrsnd;                /* and sounds */
    Edibles *edibles;                                /* the power-ups and hazards behind the windows (edible.c) */
    Game *game;                                      /* the world's states, the lives, the clock (game.c) */
    Boss *boss;                                      /* the block's boss and the ambient ones (boss.c) */
    /* the bosses' effects that follow (monster: the monster slot followed, -1 none) */
    struct { int id, fx, obj, monster; float off[3]; } bfx[64]; int nbfx;
    int game_block;                                  /* a block the game moved on to, to build after the frame; -1 */
    int game_built;                                  /* this build is the game's: keep the game going */
    struct { int fx, edible; float off[3]; } efx[32]; int nefx;   /* the edibles' effects that follow them */
    struct { int id, fx, obj; float off[3]; } vfx[64]; int nvfx;   /* the vehicles' effects that follow them */
    StreetObjs *sobj;                                /* the street and roof objects (streetobj.c) */
    int sobj_of[SCENE_STREET_MAX];                   /* per Scene.street entry its streetobj index, -1 */
    int street_of[SCENE_STREET_MAX];                 /* per streetobj index its Scene.street entry */
    /* their effects that follow an object (obj >= 0: at its point off, or its model node) or monster mon's node */
    struct { int id, fx, obj, node_id, mon; const char *node; char mnode[32]; float off[3]; } sofx[64]; int nsofx;
    Army *army;
    struct { int id, fx, unit, shot; float off[3]; } fxa[128]; int nfxa;   /* the army's effects that follow */
    int armed_people[32]; int narmed_people;         /* people firing (army_person_moved) */
    Bundle fx_bundle; int have_fx_bundle;            /* fx.bdp: the effects (fx.c) */
    FxLib *fxlib; FxWorld *fxw;
    ShadowList shl; ShadowGround sg; ShadowLighting slight;   /* the shadows (shadow.c) */
    int boss_block;                                  /* the block has a boss (the HUD's boss bar) */
    CamDirector cd; CamView view;                    /* the camera modes and their blend; the camera shown */
    /* the game's next block, activated at the results: the scene keeps the block left (the next one stands 41 behind
     * it, slot 1) until the walk is over and the next block begins; meanwhile the game's places, which are the next
     * block's, are block_shift (41) further on in y here */
    int next_block; float block_shift;
    int rubble_fx[3];                                /* ResultsNI's three effects in the rubble (fx ids, -1) */
    int unattended;                                  /* --shot: the results screens are answered for the player */
    SceneMesh fx_shapes[32][8]; int fx_nshapes[32]; /* the effects' debris models (fx.bdp's .pwk), by shape */
    Bundle gui_world, gui_common; int have_gui;      /* WorldGui.bdp, CommonGUI.bdp: the HUD (hud.c) */
    Hud *hud; int no_hud;                            /* player 1's HUD, which has the screens; --nohud: none */
    Camera hud_cam;                                  /* the frame's camera, for the floating scores */
    Challenge challenge;                             /* the block's challenge, its goals counted (challenge.c) */
    uint32_t upgrades[64];                           /* the moves each monster took from upgrades (the profile's
                                                      * getMonsterFlags; a new profile: none [G]) */
    int challenges_lost;                             /* a block of this city was won with its challenge not met */
    int upgrade_fx;                                  /* the MonsterUpgrade object's effect, -1 */
    int exit_game;                                   /* the game went back to the shell (the Pause menu's Quit) */
    float sfx_vol, music_vol, music_now;             /* the Options page's volumes, 1 = as the game; the music now */
    Reveal reveal; RevealRoster roster;              /* the cryotube's monster and the unlocks (reveal.c): the
                                                      * roster's flags stand for the profile's, from 0 */
    RevealLoc tubes[REVEAL_LOCS]; int ntubes;        /* the tubes of a block activated but not built yet */
    int extra;                                       /* the slot after the players that holds the monster to reveal
                                                      * (0x2B6BE0: players + 1 monsters, the last hidden), -1 none */
    RevealWalk rwalk;                                /* its walk-on (Monsters::onRevealNITick) */
    Rubble rubble;                                   /* findPointInRubble's slots: the block left's buildings */
    Profile prof;                                    /* the player's profile (profile.c): unlocks, moves, settings */
    int prof_on_card;                                /* its file exists (the shell's profile list) */
    Shell *shell; Scene *shell_scene;                /* the front end while it shows (shell.c), its empty scene */
    struct GuiDraw *gd;                              /* its 3D objects: models, effects, this frame's layers */
    char elf_path[512];                              /* RAMPAGE.ELF beside the VOL (fx.c's words) */
    Movie *movie;                                    /* the movie the shell plays (movie.c), NULL none */
    int shell_mode, shell_go;                        /* --shell: a game's Quit goes back to it; a game to start */
    ShellGame last_game;                             /* the shell's last choices (back from a game) */
    const char *prof_path; int prof_write;           /* its file; written at the Results (not by --shot runs) */
    int play_type;                                   /* --play: GP_CAMPAIGN (default), King of the City / World, Timed Run */
    CollideWorld cw;                                 /* the frame's collision world (collide.c): built by world_build
                                                      * before anything moves, asked by the monsters and the modules */
    float mid_x;                                     /* the middle of the players' x this frame (0x336360) */
    uint32_t bot_seed;                               /* the bots' rand01 */
} Viewer;

/* the monster's instance in the scene, NULL none */
static Instance *mon_in(const Mon *mo)
{
    const Scene *s = &mo->v->scene;
    int i = mo->slot < s->nmon ? s->mon[mo->slot].inst : -1;
    return i >= 0 ? &mo->v->scene.instances[i] : NULL;
}

/* a monster is in play: there is one at all (slot 0's), and it is in the scene */
static int have_monster(const Viewer *v)
{
    return v->nmon > 0 && v->mon[0].monster >= 0;
}

/* another monster rages or has its energy full (0x2E5730: then a full energy is 99.9, the rage one at a time) */
static int others_full(const Mon *mo)
{
    for (int k = 0; k < mo->v->nmon; k++)
        if (k != mo->slot && (mo->v->mon[k].power.rage || mo->v->mon[k].power.energy >= 100))
            return 1;
    return 0;
}

/* the monster's own camera shake (slot 0 first, then after the effects') */
static CamShake *mon_shake(Mon *mo)
{
    return &mo->v->shakes[mo->slot == 0 ? 0 : 7 + mo->slot];
}

static void monster_hit(Mon *mo, const char *name, float amount, int level, const float knock[3], const float from[3]);
static int water_at(const Viewer *v, float x, float y);
static void challenge_stats(Viewer *v, const char *name, int points);
static void power_from_profile(Mon *mo);
static void upgrade_touch(Mon *mo, const float centre[3], float radius);
static void street_step(Viewer *v, float dt);
static float water_surface(const Water *w);
static float ground_at(const Viewer *v, float x, float y, float z);

static void hud_frame(Viewer *v, int after);
static void rage_started(Mon *mo);

/* seconds into the monster's clip at the state's time: the game runs a clip on a 0 .. 1 clock that the state's rate
 * (node +0x0, 1 / its length) moves (0x2FFF10), so the state's time x rate x the clip's duration */
static double clip_at(Mon *mo, const MoveState *st, double t)
{
    const Instance *in = mon_in(mo);
    return t * st->rate * (in && in->has_clip ? in->clip.duration : 1);
}

/* the clip's root at time t (seconds of the clip, clip_at), held at the clip's last frame (clip_pose loops: its end
 * is its start) */
static int root_at(Mon *mo, double t, float tr[3], float q[4])
{
    Viewer *v = mo->v;
    const Instance *in = mon_in(mo);
    if (in && in->has_clip && t > in->clip.duration - 1e-4)
        t = in->clip.duration - 1e-4;
    return scene_monster_root(&v->scene, mo->slot, t < 0 ? 0 : t, tr, q);
}

/* a root's place and rotation (rows of 3) in the monster's frame as drawn: mirrored, across x (S R S) */
static void seen_root(int mirrored, float p[3], const Mat4 *m, float r[3][3])
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i][j] = m->m[4 * j + i] * (mirrored && (i == 0) != (j == 0) ? -1.0f : 1.0f);
    if (mirrored)
        p[0] = -p[0];
}

/* the root's turn about Z from clip time ta to tb, in degrees: the yaw of its rotation at tb against the one at ta
 * (D = M(tb) x M(ta)^T, so a root that leans or crouches does not seem to turn) */
static float root_turn(Mon *mo, double ta, double tb)
{
    float tr[3], qa[4], qb[4], zero[3] = { 0, 0, 0 };
    if (root_at(mo, ta, tr, qa) != 0 || root_at(mo, tb, tr, qb) != 0)
        return 0;
    Mat4 a = mat_tq(zero, qa), b = mat_tq(zero, qb);
    float d00 = 0, d10 = 0;
    for (int k = 0; k < 3; k++) {
        d00 += b.m[4 * k] * a.m[4 * k];
        d10 += b.m[4 * k + 1] * a.m[4 * k];
    }
    return atan2f(d10, d00) * 57.2957795f;
}

static float root_yaw(Mon *mo, double t)
{
    float tr[3], q[4];
    if (root_at(mo, t, tr, q) != 0)
        return 0;
    float zero[3] = { 0, 0, 0 };
    Mat4 m = mat_tq(zero, q);
    return atan2f(m.m[4 * mo->yaw_axis + 1], m.m[4 * mo->yaw_axis]) * 57.2957795f;
}

/* the monster plays the clip of the runner's state, from the runner's time */
static void play_state(Mon *mo)
{
    Viewer *v = mo->v;
    const MoveState *st = &mo->graph.states[mo->run.state];
    if (scene_monster_play(&v->scene, mo->slot, st->clip) != 0)
        printf("%s: no clip %s\n", st->name ? st->name : "?", st->clip ? st->clip : "-");
    mo->entry_yaw = root_yaw(mo, clip_at(mo, st, mo->run.time));
    mo->entry_t = clip_at(mo, st, mo->run.time);
    if (getenv("RAMPAGE_ROOT")) {
        float rt[3], rq[4];
        if (root_at(mo, clip_at(mo, st, mo->run.time), rt, rq) == 0)
            printf("  root of %s: %.2f %.2f %.2f\n", st->clip, rt[0], rt[1], rt[2]);
    }
    mo->armed = 0;                                    /* 0x2E7F70: a clip starts with no attack armed, on the ground */
    mo->mode = MODE_GROUND;                           /* (its events set the mode it is in) */
    climb_clip_started(&mo->mount, st->name);
    mo->mirror = mo->mirror_queued;                    /* 0x2E67E8: a clip starts in the queued mirror state */
    mo->input.mirrored = mo->mirror;
    if (mon_in(mo))
        mon_in(mo)->mirrored = mo->mirror;
    float q[4];
    if (root_at(mo, clip_at(mo, st, mo->run.time), mo->entry_root, q) != 0)
        memset(mo->entry_root, 0, sizeof mo->entry_root);
}

/* setFacialAnim (0x2E9200, 0x2E9350): the state node of that name plays its clip on the face, from its start; a name
 * no node has does nothing */
static void face_start(Mon *mo)
{
    Viewer *v = mo->v;
    const FaceRec *f = &mo->faces.list[0];
    const MoveNode *n = f->node;
    int r = scene_monster_face(&v->scene, mo->slot, n->clip, f->head);
    if (getenv("RAMPAGE_TRACE"))
        printf("face %s: %s -> %d\n", f->name, n->clip, r);
    if (r != 0)
        return;
    mo->face = n;
    scene_monster_face_blend(&v->scene, mo->slot, n->blend);   /* 0x2FF910: from the face as it was, over the node's blend */
}

static void face_play(Mon *mo, const char *name, int head, int loop, int count)
{
    const MoveNode *n = moves_node(&mo->graph, name);
    if (n && n->clip && face_set(&mo->faces, n, name, head, loop, count) == FACE_START)
        face_start(mo);
}

/* Monsters.bsf's DefaultFacialAnim: Expression_Idle, looping, the head itself left to the body */
static void face_default(Mon *mo)
{
    face_play(mo, "Expression_Idle", 0, 1, 0);
}

/* the face clip's clock: 0 .. 1 at its node's rate; at its end (0x2E95D0) it plays again while it loops or has
 * plays left, else the next queued face, else the default face (look.c); the delayed belches (appendBelch) */
static void face_step(Mon *mo, float dt)
{
    Viewer *v = mo->v;
    if (!mo->face || !mon_in(mo))
        return;
    if (face_tick(&mo->faces, dt))
        face_play(mo, "Expression_Burp", 0, 0, 1);
    Instance *in = mon_in(mo);
    in->face_t += mo->face->rate * dt;
    if (in->face_t < 1)
        return;
    switch (face_clip_end(&mo->faces)) {
    case FACE_RESTART:
        in->face_t = 0;
        scene_monster_face_blend(&v->scene, mo->slot, mo->face->blend);
        break;
    case FACE_START: face_start(mo); break;
    default: face_default(mo); break;
    }
}

static void start_moves(Mon *mo)
{
    Viewer *v = mo->v;
    if (mo->have_graph) {
        moves_stop(&mo->run);
        moves_free(&mo->graph);
        mo->have_graph = 0;
    }
    if (!mon_in(mo) || !v->scene.mon[mo->slot].bundle || moves_read(v->scene.mon[mo->slot].bundle, &mo->graph) != 0)
        return;
    mo->have_graph = 1;
    {                                                /* the property book's footfalls (MonsterFx), DisableFootDrag */
        char book[64];
        snprintf(book, sizeof book, "%s", mo->graph.props ? mo->graph.props : "");
        char *dot = strrchr(book, '.');
        if (dot)
            *dot = 0;
        foot_props_default(&mo->feet_fx);
        Group g = book[0] ? city_book_group(v->city, book, "MonsterFx") : (Group){ 0 };
        foot_props_read(&mo->feet_fx, g);
        g = book[0] ? city_book_group(v->city, book, "MiscData") : (Group){ 0 };
        foot_init(&mo->feet, g.at && group_num(g, "DisableFootDrag", 0) != 0);
        if (mon_in(mo))                   /* Use3LinkFootIK, RTWSmoothFactor, the ankle (0x2E5640) */
            foot_ik_props(&mo->feet, g, &mon_in(mo)->tree);
        memset(&mo->feet_ik, 0, sizeof mo->feet_ik);
        mo->feet_ik.links = mo->feet.links;
    }
    if (mo->have_ag)
        dir_graph_free(&mo->ag);
    mo->have_ag = dir_graph_read(v->scene.mon[mo->slot].bundle, &mo->ag) == 0;
    dir_ai_init(&mo->ai);
    mo->is_ai = 0;
    moves_start(&mo->run, &mo->graph, moves_find(&mo->graph, "Ground_Idle"));
    mo->face = NULL;
    face_init(&mo->faces);
    memset(&mo->head, 0, sizeof mo->head);
    look_limits_default(&mo->look_lim);
    mo->friction = MONSTER_FRICTION;
    face_default(mo);                                 /* Monsters::PostInit */
    memset(&mo->input, 0, sizeof mo->input);
    memset(mo->vel, 0, sizeof mo->vel);
    mo->mirror = mo->mirror_queued = 0;
    memset(&mo->mount, 0, sizeof mo->mount);
    memset(mo->hand, 0, sizeof mo->hand);
    mo->held = -1;
    mo->held_unit = -1;
    mo->held_vehicle = -1;
    mo->held_rail = -1;
    mo->held_parade = -1;
    mo->pending_slot = -1;
    mo->defeated = 0;
    mo->scale = mo->scale_to = 1;
    mo->scale_left = 0;
    mo->hp = 200;
    if (getenv("RAMPAGE_HP"))                        /* a test's start (the game starts at MONSTER_DATA +0x138) */
        mo->hp = (float)atof(getenv("RAMPAGE_HP"));
    mo->energy = 0;
    edible_power_init(&mo->power);
    power_from_profile(mo);
    memset(mon_shake(mo), 0, sizeof *mon_shake(mo));
    mo->mount.at.building = mo->mount.cand.building = -1;
    memset(mo->air, 0, sizeof mo->air);
    memset(mo->stick, 0, sizeof mo->stick);
    memcpy(mo->frame, v->view.rows, sizeof mo->frame);
    mo->mz = 0;
    mo->mode = MODE_GROUND;
    mo->no_gravity = 0;
    mo->column = -1;
    const Instance *in = mon_in(mo);
    mo->yaw_axis = 0;                                 /* the most level axis of the root at rest */
    for (uint32_t i = 0; i < in->tree.n; i++)
        if (in->tree.nodes[i].parent < 0) {
            float zero[3] = { 0, 0, 0 };
            Mat4 m = mat_tq(zero, in->tree.nodes[i].q);
            for (int k = 1; k < 3; k++)
                if (fabsf(m.m[4 * k + 2]) < fabsf(m.m[4 * mo->yaw_axis + 2]))
                    mo->yaw_axis = k;
            break;
        }
    if (v->play)                                     /* with --clip the chosen clip stays until G */
        play_state(mo);
}

/* the world's side of a monster's start (with player 1's): the game's next block, the upgrade, the shakes */
static void play_reset(Viewer *v)
{
    v->game_block = -1;
    v->upgrade_fx = -1;
    if (!v->game_built) {
        v->next_block = -1;
        v->block_shift = 0;
        for (int k = 0; k < 3; k++)
            v->rubble_fx[k] = -1;
    }
    memset(v->shakes, 0, sizeof v->shakes);
    for (int i = 0; i < 64; i++) v->last_collapse[i] = -1;
}

/* Monster.isFemale (script method 0x96, 0x2AC0E0): the monster's +0xAB6, which its setup (0x2E98C0) sets from
 * MiscData|IsFemale of its property book (MONSTER_DATA +0xAA4): Amanda, Jill, Lizzie, Natalie, Snapper, Venus */
static int monster_female(Mon *mo)
{
    Viewer *v = mo->v;
    char book[64];                                   /* "George_prop.prb": the book George_prop(.bdp) */
    snprintf(book, sizeof book, "%s", mo->have_graph && mo->graph.props ? mo->graph.props : "");
    char *dot = strrchr(book, '.');
    if (dot)
        *dot = 0;
    Group g = book[0] ? city_book_group(v->city, book, "MiscData") : (Group){ 0 };
    return g.at && group_num(g, "IsFemale", 0) != 0;
}

/* gWorld.isSphereVisible for people.c: the sphere inside the game camera's view (its fov across the 640 x 480 GUI
 * screen's height, 4:3 across) */
static int people_visible(void *ctx, const float p[3], float radius)
{
    const Viewer *v = ctx;
    const float (*r)[3] = (const float (*)[3])v->view.rows;
    float d[3] = { p[0] - v->view.pos[0], p[1] - v->view.pos[1], p[2] - v->view.pos[2] };
    float x = d[0] * r[0][0] + d[1] * r[0][1] + d[2] * r[0][2], y = d[0] * r[2][0] + d[1] * r[2][1] + d[2] * r[2][2];
    float z = d[0] * r[1][0] + d[1] * r[1][1] + d[2] * r[1][2];
    if (z < -radius)
        return 0;
    float t = tanf((v->view.fov > 0 ? v->view.fov : 1) / 2), zz = z > 0.1f ? z : 0.1f;
    return fabsf(x) - radius <= zz * t * 4 / 3 && fabsf(y) - radius <= zz * t;
}

static int edible_cell_state(void *ctx, int building, int storey, int column);
static int edible_cell_where(void *ctx, int building, int storey, int column, float out[3]);
static int edible_call(Mon *mo, const char *call, const float *xyz);
static void monster_stats(Mon *mo, float health, float energy);
static int boss_weapon_make(void *ctx, ArmyWeapon *w, Group g);
static void boss_weapon_tick(void *ctx, ArmyWeapon *w, int may, const float from[3], const float dir[3], int target,
                             float dt);

/* the army's host (army.h): books, clip lengths and model bounds from the city */
static Group rubble_tmpl(void *ctx, const char *name)
{
    return city_template(((Viewer *)ctx)->city, name);
}
static int rubble_box(void *ctx, const char *shape, float min[3], float max[3])
{
    return city_shape_box(((Viewer *)ctx)->city, shape, min, max) == 0 ? 0 : -1;
}
/* the city's WorldData (<City>_Props): its MonsterRevealDef */
static Group world_data(Viewer *v)
{
    char book[64];
    snprintf(book, sizeof book, "%s_Props", CITIES[v->city_index]);
    return city_book_group(v->city, book, "WorldData");
}
static int game_all_revealed(void *ctx)
{
    Viewer *v = ctx;
    return reveal_all_revealed(world_data(v), &v->roster);
}
/* the cryotubes of the block: the edibles get them in their cells */
static void add_tubes(Viewer *v)
{
    for (int i = 0; v->edibles && i < v->ntubes; i++)
        if (v->tubes[i].building >= 0 && v->tubes[i].building < (int)v->scene.nbuildings) {
            int s = edible_add_cryotube(v->edibles, v->tubes[i].building, v->tubes[i].floor, v->tubes[i].cell);
            if (getenv("RAMPAGE_TRACE"))
                printf("  cryotube: building %d (%d storeys, %d wide), floor %d, cell %d: %s\n", v->tubes[i].building,
                       v->scene.buildings[v->tubes[i].building].b.n, v->scene.buildings[v->tubes[i].building].b.width,
                       v->tubes[i].floor, v->tubes[i].cell, s >= 0 ? "placed" : "no such cell");
        }
    v->ntubes = 0;
}

static Group army_find(void *ctx, const char *book, const char *name)
{
    return city_book_group(((Viewer *)ctx)->city, book, name);
}
static int army_shape_box(void *ctx, const char *shape, float min[3], float max[3])
{
    return city_shape_box(((Viewer *)ctx)->city, shape, min, max) == 0;
}
static int army_node_world(void *ctx, const ArmyUnit *u, const char *node, float out[16])
{
    Viewer *v = ctx;                                 /* the weapon's node on the unit's look as drawn */
    int ok = v->army && scene_unit_node(&v->scene, (int)(u - army_unit(v->army, 0)), node, out) == 0;
    if (getenv("RAMPAGE_NODES"))
        printf("  %s node %s: %s\n", u->name ? u->name : "?", node, ok ? "found" : "not found");
    return ok;
}
static float parade_clip_seconds(void *ctx, const char *anim, const char *shape)
{
    return city_clip_seconds(((Viewer *)ctx)->city, anim, shape);
}
static float army_clip_seconds(void *ctx, const char *anim, const char *shape)
{
    (void)ctx; (void)anim; (void)shape;
    return 0;                                        /* army.c falls back to the clip's own length */
}

/* a world point on the GUI's 640 x 480 screen through the frame's camera (render_gl.c's look and perspective; the GUI
 * at the window's height, centred): the floating scores */
static int hud_project(void *ctx, const float p[3], float screen[2], float *px_per_unit)
{
    const Viewer *v = ctx;
    const Camera *c = &v->hud_cam;
    float cp = cosf(c->pitch), f[3] = { cp * cosf(c->yaw), cp * sinf(c->yaw), sinf(c->pitch) };
    float s[3] = { f[1], -f[0], 0 }, sl = sqrtf(s[0] * s[0] + s[1] * s[1]);
    s[0] /= sl; s[1] /= sl;
    float u[3] = { s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0] };
    if (c->use_rows)
        for (int k = 0; k < 3; k++) { s[k] = c->rows[0][k]; f[k] = c->rows[1][k]; u[k] = c->rows[2][k]; }
    float d[3] = { p[0] - c->pos[0], p[1] - c->pos[1], p[2] - c->pos[2] };
    float x = d[0] * s[0] + d[1] * s[1] + d[2] * s[2], y = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
    float z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    if (z < 0.5f)
        return 0;
    float k = 1.0f / tanf(c->fov_deg * 3.14159265f / 360.0f) / z;
    screen[0] = HUD_SCREEN_W / 2 + HUD_SCREEN_H / 2 * x * k;
    screen[1] = HUD_SCREEN_H / 2 * (1 - y * k);
    *px_per_unit = HUD_SCREEN_H / 2 * k;
    return 1;
}

/* the players' HUDs closed (player 1's is the screen's, Viewer.hud) */
static void huds_close(Viewer *v)
{
    for (int k = 0; k < MONSTERS; k++) {
        if (v->mon[k].hud != v->hud)
            hud_close(v->mon[k].hud);
        v->mon[k].hud = NULL;
    }
    hud_close(v->hud);
    v->hud = NULL;
}

static int monster_index(const Viewer *v, const char *name);

/* 0x2B6BE0 / 0x2B72B0: the slot after the players gets the monster to reveal (roster index rm), hidden and out of
 * the play (+0x108 = +0x109 = 1) until startReveal */
static void extra_add(Viewer *v, int k, int rm)
{
    int mi = monster_index(v, v->roster.name[rm]);
    if (k != v->nmon || k >= MONSTERS || mi < 0 || v->scene.nmon != k)
        return;
    Mon *mo = &v->mon[k];
    mo->v = v;
    mo->slot = k;
    mo->monster = mi;
    mo->bot = 0;
    mo->drive = DRIVE_COMPUTER;
    mo->hud = NULL;
    mo->mx = 0; mo->my = -10; mo->mz = 0;
    mo->heading = 180;
    memset(&mo->pad, 0, sizeof mo->pad);
    mo->human = mo->dropping = 0;
    mo->streaker = -1;
    mo->reveal = 1;
    mo->shown = 0;
    if (city_monster(v->city, &v->scene, v->monsters[mi], -1, mo->mx, mo->my, mo->heading) != k)
        return;
    start_moves(mo);
    v->extra = k;
    if (mon_in(mo))
        mon_in(mo)->hidden = 1;
}

/* 0x2D3F90: the extra monster hides and leaves the world (stopReveal, the Continue) */
static void extra_hide(Viewer *v, int k)
{
    if (k < 0 || k != v->extra)
        return;
    Mon *mo = &v->mon[k];
    mo->shown = 0;
    mo->is_ai = 0;
    dir_ai_clear(&mo->ai);
    mo->scale = mo->scale_to = 1;
    mo->scale_left = 0;
    edible_power_init(&mo->power);
    if (mon_in(mo))
        mon_in(mo)->hidden = 1;
    if (getenv("RAMPAGE_TRACE"))
        printf("  reveal: [%d] hidden\n", k + 1);
}

/* the monsters in the world: the players and bots, and the one being revealed while it shows */
static int mon_world(const Viewer *v)
{
    return v->nmon + (v->extra >= 0 && v->mon[v->extra].shown);
}

/* the walk-on's calls (reveal.c's RevealEvents) on the extra monster */
static void reveal_events(Viewer *v)
{
    RevealEvent ev[16];
    int n = reveal_walk_events(&v->rwalk, ev, 16);
    for (int i = 0; i < n; i++) {
        const RevealEvent *e = &ev[i];
        int k = e->type == RW_HIDE ? e->slot : v->rwalk.slot;
        if (k < 0 || k != v->extra)
            continue;
        Mon *mo = &v->mon[k];
        switch (e->type) {
        case RW_SHOW:                                /* at the tube, facing pi (setFacingZ), into the world */
            mo->mx = e->pos[0]; mo->my = e->pos[1]; mo->mz = e->pos[2];
            mo->heading = e->v[0] * 57.2957795f;
            mo->mode = MODE_GROUND;
            memset(mo->vel, 0, sizeof mo->vel);
            mo->shown = 1;
            if (mon_in(mo))
                mon_in(mo)->hidden = 0;
            scene_monster_place(&v->scene, k, mo->mx, mo->my, mo->mz, mo->heading);
            break;
        case RW_AI: mo->is_ai = 1; break;
        case RW_MOVE:
            if (mo->have_graph && mo->have_ag)
                dir_ai_play_move(&mo->ai, e->name ? moves_find(&mo->graph, e->name) : -1, 0);
            break;
        case RW_SCALE:                               /* setDynamicScale(scale, seconds) */
            mo->scale_from = mo->scale;
            mo->scale_to = e->v[0] > 0 ? e->v[0] : 1;
            mo->scale_time = mo->scale_left = e->v[1] > 0 ? e->v[1] : 0;
            if (mo->scale_left <= 0)
                mo->scale = mo->scale_to;
            break;
        case RW_WALK: {                              /* walkTo(pos, radius, "", "Monsters::onRevealNIEnd") */
            float to[3] = { e->pos[0], e->pos[1] + v->block_shift, e->pos[2] };
            if (mo->have_ag)
                dir_ai_walk_to(&mo->ai, to, e->v[0], 1);
            break;
        }
        case RW_SPEED: edible_power_speed(&mo->power, e->v[0], e->v[1]); break;
        case RW_HIDE: extra_hide(v, k); break;
        }
        if (getenv("RAMPAGE_TRACE"))
            printf("  reveal walk %d (%s) [%d] at %.1f %.1f %.1f\n", e->type, e->name ? e->name : "-", k + 1,
                   e->pos[0], e->pos[1], e->pos[2]);
    }
}

static void build(Viewer *v)
{
    if (v->have_scene)
        scene_free(&v->scene);
    v->have_scene = city_build(v->city, v->blocks[v->block], v->night, &v->scene) == 0;
    if (!v->game_built) {                            /* a new world: its monster to reveal comes with reveal_load */
        v->extra = -1;
        memset(&v->rwalk, 0, sizeof v->rwalk);
    }
    for (int k = 0; k < v->nmon + (v->extra >= 0); k++) {   /* the monsters, slot by slot (player 1's: none, none at
                                                      * all), and the one to reveal */
        Mon *mo = &v->mon[k];
        if (v->have_scene && have_monster(v) && mo->monster >= 0 &&
            city_monster(v->city, &v->scene, v->monsters[mo->monster], k ? -1 : v->clip, mo->mx, mo->my,
                         mo->heading) == k)
            printf("%s: %s\n", v->monsters[mo->monster], v->scene.mon[k].clip ? v->scene.mon[k].clip : "-");
        {                                            /* the game's next block: its goals and AI go on */
            DirAI ai = mo->ai;
            int is_ai = mo->is_ai;
            start_moves(mo);
            if (v->game_built) {
                mo->ai = ai;
                mo->is_ai = is_ai;
                mo->ai.bot.target = mo->ai.bot.avoid = -1;   /* (the buildings are the new block's) */
                mo->ai.bot.have_cell = mo->ai.bot.use_hit = 0;
            }
        }
        if (getenv("RAMPAGE_TRACE") && mo->have_graph)
            printf("properties %s%s\n", mo->graph.props ? mo->graph.props : "-", monster_female(mo) ? ", female" : "");
    }
    if (v->extra >= 0 && !v->mon[v->extra].shown && mon_in(&v->mon[v->extra]))
        mon_in(&v->mon[v->extra])->hidden = 1;
    if (v->nmon && v->mon[0].have_graph)
        play_reset(v);
    if (v->have_scene && v->scene.music)            /* the block's music, looping (sound.c; silent unless started) */
        music_play(v->scene.music);
    if (v->army) {
        army_free(v->army);
        v->army = NULL;
    }
    v->nfxa = 0;
    v->narmed_people = 0;
    if (v->have_scene && have_monster(v)) {          /* the army comes for the monsters (the block's AIDef) */
        static ArmyHost host;
        host.find = army_find;
        host.clip_seconds = army_clip_seconds;
        host.shape_box = army_shape_box;
        host.node_world = army_node_world;
        host.ctx = v;
        v->army = army_new(&host);
        if (v->army)
            army_set_collide(v->army, &v->cw);
        if (v->army) {
            army_load_block(v->army, v->scene.block);
            army_activate(v->army, 1);
        }
    }
    v->scene.army = v->army;
    vehicle_free(v->veh);                            /* the traffic of the three block slots, the roof rockets */
    v->veh = NULL;
    v->nvfx = 0;
    for (int k = 0; k < v->nmon; k++)
        v->mon[k].held_vehicle = v->mon[k].held_rail = -1;
    if (v->have_scene) {
        static VehicleHost vh;
        vh.find = army_find;
        vh.shape_box = army_shape_box;
        vh.ctx = v;
        v->veh = vehicle_new(&vh);
        if (v->veh)
            vehicle_set_collide(v->veh, &v->cw);
        if (v->veh) {
            if (v->army)
                vehicle_bind_army(v->veh, v->army);
            for (int k = 0; k < 3; k++)
                if (v->scene.traffic_def[k].at)
                    vehicle_load_traffic(v->veh, k, v->scene.traffic_def[k]);
            vehicle_level_start(v->veh);
            for (int k = 0; k < v->scene.nrockets; k++)
                vehicle_spawn(v->veh, v->scene.rockets[k].name, v->scene.rockets[k].pos, v->scene.rockets[k].facing,
                              v->scene.rockets[k].building);
        }
    }
    v->scene.traffic = v->veh;
    streetobj_free(v->sobj);                         /* the block's StreetObject.bsf and RoofItem.bsf objects */
    v->sobj = NULL;
    v->nsofx = 0;
    if (v->have_scene) {
        static StreetHost sh;
        sh.shape_box = army_shape_box;
        sh.ctx = v;
        v->sobj = streetobj_new(&sh);
        static StreetBuilding sbs[64];
        int nb = 0;
        for (uint32_t i = 0; i < v->scene.nbuildings && nb < 64; i++, nb++) {
            memcpy(sbs[nb].min, v->scene.buildings[i].b.min, sizeof sbs[nb].min);
            memcpy(sbs[nb].max, v->scene.buildings[i].b.max, sizeof sbs[nb].max);
            sbs[nb].falling = v->scene.buildings[i].b.collapse >= 0;
        }
        for (int k = 0; k < v->scene.nstreet; k++) {
            const struct SceneStreetObj *so = &v->scene.street[k];
            v->sobj_of[k] = v->sobj ? streetobj_add(v->sobj, so->t, so->inst, so->world, so->building) : -1;
            if (v->sobj_of[k] >= 0)
                v->street_of[v->sobj_of[k]] = k;
        }
        if (v->sobj)
            streetobj_start(v->sobj, sbs, nb);
        if (getenv("RAMPAGE_TRACE"))
            printf("street objects: %d\n", v->sobj ? streetobj_count(v->sobj) : 0);
    }
    parade_free(v->parade);                          /* the block's balloons, bikers and food carts */
    v->parade = NULL;
    for (int k = 0; k < v->nmon; k++)
        v->mon[k].held_parade = -1;
    v->npfx = 0;
    if (v->have_scene) {
        static ParadeHost ph;
        ph.find = army_find;
        ph.shape_box = army_shape_box;
        ph.clip_seconds = parade_clip_seconds;
        ph.ctx = v;
        v->parade = parade_new(&ph);
        if (v->parade)
            parade_set_collide(v->parade, &v->cw);
        if (v->parade) {
            int n = parade_load_block(v->parade, v->scene.block);
            if (getenv("RAMPAGE_TRACE"))
                printf("parade: %d objects\n", n);
            parade_activate(v->parade, !v->game);    /* the game's BeginBlockNI activates it (GE_AI_ACTIVATE) */
        }
    }
    v->scene.parade = v->parade;
    rail_free(v->rail);                              /* the trains, cable cars and freighters (rail.c) */
    v->rail = NULL;
    v->nrfx = v->nrsnd = 0;
    if (v->have_scene) {
        static RailHost rh;
        rh.find = army_find;
        rh.shape_box = army_shape_box;
        rh.ctx = v;
        v->rail = rail_new(&rh);
        if (v->rail)
            rail_set_collide(v->rail, &v->cw);
        if (v->rail) {
            if (v->army)
                rail_bind_army(v->rail, v->army);
            rail_load_block(v->rail, v->scene.block);
            rail_activate(v->rail, 1);
            static const char *const OBJ[] = { "ObjectInstance|", "AttackObjectInstance|", "XObjectInstance|" };
            for (int k = 0; k < 3; k++) {
                Group bd = k == 0 ? v->scene.block : v->scene.traffic_def[k];
                if (!bd.at)
                    continue;
                for (int j = 0; j < 3; j++) {
                    Group where;
                    Record rr[256];
                    uint32_t n = group_values(bd, OBJ[j], &where, rr, 256);
                    for (uint32_t i = 0; i < n; i++) {
                        Group o = record_group(where, rr[i]);
                        if (k && group_num(o, k == 1 ? "LoadWhenWaiting" : "LoadWhenDistant", 0) == 0)
                            continue;
                        const char *name = group_str(o, "Name");
                        if (!name || rail_kind_of(v->rail, name) < 0)
                            continue;
                        Group loc = group_sub(o, "Location");
                        Record lr[3];
                        uint32_t nl = loc.at ? group_items(loc, lr, 3) : 0;
                        float p[3];
                        for (uint32_t q = 0; q < 3; q++)
                            p[q] = q < nl ? record_num(lr[q]) : 0;
                        p[1] += 41.0f * (float)k;
                        int id = rail_spawn(v->rail, name, p, group_num(o, "ZRotation", 0) / 57.2957795f, o);
                        if (getenv("RAMPAGE_TRACE"))
                            printf("rail: %s at %.1f %.1f %.1f -> %d\n", name, p[0], p[1], p[2], id);
                    }
                }
            }
        }
    }
    v->scene.rail = v->rail;
    edible_free(v->edibles);                         /* the edibles behind the block's windows */
    v->edibles = NULL;
    v->nefx = 0;
    if (v->have_scene) {
        static EdibleHost eh;
        eh.find = army_find;
        eh.cell_state = edible_cell_state;
        eh.cell_place = edible_cell_where;
        eh.ctx = v;
        v->edibles = edible_new(&eh);
        if (v->edibles) {
            edible_load_block(v->edibles, v->scene.block);
            for (uint32_t i = 0; i < v->scene.nbuildings; i++) {
                const SceneBuilding *sb = &v->scene.buildings[i];
                edible_add_building(v->edibles, (int)i, sb->inst, sb->props, sb->b.width, sb->b.n);
            }
            add_tubes(v);                            /* a reveal's tubes, for the block now built */
            if (getenv("RAMPAGE_TRACE"))
                printf("edibles: %d kinds, %d slots\n", edible_kinds(v->edibles), edible_slots(v->edibles));
        }
    }
    v->scene.edibles = v->edibles;
    {                                                /* the buildings' shadows on the lot, cast at the block's start */
        static float boxes[SHADOW_MAX_BUILDINGS][6];
        int nb = 0;
        for (uint32_t i = 0; i < v->scene.nbuildings && nb < SHADOW_MAX_BUILDINGS; i++, nb++) {
            memcpy(boxes[nb], v->scene.buildings[i].b.min, 3 * sizeof(float));
            memcpy(boxes[nb] + 3, v->scene.buildings[i].b.max, 3 * sizeof(float));
        }
        shadow_ground_init(&v->sg, v->have_scene && group_num(v->scene.block, "EnableGroundShadow", 1) != 0, nb,
                           (const float (*)[6])boxes, NULL, NULL);
        for (int k = 0; k < v->nmon; k++)
            shadow_monster_init(&v->mon[k].smon);
    }
    boss_free(v->boss);                              /* the block's boss, and the ambient ones of the blocks behind */
    v->boss = NULL;
    v->nbfx = 0;
    if (v->have_scene && have_monster(v)) {
        static BossHost bh;
        bh.find = army_find;
        bh.clip_seconds = army_clip_seconds;
        bh.shape_box = army_shape_box;
        bh.weapon_make = boss_weapon_make;
        bh.weapon_tick = boss_weapon_tick;
        bh.ctx = v;
        v->boss = boss_new(&bh);
        v->boss_block = 0;
        if (v->boss) {
            int n = boss_load_block(v->boss, v->scene.block);
            v->boss_block = n > 0;
            if (v->scene.traffic_def[1].at)
                n += boss_load_waiting(v->boss, v->scene.traffic_def[1], 41, 1);
            if (v->scene.traffic_def[2].at)
                n += boss_load_waiting(v->boss, v->scene.traffic_def[2], 82, 2);
            boss_activate(v->boss, 1);
            if (getenv("RAMPAGE_TRACE"))
                printf("bosses: %d\n", n);
        }
    }
    v->scene.boss = v->boss;
    for (int k = 0; k < v->nmon; k++)
        v->mon[k].streaker = -1;
    if (!v->game_built) {                            /* the flow of the city from this block on: the play type, the
                                                      * players and the bots */
        game_free(v->game);
        v->game = NULL;
        if (v->have_scene && have_monster(v)) {
            static GameHost gh;
            gh.find = army_find;
            gh.all_revealed = game_all_revealed;
            gh.ctx = v;
            if (v->roster.n == 0) {                  /* ShellData's MonsterData: the 30, the profile's flags */
                reveal_roster_load(&v->roster, city_book_group(v->city, "ShellData", "MonsterData"));
                rubble_seed(&v->rubble, 1);
            }
            int pm[MONSTERS];                        /* 0x3236F0: the city's monster, not one of the players' */
            for (int k = 0; k < v->nmon; k++)
                pm[k] = reveal_roster_find(&v->roster, v->monsters[v->mon[k].monster]);
            for (int i = 0; i < v->roster.n; i++)    /* the roster's flags are the profile's (ShellData's order) */
                v->roster.flags[i] = profile_monster_flags(&v->prof, 0, i);
            int rm = reveal_load(&v->reveal, world_data(v), (v->play_type ? v->play_type : GP_CAMPAIGN) == GP_CAMPAIGN,
                                 v->nmon, pm, &v->roster);
            v->ntubes = 0;
            if (rm >= 0)                             /* with one player: the world's extra monster, hidden */
                extra_add(v, reveal_extra_slot(&v->reveal, v->nmon), rm);
            if (getenv("RAMPAGE_TRACE"))
                printf("reveal: %s%s\n", rm >= 0 ? v->roster.name[rm] : "-", v->extra >= 0 ? " (shown)" : "");
            GameConfig gc = { v->play_type ? v->play_type : GP_CAMPAIGN, v->nmon, v->num_bots,
                              campaign_index(CITIES[v->city_index]), 7,
                              city_block_index(v->city, v->blocks[v->block]), v->prof.current.seed[0],
                              v->prof.cheats };
            v->game = game_new(&gh, &gc);
            dir_ai_seed(v->prof.current.seed[1]);   /* RWorld's reset: gWorld +0x82C */
            v->challenges_lost = 0;                  /* a new city: "All Challenges Met" until one is not */
            if (v->game && game_load_city(v->game, CITIES[v->city_index]) == 0) {
                game_start(v->game);
            } else {
                game_free(v->game);
                v->game = NULL;
            }
        }
    }
    if (v->have_scene && v->scene.folk)
        people_set_visible(v->scene.folk, people_visible, v);
    if (!v->game_built || !v->hud) {                 /* the game's next block keeps the HUDs (their results going) */
        huds_close(v);
        for (int k = 0; v->have_gui && v->have_scene && have_monster(v) && !v->no_hud && k < v->nmon; k++) {
            Mon *mo = &v->mon[k];                    /* each player's HUD (Hud<k>.ifc); player 1's has the screens */
            int pt = v->play_type ? v->play_type : HUD_PLAY_CAMPAIGN;   /* the King modes: the blocks to win */
            HudConfig hc = { k, v->nmon, pt, v->game && (pt == GP_KING_CITY || pt == GP_KING_WORLD) ?
                             game_blocks_to_win(v->game) : 0, v->monsters[mo->monster] };
            mo->hud = hud_open(&v->gui_world, &v->gui_common, &hc);
            if (k == 0)
                v->hud = mo->hud;
            if (mo->hud) {
                hud_set(mo->hud, mo->hp, 200, mo->energy, 100, 0, 3, -1);   /* three lives: the campaign's start [G] */
                hud_set_project(mo->hud, hud_project, v);
                hud_set_settings(mo->hud, profile_sound_volume(&v->prof), profile_music_volume(&v->prof),
                                 profile_rumble(&v->prof));
                hud_add_bundle(mo->hud, v->scene.city);   /* <City>.loc: the block names, the challenge texts */
                hud_results_auto(mo->hud, v->unattended ? 2.0f : 0);
            }
        }
    }
    challenge_init(&v->challenge);                   /* the block's challenge (0x3468F0, at its activation) */
    challenge_upgrade_props(&v->challenge, city_template(v->city, "MonsterUpgrade"));
    if (v->have_scene && have_monster(v))           /* (player 1's monster: its upgrades [G with more players]) */
        challenge_load(&v->challenge, v->scene.block, v->game ? game_play_type(v->game) : GP_CAMPAIGN, v->boss_block,
                       v->monsters[v->mon[0].monster], v->upgrades[v->mon[0].monster & 63]);
    if (v->hud) {                                    /* Levels::onActivateBlock: ChallengeStatus (the count, or the
                                                      * boss bar on the boss block) and ChallengeText's card */
        HudChallenge ch;
        challenge_hud(&v->challenge, v->boss_block, &ch);
        ch.pause_scale = challenge_icon_scale(&v->challenge, 1);
        hud_challenge(v->hud, &ch);
    }
    if (v->game && !v->hud && !v->game_built)        /* no screens to answer the results: after 3 s */
        game_auto_results(v->game, 3.0f);
    Group tt;
    if (city_camera(v->city, &tt) == 0)
        cam_settings(&v->cam_settings, tt);
    if (!v->game_built) {                            /* the game's next block: the cameras go on */
        cam_init(&v->cam, &v->cam_settings);
        dircam_init(&v->cd, &v->cam);
        v->view = v->cd.out;
    }
    if (v->have_scene)
        rnd_scene(v->rnd, &v->scene);
    printf("%s %s%s: %u models drawn, %d people, %d rockets, %d not found\n", CITIES[v->city_index],
           v->blocks[v->block], v->night ? " (night)" : "", v->scene.nitems, v->scene.people, v->scene.nrockets,
           v->scene.missing);
}

static int open_city(Viewer *v, int index, uint32_t block)
{
    City *c = city_open(&v->vol, CITIES[index]);
    if (!c)
        return -1;
    if (v->have_scene) {
        scene_free(&v->scene);
        v->have_scene = 0;
    }
    city_close(v->city);
    v->city = c;
    v->city_index = index;
    v->nblocks = city_blocks(c, v->blocks, 64);
    v->block = block < v->nblocks ? block : 0;
    build(v);
    return 0;
}

/* the viewer's answers to the graph's script checks (Monsters.bsf: there are no buildings, people or power-ups to
 * test against here, so only the monster's own abilities say yes) */
static int attach_world(Mon *mo, const char *name, Mat4 *out);
static int window_person(Mon *mo, const float *xyz);

/* the middle of the players' x (0x336360, kept at gp - 0x6C70): the camera's reach is measured from it */
static float players_mid_x(const Viewer *v)
{
    float lo = 0, hi = 0;
    int n = 0;
    for (int k = 0; k < v->nmon; k++) {
        const Mon *o = &v->mon[k];
        if (!mon_in(o))
            continue;
        if (!n || o->mx < lo) lo = o->mx;
        if (!n || o->mx > hi) hi = o->mx;
        n++;
    }
    return n ? (lo + hi) / 2 : 0;
}

/* the monster as the climbing code sees it: the camera's reach (-9) is measured from the players' middle, and is
 * not asked of an AI monster (0x2DDC40) */
static Climber climber(Mon *mo)
{
    Climber c = { { mo->mx, mo->my, mo->mz }, mo->heading, mo->graph.box[0],
                  mo->v->nmon > 1 && !mo->is_ai ? mo->v->mid_x : mo->mx, { mo->graph.probe[0], mo->graph.probe[1] } };
    return c;
}

/* the hop / swing scripts' way along the face: canClimb(0, -1) for 0 mirrored or > 0 not, else (0, 1) */
static int along(Mon *mo, float p0)
{
    return (mo->mirror && p0 == 0) || (!mo->mirror && p0 > 0) ? -1 : 1;
}

static int answer(void *ctx, const char *call, const float *a)
{
    static const char *const YES[] = { "Monsters::canSpin", "Monsters::hasDoubleJump" };
    Mon *mo = ctx;
    Viewer *v = mo ? mo->v : NULL;
    int r = v ? edible_power_answer(&mo->power, call) : -1;   /* hasRoarEnergy, hasChargePunch, ... (edible.c) */
    if (r >= 0)
        return r;
    for (size_t i = 0; i < sizeof YES / sizeof *YES; i++)
        if (strcmp(call, YES[i]) == 0)
            return 1;
    if (!v)
        return 0;
    Climber c = climber(mo);
    const Scene *sc = &v->scene;
    if (!strcmp(call, "Monsters::canMount")) {       /* (side, z offset, style): styles 2 / 3 swap the sides */
        int side = (int)a[0];
        if ((a[2] == 2 && mo->mirror) || (a[2] == 3 && !mo->mirror))
            side = side == 0 ? 1 : side == 1 ? 0 : side;
        return climb_can_mount(&mo->mount, sc, &c, side, a[1]) == 1;
    }
    if (!strcmp(call, "Monsters::mountIsOccupied")) {
        int r = climb_can_mount(&mo->mount, sc, &c, (int)a[0], 0);
        return r == -1 || r == -2 || r == -3 || r == -5 || r == -6;
    }
    if (!strcmp(call, "Monsters::canHop")) return climb_can_climb(&mo->mount, sc, &c, 0, along(mo, a[0]), 0) == 3;
    if (!strcmp(call, "Monsters::canSwing")) return climb_can_climb(&mo->mount, sc, &c, 0, along(mo, a[0]), 0) == 4;
    if (!strcmp(call, "Monsters::canMantle")) return climb_can_climb(&mo->mount, sc, &c, 1, 0, 0) == 5;
    if (!strcmp(call, "Monsters::canRespawn"))       /* 0x2D3D80: lives left, become human, its streaker not held */
        return v->game ? game_can_respawn(v->game, mo->slot, mo->streaker >= 0 && v->scene.folk &&
                                          people_get(v->scene.folk, mo->streaker)->phase == PP_HELD) : mo->defeated;
    if (!strcmp(call, "Monsters::canGrabPerson")) return window_person(mo, a + 1) >= 0;
    if (!strcmp(call, "Monsters::canDoPunch")) {     /* not at a person's or a power-up's window */
        int e = edible_call(mo, call, a + 1);
        return e >= 0 ? e : window_person(mo, a + 1) < 0;
    }
    if (strstr(call, "::canGrabPowerup") || strstr(call, "::checkForHazard"))
        return edible_call(mo, call, a + 1) > 0;
    if (!strcmp(call, "Monsters::pickupSuccess") || !strcmp(call, "Monsters::isHoldlingEdible"))
        return mo->held >= 0 || mo->held_unit >= 0 || mo->held_vehicle >= 0 || mo->held_rail >= 0 || mo->held_parade >= 0;                         /* hasGrabbedObject: 2 for an actor (0x2E5590) */
    if (!strcmp(call, "Monsters::canTransfer"))      /* (right): mirrored the other way */
        return climb_can_transfer(&mo->mount, sc, &c, mo->mirror ? a[0] == 0 : a[0] == 1) == 1;
    if (!strcmp(call, "Monsters::canSpiderTransfer"))   /* (right): mirrored always right */
        return climb_can_transfer(&mo->mount, sc, &c, mo->mirror || a[0] != 0) == 1;
    if (!strcmp(call, "Monsters::stopClimbUp") || !strcmp(call, "Monsters::stopSpiderClimbUp")) {
        int r = climb_can_climb(&mo->mount, sc, &c, 1, 0, !strcmp(call, "Monsters::stopSpiderClimbUp"));
        if (getenv("RAMPAGE_TRACE"))
            printf("  canClimb up %d (mount z %.1f, top %.1f)\n", r, mo->mount.at.pt[2],
                   mo->mount.at.building >= 0 ? sc->buildings[mo->mount.at.building].b.max[2] : 0);
        return r != 2;
    }
    if (!strcmp(call, "Monsters::stopClimbDown")) return climb_can_climb(&mo->mount, sc, &c, -1, 0, 0) != 2;
    if (!strcmp(call, "Monsters::shouldUnmountDown")) return climb_can_climb(&mo->mount, sc, &c, -1, 0, 0) == -1;
    if (!strcmp(call, "Monsters::hitElectricSign"))
        return climb_can_climb(&mo->mount, sc, &c, a[0] == 0 ? 1 : -1, 0, 0) == -2;
    return 0;                                        /* canGrabPerson, ...: not here */
}

/* the clip events the viewer acts on: the movement mode, and Monsters.bsf's velocity functions (jumpNorm and its kin
 * set the velocity, addVelocity adds to it, in the world or turned with the monster) */
/* Monsters.bsf's hitBuildingFront(kind, x, y, z): which face (by kind, the mirror state, or for kinds 6 and 7 the way
 * the monster faces), the point in the monster's frame, then hitBuildingPt(point, side, 1, 2.0): the point in the
 * world (0x2B3A80), the building whose face that side looks at within 5 units, the point on it (0x2D54F0), and its
 * hit (0x2BC150). Sides: 0 the building's left face (looking +x), 1 its right (-x), 2 its front (+y), 5 its roof. */
static void hit_building_front(Mon *mo, const float *p)
{
    Viewer *v = mo->v;
    int mirror = mo->mirror, side = 0, kind = (int)p[0];
    if (kind == 0 || kind == 1 || kind == 3 || kind == 4) side = mirror;
    else if (kind == 2 || kind == 5) side = 2;
    else if (kind == 6 || kind == 7) {
        float yaw = (mo->heading > 180 ? mo->heading - 360 : mo->heading) / 57.2957795f;
        if (yaw >= -0.7853982f && yaw <= 0.7853982f) side = 2;
        else if (yaw >= 0.7853982f && yaw <= 2.3561945f) side = 1;
        else if (yaw <= -0.7853982f && yaw >= -2.3561945f) side = 0;
        /* facing the street the script leaves side as it was declared: 0 */
    }
    float l[3] = { (mirror && side == 2) || side == 1 ? -p[1] : p[1], p[2], p[3] };
    float c = cosf(mo->heading / 57.2957795f), sn = sinf(mo->heading / 57.2957795f);
    float w[3] = { mo->mx + c * l[0] - sn * l[1], mo->my + sn * l[0] + c * l[1], mo->mz + l[2] };
    SceneBuilding *best = NULL;
    float bq[3], bd = 25;                            /* within 5 units of the face */
    for (uint32_t i = 0; i < v->scene.nbuildings; i++) {
        Building *b = &v->scene.buildings[i].b;
        float q[3] = { w[0], w[1], w[2] };
        int in_x = w[0] >= b->min[0] && w[0] <= b->max[0], in_y = w[1] >= b->min[1] && w[1] <= b->max[1];
        if (side == 2 && in_x) q[1] = b->min[1];
        else if (side == 0 && in_y) q[0] = b->min[0];
        else if (side == 1 && in_y) q[0] = b->max[0];
        else continue;
        if (w[2] > b->max[2])
            continue;
        float d = (q[0] - w[0]) * (q[0] - w[0]) + (q[1] - w[1]) * (q[1] - w[1]);
        if (d <= bd) {
            bd = d;
            best = &v->scene.buildings[i];
            memcpy(bq, q, sizeof bq);
        }
    }
    if (best)                                        /* Building::hit's +0x190: its Scoring pays this monster */
        best->hitter = mo->slot;
    if (best && mo->is_ai) {                         /* the AI's onHit (hitBuildingPt -> 0x2EED40) */
        float off[3] = { w[0] - mo->mx, w[1] - mo->my, w[2] - mo->mz };
        dir_ai_on_hit(&mo->ai, off);
    }
    int hit = best && building_hit(&best->b, bq, 2.0f);
    if (hit && (kind <= 2 || kind == 6) &&             /* +2 energy (Monsters.bsf), the rage when full */
        edible_power_hit_building(&mo->power, 1, mo->mode == MODE_AIRBORNE || mo->mode == MODE_TAKEOFF, others_full(mo))) {
        edible_power_start_rage(&mo->power, others_full(mo));
        mo->pending_slot = 54;
        rage_started(mo);
    }
    mo->energy = mo->power.energy;
    if (getenv("RAMPAGE_TRACE"))
        printf("  hitBuildingFront side %d at %.1f %.1f %.1f: %s", side, w[0], w[1], w[2],
               !best ? "no building\n" : hit ? "hit" : "no damage");
    if (getenv("RAMPAGE_TRACE") && best)
        printf(" (state %d, %d storeys, HP %d)\n", best->b.state, best->b.n, best->b.hp);
}

/* Monsters.bsf's damageRoof (the roof punch, a jump's landing, a fall's, the butt slam's six beats; its arguments
 * unused): monster.hitBuildingRoof(bone, 1, 2.0), Monster method 0x6F (descriptor 0x42B9C0, dispatcher case 0x2AB620):
 * the bone's place in the world (vtable +0x48 0x2E54A0 by name, +0x4C 0x2E4FF0 its matrix), the building under the
 * monster's body (0x2D6150: the one at its probe point, its top no more than 5 above the feet), and its hit 0x2BBE80
 * (building_hit_roof) with kind 1 and radius 2.0. The script ignores the answer: no energy, no rage; the points are
 * Building::hit's (its hitter). The monsters with a damageRoof of their own (Cal, Eyegore, Jill, Natalie, Venus) take
 * "Bip01 Spine". */
static void hit_building_roof(Mon *mo, const char *bone)
{
    Viewer *v = mo->v;
    float s = mo->scale > 0 ? mo->scale : 1;
    CollideMonster cm = { { mo->mx, mo->my, mo->mz }, mo->heading, mo->mode,
                          (mo->graph.box[0] > 0 ? mo->graph.box[0] : 3) * s, (mo->graph.box[1] > 0 ? mo->graph.box[1] : 4.25f) * s,
                          { mo->graph.probe[0] * s, mo->graph.probe[1] * s }, 0, 0, mo->defeated || mo->hp <= 0,
                          { 0, 0, 0 }, { 0, 0, 0 } };
    int bi = collide_building_below(&v->cw, &cm);
    Mat4 w;
    float at[3] = { mo->mx, mo->my, mo->mz };
    if (scene_monster_node(&v->scene, mo->slot, bone, &w) == 0) {   /* the named bone, as the game's mirrored pose has it */
        at[0] = w.m[12]; at[1] = w.m[13]; at[2] = w.m[14];
    }
    if (bi < 0 || (uint32_t)bi >= v->scene.nbuildings) {
        if (getenv("RAMPAGE_TRACE"))
            printf("  hitBuildingRoof %s at %.1f %.1f %.1f: no building below\n", bone, at[0], at[1], at[2]);
        return;
    }
    SceneBuilding *sb = &v->scene.buildings[bi];
    if (sb->b.state == BUILDING_STANDING)            /* Building::hit's +0x190: its Scoring pays this monster */
        sb->hitter = mo->slot;
    int col, roof = sb->b.roof, hit = building_hit_roof(&sb->b, at, 2.0f, &col);
    if (getenv("RAMPAGE_TRACE"))
        printf("  hitBuildingRoof %s at %.1f %.1f %.1f: building %d roof %d -> %d, top storey %d column %d %s "
               "(state %d, %d storeys, HP %d)\n", bone, at[0], at[1], at[2], bi, roof, sb->b.roof, sb->b.n - 1, col,
               hit ? "hit" : "no damage", sb->b.state, sb->b.n, sb->b.hp);
}

/* the monster as its foot object reads it (footstep.c): the feet's bones, its velocity, its ground, the water */
static void foot_pose(Mon *mo, FootPose *m)
{
    Viewer *v = mo->v;
    memset(m, 0, sizeof *m);
    m->place[0] = mo->mx; m->place[1] = mo->my; m->place[2] = mo->mz;
    float h = mo->heading / 57.2957795f;
    m->fwd[0] = -sinf(h); m->fwd[1] = cosf(h);
    for (int f = 0; f < 2; f++) {
        Mat4 w;
        if (scene_monster_node(&v->scene, mo->slot, foot_bone(f, mo->mirror), &w) == 0) {
            m->foot[f][0] = w.m[12]; m->foot[f][1] = w.m[13]; m->foot[f][2] = w.m[14];
        } else
            memcpy(m->foot[f], m->place, sizeof m->place);
    }
    for (int k = 0; k < 3; k++)
        m->vel[k] = mo->vel[k] + mo->air[k];
    m->ground = ground_at(v, mo->mx, mo->my, mo->mz);
    int wk = water_at(v, mo->mx, mo->my);
    m->in_water = wk >= 0;
    m->water_z = wk >= 0 ? water_surface(v->scene.water[wk]) : 0;
    m->node_flags = mo->have_graph ? mo->graph.states[mo->run.state].flags : 0;
    m->mirrored = mo->mirror;
    if (mo->have_graph && mon_in(mo)) {    /* the clip now started, on its own (0x33C5A0) */
        const Instance *in = mon_in(mo);
        double ct = clip_at(mo, &mo->graph.states[mo->run.state], mo->run.time);
        m->clip_ok = ik_clip_node(in, ct, foot_bone(0, mo->mirror), m->clip_foot[0]) == 0 &&
                     ik_clip_node(in, ct, foot_bone(1, mo->mirror), m->clip_foot[1]) == 0;
    }
}

static void clip_event(Mon *mo, const MoveEvent *e)
{
    Viewer *v = mo->v;
    float c = cosf(mo->heading / 57.2957795f), s = sinf(mo->heading / 57.2957795f);
    float mirror = mo->mirror ? -1.0f : 1.0f;        /* Monster.getMirrorState */
    float w[3] = { 0, 0, 0 }, l[3] = { 0, 0, 0 };   /* world / relative velocity */
    int set = 0, add = 0, relative = 0;
    if (e->type == 7) {                              /* 0x2E7DE0 */
        mo->mode = (int)e->value;
        return;
    }
    if (e->type == 6) {                              /* a sound at the monster ({6, at, ?, name}) */
        float at[3] = { mo->mx, mo->my, mo->mz + 4 };
        int h = e->name ? sound_play(e->name, at, 1) : 0;
        if (e->name && getenv("RAMPAGE_TRACE"))
            printf(h ? "  sound %s\n" : "  no sound %s\n", e->name);
        return;
    }
    if (e->type == 9) {                              /* 0x2E7E18 -> 0x2F5AC0: the camera shakes, f32 strength at +8 */
        union { uint32_t u; float f; } k = { e->value };
        float at[3] = { mo->mx, mo->my, mo->mz };
        shake_start(mon_shake(mo), &SHAKE_MONSTER, at, k.f, 1, &v->shake_seed);
        return;
    }
    if (e->type == 8) {                              /* 0x2E7F18: the next clip the other way round */
        mo->mirror_queued = !mo->mirror_queued;
        return;
    }
    if (e->type == 23) {                             /* 0x2E7D38 */
        mo->column = (int32_t)e->value;
        return;
    }
    if (e->type == 0) {                              /* 0x2E7880: the parts' bones armed with this attack, or not */
        mo->armed = e->p[0] != 0 ? mo->armed | e->value : mo->armed & ~e->value;
        if (e->p[0] != 0 && v->sobj)                 /* a new attack: its hit list is empty (0x2E33A0) */
            streetobj_arm(v->sobj);
        if (e->p[0] != 0 && v->boss)
            boss_arm(v->boss, mo->slot);
        if (e->p[0] != 0 && v->army)
            army_arm(v->army, mo->slot);
        if (e->p[0] != 0) {                          /* its record, for the other monsters (0x2E35D0) */
            mo->atk.damage = e->p[1];
            mo->atk.knock = e->p[2];
            mo->atk.knock_time = e->p[3];
            mo->atk.level = e->level;
            mo->atk.flags = e->aflags;
            mo->atk.victims = e->victims;
            mo->atk.hit = 0;
        }
        if (e->p[0] != 0)
            mo->blow++;                               /* a new blow: parade.c's objects take one TakeDamage each */
        if (getenv("RAMPAGE_TRACE"))
            printf("  attack %x %s (%g %g %g)\n", e->value, e->p[0] != 0 ? "on" : "off", e->p[1], e->p[2], e->p[3]);
        return;
    }
    if (e->type != 21 || !e->name)
        return;
    const char *n = e->name;
    if (getenv("RAMPAGE_TRACE"))
        printf("  script %s (%g %g %g %g)\n", n, e->p[0], e->p[1], e->p[2], e->p[3]);
    if (!strcmp(n, "Monsters::jumpNull"))        { w[0] = mirror * e->p[0]; w[1] = e->p[2]; set = 1; }
    else if (!strcmp(n, "Monsters::jumpNorm"))   { w[0] = mirror * e->p[0]; w[2] = 22; set = 1; }
    else if (!strcmp(n, "Monsters::jumpLow"))    { w[0] = mirror * e->p[0]; w[2] = 15; set = 1; }
    else if (!strcmp(n, "Monsters::jumpDrop"))   { w[2] = -10; set = 1; }
    else if (!strcmp(n, "Monsters::jumpHighMonster")) { l[1] = e->p[0]; l[2] = 32; set = relative = 1; }
    else if (!strcmp(n, "Monsters::addVelocity")) {
        relative = e->p[0] != 0;
        float *dst = relative ? l : w;
        dst[0] = e->p[1]; dst[1] = e->p[2]; dst[2] = e->p[3];
        add = 1;
    }
    else if (!strcmp(n, "Monsters::doFart"))     { l[2] = 20; add = relative = 1; }
    else if (!strcmp(n, "Monsters::disableGravity")) { mo->no_gravity = 1; return; }
    else if (!strcmp(n, "Monsters::footStep")) { foot_step(&mo->feet, (int)e->p[0], mo->mirror); return; }
    else if (!strcmp(n, "Monsters::footPlant") || !strcmp(n, "Monsters::cancelFootStep")) {
        FootPose m;                                  /* the footfall under a still monster's foot; the feet set down */
        FootFx fx;
        foot_pose(mo, &m);
        if (n[10] == 'c')
            foot_cancel(&mo->feet, (int)e->p[0], &m);
        else if (foot_plant(&mo->feet, &mo->feet_fx, (int)e->p[0], &m, &fx) && v->fxw) {
            fx_spawn(v->fxw, fx.name, fx.pos, NULL, 1);
            if (getenv("RAMPAGE_TRACE"))
                printf("  footfall %s at %.1f %.1f %.1f\n", fx.name, fx.pos[0], fx.pos[1], fx.pos[2]);
        }
        return;
    }
    else if (!strcmp(n, "Monsters::setMirrorState")) { mo->mirror_queued = e->p[0] != 0; return; }
    else if (!strcmp(n, "Monsters::faceAnim")) {     /* (expression, head layer, -, plays): never looping */
        int head, plays;                             /* (and a delay > 1: the belch that many seconds later) */
        const char *face = face_anim_event(&mo->faces, e->p, &head, &plays);
        if (face)
            face_play(mo, face, head, 0, plays);
        return;
    }
    else if (!strcmp(n, "Monsters::prepareToMount")) { climb_prepare(&mo->mount); return; }
    else if (!strcmp(n, "Monsters::mountBuilding")) {
        Climber cl = climber(mo);
        climb_mount(&mo->mount, &cl, e->p[0]);        /* and transitionToMount(param0) */
        if (mo->is_ai && mo->mount.at.building >= 0)  /* the AI's onMount (0x2EEC40) */
            dir_ai_on_mount(&mo->ai, mo->mount.at.building, mo->mount.at.rail,
                            v->scene.buildings[mo->mount.at.building].b.width);
        return;
    }
    else if (!strcmp(n, "Monsters::unmountBuilding")) { climb_unmount(&mo->mount); return; }
    else if (!strcmp(n, "Monsters::attachHand")) {   /* (hand, time, mirror): only on a building */
        int h = ((int)e->p[0] & 1) ^ (mo->mirror ? 1 : 0);
        Mat4 w;
        if (mo->mount.at.building < 0 || scene_monster_hand(&v->scene, mo->slot, h, &w) != 0)
            return;
        /* the place: the clip's hand now, held to the face; the rotation the clip's (the game's is the clip's hand
         * at the end of the blend, 0x321E80) */
        float pos[3] = { w.m[12], w.m[13], w.m[14] }, r[9], q[4];
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++)
                r[3 * c + k] = w.m[4 * c + k];
        ik_quat_of(r, q);
        climb_hand_snap(&mo->mount, &v->scene, h, pos);
        if (hand_attach(&mo->hand[h], e->p[1], pos, q) && e->p[2] != 0)
            mo->mirror_queued = !mo->mirror_queued;
        return;
    }
    else if (!strcmp(n, "Monsters::detachHand")) {
        hand_detach(&mo->hand[((int)e->p[0] & 1) ^ (mo->mirror ? 1 : 0)], e->p[1]);
        return;
    }
    else if (!strcmp(n, "Monsters::transferMount")) { climb_transfer(&mo->mount); return; }
    else if (!strcmp(n, "Monsters::setScale")) {     /* (scale, time): setDynamicScale, from its size now */
        mo->scale_from = mo->scale;
        mo->scale_to = e->p[0] > 0 ? e->p[0] : 1;
        mo->scale_time = mo->scale_left = e->p[1] > 0 ? e->p[1] : 0;
        if (mo->scale_left <= 0)
            mo->scale = mo->scale_to;
        return;
    }
    else if (!strcmp(n, "Monsters::startRespawn")) { /* 0x2D3930: a life, back to full health where the streaker is
                                                        or dropping in from the view */
        mo->hp = 200;
        mo->defeated = 0;
        if (mon_in(mo))
            mon_in(mo)->hidden = 0;
        if (v->game) {
            GameRespawnIn in;
            GameRespawn out;
            memset(&in, 0, sizeof in);
            const Person *p = mo->streaker >= 0 && v->scene.folk ? people_get(v->scene.folk, mo->streaker) : NULL;
            if (p && p->phase != PP_GONE) {
                in.streaker = 1;
                in.on_line = p->mv.active && p->phase != PP_HELD;
                memcpy(in.streaker_pos, p->pos, sizeof in.streaker_pos);
                in.streaker_facing = p->heading / 57.2957795f;
            }
            memcpy(in.cam_pos, v->view.pos, sizeof in.cam_pos);
            memcpy(in.cam_dir, v->view.rows[1], sizeof in.cam_dir);
            in.cam_half_fov = v->view.fov / 2;
            game_start_respawn(v->game, mo->slot, &in, &out);
            mo->mx = out.pos[0]; mo->my = out.pos[1]; mo->mz = out.pos[2];
            mo->heading = out.facing * 57.2957795f;
            mo->human = 0;
            if (out.fall) {                          /* in the air, Jump_Butt_Slam_fall */
                mo->mode = MODE_AIRBORNE;
                mo->dropping = 1;                    /* +0xAB5: the camera leaves it out until it lands */
                for (int k = 0; mo->have_graph && k < mo->graph.n; k++)
                    if (!_stricmp(mo->graph.states[k].name, "Jump_Butt_Slam_fall")) {
                        moves_force(&mo->run, k);
                        break;
                    }
            }
            if (out.delete_streaker && v->scene.folk && mo->streaker >= 0)
                people_remove(v->scene.folk, mo->streaker);
            if (getenv("RAMPAGE_TRACE"))
                printf("  respawn at %.1f %.1f %.1f%s, %d lives\n", out.pos[0], out.pos[1], out.pos[2],
                       out.fall ? ", falling" : "", game_lives(v->game, mo->slot));
        }
        mo->streaker = -1;
        return;
    }
    else if (!strcmp(n, "Monsters::becomeHuman")) {  /* while a block plays (state 6): createStreaker, and the monster
                                                        stops drawing itself (0x2D3ED0) */
        if (!v->scene.folk || !mon_in(mo))
            return;
        int human = v->game ? game_become_human(v->game, mo->slot) : 1;
        if (getenv("RAMPAGE_TRACE"))
            printf("  becomeHuman: %s (state %d)\n", human == 1 ? "a streaker" : "not in play", v->game ? game_state(v->game) : -1);
        if (human != 1)
            return;
        Group t = city_template(v->city, monster_female(mo) ? "NakedFemale" : "NakedMale");
        float at[3] = { mo->mx, mo->my, mo->mz };
        if (t.at && (mo->streaker = people_add_streaker(v->scene.folk, t, at, v->view.pos[0])) >= 0) {
            mon_in(mo)->hidden = 1;
            mo->human = 1;
        }
        return;
    }
    else if (!strcmp(n, "Monsters::doGrabPerson")) { /* grabBuildingOccupant(loc, 1) (0x2B3350): 0.25 s blend */
        int who = mo->held < 0 ? window_person(mo, e->p + 1) : -1;
        if (who >= 0) {
            people_grab(v->scene.folk, who, 0.25f);
            mo->held = who;
        }
        return;
    }
    else if (!strcmp(n, "Monsters::doGrabObject")) { /* (0x2D99C0): the nearest within 15 of the hand, in front */
        Mat4 h;
        if (mo->held >= 0 || !v->scene.folk || attach_world(mo, "Helper R Hand", &h) != 0)
            return;
        float hand[3] = { h.m[12], h.m[13], h.m[14] }, root[3] = { mo->mx, mo->my, mo->mz };
        float fwd[3] = { -sinf(mo->heading / 57.2957795f), cosf(mo->heading / 57.2957795f), 0 };
        int who = people_nearest(v->scene.folk, hand, root, fwd, 15);
        if (who >= 0) {
            people_grab(v->scene.folk, who, 0.1f);
            mo->held = who;
        } else if (v->army && (who = army_nearest_pickup(v->army, hand, root, fwd, 15)) >= 0) {
            army_grab(v->army, who, mo->slot);
            mo->held_unit = who;
        } else if (v->parade && mo->held_parade < 0 && (who = parade_nearest_pickup(v->parade, hand, root, fwd, 15)) >= 0) {
            parade_grab(v->parade, who, mo->slot, 0.1f);    /* on the sidewalk: before the street's vehicles */
            mo->held_parade = who;
        } else if (v->veh && mo->held_vehicle < 0 && (who = vehicle_nearest_pickup(v->veh, hand, root, fwd, 15)) >= 0) {
            vehicle_grab(v->veh, who, mo->slot);
            mo->held_vehicle = who;
        } else if (v->rail && mo->held_rail < 0 && (who = rail_nearest_pickup(v->rail, hand, root, fwd, 15)) >= 0) {
            rail_grab(v->rail, who, mo->slot);
            mo->held_rail = who;
        }
        return;
    }
    else if (!strcmp(n, "Monsters::grabBoulder")) {  /* Monsters.bsf: a Boulder torn out of the street, 6.5 up */
        float at[3] = { mo->mx, mo->my, mo->mz + 6.5f };
        int who = v->veh && mo->held < 0 && mo->held_unit < 0 && mo->held_vehicle < 0 ?
                  vehicle_spawn(v->veh, "Boulder", at, 0, -1) : -1;
        if (who >= 0) {
            vehicle_grab(v->veh, who, mo->slot);
            mo->held_vehicle = who;
        }
        return;
    }
    else if (!strcmp(n, "Monsters::throwObject")) {  /* (eat, x, y, z) (0x2D9DC0): eaten, or smashed away */
        if (mo->held_unit >= 0 && v->army) {
            army_throw(v->army, mo->held_unit, mo->slot, e->p[0] != 0, e->p + 1, mo->heading / 57.2957795f);
            mo->held_unit = -1;
            return;
        }
        if (mo->held_vehicle >= 0 && v->veh) {
            vehicle_throw(v->veh, mo->held_vehicle, mo->slot, e->p[0] != 0, e->p + 1, mo->heading / 57.2957795f);
            mo->held_vehicle = -1;
            return;
        }
        if (mo->held_parade >= 0 && v->parade) {
            parade_throw(v->parade, mo->held_parade, mo->slot, e->p[0] != 0, mo->heading / 57.2957795f);
            mo->held_parade = -1;
            return;
        }
        if (mo->held_rail >= 0 && v->rail) {
            rail_throw(v->rail, mo->held_rail, mo->slot, e->p[0] != 0, e->p + 1);
            mo->held_rail = -1;
            return;
        }
        if (mo->held < 0 || !v->scene.folk)
            return;
        if (e->p[0] != 0) {
            const Person *pp = people_get(v->scene.folk, mo->held);
            int first = pp && !(pp->flags & 0x40000000u);    /* addStats once per object */
            const char *name = pp ? pp->name : NULL;
            PeopleEaten got = people_eat(v->scene.folk, mo->held);
            monster_stats(mo, got.health, got.energy);
            if (first) {                                 /* gCommon.addStats(actor, slot, 1): civilian points */
                if (v->game && got.points != 0)
                    game_add_points(v->game, mo->slot, GPT_CIVILIAN, (int)got.points);
                challenge_stats(v, name, (int)got.points);
            }
            if (getenv("RAMPAGE_TRACE"))
                printf("  ate: health %+.0f -> %.0f, energy %+.0f -> %.0f\n", got.health, mo->hp, got.energy, mo->energy);
        } else {
            people_throw(v->scene.folk, mo->held, mo->heading);
        }
        mo->held = -1;
        return;
    }
    else if (strstr(n, "::doGrabPowerup") || strstr(n, "::destroyPowerup") || strstr(n, "::checkForHazard")) {
        int r = edible_call(mo, n, e->p + 1);         /* grabBuildingOccupant: the edible at that window */
        if (getenv("RAMPAGE_TRACE"))
            printf("  %s: %d\n", n, r);
        return;
    }
    else if (!strcmp(n, "Monsters::useRoarEnergy")) { edible_power_use_roar(&mo->power); mo->energy = mo->power.energy; return; }
    else if (!strcmp(n, "Monsters::changeMountRail")) {
        float d = mo->mirror ? -e->p[0] : e->p[0];
        climb_change_rail(&mo->mount, d < 0 ? -1 : 1);
        return;
    }
    else if (!strcmp(n, "Monsters::transitionFromBuildingTop")) { mo->mount.from_top = 1; return; }
    else if (strstr(n, "::hitBuildingFront")) { hit_building_front(mo, e->p); return; }
    else if (strstr(n, "::damageRoof")) {            /* Monsters.bsf the foot, Cal.bsf's kin the spine */
        hit_building_roof(mo, !strncmp(n, "Monsters::", 10) ? "Bip01 R Foot" : "Bip01 Spine");
        return;
    }
    else return;
    if (relative) {                                  /* the monster's frame (0x2E7330 / 0x2E74F0: by its matrix) */
        w[0] = c * l[0] - s * l[1];
        w[1] = s * l[0] + c * l[1];
        w[2] = l[2];
    }
    for (int k = 0; k < 3; k++)
        mo->vel[k] = set ? w[k] : mo->vel[k] + (add ? w[k] : 0);
}

static void clip_events(Mon *mo, const MoveState *st, float from, float to)
{
    const MoveEvent *e;
    int n = moves_events_between(st, from, to, &e);
    for (int i = 0; i < n; i++)
        clip_event(mo, &e[i]);
}

/* the stick (x right, y down) as a direction on the ground, in the camera's frame the monster keeps (0x2E8410) */
static void stick_world(Mon *mo, float lx, float ly, float w[3])
{
    if (mo->is_ai) {                                  /* the AI's stick is in world x, y (0x2C0AD0, 0x2E8410) */
        w[0] = mo->ai.stick[0];
        w[1] = mo->ai.stick[1];
        w[2] = 0;
        return;
    }
    for (int k = 0; k < 3; k++)
        w[k] = lx * mo->frame[0][k] - ly * mo->frame[1][k];
    w[2] = 0;
}

/* the monster's collision box (0x2E2C50): MONSTER_DATA +0x198 across (3 each way) and +0x19C up (4.25 each way from
 * its middle: 8.5 tall), against the buildings' boxes */
#define MONSTER_HALF_WIDTH 3.0f
#define MONSTER_HEIGHT 8.5f
#define ROOF_BAND 1.5f                               /* a building box's top 1.5 is its roof's band (0x2D1A30) */

/* the highest ground at (x, y) not above z + 1: the street, or the roof of a building it stands over */
/* the water the monster stands in (0x2D5F00: its box against the waters'), -1 none */
static int water_at(const Viewer *v, float x, float y)
{
    for (int k = 0; k < v->scene.nwater; k++)
        if (water_contains(v->scene.water[k], x, y))
            return k;
    return -1;
}

/* a water's surface: its z (water_floor gives it outside the pool's rectangle) */
static float water_surface(const Water *w)
{
    return water_floor(w, 1e9f, 1e9f);
}

static float ground_at(const Viewer *v, float x, float y, float z)
{
    int wk = water_at(v, x, y);                      /* in the water: its floor (0x2E859C: +0xA48 = 0x337E00) */
    float g = wk >= 0 ? water_floor(v->scene.water[wk], x, y) : 0;
    for (uint32_t i = 0; i < v->scene.nbuildings; i++) {
        const Building *b = &v->scene.buildings[i].b;
        float top = b->max[2];
        if (b->max[2] > b->min[2] + ROOF_BAND && x >= b->min[0] && x <= b->max[0] && y >= b->min[1] && y <= b->max[1] &&
            top <= z + 1 && top > g)
            g = top;
    }
    return g;
}

/* the monster's collision spheres (MoveShape: a bone and an offset) in the world now: centre, radius; how many */
static int monster_spheres(Mon *mo, float out[][4], int max)
{
    if (!mo->have_graph || !mon_in(mo))
        return 0;
    const Instance *in = mon_in(mo);
    int ns = 0;
    for (int i = 0; i < mo->graph.nshapes && ns < max; i++) {
        const MoveShape *sh = &mo->graph.shapes[i];
        if (sh->bone < 0 || (uint32_t)sh->bone >= in->tree.n)
            continue;
        const float *m = in->node[sh->bone].m, *bm = in->base.m;
        float l[3];
        for (int r = 0; r < 3; r++)
            l[r] = m[r] * sh->off[0] + m[4 + r] * sh->off[1] + m[8 + r] * sh->off[2] + m[12 + r];
        if (mo->mirror)
            l[0] = -l[0];
        for (int r = 0; r < 3; r++)
            out[ns][r] = bm[r] * l[0] + bm[4 + r] * l[1] + bm[8 + r] * l[2] + bm[12 + r];
        out[ns++][3] = sh->radius;
    }
    return ns;
}

/* the frame's collision world (collide.c; the game's RGridMgr 0x4D0900 and RSceneryMgr 0x4D0B00), built once a frame
 * before anything moves: the block's buildings (tag COLLIDE_TAG(COLLIDE_OWN_BUILDING, scene index)), the monster's
 * spheres (flags 4 | COLLIDE_F_MONSTER: what the shots' rays hit; WCISpheres 0x449A20 [G: as separate spheres]),
 * then every module's objects with their shapes and flags (army, vehicles, rail, parade, street and roof objects,
 * the bosses' parts and VolumeTriggers). The monster (push_out), the shots and the flying objects ask it */
static void world_build(Viewer *v)
{
    CollideWorld *w = &v->cw;
    collide_begin(w, -1);
    for (uint32_t i = 0; i < v->scene.nbuildings; i++) {
        const Building *b = &v->scene.buildings[i].b;
        collide_add_building(w, b->min, b->max, b->row, b->state, b->collapse != -1,
                             COLLIDE_TAG(COLLIDE_OWN_BUILDING, (int)i));
    }
    for (int j = 0; j < v->nmon; j++) {              /* each monster's, tagged with its slot */
        Mon *mo = &v->mon[j];
        float sp[64][4];
        int ns = monster_spheres(mo, sp, 64);
        mo->msph0 = w->no;
        mo->nmsph = 0;
        for (int k = 0; k < ns; k++) {
            CollideShape sh;
            float m[12], zero[3] = { 0, 0, 0 };
            collide_shape_sphere(&sh, zero, sp[k][3]);
            collide_matrix(m, 0, sp[k]);
            if (collide_add(w, &sh, m, COLLIDE_F_WORLD | COLLIDE_F_MONSTER, COLLIDE_TAG(COLLIDE_OWN_MONSTER, j), k) >= 0)
                mo->nmsph++;
        }
    }
    if (v->army) army_collide_add(v->army, w);
    if (v->veh) vehicle_collide_add(v->veh, w);
    if (v->rail) rail_collide_add(v->rail, w);
    if (v->parade) parade_collide_add(v->parade, w);
    if (v->sobj) streetobj_collide_add(v->sobj, w);
    if (v->boss) boss_collide_add(v->boss, w);
    collide_finish(w);
}

/* the monster's spheres moved with it: their objects in the frame's world follow (before the modules update) */
static void world_monster_moved(Mon *mo, const float (*sp)[4], int ns)
{
    Viewer *v = mo->v;
    for (int k = 0; k < ns && k < mo->nmsph; k++) {
        CollideObj *o = &v->cw.o[mo->msph0 + k];
        memcpy(o->m + 9, sp[k], 3 * sizeof(float));
        o->shape.radius = o->shape.half[0] = o->shape.half[1] = o->shape.half[2] = sp[k][3];
        o->shape.raw[3] = sp[k][3] * sp[k][3];
    }
}

/* TakeImpact on what a module's flying object met (0x287430, vtable +0x38 of the other): a building is hit at the
 * contact with radius Mass / 1000 of the mover (Building 0x2BC0F0 -> 0x2BC150) [R]; another module's object gets its
 * script's TakeImpact (Vehicle.bsf, ElevatedTrain.bsf, CableCar.bsf, StreetObject.bsf, Bomber.bsf); the monster,
 * the parade's and the bosses' scripts have none */
static void impact_route(Viewer *v, int tag, int kind, const float at[3], float mass)
{
    int i = COLLIDE_TAG_INDEX(tag), hit = 0;
    switch (COLLIDE_TAG_OWNER(tag)) {
    case COLLIDE_OWN_BUILDING:
        if ((uint32_t)i < v->scene.nbuildings) {    /* Building::hit (0x2BC150): +0x190 = the attacker if it is a */
            v->scene.buildings[i].hitter = -1;       /* monster, else none: a thrown object clears it */
            hit = building_hit(&v->scene.buildings[i].b, at, mass / 1000);
        }
        break;
    case COLLIDE_OWN_VEHICLE: if (v->veh) vehicle_impact(v->veh, i); break;
    case COLLIDE_OWN_RAIL: if (v->rail) rail_impact(v->rail, i); break;
    case COLLIDE_OWN_STREET: if (v->sobj) hit = streetobj_take_impact(v->sobj, i); break;
    case COLLIDE_OWN_ARMY: if (v->army) army_impact(v->army, i); break;
    default: break;
    }
    if (getenv("RAMPAGE_TRACE"))
        printf("  impact on %d.%d (kind %d) at %.1f %.1f %.1f, mass %.0f%s\n", COLLIDE_TAG_OWNER(tag), i, kind, at[0],
               at[1], at[2], mass, hit ? ": hit" : "");
}

/* the monster's box against the frame's world, as the game moves it (0x2E2C50): the PushOutMonsters objects and the
 * VolumeTriggers (their touches go to their owner: boss.c's barriers), then the buildings (fronts, alley walls and
 * corners), held on a roof, the street's limits; on a building's face (mode 5) only the triggers. Roofs are landed on
 * by ground_at */
static void push_out(Mon *mo)
{
    Viewer *v = mo->v;
    if (mo->slot >= v->nmon)                         /* 0x2E2C6C: a slot (+0xFC) past the players' count (0x4D7E28),
                                                      * the revealed monster's: none of it, so it walks off the street */
        return;
    float s = mo->scale > 0 ? mo->scale : 1;
    CollideMonster m = { { mo->mx, mo->my, mo->mz }, mo->heading, mo->mode,
                         (mo->graph.box[0] > 0 ? mo->graph.box[0] : 3) * s, (mo->graph.box[1] > 0 ? mo->graph.box[1] : 4.25f) * s,
                         { mo->graph.probe[0] * s, mo->graph.probe[1] * s }, 0, mo->is_ai, 0,
                         { mo->vel[0], mo->vel[1], mo->vel[2] }, { mo->air[0], mo->air[1], mo->air[2] } };
    CollideMonsterOut o;
    collide_monster(&v->cw, &m, &o);
    for (int k = 0; k < o.ntouch; k++) {             /* 0x335BF0: the trigger's FireFunc, by its owner */
        const CollideObj *t = &v->cw.o[o.touch[k]];
        if (COLLIDE_TAG_OWNER(t->tag) == COLLIDE_OWN_BOSS && t->kind == 1 && v->boss)
            boss_trigger_touch(v->boss, COLLIDE_TAG_INDEX(t->tag), mo->slot, o.touch_normal[k]);
    }
    if (getenv("RAMPAGE_TRACE")) {
        for (int k = 0; k < o.npushed; k++) {
            const CollideObj *t = &v->cw.o[o.pushed[k]];
            printf("  collide: pushed out of %d.%d (kind %d)\n", COLLIDE_TAG_OWNER(t->tag), COLLIDE_TAG_INDEX(t->tag),
                   t->kind);
        }
        for (int k = 0; k < o.ntouch; k++) {
            const CollideObj *t = &v->cw.o[o.touch[k]];
            printf("  collide: touched trigger %d.%d\n", COLLIDE_TAG_OWNER(t->tag), COLLIDE_TAG_INDEX(t->tag));
        }
        if (m.pos[0] != mo->mx || m.pos[1] != mo->my)
            printf("  collide: %.2f %.2f -> %.2f %.2f (below %d roof %d walled %d)\n", mo->mx, mo->my, m.pos[0], m.pos[1],
                   o.below, o.on_roof, o.walled);
    }
    mo->mx = m.pos[0];
    mo->my = m.pos[1];
    mo->vel[0] = m.vel[0]; mo->vel[1] = m.vel[1];
    mo->air[0] = m.stick[0]; mo->air[1] = m.stick[1];
    if (o.feet_lift) {                               /* 0x33BE90: both feet lifted, no anchor */
        foot_step(&mo->feet, 0, 0);
        foot_step(&mo->feet, 1, 0);
        mo->feet.anchor = -1;
    }
}

/* NODE_STEER (0x2E8190): the turn towards the stick this frame, in degrees. The stick in the monster's own frame
 * against the state's forward axis; beyond one frame's worth of turning it turns by the angle Ã— `turn` Ã— dt, so it
 * swings round fast and settles gently. */
static float steer(Mon *mo, const MoveState *st, float lx, float ly, float dt)
{
    float w[3], h = mo->heading / 57.2957795f, c = cosf(h), s = sinf(h);
    stick_world(mo, lx, ly, w);
    float l[2] = { c * w[0] + s * w[1], -s * w[0] + c * w[1] };   /* by the monster's matrix: right, forward */
    float n = l[0] * l[0] + l[1] * l[1];
    if (n <= 1e-6f)
        return 0;
    n = 1 / sqrtf(n);
    l[0] *= n;
    l[1] *= n;
    float fx = mo->mirror ? -st->fwd[0] : st->fwd[0];   /* mirrored: its forward axis' x the other way */
    float d = l[0] * fx + l[1] * st->fwd[1];
    float angle = acosf(d < -1 ? -1 : d > 1 ? 1 : d);
    if (angle <= st->turn * dt)
        return 0;
    float sign = l[0] * st->fwd[1] - l[1] * fx < 0 ? 1.0f : -1.0f;
    return sign * angle * st->turn * dt * 57.2957795f;
}

/* NODE_AIR_* (0x2E6580): the stick asks for `speed` along the monster's own axes the state allows; the velocity
 * moves towards that by MOVES_AIR_ACCEL (MOVES_AIR_BRAKE when slowing), each axis on its own, and is kept to the
 * state's speed */
static void air_control(Mon *mo, const MoveState *st, float lx, float ly, float dt)
{
    float goal[3] = { 0, 0, 0 }, w[3];
    if (st->flags & (NODE_AIR_X | NODE_AIR_Y | NODE_AIR_Z)) {
        stick_world(mo, lx, ly, w);
        float n = w[0] * w[0] + w[1] * w[1];
        if (n >= 1e-6f) {
            float h = mo->heading / 57.2957795f, c = cosf(h), s = sinf(h);
            float axis[2][3] = { { c, s, 0 }, { -s, c, 0 } };   /* the monster's x (right) and y (forward) */
            n = st->speed / sqrtf(n);
            for (int k = 0; k < 3; k++)
                w[k] *= n;
            for (int a = 0; a < 2; a++)
                if (st->flags & (a ? NODE_AIR_Y : NODE_AIR_X)) {
                    float p = w[0] * axis[a][0] + w[1] * axis[a][1];
                    for (int k = 0; k < 3; k++)
                        goal[k] += p * axis[a][k];
                }
        }
    }
    float cur = mo->air[0] * mo->air[0] + mo->air[1] * mo->air[1] + mo->air[2] * mo->air[2];
    float want = goal[0] * goal[0] + goal[1] * goal[1] + goal[2] * goal[2];
    float step = (cur <= want ? MOVES_AIR_ACCEL : MOVES_AIR_BRAKE) * dt;
    for (int k = 0; k < 3; k++) {
        float d = goal[k] - mo->air[k];
        mo->air[k] += d > step ? step : d < -step ? -step : d;
    }
    cur = mo->air[0] * mo->air[0] + mo->air[1] * mo->air[1] + mo->air[2] * mo->air[2];
    if (cur > st->speed * st->speed) {
        float k = st->speed / sqrtf(cur);
        for (int i = 0; i < 3; i++)
            mo->air[i] *= k;
    }
}

/* one frame of play. The left stick (x right, y down) and the buttons pressed this frame (MV_CROSS ...) or held.
 * The graph picks the state; the clip's root carries the monster (its travel added to where it stands, its turn
 * when a NODE_BAKE_ROOT state ends), the stick turns it (NODE_STEER) or drives it (NODE_AIR_*) in the camera's frame,
 * the clip's events set its movement mode and velocity, and in the air it falls by MOVES_GRAVITY until it meets the
 * ground (here the street at z = 0), which fires the landing slot. */
/* the collision spheres an armed attack can hit with, in the world (0x2DA9E0: the shape's offset by its bone's
 * matrix, then by the monster's), into the scene's overlay */
static void hit_spheres(Mon *mo)
{
    Viewer *v = mo->v;
    if (!mo->have_graph || !mo->armed || !mon_in(mo))
        return;
    const Instance *in = mon_in(mo);
    const MoveGraph *g = &mo->graph;
    for (int i = 0; i < g->nshapes && v->scene.nspheres < 256; i++) {
        const MoveShape *sh = &g->shapes[i];
        int armed = 0;
        for (int p = 0; p < g->nparts && !armed; p++)
            if (mo->armed >> p & 1)
                for (int k = 0; k < g->part_nbones[p]; k++)
                    armed |= g->part_bone[p][k] == sh->bone;
        if (!armed || sh->bone < 0 || (uint32_t)sh->bone >= in->tree.n)
            continue;
        const float *m = in->node[sh->bone].m;      /* in the monster's frame, then reflected if mirrored */
        float l[3];
        for (int r = 0; r < 3; r++)
            l[r] = m[r] * sh->off[0] + m[4 + r] * sh->off[1] + m[8 + r] * sh->off[2] + m[12 + r];
        if (mo->mirror)
            l[0] = -l[0];
        const float *bm = in->base.m;
        float *c = v->scene.spheres[v->scene.nspheres++];
        for (int r = 0; r < 3; r++)
            c[r] = bm[r] * l[0] + bm[4 + r] * l[1] + bm[8 + r] * l[2] + bm[12 + r];
        c[3] = sh->radius;
        scene_adornments_hit(&v->scene, c, c[3]);    /* 0x2D4390: it knocks adornments off */
        if (v->army)                                 /* and the army's units: the attack's damage, once an attack */
            army_hit(v->army, mo->slot, c, c[3], mo->atk.damage);
        if (v->veh)                                  /* and the vehicles and rockets */
            vehicle_hit(v->veh, mo->slot, c, c[3], 20);
        if (v->rail)                                 /* and the trains, cable cars and freighters */
            rail_hit(v->rail, mo->slot, c, c[3], 20);
        if (v->boss)                                 /* and the bosses */
            boss_hit(v->boss, mo->slot, c, c[3], mo->atk.damage);   /* the attack's damage, once a part */
        if (v->parade)                               /* and the balloons, bikers and food carts */
            parade_hit(v->parade, mo->slot, c, c[3], mo->blow);
        upgrade_touch(mo, c, c[3]);                   /* and the MonsterUpgrade object */
        if (v->sobj) {                               /* and the street and roof objects (TakeDamage, type 0) */
            StreetMonster me = { { mo->mx, mo->my, mo->mz }, mo->heading / 57.2957795f };
            streetobj_hit(v->sobj, mo->slot, &me, c, c[3], 20);
        }
        if (v->scene.folk) {                         /* and people (TakeDamage), and the guards' huts */
            people_hit(v->scene.folk, mo->slot, c, c[3], mo->heading, mo->graph.states[mo->run.state].name);
            float from[3] = { mo->mx, mo->my, mo->mz };
            PeopleEaten got = scene_huts_hit(&v->scene, c, c[3], from);
            if (got.health != 0 || got.energy != 0 || got.points != 0)
                monster_stats(mo, got.health, got.energy);
        }
    }
}

/* monster against monster (0x2E35D0, each frame for each attacker): its armed spheres against the others' collision
 * spheres; a victim within 40, not hidden, not down, in front or behind as the attack allows and in its victims'
 * mask, is hit once an attack: the record's damage and ReactionLevel, knocked the record's distance in its time away
 * from the attacker (0x2D6A50 as for the army's hits). No points, no team [R] */
static void monsters_fight(Viewer *v)
{
    for (int a = 0; a < v->nmon; a++) {
        Mon *at = &v->mon[a];
        if (!at->nsph || at->defeated)
            continue;
        float h = at->heading / 57.2957795f, fwd[2] = { -sinf(h), cosf(h) };
        for (int j = 0; j < v->nmon; j++) {
            Mon *vi = &v->mon[j];
            if (j == a || !(at->atk.victims >> j & 1) || (at->atk.hit >> j & 1) || vi->defeated || vi->hp <= 0 ||
                !mon_in(vi) || mon_in(vi)->hidden)
                continue;
            float dx = vi->mx - at->mx, dy = vi->my - at->my, dz = vi->mz - at->mz;
            if (dx * dx + dy * dy + dz * dz > 1600)
                continue;
            int front = fwd[0] * dx + fwd[1] * dy >= 0;
            if (!(at->atk.flags & (front ? 2u : 4u)))
                continue;
            float sp[64][4];
            int ns = monster_spheres(vi, sp, 64), hit = 0;
            for (int i = 0; i < at->nsph && !hit; i++) {
                const float *c = v->scene.spheres[at->sph0 + i];
                for (int k = 0; k < ns && !hit; k++) {
                    float ex = sp[k][0] - c[0], ey = sp[k][1] - c[1], ez = sp[k][2] - c[2], r = sp[k][3] + c[3];
                    hit = ex * ex + ey * ey + ez * ez < r * r;
                }
            }
            if (!hit)
                continue;
            at->atk.hit |= 1u << j;
            float l = sqrtf(dx * dx + dy * dy), sp_ = at->atk.knock_time > 0 ? at->atk.knock / at->atk.knock_time : 0;
            float knock[3] = { l > 1e-4f ? dx / l * sp_ : 0, l > 1e-4f ? dy / l * sp_ : 0, 0 };
            float from[3] = { at->mx, at->my, at->mz };
            char name[16];
            snprintf(name, sizeof name, "monster %d", a + 1);
            monster_hit(vi, name, at->atk.damage, at->atk.level, knock, from);
        }
    }
}

/* every monster's armed spheres, after the frame's posing (drawn with H), and what they hit of each other */
static void hit_spheres_all(Viewer *v)
{
    v->scene.nspheres = 0;
    for (int k = 0; k < v->nmon; k++) {
        Mon *mo = &v->mon[k];
        mo->sph0 = v->scene.nspheres;
        hit_spheres(mo);
        mo->nsph = v->scene.nspheres - mo->sph0;
    }
    if (v->nmon > 1)
        monsters_fight(v);
    if (!v->show_hits)
        v->scene.nspheres = 0;
}

static void hands_step(Mon *mo, float dt);

/* the foot object's update (0x33D450), from ik_feet between the posing and the foot IK, once a game frame */
static void feet_update(void *ctx, IkFeet *f)
{
    Mon *mo = ctx;
    Viewer *v = mo->v;
    if (!mo->feet_due || !mon_in(mo))
        return;
    mo->feet_due = 0;
    const Instance *in = mon_in(mo);
    FootFrame m;
    memset(&m, 0, sizeof m);
    m.place[0] = mo->mx; m.place[1] = mo->my; m.place[2] = mo->mz;
    m.heading = mo->heading;
    m.ground = ground_at(v, mo->mx, mo->my, mo->mz);
    for (int r = 0; r < 2; r++) {
        m.ok[r] = f->ok[r];
        memcpy(m.foot[r], f->foot[r], sizeof m.foot[r]);
        memcpy(m.hip[r], f->hip[r], sizeof m.hip[r]);
        m.reach[r] = f->reach[r];
    }
    m.blend_left = in->blend_left;
    m.blend_time = in->blend_time;
    m.steps = 30.0f / 60;                            /* a viewer frame of 1/60 s is half a world step */
    foot_update(&mo->feet, &m, f->w, f->pos);
}

/* the end of the frame's move: the foot object updates at the next posing */
static void feet_step(Mon *mo)
{
    Viewer *v = mo->v;
    if (!mon_in(mo))
        return;
    v->scene.mon[mo->slot].feet = &mo->feet_ik;
    mo->feet_ik.update = feet_update;
    mo->feet_ik.ctx = mo;
    mo->feet_due = 1;
}

/* the frame's shadows (shadow.c): the buildings' on the lot (redone where a top came down), every object whose
 * template has ShadowData, the monster's blobs on the ground or roof and on the wall it climbs */
static void shadow_frame(Viewer *v, float dt)
{
    if (!v->slight.monster_blob)
        shadow_lighting_default(&v->slight);
    shadow_list_clear(&v->shl);
    v->scene.shadows = &v->shl;
    v->scene.shadow_bundle = v->have_fx_bundle ? &v->fx_bundle : NULL;
    if (!v->have_scene)
        return;
    for (uint32_t i = 0; i < v->scene.nbuildings && i < SHADOW_MAX_BUILDINGS; i++) {
        const Building *b = &v->scene.buildings[i].b;
        if (b->max[2] != v->sg.top[i]) {
            float box[6] = { b->min[0], b->min[1], b->min[2], b->max[0], b->max[1], b->max[2] };
            shadow_ground_top(&v->sg, (int)i, box);
        }
    }
    shadow_ground_draw(&v->shl, &v->sg, &v->slight);
    for (uint32_t i = 0; i < v->scene.ninstances; i++) {
        const Instance *in = &v->scene.instances[i];
        if (!in->shadow.at || in->hidden)
            continue;
        ShadowData d;
        shadow_data_default(&d);
        Group g = in->shadow;
        const char *tex = group_str(g, "Texture|TextureName");
        if (tex) d.texture = tex;
        d.intensity = group_num(g, "Intensity", d.intensity);
        d.max_height = group_num(g, "MaxHeight", d.max_height);
        d.full_height = group_num(g, "FullHeight", d.full_height);
        d.ex_proj_height = group_num(g, "ExProjHeight", d.ex_proj_height);
        Group rc = group_sub(g, "ShadowRect");
        Record r[4];
        if (rc.at && group_items(rc, r, 4) == 4)
            for (int k = 0; k < 4; k++)
                d.rect[k] = record_num(r[k]);
        const float *m = in->base.m;                 /* rows: its x, y, z axes, then its place */
        float m12[12] = { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14] };
        shadow_object(&v->shl, &v->slight, &d, m12);
    }
    for (int k = 0; k < mon_world(v); k++) {        /* each monster's */
        Mon *mo = &v->mon[k];
        if (mon_in(mo) && !mon_in(mo)->hidden && v->play) {
            float s = mo->scale > 0 ? mo->scale : 1, h = mo->heading / 57.2957795f, c = cosf(h), sn = sinf(h);
            float centre[3] = { mo->mx, mo->my, mo->mz }, body[3] = { mo->mx, mo->my, mo->mz + 4 * s };   /* [G] the pelvis */
            float mbox[6], rbox[6], *roof = NULL;
            int mounted = mo->mode == MODE_BUILDING && mo->mount.at.building >= 0 &&
                          (uint32_t)mo->mount.at.building < v->scene.nbuildings;
            if (mounted) {
                const Building *b = &v->scene.buildings[mo->mount.at.building].b;
                memcpy(mbox, b->min, 3 * sizeof(float));
                memcpy(mbox + 3, b->max, 3 * sizeof(float));
            }
            for (uint32_t i = 0; i < v->scene.nbuildings && !roof; i++) {   /* the building it is over (0x2D6150) */
                const Building *b = &v->scene.buildings[i].b;
                if (mo->mx >= b->min[0] && mo->mx <= b->max[0] && mo->my >= b->min[1] && mo->my <= b->max[1] &&
                    mo->mz >= b->max[2] - 1) {
                    memcpy(rbox, b->min, 3 * sizeof(float));
                    memcpy(rbox + 3, b->max, 3 * sizeof(float));
                    roof = rbox;
                }
            }
            shadow_monster_update(&mo->smon, dt, centre, body, MONSTER_HALF_WIDTH * s, mounted, mbox, roof, 0);
            float rot[9] = { c, sn, 0, -sn, c, 0, 0, 0, 1 };
            shadow_monster_draw(&v->shl, &v->slight, &mo->smon, rot, MONSTER_HALF_WIDTH * s);
        }
    }
}

/* an emitter's sounds (fx.c): add3DSfx at the effect's place as it starts, moved with it, killed as it ends */
static int fx_sound(void *ctx, int event, const char *name, const float pos[3], int handle)
{
    (void)ctx;
    if (event == FXS_START)
        return sound_add3d(name, pos, 0, 0, 1);
    if (event == FXS_MOVE)
        sound_move(handle, pos);
    else
        sound_kill(handle);
    return 0;
}

/* a composite effect's camerashake part (fx.c): camshake or camshake_collapse at strength 1 where it plays */
static void fx_shake(void *ctx, const char *name, const float pos[3])
{
    Viewer *v = ctx;
    int slot = 1;
    for (int j = 2; j < 8; j++)                      /* a free one, else the oldest */
        if (!v->shakes[j].def || v->shakes[j].age > v->shakes[slot].age)
            slot = j;
    const ShakeDef *d = name && _stricmp(name, "camshake_collapse") == 0 ? &SHAKE_COLLAPSE : &SHAKE_MONSTER;
    shake_start(&v->shakes[slot], d, pos, 1, 0, &v->shake_seed);
}

/* the army each frame (army.c): the monster as it sees it, the buildings, then what it makes happen */
/* a move state of the monster by name, now (playReaction) */
static void force_state(Mon *mo, const char *name)
{
    for (int k = 0; name && mo->have_graph && k < mo->graph.n; k++)
        if (!_stricmp(mo->graph.states[k].name, name)) {
            moves_force(&mo->run, k);
            return;
        }
}

/* the bosses' host: the army's weapons */
static int boss_weapon_make(void *ctx, ArmyWeapon *w, Group g)
{
    Viewer *v = ctx;
    return v->army ? army_weapon_make(v->army, w, g) : 0;
}
static void boss_weapon_tick(void *ctx, ArmyWeapon *w, int may, const float from[3], const float dir[3], int target,
                             float dt)
{
    Viewer *v = ctx;
    if (v->army)
        army_weapon_tick(v->army, w, may, from, dir, target, dt);
}

/* the monster an event names (its slot), else player 1's (a module's event for "the monster") */
static Mon *ev_mon(Viewer *v, int slot)
{
    return &v->mon[slot >= 0 && slot < v->nmon ? slot : 0];
}

/* ChallengeDef.Points (or the boss's 5000) / players into each player's category 5 [R ChallengeStatus.scr] */
static void challenge_points_all(Viewer *v, int points)
{
    for (int p = 0; v->game && p < v->nmon; p++)
        game_add_points(v->game, p, GPT_CHALLENGE, points / v->nmon);
}

/* the bosses' frame: what they see of the monsters and the buildings, then what they did */
static void boss_step(Viewer *v, float dt, const ArmyMonster *am, int nm)
{
    if (!v->boss)
        return;
    BossMonster m[MONSTERS];
    for (int k = 0; k < nm; k++) {
        memset(&m[k], 0, sizeof m[k]);
        memcpy(m[k].pos, am[k].pos, sizeof m[k].pos);
        m[k].heading = am[k].heading;
        m[k].move = am[k].move;
        m[k].climbing = v->mon[k].mount.at.building >= 0;
        m[k].spheres = am[k].spheres;
        m[k].nspheres = am[k].nspheres;
        memcpy(m[k].box_min, am[k].box_min, sizeof m[k].box_min);
        memcpy(m[k].box_max, am[k].box_max, sizeof m[k].box_max);
    }
    static BossBuilding bl[64];
    int nb = 0;
    for (uint32_t i = 0; i < v->scene.nbuildings && nb < 64; i++, nb++) {
        const SceneBuilding *sb = &v->scene.buildings[i];
        const Building *b = &sb->b;
        BossBuilding *o = &bl[nb];
        memset(o, 0, sizeof *o);
        o->pos[0] = sb->node.m[12]; o->pos[1] = sb->node.m[13]; o->pos[2] = sb->node.m[14];
        memcpy(o->min, b->min, sizeof o->min);
        memcpy(o->max, b->max, sizeof o->max);
        o->width = b->width;
        o->storeys = b->storeys;
        o->height = b->max[2];
        o->destroyed = b->state == BUILDING_GONE || b->state >= BUILDING_CRACKING;
        o->falling = b->state >= BUILDING_CRACKING;
        o->standing = !o->destroyed;
        o->hp_frac = b->hp_max > 0 ? (float)b->hp / (float)b->hp_max : 0;
        o->inst = sb->inst;
    }
    boss_set_view(v->boss, v->view.pos[0] - 45, v->view.pos[0] + 45);   /* the view's x range [G], as the army's */
    boss_update(v->boss, dt, m, nm, bl, nb);
    BossEvent ev[64];
    int ne;
    while ((ne = boss_events(v->boss, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const BossEvent *e = &ev[i];
            Mon *mo = ev_mon(v, e->monster);
            switch (e->type) {
            case BE_EFFECT:
                if (v->fxw) {
                    int id = fx_spawn(v->fxw, e->name, e->pos, e->dir, 1);
                    if (id >= 0 && v->nbfx < 64) {   /* to follow its part or the monster, or to stop by its id */
                        const BossPart *p = e->obj >= 0 ? boss_part(v->boss, e->obj) : NULL;
                        v->bfx[v->nbfx].id = e->id;
                        v->bfx[v->nbfx].fx = id;
                        v->bfx[v->nbfx].obj = p ? e->obj : -1;
                        v->bfx[v->nbfx].monster = !p && e->monster >= 0 ? mo->slot : -1;
                        for (int k = 0; k < 3; k++)
                            v->bfx[v->nbfx].off[k] = e->pos[k] - (p ? p->pos[k] : k == 0 ? mo->mx : k == 1 ? mo->my : mo->mz);
                        v->nbfx++;
                    }
                }
                break;
            case BE_EFFECT_STOP:
                for (int k = 0; k < v->nbfx; k++)
                    if (v->bfx[k].id == e->id) {
                        fx_stop(v->fxw, v->bfx[k].fx);
                        v->bfx[k] = v->bfx[--v->nbfx];
                        break;
                    }
                break;
            case BE_SOUND: if (e->level != 0 && e->name) sound_play(e->name, e->pos, 1); break;
            case BE_REACTION: if (mo->hp > 0) force_state(mo, e->name); break;
            case BE_VEL: memcpy(mo->vel, e->v, sizeof mo->vel); break;
            case BE_HP: if (mo->hp > 0) mo->hp = fmaxf(0, fminf(200, mo->hp + e->amount)); break;
            case BE_FACING: mo->heading = e->amount * 57.2957795f; break;
            case BE_PUSH: mo->mx += e->v[0]; mo->my += e->v[1]; break;
            case BE_STATS: if (v->army) army_add_stats(v->army, mo->slot, e->tmpl, e->level); break;
            case BE_BOSS_HEALTH:                     /* the boss bar; at 0 the challenge is met: 5000 points */
                if (v->hud && hud_boss_health(v->hud, e->amount))
                    challenge_points_all(v, 5000);
                break;
            case BE_DEFEATED:                        /* New York's callback finishes the block (isBossBlockFinished) */
                if (v->game && e->name && strstr(e->name, "NewYork"))
                    game_set_boss_finished(v->game, 0);
                break;
            case BE_COLLAPSE:
                if (e->building >= 0 && (uint32_t)e->building < v->scene.nbuildings)
                    building_destroy(&v->scene.buildings[e->building].b);
                break;
            case BE_NO_AUTOCOLLAPSE:
                if (e->building >= 0 && (uint32_t)e->building < v->scene.nbuildings)
                    v->scene.buildings[e->building].b.no_autocollapse = 1;
                break;
            case BE_CELL_DAMAGE:                     /* New York's walls broken open before the generators */
                if (e->building >= 0 && (uint32_t)e->building < v->scene.nbuildings)
                    building_set_cell(&v->scene.buildings[e->building].b, e->level, e->a, e->b, e->c);
                break;
            case BE_FORCE_DAMAGE:                    /* RoofItem.bsf's forceDamage: the roof item there is knocked
                                                      * off (the adornment nearest pos, within 2 units [G]) */
                if (!v->sobj || streetobj_force_damage(v->sobj, e->name, e->pos) < 0)
                    scene_adornments_hit(&v->scene, e->pos, 2);
                break;
            case BE_CAMERA_RMH:                      /* Levels::changeTwoTiltCameraRMH: how tall the camera frames */
                v->cam.s.rep_monster_height = e->amount;
                break;
            default: break;
            }
            if (getenv("RAMPAGE_TRACE") && e->type != BE_EFFECT && e->type != BE_EFFECT_STOP && e->type != BE_SOUND)
                printf("  boss event %d (%s) part %d, %.2f\n", e->type, e->name ? e->name : "-", e->obj, e->amount);
        }
    for (int k = 0; k < v->nbfx; k++) {              /* the effects that follow their part or the monster */
        float at[3];
        if (v->bfx[k].obj >= 0) {
            const BossPart *p = boss_part(v->boss, v->bfx[k].obj);
            if (!p || !p->alive) {
                v->bfx[k].obj = -1;
                continue;
            }
            for (int j = 0; j < 3; j++)
                at[j] = p->pos[j] + v->bfx[k].off[j];
        } else if (v->bfx[k].monster >= 0) {
            const Mon *mo = &v->mon[v->bfx[k].monster];
            at[0] = mo->mx + v->bfx[k].off[0]; at[1] = mo->my + v->bfx[k].off[1]; at[2] = mo->mz + v->bfx[k].off[2];
        } else {
            if (!fx_alive(v->fxw, v->bfx[k].fx))
                v->bfx[k--] = v->bfx[--v->nbfx];
            continue;
        }
        fx_move(v->fxw, v->bfx[k].fx, at);
    }
}

/* the game's frame (game.c): what it sees (the monster's HP, the buildings standing), then what it says. The viewer
 * has no intro or walk-out choreography: a move the flow waits for is done at once, the walks and the script cameras
 * are left out [G], and a new block is built after the frame */
static void game_step(Viewer *v, float dt)
{
    if (!v->game || game_left(v->game))
        return;
    GameInput in;
    memset(&in, 0, sizeof in);
    for (int k = 0; k < v->nmon && k < GAME_PLAYERS; k++)
        in.hp[k] = v->mon[k].hp;
    for (uint32_t i = 0; i < v->scene.nbuildings; i++) {
        const Building *bb = &v->scene.buildings[i].b;
        in.buildings += bb->state == BUILDING_STANDING || bb->state == BUILDING_FLOOR_FALLING;
        float cy = 10 + v->block_shift;              /* NewYork.scr's centre (0, 10, 1), in the scene */
        if (bb->min[0] <= 0 && bb->max[0] >= 0 && bb->min[1] <= cy && bb->max[1] >= cy)
            in.centre_down = bb->state != BUILDING_STANDING && bb->state != BUILDING_FLOOR_FALLING;   /* (0, 10, 1) */
    }
    game_update(v->game, dt, &in);
    GameEvent ev[32];
    int ne;
    while ((ne = game_events(v->game, ev, 32)) > 0)
        for (int i = 0; i < ne; i++) {
            const GameEvent *e = &ev[i];
            Mon *mo = ev_mon(v, e->slot);            /* the monster slot it is for (the monster events) */
            switch (e->type) {
            case GE_BLOCK: {                         /* the next block: built when it begins (GS_BEGIN_BLOCK) */
                int k = -1;
                for (uint32_t i = 0; e->name && i < v->nblocks; i++)
                    if (!_stricmp(v->blocks[i], e->name))
                        k = (int)i;
                if (k >= 0 && k != (int)v->block) {
                    v->next_block = k;
                    v->block_shift = 41;             /* slot 1 (city.c), the block left 41 towards -y [G] */
                }
                /* the reveal's tubes in this block (0x3235F0), now if it is the one built */
                v->ntubes = reveal_block(&v->reveal, e->a, 1 << 30, v->tubes, REVEAL_LOCS);
                if (k == (int)v->block)
                    add_tubes(v);
                static RubbleHost rh;                /* the block left's rubble (0x303C90) */
                rh.tmpl = rubble_tmpl;
                rh.shape_box = rubble_box;
                rh.ctx = v;
                rubble_load(&v->rubble, v->scene.block, k >= 0 && k != (int)v->block, &rh);
                break;
            }
            case GE_STATE:
                if (e->a == GS_BLOCK_WON) {          /* 0x3255B0: the clock's seconds to "Clock", the check */
                    int vis;
                    float left = game_clock(v->game, &vis);
                    if (!challenge_block_won(&v->challenge, game_play_type(v->game), left))
                        v->challenges_lost = 1;
                    if (getenv("RAMPAGE_TRACE"))
                        printf("  challenge %s: %d/%d\n", challenge_was_met(&v->challenge) ? "met" : "not met",
                               challenge_collected(&v->challenge), challenge_required(&v->challenge));
                }
                if (e->a == GS_EXIT)
                    v->exit_game = 1;
                if (e->a == GS_BEGIN_BLOCK && v->next_block >= 0) {
                    v->game_block = v->next_block;   /* built once this frame is over */
                    v->next_block = -1;
                }
                break;
            case GE_RUBBLE_FX: {                     /* findPointInRubble(pos, 8): a point in the block left's
                                                      * rubble, 1.5 up; none, no effect */
                float loc[3];
                uint32_t *rng = game_rng(v->game);
                if (rng)                             /* the world's one state (gWorld +0x828) */
                    v->rubble.seed = *rng;
                int found = v->fxw && e->a >= 0 && e->a < 3 && rubble_find_point(&v->rubble, e->pos, e->f, loc);
                if (rng)
                    *rng = v->rubble.seed;
                if (found) {
                    float at[3] = { loc[0], loc[1] + v->block_shift, loc[2] + 1.5f };
                    int fx = (int)(2.0f * game_frand(v->game));   /* Levels.scr: drawn after the point is found */
                    const char *name = fx < 1 && loc[1] > -23 ? "Rubble_Smolder" : "Fire_Loop";
                    if (v->rubble_fx[e->a] >= 0)
                        fx_stop(v->fxw, v->rubble_fx[e->a]);
                    v->rubble_fx[e->a] = fx_spawn(v->fxw, name, at, NULL, 1);
                }
                break;
            }
            case GE_RUBBLE_FX_STOP:
                for (int k = 0; k < 3; k++) {
                    if (v->fxw && v->rubble_fx[k] >= 0)
                        fx_stop(v->fxw, v->rubble_fx[k]);
                    v->rubble_fx[k] = -1;
                }
                break;
            case GE_AI:                              /* setIsAI; a bot's destroyBuildings, the computer's players' */
                mo->is_ai = e->a || mo->drive == DRIVE_COMPUTER;
                if ((e->b || (!e->a && mo->drive == DRIVE_COMPUTER)) && mo->have_graph)
                    dir_ai_destroy(&mo->ai, &mo->graph, v->bot_seed = v->bot_seed * 69069u + 1u + (uint32_t)mo->slot);
                break;
            case GE_WALK:                            /* walkTo(pos, radius) and its way point */
                if (mo->have_ag) {
                    float to[3] = { e->pos[0], e->pos[1] + v->block_shift, e->pos[2] };
                    float wp[3] = { e->v[0], e->v[1] + v->block_shift, e->v[2] };
                    int w = dir_ai_walk_to(&mo->ai, to, e->f, 0);
                    if (w >= 0)
                        dir_ai_add_waypoint(&mo->ai, w, wp, e->f);
                }
                break;
            case GE_CAMERA: {
                float at[3] = { e->pos[0], e->pos[1] + v->block_shift, e->pos[2] };
                float to[3] = { e->v[0], e->v[1] + v->block_shift, e->v[2] };
                dircam_game_event(&v->cd, e->name, e->f, at, to, e->a);
                break;
            }
            case GE_RESET:
                if (e->type == GE_RESET)
                    dir_ai_clear(&mo->ai);            /* clearAllGoals */
                /* fall through */
            case GE_PLACE:
                if (e->a == 1 && e->type == GE_PLACE) {  /* SkipResultsNI's -41: the block left; here it stays */
                    mo->mx += e->pos[0]; mo->my += e->pos[1] + v->block_shift; mo->mz += e->pos[2];
                } else if (e->a == 1 || e->type == GE_PLACE) {
                    mo->mx = e->pos[0]; mo->my = e->pos[1] + v->block_shift; mo->mz = e->pos[2];
                    if (e->type == GE_RESET)
                        mo->heading = e->f * 57.2957795f;
                }
                break;
            case GE_MOVE:                            /* playMove: a goal of the AI; without one, done at once */
                if (mo->is_ai && mo->have_ag && mo->have_graph)
                    dir_ai_play_move(&mo->ai, e->name ? moves_find(&mo->graph, e->name) : -1, e->a);
                else if (e->a == 1)
                    game_move_done(v->game, e->slot);
                break;
            case GE_AI_ACTIVATE:
                if (v->army) army_activate(v->army, 1);
                parade_activate(v->parade, 1);
                break;
            case GE_HUD:                             /* every player's HUD */
                for (int k = 0; k < v->nmon; k++)
                    if (v->mon[k].hud)
                        hud_show(v->mon[k].hud, (float)e->a);
                break;
            case GE_GUI: if (v->hud && e->name) hud_load_interface(v->hud, e->name); break;
            case GE_RESULTS:                         /* WorldGuiDriver::startResults: filled now (the points are
                                                      * cleared once it is done) */
                if (v->hud) {
                    HudResults r;
                    memset(&r, 0, sizeof r);
                    r.play_type = game_play_type(v->game);   /* GP_* and HUD_PLAY_* agree */
                    r.num_players = v->nmon;
                    for (int p = 0; p < r.num_players; p++) {
                        for (int c = 0; c < 7; c++) {
                            r.points[p][c] = game_points(v->game, p, c);
                            r.kills[p][c] = game_kills(v->game, p, c);
                        }
                        r.monsters[p] = v->mon[p].monster >= 0 ? v->monsters[v->mon[p].monster] : NULL;
                        r.blocks_won[p] = game_blocks_won(v->game, p);
                        r.cities_won[p] = game_cities_won(v->game, p);
                    }
                    r.king_block = game_king(v->game, 0);
                    r.king_city = game_king(v->game, 1);
                    r.king_world = game_king(v->game, 2);
                    r.blocks_to_win = game_blocks_to_win(v->game);
                    r.cities_to_win = game_cities_to_win(v->game);
                    int ci = game_city_index(v->game);
                    r.next_city = ci >= 0 && ci < 6 ? CAMPAIGN[ci + 1] : NULL;
                    r.clock_time = game_clock(v->game, NULL);
                    r.clock_limit = game_clock_limit(v->game);
                    r.block_played = game_block_name(v->game, 0);
                    r.block_current = game_block_name(v->game, 1);
                    for (uint32_t k = 0; k < v->scene.nbuildings; k++)
                        r.buildings += v->scene.buildings[k].b.state == BUILDING_STANDING ||
                                       v->scene.buildings[k].b.state == BUILDING_FLOOR_FALLING;
                    r.city_destroyed = game_city_destroyed(v->game);
                    r.city = CITIES[v->city_index];
                    r.city_score = game_city_total(v->game);
                    r.par_score = game_par_score(v->game);
                    r.city_flags = game_city_flags(v->game) | (v->challenges_lost ? 0 : CITY_FLAG_CHALLENGES);
                    r.monster_revealed = reveal_was_revealed(&v->reveal);
                    if (e->a == GR_SCREEN_RESULTS && r.play_type == GP_CAMPAIGN) {   /* Results.bsf: updateProfile,
                                                      * saveProfile (1.5 s in) */
                        profile_block_won(&v->prof, r.city_flags | PC_AVAILABLE, r.city_score, game_block(v->game, NULL),
                                          r.city_destroyed);
                        profile_update(&v->prof);
                        if (v->prof_write && v->prof_path &&
                            profile_write(&v->prof, v->prof_path, 0, (uint64_t)time(NULL) * 1000) < 0)
                            printf("could not write the profile %s\n", v->prof_path);
                    }
                    hud_results(v->hud, e->a, &r);
                }
                break;
            case GE_REVIVE:                          /* cancelRespawn: full HP where it is */
                mo->hp = 200;
                mo->defeated = 0;
                mo->human = mo->dropping = 0;
                mo->scale = mo->scale_to = 1;
                if (mon_in(mo))
                    mon_in(mo)->hidden = 0;
                if (mo->streaker >= 0 && v->scene.folk)
                    people_remove(v->scene.folk, mo->streaker);
                mo->streaker = -1;
                if (e->a == 1) {
                    edible_power_init(&mo->power);
                    power_from_profile(mo);
                    mo->energy = 0;
                }
                break;
            default: break;
            }
            if (getenv("RAMPAGE_TRACE") && e->type != GE_MOVE)
                printf("  game event %d (%s) a %d, state %d\n", e->type, e->name ? e->name : "-", e->a,
                       game_state(v->game));
        }
}

/* after the screens ran: a results screen's answer to the game, the sounds they start and stop */
static void hud_frame(Viewer *v, int after)
{
    if (!after || !v->hud)
        return;
    int r = hud_result(v->hud);
    if (r == GR_CONTINUE && v->game && game_state(v->game) == GS_GAME_OVER) {   /* back to the checkpoint */
        profile_restore_checkpoint(&v->prof);
        profile_new_city(&v->prof);
    }
    if (r == GR_CONTINUE && v->game && game_state(v->game) == GS_GAME_OVER && reveal_was_revealed(&v->reveal))
        extra_hide(v, reveal_continue(&v->reveal));  /* 0x323450: the tube comes back, the monster hides */
    if (r && v->game)
        game_set_result(v->game, r);
    float sv, mv;
    int rumble;
    switch (hud_pause_result(v->hud)) {
    case HUD_PAUSE_QUIT:                             /* ConfirmQuit's Yes: gWorld.restartShell */
        if (v->game)
            game_restart_shell(v->game);
        else
            v->exit_game = 1;
        break;
    case HUD_PAUSE_SETTINGS:                         /* the profile's volumes (0 .. 0.75: the game's full one) */
        hud_settings(v->hud, &sv, &mv, &rumble);
        profile_set_sound_volume(&v->prof, sv);      /* written with the next Results */
        profile_set_music_volume(&v->prof, mv);
        profile_set_rumble(&v->prof, rumble, 1);
        v->sfx_vol = sv / 0.75f;
        v->music_vol = v->music_now = mv / 0.75f;
        sound_volume(v->sfx_vol, v->music_now);
        break;
    default: break;
    }
    HudCall calls[16];
    int nc = hud_calls(v->hud, calls, 16);
    for (int i = 0; i < nc; i++)                     /* gSndDev's volumes (no fades here) */
        switch (calls[i].call) {
        case HUD_CALL_MUSIC_VOLUME: v->music_now = calls[i].value / 0.75f; sound_volume(v->sfx_vol, v->music_now); break;
        case HUD_CALL_SFX_VOLUME: v->sfx_vol = calls[i].value / 0.75f; sound_volume(v->sfx_vol, v->music_now); break;
        case HUD_CALL_SAVE_SETTINGS:
            hud_settings(v->hud, &sv, &mv, &rumble);
            profile_set_sound_volume(&v->prof, sv);
            profile_set_music_volume(&v->prof, mv);
            profile_set_rumble(&v->prof, rumble, 1);
            v->sfx_vol = sv / 0.75f;
            v->music_vol = v->music_now = mv / 0.75f;
            sound_volume(v->sfx_vol, v->music_now);
            break;
        case HUD_CALL_PLAY_MUSIC:
            if (!music_playing() && v->scene.music)
                music_play(v->scene.music);
            break;
        default: break;                              /* the vibration, the cheats: no pad here */
        }
    HudSound hs[16];
    int n = hud_sounds(v->hud, hs, 16);
    for (int i = 0; i < n; i++) {
        if (hs[i].kill)
            sound_kill_name(hs[i].name);
        else
            sound_play(hs[i].name, NULL, 1);
    }
}

/* Monsters::startRage on screen: the RAMPAGE banner (Rage.ifc) */
static void rage_started(Mon *mo)
{
    Viewer *v = mo->v;
    if (v->hud)
        hud_banner_rage(v->hud);
    dircam_rage(&v->cd, mo->slot);                          /* the script camera on the monster for 1.5 s */
}

/* gCommon.addStats on the monster (0x2C51A0): HP and energy by the rules edible.c keeps (none at 0 HP; while raging
 * only a full Energy restarts it); a full one starts the rage: Monsters::startRage, slot 54 [G: its slot] */
static void monster_stats(Mon *mo, float health, float energy)
{
    if (edible_power_add_stats(&mo->power, &mo->hp, 200, health, energy, others_full(mo))) {
        edible_power_start_rage(&mo->power, others_full(mo));
        if (!mo->defeated)
            mo->pending_slot = 54;
        rage_started(mo);
        if (getenv("RAMPAGE_TRACE"))
            printf("  rage\n");
    }
    mo->energy = mo->power.energy;
}

/* the cell a monster call's (x, y, z) names (getBuildingOccupant, people_probe): its building, storey as built and
 * column, and whether a person is at its window; the edibles' answer to the call, -1 none */
static int edible_call(Mon *mo, const char *call, const float *xyz)
{
    Viewer *v = mo->v;
    if (!v->edibles)
        return -1;
    PeopleBox boxes[64];
    int nb = scene_people_boxes(&v->scene, boxes, 64), b = -1, ls = -1, col = -1;
    float at[3] = { mo->mx, mo->my, mo->mz };
    int found = people_probe(boxes, nb, at, xyz, mo->mirror, &b, &ls, &col);
    int o = found && b >= 0 && ls >= 0 && ls < v->scene.buildings[b].b.n ? v->scene.buildings[b].b.live[ls] : -1;
    int person = o >= 0 && v->scene.folk && people_window_at(v->scene.folk, b, ls, col) >= 0;
    return edible_monster_call(v->edibles, call, o >= 0 ? b : -1, o, col, person, mo->slot,
                               mo->have_graph ? mo->graph.states[mo->run.state].name : NULL);
}

/* the edibles' frame: their clocks, then what they did */
static void edible_step(Viewer *v, float dt)
{
    if (!v->edibles)
        return;
    edible_update(v->edibles, dt);
    for (int k = 0; k < v->nmon; k++) {              /* each monster's rage and speed */
        Mon *mo = &v->mon[k];
        if (edible_power_update(&mo->power, dt, mo->hp) && getenv("RAMPAGE_TRACE"))
            printf("  the rage ends\n");
        mo->energy = mo->power.energy;
    }
    EdibleEvent ev[32];
    int ne;
    while ((ne = edible_events(v->edibles, ev, 32)) > 0)
        for (int i = 0; i < ne; i++) {
            const EdibleEvent *e = &ev[i];
            Mon *mo = ev_mon(v, e->monster);
            switch (e->type) {
            case EE_EFFECT:
                if (v->fxw) {
                    int id = fx_spawn(v->fxw, e->name, e->pos, NULL, 1);
                    if (id >= 0 && e->edible >= 0 && v->nefx < 32) {
                        const Edible *it = edible_get(v->edibles, e->edible);
                        v->efx[v->nefx].fx = id;
                        v->efx[v->nefx].edible = e->edible;
                        for (int k = 0; k < 3; k++)
                            v->efx[v->nefx].off[k] = it ? e->pos[k] - it->pos[k] : 0;
                        v->nefx++;
                    }
                }
                break;
            case EE_EFFECT_STOP: if (v->fxw) fx_stop(v->fxw, e->id); break;
            case EE_SOUND: sound_play(e->name, e->pos, 1); break;
            case EE_REACTION:                        /* playReaction: the state of that name, only while alive */
                if (mo->hp > 0 && mo->have_graph && e->name)
                    for (int k = 0; k < mo->graph.n; k++)
                        if (!_stricmp(mo->graph.states[k].name, e->name)) {
                            moves_force(&mo->run, k);
                            break;
                        }
                break;
            case EE_SPEED: edible_power_speed(&mo->power, e->amount, e->v[0]); break;
            case EE_STATS:                           /* the score through the army, HP and energy here */
                if (v->army)
                    army_add_stats(v->army, mo->slot, e->tmpl, e->level);
                monster_stats(mo, e->v[0], e->v[1]);
                break;
            case EE_BANNER:
                if (v->hud)
                    hud_float(v->hud, (int)e->amount, e->pos, 0, 1.0f);
                break;
            case EE_MESSAGE:                         /* "Instant Rampage", "Speed Increase", ... (Rage.ifc) */
                if (v->hud)
                    hud_banner_text(v->hud, e->name, e->v, e->amount, e->name2);
                break;
            case EE_REVEAL: {                        /* the cryotube: gWorld.startReveal, "<monster> Unlocked" */
                int slot, i = reveal_start(&v->reveal, v->nmon, &v->roster, &slot);
                if (i >= 0)                          /* unlocked: the profile's monster flags */
                    profile_set_monster_flags(&v->prof, 0, i, v->roster.flags[i]);
                if (v->hud && i >= 0)
                    hud_banner_reveal(v->hud, v->roster.name[i]);
                if (getenv("RAMPAGE_TRACE"))
                    printf("  revealed: %s\n", i >= 0 ? v->roster.name[i] : "-");
                if (i >= 0 && slot >= 0 && slot == v->extra)   /* shown: the walk-on (0x2D40B0) */
                    reveal_walk_start(&v->rwalk, slot, e->pos);
                else
                    reveal_stop(&v->reveal);
                break;
            }
            default: break;
            }
            if (getenv("RAMPAGE_TRACE") && e->type != EE_EFFECT && e->type != EE_EFFECT_STOP)
                printf("  edible event %d (%s) from %d\n", e->type, e->name ? e->name : "-", e->edible);
        }
    for (int k = 0; k < v->nefx; k++) {              /* the effects that follow their edible, while it is out */
        const Edible *it = edible_get(v->edibles, v->efx[k].edible);
        if (!it || it->phase == EP_GONE || !fx_alive(v->fxw, v->efx[k].fx)) {
            v->efx[k--] = v->efx[--v->nefx];
            continue;
        }
        float at[3] = { it->pos[0] + v->efx[k].off[0], it->pos[1] + v->efx[k].off[1], it->pos[2] + v->efx[k].off[2] };
        fx_move(v->fxw, v->efx[k].fx, at);
    }
}

/* the edibles' host: a front cell's state and where its edible stands */
static int edible_cell_state(void *ctx, int building, int storey, int column)
{
    const Viewer *v = ctx;
    if (building < 0 || (uint32_t)building >= v->scene.nbuildings)
        return -1;
    const Building *b = &v->scene.buildings[building].b;
    int ci = building_cell_index(b, FACE_FRONT, column);
    return ci < 0 || storey < 0 || storey >= b->storeys || building_live_of(b, storey) < 0 ? -1 : b->cell[storey][ci].state;
}
static int edible_cell_where(void *ctx, int building, int storey, int column, float out[3])
{
    const Viewer *v = ctx;
    if (building < 0 || (uint32_t)building >= v->scene.nbuildings)
        return 0;
    const Building *b = &v->scene.buildings[building].b;
    int ls = storey >= 0 && storey < b->storeys ? building_live_of(b, storey) : -1;
    if (ls < 0)
        return 0;
    edible_cell_place(b->min, column, ls, out);
    return 1;
}

/* 0x2D6A50: a hit on the monster: HP off, the reaction by its level and the knock */
static void monster_hit(Mon *mo, const char *name, float amount, int level, const float knock[3], const float from[3])
{
    if (mo->defeated)
        return;
    mo->hp = fmaxf(0, mo->hp - amount);
    float heading = mo->heading / 57.2957795f, at[3] = { mo->mx, mo->my, mo->mz };
    int slot = army_reaction_slot(level, knock, mo->mode, army_hit_behind(at, heading, from));
    if (slot >= 0) {
        mo->friction = monster_knock_shot(mo->vel, knock);   /* 0x2D9650: per axis the larger of knock and velocity */
        mo->pending_slot = slot;
    }
    if (mo->hp <= 0) {                                /* 0x2D9364: at 0 HP slot 67, React_Defeated */
        mo->defeated = 1;
        mo->pending_slot = 67;
        memset(mo->vel, 0, sizeof mo->vel);
    }
    if (getenv("RAMPAGE_TRACE"))
        printf("  hit by %s: %.1f, HP %.0f, slot %d\n", name ? name : "?", amount, mo->hp, slot);
}

/* the vehicles' frame (vehicle.c): what they see of the monster and the buildings, then what they did */
static void vehicle_step(Viewer *v, float dt, const ArmyMonster *m, int nm, const ArmyBox *boxes, int nb)
{
    if (!v->veh)
        return;
    vehicle_update(v->veh, dt, m, nm, boxes, nb);
    VehicleEvent ev[64];
    int ne;
    while ((ne = vehicle_events(v->veh, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const VehicleEvent *e = &ev[i];
            Mon *mo = ev_mon(v, e->monster);
            switch (e->type) {
            case VE_EFFECT:
                if (v->fxw) {
                    int id = fx_spawn(v->fxw, e->name, e->pos, e->dir, 1);
                    if (id >= 0 && v->nvfx < 64) {   /* to follow its object, or to stop by its handle */
                        const VehObj *o = e->obj >= 0 ? vehicle_obj(v->veh, e->obj) : NULL;
                        v->vfx[v->nvfx].id = e->id;
                        v->vfx[v->nvfx].fx = id;
                        v->vfx[v->nvfx].obj = o ? e->obj : -1;
                        for (int k = 0; k < 3; k++)
                            v->vfx[v->nvfx].off[k] = o ? e->pos[k] - o->pos[k] : 0;
                        v->nvfx++;
                    }
                }
                break;
            case VE_EFFECT_STOP:
                for (int k = 0; k < v->nvfx; k++)
                    if (v->vfx[k].id == e->id) {
                        fx_stop(v->fxw, v->vfx[k].fx);
                        v->vfx[k] = v->vfx[--v->nvfx];
                        break;
                    }
                break;
            case VE_SOUND: sound_play(e->name, e->pos, 1); break;
            case VE_HIT: monster_hit(mo, e->name, e->amount, e->level, e->v, e->pos); break;
            case VE_REACTION: mo->pending_slot = e->level; break;
            case VE_SET_VEL: memcpy(mo->vel, e->v, sizeof mo->vel); break;
            case VE_HP: if (mo->hp > 0) mo->hp = fmaxf(0, fminf(200, mo->hp + e->amount)); break;
            case VE_STATS:                           /* id 1: the army applied it (its events follow) */
                if (e->id == 0) {
                    monster_stats(mo, e->v[0], e->v[1]);
                    challenge_stats(v, e->name, (int)e->amount);
                }
                break;
            case VE_REMOVED:                         /* out of whichever hand held it */
                for (int k = 0; k < v->nmon; k++)
                    if (e->obj == v->mon[k].held_vehicle)
                        v->mon[k].held_vehicle = -1;
                break;
            case VE_IMPACT: impact_route(v, e->id, e->level, e->pos, e->amount); break;
            default: break;
            }
            if (getenv("RAMPAGE_TRACE") && e->type != VE_EFFECT && e->type != VE_EFFECT_STOP)
                printf("  vehicle event %d (%s) from %d\n", e->type, e->name ? e->name : "-", e->obj);
        }
    for (int k = 0; k < v->nvfx; k++) {              /* the effects that follow their vehicle, while it is there */
        if (v->vfx[k].obj < 0) {
            if (!fx_alive(v->fxw, v->vfx[k].fx))
                v->vfx[k--] = v->vfx[--v->nvfx];
            continue;
        }
        const VehObj *o = vehicle_obj(v->veh, v->vfx[k].obj);
        if (!o || o->phase == VP_GONE) {
            v->vfx[k].obj = -1;                      /* it stays where it is */
            continue;
        }
        float at[3] = { o->pos[0] + v->vfx[k].off[0], o->pos[1] + v->vfx[k].off[1], o->pos[2] + v->vfx[k].off[2] };
        fx_move(v->fxw, v->vfx[k].fx, at);
    }
    for (int k = 0; k < v->nmon; k++) {              /* one in a monster's hand */
        Mon *mo = &v->mon[k];
        Mat4 h;
        if (mo->held_vehicle >= 0 && attach_world(mo, "Helper R Hand", &h) == 0)
            vehicle_hold(v->veh, mo->held_vehicle, h.m, mo->mirror);
    }
}

/* the rail objects' frame (rail.c): what they see of the monster and the buildings, then what they did */
static void rail_step(Viewer *v, float dt, const ArmyMonster *m, int nm)
{
    if (!v->rail)
        return;
    static RailBox rb[64];
    int nb = 0;
    for (uint32_t i = 0; i < v->scene.nbuildings && nb < 64; i++, nb++) {
        const Building *b = &v->scene.buildings[i].b;
        memcpy(rb[nb].min, b->min, sizeof rb[nb].min);
        memcpy(rb[nb].max, b->max, sizeof rb[nb].max);
        rb[nb].destroyed = b->state == BUILDING_GONE;
        rb[nb].falling = b->collapse != -1;
    }
    rail_update(v->rail, dt, m, nm, rb, nb);
    for (int k = 0; k < v->nmon; k++) {
        Mon *mo = &v->mon[k];
        if (mo->pending_slot < 0)
            mo->rail_knock = 0;
        else if (mo->rail_knock == 1)                 /* the frame after the knock: the repeats are dropped */
            mo->rail_knock = 2;
    }
    RailEvent ev[64];
    int ne;
    while ((ne = rail_events(v->rail, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const RailEvent *e = &ev[i];
            Mon *mo = ev_mon(v, e->monster);
            const RailObj *o = e->obj >= 0 ? rail_obj(v->rail, e->obj) : NULL;
            switch (e->type) {
            case RE_EFFECT:
                if (v->fxw) {
                    int id = fx_spawn(v->fxw, e->name, e->pos, e->dir, 1);
                    if (id >= 0 && v->nrfx < 64) {
                        v->rfx[v->nrfx].id = e->id;
                        v->rfx[v->nrfx].fx = id;
                        v->rfx[v->nrfx].obj = o ? e->obj : -1;
                        for (int k = 0; k < 3; k++)
                            v->rfx[v->nrfx].off[k] = o ? e->pos[k] - o->pos[k] : 0;
                        v->nrfx++;
                    }
                }
                break;
            case RE_EFFECT_STOP:
                for (int k = 0; k < v->nrfx; k++)
                    if (v->rfx[k].id == e->id) {
                        fx_stop(v->fxw, v->rfx[k].fx);
                        v->rfx[k] = v->rfx[--v->nrfx];
                        break;
                    }
                break;
            case RE_SOUND: {
                int h = sound_add3d(e->name, e->pos, 0, 0, 1);
                if (h && v->nrsnd < 64) {
                    v->rsnd[v->nrsnd].id = e->id;
                    v->rsnd[v->nrsnd].h = h;
                    v->rsnd[v->nrsnd++].obj = e->obj;
                }
                break;
            }
            case RE_SOUND_STOP:
                for (int k = 0; k < v->nrsnd; k++)
                    if (v->rsnd[k].id == e->id) {
                        sound_kill(v->rsnd[k].h);
                        v->rsnd[k] = v->rsnd[--v->nrsnd];
                        break;
                    }
                break;
            /* a knock-down is asked again every frame until its slot starts (the viewer takes pending_slot at the
             * clip's next event): one is enough [G] */
            case RE_REACTION: if (mo->pending_slot < 0) { mo->pending_slot = e->level; mo->rail_knock = 1; } break;
            case RE_SET_VEL: if (mo->rail_knock < 2) memcpy(mo->vel, e->v, sizeof mo->vel); break;
            case RE_HP: if (mo->hp > 0 && mo->rail_knock < 2) mo->hp = fmaxf(0, fminf(200, mo->hp + e->amount)); break;
            case RE_PUSH: mo->mx += e->v[0]; mo->my += e->v[1]; break;
            case RE_IMPACT: impact_route(v, e->id, e->level, e->pos, e->amount); break;
            case RE_STATS:
                if (e->id == 0) {
                    monster_stats(mo, e->v[0], e->v[1]);
                    challenge_stats(v, e->name, (int)e->amount);
                }
                break;
            case RE_REMOVED:
                for (int k = 0; k < v->nmon; k++)
                    if (e->obj == v->mon[k].held_rail)
                        v->mon[k].held_rail = -1;
                break;
            default: break;
            }
            if (getenv("RAMPAGE_TRACE") && e->type != RE_PUSH && e->type != RE_SOUND && e->type != RE_SOUND_STOP)
                printf("  rail event %d (%s) from %d (%s) at %.1f %.1f %.1f\n", e->type, e->name ? e->name : "-", e->obj,
                       o && o->name ? o->name : "-", e->pos[0], e->pos[1], e->pos[2]);
        }
    for (int k = 0; k < v->nrfx; k++) {              /* the effects that follow their object, while it is there */
        if (v->rfx[k].obj < 0) {
            if (!fx_alive(v->fxw, v->rfx[k].fx))
                v->rfx[k--] = v->rfx[--v->nrfx];
            continue;
        }
        const RailObj *o = rail_obj(v->rail, v->rfx[k].obj);
        if (!o || o->phase == RP_GONE) {
            v->rfx[k].obj = -1;
            continue;
        }
        float at[3] = { o->pos[0] + v->rfx[k].off[0], o->pos[1] + v->rfx[k].off[1], o->pos[2] + v->rfx[k].off[2] };
        fx_move(v->fxw, v->rfx[k].fx, at);
    }
    for (int k = 0; k < v->nrsnd; k++) {             /* the sounds that follow theirs */
        const RailObj *o = rail_obj(v->rail, v->rsnd[k].obj);
        if (o && o->phase != RP_GONE)
            sound_move(v->rsnd[k].h, o->pos);
    }
    for (int k = 0; k < v->nmon; k++) {
        Mon *mo = &v->mon[k];
        Mat4 h;
        if (mo->held_rail >= 0 && attach_world(mo, "Helper R Hand", &h) == 0)
            rail_hold(v->rail, mo->held_rail, h.m, mo->mirror);
    }
}

/* the balloons', bikers' and food carts' frame (parade.c): what they see of the monster and the camera, then what
 * they did; the one in the monster's hand follows it */
static void parade_step(Viewer *v, float dt, const ArmyMonster *m, int nm)
{
    if (!v->parade)
        return;
    parade_update(v->parade, dt, m, m ? nm : 0, v->view.pos[0]);
    ParadeEvent ev[64];
    int ne;
    while ((ne = parade_events(v->parade, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const ParadeEvent *e = &ev[i];
            Mon *mo = ev_mon(v, e->monster);
            switch (e->type) {
            case PAE_EFFECT:
                if (v->fxw) {
                    int id = fx_spawn(v->fxw, e->name, e->pos, NULL, 1);
                    const ParadeObj *o = e->obj >= 0 ? parade_obj(v->parade, e->obj) : NULL;
                    if (id >= 0 && o && v->npfx < 32) {   /* attached: it follows its object while it is there */
                        v->pfx[v->npfx].fx = id;
                        v->pfx[v->npfx].obj = e->obj;
                        for (int k = 0; k < 3; k++)
                            v->pfx[v->npfx].off[k] = e->pos[k] - o->pos[k];
                        v->npfx++;
                    }
                }
                break;
            case PAE_SOUND: sound_play(e->name, e->pos, 1); break;
            case PAE_MONSTER_SOUND: {
                float at[3] = { mo->mx, mo->my, mo->mz };
                sound_play(e->name, at, 1);
                break;
            }
            case PAE_STATS:                          /* gCommon.addStats: the score through the army (its AE_SCORE
                                                      * counts the challenge), HP and energy */
                if (v->army)
                    army_add_stats(v->army, mo->slot, e->tmpl, e->level);
                else {
                    monster_stats(mo, e->v[0], e->v[1]);
                    challenge_stats(v, e->name, (int)e->amount);
                }
                break;
            case PAE_POINTS:                         /* gCommon.addPoints: the category only */
                if (v->game)
                    game_add_points(v->game, mo->slot, e->level, (int)e->amount);
                break;
            case PAE_FART: {                         /* Monsters::timedDoFart -> doFart */
                Mat4 n;
                face_play(mo, "Expression_Smile", 0, 0, 1);
                if (v->fxw && scene_monster_node(&v->scene, mo->slot, "fxNode_Fart", &n) == 0) {
                    float at[3] = { n.m[12], n.m[13], n.m[14] };
                    fx_spawn(v->fxw, "Fart", at, NULL, 1);
                }
                mo->vel[2] += 20;                     /* addVel((0, 0, 20), 1) */
                break;
            }
            case PAE_WEAPON:                         /* the biker's bottle: the army's person weapons */
                if (v->army) {
                    const ParadeObj *o = parade_obj(v->parade, e->obj);
                    army_person_weapon(v->army, 1000 + e->obj, e->name, o ? o->world : NULL, e->monster >= 0 ? e->monster : 0);
                }
                break;
            case PAE_REMOVED:
                for (int k = 0; k < v->nmon; k++)
                    if (e->obj == v->mon[k].held_parade)
                        v->mon[k].held_parade = -1;
                break;
            case PAE_IMPACT: impact_route(v, e->id, e->level, e->pos, e->amount); break;
            default: break;
            }
            if (getenv("RAMPAGE_TRACE"))
                printf("  parade event %d (%s) from %d\n", e->type, e->name ? e->name : "-", e->obj);
        }
    for (int k = 0; k < v->npfx; k++) {              /* the effects that follow their object, while it is there */
        const ParadeObj *o = parade_obj(v->parade, v->pfx[k].obj);
        if (!o || o->phase == PAP_GONE || !fx_alive(v->fxw, v->pfx[k].fx)) {
            v->pfx[k--] = v->pfx[--v->npfx];
            continue;
        }
        float at[3] = { o->pos[0] + v->pfx[k].off[0], o->pos[1] + v->pfx[k].off[1], o->pos[2] + v->pfx[k].off[2] };
        fx_move(v->fxw, v->pfx[k].fx, at);
    }
    for (int i = 0, n = parade_count(v->parade); v->army && i < n; i++) {   /* the bikers' weapons go with them */
        const ParadeObj *o = parade_obj(v->parade, i);
        if (o->kind == PAK_BIKER && o->weapon_on)
            army_person_moved(v->army, 1000 + i, o->world);
    }
    for (int k = 0; k < v->nmon; k++) {              /* one in a monster's hand */
        Mon *mo = &v->mon[k];
        Mat4 h;
        if (mo->held_parade >= 0 && attach_world(mo, "Helper R Hand", &h) == 0)
            parade_hold(v->parade, mo->held_parade, h.m, mo->mirror, dt);
    }
}

/* the street and roof objects' frame (streetobj.c): the monster's body against them (TakeDamage, Collision), what
 * they see of it and of the buildings, then what they did; they are drawn where they are now */
static void street_step(Viewer *v, float dt)
{
    if (!v->sobj)
        return;
    StreetMonster me[MONSTERS];
    int nm = have_monster(v) ? v->nmon : 0;
    for (int k = 0; k < nm; k++) {
        Mon *mo = &v->mon[k];
        me[k] = (StreetMonster){ { mo->mx, mo->my, mo->mz }, mo->heading / 57.2957795f };
        if (mo->monster >= 0 && mo->have_graph) {    /* its box: 3 each way across, 8.5 tall */
            float lo[3] = { mo->mx - MONSTER_HALF_WIDTH, mo->my - MONSTER_HALF_WIDTH, mo->mz };
            float hi[3] = { mo->mx + MONSTER_HALF_WIDTH, mo->my + MONSTER_HALF_WIDTH, mo->mz + MONSTER_HEIGHT };
            streetobj_touch(v->sobj, k, &me[k], lo, hi);
        }
    }
    static StreetBuilding sbs[64];
    int nb = 0;
    for (uint32_t i = 0; i < v->scene.nbuildings && nb < 64; i++, nb++) {
        memcpy(sbs[nb].min, v->scene.buildings[i].b.min, sizeof sbs[nb].min);
        memcpy(sbs[nb].max, v->scene.buildings[i].b.max, sizeof sbs[nb].max);
        sbs[nb].falling = v->scene.buildings[i].b.collapse >= 0;
    }
    streetobj_update(v->sobj, dt, nm ? me : NULL, nm, sbs, nb);
    StreetEvent ev[64];
    int ne;
    while ((ne = streetobj_events(v->sobj, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const StreetEvent *e = &ev[i];
            int k = e->obj >= 0 ? v->street_of[e->obj] : -1;
            switch (e->type) {
            case SE_EFFECT:
                if (v->fxw) {
                    int id = fx_spawn(v->fxw, e->name, e->pos, NULL, 1);
                    if (id >= 0 && v->nsofx < 64) {
                        v->sofx[v->nsofx].id = e->id;
                        v->sofx[v->nsofx].fx = id;
                        v->sofx[v->nsofx].obj = e->level ? e->obj : -1;
                        v->sofx[v->nsofx].node = e->node;
                        v->sofx[v->nsofx].node_id = e->node_id;
                        v->sofx[v->nsofx].mnode[0] = 0;
                        memcpy(v->sofx[v->nsofx].off, e->off, sizeof e->off);
                        v->nsofx++;
                    }
                }
                break;
            case SE_EFFECT_STOP:
                for (int j = 0; j < v->nsofx; j++)
                    if (v->sofx[j].id == e->id) {
                        fx_stop(v->fxw, v->sofx[j].fx);
                        v->sofx[j] = v->sofx[--v->nsofx];
                        break;
                    }
                break;
            case SE_SOUND: sound_play(e->name, e->pos, 1); break;
            case SE_SHAPE: scene_street_shape(&v->scene, k, e->name); break;
            case SE_STATS:                           /* gCommon.addStats(obj, monster, 0) */
                if (v->army)
                    army_add_stats(v->army, e->monster, e->tmpl, 0);
                else
                    challenge_stats(v, e->name, (int)e->amount);
                break;
            case SE_MONSTER_EFFECT:                  /* on the monster's node: fxNode_Belch, fxNode_Shoulder1 .. */
                for (int n = 1; v->fxw && n <= (e->level ? 20 : 1); n++) {
                    char node[32];
                    Mat4 m;
                    if (e->level) snprintf(node, sizeof node, "%s%d", e->node, n);
                    else snprintf(node, sizeof node, "%s", e->node);
                    int mk = ev_mon(v, e->monster)->slot;
                    if (scene_monster_node(&v->scene, mk, node, &m) != 0)
                        break;
                    int id = fx_spawn(v->fxw, e->name, m.m + 12, NULL, 1);
                    if (id >= 0 && v->nsofx < 64) {
                        v->sofx[v->nsofx].id = 0;
                        v->sofx[v->nsofx].mon = mk;
                        v->sofx[v->nsofx].fx = id;
                        v->sofx[v->nsofx].obj = -1;
                        v->sofx[v->nsofx].node = NULL;
                        snprintf(v->sofx[v->nsofx].mnode, sizeof v->sofx[v->nsofx].mnode, "%s", node);
                        v->nsofx++;
                    }
                }
                break;
            default: break;
            }
            if (getenv("RAMPAGE_TRACE"))
                printf("  street event %d (%s) from %d\n", e->type, e->name ? e->name : "-", e->obj);
        }
    for (int i = 0, n = streetobj_count(v->sobj); i < n; i++) {   /* drawn where they are now */
        const StreetObj *o = streetobj_get(v->sobj, i);
        if (o->moved)
            scene_street_place(&v->scene, v->street_of[i], o->world, !o->alive);
    }
    for (int j = 0; j < v->nsofx; j++) {             /* the effects that follow */
        float at[3];
        Mat4 m;
        if (v->sofx[j].mnode[0]) {
            if (scene_monster_node(&v->scene, v->sofx[j].mon, v->sofx[j].mnode, &m) != 0 || !fx_alive(v->fxw, v->sofx[j].fx)) {
                v->sofx[j--] = v->sofx[--v->nsofx];
                continue;
            }
            memcpy(at, m.m + 12, sizeof at);
        } else if (v->sofx[j].obj >= 0) {
            const StreetObj *o = streetobj_get(v->sobj, v->sofx[j].obj);
            int k = v->street_of[v->sofx[j].obj];
            if (!o || !o->alive) {
                v->sofx[j].obj = -1;                 /* it stays where it is */
                continue;
            }
            if ((v->sofx[j].node || v->sofx[j].node_id > 0) &&
                scene_instance_node(&v->scene, v->scene.street[k].model, v->sofx[j].node, v->sofx[j].node_id, m.m) == 0) {
                for (int r = 0; r < 3; r++)
                    at[r] = m.m[12 + r] + m.m[r] * v->sofx[j].off[0] + m.m[4 + r] * v->sofx[j].off[1] +
                            m.m[8 + r] * v->sofx[j].off[2];
            } else
                streetobj_point(v->sobj, v->sofx[j].obj, v->sofx[j].off, at);
        } else {
            if (!fx_alive(v->fxw, v->sofx[j].fx))
                v->sofx[j--] = v->sofx[--v->nsofx];
            continue;
        }
        fx_move(v->fxw, v->sofx[j].fx, at);
    }
}

/* what the modules see of the monsters this frame (army.h's ArmyMonster, index = the monster's slot): their places,
 * moves and collision spheres, which the frame's world follows too */
static int army_monsters(Viewer *v, ArmyMonster *m)
{
    static float spheres[MONSTERS][64][4];
    int nm = 0;
    for (int k = 0; k < v->nmon; k++, nm++) {
        Mon *mo = &v->mon[k];
        memset(&m[k], 0, sizeof m[k]);
        if (!mo->have_graph || !mon_in(mo))
            return k;                                /* (a monster without its graph: the ones before it) */
        int ns = monster_spheres(mo, spheres[k], 64);   /* every collision sphere, in the world */
        world_monster_moved(mo, (const float (*)[4])spheres[k], ns);
        m[k].pos[0] = mo->mx; m[k].pos[1] = mo->my; m[k].pos[2] = mo->mz;
        m[k].heading = mo->heading / 57.2957795f;
        m[k].move = mo->graph.states[mo->run.state].name;
        m[k].mode = mo->mode;
        m[k].spheres = (const float (*)[4])spheres[k];
        m[k].nspheres = ns;
        float hw = mo->graph.box[0] > 0 ? mo->graph.box[0] : 3, hh = mo->graph.box[1] > 0 ? mo->graph.box[1] : 4.25f;
        m[k].box_min[0] = mo->mx - hw; m[k].box_min[1] = mo->my - hw; m[k].box_min[2] = mo->mz;
        m[k].box_max[0] = mo->mx + hw; m[k].box_max[1] = mo->my + hw; m[k].box_max[2] = mo->mz + 2 * hh;
        v->scene.mon[k].move = m[k].move;
    }
    return nm;
}

static void army_step(Viewer *v, float dt)
{
    if (!v->army || !have_monster(v) || !v->mon[0].have_graph || !mon_in(&v->mon[0]))
        return;
    ArmyMonster m[MONSTERS];
    int nm = army_monsters(v, m);
    static ArmyBox boxes[64];
    int nb = 0;
    for (uint32_t i = 0; i < v->scene.nbuildings && nb < 64; i++, nb++) {
        const Building *b = &v->scene.buildings[i].b;
        memcpy(boxes[nb].min, b->min, sizeof boxes[nb].min);
        memcpy(boxes[nb].max, b->max, sizeof boxes[nb].max);
        boxes[nb].width = b->width;
        boxes[nb].storeys = b->n;
        boxes[nb].gone = b->state == BUILDING_GONE;
    }
    army_set_view(v->army, v->view.pos[0] - 45, v->view.pos[0] + 45);   /* the view's x range [G] */
    army_update(v->army, dt, m, nm, boxes, nb);
    vehicle_step(v, dt, m, nm, boxes, nb);
    parade_step(v, dt, m, nm);
    rail_step(v, dt, m, nm);
    edible_step(v, dt);
    boss_step(v, dt, m, nm);
    game_step(v, dt);
    ArmyEvent ev[64];
    int ne;
    while ((ne = army_events(v->army, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const ArmyEvent *e = &ev[i];
            Mon *mo = ev_mon(v, e->monster);
            switch (e->type) {
            case AE_EFFECT:
                if (v->fxw) {
                    int id = fx_spawn(v->fxw, e->name, e->pos, e->dir, 1);
                    if (id >= 0 && (e->unit >= 0 || e->shot >= 0) && v->nfxa < 128) {
                        v->fxa[v->nfxa].id = e->id;
                        v->fxa[v->nfxa].fx = id;
                        v->fxa[v->nfxa].unit = e->shot >= 0 ? -1 : e->unit;
                        v->fxa[v->nfxa].shot = e->shot;
                        const ArmyUnit *u = e->unit >= 0 ? army_unit(v->army, e->unit) : NULL;
                        for (int k = 0; k < 3; k++)
                            v->fxa[v->nfxa].off[k] = u && e->shot < 0 ? e->pos[k] - u->pos[k] : 0;
                        v->nfxa++;
                    } else if (id >= 0 && v->nfxa < 128) {   /* a handle to stop it by */
                        v->fxa[v->nfxa].id = e->id;
                        v->fxa[v->nfxa].fx = id;
                        v->fxa[v->nfxa].unit = v->fxa[v->nfxa].shot = -1;
                        v->nfxa++;
                    }
                }
                break;
            case AE_EFFECT_STOP:
                for (int k = 0; k < v->nfxa; k++)
                    if (v->fxa[k].id == e->id) {
                        fx_stop(v->fxw, v->fxa[k].fx);
                        v->fxa[k] = v->fxa[--v->nfxa];
                        break;
                    }
                break;
            case AE_SOUND:
                if (e->level == 0) sound_kill_name(e->name);
                else sound_play(e->name, e->pos, 1);
                break;
            case AE_HIT: monster_hit(mo, e->name, e->amount, e->level, e->v, e->pos); break;
            case AE_REACTION: mo->pending_slot = e->level; break;
            case AE_PUSH: mo->mx += e->v[0]; mo->my += e->v[1]; break;
            case AE_IMPACT: impact_route(v, e->id, e->level, e->pos, e->amount); break;
            case AE_STATS_HEALTH: mo->hp = mo->hp > 0 ? fmaxf(1, fminf(200, mo->hp + e->amount)) : mo->hp; break;
            case AE_STATS_ENERGY: monster_stats(mo, 0, e->amount); break;
            case AE_FLOAT:                           /* a PointsBanner: 0.8 for the army's awards (0x2BAB2C) */
                if (v->hud)
                    hud_float(v->hud, (int)e->amount, e->pos, e->level, 0.8f);
                break;
            case AE_WINDOW_CLOSE: scene_window_close_at(&v->scene, e->pos); break;
            case AE_SCORE:
                if (v->game)
                    game_add_points(v->game, mo->slot, e->level, (int)e->amount);
                if (e->name)                         /* the object's template counts for the challenge (0x346780) */
                    challenge_stats(v, e->name, (int)e->amount);
                if (getenv("RAMPAGE_TRACE"))
                    printf(mo->slot ? "  score %+.0f (category %d, %s), total %.0f, monster %d\n" :
                           "  score %+.0f (category %d, %s), total %.0f\n", e->amount, e->level, e->name ? e->name : "-",
                           army_stats(v->army, mo->slot)->points[6], mo->slot);
                break;
            default: break;
            }
        }
    for (int k = 0; k < v->nfxa; k++) {              /* the effects that follow their unit or shot */
        float at[3];
        if (v->fxa[k].shot >= 0) {
            const ArmyShot *sh = NULL;
            for (int j = 0, n = army_shots(v->army); j < n && !sh; j++)
                if (army_shot(v->army, j)->id == v->fxa[k].shot && army_shot(v->army, j)->alive)
                    sh = army_shot(v->army, j);
            if (!sh) {
                fx_stop(v->fxw, v->fxa[k].fx);
                v->fxa[k--] = v->fxa[--v->nfxa];
                continue;
            }
            memcpy(at, sh->pos, sizeof at);
        } else if (v->fxa[k].unit >= 0) {
            const ArmyUnit *u = army_unit(v->army, v->fxa[k].unit);
            if (!u || u->phase == AU_GONE) {
                fx_stop(v->fxw, v->fxa[k].fx);
                v->fxa[k--] = v->fxa[--v->nfxa];
                continue;
            }
            for (int j = 0; j < 3; j++)
                at[j] = u->pos[j] + v->fxa[k].off[j];
        } else {
            if (!fx_alive(v->fxw, v->fxa[k].fx))
                v->fxa[k--] = v->fxa[--v->nfxa];
            continue;
        }
        fx_move(v->fxw, v->fxa[k].fx, at);
    }
    for (int k = 0; k < v->nmon; k++) {              /* a unit in a monster's hand */
        Mon *mo = &v->mon[k];
        Mat4 h;
        if (mo->held_unit >= 0 && attach_world(mo, "Helper R Hand", &h) == 0)
            army_hold(v->army, mo->held_unit, h.m);
    }
}

/* the effects the scene and the people asked for, started; the world's particles moved on */
static void fx_step(Viewer *v, float dt)
{
    for (int i = 0; i < v->scene.nfxreq; i++)
        if (v->fxw) {
            const struct SceneFx *f = &v->scene.fxreq[i];
            int id = fx_spawn(v->fxw, f->name, f->pos, f->dir, 1);
            if (getenv("RAMPAGE_TRACE"))
                printf("  effect %s at %.1f %.1f %.1f -> %d\n", f->name, f->pos[0], f->pos[1], f->pos[2], id);
        }
    v->scene.nfxreq = 0;
    if (v->fxw)
        fx_update(v->fxw, dt, 0);
}

static int ends_with_i(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && _stricmp(s + a - b, tail) == 0;
}

/* an attachment node of MONSTER_DATA (+0xA84) in the world as drawn: its bone's place x its matrix (rows, then the
 * place); mirrored, the other side's node on the other side's bone, reflected; 0 = ok */
static int attach_world(Mon *mo, const char *name, Mat4 *out)
{
    if (!mo->have_graph || !mon_in(mo))
        return -1;
    const Instance *in = mon_in(mo);
    const struct MoveAttach *a = NULL;
    for (int i = 0; i < mo->graph.nattach && !a; i++)
        if (mo->graph.attach[i].name && ends_with_i(mo->graph.attach[i].name, name))
            a = &mo->graph.attach[i];
    if (!a || a->bone < 0 || (uint32_t)a->bone >= in->tree.n)
        return -1;
    if (mo->mirror) {                                 /* the partner bone's own attachment */
        int mb = in->tree.nodes[a->bone].mirror;
        for (int i = 0; i < mo->graph.nattach; i++)
            if (mo->graph.attach[i].bone == mb && mb != a->bone)
                a = &mo->graph.attach[i];
    }
    Mat4 t = mat_identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            t.m[4 * i + j] = a->m[3 * i + j];
    for (int j = 0; j < 3; j++)
        t.m[12 + j] = a->m[9 + j];
    Mat4 m = mat_mul(in->node[a->bone], t);
    if (mo->mirror)
        for (int c = 0; c < 4; c++) {                /* S m S */
            m.m[4 * c] = -m.m[4 * c];
            m.m[c] = -m.m[c];
        }
    *out = mat_mul(in->base, m);
    return 0;
}

/* canGrabPerson's / doGrabPerson's window person (getBuildingOccupant 0x2B37C0), -1 none */
static int window_person(Mon *mo, const float *xyz)
{
    Viewer *v = mo->v;
    PeopleBox boxes[64];
    int nb = scene_people_boxes(&v->scene, boxes, 64);
    float at[3] = { mo->mx, mo->my, mo->mz };
    return v->scene.folk ? people_can_grab(v->scene.folk, boxes, nb, at, xyz, mo->mirror) : -1;
}

/* ---- the bots' view of the world (director.h DirBotView): the climbing code's answers for this monster */
static int bot_cell(void *ctx, int bi, int storey, int col)
{
    const Viewer *v = ((Mon *)ctx)->v;
    if (bi < 0 || (uint32_t)bi >= v->scene.nbuildings)
        return -1;
    const Building *b = &v->scene.buildings[bi].b;
    int ci = building_cell_index(b, FACE_FRONT, col);
    return ci < 0 || storey < 0 || storey >= b->n ? -1 : b->cell[b->live[storey]][ci].state;
}
static int bot_can_mount(void *ctx, int side, int *building)
{
    Mon *mo = ctx;
    Climber c = climber(mo);
    int r = climb_can_mount(&mo->mount, &mo->v->scene, &c, side, 0);
    *building = r == 1 ? mo->mount.cand.building : -1;
    return r;
}
static int bot_can_climb(void *ctx, int dv, int dh)
{
    Mon *mo = ctx;
    Climber c = climber(mo);
    return climb_can_climb(&mo->mount, &mo->v->scene, &c, dv, dh, 0);
}
static int bot_place(void *ctx, int bi, const float pt[3])
{
    Mon *mo = ctx;
    return climb_place_ok(&mo->v->scene, bi, pt, pt[0]);   /* (an AI: no camera reach) */
}
static void bot_collapse(void *ctx, int bi)          /* collapseBuilding, scored to the bot (+0x190) */
{
    Mon *mo = ctx;
    if (bi >= 0 && (uint32_t)bi < mo->v->scene.nbuildings) {
        mo->v->scene.buildings[bi].hitter = mo->slot;
        building_destroy(&mo->v->scene.buildings[bi].b);
    }
}

/* what the AI monsters' DestroyBuildings goals see this frame: the buildings, the monsters, each one itself */
static void bot_views(Viewer *v, DirBuilding *db, DirMonster *dm, DirBotView *bv)
{
    int nb = 0;
    for (uint32_t i = 0; i < v->scene.nbuildings && nb < 64; i++, nb++) {
        const Building *b = &v->scene.buildings[i].b;
        DirBuilding *o = &db[nb];
        memcpy(o->min, b->min, sizeof o->min);
        memcpy(o->max, b->max, sizeof o->max);
        o->pos[0] = (b->min[0] + b->max[0]) / 2;
        o->pos[1] = (b->min[1] + b->max[1]) / 2;
        o->pos[2] = b->min[2];
        o->state = b->state;
        o->row = b->row;
        o->width = b->width;
        o->storeys = b->n;
    }
    for (int k = 0; k < v->nmon; k++) {
        const Mon *mo = &v->mon[k];
        DirMonster *o = &dm[k];
        o->pos[0] = mo->mx; o->pos[1] = mo->my; o->pos[2] = mo->mz;
        o->mounted = mo->mount.mounted ? mo->mount.at.building : -1;
        o->rail = mo->mount.at.rail;
        o->human = mo->human;
        o->ai = mo->is_ai;
        o->radius = (mo->graph.box[0] > 0 ? mo->graph.box[0] : MONSTER_HALF_WIDTH) * (mo->scale > 0 ? mo->scale : 1);
    }
    for (int k = 0; k < v->nmon; k++) {
        Mon *mo = &v->mon[k];
        DirBotView *b = &bv[k];
        memset(b, 0, sizeof *b);
        float h = mo->heading / 57.2957795f;
        b->pos[0] = mo->mx; b->pos[1] = mo->my; b->pos[2] = mo->mz;
        b->fwd[0] = -sinf(h); b->fwd[1] = cosf(h);
        b->mirror = mo->mirror;
        b->mirror_queued = mo->mirror_queued;
        b->mode = mo->mode;
        b->mounted = dm[k].mounted;
        b->rail = mo->mount.at.rail;
        b->storey = mo->mount.at.storey;
        b->smash = (mo->power.abilities & 8) != 0 || mo->power.rage != 0;
        b->hp = mo->hp;
        b->half_width = dm[k].radius;
        b->self = k;
        b->b = db;
        b->nb = nb;
        b->m = dm;
        b->nm = v->nmon;
        b->under = -1;
        for (int i = 0; i < nb && b->under < 0; i++)   /* the building under its body (0x2D6150) */
            if (mo->mx >= db[i].min[0] && mo->mx <= db[i].max[0] && mo->my >= db[i].min[1] && mo->my <= db[i].max[1] &&
                mo->mz >= db[i].max[2] - 1 && b->mounted < 0)
                b->under = i;
        b->ctx = mo;
        b->cell = bot_cell;
        b->can_mount = bot_can_mount;
        b->can_climb = bot_can_climb;
        b->place = bot_place;
        b->collapse = bot_collapse;
        mo->ai.view = b;
    }
}

/* the TwoTilts camera frames this monster (0x2D69A0) [R]: one in the scene, not hidden, not dropping in, not human
 * unless its streaker still runs and is not held; alone too (0x312B50 holds its last view when it frames none) */
static int cam_follows(const Mon *mo)
{
    const Instance *in = mon_in(mo);
    if (!in)
        return 0;
    if (mo->dropping)                                /* 0x2D69A0: not dropping in, not human unless its streaker */
        return 0;                                    /* still runs and is not held [R] */
    if (mo->human) {
        const Person *p = mo->streaker >= 0 && mo->v->scene.folk ? people_get(mo->v->scene.folk, mo->streaker) : NULL;
        return p && p->phase != PP_GONE && p->phase != PP_HELD;
    }
    return !in->hidden;
}

/* one frame of a monster's own play, from its pad (Mon.pad). The graph picks the state; the clip's root carries the
 * monster (its travel added to where it stands, its turn when a NODE_BAKE_ROOT state ends), the stick turns it
 * (NODE_STEER) or drives it (NODE_AIR_*) in the camera's frame, the clip's events set its movement mode and velocity,
 * and in the air it falls by MOVES_GRAVITY until it meets the ground, which fires the landing slot. Then its place,
 * head, scale, hands and feet for the posing */
static void mon_step(Mon *mo, float dt)
{
    Viewer *v = mo->v;
    static const uint8_t none[MV_COUNT];
    float lx = mo->pad.lx, ly = mo->pad.ly;
    const uint8_t *pressed = mo->pad.pressed, *held = mo->pad.held;
    if (mo->is_ai) {                                  /* the game drives the monster: no pad */
        lx = mo->ai.stick[0];
        ly = mo->ai.stick[1];
        pressed = held = none;
    }
    if (mo->have_graph) {
        MoveInput *in = &mo->input;
        face_step(mo, dt);
        if (in->v[MV_LX] != lx) in->fresh[MV_LX] = 0.2f;
        if (in->v[MV_LY] != ly) in->fresh[MV_LY] = 0.2f;
        in->v[MV_LX] = lx;
        in->v[MV_LY] = ly;
        for (int k = MV_UP; k <= MV_L3; k++) {
            in->v[k] = held[k] || pressed[k];
            if (pressed[k]) in->fresh[k] = 0.2f;
        }
        moves_derive(in);
        {                                            /* the stick in the monster's frame (0x2C0AD0 -> 0x2C03E0) */
            float w[3], h = mo->heading / 57.2957795f, c = cosf(h), s = sinf(h);
            stick_world(mo, lx, ly, w);
            moves_sectors(in, c * w[0] + s * w[1], -s * w[0] + c * w[1]);
        }
        const MoveState *st = &mo->graph.states[mo->run.state];
        /* the camera frame the stick is read in: every frame (0x2E0738), or, in a NODE_HOLD_FRAME state, only when the
         * stick moves (0x2E8190), so a climb keeps its direction while the camera swings */
        if (!(st->flags & NODE_HOLD_FRAME) || lx != mo->stick[0] || ly != mo->stick[1])
            memcpy(mo->frame, v->view.rows, sizeof mo->frame);
        mo->stick[0] = lx;
        mo->stick[1] = ly;
        float t0 = mo->run.time, length = st->length;
        float cdt = dt * mo->power.speed;             /* the clips run at the speed scale x their rate (0x2EBFE0:
                                                      * setSpeedScale, the rage's 1.33, the speed power-up) */
        int ev = -1;
        if (mo->mode == MODE_AIRBORNE || mo->mode == MODE_TAKEOFF) {   /* the air: velocity, then gravity */
            float pos[3] = { mo->mx, mo->my, mo->mz };  /* 0x2DEBD8: drag, the move, gravity */
            monster_air_velocity(mo->vel, pos, dt, mo->no_gravity);
            mo->mx = pos[0]; mo->my = pos[1]; mo->mz = pos[2];
            float g = ground_at(v, mo->mx, mo->my, mo->mz);
            if (mo->mz <= g && mo->vel[2] <= 0) {
                mo->mz = g;
                int wk = water_at(v, mo->mx, mo->my);
                if (mo->mode == MODE_AIRBORNE && wk >= 0) {   /* into the water (0x2DCA94): pressed down 1.25 x its
                                                      * radius across, by its fall speed */
                    float s = fminf(fmaxf(-mo->vel[2] / 50, 0), 1), at[3] = { mo->mx, mo->my, 0 };
                    at[2] = water_surface(v->scene.water[wk]);
                    water_disturb(v->scene.water[wk], at, 1.25f * MONSTER_HALF_WIDTH, -(2 * s + 0.5f * (1 - s)));
                }
                if (mo->mode == MODE_AIRBORNE && v->fxw) {   /* every landing (0x2DC450 -> 0x2F48D0): the big
                                                      * footfall under the spine, GroundImpactFx / WaterImpactFx */
                    Mat4 sp;
                    float spine[3] = { mo->mx, mo->my, mo->mz };
                    FootFx fx;
                    if (scene_monster_node(&v->scene, mo->slot, FOOT_IMPACT_BONE, &sp) == 0) {
                        spine[0] = sp.m[12]; spine[1] = sp.m[13];
                    }
                    if (foot_impact(&mo->feet_fx, spine, g, wk >= 0,
                                    wk >= 0 ? water_surface(v->scene.water[wk]) : 0, &fx)) {
                        fx_spawn(v->fxw, fx.name, fx.pos, NULL, 1);
                        if (getenv("RAMPAGE_TRACE"))
                            printf("  landing %s at %.1f %.1f %.1f%c", fx.name, fx.pos[0], fx.pos[1], fx.pos[2], 10);
                    }
                }
                if (mo->mode == MODE_AIRBORNE)
                    mo->dropping = 0;                /* (0x2E1A14: in the camera again) */
                if (mo->mode == MODE_AIRBORNE) {      /* landed (0x2DC450): slot 56 in the current column */
                    int to = moves_slot(&mo->graph, SLOT_LAND, mo->column);
                    if (to >= 0) {
                        moves_force(&mo->run, to);
                        ev = 0;
                    }
                    mo->mode = MODE_PRONE;
                    memset(mo->vel, 0, sizeof mo->vel);
                } else {
                    mo->vel[2] = 0;
                }
            }
        } else if (mo->mode == MODE_BUILDING && mo->mount.at.building >= 0) {
            memset(mo->vel, 0, sizeof mo->vel);        /* on a face: held there, no gravity, no ground */
        } else {                                     /* 0x2DF6F0: a knock slides it, friction stops it */
            float pos[3] = { mo->mx, mo->my, mo->mz };
            monster_ground_velocity(mo->vel, pos, mo->friction, dt, st->flags, mo->pending_slot >= 0);
            mo->mx = pos[0]; mo->my = pos[1]; mo->mz = pos[2];
            float g = ground_at(v, mo->mx, mo->my, mo->mz);
            if (g < mo->mz - 0.5f)                    /* walked off a roof: it falls (a stand-in: the game eases it */
                mo->mode = MODE_AIRBORNE;             /* down, 0x2E0330) and lands through the landing slot */
            else
                mo->mz = monster_ground_follow(mo->mz, g, mo->no_gravity);
        }
        {                                            /* wading (0x2E2900 -> 0x2E2BD0): the water pressed down under
                                                      * it, half its radius across, by how deep it stands */
            int wk = mo->mode != MODE_BUILDING ? water_at(v, mo->mx, mo->my) : -1;
            if (wk >= 0) {
                float at[3] = { mo->mx, mo->my, water_surface(v->scene.water[wk]) };
                float deep = fminf(fmaxf(at[2] - mo->mz, 0), MONSTER_HEIGHT);
                if (deep > 0)
                    water_disturb(v->scene.water[wk], at, 0.5f * MONSTER_HALF_WIDTH, -0.9f * deep / MONSTER_HEIGHT);
            }
        }
        air_control(mo, st, lx, ly, dt);
        if (mo->mode == MODE_GROUND || mo->mode == MODE_AIRBORNE || mo->mode == MODE_TAKEOFF || mo->mode == MODE_PRONE) {
            mo->mx += mo->air[0] * dt;                 /* 0x2E0C50: in modes 0, 1, 2 and 4 */
            mo->my += mo->air[1] * dt;
        }
        if (st->flags & NODE_STEER)
            mo->heading += steer(mo, st, lx, ly, dt);
        scene_monster_blend_step(&v->scene, mo->slot, dt);
        if (ev != 0 && mo->pending_slot >= 0) {       /* the army's hit: the reaction slot (0x2D6A50), its column's */
            int to = moves_slot(&mo->graph, mo->pending_slot, mo->column);
            mo->pending_slot = -1;
            if (to >= 0) {
                climb_unmount(&mo->mount);
                moves_force(&mo->run, to);
                if (mo->vel[2] > 0)
                    mo->mode = MODE_AIRBORNE;
                ev = 0;
            }
        }
        if (ev != 0) {                               /* 0x2E14F0: held on the face, or knocked off it */
            Climber cl = climber(mo);
            int was = mo->mount.at.building;
            int slot = climb_check(&mo->mount, &v->scene, &cl, mo->mode);
            int to = slot >= 0 ? moves_slot(&mo->graph, slot, mo->column) : -1;
            if (slot >= 0 && mo->is_ai)              /* knocked off: the AI keeps off it a while (0x2EEBE0) */
                dir_ai_avoid(&mo->ai, was, NULL);
            if (to >= 0) {
                climb_unmount(&mo->mount);
                moves_force(&mo->run, to);
                mo->mode = MODE_AIRBORNE;
                ev = 0;
            }
        }
        float before[3] = { mo->mx, mo->my, mo->mz };
        float root[3] = { mo->mx, mo->my, mo->mz };
        int switched = ev == 0 || (mo->is_ai && mo->have_ag ?   /* the AI picks the transitions (0x2E1AA0) */
                                   dir_ai_step(&mo->ai, &mo->ag, &mo->run, root, cdt, answer, mo) :
                                   moves_step(&mo->run, in, cdt, answer, mo));
        for (; mo->is_ai && mo->ai.done > 0; mo->ai.done--)   /* Levels::moveComplete */
            if (mo->reveal)                          /* Monsters::onRevealNIEnd: stopReveal */
                reveal_walk_arrived(&v->rwalk, &v->reveal);
            else if (v->game)
                game_move_done(v->game, mo->slot);
        float t1 = switched ? (ev == 0 ? t0 : (t0 + cdt < length ? t0 + cdt : length)) : mo->run.time;
        float a[3], b[3], q[4];                      /* the root's travel and turn this frame the state hands the monster */
        if (root_at(mo, clip_at(mo, st, t0), a, q) == 0 && root_at(mo, clip_at(mo, st, t1), b, q) == 0) {
            float c = cosf(mo->heading / 57.2957795f), s = sinf(mo->heading / 57.2957795f);
            float dx = st->flags & NODE_ROOT_X ? b[0] - a[0] : 0, dy = st->flags & NODE_ROOT_Y ? b[1] - a[1] : 0;
            float m = mo->mirror ? -1.0f : 1.0f;      /* mirrored: across the monster's x */
            mo->mx += c * m * dx - s * dy;
            mo->my += s * m * dx + c * dy;
            if (st->flags & NODE_ROOT_Z)
                mo->mz += b[2] - a[2];
            if (st->flags & NODE_ROOT_TURN)
                mo->heading += m * root_turn(mo, clip_at(mo, st, t0), clip_at(mo, st, t1));
        }
        clip_events(mo, st, t0, t1);                  /* the old clip's events up to now */
        if (mo->mode == MODE_BUILDING && mo->mount.at.building >= 0) {   /* held on the face (0x2D5870) */
            float pos[3] = { mo->mx, mo->my, mo->mz }, delta[3] = { mo->mx - before[0], mo->my - before[1], mo->mz - before[2] };
            climb_place(&mo->mount, &v->scene, st->flags, delta, dt, pos, &mo->heading);
            mo->mx = pos[0];
            mo->my = pos[1];
            mo->mz = pos[2];
        }
        if (switched) {
            /* a clip that bakes: its root, as drawn where it ended (the axes it handed over were held at its start, a
             * turn it handed over taken out), in the monster's frame as seen (mirrored if it was) */
            int bake = (st->flags & NODE_BAKE_ROOT) && root_at(mo, clip_at(mo, st, t1), b, q) == 0;
            float bake_p[3], bake_r[3][3];
            if (bake) {
                for (int k = 0; k < 3; k++)
                    bake_p[k] = st->flags & (k == 0 ? NODE_ROOT_X : k == 1 ? NODE_ROOT_Y : NODE_ROOT_Z) ?
                                mo->entry_root[k] : b[k];
                float zero[3] = { 0, 0, 0 }, tw = st->flags & NODE_ROOT_TURN ? root_turn(mo, mo->entry_t, clip_at(mo, st, t1)) : 0;
                Mat4 r = mat_mul(mat_trs_z(0, 0, 0, -tw), mat_tq(zero, q));
                seen_root(mo->mirror, bake_p, &r, bake_r);
            }
            const MoveState *to = &mo->graph.states[mo->run.state];
            if (mo->run.from != mo->run.state && mo->slot)   /* (the other monsters' marked with their slot) */
                printf(getenv("RAMPAGE_TRACE") ? "[%d] %s -> %s (%s) by %d\n" : "[%d] %s -> %s (%s)\n", mo->slot + 1,
                       st->name ? st->name : "?", to->name ? to->name : "?", to->clip ? to->clip : "-",
                       ev == 0 ? -2 : mo->run.via);
            else if (mo->run.from != mo->run.state)
                printf(getenv("RAMPAGE_TRACE") ? "%s -> %s (%s) by %d\n" : "%s -> %s (%s)\n", st->name ? st->name : "?",
                       to->name ? to->name : "?", to->clip ? to->clip : "-", ev == 0 ? -2 : mo->run.via);
            if (getenv("RAMPAGE_TRACE"))
                printf("  stick %.2f %.2f size %.2f\n", lx, ly, in->v[MV_L_SIZE]);
            /* the transition says whether the new clip blends in (over the new state's blend time) or cuts; a start
             * the game forces (a slot) takes no transition, and its blend is not traced: it blends */
            int blend = ev == 0 ? BLEND_IN : mo->run.via >= 0 ? st->trans[mo->run.via].blend : st->end_blend;
            scene_monster_blend(&v->scene, mo->slot, blend == BLEND_IN ? to->blend : 0);
            foot_command(&mo->feet, st->flags);      /* 0x33CCA0: the foot object keeps the state left */
            play_state(mo);
            if (bake && root_at(mo, mo->entry_t, b, q) == 0) {
                /* 0x2E6580: the monster moves and turns so that the new clip's root, where it starts, is where the old
                 * one's was: the old root's place and turn become the monster's */
                float zero[3] = { 0, 0, 0 }, np[3] = { b[0], b[1], b[2] }, nr[3][3];
                Mat4 r = mat_tq(zero, q);
                seen_root(mo->mirror, np, &r, nr);
                float h = atan2f(bake_r[1][0] * nr[0][0] + bake_r[1][1] * nr[0][1] + bake_r[1][2] * nr[0][2],
                                 bake_r[0][0] * nr[0][0] + bake_r[0][1] * nr[0][1] + bake_r[0][2] * nr[0][2]);
                float ch = cosf(h), sh = sinf(h);
                float d[3] = { bake_p[0] - (ch * np[0] - sh * np[1]), bake_p[1] - (sh * np[0] + ch * np[1]),
                               bake_p[2] - np[2] };
                float c = cosf(mo->heading / 57.2957795f), s = sinf(mo->heading / 57.2957795f);
                mo->mx += c * d[0] - s * d[1];
                mo->my += s * d[0] + c * d[1];
                mo->mz += d[2];
                mo->heading += h * 57.2957795f;
                if (getenv("RAMPAGE_ROOT"))
                    printf("  bake: old root %.2f %.2f %.2f, new %.2f %.2f %.2f, turn %.0f\n", bake_p[0], bake_p[1],
                           bake_p[2], np[0], np[1], np[2], h * 57.2957795f);
            }
            mo->column = -1;                         /* 0x2E7F70: a new state starts without the old one's effects */
            mo->no_gravity = 0;
            /* and the new one's up to where it starts: the game's event player (0x2F45D0) begins at the clip's first
             * event and fires, in order, every one at or before the start, so a state entered part-way (phase
             * kept) still gets its set-up (mode, column) */
            clip_events(mo, to, -1, mo->run.time);
        }
        if (mon_in(mo) && mo->feet.anchor >= 0) {   /* the foot lock (0x33BF10): the root eases onto
                                                      * the planted foot */
            const Instance *in = mon_in(mo);
            double ct = clip_at(mo, &mo->graph.states[mo->run.state], mo->run.time);
            float feet[2][3], place[3] = { mo->mx, mo->my, mo->mz };
            if (ik_clip_node(in, ct, "Bip01 L Foot", feet[0]) == 0 && ik_clip_node(in, ct, "Bip01 R Foot", feet[1]) == 0 &&
                foot_lock(&mo->feet, mo->graph.states[mo->run.state].flags, (const float (*)[3])feet, place, mo->heading)) {
                mo->mx = place[0];
                mo->my = place[1];
                if (mo->is_ai)                       /* 0x2E0F54: the goal's +0x38 */
                    dir_ai_cooldown(&mo->ai);
            }
        }
        if (!mo->reveal) {                           /* (the revealed one walks off to x 100 [R]) */
            if (mo->mx < -75) mo->mx = -75;
            if (mo->mx > 75) mo->mx = 75;
        }
        if (mo->my < -30) mo->my = -30;
        push_out(mo);                                 /* on a face (mode 5) only the triggers: it is held by the face */
        if (!mo->is_ai && !mo->mount.mounted) {   /* the camera's reach (0x336290): a player off a face stays
                                                      * at most 70 up and 30 along x from the players' middle */
            if (mo->mz > 70) {
                mo->mz = 70;
                if (mo->vel[2] > 0) mo->vel[2] = 0;
                if (mo->air[2] > 0) mo->air[2] = 0;
            }
            if (fabsf(mo->mx - v->mid_x) > 30) {
                mo->mx = v->mid_x + (mo->mx > v->mid_x ? 30 : -30);
                mo->vel[0] = mo->air[0] = 0;
            }
        }
        mo->heading = fmodf(mo->heading + 360, 360);
        if (getenv("RAMPAGE_TRACE") && mo->slot)
            printf("  [%d] %s %.2f at %.1f %.1f %.1f heading %.0f mode %d vz %.1f\n", mo->slot + 1,
                   mo->graph.states[mo->run.state].name, mo->run.time, mo->mx, mo->my, mo->mz, mo->heading, mo->mode,
                   mo->vel[2]);
        else if (getenv("RAMPAGE_TRACE"))
            printf("  %s %.2f at %.1f %.1f %.1f heading %.0f mode %d vz %.1f\n", mo->graph.states[mo->run.state].name,
                   mo->run.time, mo->mx, mo->my, mo->mz, mo->heading, mo->mode, mo->vel[2]);
    }
    scene_monster_place(&v->scene, mo->slot, mo->mx, mo->my, mo->mz, mo->heading);
    {                                                /* the head (0x2E70C0, 0x2FBE30, 0x2FBC90): in a look state the
                                                        right stick's deflection is the target, 81 degrees of yaw and
                                                        45 of pitch (MONSTER_DATA +0xA08 .. +0xA10, the same in all
                                                        30); eased at pi / 2 a second (MD +0x174, +0x178), half that
                                                        while the stick is held; back to 0 outside */
        uint32_t fl = mo->have_graph ? mo->graph.states[mo->run.state].flags : 0;
        look_stick(&mo->head, &mo->look_lim, (fl & NODE_LOOK) != 0, mo->pad.rx, mo->pad.ry);
        look_update(&mo->head, &mo->look_lim, 0, fl); /* no target: tracking is off in this build (look.c) */
        look_step(&mo->head, &mo->look_lim, dt);
        mo->look_yaw = mo->head.yaw;
        mo->look_pitch = mo->head.pitch;
        v->scene.mon[mo->slot].look_yaw = mo->mirror ? -mo->look_yaw : mo->look_yaw;   /* mirrored: not traced [G] */
        v->scene.mon[mo->slot].look_pitch = mo->look_pitch;
    }
    if (mo->scale_left > 0) {                         /* the dynamic scale, linear over its time [G] */
        mo->scale_left -= dt;
        float f = mo->scale_left > 0 ? mo->scale_left / mo->scale_time : 0;
        mo->scale = mo->scale_to + (mo->scale_from - mo->scale_to) * f;
    }
    if (mo->scale != 1 && mon_in(mo))
        for (int k = 0; k < 11; k++)
            if ((k & 3) != 3)
                mon_in(mo)->base.m[k] *= mo->scale;
    hands_step(mo, dt);
    feet_step(mo);
}

/* one frame of play: the monsters (each from its pad: Mon.pad), then the world around them, the HUDs and the camera */
static void play_step(Viewer *v, float dt, float aspect)
{
    if (v->hud && hud_paused(v->hud)) {              /* GameOver's pauseGame(1): only the screen runs */
        hud_update(v->hud, dt);
        hud_frame(v, 1);
        return;
    }
    world_build(v);                                  /* the collision world as the frame begins */
    scene_cloth_step(&v->scene, dt);                 /* the cloths' time, simulated at the next posing */
    v->mid_x = players_mid_x(v);                     /* 0x336360, once a frame (0x2B6CE0) */
    static DirBuilding db[64];
    static DirMonster dm[MONSTERS];
    static DirBotView bv[MONSTERS];
    bot_views(v, db, dm, bv);
    if (v->extra >= 0) {                             /* the revealed monster's 0.25 s timer, its calls */
        reveal_walk_update(&v->rwalk, dt, v->mon[v->extra].mx, v->view.pos[0]);
        reveal_events(v);
    }
    for (int k = 0; k < mon_world(v); k++)
        mon_step(&v->mon[k], dt);
    if (v->extra >= 0)                               /* (it arrived: hidden) */
        reveal_events(v);
    if (v->scene.folk) {                             /* the people, the ones in their hands, what they make happen */
        PeopleMonster pm[MONSTERS];
        for (int k = 0; k < v->nmon; k++) {
            const Mon *mo = &v->mon[k];
            pm[k] = (PeopleMonster){ { mo->mx, mo->my, mo->mz },
                                     mo->have_graph ? mo->graph.states[mo->run.state].name : NULL };
        }
        scene_people_spawn(&v->scene, dt);
        people_update(v->scene.folk, dt, pm, v->nmon);
        scene_huts_update(&v->scene, dt);
        for (int k = 0; k < v->nmon; k++) {
            Mon *mo = &v->mon[k];
            Mat4 h;
            if (mo->held >= 0 && people_get(v->scene.folk, mo->held)->phase != PP_HELD)
                mo->held = -1;
            if (mo->held >= 0 && attach_world(mo, "Helper R Hand", &h) == 0)
                people_hold(v->scene.folk, mo->held, h.m, mo->mirror, dt);
        }
        PeopleEvent ev[32];
        int ne = people_events(v->scene.folk, ev, 32);
        for (int i = 0; i < ne; i++) {               /* what the army takes over: droppables, the punks' bottles */
            const Person *p = people_get(v->scene.folk, ev[i].who);
            if (!v->army || !p)
                continue;
            if (ev[i].type == PE_DROP) {
                const char *tmpl = NULL;             /* the block's DroppableDef: one of its OTRef| at random */
                Group dd = group_sub(v->scene.block, "DroppableDef"), where;
                Record r[16];
                uint32_t n = dd.at ? group_values(dd, "OTRef|", &where, r, 16) : 0;
                if (n)
                    tmpl = record_str(where, r[(uint32_t)rand() % n]);
                if (tmpl)
                    army_drop(v->army, tmpl, ev[i].v, NULL, 0, NULL, 0);
            } else if (ev[i].type == PE_WEAPON && ev[i].name) {
                army_person_weapon(v->army, ev[i].who, ev[i].on ? ev[i].name : NULL, p->world, 0);
                if (ev[i].on && v->narmed_people < 32)
                    v->armed_people[v->narmed_people++] = ev[i].who;
            }
        }
        for (int i = 0; v->army && i < v->narmed_people; i++) {
            const Person *p = people_get(v->scene.folk, v->armed_people[i]);
            if (p)
                army_person_moved(v->army, v->armed_people[i], p->world);
        }
        for (int i = 0; i < ne; i++) {
            if (ev[i].type == PE_SOUND && ev[i].name) {
                const Person *p = people_get(v->scene.folk, ev[i].who);
                sound_play(ev[i].name, p ? p->pos : ev[i].v, 1);
            }
            if (ev[i].type == PE_EFFECT && !ev[i].node) {
                float up[3] = { 0, 1, 0 };
                scene_fx(&v->scene, ev[i].name, ev[i].v, up);
            }
            if (ev[i].type == PE_WINDOW_CLOSE)       /* back in: its window shuts */
                scene_window_close_at(&v->scene, ev[i].v);
            Mon *mo = ev_mon(v, ev[i].monster);
            if (ev[i].type == PE_MONSTER_HP)
                mo->hp = fmaxf(0, fminf(200, mo->hp + ev[i].amount));
            else if (ev[i].type == PE_MONSTER_KNOCK) {   /* setVel(v, world), setReaction(2) */
                memcpy(mo->vel, ev[i].v, sizeof mo->vel);
                mo->mode = MODE_AIRBORNE;
            }
            if (getenv("RAMPAGE_TRACE"))
                printf("  people event %d (%s) from %d\n", ev[i].type, ev[i].name ? ev[i].name : "-", ev[i].who);
        }
    }
    scene_buildings_update(&v->scene, dt);
    street_step(v, dt);
    army_step(v, dt);
    fx_step(v, dt);
    shake_step(v->shakes, 8 + MONSTERS - 1, dt);
    for (int k = v->nmon - 1; k >= 0; k--) {         /* Hud.bsf: health, rage, the score (ArmyStats points[6]); player
                                                      * 1's last, as it has the screens */
        Mon *mo = &v->mon[k];
        if (!mo->hud)
            continue;
        int vis = 0;
        float clock = v->game ? game_clock(v->game, &vis) : 0;
        hud_set(mo->hud, mo->hp, 200, mo->energy, 100, v->game ? game_points(v->game, k, GPT_TOTAL) :
                v->army ? (int)army_stats(v->army, k)->points[6] : 0, v->game ? game_lives(v->game, k) : -1,
                v->game && (game_play_type(v->game) == GP_KING_CITY || game_play_type(v->game) == GP_KING_WORLD) ?
                game_blocks_won(v->game, k) : -1);   /* (the King modes: the blocks won by the block icon) */
        hud_set_clock(mo->hud, vis, clock);
        if (k == 0 && hud_challenge_items(mo->hud, challenge_collected(&v->challenge), challenge_required(&v->challenge)))
            challenge_points_all(v, challenge_points(&v->challenge));   /* CHALLENGE MET!: ChallengeDef.Points / players */
        hud_update(mo->hud, dt);
        if (k == 0)
            hud_frame(v, 1);
    }
    for (int k = 0; k < v->nmon && v->challenge.up_state == CU_WAITING; k++) {   /* a monster's body against it
                                                      * (TakeDamage, Collision): its box, 3 each way across and 8.5
                                                      * tall, as a sphere [G] */
        Mon *mo = &v->mon[k];
        if (!mon_in(mo))
            continue;
        float body[3] = { mo->mx, mo->my, fminf(fmaxf(v->challenge.up.pos[2], mo->mz), mo->mz + MONSTER_HEIGHT) };
        upgrade_touch(mo, body, MONSTER_HALF_WIDTH);
    }
    switch (challenge_upgrade_tick(&v->challenge, dt)) {   /* gAIMgr: the MonsterUpgrade object, its last banner */
    case CUE_SPAWN:
        if (v->fxw && v->challenge.up.effect)
            v->upgrade_fx = fx_spawn(v->fxw, v->challenge.up.effect, v->challenge.up.pos, NULL, 1);
        if (getenv("RAMPAGE_TRACE"))
            printf("  upgrade on offer: %s\n", challenge_upgrade_move(v->challenge.upgrade));
        break;
    case CUE_BANNER:
        if (v->hud) {
            static const float green[4] = { 0, 1, 0, 1 };
            hud_banner_text2(v->hud, challenge_upgrade_key(v->challenge.up_taken),
                             challenge_upgrade_inst(v->challenge.up_taken), green, 1, 3, "");
        }
        break;
    default: break;
    }
    float at[MONSTERS][3];                           /* the camera's points: every monster's root */
    DirObject me[MONSTERS];                          /* the script camera's objects: the monsters by slot */
    int nat = 0;
    for (int k = 0; k < mon_world(v); k++) {
        Mon *mo = &v->mon[k];
        if (mo->have_graph) {                         /* the pose keeps what the monster did not take over */
            const MoveState *st = &mo->graph.states[mo->run.state];
            float turn = st->flags & NODE_ROOT_TURN ?
                root_turn(mo, mo->entry_t, clip_at(mo, st, mo->run.time)) : 0;
            scene_monster_root_motion(&v->scene, mo->slot, (st->flags & NODE_ROOT_X ? 1 : 0) |
                                      (st->flags & NODE_ROOT_Y ? 2 : 0), mo->entry_root, turn);
        }
        if (k >= v->nmon)                            /* (the revealed monster: not the camera's) */
            continue;
        me[k] = (DirObject){ { mo->mx, mo->my, mo->mz }, mo->heading / 57.2957795f };
        if (cam_follows(mo)) {
            at[nat][0] = mo->mx; at[nat][1] = mo->my; at[nat][2] = mo->mz;
            nat++;
        }
    }
    cam_update(&v->cam, (const float (*)[3])at, nat, dt, aspect);
    dircam_update(&v->cd, dt, me, v->nmon, &v->view);   /* the modes (the script camera) and their blend */
}

/* the hands each frame (0x323130, 0x3230F0): let go off a building (unmountBuilding's 0x322EC0, 0.1 s), the timers and
 * targets, then the IK's targets in the monster's frame */
static void hands_step(Mon *mo, float dt)
{
    Viewer *v = mo->v;
    /* 0x322360 (from 0x2E0FEC; not in states whose node flags have 0x1080, 0x50, 0x80002 or 0x40000): a planted hand
     * the arm cannot reach from below lifts the monster just enough, h = dz - sqrt(dz^2 - (|d|^2 - L^2)), at most
     * 0.4, the larger of the two hands' */
    const MoveState *st = mo->have_graph ? &mo->graph.states[mo->run.state] : NULL;
    if (st && !(st->flags & (0x1080 | 0x50 | 0x80002 | 0x40000))) {
        float lift = 0;
        for (int h = 0; h < 2; h++) {
            float sh[3], reach, d[3], d2 = 0;
            if (!mo->hand[h].planted || scene_monster_shoulder(&v->scene, mo->slot, h, sh, &reach) != 0)
                continue;
            for (int k = 0; k < 3; k++) {
                d[k] = mo->hand[h].tp[k] - sh[k];
                d2 += d[k] * d[k];
            }
            float disc = d[2] * d[2] - (d2 - reach * reach);
            if (d[2] < 0 || d2 < reach * reach || disc < 0)
                continue;
            float up = d[2] - sqrtf(disc);
            if (up <= 0.4f && up > lift)
                lift = up;
        }
        mo->mz += lift;
        if (lift > 0 && mo->is_ai)                   /* 0x2E1044: the goal's +0x38 */
            dir_ai_cooldown(&mo->ai);
    }
    float h0 = mo->heading / 57.2957795f, c = cosf(h0), sn = sinf(h0);
    for (int h = 0; h < 2; h++) {
        Hand *hd = &mo->hand[h];
        if (mo->mount.at.building < 0 && hd->target > 0)
            hand_detach(hd, 0.1f);
        Mat4 w;
        float anim[3] = { hd->tp[0], hd->tp[1], hd->tp[2] };
        if (scene_monster_hand(&v->scene, mo->slot, h, &w) == 0) {
            anim[0] = w.m[12];
            anim[1] = w.m[13];
            anim[2] = w.m[14];
        }
        hand_update(hd, h, &mo->mount, &v->scene, anim, dt);
        float wt = hand_weight(hd);
        if (getenv("RAMPAGE_HANDS") && (wt > 0 || hd->target > 0))
            printf("  hand %d: w %.2f%s%s, clip's at %.2f %.2f %.2f, held at %.2f %.2f %.2f\n", h, wt,
                   hd->attaching ? " attaching" : "", hd->planted ? " planted" : "", anim[0], anim[1], anim[2],
                   hd->sp[0], hd->sp[1], hd->sp[2]);
        v->scene.mon[mo->slot].ik_w[h] = hd->valid ? wt : 0;
        if (!hd->valid)
            continue;
        /* model space: the world less the monster's place, turned back by its heading */
        float d[3] = { hd->sp[0] - mo->mx, hd->sp[1] - mo->my, hd->sp[2] - mo->mz }, r[9], rz[9] = { c, -sn, 0, sn, c, 0, 0, 0, 1 };
        v->scene.mon[mo->slot].ik_pos[h][0] = c * d[0] + sn * d[1];
        v->scene.mon[mo->slot].ik_pos[h][1] = -sn * d[0] + c * d[1];
        v->scene.mon[mo->slot].ik_pos[h][2] = d[2];
        ik_rot_of(hd->sq, r);
        for (int col = 0; col < 3; col++)            /* Rz(-heading) x r */
            for (int k = 0; k < 3; k++)
                v->scene.mon[mo->slot].ik_rot[h][3 * col + k] = rz[k] * r[3 * col] + rz[3 + k] * r[3 * col + 1] + rz[6 + k] * r[3 * col + 2];
    }
}

/* gCommon.addStats's award to the block's challenge (0x346780): the object's template counts */
static void challenge_stats(Viewer *v, const char *name, int points)
{
    if (challenge_add_stats(&v->challenge, name, points) > 0 && getenv("RAMPAGE_TRACE"))
        printf("  challenge: %s counted, %d/%d%c", name, challenge_collected(&v->challenge),
               challenge_required(&v->challenge), 10);
}

/* a monster's damage on the MonsterUpgrade object (StreetObject.bsf TakeDamage: any type, so a blow or its body's
 * touch): once its delay is over, the move is player 1's monster's, whoever touched it (aquireUpgrade 0x2A62B8: into
 * GameStatus +0x234 + 4 x player 1's monster), the "Monster Upgrade" banner */
static void upgrade_touch(Mon *toucher, const float centre[3], float radius)
{
    Viewer *v = toucher->v;
    Mon *mo = &v->mon[0];
    if (v->challenge.up_state != CU_WAITING || mo->monster < 0)
        return;
    uint32_t mv = challenge_upgrade_hit(&v->challenge, centre, radius, &v->upgrades[mo->monster & 63]);
    if (!mv)
        return;
    if (v->fxw && v->upgrade_fx >= 0)
        fx_stop(v->fxw, v->upgrade_fx);
    v->upgrade_fx = -1;
    if (v->hud) {                                    /* gWorld.hideChallengeCompletionUI, then its banner */
        hud_hide_challenge_completion(v->hud);
        hud_banner_text(v->hud, v->challenge.up.text, v->challenge.up.colour, 1, "v_upgrade");
    }
    mo->power.abilities |= mv;
    int pm = profile_find_monster(&v->prof, v->monsters[mo->monster]);
    if (pm >= 0)                                     /* aquireUpgrade: into the profile's monster flags */
        profile_set_monster_flags(&v->prof, 0, pm, profile_monster_flags(&v->prof, 0, pm) | mv);
    if (getenv("RAMPAGE_TRACE"))
        printf("  upgrade taken: %s\n", challenge_upgrade_move(mv));
}

/* gCommon.getMonsterFlags: the moves the profile has for the monster (a new one: none; cheat 011235 all four) */
static void power_from_profile(Mon *mo)
{
    Viewer *v = mo->v;
    int pm = mo->monster >= 0 ? profile_find_monster(&v->prof, v->monsters[mo->monster]) : -1;
    if (pm < 0)
        return;                                      /* not in the roster: edible.c's all four */
    mo->power.abilities = profile_moves(&v->prof, 0, pm);
    mo->power.cheats = profile_cheat_flags(&v->prof);
    v->upgrades[mo->monster & 63] = profile_monster_flags(&v->prof, 0, pm) & 0xF;
}

/* Start: the Pause menu (WorldGuiDriver::process, event 128) */
static int pause_open(Viewer *v)
{
    HudPause p = { CITIES[v->city_index], v->game ? game_block_name(v->game, 1) : NULL };
    return v->hud && hud_pause_open(v->hud, 0, &p);
}

/* each monster's clip at its state's time (clip_at), for a posing at time t (the scene's clock); how many have their
 * move graph */
static int pose_monsters(Viewer *v, double t)
{
    int n = 0;
    for (int k = 0; k < v->nmon; k++) {
        Mon *mo = &v->mon[k];
        Instance *in = mon_in(mo);
        if (!in)
            continue;
        in->offset = (float)((mo->have_graph ? clip_at(mo, &mo->graph.states[mo->run.state], mo->run.time) :
                              mo->run.time) - t);
        n += mo->have_graph;
    }
    return n;
}

/* the game's next block begins: the scene is rebuilt around it, the monster and the cameras carried along (the block
 * left was 41 towards -y of it) */
static void game_next_block(Viewer *v)
{
    if (v->game_block < 0)
        return;
    for (int k = 0; k < v->nmon; k++)
        v->mon[k].my -= v->block_shift;
    v->cam.pos[1] -= v->block_shift;
    v->cd.out.pos[1] -= v->block_shift;
    v->cd.snap.pos[1] -= v->block_shift;
    v->view.pos[1] -= v->block_shift;
    v->block_shift = 0;
    v->block = (uint32_t)v->game_block;
    v->game_block = -1;
    v->game_built = 1;
    build(v);
    v->game_built = 0;
}

/* --do: "right 1, cross 0.5, wait 1.5": the stick held one way or a button held down, for that many seconds of play
 * (no seconds: one frame, a tap); "collapse" destroys the building in front of the monster, as collapseBuilding does;
 * "mirror" flips the mirror state for the next clip; "at x y [heading]" puts the monster there, facing the buildings (or that way);
 * "square+up 0.3": a button pressed with the stick held that way (a throw up, down);
 * "fx name" starts that effect 6 units in front of the monster, 3 up; "hitcell b floor col" hits that front cell of
 * building b as an attack would; "p2" (p1 .. p4): the words after it play
 * that player's monster (the others' keys idle meanwhile, the computer's monsters go on) */
static void play_script(Viewer *v, const char *script, float aspect)
{
    Mon *mo = &v->mon[0];                            /* the monster the words play */
    static const struct { const char *name; int button; float x, y; } WORDS[] = {
        { "left", -1, -1, 0 }, { "right", -1, 1, 0 }, { "up", -1, 0, -1 }, { "down", -1, 0, 1 }, { "wait", -1, 0, 0 },
        { "cross", MV_CROSS, 0, 0 }, { "square", MV_SQUARE, 0, 0 }, { "triangle", MV_TRIANGLE, 0, 0 },
        { "circle", MV_CIRCLE, 0, 0 }, { "r2", MV_R2, 0, 0 },
        { "rleft", -2, -1, 0 }, { "rright", -2, 1, 0 }, { "rup", -2, 0, -1 }, { "rdown", -2, 0, 1 },
        { "rcenter", -2, 0, 0 } };
    char word[32];
    const char *p = script;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        int n = 0;
        while (*p && *p != ' ' && *p != ',' && n < 31) word[n++] = *p++;
        word[n] = 0;
        float seconds = 0;
        while (*p == ' ') p++;
        if ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-') {
            char *end;
            seconds = (float)strtod(p, &end);
            p = end;
        }
        if ((word[0] == 'p' || word[0] == 'P') && word[1] >= '1' && word[1] <= '4' && !word[2]) {   /* p2: player 2 */
            int k = word[1] - '1';
            if (k < v->nmon)
                mo = &v->mon[k];
            else
                printf("no player %d\n", k + 1);
            continue;
        }
        if (_stricmp(word, "mirror") == 0) {         /* the next clip mirrored, as an event of type 8 would */
            mo->mirror_queued = !mo->mirror_queued;
            continue;
        }
        if (_stricmp(word, "at") == 0) {             /* at x y: the monster stands there, facing the buildings */
            char *end;
            mo->mx = seconds;
            mo->my = (float)strtod(p, &end);
            p = end;
            mo->mz = 0;
            mo->heading = 0;
            while (*p == ' ') p++;
            if ((*p >= '0' && *p <= '9') || *p == '-') {   /* at x y heading */
                mo->heading = (float)strtod(p, &end);
                p = end;
            }
            continue;
        }
        if (_stricmp(word, "fx") == 0) {             /* fx name: an effect in front of the monster */
            char name[64];
            int k = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && *p != ',' && k < 63) name[k++] = *p++;
            name[k] = 0;
            float h = mo->heading / 57.2957795f, at[3] = { mo->mx - sinf(h) * 6, mo->my + cosf(h) * 6, mo->mz + 3 };
            if (v->fxw && fx_spawn(v->fxw, name, at, NULL, 1) < 0)
                printf("no effect %s\n", name);
            continue;
        }
        if (_stricmp(word, "award") == 0) {          /* award name: gCommon.addStats of an object of that template */
            char name[64];
            int k = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && *p != ',' && k < 63) name[k++] = *p++;
            name[k] = 0;
            challenge_stats(v, name, 0);
            continue;
        }
        if (_stricmp(word, "pause") == 0) {          /* Start: the Pause menu */
            pause_open(v);
            continue;
        }
        if (_stricmp(word, "menu") == 0) {           /* menu up / down / left / right / accept / back / start */
            static const struct { const char *name; int ev; } EV[] = {
                { "up", HUD_EV_UP }, { "down", HUD_EV_DOWN }, { "left", HUD_EV_LEFT }, { "right", HUD_EV_RIGHT },
                { "accept", HUD_EV_ACCEPT }, { "back", HUD_EV_BACK }, { "start", HUD_EV_START } };
            char name[16];
            int k = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && *p != ',' && k < 15) name[k++] = *p++;
            name[k] = 0;
            for (size_t i = 0; i < sizeof EV / sizeof *EV; i++)
                if (v->hud && _stricmp(name, EV[i].name) == 0)
                    hud_input(v->hud, EV[i].ev, 0);
            continue;
        }
        if (_stricmp(word, "collapseall") == 0) {    /* every building of the block falls (the end of a block) */
            for (uint32_t i = 0; i < v->scene.nbuildings; i++)
                building_destroy(&v->scene.buildings[i].b);
            continue;
        }
        if (_stricmp(word, "hitcell") == 0) {        /* hitcell b floor col: a hit on that front cell (player 1's) */
            char *end;
            int bi = (int)seconds, o = (int)strtol(p, &end, 10);
            p = end;
            int col = (int)strtol(p, &end, 10);
            p = end;
            if (bi >= 0 && (uint32_t)bi < v->scene.nbuildings) {
                SceneBuilding *sb = &v->scene.buildings[bi];
                float w = (sb->b.max[0] - sb->b.min[0]) / (sb->b.width > 0 ? sb->b.width : 1);
                /* the cell's middle: the ground floor 6 high, the storeys 3 (building.c) */
                float at[3] = { sb->b.min[0] + w * ((float)col + 0.5f), sb->b.min[1], 6.0f + 3.0f * ((float)o + 0.5f) };
                if (sb->b.state == BUILDING_STANDING)
                    sb->hitter = 0;
                int hit = building_hit(&sb->b, at, 2.0f);   /* (a monster attack's radius) */
                if (getenv("RAMPAGE_TRACE"))
                    printf("  hitcell %d %d %d at %.1f %.1f %.1f: %s (%d storeys, %.1f .. %.1f)\n", bi, o, col, at[0],
                           at[1], at[2], hit ? "hit" : "no damage", sb->b.n, sb->b.min[2], sb->b.max[2]);
            }
            continue;
        }
        if (_stricmp(word, "collapse") == 0) {       /* collapseBuilding on the building in front of the monster */
            for (uint32_t i = 0; i < v->scene.nbuildings; i++) {
                Building *b = &v->scene.buildings[i].b;
                if (mo->mx >= b->min[0] && mo->mx <= b->max[0] && mo->my < b->min[1] && mo->my > b->min[1] - 10)
                    building_destroy(b);
            }
            continue;
        }
        char *plus = strchr(word, '+');              /* button+direction: the stick held with it */
        float sx = 0, sy = 0;
        if (plus) {
            *plus = 0;
            for (size_t i = 0; i < sizeof WORDS / sizeof *WORDS; i++)
                if (WORDS[i].button == -1 && _stricmp(plus + 1, WORDS[i].name) == 0) {
                    sx = WORDS[i].x;
                    sy = WORDS[i].y;
                }
        }
        for (size_t i = 0; n && i < sizeof WORDS / sizeof *WORDS; i++) {
            if (_stricmp(word, WORDS[i].name) != 0)
                continue;
            uint8_t pressed[MV_COUNT] = { 0 }, held[MV_COUNT] = { 0 };
            if (WORDS[i].button >= 0) pressed[WORDS[i].button] = 1;
            int frames = (int)(seconds * 60 + 0.5f);
            if (frames < 1) frames = 1;
            for (int k = 0; k < frames; k++) {
                int right = WORDS[i].button == -2;  /* the right stick: held from here until rcenter */
                if (right) {
                    mo->pad.rx = WORDS[i].x;
                    mo->pad.ry = WORDS[i].y;
                }
                for (int j = 0; j < v->nmon; j++)    /* the keys' pads: this monster's from the words, the rest idle */
                    if (v->mon[j].drive != DRIVE_COMPUTER) {
                        Pad *pd = &v->mon[j].pad;
                        float rx = pd->rx, ry = pd->ry;
                        memset(pd, 0, sizeof *pd);
                        pd->rx = rx;
                        pd->ry = ry;
                    }
                mo->pad.lx = right ? 0 : WORDS[i].x + sx;
                mo->pad.ly = right ? 0 : WORDS[i].y + sy;
                memcpy(mo->pad.pressed, pressed, sizeof pressed);
                memcpy(mo->pad.held, held, sizeof held);
                play_step(v, 1.0f / 60, aspect);
                if (pose_monsters(v, 0)) {          /* posed each frame, as the main loop does */
                    scene_update(&v->scene, 0);
                    hit_spheres_all(v);
                }
                if (WORDS[i].button >= 0)
                    held[WORDS[i].button] = 1;
                memset(pressed, 0, sizeof pressed);
                game_next_block(v);                  /* the game moved on, as the main loop does */
            }
        }
    }
}

/* ---- the front end (shell.c): --shell starts there, and a game's Quit goes back */

/* the profile as the shell reads it */
static void shell_profile(Viewer *v, ShellProfile *sp)
{
    memset(sp, 0, sizeof *sp);
    sp->loaded = v->prof.loaded;
    sp->allow_save = v->prof.allow_save && v->prof_write;
    snprintf(sp->name, sizeof sp->name, "%s", v->prof.block.name);
    for (int p = 0; p < 4; p++)
        for (int m = 0; m < SHELL_MONSTERS; m++)
            sp->monster_flags[p][m] = (uint8_t)profile_monster_flags(&v->prof, p, m);
    for (int c = 0; c < SHELL_CITIES; c++) {
        sp->city_flags[c] = (uint8_t)profile_city_flags(&v->prof, c);
        sp->start_block[c] = profile_start_block(&v->prof, c);
    }
    sp->sound = profile_sound_volume(&v->prof);
    sp->music = profile_music_volume(&v->prof);
    sp->rumble = profile_rumble(&v->prof);
    sp->cheats = profile_cheat_flags(&v->prof);
    sp->nprofiles = v->prof_on_card ? 1 : 0;         /* one profile file: the "card" holds it or nothing */
    if (sp->nprofiles)
        snprintf(sp->profiles[0], sizeof sp->profiles[0], "%s", v->prof.block.name[0] ? v->prof.block.name : "Profile");
}

/* ---- the front end's 3D objects (shell_3d): each shape's model from its interface's bundle, every mesh on its node at
 * the rest pose (the RawAnim of the model's name: the globe's is one key); each effect played by fx.c over that bundle
 * (then fx.bdp), one FxWorld per bundle, the GuiEffect's instance its own [R 0x2377E0: not in the world's list] */
typedef struct {
    const Bundle *b; char name[40];
    Tree tree; Mat4 *node;
    SceneMesh mesh[24]; int mesh_node[24], nmesh;
} GuiModel;
typedef struct { const Bundle *b; FxLib *lib; FxWorld *w; } GuiFxLib;
typedef struct { const void *key; int gen, lib, fx, started, stopped; } GuiFx;
#define GUI_ITEMS 64
typedef struct GuiDraw {
    GuiModel model[4]; int nmodel;
    GuiFxLib lib[4]; int nlib;
    GuiFx fx[48]; int nfx;
    Shell3D item[GUI_ITEMS]; int nitem;
    SceneGui3D layer[GUI_ITEMS]; SceneGuiMesh mesh[256]; FxQuad quad[6144];
} GuiDraw;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static GuiModel *gui_model(GuiDraw *g, const Bundle *b, const char *file)
{
    char key[40];
    snprintf(key, sizeof key, "%s", file ? file : "");
    size_t n = strlen(key);
    if (n > 4 && !_stricmp(key + n - 4, ".pwk"))
        key[n - 4] = 0;
    for (int i = 0; i < g->nmodel; i++)
        if (g->model[i].b == b && !_stricmp(g->model[i].name, key))
            return &g->model[i];
    if (!b || !key[0] || g->nmodel >= 4)
        return NULL;
    GuiModel *m = &g->model[g->nmodel++];
    memset(m, 0, sizeof *m);
    m->b = b;
    snprintf(m->name, sizeof m->name, "%s", key);
    const BdpEntry *te = NULL, *raw = NULL;
    for (uint32_t i = 0; i < b->nentries; i++) {
        const BdpEntry *e = &b->entries[i];
        if (e->type == ET_TREE && !_stricmp(bdp_name(b, e->name), key))
            te = e;
        else if (e->type == ET_ANIM && e->size >= 0x44 && le32(e->data + 0x20) == 1 &&
                 !_stricmp(bdp_name(b, (uint32_t)(e->data[0x28] | e->data[0x29] << 8)), key))
            raw = e;                                 /* its RawAnim: the rest pose */
    }
    if (te && tree_read(b, te, raw, &m->tree) == 0 && m->tree.n) {
        m->node = calloc(m->tree.n, sizeof *m->node);
        for (uint32_t i = 0; m->node && i < m->tree.n; i++) {
            const TreeNode *nd = &m->tree.nodes[i];
            Mat4 local = mat_tq(nd->t, nd->q);
            m->node[i] = nd->parent >= 0 && (uint32_t)nd->parent < i ? mat_mul(m->node[nd->parent], local) : local;
        }
    }
    for (uint32_t i = 0; i < b->nentries && m->nmesh < 24; i++) {   /* <model>|<object> on the node <object> */
        const BdpEntry *e = &b->entries[i];
        const char *en = bdp_name(b, e->name);
        size_t kl = strlen(key);
        if (e->type != ET_MESH || _strnicmp(en, key, kl) || en[kl] != '|')
            continue;
        SceneMesh *sm = &m->mesh[m->nmesh];
        memset(sm, 0, sizeof *sm);
        if (mesh_read(e, &sm->mesh) != 0)
            continue;
        sm->b = b;
        m->mesh_node[m->nmesh] = -1;
        for (uint32_t k = 0; m->node && k < m->tree.n; k++)
            if (!_stricmp(m->tree.nodes[k].name, en + kl + 1))
                m->mesh_node[m->nmesh] = (int)k;
        m->nmesh++;
    }
    return m;
}

static int gui_lib(Viewer *v, GuiDraw *g, const Bundle *b)
{
    for (int i = 0; i < g->nlib; i++)
        if (g->lib[i].b == b)
            return g->lib[i].w ? i : -1;
    if (!b || g->nlib >= 4)
        return -1;
    GuiFxLib *l = &g->lib[g->nlib];
    const Bundle *bl[2] = { b, &v->fx_bundle };
    l->b = b;
    l->lib = v->elf_path[0] ? fx_open(v->elf_path, bl, v->have_fx_bundle ? 2 : 1) : NULL;
    l->w = l->lib ? fx_world(l->lib) : NULL;
    return l->w ? g->nlib++ : (g->nlib++, -1);
}

static void gui3d_free(Viewer *v)
{
    GuiDraw *g = v->gd;
    if (!g)
        return;
    for (int i = 0; i < g->nmodel; i++) {
        for (int k = 0; k < g->model[i].nmesh; k++)
            mesh_free(&g->model[i].mesh[k].mesh);
        tree_free(&g->model[i].tree);
        free(g->model[i].node);
    }
    for (int i = 0; i < g->nlib; i++) {
        fx_world_free(g->lib[i].w);
        fx_close(g->lib[i].lib);
    }
    free(g);
    v->gd = NULL;
}

/* where an effect is: its world, or on its shape's node (the node's frame x the shape's world) */
static Mat4 gui_effect_world(GuiDraw *g, const Shell3D *it)
{
    Mat4 w;
    memcpy(w.m, it->world, sizeof w.m);
    if (it->attach && it->parent >= 0 && it->parent < g->nitem) {
        const Shell3D *sh = &g->item[it->parent];
        GuiModel *m = gui_model(g, sh->b, sh->name);
        for (uint32_t k = 0; m && m->node && k < m->tree.n; k++)
            if (!_stricmp(m->tree.nodes[k].name, it->attach)) {
                Mat4 sw;
                memcpy(sw.m, sh->world, sizeof sw.m);
                w = mat_mul(sw, m->node[k]);
                break;
            }
    }
    return w;
}

/* the effects follow their GuiEffects: one running is started (once) and kept where it is drawn, one that stops
 * running is shut down, one whose edf ended says so; then every effect world steps */
static void gui3d_step(Viewer *v, float dt)
{
    if (!v->shell)
        return;
    if (!v->gd && !(v->gd = calloc(1, sizeof *v->gd)))
        return;
    GuiDraw *g = v->gd;
    g->nitem = shell_3d(v->shell, g->item, GUI_ITEMS);
    for (int i = 0; i < g->nitem; i++) {
        const Shell3D *it = &g->item[i];
        if (it->kind != SHELL_3D_EFFECT)
            continue;
        GuiFx *x = NULL;
        for (int k = 0; k < g->nfx && !x; k++)
            if (g->fx[k].key == it->key)
                x = &g->fx[k];
        if (!x) {
            if (g->nfx >= 48)
                continue;
            x = &g->fx[g->nfx++];
            memset(x, 0, sizeof *x);
            x->key = it->key;
            x->gen = it->gen;
            x->fx = -1;
            x->lib = gui_lib(v, g, it->b);
        }
        FxWorld *w = x->lib >= 0 ? g->lib[x->lib].w : NULL;
        if (!w)
            continue;
        if (x->gen != it->gen) {                     /* setEffect: the old one is freed */
            fx_kill(w, x->fx);
            x->fx = -1;
            x->started = 0;
            x->gen = it->gen;
        }
        Mat4 m = gui_effect_world(g, it);
        float pos[3] = { m.m[12], m.m[13], m.m[14] }, R[3][3], scale = 0;
        for (int r = 0; r < 3; r++) {
            float l = sqrtf(m.m[4 * r] * m.m[4 * r] + m.m[4 * r + 1] * m.m[4 * r + 1] + m.m[4 * r + 2] * m.m[4 * r + 2]);
            if (r == 0) scale = l;
            for (int c = 0; c < 3; c++)
                R[r][c] = l > 0 ? m.m[4 * r + c] / l : (r == c);
        }
        if (it->running) {
            if (!x->started) {                       /* [G: the frame's scale scales the effect] */
                x->fx = fx_spawn_frame(w, it->name, pos, R, scale > 0 ? scale : 1);
                x->started = 1;
                x->stopped = 0;
                if (getenv("RAMPAGE_TRACE"))
                    printf("  gui effect %s at %.2f %.2f %.2f%s\n", it->name, pos[0], pos[1], pos[2],
                           x->fx < 0 ? ": none" : "");
                if (x->fx < 0)
                    shell_effect_ended(v->shell, it->key);
            } else if (fx_alive(w, x->fx)) {
                if (it->stopping && !x->stopped) {   /* stopEffect: its particles play out */
                    fx_stop(w, x->fx);
                    x->stopped = 1;
                }
                if (it->layer >= 0)
                    fx_place(w, x->fx, pos, R);
            } else {                                 /* it is over */
                shell_effect_ended(v->shell, it->key);
                x->started = 0;
                x->fx = -1;
            }
        } else if (x->started) {
            fx_kill(w, x->fx);
            x->fx = -1;
            x->started = 0;
        }
    }
    for (int i = 0; i < g->nlib; i++)
        fx_update(g->lib[i].w, dt, -1e9f);
}

/* this frame's 3D layers for the renderer, one per HUD_TEX_3D quad */
static void gui3d_layers(Viewer *v, Scene *s)
{
    s->gui3d = NULL;
    s->ngui3d = 0;
    if (!v->shell)
        return;
    if (!v->gd && !(v->gd = calloc(1, sizeof *v->gd)))
        return;
    GuiDraw *g = v->gd;
    g->nitem = shell_3d(v->shell, g->item, GUI_ITEMS);
    int nl = 0, nm = 0, nq = 0;
    for (int i = 0; i < g->nitem; i++) {
        const Shell3D *it = &g->item[i];
        if (it->layer < 0)
            continue;
        while (nl <= it->layer && nl < GUI_ITEMS)
            memset(&g->layer[nl++], 0, sizeof g->layer[0]);
        if (it->layer >= GUI_ITEMS)
            continue;
        SceneGui3D *l = &g->layer[it->layer];
        if (it->kind == SHELL_3D_SHAPE) {
            GuiModel *m = gui_model(g, it->b, it->name);
            l->meshes = &g->mesh[nm];
            for (int k = 0; m && k < m->nmesh && nm < 256; k++) {
                Mat4 w;
                memcpy(w.m, it->world, sizeof w.m);
                if (m->mesh_node[k] >= 0)
                    w = mat_mul(w, m->node[m->mesh_node[k]]);
                g->mesh[nm].mesh = &m->mesh[k];
                g->mesh[nm++].world = w;
                l->nmeshes++;
            }
            l->lit = it->lit;
            memcpy(l->ambient, it->ambient, sizeof l->ambient);
            l->nlights = it->nlights;
            memcpy(l->light_dir, it->light_dir, sizeof l->light_dir);
            memcpy(l->light_diffuse, it->light_diffuse, sizeof l->light_diffuse);
            memcpy(l->light_ambient, it->light_ambient, sizeof l->light_ambient);
        } else {
            for (int k = 0; k < g->nfx; k++) {
                const GuiFx *x = &g->fx[k];
                if (x->key != it->key || x->lib < 0)
                    continue;
                l->fx = &g->quad[nq];
                FxWorld *w = g->lib[x->lib].w;
                if (x->fx >= 0)
                    nq += fx_quads_of(w, x->fx, g->quad + nq, 6144 - nq);
                l->nfx = (int)(&g->quad[nq] - l->fx);
            }
        }
    }
    s->gui3d = g->layer;
    s->ngui3d = nl;
}

static void shell_enter(Viewer *v, const ShellStart *st)
{
    ShellProfile sp;
    shell_profile(v, &sp);
    v->shell = shell_open(&v->vol, st, &sp);
    static Scene empty;                              /* nothing but the screens, black around their 4:3 */
    memset(&empty, 0, sizeof empty);
    empty.fog_end = 1;
    v->shell_scene = &empty;
    if (v->shell)
        rnd_scene(v->rnd, v->shell_scene);           /* (the texture cache is keyed by the bundles: anew) */
}

/* the movie's sound, pulled by the mixer */
static int movie_pull(void *ctx, int16_t *out, int frames) { return movie_audio(ctx, out, frames); }

static void movie_stop(Viewer *v)
{
    if (!v->movie)
        return;
    sound_stream(NULL, NULL);
    rnd_video(v->rnd, NULL, 0, 0);
    movie_close(v->movie);
    v->movie = NULL;
    if (v->shell)
        shell_movie_picture(v->shell, 0);
}

/* SHELL_EVT_MOVIE: playVideo(name), name.pss from the Vol (the NTSC ones) */
static void movie_start(Viewer *v, const char *name)
{
    movie_stop(v);
    v->movie = movie_open(&v->vol, name);
    if (!v->movie) {
        printf("no movie %s\n", name);
        return;
    }
    shell_movie_hold(v->shell);
    sound_stream(movie_pull, v->movie);
    if (getenv("RAMPAGE_TRACE"))
        printf("  movie %s\n", name);
}

/* the movie at the shell's time: its picture to the Video quad; at its end the shell goes on; skipped: stopped */
static void movie_step(Viewer *v)
{
    if (!v->movie)
        return;
    float t = v->shell ? shell_movie_clock(v->shell) : -1;
    if (t < 0) {
        movie_stop(v);
        return;
    }
    int w, h, changed;
    const uint8_t *pic = movie_video(v->movie, t, &changed);
    movie_size(v->movie, &w, &h);
    if (pic) {
        if (changed)
            rnd_video(v->rnd, pic, w, h);
        shell_movie_picture(v->shell, 1);
    }
    if (movie_done(v->movie, t)) {
        if (getenv("RAMPAGE_TRACE"))
            printf("  movie over: %d pictures\n", movie_pictures(v->movie));
        shell_movie_end(v->shell);
    }
}

/* what the shell asked for since the last call; 1 when a game is to start */
static int shell_answer(Viewer *v)
{
    ShellEvent ev[32];
    int n, start = 0;
    float sv, mv;
    int rumble;
    while ((n = shell_events(v->shell, ev, 32)) > 0)
        for (int i = 0; i < n; i++) {
            const ShellEvent *e = &ev[i];
            if (getenv("RAMPAGE_TRACE"))
                printf("  shell event %d (%s) %d %.2f\n", e->kind, e->name, e->arg, e->value);
            switch (e->kind) {
            case SHELL_EVT_SOUND: sound_play(e->name, NULL, 1); break;
            case SHELL_EVT_MUSIC:
                if (e->name[0])
                    music_replace(e->name, 0.5f, 0.5f);
                else
                    music_stop(0.5f);
                break;
            case SHELL_EVT_MUSIC_VOLUME: v->music_now = e->value / 0.75f; sound_volume(v->sfx_vol, v->music_now); break;
            case SHELL_EVT_SFX_VOLUME: v->sfx_vol = e->value / 0.75f; sound_volume(v->sfx_vol, v->music_now); break;
            case SHELL_EVT_SAVE_SETTINGS:            /* Options' Accept: into the profile, saved */
                shell_settings(v->shell, &sv, &mv, &rumble);
                profile_set_sound_volume(&v->prof, sv);
                profile_set_music_volume(&v->prof, mv);
                profile_set_rumble(&v->prof, rumble, 1);
                v->sfx_vol = sv / 0.75f;
                v->music_vol = v->music_now = mv / 0.75f;
                sound_volume(v->sfx_vol, v->music_now);
                if (v->prof_write && v->prof.loaded)
                    profile_write(&v->prof, v->prof_path, 1, (uint64_t)time(NULL) * 1000);
                break;
            case SHELL_EVT_PROFILE_LOAD: {           /* the one profile file */
                ShellProfile sp;
                if (profile_read(&v->prof, v->prof_path) > 0) {
                    v->prof.loaded = 1;
                    profile_restore_checkpoint(&v->prof);
                }
                shell_profile(v, &sp);
                shell_set_profile(v->shell, &sp);
                break;
            }
            case SHELL_EVT_PROFILE_CREATE: {
                ShellProfile sp;
                profile_create(&v->prof, e->name);
                v->prof.loaded = 1;
                if (v->prof_write && profile_write(&v->prof, v->prof_path, 1, (uint64_t)time(NULL) * 1000) > 0)
                    v->prof_on_card = 1;
                shell_profile(v, &sp);
                shell_set_profile(v->shell, &sp);
                break;
            }
            case SHELL_EVT_PROFILE_NONE: v->prof.allow_save = 0; break;
            case SHELL_EVT_BUDDY_PROFILE: shell_buddy_profile(v->shell, -1); break;   /* no second profile here */
            case SHELL_EVT_CHEATS: v->prof.cheats = (uint32_t)e->arg; break;
            case SHELL_EVT_START: start = 1; break;
            case SHELL_EVT_MOVIE: movie_start(v, e->name); break;
            default: break;                          /* the emulators, the demo game: skipped */
            }
        }
    movie_step(v);
    return start;
}

/* the index of a monster by name (Viewer.monsters), -1 none */
static int monster_index(const Viewer *v, const char *name)
{
    for (uint32_t i = 0; name && i < v->nmonsters; i++)
        if (!_stricmp(name, v->monsters[i]))
            return (int)i;
    return -1;
}

/* the players and bots of a game: n monsters (monster[k] of Viewer.monsters each), the last `bots` of them the
 * computer's. Player 1 plays on the first keys, player 2 (not a bot) on the second; the computer drives the rest.
 * They stand in the street as BeginLevelNI puts them: x = -17 (n - 1) / 2 + 17 i, facing the camera */
static void setup_players(Viewer *v, int n, int bots, const int *monster)
{
    v->nmon = n < 1 ? 1 : n > MONSTERS ? MONSTERS : n;
    v->num_bots = bots < 0 ? 0 : bots >= v->nmon ? v->nmon - 1 : bots;
    for (int k = 0; k < MONSTERS; k++) {
        Mon *mo = &v->mon[k];
        mo->v = v;
        mo->slot = k;
        mo->monster = k < v->nmon ? monster[k] : -1;
        mo->bot = k < v->nmon && k >= v->nmon - v->num_bots;
        mo->drive = mo->bot || k >= 2 ? DRIVE_COMPUTER : k == 1 ? DRIVE_KEYS2 : DRIVE_KEYS1;
        mo->mx = -17.0f * (float)(v->nmon - 1) / 2 + 17.0f * (float)k;
        mo->my = -18;
        mo->heading = 180;
        memset(&mo->pad, 0, sizeof mo->pad);
        mo->reveal = mo->shown = 0;
    }
    v->extra = -1;
}

/* gShell.startGame: the game the shell chose (GameStatus), the city opened at its block */
static void shell_start_game(Viewer *v)
{
    const ShellGame *g = shell_game(v->shell);
    int city = v->city_index;
    for (int i = 0; i < NCITIES; i++)
        if (!_stricmp(g->city, CITIES[i]))
            city = i;
    v->play_type = g->play_type;
    v->night = g->night;
    int pick[MONSTERS], n = g->num_players < 1 ? 1 : g->num_players > MONSTERS ? MONSTERS : g->num_players;
    for (int k = 0; k < n; k++)                      /* the players' monsters, then the bots' (GameStatus +0x14) */
        pick[k] = monster_index(v, g->monster_name[k]);
    for (int k = 1; k < n; k++)                      /* a slot the shell left empty: player 1's monster [G] */
        if (pick[k] < 0)
            pick[k] = pick[0];
    setup_players(v, n, g->num_bots, pick);
    v->clip = -1;
    v->play = have_monster(v);
    v->last_game = *g;
    movie_stop(v);
    gui3d_free(v);
    shell_close(v->shell);
    v->shell = NULL;
    v->exit_game = 0;
    open_city(v, city, (uint32_t)(v->last_game.start_block > 0 ? v->last_game.start_block : 0));   /* (g is gone) */
}

/* the shell's frame: the pad (Enter / Space accept, Backspace back, the arrows, P start, C / V / R the buttons,
 * letters and digits into the name entry, F2 L2 + R2 held for the cheat entry), its time, its answers */
static void shell_frame(Viewer *v, const Window *w, float dt)
{
    static const struct { int key, ev, button; } KEYS[] = {
        { VK_RETURN, HUD_EV_ACCEPT, 0 }, { VK_SPACE, HUD_EV_ACCEPT, 0 }, { VK_BACK, HUD_EV_BACK, 0 },
        { VK_UP, HUD_EV_UP, 0 }, { VK_DOWN, HUD_EV_DOWN, 0 }, { VK_LEFT, HUD_EV_LEFT, 0 }, { VK_RIGHT, HUD_EV_RIGHT, 0 },
        { 'P', HUD_EV_START, 0 }, { 'C', HUD_EV_BUTTON, 0 }, { 'V', HUD_EV_BUTTON, 1 }, { 'R', HUD_EV_BUTTON, 4 } };
    const char *focus = shell_focus(v->shell);
    int typing = focus && !strcmp(focus, "MemoryCard");
    for (int c = 'A'; typing && c <= 'Z'; c++)
        if (w->pressed[c])
            shell_type(v->shell, c);
    for (int c = '0'; typing && c <= '9'; c++)
        if (w->pressed[c])
            shell_type(v->shell, c);
    if (typing && w->pressed[VK_RETURN])             /* the name entry: Enter is DONE, Backspace rubs out */
        shell_type(v->shell, '\r');
    if (typing && w->pressed[VK_BACK])
        shell_type(v->shell, '\b');
    for (size_t k = 0; k < sizeof KEYS / sizeof *KEYS; k++)
        if (w->pressed[KEYS[k].key] && !(typing && ((KEYS[k].key >= 'A' && KEYS[k].key <= 'Z') ||
                                                    KEYS[k].key == VK_RETURN || KEYS[k].key == VK_BACK)))
            shell_input(v->shell, KEYS[k].ev, KEYS[k].button);
    int held = (w->keys[VK_LEFT] ? SHELL_HELD_LEFT : 0) | (w->keys[VK_RIGHT] ? SHELL_HELD_RIGHT : 0) |
               (w->keys[VK_F2] ? SHELL_HELD_L2 | SHELL_HELD_R2 : 0);
    shell_pad_held(v->shell, 0, held);
    shell_update(v->shell, dt);
    gui3d_step(v, dt);
    if (shell_answer(v))
        v->shell_go = 1;                             /* after this frame: the loading screen is drawn once */
}

/* --shell with --shot: the --do words for the shell (wait S at 1/60 s, menu <ev>, button N, type TEXT, done); the
 * words left once a game starts (they play in it), NULL none */
static const char *shell_script(Viewer *v, const char *script)
{
    static const struct { const char *name; int ev; } EV[] = {
        { "up", HUD_EV_UP }, { "down", HUD_EV_DOWN }, { "left", HUD_EV_LEFT }, { "right", HUD_EV_RIGHT },
        { "accept", HUD_EV_ACCEPT }, { "back", HUD_EV_BACK }, { "start", HUD_EV_START } };
    const char *p = script;
    char word[32], arg[32];
    while (*p && v->shell && !v->shell_go) {
        while (*p == ' ' || *p == ',') p++;
        int n = 0, k = 0;
        while (*p && *p != ' ' && *p != ',' && n < 31) word[n++] = *p++;
        word[n] = 0;
        while (*p == ' ') p++;
        while (*p && *p != ',' && k < 31) arg[k++] = *p++;
        while (k > 0 && arg[k - 1] == ' ') k--;
        arg[k] = 0;
        if (!_stricmp(word, "wait"))
            for (int f = 0, frames = (int)(atof(arg) * 60 + 0.5); f < frames && !v->shell_go; f++) {
                shell_update(v->shell, 1.0f / 60);
                gui3d_step(v, 1.0f / 60);
                if (shell_answer(v))
                    v->shell_go = 1;
            }
        else if (!_stricmp(word, "menu"))
            for (size_t i = 0; i < sizeof EV / sizeof *EV; i++) {
                if (!_stricmp(arg, EV[i].name))
                    shell_input(v->shell, EV[i].ev, 0);
            }
        else if (!_stricmp(word, "button"))
            shell_input(v->shell, HUD_EV_BUTTON, atoi(arg));
        else if (!_stricmp(word, "done"))            /* the name entry's DONE */
            shell_type(v->shell, '\r');
        else if (!_stricmp(word, "type"))
            for (const char *c = arg; *c; c++)
                shell_type(v->shell, toupper((unsigned char)*c));
        shell_update(v->shell, 1.0f / 60);
        gui3d_step(v, 1.0f / 60);
        if (shell_answer(v))
            v->shell_go = 1;
    }
    while (*p == ' ' || *p == ',') p++;
    return *p ? p : NULL;
}

/* a player's pad from the keys. Player 1: W A S D / the left stick, the arrows the right stick, Space Cross (jump), J
 * Square (punch), K Triangle (kick), L Circle (grab), R R2 (roar). Player 2: the numeric keypad (Num Lock on): 8 4 2
 * 6 (or 5) the left stick, 0 Cross, 7 Square, 9 Triangle, 1 Circle, 3 R2, / and * the right stick left and right */
static void keys_pad(const Window *w, int drive, Pad *pd)
{
    static const struct { int key, button; } PAD1[] = {
        { VK_SPACE, MV_CROSS }, { 'J', MV_SQUARE }, { 'K', MV_TRIANGLE }, { 'L', MV_CIRCLE }, { 'R', MV_R2 } };
    static const struct { int key, button; } PAD2[] = {
        { VK_NUMPAD0, MV_CROSS }, { VK_NUMPAD7, MV_SQUARE }, { VK_NUMPAD9, MV_TRIANGLE }, { VK_NUMPAD1, MV_CIRCLE },
        { VK_NUMPAD3, MV_R2 } };
    memset(pd->pressed, 0, sizeof pd->pressed);
    memset(pd->held, 0, sizeof pd->held);
    if (drive == DRIVE_KEYS1) {
        pd->lx = (float)(w->keys['D'] - w->keys['A']);
        pd->ly = (float)(w->keys['S'] - w->keys['W']);
        pd->rx = (float)(w->keys[VK_RIGHT] - w->keys[VK_LEFT]);
        pd->ry = (float)(w->keys[VK_DOWN] - w->keys[VK_UP]);
        for (size_t k = 0; k < sizeof PAD1 / sizeof *PAD1; k++) {
            pd->pressed[PAD1[k].button] = w->pressed[PAD1[k].key];
            pd->held[PAD1[k].button] = w->keys[PAD1[k].key];
        }
    } else if (drive == DRIVE_KEYS2) {
        pd->lx = (float)(w->keys[VK_NUMPAD6] - w->keys[VK_NUMPAD4]);
        pd->ly = (float)((w->keys[VK_NUMPAD2] || w->keys[VK_NUMPAD5]) - w->keys[VK_NUMPAD8]);
        pd->rx = (float)(w->keys[VK_MULTIPLY] - w->keys[VK_DIVIDE]);
        pd->ry = 0;
        for (size_t k = 0; k < sizeof PAD2 / sizeof *PAD2; k++) {
            pd->pressed[PAD2[k].button] = w->pressed[PAD2[k].key];
            pd->held[PAD2[k].button] = w->keys[PAD2[k].key];
        }
    }
}

int main(int argc, char **argv)
{
    static Viewer v;
    if (getenv("RAMPAGE_TRACE"))                     /* a trace survives a crash */
        setvbuf(stdout, NULL, _IONBF, 0);
    const char *shot = NULL, *monster[MONSTERS] = { NULL }, *pos[3] = { "build\\disc\\RAMPAGE.VOL", "Chicago", "1" };
    int npos = 0, night = 0, clip = -1, have_view = 0, mute = 0, players = 1, bots = 0;
    float view[5] = { 0 };
    v.sfx_vol = v.music_vol = v.music_now = 1;      /* the Options page: as the game until changed */
    int newprofile = 0;
    const char *cheat = NULL;
    double shot_time = 0;
    const char *script = NULL;
    char walk_script[48];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) { shot = argv[++i]; v.unattended = 1; }
        else if (strcmp(argv[i], "--monster") == 0 && i + 1 < argc) monster[0] = argv[++i];
        else if (!strncmp(argv[i], "--monster", 9) && argv[i][9] >= '1' && argv[i][9] <= '4' && !argv[i][10] &&
                 i + 1 < argc)               /* --monster2 Lizzie: player 2's (--monster1 is --monster) */
        {
            int k = argv[i][9] - '1';
            monster[k] = argv[++i];
        }
        else if (strcmp(argv[i], "--players") == 0 && i + 1 < argc) players = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bots") == 0 && i + 1 < argc) bots = atoi(argv[++i]);
        else if (strcmp(argv[i], "--clip") == 0 && i + 1 < argc) clip = atoi(argv[++i]);
        else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) shot_time = atof(argv[++i]);
        else if (strcmp(argv[i], "--walk") == 0 && i + 1 < argc) {
            snprintf(walk_script, sizeof walk_script, "right %s", argv[++i]);
            script = walk_script;
        }
        else if (strcmp(argv[i], "--do") == 0 && i + 1 < argc) script = argv[++i];
        else if (strcmp(argv[i], "--night") == 0) night = 1;
        else if (strcmp(argv[i], "--nofog") == 0) v.fog = -1;
        else if (strcmp(argv[i], "--mute") == 0) mute = 1;
        else if (strcmp(argv[i], "--nohud") == 0) v.no_hud = 1;
        else if (strcmp(argv[i], "--newprofile") == 0) newprofile = 1;
        else if (strcmp(argv[i], "--shell") == 0) v.shell_mode = 1;
        else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) v.prof_path = argv[++i];
        else if (strcmp(argv[i], "--cheat") == 0 && i + 1 < argc) cheat = argv[++i];
        else if (strcmp(argv[i], "--play") == 0 && i + 1 < argc) {   /* campaign, kingcity, kingworld, timed */
            const char *m = argv[++i];
            v.play_type = !_stricmp(m, "kingcity") ? GP_KING_CITY : !_stricmp(m, "kingworld") ? GP_KING_WORLD :
                          !_stricmp(m, "timed") ? GP_TIMED_RUN : GP_CAMPAIGN;
        }
        else if (strcmp(argv[i], "--cam") == 0 && i + 5 < argc) {   /* x y z yaw pitch (degrees): a fixed view */
            for (int k = 0; k < 5; k++)
                view[k] = (float)atof(argv[++i]);
            have_view = 1;
        }
        else if (npos < 3) pos[npos++] = argv[i];
    }
    if (vol_open(&v.vol, pos[0]) != 0) {
        char msg[512];
        snprintf(msg, sizeof msg, "Cannot read %s.\nRun build.ps1 first (it extracts the disc), or pass the path to "
                 "RAMPAGE.VOL.", pos[0]);
        plat_message(msg, 1);
        return 1;
    }
    int city = 0;
    for (int i = 0; i < NCITIES; i++)
        if (_stricmp(pos[1], CITIES[i]) == 0)
            city = i;
    uint32_t block = (uint32_t)(atoi(pos[2]) > 0 ? atoi(pos[2]) - 1 : 0);
    Window w;
    if (win_open(&w, "RampageRE", 1280, 720, shot != NULL) != 0) {   /* a --shot run never shows a window */
        plat_message("Cannot open an OpenGL window.", 1);
        return 1;
    }
    v.fog = v.fog < 0 ? 0 : 1;                       /* the game's fog (LightingData FogStart / FogEnd), F toggles it */
    v.rnd = rnd_create();
    /* the game's sound (sound.c: Rampage.uber / .samp, the .mic music); the output only for a window of one's own,
     * never for a --shot run, nor with --mute */
    if (sound_init(&v.vol) > 0 && !shot && !mute && sound_start() != 0)
        printf("no sound output\n");
    {                                                /* the effects: fx.bdp, its words from RAMPAGE.ELF beside the VOL */
        uint32_t size = 0;
        uint8_t *data = vol_read(&v.vol, "fx.bdp", &size);
        uint32_t gsize = 0, csize = 0;                /* the HUD's: WorldGui.bdp and CommonGUI.bdp */
        uint8_t *gui = vol_read(&v.vol, "WorldGui.bdp", &gsize), *common = vol_read(&v.vol, "CommonGUI.bdp", &csize);
        v.have_gui = gui && common && bdp_load(&v.gui_world, gui, gsize) == 0 && bdp_load(&v.gui_common, common, csize) == 0;
        if (data && bdp_load(&v.fx_bundle, data, size) == 0) {
            char elf[512];
            snprintf(elf, sizeof elf, "%s", pos[0]);
            char *slash = strrchr(elf, '\\'), *fwd = strrchr(elf, '/');
            if (fwd > slash) slash = fwd;
            snprintf(slash ? slash + 1 : elf, sizeof elf - (size_t)(slash ? slash + 1 - elf : 0), "RAMPAGE.ELF");
            const Bundle *bl[1] = { &v.fx_bundle };
            snprintf(v.elf_path, sizeof v.elf_path, "%s", elf);
            v.have_fx_bundle = 1;
            v.fxlib = fx_open(elf, bl, 1);
            v.fxw = v.fxlib ? fx_world(v.fxlib) : NULL;
            if (v.fxw) {
                fx_on_shake(v.fxw, fx_shake, &v);
                fx_on_sound(v.fxw, fx_sound, &v);
            }
            for (int i = 0; v.fxlib && i < fx_shape_count(v.fxlib) && i < 32; i++) {   /* the debris models */
                const BdpEntry *me[8];
                int nm = fx_shape_meshes(v.fxlib, i, me, 8);
                for (int k = 0; k < nm; k++)
                    if (mesh_read(me[k], &v.fx_shapes[i][v.fx_nshapes[i]].mesh) == 0)
                        v.fx_shapes[i][v.fx_nshapes[i]++].b = fx_shape_bundle(v.fxlib, i);
            }

        } else
            free(data);
    }
    v.night = night;
    v.show_hits = 1;
    profile_init(&v.prof);                           /* the profile: rampage_profile.bin (a --shot run: a new one,
                                                      * not written, unless --profile names one) */
    v.prof_write = !shot || v.prof_path;
    if (!v.prof_path)
        v.prof_path = "rampage_profile.bin";
    if (!newprofile && v.prof_write && profile_read(&v.prof, v.prof_path) > 0) {
        printf("profile %s\n", v.prof_path);
        v.prof_on_card = 1;
        if (v.shell_mode)                            /* the shell's popup loads it */
            profile_init(&v.prof);
    }
    profile_restore_checkpoint(&v.prof);              /* MainMenu's campaign start */
    if (cheat && !profile_cheat_code(&v.prof, cheat))
        printf("no cheat %s\n", cheat);
    v.sfx_vol = profile_sound_volume(&v.prof) / 0.75f;
    v.music_vol = v.music_now = profile_music_volume(&v.prof) / 0.75f;
    sound_volume(v.sfx_vol, v.music_now);
    v.nmonsters = monster_names(&v.vol, v.monsters, 64);
    v.clip = clip;
    {                                                /* --players N (1 .. 4, --bots B of them the computer's) */
        static const char *const OTHERS[] = { "George", "Lizzie", "Ralph", "Boosy" };   /* the ones not named */
        int pick[MONSTERS];
        players = players < 1 ? 1 : players > MONSTERS ? MONSTERS : players;
        for (int k = 0; k < players; k++) {
            pick[k] = monster_index(&v, monster[k]);
            for (int j = 0; (k || players > 1) && pick[k] < 0 && j < 4; j++) {   /* one of the classic three (and
                                                      * Boosy) not taken yet; alone without --monster: none */
                int c = monster_index(&v, OTHERS[j]), taken = 0;
                for (int q = 0; q < k; q++)
                    taken |= pick[q] == c;
                if (!taken)
                    pick[k] = c;
            }
        }
        if (pick[0] < 0)                             /* no monster: the free camera only */
            players = 1;
        setup_players(&v, players, bots, pick);
    }
    v.play = have_monster(&v) && clip < 0;
    if (v.shell_mode) {                              /* the front end first: the city once a game starts */
        shell_enter(&v, NULL);
        if (!v.shell) {
            plat_message("The shell's bundles are not in RAMPAGE.VOL.", 1);
            return 1;
        }
        if (shot && script)                          /* its words, then the shot (of the shell, or of the game, which
                                                      * plays the words left) */
            script = shell_script(&v, script);
        if (!v.shell_go)
            script = NULL;
        if (v.shell_go) {
            v.shell_go = 0;
            shell_start_game(&v);
        }
    } else if (open_city(&v, city, block) != 0) {
        plat_message("The city's bundles are not in RAMPAGE.VOL.", 1);
        return 1;
    }
    for (uint32_t i = 0; getenv("RAMPAGE_TRACE") && i < v.scene.nbuildings; i++) {
        const Building *b = &v.scene.buildings[i].b;
        printf("building %u: x %.0f .. %.0f, y %.0f .. %.0f, top %.1f, row %d, %d wide\n", i, b->min[0], b->max[0],
               b->min[1], b->max[1], b->max[2], b->row, b->width);
    }
    for (uint32_t i = 0; getenv("RAMPAGE_TRACE") && i < v.scene.nobstacles; i++) {
        const float *o = v.scene.obstacles[i];
        printf("obstacle %u: %.1f %.1f %.1f .. %.1f %.1f %.1f\n", i, o[0], o[1], o[2], o[3], o[4], o[5]);
    }
    Camera cam = { { 0, -110, 45 }, 3.14159265f / 2, -0.3f, 60 };
    if (have_monster(&v)) {                                /* closer, the monster in front */
        cam.pos[1] = -55; cam.pos[2] = 18; cam.pitch = -0.2f;
    }
    float speed = 30;
    double last = win_time(), title_at = last, start = last;
    int frames = 0;
    while (!w.quit) {
        win_poll(&w);
        double now = win_time();
        float dt = (float)(now - last);
        last = now;
        if (w.pressed[VK_ESCAPE]) w.quit = 1;
        if (v.shell) {                               /* the front end has the screen and the pad */
            if (v.shell_go) {                        /* the loading screen was drawn: the game */
                v.shell_go = 0;
                shell_start_game(&v);
                continue;
            }
            if (!shot)
                shell_frame(&v, &w, dt > 0.1f ? 0.1f : dt);
            static HudQuad sq[4096];
            v.shell_scene->hud = sq;
            v.shell_scene->nhud = shell_quads(v.shell, sq, 4096);
            gui3d_layers(&v, v.shell_scene);
            Camera none = { { 0, 0, 0 }, 0, 0, 60 };
            sound_update(shot ? 0 : (dt > 0.1f ? 0.1f : dt));
            rnd_frame(v.rnd, &w, &none, now - start, 0);
            if (shot) {
                int ok = shot_png(shot, w.width, w.height) == 0;
                printf(ok ? "wrote %s\n" : "cannot write %s\n", shot);
                break;
            }
            win_swap(&w);
            continue;
        }
        if (v.hud && hud_results_showing(v.hud)) {   /* a results screen has the pad: Enter / Space go on,
                                                      * Backspace quits, the arrows choose, C changes monster */
            static const struct { int key, ev, button; } KEYS[] = {
                { VK_RETURN, HUD_EV_ACCEPT, 0 }, { VK_SPACE, HUD_EV_ACCEPT, 0 }, { VK_BACK, HUD_EV_BACK, 0 },
                { VK_UP, HUD_EV_UP, 0 }, { VK_DOWN, HUD_EV_DOWN, 0 }, { VK_LEFT, HUD_EV_LEFT, 0 },
                { VK_RIGHT, HUD_EV_RIGHT, 0 }, { 'C', HUD_EV_BUTTON, 0 } };
            for (size_t k = 0; k < sizeof KEYS / sizeof *KEYS; k++)
                if (w.pressed[KEYS[k].key])
                    hud_input(v.hud, KEYS[k].ev, KEYS[k].button);
        } else if (v.hud && hud_pause_showing(v.hud)) {   /* the Pause menu has the pad: the arrows choose, Enter /
                                                      * Space take, Backspace back, P (Start) takes too */
            static const struct { int key, ev; } KEYS[] = {
                { VK_RETURN, HUD_EV_ACCEPT }, { VK_SPACE, HUD_EV_ACCEPT }, { VK_BACK, HUD_EV_BACK },
                { VK_UP, HUD_EV_UP }, { VK_DOWN, HUD_EV_DOWN }, { VK_LEFT, HUD_EV_LEFT }, { VK_RIGHT, HUD_EV_RIGHT },
                { 'P', HUD_EV_START } };
            for (size_t k = 0; k < sizeof KEYS / sizeof *KEYS; k++)
                if (w.pressed[KEYS[k].key])
                    hud_input(v.hud, KEYS[k].ev, 0);
            hud_pad_held(v.hud, 0, w.keys[VK_LEFT] != 0, w.keys[VK_RIGHT] != 0);
        } else if (v.hud && v.play && (w.pressed['P'] || w.pressed[VK_RETURN]))   /* Start: the Pause menu */
            pause_open(&v);
        if (v.exit_game) {                           /* back to the shell (--shell), or the viewer ends */
            v.exit_game = 0;
            if (v.shell_mode) {
                ShellStart st = { SHELL_FROM_GAME, 1, 0, 0, -1, 0, v.last_game };
                v.play = 0;
                shell_enter(&v, &st);
                continue;
            }
            w.quit = 1;
        }
        game_next_block(&v);                         /* the game moved on to the next block */
        if (w.pressed[VK_NEXT] && v.nblocks) { v.block = (v.block + 1) % v.nblocks; build(&v); }
        if (w.pressed[VK_PRIOR] && v.nblocks) { v.block = (v.block + v.nblocks - 1) % v.nblocks; build(&v); }
        if (w.pressed['N']) { v.night = !v.night; build(&v); }
        if (w.pressed['F']) v.fog = !v.fog;
        if (w.pressed['H']) v.show_hits = !v.show_hits;
        if (w.pressed['M']) {                        /* player 1's monster: the next one */
            v.mon[0].monster = v.mon[0].monster + 1 < (int)v.nmonsters ? v.mon[0].monster + 1 : -1;
            for (int k = 1; k < v.nmon; k++)         /* (the others keep theirs; with none, none at all) */
                if (v.mon[k].monster < 0)
                    v.mon[k].monster = v.mon[0].monster;
            v.clip = -1;
            build(&v);
        }
        if (w.pressed['G'] && have_monster(&v)) {
            v.play = !v.play;
            cam_init(&v.cam, &v.cam_settings);
            dircam_init(&v.cd, &v.cam);
            v.view = v.cd.out;
            for (int k = 0; v.play && k < v.nmon; k++)
                if (v.mon[k].have_graph)
                    play_state(&v.mon[k]);
        }
        if (have_monster(&v) && !v.play && (w.pressed['K'] || w.pressed['L'])) {
            v.clip = w.pressed['L'] ? v.clip + 1 : (v.clip > 0 ? v.clip - 1 : 0);
            build(&v);
        }
        for (int i = 0; i < NCITIES; i++)
            if (w.pressed['1' + i] && i != v.city_index)
                open_city(&v, i, 0);
        if (w.wheel) speed *= powf(1.25f, (float)w.wheel);
        cam.yaw -= w.mouse_dx * 0.005f;
        cam.pitch -= w.mouse_dy * 0.005f;
        if (cam.pitch > 1.5f) cam.pitch = 1.5f;
        if (cam.pitch < -1.5f) cam.pitch = -1.5f;
        int playing = v.play && have_monster(&v) && v.have_scene && mon_in(&v.mon[0]);
        if (playing) {
            float aspect = w.width ? (float)w.height / w.width : 0.75f;
            static int settled;
            if (shot && !settled) {                     /* a shot: the camera settles, then the script */
                play_script(&v, "wait 2", aspect);
                if (script)
                    play_script(&v, script, aspect);
            }
            settled = 1;
            if (v.exit_game)                         /* the script quit the game: the shell's turn */
                continue;
            for (int k = 0; k < v.nmon; k++)         /* the pads of the players on keys */
                if (v.mon[k].drive != DRIVE_COMPUTER)
                    keys_pad(&w, v.mon[k].drive, &v.mon[k].pad);
            play_step(&v, shot ? 0 : (dt > 0.1f ? 0.1f : dt), aspect);
            pose_monsters(&v, shot ? shot_time : now - start);   /* the clips at the states' times (clip_at) */
            memcpy(cam.pos, v.view.pos, sizeof cam.pos);
            cam_shaken(v.view.pos, (const float (*)[3])v.view.rows, v.shakes, 8 + MONSTERS - 1, cam.rows);   /* after the
                                                      * smoothing */
            cam.use_rows = 1;
            cam.fov_deg = v.view.fov * 57.2957795f;
        } else {
            if (v.have_scene) {
                world_build(&v);                     /* the traffic's and the parade's world */
                scene_buildings_update(&v.scene, dt > 0.1f ? 0.1f : dt);
                v.scene.cloth_dt += dt > 0.1f ? 0.1f : dt;   /* the flags fly in the free camera too */
                if (v.veh)                           /* and the traffic drives on, with no monster about */
                    vehicle_update(v.veh, dt > 0.1f ? 0.1f : dt, NULL, 0, NULL, 0);
                if (v.parade)                        /* and the balloons drift, the bikers ride */
                    parade_update(v.parade, dt > 0.1f ? 0.1f : dt, NULL, 0, cam.pos[0]);
            }
            if (cam.use_rows) {                        /* back to the free camera where the game camera was */
                cam.use_rows = 0;
                cam.fov_deg = 60;
                cam.yaw = atan2f(cam.rows[1][1], cam.rows[1][0]);
                cam.pitch = asinf(cam.rows[1][2]);
            }
            float step = speed * dt * (w.keys[VK_SHIFT] ? 4.0f : 1.0f);
            float fx = cosf(cam.yaw), fy = sinf(cam.yaw);
            if (w.keys['W']) { cam.pos[0] += fx * step; cam.pos[1] += fy * step; }
            if (w.keys['S']) { cam.pos[0] -= fx * step; cam.pos[1] -= fy * step; }
            if (w.keys['A']) { cam.pos[0] -= fy * step; cam.pos[1] += fx * step; }
            if (w.keys['D']) { cam.pos[0] += fy * step; cam.pos[1] -= fx * step; }
            if (w.keys['E']) cam.pos[2] += step;
            if (w.keys['Q']) cam.pos[2] -= step;
        }
        double t = shot ? shot_time : now - start;
        sound_listener(cam.pos, (const float (*)[3])cam.rows);   /* the camera (its rows when it is the game's) */
        sound_update(shot ? 0 : (dt > 0.1f ? 0.1f : dt));
        if (v.have_scene) {
            scene_update(&v.scene, t);
            if (playing)
                hit_spheres_all(&v);
            else
                v.scene.nspheres = 0;
        }
        {                                            /* this frame's particles */
            static FxQuad quads[8192];
            v.scene.fx = quads;
            v.scene.nfx = v.fxw ? fx_quads(v.fxw, quads, 8192) : 0;
            shadow_frame(&v, shot ? 1.0f : (dt > 0.1f ? 0.1f : dt));
            if (shot && getenv("RAMPAGE_TRACE")) {
                int np[3] = { 0, 0, 0 };
                for (int k = 0; k < v.shl.nb; k++)
                    np[v.shl.b[k].pass] += v.shl.b[k].count / 3;
                printf("shadows: %d ground, %d decal, %d wall triangles; grid %s, blob fade %.2f\n", np[0], np[1], np[2],
                       v.sg.enabled ? "on" : "off", v.mon[0].smon.ground_fade);
            }
            static FxMesh fxm[1024];
            v.scene.fxm = fxm;
            v.scene.nfxm = v.fxw ? fx_meshes(v.fxw, fxm, 1024) : 0;
            v.scene.fx_shapes = (const SceneMesh (*)[8])v.fx_shapes;
            v.scene.fx_nshapes = v.fx_nshapes;
            if (shot && getenv("RAMPAGE_TRACE") && v.fxw)
                printf("particles %d, quads %d (first at %.1f %.1f %.1f, size %.2f, alpha %.2f)\n", fx_particles(v.fxw),
                       v.scene.nfx, v.scene.nfx ? quads[0].pos[0] : 0, v.scene.nfx ? quads[0].pos[1] : 0,
                       v.scene.nfx ? quads[0].pos[2] : 0, v.scene.nfx ? quads[0].size[0] : 0, v.scene.nfx ? quads[0].col[3] : 0);
        }
        {                                            /* the HUD, through this frame's camera */
            static HudQuad hq[8192];
            if (have_view) {
                Camera fixed = { { view[0], view[1], view[2] }, view[3] / 57.2957795f, view[4] / 57.2957795f, 60 };
                v.hud_cam = fixed;
            } else
                v.hud_cam = cam;
            v.scene.hud = hq;
            int nq = 0;                              /* the other players' HUDs, then player 1's (the screens) */
            for (int k = 1; k < v.nmon; k++)
                if (v.mon[k].hud && v.mon[k].hud != v.hud)
                    nq += hud_quads(v.mon[k].hud, hq + nq, 8192 - nq);
            v.scene.nhud = nq + (v.hud ? hud_quads(v.hud, hq + nq, 8192 - nq) : 0);
        }
        if (have_view) {
            Camera fixed = { { view[0], view[1], view[2] }, view[3] / 57.2957795f, view[4] / 57.2957795f, 60 };
            rnd_frame(v.rnd, &w, &fixed, t, v.fog);
        } else
            rnd_frame(v.rnd, &w, &cam, t, v.fog);
        if (shot) {
            if (cam.use_rows)
                printf("camera at %.2f %.2f %.2f, forward %.3f %.3f %.3f, up %.3f %.3f %.3f, tilt %.3f, state %d\n",
                       cam.pos[0], cam.pos[1], cam.pos[2], cam.rows[1][0], cam.rows[1][1], cam.rows[1][2],
                       cam.rows[2][0], cam.rows[2][1], cam.rows[2][2], v.cam.tilt, v.cam.state);
            int ok = shot_png(shot, w.width, w.height) == 0;
            printf(ok ? "wrote %s\n" : "cannot write %s\n", shot);
            break;
        }
        if (w.pressed[VK_F12]) {
            static int n;
            char name[32];
            snprintf(name, sizeof name, "shot_%03d.png", ++n);
            if (shot_png(name, w.width, w.height) == 0)
                printf("wrote %s\n", name);
        }
        win_swap(&w);
        frames++;
        if (now - title_at > 0.5) {
            char title[256];
            snprintf(title, sizeof title, "RampageRE - %s, %s%s%s%s - %u triangles, %.0f fps%s", CITIES[v.city_index],
                     v.nblocks ? v.blocks[v.block] : "-", v.night ? ", night" : "",
                     have_monster(&v) ? ", " : "", have_monster(&v) && v.scene.mon[0].clip ? v.scene.mon[0].clip : "",
                     rnd_triangles(v.rnd), frames / (now - title_at), v.fog ? ", fog" : "");
            win_title(&w, title);
            title_at = now;
            frames = 0;
        }
    }
    rnd_free(v.rnd);
    for (int k = 0; k < MONSTERS; k++)
        if (v.mon[k].have_graph) {
            moves_stop(&v.mon[k].run);
            moves_free(&v.mon[k].graph);
        }
    if (v.have_scene)
        scene_free(&v.scene);
    city_close(v.city);
    sound_end();
    sound_free();
    win_close(&w);
    return 0;
}
