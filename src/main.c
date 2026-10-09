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
    int yaw_axis; float entry_yaw;                   /* the root's axis that tells its turn; its turn at the state's start */
} Viewer;

/* the clip's root at time t, held at the clip's last frame (clip_pose loops: its end is its start) */
static int root_at(const Viewer *v, double t, float tr[3], float q[4])
{
    const Instance *in = &v->scene.instances[v->scene.monster];
    if (in->has_clip && t > in->clip.duration - 1e-4)
        t = in->clip.duration - 1e-4;
    return scene_monster_root(&v->scene, t < 0 ? 0 : t, tr, q);
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
    v->entry_yaw = root_yaw(v, v->run.time);
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
static int answer(void *ctx, const char *call)
{
    static const char *const YES[] = { "Monsters::hasRoarEnergy", "Monsters::canDoPunch", "Monsters::canSpin",
                                       "Monsters::hasDoubleJump", "Monsters::hasJumpSmash", "Monsters::hasChargePunch" };
    (void)ctx;
    for (size_t i = 0; i < sizeof YES / sizeof *YES; i++)
        if (strcmp(call, YES[i]) == 0)
            return 1;
    return 0;
}

static int starts(const char *s, const char *prefix) { return s && _strnicmp(s, prefix, strlen(prefix)) == 0; }

/* the clip events the viewer acts on: the movement mode, and Monsters.bsf's velocity functions (jumpNorm and its kin
 * set the velocity, addVelocity adds to it, in the world or turned with the monster) */
static void clip_event(Viewer *v, const MoveEvent *e)
{
    float c = cosf(v->heading / 57.2957795f), s = sinf(v->heading / 57.2957795f);
    float mirror = s > 0 ? -1.0f : 1.0f;            /* Monster.getMirrorState: facing -x */
    float w[3] = { 0, 0, 0 }, l[3] = { 0, 0, 0 };   /* world / relative velocity */
    int set = 0, add = 0, relative = 0;
    if (e->type == 7) {                              /* 0x2E7DE0 */
        v->mode = (int)e->value;
        return;
    }
    if (e->type == 23) {                             /* 0x2E7D38 */
        v->column = (int32_t)e->value;
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

/* one frame of play. The left stick (x right, y down) and the buttons pressed this frame (MV_CROSS ...) or held.
 * The graph picks the state; the clip's root carries the monster (its travel and turn added to where it stands), the
 * clip's events set its movement mode and velocity, and in the air it falls by MOVES_GRAVITY until it meets the
 * ground (here the street at z = 0), which is the graph's landing event. Two things stand in for game code not read
 * yet: turning round (Ground_TurnAround) when the stick points back, and turning towards the stick while walking. */
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
        const MoveState *st = &v->graph.states[v->run.state];
        float t0 = v->run.time, length = st->length;
        int ev = -1;
        if (v->mode == MODE_AIRBORNE || v->mode == MODE_TAKEOFF) {   /* the air: velocity, then gravity */
            v->mx += v->vel[0] * dt;
            v->my += v->vel[1] * dt;
            v->mz += v->vel[2] * dt;
            if (!v->no_gravity)
                v->vel[2] -= MOVES_GRAVITY * dt;
            if (v->mz <= 0 && v->vel[2] <= 0) {
                v->mz = 0;
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
        } else {
            memset(v->vel, 0, sizeof v->vel);
            v->mz = 0;
        }
        float goal = atan2f(-ly, lx) * 57.2957795f - 90, turn = fmodf(goal - v->heading + 540, 360) - 180;
        if (ev != 0 && in->v[MV_L_SIZE] > 0.5f && starts(st->name, "Ground_Walk") && fabsf(turn) > 135)
            ev = moves_event(&v->run, "Ground_TurnAround");
        int switched = ev == 0 || moves_step(&v->run, in, dt, answer, NULL);
        float t1 = switched ? (ev == 0 ? t0 : (t0 + dt < length ? t0 + dt : length)) : v->run.time;
        float a[3], b[3], q[4];                      /* the root's travel this frame, in the old clip */
        if (root_at(v, t0, a, q) == 0 && root_at(v, t1, b, q) == 0) {
            float c = cosf(v->heading / 57.2957795f), s = sinf(v->heading / 57.2957795f);
            float dx = b[0] - a[0], dy = b[1] - a[1];
            v->mx += c * dx - s * dy;
            v->my += s * dx + c * dy;
        }
        clip_events(v, st, t0, t1);                  /* the old clip's events up to now */
        if (switched) {                              /* the turn the old clip made, if it turned (not a sway) */
            float turned = fmodf(root_yaw(v, t1) - v->entry_yaw + 540, 360) - 180;
            if (fabsf(turned) > 90)
                v->heading += turned;
            const MoveState *to = &v->graph.states[v->run.state];
            if (v->run.from != v->run.state)
                printf("%s -> %s (%s)\n", st->name ? st->name : "?", to->name ? to->name : "?", to->clip ? to->clip : "-");
            play_state(v);
            v->column = -1;                          /* 0x2E7F70: a new state starts without the old one's effects */
            v->no_gravity = 0;
            /* and the new one's first: those at its start (its set-up: mode, column) and, entered part-way (phase
             * kept), those where it starts; the game's own event playback (under 0x2FFA00) is not read yet */
            const MoveEvent *e0;
            int n0 = moves_events_between(to, -1, 0, &e0);
            for (int i = 0; i < n0; i++)
                clip_event(v, &e0[i]);
            if (v->run.time > 0)
                clip_events(v, to, v->run.via >= 0 ? v->run.time - 1e-5f : 0, v->run.time);
        } else if (starts(st->name, "Ground_Walk") && in->v[MV_L_SIZE] > 0) {
            float step = 270 * dt;
            v->heading += turn > step ? step : turn < -step ? -step : turn;
        }
        if (v->mx < -75) v->mx = -75;
        if (v->mx > 75) v->mx = 75;
        if (v->my < -30) v->my = -30;
        if (v->my > -3) v->my = -3;
        v->heading = fmodf(v->heading + 360, 360);
        if (getenv("RAMPAGE_TRACE"))
            printf("  %s %.2f at %.1f %.1f %.1f heading %.0f mode %d vz %.1f\n", v->graph.states[v->run.state].name,
                   v->run.time, v->mx, v->my, v->mz, v->heading, v->mode, v->vel[2]);
    }
    scene_monster_place(&v->scene, v->mx, v->my, v->mz, v->heading, 1);
    float at[1][3] = { { v->mx, v->my, v->mz } };
    cam_update(&v->cam, (const float (*)[3])at, 1, dt, aspect);
}

/* --do: "right 1, cross 0.5, wait 1.5": the stick held one way or a button held down, for that many seconds of play
 * (no seconds: one frame, a tap) */
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
        if ((*p >= '0' && *p <= '9') || *p == '.') {
            char *end;
            seconds = (float)strtod(p, &end);
            p = end;
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
            v.scene.instances[v.scene.monster].offset = (float)(v.run.time - (shot ? shot_time : now - start));
            memcpy(cam.pos, v.cam.pos, sizeof cam.pos);
            memcpy(cam.rows, v.cam.rot, sizeof cam.rows);
            cam.use_rows = 1;
            cam.fov_deg = v.cam.s.fov * 57.2957795f;
        } else {
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
        if (v.have_scene)
            scene_update(&v.scene, t);
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
