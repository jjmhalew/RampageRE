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
        } else if (ev->type == 0 && o + 0x38 <= size) {   /* {0, at, i16 part[4], u16 count, flags, ...} */
            int count = d[o + 0x10] | d[o + 0x11] << 8;
            for (int j = 0; j < count && j < 4; j++) {
                int part = (int16_t)(d[o + 8 + 2 * j] | d[o + 9 + 2 * j] << 8);
                if (part >= 0 && part < 32)
                    ev->value |= 1u << part;
            }
            ev->p[0] = rd32(d + o + 0x14) & 6 ? 1.0f : 0.0f;
            ev->aflags = rd32(d + o + 0x14);
            ev->victims = rd32(d + o + 0x18);
            ev->level = (int32_t)rd32(d + o + 0x24);
            for (int j = 1; j < 4; j++)
                ev->p[j] = rdf(d + o + 0x28 + 4 * j);
        } else {
            ev->value = rd32(d + o + 8);             /* 7: {7, at, mode} */
            if ((ev->type == 15 || ev->type == 16) && o + 0x14 <= size)
                ev->p[0] = rdf(d + o + 0x10);        /* 15, 16: the record's f32 +0x10 */
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
    g->props = name_at(b, (int32_t)rd32(d + 0xAA4));
    uint32_t nattach = rd32(d + 0xA84), attach = rd32(d + 0xA88);
    if (nattach < 256 && attach + 0x38 * (uint64_t)nattach <= size) {
        g->attach = calloc(nattach ? nattach : 1, sizeof *g->attach);
        g->nattach = (int)nattach;
        for (uint32_t k = 0; k < nattach; k++) {
            const uint8_t *a = d + attach + 0x38 * k;
            for (int j = 0; j < 12; j++)
                g->attach[k].m[j] = rdf(a + 4 * j);
            g->attach[k].name = name_at(b, (int32_t)rd32(a + 0x30));
            g->attach[k].bone = (int)(rd32(a + 0x34) & 0xFFFF);
        }
    }
    uint32_t nnodes = rd32(d + 0xA4C), nodes = rd32(d + 0xA50);
    if (nodes + 0x84 * (uint64_t)nnodes <= size) {
        g->nodes = calloc(nnodes ? nnodes : 1, sizeof *g->nodes);
        g->nnodes = (int)nnodes;
        for (uint32_t k = 0; k < nnodes; k++) {
            const uint8_t *nd = d + nodes + 0x84 * k;
            MoveNode *m = &g->nodes[k];
            m->rate = rdf(nd);
            m->blend = rdf(nd + 0x14);
            m->clip = name_at(b, (int32_t)rd32(nd + 0x78));
            m->name = name_at(b, (int32_t)rd32(nd + 0x7C));
        }
    }
    MoveTrans *t = g->all;
    for (int k = 0; k < n; k++) {
        MoveState *s = &g->states[k];
        const uint8_t *p = d + offset[k];
        uint32_t node = rd32(p + 4);
        s->rate = 1;
        s->fwd[1] = 1;
        if (node + 0x80 <= size) {
            const uint8_t *nd = d + node;
            s->rate = rdf(nd);
            s->turn = rdf(nd + 0x4);
            s->speed = rdf(nd + 0x8);
            s->blend = rdf(nd + 0x14);
            s->fwd[0] = rdf(nd + 0x18);
            s->fwd[1] = rdf(nd + 0x1C);
            s->flags = rd32(nd + 0x2C);
            if (!(s->rate > 0))
                s->rate = 1;
            s->clip = name_at(b, (int32_t)rd32(d + node + 0x78));
            s->name = name_at(b, (int32_t)rd32(d + node + 0x7C));
            uint32_t span = rd32(d + node + 0x28), first = span >> 16, count = span & 0xFFFF;
            if (first + count <= nitems) {
                s->events = g->all_events + first;
                s->nevents = (int)count;
            }
        }
        s->length = rdf(p + 8);
        s->end_blend = rd32(p + 0x10) & 0xF;           /* the "clip ended" transition's +0x0 */
        s->ntrans = (int16_t)(p[0xE] | p[0xF] << 8);
        s->trans = t;
        s->next = k;
        for (int j = 0; j < n; j++)
            if (offset[j] == rd32(p + 0x14))
                s->next = j;
        for (int i = 0; i < s->ntrans; i++, t++) {
            const uint8_t *q = p + 0x2C + 0x74 * i;
            t->target = -1;
            t->blend = rd32(q) & 0xF;
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
            for (int j = 0; j < 4; j++)
                t->args[j] = rdf(q + 0x64 + 4 * j);
        }
    }
    free(offset);
    g->box[0] = rdf(d + 0x198);
    g->box[1] = rdf(d + 0x19C);
    g->probe[0] = rdf(d + 0x1A0);
    g->probe[1] = rdf(d + 0x1A4);
    uint32_t nsh = rd32(d + 0xA44), sh = rd32(d + 0xA48);
    if (sh + 24 * nsh <= size && nsh < 256) {
        g->shapes = calloc(nsh ? nsh : 1, sizeof *g->shapes);
        for (uint32_t k = 0; k < nsh; k++) {
            const uint8_t *p = d + sh + 24 * k;
            MoveShape *s = &g->shapes[g->nshapes++];
            for (int i = 0; i < 3; i++)
                s->off[i] = rdf(p + 4 * i);
            s->size = rdf(p + 12);
            s->radius = rdf(p + 16);
            s->bone = (int16_t)(p[20] | p[21] << 8);
            s->part = (int16_t)(p[22] | p[23] << 8);
        }
    }
    uint32_t nparts = rd32(d + 0xA6C), parts = rd32(d + 0xA74);
    for (uint32_t k = 0; k < nparts && k < MOVES_PARTS && parts + 4 * k + 4 <= size; k++) {
        uint32_t o = rd32(d + parts + 4 * k);        /* {name, ?, ?, u16 count, i16 bones[]} */
        if (o + 0xE > size)
            break;
        int nb = d[o + 0xC] | d[o + 0xD] << 8;
        g->part_name[k] = name_at(b, (int32_t)rd32(d + o));
        for (int i = 0; i < nb && i < MOVES_PART_BONES && o + 0x10 + 2 * i <= size; i++)
            g->part_bone[k][g->part_nbones[k]++] = (int16_t)(d[o + 0xE + 2 * i] | d[o + 0xF + 2 * i] << 8);
        g->nparts = (int)k + 1;
    }
    return 0;
}

void moves_free(MoveGraph *g)
{
    free(g->states);
    free(g->all);
    free(g->all_events);
    free(g->slot);
    free(g->shapes);
    free(g->nodes);
    free(g->attach);
    memset(g, 0, sizeof *g);
}

const MoveNode *moves_node(const MoveGraph *g, const char *name)
{
    for (int i = 0; name && i < g->nnodes; i++)
        if (g->nodes[i].name && _stricmp(g->nodes[i].name, name) == 0)
            return &g->nodes[i];
    return NULL;
}

int moves_find(const MoveGraph *g, const char *state)
{
    for (int k = 0; k < g->n; k++)
        if (g->states[k].name && _stricmp(g->states[k].name, state) == 0)
            return k;
    return -1;
}

/* ---------------------------------------------------------------- input (0x2C0AD0, 0x2C1590, 0x2C0870) */

void moves_sectors(MoveInput *in, float a, float b)
{
    float *v = in->v, size = v[MV_L_SIZE];
    for (int k = MV_M_FRONT; k <= MV_M_BACKLEFT8; k++)
        v[k] = in->fresh[k] = 0;
    if (size < 0.01f)                                /* 0x2C0BC0: no direction */
        return;
    /* halves, and the quadrant by the larger of the two (0x2C0740) */
    v[b >= 0 ? MV_M_FRONTHALF : MV_M_BACKHALF] = size;
    v[a >= 0 ? MV_M_RIGHTHALF : MV_M_LEFTHALF] = size;
    int q = b >= 0 ? (a >= 0 ? (b - a >= 0 ? MV_M_FRONT : MV_M_RIGHT) : (a + b >= 0 ? MV_M_FRONT : MV_M_LEFT))
                   : (a >= 0 ? (a + b >= 0 ? MV_M_RIGHT : MV_M_BACK) : (b - a >= 0 ? MV_M_LEFT : MV_M_BACK));
    v[q] = size;
    /* the eighths: the same tests with the stick turned by pi / 8 (0x2C0660) */
    const float c = 0.92387953f, s = 0.38268343f;
    float ra = a * c - b * s, rb = a * s + b * c;
    int e = rb >= 0 ? (ra >= 0 ? (rb - ra >= 0 ? MV_M_FRONTRIGHT8 : MV_M_RIGHT8)
                               : (ra + rb >= 0 ? MV_M_FRONT8 : MV_M_FRONTLEFT8))
                    : (ra >= 0 ? (ra + rb >= 0 ? MV_M_BACKRIGHT8 : MV_M_BACK8)
                               : (rb - ra >= 0 ? MV_M_LEFT8 : MV_M_BACKLEFT8));
    v[e] = size;
    for (int k = MV_M_FRONT; k <= MV_M_BACKLEFT8; k++)   /* 0x2C18F0: new while they are set */
        in->fresh[k] = v[k] > 0 ? 1.0f : 0.0f;
}

void moves_derive(MoveInput *in)
{
    float *v = in->v;
    v[MV_R_SIZE] = fmaxf(fabsf(v[MV_RX]), fabsf(v[MV_RY]));
    v[MV_L_SIZE] = fmaxf(fabsf(v[MV_LX]), fabsf(v[MV_LY]));
    in->fresh[MV_R_SIZE] = in->fresh[MV_L_SIZE] = 1;   /* 0x2C17E8: the sizes always count as new */
    for (int k = MV_M_FRONT; k <= MV_M_BACKLEFT8; k++)   /* the monster's frame: moves_sectors */
        v[k] = 0;
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
    uint32_t var = c->var;
    if ((c->flags & 2) && in->mirrored) {            /* mirrored: the other side (0x2C1638, 0x2C1688) */
        static const uint8_t SWAP[][2] = { { 50, 53 }, { 51, 54 }, { 52, 55 }, { 33, 34 }, { 41, 42 }, { 43, 44 },
                                           { 46, 47 } };
        for (size_t k = 0; k < sizeof SWAP / sizeof *SWAP; k++)
            if (var == SWAP[k][0]) { var = SWAP[k][1]; break; }
            else if (var == SWAP[k][1]) { var = SWAP[k][0]; break; }
    }
    if (!(c->flags & 1) && in->fresh[var] <= 0)      /* a "new" condition: the input must have just come */
        return 0;
    float a = in->v[var], b;
    if (is_bool(var)) {
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
            ok = call ? call(ctx, t->call, t->args) : 0;
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
