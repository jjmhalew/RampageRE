/* reveal.c - the monster reveal and findPointInRubble (reveal.h; docs/EDIBLE_NOTES.md "The cryotube",
 * docs/FLOW_NOTES.md "The rubble"). [R] marks what is read from the game's code or scripts, [G] what is a guess or a
 * stand-in. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "reveal.h"

#define PI 3.14159265f
#define MAX_REC 64

/* [R] 0x32C9C8: the names compare in any case */
static int same(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    for (; *a && *b; a++, b++) {
        int x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a, y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

/* an int key, -1 when missing (0x323B30 .. 0x323C48 read RevealLoc's four keys so) */
static int int_key(Group g, const char *key)
{
    Record r;
    return group_get(g, key, &r) ? (int)record_num(r) : -1;
}

/* ---------------------------------------------------------------- the roster */

int reveal_roster_load(RevealRoster *r, Group monster_data)
{
    r->n = 0;
    if (!monster_data.at)
        return 0;
    int n = (int)group_num(monster_data, "NumMonsters", 0);
    for (int i = 0; i < n && i < REVEAL_MONSTERS; i++) {
        char key[40];
        snprintf(key, sizeof key, "Monster|Monster%d", i);
        Group m = group_sub(monster_data, key);
        r->name[i] = m.at ? group_str(m, "Name") : NULL;
        r->n = i + 1;
    }
    return r->n;
}

int reveal_roster_find(const RevealRoster *r, const char *name)
{
    for (int i = 0; name && i < r->n; i++)           /* [R] 0x33DAF0: the first, any case */
        if (same(r->name[i], name))
            return i;
    return -1;
}

/* ---------------------------------------------------------------- the reveal */

void reveal_reset(Reveal *r)
{
    memset(r, 0, sizeof *r);
    r->monster = -1;
    r->shown_slot = r->shown = -1;                   /* [R] 0x323CB0 */
    r->revealed = -1;
}

int reveal_load(Reveal *r, Group world_data, int campaign, int players, const int *players_monster,
                const RevealRoster *roster)
{
    reveal_reset(r);                                 /* [R] 0x3236F0: +0x44 -1, +0x48 +0x4C +0x50 0, +0x60 0 */
    if (!campaign)                                   /* [R] 0x4D7E44 == 2 */
        return -1;
    r->show = players == 1;                          /* [R] 0x4D7E28 == 1 */
    Group def = world_data.at ? group_sub(world_data, "MonsterRevealDef") : (Group){ 0 };
    if (!def.at)
        return -1;
    const char *p1 = players > 0 && players_monster && players_monster[0] >= 0 && players_monster[0] < roster->n
                     ? roster->name[players_monster[0]] : NULL;   /* [R] GameStatus +0x214's name */
    uint32_t n = group_count(def);
    for (uint32_t i = 0; i < n; i++) {
        Record rec = group_record(def, i);
        if ((rec.kind & 0xFF) != PK_GROUP || !rec.name)
            continue;
        Group g = record_group(def, rec);
        if (r->count <= 0 && same(rec.name, "RevealDef|")) {
            const char *mon = group_str(g, "Monster");
            if (!mon)
                continue;
            int free = 1;                            /* [R] no player plays it (0x3238F8 .. 0x323950) */
            for (int k = 0; free && k < players; k++)
                if (players_monster && players_monster[k] >= 0 && players_monster[k] < roster->n &&
                    same(roster->name[players_monster[k]], mon))
                    free = 0;
            int idx = free ? reveal_roster_find(roster, mon) : -1;
            if (idx < 0 || (roster->flags[idx] & REVEAL_UNLOCKED))
                continue;                            /* [R] 0x323970 / 0x32398C */
            Record it[MAX_REC];                      /* [R] the list from the group's +0xC on: player 1's monster or */
            uint32_t m = group_items(g, it, MAX_REC);    /* "everyone", any case */
            for (uint32_t k = 0; k < m; k++) {
                const char *who = (it[k].kind & 0xFF) == PK_NAME ? record_str(g, it[k]) : NULL;
                if (who && (same(who, p1) || same(who, "everyone"))) {
                    r->monster = idx;                /* [R] +0x40 + 4 x count; the slot after the players gets it */
                    r->count = 1;                    /* (GameStatus +0x214 + 4 x players), +0x48 + 1, +0x44 = 0 */
                    r->revealed = 0;
                    break;
                }
            }
        } else if (same(rec.name, "RevealLoc|") && r->nlocs < REVEAL_LOCS) {
            RevealLoc *l = &r->loc[r->nlocs++];      /* [R] 0x323B10 .. 0x323C48 */
            l->block = int_key(g, "Block");
            l->building = int_key(g, "Building");
            l->floor = int_key(g, "floor");
            l->cell = int_key(g, "cell");
        }
    }
    return r->count > 0 ? r->monster : -1;
}

int reveal_extra_slot(const Reveal *r, int players)
{
    return r->show && r->count > 0 ? players : -1;  /* [R] 0x2B6C10: players + min(count, 1) monsters */
}

int reveal_block(Reveal *r, int block, int nbuildings, RevealLoc *out, int max)
{
    int made = 0;
    r->placed = 0;                                   /* [R] 0x3235F0 */
    if (r->recent)
        r->recent--;
    if (r->count == 0 || r->revealed >= r->count)
        return 0;
    for (int i = 0; i < r->nlocs; i++) {
        const RevealLoc *l = &r->loc[i];
        if (l->block != block || l->building >= nbuildings || l->building < 0)
            continue;                                /* [G] building < 0: the game would index before its list */
        if (made < max)
            out[made++] = *l;                        /* 0x318FD0(mgr, building, "Edible_Cryotube", floor, cell, ...) */
        r->placed++;
    }
    return made;
}

int reveal_start(Reveal *r, int players, RevealRoster *roster, int *show_slot)
{
    int slot = -1;
    if (r->placed > 0) {                             /* [R] 0x323520 */
        if (r->revealed > 0)
            r->show = 0;
        if (r->show) {
            r->shown_slot = players + r->revealed;   /* [R] 0x4D7E28 + 0x44: 0x2D40B0(monster, pos) */
            slot = r->shown_slot;
        }
        r->shown = r->monster;                       /* [R] +0x40 + 4 x revealed: placed > 0 means revealed is 0 */
        if (r->shown >= 0 && r->shown < roster->n)
            roster->flags[r->shown] |= REVEAL_UNLOCKED;   /* [R] 0x4D7BC4 + 4 x monster |= 0x80 */
        r->placed = 0;
        r->revealed++;
        r->recent = 2;
    }
    int ret = r->shown;                              /* [R] the binding answers +0x58, then stopReveal unless shown */
    if (!r->show) {
        reveal_stop(r);
        slot = -1;
    }
    if (show_slot)
        *show_slot = slot;
    return ret;
}

int reveal_stop(Reveal *r)
{
    int slot = r->show && r->shown_slot >= 0 ? r->shown_slot : -1;   /* [R] 0x3234D0: hidden only when shown */
    r->shown_slot = r->shown = -1;
    return slot;
}

int reveal_was_revealed(const Reveal *r) { return r->recent != 0; }

int reveal_continue(Reveal *r)
{
    int slot = -1;
    if (r->revealed > 0)                             /* [R] 0x323450 */
        r->revealed--;
    if (r->shown_slot >= 0) {
        if (r->show)
            slot = r->shown_slot;
        r->shown_slot = r->shown = -1;
    }
    r->recent = 0;
    return slot;
}

int reveal_all_revealed(Group world_data, const RevealRoster *roster)
{
    Group def = world_data.at ? group_sub(world_data, "MonsterRevealDef") : (Group){ 0 };
    if (!def.at)
        return 1;                                    /* [R] 0x323240: no MonsterRevealDef answers 1 */
    uint32_t n = group_count(def);
    for (uint32_t i = 0; i < n; i++) {
        Record rec = group_record(def, i);
        if ((rec.kind & 0xFF) != PK_GROUP || !same(rec.name, "RevealDef|"))
            continue;
        int idx = reveal_roster_find(roster, group_str(record_group(def, rec), "Monster"));
        if (idx >= 0 && !(roster->flags[idx] & REVEAL_UNLOCKED))
            return 0;
    }
    return 1;
}

int reveal_boosy(RevealRoster *roster, const uint32_t *city_flags, int ncities, int current)
{
    uint32_t all = 0xF;                              /* [R] 0x3259E4 .. 0x325A5C */
    for (int i = 0; i < ncities; i++)
        all &= city_flags[i] | (i == current ? 8u : 0u);
    if (all != 0xF)
        return 0;
    int b = reveal_roster_find(roster, "Boosy");
    if (b < 0 || (roster->flags[b] & REVEAL_UNLOCKED))
        return 0;
    roster->flags[b] |= REVEAL_UNLOCKED;
    return 1;
}

/* ---------------------------------------------------------------- the walk-on */

static RevealEvent *wemit(RevealWalk *w, int type)
{
    if (w->nev >= (int)(sizeof w->ev / sizeof w->ev[0]))
        return NULL;
    RevealEvent *e = &w->ev[w->nev++];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->slot = w->slot;
    return e;
}

static void wmove(RevealWalk *w, const char *name)
{
    RevealEvent *e = wemit(w, RW_MOVE);
    if (e)
        e->name = name;
}

static void wscale(RevealWalk *w, float scale, float seconds)
{
    RevealEvent *e = wemit(w, RW_SCALE);
    if (e) {
        e->v[0] = scale;
        e->v[1] = seconds;
    }
}

void reveal_walk_start(RevealWalk *w, int slot, const float pos[3])
{
    memset(w, 0, sizeof *w);
    w->slot = slot;
    w->active = slot >= 0;
    if (!w->active)
        return;
    RevealEvent *e = wemit(w, RW_SHOW);              /* [R] 0x2D40B0: at pos, facing pi (0x40490FDB) */
    if (e) {
        memcpy(e->pos, pos, sizeof e->pos);
        e->v[0] = PI;
    }
    /* [R] onRevealNITick's first call (no timer yet): setIsAI(1), revealTime 0, the 0.25 s timer, Ground_Idle,
     * setDynamicScale(0.1, 0) then (1.0, 1.0) */
    wemit(w, RW_AI);
    w->timer = 1;
    w->time = 0;
    w->clock = 0;                                    /* [R] installObjectTimer(0.25, 0, 0): its clock from 0 */
    wmove(w, "Ground_Idle");
    wscale(w, 0.1f, 0);
    wscale(w, 1.0f, 1.0f);
}

void reveal_walk_update(RevealWalk *w, float dt, float monster_x, float camera_x)
{
    if (!w->active || !w->timer)
        return;
    /* [R] 0x28A780, each world step: the clock grows by the step; at the period it becomes min(clock - period, 1)
     * and the timer fires, once a step: the first tick on the 8th step (0.267 s) */
    w->clock += dt;
    if (w->clock >= 0.25f) {
        w->clock = w->clock - 0.25f < 1 ? w->clock - 0.25f : 1;
        if (w->time == 0.5f)                         /* [R] exact compares, revealTime in steps of 0.25 */
            wmove(w, "Ground_Taunt");
        else if (w->time == 4.0f) {
            RevealEvent *e = wemit(w, RW_WALK);      /* [R] runGoal (100, -10, 0), x negated left of the camera */
            if (e) {
                e->pos[0] = monster_x < camera_x ? -100.0f : 100.0f;
                e->pos[1] = -10.0f;
                e->v[0] = 1.0f;
            }
            e = wemit(w, RW_SPEED);                  /* [R] setSpeedScale(1.5, 100000) */
            if (e) {
                e->v[0] = 1.5f;
                e->v[1] = 100000.0f;
            }
        } else if (w->time == 5.0f)
            w->timer = 0;                            /* [R] gWorld.removeTimer; revealTime still goes up */
        w->time += 0.25f;
    }
}

void reveal_walk_arrived(RevealWalk *w, Reveal *r)
{
    int slot = reveal_stop(r);                       /* [R] Monsters::onRevealNIEnd: gWorld.stopReveal() */
    if (slot >= 0) {
        RevealEvent *e = wemit(w, RW_HIDE);
        if (e)
            e->slot = slot;
    }
    w->active = 0;
}

int reveal_walk_events(RevealWalk *w, RevealEvent *out, int max)
{
    int n = w->nev < max ? w->nev : max;
    memcpy(out, w->ev, sizeof *out * (size_t)n);
    memmove(w->ev, w->ev + n, sizeof *w->ev * (size_t)(w->nev - n));
    w->nev -= n;
    return n;
}

/* ---------------------------------------------------------------- the rubble */

void rubble_clear(Rubble *r) { r->n = 0; }
void rubble_seed(Rubble *r, uint32_t seed) { r->seed = seed % 0x7FFFFFFFu ? seed % 0x7FFFFFFFu : 1; }

/* [R] 0x106BD0: seed = 16807 x seed mod 2^31 - 1 (the high part folded in twice; 2^31 - 1 becomes 0) */
static uint32_t park_miller(uint32_t *seed)
{
    uint64_t x = (uint64_t)*seed * 16807u;
    if (x >= 0x80000000u)
        x = (x & 0x7FFFFFFFu) + (x >> 31);
    if (x >= 0x80000000u)
        x = (x & 0x7FFFFFFFu) + (x >> 31);
    if (x == 0x7FFFFFFFu)
        x = 0;
    *seed = (uint32_t)x;
    return *seed;
}

/* [R] (float) of that, x 2^-31 (0x30000000) */
static float unit(Rubble *r) { return (float)park_miller(&r->seed) * 4.656612873e-10f; }

int rubble_load(Rubble *r, Group block_left, int had_previous, const RubbleHost *host)
{
    r->n = 0;                                        /* [R] 0x303D50 */
    if (!had_previous || !block_left.at)
        return 0;
    /* [R] BackRowOffset (an int) [G: 10 when missing, as city.c's DefBlockProps default; the game reads address 0] */
    float back = group_num(block_left, "BackRowOffset", 10);
    Record rec[MAX_REC];
    Group where;
    uint32_t m = group_values(block_left, "BuildingInstance|", &where, rec, MAX_REC);
    for (uint32_t i = 0; i < m && r->n < RUBBLE_SLOTS; i++) {
        Group inst = record_group(where, rec[i]);
        float left = group_num(inst, "LeftEdgeXPos", 0);
        if (left < -50.0f || !(left < 50.0f))        /* [R] 0x303EB4 .. 0x303EF0: -50 <= x < 50 */
            continue;
        Group t = host && host->tmpl ? host->tmpl(host->ctx, group_str(inst, "TemplateName")) : (Group){ 0 };
        if (!t.at)
            continue;
        const char *shape = group_str(t, "Shape|RubbleShape");
        if (!shape)
            shape = group_str(t, "RubbleShape");
        float mn[3], mx[3];
        if (!shape || !host->shape_box || host->shape_box(host->ctx, shape, mn, mx) != 0)
            continue;                                /* [R] no RubbleShape: no slot */
        int width = (int)group_num(t, "Width", 0);   /* [R] the template's Width, the instance's Row */
        int row = (int)group_num(inst, "Row", 0);
        float *p = r->s[r->n].pos;
        p[0] = left + 0.5f * (5.0f * (float)width);
        p[1] = -36.0f + (row != 0 ? back : 0);       /* [R] 0xC2100000; the block left lies 41 behind, its fronts 5 in */
        p[2] = 0;
        memcpy(r->s[r->n].min, mn, sizeof mn);
        memcpy(r->s[r->n].max, mx, sizeof mx);
        r->n++;
    }
    return r->n;
}

int rubble_find_point(Rubble *r, const float pos[3], float radius, float out[3])
{
    float box[RUBBLE_SLOTS][6];
    int n = 0;
    for (int i = 0; i < r->n; i++) {                 /* [R] 0x3036F8 .. 0x3037F4: the box moved to its place */
        float b[6];
        for (int k = 0; k < 3; k++) {
            b[k] = r->s[i].min[k] + r->s[i].pos[k];
            b[3 + k] = r->s[i].max[k] + r->s[i].pos[k];
        }
        float d2 = 0;                                /* [R] 0x100810: the nearest point's distance squared <= r^2 */
        for (int k = 0; k < 3; k++) {
            float d = pos[k] < b[k] ? pos[k] - b[k] : pos[k] <= b[3 + k] ? 0 : pos[k] - b[3 + k];
            d2 += d * d;
        }
        if (d2 <= radius * radius)
            memcpy(box[n++], b, sizeof b);
    }
    if (n == 0)
        return 0;
    int pick = (int)(((float)n - 0.01f) * unit(r));  /* [R] 0x303848 .. 0x303878 (0.01: 0x3C23D70A) */
    const float *b = box[pick];
    float p[3];
    p[0] = b[0] + 0.5f + (b[3] - b[0] - 1.0f) * unit(r);
    p[1] = b[1] + 0.5f + (b[4] - b[1] - 1.0f) * unit(r);
    p[2] = b[2];
    float d[3] = { p[0] - pos[0], p[1] - pos[1], p[2] - pos[2] };
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len <= radius)) {                          /* [R] 0x303A00: onto the sphere */
        float s = radius / len;
        for (int k = 0; k < 3; k++)
            p[k] = pos[k] + d[k] * s;
    }
    memcpy(out, p, sizeof p);
    return 1;
}
