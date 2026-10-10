/* parade.c - the parade balloons, the bikers and their bikes, the food carts (parade.h; docs/PARADE_NOTES.md).
 * [R] marks what is read from the game's code or scripts, [G] what is a guess or a stand-in. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parade.h"
#include "wrand.h"

#define PI 3.14159265f
#define TIMESTEP (1.0f / 30.0f)                      /* gWorld.getTimeStep(): its step, 1/30 [R 0x2B7800] */
#define MAX_OBJS 256
#define MAX_EVENTS 256
#define MAX_PENDING 32
#define MAX_TIMERS 16
#define FACE_RIGHT (-PI / 2)                         /* [R] the scripts' FACE_RIGHT.z: facing +x */
#define FACE_LEFT (PI / 2)                           /* [R] FACE_LEFT.z: facing -x */

struct Parade {
    ParadeHost host;
    int active;                                      /* gAIMgr activated (bit 8, 0x2A6208): its delays run */
    ParadeObj o[MAX_OBJS];
    int n;
    int gone_age[MAX_OBJS];                          /* frames since a slot went (a slot is reused after 2 s) */
    ParadeEvent ev[MAX_EVENTS];
    int nev;
    struct { Group inst; float left; } pend[MAX_PENDING];   /* gAIMgr's delayed instances (0x334A20) */
    int npend;
    struct { float left; int monster; } fart[MAX_TIMERS];   /* installObjectTimer("Monsters::timedDoFart", 2.0) */
    int nfart;
    Group block;                                     /* the block's BlockDef| (its ActorInstance|s for SpawnsItem) */
    const ArmyMonster *m;                            /* this frame's monsters */
    int nm;
    float camera_x;
    CollideWorld *cw;                                /* the frame's collision world (parade_set_collide), NULL none */
};

/* ---------------------------------------------------------------- small things */

/* gWorld.frand() (0x279F98): the world's Park-Miller, gWorld +0x828 (wrand.c) [R] */
static float frand(Parade *p)
{
    (void)p;
    return wrand_frand();
}

static const char *tstr(const ParadeObj *o, const char *key) { return o->tmpl.at ? group_str(o->tmpl, key) : NULL; }
static float tnum(const ParadeObj *o, const char *key, float def) { return o->tmpl.at ? group_num(o->tmpl, key, def) : def; }
static int idx(const Parade *p, const ParadeObj *o) { return (int)(o - p->o); }
static int alive(const Parade *p, int i) { return i >= 0 && i < p->n && p->o[i].phase != PAP_GONE; }

static int same_prefix(const char *s, const char *prefix)   /* any case, as the game compares names */
{
    for (; *prefix; s++, prefix++) {
        int x = *s >= 'A' && *s <= 'Z' ? *s + 32 : *s, y = *prefix >= 'A' && *prefix <= 'Z' ? *prefix + 32 : *prefix;
        if (x != y)
            return 0;
    }
    return 1;
}

static Group template_of(Parade *p, const char *name)
{
    Group none = { NULL, 0 };
    return name && p->host.find ? p->host.find(p->host.ctx, "Objects1_Props", name) : none;
}

static ParadeEvent *emit(Parade *p, int type, int obj, const char *name, const float pos[3])
{
    static ParadeEvent spare;
    if (p->nev >= MAX_EVENTS) {
        memset(&spare, 0, sizeof spare);
        return &spare;
    }
    ParadeEvent *e = &p->ev[p->nev++];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->obj = obj;
    e->monster = -1;
    e->name = name;
    if (pos)
        memcpy(e->pos, pos, sizeof e->pos);
    return e;
}

static void sound(Parade *p, ParadeObj *o, const char *name, int level)   /* add3DSfx(name, level) */
{
    if (name)
        emit(p, PAE_SOUND, idx(p, o), name, o->pos)->level = level;
}

/* FollowEffect.createAndStartAttached(obj, name): at obj, following it */
static void effect_on(Parade *p, ParadeObj *o, const char *name)
{
    if (name)
        emit(p, PAE_EFFECT, idx(p, o), name, o->pos);
}

static const ArmyMonster *monster(const Parade *p, int i) { return i >= 0 && i < p->nm ? &p->m[i] : NULL; }

static int nearest_monster(const Parade *p, const float pos[3])   /* getNearestMonster; -1 none */
{
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < p->nm; i++) {
        float dx = p->m[i].pos[0] - pos[0], dy = p->m[i].pos[1] - pos[1], dz = p->m[i].pos[2] - pos[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* the world matrix (column-major) of a place turned by setFacing (x, y, z): R = Rz(z) Rx(x) Ry(y) [R 0x277E78 ->
 * 0x101330(m, z, x, y)] */
static void update_world(ParadeObj *o)
{
    if (o->sphere.on && o->cm_on) {                  /* flying: its mover's matrix */
        collide_matrix_to16(o->cmat, o->world);
        return;
    }
    float ax = o->rot[0], ay = o->rot[1], h = o->rot[2];
    float ch = cosf(h), sh = sinf(h), cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay);
    float r[9] = { ch * cy - sh * sx * sy, sh * cy + ch * sx * sy, -cx * sy,   /* Rz(h) Rx(ax) Ry(ay) */
                   -sh * cx, ch * cx, sx,
                   ch * sy + sh * sx * cy, sh * sy - ch * sx * cy, cx * cy };
    for (int c = 0; c < 3; c++) {
        for (int k = 0; k < 3; k++)
            o->world[4 * c + k] = r[3 * c + k];
        o->world[4 * c + 3] = 0;
    }
    for (int k = 0; k < 3; k++)
        o->world[12 + k] = o->pos[k];
    o->world[15] = 1;
}

/* rotation matrix (columns of a column-major 4x4) <-> quaternion (x, y, z, w), for the hold's blend */
static void mat_quat(const float m[16], float q[4])
{
    float t = m[0] + m[5] + m[10];
    if (t > 0) {
        float s = sqrtf(t + 1) * 2;
        q[3] = s / 4; q[0] = (m[6] - m[9]) / s; q[1] = (m[8] - m[2]) / s; q[2] = (m[1] - m[4]) / s;
    } else if (m[0] > m[5] && m[0] > m[10]) {
        float s = sqrtf(1 + m[0] - m[5] - m[10]) * 2;
        q[3] = (m[6] - m[9]) / s; q[0] = s / 4; q[1] = (m[4] + m[1]) / s; q[2] = (m[8] + m[2]) / s;
    } else if (m[5] > m[10]) {
        float s = sqrtf(1 + m[5] - m[0] - m[10]) * 2;
        q[3] = (m[8] - m[2]) / s; q[0] = (m[4] + m[1]) / s; q[1] = s / 4; q[2] = (m[9] + m[6]) / s;
    } else {
        float s = sqrtf(1 + m[10] - m[0] - m[5]) * 2;
        q[3] = (m[1] - m[4]) / s; q[0] = (m[8] + m[2]) / s; q[1] = (m[9] + m[6]) / s; q[2] = s / 4;
    }
}

static void quat_mat(const float q[4], float m[16])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);
    m[4] = 2 * (x * y - z * w);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + x * w);
    m[8] = 2 * (x * z + y * w);     m[9] = 2 * (y * z - x * w);     m[10] = 1 - 2 * (x * x + y * y);
    m[3] = m[7] = m[11] = 0;
    m[15] = 1;
}

static void slerp(const float a[4], const float b0[4], float f, float out[4])
{
    float b[4] = { b0[0], b0[1], b0[2], b0[3] };
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) {
        d = -d;
        for (int k = 0; k < 4; k++)
            b[k] = -b[k];
    }
    float wa = 1 - f, wb = f;
    if (d < 0.9995f) {
        float th = acosf(d), s = sinf(th);
        wa = sinf((1 - f) * th) / s;
        wb = sinf(f * th) / s;
    }
    float len = 0;
    for (int k = 0; k < 4; k++) {
        out[k] = wa * a[k] + wb * b[k];
        len += out[k] * out[k];
    }
    len = len > 0 ? 1 / sqrtf(len) : 1;
    for (int k = 0; k < 4; k++)
        out[k] *= len;
}

/* setPulseDelay (ScriptObject 0x2AA2A0, Actor 0x2A94C8) [R]: only the delay; the count runs on. Below 0.01 there are
 * no pulses (0x309BA0 skips the count) */
static void set_pulse(ParadeObj *o, float delay)
{
    o->pulse_delay = delay;
}

/* ---------------------------------------------------------------- the actor's anim (0x30AAD0) */

/* a look's one play when the host cannot say: the bikers' clips as read from SanFrancisco.bdp [R] */
static float known_length(const char *clip, const char *shape)
{
    static const struct { const char *name; float len; } k[] = {
        { "Biker_Riding_Throw", 0.7f }, { "Biker_Held", 1.0f }, { "Biker_Riding", 0.1f }, { "Biker_Bike", 0.1f },
    };
    const char *n = clip ? clip : shape;
    for (size_t i = 0; n && i < sizeof k / sizeof k[0]; i++)
        if (same_prefix(n, k[i].name) && strlen(n) == strlen(k[i].name))
            return k[i].len;
    return 1.0f;                                     /* [G] */
}

/* [R] startAnim / startAnimAtRandomOffset: the look ActorAnimShape|<name> (its Shape|ShapeName, else the template's,
 * and its Anim|AnimName, else the shape's own RawAnim); one play lasts its Duration, else the clip's; AnimRepeat
 * loops it, otherwise its end is onAnimComplete */
static void start_anim(Parade *p, ParadeObj *o, const char *name, int random_offset)
{
    snprintf(o->anim, sizeof o->anim, "%s", name);
    char key[64];
    snprintf(key, sizeof key, "ActorAnimShape|%s", name);
    Group look = o->tmpl.at ? group_sub(o->tmpl, key) : o->tmpl;
    const char *shape = look.at ? group_str(look, "Shape|ShapeName") : NULL;
    if (!shape)
        shape = tstr(o, "Shape|ShapeName");
    if (!shape)
        shape = tstr(o, "Shape|Shape");
    o->shape = shape;
    o->clip = look.at ? group_str(look, "Anim|AnimName") : NULL;
    o->anim_loop = look.at && group_num(look, "AnimRepeat", 0) != 0;
    float len = look.at ? fabsf(group_num(look, "Duration", 0)) : 0;
    if (len <= 0 && p->host.clip_seconds)
        len = p->host.clip_seconds(p->host.ctx, o->clip, shape);
    if (len <= 0)
        len = known_length(o->clip, shape);
    o->anim_len = len;
    o->anim_time = random_offset ? frand(p) * len : 0;
    o->anim_done = 0;
}

/* ---------------------------------------------------------------- the movers */

/* LineMover.init(obj, a, b, speed, 0) [R]: from a, the share of the way growing by speed x dt / length */
static void line_init(ParadeObj *o, const float a[3], const float b[3], float speed)
{
    memcpy(o->line.a, a, sizeof o->line.a);
    memcpy(o->line.b, b, sizeof o->line.b);
    o->line.speed = speed;
    o->line.t = 0;
    o->line.on = 1;
    o->sphere.on = 0;
    memcpy(o->pos, a, sizeof o->pos);
}

/* the LineMover's frame (0x315E10) [R]: past the end it stays there and calls back 2 ([G] once). None of these
 * templates has a MonsterResponseDist, so callback 4 never comes. 0 or 2 */
static int line_step(ParadeObj *o, float dt)
{
    if (!o->line.on)
        return 0;
    float d[3] = { o->line.b[0] - o->line.a[0], o->line.b[1] - o->line.a[1], o->line.b[2] - o->line.a[2] };
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    int r = 0;
    if (len != 0)
        o->line.t += dt * o->line.speed / len;
    if (o->line.t >= 1 || len == 0) {
        o->line.t = 1;
        o->line.on = 0;
        r = 2;
    }
    for (int k = 0; k < 3; k++)
        o->pos[k] = o->line.a[k] + d[k] * o->line.t;
    return r;
}

/* [R] smashObject (0x2B2F50, as people.c / vehicle.c): off on a SpherePhysicsMover, 15 along a yaw in the 120 degree
 * cone towards the camera (narrowed on the side the monster faces) and 15 x (0.8 .. 1.3) up, spinning
 * +-(2 pi + pi/6 r) about x and +-(pi/16 + pi/16 r) about y and z */
static void smash_object(Parade *p, ParadeObj *o, float monster_heading)
{
    const float c = 3 * PI / 2, lo = 20 * PI / 180, wd = 120 * PI / 180;
    float A = sinf(c), B = cosf(c), A2 = sinf(monster_heading + PI / 2), B2 = cosf(monster_heading + PI / 2);
    float d = -(B2 * B + A2 * A), cross = B2 * A - A2 * B;
    float width = lo + (wd - lo) * (1 - d) / 2, a0 = c - wd / 2, a1 = c + wd / 2;
    if (cross < 0)
        a0 = a1 - width;
    else
        a1 = a0 + width;
    float yaw = a0 + (a1 - a0) * frand(p);
    o->sphere.vel[0] = 15 * cosf(yaw);
    o->sphere.vel[1] = 15 * sinf(yaw);
    o->sphere.vel[2] = 15 * (0.8f + 0.5f * frand(p));
    o->sphere.spin[0] = (frand(p) < 0.5f ? 1 : -1) * (2 * PI + PI / 6 * frand(p));
    o->sphere.spin[1] = (frand(p) < 0.5f ? 1 : -1) * (PI / 16 + PI / 16 * frand(p));
    o->sphere.spin[2] = (frand(p) < 0.5f ? 1 : -1) * (PI / 16 + PI / 16 * frand(p));
    o->sphere.on = 1;
    o->sphere.landed = 0;
    o->cm_on = 0;
    o->line.on = 0;
    memcpy(o->pos, &o->world[12], sizeof o->pos);   /* from where it is (in the hand, on its ride) */
    o->oflags &= ~(uint32_t)PAF_HELD;
    o->phase = PAP_FLYING;
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

/* an object's shape (0x28FD70, collide_shape_pick): the template's CollisionBox, CollisionRadius (the bikers'
 * DefActorProps 2, the carts' own) or AutoCollisionBox (the balloons) with the model's box; else the model's bounding
 * sphere [G: taken as its box's, the host's] */
static void obj_shape_read(const Parade *p, const ParadeObj *o, CollideShape *s)
{
    float tb[6], mb[6], c[3] = { 0, 0, 0 }, r = 1;
    int have = o->shape && p->host.shape_box && p->host.shape_box(p->host.ctx, o->shape, mb, mb + 3);
    if (have) {
        float h[3];
        for (int k = 0; k < 3; k++) {
            c[k] = (mb[k] + mb[3 + k]) / 2;
            h[k] = (mb[3 + k] - mb[k]) / 2;
        }
        r = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    }
    /* [R 0x28FD70 -> 0x184CB0] the sphere the model keeps, the union of its parts' spheres (0x185AA0) */
    if (have && p->host.shape_sphere)
        p->host.shape_sphere(p->host.ctx, o->shape, c, &r);
    collide_shape_pick(s, have ? c : NULL, r, o->tmpl.at ? book_box(o->tmpl, tb) : NULL, tnum(o, "CollisionRadius", -1),
                       tnum(o, "AutoCollisionBox", 0) != 0, NULL, -1, 0, have ? mb : NULL);
}

static void obj_shape(const Parade *p, ParadeObj *o, CollideShape *s)   /* read once, at its first use */
{
    if (!o->cshape_ok) {
        obj_shape_read(p, o, &o->cshape);
        o->cshape_ok = 1;
    }
    *s = o->cshape;
}

typedef struct { Parade *p; ParadeObj *o; int ground; } FlightCtx;

/* a contact of the flight: the street is OnHitGround (event 11, every sub-step it touches); an object is TakeImpact
 * on both (0x287430): none of these scripts has one, so only another module's object hears of it (PAE_IMPACT) */
static void flight_contact(void *user, int other, const float pt[3], const float n[3])
{
    FlightCtx *c = user;
    Parade *p = c->p;
    if (other < 0) {
        if (getenv("RAMPAGE_TRACE") && c->o->cm.vel[2] > 0.5f)   /* a bounce (the impulse is in) */
            printf("  %s bounced off the street at %.1f %.1f %.1f\n", c->o->name ? c->o->name : "?", pt[0], pt[1], pt[2]);
        c->ground++;
        return;
    }
    if (!p->cw)
        return;
    const CollideObj *w = &p->cw->o[other];
    if (getenv("RAMPAGE_TRACE"))
        printf("  %s met %d.%d at %.1f %.1f %.1f\n", c->o->name ? c->o->name : "?", COLLIDE_TAG_OWNER(w->tag),
               COLLIDE_TAG_INDEX(w->tag), pt[0], pt[1], pt[2]);
    if (COLLIDE_TAG_OWNER(w->tag) == COLLIDE_OWN_PARADE)
        return;
    ParadeEvent *e = emit(p, PAE_IMPACT, idx(p, c->o), c->o->name, pt);
    e->id = w->tag;
    e->level = w->kind;
    memcpy(e->v, n, sizeof e->v);
    e->amount = c->o->cm.mass;
}

/* the SpherePhysicsMover's frame (collide.c, 0x28E440): [R] Gravity (9.81 unless the template says: the objects'
 * DefObjectProps 35), Mass, Restitution (0.8), SlidingFriction (0.4) and InertiaSolidity (0x28F220); its spin
 * smashObject's turn rates about the object's x, y, z; the street and the frame's objects. No rest: it rolls on until
 * its script deletes it. Returns its street contacts this frame (OnHitGround, event 11) */
static int sphere_step(Parade *p, ParadeObj *o, float dt)
{
    if (!o->sphere.on)
        return 0;
    if (!o->cm_on) {
        CollideShape sh;
        obj_shape(p, o, &sh);
        float r = collide_mover_radius(&sh);
        if (r < 0.05f)
            r = sh.radius > 0.05f ? sh.radius : 1;
        collide_mover_init(&o->cm, r, tnum(o, "Gravity", 9.81f), tnum(o, "Mass", 1), tnum(o, "Restitution", 0.8f),
                           tnum(o, "SlidingFriction", 0.4f), tnum(o, "InertiaSolidity", 1));
        update_world(o);                             /* from where it is, turned as it is */
        collide_mover_spin(&o->cm, o->sphere.spin);  /* [R 0x28DD80] the rates as given: about a world axis */
        collide_matrix_from16(o->cmat, o->world);
        o->cm_on = 1;
    }
    memcpy(o->cm.vel, o->sphere.vel, sizeof o->cm.vel);
    int self = collide_check(p->cw, o->cobj, COLLIDE_TAG(COLLIDE_OWN_PARADE, idx(p, o)), 0);
    FlightCtx c = { p, o, 0 };
    collide_mover_step_ex(p->cw, &o->cm, o->cmat, self, dt, COLLIDE_F_MONSTER, flight_contact, &c);
    memcpy(o->sphere.vel, o->cm.vel, sizeof o->sphere.vel);
    memcpy(o->pos, o->cmat + 9, sizeof o->pos);
    if (c.ground)
        o->sphere.landed = 1;
    return c.ground;
}

/* ---------------------------------------------------------------- objects */

static int new_obj(Parade *p, Group tmpl, int kind, const float pos[3], float facing)
{
    int i = -1;
    for (int k = 0; k < p->n && i < 0; k++)          /* a slot gone 2 s ago (the caller's handles to it are stale) */
        if (p->o[k].phase == PAP_GONE && p->gone_age[k] >= 120)
            i = k;
    if (i < 0) {
        if (p->n >= MAX_OBJS)
            return -1;
        i = p->n++;
    }
    ParadeObj *o = &p->o[i];
    memset(o, 0, sizeof *o);
    p->gone_age[i] = 0;
    o->tmpl = tmpl;
    o->name = tmpl.at ? group_name(tmpl) : "";
    o->kind = kind;
    o->phase = PAP_ALIVE;
    o->partner = -1;
    o->cobj = -1;
    o->last_blow = -1;
    o->pulse_delay = -1;                             /* none until a script sets one */
    o->scale = tnum(o, "RenderScale", 1);
    o->shape = tstr(o, "Shape|Shape");
    if (!o->shape)
        o->shape = tstr(o, "Shape|ShapeName");
    memcpy(o->pos, pos, sizeof o->pos);
    o->rot[2] = facing;
    update_world(o);
    return i;
}

static void weapon(Parade *p, ParadeObj *o, int on, int target)   /* enableWeapon(on) (+ setWeaponTarget) */
{
    if (!on && !o->weapon_on)
        return;
    o->weapon_on = on;
    const char *name = on ? group_str(group_sub(o->tmpl, "Weapon|RifleWeapon"), "WeaponName") : NULL;
    ParadeEvent *e = emit(p, PAE_WEAPON, idx(p, o), name, o->pos);
    e->monster = target;
}

static void make_biker(Parade *p, Group inst, int walk_in);

/* requestDelete (or eaten): [R] the Actor's destructor (0x30DF80) then makes each SpawnsItem<n> of its instance (n =
 * 0, 1, ... while there is one) from the block and sends it onWalkIn ([G] the world is always in state 6 here) */
static void obj_delete(Parade *p, ParadeObj *o)
{
    if (o->phase == PAP_GONE)
        return;
    weapon(p, o, 0, -1);
    o->phase = PAP_GONE;
    o->line.on = o->sphere.on = 0;
    o->oflags = 0;
    p->gone_age[idx(p, o)] = 0;
    emit(p, PAE_REMOVED, idx(p, o), o->name, o->pos);
    if (o->kind != PAK_BIKER || !o->instance.at || !p->block.at)
        return;
    Group inst = o->instance;
    for (int n = 0; n < 16; n++) {
        char key[24], name[96];
        snprintf(key, sizeof key, "SpawnsItem%d", n);
        const char *next = group_str(inst, key);
        if (!next)
            break;
        snprintf(name, sizeof name, "ActorInstance|%s", next);
        Group g = group_sub(p->block, name);
        if (g.at)
            make_biker(p, g, 1);
    }
}

/* gCommon.addStats(obj, slot, type) [R] (0x2C51A0): once per object (flag 0x40000000) */
static void add_stats(Parade *p, ParadeObj *o, int m, int type)
{
    if (o->oflags & PAF_COUNTED)
        return;
    o->oflags |= PAF_COUNTED;
    ParadeEvent *e = emit(p, PAE_STATS, idx(p, o), o->name, o->pos);
    e->tmpl = o->tmpl;
    e->monster = m;
    e->level = type;
    e->amount = (float)(int)tnum(o, "Points", 0);
    e->v[0] = tnum(o, "Health", 0);
    e->v[1] = tnum(o, "Energy", 0);
}

static void fart_later(Parade *p, int m)            /* installObjectTimer("Monsters::timedDoFart", monster, 2.0) */
{
    if (p->nfart < MAX_TIMERS) {
        p->fart[p->nfart].left = 2.0f;
        p->fart[p->nfart++].monster = m;
    }
}

static float heading_of_monster(const Parade *p, int m) { const ArmyMonster *a = monster(p, m); return a ? a->heading : 0; }

/* ---------------------------------------------------------------- ParadeBalloon.bsf */

/* moveBalloon [R]: a point up to 5 off along each axis (each its own frand, its sign by that frand > 0.5), kept
 * near where it started (more than 10 off in x: 8 off; 5 in y: 3; 2.5 in z: 1), and a LineMover there at 1 */
static void balloon_move(Parade *p, ParadeObj *o)
{
    if (o->state >= 9)
        return;
    float start[3] = { o->pos[0], o->pos[1], o->pos[2] }, end[3];
    for (int k = 0; k < 3; k++) {
        float r = frand(p);
        end[k] = start[k] + (r > 0.5f ? -1.0f : 1.0f) * 5 * r;
    }
    static const float far_[3] = { 10, 5, 2.5f }, back[3] = { 8, 3, 1 };
    for (int k = 0; k < 3; k++) {
        if (end[k] - o->start[k] > far_[k])
            end[k] = o->start[k] + back[k];
        else if (o->start[k] - end[k] > far_[k])
            end[k] = o->start[k] - back[k];
    }
    line_init(o, start, end, 1.0f);
    o->state = 2;
}

/* rotateBalloon [R], every frame: the wobble's scale falls by 4 a second towards 1; facing (pi/3 + 0.0625 s sin 2t,
 * 0.0625 s cos 3t, -pi/2): pitched 60 degrees onto its face and turned a quarter, so (the models face -y) it flies
 * head first towards -x, belly down */
static void balloon_rotate(ParadeObj *o, float dt)
{
    if (o->osc_scale > 1)
        o->osc_scale -= 4 * dt;
    o->osc += dt;
    o->rot[0] = 0.0625f * o->osc_scale * sinf(o->osc * 2) + PI / 3;
    o->rot[1] = 0.0625f * o->osc_scale * cosf(o->osc * 3);
    o->rot[2] = -PI / 2;
}

static void balloon_construct(Parade *p, ParadeObj *o)
{
    o->osc_scale = 1;
    o->oflags |= PAF_PUSH_OUT | PAF_EFF_NODES;
    o->hp = (int)tnum(o, "HitPoints", 3);
    set_pulse(o, frand(p));                          /* [R] it has no onPulse */
    o->state = 1;
    memcpy(o->start, o->pos, sizeof o->start);
    o->rot[0] = PI / 3;
    o->rot[1] = 0;
    o->rot[2] = -PI / 2;
    update_world(o);
}

static void balloon_mover(Parade *p, ParadeObj *o)   /* onMoverCallback(2) */
{
    if (o->state == 1 || o->state == 2 || o->state == 8)
        balloon_move(p, o);
    else if (o->state == 4) {                        /* recoverFromKnockBack: back to its place at 4 */
        float at[3] = { o->pos[0], o->pos[1], o->pos[2] };
        line_init(o, at, o->start, 4.0f);
        o->state = 8;
    } else if (o->state == 9)
        obj_delete(p, o);
}

/* TakeDamage(monster) [R]: HitSound, the wobble at 20 x, knockBackBalloon (5 away from the monster along x and 7 up,
 * at 20; state 4), one of its HitPoints; at 0: PopSound, its HitEffect on it, the monster's doChuckle, addStats type
 * 0 and it is deleted */
static int balloon_damage(Parade *p, ParadeObj *o, int m)
{
    const ArmyMonster *a = monster(p, m);
    if (!a)
        return 0;
    sound(p, o, tstr(o, "Sound|HitSound"), 0);
    o->osc_scale = 20;
    if (o->state != 9) {
        float at[3] = { o->pos[0], o->pos[1], o->pos[2] };
        float to[3] = { a->pos[0] > at[0] ? at[0] - 5 : at[0] + 5, at[1], at[2] + 7 };
        line_init(o, at, to, 20.0f);
        o->state = 4;
    }
    if (--o->hp == 0) {
        sound(p, o, tstr(o, "Sound|PopSound"), 0);
        effect_on(p, o, tstr(o, "Effect|HitEffect"));
        ParadeEvent *e = emit(p, PAE_MONSTER_SOUND, idx(p, o), "APE_CHUCKLE_HELIUM", a->pos);   /* Monsters::doChuckle */
        e->monster = m;
        add_stats(p, o, m, 0);
        obj_delete(p, o);
    }
    return 1;
}

/* ---------------------------------------------------------------- Biker.bsf and Bike.bsf */

static void bike_hit(Parade *p, ParadeObj *b)        /* Bike.bsf eventMonsterHit: the nearest monster smashes it */
{
    int m = nearest_monster(p, b->pos);
    if (m < 0)
        return;
    smash_object(p, b, p->m[m].heading);
    b->state = 1;
}

/* createBike [R]: the ride's bike before goes (requestDelete), a new one (the template's Attachment, Bike) at start
 * facing the same way, on its own LineMover to end at the biker's speed */
static void create_bike(Parade *p, ParadeObj *o, const float start[3], const float end[3], float facing)
{
    if (alive(p, o->partner) && p->o[o->partner].kind == PAK_BIKE)
        obj_delete(p, &p->o[o->partner]);
    o->partner = -1;
    Group t = template_of(p, tstr(o, "OTRef|Attachment"));
    if (!t.at)
        return;
    int self = idx(p, o), i = new_obj(p, t, PAK_BIKE, start, facing);
    if (i < 0)
        return;
    ParadeObj *b = &p->o[i];
    set_pulse(b, 1.0f);                              /* Bike.bsf Construct */
    b->fly_duration = 1.5f;                          /* dyingDuration */
    line_init(b, start, end, p->o[self].walk_speed);
    b->partner = self;
    p->o[self].partner = i;
}

/* walk [R]: from the end of the street it is at (x = 80 or -80, a share up to 1/40 further out) to the other, at
 * WalkSpeed, facing that way, with a new bike */
static void biker_walk(Parade *p, ParadeObj *o)
{
    float start[3] = { o->pos[0] > 0 ? 80.0f : -80.0f, o->pos[1], o->pos[2] };
    float end[3] = { -start[0], start[1], start[2] };
    start[0] *= 1 + frand(p) / 40;
    float facing = start[0] > 0 ? FACE_LEFT : FACE_RIGHT;
    o->rot[0] = o->rot[1] = 0;
    o->rot[2] = facing;
    line_init(o, start, end, o->walk_speed);
    create_bike(p, o, start, end, facing);
    o->state = 1;
}

static void biker_construct(Parade *p, ParadeObj *o)
{
    o->oflags |= PAF_EDIBLE | PAF_EFF_NODES | PAF_PICKUP;   /* [R] the Actor's 0x80000, setFlags(32768 | 65536) */
    start_anim(p, o, "Ride", 1);
    memcpy(o->start, o->pos, sizeof o->start);
    o->has_throw = (int)tnum(o, "HasThrow", 1);
    o->walk_speed = tnum(o, "WalkSpeed", 4);
    o->throwing_range = tnum(o, "ThrowingRange", 5);
    o->fly_duration = 3;
    o->pos[0] = o->pos[0] < 0 ? -80.0f : 80.0f;      /* [R] at the end of the street on its side */
    set_pulse(o, TIMESTEP);
    o->state = 0;
    update_world(o);
}

/* onWalkIn [R] (event 17, from SpawnsItem or gAIMgr's delayed spawn): its instance's DelayedSpawn before it rides
 * (else the next frame), state 0 */
static void biker_walk_in(ParadeObj *o)
{
    float d = o->instance.at ? group_num(o->instance, "DelayedSpawn", 0) : 0;
    set_pulse(o, d != 0 ? d : TIMESTEP);
    o->state = 0;
}

/* an ActorInstance| of a biker: made, its Construct run, and onWalkIn when the game sends it */
static void make_biker(Parade *p, Group inst, int walk_in)
{
    Group t = template_of(p, group_str(inst, "Name"));
    const char *script = t.at ? group_str(t, "Script|ScriptName") : NULL;
    if (!script || !same_prefix(script, "Biker."))
        return;
    float pos[3] = { 0, 0, 0 };
    Group loc = group_sub(inst, "Location");
    Record r[3];
    if (loc.at && group_items(loc, r, 3) == 3)
        for (int k = 0; k < 3; k++)
            pos[k] = record_num(r[k]);
    int i = new_obj(p, t, PAK_BIKER, pos, group_num(inst, "ZRotation", 0) * PI / 180);
    if (i < 0)
        return;
    p->o[i].instance = inst;
    biker_construct(p, &p->o[i]);
    if (walk_in)
        biker_walk_in(&p->o[i]);
}

/* smash [R]: monster.smashObject(actor), its FlyingSound, state 5 */
static void biker_smash(Parade *p, ParadeObj *o, int m)
{
    smash_object(p, o, heading_of_monster(p, m));
    sound(p, o, tstr(o, "Sound|FlyingSound"), 1);
    o->state = 5;
}

static void biker_pickup(Parade *p, ParadeObj *o)    /* onPickup [R]: PickedUp, no more pulses, its Voice */
{
    o->state = 2;
    start_anim(p, o, "PickedUp", 0);
    set_pulse(o, 0);
    sound(p, o, tstr(o, "Sound|Voice"), 1);
}

/* onPulse [R] */
static void biker_pulse(Parade *p, ParadeObj *o)
{
    if (o->state == 0) {
        biker_walk(p, o);
        set_pulse(o, 0.5f);
        o->state = 1;
    }
    if (o->has_throw == 0 || o->state == 2) {
        weapon(p, o, 0, -1);
        set_pulse(o, 0);
    } else if (o->state == 5) {                      /* flying: deleted after flyingDuration (3 s of pulses) */
        weapon(p, o, 0, -1);
        o->fly_duration -= o->pulse_delay;
        if (o->fly_duration < 0)
            obj_delete(p, o);
    } else if (o->state == 4) {                      /* the throw's pulse: its weapon off, 2 s before the next look */
        weapon(p, o, 0, -1);
        set_pulse(o, 2.0f);
        o->state = 1;
    } else if (o->state == 1) {
        set_pulse(o, 0.5f);
        int m = nearest_monster(p, o->pos);
        if (m >= 0) {                                /* a monster ahead within ThrowingRange (3D): a bottle at it */
            const float *mp = p->m[m].pos;
            if ((o->rot[2] == FACE_RIGHT && o->pos[0] < mp[0]) || (o->rot[2] == FACE_LEFT && o->pos[0] > mp[0])) {
                float dx = o->pos[0] - mp[0], dy = o->pos[1] - mp[1], dz = o->pos[2] - mp[2];
                if (sqrtf(dx * dx + dy * dy + dz * dz) < o->throwing_range && o->state == 1) {
                    set_pulse(o, 0.5f);
                    start_anim(p, o, "Throw", 0);
                    weapon(p, o, 1, m);
                    o->state = 4;
                }
            }
        }
        if (o->pos[0] > p->camera_x - 10 && o->pos[0] < p->camera_x + 10)
            sound(p, o, tstr(o, "Sound|VisibleSound"), 1);   /* the Harley, in view */
    }
}

static void biker_anim_done(Parade *p, ParadeObj *o)   /* onAnimComplete [R] */
{
    if (o->state == 4 || o->state == 1)
        start_anim(p, o, "Ride", 0);
    else if (o->state == 2)
        start_anim(p, o, "PickedUp", 0);
}

/* TakeDamage(monster) [R], not while flying from a throw (state 5): smashed (state 5), then this.actor.onPickup(), the
 * Actor native (id 0x44 -> 0x30C5E0(actor, 0)): its AmbientEffect and ChallengeEffect stopped, nothing else; then
 * PickedUp, and its bike is smashed too. It flies in state 5: deleted when it lands, or when flyingDuration is up */
static int biker_damage(Parade *p, ParadeObj *o, int m)
{
    if (!monster(p, m) || o->state == 5)
        return 0;
    biker_smash(p, o, m);
    start_anim(p, o, "PickedUp", 0);
    if (alive(p, o->partner) && p->o[o->partner].kind == PAK_BIKE)
        bike_hit(p, &p->o[o->partner]);
    return 1;
}

/* ---------------------------------------------------------------- GroundCart.bsf */

/* node 1 of the stands as read from the bundles' RawAnims (FoodCart_Hotdog's Point01, food_cart_Burger's
 * Point02_burg, icecream_stand's icecream_Point02) [R], for a host that cannot say */
static int known_node(const char *shape, float out[3])
{
    static const struct { const char *name; float at[3]; } k[] = {
        { "FoodCart_Hotdog", { 0, 0.417f, 4.778f } }, { "Food_Cart_Burger", { 0, 0, 5.0736f } },
        { "Icecream_Stand", { 0, 0, 4.005f } },
    };
    for (size_t i = 0; shape && i < sizeof k / sizeof k[0]; i++)
        if (same_prefix(shape, k[i].name) && strlen(shape) == strlen(k[i].name)) {
            memcpy(out, k[i].at, 3 * sizeof(float));
            return 1;
        }
    return 0;
}

/* Construct [R]: Edible, PushOutMonsters, OnlyHitWithEffNodes, Pickupable; its Attachment (the stand) made where
 * it stands, facing 0; the food itself moved up to the stand's node 1 (else 3.6 up) */
static void cart_construct(Parade *p, int self)
{
    ParadeObj *o = &p->o[self];
    o->oflags |= PAF_EDIBLE | PAF_PUSH_OUT | PAF_EFF_NODES | PAF_PICKUP;
    o->state = 0;
    o->fly_duration = 3;
    Group t = template_of(p, tstr(o, "OTRef|Attachment"));
    if (!t.at)
        return;
    float at[3] = { o->pos[0], o->pos[1], o->pos[2] };
    int b = new_obj(p, t, PAK_CART_BASE, at, 0);
    o = &p->o[self];
    if (b < 0)
        return;
    ParadeObj *base = &p->o[b];
    base->partner = self;
    o->partner = b;
    float node[3];
    int ok = base->shape && p->host.shape_node && p->host.shape_node(p->host.ctx, base->shape, 1, node);
    if (!ok)
        ok = known_node(base->shape, node);
    if (!ok) {
        node[0] = node[1] = 0;
        node[2] = 3.6f;
    }
    for (int k = 0; k < 3; k++)
        o->pos[k] = at[k] + node[k];
    update_world(o);
}

static ParadeObj *cart_base(Parade *p, ParadeObj *o)
{
    return alive(p, o->partner) && p->o[o->partner].kind == PAK_CART_BASE ? &p->o[o->partner] : NULL;
}

/* onThrow(monster, eaten) [R]: its HitEffect on it; eaten: Monsters::timedDoFart 2 s later (always) and addStats
 * type 0 ([G] the engine then takes it away, as an eaten actor); not eaten: smashObject, flags 2, no longer
 * Pickupable, pulsing every 0.25 s, and the monster's player gets its Points (addPoints, category 0) */
static void cart_throw(Parade *p, ParadeObj *o, int m, int eaten, float heading)
{
    effect_on(p, o, tstr(o, "Effect|HitEffect"));
    if (eaten) {
        if (m >= 0) {
            fart_later(p, m);
            add_stats(p, o, m, 0);
        }
        obj_delete(p, o);
        return;
    }
    smash_object(p, o, heading);
    o->state = 2;
    o->oflags &= ~(uint32_t)PAF_PICKUP;
    set_pulse(o, 0.25f);
    if (m >= 0) {
        ParadeEvent *e = emit(p, PAE_POINTS, idx(p, o), o->name, o->pos);
        e->monster = m;
        e->level = 0;
        e->amount = (float)(int)tnum(o, "Points", 0);
    }
}

static void cart_pulse(Parade *p, ParadeObj *o)      /* onPulse [R]: smashed, 3 s of 0.25 s pulses, then debris */
{
    if (o->state != 2)
        return;
    o->fly_duration -= 0.25f;
    if (o->fly_duration < 0) {
        effect_on(p, o, tstr(o, "Effect|HitEffect"));
        obj_delete(p, o);
    }
}

static void cart_pickup(Parade *p, ParadeObj *o)     /* onPickup [R]: the stand breaks (HitEffectBase) and goes */
{
    o->oflags &= ~(uint32_t)(PAF_PUSH_OUT | PAF_PICKUP);
    ParadeObj *b = cart_base(p, o);
    if (b) {
        effect_on(p, b, tstr(o, "Effect|HitEffectBase"));
        obj_delete(p, b);
    }
}

/* TakeDamage(monster) [R], while the stand is there: HitEffect on it, HitEffectBase on the stand, the stand gone,
 * then onThrow(monster, not eaten): smashed away */
static int cart_damage(Parade *p, ParadeObj *o, int m)
{
    ParadeObj *b = cart_base(p, o);
    if (!monster(p, m) || !b)
        return 0;
    effect_on(p, o, tstr(o, "Effect|HitEffect"));
    effect_on(p, b, tstr(o, "Effect|HitEffectBase"));
    obj_delete(p, b);
    cart_throw(p, o, m, 0, heading_of_monster(p, m));
    return 1;
}

/* ---------------------------------------------------------------- the frame */

static int kind_of_script(const char *script)
{
    if (!script) return -1;
    if (same_prefix(script, "ParadeBalloon.")) return PAK_BALLOON;
    if (same_prefix(script, "Biker.")) return PAK_BIKER;
    if (same_prefix(script, "Bike.")) return PAK_BIKE;
    if (same_prefix(script, "GroundCart.")) return PAK_CART;
    return -1;
}

int parade_script_ours(const char *script) { return kind_of_script(script) >= 0; }

static void obj_step(Parade *p, ParadeObj *o, float dt)
{
    if (o->phase == PAP_HELD || o->phase == PAP_GONE)
        return;
    if (o->kind == PAK_BIKER) {                      /* an Actor: its anim, its pulse, then its mover */
        if (o->anim_loop) {
            if (o->anim_len > 0)
                o->anim_time = fmodf(o->anim_time + dt, o->anim_len);
        } else if (!o->anim_done) {
            o->anim_time += dt;
            if (o->anim_time >= o->anim_len) {
                o->anim_time = o->anim_len;
                o->anim_done = 1;
                biker_anim_done(p, o);
            }
        }
        if (o->pulse_delay >= 0.01f && (o->pulse_time += dt) >= o->pulse_delay) {
            o->pulse_time -= o->pulse_delay;
            biker_pulse(p, o);
        }
        if (o->phase == PAP_ALIVE) {
            if (line_step(o, dt) == 2 && o->state != 2 && o->state != 5)   /* onMoverCallback(2): ride back */
                biker_walk(p, o);
        } else if (o->phase == PAP_FLYING && sphere_step(p, o, dt) && o->state == 5) {
            obj_delete(p, o);                        /* OnHitGround in state 5 */
            return;
        }
        update_world(o);
        return;
    }
    /* a ScriptObject (0x309AC0) [R]: its mover's callback, Process (every frame), then its pulse */
    if (o->phase == PAP_ALIVE) {
        if (line_step(o, dt) == 2) {
            if (o->kind == PAK_BALLOON)
                balloon_mover(p, o);
            else if (o->kind == PAK_BIKE)            /* Bike.bsf: any callback deletes it */
                obj_delete(p, o);
        }
    } else if (o->phase == PAP_FLYING)
        sphere_step(p, o, dt);
    if (o->phase == PAP_GONE)
        return;
    if (o->kind == PAK_BALLOON) {                    /* Process */
        if (o->state == 1)
            balloon_move(p, o);
        balloon_rotate(o, dt);
    }
    if (o->pulse_delay >= 0.01f && (o->pulse_time += dt) >= o->pulse_delay) {
        o->pulse_time -= o->pulse_delay;
        if (o->kind == PAK_CART)
            cart_pulse(p, o);
        else if (o->kind == PAK_BIKE && o->state == 1) {   /* dying: deleted after 1.5 s of 1 s pulses */
            o->fly_duration -= o->pulse_delay;
            if (o->fly_duration < 0)
                obj_delete(p, o);
        }
    }
    if (o->phase != PAP_GONE)
        update_world(o);
}

void parade_update(Parade *p, float dt, const ArmyMonster *m, int nm, float camera_x)
{
    p->m = m;
    p->nm = m ? nm : 0;
    p->camera_x = camera_x;
    for (int i = 0; i < p->nfart; i++)
        if ((p->fart[i].left -= dt) <= 0) {
            ParadeEvent *e = emit(p, PAE_FART, -1, "Monsters::timedDoFart", NULL);
            e->monster = p->fart[i].monster;
            p->fart[i--] = p->fart[--p->nfart];
        }
    for (int i = 0; p->active && i < p->npend; i++)   /* gAIMgr: a SpawnDelay instance made, then onWalkIn [R]; */
        if ((p->pend[i].left -= dt) <= 0) {          /* its clock runs only while it is activated (0x333D48) */
            Group g = p->pend[i].inst;
            p->pend[i--] = p->pend[--p->npend];
            make_biker(p, g, 1);
        }
    for (int i = 0, n = p->n; i < n; i++) {
        if (p->o[i].phase == PAP_GONE) {
            if (p->gone_age[i] < 1000)
                p->gone_age[i]++;
            continue;
        }
        obj_step(p, &p->o[i], dt);
    }
}

/* ---------------------------------------------------------------- making them */

int parade_spawn(Parade *p, const char *tmpl_name, const float pos[3], float facing)
{
    Group t = template_of(p, tmpl_name);
    int kind = kind_of_script(t.at ? group_str(t, "Script|ScriptName") : NULL);
    if (kind < 0)
        return -1;
    int i = new_obj(p, t, kind, pos, facing);
    if (i < 0)
        return -1;
    ParadeObj *o = &p->o[i];
    switch (kind) {
    case PAK_BALLOON: balloon_construct(p, o); break;
    case PAK_BIKER: biker_construct(p, o); break;
    case PAK_BIKE: set_pulse(o, 1.0f); o->fly_duration = 1.5f; break;
    case PAK_CART: cart_construct(p, i); break;
    default: break;
    }
    return i;
}

int parade_load_block(Parade *p, Group block)
{
    parade_clear(p);
    p->block = block;
    if (!block.at)
        return 0;
    int made = 0;
    static const char *const objects[] = { "ObjectInstance|", "AttackObjectInstance|", "XObjectInstance|" };
    Record r[256];
    Group where;
    for (int k = 0; k < 3; k++) {                    /* the block's own objects (0x304450) */
        uint32_t n = group_values(block, objects[k], &where, r, 256);
        for (uint32_t i = 0; i < n; i++) {
            Group inst = record_group(where, r[i]);
            Group t = template_of(p, group_str(inst, "Name"));
            int kind = kind_of_script(t.at ? group_str(t, "Script|ScriptName") : NULL);
            if (kind != PAK_BALLOON && kind != PAK_CART)
                continue;
            float pos[3] = { 0, 0, 0 };
            Group loc = group_sub(inst, "Location");
            Record lr[3];
            if (loc.at && group_items(loc, lr, 3) == 3)
                for (int j = 0; j < 3; j++)
                    pos[j] = record_num(lr[j]);
            made += parade_spawn(p, group_str(inst, "Name"), pos, group_num(inst, "ZRotation", 0) * PI / 180) >= 0;
        }
    }
    /* [R] 0x304450: an ActorInstance| with SpawnedItem waits for a SpawnsItem; one with a SpawnDelay goes to
     * gAIMgr (0x334A20: the instance and its delay), made that much later with onWalkIn; the rest now, no onWalkIn */
    uint32_t n = group_all(block, "ActorInstance|", r, 256);
    for (uint32_t i = 0; i < n; i++) {
        Group inst = record_group(block, r[i]);
        Group t = template_of(p, group_str(inst, "Name"));
        if (kind_of_script(t.at ? group_str(t, "Script|ScriptName") : NULL) != PAK_BIKER ||
            group_num(inst, "SpawnedItem", 0) != 0)
            continue;
        float delay = group_num(inst, "SpawnDelay", 0);
        if (delay != 0 && p->npend < MAX_PENDING) {
            p->pend[p->npend].inst = inst;
            p->pend[p->npend++].left = delay;
            made++;
        } else if (delay == 0) {
            int before = p->n;
            make_biker(p, inst, 0);
            made += p->n > before;
        }
    }
    return made;
}

/* ---------------------------------------------------------------- the monster's side */

int parade_hit(Parade *p, int monster_i, const float c[3], float radius, int blow)
{
    if (!monster(p, monster_i))
        return 0;
    int n = 0;
    for (int i = 0; i < p->n; i++) {
        ParadeObj *o = &p->o[i];
        if (o->phase == PAP_GONE || o->phase == PAP_HELD || o->last_blow == blow)
            continue;
        if (o->kind != PAK_BALLOON && o->kind != PAK_BIKER && !(o->kind == PAK_CART && o->phase == PAP_ALIVE))
            continue;
        /* [R 0x2E2598] the monster's sphere against the object's own shape (0x28FD70: obj_shape) with 0x280470 */
        CollideObj co;
        float nrm[3], pt[3], push[3];
        memset(&co, 0, sizeof co);
        obj_shape(p, o, &co.shape);
        collide_matrix_from16(co.m, o->world);
        co.flags = COLLIDE_F_WORLD;
        co.building = -1;
        if (!collide_obj_sphere(&co, c, radius, nrm, pt, push))
            continue;
        int took = 0;
        if (o->kind == PAK_BALLOON)
            took = balloon_damage(p, o, monster_i);
        else if (o->kind == PAK_BIKER)
            took = biker_damage(p, o, monster_i);
        else if (o->kind == PAK_CART)
            took = cart_damage(p, o, monster_i);
        if (took) {
            o->last_blow = blow;
            n++;
        }
    }
    return n;
}

int parade_nearest_pickup(const Parade *p, const float hand[3], const float root[3], const float forward[3],
                          float radius)
{
    int best = -1;
    float bd = 3.4e38f;
    for (int i = 0; i < p->n; i++) {
        const ParadeObj *o = &p->o[i];
        if (o->phase == PAP_GONE || !(o->oflags & PAF_PICKUP) || (o->oflags & PAF_HELD))
            continue;
        const float *at = &o->world[12];
        float dx = at[0] - hand[0], dy = at[1] - hand[1], dz = at[2] - hand[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d > radius * radius || d >= bd)
            continue;
        float v[2] = { at[0] - root[0], at[1] - root[1] };   /* in front of the root */
        if (v[0] * forward[0] + v[1] * forward[1] <= 0)
            continue;
        best = i;
        bd = d;
    }
    return best;
}

/* [R] the hold (0x2DA310): BeingHeld, neither Pickupable nor PushOutMonsters, its mover replaced, onPickup */
void parade_grab(Parade *p, int obj, int monster_i, float blend)
{
    (void)monster_i;
    if (!alive(p, obj) || p->o[obj].phase == PAP_HELD)
        return;
    ParadeObj *o = &p->o[obj];
    o->oflags = (o->oflags | PAF_HELD) & ~(uint32_t)(PAF_PICKUP | PAF_PUSH_OUT);
    o->phase = PAP_HELD;
    o->line.on = o->sphere.on = 0;
    o->blend = blend > 0.001f ? blend : 0;
    o->blend_left = o->blend;
    mat_quat(o->world, o->from_q);
    memcpy(o->from_p, &o->world[12], sizeof o->from_p);
    if (o->kind == PAK_BIKER)
        biker_pickup(p, o);
    else if (o->kind == PAK_CART)
        cart_pickup(p, o);
}

/* [R] HeldObjectMover (init 0x310ED0, held 0x3109E0), as people.c: the MajorAxis (default 2) along the hand's z, the
 * next along its y, the last their cross product; at the hand plus HoldOffset (MirroredHoldOffset when mirrored) in
 * the hand's frame; [G] blended in linearly from where it was */
void parade_hold(Parade *p, int obj, const float hand[16], int mirrored, float dt)
{
    if (!alive(p, obj) || p->o[obj].phase != PAP_HELD)
        return;
    ParadeObj *o = &p->o[obj];
    int major = (int)tnum(o, "MajorAxis", 2);
    if (major < 0 || major > 2)
        major = 2;
    int i1 = (major + 1) % 3, i2 = (major + 2) % 3;
    float axis[3][3];
    memcpy(axis[major], hand + 8, sizeof axis[0]);
    memcpy(axis[i1], hand + 4, sizeof axis[0]);
    axis[i2][0] = axis[major][1] * axis[i1][2] - axis[major][2] * axis[i1][1];
    axis[i2][1] = axis[major][2] * axis[i1][0] - axis[major][0] * axis[i1][2];
    axis[i2][2] = axis[major][0] * axis[i1][1] - axis[major][1] * axis[i1][0];
    float off[3] = { 0, 0, 0 };
    Group og = group_sub(o->tmpl, mirrored ? "MirroredHoldOffset" : "HoldOffset");
    Record r[3];
    if (og.at && group_items(og, r, 3) == 3)
        for (int k = 0; k < 3; k++)
            off[k] = record_num(r[k]);
    float t[16];
    for (int c = 0; c < 3; c++) {
        for (int k = 0; k < 3; k++)
            t[4 * c + k] = axis[c][k];
        t[4 * c + 3] = 0;
    }
    for (int k = 0; k < 3; k++)
        t[12 + k] = hand[12 + k] + off[0] * hand[k] + off[1] * hand[4 + k] + off[2] * hand[8 + k];
    t[15] = 1;
    if (o->blend_left > 0) {
        o->blend_left -= dt;
        float f = o->blend_left > 0 ? 1 - o->blend_left / o->blend : 1, tq[4], oq[4];
        mat_quat(t, tq);
        slerp(o->from_q, tq, f, oq);
        quat_mat(oq, o->world);
        for (int k = 0; k < 3; k++)
            o->world[12 + k] = o->from_p[k] + (t[12 + k] - o->from_p[k]) * f;
    } else {
        memcpy(o->world, t, sizeof t);
    }
    memcpy(o->pos, &o->world[12], sizeof o->pos);
    if (o->kind == PAK_BIKER) {                      /* its anim goes on in the hand */
        if (o->anim_loop && o->anim_len > 0)
            o->anim_time = fmodf(o->anim_time + dt, o->anim_len);
        else if (!o->anim_done && (o->anim_time += dt) >= o->anim_len) {
            o->anim_time = o->anim_len;
            o->anim_done = 1;
            biker_anim_done(p, o);
        }
    }
}

/* throwObject(eat) (0x2D9DC0): onThrow(monster, eat). [R] Biker.bsf: eaten, addStats type 1 and a fart one time in
 * five 2 s later, then the engine deletes the eaten actor; not eaten, smash (state 5: deleted when it lands) */
void parade_throw(Parade *p, int obj, int monster_i, int eat, float monster_heading)
{
    if (!alive(p, obj) || p->o[obj].phase != PAP_HELD)
        return;
    ParadeObj *o = &p->o[obj];
    o->oflags &= ~(uint32_t)PAF_HELD;
    o->phase = PAP_ALIVE;
    memcpy(o->pos, &o->world[12], sizeof o->pos);
    int m = monster(p, monster_i) ? monster_i : -1;
    if (o->kind == PAK_BIKER) {
        if (eat) {
            if (m >= 0) {
                add_stats(p, o, m, 1);
                if (frand(p) <= 0.2f)
                    fart_later(p, m);
            }
            obj_delete(p, o);
        } else {
            smash_object(p, o, monster_heading);
            sound(p, o, tstr(o, "Sound|FlyingSound"), 1);
            o->state = 5;
        }
    } else if (o->kind == PAK_CART)
        cart_throw(p, o, m, eat, monster_heading);
}

int parade_is_edible(const Parade *p, int obj) { return alive(p, obj) && (p->o[obj].oflags & PAF_EDIBLE) != 0; }

/* ---------------------------------------------------------------- the rest */

Parade *parade_new(const ParadeHost *host)
{
    Parade *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    if (host)
        p->host = *host;
    return p;
}

void parade_free(Parade *p) { free(p); }

void parade_clear(Parade *p)
{
    p->n = 0;
    p->nev = 0;
    p->npend = 0;
    p->nfart = 0;
    p->block.at = 0;
}

void parade_set_collide(Parade *p, CollideWorld *w) { p->cw = w; }
void parade_activate(Parade *p, int on) { if (p) p->active = on; }

void parade_collide_add(Parade *p, CollideWorld *w)
{
    for (int i = 0; i < p->n; i++) {
        ParadeObj *o = &p->o[i];
        o->cobj = -1;
        if (!w || o->phase == PAP_GONE)
            continue;
        CollideShape sh;
        float m[12];
        obj_shape(p, o, &sh);
        collide_matrix_from16(m, o->world);
        uint32_t fl = COLLIDE_F_WORLD | (o->oflags & ~(uint32_t)(PAF_HELD | PAF_COUNTED));
        /* [R 0x2DA310] a held object stays in the world with its shape: PushOutMonsters cleared (the monster's box no longer
         * meets it), flying objects and shots still do (flag 4 stays: no removeObject; a shot's own owner is no hit) */
        if (o->phase == PAP_HELD)
            fl &= ~(uint32_t)COLLIDE_F_PUSHOUT;
        o->cobj = collide_add(w, &sh, m, fl, COLLIDE_TAG(COLLIDE_OWN_PARADE, i), 0);
        if (o->sphere.on && o->cm_on)                /* flying: another flying object exchanges impulses with it (0x285E90) */
            collide_set_mover(w, o->cobj, &o->cm, o->cmat, o->sphere.vel);
    }
}

int parade_count(const Parade *p) { return p->n; }
const ParadeObj *parade_obj(const Parade *p, int i) { return i >= 0 && i < p->n ? &p->o[i] : NULL; }

int parade_events(Parade *p, ParadeEvent *out, int max)
{
    int n = p->nev < max ? p->nev : max;
    memcpy(out, p->ev, sizeof *out * (size_t)n);
    memmove(p->ev, p->ev + n, sizeof *p->ev * (size_t)(p->nev - n));
    p->nev -= n;
    return n;
}
