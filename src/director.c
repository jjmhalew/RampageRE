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
#include "wrand.h"

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
static int node_action(const DirGraph *ag, int state)   /* the state's node's action (+0x3C) */
{
    return ag->action && state >= 0 && state < ag->n ? ag->action[state] : -1;
}
/* pickByAction 0x2F2630 (pad, action, again, again) [R]: the floors (pad +0x8 for a transition that needs nothing,
 * +0xC for one of the action) start again (-1) only with no pick pending, so they hold while one waits for its
 * switch window; with again 0 no pick by the action while the state or the pending pick already has it (0x2F26B0 ..
 * 0x2F26EC). Each transition in its input window at the floor or above: one that needs nothing raises +0x8 and is
 * the pick, the scan going on (0x2F2784), so a later one may take its place; one leading to a state of the action
 * at +0x8 or above, usable, its target allowed and not where the pending pick leads raises +0xC and is the pick.
 * None: the state's own transition for the action, in its window (0x2F28B8) */
static void pick_by_action(MoveRunner *r, const DirGraph *ag, int action, int again, MoveCall call, void *ctx)
{
    const MoveGraph *g = r->g;
    const MoveState *s = &g->states[r->state];
    int want = action != DA_KEEP;
    const MoveTrans *pend = r->chosen >= 0 && r->chosen < s->ntrans ? &s->trans[r->chosen] : NULL;
    if (!pend)
        r->floor_any = r->floor_act = -1;
    else if (!again && node_action(ag, pend->target) == action)
        want = 0;
    if (!again && node_action(ag, r->state) == action)
        want = 0;
    int thr = r->floor_any > r->floor_act ? r->floor_any : r->floor_act, pick = -1;
    for (int i = 0; i < s->ntrans; i++) {
        const MoveTrans *t = &s->trans[i];
        if (t->priority < thr || t->target < 0 || t->target >= g->n)
            continue;
        if (r->in_window[i] && usable(t, call, ctx) == 0) {
            r->floor_any = thr = t->priority;
            pick = i;
            continue;
        }
        if (!want || node_action(ag, t->target) != action || t->priority < r->floor_any || !r->in_window[i] ||
            usable(t, call, ctx) == 2 || !target_ok(g, t) || (pend && pend->target == t->target))
            continue;
        r->floor_act = thr = t->priority;
        pick = i;
    }
    if (pick < 0 && want && action >= 0 && action < 16 && ag->fallback) {
        int f = ag->fallback[r->state][action];
        if (f >= 0 && f < s->ntrans && r->in_window[f] && usable(&s->trans[f], call, ctx) != 2 &&
            target_ok(g, &s->trans[f]))
            pick = f;
    }
    if (pick >= 0)
        r->chosen = pick;
}

static void ai_pick(MoveRunner *r, const DirGraph *ag, int action, int to_state, MoveCall call, void *ctx)
{
    if (to_state < 0) {                              /* by action: pickByAction (pad, action, 0, 0) */
        pick_by_action(r, ag, action, 0, call, ctx);
        return;
    }
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
    if (g->state < 0 || g->state >= r->g->n) {      /* no such state: over at once [G: the game installs the NULL
                                                      * state node (0x2C0E18) and reads through it, which is undefined
                                                      * and cannot be ported; climbing it is stuck forcing slot 34
                                                      * (FLOW_NOTES.md)] */
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
    if (g->nwp <= 0) {                               /* no way point: over (a guard: walkTo always puts its point in,
                                                      * 0x2F20E0, and the game reads the current one unchecked,
                                                      * 0x2F1F4C [R]) */
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

/* ================================================================ the bots: DestroyBuildings (goal type 2) */

/* What the bot does, each frame, as 0x2F1A60 [R, docs/MULTIPLAYER_NOTES.md]: dead, it heads for the respawn; with a
 * building it can still take (validate 0x2F15F0, else pickBuilding 0x2F0F60: the nearest standing one of the front
 * row) it walks there, mounts it on a side (approachAndMount 0x2EFA90), punches the cell in front, climbs and hops to
 * the next intact cell (moveToCell 0x2EF5D0), and on a roof punches down or butt-slams it (onRoof 0x2F0140); a
 * 1-storey building it punches from the street. With no building left it goes for the nearest other monster
 * (huntMonster 0x2F1730). It does not eat, nor seek out the army. Its own constants: none (no difficulty). */

#define BOT_STUCK 60.0f                              /* +0x70 > 60: collapseBuilding(target) [R] */
#define BOT_COOLDOWN 0.2f                            /* +0x64 (0x2EEBD0) [R] */
#define BOT_AVOID_TIME 3.0f                          /* +0x6C (0x2EEBE0) [R] */

/* rand01 [R]: the lagged Fibonacci 0x106950 on the world's one state (gWorld +0x82C, wrand.c, shared with the
 * edibles and the smashes), as a float v x 2^-32 (0x2F113C); seeded (0x1069C0) at the world's reset */
void dir_ai_seed(uint32_t seed) { wrand_seed_lf(seed); }

static float bot_rand(DirBot *b)
{
    (void)b;
    return (float)wrand_lf() * 2.3283064e-10f;
}

static int bot_standing(const DirBuilding *b) { return b->state >= 1 && b->state <= 3; }

static int name_is(const char *a, const char *b)     /* the same name, in any case */
{
    for (; *a && *b; a++, b++)
        if ((*a | 32) != (*b | 32))
            return 0;
    return *a == *b;
}

/* the state of that name in the graph, -1 none */
static int state_named(const MoveGraph *g, const char *name)
{
    for (int i = 0; i < g->n; i++)
        if (g->states[i].name && name_is(g->states[i].name, name))
            return i;
    return -1;
}

int dir_ai_destroy(DirAI *ai, const MoveGraph *g, uint32_t seed)
{
    DirGoal *gl = add_goal(ai, DG_DESTROY, 0);
    if (!gl)
        return -1;
    DirBot *b = &ai->bot;
    memset(b, 0, sizeof *b);
    b->target = b->avoid = -1;
    b->side = 3;
    b->seed = seed ? seed : 1;
    b->st_front = state_named(g, "Ground_Transition_Front");   /* 0x2F1C10 [R] */
    b->st_side_l = state_named(g, "Ground_Transition_Side_L");
    b->st_side_r = state_named(g, "Ground_Transition_Side_R");
    b->st_respawn = state_named(g, "Faceup_Idle_Respawn");
    b->st_punch_down = state_named(g, "Ground_Punch_Down");
    return (int)(gl - ai->q);
}

void dir_ai_on_hit(DirAI *ai, const float off[3], int mirror)   /* +0x28 0x2EED40 [R] */
{
    memcpy(ai->bot.hit_off, off, sizeof ai->bot.hit_off);
    if (mirror)                                      /* 0x2EEDAC: x negated while mirrored (+0xAA0) */
        ai->bot.hit_off[0] = -off[0];
    ai->bot.use_hit = 1;
}

void dir_ai_cooldown(DirAI *ai)                      /* +0x38 0x2EEBD0 [R] */
{
    for (int i = 0; i < ai->n; i++)
        if (ai->q[i].type == DG_DESTROY)
            ai->bot.cooldown = BOT_COOLDOWN;
}

void dir_ai_on_mount(DirAI *ai, int b, int rail, int width)   /* +0x34 0x2EEC40 [R] */
{
    ai->bot.target = b;
    ai->bot.side = rail == 0 ? 0 : rail == width ? 1 : 2;
}

/* the point in front of a side's middle the bot walks to (sidePoint 0x2EEF30) [R]: off = its half width r, or 0
 * when the bot is past that side by half of r already (0x2EEF64 .. 0x2EEFD4): the left side with x + r/2 at or left
 * of the box's left (+0x1DC), the right with x - r/2 at or right of its right (+0x1E8), the front with x within
 * them (x + r/2 past the left and x - r/2 short of the right); z 0 */
static void side_point(const DirBotView *v, const DirBuilding *b, int side, float out[3])
{
    float hw = v->half_width, x = v->pos[0];
    int left = x + 0.5f * hw <= b->min[0], right = !left && x - 0.5f * hw >= b->max[0];
    out[2] = 0;
    if (side == 0) {
        out[0] = b->min[0] - (left ? 0 : hw);
        out[1] = b->pos[1];
    } else if (side == 1) {
        out[0] = b->max[0] + (right ? 0 : hw);
        out[1] = b->pos[1];
    } else {
        out[0] = b->pos[0];
        out[1] = b->min[1] - (!left && !right ? 0 : hw);
    }
}

/* the place on a side it mounts at (mountPoint 0x2EEDE0) [R]: the front-left corner, the front-right one, or the
 * front's rail nearest the bot (1 .. width - 1; under 2 wide, min x - 5), at the street (z 0) */
static void mount_point(const DirBotView *v, const DirBuilding *b, int side, float out[3])
{
    out[1] = b->min[1];
    out[2] = 0;
    if (side == 0)
        out[0] = b->min[0];
    else if (side == 1)
        out[0] = b->max[0];
    else if (b->width < 2)
        out[0] = b->min[0] - 5;
    else {
        int rail = (int)floorf((v->pos[0] - b->min[0]) / 5 + 0.5f);
        rail = rail < 1 ? 1 : rail > b->width - 1 ? b->width - 1 : rail;
        out[0] = b->min[0] + 5 * (float)rail;
    }
}

/* approachClear 0x2F1290: a box the bot's size, pushed out from the side, meets no other standing building [R] */
static int approach_clear(const DirBotView *v, int bi, int side, const float pt[3])
{
    float hw = v->half_width, c[2] = { pt[0], pt[1] };
    if (side == 0) c[0] -= 2 * hw;
    else if (side == 1) c[0] += 2 * hw;
    else c[1] -= 2 * hw;
    for (int i = 0; i < v->nb; i++) {
        const DirBuilding *o = &v->b[i];
        if (i == bi || !bot_standing(o))
            continue;
        if (c[0] + hw > o->min[0] && c[0] - hw < o->max[0] && c[1] + hw > o->min[1] && c[1] - hw < o->max[1])
            return 0;
    }
    return 1;
}

static int place_fine(const DirBotView *v, int bi, const float pt[3])
{
    int r = v->place ? v->place(v->ctx, bi, pt) : 1;
    return r == 1 || r == -2 || r == -9;             /* (-9, the camera's reach, is not asked of an AI [R]) */
}

/* pickSide 0x2EF090: of the left, right and front, a side whose mount point is fine and clear and that no other
 * monster on the building is on, the nearest; 3 none [R] */
static int pick_side(const DirBotView *v, int bi)
{
    const DirBuilding *b = &v->b[bi];
    int best = 3;
    float bd = 1e30f;
    for (int side = 0; side < 3; side++) {
        float pt[3];
        mount_point(v, b, side, pt);
        if (!place_fine(v, bi, pt) || !approach_clear(v, bi, side, pt))
            continue;
        int taken = 0;
        for (int k = 0; k < v->nm; k++)
            if (k != v->self && v->m[k].mounted == bi) {
                int r = v->m[k].rail, s = r == 0 ? 0 : r == b->width ? 1 : 2;
                taken |= s == side;
            }
        if (taken)
            continue;
        float d = 0;
        for (int a = 0; a < 3; a++)
            d += (pt[a] - v->pos[a]) * (pt[a] - v->pos[a]);
        if (d < bd) {
            bd = d;
            best = side;
        }
    }
    return best;
}

/* pickBuilding 0x2F0F60: the nearest standing building of the frontmost row with a side to take; one another monster
 * climbs, or the one it was knocked off (half the time), only when there is no other [R] */
static void pick_building(DirBot *bt, const DirBotView *v)
{
    bt->target = -1;
    bt->side = 3;
    float best = 1e30f;
    int best_row = 1 << 30;
    for (int i = 0; i < v->nb; i++) {
        const DirBuilding *b = &v->b[i];
        if (!bot_standing(b))
            continue;
        if (b->row < best_row)
            best = 1e30f;
        else if (b->row > best_row)
            continue;
        float d = 0;
        for (int a = 0; a < 3; a++)
            d += (v->pos[a] - b->pos[a]) * (v->pos[a] - b->pos[a]);
        if (d >= best)
            continue;
        int s = pick_side(v, i);
        if (s == 3)
            continue;
        int climbed = 0;
        for (int k = 0; k < v->nm; k++)
            climbed |= k != v->self && v->m[k].mounted == i;
        int skip = 0;                                /* [R 0x2F10F0 .. 0x2F1140] the skip is taken after the
                                                      * avoided building's draw, which is made for it anyway */
        if (climbed) {
            if (bt->target >= 0)
                skip = 1;
            d = 1e6f;
        }
        if (i == bt->avoid && bot_rand(bt) <= 0.5f) {
            if (bt->target >= 0)
                skip = 1;
            d = 1e6f;
        }
        if (skip)
            continue;
        best = d;
        bt->target = i;
        bt->side = s;
        bt->on_target = 0;
        side_point(v, b, s, bt->walk);
        best_row = b->row;
    }
}

/* validate 0x2F15F0 [R] */
static int bot_validate(const DirBot *bt, const DirBotView *v)
{
    if (bt->target < 0 || bt->target >= v->nb || !bot_standing(&v->b[bt->target]))
        return 0;
    if (v->mounted >= 0)
        return 1;
    if (bt->side == 3)
        return 0;
    float pt[3];
    mount_point(v, &v->b[bt->target], bt->side, pt);
    if (!place_fine(v, bt->target, pt) || !approach_clear(v, bt->target, bt->side, pt))
        return 0;
    int cand = -1, cm = v->can_mount ? v->can_mount(v->ctx, bt->side, &cand) : 1;
    return cm != -5 && cm != -6;
}

static void bot_action(MoveRunner *r, const DirGraph *ag, int action, int again, MoveCall call, void *ctx);

/* 0x2F2970 by state: the next state on the way there, else action 15 [R] */
static void bot_go_state(MoveRunner *r, const DirGraph *ag, int state, MoveCall call, void *ctx)
{
    if (state >= 0 && ag->hop && r->state < ag->n && state < ag->n && ag->hop[r->state * ag->n + state] != 0xFF)
        ai_pick(r, ag, -1, ag->hop[r->state * ag->n + state], call, ctx);
    else
        ai_pick(r, ag, DA_KEEP, -1, call, ctx);
}

/* walk 0x2EDB40 (pt, mode: 0 walk, 1 jump towards it; bld: the building it is after, -1 none): the stick towards
 * the point, kept off other buildings and other monsters, round bld's front corner, and the action [R] */
static void bot_walk(DirAI *ai, const DirGraph *ag, MoveRunner *r, const DirBotView *v, const float pt[3], int mode,
                     int bld, MoveCall call, void *ctx)
{
    float dx = pt[0] - v->pos[0], dy = pt[1] - v->pos[1], l2 = dx * dx + dy * dy;
    float st[2] = { 1, 0 };
    if (l2 > 1e-6f) {
        float l = 1 / sqrtf(l2);
        st[0] = dx * l;
        st[1] = dy * l;
    }
    if (mode == 1) {
        ai->stick[0] = st[0];
        ai->stick[1] = st[1];
        bot_action(r, ag, DA_JUMP, 0, call, ctx);
        return;
    }
    if (mode == 0 && v->ni) {                        /* [R 0x2EDC94] while an NI runs: straight at it, a walk */
        ai->stick[0] = st[0];
        ai->stick[1] = st[1];
        ai_pick(r, ag, DA_WALK, -1, call, ctx);
        return;
    }
    if (bld >= 0) {
        /* [R 0x2ED990, IsBuilding] the ray from the bot along the flat way to the point, without end: when the first
         * building it meets is not bld, out into the street, or along it when it is that far out already, and a walk
         * at once (0x2EE378: no monster checks) */
        float l2 = dx * dx + dy * dy, rx = 1, ry = 0;
        if (l2 >= 1e-6f) {
            rx = dx / sqrtf(l2);
            ry = dy / sqrtf(l2);
        }
        int first = -1;
        float tbest = 1e30f;
        for (int i = 0; i < v->nb; i++) {            /* the buildings' boxes, x and y (a start inside meets at 0) */
            const DirBuilding *b = &v->b[i];
            if (b->state == 0)
                continue;
            float t0 = 0, t1 = 1e30f, o[2] = { v->pos[0], v->pos[1] }, d[2] = { rx, ry };
            int miss = 0;
            for (int a = 0; a < 2 && !miss; a++) {
                if (fabsf(d[a]) < 1e-12f) {
                    miss = o[a] < b->min[a] || o[a] > b->max[a];
                    continue;
                }
                float ta = (b->min[a] - o[a]) / d[a], tb = (b->max[a] - o[a]) / d[a];
                if (ta > tb) { float x = ta; ta = tb; tb = x; }
                if (ta > t0) t0 = ta;
                if (tb < t1) t1 = tb;
                miss = t0 > t1;
            }
            if (!miss && t0 < tbest) {
                tbest = t0;
                first = i;
            }
        }
        if (first >= 0 && first != bld) {
            if (v->pos[1] < -5) {
                st[1] = 0;
                float l = fabsf(st[0]);
                st[0] = l > 1e-6f ? st[0] / l : 1;
            } else {
                st[0] = 0;
                st[1] = -1;
            }
            ai->stick[0] = st[0];
            ai->stick[1] = st[1];
            if (mode == 0)
                ai_pick(r, ag, DA_WALK, -1, call, ctx);
            return;
        }
        /* the other monsters (not human): punch one standing on its spot, step past one in the way in depth */
        float rme = v->half_width;
        for (int k = 0; k < v->nm; k++) {
            const DirMonster *o = &v->m[k];
            if (k == v->self || o->human)
                continue;
            float px = pt[0] - o->pos[0], py = pt[1] - o->pos[1];
            float ex = v->pos[0] - pt[0], ey = v->pos[1] - pt[1];
            float ro = o->radius;
            if (px * px + py * py < 2.25f * rme * rme && ex * ex + ey * ey < 2.25f * (rme + ro) * (rme + ro)) {
                ai->stick[0] = st[0];
                ai->stick[1] = st[1];
                bot_action(r, ag, DA_ATTACK, 1, call, ctx);
                return;
            }
            /* [R 0x2EDF1C .. 0x2EDFCC] in the way: the two reference points (0x2E4E90) 0.75 apart within the
             * radii (0x102DA0) */
            const float *me = v->m[v->self].ref;
            float mx = o->ref[0] - me[0], my = o->ref[1] - me[1];
            if (0.75f * sqrtf(mx * mx + my * my) < rme + ro) {
                if (!o->ai || v->pos[1] < o->pos[1] || o->mounted >= 0)
                    st[1] -= 1;
                else
                    st[1] += 1;
                float l = sqrtf(st[0] * st[0] + st[1] * st[1]);
                if (l > 1e-6f) {
                    st[0] /= l;
                    st[1] /= l;
                }
            }
        }
        /* [R 0x2EE150 .. 0x2EE374] round bld's front corner: with the point at or before its front (0x2EE160:
         * pt.y <= the front, +0x1E0) and the bot
         * reaching past that line (y + R > the point's y), D = R + sqrt(2) A (R the half width, A the probe); going
         * right with its left side D or more to the right, or left with its right side D or more to the left, the
         * stick less (0, 1), made 1 long ((1, 0) when nothing is left) */
        const DirBuilding *b = bld < v->nb ? &v->b[bld] : NULL;
        if (b && pt[1] <= b->min[1] && v->pos[1] + v->half_width > pt[1]) {
            float D = v->half_width + sqrtf(2 * v->probe * v->probe);
            if (dx > 0 ? b->min[0] - v->pos[0] >= D : b->max[0] - v->pos[0] <= -D) {
                st[1] -= 1;
                float l = sqrtf(st[0] * st[0] + st[1] * st[1]);
                if (l * l < 1e-6f) {
                    st[0] = 1;
                    st[1] = 0;
                } else {
                    st[0] /= l;
                    st[1] /= l;
                }
            }
        }
    }
    ai->stick[0] = st[0];
    ai->stick[1] = st[1];
    if (mode == 0)
        ai_pick(r, ag, DA_WALK, -1, call, ctx);
}

/* pickByAction(pad, action, again, again) 0x2F2630: with again 1 a pick by the action even when the state or the
 * pending pick has it already, so attacks and hops repeat [R] */
static void bot_action(MoveRunner *r, const DirGraph *ag, int action, int again, MoveCall call, void *ctx)
{
    pick_by_action(r, ag, action, again, call, ctx);
}

static int bot_facing(const DirBotView *v, float ax, float ay) { return v->fwd[0] * ax + v->fwd[1] * ay >= 0.95f; }
static int bot_in_air(const DirBotView *v) { return v->mode == 1 || v->mode == 2; }

/* the state of a front cell (storey of the standing ones, column), 3 when there is none (destroyed) */
static int bot_cell(const DirBotView *v, int b, int storey, int col)
{
    int s = v->cell ? v->cell(v->ctx, b, storey, col) : -1;
    return s < 0 ? 3 : s;
}

/* the cell it is at (0x2EEA90) [R]: without a hit the mount's own cell record as it is (+0xC628 face, +0xC62C
 * storey, +0xC630 column, copied to +0x58 .. +0x60 at 0x2EEAB4: the column is the rail, mirrored or not); with one
 * the mount's point on the target's face (0x2BB7A0 -> 0x2CF4B0: on the front x = min x + 5 x the column) at the
 * monster's height, plus the hit's offset, at least 6.02 up, its cell (0x2BB7B0) */
static void bot_update_cell(DirBot *bt, const DirBotView *v)
{
    const DirBuilding *b = &v->b[bt->target];
    if (bt->use_hit) {
        float at = v->mounted >= 0 ? b->min[0] + 5.0f * (float)v->rail : v->pos[0];   /* (off a face: its place) */
        /* 0x2EEB10: the offset's x negated again while mirrored now: a hit to its right stays to its right, so once
         * it mirrors to go the other way the cell is the one on that side [R] */
        float x = at + (v->mirror ? -bt->hit_off[0] : bt->hit_off[0]), z = v->pos[2] + bt->hit_off[2];
        if (z < b->min[2] + 6.02f)
            z = b->min[2] + 6.02f;
        bt->cur_col = (int)floorf((x - b->min[0]) / 5);
        bt->cur_storey = (int)floorf((z - b->min[2] - 6) / 3);
    } else {
        bt->cur_storey = v->storey;
        bt->cur_col = v->rail;
    }
    if (bt->cur_col < 0) bt->cur_col = 0;
    if (bt->cur_col > b->width - 1) bt->cur_col = b->width - 1;
    if (bt->cur_storey < 0) bt->cur_storey = 0;
}

/* the nearest intact cell of this storey from column c that way (distLeft 0x2EF450 / distRight 0x2EF2B0): how many
 * columns away, width + 1 when none or another monster is on that rail [R] */
static int bot_dist(const DirBotView *v, int bi, int storey, int c, int dir)
{
    const DirBuilding *b = &v->b[bi];
    for (int k = c + dir, n = 1; k >= 0 && k < b->width; k += dir, n++) {
        if (bot_cell(v, bi, storey, k) >= 3)
            continue;
        for (int m = 0; m < v->nm; m++)
            if (m != v->self && v->m[m].mounted == bi && (v->m[m].rail == k || v->m[m].rail == k + 1))
                return b->width + 1;
        return n;
    }
    return b->width + 1;
}

/* climb up towards a cell above (canClimb(+1, 0)): 5 mantle, -2 an obstacle (it lets go of this building) [R] */
static void bot_climb_up(DirAI *ai, const DirGraph *ag, MoveRunner *r, const DirBotView *v, MoveCall call, void *ctx)
{
    int c = v->can_climb ? v->can_climb(v->ctx, 1, 0) : 2;
    if (c == 5)
        bot_action(r, ag, DA_MANTLE, 1, call, ctx);
    else if (c == -2) {
        dir_ai_avoid(ai, ai->bot.target, v);
        bot_action(r, ag, DA_JUMP, 0, call, ctx);
    } else
        bot_action(r, ag, DA_CLIMB_UP, 0, call, ctx);
}

/* moveToCell 0x2EF5D0: up, down, or along the face to the target cell; hops go to its right, or its left when
 * mirrored, so the other way it first mirrors [R] */
static void bot_move_to_cell(DirAI *ai, const DirGraph *ag, MoveRunner *r, const DirBotView *v, MoveCall call,
                             void *ctx)
{
    DirBot *bt = &ai->bot;
    if (bt->cur_storey < bt->cell_storey) {
        bot_climb_up(ai, ag, r, v, call, ctx);
        return;
    }
    if (bt->cur_storey > bt->cell_storey) {
        bot_action(r, ag, DA_CLIMB_DOWN, 0, call, ctx);
        return;
    }
    int mirrored = v->mirror || v->mirror_queued;
    if (bt->cell_col < bt->cur_col) {                /* to its left */
        if (!mirrored) {
            bot_action(r, ag, DA_MIRROR, 0, call, ctx);
            return;
        }
        int c = v->can_climb ? v->can_climb(v->ctx, 0, -1) : -7;
        if (c == 3 || c == 4)
            bot_action(r, ag, DA_HOP, 1, call, ctx);
        else if (v->mirror) {
            bt->cell_storey = v->b[bt->target].storeys;
            bot_climb_up(ai, ag, r, v, call, ctx);
        } else
            ai_pick(r, ag, DA_KEEP, -1, call, ctx);
        return;
    }
    if (bt->cell_col > bt->cur_col) {                /* to its right */
        if (mirrored) {
            bot_action(r, ag, DA_MIRROR, 0, call, ctx);
            return;
        }
        int c = v->can_climb ? v->can_climb(v->ctx, 0, 1) : -7;
        if (c == 3 || c == 4)
            bot_action(r, ag, DA_HOP, 1, call, ctx);
        else {
            bt->cell_storey = v->b[bt->target].storeys;
            bot_climb_up(ai, ag, r, v, call, ctx);
        }
        return;
    }
    bot_climb_up(ai, ag, r, v, call, ctx);
}

/* punching from the street (a 1-storey building, or something in the way of the mount): the nearest front column
 * with an intact ground cell (frontPoint 0x2EF8F0) [R] */
static void bot_street_punch(DirAI *ai, const DirGraph *ag, MoveRunner *r, const DirBotView *v, MoveCall call,
                             void *ctx)
{
    DirBot *bt = &ai->bot;
    const DirBuilding *b = &v->b[bt->target];
    bt->side = 2;
    int best = -1;
    float bd = 1e30f;
    /* [R 0x2EF8F0 .. 0x2EFA48] the inner rails c = 1 .. width - 1 whose ground cell (storey 0, column c) is intact,
     * the nearest; with none, or no storeys, sidePoint of the front (0x2EEF30) */
    for (int c = 1; c < b->width && b->storeys > 0; c++)
        if (bot_cell(v, bt->target, 0, c) < 3) {
            float x = b->min[0] + 5 * (float)c, d = fabsf(x - v->pos[0]);
            if (d < bd) {
                bd = d;
                best = c;
            }
        }
    float pt[3] = { best >= 0 ? b->min[0] + 5 * (float)best : b->pos[0], b->min[1] - v->half_width, 0 };
    if (best < 0)
        side_point(v, b, 2, pt);
    memcpy(bt->walk, pt, sizeof pt);
    /* [R 0x2F0E78 .. 0x2F0EB8, 0x2EFFF0 .. 0x2F0030] there when the body centre (0x2E4D30) is within r of it */
    float dx = pt[0] - v->centre[0], dy = pt[1] - v->centre[1];
    if (dx * dx + dy * dy <= v->half_width * v->half_width) {
        if (!bot_facing(v, 0, 1))                    /* turning to the front: the walk to the point (0x2F0EEC) */
            bot_walk(ai, ag, r, v, pt, 0, bt->target, call, ctx);
        else
            bot_action(r, ag, DA_ATTACK, 1, call, ctx);
    } else
        bot_walk(ai, ag, r, v, pt, 0, bt->target, call, ctx);
}

/* approachAndMount 0x2EFA90 [R] */
static void bot_approach(DirAI *ai, const DirGraph *ag, MoveRunner *r, const DirBotView *v, MoveCall call, void *ctx)
{
    DirBot *bt = &ai->bot;
    const DirBuilding *b = &v->b[bt->target];
    bt->side = pick_side(v, bt->target);
    if (bt->side == 3) {
        bt->target = -1;
        ai_pick(r, ag, DA_IDLE, -1, call, ctx);
        return;
    }
    side_point(v, b, bt->side, bt->walk);
    float mp[3];
    mount_point(v, b, bt->side, mp);
    static const int FACE[3] = { 0, 1, 2 };          /* climb.h's FACE_LEFT, FACE_RIGHT, FACE_FRONT */
    int cand = -1, cm = v->can_mount ? v->can_mount(v->ctx, FACE[bt->side], &cand) : -7;
    int place = place_fine(v, bt->target, mp) ? 1 : -5;
    if (cm == 1 && cand == bt->target) {
        int air = bot_in_air(v);
        if (bt->side == 2) {
            if (!bot_facing(v, 0, 1) || air) bot_walk(ai, ag, r, v, mp, 0, bt->target, call, ctx);
            else bot_go_state(r, ag, bt->st_front, call, ctx);
        } else if (bt->side == 0) {
            if (!bot_facing(v, 1, 0) || air) bot_walk(ai, ag, r, v, bt->walk, 0, bt->target, call, ctx);
            else bot_go_state(r, ag, v->mirror ? bt->st_side_r : bt->st_side_l, call, ctx);
        } else {
            if (!bot_facing(v, -1, 0) || air) bot_walk(ai, ag, r, v, bt->walk, 0, bt->target, call, ctx);
            else bot_go_state(r, ag, v->mirror ? bt->st_side_l : bt->st_side_r, call, ctx);
        }
    } else if (place == -5 || cm == -5 || cm == -6) {
        bt->target = -1;
        ai_pick(r, ag, DA_IDLE, -1, call, ctx);
    } else if (cm == -2 || cm == -3) {
        bot_street_punch(ai, ag, r, v, call, ctx);
    } else {
        /* [R 0x2F00B8 ..] anything else: on to the walk point, jumping there while in the air (mode 1 or 2) */
        bot_walk(ai, ag, r, v, bt->walk, bot_in_air(v) ? 1 : 0, bt->target, call, ctx);
    }
}

/* onRoof 0x2F0140 (u the building under it) [R] */
static void bot_on_roof(DirAI *ai, const DirGraph *ag, MoveRunner *r, const DirBotView *v, int u, MoveCall call,
                        void *ctx)
{
    DirBot *bt = &ai->bot;
    const DirBuilding *b = &v->b[u];
    bt->use_hit = bt->have_cell = 0;                 /* [R 0x2F0160 .. 0x2F0164]: +0x38 and +0x48 */
    float c[3] = { b->pos[0], (b->min[1] + b->max[1]) / 2, b->max[2] };
    if (b->state >= 4) {                             /* falling: off it */
        if (bt->target >= 0 && bt->target != u) {
            const DirBuilding *t = &v->b[bt->target];
            float to[3] = { t->pos[0], (t->min[1] + t->max[1]) / 2, t->max[2] };
            float dx = to[0] - v->pos[0], dy = to[1] - v->pos[1], l = sqrtf(dx * dx + dy * dy);
            int toward = l > 1e-6f && (v->fwd[0] * dx + v->fwd[1] * dy) / l >= 0.9f;
            bot_walk(ai, ag, r, v, to, toward ? 1 : 0, toward ? -1 : u, call, ctx);
        } else
            bot_action(r, ag, DA_JUMP, 0, call, ctx);
        return;
    }
    float dx = c[0] - v->pos[0], dy = c[1] - v->pos[1], d2 = dx * dx + dy * dy, hw = v->half_width;
    if (!v->smash) {
        if (d2 <= hw * hw)
            bot_go_state(r, ag, bt->st_punch_down, call, ctx);
        else                                         /* with the roof's building (0x2F0464): its monster checks */
            bot_walk(ai, ag, r, v, c, 0, u, call, ctx);
    } else if (bot_in_air(v)) {
        if (d2 > 2.25f * hw * hw)
            bot_walk(ai, ag, r, v, c, 1, -1, call, ctx);
        else
            bot_action(r, ag, DA_ATTACK, 0, call, ctx);
    } else
        bot_action(r, ag, DA_JUMP, 0, call, ctx);
}

/* withTarget 0x2F07F0 [R] */
static void bot_with_target(DirAI *ai, const DirGraph *ag, MoveRunner *r, const DirBotView *v, float dt, MoveCall call,
                            void *ctx)
{
    DirBot *bt = &ai->bot;
    const DirBuilding *b = &v->b[bt->target];
    /* [R 0x2F0858 .. 0x2F08D4] the timers run here only: the building knocked off (+0x68) is avoided while it is the
     * target and its 3 s (+0x6C) last, forgotten at once for any other; the cooldown (+0x64) runs down to 0 */
    if (bt->avoid >= 0 && (bt->avoid != bt->target || (bt->avoid_time -= dt) <= 0))
        bt->avoid = -1;
    bt->cooldown = bt->cooldown > dt ? bt->cooldown - dt : 0;
    bt->on_target += dt;
    if (bt->on_target > BOT_STUCK && v->collapse) {  /* stuck on it for a minute: it falls */
        v->collapse(v->ctx, bt->target);
        bt->on_target = 0;
    }
    if (v->mounted >= 0) {
        if (b->state >= 4) {
            bot_action(r, ag, DA_JUMP, 0, call, ctx);
            return;
        }
        bot_update_cell(bt, v);
        /* [R 0x2F0928 .. 0x2F0994] the punch: the target cell's storey (+0x50) below the top, the current cell
         * (+0x58) intact */
        if ((bt->cooldown <= 0 || bt->cur_storey == bt->cell_storey) && bt->cell_storey < b->storeys &&
            bot_cell(v, bt->target, bt->cur_storey, bt->cur_col) < 3) {
            bt->have_cell = 0;
            bot_action(r, ag, DA_ATTACK, 1, call, ctx);   /* the cell in front */
            return;
        }
        /* [R 0x2F09A8 .. 0x2F0A14] the target kept while it is above no top and, below the roof, intact; the roof
         * (its storey the building's storeys) is kept: the bot climbs straight to it */
        if (!bt->have_cell || bt->cell_storey > b->storeys ||
            (bt->cell_storey < b->storeys && bot_cell(v, bt->target, bt->cell_storey, bt->cell_col) >= 3)) {
            int s = bt->cur_storey, c = bt->cur_col, found = 0;
            if (!(b->width < 3 && v->rail == 1)) {
                int dl = bot_dist(v, bt->target, s, c, -1), dr = bot_dist(v, bt->target, s, c, 1);
                if (dl < dr && dl <= b->width) {
                    bt->cell_col = c - dl;
                    bt->cell_storey = dl > 1 && s < b->storeys - 1 ? s + 1 : s;
                    found = 1;
                } else if (dr <= b->width && !(dl == dr && dl >= 2)) {
                    bt->cell_col = c + dr;
                    bt->cell_storey = dr > 1 && s < b->storeys - 1 ? s + 1 : s;
                    found = 1;
                }
            }
            for (int up = s + 1; !found && up < b->storeys; up++)   /* the first intact cell above */
                if (bot_cell(v, bt->target, up, c) < 3) {
                    bt->cell_col = c;
                    bt->cell_storey = up;
                    found = 1;
                }
            if (!found) {                            /* none: onto the roof */
                bt->cell_col = c;
                bt->cell_storey = b->storeys;
            }
            bt->have_cell = 1;
        }
        bot_move_to_cell(ai, ag, r, v, call, ctx);
        return;
    }
    if (v->under >= 0 && v->b[v->under].state != 0) {
        bot_on_roof(ai, ag, r, v, v->under, call, ctx);
        return;
    }
    bt->use_hit = bt->have_cell = 0;
    if (b->storeys >= 2 && bt->target != bt->avoid)
        bot_approach(ai, ag, r, v, call, ctx);
    else
        bot_street_punch(ai, ag, r, v, call, ctx);
}

/* huntMonster 0x2F1730: the nearest other monster (its HP not asked: the game tests its own [R]) */
static void bot_hunt(DirAI *ai, const DirGraph *ag, MoveRunner *r, const DirBotView *v, MoveCall call, void *ctx)
{
    int best = -1;
    float bd = 1e30f;
    for (int k = 0; k < v->nm; k++) {
        if (k == v->self)
            continue;
        float dx = v->m[k].pos[0] - v->pos[0], dy = v->m[k].pos[1] - v->pos[1], d = dx * dx + dy * dy;
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    if (best < 0) {
        ai_pick(r, ag, DA_IDLE, -1, call, ctx);
        return;
    }
    const DirMonster *o = &v->m[best];
    float dx = o->pos[0] - v->pos[0], dy = o->pos[1] - v->pos[1], l = sqrtf(bd);
    if (bd >= 225)
        bot_walk(ai, ag, r, v, o->pos, 0, o->mounted, call, ctx);
    else if (o->mounted >= 0 && o->mounted < v->nb && v->pos[1] < v->b[o->mounted].min[1]) {
        /* [R 0x2F1900 .. 0x2F19B4] in front of a climber's building (its y below the front's, +0x1E0), however close:
         * back off 10 towards the street (0x40A018: (0, 1, 0) x -10), the walk with no building */
        float back[3] = { v->pos[0], v->pos[1] - 10, v->pos[2] };
        bot_walk(ai, ag, r, v, back, 0, -1, call, ctx);
    } else if (l > 1e-6f && (v->fwd[0] * dx + v->fwd[1] * dy) / l < 0.95f)
        bot_walk(ai, ag, r, v, o->pos, 0, o->mounted, call, ctx);
    else
        bot_action(r, ag, DA_ATTACK, 1, call, ctx);
}

/* +0x30 0x2EEBE0: knocked off building b (or an obstacle in its way): 3 s of avoiding it, the walk to its front */
void dir_ai_avoid(DirAI *ai, int b, const DirBotView *view)
{
    DirBot *bt = &ai->bot;
    bt->avoid = b;
    bt->avoid_time = BOT_AVOID_TIME;
    bt->side = 2;
    if (view && b >= 0 && b < view->nb)
        side_point(view, &view->b[b], 2, bt->walk);
    bt->target = -1;
    bt->have_cell = bt->use_hit = 0;
}

/* DestroyBuildings::update 0x2F1A60: never over [R] */
static int destroy_update(DirAI *ai, const DirGraph *ag, MoveRunner *r, float dt, MoveCall call, void *ctx)
{
    DirBot *bt = &ai->bot;
    const DirBotView *v = ai->view;
    if (!v)
        return 1;
    if (v->hp <= 0) {                                /* down: towards the respawn */
        bot_go_state(r, ag, bt->st_respawn, call, ctx);
        return 1;
    }
    if (!bot_validate(bt, v)) {
        bt->use_hit = bt->have_cell = 0;
        pick_building(bt, v);
    }
    if (bt->target >= 0)
        (v->mounted < 0 || v->mounted == bt->target) ? bot_with_target(ai, ag, r, v, dt, call, ctx)
                                                     : bot_action(r, ag, DA_JUMP, 0, call, ctx);
    else if (v->mounted >= 0)
        bot_action(r, ag, DA_JUMP, 0, call, ctx);
    else
        bot_hunt(ai, ag, r, v, call, ctx);
    return 1;
}

/* dt: the clip's time (the speed scale's, 0x2EBFE0); world_dt: the world's step, which the goals' clocks count (gWorld
 * +4: 0x2F0838, 0x2F087C, 0x2F08BC) [R] */
int dir_ai_step(DirAI *ai, const DirGraph *ag, MoveRunner *r, const float pos[3], float dt, float world_dt, MoveCall call,
                void *ctx)
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
                : gl->type == DG_DESTROY ? destroy_update(ai, ag, r, world_dt, call, ctx)
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
 * (pi / 2, pi / 2, 0.001) and pi / 2 (camera.prb's Script group is empty), each made per step with RWorld +0x4
 * (0x312238 ..: the world step, 1 / 30 s, 1 / 25 at 50 Hz, 0x2B7898): the speed x h, the acceleration x h^2, the
 * gain 2 accel / speed x h (0x13B1B0); here per second, the same at the game's fixed step [R] */
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
    if (local) {                                     /* in its frame [R 0x311884 ..]: off x its matrix's rows
                                                      * (+0x8 x, +0x14 y, +0x20 z) + its place (+0x2C); a monster's
                                                      * x row (cos h, sin h), y (-sin h, cos h), unmirrored */
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
    d->frame_hz = 30;                                /* the world's step: gWorld +8 = 30 (RWorld 0x2B7800) [R] */
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
    else if (m == DCM_FOLLOW) {                      /* 0x3124E0: the shown camera's turn, place and fov; the place */
        d->follow = d->out;                          /* less player 1's monster's (+0x2C), else 0x409FF8 (0, 0, 0) [R] */
        for (int k = 0; k < 3; k++)
            d->follow_off[k] = d->have_p1 ? d->out.pos[k] - d->p1[k] : 0;
    }
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
    if (name)                                        /* (no name: the script camera moved, the mode kept) */
        dircam_set_mode(d, name, blend);
    if ((!name || mode_of(name) == DCM_SCRIPT) && pos && v) {
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
    if (d->nrage < 8)                                /* installTimer("Monsters::restoreCam", 1.5): one a rage, so a */
        d->rage_time[d->nrage++] = 1.5f;             /* rage soon after another loses its shot to the first's */
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
    } else if (m == DCM_FOLLOW) {                    /* 0x3123F0: player 1's monster + the offset, the turn and fov kept;
                                                      * without a monster the camera stays [R] (no script uses it) */
        *v = d->follow;
        if (n > 0)
            for (int k = 0; k < 3; k++)
                d->follow.pos[k] = v->pos[k] = o[0].pos[k] + d->follow_off[k];
    } else {
        *v = d->out;                                 /* Free: a debug fly camera on a pad, held without input [R] */
    }
}

/* 0x167690: the mode's camera; while blending, from the old one: the rotation a slerp from the camera when the blend
 * began, the place a lerp from the old mode's live place (flag 1), the fov a lerp, by the share of the blend left [R] */
void dircam_update(CamDirector *d, float dt, const DirObject *objs, int nobjs, CamView *out)
{
    for (int i = 0; i < d->nrage; i++)               /* Monsters::restoreCam */
        if ((d->rage_time[i] -= dt) <= 0) {
            d->rage_time[i--] = d->rage_time[--d->nrage];
            dircam_set_prev_mode(d, 0.5f);
        }
    if (nobjs > 0) {
        memcpy(d->p1, objs[0].pos, sizeof d->p1);
        d->have_p1 = 1;
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

/* the world moved by dy along y under the cameras (the game's next block: the scene rebuilt 41 back): what they keep
 * as world places moves with it - the camera shown, the blend's, Follow's, and the script camera's followers and the
 * world points it was given (ResultsNI's, set in the new block's frame: activateNextBlock 0x325924 comes before state
 * 7's NI 0x324500 [R]); an offset from an object stays, the object carries it */
void dircam_shift_y(CamDirector *d, float dy)
{
    ScriptCam *s = &d->script;
    d->out.pos[1] += dy;
    d->snap.pos[1] += dy;
    d->follow.pos[1] += dy;
    d->p1[1] += dy;
    s->goal[1] += dy;
    s->cur[1] += dy;
    s->last_pos[1] += dy;
    if (s->loc_obj < 0)
        s->loc_off[1] += dy;
    if (s->tgt_obj < 0)
        s->tgt_off[1] += dy;
}
