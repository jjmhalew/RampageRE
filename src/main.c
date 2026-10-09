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
#include "edible.h"
#include "game.h"
#include "look.h"
#include "boss.h"

static const char *const CITIES[] = { "Chicago", "NewYork", "SanFrancisco", "LosAngeles", "London", "HongKong", "LasVegas" };
#define NCITIES 7

typedef struct {
    Vol vol;
    City *city; int city_index;
    const char *blocks[64]; uint32_t nblocks, block;
    int night, fog;
    const char *monsters[64]; uint32_t nmonsters; int monster, clip;   /* monster -1 = none; clip -1 = its idle */
    Scene scene; int have_scene;
    Renderer *rnd;
    int play; CamSettings cam_settings; GameCamera cam;   /* play: the monster under control, the game camera */
    float mx, my, mz, heading;                       /* where the monster stands, its turn about Z (0: facing +Y) */
    float vel[3]; int mode, no_gravity, column;      /* velocity (+0xBD30), movement mode (+0xAD8), no gravity
                                                        (+0xAB8), the slots' column (+0xBAB4) */
    MoveGraph graph; MoveRunner run; MoveInput input; int have_graph;
    int yaw_axis; float entry_yaw, entry_root[3], entry_t;    /* the root's axis that tells its turn; its turn and place at the
                                                        state's start */
    float frame[2][3], stick[2];                     /* the camera's right and forward axes the stick is read in
                                                        (+0xA18, +0xA24) and the stick they were taken for (+0xA3C) */
    float air[3];                                    /* the stick's own velocity (+0xA68), NODE_AIR_* */
    uint32_t armed; int show_hits;                   /* body parts an attack event armed (bits); H draws them */
    int mirror, mirror_queued;                       /* the clip plays mirrored (+0xAA0); for the next clip (+0xAA1) */
    Mount mount;                                     /* on a building's face (climb.c) */
    Hand hand[2];                                    /* its hands on the wall (0 left, 1 right, as seen) */
    CamShake shakes[8];                              /* 0: the monster's own (monster +0x7DFC +0x160), then effects' */
    uint32_t shake_seed;
    int last_collapse[64];                           /* per building: its collapse when last looked at */
    int held;                                        /* the person in its hand (people.c), -1 none */
    int held_unit;                                   /* or the army unit (army.c), -1 none */
    int held_vehicle;                                /* or a vehicle (vehicle.c), -1 none */
    Vehicles *veh;                                   /* the traffic, the fire truck and the rockets (vehicle.c) */
    Edibles *edibles;                                /* the power-ups and hazards behind the windows (edible.c) */
    Game *game;                                      /* the world's states, the lives, the clock (game.c) */
    Boss *boss;                                      /* the block's boss and the ambient ones (boss.c) */
    struct { int id, fx, obj, monster; float off[3]; } bfx[64]; int nbfx;   /* the bosses' effects that follow */
    int game_block;                                  /* a block the game moved on to, to build after the frame; -1 */
    int game_built;                                  /* this build is the game's: keep the game going */
    int streaker;                                    /* the defeated monster's streaker (people.c), -1 none */
    EdiblePower power;                               /* the monster's energy, rage and speed (energy mirrored in .energy) */
    struct { int fx, edible; float off[3]; } efx[32]; int nefx;   /* the edibles' effects that follow them */
    struct { int id, fx, obj; float off[3]; } vfx[64]; int nvfx;   /* the vehicles' effects that follow them */
    Army *army;
    int pending_slot;                                /* a reaction the army caused: started at the next step, -1 */
    int defeated;                                    /* its HP ran out: React_Defeated .. the respawn */
    float rx, ry;                                    /* the right stick (x right, y down) */
    float look_yaw, look_pitch;                      /* the head animator's angles now (+0xBC, +0xE8) */
    float scale, scale_from, scale_to, scale_time, scale_left;   /* setDynamicScale */
    struct { int id, fx, unit, shot; float off[3]; } fxa[128]; int nfxa;   /* the army's effects that follow */
    int armed_people[32]; int narmed_people;         /* people firing (army_person_moved) */
    Bundle fx_bundle; int have_fx_bundle;            /* fx.bdp: the effects (fx.c) */
    FxLib *fxlib; FxWorld *fxw;
    Bundle gui_world, gui_common; int have_gui;      /* WorldGui.bdp, CommonGUI.bdp: the HUD (hud.c) */
    Hud *hud; int no_hud;                            /* --nohud: none */
    Camera hud_cam;                                  /* the frame's camera, for the floating scores */
    float hp, energy;                                /* its hit points (MONSTER_DATA +0x138: 200) and energy, eating */
    const MoveNode *face;                            /* the face layer's node playing now */
    FaceQueue faces;                                 /* setFacialAnim / queueFacialAnim's records, the belches (look.c) */
    HeadLook head; LookLimits look_lim;              /* the head animator (look.c) */
    float friction;                                  /* +0xF4: the ground velocity's friction, set by the last knock */
} Viewer;

/* seconds into the monster's clip at the state's time: the game runs a clip on a 0 .. 1 clock that the state's rate
 * (node +0x0, 1 / its length) moves (0x2FFF10), so the state's time x rate x the clip's duration */
static double clip_at(const Viewer *v, const MoveState *st, double t)
{
    const Instance *in = &v->scene.instances[v->scene.monster];
    return t * st->rate * (in->has_clip ? in->clip.duration : 1);
}

/* the clip's root at time t (seconds of the clip, clip_at), held at the clip's last frame (clip_pose loops: its end
 * is its start) */
static int root_at(const Viewer *v, double t, float tr[3], float q[4])
{
    const Instance *in = &v->scene.instances[v->scene.monster];
    if (in->has_clip && t > in->clip.duration - 1e-4)
        t = in->clip.duration - 1e-4;
    return scene_monster_root(&v->scene, t < 0 ? 0 : t, tr, q);
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
static float root_turn(const Viewer *v, double ta, double tb)
{
    float tr[3], qa[4], qb[4], zero[3] = { 0, 0, 0 };
    if (root_at(v, ta, tr, qa) != 0 || root_at(v, tb, tr, qb) != 0)
        return 0;
    Mat4 a = mat_tq(zero, qa), b = mat_tq(zero, qb);
    float d00 = 0, d10 = 0;
    for (int k = 0; k < 3; k++) {
        d00 += b.m[4 * k] * a.m[4 * k];
        d10 += b.m[4 * k + 1] * a.m[4 * k];
    }
    return atan2f(d10, d00) * 57.2957795f;
}

static float root_yaw(const Viewer *v, double t)
{
    float tr[3], q[4];
    if (root_at(v, t, tr, q) != 0)
        return 0;
    float zero[3] = { 0, 0, 0 };
    Mat4 m = mat_tq(zero, q);
    return atan2f(m.m[4 * v->yaw_axis + 1], m.m[4 * v->yaw_axis]) * 57.2957795f;
}

/* the monster plays the clip of the runner's state, from the runner's time */
static void play_state(Viewer *v)
{
    const MoveState *st = &v->graph.states[v->run.state];
    if (scene_monster_play(&v->scene, st->clip) != 0)
        printf("%s: no clip %s\n", st->name ? st->name : "?", st->clip ? st->clip : "-");
    v->entry_yaw = root_yaw(v, clip_at(v, st, v->run.time));
    v->entry_t = clip_at(v, st, v->run.time);
    if (getenv("RAMPAGE_ROOT")) {
        float rt[3], rq[4];
        if (root_at(v, clip_at(v, st, v->run.time), rt, rq) == 0)
            printf("  root of %s: %.2f %.2f %.2f\n", st->clip, rt[0], rt[1], rt[2]);
    }
    v->armed = 0;                                    /* 0x2E7F70: a clip starts with no attack armed, on the ground */
    v->mode = MODE_GROUND;                           /* (its events set the mode it is in) */
    climb_clip_started(&v->mount, st->name);
    v->mirror = v->mirror_queued;                    /* 0x2E67E8: a clip starts in the queued mirror state */
    v->input.mirrored = v->mirror;
    if (v->scene.monster >= 0)
        v->scene.instances[v->scene.monster].mirrored = v->mirror;
    float q[4];
    if (root_at(v, clip_at(v, st, v->run.time), v->entry_root, q) != 0)
        memset(v->entry_root, 0, sizeof v->entry_root);
}

/* setFacialAnim (0x2E9200, 0x2E9350): the state node of that name plays its clip on the face, from its start; a name
 * no node has does nothing */
static void face_start(Viewer *v)
{
    const FaceRec *f = &v->faces.list[0];
    const MoveNode *n = f->node;
    int r = scene_monster_face(&v->scene, n->clip, f->head);
    if (getenv("RAMPAGE_TRACE"))
        printf("face %s: %s -> %d\n", f->name, n->clip, r);
    if (r != 0)
        return;
    v->face = n;
    scene_monster_face_blend(&v->scene, n->blend);   /* 0x2FF910: from the face as it was, over the node's blend */
}

static void face_play(Viewer *v, const char *name, int head, int loop, int count)
{
    const MoveNode *n = moves_node(&v->graph, name);
    if (n && n->clip && face_set(&v->faces, n, name, head, loop, count) == FACE_START)
        face_start(v);
}

/* Monsters.bsf's DefaultFacialAnim: Expression_Idle, looping, the head itself left to the body */
static void face_default(Viewer *v)
{
    face_play(v, "Expression_Idle", 0, 1, 0);
}

/* the face clip's clock: 0 .. 1 at its node's rate; at its end (0x2E95D0) it plays again while it loops or has
 * plays left, else the next queued face, else the default face (look.c); the delayed belches (appendBelch) */
static void face_step(Viewer *v, float dt)
{
    if (!v->face || v->scene.monster < 0)
        return;
    if (face_tick(&v->faces, dt))
        face_play(v, "Expression_Burp", 0, 0, 1);
    Instance *in = &v->scene.instances[v->scene.monster];
    in->face_t += v->face->rate * dt;
    if (in->face_t < 1)
        return;
    switch (face_clip_end(&v->faces)) {
    case FACE_RESTART:
        in->face_t = 0;
        scene_monster_face_blend(&v->scene, v->face->blend);
        break;
    case FACE_START: face_start(v); break;
    default: face_default(v); break;
    }
}

static void start_moves(Viewer *v)
{
    if (v->have_graph) {
        moves_stop(&v->run);
        moves_free(&v->graph);
        v->have_graph = 0;
    }
    if (v->scene.monster < 0 || !v->scene.monster_bundle || moves_read(v->scene.monster_bundle, &v->graph) != 0)
        return;
    v->have_graph = 1;
    moves_start(&v->run, &v->graph, moves_find(&v->graph, "Ground_Idle"));
    v->face = NULL;
    face_init(&v->faces);
    memset(&v->head, 0, sizeof v->head);
    look_limits_default(&v->look_lim);
    v->friction = MONSTER_FRICTION;
    face_default(v);                                 /* Monsters::PostInit */
    memset(&v->input, 0, sizeof v->input);
    memset(v->vel, 0, sizeof v->vel);
    v->mirror = v->mirror_queued = 0;
    memset(&v->mount, 0, sizeof v->mount);
    memset(v->hand, 0, sizeof v->hand);
    v->held = -1;
    v->held_unit = -1;
    v->held_vehicle = -1;
    v->game_block = -1;
    v->pending_slot = -1;
    v->defeated = 0;
    v->scale = v->scale_to = 1;
    v->scale_left = 0;
    v->hp = 200;
    if (getenv("RAMPAGE_HP"))                        /* a test's start (the game starts at MONSTER_DATA +0x138) */
        v->hp = (float)atof(getenv("RAMPAGE_HP"));
    v->energy = 0;
    edible_power_init(&v->power);
    memset(v->shakes, 0, sizeof v->shakes);
    for (int i = 0; i < 64; i++) v->last_collapse[i] = -1;
    v->mount.at.building = v->mount.cand.building = -1;
    memset(v->air, 0, sizeof v->air);
    memset(v->stick, 0, sizeof v->stick);
    memcpy(v->frame, v->cam.rot, sizeof v->frame);
    v->mz = 0;
    v->mode = MODE_GROUND;
    v->no_gravity = 0;
    v->column = -1;
    const Instance *in = &v->scene.instances[v->scene.monster];
    v->yaw_axis = 0;                                 /* the most level axis of the root at rest */
    for (uint32_t i = 0; i < in->tree.n; i++)
        if (in->tree.nodes[i].parent < 0) {
            float zero[3] = { 0, 0, 0 };
            Mat4 m = mat_tq(zero, in->tree.nodes[i].q);
            for (int k = 1; k < 3; k++)
                if (fabsf(m.m[4 * k + 2]) < fabsf(m.m[4 * v->yaw_axis + 2]))
                    v->yaw_axis = k;
            break;
        }
    if (v->play)                                     /* with --clip the chosen clip stays until G */
        play_state(v);
}

/* Monster.isFemale (script method 0x96, 0x2AC0E0): the monster's +0xAB6, which its setup (0x2E98C0) sets from
 * MiscData|IsFemale of its property book (MONSTER_DATA +0xAA4): Amanda, Jill, Lizzie, Natalie, Snapper, Venus */
static int monster_female(const Viewer *v)
{
    char book[64];                                   /* "George_prop.prb": the book George_prop(.bdp) */
    snprintf(book, sizeof book, "%s", v->have_graph && v->graph.props ? v->graph.props : "");
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
    const float (*r)[3] = (const float (*)[3])v->cam.rot;
    float d[3] = { p[0] - v->cam.pos[0], p[1] - v->cam.pos[1], p[2] - v->cam.pos[2] };
    float x = d[0] * r[0][0] + d[1] * r[0][1] + d[2] * r[0][2], y = d[0] * r[2][0] + d[1] * r[2][1] + d[2] * r[2][2];
    float z = d[0] * r[1][0] + d[1] * r[1][1] + d[2] * r[1][2];
    if (z < -radius)
        return 0;
    float t = tanf((v->cam.s.fov > 0 ? v->cam.s.fov : 1) / 2), zz = z > 0.1f ? z : 0.1f;
    return fabsf(x) - radius <= zz * t * 4 / 3 && fabsf(y) - radius <= zz * t;
}

static int edible_cell_state(void *ctx, int building, int storey, int column);
static int edible_cell_where(void *ctx, int building, int storey, int column, float out[3]);
static int edible_call(Viewer *v, const char *call, const float *xyz);
static void monster_stats(Viewer *v, float health, float energy);
static int boss_weapon_make(void *ctx, ArmyWeapon *w, Group g);
static void boss_weapon_tick(void *ctx, ArmyWeapon *w, int may, const float from[3], const float dir[3], int target,
                             float dt);

/* the army's host (army.h): books, clip lengths and model bounds from the city */
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

static void build(Viewer *v)
{
    if (v->have_scene)
        scene_free(&v->scene);
    v->have_scene = city_build(v->city, v->blocks[v->block], v->night, &v->scene) == 0;
    if (v->have_scene && v->monster >= 0 &&
        city_monster(v->city, &v->scene, v->monsters[v->monster], v->clip, v->mx, v->my, v->heading) == 0)
        printf("%s: %s\n", v->monsters[v->monster], v->scene.monster_clip ? v->scene.monster_clip : "-");
    start_moves(v);
    if (getenv("RAMPAGE_TRACE") && v->have_graph)
        printf("properties %s%s\n", v->graph.props ? v->graph.props : "-", monster_female(v) ? ", female" : "");
    if (v->have_scene && v->scene.music)            /* the block's music, looping (sound.c; silent unless started) */
        music_play(v->scene.music);
    if (v->army) {
        army_free(v->army);
        v->army = NULL;
    }
    v->nfxa = 0;
    v->narmed_people = 0;
    if (v->have_scene && v->monster >= 0) {          /* the army comes for the monster (the block's AIDef) */
        static ArmyHost host;
        host.find = army_find;
        host.clip_seconds = army_clip_seconds;
        host.shape_box = army_shape_box;
        host.node_world = army_node_world;
        host.ctx = v;
        v->army = army_new(&host);
        if (v->army) {
            army_load_block(v->army, v->scene.block);
            army_activate(v->army, 1);
        }
    }
    v->scene.army = v->army;
    vehicle_free(v->veh);                            /* the traffic of the three block slots, the roof rockets */
    v->veh = NULL;
    v->nvfx = 0;
    v->held_vehicle = -1;
    if (v->have_scene) {
        static VehicleHost vh;
        vh.find = army_find;
        vh.shape_box = army_shape_box;
        vh.ctx = v;
        v->veh = vehicle_new(&vh);
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
            if (getenv("RAMPAGE_TRACE"))
                printf("edibles: %d kinds, %d slots\n", edible_kinds(v->edibles), edible_slots(v->edibles));
        }
    }
    v->scene.edibles = v->edibles;
    boss_free(v->boss);                              /* the block's boss, and the ambient ones of the blocks behind */
    v->boss = NULL;
    v->nbfx = 0;
    if (v->have_scene && v->monster >= 0) {
        static BossHost bh;
        bh.find = army_find;
        bh.clip_seconds = army_clip_seconds;
        bh.shape_box = army_shape_box;
        bh.weapon_make = boss_weapon_make;
        bh.weapon_tick = boss_weapon_tick;
        bh.ctx = v;
        v->boss = boss_new(&bh);
        if (v->boss) {
            int n = boss_load_block(v->boss, v->scene.block);
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
    v->streaker = -1;
    if (!v->game_built) {                            /* the flow of the city from this block on: campaign, one player */
        game_free(v->game);
        v->game = NULL;
        if (v->have_scene && v->monster >= 0) {
            static GameHost gh;
            gh.find = army_find;
            gh.ctx = v;
            GameConfig gc = { GP_CAMPAIGN, 1, 0, 0, 7, city_block_index(v->city, v->blocks[v->block]), 1 };
            v->game = game_new(&gh, &gc);
            if (v->game && game_load_city(v->game, CITIES[v->city_index]) == 0) {
                game_auto_results(v->game, 3.0f);   /* no results screens here: answered after 3 s */
                game_start(v->game);
            } else {
                game_free(v->game);
                v->game = NULL;
            }
        }
    }
    if (v->have_scene && v->scene.folk)
        people_set_visible(v->scene.folk, people_visible, v);
    hud_close(v->hud);                               /* player 1's HUD for this monster (Hud0.ifc) */
    v->hud = NULL;
    if (v->have_gui && v->have_scene && v->monster >= 0 && !v->no_hud) {
        HudConfig hc = { 0, 1, HUD_PLAY_CAMPAIGN, 0, v->monsters[v->monster] };
        v->hud = hud_open(&v->gui_world, &v->gui_common, &hc);
        if (v->hud) {
            hud_set(v->hud, v->hp, 200, v->energy, 100, 0, 3, -1);   /* three lives: the campaign's start [G] */
            hud_set_project(v->hud, hud_project, v);
        }
    }
    Group tt;
    if (city_camera(v->city, &tt) == 0)
        cam_settings(&v->cam_settings, tt);
    cam_init(&v->cam, &v->cam_settings);
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
static int attach_world(const Viewer *v, const char *name, Mat4 *out);
static int window_person(const Viewer *v, const float *xyz);

static Climber climber(const Viewer *v)
{
    Climber c = { { v->mx, v->my, v->mz }, v->heading, v->graph.box[0], v->mx, { v->graph.probe[0], v->graph.probe[1] } };
    return c;
}

/* the hop / swing scripts' way along the face: canClimb(0, -1) for 0 mirrored or > 0 not, else (0, 1) */
static int along(const Viewer *v, float p0)
{
    return (v->mirror && p0 == 0) || (!v->mirror && p0 > 0) ? -1 : 1;
}

static int answer(void *ctx, const char *call, const float *a)
{
    static const char *const YES[] = { "Monsters::canSpin", "Monsters::hasDoubleJump" };
    Viewer *v = ctx;
    int r = v ? edible_power_answer(&v->power, call) : -1;   /* hasRoarEnergy, hasChargePunch, ... (edible.c) */
    if (r >= 0)
        return r;
    for (size_t i = 0; i < sizeof YES / sizeof *YES; i++)
        if (strcmp(call, YES[i]) == 0)
            return 1;
    if (!v)
        return 0;
    Climber c = climber(v);
    const Scene *sc = &v->scene;
    if (!strcmp(call, "Monsters::canMount")) {       /* (side, z offset, style): styles 2 / 3 swap the sides */
        int side = (int)a[0];
        if ((a[2] == 2 && v->mirror) || (a[2] == 3 && !v->mirror))
            side = side == 0 ? 1 : side == 1 ? 0 : side;
        return climb_can_mount(&v->mount, sc, &c, side, a[1]) == 1;
    }
    if (!strcmp(call, "Monsters::mountIsOccupied")) {
        int r = climb_can_mount(&v->mount, sc, &c, (int)a[0], 0);
        return r == -1 || r == -2 || r == -3 || r == -5 || r == -6;
    }
    if (!strcmp(call, "Monsters::canHop")) return climb_can_climb(&v->mount, sc, &c, 0, along(v, a[0]), 0) == 3;
    if (!strcmp(call, "Monsters::canSwing")) return climb_can_climb(&v->mount, sc, &c, 0, along(v, a[0]), 0) == 4;
    if (!strcmp(call, "Monsters::canMantle")) return climb_can_climb(&v->mount, sc, &c, 1, 0, 0) == 5;
    if (!strcmp(call, "Monsters::canRespawn"))       /* 0x2D3D80: lives left, become human, its streaker not held */
        return v->game ? game_can_respawn(v->game, 0, v->streaker >= 0 && v->scene.folk &&
                                          people_get(v->scene.folk, v->streaker)->phase == PP_HELD) : v->defeated;
    if (!strcmp(call, "Monsters::canGrabPerson")) return window_person(v, a + 1) >= 0;
    if (!strcmp(call, "Monsters::canDoPunch")) {     /* not at a person's or a power-up's window */
        int e = edible_call(v, call, a + 1);
        return e >= 0 ? e : window_person(v, a + 1) < 0;
    }
    if (strstr(call, "::canGrabPowerup") || strstr(call, "::checkForHazard"))
        return edible_call(v, call, a + 1) > 0;
    if (!strcmp(call, "Monsters::pickupSuccess") || !strcmp(call, "Monsters::isHoldlingEdible"))
        return v->held >= 0 || v->held_unit >= 0 || v->held_vehicle >= 0;                         /* hasGrabbedObject: 2 for an actor (0x2E5590) */
    if (!strcmp(call, "Monsters::canTransfer"))      /* (right): mirrored the other way */
        return climb_can_transfer(&v->mount, sc, &c, v->mirror ? a[0] == 0 : a[0] == 1) == 1;
    if (!strcmp(call, "Monsters::canSpiderTransfer"))   /* (right): mirrored always right */
        return climb_can_transfer(&v->mount, sc, &c, v->mirror || a[0] != 0) == 1;
    if (!strcmp(call, "Monsters::stopClimbUp") || !strcmp(call, "Monsters::stopSpiderClimbUp")) {
        int r = climb_can_climb(&v->mount, sc, &c, 1, 0, !strcmp(call, "Monsters::stopSpiderClimbUp"));
        if (getenv("RAMPAGE_TRACE"))
            printf("  canClimb up %d (mount z %.1f, top %.1f)\n", r, v->mount.at.pt[2],
                   v->mount.at.building >= 0 ? sc->buildings[v->mount.at.building].b.max[2] : 0);
        return r != 2;
    }
    if (!strcmp(call, "Monsters::stopClimbDown")) return climb_can_climb(&v->mount, sc, &c, -1, 0, 0) != 2;
    if (!strcmp(call, "Monsters::shouldUnmountDown")) return climb_can_climb(&v->mount, sc, &c, -1, 0, 0) == -1;
    if (!strcmp(call, "Monsters::hitElectricSign"))
        return climb_can_climb(&v->mount, sc, &c, a[0] == 0 ? 1 : -1, 0, 0) == -2;
    return 0;                                        /* canGrabPerson, ...: not here */
}

/* the clip events the viewer acts on: the movement mode, and Monsters.bsf's velocity functions (jumpNorm and its kin
 * set the velocity, addVelocity adds to it, in the world or turned with the monster) */
/* Monsters.bsf's hitBuildingFront(kind, x, y, z): which face (by kind, the mirror state, or for kinds 6 and 7 the way
 * the monster faces), the point in the monster's frame, then hitBuildingPt(point, side, 1, 2.0): the point in the
 * world (0x2B3A80), the building whose face that side looks at within 5 units, the point on it (0x2D54F0), and its
 * hit (0x2BC150). Sides: 0 the building's left face (looking +x), 1 its right (-x), 2 its front (+y), 5 its roof. */
static void hit_building_front(Viewer *v, const float *p)
{
    int mirror = v->mirror, side = 0, kind = (int)p[0];
    if (kind == 0 || kind == 1 || kind == 3 || kind == 4) side = mirror;
    else if (kind == 2 || kind == 5) side = 2;
    else if (kind == 6 || kind == 7) {
        float yaw = (v->heading > 180 ? v->heading - 360 : v->heading) / 57.2957795f;
        if (yaw >= -0.7853982f && yaw <= 0.7853982f) side = 2;
        else if (yaw >= 0.7853982f && yaw <= 2.3561945f) side = 1;
        else if (yaw <= -0.7853982f && yaw >= -2.3561945f) side = 0;
        /* facing the street the script leaves side as it was declared: 0 */
    }
    float l[3] = { (mirror && side == 2) || side == 1 ? -p[1] : p[1], p[2], p[3] };
    float c = cosf(v->heading / 57.2957795f), sn = sinf(v->heading / 57.2957795f);
    float w[3] = { v->mx + c * l[0] - sn * l[1], v->my + sn * l[0] + c * l[1], v->mz + l[2] };
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
    int hit = best && building_hit(&best->b, bq, 2.0f);
    if (hit && (kind <= 2 || kind == 6) &&             /* +2 energy (Monsters.bsf), the rage when full */
        edible_power_hit_building(&v->power, 1, v->mode == MODE_AIRBORNE || v->mode == MODE_TAKEOFF, 0)) {
        edible_power_start_rage(&v->power, 0);
        v->pending_slot = 54;
    }
    v->energy = v->power.energy;
    if (getenv("RAMPAGE_TRACE"))
        printf("  hitBuildingFront side %d at %.1f %.1f %.1f: %s", side, w[0], w[1], w[2],
               !best ? "no building\n" : hit ? "hit" : "no damage");
    if (getenv("RAMPAGE_TRACE") && best)
        printf(" (state %d, %d storeys, HP %d)\n", best->b.state, best->b.n, best->b.hp);
}

static void clip_event(Viewer *v, const MoveEvent *e)
{
    float c = cosf(v->heading / 57.2957795f), s = sinf(v->heading / 57.2957795f);
    float mirror = v->mirror ? -1.0f : 1.0f;        /* Monster.getMirrorState */
    float w[3] = { 0, 0, 0 }, l[3] = { 0, 0, 0 };   /* world / relative velocity */
    int set = 0, add = 0, relative = 0;
    if (e->type == 7) {                              /* 0x2E7DE0 */
        v->mode = (int)e->value;
        return;
    }
    if (e->type == 6) {                              /* a sound at the monster ({6, at, ?, name}) */
        float at[3] = { v->mx, v->my, v->mz + 4 };
        int h = e->name ? sound_play(e->name, at, 1) : 0;
        if (e->name && getenv("RAMPAGE_TRACE"))
            printf(h ? "  sound %s\n" : "  no sound %s\n", e->name);
        return;
    }
    if (e->type == 9) {                              /* 0x2E7E18 -> 0x2F5AC0: the camera shakes, f32 strength at +8 */
        union { uint32_t u; float f; } k = { e->value };
        float at[3] = { v->mx, v->my, v->mz };
        shake_start(&v->shakes[0], &SHAKE_MONSTER, at, k.f, 1, &v->shake_seed);
        return;
    }
    if (e->type == 8) {                              /* 0x2E7F18: the next clip the other way round */
        v->mirror_queued = !v->mirror_queued;
        return;
    }
    if (e->type == 23) {                             /* 0x2E7D38 */
        v->column = (int32_t)e->value;
        return;
    }
    if (e->type == 0) {                              /* 0x2E7880: the parts' bones armed with this attack, or not */
        v->armed = e->p[0] != 0 ? v->armed | e->value : v->armed & ~e->value;
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
    else if (!strcmp(n, "Monsters::disableGravity")) { v->no_gravity = 1; return; }
    else if (!strcmp(n, "Monsters::setMirrorState")) { v->mirror_queued = e->p[0] != 0; return; }
    else if (!strcmp(n, "Monsters::faceAnim")) {     /* (expression, head layer, -, plays): never looping */
        int head, plays;                             /* (and a delay > 1: the belch that many seconds later) */
        const char *face = face_anim_event(&v->faces, e->p, &head, &plays);
        if (face)
            face_play(v, face, head, 0, plays);
        return;
    }
    else if (!strcmp(n, "Monsters::prepareToMount")) { climb_prepare(&v->mount); return; }
    else if (!strcmp(n, "Monsters::mountBuilding")) {
        Climber cl = climber(v);
        climb_mount(&v->mount, &cl, e->p[0]);        /* and transitionToMount(param0) */
        return;
    }
    else if (!strcmp(n, "Monsters::unmountBuilding")) { climb_unmount(&v->mount); return; }
    else if (!strcmp(n, "Monsters::attachHand")) {   /* (hand, time, mirror): only on a building */
        int h = ((int)e->p[0] & 1) ^ (v->mirror ? 1 : 0);
        Mat4 w;
        if (v->mount.at.building < 0 || scene_monster_hand(&v->scene, h, &w) != 0)
            return;
        /* the place: the clip's hand now, held to the face; the rotation the clip's (the game's is the clip's hand
         * at the end of the blend, 0x321E80) */
        float pos[3] = { w.m[12], w.m[13], w.m[14] }, r[9], q[4];
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++)
                r[3 * c + k] = w.m[4 * c + k];
        ik_quat_of(r, q);
        climb_hand_snap(&v->mount, &v->scene, h, pos);
        if (hand_attach(&v->hand[h], e->p[1], pos, q) && e->p[2] != 0)
            v->mirror_queued = !v->mirror_queued;
        return;
    }
    else if (!strcmp(n, "Monsters::detachHand")) {
        hand_detach(&v->hand[((int)e->p[0] & 1) ^ (v->mirror ? 1 : 0)], e->p[1]);
        return;
    }
    else if (!strcmp(n, "Monsters::transferMount")) { climb_transfer(&v->mount); return; }
    else if (!strcmp(n, "Monsters::setScale")) {     /* (scale, time): setDynamicScale, from its size now */
        v->scale_from = v->scale;
        v->scale_to = e->p[0] > 0 ? e->p[0] : 1;
        v->scale_time = v->scale_left = e->p[1] > 0 ? e->p[1] : 0;
        if (v->scale_left <= 0)
            v->scale = v->scale_to;
        return;
    }
    else if (!strcmp(n, "Monsters::startRespawn")) { /* 0x2D3930: a life, back to full health where the streaker is
                                                        or dropping in from the view */
        v->hp = 200;
        v->defeated = 0;
        if (v->scene.monster >= 0)
            v->scene.instances[v->scene.monster].hidden = 0;
        if (v->game) {
            GameRespawnIn in;
            GameRespawn out;
            memset(&in, 0, sizeof in);
            const Person *p = v->streaker >= 0 && v->scene.folk ? people_get(v->scene.folk, v->streaker) : NULL;
            if (p && p->phase != PP_GONE) {
                in.streaker = 1;
                in.on_line = p->mv.active && p->phase != PP_HELD;
                memcpy(in.streaker_pos, p->pos, sizeof in.streaker_pos);
                in.streaker_facing = p->heading / 57.2957795f;
            }
            memcpy(in.cam_pos, v->cam.pos, sizeof in.cam_pos);
            memcpy(in.cam_dir, v->cam.rot[1], sizeof in.cam_dir);
            in.cam_half_fov = v->cam.s.fov / 2;
            game_start_respawn(v->game, 0, &in, &out);
            v->mx = out.pos[0]; v->my = out.pos[1]; v->mz = out.pos[2];
            v->heading = out.facing * 57.2957795f;
            if (out.fall) {                          /* in the air, Jump_Butt_Slam_fall */
                v->mode = MODE_AIRBORNE;
                for (int k = 0; v->have_graph && k < v->graph.n; k++)
                    if (!_stricmp(v->graph.states[k].name, "Jump_Butt_Slam_fall")) {
                        moves_force(&v->run, k);
                        break;
                    }
            }
            if (out.delete_streaker && v->scene.folk && v->streaker >= 0)
                people_remove(v->scene.folk, v->streaker);
            if (getenv("RAMPAGE_TRACE"))
                printf("  respawn at %.1f %.1f %.1f%s, %d lives\n", out.pos[0], out.pos[1], out.pos[2],
                       out.fall ? ", falling" : "", game_lives(v->game, 0));
        }
        v->streaker = -1;
        return;
    }
    else if (!strcmp(n, "Monsters::becomeHuman")) {  /* while a block plays (state 6): createStreaker, and the monster
                                                        stops drawing itself (0x2D3ED0) */
        if (!v->scene.folk || v->scene.monster < 0)
            return;
        int human = v->game ? game_become_human(v->game, 0) : 1;
        if (getenv("RAMPAGE_TRACE"))
            printf("  becomeHuman: %s (state %d)\n", human == 1 ? "a streaker" : "not in play", v->game ? game_state(v->game) : -1);
        if (human != 1)
            return;
        Group t = city_template(v->city, monster_female(v) ? "NakedFemale" : "NakedMale");
        float at[3] = { v->mx, v->my, v->mz };
        if (t.at && (v->streaker = people_add_streaker(v->scene.folk, t, at, v->cam.pos[0])) >= 0)
            v->scene.instances[v->scene.monster].hidden = 1;
        return;
    }
    else if (!strcmp(n, "Monsters::doGrabPerson")) { /* grabBuildingOccupant(loc, 1) (0x2B3350): 0.25 s blend */
        int who = v->held < 0 ? window_person(v, e->p + 1) : -1;
        if (who >= 0) {
            people_grab(v->scene.folk, who, 0.25f);
            v->held = who;
        }
        return;
    }
    else if (!strcmp(n, "Monsters::doGrabObject")) { /* (0x2D99C0): the nearest within 15 of the hand, in front */
        Mat4 h;
        if (v->held >= 0 || !v->scene.folk || attach_world(v, "Helper R Hand", &h) != 0)
            return;
        float hand[3] = { h.m[12], h.m[13], h.m[14] }, root[3] = { v->mx, v->my, v->mz };
        float fwd[3] = { -sinf(v->heading / 57.2957795f), cosf(v->heading / 57.2957795f), 0 };
        int who = people_nearest(v->scene.folk, hand, root, fwd, 15);
        if (who >= 0) {
            people_grab(v->scene.folk, who, 0.1f);
            v->held = who;
        } else if (v->army && (who = army_nearest_pickup(v->army, hand, root, fwd, 15)) >= 0) {
            army_grab(v->army, who, 0);
            v->held_unit = who;
        } else if (v->veh && v->held_vehicle < 0 && (who = vehicle_nearest_pickup(v->veh, hand, root, fwd, 15)) >= 0) {
            vehicle_grab(v->veh, who, 0);
            v->held_vehicle = who;
        }
        return;
    }
    else if (!strcmp(n, "Monsters::grabBoulder")) {  /* Monsters.bsf: a Boulder torn out of the street, 6.5 up */
        float at[3] = { v->mx, v->my, v->mz + 6.5f };
        int who = v->veh && v->held < 0 && v->held_unit < 0 && v->held_vehicle < 0 ?
                  vehicle_spawn(v->veh, "Boulder", at, 0, -1) : -1;
        if (who >= 0) {
            vehicle_grab(v->veh, who, 0);
            v->held_vehicle = who;
        }
        return;
    }
    else if (!strcmp(n, "Monsters::throwObject")) {  /* (eat, x, y, z) (0x2D9DC0): eaten, or smashed away */
        if (v->held_unit >= 0 && v->army) {
            army_throw(v->army, v->held_unit, 0, e->p[0] != 0, e->p + 1, v->heading / 57.2957795f);
            v->held_unit = -1;
            return;
        }
        if (v->held_vehicle >= 0 && v->veh) {
            vehicle_throw(v->veh, v->held_vehicle, 0, e->p[0] != 0, e->p + 1, v->heading / 57.2957795f);
            v->held_vehicle = -1;
            return;
        }
        if (v->held < 0 || !v->scene.folk)
            return;
        if (e->p[0] != 0) {
            PeopleEaten got = people_eat(v->scene.folk, v->held);
            monster_stats(v, got.health, got.energy);
            if (getenv("RAMPAGE_TRACE"))
                printf("  ate: health %+.0f -> %.0f, energy %+.0f -> %.0f\n", got.health, v->hp, got.energy, v->energy);
        } else {
            people_throw(v->scene.folk, v->held, v->heading);
        }
        v->held = -1;
        return;
    }
    else if (strstr(n, "::doGrabPowerup") || strstr(n, "::destroyPowerup") || strstr(n, "::checkForHazard")) {
        int r = edible_call(v, n, e->p + 1);         /* grabBuildingOccupant: the edible at that window */
        if (getenv("RAMPAGE_TRACE"))
            printf("  %s: %d\n", n, r);
        return;
    }
    else if (!strcmp(n, "Monsters::useRoarEnergy")) { edible_power_use_roar(&v->power); v->energy = v->power.energy; return; }
    else if (!strcmp(n, "Monsters::changeMountRail")) {
        float d = v->mirror ? -e->p[0] : e->p[0];
        climb_change_rail(&v->mount, d < 0 ? -1 : 1);
        return;
    }
    else if (!strcmp(n, "Monsters::transitionFromBuildingTop")) { v->mount.from_top = 1; return; }
    else if (strstr(n, "::hitBuildingFront")) { hit_building_front(v, e->p); return; }
    else return;
    if (relative) {                                  /* the monster's frame (0x2E7330 / 0x2E74F0: by its matrix) */
        w[0] = c * l[0] - s * l[1];
        w[1] = s * l[0] + c * l[1];
        w[2] = l[2];
    }
    for (int k = 0; k < 3; k++)
        v->vel[k] = set ? w[k] : v->vel[k] + (add ? w[k] : 0);
}

static void clip_events(Viewer *v, const MoveState *st, float from, float to)
{
    const MoveEvent *e;
    int n = moves_events_between(st, from, to, &e);
    for (int i = 0; i < n; i++)
        clip_event(v, &e[i]);
}

/* the stick (x right, y down) as a direction on the ground, in the camera's frame the monster keeps (0x2E8410) */
static void stick_world(const Viewer *v, float lx, float ly, float w[3])
{
    for (int k = 0; k < 3; k++)
        w[k] = lx * v->frame[0][k] - ly * v->frame[1][k];
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

/* the box out of the buildings it went into, the shorter way across (the game's response, 0x2801A0, is not read);
 * within a step (1) of a roof it is on the roof */
static void push_out(Viewer *v)
{
    for (uint32_t i = 0; i < v->scene.nbuildings; i++) {
        const Building *b = &v->scene.buildings[i].b;
        if (b->max[2] <= b->min[2] + 0.01f || v->mz >= b->max[2] - 1.0f || v->mz + MONSTER_HEIGHT <= b->min[2])
            continue;
        float dl = v->mx + MONSTER_HALF_WIDTH - b->min[0], dr = b->max[0] - (v->mx - MONSTER_HALF_WIDTH);
        float df = v->my + MONSTER_HALF_WIDTH - b->min[1], db = b->max[1] - (v->my - MONSTER_HALF_WIDTH);
        if (dl <= 0 || dr <= 0 || df <= 0 || db <= 0)
            continue;
        float dx = dl < dr ? -dl : dr, dy = df < db ? -df : db;
        if (fabsf(dx) < fabsf(dy)) {
            v->mx += dx;
            v->air[0] = 0;
        } else {
            v->my += dy;
            v->air[1] = 0;
        }
    }
}

/* NODE_STEER (0x2E8190): the turn towards the stick this frame, in degrees. The stick in the monster's own frame
 * against the state's forward axis; beyond one frame's worth of turning it turns by the angle Ã— `turn` Ã— dt, so it
 * swings round fast and settles gently. */
static float steer(const Viewer *v, const MoveState *st, float lx, float ly, float dt)
{
    float w[3], h = v->heading / 57.2957795f, c = cosf(h), s = sinf(h);
    stick_world(v, lx, ly, w);
    float l[2] = { c * w[0] + s * w[1], -s * w[0] + c * w[1] };   /* by the monster's matrix: right, forward */
    float n = l[0] * l[0] + l[1] * l[1];
    if (n <= 1e-6f)
        return 0;
    n = 1 / sqrtf(n);
    l[0] *= n;
    l[1] *= n;
    float fx = v->mirror ? -st->fwd[0] : st->fwd[0];   /* mirrored: its forward axis' x the other way */
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
static void air_control(Viewer *v, const MoveState *st, float lx, float ly, float dt)
{
    float goal[3] = { 0, 0, 0 }, w[3];
    if (st->flags & (NODE_AIR_X | NODE_AIR_Y | NODE_AIR_Z)) {
        stick_world(v, lx, ly, w);
        float n = w[0] * w[0] + w[1] * w[1];
        if (n >= 1e-6f) {
            float h = v->heading / 57.2957795f, c = cosf(h), s = sinf(h);
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
    float cur = v->air[0] * v->air[0] + v->air[1] * v->air[1] + v->air[2] * v->air[2];
    float want = goal[0] * goal[0] + goal[1] * goal[1] + goal[2] * goal[2];
    float step = (cur <= want ? MOVES_AIR_ACCEL : MOVES_AIR_BRAKE) * dt;
    for (int k = 0; k < 3; k++) {
        float d = goal[k] - v->air[k];
        v->air[k] += d > step ? step : d < -step ? -step : d;
    }
    cur = v->air[0] * v->air[0] + v->air[1] * v->air[1] + v->air[2] * v->air[2];
    if (cur > st->speed * st->speed) {
        float k = st->speed / sqrtf(cur);
        for (int i = 0; i < 3; i++)
            v->air[i] *= k;
    }
}

/* one frame of play. The left stick (x right, y down) and the buttons pressed this frame (MV_CROSS ...) or held.
 * The graph picks the state; the clip's root carries the monster (its travel added to where it stands, its turn
 * when a NODE_BAKE_ROOT state ends), the stick turns it (NODE_STEER) or drives it (NODE_AIR_*) in the camera's frame,
 * the clip's events set its movement mode and velocity, and in the air it falls by MOVES_GRAVITY until it meets the
 * ground (here the street at z = 0), which fires the landing slot. */
/* the collision spheres an armed attack can hit with, in the world (0x2DA9E0: the shape's offset by its bone's
 * matrix, then by the monster's), into the scene's overlay */
static void hit_spheres(Viewer *v)
{
    v->scene.nspheres = 0;
    if (!v->have_graph || !v->armed || v->scene.monster < 0)
        return;
    const Instance *in = &v->scene.instances[v->scene.monster];
    const MoveGraph *g = &v->graph;
    for (int i = 0; i < g->nshapes && v->scene.nspheres < 64; i++) {
        const MoveShape *sh = &g->shapes[i];
        int armed = 0;
        for (int p = 0; p < g->nparts && !armed; p++)
            if (v->armed >> p & 1)
                for (int k = 0; k < g->part_nbones[p]; k++)
                    armed |= g->part_bone[p][k] == sh->bone;
        if (!armed || sh->bone < 0 || (uint32_t)sh->bone >= in->tree.n)
            continue;
        const float *m = in->node[sh->bone].m;      /* in the monster's frame, then reflected if mirrored */
        float l[3];
        for (int r = 0; r < 3; r++)
            l[r] = m[r] * sh->off[0] + m[4 + r] * sh->off[1] + m[8 + r] * sh->off[2] + m[12 + r];
        if (v->mirror)
            l[0] = -l[0];
        const float *bm = in->base.m;
        float *c = v->scene.spheres[v->scene.nspheres++];
        for (int r = 0; r < 3; r++)
            c[r] = bm[r] * l[0] + bm[4 + r] * l[1] + bm[8 + r] * l[2] + bm[12 + r];
        c[3] = sh->radius;
        scene_adornments_hit(&v->scene, c, c[3]);    /* 0x2D4390: it knocks adornments off */
        if (v->army)                                 /* and the army's units (20 a blow [G]) */
            army_hit(v->army, 0, c, c[3], 20);
        if (v->veh)                                  /* and the vehicles and rockets */
            vehicle_hit(v->veh, 0, c, c[3], 20);
        if (v->boss)                                 /* and the bosses */
            boss_hit(v->boss, 0, c, c[3], 20);
        if (v->scene.folk) {                         /* and people (TakeDamage), and the guards' huts */
            people_hit(v->scene.folk, c, c[3], v->heading, v->graph.states[v->run.state].name);
            float from[3] = { v->mx, v->my, v->mz };
            PeopleEaten got = scene_huts_hit(&v->scene, c, c[3], from);
            if (got.health != 0 || got.energy != 0 || got.points != 0)
                monster_stats(v, got.health, got.energy);
        }
    }
    if (!v->show_hits)
        v->scene.nspheres = 0;
}

static void hands_step(Viewer *v, float dt);

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
static void force_state(Viewer *v, const char *name)
{
    for (int k = 0; name && v->have_graph && k < v->graph.n; k++)
        if (!_stricmp(v->graph.states[k].name, name)) {
            moves_force(&v->run, k);
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

/* the bosses' frame: what they see of the monster and the buildings, then what they did */
static void boss_step(Viewer *v, float dt, const ArmyMonster *am)
{
    if (!v->boss)
        return;
    BossMonster m;
    memset(&m, 0, sizeof m);
    memcpy(m.pos, am->pos, sizeof m.pos);
    m.heading = am->heading;
    m.move = am->move;
    m.climbing = v->mount.at.building >= 0;
    m.spheres = am->spheres;
    m.nspheres = am->nspheres;
    memcpy(m.box_min, am->box_min, sizeof m.box_min);
    memcpy(m.box_max, am->box_max, sizeof m.box_max);
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
    boss_set_view(v->boss, v->cam.pos[0] - 45, v->cam.pos[0] + 45);   /* the view's x range [G], as the army's */
    boss_update(v->boss, dt, &m, 1, bl, nb);
    BossEvent ev[64];
    int ne;
    while ((ne = boss_events(v->boss, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const BossEvent *e = &ev[i];
            switch (e->type) {
            case BE_EFFECT:
                if (v->fxw) {
                    int id = fx_spawn(v->fxw, e->name, e->pos, e->dir, 1);
                    if (id >= 0 && v->nbfx < 64) {   /* to follow its part or the monster, or to stop by its id */
                        const BossPart *p = e->obj >= 0 ? boss_part(v->boss, e->obj) : NULL;
                        v->bfx[v->nbfx].id = e->id;
                        v->bfx[v->nbfx].fx = id;
                        v->bfx[v->nbfx].obj = p ? e->obj : -1;
                        v->bfx[v->nbfx].monster = !p && e->monster >= 0;
                        for (int k = 0; k < 3; k++)
                            v->bfx[v->nbfx].off[k] = e->pos[k] - (p ? p->pos[k] : k == 0 ? v->mx : k == 1 ? v->my : v->mz);
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
            case BE_REACTION: if (v->hp > 0) force_state(v, e->name); break;
            case BE_VEL: memcpy(v->vel, e->v, sizeof v->vel); break;
            case BE_HP: if (v->hp > 0) v->hp = fmaxf(0, fminf(200, v->hp + e->amount)); break;
            case BE_FACING: v->heading = e->amount * 57.2957795f; break;
            case BE_PUSH: v->mx += e->v[0]; v->my += e->v[1]; break;
            case BE_STATS: if (v->army) army_add_stats(v->army, 0, e->tmpl, e->level); break;
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
        } else if (v->bfx[k].monster) {
            at[0] = v->mx + v->bfx[k].off[0]; at[1] = v->my + v->bfx[k].off[1]; at[2] = v->mz + v->bfx[k].off[2];
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
    in.hp[0] = v->hp;
    for (uint32_t i = 0; i < v->scene.nbuildings; i++)
        in.buildings += v->scene.buildings[i].b.state == BUILDING_STANDING ||
                        v->scene.buildings[i].b.state == BUILDING_FLOOR_FALLING;
    game_update(v->game, dt, &in);
    GameEvent ev[32];
    int ne;
    while ((ne = game_events(v->game, ev, 32)) > 0)
        for (int i = 0; i < ne; i++) {
            const GameEvent *e = &ev[i];
            switch (e->type) {
            case GE_BLOCK:                           /* the next block: built once this frame is over */
                for (uint32_t k = 0; e->name && k < v->nblocks; k++)
                    if (!_stricmp(v->blocks[k], e->name) && k != v->block)
                        v->game_block = (int)k;
                break;
            case GE_RESET:
            case GE_PLACE:
                if (e->a == 1 && e->type == GE_PLACE) {
                    v->mx += e->pos[0]; v->my += e->pos[1]; v->mz += e->pos[2];
                } else if (e->a == 1 || e->type == GE_PLACE) {
                    v->mx = e->pos[0]; v->my = e->pos[1]; v->mz = e->pos[2];
                    if (e->type == GE_RESET)
                        v->heading = e->f * 57.2957795f;
                }
                break;
            case GE_MOVE:
                if (e->a == 1)
                    game_move_done(v->game, e->slot);
                break;
            case GE_AI_ACTIVATE: if (v->army) army_activate(v->army, 1); break;
            case GE_REVIVE:                          /* cancelRespawn: full HP where it is */
                v->hp = 200;
                v->defeated = 0;
                v->scale = v->scale_to = 1;
                if (v->scene.monster >= 0)
                    v->scene.instances[v->scene.monster].hidden = 0;
                if (v->streaker >= 0 && v->scene.folk)
                    people_remove(v->scene.folk, v->streaker);
                v->streaker = -1;
                if (e->a == 1) {
                    edible_power_init(&v->power);
                    v->energy = 0;
                }
                break;
            default: break;
            }
            if (getenv("RAMPAGE_TRACE") && e->type != GE_MOVE)
                printf("  game event %d (%s) a %d, state %d\n", e->type, e->name ? e->name : "-", e->a,
                       game_state(v->game));
        }
}

/* gCommon.addStats on the monster (0x2C51A0): HP and energy by the rules edible.c keeps (none at 0 HP; while raging
 * only a full Energy restarts it); a full one starts the rage: Monsters::startRage, slot 54 [G: its slot] */
static void monster_stats(Viewer *v, float health, float energy)
{
    if (edible_power_add_stats(&v->power, &v->hp, 200, health, energy, 0)) {
        edible_power_start_rage(&v->power, 0);
        if (!v->defeated)
            v->pending_slot = 54;
        if (getenv("RAMPAGE_TRACE"))
            printf("  rage\n");
    }
    v->energy = v->power.energy;
}

/* the cell a monster call's (x, y, z) names (getBuildingOccupant, people_probe): its building, storey as built and
 * column, and whether a person is at its window; the edibles' answer to the call, -1 none */
static int edible_call(Viewer *v, const char *call, const float *xyz)
{
    if (!v->edibles)
        return -1;
    PeopleBox boxes[64];
    int nb = scene_people_boxes(&v->scene, boxes, 64), b = -1, ls = -1, col = -1;
    float at[3] = { v->mx, v->my, v->mz };
    int found = people_probe(boxes, nb, at, xyz, v->mirror, &b, &ls, &col);
    int o = found && b >= 0 && ls >= 0 && ls < v->scene.buildings[b].b.n ? v->scene.buildings[b].b.live[ls] : -1;
    int person = o >= 0 && v->scene.folk && people_window_at(v->scene.folk, b, ls, col) >= 0;
    return edible_monster_call(v->edibles, call, o >= 0 ? b : -1, o, col, person, 0,
                               v->have_graph ? v->graph.states[v->run.state].name : NULL);
}

/* the edibles' frame: their clocks, then what they did */
static void edible_step(Viewer *v, float dt)
{
    if (!v->edibles)
        return;
    edible_update(v->edibles, dt);
    if (edible_power_update(&v->power, dt, v->hp) && getenv("RAMPAGE_TRACE"))
        printf("  the rage ends\n");
    v->energy = v->power.energy;
    EdibleEvent ev[32];
    int ne;
    while ((ne = edible_events(v->edibles, ev, 32)) > 0)
        for (int i = 0; i < ne; i++) {
            const EdibleEvent *e = &ev[i];
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
                if (v->hp > 0 && v->have_graph && e->name)
                    for (int k = 0; k < v->graph.n; k++)
                        if (!_stricmp(v->graph.states[k].name, e->name)) {
                            moves_force(&v->run, k);
                            break;
                        }
                break;
            case EE_SPEED: edible_power_speed(&v->power, e->amount, e->v[0]); break;
            case EE_STATS:                           /* the score through the army, HP and energy here */
                if (v->army)
                    army_add_stats(v->army, 0, e->tmpl, e->level);
                monster_stats(v, e->v[0], e->v[1]);
                break;
            case EE_BANNER:
                if (v->hud)
                    hud_float(v->hud, (int)e->amount, e->pos, 0, 1.0f);
                break;
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
static void monster_hit(Viewer *v, const char *name, float amount, int level, const float knock[3], const float from[3])
{
    if (v->defeated)
        return;
    v->hp = fmaxf(0, v->hp - amount);
    float heading = v->heading / 57.2957795f, at[3] = { v->mx, v->my, v->mz };
    int slot = army_reaction_slot(level, knock, v->mode, army_hit_behind(at, heading, from));
    if (slot >= 0) {
        v->friction = monster_knock_shot(v->vel, knock);   /* 0x2D9650: per axis the larger of knock and velocity */
        v->pending_slot = slot;
    }
    if (v->hp <= 0) {                                /* 0x2D9364: at 0 HP slot 67, React_Defeated */
        v->defeated = 1;
        v->pending_slot = 67;
        memset(v->vel, 0, sizeof v->vel);
    }
    if (getenv("RAMPAGE_TRACE"))
        printf("  hit by %s: %.1f, HP %.0f, slot %d\n", name ? name : "?", amount, v->hp, slot);
}

/* the vehicles' frame (vehicle.c): what they see of the monster and the buildings, then what they did */
static void vehicle_step(Viewer *v, float dt, const ArmyMonster *m, const ArmyBox *boxes, int nb)
{
    if (!v->veh)
        return;
    vehicle_update(v->veh, dt, m, 1, boxes, nb);
    VehicleEvent ev[64];
    int ne;
    while ((ne = vehicle_events(v->veh, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const VehicleEvent *e = &ev[i];
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
            case VE_HIT: monster_hit(v, e->name, e->amount, e->level, e->v, e->pos); break;
            case VE_REACTION: v->pending_slot = e->level; break;
            case VE_SET_VEL: memcpy(v->vel, e->v, sizeof v->vel); break;
            case VE_HP: if (v->hp > 0) v->hp = fmaxf(0, fminf(200, v->hp + e->amount)); break;
            case VE_STATS:                           /* id 1: the army applied it (its events follow) */
                if (e->id == 0) {
                    monster_stats(v, e->v[0], e->v[1]);
                }
                break;
            case VE_REMOVED:
                if (e->obj == v->held_vehicle)
                    v->held_vehicle = -1;
                break;
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
    if (v->held_vehicle >= 0) {                      /* one in its hand */
        Mat4 h;
        if (attach_world(v, "Helper R Hand", &h) == 0)
            vehicle_hold(v->veh, v->held_vehicle, h.m, v->mirror);
    }
}

static void army_step(Viewer *v, float dt)
{
    if (!v->army || !v->have_graph || v->scene.monster < 0)
        return;
    static float spheres[64][4];
    const Instance *in = &v->scene.instances[v->scene.monster];
    int ns = 0;
    for (int i = 0; i < v->graph.nshapes && ns < 64; i++) {   /* every collision sphere, in the world */
        const MoveShape *sh = &v->graph.shapes[i];
        if (sh->bone < 0 || (uint32_t)sh->bone >= in->tree.n)
            continue;
        const float *m = in->node[sh->bone].m, *bm = in->base.m;
        float l[3];
        for (int r = 0; r < 3; r++)
            l[r] = m[r] * sh->off[0] + m[4 + r] * sh->off[1] + m[8 + r] * sh->off[2] + m[12 + r];
        if (v->mirror)
            l[0] = -l[0];
        for (int r = 0; r < 3; r++)
            spheres[ns][r] = bm[r] * l[0] + bm[4 + r] * l[1] + bm[8 + r] * l[2] + bm[12 + r];
        spheres[ns++][3] = sh->radius;
    }
    ArmyMonster m;
    memset(&m, 0, sizeof m);
    m.pos[0] = v->mx; m.pos[1] = v->my; m.pos[2] = v->mz;
    m.heading = v->heading / 57.2957795f;
    m.move = v->graph.states[v->run.state].name;
    m.mode = v->mode;
    m.spheres = (const float (*)[4])spheres;
    m.nspheres = ns;
    float hw = v->graph.box[0] > 0 ? v->graph.box[0] : 3, hh = v->graph.box[1] > 0 ? v->graph.box[1] : 4.25f;
    m.box_min[0] = v->mx - hw; m.box_min[1] = v->my - hw; m.box_min[2] = v->mz;
    m.box_max[0] = v->mx + hw; m.box_max[1] = v->my + hw; m.box_max[2] = v->mz + 2 * hh;
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
    army_set_view(v->army, v->cam.pos[0] - 45, v->cam.pos[0] + 45);   /* the view's x range [G] */
    v->scene.monster_move = m.move;
    army_update(v->army, dt, &m, 1, boxes, nb);
    vehicle_step(v, dt, &m, boxes, nb);
    edible_step(v, dt);
    boss_step(v, dt, &m);
    game_step(v, dt);
    ArmyEvent ev[64];
    int ne;
    while ((ne = army_events(v->army, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const ArmyEvent *e = &ev[i];
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
            case AE_HIT: monster_hit(v, e->name, e->amount, e->level, e->v, e->pos); break;
            case AE_REACTION: v->pending_slot = e->level; break;
            case AE_PUSH: v->mx += e->v[0]; v->my += e->v[1]; break;
            case AE_STATS_HEALTH: v->hp = v->hp > 0 ? fmaxf(1, fminf(200, v->hp + e->amount)) : v->hp; break;
            case AE_STATS_ENERGY: monster_stats(v, 0, e->amount); break;
            case AE_FLOAT:                           /* a PointsBanner: 0.8 for the army's awards (0x2BAB2C) */
                if (v->hud)
                    hud_float(v->hud, (int)e->amount, e->pos, e->level, 0.8f);
                break;
            case AE_SCORE:
                if (v->game)
                    game_add_points(v->game, 0, e->level, (int)e->amount);
                if (getenv("RAMPAGE_TRACE"))
                    printf("  score %+.0f (category %d), total %.0f\n", e->amount, e->level,
                           army_stats(v->army, 0)->points[6]);
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
    if (v->held_unit >= 0) {                         /* a unit in its hand */
        Mat4 h;
        if (attach_world(v, "Helper R Hand", &h) == 0)
            army_hold(v->army, v->held_unit, h.m);
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
static int attach_world(const Viewer *v, const char *name, Mat4 *out)
{
    if (!v->have_graph || v->scene.monster < 0)
        return -1;
    const Instance *in = &v->scene.instances[v->scene.monster];
    const struct MoveAttach *a = NULL;
    for (int i = 0; i < v->graph.nattach && !a; i++)
        if (v->graph.attach[i].name && ends_with_i(v->graph.attach[i].name, name))
            a = &v->graph.attach[i];
    if (!a || a->bone < 0 || (uint32_t)a->bone >= in->tree.n)
        return -1;
    if (v->mirror) {                                 /* the partner bone's own attachment */
        int mb = in->tree.nodes[a->bone].mirror;
        for (int i = 0; i < v->graph.nattach; i++)
            if (v->graph.attach[i].bone == mb && mb != a->bone)
                a = &v->graph.attach[i];
    }
    Mat4 t = mat_identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            t.m[4 * i + j] = a->m[3 * i + j];
    for (int j = 0; j < 3; j++)
        t.m[12 + j] = a->m[9 + j];
    Mat4 m = mat_mul(in->node[a->bone], t);
    if (v->mirror)
        for (int c = 0; c < 4; c++) {                /* S m S */
            m.m[4 * c] = -m.m[4 * c];
            m.m[c] = -m.m[c];
        }
    *out = mat_mul(in->base, m);
    return 0;
}

/* canGrabPerson's / doGrabPerson's window person (getBuildingOccupant 0x2B37C0), -1 none */
static int window_person(const Viewer *v, const float *xyz)
{
    PeopleBox boxes[64];
    int nb = scene_people_boxes(&v->scene, boxes, 64);
    float at[3] = { v->mx, v->my, v->mz };
    return v->scene.folk ? people_can_grab(v->scene.folk, boxes, nb, at, xyz, v->mirror) : -1;
}

static void play_step(Viewer *v, float lx, float ly, const uint8_t *pressed, const uint8_t *held, float dt, float aspect)
{
    if (v->have_graph) {
        MoveInput *in = &v->input;
        face_step(v, dt);
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
            float w[3], h = v->heading / 57.2957795f, c = cosf(h), s = sinf(h);
            stick_world(v, lx, ly, w);
            moves_sectors(in, c * w[0] + s * w[1], -s * w[0] + c * w[1]);
        }
        const MoveState *st = &v->graph.states[v->run.state];
        /* the camera frame the stick is read in: every frame (0x2E0738), or, in a NODE_HOLD_FRAME state, only when the
         * stick moves (0x2E8190), so a climb keeps its direction while the camera swings */
        if (!(st->flags & NODE_HOLD_FRAME) || lx != v->stick[0] || ly != v->stick[1])
            memcpy(v->frame, v->cam.rot, sizeof v->frame);
        v->stick[0] = lx;
        v->stick[1] = ly;
        float t0 = v->run.time, length = st->length;
        float cdt = dt * v->power.speed;             /* the clips run at the speed scale x their rate (0x2EBFE0:
                                                      * setSpeedScale, the rage's 1.33, the speed power-up) */
        int ev = -1;
        if (v->mode == MODE_AIRBORNE || v->mode == MODE_TAKEOFF) {   /* the air: velocity, then gravity */
            float pos[3] = { v->mx, v->my, v->mz };  /* 0x2DEBD8: drag, the move, gravity */
            monster_air_velocity(v->vel, pos, dt, v->no_gravity);
            v->mx = pos[0]; v->my = pos[1]; v->mz = pos[2];
            float g = ground_at(v, v->mx, v->my, v->mz);
            if (v->mz <= g && v->vel[2] <= 0) {
                v->mz = g;
                int wk = water_at(v, v->mx, v->my);
                if (v->mode == MODE_AIRBORNE && wk >= 0) {   /* into the water (0x2DCA94): pressed down 1.25 x its
                                                      * radius across, by its fall speed, and its splash [G: the
                                                      * props' WaterImpactFx where the game starts it was not read] */
                    float s = fminf(fmaxf(-v->vel[2] / 50, 0), 1), at[3] = { v->mx, v->my, 0 };
                    at[2] = water_surface(v->scene.water[wk]);
                    water_disturb(v->scene.water[wk], at, 1.25f * MONSTER_HALF_WIDTH, -(2 * s + 0.5f * (1 - s)));
                    if (v->fxw)
                        fx_spawn(v->fxw, "footfall_big_water", at, NULL, 1);
                }
                if (v->mode == MODE_AIRBORNE) {      /* landed (0x2DC450): slot 56 in the current column */
                    int to = moves_slot(&v->graph, SLOT_LAND, v->column);
                    if (to >= 0) {
                        moves_force(&v->run, to);
                        ev = 0;
                    }
                    v->mode = MODE_PRONE;
                    memset(v->vel, 0, sizeof v->vel);
                } else {
                    v->vel[2] = 0;
                }
            }
        } else if (v->mode == MODE_BUILDING && v->mount.at.building >= 0) {
            memset(v->vel, 0, sizeof v->vel);        /* on a face: held there, no gravity, no ground */
        } else {                                     /* 0x2DF6F0: a knock slides it, friction stops it */
            float pos[3] = { v->mx, v->my, v->mz };
            monster_ground_velocity(v->vel, pos, v->friction, dt, st->flags, v->pending_slot >= 0);
            v->mx = pos[0]; v->my = pos[1]; v->mz = pos[2];
            float g = ground_at(v, v->mx, v->my, v->mz);
            if (g < v->mz - 0.5f)                    /* walked off a roof: it falls (a stand-in: the game eases it */
                v->mode = MODE_AIRBORNE;             /* down, 0x2E0330) and lands through the landing slot */
            else
                v->mz = monster_ground_follow(v->mz, g, v->no_gravity);
        }
        {                                            /* wading (0x2E2900 -> 0x2E2BD0): the water pressed down under
                                                      * it, half its radius across, by how deep it stands */
            int wk = v->mode != MODE_BUILDING ? water_at(v, v->mx, v->my) : -1;
            if (wk >= 0) {
                float at[3] = { v->mx, v->my, water_surface(v->scene.water[wk]) };
                float deep = fminf(fmaxf(at[2] - v->mz, 0), MONSTER_HEIGHT);
                if (deep > 0)
                    water_disturb(v->scene.water[wk], at, 0.5f * MONSTER_HALF_WIDTH, -0.9f * deep / MONSTER_HEIGHT);
            }
        }
        air_control(v, st, lx, ly, dt);
        if (v->mode == MODE_GROUND || v->mode == MODE_AIRBORNE || v->mode == MODE_TAKEOFF || v->mode == MODE_PRONE) {
            v->mx += v->air[0] * dt;                 /* 0x2E0C50: in modes 0, 1, 2 and 4 */
            v->my += v->air[1] * dt;
        }
        if (st->flags & NODE_STEER)
            v->heading += steer(v, st, lx, ly, dt);
        scene_monster_blend_step(&v->scene, dt);
        if (ev != 0 && v->pending_slot >= 0) {       /* the army's hit: the reaction slot (0x2D6A50), its column's */
            int to = moves_slot(&v->graph, v->pending_slot, v->column);
            v->pending_slot = -1;
            if (to >= 0) {
                climb_unmount(&v->mount);
                moves_force(&v->run, to);
                if (v->vel[2] > 0)
                    v->mode = MODE_AIRBORNE;
                ev = 0;
            }
        }
        if (ev != 0) {                               /* 0x2E14F0: held on the face, or knocked off it */
            Climber cl = climber(v);
            int slot = climb_check(&v->mount, &v->scene, &cl, v->mode);
            int to = slot >= 0 ? moves_slot(&v->graph, slot, v->column) : -1;
            if (to >= 0) {
                climb_unmount(&v->mount);
                moves_force(&v->run, to);
                v->mode = MODE_AIRBORNE;
                ev = 0;
            }
        }
        float before[3] = { v->mx, v->my, v->mz };
        int switched = ev == 0 || moves_step(&v->run, in, cdt, answer, v);
        float t1 = switched ? (ev == 0 ? t0 : (t0 + cdt < length ? t0 + cdt : length)) : v->run.time;
        float a[3], b[3], q[4];                      /* the root's travel and turn this frame the state hands the monster */
        if (root_at(v, clip_at(v, st, t0), a, q) == 0 && root_at(v, clip_at(v, st, t1), b, q) == 0) {
            float c = cosf(v->heading / 57.2957795f), s = sinf(v->heading / 57.2957795f);
            float dx = st->flags & NODE_ROOT_X ? b[0] - a[0] : 0, dy = st->flags & NODE_ROOT_Y ? b[1] - a[1] : 0;
            float m = v->mirror ? -1.0f : 1.0f;      /* mirrored: across the monster's x */
            v->mx += c * m * dx - s * dy;
            v->my += s * m * dx + c * dy;
            if (st->flags & NODE_ROOT_Z)
                v->mz += b[2] - a[2];
            if (st->flags & NODE_ROOT_TURN)
                v->heading += m * root_turn(v, clip_at(v, st, t0), clip_at(v, st, t1));
        }
        clip_events(v, st, t0, t1);                  /* the old clip's events up to now */
        if (v->mode == MODE_BUILDING && v->mount.at.building >= 0) {   /* held on the face (0x2D5870) */
            float pos[3] = { v->mx, v->my, v->mz }, delta[3] = { v->mx - before[0], v->my - before[1], v->mz - before[2] };
            climb_place(&v->mount, &v->scene, st->flags, delta, dt, pos, &v->heading);
            v->mx = pos[0];
            v->my = pos[1];
            v->mz = pos[2];
        }
        if (switched) {
            /* a clip that bakes: its root, as drawn where it ended (the axes it handed over were held at its start, a
             * turn it handed over taken out), in the monster's frame as seen (mirrored if it was) */
            int bake = (st->flags & NODE_BAKE_ROOT) && root_at(v, clip_at(v, st, t1), b, q) == 0;
            float bake_p[3], bake_r[3][3];
            if (bake) {
                for (int k = 0; k < 3; k++)
                    bake_p[k] = st->flags & (k == 0 ? NODE_ROOT_X : k == 1 ? NODE_ROOT_Y : NODE_ROOT_Z) ?
                                v->entry_root[k] : b[k];
                float zero[3] = { 0, 0, 0 }, tw = st->flags & NODE_ROOT_TURN ? root_turn(v, v->entry_t, clip_at(v, st, t1)) : 0;
                Mat4 r = mat_mul(mat_trs_z(0, 0, 0, -tw), mat_tq(zero, q));
                seen_root(v->mirror, bake_p, &r, bake_r);
            }
            const MoveState *to = &v->graph.states[v->run.state];
            if (v->run.from != v->run.state)
                printf(getenv("RAMPAGE_TRACE") ? "%s -> %s (%s) by %d\n" : "%s -> %s (%s)\n", st->name ? st->name : "?",
                       to->name ? to->name : "?", to->clip ? to->clip : "-", ev == 0 ? -2 : v->run.via);
            if (getenv("RAMPAGE_TRACE"))
                printf("  stick %.2f %.2f size %.2f\n", lx, ly, in->v[MV_L_SIZE]);
            /* the transition says whether the new clip blends in (over the new state's blend time) or cuts; a start
             * the game forces (a slot) takes no transition, and its blend is not traced: it blends */
            int blend = ev == 0 ? BLEND_IN : v->run.via >= 0 ? st->trans[v->run.via].blend : st->end_blend;
            scene_monster_blend(&v->scene, blend == BLEND_IN ? to->blend : 0);
            play_state(v);
            if (bake && root_at(v, v->entry_t, b, q) == 0) {
                /* 0x2E6580: the monster moves and turns so that the new clip's root, where it starts, is where the old
                 * one's was: the old root's place and turn become the monster's */
                float zero[3] = { 0, 0, 0 }, np[3] = { b[0], b[1], b[2] }, nr[3][3];
                Mat4 r = mat_tq(zero, q);
                seen_root(v->mirror, np, &r, nr);
                float h = atan2f(bake_r[1][0] * nr[0][0] + bake_r[1][1] * nr[0][1] + bake_r[1][2] * nr[0][2],
                                 bake_r[0][0] * nr[0][0] + bake_r[0][1] * nr[0][1] + bake_r[0][2] * nr[0][2]);
                float ch = cosf(h), sh = sinf(h);
                float d[3] = { bake_p[0] - (ch * np[0] - sh * np[1]), bake_p[1] - (sh * np[0] + ch * np[1]),
                               bake_p[2] - np[2] };
                float c = cosf(v->heading / 57.2957795f), s = sinf(v->heading / 57.2957795f);
                v->mx += c * d[0] - s * d[1];
                v->my += s * d[0] + c * d[1];
                v->mz += d[2];
                v->heading += h * 57.2957795f;
                if (getenv("RAMPAGE_ROOT"))
                    printf("  bake: old root %.2f %.2f %.2f, new %.2f %.2f %.2f, turn %.0f\n", bake_p[0], bake_p[1],
                           bake_p[2], np[0], np[1], np[2], h * 57.2957795f);
            }
            v->column = -1;                         /* 0x2E7F70: a new state starts without the old one's effects */
            v->no_gravity = 0;
            /* and the new one's up to where it starts: the game's event player (0x2F45D0) begins at the clip's first
             * event and fires, in order, every one at or before the start, so a state entered part-way (phase
             * kept) still gets its set-up (mode, column) */
            clip_events(v, to, -1, v->run.time);
        }
        if (v->mx < -75) v->mx = -75;
        if (v->mx > 75) v->mx = 75;
        if (v->my < -30) v->my = -30;
        if (v->mode != MODE_BUILDING)               /* on a face it is held by it, not pushed out */
            push_out(v);
        v->heading = fmodf(v->heading + 360, 360);
        if (getenv("RAMPAGE_TRACE"))
            printf("  %s %.2f at %.1f %.1f %.1f heading %.0f mode %d vz %.1f\n", v->graph.states[v->run.state].name,
                   v->run.time, v->mx, v->my, v->mz, v->heading, v->mode, v->vel[2]);
    }
    scene_monster_place(&v->scene, v->mx, v->my, v->mz, v->heading);
    {                                                /* the head (0x2E70C0, 0x2FBE30, 0x2FBC90): in a look state the
                                                        right stick's deflection is the target, 81 degrees of yaw and
                                                        45 of pitch (MONSTER_DATA +0xA08 .. +0xA10, the same in all
                                                        30); eased at pi / 2 a second (MD +0x174, +0x178), half that
                                                        while the stick is held; back to 0 outside */
        uint32_t fl = v->have_graph ? v->graph.states[v->run.state].flags : 0;
        look_stick(&v->head, &v->look_lim, (fl & NODE_LOOK) != 0, v->rx, v->ry);
        look_update(&v->head, &v->look_lim, 0, fl); /* no target: tracking is off in this build (look.c) */
        look_step(&v->head, &v->look_lim, dt);
        v->look_yaw = v->head.yaw;
        v->look_pitch = v->head.pitch;
        v->scene.look_yaw = v->mirror ? -v->look_yaw : v->look_yaw;   /* mirrored: not traced [G] */
        v->scene.look_pitch = v->look_pitch;
    }
    if (v->scale_left > 0) {                         /* the dynamic scale, linear over its time [G] */
        v->scale_left -= dt;
        float f = v->scale_left > 0 ? v->scale_left / v->scale_time : 0;
        v->scale = v->scale_to + (v->scale_from - v->scale_to) * f;
    }
    if (v->scale != 1 && v->scene.monster >= 0)
        for (int k = 0; k < 11; k++)
            if ((k & 3) != 3)
                v->scene.instances[v->scene.monster].base.m[k] *= v->scale;
    hands_step(v, dt);
    if (v->scene.folk) {                             /* the people, the one in its hand, what they make happen */
        PeopleMonster pm = { { v->mx, v->my, v->mz }, v->have_graph ? v->graph.states[v->run.state].name : NULL };
        scene_people_spawn(&v->scene, dt);
        people_update(v->scene.folk, dt, &pm, 1);
        scene_huts_update(&v->scene, dt);
        Mat4 h;
        if (v->held >= 0 && people_get(v->scene.folk, v->held)->phase != PP_HELD)
            v->held = -1;
        if (v->held >= 0 && attach_world(v, "Helper R Hand", &h) == 0)
            people_hold(v->scene.folk, v->held, h.m, v->mirror, dt);
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
            if (ev[i].type == PE_MONSTER_HP)
                v->hp = fmaxf(0, fminf(200, v->hp + ev[i].amount));
            else if (ev[i].type == PE_MONSTER_KNOCK) {   /* setVel(v, world), setReaction(2) */
                memcpy(v->vel, ev[i].v, sizeof v->vel);
                v->mode = MODE_AIRBORNE;
            }
            if (getenv("RAMPAGE_TRACE"))
                printf("  people event %d (%s) from %d\n", ev[i].type, ev[i].name ? ev[i].name : "-", ev[i].who);
        }
    }
    scene_buildings_update(&v->scene, dt);
    army_step(v, dt);
    fx_step(v, dt);
    shake_step(v->shakes, 8, dt);
    if (v->hud) {                                    /* Hud.bsf: health, rage, the score (ArmyStats points[6]) */
        int vis = 0;
        float clock = v->game ? game_clock(v->game, &vis) : 0;
        hud_set(v->hud, v->hp, 200, v->energy, 100, v->game ? game_points(v->game, 0, GPT_TOTAL) :
                v->army ? (int)army_stats(v->army, 0)->points[6] : 0, v->game ? game_lives(v->game, 0) : -1, -1);
        hud_set_clock(v->hud, vis, clock);
        hud_update(v->hud, dt);
    }
    if (v->have_graph) {                             /* the pose keeps what the monster did not take over */
        const MoveState *st = &v->graph.states[v->run.state];
        float turn = st->flags & NODE_ROOT_TURN ?
            root_turn(v, v->entry_t, clip_at(v, st, v->run.time)) : 0;
        scene_monster_root_motion(&v->scene, (st->flags & NODE_ROOT_X ? 1 : 0) | (st->flags & NODE_ROOT_Y ? 2 : 0),
                                  v->entry_root, turn);
    }
    float at[1][3] = { { v->mx, v->my, v->mz } };
    cam_update(&v->cam, (const float (*)[3])at, 1, dt, aspect);
}

/* the hands each frame (0x323130, 0x3230F0): let go off a building (unmountBuilding's 0x322EC0, 0.1 s), the timers and
 * targets, then the IK's targets in the monster's frame */
static void hands_step(Viewer *v, float dt)
{
    /* 0x322360 (from 0x2E0FEC; not in states whose node flags have 0x1080, 0x50, 0x80002 or 0x40000): a planted hand
     * the arm cannot reach from below lifts the monster just enough, h = dz - sqrt(dz^2 - (|d|^2 - L^2)), at most
     * 0.4, the larger of the two hands' */
    const MoveState *st = v->have_graph ? &v->graph.states[v->run.state] : NULL;
    if (st && !(st->flags & (0x1080 | 0x50 | 0x80002 | 0x40000))) {
        float lift = 0;
        for (int h = 0; h < 2; h++) {
            float sh[3], reach, d[3], d2 = 0;
            if (!v->hand[h].planted || scene_monster_shoulder(&v->scene, h, sh, &reach) != 0)
                continue;
            for (int k = 0; k < 3; k++) {
                d[k] = v->hand[h].tp[k] - sh[k];
                d2 += d[k] * d[k];
            }
            float disc = d[2] * d[2] - (d2 - reach * reach);
            if (d[2] < 0 || d2 < reach * reach || disc < 0)
                continue;
            float up = d[2] - sqrtf(disc);
            if (up <= 0.4f && up > lift)
                lift = up;
        }
        v->mz += lift;
    }
    float h0 = v->heading / 57.2957795f, c = cosf(h0), sn = sinf(h0);
    for (int h = 0; h < 2; h++) {
        Hand *hd = &v->hand[h];
        if (v->mount.at.building < 0 && hd->target > 0)
            hand_detach(hd, 0.1f);
        Mat4 w;
        float anim[3] = { hd->tp[0], hd->tp[1], hd->tp[2] };
        if (scene_monster_hand(&v->scene, h, &w) == 0) {
            anim[0] = w.m[12];
            anim[1] = w.m[13];
            anim[2] = w.m[14];
        }
        hand_update(hd, h, &v->mount, &v->scene, anim, dt);
        float wt = hand_weight(hd);
        if (getenv("RAMPAGE_HANDS") && (wt > 0 || hd->target > 0))
            printf("  hand %d: w %.2f%s%s, clip's at %.2f %.2f %.2f, held at %.2f %.2f %.2f\n", h, wt,
                   hd->attaching ? " attaching" : "", hd->planted ? " planted" : "", anim[0], anim[1], anim[2],
                   hd->sp[0], hd->sp[1], hd->sp[2]);
        v->scene.ik_w[h] = hd->valid ? wt : 0;
        if (!hd->valid)
            continue;
        /* model space: the world less the monster's place, turned back by its heading */
        float d[3] = { hd->sp[0] - v->mx, hd->sp[1] - v->my, hd->sp[2] - v->mz }, r[9], rz[9] = { c, -sn, 0, sn, c, 0, 0, 0, 1 };
        v->scene.ik_pos[h][0] = c * d[0] + sn * d[1];
        v->scene.ik_pos[h][1] = -sn * d[0] + c * d[1];
        v->scene.ik_pos[h][2] = d[2];
        ik_rot_of(hd->sq, r);
        for (int col = 0; col < 3; col++)            /* Rz(-heading) x r */
            for (int k = 0; k < 3; k++)
                v->scene.ik_rot[h][3 * col + k] = rz[k] * r[3 * col] + rz[3 + k] * r[3 * col + 1] + rz[6 + k] * r[3 * col + 2];
    }
}

/* --do: "right 1, cross 0.5, wait 1.5": the stick held one way or a button held down, for that many seconds of play
 * (no seconds: one frame, a tap); "collapse" destroys the building in front of the monster, as collapseBuilding does;
 * "mirror" flips the mirror state for the next clip; "at x y [heading]" puts the monster there, facing the buildings (or that way) */
static void play_script(Viewer *v, const char *script, float aspect)
{
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
        if (_stricmp(word, "mirror") == 0) {         /* the next clip mirrored, as an event of type 8 would */
            v->mirror_queued = !v->mirror_queued;
            continue;
        }
        if (_stricmp(word, "at") == 0) {             /* at x y: the monster stands there, facing the buildings */
            char *end;
            v->mx = seconds;
            v->my = (float)strtod(p, &end);
            p = end;
            v->mz = 0;
            v->heading = 0;
            while (*p == ' ') p++;
            if ((*p >= '0' && *p <= '9') || *p == '-') {   /* at x y heading */
                v->heading = (float)strtod(p, &end);
                p = end;
            }
            continue;
        }
        if (_stricmp(word, "collapse") == 0) {       /* collapseBuilding on the building in front of the monster */
            for (uint32_t i = 0; i < v->scene.nbuildings; i++) {
                Building *b = &v->scene.buildings[i].b;
                if (v->mx >= b->min[0] && v->mx <= b->max[0] && v->my < b->min[1] && v->my > b->min[1] - 10)
                    building_destroy(b);
            }
            continue;
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
                    v->rx = WORDS[i].x;
                    v->ry = WORDS[i].y;
                }
                play_step(v, right ? 0 : WORDS[i].x, right ? 0 : WORDS[i].y, pressed, held, 1.0f / 60, aspect);
                if (v->have_graph && v->scene.monster >= 0) {   /* posed each frame, as the main loop does */
                    v->scene.instances[v->scene.monster].offset =
                        (float)clip_at(v, &v->graph.states[v->run.state], v->run.time);
                    scene_update(&v->scene, 0);
                    hit_spheres(v);
                }
                if (WORDS[i].button >= 0)
                    held[WORDS[i].button] = 1;
                memset(pressed, 0, sizeof pressed);
            }
        }
    }
}

int main(int argc, char **argv)
{
    static Viewer v;
    const char *shot = NULL, *monster = NULL, *pos[3] = { "build\\disc\\RAMPAGE.VOL", "Chicago", "1" };
    int npos = 0, night = 0, clip = -1, have_view = 0, mute = 0;
    float view[5] = { 0 };
    double shot_time = 0;
    const char *script = NULL;
    char walk_script[48];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--monster") == 0 && i + 1 < argc) monster = argv[++i];
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
            v.have_fx_bundle = 1;
            v.fxlib = fx_open(elf, bl, 1);
            v.fxw = v.fxlib ? fx_world(v.fxlib) : NULL;
            if (v.fxw)
                fx_on_shake(v.fxw, fx_shake, &v);
        } else
            free(data);
    }
    v.night = night;
    v.mx = 0; v.my = -18; v.heading = 180;          /* in the street, facing the camera */
    v.show_hits = 1;
    v.nmonsters = monster_names(&v.vol, v.monsters, 64);
    v.monster = -1;
    v.clip = clip;
    for (uint32_t i = 0; monster && i < v.nmonsters; i++)
        if (_stricmp(monster, v.monsters[i]) == 0)
            v.monster = (int)i;
    v.play = v.monster >= 0 && clip < 0;
    if (open_city(&v, city, block) != 0) {
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
    if (v.monster >= 0) {                                  /* closer, the monster in front */
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
        if (v.game_block >= 0) {                     /* the game moved on to the next block */
            v.block = (uint32_t)v.game_block;
            v.game_block = -1;
            v.game_built = 1;
            build(&v);
            v.game_built = 0;
        }
        if (w.pressed[VK_NEXT] && v.nblocks) { v.block = (v.block + 1) % v.nblocks; build(&v); }
        if (w.pressed[VK_PRIOR] && v.nblocks) { v.block = (v.block + v.nblocks - 1) % v.nblocks; build(&v); }
        if (w.pressed['N']) { v.night = !v.night; build(&v); }
        if (w.pressed['F']) v.fog = !v.fog;
        if (w.pressed['H']) v.show_hits = !v.show_hits;
        if (w.pressed['M']) { v.monster = v.monster + 1 < (int)v.nmonsters ? v.monster + 1 : -1; v.clip = -1; build(&v); }
        if (w.pressed['G'] && v.monster >= 0) {
            v.play = !v.play;
            cam_init(&v.cam, &v.cam_settings);
            if (v.play && v.have_graph)
                play_state(&v);
        }
        if (v.monster >= 0 && !v.play && (w.pressed['K'] || w.pressed['L'])) {
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
        int playing = v.play && v.monster >= 0 && v.have_scene && v.scene.monster >= 0;
        if (playing) {
            float aspect = w.width ? (float)w.height / w.width : 0.75f;
            static int settled;
            if (shot && !settled) {                     /* a shot: the camera settles, then the script */
                play_script(&v, "wait 2", aspect);
                if (script)
                    play_script(&v, script, aspect);
            }
            settled = 1;
            float lx = (float)(w.keys['D'] - w.keys['A']);   /* the left stick: W A S D; the right: the arrows */
            float ly = (float)(w.keys['S'] - w.keys['W']);
            v.rx = (float)(w.keys[VK_RIGHT] - w.keys[VK_LEFT]);
            v.ry = (float)(w.keys[VK_DOWN] - w.keys[VK_UP]);
            static const struct { int key, button; } PAD[] = {
                { VK_SPACE, MV_CROSS }, { 'J', MV_SQUARE }, { 'K', MV_TRIANGLE }, { 'L', MV_CIRCLE }, { 'R', MV_R2 } };
            uint8_t pressed[MV_COUNT] = { 0 }, held[MV_COUNT] = { 0 };
            for (size_t k = 0; k < sizeof PAD / sizeof *PAD; k++) {
                pressed[PAD[k].button] = w.pressed[PAD[k].key];
                held[PAD[k].button] = w.keys[PAD[k].key];
            }
            play_step(&v, lx, ly, pressed, held, shot ? 0 : (dt > 0.1f ? 0.1f : dt), aspect);
            v.scene.instances[v.scene.monster].offset =      /* the clip at the state's time (clip_at) */
                (float)((v.have_graph ? clip_at(&v, &v.graph.states[v.run.state], v.run.time) : v.run.time) - (shot ? shot_time : now - start));
            memcpy(cam.pos, v.cam.pos, sizeof cam.pos);
            cam_shaken(v.cam.pos, (const float (*)[3])v.cam.rot, v.shakes, 8, cam.rows);   /* after the smoothing */
            cam.use_rows = 1;
            cam.fov_deg = v.cam.s.fov * 57.2957795f;
        } else {
            if (v.have_scene) {
                scene_buildings_update(&v.scene, dt > 0.1f ? 0.1f : dt);
                v.scene.cloth_dt += dt > 0.1f ? 0.1f : dt;   /* the flags fly in the free camera too */
                if (v.veh)                           /* and the traffic drives on, with no monster about */
                    vehicle_update(v.veh, dt > 0.1f ? 0.1f : dt, NULL, 0, NULL, 0);
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
                hit_spheres(&v);
            else
                v.scene.nspheres = 0;
        }
        {                                            /* this frame's particles */
            static FxQuad quads[8192];
            v.scene.fx = quads;
            v.scene.nfx = v.fxw ? fx_quads(v.fxw, quads, 8192) : 0;
            if (shot && getenv("RAMPAGE_TRACE") && v.fxw)
                printf("particles %d, quads %d (first at %.1f %.1f %.1f, size %.2f, alpha %.2f)\n", fx_particles(v.fxw),
                       v.scene.nfx, v.scene.nfx ? quads[0].pos[0] : 0, v.scene.nfx ? quads[0].pos[1] : 0,
                       v.scene.nfx ? quads[0].pos[2] : 0, v.scene.nfx ? quads[0].size[0] : 0, v.scene.nfx ? quads[0].col[3] : 0);
        }
        {                                            /* the HUD, through this frame's camera */
            static HudQuad hq[4096];
            if (have_view) {
                Camera fixed = { { view[0], view[1], view[2] }, view[3] / 57.2957795f, view[4] / 57.2957795f, 60 };
                v.hud_cam = fixed;
            } else
                v.hud_cam = cam;
            v.scene.hud = hq;
            v.scene.nhud = v.hud ? hud_quads(v.hud, hq, 4096) : 0;
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
                     v.monster >= 0 ? ", " : "", v.monster >= 0 && v.scene.monster_clip ? v.scene.monster_clip : "",
                     rnd_triangles(v.rnd), frames / (now - title_at), v.fog ? ", fog" : "");
            win_title(&w, title);
            title_at = now;
            frames = 0;
        }
    }
    rnd_free(v.rnd);
    if (v.have_graph) {
        moves_stop(&v.run);
        moves_free(&v.graph);
    }
    if (v.have_scene)
        scene_free(&v.scene);
    city_close(v.city);
    sound_end();
    sound_free();
    win_close(&w);
    return 0;
}
