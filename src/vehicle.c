/* vehicle.c - the street traffic, the fire truck and the rockets (vehicle.h; docs/VEHICLE_NOTES.md).
 * [R] marks what is read from the game's code or scripts, [G] what is a guess or a stand-in. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vehicle.h"
#include "wrand.h"

#define PI 3.14159265f
#define TIMESTEP (1.0f / 30.0f)                      /* gWorld.getTimeStep(): its step, 1/30 [R 0x2B7800] */
#define MAX_OBJS 128
#define MAX_EVENTS 512
#define SLOTS 3                                      /* the current block and the two behind it */
#define MAX_TYPES 16

typedef struct {                                     /* a VehicleDef, 0x10 bytes (0x334240) [R] */
    const char *name;                                /* +0 Name (OTRef|Name) */
    int alive;                                       /* +4 */
    int max_count;                                   /* +8 MaxCount, 10000 if absent */
    float freq;                                      /* +C Frequency, 1 if absent */
} TrafficType;

typedef struct {                                     /* a block slot's record, 0x28 bytes (0x334240) [R] */
    int loaded;
    int max_vehicles;                                /* +0 MaxVehicles (7) - 2 x slot: the first fill's count */
    float min_release;                               /* +4 MinReleaseTime (1): read, never used */
    float rear, front;                               /* +C, +10 the lanes' release timers, 2 + 3 x rand */
    TrafficType t[MAX_TYPES]; int nt;                /* +14 .. +1C */
    float freq_sum;                                  /* +20 */
    uint32_t lanes_off;                              /* +24: 1 NoFrontTraffic, 2 NoRearTraffic */
} TrafficSlot;

struct Vehicles {
    VehicleHost host;
    Army *army;
    VehObj o[MAX_OBJS]; int n;
    VehicleEvent ev[MAX_EVENTS]; int nev, next_fx;
    TrafficSlot slot[SLOTS]; int nslots;             /* gAIMgr +4 (records), +C (their count) */
    int filled;                                      /* gAIMgr +0x50 bit 2: the first fill is done */
    int block_vehicles;                              /* gp - 0x6C78: vehicles of slot 0 */
    /* this frame */
    const ArmyMonster *m; int nm;
    const ArmyBox *b; int nb;
    float acc;
    CollideWorld *cw;                                /* the frame's collision world (vehicle_set_collide), NULL none */
};

/* ---------------------------------------------------------------- small things */

/* gWorld.frand() (0x279F98): the world's Park-Miller, gWorld +0x828 (wrand.c) [R] */
static float frand(Vehicles *v)
{
    (void)v;
    return wrand_frand();
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

static float len3(const float a[3]) { return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
static void norm3(float a[3])
{
    float l = len3(a);
    if (l < 1e-6f) {                                 /* [R] the game's normalize of a null vector gives (1, 0, 0) */
        a[0] = 1;
        a[1] = a[2] = 0;
    } else {
        a[0] /= l;
        a[1] /= l;
        a[2] /= l;
    }
}
static float facing_of(float dx, float dy) { return atan2f(-dx, dy); }   /* the facing that looks along (dx, dy) */
static float wrap_pi(float a)
{
    while (a > PI)
        a -= 2 * PI;
    while (a < -PI)
        a += 2 * PI;
    return a;
}

/* 3 x 3 column-major: m[c * 3 + r] */
static void rot_z(float r[9], float a)
{
    float c = cosf(a), s = sinf(a);
    r[0] = c; r[1] = s; r[2] = 0;
    r[3] = -s; r[4] = c; r[5] = 0;
    r[6] = 0; r[7] = 0; r[8] = 1;
}
static void mul33(const float a[9], const float b[9], float out[9])
{
    float t[9];
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            t[c * 3 + r] = a[r] * b[c * 3] + a[3 + r] * b[c * 3 + 1] + a[6 + r] * b[c * 3 + 2];
    memcpy(out, t, sizeof t);
}
/* WorldObject.rotate(e) (case 0xD, 0x278258) [R]: R = 0x101330(z, x, y) = Rz(e[2]) Rx(e[0]) Ry(e[1]), as setFacing,
 * taken in the object's own frame: rot = rot x R */
static void rotate_local(float rot[9], const float e[3])
{
    float cx = cosf(e[0]), sx = sinf(e[0]), cy = cosf(e[1]), sy = sinf(e[1]);
    float rx[9] = { 1, 0, 0, 0, cx, sx, 0, -sx, cx }, ry[9] = { cy, 0, -sy, 0, 1, 0, sy, 0, cy }, rz[9];
    rot_z(rz, e[2]);
    mul33(rot, rz, rot);
    mul33(rot, rx, rot);
    mul33(rot, ry, rot);
}

static void update_world(VehObj *o)
{
    float *w = o->world;
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++)
            w[c * 4 + r] = o->rot[c * 3 + r];
        w[c * 4 + 3] = 0;
    }
    w[12] = o->pos[0];
    w[13] = o->pos[1];
    w[14] = o->pos[2];
    w[15] = 1;
}
static void set_facing(VehObj *o, float f)           /* setFacingZ */
{
    o->facing = f;
    rot_z(o->rot, f);
}

/* ---------------------------------------------------------------- properties */

/* getStringProp / getFloatProp: the books keep a record's type before a '|' ("Effect|DamageEffect",
 * "Sound|HornSound", "OTRef|Base"); the scripts ask by the bare name, so the typed names are tried too [G, as army.c].
 * Lookups follow ParentPropertySet (DefObjectProps: Gravity 35, Points 10). The objects here have no instance book of
 * their own, so getStringProp and getTemplateStringProp read the same template [G] */
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
static float gnum(Group g, const char *key, float def)
{
    Record r;
    return find_rec(g, key, &r) && (r.kind & 0xFF) <= PK_FLOAT ? record_num(r) : def;
}
static const char *tstr(const VehObj *o, const char *key) { return gstr(o->tmpl, key); }
static float tnum(const VehObj *o, const char *key, float def) { return gnum(o->tmpl, key, def); }

static Group find_template(Vehicles *v, const char *name)
{
    return v->host.find && name ? v->host.find(v->host.ctx, "Objects1_Props", name) : (Group){ 0 };
}

static int kind_of_template(Group t)
{
    const char *s = gstr(t, "ScriptName");
    if (same_text(s, "Vehicle.bsf"))
        return VK_CAR;
    if (same_text(s, "Vehicle_FireTruck.bsf"))
        return VK_FIRETRUCK;
    if (same_text(s, "Rocket.bsf"))
        return VK_ROCKET;
    if (same_text(s, "RocketBase.bsf"))
        return VK_ROCKET_PART;
    return -1;
}

/* ---------------------------------------------------------------- events */

static int idx(const Vehicles *v, const VehObj *o) { return (int)(o - v->o); }

static VehicleEvent *emit(Vehicles *v, int type, int obj, const char *name, const float pos[3])
{
    static VehicleEvent spare;
    if (v->nev >= MAX_EVENTS) {
        memset(&spare, 0, sizeof spare);
        return &spare;
    }
    VehicleEvent *e = &v->ev[v->nev++];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->obj = obj;
    e->monster = -1;
    e->name = name;
    e->dir[2] = 1;
    if (pos)
        memcpy(e->pos, pos, sizeof e->pos);
    return e;
}

/* FollowEffect.createAndStart(pos, name) */
static int effect_at(Vehicles *v, const char *name, const float pos[3])
{
    if (!name)
        return -1;
    VehicleEvent *e = emit(v, VE_EFFECT, -1, name, pos);
    return e->id = v->next_fx++;
}
/* FollowEffect.createAndStartAttached(object, name): [G] at the object's origin, following it */
static int effect_on(Vehicles *v, VehObj *o, const char *name)
{
    if (!name)
        return -1;
    VehicleEvent *e = emit(v, VE_EFFECT, idx(v, o), name, o->pos);
    return e->id = v->next_fx++;
}
static void effect_stop(Vehicles *v, int id)
{
    if (id >= 0)
        emit(v, VE_EFFECT_STOP, -1, NULL, NULL)->id = id;
}
static void sound_on(Vehicles *v, VehObj *o, const char *name, int level)   /* add3DSfx(name, level) */
{
    if (name)
        emit(v, VE_SOUND, idx(v, o), name, o->pos)->level = level;
}
static void set_shape(Vehicles *v, VehObj *o, const char *shape)   /* setShape */
{
    if (!shape)
        return;
    o->shape = shape;
    emit(v, VE_SHAPE, idx(v, o), shape, o->pos);
}

/* gCommon.addStats(obj, slot, type) (0x2C51A0) [R]: once per object (flag 0x40000000); the gains and the points go
 * through army.c's addStats when an Army is bound */
static void add_stats(Vehicles *v, VehObj *o, int monster, int type)
{
    if (o->paid)
        return;
    o->paid = 1;
    VehicleEvent *e = emit(v, VE_STATS, idx(v, o), o->name, o->pos);
    e->monster = monster;
    e->level = type;
    e->amount = (float)(int)tnum(o, "Points", 0);
    e->v[0] = tnum(o, "Health", 0);
    e->v[1] = tnum(o, "Energy", 0);
    if (v->army) {
        army_add_stats(v->army, monster, o->tmpl, type);
        e->id = 1;
    }
}

/* ---------------------------------------------------------------- the objects */

static VehObj *obj_new(Vehicles *v, const char *name, Group tmpl, int kind)
{
    int k = -1;
    for (int i = 0; i < v->n; i++)
        if (v->o[i].phase == VP_GONE && v->o[i].gone_age >= 2) {   /* the caller has seen it go */
            k = i;
            break;
        }
    if (k < 0) {
        if (v->n >= MAX_OBJS)
            return NULL;
        k = v->n++;
    }
    VehObj *o = &v->o[k];
    memset(o, 0, sizeof *o);
    o->name = name;
    o->tmpl = tmpl;
    o->kind = kind;
    o->phase = VP_ALIVE;
    o->slot = o->building = o->parent = o->child[0] = o->child[1] = -1;
    o->ambient_fx = o->beam_fx = -1;
    o->ft_building = -1;
    o->cobj = -1;
    o->shape = gstr(tmpl, "Shape");
    rot_z(o->rot, 0);
    return o;
}

/* requestDelete, or an eaten object */
static void obj_delete(Vehicles *v, VehObj *o)
{
    if (o->phase == VP_GONE)
        return;
    o->phase = VP_GONE;
    o->gone_age = 0;
    o->oflags = 0;
    o->line.on = o->sphere.on = 0;
    effect_stop(v, o->ambient_fx);                   /* [R] setTurnOffWhenObjectDies(1): the police lights */
    o->ambient_fx = -1;
    effect_stop(v, o->beam_fx);
    o->beam_fx = -1;
    o->beam.on = 0;
    emit(v, VE_REMOVED, idx(v, o), o->name, o->pos);
}

static void set_pulse(VehObj *o, float delay)        /* setPulseDelay (0x2AA2A0): only the delay [R] */
{
    o->pulse_delay = delay;
}

/* ---------------------------------------------------------------- the movers */

/* LineMover.init(obj, a, b, speed, share) (0x3173D0, 0x317650) [R]: +0x54 the speed, +0x58 the share of the way
 * done (the second form starts part way: the traffic's first fill), +0x5C MonsterResponseDist squared from the
 * object's template, or -1 */
static void line_init(VehObj *o, const float a[3], const float b[3], float speed, float share)
{
    memcpy(o->line.a, a, sizeof o->line.a);
    memcpy(o->line.b, b, sizeof o->line.b);
    o->line.speed = speed;
    o->line.t = share;
    o->line.on = 1;
    float r = tnum(o, "MonsterResponseDist", -1);
    o->line.resp2 = r >= 0 ? r * r : -1;
    for (int k = 0; k < 3; k++)
        o->pos[k] = a[k] + (b[k] - a[k]) * share;
}

/* the LineMover's frame (0x315E10) [R]: the share grows by dt x speed / length (not when the length is 0); past 1 it
 * stops at the end and calls back 2 [G: once]; while it simply moves, a monster nearer than MonsterResponseDist
 * (3D, squared) calls back 4. Returns the callback, 0 none (1, "moving", is not passed on) */
static int line_step(Vehicles *v, VehObj *o, float dt)
{
    if (!o->line.on)
        return 0;
    float d[3] = { o->line.b[0] - o->line.a[0], o->line.b[1] - o->line.a[1], o->line.b[2] - o->line.a[2] };
    float len = len3(d);
    int r = 1;
    if (len != 0)
        o->line.t += dt * o->line.speed / len;
    if (o->line.t > 1) {
        o->line.t = 1;
        o->line.on = 0;
        r = 2;
    }
    for (int k = 0; k < 3; k++)
        o->pos[k] = o->line.a[k] + d[k] * o->line.t;
    if (r == 1 && o->line.resp2 > 0)
        for (int i = 0; i < v->nm; i++) {
            float e[3] = { o->pos[0] - v->m[i].pos[0], o->pos[1] - v->m[i].pos[1], o->pos[2] - v->m[i].pos[2] };
            if (e[0] * e[0] + e[1] * e[1] + e[2] * e[2] < o->line.resp2)
                r = 4;
        }
    return r == 1 ? 0 : r;
}

/* a book's collision keys (WSimpleObject 0x28FD70): CollisionBox { min x y z, max x y z } */
static const float *book_box(Group g, float b[6])
{
    Record r[8];
    Group cb = group_sub(g, "CollisionBox|CollisionBox");
    if (!cb.at)
        cb = group_sub(g, "CollisionBox");
    if (!cb.at || group_items(cb, r, 8) < 6)
        return NULL;
    for (int k = 0; k < 6; k++)
        b[k] = record_num(r[k]);
    return b;
}

/* the object's shape (0x28FD70, collide_shape_pick): the template's CollisionBox, CollisionRadius or
 * AutoCollisionBox with the model's box; else the model's bounding sphere [G: taken as its box's, the host's] */
static void obj_shape_read(const Vehicles *v, const VehObj *o, CollideShape *s)
{
    float tb[6], mb[6], c[3] = { 0, 0, 0 }, r = 1;
    int have = o->shape && v->host.shape_box && v->host.shape_box(v->host.ctx, o->shape, mb, mb + 3);
    if (have) {
        float h[3];
        for (int k = 0; k < 3; k++) {
            c[k] = (mb[k] + mb[3 + k]) / 2;
            h[k] = (mb[3 + k] - mb[k]) / 2;
        }
        r = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    }
    /* [R 0x28FD70 -> 0x184CB0] the sphere the model keeps, the union of its parts' spheres (0x185AA0) */
    if (have && v->host.shape_sphere)
        v->host.shape_sphere(v->host.ctx, o->shape, c, &r);
    collide_shape_pick(s, have ? c : NULL, r, o->tmpl.at ? book_box(o->tmpl, tb) : NULL, tnum(o, "CollisionRadius", -1),
                       tnum(o, "AutoCollisionBox", 0) != 0, NULL, -1, 0, have ? mb : NULL);
}

static void obj_shape(const Vehicles *v, VehObj *o, CollideShape *s)   /* read once, at its first use */
{
    if (!o->cshape_ok) {
        obj_shape_read(v, o, &o->cshape);
        o->cshape_ok = 1;
    }
    *s = o->cshape;
}

static void car_hit_ground(Vehicles *v, VehObj *o);
typedef struct { Vehicles *v; VehObj *o; } FlightCtx;

/* a contact of the flight: the street is OnHitGround (event 11, every sub-step on it [R 0x28E440]); an object is
 * TakeImpact on both (0x287430): Vehicle.bsf's wrecks it; on another module's object through VE_IMPACT */
static void flight_contact(void *user, int other, const float p[3], const float n[3])
{
    FlightCtx *c = user;
    Vehicles *v = c->v;
    if (other < 0) {
        if (getenv("RAMPAGE_TRACE") && c->o->cm.vel[2] > 0.5f)   /* a bounce (the impulse is in) */
            printf("  %s bounced off the street at %.1f %.1f %.1f\n", c->o->name ? c->o->name : "?", p[0], p[1], p[2]);
        if (c->o->kind == VK_CAR && c->o->phase != VP_GONE)
            car_hit_ground(v, c->o);
        return;
    }
    if (!v->cw)
        return;
    const CollideObj *w = &v->cw->o[other];
    if (getenv("RAMPAGE_TRACE"))
        printf("  %s met %d.%d at %.1f %.1f %.1f\n", c->o->name ? c->o->name : "?", COLLIDE_TAG_OWNER(w->tag),
               COLLIDE_TAG_INDEX(w->tag), p[0], p[1], p[2]);
    vehicle_impact(v, idx(v, c->o));
    if (COLLIDE_TAG_OWNER(w->tag) == COLLIDE_OWN_VEHICLE) {
        if (COLLIDE_TAG_INDEX(w->tag) != idx(v, c->o))
            vehicle_impact(v, COLLIDE_TAG_INDEX(w->tag));
        return;
    }
    VehicleEvent *e = emit(v, VE_IMPACT, idx(v, c->o), c->o->name, p);
    e->id = w->tag;
    e->level = w->kind;
    memcpy(e->dir, n, sizeof e->dir);
    e->amount = c->o->cm.mass;
}

/* SphereMover (setVelocity) and the smash's SpherePhysicsMover: the game's (collide.c, 0x28E440) with the template's
 * Gravity (DefObjectProps: 35), Mass, Restitution, SlidingFriction and InertiaSolidity (0x28F220); its spin the
 * smash's turn rates about the object's own axes; the street (OnHitGround) and the frame's objects (TakeImpact both
 * ways: a building is hit, Vehicle.bsf's TakeImpact wrecks the car). No rest: it rolls on until the script ends it */
static void sphere_step(Vehicles *v, VehObj *o, float dt)
{
    if (!o->sphere.on)
        return;
    if (!o->cm_on) {
        CollideShape sh;
        obj_shape(v, o, &sh);
        float r = collide_mover_radius(&sh);
        if (r < 0.05f)
            r = sh.radius > 0.05f ? sh.radius : 1;
        collide_mover_init(&o->cm, r, tnum(o, "Gravity", 9.81f), tnum(o, "Mass", 1), tnum(o, "Restitution", 0.8f),
                           tnum(o, "SlidingFriction", 0.4f), tnum(o, "InertiaSolidity", 1));
        float w[3];
        for (int k = 0; k < 3; k++)                  /* [R 0x2B2F50 -> 0x28DD80] the rates as given: a world axis */
            w[k] = o->sphere.tumble ? o->sphere.spin[k] : 0;
        collide_mover_spin(&o->cm, w);
        memcpy(o->cmat, o->rot, sizeof o->rot);
        memcpy(o->cmat + 9, o->pos, sizeof o->pos);
        o->cm_on = 1;
    }
    memcpy(o->cm.vel, o->sphere.vel, sizeof o->cm.vel);
    int self = collide_check(v->cw, o->cobj, COLLIDE_TAG(COLLIDE_OWN_VEHICLE, idx(v, o)), 0);
    FlightCtx c = { v, o };
    collide_mover_step_ex(v->cw, &o->cm, o->cmat, self, dt, COLLIDE_F_MONSTER, flight_contact, &c);
    memcpy(o->sphere.vel, o->cm.vel, sizeof o->sphere.vel);
    memcpy(o->rot, o->cmat, sizeof o->rot);
    memcpy(o->pos, o->cmat + 9, sizeof o->pos);
}

/* smashObject (0x2B2F50) on a street object (class 0x4D0720) [R, as army.c / people.c]: it flies off on a
 * SpherePhysicsMover, 15 along a yaw in the 120 degree cone towards the camera (narrowed on the side the monster
 * faces) and 15 x (0.8 .. 1.3) up, spinning +-(2 pi + pi/6 r) about x and +-(pi/16 + pi/16 r) about y and z */
static void smash_object(Vehicles *v, VehObj *o, float monster_heading)
{
    const float c = 3 * PI / 2, lo = 20 * PI / 180, wd = 120 * PI / 180;
    float A = sinf(c), B = cosf(c), A2 = sinf(monster_heading + PI / 2), B2 = cosf(monster_heading + PI / 2);
    float d = -(B2 * B + A2 * A), cross = B2 * A - A2 * B;
    float width = lo + (wd - lo) * (1 - d) / 2, a0 = c - wd / 2, a1 = c + wd / 2;
    if (cross < 0)
        a0 = a1 - width;
    else
        a1 = a0 + width;
    float yaw = a0 + (a1 - a0) * frand(v);
    o->sphere.vel[0] = 15 * cosf(yaw);
    o->sphere.vel[1] = 15 * sinf(yaw);
    o->sphere.vel[2] = 15 * (0.8f + 0.5f * frand(v));
    o->sphere.spin[0] = (frand(v) < 0.5f ? 1 : -1) * (2 * PI + PI / 6 * frand(v));
    o->sphere.spin[1] = (frand(v) < 0.5f ? 1 : -1) * (PI / 16 + PI / 16 * frand(v));
    o->sphere.spin[2] = (frand(v) < 0.5f ? 1 : -1) * (PI / 16 + PI / 16 * frand(v));
    o->sphere.on = o->sphere.tumble = 1;
    o->cm_on = 0;
    o->line.on = 0;
}

/* ---------------------------------------------------------------- the traffic (gAIMgr) */

/* gAIMgr.removeVehicle (0x332BA0) [R]: the slot comes from where the vehicle is now (y > 20, y > 60: 0, 1, 2), not
 * where it was released; it leaves the manager's list (if it is in it); every type of that name in that slot has
 * one fewer alive (even when it was not in the list), and slot 0's count with them */
static void remove_vehicle(Vehicles *v, VehObj *o)
{
    int s = (o->pos[1] > 20) + (o->pos[1] > 60);
    o->in_list = 0;
    if (s >= v->nslots || !v->slot[s].loaded)
        return;
    TrafficSlot *ts = &v->slot[s];
    for (int i = 0; i < ts->nt; i++)
        if (same_text(ts->t[i].name, o->name)) {
            ts->t[i].alive--;
            if (s == 0)
                v->block_vehicles--;
        }
}

static void car_construct(Vehicles *v, VehObj *o);

/* the release (0x332D90) [R]: a type picked by Frequency x (min(MaxCount, 5) - alive) among those above 0.001;
 * lane: share >= 1 the rear (from x = 75 to -75 at y = 41 slot - 7.5, turned +pi/2), share < -1 either at random,
 * otherwise the front (from -75 to 75 at y = 41 slot - 12.5, turned -pi/2); a lane switched off releases nothing;
 * a share strictly within -1 .. 1 is where on its line the LineMover (15 a second) starts */
static void release(Vehicles *v, int s, float share)
{
    TrafficSlot *ts = &v->slot[s];
    float total = 0;
    for (int i = 0; i < ts->nt; i++)
        total += ts->t[i].freq * (float)((ts->t[i].max_count < 5 ? ts->t[i].max_count : 5) - ts->t[i].alive);
    if (ts->nt == 0 || total <= 0)
        return;
    float pick = total * frand(v), sum = 0;
    int j = 0;
    for (; j < ts->nt; j++) {
        float w = ts->t[j].freq * (float)((ts->t[j].max_count < 5 ? ts->t[j].max_count : 5) - ts->t[j].alive);
        sum += w;
        if (sum > pick && w > 0.001f)
            break;
    }
    if (j >= ts->nt)
        j = 0;                                       /* [R] the loop runs out on the first */
    Group t = find_template(v, ts->t[j].name);
    int rear = 1;
    if (share < 1) {
        rear = 0;
        if (share < -1)
            rear = frand(v) < 0.5f;
    }
    if (!t.at || (ts->lanes_off & (rear ? 2u : 1u)))
        return;
    VehObj *o = obj_new(v, ts->t[j].name, t, VK_CAR);
    if (!o)
        return;
    if (s == 0)
        v->block_vehicles++;
    ts->t[j].alive++;
    float y = 41.0f * (float)s - (rear ? 7.5f : 12.5f);
    float a[3] = { rear ? 75.0f : -75.0f, y, 0 }, b[3] = { rear ? -75.0f : 75.0f, y, 0 };
    car_construct(v, o);
    set_facing(o, rear ? PI / 2 : -PI / 2);
    line_init(o, a, b, 15, share > -1 && share < 1 ? share : 0);
    o->slot = s;
    o->in_list = 1;
    update_world(o);
}

/* the AI manager's update, its traffic part (0x3333F0) [R]:
 *  - the first time: each slot is filled along its front lane: the share grows by max(0.1, rand x 2 / n) (n =
 *    MaxVehicles - 2 x slot) and a vehicle starts there, while the share is below 1 and fewer than n were made (the
 *    last one can land on the rear lane's start: a share of 1 or more); then nothing else that frame;
 *  - after that, for each slot in turn both timers fall by dt; the front's at 0 starts again at 2 + 3 rand and
 *    releases on the front lane, else the rear's does the same on the rear lane; one release ends the whole update
 *    for that frame (the later slots' timers wait). MaxVehicles plays no part here: each type stops at 5 alive */
static void traffic_update(Vehicles *v, float dt)
{
    if (!v->filled) {
        v->filled = 1;
        for (int s = 0; s < v->nslots; s++) {
            TrafficSlot *ts = &v->slot[s];
            if (!ts->loaded)
                continue;
            float share = 0;
            for (int made = 0; share < 1 && made < ts->max_vehicles && ts->nt > 0; made++) {
                share += fmaxf(0.1f, frand(v) * (1.0f / (0.5f * (float)ts->max_vehicles)));
                release(v, s, share);
            }
        }
        return;
    }
    for (int s = 0; s < v->nslots; s++) {
        TrafficSlot *ts = &v->slot[s];
        if (!ts->loaded)
            continue;
        ts->front -= dt;
        ts->rear -= dt;
        if (ts->front <= 0) {
            ts->front = 2 + 3 * frand(v);
            if (!(ts->lanes_off & 1))
                release(v, s, -1);
            return;
        }
        if (ts->rear <= 0) {
            ts->rear = 2 + 3 * frand(v);
            if (!(ts->lanes_off & 2))
                release(v, s, 1);
            return;
        }
    }
}

int vehicle_load_traffic(Vehicles *v, int s, Group block_def)
{
    if (s < 0 || s >= SLOTS)
        return 0;
    TrafficSlot *ts = &v->slot[s];
    memset(ts, 0, sizeof *ts);
    Group td = group_sub(block_def, "TrafficDef");
    if (!td.at)
        return 0;
    ts->loaded = 1;
    if (s >= v->nslots)
        v->nslots = s + 1;
    ts->max_vehicles = (int)gnum(td, "MaxVehicles", 7) - 2 * s;
    ts->min_release = gnum(td, "MinReleaseTime", 1);
    ts->front = 2 + 3 * frand(v);
    ts->rear = 2 + 3 * frand(v);
    if (gnum(td, "NoFrontTraffic", 0) != 0)
        ts->lanes_off |= 1;
    if (gnum(td, "NoRearTraffic", 0) != 0)
        ts->lanes_off |= 2;
    Group where;
    Record r[MAX_TYPES];
    uint32_t n = group_values(td, "VehicleDef|", &where, r, MAX_TYPES);
    for (uint32_t i = 0; i < n && ts->nt < MAX_TYPES; i++) {
        Group g = record_group(where, r[i]);
        const char *name = gstr(g, "Name");
        if (!name)
            continue;
        TrafficType *t = &ts->t[ts->nt++];
        t->name = name;
        t->alive = 0;
        t->max_count = (int)gnum(g, "MaxCount", 10000);
        t->freq = gnum(g, "Frequency", 1);
        ts->freq_sum += t->freq;
    }
    return ts->nt;
}

/* ---------------------------------------------------------------- Vehicle.bsf */

/* Construct [R] */
static void car_construct(Vehicles *v, VehObj *o)
{
    o->state_time = 0;
    o->flags = 0;
    o->horn = 0;
    o->fly_duration = 3;
    set_pulse(o, 0.5f + frand(v));
    if (tnum(o, "Animates", 0) != 0)
        o->oflags = VF_EFF_NODES | VF_PICKUP | VF_ANIMATES | VF_EDIBLE;
    else if (tnum(o, "Edible", 0) != 0)
        o->oflags = VF_EFF_NODES | VF_PICKUP | VF_EDIBLE;
    else
        o->oflags = VF_EFF_NODES | VF_PICKUP;
    o->ambient_fx = effect_on(v, o, tstr(o, "AmbientEffect"));   /* the police cars' lights */
    sound_on(v, o, tstr(o, "EntranceSound"), 0);   /* the bicycle's bell */
    if (tnum(o, "IsHidden", 0) != 0) {
        o->flags |= CF_HIDDEN;
        o->oflags &= ~(uint32_t)VF_PICKUP;
        set_pulse(o, 1.0f);
        o->hidden = 1;
    }
}

static void car_wreck_shape(Vehicles *v, VehObj *o)
{
    set_shape(v, o, tstr(o, "DamageShape"));        /* getStringProp, else getTemplateStringProp */
}

/* onMoverCallback [R]: at the end of its line (2) a hidden one turns back (a new LineMover from where it is to -x,
 * at 8), any other leaves the manager and is deleted; with a monster near (4) it sounds its HornSound when its count
 * is out, then waits 3 pulses (20 for a hidden one). Returns 1 when it is deleted */
static int car_mover(Vehicles *v, VehObj *o, int state)
{
    if (state == 2) {
        if (o->flags & CF_HIDDEN) {
            float a[3] = { o->pos[0], o->pos[1], o->pos[2] }, b[3] = { -o->pos[0], o->pos[1], o->pos[2] };
            line_init(o, a, b, 8, 0);
            return 0;
        }
        remove_vehicle(v, o);
        obj_delete(v, o);
        return 1;
    }
    if (state == 4 && o->horn == 0) {
        const char *s = tstr(o, "HornSound");
        if (s) {
            sound_on(v, o, s, 0);
            o->horn = tnum(o, "IsHidden", 0) != 0 ? 20 : 3;
        }
    }
    return 0;
}

/* onPulse [R]:
 *  - a smashed edible (flags exactly 4056): 0.25 off its 3 s each pulse (every 0.25 s), deleted below 0;
 *  - wrecked (8): its ExplodeEffect where it is, out of the manager, deleted;
 *  - thrown (32): 0.5 off its 5 s each pulse, out and deleted (no explosion) at 0; the street flags step: touched
 *    since the last pulse (64) clears 64 and 128, otherwise 128 is set (a whole pulse in the air);
 *  - the horn's count falls by 1. Returns 1 when it is deleted */
static int car_pulse(Vehicles *v, VehObj *o)
{
    if (o->flags == CF_SMASHED) {
        o->fly_duration -= 0.25f;
        if (o->fly_duration < 0) {
            obj_delete(v, o);
            return 1;
        }
    } else if (o->flags & CF_WRECK) {
        effect_at(v, tstr(o, "ExplodeEffect"), o->pos);
        remove_vehicle(v, o);
        obj_delete(v, o);
        return 1;
    } else if (o->flags & CF_THROWN) {
        o->state_time -= 0.5f;
        if (o->state_time <= 0) {
            remove_vehicle(v, o);
            obj_delete(v, o);
            return 1;
        }
        if (o->flags & CF_GROUNDED)
            o->flags &= ~(CF_GROUNDED | CF_AIRBORNE);
        else
            o->flags = (o->flags & ~CF_GROUNDED) | CF_AIRBORNE;
        return 0;
    }
    if (o->horn > 0)
        o->horn--;
    return 0;
}

/* onThrow(monster, eaten) [R]: an Edible (the bicycle) leaves the manager; eaten it counts (addStats type 1, a fart
 * one time in five, 2 s later) [G: and the engine takes it away]; not eaten it is smashed away with its
 * FlyingSound, flags 4056, pulsing every 0.25 s. Anything else counts as a destroyed vehicle (addStats type 2), can
 * no longer be picked up, and is "thrown" (32) for 5 s, pulsing every 0.5 s. Returns 1 when it is gone */
static int car_throw(Vehicles *v, VehObj *o, int monster, int eaten, float heading)
{
    if (monster < 0)
        return 0;
    if (tnum(o, "Edible", 0) != 0) {
        remove_vehicle(v, o);
        if (eaten) {
            add_stats(v, o, monster, 1);
            if (frand(v) <= 0.2f) {
                VehicleEvent *e = emit(v, VE_FART, idx(v, o), NULL, o->pos);
                e->monster = monster;
                e->amount = 2;
            }
            obj_delete(v, o);
            return 1;
        }
        smash_object(v, o, heading);
        sound_on(v, o, tstr(o, "FlyingSound"), 1);
        o->flags = CF_SMASHED;
        set_pulse(o, 0.25f);
        return 0;
    }
    add_stats(v, o, monster, 2);
    o->oflags &= ~(uint32_t)VF_PICKUP;
    o->flags |= CF_THROWN;
    o->state_time = 5;
    set_pulse(o, 0.5f);
    o->bounce_count = 0;
    return 0;
}

/* TakeDamage(who, amount, type, direction) [R]: once (not while wrecked, hidden or thrown): wrecked (8), neither
 * hittable nor pickable, its DamageShape, the ambient effect stopped, DamageSound and DamageEffect (both effects)
 * on it; it flies off on a SphereMover at 25 along the way from the monster (z made positive), then onThrow(monster,
 * not eaten): so a car counts, and explodes on its next pulse half a second later; a bicycle is smashed instead */
static int car_damage(Vehicles *v, VehObj *o, int monster, const float direction[3])
{
    if (o->flags & (CF_WRECK | CF_HIDDEN | CF_THROWN))
        return 0;
    o->flags |= CF_WRECK;
    o->oflags &= ~(uint32_t)(VF_EFF_NODES | VF_PICKUP);
    car_wreck_shape(v, o);
    effect_stop(v, o->ambient_fx);
    o->ambient_fx = -1;
    effect_on(v, o, tstr(o, "DamageSound"));
    effect_on(v, o, tstr(o, "DamageEffect"));
    float d[3];
    if (monster >= 0 && monster < v->nm)
        for (int k = 0; k < 3; k++)
            d[k] = o->pos[k] - v->m[monster].pos[k];
    else
        memcpy(d, direction, sizeof d);
    norm3(d);
    for (int k = 0; k < 3; k++)
        o->sphere.vel[k] = d[k] * 25;
    o->sphere.vel[2] = fabsf(o->sphere.vel[2]);
    o->sphere.on = 1;
    o->sphere.tumble = 0;
    o->cm_on = 0;
    o->line.on = 0;                                  /* setMover(the SphereMover) */
    float heading = monster >= 0 && monster < v->nm ? v->m[monster].heading : 0;
    car_throw(v, o, monster >= 0 && monster < v->nm ? monster : -1, 0, heading);
    return 1;
}

/* OnHitGround [R]: 5 for the first touch since the last pulse, 1 for each after; over 40 it is wrecked (it explodes
 * on its next pulse); a touch after a whole pulse in the air plays its DamageEffect again. [G] The SphereMover calls
 * it every frame the object touches the street while it still moves */
static void car_hit_ground(Vehicles *v, VehObj *o)
{
    o->bounce_count += (o->flags & CF_GROUNDED) ? 1 : 5;
    if (o->bounce_count > 40) {
        o->flags |= CF_WRECK;
        return;
    }
    o->flags |= CF_GROUNDED;
    if ((o->flags & (CF_WRECK | CF_AIRBORNE)) == CF_AIRBORNE) {
        effect_on(v, o, tstr(o, "DamageEffect"));
        o->flags &= ~CF_AIRBORNE;
    }
}

/* onPickup [R]: the DamageShape, PickupSound and DamageEffect (effects) on it; an IsShaker (buses, the tram, the
 * garbage truck) its PourEffect, ScreamSound and ShakerSound. The books give the pour as an unnamed "Effect|" list
 * element ("People_pour", "Garbage_pour") that getTemplateStringProp("PourEffect") does not find, so none plays */
static void car_pickup(Vehicles *v, VehObj *o)
{
    car_wreck_shape(v, o);
    effect_on(v, o, tstr(o, "PickupSound"));
    effect_on(v, o, tstr(o, "DamageEffect"));
    if (tnum(o, "IsShaker", 0) != 0) {
        effect_on(v, o, tstr(o, "PourEffect"));
        sound_on(v, o, tstr(o, "ScreamSound"), 0);
        sound_on(v, o, tstr(o, "ShakerSound"), 0);
    }
    o->flags |= CF_PICKED;
}

/* ---------------------------------------------------------------- Vehicle_FireTruck.bsf */

static Group weapon_group(Group tmpl, Group *book_out, Vehicles *v)
{
    Group ws = group_sub(tmpl, "WeaponSystem|WeaponSystem"), wc = group_sub(ws, "WeaponCluster|WeaponCluster"), where;
    Record r[4];
    Group w = { 0 };
    if (wc.at && group_values(wc, "Weapon|", &where, r, 4) > 0)
        w = record_group(where, r[0]);
    const char *name = gstr(w, "WeaponName");
    *book_out = v->host.find && name ? v->host.find(v->host.ctx, "Weapons", name) : (Group){ 0 };
    return w;
}
/* [R] 0x299EC8: the template's own Weapon group first, then the book's (Weapons.bdp) */
static float wnum(Group own, Group book, const char *key, float def)
{
    Record r;
    if (find_rec(own, key, &r) && (r.kind & 0xFF) <= PK_FLOAT)
        return record_num(r);
    return gnum(book, key, def);
}
static const char *wstr(Group own, Group book, const char *key)
{
    const char *s = gstr(own, key);
    return s ? s : gstr(book, key);
}

/* onInit [R]: pulses every frame; a random building; EntryDelay (read, unused), TargetDistance 5, FireDuration 5,
 * MaxVelocity 8; from x = +-75 (a coin) at y = -15 to the other side on a LineMover at 8, facing the way it goes.
 * The weapon (FireEngineStream, an RBeamWeapon on WeaponNode_Y) and the WeaponCluster's turn rates as army.c reads
 * them [R]; their units [G: radians a second, degrees for the ranges] */
static void ft_init(Vehicles *v, VehObj *o)
{
    set_pulse(o, TIMESTEP);
    o->ft_building = -2;                             /* picked at the first pulse, when the buildings are known */
    o->ft_target_dist = tnum(o, "TargetDistance", 5);
    o->ft_fire_left = tnum(o, "FireDuration", 5);
    o->ft_velocity = tnum(o, "MaxVelocity", 8);
    float a[3] = { 75, -15, 0 };
    if (frand(v) < 0.5f)
        a[0] = -a[0];
    float b[3] = { -a[0], a[1], a[2] };
    line_init(o, a, b, 8, 0);
    set_facing(o, a[0] < 0 ? -PI / 2 : PI / 2);
    Group book, own = weapon_group(o->tmpl, &book, v);
    Group ws = group_sub(o->tmpl, "WeaponSystem|WeaponSystem"), wc = group_sub(ws, "WeaponCluster|WeaponCluster");
    o->w.name = wstr(own, book, "WeaponName");
    o->w.node = wstr(own, book, "WeaponNode");
    o->w.fire_fx = wstr(own, book, "FireEffect");
    o->w.impact_fx = wstr(own, book, "ImpactEffect");
    int ammo = (int)wnum(own, book, "Ammo", 0), start = (int)wnum(own, book, "StartAmmo", -1);
    o->w.ammo = start == -1 ? ammo : start;
    o->w.counts = ammo != 0;
    o->w.reaction = (int)wnum(own, book, "ReactionLevel", -1);
    o->w.charge = wnum(own, book, "ChargeTime", 0);
    o->w.offset = wnum(own, book, "FiringOffset", 0);
    o->w.refire = wnum(own, book, "RefireDelay", 0);
    o->w.duration = wnum(own, book, "Duration", 0);
    o->w.damage = wnum(own, book, "ImpactDamage", 0);
    o->w.dps = wnum(own, book, "DamagePerSec", 0);
    o->w.velocity = wnum(own, book, "Velocity", 0);
    o->w.range = wnum(own, book, "MaxFiringRange", 10);
    o->w.h_knock = wnum(own, book, "HorizontalKnockback", 0);
    o->w.v_knock = wnum(own, book, "VerticalKnockback", 0);
    o->w.timer = o->w.charge + o->w.offset;
    o->w.on = o->w.name != NULL;
    o->w.rot_rate = gnum(wc, "RotationRate", 1);
    o->w.elev_rate = gnum(wc, "ElevationRate", 1);
    o->w.elev_min = gnum(wc, "ElevationMinRange", -90) * PI / 180;
    o->w.elev_max = gnum(wc, "ElevationMaxRange", 90) * PI / 180;
}

/* [G] gWorld.getRandomBuilding: any standing building; Building.getPosition: the middle of its footprint, on the
 * ground */
/* gWorld.getRandomBuilding (0x2AEC40) [R]: from (int)(n x frand) round the list, the first not gone */
static int ft_pick_building(Vehicles *v)
{
    if (v->nb <= 0)
        return -1;
    int start = (int)(frand(v) * (float)v->nb);
    for (int i = 0; i < v->nb; i++) {
        int k = (start + i) % v->nb;
        if (!v->b[k].gone)
            return k;
    }
    return -1;
}
/* Building.getPosition (0x277C60): +0x2C, set at its making (0x2BCBC4) to (LeftEdgeXPos + 2.5 Width, 2.5 Depth (+
 * BackRowOffset on the back row), 0) [R]; the box's middle is at 5 (+ BackRowOffset), the instance's place (0x305EDC) */
static void building_pos(const ArmyBox *b, float out[3])
{
    out[0] = (b->min[0] + b->max[0]) / 2;
    out[1] = (b->min[1] + b->max[1]) / 2 - 5 + 2.5f * (float)b->depth;
    out[2] = 0;
}

static void ft_weapons_off(Vehicles *v, VehObj *o)   /* setWeaponFlags(-1, 16) [G: 16 holds every weapon] */
{
    o->w.on = 0;
    if (o->w.firing) {
        o->w.firing = 0;
        o->beam.on = 0;
        effect_stop(v, o->beam_fx);
        o->beam_fx = -1;
    }
}

/* onPulse, every frame [R]: on its way, within TargetDistance of its building along x it stops (setVelocity 0) and
 * aims at a point 10 in front of the building (+y) at its roof's height (storeys x 3 + 1.5 + 6; above 15 a random
 * height between 15 and that); stopped, FireDuration runs down by the frame, and at 0 the target is let go, it drives
 * on at MaxVelocity and its weapons are held */
static void ft_pulse(Vehicles *v, VehObj *o)
{
    if (o->ft_building == -2)
        o->ft_building = ft_pick_building(v);
    int valid = o->ft_building >= 0 && o->ft_building < v->nb && !v->b[o->ft_building].gone;
    if (!o->ft_stopped) {
        if (valid) {
            float bp[3];
            building_pos(&v->b[o->ft_building], bp);
            if (fabsf(o->pos[0] - bp[0]) < o->ft_target_dist) {
                o->ft_stopped = 1;
                o->line.speed = 0;
                if (!o->ft_has_target) {             /* selectTarget: a DummyObject there */
                    const ArmyBox *b = &v->b[o->ft_building];
                    float th = (float)b->storeys * 3 + 1.5f + 6;
                    if (th > 15)
                        th = 15 + frand(v) * (th - 15);
                    o->ft_target[0] = bp[0];
                    o->ft_target[1] = bp[1] + 10;
                    o->ft_target[2] = th;
                }
                o->ft_has_target = 1;
            }
        }
    } else if (o->ft_fire_left > 0) {
        o->ft_fire_left -= TIMESTEP;
        if (o->ft_fire_left <= 0) {
            o->ft_has_target = 0;
            o->line.speed = o->ft_velocity;
            ft_weapons_off(v, o);
        }
    }
}

/* [G] what the beam meets first: a monster's collision sphere (its box without spheres); t along dir, -1 none */
static float ray_monster(const Vehicles *v, const float p[3], const float d[3], float maxlen, int *mon)
{
    float best = -1;
    *mon = -1;
    for (int i = 0; i < v->nm; i++) {
        const ArmyMonster *m = &v->m[i];
        if (m->nspheres > 0) {
            for (int s = 0; s < m->nspheres; s++) {
                const float *c = m->spheres[s];
                float w[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] };
                float b = w[0] * d[0] + w[1] * d[1] + w[2] * d[2], cc = w[0] * w[0] + w[1] * w[1] + w[2] * w[2] - c[3] * c[3];
                float disc = b * b - cc;
                if (disc < 0)
                    continue;
                float t = -b - sqrtf(disc);
                if (t < 0)
                    t = cc <= 0 ? 0 : -1;
                if (t >= 0 && t <= maxlen && (best < 0 || t < best)) {
                    best = t;
                    *mon = i;
                }
            }
        } else {                                     /* slab test against its box */
            float t0 = 0, t1 = maxlen;
            int hit = 1;
            for (int k = 0; k < 3 && hit; k++) {
                if (fabsf(d[k]) < 1e-6f) {
                    if (p[k] < m->box_min[k] || p[k] > m->box_max[k])
                        hit = 0;
                    continue;
                }
                float a = (m->box_min[k] - p[k]) / d[k], b = (m->box_max[k] - p[k]) / d[k];
                if (a > b) { float x = a; a = b; b = x; }
                t0 = fmaxf(t0, a);
                t1 = fminf(t1, b);
                if (t0 > t1)
                    hit = 0;
            }
            if (hit && (best < 0 || t0 < best)) {
                best = t0;
                *mon = i;
            }
        }
    }
    return best;
}

/* the weapon each frame (army.c's countdown and beam, 0x2971A0 / 0x295C70 / 0x295750) [R]: the countdown falls while
 * the weapon has ammo (or does not count it), held at ChargeTime + FiringOffset while it may not fire; at 0 with ammo
 * the beam runs for Duration (ammo - 1; FireEngineStream has 1: one stream a truck); it stops at the first thing it
 * meets: a monster takes ImpactDamage on the first touch, DamagePerSec x dt after, with its ReactionLevel (1).
 * [G] may fire: a target, the turret on it (within 0.02 rad), within MaxFiringRange; the turret's yaw and pitch step
 * at the cluster's rates; the stream reaches out at Velocity a second and stops at its target (the building) */
static void ft_weapon(Vehicles *v, VehObj *o, float dt)
{
    float from[3], nw[16];
    if (v->host.node_world && o->w.node && v->host.node_world(v->host.ctx, o, o->w.node, nw)) {
        from[0] = nw[12]; from[1] = nw[13]; from[2] = nw[14];
    } else {
        from[0] = o->pos[0]; from[1] = o->pos[1]; from[2] = o->pos[2] + 3;
    }
    int on_target = 0;
    float tdist = 0;
    if (o->ft_has_target) {
        float d[3] = { o->ft_target[0] - from[0], o->ft_target[1] - from[1], o->ft_target[2] - from[2] };
        float h = sqrtf(d[0] * d[0] + d[1] * d[1]);
        tdist = len3(d);
        float want_yaw = wrap_pi(facing_of(d[0], d[1]) - o->facing), want_pitch = atan2f(d[2], h);
        if (want_pitch < o->w.elev_min) want_pitch = o->w.elev_min;
        if (want_pitch > o->w.elev_max) want_pitch = o->w.elev_max;
        float dy = wrap_pi(want_yaw - o->ft_yaw), dp = want_pitch - o->ft_pitch;
        float sy = o->w.rot_rate * dt, sp = o->w.elev_rate * dt;
        o->ft_yaw = wrap_pi(o->ft_yaw + (fabsf(dy) <= sy ? dy : (dy > 0 ? sy : -sy)));
        o->ft_pitch += fabsf(dp) <= sp ? dp : (dp > 0 ? sp : -sp);
        /* [R 0x29ADE0] on target: the turn within pi / 50, the pitch within pi / 100 */
        on_target = fabsf(wrap_pi(want_yaw - o->ft_yaw)) <= 0.0628f && fabsf(want_pitch - o->ft_pitch) < 0.0314f;
    }
    float yaw = o->facing + o->ft_yaw, cp = cosf(o->ft_pitch);
    float dir[3] = { -sinf(yaw) * cp, cosf(yaw) * cp, sinf(o->ft_pitch) };
    if (o->w.firing) {
        o->w.left -= dt;
        if (o->w.left <= 0) {
            o->w.firing = 0;
            o->w.timer = fmaxf(o->w.refire, o->w.charge);
            o->beam.on = 0;
            effect_stop(v, o->beam_fx);
            o->beam_fx = -1;
        } else {
            o->w.age += dt;
            float reach = fminf(o->w.velocity > 0 ? o->w.velocity * o->w.age : o->w.range, o->w.range);
            (void)tdist;                             /* [R 0x295750] the beam is not cut at its target */
            int mon;
            float t = reach > 0.001f ? ray_monster(v, from, dir, reach, &mon) : -1;
            /* [R 0x295750] the beam's ray is the world's (include -1, exclude 8 with the truck flagged 8 while it is
             * cast): the street, the buildings and every object stop it, the nearest wins */
            if (v->cw && reach > 0.001f) {
                int oo = collide_check(v->cw, o->cobj, COLLIDE_TAG(COLLIDE_OWN_VEHICLE, idx(v, o)), 0);
                uint32_t keep = oo >= 0 ? v->cw->o[oo].flags : 0;
                if (oo >= 0)
                    v->cw->o[oo].flags |= COLLIDE_F_RAYSKIP;
                CollideHit h;
                int got = collide_ray(v->cw, from, dir, reach, 0xFFFFFFFFu, COLLIDE_F_RAYSKIP, 0, &h);
                if (oo >= 0)
                    v->cw->o[oo].flags = keep;
                if (got && (t < 0 || h.t < t - 1e-4f)) {
                    t = h.t;
                    mon = -1;
                    if (h.obj >= 0 && COLLIDE_TAG_OWNER(v->cw->o[h.obj].tag) == COLLIDE_OWN_MONSTER) {
                        int m = COLLIDE_TAG_INDEX(v->cw->o[h.obj].tag);
                        mon = m < v->nm ? m : -1;
                    }
                }
            }
            o->beam.on = 1;
            memcpy(o->beam.from, from, sizeof from);
            memcpy(o->beam.dir, dir, sizeof dir);
            o->beam.length = t >= 0 ? t : reach;
            if (t >= 0 && mon >= 0) {
                float dmg = o->w.first ? o->w.damage : o->w.dps > 0 ? o->w.dps * dt : 0;
                o->w.first = 0;
                if (dmg > 0) {
                    float at[3] = { from[0] + dir[0] * t, from[1] + dir[1] * t, from[2] + dir[2] * t };
                    VehicleEvent *e = emit(v, VE_HIT, idx(v, o), o->w.name, at);
                    float k[2] = { dir[0], dir[1] }, kl = sqrtf(k[0] * k[0] + k[1] * k[1]);
                    if (kl > 0) { k[0] /= kl; k[1] /= kl; }
                    e->monster = mon;
                    e->amount = dmg;
                    e->level = o->w.reaction;
                    e->v[0] = k[0] * o->w.h_knock;
                    e->v[1] = k[1] * o->w.h_knock;
                    e->v[2] = o->w.v_knock;
                    memcpy(e->dir, dir, sizeof e->dir);
                    if (o->w.impact_fx)
                        effect_at(v, o->w.impact_fx, at);
                }
            }
        }
    }
    int may = o->w.on && o->ft_has_target && on_target && tdist <= o->w.range;
    if (o->w.ammo > 0 || !o->w.counts) {
        o->w.timer -= dt;
        float floor = may ? 0 : o->w.charge + o->w.offset;
        if (o->w.timer < floor)
            o->w.timer = floor;
    }
    if (!may || o->w.firing || o->w.timer > 0 || (o->w.counts && o->w.ammo <= 0))
        return;
    if (o->w.counts)
        o->w.ammo--;
    o->w.firing = 1;
    o->w.first = 1;
    o->w.left = o->w.duration;
    o->w.age = 0;
    if (o->w.fire_fx) {
        VehicleEvent *e = emit(v, VE_EFFECT, idx(v, o), o->w.fire_fx, from);
        memcpy(e->dir, dir, sizeof e->dir);
        o->beam_fx = e->id = v->next_fx++;
    }
}

/* ---------------------------------------------------------------- Rocket.bsf, RocketBase.bsf */

static VehObj *spawn_obj(Vehicles *v, const char *name, const float pos[3], float facing, int building);

/* Construct [R]: OnlyHitWithEffNodes and PushOutMonsters; pulses every frame; its Base and BasePipe made where it
 * stands (setPoint only: they keep facing 0) */
static void rocket_construct(Vehicles *v, VehObj *o)
{
    o->flags = 0;
    set_pulse(o, 0);
    o->oflags = VF_EFF_NODES | VF_PUSH_OUT;
    const char *names[2] = { tstr(o, "Base"), tstr(o, "BasePipe") };
    int me = idx(v, o);
    for (int i = 0; i < 2; i++) {
        if (!names[i])
            continue;
        VehObj *p = spawn_obj(v, names[i], o->pos, 0, o->building);
        if (p) {
            o->child[i] = idx(v, p);
            p->parent = me;
        }
    }
    memcpy(o->cur, o->pos, sizeof o->cur);
}

/* fallOff (RocketBase.bsf) [R]: for 2 s, every frame: down 6 a second, sideways 10 / n x FallDir and -20 / n along y,
 * turned by (-pi/2, FallDir pi/2, +-pi/3) / n, where n = the frames a fall to the street at 6 would take (its height /
 * 6 / the frame); then deleted. The 2 s do not depend on the height: a low roof's part ends below the street */
static void part_fall_off(Vehicles *v, VehObj *p)
{
    float dir = tnum(p, "FallDir", 1);
    p->flags = 1;
    p->elapsed = 2;
    set_pulse(p, TIMESTEP);
    float n = p->pos[2] / 6 / TIMESTEP;
    if (n == 0)
        n = 0.01f;                                   /* [R] after its dbgAssert "falling 0 dist" */
    p->rpf[0] = -PI / 2 / n;
    p->rpf[1] = dir * (PI / 2 / n);
    p->rpf[2] = PI / 3 / n;
    if (frand(v) > 0.5f)
        p->rpf[2] = -p->rpf[2];
    p->xpf = 10 / n * dir;
    p->ypf = -20 / n;
}

static int part_pulse(Vehicles *v, VehObj *p)
{
    if (p->flags != 1)
        return 0;
    p->elapsed -= TIMESTEP;
    p->pos[2] -= 6 * TIMESTEP;
    p->pos[0] += p->xpf;
    p->pos[1] += p->ypf;
    rotate_local(p->rot, p->rpf);
    if (p->elapsed <= 0) {
        obj_delete(v, p);
        return 1;
    }
    return 0;
}

/* flyAway [R]: up 50 on a LineMover at 25 (deleted at its end), its TrailingSmokeEffect on it, the Base and Pipe fall
 * off; each player's monster within 10 of where it starts along x and along z is thrown (-10 or 0 along x: away to
 * -x only when the monster is on the -x side; -5 along y; 30 up), knocked down (reaction 2) and loses Damage (20) HP */
static void rocket_fly(Vehicles *v, VehObj *o)
{
    o->flags = 1;
    float a[3] = { o->pos[0], o->pos[1], o->pos[2] }, b[3] = { o->pos[0], o->pos[1], o->pos[2] + 50 };
    line_init(o, a, b, 25, 0);
    effect_on(v, o, tstr(o, "TrailingSmokeEffect"));
    for (int i = 0; i < 2; i++)
        if (o->child[i] >= 0 && v->o[o->child[i]].phase != VP_GONE && v->o[o->child[i]].parent == idx(v, o))
            part_fall_off(v, &v->o[o->child[i]]);
    for (int i = 0; i < v->nm; i++) {
        const float *mp = v->m[i].pos;
        if (fabsf(mp[0] - a[0]) <= 10 && fabsf(mp[2] - a[2]) <= 10) {
            VehicleEvent *e = emit(v, VE_SET_VEL, idx(v, o), NULL, mp);
            e->monster = i;
            e->v[0] = mp[0] < a[0] ? -10.0f : 0.0f;
            e->v[1] = -5;
            e->v[2] = 30;
            e = emit(v, VE_REACTION, idx(v, o), NULL, mp);
            e->monster = i;
            e->level = 2;
            e = emit(v, VE_HP, idx(v, o), NULL, mp);
            e->monster = i;
            e->amount = -tnum(o, "Damage", 20);
        }
    }
}

/* TakeDamage [R]: a monster's blow on a waiting rocket starts it (its ChargeEffect where it stands, a 2 s shake
 * every frame) and counts (addStats type 0: Points 100) */
static int rocket_damage(Vehicles *v, VehObj *o, int monster)
{
    if (monster < 0 || o->flags != 0)
        return 0;
    o->flags = 2;
    effect_at(v, tstr(o, "ChargeEffect"), o->pos);
    set_pulse(o, TIMESTEP);
    o->shake_time = 2;
    add_stats(v, o, monster, 0);
    return 1;
}

/* onPulse [R]: shaking, each frame shakeObject: the step grows by 50 dt, the kept point moves by
 * 0.7 x (sin step, 0, sin step) and the rocket is put at that point - (0.5, 0.5, 0.5) (so it also sits half a unit
 * off while it shakes); after 2 s it flies */
static void rocket_pulse(Vehicles *v, VehObj *o)
{
    if (o->flags != 2)
        return;
    o->shake_time -= TIMESTEP;
    if (o->shake_time > 0) {
        o->shake_step += TIMESTEP * 50;
        float s = sinf(o->shake_step) * 0.7f;
        o->cur[0] += s;
        o->cur[2] += s;
        o->pos[0] = o->cur[0] - 0.5f;
        o->pos[1] = o->cur[1] - 0.5f;
        o->pos[2] = o->cur[2] - 0.5f;
    } else {
        rocket_fly(v, o);
        set_pulse(o, 0);
    }
}

/* ---------------------------------------------------------------- making things */

static VehObj *spawn_obj(Vehicles *v, const char *name, const float pos[3], float facing, int building)
{
    Group t = find_template(v, name);
    int kind = kind_of_template(t);
    if (!t.at || kind < 0)
        return NULL;
    VehObj *o = obj_new(v, name, t, kind);
    if (!o)
        return NULL;
    memcpy(o->pos, pos, sizeof o->pos);
    set_facing(o, facing);
    o->building = building;
    switch (kind) {
    case VK_CAR: car_construct(v, o); break;
    case VK_FIRETRUCK: ft_init(v, o); break;
    case VK_ROCKET: rocket_construct(v, o); break;
    default: set_pulse(o, 1); break;
    }
    update_world(o);
    return o;
}

int vehicle_spawn(Vehicles *v, const char *tmpl_name, const float pos[3], float facing, int building)
{
    VehObj *o = spawn_obj(v, tmpl_name, pos, facing, building);
    return o ? idx(v, o) : -1;
}

int vehicle_kind_of(Vehicles *v, const char *tmpl_name)
{
    return kind_of_template(find_template(v, tmpl_name));
}

/* Traffic.bsf onInit [R]: a HiddenVehicle from (-100 + 100 rand, 0, -0.5) to (100, -5, -0.5) at 5; it turns back at
 * each end (at 8), and a monster within its MonsterResponseDist (22) hears the Cop_Loudspeaker, then not for 20 s */
int vehicle_level_start(Vehicles *v)
{
    float a[3] = { -100 + frand(v) * 100, 0, -0.5f }, b[3] = { 100, -5, -0.5f };
    VehObj *o = spawn_obj(v, "HiddenVehicle", a, 0, -1);
    if (!o)
        return -1;
    line_init(o, a, b, 5, 0);
    update_world(o);
    return idx(v, o);
}

void vehicle_building_gone(Vehicles *v, int building)   /* Rocket.bsf onBuildingDestruction [R]: a waiting one flies */
{
    for (int i = 0; i < v->n; i++) {
        VehObj *o = &v->o[i];
        if (o->phase == VP_ALIVE && o->kind == VK_ROCKET && o->building == building && o->flags == 0)
            rocket_fly(v, o);
    }
}

/* ---------------------------------------------------------------- the frame */

static void obj_step(Vehicles *v, VehObj *o)
{
    if (o->phase == VP_GONE) {
        if (o->gone_age < 1000)
            o->gone_age++;
        return;
    }
    if (o->phase == VP_ALIVE) {
        int cb = line_step(v, o, TIMESTEP);
        if (cb) {
            if (o->kind == VK_CAR && car_mover(v, o, cb))
                return;
            if (o->kind == VK_FIRETRUCK) {           /* [R] any callback deletes it (only its end comes) */
                obj_delete(v, o);
                return;
            }
            if (o->kind == VK_ROCKET && cb == 2) {
                obj_delete(v, o);
                return;
            }
        }
        sphere_step(v, o, TIMESTEP);                 /* OnHitGround, TakeImpact: its contacts */
        if (o->phase == VP_GONE)
            return;
        if (o->kind == VK_FIRETRUCK)
            ft_weapon(v, o, TIMESTEP);
    }
    /* [R 0x309BA0] the pulse's clock runs only with a delay of 0.01 or more; at the delay it is taken off, once a
     * frame (a delay below 0.01 stops the pulses) */
    if (o->pulse_delay >= 0.01f)
        o->pulse_timer += TIMESTEP;
    if (o->pulse_delay >= 0.01f && o->pulse_timer >= o->pulse_delay) {
        o->pulse_timer -= o->pulse_delay;
        switch (o->kind) {
        case VK_CAR: if (car_pulse(v, o)) return; break;
        case VK_FIRETRUCK: if (o->phase == VP_ALIVE) ft_pulse(v, o); break;
        case VK_ROCKET: rocket_pulse(v, o); break;
        case VK_ROCKET_PART: if (part_pulse(v, o)) return; break;
        default: break;
        }
    }
    if (o->phase != VP_HELD && !o->sphere.tumble && !(o->sphere.on && o->cm_on) && o->kind != VK_ROCKET_PART)
        rot_z(o->rot, o->facing);
    update_world(o);
}

void vehicle_update(Vehicles *v, float dt, const ArmyMonster *m, int nm, const ArmyBox *b, int nb)
{
    v->m = m;
    v->nm = m ? nm : 0;
    v->b = b;
    v->nb = b ? nb : 0;
    v->acc += dt;
    if (v->acc > 0.25f)
        v->acc = 0.25f;
    while (v->acc >= TIMESTEP) {                     /* the scripts count in frames: run them at the game's step */
        v->acc -= TIMESTEP;
        traffic_update(v, TIMESTEP);
        for (int i = 0; i < v->n; i++)
            obj_step(v, &v->o[i]);
    }
}

/* ---------------------------------------------------------------- the monster's side */

/* [G] an object's shape for a blow: its model's box (AutoCollisionBox) from the host, else its ShadowRect
 * (x0, y0, x1, y1) 2.5 high, else 2 x 2 x 2 */
static void obj_box(const Vehicles *v, const VehObj *o, float mn[3], float mx[3])
{
    if (o->shape && v->host.shape_box && v->host.shape_box(v->host.ctx, o->shape, mn, mx))
        return;
    Group sd = group_sub(o->tmpl, "ShadowData"), sr = group_sub(sd, "ShadowRect");
    Record r[4];
    if (sr.at && group_items(sr, r, 4) == 4) {
        mn[0] = record_num(r[0]); mn[1] = record_num(r[1]); mn[2] = 0;
        mx[0] = record_num(r[2]); mx[1] = record_num(r[3]); mx[2] = 2.5f;
        return;
    }
    mn[0] = mn[1] = -1; mn[2] = 0;
    mx[0] = mx[1] = 1; mx[2] = 2;
}

int vehicle_hit(Vehicles *v, int monster, const float centre[3], float radius, float amount)
{
    (void)amount;                                    /* Vehicle.bsf and Rocket.bsf do not look at it */
    int hits = 0;
    for (int i = 0; i < v->n; i++) {
        VehObj *o = &v->o[i];
        if (o->phase != VP_ALIVE || o->hidden || o->kind == VK_ROCKET_PART)
            continue;
        if (o->kind == VK_CAR && !(o->oflags & VF_EFF_NODES))
            continue;                                /* wrecked: no longer hit */
        float mn[3], mx[3], d[3] = { centre[0] - o->pos[0], centre[1] - o->pos[1], centre[2] - o->pos[2] }, l[3], q = 0;
        obj_box(v, o, mn, mx);
        for (int r = 0; r < 3; r++)                  /* into its frame: rot^T d */
            l[r] = o->rot[r * 3] * d[0] + o->rot[r * 3 + 1] * d[1] + o->rot[r * 3 + 2] * d[2];
        for (int k = 0; k < 3; k++) {
            float c = l[k] < mn[k] ? mn[k] : l[k] > mx[k] ? mx[k] : l[k];
            q += (l[k] - c) * (l[k] - c);
        }
        if (q > radius * radius)
            continue;
        int took = 0;
        if (o->kind == VK_CAR)
            took = car_damage(v, o, monster, d);
        else if (o->kind == VK_FIRETRUCK) {
            if (monster >= 0)
                add_stats(v, o, monster, 2);         /* [R] TakeDamage: it counts, and drives on */
            took = 1;
        } else if (o->kind == VK_ROCKET)
            took = rocket_damage(v, o, monster);
        hits += took;
    }
    return hits;
}

void vehicle_set_collide(Vehicles *v, CollideWorld *w) { v->cw = w; }

void vehicle_collide_add(Vehicles *v, CollideWorld *w)
{
    for (int i = 0; i < v->n; i++) {
        VehObj *o = &v->o[i];
        o->cobj = -1;
        if (!w || (o->phase != VP_ALIVE && o->phase != VP_HELD) || o->hidden)
            continue;                                /* (the hidden one has no body) */
        CollideShape sh;
        float m[12];
        obj_shape(v, o, &sh);
        collide_matrix_from16(m, o->world);
        uint32_t fl = COLLIDE_F_WORLD | (o->oflags & ~(uint32_t)VF_HELD);
        /* [R 0x2DA310] a held object stays in the world with its shape: PushOutMonsters cleared (the monster's box no longer
         * meets it), flying objects and shots still do (flag 4 stays: no removeObject; a shot's own owner is no hit) */
        if (o->phase == VP_HELD)
            fl &= ~(uint32_t)COLLIDE_F_PUSHOUT;
        o->cobj = collide_add(w, &sh, m, fl, COLLIDE_TAG(COLLIDE_OWN_VEHICLE, i), 0);
        if (o->sphere.on && o->cm_on)                /* flying: another flying object exchanges impulses with it (0x285E90) */
            collide_set_mover(w, o->cobj, &o->cm, o->cmat, o->sphere.vel);
    }
}

void vehicle_impact(Vehicles *v, int obj)            /* TakeImpact [R]: wrecked, it explodes on its next pulse */
{
    if (obj >= 0 && obj < v->n && v->o[obj].phase != VP_GONE && v->o[obj].kind == VK_CAR)
        v->o[obj].flags |= CF_WRECK;
}

int vehicle_nearest_pickup(const Vehicles *v, const float hand[3], const float root[3], const float forward[3],
                           float radius)
{
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < v->n; i++) {
        const VehObj *o = &v->o[i];
        if (o->phase != VP_ALIVE || !(o->oflags & VF_PICKUP))
            continue;
        float e[3] = { o->pos[0] - hand[0], o->pos[1] - hand[1], o->pos[2] - hand[2] };
        float d = len3(e);
        float f = (o->pos[0] - root[0]) * forward[0] + (o->pos[1] - root[1]) * forward[1] +
                  (o->pos[2] - root[2]) * forward[2];
        if (d <= radius && f > 0 && d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* [R] the hold (0x2DA310): BeingHeld, neither Pickupable nor PushOutMonsters, its mover replaced (a LineMover's end
 * never comes: it stays counted in the traffic until it is thrown, eaten or explodes), then onPickup */
void vehicle_grab(Vehicles *v, int obj, int monster)
{
    (void)monster;
    if (obj < 0 || obj >= v->n || v->o[obj].phase != VP_ALIVE)
        return;
    VehObj *o = &v->o[obj];
    o->oflags = (o->oflags | VF_HELD) & ~(uint32_t)(VF_PICKUP | VF_PUSH_OUT);
    o->phase = VP_HELD;
    o->line.on = o->sphere.on = o->sphere.tumble = 0;
    if (o->kind == VK_CAR)
        car_pickup(v, o);
}

/* the HeldObjectMover (init 0x310ED0): [R] HoldOffset (MirroredHoldOffset while the monster is mirrored, +0x7DA1)
 * taken along the hand's axes; [G] the object turned as the hand is (the MajorAxis frame it builds is not ported) */
void vehicle_hold(Vehicles *v, int obj, const float hand_world[16], int mirrored)
{
    if (obj < 0 || obj >= v->n || v->o[obj].phase != VP_HELD)
        return;
    VehObj *o = &v->o[obj];
    float off[3] = { 0, 0, 0 };
    Group g = group_sub(o->tmpl, mirrored ? "MirroredHoldOffset" : "HoldOffset");
    Record r[3];
    if (g.at && group_items(g, r, 3) == 3)
        for (int k = 0; k < 3; k++)
            off[k] = record_num(r[k]);
    /* [R 0x310ED0] the turn kept in the hand: the MajorAxis row is the hand's z, the next its y, the third their cross
     * (the cars and the fire truck have MajorAxis 1: x = -hand x, y = hand z, z = hand y) */
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

/* throwObject (0x2D9DC0): [R] the thrown object gets its mover with the throw's velocity, then onThrow(monster,
 * eat); [G] the mover is the SphereMover above, the object keeping the hand's turn as it flies */
void vehicle_throw(Vehicles *v, int obj, int monster, int eat, const float vel[3], float monster_heading)
{
    if (obj < 0 || obj >= v->n || v->o[obj].phase != VP_HELD)
        return;
    VehObj *o = &v->o[obj];
    o->oflags &= ~(uint32_t)VF_HELD;
    o->phase = VP_ALIVE;
    if (vel)
        memcpy(o->sphere.vel, vel, sizeof o->sphere.vel);
    o->sphere.on = 1;
    o->sphere.tumble = 1;                            /* keeps its turn: no spin */
    o->cm_on = 0;
    memset(o->sphere.spin, 0, sizeof o->sphere.spin);
    if (o->kind == VK_CAR)
        car_throw(v, o, monster, eat, monster_heading);
    if (o->phase != VP_GONE)
        update_world(o);
}

/* ---------------------------------------------------------------- the rest */

Vehicles *vehicle_new(const VehicleHost *host)
{
    Vehicles *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    if (host)
        v->host = *host;
    v->next_fx = 1;
    return v;
}

void vehicle_free(Vehicles *v) { free(v); }
void vehicle_bind_army(Vehicles *v, Army *army) { v->army = army; }

void vehicle_clear(Vehicles *v)
{
    for (int i = 0; i < v->n; i++)
        obj_delete(v, &v->o[i]);
    memset(v->slot, 0, sizeof v->slot);
    v->nslots = 0;
    v->filled = 0;
    v->block_vehicles = 0;
    v->acc = 0;
}

int vehicle_count(const Vehicles *v) { return v->n; }
const VehObj *vehicle_obj(const Vehicles *v, int i) { return i >= 0 && i < v->n ? &v->o[i] : NULL; }

int vehicle_events(Vehicles *v, VehicleEvent *out, int max)
{
    int n = v->nev < max ? v->nev : max;
    memcpy(out, v->ev, sizeof *out * (size_t)n);
    memmove(v->ev, v->ev + n, sizeof *v->ev * (size_t)(v->nev - n));
    v->nev -= n;
    return n;
}
