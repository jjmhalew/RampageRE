/* moves.c - MONSTER_DATA's move graph and the game's runner for it (moves.h; docs/FORMAT_MONSTER.md).
 * The graph (offsets from the start of the entry): +0xA54 its header, +0x2C state count, +0x30 the first state.
 * A state: +0x4 its node (+0x78 clip name, +0x7C state name), +0x8 f32 length, +0xE i16 transitions, +0x14 the state
 * after the clip, then 0x74 bytes per transition: +0x4 target, +0x8 flags, +0xC / +0x10 input window, +0x14 / +0x18
 * switch window, +0x1C i16 priority, +0x1E i16 conditions, +0x20 4 x {op, variable, value, flags}, +0x60 check. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "moves.h"

static const char *name_at(const Bundle *b, int32_t i)
{
    if (i < 0 || (uint32_t)i >= b->nnames)
        return NULL;
    const char *n = bdp_name(b, (uint32_t)i);
    return strncmp(n, "000", 3) == 0 ? n + 3 : n;
}

int moves_read(const Bundle *b, MoveGraph *g)
{
    memset(g, 0, sizeof *g);
    const BdpEntry *e = NULL;
    for (uint32_t i = 0; i < b->nentries && !e; i++) {
        const char *n = bdp_name(b, b->entries[i].name);
        size_t len = strlen(n);
        if (b->entries[i].type == ET_RAW && len >= 12 && strcmp(n + len - 12, "MONSTER_DATA") == 0)
            e = &b->entries[i];
    }
    if (!e || e->size < 0xAA8 || rd32(e->data) != 11)
        return -1;
    const uint8_t *d = e->data;
    uint32_t size = e->size, graph = rd32(d + 0xA54);
    if (graph + 0x34 > size)
        return -1;
    int n = (int)rd32(d + graph + 0x2C);
    uint32_t first = rd32(d + graph + 0x30), at = first;
    uint32_t *offset = malloc(sizeof *offset * (n ? n : 1));
    int total = 0;
    for (int k = 0; k < n; k++) {                    /* sizes first: the states follow each other */
        if (at + 0x2C > size || rd32(d + at) != (uint32_t)k)
            return free(offset), -1;
        offset[k] = at;
        int nt = (int16_t)(d[at + 0xE] | d[at + 0xF] << 8);
        total += nt;
        at += 0x2C + 0x74 * nt;
        if (at > size)
            return free(offset), -1;
    }
    g->states = calloc(n ? n : 1, sizeof *g->states);
    g->all = calloc(total ? total : 1, sizeof *g->all);
    g->n = n;
    /* coded items: +0xA60 count, +0xA64 the block, +0xA68 a u16 offset into it per item; a node's +0x28 holds
     * first << 16 | count of its own */
    uint32_t nitems = rd32(d + 0xA60), block = rd32(d + 0xA64), item_offsets = rd32(d + 0xA68);
    g->all_events = calloc(nitems ? nitems : 1, sizeof *g->all_events);
    for (uint32_t i = 0; i < nitems && item_offsets + 2 * i + 2 <= size; i++) {
        uint32_t o = block + (d[item_offsets + 2 * i] | d[item_offsets + 2 * i + 1] << 8);
        MoveEvent *ev = &g->all_events[i];
        if (o + 28 > size)
            continue;
        ev->type = rd32(d + o);
        ev->at = rdf(d + o + 4);
        if (ev->type == 21) {                        /* {21, at, name, f32 p[4]} */
            ev->name = name_at(b, (int32_t)rd32(d + o + 8));
            for (int j = 0; j < 4; j++)
                ev->p[j] = rdf(d + o + 12 + 4 * j);
        } else if (ev->type == 6) {                  /* {6, at, ?, name} */
            ev->name = name_at(b, (int32_t)rd32(d + o + 12));
        } else {
            ev->value = rd32(d + o + 8);             /* 7: {7, at, mode} */
        }
    }
    /* slots: 74 start list indices at +0x4, then +0xA94 / +0xA98 columns of 74 overrides (-1 keep); start lists
     * +0xA58 / +0xA5C, 12 bytes each, the state's offset at +4 */
    uint32_t ncol = rd32(d + 0xA94), cols = rd32(d + 0xA98), nlists = rd32(d + 0xA58), lists = rd32(d + 0xA5C);
    if (ncol > 64 || cols + 0x128 * ncol > size || lists + 12 * nlists > size)
        ncol = nlists = 0;
    g->ncolumns = (int)ncol + 1;
    g->slot = malloc(sizeof *g->slot * g->ncolumns);
    for (int c = 0; c < g->ncolumns; c++)
        for (int k = 0; k < MOVES_SLOTS; k++) {
            int32_t list = (int32_t)rd32(d + (c == 0 ? 4 + 4 * k : cols + 0x128 * (c - 1) + 4 * k));
            int state = -1;
            if (list >= 0 && (uint32_t)list < nlists)
                for (int j = 0; j < n; j++)
                    if (offset[j] == rd32(d + lists + 12 * list + 4))
                        state = j;
            g->slot[c][k] = c > 0 && list < 0 ? g->slot[0][k] : state;
        }
    MoveTrans *t = g->all;
    for (int k = 0; k < n; k++) {
        MoveState *s = &g->states[k];
        const uint8_t *p = d + offset[k];
        uint32_t node = rd32(p + 4);
        if (node + 0x80 <= size) {
            s->clip = name_at(b, (int32_t)rd32(d + node + 0x78));
            s->name = name_at(b, (int32_t)rd32(d + node + 0x7C));
            uint32_t span = rd32(d + node + 0x28), first = span >> 16, count = span & 0xFFFF;
            if (first + count <= nitems) {
                s->events = g->all_events + first;
                s->nevents = (int)count;
            }
        }
        s->length = rdf(p + 8);
        s->ntrans = (int16_t)(p[0xE] | p[0xF] << 8);
        s->trans = t;
        s->next = k;
        for (int j = 0; j < n; j++)
            if (offset[j] == rd32(p + 0x14))
                s->next = j;
        for (int i = 0; i < s->ntrans; i++, t++) {
            const uint8_t *q = p + 0x2C + 0x74 * i;
            t->target = -1;
            for (int j = 0; j < n; j++)
                if (offset[j] == rd32(q + 4))
                    t->target = j;
            t->flags = rd32(q + 8);
            t->in0 = rdf(q + 0xC); t->in1 = rdf(q + 0x10);
            t->sw0 = rdf(q + 0x14); t->sw1 = rdf(q + 0x18);
            t->priority = (int16_t)(q[0x1C] | q[0x1D] << 8);
            t->ncond = (int16_t)(q[0x1E] | q[0x1F] << 8);
            if (t->ncond > 4) t->ncond = 4;
            for (int c = 0; c < t->ncond; c++) {
                const uint8_t *r = q + 0x20 + 16 * c;
                t->cond[c] = (MoveCond){ rd32(r), rd32(r + 4), rd32(r + 12), rd32(r + 8) };
            }
            t->call = name_at(b, (int32_t)rd32(q + 0x60));
        }
    }
    free(offset);
    return 0;
}

void moves_free(MoveGraph *g)
{
    free(g->states);
    free(g->all);
    free(g->all_events);
    free(g->slot);
    memset(g, 0, sizeof *g);
}

int moves_find(const MoveGraph *g, const char *state)
{
    for (int k = 0; k < g->n; k++)
        if (g->states[k].name && _stricmp(g->states[k].name, state) == 0)
            return k;
    return -1;
}

/* ---------------------------------------------------------------- input (0x2C0AD0, 0x2C1590, 0x2C0870) */

void moves_derive(MoveInput *in)
{
    float *v = in->v;
    v[MV_R_SIZE] = fmaxf(fabsf(v[MV_RX]), fabsf(v[MV_RY]));
    v[MV_L_SIZE] = fmaxf(fabsf(v[MV_LX]), fabsf(v[MV_LY]));
    for (int k = 32; k < 48; k++)                    /* the stick's sector as seen by the monster (0x2C03E0): */
        v[k] = 0;                                    /* only asked with Monsters::canMount, which says no here */
    for (int k = MV_L_UP; k <= MV_L_DOWNRIGHT; k++)
        v[k] = 0;
    /* 0x2C0870: the eighth the stick points into, (x, -y) turned by pi / 8; none under 0.2 */
    float x = v[MV_LX], y = -v[MV_LY];
    if (fmaxf(fabsf(x), fabsf(y)) >= 0.2f) {
        static const int OCTANT[8] = { MV_L_RIGHT, MV_L_UPRIGHT, MV_L_UP, MV_L_UPLEFT,
                                       MV_L_LEFT, MV_L_DOWNLEFT, MV_L_DOWN, MV_L_DOWNRIGHT };
        float a = atan2f(y, x) + 3.14159265f / 8;
        int k = (int)floorf(a / (3.14159265f / 4));
        v[OCTANT[(k % 8 + 8) % 8]] = 1;
    }
    v[MV_TRUE] = 1;
    v[MV_FALSE] = 0;
    in->fresh[MV_TRUE] = 1;
}

static int is_bool(uint32_t var) { return (var >= 4 && var < 0x16) || (var >= 0x30 && var < 0x3A); }

/* 0x2EC3D0's test of one condition: var OP value with OP 0 >, 1 >=, 2 <, 3 <=, 4 == (0x3F5950 / 0x3F5970) */
static int holds(const MoveCond *c, const MoveInput *in)
{
    if (c->var >= MV_COUNT)
        return 0;
    if (!(c->flags & 1) && in->fresh[c->var] <= 0)   /* a "new" condition: the input must have just come */
        return 0;
    float a = in->v[c->var], b;
    if (is_bool(c->var)) {
        a = a != 0;
        b = (c->value & 0xFF) != 0;
    } else {
        union { uint32_t u; float f; } u = { c->value };
        b = u.f;
    }
    switch (c->op) {
    case 0: return a > b;
    case 1: return a >= b;
    case 2: return a < b;
    case 3: return a <= b;
    case 4: return a == b;
    }
    return 0;
}

/* ---------------------------------------------------------------- the runner (0x2EBFE0) */

static void enter(MoveRunner *r, int state, float time)
{
    const MoveState *s = &r->g->states[state];
    r->state = state;
    r->time = time;
    r->chosen = -1;
    free(r->in_window);
    free(r->sw_window);
    r->in_window = calloc(s->ntrans ? s->ntrans : 1, 1);
    r->sw_window = calloc(s->ntrans ? s->ntrans : 1, 1);
}

void moves_start(MoveRunner *r, const MoveGraph *g, int state)
{
    memset(r, 0, sizeof *r);
    r->g = g;
    r->from = r->via = -1;
    enter(r, state >= 0 && state < g->n ? state : 0, 0);
}

void moves_stop(MoveRunner *r)
{
    free(r->in_window);
    free(r->sw_window);
    memset(r, 0, sizeof *r);
}

/* a window [a, b) the time is in, or jumped over this frame */
static int in_window(float a, float b, float prev, float time)
{
    return (time >= a && time < b) || (prev < a && time > b);
}

/* 0x2ECBE0: where the target's clip starts */
static float start_time(const MoveRunner *r, const MoveTrans *t)
{
    const MoveState *s = &r->g->states[r->state], *to = &r->g->states[t->target];
    if (!(t->flags & 1) || s->length <= 0)
        return 0;
    float f = to->length * (r->time / s->length);    /* keep the phase (walk into run) */
    return f < to->length ? f : to->length;
}

int moves_step(MoveRunner *r, MoveInput *in, float dt, MoveCall call, void *ctx)
{
    const MoveState *s = &r->g->states[r->state];
    float prev = r->time;
    r->time += dt;
    for (int i = 0; i < s->ntrans; i++) {
        const MoveTrans *t = &s->trans[i];
        if (in_window(t->in0, t->in1, prev, r->time)) r->in_window[i] = 1;
        if (in_window(t->sw0, t->sw1, prev, r->time)) r->sw_window[i] = 1;
    }
    /* 0x2EC3D0: of the transitions in their input window and allowed for a pad (0x10; 0x2FF750 returns 0), the
     * last one of the highest priority whose conditions and check hold; it stays picked until its switch window */
    for (int i = 0; i < s->ntrans; i++) {
        const MoveTrans *t = &s->trans[i];
        if (!r->in_window[i] || !(t->flags & 0x10) || t->target < 0)
            continue;
        if (r->chosen >= 0 && t->priority < s->trans[r->chosen].priority)
            continue;
        int ok = 1;
        for (int c = 0; c < t->ncond && ok; c++)
            ok = holds(&t->cond[c], in);
        if (ok && t->call)
            ok = call ? call(ctx, t->call) : 0;
        if (ok)
            r->chosen = i;
    }
    int switched = 0;
    if (r->chosen >= 0 && r->sw_window[r->chosen]) {
        const MoveTrans *t = &s->trans[r->chosen];
        float start = start_time(r, t);
        r->from = r->state;
        r->via = r->chosen;
        enter(r, t->target, start);
        switched = 1;
    } else if (r->time > s->length) {                /* the clip ended: its follow-up, or around again */
        r->from = r->state;
        r->via = -1;
        if (s->next == r->state) {
            float t = s->length > 0 ? fmodf(r->time, s->length) : 0;
            enter(r, s->next, t);
            r->time = t;
        } else {
            enter(r, s->next, 0);
        }
        switched = 1;
    } else {
        for (int i = 0; i < s->ntrans; i++) {        /* windows passed: cleared, a pick whose switch window passed lost */
            const MoveTrans *t = &s->trans[i];
            if (r->time >= t->in1) r->in_window[i] = 0;
            if (r->time >= t->sw1) {
                r->sw_window[i] = 0;
                if (r->chosen == i) r->chosen = -1;
            }
        }
    }
    for (int k = 0; k < MV_COUNT; k++)               /* inputs age; a switch uses them up */
        in->fresh[k] = switched ? 0 : in->fresh[k] - dt;
    return switched;
}

int moves_event(MoveRunner *r, const char *to)
{
    const MoveState *s = &r->g->states[r->state];
    for (int i = 0; i < s->ntrans; i++) {
        const MoveTrans *t = &s->trans[i];
        if (t->target < 0 || t->ncond != 1 || t->cond[0].var != MV_FALSE || t->cond[0].op != 4 ||
            !r->g->states[t->target].name || _stricmp(r->g->states[t->target].name, to) != 0)
            continue;
        r->from = r->state;
        r->via = i;
        enter(r, t->target, 0);
        return 0;
    }
    return -1;
}

int moves_events_between(const MoveState *s, float from, float to, const MoveEvent **first)
{
    int i = 0, n = 0;
    while (i < s->nevents && s->events[i].at * s->length <= from)
        i++;
    while (i + n < s->nevents && s->events[i + n].at * s->length <= to)
        n++;
    *first = s->events + i;
    return n;
}

int moves_slot(const MoveGraph *g, int slot, int column)
{
    if (slot < 0 || slot >= MOVES_SLOTS || !g->slot)
        return -1;
    return g->slot[column >= 0 && column + 1 < g->ncolumns ? column + 1 : 0][slot];
}

void moves_force(MoveRunner *r, int state)
{
    r->from = r->state;
    r->via = -1;
    enter(r, state, 0);
}
