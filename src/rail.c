/* rail.c - the elevated trains, the New York freight trains, the cable cars and the freighters (rail.h;
 * docs/RAIL_NOTES.md). [R] marks what is read from the game's code or scripts, [G] what is a guess or a stand-in. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rail.h"

#define SPI 3.1415927f                               /* the scripts' own pi */
#define TIMESTEP (1.0f / 30.0f)                      /* gWorld.getTimeStep(): its step, 1/30 [R 0x2B7800] */
#define MAX_OBJS 128
#define MAX_EVENTS 512
#define MAX_SPAWN 8

enum { MV_NONE, MV_LINE, MV_SPLINE, MV_SPHERE, MV_HELD };

typedef struct {                                     /* an AIInstance record (0x3350F0, as army.c's) [R] */
    const char *name;
    Group g;
    float min_delay, respawn, offset, timer;
    int left, alive, enabled;
} Spawner;

struct Rail {
    RailHost host;
    Army *army;
    RailObj o[MAX_OBJS]; int n;
    RailEvent ev[MAX_EVENTS]; int nev, next_fx, next_sfx;
    uint32_t rng, next_uid;
    Spawner sp[MAX_SPAWN]; int nsp, active;
    /* the scripts' globals */
    int et_given_left, et_given_right;               /* ElevatedTrain.bsf hasGivenStatsLeftGroup / RightGroup */
    int et_vehicles, et_global_id, ny_vehicles, loop_trains;
    float time;                                      /* gWorld.curTime() */
    /* this frame */
    const ArmyMonster *m; int nm;
    const RailBox *b; int nb;
    float acc;
    CollideWorld *cw;                                /* the frame's collision world (rail_set_collide), NULL none */
};

/* ---------------------------------------------------------------- small things */

static float frand(Rail *r)                          /* [G] gWorld.frand(): [0, 1), not the game's Park-Miller (0x106BD0) */
{
    r->rng = r->rng * 1664525u + 1013904223u;
    return (float)(r->rng >> 8) / 16777216.0f;
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
static int contains(const char *s, const char *what) { return s && strstr(s, what) != NULL; }   /* String.fFind >= 0 */

static void copy3(float d[3], const float s[3]) { memcpy(d, s, 3 * sizeof(float)); }
static void set3(float d[3], float x, float y, float z) { d[0] = x; d[1] = y; d[2] = z; }
static float len3(const float a[3]) { return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
static float dist3(const float a[3], const float b[3])
{
    float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return len3(d);
}
static void norm3(float a[3])                        /* Point3.fastNormalize */
{
    float l = len3(a);
    if (l < 1e-6f) {                                 /* [R] the game's normalize of a null vector gives (1, 0, 0) */
        set3(a, 1, 0, 0);
    } else {
        a[0] /= l;
        a[1] /= l;
        a[2] /= l;
    }
}

/* 3 x 3 column-major: m[c * 3 + r] */
static void rot_z(float r[9], float a)
{
    float c = cosf(a), s = sinf(a);
    r[0] = c; r[1] = s; r[2] = 0;
    r[3] = -s; r[4] = c; r[5] = 0;
    r[6] = 0; r[7] = 0; r[8] = 1;
}
/* setFacing(x, y, z): Rz(z) x Rx(x) x Ry(y) [R 0x277E78 -> 0x101330(m, z, x, y)]; only the freighter's sway uses x
 * and y. rot's rows are the object's axes */
static void rot_of_facing(float rot[9], const float f[3])
{
    float ch = cosf(f[2]), sh = sinf(f[2]), cx = cosf(f[0]), sx = sinf(f[0]), cy = cosf(f[1]), sy = sinf(f[1]);
    float r[9] = { ch * cy - sh * sx * sy, sh * cy + ch * sx * sy, -cx * sy,
                   -sh * cx, ch * cx, sx,
                   ch * sy + sh * sx * cy, sh * sy - ch * sx * cy, cx * cy };
    memcpy(rot, r, sizeof r);
}

static void update_world(RailObj *o)
{
    float *w = o->world;
    for (int c = 0; c < 3; c++) {
        for (int k = 0; k < 3; k++)
            w[c * 4 + k] = o->rot[c * 3 + k];
        w[c * 4 + 3] = 0;
    }
    w[12] = o->pos[0];
    w[13] = o->pos[1];
    w[14] = o->pos[2];
    w[15] = 1;
}
static void set_facing(RailObj *o, const float f[3])   /* setFacing: the turn follows the facing again */
{
    copy3(o->facing, f);
    o->own_rot = 0;
    rot_of_facing(o->rot, o->facing);
}
static void set_facing_z(RailObj *o, float z)        /* setFacingZ */
{
    float f[3] = { o->facing[0], o->facing[1], z };
    set_facing(o, f);
}

/* ---------------------------------------------------------------- properties */

/* getStringProp / getFloatProp read the instance book (the ObjectInstance or AIInstance group) first, then the
 * template; getTemplate*Prop the template only [R, as army.c]. The books keep a record's type before a '|'
 * ("Effect|DamageEffect", "Sound|HornSound", "OTRef|CarType"); the scripts ask by the bare name, so the typed names are
 * tried too. Lookups follow ParentPropertySet (DefObjectProps: Gravity 35, Points 10) */
static const char *const TYPED[] = { "", "Effect|", "Sound|", "Shape|", "OTRef|", "Script|" };

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
static const char *gstr(Group g, const char *key)    /* "" and "StringNotFound" count as absent */
{
    char k[96];
    if (!g.at || !key)
        return NULL;
    for (size_t i = 0; i < sizeof TYPED / sizeof *TYPED; i++) {
        snprintf(k, sizeof k, "%s%s", TYPED[i], key);
        const char *s = group_str(g, k);
        if (s)
            return *s && strcmp(s, "StringNotFound") ? s : NULL;
    }
    return NULL;
}
static int has_num(Group g, const char *key, float *v)
{
    Record r;
    if (find_rec(g, key, &r) && (r.kind & 0xFF) <= PK_FLOAT) {
        *v = record_num(r);
        return 1;
    }
    return 0;
}
static float gnum(Group g, const char *key, float def)
{
    float v;
    return has_num(g, key, &v) ? v : def;
}
static const char *tstr(const RailObj *o, const char *key) { return gstr(o->tmpl, key); }
static float tnum(const RailObj *o, const char *key, float def) { return gnum(o->tmpl, key, def); }
static const char *pstr(const RailObj *o, const char *key)
{
    const char *s = gstr(o->inst, key);
    return s ? s : gstr(o->tmpl, key);
}
static float pnum(const RailObj *o, const char *key, float def)
{
    float v;
    return has_num(o->inst, key, &v) ? v : gnum(o->tmpl, key, def);
}

static Group find_template(Rail *r, const char *name)
{
    return r->host.find && name ? r->host.find(r->host.ctx, "Objects1_Props", name) : (Group){ 0 };
}

static int kind_of_script(const char *s)
{
    static const struct { const char *s; int k; } K[] = {
        { "ElevatedTrainController.bsf", RK_ETRAIN_CTRL }, { "ElevatedTrain.bsf", RK_ETRAIN },
        { "NewYorkTrainController.bsf", RK_NYTRAIN_CTRL }, { "NewYorkTrain.bsf", RK_NYTRAIN },
        { "CableCarController.bsf", RK_CABLE_CTRL }, { "CableCar.bsf", RK_CABLECAR },
        { "freighter.bsf", RK_FREIGHTER }, { "Train.bsf", RK_LOOPTRAIN } };
    for (size_t i = 0; i < sizeof K / sizeof *K; i++)
        if (same_text(s, K[i].s))
            return K[i].k;
    return -1;
}
static int kind_of_template(Group t) { return t.at ? kind_of_script(gstr(t, "ScriptName")) : -1; }
static int is_controller(int kind) { return kind == RK_ETRAIN_CTRL || kind == RK_NYTRAIN_CTRL || kind == RK_CABLE_CTRL; }

/* ---------------------------------------------------------------- events */

static int idx(const Rail *r, const RailObj *o) { return (int)(o - r->o); }

static RailEvent *emit(Rail *r, int type, int obj, const char *name, const float pos[3])
{
    static RailEvent spare;
    if (r->nev >= MAX_EVENTS) {
        memset(&spare, 0, sizeof spare);
        return &spare;
    }
    RailEvent *e = &r->ev[r->nev++];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->obj = obj;
    e->monster = -1;
    e->name = name;
    e->dir[2] = 1;
    if (pos)
        copy3(e->pos, pos);
    return e;
}

/* FollowEffect.createAndStart(pos, name): where it is, on its own */
static int effect_at(Rail *r, const char *name, const float pos[3])
{
    if (!name)
        return -1;
    RailEvent *e = emit(r, RE_EFFECT, -1, name, pos);
    return e->id = r->next_fx++;
}
/* FollowEffect.createAndStartAttached(object, name) / ...WithOffset(object, name, offset): [G] at the object's
 * place + offset (in its frame [R 0x27A990, 0x291440]), following it; turn_off is
 * setTurnOffWhenObjectDies(1): it stops when the object goes */
static int effect_on(Rail *r, RailObj *o, const char *name, const float off[3], int turn_off)
{
    if (!name)
        return -1;
    float at[3] = { o->pos[0], o->pos[1], o->pos[2] };
    if (off)                                         /* [R 0x27A990, 0x291440] the offset in the object's frame */
        for (int k = 0; k < 3; k++)
            at[k] += off[0] * o->rot[k] + off[1] * o->rot[3 + k] + off[2] * o->rot[6 + k];
    RailEvent *e = emit(r, RE_EFFECT, idx(r, o), name, at);
    copy3(e->dir, o->rot + 3);                       /* [G] it takes the object's turn: its +Y along the object's */
    e->id = r->next_fx++;
    if (turn_off && o->nfx_off < 6)
        o->fx_off[o->nfx_off++] = e->id;
    return e->id;
}
static void effect_stop(Rail *r, RailObj *o, int id)   /* FollowEffect.stop */
{
    if (id < 0)
        return;
    emit(r, RE_EFFECT_STOP, -1, NULL, NULL)->id = id;
    if (o)
        for (int i = 0; i < o->nfx_off; i++)
            if (o->fx_off[i] == id) {
                o->fx_off[i] = o->fx_off[--o->nfx_off];
                break;
            }
}
static int sfx_add(Rail *r, RailObj *o, const char *name)   /* add3DSfx(name, 0): its index, -1 none */
{
    if (!name)
        return -1;
    RailEvent *e = emit(r, RE_SOUND, idx(r, o), name, o->pos);
    e->id = r->next_sfx++;
    if (o->nsfx < 6)
        o->sfx[o->nsfx++] = e->id;
    return e->id;
}
static void sfx_remove(Rail *r, RailObj *o, int id)   /* remove3DSfx(index) */
{
    if (id < 0)
        return;
    for (int i = 0; i < o->nsfx; i++)
        if (o->sfx[i] == id) {
            o->sfx[i] = o->sfx[--o->nsfx];
            emit(r, RE_SOUND_STOP, idx(r, o), NULL, o->pos)->id = id;
            return;
        }
}
static void set_shape(Rail *r, RailObj *o, const char *shape)   /* setShape */
{
    if (!shape)
        return;
    o->shape = shape;
    emit(r, RE_SHAPE, idx(r, o), shape, o->pos);
}

/* gCommon.addStats(obj, slot, type) (0x2C51A0) [R]: once per object (flag 0x40000000); the gains and the points go
 * through army.c's addStats when an Army is bound (as vehicle.c) */
static void add_stats(Rail *r, RailObj *o, int monster, int type)
{
    if (o->paid || monster < 0)
        return;
    o->paid = 1;
    RailEvent *e = emit(r, RE_STATS, idx(r, o), o->name, o->pos);
    e->monster = monster;
    e->level = type;
    e->amount = (float)(int)tnum(o, "Points", 0);
    e->v[0] = tnum(o, "Health", 0);
    e->v[1] = tnum(o, "Energy", 0);
    if (r->army) {
        army_add_stats(r->army, monster, o->tmpl, type);
        e->id = 1;
    }
}

/* the monster's side of a script call */
static void monster_knock(Rail *r, RailObj *o, int m, const float vel[3], float damage)
{
    RailEvent *e = emit(r, RE_SET_VEL, idx(r, o), NULL, o->pos);   /* m.setVel(vel, 0) */
    e->monster = m;
    copy3(e->v, vel);
    e = emit(r, RE_REACTION, idx(r, o), NULL, o->pos);   /* m.setReaction(2): a knock-down */
    e->monster = m;
    e->level = 2;
    e = emit(r, RE_HP, idx(r, o), NULL, o->pos);          /* m.addHitPoints(-damage) */
    e->monster = m;
    e->amount = -damage;
}

/* getNearestMonster: [G] the nearest by 3D distance; -1 with none */
static int nearest_monster(const Rail *r, const float p[3])
{
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < r->nm; i++) {
        float d = dist3(p, r->m[i].pos);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* gWorld.findBuildingByPosition (0x2AEAA0 -> 0x2A3D20 on the world's list at 0x4D0900, a zero direction) [R]: the
 * last building whose box holds the point in x and y, ends included, whatever its height and state (the trains ask
 * at their own height, z 10, and need a destroyed building to be found); -1 none */
static int building_at(const Rail *r, const float p[3])
{
    for (int i = r->nb - 1; i >= 0; i--) {
        const RailBox *b = &r->b[i];
        if (p[0] >= b->min[0] && p[0] <= b->max[0] && p[1] >= b->min[1] && p[1] <= b->max[1])
            return i;
    }
    return -1;
}

/* ---------------------------------------------------------------- the objects */

typedef struct { int i; uint32_t uid; } Ref;
static Ref ref_of(const Rail *r, const RailObj *o)
{
    Ref f = { o ? idx(r, o) : -1, o ? o->uid : 0 };
    return f;
}
static RailObj *deref(Rail *r, int i, uint32_t uid)    /* ScriptObject.isValid */
{
    if (i < 0 || i >= r->n || r->o[i].phase == RP_GONE || r->o[i].uid != uid)
        return NULL;
    return &r->o[i];
}
#define DEREF(f) deref(r, (f).i, (f).uid)
#define SETREF(f, o) do { Ref t_ = ref_of(r, o); (f).i = t_.i; (f).uid = t_.uid; } while (0)

static RailObj *obj_new(Rail *r, const char *name, Group tmpl, int kind)
{
    int k = -1;
    for (int i = 0; i < r->n; i++)
        if (r->o[i].phase == RP_GONE && r->o[i].gone_age >= 2) {   /* the caller has seen it go */
            k = i;
            break;
        }
    if (k < 0) {
        if (r->n >= MAX_OBJS)
            return NULL;
        k = r->n++;
    }
    RailObj *o = &r->o[k];
    memset(o, 0, sizeof *o);
    o->name = name;
    o->tmpl = tmpl;
    o->kind = kind;
    o->phase = RP_ALIVE;
    o->uid = ++r->next_uid;
    o->spawner = -1;
    o->cobj = -1;
    o->next.i = -1;
    o->ambient_sfx = o->stopping_sfx = o->sparks_fx = o->ambient_fx = o->light_fx = -1;
    o->shape = gstr(tmpl, "Shape");
    o->hidden = is_controller(kind) || same_text(o->shape, "InvisShape");
    rot_z(o->rot, 0);
    update_world(o);
    return o;
}

/* requestDelete: its 3D sounds end with it [G] and its turn-off effects stop [R] */
static void obj_delete(Rail *r, RailObj *o)
{
    if (o->phase == RP_GONE)
        return;
    for (int i = 0; i < o->nsfx; i++)
        emit(r, RE_SOUND_STOP, idx(r, o), NULL, o->pos)->id = o->sfx[i];
    o->nsfx = 0;
    for (int i = 0; i < o->nfx_off; i++)
        emit(r, RE_EFFECT_STOP, -1, NULL, NULL)->id = o->fx_off[i];
    o->nfx_off = 0;
    o->phase = RP_GONE;
    o->gone_age = 0;
    o->oflags = 0;
    o->mv = MV_NONE;
    emit(r, RE_REMOVED, idx(r, o), o->name, o->pos);
}

/* setPulseDelay: [G] the interval changes, the clock since the last pulse runs on (pulse_timer counts up). A cable
 * car sets 0.375 at every touch of the street, which would hold its dying off for as long as it slides if the call
 * restarted the clock (vehicle.c's reading) */
static void set_pulse(RailObj *o, float delay) { o->pulse_delay = delay; }

/* ---------------------------------------------------------------- the movers */

/* LineMover.init(obj, a, b, speed, share) (0x3173D0, 0x317650) [R, as vehicle.c]: +0x54 the speed, +0x58 the share of
 * the way done, +0x5C MonsterResponseDist squared from the object's template, or -1. setFlags(1): it faces the way it
 * goes [R, BOSS_NOTES] */
static void line_init(RailObj *o, const float a[3], const float b[3], float speed, float share)
{
    float aa[3], bb[3];
    copy3(aa, a);
    copy3(bb, b);
    copy3(o->line.a, aa);
    copy3(o->line.b, bb);
    o->line.speed = speed;
    o->line.t = share;
    o->line.flags = 0;
    float rr = tnum(o, "MonsterResponseDist", -1);
    o->line.resp2 = rr >= 0 ? rr * rr : -1;
    for (int k = 0; k < 3; k++)
        o->pos[k] = aa[k] + (bb[k] - aa[k]) * share;
    o->mv = MV_LINE;
}

/* the LineMover's frame (0x315E10) [R]: the share grows by dt x speed / length; past 1 it stops at the end and calls
 * back 2 [G: once]; while it simply moves, a monster nearer than MonsterResponseDist (3D) calls back 4. Returns the
 * callback, 0 none */
static int line_step(Rail *r, RailObj *o, float dt)
{
    float d[3] = { o->line.b[0] - o->line.a[0], o->line.b[1] - o->line.a[1], o->line.b[2] - o->line.a[2] };
    float len = len3(d);
    int cb = 1;
    if (len != 0)
        o->line.t += dt * o->line.speed / len;
    if (o->line.t > 1) {
        o->line.t = 1;
        o->mv = MV_NONE;
        cb = 2;
    }
    for (int k = 0; k < 3; k++)
        o->pos[k] = o->line.a[k] + d[k] * o->line.t;
    if ((o->line.flags & 1) && (d[0] != 0 || d[1] != 0))
        set_facing_z(o, atan2f(-d[0], d[1]));
    if (cb == 1 && o->line.resp2 > 0)
        for (int i = 0; i < r->nm; i++) {
            float e[3] = { o->pos[0] - r->m[i].pos[0], o->pos[1] - r->m[i].pos[1], o->pos[2] - r->m[i].pos[2] };
            if (e[0] * e[0] + e[1] * e[1] + e[2] * e[2] < o->line.resp2)
                cb = 4;
        }
    return cb == 1 ? 0 : cb;
}

/* SplineMover (vtable 0x44B800, update 0x33ACA0; boss.c has the same) [R]: points with their distances along the
 * path (addPoint(p, d), addPointWithVel(p, d, k) [R 0x33B010: k the point's tension, the tangent x (1 - k)]), the place along it grows by
 * velocity x dt; past the last point it stays there and answers 2 every frame; flag 2 turns the object along the
 * tangent. The curve (0x13B780, tangents 0x13B2F0) is Hermite with Kochanek-Bartels tangents, T = B = C = 0 */
static void sp_clear(RailObj *o)                     /* createSplineMover + clearPath [G: the place starts at 0] */
{
    memset(&o->sp, 0, sizeof o->sp);
    o->mv = MV_SPLINE;
}
static void sp_add(RailObj *o, const float p[3], float d, float k)
{
    if (o->sp.n >= 8)
        return;
    copy3(o->sp.pt[o->sp.n].p, p);
    o->sp.pt[o->sp.n].s = d;
    o->sp.pt[o->sp.n].k = k;
    o->sp.n++;
    o->sp.ready = 0;
}
static void sp_prepare(RailObj *o)
{
    int n = o->sp.n;
    int closed = n > 2 && memcmp(o->sp.pt[0].p, o->sp.pt[n - 1].p, 3 * sizeof(float)) == 0;
    for (int i = 0; i < n; i++) {
        int pv = i - 1, nx = i + 1;
        if (pv < 0)
            pv = closed ? n - 2 : 0;
        if (nx >= n)
            nx = closed ? 1 : n - 1;
        for (int k = 0; k < 3; k++)
            o->sp.pt[i].m[k] = 0.5f * (1 - o->sp.pt[i].k) *
                               ((o->sp.pt[i].p[k] - o->sp.pt[pv].p[k]) + (o->sp.pt[nx].p[k] - o->sp.pt[i].p[k]));
    }
    o->sp.ready = 1;
}
static void sp_eval(RailObj *o, float u, float pos[3], float tan[3])
{
    if (!o->sp.ready)
        sp_prepare(o);
    int n = o->sp.n;
    set3(tan, 0, 0, 0);
    if (n == 0)
        return;
    if (n == 1 || u <= o->sp.pt[0].s) {
        copy3(pos, o->sp.pt[0].p);
        copy3(tan, o->sp.pt[0].m);
        return;
    }
    if (u >= o->sp.pt[n - 1].s) {
        copy3(pos, o->sp.pt[n - 1].p);
        copy3(tan, o->sp.pt[n - 1].m);
        return;
    }
    int i = 0;
    while (i < n - 2 && u >= o->sp.pt[i + 1].s)
        i++;
    float len = o->sp.pt[i + 1].s - o->sp.pt[i].s, t = len > 0 ? (u - o->sp.pt[i].s) / len : 1;
    float t2 = t * t, t3 = t2 * t;
    float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
    float d00 = 6 * t2 - 6 * t, d10 = 3 * t2 - 4 * t + 1, d01 = -6 * t2 + 6 * t, d11 = 3 * t2 - 2 * t;
    for (int k = 0; k < 3; k++) {
        float p0 = o->sp.pt[i].p[k], p1 = o->sp.pt[i + 1].p[k], m0 = o->sp.pt[i].m[k], m1 = o->sp.pt[i + 1].m[k];
        pos[k] = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
        tan[k] = d00 * p0 + d10 * m0 + d01 * p1 + d11 * m1;
    }
}
static int sp_step(RailObj *o, float dt)
{
    if (o->sp.n == 0)
        return 0;
    int cb = 0;
    float end = o->sp.pt[o->sp.n - 1].s;
    o->sp.s += o->sp.vel * dt;
    if (o->sp.s > end) {
        o->sp.s = end;
        cb = 2;
    }
    float tan[3];
    sp_eval(o, o->sp.s, o->pos, tan);
    if (o->sp.flags & 2) {                           /* +Y along the tangent, +X = tangent x Z, +Z = X x tangent */
        if (len3(tan) < 1e-6f)
            set3(tan, 1, 0, 0);
        norm3(tan);
        float x[3] = { tan[1], -tan[0], 0 };
        if (len3(x) < 1e-6f)
            set3(x, 1, 0, 0);
        norm3(x);
        float z[3] = { x[1] * tan[2] - x[2] * tan[1], x[2] * tan[0] - x[0] * tan[2], x[0] * tan[1] - x[1] * tan[0] };
        copy3(o->rot, x);
        copy3(o->rot + 3, tan);
        copy3(o->rot + 6, z);
        o->own_rot = 1;
        set3(o->facing, 0, 0, atan2f(-tan[0], tan[1]));
    }
    return cb;
}

/* SphereMover (setVelocity, setRotation): the game's SpherePhysicsMover (collide.c, 0x28E440) with the template's
 * Gravity (DefObjectProps: 35), Mass, Restitution, SlidingFriction and InertiaSolidity (0x28F220); setRotation's
 * turn as radians a second about the object's own x, y, z; the street (OnHitGround every sub-step on it) and the
 * frame's objects: a track building's roof or front is met as its box, and TakeImpact goes both ways (0x287430) */
static void rail_take_impact(Rail *r, RailObj *o);
static void hit_ground(Rail *r, RailObj *o);
static void obj_shape(const Rail *r, RailObj *o, CollideShape *s);
static void sphere_init(RailObj *o, const float vel[3], const float spin[3])
{
    copy3(o->sphere.vel, vel);
    if (spin)
        copy3(o->sphere.spin, spin);
    else
        set3(o->sphere.spin, 0, 0, 0);
    o->sphere.tumble = 1;
    o->own_rot = 1;
    o->cm_on = 0;
    o->mv = MV_SPHERE;
}

typedef struct { Rail *r; RailObj *o; } FlightCtx;

static void flight_contact(void *user, int other, const float p[3], const float n[3])
{
    FlightCtx *c = user;
    Rail *r = c->r;
    RailObj *o = c->o;
    if (o->phase == RP_GONE)
        return;
    if (other < 0) {
        if (getenv("RAMPAGE_TRACE") && o->cm.vel[2] > 0.5f)   /* a bounce (the impulse is in) */
            printf("  %s bounced off the street at %.1f %.1f %.1f\n", o->name ? o->name : "?", p[0], p[1], p[2]);
        hit_ground(r, o);                            /* OnHitGround (event 11) */
        return;
    }
    if (!r->cw)
        return;
    const CollideObj *w = &r->cw->o[other];
    if (getenv("RAMPAGE_TRACE"))
        printf("  %s met %d.%d at %.1f %.1f %.1f\n", o->name ? o->name : "?", COLLIDE_TAG_OWNER(w->tag),
               COLLIDE_TAG_INDEX(w->tag), p[0], p[1], p[2]);
    rail_take_impact(r, o);                          /* TakeImpact (event 10) on both */
    if (COLLIDE_TAG_OWNER(w->tag) == COLLIDE_OWN_RAIL) {
        int j = COLLIDE_TAG_INDEX(w->tag);
        if (j < r->n && j != idx(r, o) && r->o[j].phase == RP_ALIVE)
            rail_take_impact(r, &r->o[j]);
        return;
    }
    RailEvent *e = emit(r, RE_IMPACT, idx(r, o), o->name, p);
    e->id = w->tag;
    e->level = w->kind;
    copy3(e->dir, n);
    e->amount = o->cm.mass;
}

static void sphere_step(Rail *r, RailObj *o, float dt)
{
    if (!o->cm_on) {
        CollideShape sh;
        obj_shape(r, o, &sh);
        float rad = collide_mover_radius(&sh);
        if (rad < 0.05f)
            rad = sh.radius > 0.05f ? sh.radius : 1;
        collide_mover_init(&o->cm, rad, tnum(o, "Gravity", 9.81f), tnum(o, "Mass", 1), tnum(o, "Restitution", 0.8f),
                           tnum(o, "SlidingFriction", 0.4f), tnum(o, "InertiaSolidity", 1));
        float w[3];
        for (int k = 0; k < 3; k++)                  /* [R 0x28DD80, 0x28E440] setRotation(r): |r| radians a second */
            w[k] = o->sphere.tumble ? o->sphere.spin[k] : 0;   /* about the world axis r / |r| */
        collide_mover_spin(&o->cm, w);
        memcpy(o->cmat, o->rot, sizeof o->rot);
        copy3(o->cmat + 9, o->pos);
        o->cm_on = 1;
    }
    copy3(o->cm.vel, o->sphere.vel);
    int self = collide_check(r->cw, o->cobj, COLLIDE_TAG(COLLIDE_OWN_RAIL, idx(r, o)), 0);
    FlightCtx c = { r, o };
    collide_mover_step_ex(r->cw, &o->cm, o->cmat, self, dt, COLLIDE_F_MONSTER, flight_contact, &c);
    copy3(o->sphere.vel, o->cm.vel);
    memcpy(o->rot, o->cmat, sizeof o->rot);
    copy3(o->pos, o->cmat + 9);
}

static RailObj *create(Rail *r, const char *name);   /* gWorld.createScriptObject */

/* ---------------------------------------------------------------- ElevatedTrain.bsf */

/* this.flags [R]: 1 the crash sound played, 2 the stopping sound on, 4 the explosion played, 8 the fire on, 16 it pulls
 * a next car, 32 it runs to -x (faces left), 64 a speeding train, 128 it knocks monsters down (the lead car of a
 * speeding chain), 256 its slow-down path made, 2048 the lead car (the horn, the watch for a broken track), 4096 it has
 * touched the street, 16384 the sparks on. this.state: 0 new, 1 running, 2 braking, 3 stopped, 5 crashing, 6 held,
 * 8 exploding (7 is tested, never set) */
enum { ET_FAST = 0, ET_SLOW = 1 };

/* Construct [R]: pulses every 0.15 s; the object flags 0x2000000 | PushOutMonsters | 0x10000000, Pickupable unless it
 * knocks monsters down (getIntProp KnockDownMonster: the speeding train); HitPoints (5; the slow train 1, the speeding
 * one 1000), 2 s to die once it crashes */
static void et_construct(Rail *r, RailObj *o)
{
    o->state = 0;
    o->horn = 0;
    o->id = r->et_vehicles++;
    set_pulse(o, 0.15f);
    o->oflags |= 0x2000000 | RF_PUSH_OUT | 0x10000000;
    if (pnum(o, "KnockDownMonster", 0) == 0)
        o->oflags |= RF_PICKUP;
    o->hp = (int)tnum(o, "HitPoints", 5);
    o->die = 2;
    o->t0 = r->time;                                 /* creationTime */
    if (tnum(o, "KnockDownMonster", 0) != 0)
        o->flags |= 128;
    o->id = r->et_global_id++;
}

/* spawnTrainChain(n) [R]: the next car 14 behind (x + 14 when it runs to -x, else x - 14), facing the same way, in the
 * same state, with the same flags but neither the lead's 2048 nor 128; it makes its own next car, n - 1. Making a
 * chain clears the side's "stats given" */
static int et_spawn_chain(Rail *r, RailObj *o, int n)
{
    if (n < 1)
        return 0;
    RailObj *t = create(r, o->train_type);
    if (!t)
        return 0;
    float loc[3] = { o->pos[0], o->pos[1], o->pos[2] };
    if (o->flags & 32) {
        r->et_given_left = 0;
        loc[0] += 14;
    } else {
        r->et_given_right = 0;
        loc[0] -= 14;
    }
    copy3(t->pos, loc);
    set_facing(t, o->facing);
    t->state = o->state;
    t->flags |= o->flags;
    t->flags &= ~2048;
    t->flags &= ~128;
    t->train_type = o->train_type;
    update_world(t);
    et_spawn_chain(r, t, n - 1);
    SETREF(o->next, t);
    o->flags |= 16;
    return 1;
}

/* ElevatedTrainController's pushLeftTrainStack / pushRightTrainStack [R]: the trains running to -x go on the back
 * stack when the controller's 32 is set, else the front one; the others the other way */
static void etc_push(Rail *r, RailObj *c, RailObj *t, int left)
{
    int back = (c->flags & 32) ? left : !left;
    if (back) {
        if (c->nlink2 < 8)
            SETREF(c->link2[c->nlink2++], t);
        c->flags |= 512;
    } else {
        if (c->nlink < 8)
            SETREF(c->link[c->nlink++], t);
        c->flags |= 256;
    }
}
static void et_reg(Rail *r, RailObj *o, RailObj *c)  /* regWithController: itself, then its chain */
{
    etc_push(r, c, o, (o->flags & 32) != 0);
    RailObj *n = DEREF(o->next);
    if (n)
        et_reg(r, n, c);
}

static void et_crash(Rail *r, RailObj *o);
static void et_crash_chain(Rail *r, RailObj *o)      /* eventCrashChain [R]: the chain behind first, its DamageEffect */
{
    RailObj *n = DEREF(o->next);
    if ((o->flags & 16) && n)
        et_crash_chain(r, n);
    if (o->phase == RP_GONE)
        return;
    effect_on(r, o, tstr(o, "DamageEffect"), NULL, 0);
    et_crash(r, o);
}
static int et_damage(Rail *r, RailObj *o, int dmg, int chain)   /* eventDamage [R]: 1 HP off this car and those behind */
{
    (void)dmg;
    RailObj *n = DEREF(o->next);
    if (chain && (o->flags & 16) && n)
        et_damage(r, n, dmg, chain);
    o->hp -= 1;
    if (o->hp < 1) {
        et_crash_chain(r, o);
        return 1;
    }
    return 0;
}

static int et_near_monster(Rail *r, RailObj *o)      /* isNearMonster [R]: the nearest monster within 60 */
{
    int m = nearest_monster(r, o->pos);
    return m >= 0 && dist3(o->pos, r->m[m].pos) < 60;
}
static void et_horn(Rail *r, RailObj *o) { sfx_add(r, o, pstr(o, "HornSound")); }

/* playSparks [R]: once (16384), the template's SparksEffect (Train_Sparks) on it, 0.6 up and, running to +x, 2 ahead
 * [G: the offset taken in the world], turned off when it goes */
static void et_sparks(Rail *r, RailObj *o)
{
    if (o->flags & 16384)
        return;
    if (o->sparks_fx >= 0)
        effect_stop(r, o, o->sparks_fx);
    float off[3] = { 0, 0, 0.6f };
    if ((o->flags & 32) == 0)
        off[0] = 2.0f;
    const char *s = tstr(o, "SparksEffect");
    o->sparks_fx = effect_on(r, o, s, off, 1);
    o->flags |= 16384;
}
static void et_stop_sparks(Rail *r, RailObj *o)
{
    if (o->flags & 16384) {
        if (o->sparks_fx >= 0)
            effect_stop(r, o, o->sparks_fx);
        o->sparks_fx = -1;
        o->flags &= ~16384;
    }
}
static void et_stopping_sound(Rail *r, RailObj *o)   /* playStoppingSound [R]: StoppingSound (MetalCreak_RND), once */
{
    if (o->flags & 2)
        return;
    const char *s = pstr(o, "StoppingSound");
    if (s) {
        o->stopping_sfx = sfx_add(r, o, s);
        o->flags |= 2;
    }
}
static void et_kill_stopping(Rail *r, RailObj *o)    /* killStoppingSound [R]: the stopping and the rolling sound end */
{
    if (o->flags & 2) {
        sfx_remove(r, o, o->stopping_sfx);
        sfx_remove(r, o, o->ambient_sfx);
        o->stopping_sfx = o->ambient_sfx = -1;
        o->flags &= ~2;
    }
}

static int bad_building(const Rail *r, const float p[3])   /* isDestroyed() || isFalling() */
{
    int b = building_at(r, p);
    return b >= 0 && (r->b[b].destroyed || r->b[b].falling);
}
/* findBrokenTrack [R]: 5 points, 4 apart, up to 20 ahead; the first within x -49 .. 49 over a destroyed or falling
 * building: its distance, else -1 */
static float et_find_broken(Rail *r, RailObj *o)
{
    float end[3] = { o->pos[0] + ((o->flags & 32) ? -20.0f : 20.0f), o->pos[1], o->pos[2] };
    int loop = (int)(20.0f / 4.0f);
    for (int i = 1; i <= loop; i++) {
        float a = (float)i / (float)loop, cur[3];
        for (int k = 0; k < 3; k++)
            cur[k] = o->pos[k] + (end[k] - o->pos[k]) * a;
        if (!(cur[0] > 49 || cur[0] < -49) && bad_building(r, cur))
            return dist3(o->pos, cur);
    }
    return -1;
}
/* trackUnderUsGood [R]: 3 behind and 3 ahead along x; the first point within x -49 .. 49 decides: no building there is
 * good, a destroyed or falling one is not */
static int et_track_good(Rail *r, RailObj *o)
{
    float start[3] = { o->pos[0] - 3, o->pos[1], o->pos[2] }, end[3] = { o->pos[0] + 3, o->pos[1], o->pos[2] };
    for (int i = 0; i <= 1; i++) {
        float cur[3];
        for (int k = 0; k < 3; k++)
            cur[k] = start[k] + (end[k] - start[k]) * (float)i;
        if (!(cur[0] > 49 || cur[0] < -49)) {
            int b = building_at(r, cur);
            if (b < 0)
                return 1;
            if (r->b[b].destroyed || r->b[b].falling)
                return 0;
        }
    }
    return 1;
}
/* createPathSlowDown [R]: a SplineMover from where it is to 50 ahead (a speeding train) or 15 (a slow one), the two
 * points at 0 and 3, at 0.08 x its speed (18 or 10): it comes to a stop at the end after 2.1 or 3.75 s */
static void et_slowdown_path(RailObj *o)
{
    float dist = (o->flags & 64) ? 50.0f : 15.0f, speed = (o->flags & 64) ? 18.0f : 10.0f;
    float start[3], end[3];
    copy3(start, o->pos);
    copy3(end, o->pos);
    end[0] += ((o->flags & 32) ? -1.0f : 1.0f) * dist;   /* calcNextPoint */
    sp_clear(o);
    sp_add(o, start, 0, 0);
    sp_add(o, end, 3, 0);
    o->sp.vel = speed * 0.08f;
}
static int et_slowdown_chain(Rail *r, RailObj *o)    /* eventSlowdownChain [R] */
{
    if ((o->flags & 256) == 0) {
        et_slowdown_path(o);
        o->flags |= 256;
    }
    et_sparks(r, o);
    o->state = 2;
    RailObj *n = DEREF(o->next);
    if ((o->flags & 16) && n) {
        et_slowdown_chain(r, n);
        return 1;
    }
    return 0;
}

/* crashTrain [R]: off the track on a SphereMover: (10 or 18 the way it runs, -7.5 .. -15 towards the street's front,
 * 0), turning (pi/8 .. pi/4 about x, pi/24 .. pi/12 about y, both against the way it runs when it runs to -x);
 * no longer Pickupable; state 5 */
static void et_crash(Rail *r, RailObj *o)
{
    float vel[3] = { (o->flags & 64) ? 18.0f : 10.0f, -15, 0 }, rot[3] = { 0, 0, 0 };
    if (o->flags & 32) {
        rot[1] = -SPI / 12;
        rot[0] = -SPI / 4;
        vel[0] = -vel[0];
    } else {
        rot[0] = SPI / 4;
        rot[1] = SPI / 12;
    }
    rot[0] = rot[0] / 2 * (1 + frand(r));
    rot[1] = rot[1] / 2 * (1 + frand(r));
    vel[1] = vel[1] / 2 * (1 + frand(r));
    o->oflags &= ~(uint32_t)RF_PICKUP;
    sphere_init(o, vel, rot);
    if (o->state != 8 && o->state != 5)
        o->state = 5;
}

/* Process [R], [G: every frame]: running, braking or stopped, a car whose track gives way under it crashes */
static void et_process(Rail *r, RailObj *o)
{
    if ((o->state == 1 || o->state == 2 || o->state == 3) && !et_track_good(r, o))
        et_crash(r, o);
}

/* onPulse [R]:
 *  0: a LineMover to -x at 18 (speeding) or 10, its AmbientSound (Train Rolling Loop);
 *  1: the lead car near a monster (60) blows its horn once, 0.15 s after it first comes near; it watches 20 ahead for
 *     a broken track and brakes its whole chain (state 2);
 *  2: the stopping sound (MetalCreak_RND), the sparks and the slow-down path if it has none;
 *  3 (stopped): the sounds and the sparks end; 6 (held): the sparks end;
 *  5 (and 7): the CrashingSound (Train_Crash) once; on a SplineMover still it crashes again, else its FireEffect
 *     (Fire_Big) on it; after its 2 s, state 8;
 *  8: its ExplodeEffect (Tank_Explode), and deleted on the next pulse 0.25 s later */
static int et_pulse(Rail *r, RailObj *o)
{
    if (o->state == 0) {
        float end[3] = { -o->pos[0], o->pos[1], o->pos[2] }, start[3];
        copy3(start, o->pos);
        o->ambient_sfx = sfx_add(r, o, pstr(o, "AmbientSound"));
        line_init(o, start, end, (o->flags & 64) ? 18.0f : 10.0f, 0);
        o->state = 1;
    } else if (o->state == 1) {
        if ((o->flags & 2048) && et_near_monster(r, o)) {
            if (o->horn < 0) {
                et_horn(r, o);
                o->horn = 1000000;
            } else
                o->horn -= o->pulse_delay;
        }
        if ((o->flags & 2048) && et_find_broken(r, o) >= 0) {
            et_slowdown_chain(r, o);
            o->state = 2;
        }
    } else if (o->state == 2) {
        et_stopping_sound(r, o);
        if ((o->flags & 256) == 0) {
            et_sparks(r, o);
            et_slowdown_path(o);
            o->flags |= 256;
        }
    } else if (o->state == 3) {
        et_kill_stopping(r, o);
        et_stop_sparks(r, o);
    } else if (o->state == 6) {
        et_stop_sparks(r, o);
    } else if (o->state == 7 || o->state == 5) {
        if ((o->flags & 1) == 0) {
            sfx_add(r, o, pstr(o, "CrashingSound"));
            o->flags |= 1;
        }
        if ((o->flags & 8) == 0) {
            if (o->mv == MV_SPLINE) {
                et_crash(r, o);
                return 0;
            }
            effect_on(r, o, tstr(o, "FireEffect"), NULL, 1);
            o->flags |= 8;
        }
        o->die -= o->pulse_delay;
        if (o->die < 0)
            o->state = 8;
    } else if (o->state == 8) {
        if ((o->flags & 4) == 0) {
            effect_on(r, o, tstr(o, "ExplodeEffect"), NULL, 0);
            set_pulse(o, 0.25f);
            o->flags |= 4;
        } else {
            obj_delete(r, o);
            return 1;
        }
    }
    return 0;
}

static int et_mover(Rail *r, RailObj *o, int cb)     /* onMoverCallback [R]: 2 ends the braking, or the run (deleted) */
{
    if (cb == 2) {
        if (o->state == 2)
            o->state = 3;
        else if (o->state == 1) {
            obj_delete(r, o);
            return 1;
        }
    }
    return 0;
}

/* the stats of a train [R]: once for each side's chain (the script's globals hasGivenStatsLeftGroup / RightGroup,
 * cleared when a chain of that side is made) */
static void et_stats(Rail *r, RailObj *o, int monster)
{
    if ((o->flags & 32) && !r->et_given_left) {
        add_stats(r, o, monster, 2);
        r->et_given_left = 1;
    } else if ((o->flags & 32) == 0 && !r->et_given_right) {
        add_stats(r, o, monster, 2);
        r->et_given_right = 1;
    }
}

static void et_throw(Rail *r, RailObj *o, int monster)   /* onThrow [R] */
{
    o->oflags &= ~(uint32_t)RF_PICKUP;
    o->bounce = 0;
    if (monster >= 0)
        et_stats(r, o, monster);
    if (o->state != 5 && o->state != 8)
        o->state = 5;
}

/* TakeDamage(who, amount, type, direction) [R]: from a monster only.
 *  - type 0 (a blow): its DamageEffect (Car_Hit) on it;
 *  - running (1), the lead car of a speeding chain (128 and 64) knocks a monster above z 2 (on the track) that is in a
 *    Ground or Faceup move: setVel (10 the way the train runs, -20, 30), setReaction(2), addHitPoints(-Damage (20));
 *  - type 0: a car with fewer than 2 HP left counts (the side's stats, above); then the chain from this car back
 *    loses 1 HP, and a car at 0 crashes with the cars behind it */
static void et_take_damage(Rail *r, RailObj *o, int m, int type)
{
    if (m < 0 || m >= r->nm)
        return;
    if (type == 0)
        effect_on(r, o, tstr(o, "DamageEffect"), NULL, 0);
    if (o->state == 1 && r->m[m].pos[2] > 2 && (o->flags & 128) && (o->flags & 64) &&
        (contains(r->m[m].move, "Ground") || contains(r->m[m].move, "Faceup"))) {
        float vel[3] = { (o->flags & 32) ? -10.0f : 10.0f, -20, 30 };
        monster_knock(r, o, m, vel, pnum(o, "Damage", 20));
    }
    if (type == 0) {
        if (o->hp < 2)
            et_stats(r, o, m);
        et_damage(r, o, 1, 1);
    }
}

/* OnHitGround [R]: hitDelay is never counted down (the script's "this.hitDelay - pulse" assigns nothing), so only the
 * first touch acts: the DamageShape (Train_commuter_01_Damaged), the PickupSound and DamageEffect (Car_Hit) effects,
 * the bounce count, 4096; state 5 */
static void et_hit_ground(Rail *r, RailObj *o)
{
    if (o->hit_delay > 0)
        return;
    o->hit_delay = 0.25f;
    const char *s = pstr(o, "DamageShape");
    set_shape(r, o, s ? s : tstr(o, "DamageShape"));
    effect_on(r, o, tstr(o, "PickupSound"), NULL, 0);
    effect_on(r, o, tstr(o, "DamageEffect"), NULL, 0);
    o->bounce += (o->flags & 4096) ? 1 : 5;
    o->flags |= 4096;
    if ((o->flags & (8 | 8192)) == 8192) {           /* [R] 8192 is never set */
        effect_on(r, o, tstr(o, "DamageEffect"), NULL, 0);
        o->flags &= ~8192;
    }
    if (o->state != 5 && o->state != 8)
        o->state = 5;
}

/* onPickup [R]: the DamageShape, PickupSound and DamageEffect, no longer a knocker (128); the cars behind it crash;
 * state 6 */
static void et_pickup(Rail *r, RailObj *o)
{
    const char *s = pstr(o, "DamageShape");
    set_shape(r, o, s ? s : tstr(o, "DamageShape"));
    effect_on(r, o, tstr(o, "PickupSound"), NULL, 0);
    effect_on(r, o, tstr(o, "DamageEffect"), NULL, 0);
    o->flags &= ~128;
    RailObj *n = DEREF(o->next);
    if ((o->flags & 16) && n)
        et_crash_chain(r, n);
    o->state = 6;
}

/* ---------------------------------------------------------------- ElevatedTrainController.bsf */

/* this.flags [R]: 2 the track is broken, 4 the front track runs, 8 the back track runs, 16 deleted on its next pulse,
 * 32 the two tracks' ways swapped, 256 / 512 the front / back stack holds trains. t0 timeNextTrainFront, t1
 * timeNextTrainBack, hp NumChainedTrain, str0 SlowTrainType, str1 SpeedingTrainType */
static const float ETC_FRONT = 3.0f, ETC_BACK = 7.5f, ETC_X = 100, ETC_Z = 10;

static float etc_latency(RailObj *c) { return pnum(c, "AvgSpawnTrainLatency", 6); }

/* Construct [R]: its two train types, NumChainedTrain (3), a coin for the ways, pulses every 0.4 s; each track's first
 * train in (1 + rand) x latency / 4 (the book's 22: 5.5 .. 11 s) */
static void etc_construct(Rail *r, RailObj *c)
{
    c->str0 = pstr(c, "SlowTrainType");
    c->str1 = pstr(c, "SpeedingTrainType");
    c->hp = (int)pnum(c, "NumChainedTrain", 1);
    c->flags = 4 | 8;
    if (frand(r) > 0.5f)
        c->flags |= 32;
    set_pulse(c, 0.4f);
    c->t1 = (1 + frand(r)) * (etc_latency(c) / 2) / 2;
    c->t0 = (1 + frand(r)) * (etc_latency(c) / 2) / 2;
}

static int etc_all_dead(Rail *r, RailObj *c, int back)   /* frontTrainsAllDead / backTrainsAllDead [R] */
{
    int bit = back ? 512 : 256, n = back ? c->nlink2 : c->nlink, dead = 1;
    if (c->flags & bit)
        for (int i = 0; i < n && dead; i++)
            if (back ? DEREF(c->link2[i]) != NULL : DEREF(c->link[i]) != NULL)
                dead = 0;
    if (dead) {                                      /* clear*TrainStack */
        if (back)
            c->nlink2 = 0;
        else
            c->nlink = 0;
        c->flags &= ~bit;
    }
    return dead;
}

/* isTrackBroken [R]: 16 points from (-48, 2, 0) to (48, 2, 0): a point with no building under it answers "not broken"
 * at once; a destroyed one within x -49 .. 49 (not one still falling) "broken" */
static int etc_track_broken(Rail *r)
{
    for (int i = 0; i <= 15; i++) {
        float a = (float)i / 15.0f, cur[3] = { -48 + 96 * a, 2, 0 };
        int b = building_at(r, cur);
        if (b < 0)
            return 0;
        if (!(cur[0] > 49 || cur[0] < -49) && r->b[b].destroyed)
            return 1;
    }
    return 0;
}

/* createTrain [R]: the speeding or the slow type at loc facing dir; setTrainType; the speeding one 64 | 2048, the slow
 * one 2048 without 64; 32 when it faces left (pi/2); then its chain of NumChainedTrain - 1 cars */
static RailObj *etc_create_train(Rail *r, RailObj *c, int type, const float loc[3], float dir)
{
    const char *name = type == ET_FAST ? c->str1 : c->str0;
    if (!name)
        return NULL;
    RailObj *t = create(r, name);
    if (!t)
        return NULL;
    copy3(t->pos, loc);
    float f[3] = { 0, 0, dir };
    set_facing(t, f);
    t->train_type = name;
    if (type == ET_FAST)
        t->flags |= 64 | 2048;
    else {
        t->flags &= ~64;
        t->flags |= 2048;
    }
    if (dir == SPI / 2)
        t->flags |= 32;
    else
        t->flags &= ~32;
    update_world(t);
    et_spawn_chain(r, t, c->hp - 1);
    return t;
}

/* onPulse [R]: deleted once both tracks are off (16). A track's clock runs while none of its trains is left. The track
 * is broken once a building under it (y 2, x -48 .. 48) is destroyed. A track whose clock is out gets a speeding or a
 * slow train (a coin) at x -100 or 100 (by the controller's coin) at z 10, front y 3, back y 7.5, running across; with
 * the track broken that was its last one. The clock starts again at (1 + rand) x latency / 2 (11 .. 22 s) */
static int etc_pulse(Rail *r, RailObj *c)
{
    if (c->flags & 16) {
        obj_delete(r, c);
        return 1;
    }
    if (etc_all_dead(r, c, 1))
        c->t1 -= c->pulse_delay;
    if (etc_all_dead(r, c, 0))
        c->t0 -= c->pulse_delay;
    if (etc_track_broken(r))
        c->flags |= 2;
    for (int back = 0; back <= 1; back++) {
        int on = back ? 8 : 4;
        float *timer = back ? &c->t1 : &c->t0;
        if (!(c->flags & on) || *timer >= 0)
            continue;
        int type = frand(r) > 0.5f ? ET_FAST : ET_SLOW;
        int right = back ? (c->flags & 32) == 0 : (c->flags & 32) != 0;   /* FACE_RIGHT: from x -100 to +x */
        float dir = right ? -SPI / 2 : SPI / 2;
        float pos[3] = { right ? -ETC_X : ETC_X, back ? ETC_BACK : ETC_FRONT, ETC_Z };
        if (etc_all_dead(r, c, back)) {
            RailObj *t = etc_create_train(r, c, type, pos, dir);
            c->flags |= back ? 512 : 256;
            if (t)
                et_reg(r, t, c);
            if (c->flags & 2)
                c->flags &= ~on;
            *timer = (1 + frand(r)) * (etc_latency(c) / 2);
        }
    }
    if ((c->flags & 4) == 0 && (c->flags & 8) == 0)
        c->flags |= 16;
    return 0;
}

/* ---------------------------------------------------------------- NewYorkTrain.bsf and its controller */

/* this.flags [R]: 2 its hit effect played, 4 a tanker, 32 it runs to -x, 64 fast, 256 the engine, 512 the light on,
 * 1024 hit. this.state: 0 new, 1 running, 2 a tanker's blast due */

/* Construct [R]: pulses every 0.75 s; 0x2000000 | PushOutMonsters; its AmbientSound (the engine's Train Rolling
 * Loop) */
static void ny_construct(Rail *r, RailObj *o)
{
    o->state = 0;
    o->horn = 0;
    set_pulse(o, 0.75f);
    o->t0 = 0.75f;                                   /* this.pulseDelay */
    o->oflags |= 0x2000000 | RF_PUSH_OUT;
    o->ambient_sfx = sfx_add(r, o, pstr(o, "AmbientSound"));
    r->ny_vehicles++;
}

/* spawnTrainChain(n, type) [R]: the engine (256) pulls n cars, the first 14.5 behind it, the next 12 behind each
 * other: a slow train (0) flatcars (TrainNYFlat) with a tanker (TrainNYTanker, flag 4) every second car, a fast one
 * (1) TrainNYFreight cars; each with the engine's flags but 256, facing its way */
static void ny_spawn_chain(Rail *r, RailObj *o, int n, int type)
{
    float loc[3];
    copy3(loc, o->pos);
    o->flags |= 256;
    o->nlink = 0;
    for (int i = 0; i < n && i < 8; i++) {
        RailObj *t = NULL;
        if (type == 0) {
            if (i % 2 == 1) {
                if ((t = create(r, tstr(o, "TankerTrainType"))) != NULL)
                    t->flags |= 4;
            } else
                t = create(r, tstr(o, "FlatcarTrainType"));
        } else if (type == 1)
            t = create(r, tstr(o, "FreightTrainType"));
        if (t) {
            float off = i == 0 ? 14.5f : 12.0f;
            loc[0] += (o->flags & 32) ? off : -off;
            copy3(t->pos, loc);
            set_facing(t, o->facing);
            t->flags |= o->flags & ~256;
            update_world(t);
            SETREF(o->link[i], t);
            o->nlink = i + 1;
        }
    }
}

/* onPulse [R]:
 *  0: a LineMover to -x at 20 (fast) or 10; the engine's LightEffect (Train_light) on it;
 *  1: the engine near a monster (60) blows its horn once, a pulse after it first comes near;
 *  2: a hit tanker blows up: its ExplodeEffect (Tank_Explode) where it is, and the nearest monster, wherever it is,
 *     is thrown at 30 away from it (up twice its distance along the street), setReaction(2), addHitPoints
 *     (-DamageExplode (20)); back to 1 */
static int ny_pulse(Rail *r, RailObj *o)
{
    if (o->state == 0) {
        float start[3], end[3] = { -o->pos[0], o->pos[1], o->pos[2] };
        copy3(start, o->pos);
        if ((o->flags & 256) && (o->flags & 512) == 0) {
            o->light_fx = effect_on(r, o, pstr(o, "LightEffect"), NULL, 1);
            o->flags |= 512;
        }
        line_init(o, start, end, (o->flags & 64) ? 20.0f : 10.0f, 0);
        o->state = 1;
    } else if (o->state == 1 && (o->flags & 256) && et_near_monster(r, o)) {
        if (o->horn < 0) {
            sfx_add(r, o, pstr(o, "HornSound"));
            o->horn = 99999;
        } else
            o->horn -= o->t0;
    } else if (o->state == 2) {
        if (o->flags & 4) {
            float damage = pnum(o, "DamageExplode", 0);
            effect_at(r, tstr(o, "ExplodeEffect"), o->pos);
            int m = nearest_monster(r, o->pos);
            if (m >= 0) {
                float vel[3] = { r->m[m].pos[0] - o->pos[0], r->m[m].pos[1] - o->pos[1], 0 };
                vel[2] = 2 * (fabsf(vel[0]) + fabsf(vel[1]));
                norm3(vel);
                for (int k = 0; k < 3; k++)
                    vel[k] *= 30;
                monster_knock(r, o, m, vel, damage);
            }
        }
        o->t0 = 0.75f;
        set_pulse(o, o->t0);
        o->state = 1;
    }
    return 0;
}

static int ny_mover(Rail *r, RailObj *o, int cb)     /* onMoverCallback [R]: deleted at the end of its run */
{
    if (cb == 2 && o->state == 1) {
        obj_delete(r, o);
        return 1;
    }
    return 0;
}

/* doHitEffect [R]: the DamageShape; once its HitEffect (the flatcar's Debris_Crate) and its stats (type 2, not for a
 * tanker); a tanker blows up on its next pulse 0.75 s later */
static void ny_hit_effect(Rail *r, RailObj *o, int m)
{
    set_shape(r, o, pstr(o, "DamageShape"));
    if ((o->flags & 2) == 0) {
        effect_on(r, o, pstr(o, "HitEffect"), NULL, 0);
        if ((o->flags & 4) == 0)
            add_stats(r, o, m, 2);
        o->flags |= 2;
    }
    if (o->flags & 4) {
        o->t0 = 0.75f;
        set_pulse(o, o->t0);
        o->state = 2;
    }
}

/* TakeDamage [R]: from a monster only. Running, a fast engine knocks a monster in a Ground move: setVel (10 the way
 * it runs, -20, 30), setReaction(2), addHitPoints(-DamageCollide (5)), whatever the damage's type (so its touch does
 * it). A blow (type 0) on a car (not the engine) that was not hit yet: doHitEffect */
static void ny_take_damage(Rail *r, RailObj *o, int m, int type)
{
    if (m < 0 || m >= r->nm)
        return;
    if (o->state == 1 && (o->flags & 64) && (o->flags & 256) && contains(r->m[m].move, "Ground")) {
        float vel[3] = { (o->flags & 32) ? -10.0f : 10.0f, -20, 30 };
        monster_knock(r, o, m, vel, pnum(o, "DamageCollide", 5));
    }
    if (type == 0 && (o->flags & (256 | 1024)) == 0) {
        ny_hit_effect(r, o, m);
        o->flags |= 1024;
    }
}

/* NewYorkTrainController.bsf [R]: the first pulse (a frame after it is made) a slow train from x -100 (y -12.5)
 * running to +x; after that, every second while its engine is gone, a fast train (FreqFastTrain 0.75) or a slow one,
 * from either end (a coin), with NumChainedTrain - 1 (5) cars */
static RailObj *nyc_create_train(Rail *r, RailObj *c, int type, const float loc[3], float dir)
{
    RailObj *t = create(r, pstr(c, "TrainNYEngine"));
    if (!t)
        return NULL;                                 /* [R] gCommon.dbgAssert(0, "COULD NOT CREATE TRAIN") */
    copy3(t->pos, loc);
    float f[3] = { 0, 0, dir };
    set_facing(t, f);
    if (type == 1)
        t->flags |= 64;
    if (dir == SPI / 2)
        t->flags |= 32;
    update_world(t);
    ny_spawn_chain(r, t, (int)pnum(c, "NumChainedTrain", 1) - 1, type);
    return t;
}
static void nyc_construct(Rail *r, RailObj *c)
{
    (void)r;
    set_pulse(c, TIMESTEP);
    c->state = 0;
}
static int nyc_pulse(Rail *r, RailObj *c)
{
    if (DEREF(c->link[0]))
        return 0;
    RailObj *t = NULL;
    if (c->state == 0) {
        float at[3] = { -100, -12.5f, 0 };
        t = nyc_create_train(r, c, 0, at, -SPI / 2);
        set_pulse(c, 1.0f);
        c->state = 1;
    } else if (c->state == 1) {
        int type = frand(r) < pnum(c, "FreqFastTrain", 0.5f) ? 1 : 0;
        float at[3] = { 100, -12.5f, 0 }, dir = SPI / 2;
        if (!(frand(r) < 0.5f)) {
            at[0] = -100;
            dir = -SPI / 2;
        }
        t = nyc_create_train(r, c, type, at, dir);
    }
    if (t)
        SETREF(c->link[0], t);
    else
        c->link[0].i = -1;
    return 0;
}

/* ---------------------------------------------------------------- CableCar.bsf and its controller */

/* this.state [R] holds a stage (1 to the line's start, 2 to the turntable, 3 on path 1, 4 turning, 5 / 6 path 2, 7 the
 * line out) and the bits 128 dying, 256 thrown, 512 touched the street, 1024 (never set), 2048 held. flags: 1 its
 * smoke on, 2 its explosion played. t0 facingZ, t1 lastTimeStamp, t2 endRot, sub_state nextState, die dieCountdown */

/* Construct [R]: PushOutMonsters, OnlyHitWithEffNodes, Pickupable; its AmbientEffect (none in the books); facing -x;
 * stage 1, pulsing every frame */
static void cc_construct(Rail *r, RailObj *o)
{
    o->oflags |= RF_PUSH_OUT | RF_EFF_NODES | RF_PICKUP;
    o->ambient_fx = effect_on(r, o, tstr(o, "AmbientEffect"), NULL, 1);
    o->t0 = SPI / 2;
    set_facing_z(o, o->t0);
    o->t1 = 0;
    o->state = 1;
    set_pulse(o, TIMESTEP);
}

/* initSplinePaths [R]: path 1 from (30, y) along the street to x -2.8 and up the turntable's track 16.6 back (+y);
 * path 2 from there down to the street and on to x -35.6 */
static void cc_init_paths(RailObj *o)
{
    float y = o->pos[1];
    static const float P1[5][3] = { { 30, 0, 0 }, { 18, 0, 0 }, { 8, 0, 0 }, { -2.8f, 1.7f, 0 }, { -3.8f, 16.6f, 0 } };
    static const float P2[5][3] = { { -3.8f, 16.6f, 0 }, { -4.8f, 1.7f, 0 }, { -15.6f, 0, 0 }, { -25.6f, 0, 0 },
                                    { -35.6f, 0, 0 } };
    for (int i = 0; i < 5; i++) {
        set3(o->path[0][i], P1[i][0], P1[i][1] + y, P1[i][2]);
        set3(o->path[1][i], P2[i][0], P2[i][1] + y, P2[i][2]);
    }
}

/* setSplinePath [R]: a SplineMover turning the car along its way (flag 2) through the path's five points at
 * addPointWithVel(p, knot / total, 0.2), at 0.5: knot i is the path's length up to point i, total the square root of
 * the knots' sum (so path 1 takes 8.9 s) */
static void cc_set_path(RailObj *o, int path)
{
    sp_clear(o);
    o->sp.flags = 2;
    float knots[5], total = 0;
    for (int i = 0; i < 5; i++) {
        float len = 0;
        if (i > 0)
            len = dist3(o->path[path][i - 1], o->path[path][i]) + knots[i - 1];
        knots[i] = len;
        total += len;
    }
    total = sqrtf(total);
    for (int i = 0; i < 5; i++)
        sp_add(o, o->path[path][i], knots[i] / total, 0.2f);
    o->sp.vel = 0.5f;
}

/* onMoverCallback [R]: at each stage's end (2) the next: path 1; the turn (no mover, pi/4 a second through pi, every
 * frame); path 2; a LineMover from its end to x -100 at 6, facing its way; deleted at its end. A monster near (4:
 * MonsterResponseDist 10) rings its HornSound (cablecar_bell), at most every 2 s */
static int cc_mover(Rail *r, RailObj *o, int cb)
{
    if (cb == 2) {
        if (o->state == 2) {
            cc_set_path(o, 0);
            o->state = 3;
        } else if (o->state == 3) {
            o->mv = MV_NONE;
            o->t0 = o->facing[2];
            o->t2 = o->t0 - SPI;
            set_pulse(o, TIMESTEP);
            o->state = 4;
            o->sub_state = 5;
        } else if (o->state == 5) {
            cc_set_path(o, 1);
            set_pulse(o, 0);
            o->state = 6;
        } else if (o->state == 6) {
            set_pulse(o, 0);
            float start[3], end[3];
            copy3(start, o->path[1][4]);
            copy3(end, start);
            end[0] = -100;
            line_init(o, start, end, 6, 0);
            o->line.flags = 1;
            o->state = 7;
        } else if (o->state == 7) {
            obj_delete(r, o);
            return 1;
        }
    } else if (cb == 4) {
        float dt = r->time - o->t1;
        if (dt > 2.0f || o->t1 == 0) {
            sfx_add(r, o, tstr(o, "HornSound"));
            o->t1 = r->time;
        }
    }
    return 0;
}

/* onPulse [R]:
 *  1: a LineMover at 6 facing its way: with Turnaround to the turntable's path (30, y), else across to -x;
 *  dying (128): its HitEffect1 (Tank_Smoke_Med) on it and 1.5 s; then its ExplodeEffect (Car_explode) and deleted
 *     0.25 s (a pulse) later;
 *  4: the turn on the turntable, pi/4 a second, until it has turned through pi: then path 2;
 *  thrown (256): dying from the next pulse */
static int cc_pulse(Rail *r, RailObj *o)
{
    if (o->state == 1) {
        float start[3], end[3];
        copy3(start, o->pos);
        if (tnum(o, "Turnaround", 0) != 0) {
            cc_init_paths(o);
            copy3(end, o->path[0][0]);
            line_init(o, start, end, 6, 0);
            o->state = 2;
        } else {
            set3(end, -start[0], start[1], 0);
            line_init(o, start, end, 6, 0);
            o->state = 7;
        }
        o->line.flags = 1;
        set_pulse(o, 0);
    } else if (o->state & 128) {
        if (o->flags & 1)
            o->die -= o->pulse_delay;
        else {
            o->die = 1.5f;
            effect_on(r, o, pstr(o, "HitEffect1"), NULL, 0);
            o->flags |= 1;
        }
        if (o->die < 0) {
            if (o->flags & 2) {
                obj_delete(r, o);
                return 1;
            }
            effect_on(r, o, tstr(o, "ExplodeEffect"), NULL, 0);
            o->die = 0.25f;
            o->flags |= 2;
        }
    } else if (o->state == 4) {
        o->t0 -= SPI * TIMESTEP / 4;
        if (o->t0 > o->t2)
            set_facing_z(o, o->t0);
        else {
            o->t0 = o->t2;
            set_facing_z(o, o->t0);
            o->state = o->sub_state;
            return cc_mover(r, o, 2);
        }
    } else if (o->state & 256) {
        o->state = 128;
    }
    return 0;
}

static void cc_impact(RailObj *o)                    /* TakeImpact [R]: dying */
{
    set_pulse(o, 1.5f / 4);
    o->state |= 128;
}
/* OnHitGround [R]: 5 for the first touch, 1 after; over 40 dying */
static void cc_hit_ground(Rail *r, RailObj *o)
{
    set_pulse(o, 1.5f / 4);
    o->bounce += (o->state & 512) ? 1 : 5;
    if (o->bounce > 40) {
        o->state |= 128;
        return;
    }
    o->state |= 512;
    if ((o->state & (128 | 1024)) == 1024) {         /* [R] 1024 is never set */
        effect_on(r, o, tstr(o, "DamageEffect"), NULL, 0);
        o->state &= ~1024;
    }
}
/* onPickup [R]: the DamageShape, its PickupSound (cablecar_bell), DamageEffect (Car_Hit); an IsShaker (both) also
 * its PourEffect (not in the books), ScreamSound (Crowd_Panic) and ShakerSound (none); held (2048) */
static void cc_pickup(Rail *r, RailObj *o)
{
    set_pulse(o, 1.5f / 4);
    const char *s = pstr(o, "DamageShape");
    set_shape(r, o, s ? s : tstr(o, "DamageShape"));
    sfx_add(r, o, tstr(o, "PickupSound"));
    effect_on(r, o, tstr(o, "DamageEffect"), NULL, 0);
    if (tnum(o, "IsShaker", 0) != 0) {
        effect_on(r, o, tstr(o, "PourEffect"), NULL, 0);
        sfx_add(r, o, tstr(o, "ScreamSound"));
        sfx_add(r, o, tstr(o, "ShakerSound"));
    }
    o->state = 2048;
}
/* onThrow [R]: not Pickupable, thrown (256), pulsing every 0.5 s; it counts (addStats type 2: 25) */
static void cc_throw(Rail *r, RailObj *o, int monster)
{
    o->oflags &= ~(uint32_t)RF_PICKUP;
    o->state |= 256;
    set_pulse(o, 0.5f);
    o->bounce = 0;
    if (monster >= 0)
        add_stats(r, o, monster, 2);
}
/* TakeDamage [R]: a monster's blow on a car neither dying nor thrown: no longer hittable by blows only nor
 * Pickupable, the DamageShape, its ambient effect stopped, DamageSound and DamageEffect (Car_Hit) on it; it flies off
 * on a SphereMover at 25 away from the monster (the height difference a fifth, upwards), and onThrow: it counts */
static void cc_take_damage(Rail *r, RailObj *o, int m)
{
    if ((o->state & (128 | 256)) != 0 || m < 0 || m >= r->nm)
        return;
    set_pulse(o, 1.5f / 4);
    o->oflags &= ~(uint32_t)(RF_EFF_NODES | RF_PICKUP);
    set_shape(r, o, pstr(o, "DamageShape"));
    if (o->ambient_fx >= 0)
        effect_stop(r, o, o->ambient_fx);
    o->ambient_fx = -1;
    effect_on(r, o, pstr(o, "DamageSound"), NULL, 0);
    effect_on(r, o, pstr(o, "DamageEffect"), NULL, 0);
    float dir[3] = { o->pos[0] - r->m[m].pos[0], o->pos[1] - r->m[m].pos[1], o->pos[2] - r->m[m].pos[2] };
    dir[2] /= 5;
    norm3(dir);
    dir[0] *= 25;
    dir[1] *= 25;
    dir[2] = fabsf(dir[2] * 25);
    sphere_init(o, dir, NULL);
    o->sphere.tumble = 0;
    cc_throw(r, o, m);
}

/* CableCarController.bsf [R]: onInit a car at (45 + 10 rand, y - 7.2, 0), mid-way along its line; then one every
 * CarInterval (15, the turnaround one's 20) from (100, y - 7.2, 0) */
static void ccc_init(Rail *r, RailObj *c)
{
    float rnd = frand(r);
    set_pulse(c, tnum(c, "CarInterval", 5));
    RailObj *t = create(r, tstr(c, "CarType"));
    if (t) {
        set3(t->pos, 45 + rnd * 10, c->pos[1] - 7.2f, 0);
        update_world(t);
    }
}
static int ccc_pulse(Rail *r, RailObj *c)
{
    RailObj *t = create(r, tstr(c, "CarType"));
    if (t) {
        set3(t->pos, 100, c->pos[1] - 7.2f, 0);
        update_world(t);
    }
    return 0;
}

/* ---------------------------------------------------------------- freighter.bsf */

/* this.state [R]: 1 sailing, 3 sinking, 4 gone; flags 4 the sinking path made. horn_i hornCountDown, t0 fStateTime,
 * t1 fOscillate, t2 vel, t3 sinkDuration */

/* Construct [R]: pulses every frame; OnlyHitWithEffNodes; AmbientEffect and EntranceSound (none in the book); from
 * x 100 or -100 (a coin) at y -7.2, z -0.5 across to the other side on a LineMover at 6, facing its way; Oscillate (1):
 * its sway starts at 5 rand */
static void fr_construct(Rail *r, RailObj *o)
{
    o->t0 = 0;
    o->flags = 0;
    o->state = 1;
    o->horn_i = 0;
    set_pulse(o, TIMESTEP);
    o->oflags |= RF_EFF_NODES;
    o->ambient_fx = effect_on(r, o, tstr(o, "AmbientEffect"), NULL, 1);
    sfx_add(r, o, tstr(o, "EntranceSound"));
    float dir = frand(r) < 0.5f ? -1.0f : 1.0f;
    float start[3] = { 100 * dir, -7.2f, -0.5f }, end[3] = { -100 * dir, -7.2f, -0.5f };
    o->t2 = 6;
    line_init(o, start, end, o->t2, 0);
    set_facing_z(o, SPI / 2 * dir);
    if (pnum(o, "Oscillate", 0) != 0)
        o->t1 = 5 * frand(r);
}

/* pushMonster [R]: a monster within 15 degrees of its course ahead or behind is pushed 0.5 further along it; one
 * within 4 across (y) is pushed 0.6 to +y (the quay) */
static void fr_push(Rail *r, RailObj *o, int m)
{
    int move = 0;
    float mp[3], dir[3] = { r->m[m].pos[0] - o->pos[0], r->m[m].pos[1] - o->pos[1], r->m[m].pos[2] - o->pos[2] };
    copy3(mp, r->m[m].pos);
    norm3(dir);
    float tx = cosf(o->facing[2] + SPI / 2) < 0 ? -1.0f : 1.0f;
    float dot = dir[0] * tx;
    if (fabsf(dot) > 0.9659258f) {
        mp[0] += dot > 0 ? tx * 0.5f : -tx * 0.5f;
        move = 1;
    }
    if (fabsf(o->pos[1] - mp[1]) < 4) {
        mp[1] += 0.6f;
        move = 1;
    }
    if (move) {                                      /* m.queuePushRequest(mPos) */
        RailEvent *e = emit(r, RE_PUSH, idx(r, o), NULL, mp);
        e->monster = m;
        for (int k = 0; k < 3; k++)
            e->v[k] = mp[k] - r->m[m].pos[k];
    }
}

/* onMoverCallback [R], while sailing: at its line's end (2) it turns round (its facing's z negated) and sails back
 * the same way; a monster near (4: MonsterResponseDist 12) hears its HornSound (none in the book) and every monster
 * within that distance in a Ground move is pushed */
static int fr_mover(Rail *r, RailObj *o, int cb)
{
    if (o->state >= 3)
        return 0;
    if (cb == 2) {
        float start[3], end[3];
        copy3(start, o->pos);
        copy3(end, start);
        end[0] = -end[0];
        float f[3] = { o->facing[0], o->facing[1], -o->facing[2] };
        set_facing(o, f);
        line_init(o, start, end, o->t2, 0);
    } else if (cb == 4) {
        if (o->horn_i == 0) {
            const char *s = tstr(o, "HornSound");
            if (s) {
                sfx_add(r, o, s);
                o->horn_i = 3;
            }
        }
        if (o->mv == MV_LINE) {
            float d2 = pnum(o, "MonsterResponseDist", -1);
            d2 *= d2;
            for (int i = 0; i < r->nm; i++) {
                float e[3] = { r->m[i].pos[0] - o->pos[0], r->m[i].pos[1] - o->pos[1], r->m[i].pos[2] - o->pos[2] };
                if (e[0] * e[0] + e[1] * e[1] + e[2] * e[2] <= d2 && contains(r->m[i].move, "Ground"))
                    fr_push(r, o, i);
            }
        }
    }
    return 0;
}

/* oscillateObject [R]: the sway's clock runs by the pulse; (sin 2t, cos 3t) x 0.21 about x and y, its heading kept */
static void fr_oscillate(RailObj *o, float out[3])
{
    float z = o->facing[2];
    o->t1 += o->pulse_delay;
    out[0] = sinf(o->t1 * 2) * 0.21f;
    out[1] = cosf(o->t1 * 3) * 0.21f;
    out[2] = z;
}

/* onPulse [R]: sailing, it sways a fifth of that; sinking, a LineMover 30 down and 30 along -x [R: cos(+-pi/2) of the
 * script's float pi is just below 0 either way] at 6, swaying up to twice as much as the sinking goes on, for 2.5 s;
 * then its DamageEffect (Debris_Crate) where it is, gAIMgr.removeAIObject, deleted */
static void rail_remove_ai(Rail *r, RailObj *o);
static int fr_pulse(Rail *r, RailObj *o)
{
    if (o->state == 1) {
        if (o->horn_i > 0)
            o->horn_i--;
        float osc[3];
        fr_oscillate(o, osc);
        osc[0] *= 0.2f;
        osc[1] *= 0.2f;
        set_facing(o, osc);
    } else if (o->state == 3) {
        if (o->flags & 4)
            o->t3 -= o->pulse_delay;
        else {
            float start[3], end[3];
            copy3(start, o->pos);
            end[0] = cosf(o->facing[2]) < 0 ? start[0] - 30 : start[0] + 30;
            end[1] = start[1];
            end[2] = start[2] - 30;
            line_init(o, start, end, o->t2, 0);
            o->t0 = 1;
            o->t3 = 2.5f;
            o->flags |= 4;
        }
        if (pnum(o, "Oscillate", 0) != 0) {
            o->t0 += o->pulse_delay;
            float osc[3];
            fr_oscillate(o, osc);
            osc[0] *= fminf(o->t0, 2);
            osc[1] *= fminf(o->t0, 2);
            set_facing(o, osc);
        }
        if (o->t3 < 0)
            o->state = 4;
    } else if (o->state == 4) {
        effect_at(r, tstr(o, "DamageEffect"), o->pos);
        rail_remove_ai(r, o);
        obj_delete(r, o);                            /* [R] onPulse returns 4 | 8 | 16: deleted */
        return 1;
    }
    return 0;
}

/* TakeDamage [R]: a monster's blow on a sailing freighter: no longer hittable, the DamageShape
 * (ScumLabsFreighter_Damaged), its ambient effect stopped, DamageSound on it, DamageEffect (Debris_Crate) where it is;
 * it counts (addStats type 2: 25) and sinks */
static void fr_take_damage(Rail *r, RailObj *o, int m)
{
    if (m < 0 || m >= r->nm || o->state >= 3)
        return;
    o->oflags &= ~(uint32_t)(RF_EFF_NODES | RF_PICKUP);
    const char *s = pstr(o, "DamageShape");
    set_shape(r, o, s ? s : tstr(o, "DamageShape"));
    if (o->ambient_fx >= 0)
        effect_stop(r, o, o->ambient_fx);
    o->ambient_fx = -1;
    effect_on(r, o, tstr(o, "DamageSound"), NULL, 0);
    effect_at(r, tstr(o, "DamageEffect"), o->pos);
    add_stats(r, o, m, 2);
    o->state = 3;
}

/* ---------------------------------------------------------------- Train.bsf (Train1: no block places it) */

/* [R] Construct: Direction (-1) faces it left (pi/2) or right; a LineMover from x 150 Direction to the other side at
 * 30, starting at a random share; at its end (any callback) it starts again from that side */
static void lt_start(RailObj *o, float share)
{
    float start[3] = { (float)o->id * 150, o->pos[1], o->pos[2] }, end[3] = { (float)o->id * -150, o->pos[1], o->pos[2] };
    line_init(o, start, end, 30, share);
}
static void lt_construct(Rail *r, RailObj *o)
{
    o->hp = r->loop_trains++;
    o->id = (int)pnum(o, "Direction", -1);           /* this.direction */
    set_facing_z(o, o->id == -1 ? SPI / 2 : -SPI / 2);
    lt_start(o, frand(r));
}

/* ---------------------------------------------------------------- making things */

static void construct(Rail *r, RailObj *o)
{
    switch (o->kind) {
    case RK_ETRAIN_CTRL: etc_construct(r, o); break;
    case RK_ETRAIN: et_construct(r, o); break;
    case RK_NYTRAIN_CTRL: nyc_construct(r, o); break;
    case RK_NYTRAIN: ny_construct(r, o); break;
    case RK_CABLE_CTRL: set_pulse(o, 1); break;      /* its pulse is set by onInit */
    case RK_CABLECAR: cc_construct(r, o); break;
    case RK_FREIGHTER: fr_construct(r, o); break;
    case RK_LOOPTRAIN: lt_construct(r, o); break;
    default: break;
    }
    update_world(o);
}

/* gWorld.createScriptObject(template): made and constructed at the origin; the caller puts it where it goes */
static RailObj *create(Rail *r, const char *name)
{
    Group t = find_template(r, name);
    int kind = kind_of_template(t);
    if (kind < 0)
        return NULL;
    RailObj *o = obj_new(r, name, t, kind);
    if (o)
        construct(r, o);
    return o;
}

int rail_script_is_ours(const char *script_name) { return kind_of_script(script_name) >= 0; }

int rail_kind_of(Rail *r, const char *tmpl_name) { return kind_of_template(find_template(r, tmpl_name)); }

int rail_spawn(Rail *r, const char *tmpl_name, const float pos[3], float facing, Group inst)
{
    Group t = find_template(r, tmpl_name);
    int kind = kind_of_template(t);
    if (kind < 0)
        return -1;
    RailObj *o = obj_new(r, tmpl_name, t, kind);
    if (!o)
        return -1;
    o->inst = inst;
    copy3(o->pos, pos);
    set_facing_z(o, facing);
    construct(r, o);
    if (kind == RK_CABLE_CTRL)                       /* onInit, once it stands where it was put */
        ccc_init(r, o);
    update_world(o);
    return idx(r, o);
}

/* [R] 0x334C40 / 0x3350F0 (army.c's): MinDelay 10, RespawnDelay 10, OffsetDelay 5, MaxOccurances 9999 unless set;
 * the first spawn in MinDelay + OffsetDelay x rand seconds. Only the records whose template is ours are kept here */
int rail_load_block(Rail *r, Group block_def)
{
    r->nsp = 0;
    r->active = 0;
    Group ai = group_sub(block_def, "AIDef");
    if (!ai.at)
        return 0;
    Record rec[64];
    uint32_t n = group_all(ai, "AIInstance", rec, 64);
    for (uint32_t i = 0; i < n && r->nsp < MAX_SPAWN; i++) {
        Group g = record_group(ai, rec[i]);
        const char *name = gstr(g, "Name");
        if (!name || kind_of_template(find_template(r, name)) < 0)
            continue;
        Spawner *s = &r->sp[r->nsp++];
        memset(s, 0, sizeof *s);
        s->name = name;
        s->g = g;
        s->min_delay = gnum(g, "MinDelay", 10);
        s->respawn = gnum(g, "RespawnDelay", 10);
        s->offset = gnum(g, "OffsetDelay", 5);
        s->left = (int)gnum(g, "MaxOccurances", 9999);
        s->timer = s->min_delay + s->offset * frand(r);
        s->enabled = 1;
    }
    return r->nsp;
}

void rail_activate(Rail *r, int on) { r->active = on; }

/* [R] gAIMgr.removeAIObject (0x332A50): every record of that template name has one fewer alive */
static void rail_remove_ai(Rail *r, RailObj *o)
{
    for (int i = 0; i < r->nsp; i++)
        if (same_text(r->sp[i].name, o->name))
            r->sp[i].alive--;
}

/* [R] the update 0x3333F0 (army.c's): a record with nothing alive and spawns left counts down; at 0 it spawns its
 * template (an AttackObject whose instance book is the AIInstance group) and restarts at RespawnDelay + OffsetDelay x
 * rand. At most one of each alive; MaxOccurances in all. [G] It is made at the origin: the script places it */
static void spawner_update(Rail *r, float dt)
{
    if (!r->active)
        return;
    for (int i = 0; i < r->nsp; i++) {
        Spawner *s = &r->sp[i];
        if (s->alive != 0 || s->left == 0)
            continue;
        s->timer -= dt;
        if (!s->enabled || s->timer > 0)
            continue;
        s->timer = s->respawn + s->offset * frand(r);
        s->alive++;
        s->left--;
        Group t = find_template(r, s->name);
        int kind = kind_of_template(t);
        RailObj *o = kind >= 0 ? obj_new(r, s->name, t, kind) : NULL;
        if (!o)
            continue;                                /* [R] alive stays 1: that record never spawns again */
        o->inst = s->g;
        o->spawner = i;
        construct(r, o);
    }
}

/* ---------------------------------------------------------------- the scripts' handlers by kind */

static int on_mover(Rail *r, RailObj *o, int cb)     /* 1: it was deleted */
{
    switch (o->kind) {
    case RK_ETRAIN: return et_mover(r, o, cb);
    case RK_NYTRAIN: return ny_mover(r, o, cb);
    case RK_CABLECAR: return cc_mover(r, o, cb);
    case RK_FREIGHTER: return fr_mover(r, o, cb);
    case RK_LOOPTRAIN: lt_start(o, 0); return 0;
    default: return 0;
    }
}
static int on_pulse(Rail *r, RailObj *o)
{
    switch (o->kind) {
    case RK_ETRAIN_CTRL: return etc_pulse(r, o);
    case RK_ETRAIN: return et_pulse(r, o);
    case RK_NYTRAIN_CTRL: return nyc_pulse(r, o);
    case RK_NYTRAIN: return ny_pulse(r, o);
    case RK_CABLE_CTRL: return ccc_pulse(r, o);
    case RK_CABLECAR: return cc_pulse(r, o);
    case RK_FREIGHTER: return fr_pulse(r, o);
    default: return 0;
    }
}
static void take_damage(Rail *r, RailObj *o, int m, int type)
{
    switch (o->kind) {
    case RK_ETRAIN: et_take_damage(r, o, m, type); break;
    case RK_NYTRAIN: ny_take_damage(r, o, m, type); break;
    case RK_CABLECAR: cc_take_damage(r, o, m); break;   /* [R] whatever the type */
    case RK_FREIGHTER: fr_take_damage(r, o, m); break;
    default: break;
    }
}
static void rail_take_impact(Rail *r, RailObj *o)    /* TakeImpact */
{
    if (o->kind == RK_ETRAIN && o->state != 5 && o->state != 8)
        et_crash(r, o);
    else if (o->kind == RK_CABLECAR)
        cc_impact(o);
}
static void hit_ground(Rail *r, RailObj *o)          /* OnHitGround */
{
    if (o->kind == RK_ETRAIN)
        et_hit_ground(r, o);
    else if (o->kind == RK_CABLECAR)
        cc_hit_ground(r, o);
}

/* ---------------------------------------------------------------- contact with the monsters */

/* a book's collision keys (WSimpleObject 0x28FD70): CollisionBox { min x y z, max x y z } */
static const float *book_box(Group g, float b[6])
{
    Record rec[8];
    Group cb = group_sub(g, "CollisionBox|CollisionBox");
    if (!cb.at)
        cb = group_sub(g, "CollisionBox");
    if (!cb.at || group_items(cb, rec, 8) < 6)
        return NULL;
    for (int k = 0; k < 6; k++)
        b[k] = record_num(rec[k]);
    return b;
}

/* an object's shape (0x28FD70, collide_shape_pick): the template's CollisionBox (the New York engine's, a box 6 .. 8
 * ahead of it), CollisionRadius or AutoCollisionBox (every car here) with the model's box, then the instance's; else
 * the model's bounding sphere [G: taken as its box's, the host's; the freighter] */
static void obj_shape_read(const Rail *r, const RailObj *o, CollideShape *s)
{
    float tb[6], ib[6], mb[6], c[3] = { 0, 0, 0 }, rad = 1;
    int have = o->shape && r->host.shape_box && r->host.shape_box(r->host.ctx, o->shape, mb, mb + 3);
    if (have) {
        float h[3];
        for (int k = 0; k < 3; k++) {
            c[k] = (mb[k] + mb[3 + k]) / 2;
            h[k] = (mb[3 + k] - mb[k]) / 2;
        }
        rad = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    }
    /* [R 0x28FD70 -> 0x184CB0] the sphere the model keeps, the union of its parts' spheres (0x185AA0) */
    if (have && r->host.shape_sphere)
        r->host.shape_sphere(r->host.ctx, o->shape, c, &rad);
    Group in = o->inst;
    collide_shape_pick(s, have ? c : NULL, rad, o->tmpl.at ? book_box(o->tmpl, tb) : NULL, tnum(o, "CollisionRadius", -1),
                       tnum(o, "AutoCollisionBox", 0) != 0, in.at ? book_box(in, ib) : NULL,
                       in.at ? group_num(in, "CollisionRadius", -1) : -1,
                       in.at && group_num(in, "AutoCollisionBox", 0) != 0, have ? mb : NULL);
}

static void obj_shape(const Rail *r, RailObj *o, CollideShape *s)   /* read once, at its first use */
{
    if (!o->cshape_ok) {
        obj_shape_read(r, o, &o->cshape);
        o->cshape_ok = 1;
    }
    *s = o->cshape;
}

/* the object as the collision world has it: its shape at its matrix */
static void obj_collide(const Rail *r, RailObj *o, CollideObj *c)
{
    memset(c, 0, sizeof *c);
    obj_shape(r, o, &c->shape);
    collide_matrix_from16(c->m, o->world);
    c->flags = COLLIDE_F_WORLD | (o->oflags & ~(uint32_t)RF_HELD);
    c->building = -1;
}

/* the box a blow is tested on [G]: the shape's box (a sphere's bounding cube) in the object's frame */
static void obj_box(const Rail *r, RailObj *o, float mn[3], float mx[3])
{
    CollideShape s;
    obj_shape(r, o, &s);
    for (int k = 0; k < 3; k++) {
        mn[k] = s.centre[k] - s.half[k];
        mx[k] = s.centre[k] + s.half[k];
    }
}
static void to_local(const RailObj *o, const float p[3], float l[3])   /* rot^T (p - pos) */
{
    float d[3] = { p[0] - o->pos[0], p[1] - o->pos[1], p[2] - o->pos[2] };
    for (int k = 0; k < 3; k++)
        l[k] = o->rot[k * 3] * d[0] + o->rot[k * 3 + 1] * d[1] + o->rot[k * 3 + 2] * d[2];
}

/* each frame [G: where the game's touch comes from is not traced; as boss.c]: an object without OnlyHitWithEffNodes
 * that a monster's box (0x2E2C50's: its feet +- half width across, +- half height about its middle) meets
 * (collide_obj_box, the world's shape test 0x2801A0) takes TakeDamage(monster, 0, Collision (1)) every frame (how a
 * speeding train knocks a monster off the track). A PushOutMonsters object's push is the collision world's
 * (collide_monster), not this */
static void contacts(Rail *r)
{
    for (int i = 0; i < r->n; i++) {
        RailObj *o = &r->o[i];
        if (o->phase != RP_ALIVE || o->hidden || is_controller(o->kind) || o->kind == RK_LOOPTRAIN ||
            (o->oflags & RF_EFF_NODES))
            continue;
        CollideObj co;
        obj_collide(r, o, &co);
        for (int m = 0; m < r->nm && o->phase == RP_ALIVE; m++) {
            const ArmyMonster *mm = &r->m[m];
            float bmn[3], bmx[3], c[3], e[3], nrm[3], pt[3], push[3];
            if (mm->box_max[0] > mm->box_min[0]) {
                copy3(bmn, mm->box_min);
                copy3(bmx, mm->box_max);
            } else {
                set3(bmn, mm->pos[0] - 3, mm->pos[1] - 3, mm->pos[2]);
                set3(bmx, mm->pos[0] + 3, mm->pos[1] + 3, mm->pos[2] + 8.5f);
            }
            for (int k = 0; k < 3; k++) {
                c[k] = (bmn[k] + bmx[k]) / 2;
                e[k] = (bmx[k] - bmn[k]) / 2;
            }
            if (collide_obj_box(&co, c, e, nrm, pt, push))
                take_damage(r, o, m, 1);
        }
    }
}

/* ---------------------------------------------------------------- the frame */

static void obj_step(Rail *r, RailObj *o)
{
    if (o->phase == RP_GONE) {
        if (o->gone_age < 1000)
            o->gone_age++;
        return;
    }
    if (o->phase == RP_ALIVE) {
        int cb = 0;
        if (o->mv == MV_LINE)
            cb = line_step(r, o, TIMESTEP);
        else if (o->mv == MV_SPLINE)
            cb = sp_step(o, TIMESTEP);
        else if (o->mv == MV_SPHERE)
            sphere_step(r, o, TIMESTEP);             /* OnHitGround, TakeImpact: its contacts */
        if (cb && on_mover(r, o, cb))
            return;
        if (o->phase == RP_GONE)
            return;
    }
    if (o->kind == RK_ETRAIN)
        et_process(r, o);
    /* [R 0x309BA0] onPulse once pulseDelay has gone by since the last; a delay below 0.01 neither runs the clock nor
     * pulses (setPulseDelay(0) stops them) */
    if (o->pulse_delay >= 0.01f) {
        o->pulse_timer += TIMESTEP;
        if (o->pulse_timer >= o->pulse_delay) {
            o->pulse_timer -= o->pulse_delay;
            if (on_pulse(r, o))
                return;
        }
    }
    if (o->phase == RP_GONE)
        return;
    if (!o->own_rot)
        rot_of_facing(o->rot, o->facing);
    update_world(o);
}

void rail_update(Rail *r, float dt, const ArmyMonster *m, int nm, const RailBox *b, int nb)
{
    r->m = m;
    r->nm = m ? nm : 0;
    r->b = b;
    r->nb = b ? nb : 0;
    r->acc += dt;
    if (r->acc > 0.25f)
        r->acc = 0.25f;
    while (r->acc >= TIMESTEP) {                     /* the scripts count in frames: run them at the game's step */
        r->acc -= TIMESTEP;
        r->time += TIMESTEP;
        spawner_update(r, TIMESTEP);
        for (int i = 0; i < r->n; i++)
            obj_step(r, &r->o[i]);
        contacts(r);
    }
}

/* ---------------------------------------------------------------- the monster's side */

int rail_hit(Rail *r, int monster, const float centre[3], float radius, float amount)
{
    (void)amount;                                    /* the scripts do not look at it */
    int hits = 0;
    for (int i = 0; i < r->n; i++) {
        RailObj *o = &r->o[i];
        if (o->phase != RP_ALIVE || o->hidden || is_controller(o->kind) || o->kind == RK_LOOPTRAIN)
            continue;
        float mn[3], mx[3], l[3], q = 0;
        obj_box(r, o, mn, mx);
        to_local(o, centre, l);
        for (int k = 0; k < 3; k++) {
            float c = l[k] < mn[k] ? mn[k] : l[k] > mx[k] ? mx[k] : l[k];
            q += (l[k] - c) * (l[k] - c);
        }
        if (q > radius * radius)
            continue;
        take_damage(r, o, monster, 0);
        hits++;
    }
    return hits;
}

void rail_set_collide(Rail *r, CollideWorld *w) { r->cw = w; }

void rail_collide_add(Rail *r, CollideWorld *w)
{
    for (int i = 0; i < r->n; i++) {
        RailObj *o = &r->o[i];
        o->cobj = -1;
        if (!w || o->phase != RP_ALIVE || o->hidden || is_controller(o->kind))
            continue;                                /* [G] a held one is the hand's */
        CollideObj c;
        obj_collide(r, o, &c);
        o->cobj = collide_add(w, &c.shape, c.m, c.flags, COLLIDE_TAG(COLLIDE_OWN_RAIL, i), 0);
    }
}

void rail_impact(Rail *r, int obj)
{
    if (obj >= 0 && obj < r->n && r->o[obj].phase == RP_ALIVE)
        rail_take_impact(r, &r->o[obj]);
}

int rail_nearest_pickup(const Rail *r, const float hand[3], const float root[3], const float forward[3], float radius)
{
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < r->n; i++) {
        const RailObj *o = &r->o[i];
        if (o->phase != RP_ALIVE || !(o->oflags & RF_PICKUP))
            continue;
        float d = dist3(o->pos, hand);
        float f = (o->pos[0] - root[0]) * forward[0] + (o->pos[1] - root[1]) * forward[1] +
                  (o->pos[2] - root[2]) * forward[2];
        if (d <= radius && f > 0 && d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* [R] the hold (0x2DA310): BeingHeld, neither Pickupable nor PushOutMonsters, its mover replaced by the
 * HeldObjectMover (a train's run and a cable car's stages stop), then onPickup */
void rail_grab(Rail *r, int obj, int monster)
{
    (void)monster;
    if (obj < 0 || obj >= r->n || r->o[obj].phase != RP_ALIVE)
        return;
    RailObj *o = &r->o[obj];
    o->oflags = (o->oflags | RF_HELD) & ~(uint32_t)(RF_PICKUP | RF_PUSH_OUT);
    o->phase = RP_HELD;
    o->mv = MV_HELD;
    o->own_rot = 1;
    if (o->kind == RK_ETRAIN)
        et_pickup(r, o);
    else if (o->kind == RK_CABLECAR)
        cc_pickup(r, o);
}

/* the HeldObjectMover (init 0x310ED0): [R] HoldOffset (MirroredHoldOffset while the monster is mirrored) taken along
 * the hand's axes; [G] the object turned as the hand is (the MajorAxis frame it builds is not ported) */
void rail_hold(Rail *r, int obj, const float hand_world[16], int mirrored)
{
    if (obj < 0 || obj >= r->n || r->o[obj].phase != RP_HELD)
        return;
    RailObj *o = &r->o[obj];
    float off[3] = { 0, 0, 0 };
    Group g = group_sub(o->tmpl, mirrored ? "MirroredHoldOffset" : "HoldOffset");
    Record rec[3];
    if (g.at && group_items(g, rec, 3) == 3)
        for (int k = 0; k < 3; k++)
            off[k] = record_num(rec[k]);
    /* [R 0x310ED0] the turn kept in the hand: the MajorAxis row is the hand's z, the next its y, the third their cross
     * (the trains and cable cars have MajorAxis 1: x = -hand x, y = hand z, z = hand y) */
    int major = (int)tnum(o, "MajorAxis", 2);
    if (major < 0 || major > 2)
        major = 2;
    int i1 = (major + 1) % 3, i2 = (major + 2) % 3;
    float *ax = o->rot;
    for (int k = 0; k < 3; k++) {
        ax[major * 3 + k] = hand_world[8 + k];
        ax[i1 * 3 + k] = hand_world[4 + k];
    }
    ax[i2 * 3 + 0] = ax[major * 3 + 1] * ax[i1 * 3 + 2] - ax[major * 3 + 2] * ax[i1 * 3 + 1];
    ax[i2 * 3 + 1] = ax[major * 3 + 2] * ax[i1 * 3 + 0] - ax[major * 3 + 0] * ax[i1 * 3 + 2];
    ax[i2 * 3 + 2] = ax[major * 3 + 0] * ax[i1 * 3 + 1] - ax[major * 3 + 1] * ax[i1 * 3 + 0];
    for (int k = 0; k < 3; k++)
        o->pos[k] = hand_world[12 + k] + hand_world[k] * off[0] + hand_world[4 + k] * off[1] + hand_world[8 + k] * off[2];
    update_world(o);
}

/* throwObject (0x2D9DC0): [R] the object gets its mover with the throw's velocity, then onThrow(monster, eat); [G]
 * the SphereMover, keeping the hand's turn (vehicle.c's) */
void rail_throw(Rail *r, int obj, int monster, int eat, const float vel[3])
{
    (void)eat;                                       /* neither script looks at bWasEaten */
    if (obj < 0 || obj >= r->n || r->o[obj].phase != RP_HELD)
        return;
    RailObj *o = &r->o[obj];
    o->oflags &= ~(uint32_t)RF_HELD;
    o->phase = RP_ALIVE;
    float v[3] = { 0, 0, 0 };
    if (vel)
        copy3(v, vel);
    sphere_init(o, v, NULL);
    if (o->kind == RK_ETRAIN)
        et_throw(r, o, monster);
    else if (o->kind == RK_CABLECAR)
        cc_throw(r, o, monster);
    if (o->phase != RP_GONE)
        update_world(o);
}

/* ---------------------------------------------------------------- the rest */

Rail *rail_new(const RailHost *host)
{
    Rail *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    if (host)
        r->host = *host;
    r->rng = 0x7A11C0DEu;
    r->next_fx = 1;
    r->next_sfx = 1;
    return r;
}

void rail_free(Rail *r) { free(r); }
void rail_seed(Rail *r, uint32_t seed) { r->rng = seed; }
void rail_bind_army(Rail *r, Army *army) { r->army = army; }

void rail_clear(Rail *r)
{
    for (int i = 0; i < r->n; i++)
        obj_delete(r, &r->o[i]);
    r->nsp = 0;
    r->active = 0;
    r->et_given_left = r->et_given_right = 0;
    r->acc = 0;
}

int rail_count(const Rail *r) { return r->n; }
const RailObj *rail_obj(const Rail *r, int i) { return i >= 0 && i < r->n ? &r->o[i] : NULL; }

int rail_events(Rail *r, RailEvent *out, int max)
{
    int n = r->nev < max ? r->nev : max;
    memcpy(out, r->ev, sizeof *out * (size_t)n);
    memmove(r->ev, r->ev + n, sizeof *r->ev * (size_t)(r->nev - n));
    r->nev -= n;
    return n;
}
