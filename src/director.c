/* director.c - the game flow's choreography (director.h; docs/FLOW_NOTES.md): a monster's goals (playMove, walkTo) and
 * the AI's way of driving the move graph, and the camera manager's modes, the script camera and the blend between
 * modes, transcribed from the PS2 code. [R] = read in that code, [G] = a guess or a stand-in.
 *
 * The monster: an AI monster (setIsAI(1), +0x108) does not read the pad's conditions; each frame its goal manager
 * (0x2EE430, through 0x2E1AA0) runs the first goal of its queue, and the goal picks the move graph's transitions
 * itself: a played move goes to its state along the graph's next-hop table, one transition at a time, or is forced
 * there when no way leads to it; it has ended once the monster has been in that state and leaves it (or its clip
 * starts over). A walk pushes the stick, in world axes, towards its way point and asks for the walk (action 9) until
 * the monster stands within the way point's radius; then the next way point, then the next goal. With no goal the AI
 * asks for an idle (action 11). The picked transition is taken when its switch window opens, as for the pad.
 *
 * The camera: the manager keeps a mode and the one before it; a new mode with a blend time takes over linearly: the
 * rotation a slerp from the old camera's (frozen when the blend began) to the new one's, the place a lerp from the old
 * mode's live place, the fov a lerp. The script camera follows a place and a point to look at (each a world point or an
 * object plus an offset) with speed- and acceleration-limited followers, then smooths what it shows by 0.95 a frame;
 * a cut snaps everything. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "director.h"

#define DIR_PI 3.14159265f

/* ================================================================ the move graph, as the AI reads it */

static const uint8_t *monster_data(const Bundle *b, uint32_t *size)
{
    for (uint32_t i = 0; i < b->nentries; i++) {
        const char *n = bdp_name(b, b->entries[i].name);
        size_t len = strlen(n);
        if (b->entries[i].type == ET_RAW && len >= 12 && strcmp(n + len - 12, "MONSTER_DATA") == 0) {
            *size = b->entries[i].size;
            return b->entries[i].data;
        }
    }
    return NULL;
}

int dir_graph_read(const Bundle *monster, DirGraph *ag)
{
    memset(ag, 0, sizeof *ag);
    uint32_t size = 0;
    const uint8_t *d = monster_data(monster, &size);
    if (!d || size < 0xAA8 || rd32(d) != 11)
        return -1;
    uint32_t graph = rd32(d + 0xA54);                /* header 0x34 bytes: +0x2C count, +0x30 the first state */
    if (graph + 0x34 > size)
        return -1;
    int n = (int)rd32(d + graph + 0x2C);
    uint32_t at = rd32(d + graph + 0x30);
    if (n <= 0 || n > 255)
        return -1;
    ag->n = n;
    ag->action = malloc((size_t)n);
    ag->fallback = malloc(sizeof *ag->fallback * (size_t)n);
    for (int k = 0; k < n; k++) {
        if (at + 0x2C > size || rd32(d + at) != (uint32_t)k)
            return dir_graph_free(ag), -1;
        uint32_t node = rd32(d + at + 4);            /* the state's node (0x84 bytes): +0x3C its action code [R 0x2F2630] */
        ag->action[k] = node + 0x40 <= size ? (int8_t)rd32(d + node + 0x3C) : DA_NONE;
        for (int a = 0; a < 16; a++)                 /* +0x18: a transition per action code, -1 none [R 0x2F2884] */
            ag->fallback[k][a] = (int8_t)d[at + 0x18 + a];
        at += 0x2C + 0x74 * (uint32_t)(int16_t)(d[at + 0xE] | d[at + 0xF] << 8);
    }
    /* the next-hop table: n x n bytes right after the states, [from * n + to] the state to go to next (0xFF none)
     * [R 0x2F2B90: the graph's +0x24 table, +0x2C stride; George's file ends with it] */
    if (at + (uint32_t)(n * n) > size)
        return dir_graph_free(ag), -1;
    ag->hop = malloc((size_t)(n * n));
    memcpy(ag->hop, d + at, (size_t)(n * n));
    return 0;
}

void dir_graph_free(DirGraph *ag)
{
    free(ag->action);
    free(ag->fallback);
    free(ag->hop);
    memset(ag, 0, sizeof *ag);
}

#define NODE_NOT_AI 0x40000000u                      /* a target node the AI may not enter [R 0x2F27F0] */

/* 0x2F22D0: 2 the AI may not take the transition (not open to a pad, a condition on "false" (the game's own events),
 * its script check says no); 0 it needs nothing (every condition is on "true"); 1 it waits on an input the AI does not
 * give, which the AI may take anyway [R] */
static int usable(const MoveTrans *t, MoveCall call, void *ctx)
{
    if (!(t->flags & 0x10))                          /* 0x2FF750 says pad: flag 0x10 [R] */
        return 2;
    int n = 0;
    for (int c = 0; c < t->ncond; c++) {
        if (t->cond[c].var == MV_FALSE)
            return 2;
        if (t->cond[c].var == MV_TRUE)
            n++;
    }
    if (t->call && !(call && call(ctx, t->call, t->args)))
        return 2;
    return n == t->ncond ? 0 : 1;
}

static int target_ok(const MoveGraph *g, const MoveTrans *t)
{
    return t->target >= 0 && t->target < g->n && !(g->states[t->target].flags & NODE_NOT_AI);
}

int dir_graph_reachable(const DirGraph *ag, const MoveGraph *g, int from, int to, MoveCall call, void *ctx)
{
    if (!ag->hop || from < 0 || to < 0 || from >= ag->n || to >= ag->n || from >= g->n || to >= g->n)
        return 0;
    int c = from;
    for (int step = 0; step < ag->n; step++) {       /* along the table, a usable transition (or the clip's end) each */
        int next = ag->hop[c * ag->n + to];
        if (next == 0xFF || next >= g->n)
            return 0;
        const MoveState *s = &g->states[c];
        int ok = 0;
        for (int i = 0; i < s->ntrans && !ok; i++)
            ok = s->trans[i].target == next && usable(&s->trans[i], call, ctx) != 2 && target_ok(g, &s->trans[i]);
        if (!ok && s->next != next)
            return 0;
        if (next == to)
            return 1;
        c = next;
    }
    return 0;
}

/* The AI's pick (0x2F2630 by action, 0x2F2970 by state): of the current state's transitions in their input window and
 * of at least the priority already picked, the first that needs nothing is taken at once; otherwise the last of the
 * highest priority leading where it wants (a state of that action code, or that state), unless the pick already leads
 * there; with none, the state's own transition for that action (no window asked). It then waits for its switch window
 * (0x2EBF40 sets the runner's pick, +0x1E) [R] */
static void ai_pick(MoveRunner *r, const DirGraph *ag, int action, int to_state, MoveCall call, void *ctx)
{
    const MoveGraph *g = r->g;
    const MoveState *s = &g->states[r->state];
    int want = to_state >= 0 ? r->state != to_state : action != DA_KEEP;   /* 0x2F2A84: not where it is already */
    int thr = r->chosen >= 0 ? s->trans[r->chosen].priority : -1, pick = -1;
    for (int i = 0; i < s->ntrans; i++) {
        const MoveTrans *t = &s->trans[i];
        if (t->priority < thr || t->target < 0 || t->target >= g->n)
            continue;
        if (r->in_window[i] && usable(t, call, ctx) == 0) {   /* one that needs nothing: at once */
            pick = i;
            break;
        }
        if (!want || !r->in_window[i])
            continue;
        int match = to_state >= 0 ? t->target == to_state : ag->action && ag->action[t->target] == action;
        if (!match || !target_ok(g, t) || usable(t, call, ctx) == 2)
            continue;
        if (r->chosen >= 0 && s->trans[r->chosen].target == t->target)
            continue;                                /* the pick already leads there */
        pick = i;
        thr = t->priority;
    }
    if (pick < 0 && want && to_state < 0 && action >= 0 && action < 16 && ag->fallback) {
        int f = ag->fallback[r->state][action];
        if (f >= 0 && f < s->ntrans && target_ok(g, &s->trans[f]) && usable(&s->trans[f], call, ctx) != 2)
            pick = f;
    }
    if (pick >= 0)
        r->chosen = pick;
}

/* ================================================================ the goals */

void dir_ai_init(DirAI *ai)
{
    memset(ai, 0, sizeof *ai);
    ai->force = -1;
}

void dir_ai_clear(DirAI *ai)
{
    ai->n = 0;                                       /* every goal unlinked and deleted, no callback [R 0x2EE760] */
    ai->current = 0;
    ai->force = -1;
}

/* 0x2EE600 (to the queue's end) then 0x2EE650: the goal running now is stopped and, its flag 2 being set on every
 * new goal (0x2ED810), deleted without its end callback [R] */
static DirGoal *add_goal(DirAI *ai, int type, int notify)
{
    if (ai->current && ai->n > 0) {                  /* the current goal (+0x1C, the queue's head once the update
                                                      * took it) */
        memmove(&ai->q[0], &ai->q[1], sizeof *ai->q * (size_t)(ai->n - 1));
        ai->n--;
    }
    ai->current = 0;
    if (ai->n >= DIR_GOALS)
        return NULL;
    DirGoal *g = &ai->q[ai->n++];
    memset(g, 0, sizeof *g);
    g->type = type;
    g->notify = notify;
    g->state = -1;
    return g;
}

int dir_ai_play_move(DirAI *ai, int state, int notify)
{
    DirGoal *g = add_goal(ai, DG_PLAY_MOVE, notify);
    if (!g)
        return -1;
    g->state = state;                                /* 0x2F1EE0: the state by name (0x2D9890) */
    return (int)(g - ai->q);
}

int dir_ai_walk_to(DirAI *ai, const float pos[3], float radius, int notify)
{
    DirGoal *g = add_goal(ai, DG_WALK_TO, notify);
    if (!g)
        return -1;
    dir_ai_add_waypoint(ai, (int)(g - ai->q), pos, radius);   /* 0x2F20E0: the list cleared, this one, from 0 */
    return (int)(g - ai->q);
}

void dir_ai_add_waypoint(DirAI *ai, int goal, const float pos[3], float radius)
{
    if (goal < 0 || goal >= ai->n || ai->q[goal].type != DG_WALK_TO || ai->q[goal].nwp >= DIR_WAYPOINTS)
        return;
    DirGoal *g = &ai->q[goal];
    float *w = g->wp[g->nwp++];                     /* {x, y, z, radius squared} [R 0x2F2020] */
    w[0] = pos[0];
    w[1] = pos[1];
    w[2] = pos[2];
    w[3] = radius * radius;
}

/* 0x2F21C0: begun and running, or begun and over (the end callback); returns 0 once the goal is over */
static int finish(DirAI *ai, DirGoal *g, int running)
{
    if (g->started) {
        if (running)
            return 1;
        g->started = 0;
        if (g->notify)                               /* the end callback (+0x14): Levels::moveComplete */
            ai->done++;
        return 0;
    }
    if (running)
        g->started = 1;                              /* the start callback (+0x10): none in the scripts */
    return 1;
}

/* PlayMoveGoal::update (0x2F1DB0) [R] */
static int play_update(DirAI *ai, DirGoal *g, const DirGraph *ag, MoveRunner *r, MoveCall call, void *ctx)
{
    if (g->state < 0 || g->state >= r->g->n) {      /* no such state: over at once [G: the game would wait forever] */
        if (g->notify)
            ai->done++;
        return 0;
    }
    int running = 0;
    if (!g->restarted && r->state == g->state) {     /* in it: only what needs nothing (action 15) */
        ai_pick(r, ag, DA_KEEP, -1, call, ctx);
        running = 1;
    } else if (!g->started) {                        /* on the way: the next state, or forced there */
        if (dir_graph_reachable(ag, r->g, r->state, g->state, call, ctx))
            ai_pick(r, ag, -1, ag->hop[r->state * ag->n + g->state], call, ctx);
        else
            ai->force = g->state;                    /* 0x2D9710(monster, state, 1, 0) */
    }
    return finish(ai, g, running);
}

/* 0x2EDB40 (mode 0, no avoiding): the stick towards the point, flat, length 1 ((1, 0) when on it), and the walk */
static void steer_to(DirAI *ai, const DirGraph *ag, MoveRunner *r, const float *p, const float pos[3], MoveCall call,
                     void *ctx)
{
    float dx = p[0] - pos[0], dy = p[1] - pos[1], l2 = dx * dx + dy * dy;
    if (l2 < 1e-6f) {
        ai->stick[0] = 1;
        ai->stick[1] = 0;
    } else {
        float l = 1 / sqrtf(l2);
        ai->stick[0] = dx * l;
        ai->stick[1] = dy * l;
    }
    ai_pick(r, ag, DA_WALK, -1, call, ctx);          /* 0x2F2630(pad, 9) [R 0x2EE378] */
}

/* WalkToGoal::update (0x2F1F20) [R] */
static int walk_update(DirAI *ai, DirGoal *g, const DirGraph *ag, MoveRunner *r, const float pos[3], MoveCall call,
                       void *ctx)
{
    if (g->nwp <= 0) {                               /* no way point: over [G] */
        if (g->notify)
            ai->done++;
        return 0;
    }
    const float *w = g->wp[g->cur];
    float dx = w[0] - pos[0], dy = w[1] - pos[1];
    if (dx * dx + dy * dy <= w[3] && ++g->cur >= g->nwp) {   /* there (in x, y): the next way point, or over */
        if (!g->started) {                           /* never begun: both callbacks, over */
            if (g->notify)
                ai->done++;
            return 0;
        }
        return finish(ai, g, 0);
    }
    steer_to(ai, ag, r, g->wp[g->cur], pos, call, ctx);
    return finish(ai, g, 1);
}

/* 0x2E6580 -> 0x2EE3F0 -> 0x2F1D80: a clip starts; a begun move whose own state starts again is over */
static void clip_started(DirAI *ai, const MoveRunner *r)
{
    if (ai->n > 0 && ai->q[0].type == DG_PLAY_MOVE && ai->q[0].started && r->state == ai->q[0].state)
        ai->q[0].restarted = 1;
}

/* a window [a, b) the time is in, or jumped over this frame (as moves.c) */
static int in_window(float a, float b, float prev, float time)
{
    return (time >= a && time < b) || (prev < a && time > b);
}

int dir_ai_step(DirAI *ai, const DirGraph *ag, MoveRunner *r, const float pos[3], float dt, MoveCall call, void *ctx)
{
    ai->done = 0;
    ai->stick[0] = ai->stick[1] = 0;                 /* +0x0 and +0xC cleared each frame [R 0x2EE430] */
    if (!r->g || r->state < 0 || r->state >= r->g->n)
        return 0;
    if (ai->force >= 0 && ai->force < r->g->n) {     /* last frame's forced state: the controller takes the start list
                                                      * (0x2C0AD0 -> 0x2EBF50), no transition [R] */
        moves_force(r, ai->force);
        ai->force = -1;
        clip_started(ai, r);
        return 1;
    }
    ai->force = -1;
    const MoveGraph *g = r->g;
    const MoveState *s = &g->states[r->state];
    float prev = r->time;
    r->time += dt;                                   /* 0x2EBFE0: the windows */
    for (int i = 0; i < s->ntrans; i++) {
        const MoveTrans *t = &s->trans[i];
        if (in_window(t->in0, t->in1, prev, r->time)) r->in_window[i] = 1;
        if (in_window(t->sw0, t->sw1, prev, r->time)) r->sw_window[i] = 1;
    }
    /* 0x2EE430: the current goal, or the next one; over: out of the queue, and the idle asked for at once */
    int running = 0;
    if (ai->n > 0) {
        DirGoal *gl = &ai->q[0];
        ai->current = 1;
        running = gl->type == DG_WALK_TO ? walk_update(ai, gl, ag, r, pos, call, ctx)
                                         : play_update(ai, gl, ag, r, call, ctx);
        if (!running) {
            memmove(&ai->q[0], &ai->q[1], sizeof *ai->q * (size_t)(ai->n - 1));
            ai->n--;
            ai->current = 0;
        }
    }
    if (!running)
        ai_pick(r, ag, DA_IDLE, -1, call, ctx);      /* 0x2F2630(pad, 11) */
    /* the switch, as moves_step: the pick when its switch window is open, else the clip's end */
    int switched = 0;
    if (r->chosen >= 0 && r->sw_window[r->chosen]) {
        const MoveTrans *t = &s->trans[r->chosen];
        const MoveState *to = &g->states[t->target];
        float start = 0;                             /* 0x2ECBE0: flag 1 keeps the phase */
        if ((t->flags & 1) && s->length > 0) {
            start = to->length * (r->time / s->length);
            if (start > to->length) start = to->length;
        }
        int via = r->chosen;
        moves_force(r, t->target);
        r->via = via;
        r->time = start;
        switched = 1;
    } else if (r->time > s->length) {
        if (s->next == r->state) {
            float t = s->length > 0 ? fmodf(r->time, s->length) : 0;
            moves_force(r, s->next);
            r->time = t;
        } else {
            moves_force(r, s->next >= 0 ? s->next : r->state);
        }
        switched = 1;
    } else {
        for (int i = 0; i < s->ntrans; i++) {
            const MoveTrans *t = &s->trans[i];
            if (r->time >= t->in1) r->in_window[i] = 0;
            if (r->time >= t->sw1) {
                r->sw_window[i] = 0;
                if (r->chosen == i) r->chosen = -1;
            }
        }
    }
    if (switched)
        clip_started(ai, r);
    return switched;
}

/* ================================================================ quaternions (x, y, z, w; rows = R e_i, as camera.c) */

static void quat_from_rows(const float m[3][3], float q[4])
{
    float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        float s = sqrtf(tr + 1) * 2;
        q[3] = 0.25f * s; q[0] = (m[1][2] - m[2][1]) / s; q[1] = (m[2][0] - m[0][2]) / s; q[2] = (m[0][1] - m[1][0]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrtf(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q[3] = (m[1][2] - m[2][1]) / s; q[0] = 0.25f * s; q[1] = (m[1][0] + m[0][1]) / s; q[2] = (m[2][0] + m[0][2]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = sqrtf(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q[3] = (m[2][0] - m[0][2]) / s; q[0] = (m[1][0] + m[0][1]) / s; q[1] = 0.25f * s; q[2] = (m[2][1] + m[1][2]) / s;
    } else {
        float s = sqrtf(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q[3] = (m[0][1] - m[1][0]) / s; q[0] = (m[2][0] + m[0][2]) / s; q[1] = (m[2][1] + m[1][2]) / s; q[2] = 0.25f * s;
    }
}

static void rows_from_quat(const float q[4], float m[3][3])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y + z * w); m[0][2] = 2 * (x * z - y * w);
    m[1][0] = 2 * (x * y - z * w); m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z + x * w);
    m[2][0] = 2 * (x * z + y * w); m[2][1] = 2 * (y * z - x * w); m[2][2] = 1 - 2 * (x * x + y * y);
}

static void quat_mul(const float a[4], const float b[4], float o[4])   /* R(o) = R(a) R(b) */
{
    float r[4] = { a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                   a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                   a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                   a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2] };
    memcpy(o, r, sizeof r);
}

static void quat_norm(float q[4])
{
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l < 1e-12f) {
        q[0] = q[1] = q[2] = 0;
        q[3] = 1;
        return;
    }
    for (int i = 0; i < 4; i++) q[i] /= l;
}

static float qdot(const float a[4], const float b[4]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; }

/* 0x1042F0 / 0x167DE0: from a towards b by t (b turned to a's side first by the caller); t under 0.001 gives a, over
 * 0.999 counts as 1 [R] */
static void slerp(const float a[4], const float b[4], float t, float o[4])
{
    if (t < 0.001f) {
        memcpy(o, a, sizeof(float) * 4);
        return;
    }
    if (t > 0.999f)
        t = 1;
    float c = qdot(a, b);
    if (c > 1) c = 1;
    float th = acosf(c), s = sinf(th);
    if (th < 1e-5f || s < 1e-6f) {
        for (int i = 0; i < 4; i++) o[i] = a[i] + t * (b[i] - a[i]);
    } else {
        float wa = sinf((1 - t) * th) / s, wb = sinf(t * th) / s;
        for (int i = 0; i < 4; i++) o[i] = wa * a[i] + wb * b[i];
    }
    quat_norm(o);
}

/* per-frame smoothing (0x103FB0: last + (new - last) x (1 - k)) for frames of dt at the game's rate */
static float frame_keep(float k, float dt, float hz)
{
    if (k <= 0) return 0;
    if (k >= 1) return 1;
    return powf(k, dt * hz);
}

/* ================================================================ the script camera (RScriptCam) */

/* the followers (0x13B1B0 place, 0x13AD70 rotation, 0x13B2E0 fov), built by the loader 0x3121C0 with (1000, 1000, 0.1),
 * (pi / 2, pi / 2, 0.001) and pi / 2 (camera.prb's Script group is empty), each times the frame's dt into per-frame
 * steps; here per second [G: the dt at load taken as one frame] */
#define POS_SPEED 1000.0f                            /* max speed, units / s */
#define POS_ACCEL 1000.0f                            /* units / s^2 */
#define POS_RADIUS 0.1f                              /* there: closer than this, slower than one frame's accel */
#define ROT_SPEED (DIR_PI / 2)
#define ROT_ACCEL (DIR_PI / 2)
#define ROT_RADIUS 0.001f
#define FOV_RATE (DIR_PI / 2)                        /* radians / s */

static void script_load(ScriptCam *s)                /* 0x3121C0 [R] */
{
    memset(s, 0, sizeof *s);
    s->tgt_obj = s->loc_obj = -1;
    s->smoothing = 0.95f;
    s->pos_inertia = s->rot_inertia = 1;
    s->q[3] = s->qgoal[3] = s->last_q[3] = 1;
    s->fov = s->fov_goal = 0.5f;
}

/* 0x312100: it starts from the camera shown (the controller's final one), nothing moving, nothing shown yet [R] */
static void script_activate(ScriptCam *s, const CamView *from)
{
    memcpy(s->cur, from->pos, sizeof s->cur);
    memcpy(s->goal, from->pos, sizeof s->goal);
    memset(s->vel, 0, sizeof s->vel);
    quat_from_rows(from->rows, s->q);
    memcpy(s->qgoal, s->q, sizeof s->q);
    memset(s->w, 0, sizeof s->w);
    s->fov = s->fov_goal = from->fov;
    s->have_last = 0;
    s->cut = 0;
}

static int object_point(const DirObject *o, int n, int obj, int local, const float off[3], float out[3])
{
    if (obj < 0 || obj >= n || !o)
        return 0;
    const DirObject *b = &o[obj];
    if (local) {                                     /* in its frame: forward is where it faces [G: the matrix's axes] */
        float c = cosf(b->facing), s = sinf(b->facing);
        out[0] = b->pos[0] + c * off[0] - s * off[1];
        out[1] = b->pos[1] + s * off[0] + c * off[1];
        out[2] = b->pos[2] + off[2];
    } else {
        for (int k = 0; k < 3; k++) out[k] = b->pos[k] + off[k];
    }
    return 1;
}

/* 0x13AF00: towards the goal at a speed of k x the distance (k = 2 accel / speed), the change of speed and the speed
 * limited; there when close and slow [R] */
static void follow_pos(ScriptCam *s, float dt)
{
    float d[3] = { s->goal[0] - s->cur[0], s->goal[1] - s->cur[1], s->goal[2] - s->cur[2] };
    float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], vv = s->vel[0] * s->vel[0] + s->vel[1] * s->vel[1] +
              s->vel[2] * s->vel[2];
    if (dd < POS_RADIUS * POS_RADIUS && vv < (POS_ACCEL * dt) * (POS_ACCEL * dt)) {
        memcpy(s->cur, s->goal, sizeof s->cur);
        memset(s->vel, 0, sizeof s->vel);
        return;
    }
    float k = 2 * POS_ACCEL / POS_SPEED, dv[3], l;
    for (int i = 0; i < 3; i++) dv[i] = d[i] * k - s->vel[i];
    l = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
    if (l > POS_ACCEL * dt)
        for (int i = 0; i < 3; i++) dv[i] *= POS_ACCEL * dt / l;
    for (int i = 0; i < 3; i++) s->vel[i] += dv[i];
    l = sqrtf(s->vel[0] * s->vel[0] + s->vel[1] * s->vel[1] + s->vel[2] * s->vel[2]);
    if (l > POS_SPEED)
        for (int i = 0; i < 3; i++) s->vel[i] *= POS_SPEED / l;
    for (int i = 0; i < 3; i++) s->cur[i] += s->vel[i] * dt;
}

/* 0x13A980: the same for the rotation, on the turn from the current one to the goal (its axis and angle) [R; the
 * game's angular velocity is a quaternion, here a rotation vector] */
static void follow_rot(ScriptCam *s, float dt)
{
    float ci[4] = { -s->q[0], -s->q[1], -s->q[2], s->q[3] }, dq[4];
    quat_mul(s->qgoal, ci, dq);                      /* goal = dq cur */
    if (dq[3] < 0)
        for (int i = 0; i < 4; i++) dq[i] = -dq[i];
    float sn = sqrtf(dq[0] * dq[0] + dq[1] * dq[1] + dq[2] * dq[2]), ang = 2 * atan2f(sn, dq[3]);
    if (ang < ROT_RADIUS) {
        memcpy(s->q, s->qgoal, sizeof s->q);
        memset(s->w, 0, sizeof s->w);
        return;
    }
    float want = ang * (2 * ROT_ACCEL / ROT_SPEED), dw[3], l;
    if (want > ROT_SPEED) want = ROT_SPEED;
    for (int i = 0; i < 3; i++) dw[i] = dq[i] / sn * want - s->w[i];
    l = sqrtf(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]);
    if (l > ROT_ACCEL * dt)
        for (int i = 0; i < 3; i++) dw[i] *= ROT_ACCEL * dt / l;
    for (int i = 0; i < 3; i++) s->w[i] += dw[i];
    l = sqrtf(s->w[0] * s->w[0] + s->w[1] * s->w[1] + s->w[2] * s->w[2]);
    if (l > ROT_SPEED)
        for (int i = 0; i < 3; i++) s->w[i] *= ROT_SPEED / l;
    l = sqrtf(s->w[0] * s->w[0] + s->w[1] * s->w[1] + s->w[2] * s->w[2]);
    if (l > 1e-9f) {
        float h = 0.5f * l * dt, sh = sinf(h) / l, step[4] = { s->w[0] * sh, s->w[1] * sh, s->w[2] * sh, cosf(h) };
        quat_mul(step, s->q, s->q);
        quat_norm(s->q);
    }
}

static void follow_fov(ScriptCam *s, float dt)       /* 0x13B260: at a fixed rate [R] */
{
    float d = s->fov_goal - s->fov;
    if (fabsf(d) < 0.001f || fabsf(d) <= FOV_RATE * dt)
        s->fov = s->fov_goal;
    else
        s->fov += d > 0 ? FOV_RATE * dt : -FOV_RATE * dt;
}

/* 0x311820: the place's goal, the look's goal, the followers (snapped when forced or without inertia), then what it
 * shows smoothed by 0.95 a frame (taken as it is after a snap) [R] */
static void script_update(ScriptCam *s, int force, const DirObject *o, int n, float dt, float hz)
{
    float p[3];
    if (s->loc_obj < 0)
        memcpy(s->goal, s->loc_off, sizeof s->goal);
    else if (object_point(o, n, s->loc_obj, s->loc_local, s->loc_off, p))
        memcpy(s->goal, p, sizeof s->goal);          /* an object gone: the goal stays */
    if (force || !s->pos_inertia) {
        memcpy(s->cur, s->goal, sizeof s->cur);
        memset(s->vel, 0, sizeof s->vel);
    } else {
        follow_pos(s, dt);
    }
    int ok = 1;
    if (s->tgt_obj < 0)
        memcpy(p, s->tgt_off, sizeof p);
    else
        ok = object_point(o, n, s->tgt_obj, s->tgt_local, s->tgt_off, p);
    if (ok) {                                        /* rows: forward to the point, right = forward x z, up */
        float f[3] = { p[0] - s->cur[0], p[1] - s->cur[1], p[2] - s->cur[2] }, l = sqrtf(f[0] * f[0] + f[1] * f[1] +
                                                                                          f[2] * f[2]);
        if (l < 1e-5f) { f[0] = 1; f[1] = f[2] = 0; } else { f[0] /= l; f[1] /= l; f[2] /= l; }
        float rt[3] = { f[1], -f[0], 0 };
        l = sqrtf(rt[0] * rt[0] + rt[1] * rt[1]);
        if (l < 1e-5f) { rt[0] = 1; rt[1] = 0; } else { rt[0] /= l; rt[1] /= l; }
        float m[3][3] = { { rt[0], rt[1], rt[2] }, { f[0], f[1], f[2] },
                          { rt[1] * f[2] - rt[2] * f[1], rt[2] * f[0] - rt[0] * f[2], rt[0] * f[1] - rt[1] * f[0] } };
        quat_from_rows(m, s->qgoal);
    }
    if (force || !s->rot_inertia) {
        memcpy(s->q, s->qgoal, sizeof s->q);
        memset(s->w, 0, sizeof s->w);
        s->fov = s->fov_goal;
        s->have_last = 0;
    } else {
        follow_rot(s, dt);
        follow_fov(s, dt);
    }
    if (s->have_last) {
        float q[4], k = frame_keep(s->smoothing, dt, hz);
        memcpy(q, s->q, sizeof q);
        if (qdot(q, s->last_q) < 0)
            for (int i = 0; i < 4; i++) q[i] = -q[i];
        for (int i = 0; i < 4; i++) s->last_q[i] = q[i] + k * (s->last_q[i] - q[i]);
        quat_norm(s->last_q);
        for (int i = 0; i < 3; i++) s->last_pos[i] = s->cur[i] + k * (s->last_pos[i] - s->cur[i]);
    } else {
        s->have_last = 1;
        memcpy(s->last_q, s->q, sizeof s->q);
        memcpy(s->last_pos, s->cur, sizeof s->cur);
    }
}

/* ================================================================ the camera manager and its controller */

static const char *const MODE_NAMES[DCM_COUNT] = { "TwoTilts", "Free", "Follow", "Script" };

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int same(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) a++, b++;
    return lower((unsigned char)*a) == lower((unsigned char)*b);
}

static int mode_of(const char *name)                 /* 0x3114C0: "default" is the first, TwoTilts [R] */
{
    if (!name)
        return DCM_NONE;
    if (same(name, "default"))
        return DCM_TWOTILTS;
    for (int m = 0; m < DCM_COUNT; m++)
        if (same(name, MODE_NAMES[m]))
            return m;
    return DCM_NONE;
}

static void tt_view(const CamDirector *d, CamView *v)   /* TwoTilts' output: its smoothed camera and FOV */
{
    memcpy(v->pos, d->tt->pos, sizeof v->pos);
    memcpy(v->rows, d->tt->rot, sizeof v->rows);
    v->fov = d->tt->s.fov;
}

void dircam_init(CamDirector *d, GameCamera *tt)
{
    memset(d, 0, sizeof *d);
    d->tt = tt;
    d->cur = d->prev = DCM_TWOTILTS;
    d->old = DCM_NONE;
    d->frame_hz = 60;
    script_load(&d->script);
    for (int i = 0; i < 3; i++)
        d->out.rows[i][i] = 1;
    d->out.fov = tt ? tt->s.fov : 0.5f;
    if (tt) {
        cam_activate(tt);
        tt_view(d, &d->out);
    }
}

static void activate(CamDirector *d, int m)          /* the mode's +0xC */
{
    if (m == DCM_TWOTILTS && d->tt)
        cam_activate(d->tt);
    else if (m == DCM_SCRIPT)
        script_activate(&d->script, &d->out);
}

/* CamController::setMode (0x167400): the mode the blend comes from is not taken again; the new one is (re)activated;
 * a blend of 0.001 s or less cuts, a longer one takes the old mode (its flags, 1 for every mode here: its place stays
 * live) and the camera shown now [R] */
static void set_mode(CamDirector *d, int m, float blend)
{
    if (m == d->old)
        return;
    activate(d, m);
    if (d->cur != DCM_NONE) {
        if (blend <= 0.001f) {
            d->old = DCM_NONE;
        } else {
            d->flags = 1;                            /* the mode's +0x1C: 0x167660 returns 1 [R TwoTilts, Script] */
            d->snap = d->out;
            d->old = d->cur;
            d->timer = blend;
            d->inv = 1 / blend;
        }
    }
    d->cur = m;
}

int dircam_set_mode(CamDirector *d, const char *name, float blend)
{
    int m = mode_of(name);                           /* 0x27EDF0: the mode before is kept for setPrevCamMode [R] */
    if (m == DCM_NONE)
        return -1;
    d->prev = d->cur;
    if (m != d->prev)
        set_mode(d, m, blend);
    return 0;
}

void dircam_set_prev_mode(CamDirector *d, float blend)
{
    if (d->cur != d->prev && d->prev != DCM_NONE)     /* 0x27EDB0 [R] */
        set_mode(d, d->prev, blend);
}

const char *dircam_mode(const CamDirector *d)
{
    return d->cur >= 0 && d->cur < DCM_COUNT ? MODE_NAMES[d->cur] : "";
}

void dircam_script_target(CamDirector *d, int obj, int local, const float off[3])   /* 0x3123B0 [R] */
{
    d->script.tgt_obj = obj;
    d->script.tgt_local = local;
    memcpy(d->script.tgt_off, off, sizeof d->script.tgt_off);
}

void dircam_script_location(CamDirector *d, int obj, int local, const float off[3], int cut)   /* 0x312370 [R] */
{
    d->script.loc_obj = obj;
    d->script.loc_local = local;
    memcpy(d->script.loc_off, off, sizeof d->script.loc_off);
    if (cut)
        d->script.cut = 1;
}

void dircam_script_cut(CamDirector *d)               /* 0x312300: 0x311820(force) at once [R]; here at the next update,
                                                      * with that frame's objects */
{
    d->script.cut = 1;
}

void dircam_game_event(CamDirector *d, const char *name, float blend, const float pos[3], const float v[3], int cut)
{
    dircam_set_mode(d, name, blend);
    if (mode_of(name) == DCM_SCRIPT && pos && v) {
        dircam_script_location(d, -1, 0, pos, 0);
        dircam_script_target(d, -1, 0, v);
        if (cut)
            dircam_script_cut(d);
    }
}

void dircam_rage(CamDirector *d, int obj)            /* Monsters::startRage [R Monsters.scr] */
{
    static const float TARGET[3] = { 0, 0, 8 }, LOCATION[3] = { 0, -50, 15 };
    if (d->cur != DCM_SCRIPT)
        dircam_set_mode(d, "Script", 0.25f);
    dircam_script_target(d, obj, 0, TARGET);
    dircam_script_location(d, obj, 0, LOCATION, 0);
    dircam_script_cut(d);
    d->rage_time = 1.5f;                             /* installTimer("Monsters::restoreCam", 1.5) */
}

static void mode_view(CamDirector *d, int m, const DirObject *o, int n, float dt, CamView *v)   /* the mode's +0x14 */
{
    if (m == DCM_TWOTILTS && d->tt) {
        tt_view(d, v);
    } else if (m == DCM_SCRIPT) {
        ScriptCam *s = &d->script;
        if (s->cut) {                                /* the cut, then the frame's own update [R 0x312300, 0x311750] */
            s->cut = 0;
            script_update(s, 1, o, n, 0, d->frame_hz);
        }
        script_update(s, 0, o, n, dt, d->frame_hz);
        memcpy(v->pos, s->last_pos, sizeof v->pos);
        rows_from_quat(s->last_q, v->rows);
        v->fov = s->fov;
    } else {
        *v = d->out;                                 /* Free, Follow: the camera stays [G: not used by the scripts] */
    }
}

/* 0x167690: the mode's camera; while blending, from the old one: the rotation a slerp from the camera when the blend
 * began, the place a lerp from the old mode's live place (flag 1), the fov a lerp, by the share of the blend left [R] */
void dircam_update(CamDirector *d, float dt, const DirObject *objs, int nobjs, CamView *out)
{
    if (d->rage_time > 0 && (d->rage_time -= dt) <= 0) {   /* Monsters::restoreCam */
        d->rage_time = 0;
        dircam_set_prev_mode(d, 0.5f);
    }
    CamView nv;
    mode_view(d, d->cur, objs, nobjs, dt, &nv);
    if (d->old != DCM_NONE) {
        d->timer -= dt;
        if (d->timer <= 0.001f) {
            d->old = DCM_NONE;
            d->out = nv;
        } else {
            float t = d->timer * d->inv, qn[4], qo[4], q[4];
            CamView ov = d->snap;
            if (d->flags & 1) {                      /* the old mode keeps running; its place is used */
                CamView live;
                mode_view(d, d->old, objs, nobjs, dt, &live);
                memcpy(ov.pos, live.pos, sizeof ov.pos);
            }
            quat_from_rows(nv.rows, qn);
            quat_from_rows(ov.rows, qo);
            if (qdot(qn, qo) < 0)
                for (int i = 0; i < 4; i++) qo[i] = -qo[i];
            slerp(qn, qo, t, q);
            rows_from_quat(q, d->out.rows);
            for (int i = 0; i < 3; i++) d->out.pos[i] = nv.pos[i] + t * (ov.pos[i] - nv.pos[i]);
            d->out.fov = nv.fov + t * (ov.fov - nv.fov);
        }
    } else {
        d->out = nv;
    }
    if (out)
        *out = d->out;
}
