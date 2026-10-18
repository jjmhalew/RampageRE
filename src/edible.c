/* edible.c - the edibles: power-ups and hazards behind the windows, and the monster's energy, rage and speed (edible.h;
 * docs/EDIBLE_NOTES.md). [R] marks what is read from the game's code or scripts, [G] what is a guess or a stand-in. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "edible.h"
#include "wrand.h"

#define PI 3.14159265f
#define MAX_EVENTS 256
#define MAX_CELLS 0x12C                              /* [R] 0x3196F0 asks 0x2BE750 for at most 300 cells */
#define NOT_FOUND "StringNotFound"
#define EPS 0.001f                                   /* [R] 0x3A83126F, the clocks' "out" */
#define LIFT 1.5f                                    /* [R] 0x317D38: +0xF4, its height above its storey's floor */

/* building states (+0x14C, building.h's BUILDING_*): from 4 on the building is being destroyed */
enum { BSTATE_DESTROYED = 4 };

struct Edibles {
    EdibleHost host;
    EdibleKind *kind; int nkinds, kcap;
    EdibleSlot *slot; int nslots, scap;
    Edible *ed; int n, cap;
    EdibleEvent ev[MAX_EVENTS]; int nev, next_fx;
    float fill;                                      /* the Edibles' FillRate (f20 in 0x3196F0) */
    float freq_sum;                                  /* [R] +0x28: the edible kinds' Frequency summed */
};

/* ---------------------------------------------------------------- small things */

/* rand(): the lagged Fibonacci 0x106950 on the world's gWorld +0x82C (wrand.c) [R] */
static uint32_t rnd(Edibles *e)
{
    (void)e;
    return wrand_lf();
}

static int same_text(const char *x, const char *y)   /* any case, as the game compares names (0x32C9C8) */
{
    if (!x || !y)
        return 0;
    for (; *x && *y; x++, y++) {
        int p = *x >= 'A' && *x <= 'Z' ? *x + 32 : *x, q = *y >= 'A' && *y <= 'Z' ? *y + 32 : *y;
        if (p != q)
            return 0;
    }
    return *x == *y;
}

static int contains(const char *s, const char *what) { return s && what && strstr(s, what) != NULL; }

/* a property as the scripts ask for it: the books keep a record's type before a '|' ("Effect|PickupFx",
 * "Sound|PickupSound", "Color|MessageColor"); the scripts ask by the bare name, so the typed names are tried too
 * (army.c's convention). An edible has no instance book (0x317BC0 inits it with none), so getStringProp and
 * getTemplateStringProp both read the template [R] */
static const char *const TYPED[] = { "", "Effect|", "Sound|", "Shape|", "OTRef|", "Script|", "Color|" };

static int find_rec(Group g, const char *key, Record *out)
{
    char k[96];
    if (!g.at || !key)
        return 0;
    for (size_t i = 0; i < sizeof TYPED / sizeof *TYPED; i++) {
        snprintf(k, sizeof k, "%s%s", TYPED[i], key);
        if (group_get(g, k, out))
            return 1;
    }
    return 0;
}
static const char *gstr(Group g, const char *key)
{
    char k[96];
    if (!g.at || !key)
        return NULL;
    for (size_t i = 0; i < sizeof TYPED / sizeof *TYPED; i++) {
        snprintf(k, sizeof k, "%s%s", TYPED[i], key);
        const char *s = group_str(g, k);
        if (s && *s)
            return s;
    }
    return NULL;
}
static float gnum(Group g, const char *key, float def)
{
    Record r;
    return find_rec(g, key, &r) && (r.kind & 0xFF) <= PK_FLOAT ? record_num(r) : def;
}

static Group find_template(Edibles *e, const char *name)
{
    Group none = { NULL, 0 };
    return e->host.find && name ? e->host.find(e->host.ctx, "Objects1_Props", name) : none;
}

static EdibleEvent *emit(Edibles *e, int type, int edible, const char *name, const float pos[3])
{
    static EdibleEvent spare;
    if (e->nev >= MAX_EVENTS) {
        memset(&spare, 0, sizeof spare);
        return &spare;
    }
    EdibleEvent *v = &e->ev[e->nev++];
    memset(v, 0, sizeof *v);
    v->type = type;
    v->id = -1;
    v->edible = edible;
    v->monster = -1;
    v->name = name;
    if (pos)
        memcpy(v->pos, pos, sizeof v->pos);
    return v;
}

/* FollowEffect.createAndStart(pos, name) (attached: createAndStartAttachedWithOffset(actor, name, 0)); its handle */
static int effect(Edibles *e, const char *name, const float pos[3], int follow)
{
    if (!name)
        return -1;
    EdibleEvent *v = emit(e, EE_EFFECT, follow, name, pos);
    v->id = e->next_fx++;
    return v->id;
}
static void effect_stop(Edibles *e, Edible *d)
{
    if (d->fx >= 0) {
        EdibleEvent *v = emit(e, EE_EFFECT_STOP, (int)(d - e->ed), NULL, d->pos);
        v->id = d->fx;
        d->fx = -1;
    }
}

static void place_world(Edible *d)                   /* translation x Rz(spin) x Rx(pitch), column-major */
{
    float cz = cosf(d->spin), sz = sinf(d->spin), cx = cosf(d->pitch), sx = sinf(d->pitch);
    float *m = d->world;
    m[0] = cz;  m[1] = sz;  m[2] = 0;  m[3] = 0;
    m[4] = -sz * cx; m[5] = cz * cx; m[6] = sx; m[7] = 0;
    m[8] = sz * sx;  m[9] = -cz * sx; m[10] = cx; m[11] = 0;
    m[12] = d->pos[0]; m[13] = d->pos[1]; m[14] = d->pos[2]; m[15] = 1;
}

void edible_cell_place(const float building_min[3], int column, float bottom, float out[3])
{
    out[0] = building_min[0] + 5.0f * (float)column + 2.5f;        /* [R] 0x317BC0: grid +0x24 + 5 x column + 2.5 */
    out[1] = building_min[1] + 2.5f;                               /* [R] grid +0x28 + 2.5: half a cell inside */
    out[2] = building_min[2] + bottom;                             /* [R] 0x2CD010: 6 + 3 x storey (less the sinking) */
}

/* ---------------------------------------------------------------- setup */

Edibles *edible_new(const EdibleHost *host)
{
    Edibles *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    if (host)
        e->host = *host;
    return e;
}

void edible_free(Edibles *e)
{
    if (!e)
        return;
    free(e->kind);
    free(e->slot);
    free(e->ed);
    free(e);
}


void edible_clear(Edibles *e)
{
    e->nkinds = e->nslots = e->n = e->nev = 0;
    e->fill = e->freq_sum = 0;
}

int edible_kinds(const Edibles *e) { return e->nkinds; }
const EdibleKind *edible_kind(const Edibles *e, int k) { return k >= 0 && k < e->nkinds ? &e->kind[k] : NULL; }
int edible_slots(const Edibles *e) { return e->nslots; }
const EdibleSlot *edible_slot(const Edibles *e, int i) { return i >= 0 && i < e->nslots ? &e->slot[i] : NULL; }
int edible_count(const Edibles *e) { return e->n; }
const Edible *edible_get(const Edibles *e, int i) { return i >= 0 && i < e->n ? &e->ed[i] : NULL; }

/* [R] 0x318B40 / 0x318FD0: a kind record {+0 name, +4 sort (1: edible), +8 1 for a hazard (strncmp(name, "Hazard", 6)
 * == 0, 0x32D1B0: case counts), +0xC Frequency, +0x10 the DropRate} */
static int add_kind(Edibles *e, const char *name, float freq)
{
    if (e->nkinds == e->kcap) {
        int cap = e->kcap ? 2 * e->kcap : 16;
        EdibleKind *k = realloc(e->kind, sizeof *k * (size_t)cap);
        if (!k)
            return -1;
        e->kind = k;
        e->kcap = cap;
    }
    EdibleKind *k = &e->kind[e->nkinds];
    k->name = name;
    k->tmpl = find_template(e, name);
    k->freq = freq;
    k->hazard = strncmp(name, "Hazard", 6) == 0;
    return e->nkinds++;
}

static int kind_of(const Edibles *e, const char *name)
{
    for (int k = 0; k < e->nkinds; k++)
        if (same_text(e->kind[k].name, name))       /* [G] the game compares the books' interned name pointers */
            return k;
    return -1;
}

int edible_load_block(Edibles *e, Group block)
{
    edible_clear(e);
    /* [R] 0x31A540: the block's "Edibles" (0x3F5CB4) group, its "FillRate" (0x3F5CC4) into f20 */
    Group g = group_sub(block, "Edibles"), where;
    if (!g.at)
        return 0;
    e->fill = group_num(g, "FillRate", 0);
    /* [R] 0x318B40(mgr, block, "Edibles", sort 1, 0, "EdibleInstance"): each record of that type with an OTRef|Name
     * and a Frequency; the sum to +0x28 */
    Record r[64];
    uint32_t n = group_values(g, "EdibleInstance|", &where, r, 64);
    for (uint32_t i = 0; i < n; i++) {
        Group inst = record_group(where, r[i]);
        const char *name = group_str(inst, "OTRef|Name");
        Record fr;
        if (!name || !group_get(inst, "Frequency", &fr))
            continue;
        float f = record_num(fr);
        if (add_kind(e, name, f) >= 0)
            e->freq_sum += f;
    }
    return e->nkinds;
}

static int find_slot(const Edibles *e, int building, int storey, int column)
{
    for (int i = 0; i < e->nslots; i++)
        if (e->slot[i].building == building && e->slot[i].column == column && e->slot[i].storey == storey)
            return i;
    return -1;
}

static int new_slot(Edibles *e, int building, int storey, int column, int side, int kind)
{
    if (e->nslots == e->scap) {
        int cap = e->scap ? 2 * e->scap : 64;
        EdibleSlot *s = realloc(e->slot, sizeof *s * (size_t)cap);
        if (!s)
            return -1;
        e->slot = s;
        e->scap = cap;
    }
    EdibleSlot *s = &e->slot[e->nslots];
    s->building = building;
    s->storey = storey;
    s->column = column;
    s->side = side;
    s->kind = kind;
    s->item = -1;
    return e->nslots++;
}

/* [R] the pick (0x319D50): r = rand() / 2^32 x the sum; the first kind whose running sum reaches r */
static int pick_kind(Edibles *e)
{
    float r = (float)rnd(e) * 2.3283064e-10f * e->freq_sum, sum = 0;
    int last = -1;
    for (int k = 0; k < e->nkinds; k++) {
        if (e->kind[k].freq <= 0)
            continue;
        last = k;
        sum += e->kind[k].freq;
        if (r <= sum)
            return k;
    }
    return last;                                     /* [G] rounding: the game would run past its list */
}

int edible_add_building(Edibles *e, int building, Group inst, Group props, int width, int storeys)
{
    int made = 0;
    /* [R] 0x2BE750 -> 0x2D01F0(grid, list, 300, from 1, to min(100, standing - 1), flags 6): storey by storey, column
     * by column, the front cells not damaged (cell +0 <= 0); flag 4: whether the cell has a window does not matter; a
     * random side of the two (0x106BD0 < 0.5); flag 2: one entry per cell */
    int cell[MAX_CELLS][3], n = 0, top = storeys - 1 < 100 ? storeys - 1 : 100;
    for (int s = 1; s <= top && n < MAX_CELLS; s++)
        for (int c = 0; c < width && n < MAX_CELLS; c++) {
            if (e->host.cell_state && e->host.cell_state(e->host.ctx, building, s, c) > 0)
                continue;
            cell[n][0] = s;
            cell[n][1] = c;
            cell[n][2] = wrand_frand() >= 0.5f;           /* [R 0x2D02E0] Park-Miller x 2^-31 */
            n++;
        }
    if (e->nkinds > 0 && e->freq_sum > 0 && n > 0) {
        int total = (int)(0.5f + (float)n * e->fill);   /* [R] cvt.w.s of 0.5 + n x f20 */
        /* [R] "NumEdibles" (0x3F59F4) of the building (+0x7C), an int: that many, at most n. No book sets it */
        Record r;
        if ((group_get(inst, "NumEdibles", &r) || group_get(props, "NumEdibles", &r)) && (r.kind & 0xFF) == PK_INT &&
            (int32_t)r.value >= 0)
            total = (int32_t)r.value < n ? (int32_t)r.value : n;
        int idx[MAX_CELLS];
        for (int i = 0; i < n; i++)
            idx[i] = i;
        for (int i = 0; i < n; i++) {                /* [R] each swapped with a random one (rand() % n) */
            int j = (int)(rnd(e) % (uint32_t)n), t = idx[i];
            idx[i] = idx[j];
            idx[j] = t;
        }
        for (int k = 0; k < total; k++) {
            int kind = pick_kind(e);
            const int *c = cell[idx[k]];
            /* [R] a slot of that cell (a window occupant's) takes the edible (+0xE); otherwise a new slot, its
             * occupant 0xFF */
            int s = find_slot(e, building, c[0], c[1]);
            if (s >= 0)
                e->slot[s].kind = kind;
            else if (new_slot(e, building, c[0], c[1], c[2], kind) >= 0)
                made++;
        }
    }
    /* [R] then the building's own records (0x319F98 on): each "EdibleInstance" (0x3F5CB8) with Name, cell and floor
     * (0x3F5990, 0x3F59A4, 0x3F59A0) -> 0x318FD0(mgr, building, Name, floor, cell, edible 1, military 0, 0) */
    Group src[2] = { inst, props }, where;
    for (int b = 0; b < 2; b++) {
        Record r[32];
        uint32_t m = src[b].at ? group_values(src[b], "EdibleInstance|", &where, r, 32) : 0;
        for (uint32_t i = 0; i < m; i++) {
            Group g = record_group(where, r[i]);
            const char *name = group_str(g, "Name");
            Record cr, fr;
            if (!name || !group_get(g, "cell", &cr) || !group_get(g, "floor", &fr))
                continue;
            int before = e->nslots;
            edible_add_fixed(e, building, (int)record_num(fr), (int)record_num(cr), name);
            made += e->nslots - before;
        }
        if (m)
            break;                                   /* the instance's own list; the template's only without one [G;
                                                      * moot: no building template (Buildings1_props, the cities'
                                                      * _buildings books) has an EdibleInstance|] */
    }
    return made;
}

int edible_add_fixed(Edibles *e, int building, int storey, int column, const char *name)
{
    if (!name)
        return -1;
    int k = kind_of(e, name);
    if (k < 0)
        k = add_kind(e, name, 0);                    /* [R] 0x318FD0: a new record, Frequency 0 */
    if (k < 0)
        return -1;
    int s = find_slot(e, building, storey, column);
    if (s >= 0)
        e->slot[s].kind = k;                         /* [R] +0xE of the slot there */
    else
        s = new_slot(e, building, storey, column, 0, k);   /* [G] its side from 0x2D0460 (a tile byte per side, +0x100),
                                                      * not followed: an edible's slot opens no window */
    return s;
}

int edible_add_cryotube(Edibles *e, int building, int storey, int column)
{
    static const char NAME[] = "Edible_Cryotube";
    int had = kind_of(e, NAME) >= 0;
    int s = edible_add_fixed(e, building, storey, column, NAME);
    if (s >= 0 && !had)                              /* [R] 0x318FD0's 7th argument (1) is the new kind's +8; an */
        e->kind[e->slot[s].kind].hazard = 1;         /* existing kind keeps its own (0x319050) */
    return s;
}

/* ---------------------------------------------------------------- the edibles */

static Edible *new_edible(Edibles *e)
{
    if (e->n == e->cap) {
        int cap = e->cap ? 2 * e->cap : 32;
        Edible *d = realloc(e->ed, sizeof *d * (size_t)cap);
        if (!d)
            return NULL;
        e->ed = d;
        e->cap = cap;
    }
    Edible *d = &e->ed[e->n++];
    memset(d, 0, sizeof *d);
    return d;
}

/* requestDelete: the effect turns off with it (setTurnOffWhenObjectDies), and the slot forgets it (the destructor ->
 * 0x318140: +0x10 = 0, +0xE = 0xFF) */
static void remove_edible(Edibles *e, Edible *d)
{
    if (d->phase == EP_GONE)
        return;
    effect_stop(e, d);
    if (d->slot >= 0 && d->slot < e->nslots) {
        e->slot[d->slot].item = -1;
        e->slot[d->slot].kind = -1;
    }
    d->slot = -1;
    d->phase = EP_GONE;
    emit(e, EE_REMOVED, (int)(d - e->ed), d->name, d->pos);
}

/* [R] 0x317BC0: the kind's template made an Actor (0x283DA0, init 0x30D280), placed in its cell, given its slot
 * (0x30C3C0), +0xF4 = 1.5; then its script's Construct */
static void spawn(Edibles *e, int s)
{
    EdibleSlot *sl = &e->slot[s];
    if (sl->kind < 0 || sl->item >= 0)
        return;
    const EdibleKind *k = &e->kind[sl->kind];
    float pos[3];
    if (!e->host.cell_place || !e->host.cell_place(e->host.ctx, sl->building, sl->storey, sl->column, pos))
        return;                                      /* [G] the game places it anyway, where the storey would be */
    Edible *d = new_edible(e);
    if (!d)
        return;
    int me = (int)(d - e->ed);
    d->name = k->name;
    d->tmpl = k->tmpl;
    d->slot = s;
    d->phase = EP_WINDOW;
    memcpy(d->pos, pos, sizeof d->pos);
    d->fx = -1;
    d->pulse = -1;                                   /* [R] the Actor's +0xF8: no pulse until a script sets one */
    const char *script = gstr(k->tmpl, "ScriptName");
    d->script = contains(script, "Cryotube") ? EK_CRYOTUBE : EK_EDIBLE;
    /* the look and the lifetime: ActorAnimShape|Default { Duration, Shape|ShapeName } [R] no edible has a
     * DefaultAnim, so neither 0x317D3C nor the init (0x30D5D0) starts one; the current record (+0xE8) is 0 from the
     * constructor (0x30E45C), which the update plays (0x30CE0C: the record's vtable +0xC; 2 = done -> onAnimComplete):
     * their only record, Default, so onAnimComplete comes Duration seconds on */
    Group look = group_sub(k->tmpl, "ActorAnimShape|Default");
    const char *shape = look.at ? group_str(look, "Shape|ShapeName") : NULL;
    d->shape = shape && !same_text(shape, "InvisShape") ? shape : NULL;
    d->life = look.at ? group_num(look, "Duration", -1) : -1;
    /* [R] Edible.bsf / Cryotube.bsf Construct: the Effect attached with no offset, turned off when it dies; Rotate: a
     * RotationMover of (0, 0, pi) */
    d->fx = effect(e, gstr(k->tmpl, "Effect"), d->pos, me);
    d->rotate = (int)gnum(k->tmpl, "Rotate", 0) != 0;
    if (d->script == EK_CRYOTUBE) {
        const char *snd = gstr(k->tmpl, "Sound");    /* [R] add3DSfx(getStringProp("Sound")) */
        if (snd)
            emit(e, EE_SOUND, me, snd, d->pos);
    }
    place_world(d);
    sl->item = me;
}

/* [R] Cryotube.bsf onPickup(monster, eat): its PickupFx, addStats(type 0), then out of the building along a
 * LineMover: 5 towards the street at 20, then 5 more and down to z = 1; leaveBuilding; a pulse every 1/24 s tumbles
 * it (pitch + 2 pi / 24 each) */
static void tube_pickup(Edibles *e, Edible *d, int monster)
{
    int me = (int)(d - e->ed);
    effect(e, gstr(d->tmpl, "PickupFx"), d->pos, -1);
    if (monster >= 0 && !d->counted) {
        EdibleEvent *v = emit(e, EE_STATS, me, d->name, d->pos);
        v->monster = monster;
        v->level = 0;
        v->tmpl = d->tmpl;
        v->v[0] = gnum(d->tmpl, "Health", 0);
        v->v[1] = gnum(d->tmpl, "Energy", 0);
        v->v[2] = (float)(int)gnum(d->tmpl, "Points", 0);
        d->counted = 1;
    }
    memcpy(d->mv.pts[0], d->pos, sizeof d->pos);
    memcpy(d->mv.pts[1], d->pos, sizeof d->pos);
    d->mv.pts[1][1] -= 5;
    memcpy(d->mv.pts[2], d->mv.pts[1], sizeof d->pos);
    d->mv.pts[2][1] -= 5;
    d->mv.pts[2][2] = 1;
    d->mv.n = 3;
    d->mv.seg = 0;
    d->mv.t = 0;
    if (d->slot >= 0 && d->slot < e->nslots) {       /* [R] leaveBuilding (0x30C3D0 -> 0x318140(mgr, actor, 0)) */
        e->slot[d->slot].item = -1;
        e->slot[d->slot].kind = -1;
    }
    d->slot = -1;
    d->phase = EP_FALLING;
    d->pulse = 1.0f / 24.0f;
    d->pitch = 0;
    d->spin = 0;
    d->rotate = 0;                                   /* the LineMover replaces the RotationMover */
}

/* [R] Edible.bsf onPickup(monster, bDoEat) */
static void edible_pickup(Edibles *e, Edible *d, int monster, int eat, const char *move)
{
    int me = (int)(d - e->ed);
    effect_stop(e, d);
    if (!d->played_fx) {                             /* PickupFx, once, where it is */
        effect(e, gstr(d->tmpl, "PickupFx"), d->pos, -1);
        d->played_fx = 1;
    }
    if (!eat) {                                      /* destroyPowerup: setPulseDelay(0.5); onPulse requestDelete */
        d->pulse = 0.5f;
        d->phase = EP_PICKED;
        return;
    }
    const char *reaction = gstr(d->tmpl, "Reaction");
    if (reaction) {                                  /* "React_Front_" + it, "React_Side_L_" + it in a Side move */
        static char names[8][64];
        static int at;
        char *nm = names[at++ & 7];
        snprintf(nm, 64, "%s%s", contains(move, "Side") ? "React_Side_L_" : "React_Front_", reaction);
        EdibleEvent *v = emit(e, EE_REACTION, me, nm, d->pos);
        v->monster = monster;
    }
    float speed = gnum(d->tmpl, "SpeedFactor", 0);
    if (speed != 0) {                                /* setSpeedScale(SpeedFactor, 20) */
        EdibleEvent *v = emit(e, EE_SPEED, me, d->name, d->pos);
        v->monster = monster;
        v->amount = speed;
        v->v[0] = 20;
    }
    if (monster >= 0 && !d->counted) {               /* gCommon.addStats(actor, slot, 3): once (0x40000000) */
        EdibleEvent *v = emit(e, EE_STATS, me, d->name, d->pos);
        v->monster = monster;
        v->level = 3;
        v->tmpl = d->tmpl;
        v->v[0] = gnum(d->tmpl, "Health", 0);
        v->v[1] = gnum(d->tmpl, "Energy", 0);
        v->v[2] = (float)(int)gnum(d->tmpl, "Points", 0);
        d->counted = 1;
    }
    int points = (int)gnum(d->tmpl, "Points", 0);
    if ((int)gnum(d->tmpl, "ShowBanner", 0) && points > 0) {   /* PointsBanner(points, pos, white, 1.0) */
        EdibleEvent *v = emit(e, EE_BANNER, me, d->name, d->pos);
        v->monster = monster;
        v->amount = (float)points;
        v->v[0] = v->v[1] = v->v[2] = v->v[3] = 1;
    }
    const char *message = gstr(d->tmpl, "Message");
    if (message) {                                   /* createAndStartText(message, MessageColor, 0.5, PickupSound) */
        EdibleEvent *v = emit(e, EE_MESSAGE, me, message, d->pos);
        v->monster = monster;
        v->amount = 0.5f;
        v->v[0] = 220.0f / 255; v->v[1] = 0; v->v[2] = 0; v->v[3] = 1;   /* the default: set255(220, 0, 0, 255) */
        Group col = group_sub(d->tmpl, "Color|MessageColor");
        Record c[4];
        uint32_t nc = col.at ? group_items(col, c, 4) : 0;
        /* [R 0x27B4F0 -> 0x1C97F0] getColorProp: three or four numbers (the fourth 255 without one), each cut to an
         * int and its low byte, then / 255; fewer than three keep the default */
        for (uint32_t i = 0; nc >= 3 && i < nc && i < 4; i++)
            v->v[i] = (float)((int)record_num(c[i]) & 0xFF) / 255.0f;
        v->name2 = gstr(d->tmpl, "PickupSound");
    }
    const char *extra = gstr(d->tmpl, "ExtraEffect");
    if (extra) {
        float at[3];
        memcpy(at, d->pos, sizeof at);
        if ((int)gnum(d->tmpl, "ExtraEffectCentered", 0)) {
            /* the building at its place: x, y its position, z = (storeys x 3 + 1.5 + 6) / 2, y - 7 */
            const EdibleSlot *sl = d->slot >= 0 ? &e->slot[d->slot] : NULL;
            float bp[3] = { at[0], at[1] - 2.5f, 0 };
            int size_z = 0;
            if (!(sl && e->host.building_place && e->host.building_place(e->host.ctx, sl->building, bp, &size_z)))
                size_z = 0;                          /* [G] unknown: its front at the edible's x, the street */
            at[0] = bp[0];
            at[1] = bp[1] - 7.0f;
            at[2] = ((float)size_z * 3.0f + 1.5f + 6.0f) / 2.0f;
        } else {
            at[1] -= 6.0f;                           /* its own place, 6 towards the street */
        }
        effect(e, extra, at, -1);
    }
    const char *fn = gstr(d->tmpl, "MonsterFunction");
    if (fn) {
        EdibleEvent *v = emit(e, EE_MONSTER_FUNCTION, me, fn, d->pos);
        v->monster = monster;
    }
    remove_edible(e, d);                             /* returns 16: 0x30C510 takes it out of its slot, 0x800 */
}

/* ---------------------------------------------------------------- the building's side */

void edible_cell_broken(Edibles *e, int building, int storey, int column, int state, int building_state)
{
    if (state < 2)                                   /* [R] 0x317DC0: 0x2CF7C0's state < 2: nothing */
        return;
    int s = find_slot(e, building, storey, column);
    if (s < 0 || e->slot[s].kind < 0)
        return;
    const EdibleKind *k = &e->kind[e->slot[s].kind];
    /* [R] no edible yet: it appears, unless the building is being destroyed (+0x14C >= 4) and it is a hazard */
    if (e->slot[s].item < 0 && (building_state < BSTATE_DESTROYED || !k->hazard))
        spawn(e, s);
    /* [R] the monster reveal's tube: onPickup(gWorld's monster list +0x40 [0], i.e. slot 0, 1) at once (0x318020),
     * whoever broke the cell: its 500 points go to player 1 */
    int it = e->slot[s].item;
    if (it >= 0 && same_text(k->name, "Edible_Cryotube") && e->ed[it].phase == EP_WINDOW)
        tube_pickup(e, &e->ed[it], 0);
}

void edible_floor_fell(Edibles *e, int building, int storey, int above, int below)
{
    for (int i = 0; i < e->nslots; i++) {
        EdibleSlot *s = &e->slot[i];
        if (s->building != building)
            continue;
        if (s->storey == storey) {                   /* [R] 0x317A70: its actors flagged 0x800, the slot emptied */
            if (s->item >= 0)
                remove_edible(e, &e->ed[s->item]);
            s->item = -1;
            s->kind = -1;
        } else if ((s->storey == above && above >= 0) || (s->storey == below && below >= 0)) {
            if (s->kind >= 0 && s->item < 0)         /* [R] the floors next to it: their edibles appear */
                spawn(e, i);
        }
    }
}

/* ---------------------------------------------------------------- each frame */

void edible_update(Edibles *e, float dt)
{
    for (int i = 0; i < e->n; i++) {
        Edible *d = &e->ed[i];
        if (d->phase == EP_GONE)
            continue;
        d->age += dt;
        /* [R 0x30CD20 .. 0x30D1F8] an actor its slot still holds (+0x10), while its building shakes (0x2BABC0): at
         * its cell's place with the shaker's offset (0x2BB7A0 + 2.5, 0x2BAD50), z its storey's floor now (0x2CD010)
         * + its +0xF4. A cell's break shakes the building, so the edible rises to the middle of its storey at once
         * and, with its floor, comes down with the storeys below it */
        float now[3];
        if (d->slot >= 0 && e->slot[d->slot].item == i && e->host.cell_follow &&
            e->host.cell_follow(e->host.ctx, e->slot[d->slot].building, e->slot[d->slot].storey,
                                e->slot[d->slot].column, now)) {
            memcpy(d->pos, now, sizeof d->pos);
            d->pos[2] += LIFT;
        }
        if (d->rotate)                               /* [R 0x31C540] the RotationMover: its rate x the world's step
                                                      * added to the facing (0x101220 / 0x101330): radians a second */
            d->spin = fmodf(d->spin + PI * dt, 2 * PI);
        if (d->phase == EP_FALLING) {                /* the tube's LineMover, at 20 */
            /* [R 0x315E10] the share of the segment grows by speed x dt / its length; past 1 the next segment starts
             * from its beginning (the rest is dropped; callback 3, which Cryotube.bsf ignores), past the last it ends
             * (callback 2) */
            int landed = 0;
            const float *a = d->mv.pts[d->mv.seg], *b = d->mv.pts[d->mv.seg + 1];
            float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
            float len = sqrtf(dx * dx + dy * dy + dz * dz);
            if (len != 0)
                d->mv.t += dt * 20.0f / len;
            if (d->mv.t > 1) {
                if (d->mv.seg < d->mv.n - 2) {
                    d->mv.seg++;
                    d->mv.t = 0;
                } else {
                    d->mv.t = 1;
                    landed = 1;
                }
            }
            if (landed) {                            /* [R] onMoverCallback(2): it landed */
                memcpy(d->pos, d->mv.pts[d->mv.n - 1], sizeof d->pos);
                effect(e, gstr(d->tmpl, "HitGroundFx"), d->pos, -1);
                const char *snd = gstr(d->tmpl, "HitGroundSound");
                if (snd)
                    emit(e, EE_SOUND, i, snd, d->pos);
                emit(e, EE_REVEAL, i, d->name, d->pos);
                place_world(d);
                remove_edible(e, d);
                continue;
            }
            a = d->mv.pts[d->mv.seg];
            b = d->mv.pts[d->mv.seg + 1];
            for (int k = 0; k < 3; k++)
                d->pos[k] = a[k] + (b[k] - a[k]) * d->mv.t;
        }
        if (d->pulse >= 0) {
            d->pulse -= dt;
            if (d->pulse <= 0) {
                if (d->script == EK_CRYOTUBE && d->phase == EP_FALLING) {   /* onPulse: setFacing(fallPitch, 0, 0) */
                    d->pitch += 2 * PI / 24.0f;
                    d->pulse += 1.0f / 24.0f;
                } else {                             /* Edible.bsf onPulse: requestDelete */
                    place_world(d);
                    remove_edible(e, d);
                    continue;
                }
            }
        }
        /* the Default record's Duration: Edible.bsf onAnimComplete stops the effect and returns 4|8|16 (deleted);
         * Cryotube.bsf's returns 1 */
        if (d->script == EK_EDIBLE && d->life > 0 && d->age >= d->life) {
            place_world(d);
            remove_edible(e, d);
            continue;
        }
        place_world(d);
    }
}

int edible_events(Edibles *e, EdibleEvent *out, int max)
{
    int n = e->nev < max ? e->nev : max;
    memcpy(out, e->ev, sizeof *out * (size_t)n);
    if (n < e->nev)
        memmove(e->ev, e->ev + n, sizeof *e->ev * (size_t)(e->nev - n));
    e->nev -= n;
    return n;
}

/* ---------------------------------------------------------------- the monster's side */

static int cell_broken(const Edibles *e, const EdibleSlot *s)
{
    if (!e->host.cell_state)
        return 1;                                    /* the edible appeared when it broke */
    return e->host.cell_state(e->host.ctx, s->building, s->storey, s->column) >= 2;
}

int edible_occupant(const Edibles *e, int building, int storey, int column)
{
    int s = find_slot(e, building, storey, column);
    if (s < 0 || e->slot[s].item < 0 || e->slot[s].kind < 0)
        return EDIBLE_NONE;
    if (!cell_broken(e, &e->slot[s]))               /* [R] 0x2CF7C0 < 2: 0 */
        return EDIBLE_NONE;
    return e->kind[e->slot[s].kind].hazard ? EDIBLE_HAZARD : EDIBLE_POWERUP;
}

const char *edible_occupant_name(const Edibles *e, int building, int storey, int column)
{
    int s = find_slot(e, building, storey, column);
    if (s < 0 || e->slot[s].item < 0)
        return NULL;
    return e->ed[e->slot[s].item].name;              /* [R] the item's +0x88 */
}

int edible_grab(Edibles *e, int building, int storey, int column, int monster, const char *as_type, int eat,
                const char *monster_move)
{
    int s = find_slot(e, building, storey, column);
    if (s < 0 || e->slot[s].item < 0 || !cell_broken(e, &e->slot[s]))
        return 0;
    Edible *d = &e->ed[e->slot[s].item];
    if (d->phase != EP_WINDOW && d->phase != EP_PICKED)
        return 0;
    if (as_type) {                                   /* [R] grabBuildingOccupantAsType: its template (+0x80) replaced */
        Group t = find_template(e, as_type);
        if (t.at)
            d->tmpl = t;
    }
    if (d->script == EK_CRYOTUBE)
        tube_pickup(e, d, monster);
    else
        edible_pickup(e, d, monster, eat, monster_move);
    return 1;
}

/* [R] the monsters' scripts that override Monsters::checkForHazard / doGrabPowerup (getBuildingOccupantName, then
 * grabBuildingOccupantAsType(loc, as, 1)) */
static const struct { const char *ns, *func, *from, *to; } SUBST[] = {
    { "Cal", "checkForHazard", "Hazard_BrokenPipe", "Hazard_Gilman_BrokenPipe" },
    { "Gilman", "checkForHazard", "Hazard_BrokenPipe", "Hazard_Gilman_BrokenPipe" },
    { "Marco", "checkForHazard", "Hazard_BrokenPipe", "Hazard_Gilman_BrokenPipe" },
    { "Natalie", "checkForHazard", "Hazard_BrokenPipe", "Hazard_Gilman_BrokenPipe" },
    { "Jill", "checkForHazard", "Hazard_Shock", "Hazard_Jill_Shock" },
    { "Jill", "checkForHazard", "Hazard_BrokenPipe", "Hazard_Gilman_BrokenPipe" },
    { "Nick", "checkForHazard", "Hazard_Fire", "Hazard_Nick_Fire" },
    { "Nick", "checkForHazard", "Hazard_FireWithPopcorn", "Hazard_Nick_FireWithPopcorn" },
    { "Rock", "checkForHazard", "Hazard_Fire", "Hazard_Rock_Fire" },
    { "Rock", "checkForHazard", "Hazard_FireWithPopcorn", "Hazard_Rock_FireWithPopcorn" },
    { "Venus", "checkForHazard", "Hazard_Fire", "Hazard_Venus_Fire" },
    { "Venus", "checkForHazard", "Hazard_FireWithPopcorn", "Hazard_Venus_FireWithPopcorn" },
    { "Venus", "checkForHazard", "Hazard_Shock", "Hazard_Venus_Shock" },
    { "Venus", "checkForHazard", "Hazard_BrokenPipe", "Hazard_Venus_BrokenPipe" },
    { "Crock", "doGrabPowerup", "Powerup_Cat", "Powerup_Crock_Cat" },
    { "George", "doGrabPowerup", "Powerup_Banana", "Powerup_George_Banana" },
    { "Icky", "doGrabPowerup", "Powerup_Cockroach", "Powerup_Icky_Cockroach" },
    { "Kingston", "doGrabPowerup", "Powerup_Poison", "Powerup_Kingston_Poison" },
    { "Rojo", "doGrabPowerup", "Powerup_Steak", "Powerup_Rojo_Steak" },
    { "Venus", "doGrabPowerup", "Powerup_Radiation", "Powerup_Venus_Radiation" },
    { "Venus", "doGrabPowerup", "Powerup_Biohazard", "Powerup_Venus_Biohazard" },
};

const char *edible_substitute(const char *ns, const char *func, const char *name)
{
    for (size_t i = 0; i < sizeof SUBST / sizeof *SUBST; i++)
        if (same_text(SUBST[i].ns, ns) && same_text(SUBST[i].func, func) && same_text(SUBST[i].from, name))
            return SUBST[i].to;
    return NULL;
}

int edible_monster_call(Edibles *e, const char *call, int building, int storey, int column, int person_there,
                        int monster, const char *monster_move)
{
    if (!call)
        return -1;
    const char *colon = strstr(call, "::"), *func = colon ? colon + 2 : call;
    char ns[32] = "";
    if (colon) {
        size_t len = (size_t)(colon - call) < sizeof ns - 1 ? (size_t)(colon - call) : sizeof ns - 1;
        memcpy(ns, call, len);
        ns[len] = 0;
    }
    int found = building >= 0;
    /* getBuildingOccupant: 1 for a person (its slot's occupant first), else the edible's 2 / 3, else 0 */
    int occ = !found ? 0 : person_there ? 1 : edible_occupant(e, building, storey, column);
    if (same_text(func, "canGrabPowerup"))
        return occ == EDIBLE_POWERUP;
    if (same_text(func, "canDoPunch"))               /* not at a person's or a power-up's window */
        return occ != 1 && occ != EDIBLE_POWERUP;
    int check = same_text(func, "checkForHazard"), grab = same_text(func, "doGrabPowerup"),
        destroy = same_text(func, "destroyPowerup");
    if (!check && !grab && !destroy)
        return -1;
    if (!found || person_there)                      /* grabBuildingOccupant would take the person */
        return 0;
    const char *name = edible_occupant_name(e, building, storey, column);
    const char *as = destroy ? NULL : edible_substitute(ns, func, name);
    if (as)                                          /* [R] AsType, whatever getBuildingOccupant says */
        return edible_grab(e, building, storey, column, monster, as, 1, monster_move);
    if (check)                                       /* [R] getBuildingOccupant == 3: grabBuildingOccupant(loc, 1) */
        return occ == EDIBLE_HAZARD ? edible_grab(e, building, storey, column, monster, NULL, 1, monster_move) : 0;
    return edible_grab(e, building, storey, column, monster, NULL, grab, monster_move);
}

/* ---------------------------------------------------------------- the energy, the rage, the speed */

void edible_power_init(EdiblePower *p)
{
    memset(p, 0, sizeof *p);
    p->speed = 1;
    p->abilities = 0xF;                              /* until the viewer sets them: [R] gCommon.getMonsterFlags,
                                                      * the profile's (main.c profile_moves) */
}

void edible_power_set_energy(EdiblePower *p, float energy, int others_full)
{
    if (energy < 100)                                /* [R] 0x2E5730 */
        p->energy = energy;
    else
        p->energy = others_full ? 99.9f : 100.0f;
}

void edible_power_start_rage(EdiblePower *p, int others_full)
{
    p->rage = 1;                                     /* [R] 0x2D4360: +0xA58 1, +0xA54 25, +0xA8C 1.33, +0xA90 25 */
    p->rage_time = 25;
    p->speed = 1.33f;
    p->speed_time = 25;
    edible_power_set_energy(p, 100, others_full);    /* [R] Monsters::startRage: setEnergy(100) */
}

static void stop_rage(EdiblePower *p)                /* [R] 0x2D4340 */
{
    p->rage = 0;
    p->rage_time = 0;
    p->energy = 0;
    p->speed = 1;
    p->speed_time = 0;
}

int edible_power_add_stats(EdiblePower *p, float *hp, float hp_max, float health, float energy, int others_full)
{
    if (!hp || *hp <= 0)                             /* [R] 0x2C51A0: at 0 HP only the points count */
        return 0;
    float h = *hp + health;
    h = h > 1 ? h : 1;
    *hp = h < hp_max ? h : hp_max;
    float en = p->energy + energy;
    en = en > 0 ? en : 0;
    en = en < 100 ? en : 100;
    if (!p->rage) {
        edible_power_set_energy(p, en, others_full);
        return en >= 100;                            /* -> Monsters::startRage */
    }
    if (energy >= 100) {                             /* raging: only a full refill (Hazard_Rage) starts it over */
        stop_rage(p);
        edible_power_set_energy(p, en, others_full);
        return 1;
    }
    return 0;
}

void edible_power_speed(EdiblePower *p, float scale, float seconds)
{
    p->speed = scale;                                /* [R] 0x2E8650: +0xA8C, +0xA90 */
    p->speed_time = seconds;
}

int edible_power_update(EdiblePower *p, float dt, float hp)
{
    if (p->speed_time > 0) {                         /* [R] 0x2E1CB0: the speed scale's clock, then 1 */
        p->speed_time -= dt;
        if (p->speed_time <= 0) {
            p->speed_time = 0;
            p->speed = 1;
        }
    }
    if (p->rage) {                                   /* [R] the rage's clock; the energy runs down 100 per 25 s, 3 x
                                                      * as fast once the clock is out */
        float step = dt;
        p->rage_time -= dt;
        if (p->rage_time <= EPS) {
            p->rage_time = 0;
            step *= 3;
        }
        p->energy -= 100 * step / 25;
        if (p->energy < EPS)
            p->energy = 0;
        /* [R] 0x2DE530: over when both are out, or at 0 HP */
        if ((p->rage_time <= EPS && p->energy <= EPS) || hp <= 0) {
            stop_rage(p);
            return 1;
        }
    }
    return 0;
}

/* [R Furby.scr, Leon.scr] the energy a roar needs and costs: Monsters.bsf's 15; Furby's own 30, Leon's own 8 (their
 * clips call "Furby::hasRoarEnergy" and the rest) */
static float roar_cost(const char *call)
{
    if (call && !strncmp(call, "Furby::", 7))
        return 30;
    if (call && !strncmp(call, "Leon::", 6))
        return 8;
    return 15;
}

float edible_power_hit_gain(const char *call)       /* [R Dick.scr, Philbert.scr] 2 - 1, 2 + 1.5 */
{
    if (call && !strncmp(call, "Dick::", 6))
        return 2.0f - 1.0f;
    if (call && !strncmp(call, "Philbert::", 10))
        return 2.0f + 1.5f;
    return 2;
}

int edible_power_answer(const EdiblePower *p, const char *call)
{
    if (!call)
        return -1;
    const char *colon = strstr(call, "::"), *f = colon ? colon + 2 : call;
    int all = (p->cheats & 8) != 0;
    if (same_text(f, "hasRoarEnergy"))               /* [R] Monsters.bsf, Furby.bsf, Leon.bsf */
        return (p->energy >= roar_cost(call) && ((p->abilities & 4) || all)) || p->rage;
    if (same_text(f, "hasRoarAttack"))
        return (p->abilities & 4) || all || p->rage;
    if (same_text(f, "hasChargePunch"))
        return (p->abilities & 1) || all || p->rage;
    if (same_text(f, "hasJumpSmash"))
        return (p->abilities & 8) || all || p->rage;
    if (same_text(f, "canStartRage"))                /* [R] 0x2ABF00: full, and no monster raging */
        return p->energy >= 100 && !p->rage;
    if (same_text(f, "inRageMode"))
        return p->rage;
    return -1;
}

void edible_power_use_roar(EdiblePower *p, const char *call)
{
    float cost = roar_cost(call);
    if (!p->rage)                                    /* [R] useRoarEnergy */
        p->energy = p->energy > cost ? p->energy - cost : 0;
}

int edible_power_hit_building(EdiblePower *p, int energy_gain, float amount, int airborne, int others_full)
{
    if (!energy_gain || p->rage)                     /* [R] Monsters::hitBuildingFront */
        return 0;
    if (p->energy + amount >= 100 && airborne) {
        edible_power_set_energy(p, 99, others_full);
        return 0;
    }
    edible_power_set_energy(p, p->energy + amount, others_full);   /* addEnergy (0x2E5720) */
    return p->energy >= 100;
}
