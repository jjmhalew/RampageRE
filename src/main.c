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
} Viewer;

/* the clip's root at time t (seconds of the clip: the state's time × its rate), held at the clip's last frame
 * (clip_pose loops: its end is its start) */
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
    v->entry_yaw = root_yaw(v, v->run.time * st->rate);
    v->entry_t = v->run.time * st->rate;
    if (getenv("RAMPAGE_ROOT")) {
        float rt[3], rq[4];
        if (root_at(v, v->run.time * st->rate, rt, rq) == 0)
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
    if (root_at(v, v->run.time * st->rate, v->entry_root, q) != 0)
        memset(v->entry_root, 0, sizeof v->entry_root);
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
    memset(&v->input, 0, sizeof v->input);
    memset(v->vel, 0, sizeof v->vel);
    v->mirror = v->mirror_queued = 0;
    memset(&v->mount, 0, sizeof v->mount);
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

static void build(Viewer *v)
{
    if (v->have_scene)
        scene_free(&v->scene);
    v->have_scene = city_build(v->city, v->blocks[v->block], v->night, &v->scene) == 0;
    if (v->have_scene && v->monster >= 0 &&
        city_monster(v->city, &v->scene, v->monsters[v->monster], v->clip, v->mx, v->my, v->heading) == 0)
        printf("%s: %s\n", v->monsters[v->monster], v->scene.monster_clip ? v->scene.monster_clip : "-");
    start_moves(v);
    Group tt;
    if (city_camera(v->city, &tt) == 0)
        cam_settings(&v->cam_settings, tt);
    cam_init(&v->cam, &v->cam_settings);
    if (v->have_scene)
        rnd_scene(v->rnd, &v->scene);
    printf("%s %s%s: %u models drawn, %d people, %u vehicles, %d not found\n", CITIES[v->city_index],
           v->blocks[v->block], v->night ? " (night)" : "", v->scene.nitems, v->scene.people, v->scene.nvehicles,
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
    static const char *const YES[] = { "Monsters::hasRoarEnergy", "Monsters::canDoPunch", "Monsters::canSpin",
                                       "Monsters::hasDoubleJump", "Monsters::hasJumpSmash", "Monsters::hasChargePunch" };
    Viewer *v = ctx;
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
    else if (!strcmp(n, "Monsters::prepareToMount")) { climb_prepare(&v->mount); return; }
    else if (!strcmp(n, "Monsters::mountBuilding")) {
        Climber cl = climber(v);
        climb_mount(&v->mount, &cl, e->p[0]);        /* and transitionToMount(param0) */
        return;
    }
    else if (!strcmp(n, "Monsters::unmountBuilding")) { climb_unmount(&v->mount); return; }
    else if (!strcmp(n, "Monsters::transferMount")) { climb_transfer(&v->mount); return; }
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
static float ground_at(const Viewer *v, float x, float y, float z)
{
    float g = 0;
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
 * against the state's forward axis; beyond one frame's worth of turning it turns by the angle × `turn` × dt, so it
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
    if (!v->have_graph || !v->show_hits || !v->armed || v->scene.monster < 0)
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
    }
}

static void play_step(Viewer *v, float lx, float ly, const uint8_t *pressed, const uint8_t *held, float dt, float aspect)
{
    if (v->have_graph) {
        MoveInput *in = &v->input;
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
        int ev = -1;
        if (v->mode == MODE_AIRBORNE || v->mode == MODE_TAKEOFF) {   /* the air: velocity, then gravity */
            v->mx += v->vel[0] * dt;
            v->my += v->vel[1] * dt;
            v->mz += v->vel[2] * dt;
            if (!v->no_gravity)
                v->vel[2] -= MOVES_GRAVITY * dt;
            float g = ground_at(v, v->mx, v->my, v->mz);
            if (v->mz <= g && v->vel[2] <= 0) {
                v->mz = g;
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
        } else {
            memset(v->vel, 0, sizeof v->vel);
            float g = ground_at(v, v->mx, v->my, v->mz);
            if (g < v->mz - 0.5f)                    /* walked off a roof: it falls (a stand-in, the game's way is */
                v->mode = MODE_AIRBORNE;             /* not read) and lands through the landing slot */
            else
                v->mz = g;
        }
        air_control(v, st, lx, ly, dt);
        if (v->mode == MODE_GROUND || v->mode == MODE_AIRBORNE || v->mode == MODE_TAKEOFF || v->mode == MODE_PRONE) {
            v->mx += v->air[0] * dt;                 /* 0x2E0C50: in modes 0, 1, 2 and 4 */
            v->my += v->air[1] * dt;
        }
        if (st->flags & NODE_STEER)
            v->heading += steer(v, st, lx, ly, dt);
        scene_monster_blend_step(&v->scene, dt);
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
        int switched = ev == 0 || moves_step(&v->run, in, dt, answer, v);
        float t1 = switched ? (ev == 0 ? t0 : (t0 + dt < length ? t0 + dt : length)) : v->run.time;
        float a[3], b[3], q[4];                      /* the root's travel and turn this frame the state hands the monster */
        if (root_at(v, t0 * st->rate, a, q) == 0 && root_at(v, t1 * st->rate, b, q) == 0) {
            float c = cosf(v->heading / 57.2957795f), s = sinf(v->heading / 57.2957795f);
            float dx = st->flags & NODE_ROOT_X ? b[0] - a[0] : 0, dy = st->flags & NODE_ROOT_Y ? b[1] - a[1] : 0;
            float m = v->mirror ? -1.0f : 1.0f;      /* mirrored: across the monster's x */
            v->mx += c * m * dx - s * dy;
            v->my += s * m * dx + c * dy;
            if (st->flags & NODE_ROOT_Z)
                v->mz += b[2] - a[2];
            if (st->flags & NODE_ROOT_TURN)
                v->heading += m * root_turn(v, t0 * st->rate, t1 * st->rate);
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
            int bake = (st->flags & NODE_BAKE_ROOT) && root_at(v, t1 * st->rate, b, q) == 0;
            float bake_p[3], bake_r[3][3];
            if (bake) {
                for (int k = 0; k < 3; k++)
                    bake_p[k] = st->flags & (k == 0 ? NODE_ROOT_X : k == 1 ? NODE_ROOT_Y : NODE_ROOT_Z) ?
                                v->entry_root[k] : b[k];
                float zero[3] = { 0, 0, 0 }, tw = st->flags & NODE_ROOT_TURN ? root_turn(v, v->entry_t, t1 * st->rate) : 0;
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
    scene_buildings_update(&v->scene, dt);
    if (v->have_graph) {                             /* the pose keeps what the monster did not take over */
        const MoveState *st = &v->graph.states[v->run.state];
        float turn = st->flags & NODE_ROOT_TURN ?
            root_turn(v, v->entry_t, v->run.time * st->rate) : 0;
        scene_monster_root_motion(&v->scene, (st->flags & NODE_ROOT_X ? 1 : 0) | (st->flags & NODE_ROOT_Y ? 2 : 0),
                                  v->entry_root, turn);
    }
    float at[1][3] = { { v->mx, v->my, v->mz } };
    cam_update(&v->cam, (const float (*)[3])at, 1, dt, aspect);
}

/* --do: "right 1, cross 0.5, wait 1.5": the stick held one way or a button held down, for that many seconds of play
 * (no seconds: one frame, a tap); "collapse" destroys the building in front of the monster, as collapseBuilding does;
 * "mirror" flips the mirror state for the next clip; "at x y" puts the monster there, facing the buildings */
static void play_script(Viewer *v, const char *script, float aspect)
{
    static const struct { const char *name; int button; float x, y; } WORDS[] = {
        { "left", -1, -1, 0 }, { "right", -1, 1, 0 }, { "up", -1, 0, -1 }, { "down", -1, 0, 1 }, { "wait", -1, 0, 0 },
        { "cross", MV_CROSS, 0, 0 }, { "square", MV_SQUARE, 0, 0 }, { "triangle", MV_TRIANGLE, 0, 0 },
        { "circle", MV_CIRCLE, 0, 0 }, { "r2", MV_R2, 0, 0 } };
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
                play_step(v, WORDS[i].x, WORDS[i].y, pressed, held, 1.0f / 60, aspect);
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
    int npos = 0, night = 0, clip = -1;
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
    if (win_open(&w, "RampageRE", 1280, 720) != 0) {
        plat_message("Cannot open an OpenGL window.", 1);
        return 1;
    }
    v.rnd = rnd_create();
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
            float lx = (float)((w.keys['D'] || w.keys[VK_RIGHT]) - (w.keys['A'] || w.keys[VK_LEFT]));
            float ly = (float)((w.keys['S'] || w.keys[VK_DOWN]) - (w.keys['W'] || w.keys[VK_UP]));
            static const struct { int key, button; } PAD[] = {
                { VK_SPACE, MV_CROSS }, { 'J', MV_SQUARE }, { 'K', MV_TRIANGLE }, { 'L', MV_CIRCLE }, { 'R', MV_R2 } };
            uint8_t pressed[MV_COUNT] = { 0 }, held[MV_COUNT] = { 0 };
            for (size_t k = 0; k < sizeof PAD / sizeof *PAD; k++) {
                pressed[PAD[k].button] = w.pressed[PAD[k].key];
                held[PAD[k].button] = w.keys[PAD[k].key];
            }
            play_step(&v, lx, ly, pressed, held, shot ? 0 : (dt > 0.1f ? 0.1f : dt), aspect);
            v.scene.instances[v.scene.monster].offset =      /* the clip at the state's time × its rate */
                (float)(v.run.time * (v.have_graph ? v.graph.states[v.run.state].rate : 1) - (shot ? shot_time : now - start));
            memcpy(cam.pos, v.cam.pos, sizeof cam.pos);
            memcpy(cam.rows, v.cam.rot, sizeof cam.rows);
            cam.use_rows = 1;
            cam.fov_deg = v.cam.s.fov * 57.2957795f;
        } else {
            if (v.have_scene)
                scene_buildings_update(&v.scene, dt > 0.1f ? 0.1f : dt);
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
        if (v.have_scene) {
            scene_update(&v.scene, t);
            if (playing)
                hit_spheres(&v);
            else
                v.scene.nspheres = 0;
        }
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
    win_close(&w);
    return 0;
}
