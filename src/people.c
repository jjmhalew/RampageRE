/* people.c - the people of a block and the monster's hold on them (people.h; docs/PEOPLE_NOTES.md).
 * [R] marks what is read from the game's code or scripts, [G] what is a guess or a stand-in. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "people.h"
#include "wrand.h"

#define PI 3.14159265f
#define DEG (180.0f / PI)
#define TIMESTEP (1.0f / 30.0f)                      /* gWorld.getTimeStep(): its step, 1/30 [R 0x2B7800] */
#define DELETE_ME (4 | 8 | 16)                       /* [R] what a script handler returns to have its actor deleted */
#define MAX_EVENTS 256
#define MAX_TIMERS 32
#define MAX_OBJECTS 16

/* object flags (+0x4C), names from the table at 0x3F5F70 [R] */
enum { OF_DYING = 0x800, OF_PUSH_OUT = 0x4000, OF_EFF_NODES = 0x8000, OF_PICKUP = 0x10000, OF_HELD = 0x20000,
       OF_ACTOR = 0x80000, OF_COUNTED = 0x40000000 };

struct People {
    Person *v;
    int n, cap;
    PeopleClipSeconds clip_seconds;
    void *ctx;
    float droppable_rate;
    const PeopleMonster *m;                          /* this frame's monsters */
    int nm;
    PeopleEvent ev[MAX_EVENTS];
    int nev;
    struct { float left; PeopleEvent ev; } timer[MAX_TIMERS];
    int ntimers;
    PeopleFindTemplate find;
    void *find_ctx;
    PeopleVisible visible;
    void *visible_ctx;
    int band_flee, band_sound;                       /* BandMember.bsf's globals: flee, bandSFXAllocated */
    int hitter;                                      /* the monster slot whose blow is being dealt (people_hit) */
    struct { int building, storey, column; } retired[64];   /* the windows grabbed from (0x318140(.., 1)) */
    int nretired;
    /* the script objects that make people: Band.bsf (once, after 0.25 s), RoyalGuardHut.bsf (pulsing each 1 s) and
     * ActorSwarm.bsf (swarm 1: pulsing each 2 s; state 0 before its first, 1 after it, 2 deleted) */
    struct { int hut, swarm; Group t; float pos[3], timer, create_time; int state, guard, made; } obj[MAX_OBJECTS];
    int nobj, nmade;                                 /* made: its making's number (people_object) */
    Group block;                                     /* people_set_block: its ActorInstance|s for the chains */
    int not_in_play;                                 /* people_set_world_state: the world's state is not 6 */
    Group spawn[16];                                 /* the chains' next ones, made after the frame's update */
    int nspawn;
};

/* ---------------------------------------------------------------- small things */

/* gWorld.frand() (0x279F98): the world's Park-Miller, gWorld +0x828 (wrand.c) [R] */
static float frand(People *p)
{
    (void)p;
    return wrand_frand();
}

static const char *tstr(const Person *q, const char *key) { return q->tmpl.at ? group_str(q->tmpl, key) : NULL; }
static float tnum(const Person *q, const char *key, float def) { return q->tmpl.at ? group_num(q->tmpl, key, def) : def; }

static void emit(People *p, int type, int who, const char *name, const char *node, const float v[3], float amount, int on)
{
    if (p->nev >= MAX_EVENTS)
        return;
    PeopleEvent *e = &p->ev[p->nev++];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->who = who;
    e->name = name;
    e->node = node;
    e->on = on;
    e->amount = amount;
    e->monster = p->hitter;
    e->object = -1;
    if (v)
        memcpy(e->v, v, sizeof e->v);
}

static void later(People *p, float delay, int type, const float v[3], float amount)   /* installObjectTimer */
{
    if (p->ntimers >= MAX_TIMERS)
        return;
    p->timer[p->ntimers].left = delay;
    memset(&p->timer[p->ntimers].ev, 0, sizeof(PeopleEvent));
    p->timer[p->ntimers].ev.type = type;
    p->timer[p->ntimers].ev.who = -1;
    p->timer[p->ntimers].ev.amount = amount;
    if (v)
        memcpy(p->timer[p->ntimers].ev.v, v, 3 * sizeof(float));
    p->ntimers++;
}

static int who_of(const People *p, const Person *q) { return (int)(q - p->v); }

static int same_text(const char *a, const char *b)   /* any case, as the game compares names (0x32C9C8) */
{
    for (; *a && *b; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

static void sound(People *p, Person *q, const char *key)   /* add3DSfx(getStringProp(key)) */
{
    const char *s = tstr(q, key);
    if (s)
        emit(p, PE_SOUND, who_of(p, q), s, NULL, q->pos, 0, 1);
}

static void effect(People *p, Person *q, const char *key, const float at[3], const char *node)
{
    const char *s = tstr(q, key);
    if (s)
        emit(p, PE_EFFECT, who_of(p, q), s, node, at, 0, 1);
}

/* an effect of the person's own, which can be stopped: in slot (PFX_*), following the person or left at `at` */
static void fx_on(People *p, Person *q, int slot, const char *name, const float at[3], int follow)
{
    if (!name)
        return;
    emit(p, PE_FX_ON, who_of(p, q), name, NULL, at, (float)follow, slot);
    q->fx |= 1 << slot;
}

static void fx_off(People *p, Person *q, int slot)
{
    if (q->fx & (1 << slot))
        emit(p, PE_FX_OFF, who_of(p, q), NULL, NULL, NULL, 0, slot);
    q->fx &= ~(1 << slot);
}

/* [R] the Actor binding onPickup (id 0x44 -> 0x30C5E0(actor, 0)), which the scripts' TakeDamage call as
 * this.actor.onPickup(): it stops the actor's AmbientEffect (+0x108) and ChallengeEffect (+0x110) and does nothing
 * else (it is not the script's onPickup, script event 8). The grabs stop them the same way (0x2B344C, 0x2DA45C) */
static void actor_on_pickup(People *p, Person *q)
{
    fx_off(p, q, PFX_AMBIENT);
    fx_off(p, q, PFX_CHALLENGE);
}

/* getNearestMonster [R 0x2B26F0, the Actor binding 0x42 of 0x2A8FA0]: the nearest player monster by 3D distance
 * (0x2B27E8 is sqrt.s), starting at 1000: none farther than 1000, so in a block any; NULL with none [G: the game then hands the
 * script gWorld +0x3C (0x4D17DC) as the monster; what the script sees is not followed] */
static const PeopleMonster *nearest_monster(const People *p, const float pos[3])
{
    const PeopleMonster *best = NULL;
    float bd = 1000;
    for (int i = 0; i < p->nm; i++) {
        float dx = p->m[i].pos[0] - pos[0], dy = p->m[i].pos[1] - pos[1], dz = p->m[i].pos[2] - pos[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d < bd) {
            bd = d;
            best = &p->m[i];
        }
    }
    return best;
}

/* the mover's target (flag 8; LineMover.getTarget, +0x8C) [R 0x316060]: given none (+0x84), the nearest player monster
 * by squared 3D distance with no limit (from FLT_MAX); NULL none */
static const PeopleMonster *mover_target(const People *p, const float pos[3])
{
    const PeopleMonster *best = NULL;
    float bd = 3.4028235e38f;
    for (int i = 0; i < p->nm; i++) {
        float dx = p->m[i].pos[0] - pos[0], dy = p->m[i].pos[1] - pos[1], dz = p->m[i].pos[2] - pos[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bd) {
            bd = d;
            best = &p->m[i];
        }
    }
    return best;
}

/* the world matrix (column-major) of a place and a heading (0 faces +y), turned on by Euler angles (x, y, z) */
static void place_matrix(float w[16], const float pos[3], float heading, const float rot[3])
{
    float h = heading / DEG + (rot ? rot[2] : 0), ax = rot ? rot[0] : 0, ay = rot ? rot[1] : 0;
    float ch = cosf(h), sh = sinf(h), cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay);
    /* R = Rz(h) Rx(ax) Ry(ay): setFacing (0x277E78 -> 0x101330(m, z, x, y)) [R] */
    float r[9] = { ch * cy - sh * sx * sy, sh * cy + ch * sx * sy, -cx * sy,   /* Rz(h) Rx(ax) Ry(ay) */
                   -sh * cx, ch * cx, sx,
                   ch * sy + sh * sx * cy, sh * sy - ch * sx * cy, cx * cy };
    for (int c = 0; c < 3; c++) {
        for (int k = 0; k < 3; k++)
            w[4 * c + k] = r[3 * c + k];
        w[4 * c + 3] = 0;
    }
    w[12] = pos[0];
    w[13] = pos[1];
    w[14] = pos[2];
    w[15] = 1;
}

/* rotation matrix (columns of a column-major 4x4) <-> quaternion (x, y, z, w) */
static void mat_quat(const float m[16], float q[4])
{
    float t = m[0] + m[5] + m[10];
    if (t > 0) {
        float s = sqrtf(t + 1) * 2;
        q[3] = s / 4;
        q[0] = (m[6] - m[9]) / s;
        q[1] = (m[8] - m[2]) / s;
        q[2] = (m[1] - m[4]) / s;
    } else if (m[0] > m[5] && m[0] > m[10]) {
        float s = sqrtf(1 + m[0] - m[5] - m[10]) * 2;
        q[3] = (m[6] - m[9]) / s;
        q[0] = s / 4;
        q[1] = (m[4] + m[1]) / s;
        q[2] = (m[8] + m[2]) / s;
    } else if (m[5] > m[10]) {
        float s = sqrtf(1 + m[5] - m[0] - m[10]) * 2;
        q[3] = (m[8] - m[2]) / s;
        q[0] = (m[4] + m[1]) / s;
        q[1] = s / 4;
        q[2] = (m[9] + m[6]) / s;
    } else {
        float s = sqrtf(1 + m[10] - m[0] - m[5]) * 2;
        q[3] = (m[1] - m[4]) / s;
        q[0] = (m[8] + m[2]) / s;
        q[1] = (m[9] + m[6]) / s;
        q[2] = s / 4;
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

static void quat_mul(const float a[4], const float b[4], float out[4])   /* a then b in a's frame: R(a) R(b) */
{
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
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

/* ---------------------------------------------------------------- the actor's anim (0x30AAD0) */

/* [R] startAnim: the look ActorAnimShape|<name>; one play lasts its Duration (+0xC) if it has one, else the clip's
 * (the AnimName's, or the shape's own); AnimRepeat sets flag 2, a play that repeats. The clip is fitted to the play:
 * its time runs by the frame's step (0x30A850) and the pose is the clip at time / that length (0x30A770; the loader
 * 0x30AC9C stores Duration as it is), so a window occupant's WindowIdle (a 1.2 s clip, Duration 0.5) plays 2.4 times
 * as fast and its WindowEmerge (0.17 s, Duration 0.5) a third as fast, in the game as here. [R] The script's second argument
 * keeps the actor's mover (Actor binding 0x2D -> 0x30C820(actor, name, keep, 0)): with 0 the mover (+0x84) is
 * deleted (0x283BF0) and the actor stands; the scripts that pass 0 set their mover after it. startAnimAtRandomOffset
 * (0x2F) passes an offset of frand (0 .. 1, seconds), which the record's setTime (vtable +0x1C, 0x30C070) takes
 * modulo the play's length: a point within the first second of the play. */
static void start_anim(People *p, Person *q, const char *name, int random_offset, int keep)
{
    if (!keep)
        q->mv.active = 0;
    snprintf(q->anim, sizeof q->anim, "%s", name);
    char key[64];
    snprintf(key, sizeof key, "ActorAnimShape|%s", name);
    Group look = q->tmpl.at ? group_sub(q->tmpl, key) : q->tmpl;
    float len = 0;
    q->anim_loop = 0;
    if (look.at) {
        q->anim_loop = group_num(look, "AnimRepeat", 0) != 0;
        len = fabsf(group_num(look, "Duration", 0));
        if (len <= 0 && p->clip_seconds) {
            const char *shape = group_str(look, "Shape|ShapeName");
            if (!shape)
                shape = tstr(q, "Shape|ShapeName");
            len = p->clip_seconds(p->ctx, group_str(look, "Anim|AnimName"), shape);
        }
    }
    q->anim_len = len;
    q->anim_time = 0;
    if (random_offset) {
        float r = frand(p);                          /* 0x2A9258: frand x 2^-31 of the world's Park-Miller */
        if (r > 0.001f && len > 0)
            q->anim_time = r - len * (float)(int)(r / len);
    }
    q->anim_done = 0;
}

/* ---------------------------------------------------------------- the LineMover (0x315E10) */

static void mover_init(Person *q, const float a[3], const float b[3], float speed)   /* LineMover.init */
{
    memcpy(q->mv.pts[0], a, sizeof q->mv.pts[0]);
    memcpy(q->mv.pts[1], b, sizeof q->mv.pts[1]);
    q->mv.n = 2;
    q->mv.seg = 0;
    q->mv.t = 0;
    q->mv.speed = speed;
    q->mv.active = 1;
    memcpy(q->pos, a, sizeof q->pos);
}

static void mover_add(Person *q, const float pt[3])
{
    if (q->mv.n < 4)
        memcpy(q->mv.pts[q->mv.n++], pt, sizeof q->mv.pts[0]);
}

static float heading_of(float dx, float dy) { return atan2f(-dx, dy) * DEG; }   /* the facing that looks along (dx, dy) */

/* [R] one frame: the share along the segment grows by speed x dt / its length (+0x58); past 1 the next segment
 * starts from its beginning (3, a point reached) or, after the last, it stays at the end (2) unless it loops (flag
 * 2); flag 1 faces the way it goes (0x316E60), flag 8 the nearest monster (the target, +0x84); while it simply moves
 * (1) a monster nearer than MonsterResponseDist (+0x5C holds its square) answers 4. 0: no mover. [R 0x315F70] At its
 * end the share is set back to 1 and the end (2) is told again every frame, until the script gives the actor another
 * mover or none (startAnim with 0, setMover). A segment of length 0 (+0x50) does not move: 1 every frame. */
static int mover_step(People *p, Person *q, float dt)
{
    if (!q->mv.active || q->mv.n < 2)
        return 0;
    int r = 1, s = q->mv.seg;
    float *a = q->mv.pts[s], *b = q->mv.pts[s + 1];
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len != 0)
        q->mv.t += dt * q->mv.speed / len;
    if (q->mv.t > 1) {
        if (q->mv.seg < q->mv.n - 2) {
            q->mv.seg++;
            q->mv.t = 0;
            r = 3;
        } else if (q->mv.flags & 2) {
            q->mv.seg = 0;
            q->mv.t = 0;
            r = 2;
        } else {
            q->mv.t = 1;
            r = 2;
        }
    }
    s = q->mv.seg;
    a = q->mv.pts[s];
    b = q->mv.pts[s + 1];
    for (int k = 0; k < 3; k++)
        q->pos[k] = a[k] + (b[k] - a[k]) * q->mv.t;
    if ((q->mv.flags & 1) && (b[0] != a[0] || b[1] != a[1]))
        q->heading = heading_of(b[0] - a[0], b[1] - a[1]);
    const PeopleMonster *m = mover_target(p, q->pos);
    if ((q->mv.flags & 8) && m)
        q->heading = heading_of(m->pos[0] - q->pos[0], m->pos[1] - q->pos[1]);
    /* [R 0x316380 .. 0x3167B4] flag 4: past +0x68 of a segment (0.5, set by both inits, 0x3174E0 / 0x317770) the
     * facing turns towards the next segment's way (the first's when it loops; none on a last one that does not), by
     * smoothstep((t - 0.5) / 0.5), under 0.001 none and over 0.999 all of it: a slerp of the two facings (+0x70, the
     * way's), the shorter way round (ActorSwarm's children round their corner). [G: a segment of no length (the
     * tourists' mover) is left out] */
    if ((q->mv.flags & 4) && q->mv.t > 0.5f && (b[0] != a[0] || b[1] != a[1])) {
        const float *c = NULL, *e = NULL;
        if (s < q->mv.n - 2) {
            c = b;
            e = q->mv.pts[s + 2];
        } else if (q->mv.flags & 2) {
            c = q->mv.pts[0];
            e = q->mv.pts[1];
        }
        if (c && (e[0] != c[0] || e[1] != c[1])) {
            float u = (q->mv.t - 0.5f) / 0.5f, w = (3 - 2 * u) * u * u;
            float h0 = heading_of(b[0] - a[0], b[1] - a[1]), dh = heading_of(e[0] - c[0], e[1] - c[1]) - h0;
            dh -= 360.0f * floorf((dh + 180.0f) / 360.0f);
            q->heading = h0 + dh * (w < 0.001f ? 0 : w > 0.999f ? 1 : w);
        }
    }
    if (r == 1 && q->mv.resp2 > 0)
        for (int i = 0; i < p->nm; i++) {
            float dx = q->pos[0] - p->m[i].pos[0], dy = q->pos[1] - p->m[i].pos[1], dz = q->pos[2] - p->m[i].pos[2];
            if (dx * dx + dy * dy + dz * dz < q->mv.resp2)
                r = 4;
        }
    return r;
}

/* ---------------------------------------------------------------- the natives the scripts call */

/* [R] smashObject (0x2B2F50): an actor (or a street object, class 0x4D0720) flies off on a SpherePhysicsMover:
 * a yaw picked by 0x2B4390 from a cone of 120 degrees around -y (towards the camera), narrowed to 20 .. 120 degrees
 * on the side the monster faces (narrow when it faces away, +y, wide when it faces the camera), at 15 along it and
 * 15 x (0.8 .. 1.3) up, spinning +-(2 pi + pi/6 x r) about x and +-(pi/16 + pi/16 x r) about y and z. */
static void smash_object(People *p, Person *q, float monster_heading)
{
    const float c = 3 * PI / 2, lo = 20 / DEG, w = 120 / DEG;
    float h = monster_heading / DEG;
    float A = sinf(c), B = cosf(c), A2 = sinf(h + PI / 2), B2 = cosf(h + PI / 2);
    float d = -(B2 * B + A2 * A), cross = B2 * A - A2 * B;
    float width = lo + (w - lo) * (1 - d) / 2, a0 = c - w / 2, a1 = c + w / 2;
    if (cross < 0)
        a0 = a1 - width;
    else
        a1 = a0 + width;
    float yaw = a0 + (a1 - a0) * frand(p);
    q->vel[0] = 15 * cosf(yaw);
    q->vel[1] = 15 * sinf(yaw);
    q->vel[2] = 15 * (0.8f + 0.5f * frand(p));
    q->spin[0] = (frand(p) < 0.5f ? 1 : -1) * (2 * PI + PI / 6 * frand(p));
    q->spin[1] = (frand(p) < 0.5f ? 1 : -1) * (PI / 16 + PI / 16 * frand(p));
    q->spin[2] = (frand(p) < 0.5f ? 1 : -1) * (PI / 16 + PI / 16 * frand(p));
    q->pos[0] = q->world[12];                        /* from where it is (in the hand, in a window) */
    q->pos[1] = q->world[13];
    q->pos[2] = q->world[14];
    q->mv.active = 0;
    q->flags &= ~(uint32_t)OF_HELD;
    q->phase = PP_FLYING;
    /* [R] the SpherePhysicsMover (0x28DE70, its properties 0x28F220): Gravity 9.81, Mass 1, Restitution 0.8,
     * SlidingFriction 0.4, InertiaSolidity 1 unless the template sets them (no person does); its radius the person's
     * shape's (sqrt of +0x34: CollisionRadius, DefActorProps 2); the turn rates as given, about the world's axes
     * (0x28DD80) */
    collide_mover_init(&q->cm, tnum(q, "CollisionRadius", 2), tnum(q, "Gravity", 9.81f), tnum(q, "Mass", 1),
                       tnum(q, "Restitution", 0.8f), tnum(q, "SlidingFriction", 0.4f), tnum(q, "InertiaSolidity", 1));
    collide_mover_spin(&q->cm, q->spin);
    memcpy(q->cm.vel, q->vel, sizeof q->cm.vel);
    collide_matrix_from16(q->cmat, q->world);
}

static void leave_window(Person *q) { q->building = -1; }   /* leaveBuilding / closeWindow / 0x318140 */

static int fart_maybe(People *p)                     /* the scripts' onThrow when eaten: 1 in 5 */
{
    return frand(p) <= 0.2f;                         /* installObjectTimer("Monsters::timedDoFart", 2.0) */
}

/* ---------------------------------------------------------------- the scripts */

/* -- Person_Window.bsf: 1 emerging, 2 looking, 3 going back in, 5 falling, 6 landing, 7 running, 20 held, 22 flying */
static int window_anim_done(People *p, Person *q)
{
    if (q->state >= 20)
        return 1;
    if (q->state == 1) {
        start_anim(p, q, "WindowLook", 0, 0);
        q->state = 2;
        q->state_time = q->delay_time = 0;
        q->pulse_time = -frand(p);
    } else if (q->state == 2) {
        float r = frand(p);
        if (q->state_time >= 7.5f) {
            start_anim(p, q, "WindowReEnter", 0, 0);
            q->state = 3;
        } else if (q->delay_time > 0) {
            start_anim(p, q, "WindowIdle", 0, 0);
        } else if (r < 0.2f) {
            start_anim(p, q, "WindowScared", 0, 0);
        } else if (r < 0.4f) {
            start_anim(p, q, "WindowLook", 0, 0);
        } else {
            q->delay_time = 0.5f + 0.5f * frand(p);
            start_anim(p, q, "WindowIdle", 0, 0);
        }
    } else if (q->state == 3) {
        q->state = 0;
        emit(p, PE_WINDOW_CLOSE, (int)(q - p->v), NULL, NULL, q->pos, 0, 1);   /* closeWindow: it shuts */
        leave_window(q);
        int drop = 0;                                /* a droppable falls on a monster well below, from storey 4 up */
        for (int i = 0; i < p->nm; i++)
            if (fabsf(q->pos[0] - p->m[i].pos[0]) < 5.0f * 2.0f && q->pos[2] > p->m[i].pos[2] + 15)
                drop = 1;
        if (q->pos[2] < 3.0f * 4.0f + 6.0f)
            drop = 0;
        if (drop && p->droppable_rate >= frand(p))
            emit(p, PE_DROP, who_of(p, q), NULL, NULL, q->pos, 0, 1);
        return DELETE_ME;
    } else if (q->state == 6) {
        start_anim(p, q, "Running", 0, 1);
        q->mv.speed = 10;
        q->heading = 180;                            /* FACE_BACK: towards the street */
        q->state = 7;
        q->sub = 0;
    }
    return 1;
}

static int window_pulse(Person *q)
{
    q->state_time += 0.25f;
    q->delay_time -= 0.25f;
    if (q->state == 22) {
        q->fly_duration -= 0.25f;
        if (q->fly_duration < 0)
            return DELETE_ME;
    }
    return 1;
}

static int window_mover(People *p, Person *q, int st)
{
    if (q->state >= 20)
        return 1;
    if (st == 2) {
        if (q->state == 7)
            return DELETE_ME;
    } else if (st == 3) {
        if (q->state == 5) {
            q->state = 6;
            start_anim(p, q, "Landing", 0, 1);
            q->mv.speed = 0;
        } else if (q->state == 7 && q->sub == 0) {
            q->heading = q->run_heading;
        }
    }
    return 1;
}

/* onFallFromWindow (sent by 0x317DC0): down to the street at 20, a step out, then running off along it */
static void window_fall(People *p, Person *q)
{
    q->state = 5;
    start_anim(p, q, "Falling", 0, 0);
    leave_window(q);
    q->flags |= OF_PICKUP;                           /* setFlags(65536) [R 0x278688: the WorldObject binding 0x277C10's
                                                      * case 0x13 ors it into +0x4C; clearFlags, 0x14, and-nots it] */
    float dir = -1.0f + 2.0f * (float)(frand(p) > 0.5f);
    float start[3] = { q->pos[0], q->pos[1], q->pos[2] }, end[3] = { q->pos[0], q->pos[1] - 1, 0 };
    float next[3] = { end[0], dir * 1.0f - 3.0f, 0 }, run[3] = { -dir * 100.0f, next[1], 0 };
    mover_init(q, start, end, 20);
    q->mv.flags = 0;
    mover_add(q, next);
    mover_add(q, run);
    sound(p, q, "Sound|FallingSound");
    q->run_heading = dir == -1 ? -90.0f : 90.0f;     /* FACE_RIGHT runs to +x, FACE_LEFT to -x */
}

/* -- the per-kind handlers, as each script has them */

static void on_pickup(People *p, Person *q);
static void smash(People *p, Person *q, float monster_heading);
static void walk(People *p, Person *q);
static void decide(People *p, Person *q);

/* -- Mime.bsf: 4 walking in, 1 performing Anim1 .. Anim8 (each twice more, then the next; after 8 back to 1), 2 held,
 * 8 flying. Anim7 shuffles 2.5 to -x and Anim8 2.5 to +x, at 50 x the frame step a second */
static void mime_anim(People *p, Person *q, int random_offset)
{
    char name[16];
    snprintf(name, sizeof name, "Anim%d", q->cur_anim);
    start_anim(p, q, name, random_offset, 1);
}

static void mime_facing(Person *q)                   /* chooseFacing */
{
    static const float h[9] = { -180, -180, -180, -180, 90, 0, -90, 90, -90 };
    q->heading = q->cur_anim >= 0 && q->cur_anim <= 8 ? h[q->cur_anim] : -180;
}

static void mime_mover(Person *q)                    /* chooseMover */
{
    float end[3] = { q->pos[0], q->pos[1], q->pos[2] };
    if (q->cur_anim == 7) {
        end[0] -= 2.5f;
        mover_init(q, q->pos, end, 50 * TIMESTEP);
        q->mv.flags = 0;
    } else if (q->cur_anim == 8) {
        if (q->state == 4) {                         /* (only reached from a walk-in) */
            float start[3] = { q->stationary ? q->end_loc[0] - 5 : -60, q->end_loc[1], 0 };
            mover_init(q, start, q->end_loc, 35 * TIMESTEP);
        } else {
            end[0] += 2.5f;
            mover_init(q, q->pos, end, 50 * TIMESTEP);
        }
        q->mv.flags = 0;
    }
}

/* -- RoyalGuard.bsf: out of the hut (the hut's mover, 3 towards -y), 4 -> 16 five along MarchDir, 512 Perform three
 * times, 32 Idle for 2 s, 64 five back, 128 three in: deleted, or (its hut destroyed) 256 eighty away, deleted */
static void guard_march(Person *q, float dx, float dy)
{
    float end[3] = { q->pos[0] + dx, q->pos[1] + dy, q->pos[2] };
    mover_init(q, q->pos, end, 5);
    q->mv.flags = 0;
}

static int on_anim_done(People *p, Person *q)
{
    const PeopleMonster *m = nearest_monster(p, q->pos);
    switch (q->kind) {
    case PK_WINDOW:
        return window_anim_done(p, q);
    case PK_WALKER:
        if (q->state == 4) {
            if (m) {
                emit(p, PE_WEAPON, who_of(p, q), group_str(group_sub(q->tmpl, "Weapon|RifleWeapon"), "WeaponName"),
                     NULL, m->pos, 0, 1);
                q->weapon_off = 0.25f;
                q->state = 5;
                start_anim(p, q, "ThrowRecover", 0, 0);
            }
        } else if (q->state == 5) {
            if (m) {
                emit(p, PE_WEAPON, who_of(p, q), NULL, NULL, m->pos, 0, 0);
                q->state = 3;
                start_anim(p, q, "Idle", 0, 0);
            }
        } else if (q->is_grenadier && q->state == 3 && m) {
            float dy = m->pos[1] - q->pos[1], dx = m->pos[0] - q->pos[0];
            float add = m->pos[1] > q->pos[1] ? 0 : PI;
            q->heading = (-atanf(dx / dy) + add) * DEG;
            /* abs(diffX < 12.0 && monsterPos.z < 10.0): a signed x, so anywhere to the left counts */
            if (dx < 12.0f && m->pos[2] < 10.0f && q->reload <= 0) {
                q->reload = tnum(q, "ReloadDelay", 5.0f);
                start_anim(p, q, "Throw", 0, 0);
                q->state = 4;
            } else {
                q->state = 1;
                start_anim(p, q, "Walk", 1, 1);
                walk(p, q);
            }
        }
        return 1;
    case PK_TOURIST:
        if (q->state == 1)
            decide(p, q);
        if (q->state == 3) {
            start_anim(p, q, "PickedUp2", 0, 1);
            q->pulse_delay = 10 * TIMESTEP;
            q->pulse_time = 0;
        }
        return 1;
    case PK_MIME:
        if (q->state == 1) {
            if (q->cur_anim <= 8 && q->reps < 2) {
                q->reps++;
            } else if (q->cur_anim <= 7 && q->reps >= 2) {
                q->reps = 0;
                q->cur_anim++;
            } else {
                q->cur_anim = 1;
                q->reps = 0;
            }
            mime_anim(p, q, 0);
            mime_facing(q);
            mime_mover(q);
        } else if (q->state == 4 || q->state == 5) {
            mime_anim(p, q, 0);
        }
        return 1;
    case PK_GUARD:
        if (q->state == 512) {
            if (q->reps > 0) {
                q->reps--;
                start_anim(p, q, "Perform", 0, 1);
            } else {
                start_anim(p, q, "Idle", 0, 1);
                q->idle_time = 2;
                q->state = 32;
            }
        }
        return 1;
    default:
        return 1;
    }
}

/* WalkingAroundPerson::walk: a stretch of up to WalkRange either way, kept within WalkRange of where it started,
 * at WalkSpeed, facing the way it goes */
static void walk(People *p, Person *q)
{
    float start[3] = { q->pos[0], q->pos[1], q->pos[2] }, end[3];
    float r = frand(p) + 0.01f, way = r > 0.5f ? -1.0f : 1.0f;
    if (fabsf(q->pos[0]) > 49.0f)
        way = start[0] > 0 ? 1.0f : -1.0f;
    memcpy(end, start, sizeof end);
    end[0] = start[0] + way * q->walk_range * r;
    if (end[0] - q->start[0] > q->walk_range) {
        end[0] = q->start[0] + 3.0f;
        if (end[0] == start[0])
            end[0] -= 0.01f;
    } else if (q->start[0] - end[0] > q->walk_range) {
        end[0] = q->start[0] - 3.0f;
        if (end[0] == start[0])
            end[0] += 0.01f;
    }
    q->heading = end[0] > start[0] ? -90.0f : 90.0f;
    mover_init(q, start, end, q->walk_speed);
    q->mv.flags = 0;
}

/* Tourist::decideFacingAndPhotoState: a monster within 15 along x has its photo taken after a moment */
static void decide(People *p, Person *q)
{
    if (q->is_child || q->state == 4)
        return;
    const PeopleMonster *m = mover_target(p, q->pos);   /* the mover's target (flag 8) */
    if (!m)
        return;
    if (fabsf(m->pos[0] - q->pos[0]) <= 15.0f) {
        q->pulse_delay = frand(p);
        q->state = 5;
    } else {
        q->pulse_delay = 0.25f;
        q->state = 2;
    }
}

static int fly_countdown(Person *q)
{
    q->fly_duration -= q->pulse_delay;
    return q->fly_duration < 0 ? DELETE_ME : 1;
}

static int on_pulse(People *p, Person *q)
{
    switch (q->kind) {
    case PK_WINDOW:
        return window_pulse(q);
    case PK_RUNNER:
        if (q->state == 16 && fly_countdown(q) == DELETE_ME)
            return DELETE_ME;
        if (strcmp(q->anim, "Streaking") == 0 && p->visible && !p->visible(p->visible_ctx, q->pos, 4.0f))
            return DELETE_ME;                        /* a streaker out of view */
        return 1;
    case PK_MIME:
        return q->state == 8 ? fly_countdown(q) : 1;
    case PK_BAND:
        if ((q->sub & 64) && p->band_flee) {
            emit(p, PE_SOUND, who_of(p, q), tstr(q, "Sound|BandSound"), NULL, q->pos, 0, 0);
            q->sub &= ~64;
        }
        if (q->state == 4 || q->state == 8)
            return 1;
        if (q->state == 16) {
            q->fly_duration -= 0.25f;
            return q->fly_duration < 0 ? DELETE_ME : 1;
        }
        if (p->band_flee) {                          /* the whole band runs off */
            q->state = 8;
            float dir = frand(p) > 0.5f ? 1.0f : -1.0f;
            start_anim(p, q, "Running", 1, 0);
            float end[3] = { q->pos[0] + dir * frand(p) * 100.0f, -10.0f - 50.0f * frand(p), 0 };
            mover_init(q, q->pos, end, 15);
            q->mv.flags = 0;
            q->heading = -90.0f * dir;               /* FACE_RIGHT x dir */
        }
        return 1;
    case PK_GUARD:
        if (q->state == 8)
            return fly_countdown(q);                 /* its flyDuration is never set: 0, so gone at the next pulse */
        if (q->state == 32 && (q->idle_time -= 1.0f) <= 0) {
            q->heading = 90.0f * q->march_dir;
            start_anim(p, q, "March", 0, 1);
            guard_march(q, -5.0f * q->march_dir, 0);
            q->state = 64;
        }
        return 1;
    case PK_WALKER:
        if (q->state == 0) {
            q->pulse_delay = q->is_grenadier ? 3 * TIMESTEP : 0;
            walk(p, q);
            q->state = 1;
            return 1;
        }
        if (q->state == 6)
            return fly_countdown(q);
        if (q->is_grenadier) {
            q->weapon_off -= q->pulse_delay;
            if (q->weapon_off <= 0 && q->weapon_off > -1000) {
                q->weapon_off = -1000;
                emit(p, PE_WEAPON, who_of(p, q), NULL, NULL, q->pos, 0, 0);
            }
            q->reload -= q->pulse_delay;
        }
        return 1;
    case PK_STATIONARY:
        if (q->state == 1)
            sound(p, q, "Sound|AmbientSound");
        else if (q->state == 2)
            return fly_countdown(q);
        return 1;
    case PK_TOURIST:
        if (q->state == 5) {
            start_anim(p, q, "Idle", 0, 1);
            q->state = 1;
            q->pulse_delay = 26 * TIMESTEP;
            q->pulse_time = 0;
        } else if (q->state == 1 || q->state == 3) {
            effect(p, q, "Effect|FlashEffect", q->pos, "Camera");
            q->pulse_delay = 100;
        } else if (q->state == 2) {
            q->pulse_time = 0;
            decide(p, q);
        } else if (q->state == 4) {
            return DELETE_ME;
        }
        return 1;
    default:
        return 1;
    }
}

static int on_mover(People *p, Person *q, int st)
{
    switch (q->kind) {
    case PK_WINDOW:
        return window_mover(p, q, st);
    case PK_RUNNER:
        if (st == 2)                                 /* it sets up another run across, then asks to be deleted */
            return DELETE_ME;
        if (st == 3 && strcmp(q->anim, "RunOut") == 0) {
            q->heading = q->pos[1] < -3.0f ? -90.0f : 90.0f;
        } else if (st == 3 && strcmp(q->anim, "StreakOut") == 0) {   /* out on the street: scurry off */
            q->heading = -90;
            start_anim(p, q, "Streaking", 0, 1);
            q->pulse_delay = 0.25f;
            if (q->pos[1] > -3.0f)                   /* going to -x while facing +x: the scurry played backwards */
                q->anim_reverse = 1;
        }
        return 1;
    case PK_MIME:
        if (st == 2 && (q->state == 4 || q->state == 5)) {
            q->state = 1;
            q->cur_anim = 1 + (int)(frand(p) * 7.995f);
            q->reps = 0;
            mime_anim(p, q, 1);
            mime_facing(q);
            mime_mover(q);
        }
        return 1;
    case PK_BAND:
        if (st == 2) {
            if (q->state == 8)
                return DELETE_ME;
            q->heading = 180;                        /* FACE_BACK */
            start_anim(p, q, "Playing", 1, 0);
            q->state = 2;
        }
        return 1;
    case PK_GUARD:
        if (st != 2)
            return 1;
        if (q->state == 4) {
            q->heading = -90.0f * q->march_dir;
            guard_march(q, 5.0f * q->march_dir, 0);
            q->state = 16;
        } else if (q->state == 16) {
            q->heading = 180;
            q->reps = 2;
            start_anim(p, q, "Perform", 0, 1);
            q->state = 512;
        } else if (q->state == 64) {
            q->heading = 0;
            guard_march(q, 0, 3);
            q->state = 128;
        } else if (q->state == 128) {
            if (!q->hut_destroyed)
                return DELETE_ME;
            q->heading = 90.0f * q->march_dir;
            guard_march(q, -80.0f * q->march_dir, 0);
            q->state = 256;
        } else if (q->state == 256) {
            return DELETE_ME;
        }
        return 1;
    case PK_CHILD:
        return st == 2 ? DELETE_ME : 1;
    case PK_WALKER:
        if (st == 2 && q->state == 1) {
            if (q->has_idle == 1) {
                start_anim(p, q, "Idle", 0, 0);
                q->mv.active = 0;                    /* setMover(an empty RWorldMover) */
                q->state = 3;
                const PeopleMonster *m = nearest_monster(p, q->pos);   /* lookAtMonster */
                if (m) {
                    float dx = m->pos[0] - q->pos[0];
                    if (dx < 5.0f && dx > -5.0f)
                        q->heading = m->pos[1] > q->pos[1] ? 0.0f : 180.0f;
                    else
                        q->heading = m->pos[0] > q->pos[0] ? -90.0f : 90.0f;
                }
            } else {
                walk(p, q);
            }
        }
        return 1;
    case PK_TOURIST:
        if (st == 2) {
            if (q->state == 6)
                return DELETE_ME;
            mover_init(q, q->pos, q->pos, 0);        /* stand, facing the monster */
            q->mv.flags = (q->mv.flags | 8 | 4) & ~1u;
            q->pulse_delay = 0.25f;
            q->state = 2;
        } else if (st == 4 && q->is_child && q->state != 6) {
            const PeopleMonster *m = nearest_monster(p, q->pos);
            float d[3] = { 0, 0, 0 };
            q->flags &= ~(uint32_t)OF_PUSH_OUT;
            if (m) {
                d[0] = q->pos[0] - m->pos[0];
                d[1] = q->pos[1] - m->pos[1];
                float l = sqrtf(d[0] * d[0] + d[1] * d[1]);
                if (l > 0) {
                    d[0] /= l;
                    d[1] /= l;
                }
            }
            float e[2] = { d[0] < 0 ? -d[1] : d[1], -fabsf(d[0]) };   /* away, across and towards the street */
            float end[3] = { q->pos[0] + 160 * e[0], q->pos[1] + 160 * e[1], q->pos[2] };
            mover_init(q, q->pos, end, 7);
            q->mv.flags = (q->mv.flags & ~8u) | 1;
            fx_off(p, q, PFX_SCRIPT);                /* BalloonFX.stop() */
            effect(p, q, "Effect|KidBalloonDetachFX", q->pos, NULL);
            start_anim(p, q, "Running", 0, 1);
            q->pulse_delay = 0;
            q->state = 6;
        }
        return 0;
    default:
        return 1;
    }
}

/* StationaryPerson::playEffect: its idleEffect stops, the PickedUpEffect 5 above it, the PickupSound */
static void play_effect(People *p, Person *q)
{
    float up[3] = { q->pos[0], q->pos[1], q->pos[2] + 5 };
    fx_off(p, q, PFX_SCRIPT);
    effect(p, q, "Effect|PickedUpEffect", up, NULL);
    sound(p, q, "Sound|PickupSound");
}

static void on_pickup(People *p, Person *q)          /* the script's onPickup (script event 8) */
{
    switch (q->kind) {
    case PK_WINDOW:
        if (q->state != 20) {
            start_anim(p, q, "PickedUp", 0, 1);
            sound(p, q, "Sound|PickupSound");
            q->state = 20;
        }
        break;
    case PK_RUNNER:
        start_anim(p, q, "PickedUp", 0, 1);
        sound(p, q, "Sound|PickupSound");
        q->heading = 0;
        break;
    case PK_WALKER:
        start_anim(p, q, "PickedUp", 0, 1);
        q->state = 2;
        sound(p, q, "Sound|Voice");
        q->sub |= 1;
        break;
    case PK_STATIONARY:
        play_effect(p, q);
        start_anim(p, q, "PickedUp", 0, 1);
        break;
    case PK_TOURIST:
        if (q->state != 3) {
            sound(p, q, "Sound|PickupSound");
            start_anim(p, q, "PickedUp2", 0, 1);
            q->state = 3;
            q->pulse_delay = 10 * TIMESTEP;
            q->pulse_time = 0;
        }
        break;
    case PK_CHILD:
        break;
    case PK_BAND:
        if (q->state != 4) {
            start_anim(p, q, "PickedUp", 1, 1);
            p->band_flee = 1;
            q->state = 4;
        }
        break;
    case PK_MIME:
    case PK_GUARD:
        start_anim(p, q, "PickedUp", 0, 1);
        q->state = 2;
        break;
    default:                                         /* no script: no onPickup */
        break;
    }
}

/* the script's smash(): monster.smashObject(actor), the FlyingSound, the state that counts the flight down */
static void smash(People *p, Person *q, float monster_heading)
{
    smash_object(p, q, monster_heading);
    sound(p, q, "Sound|FlyingSound");
    switch (q->kind) {
    case PK_WINDOW: q->state = 22; break;             /* pulses every 0.25 s */
    case PK_RUNNER: q->state = 16; q->pulse_delay = 0.5f; break;
    case PK_WALKER: q->state = 6; q->pulse_delay = 1.0f; break;
    case PK_STATIONARY: q->state = 2; q->pulse_delay = 1.0f; break;
    case PK_TOURIST: q->state = 4; q->pulse_delay = 3.0f; break;
    case PK_MIME: q->state = 8; q->pulse_delay = 1.0f; break;
    case PK_BAND: q->state = 16; break;              /* still pulsing every 0.25 s */
    case PK_GUARD: q->state = 8; break;              /* pulsing every 1 s */
    default: break;
    }
    /* [R] setPulseDelay only sets the delay (Actor binding 0x39: +0xF8): the pulse's clock runs on; Mime's smash()
     * alone also calls setPulseTime(0) */
    if (q->kind == PK_MIME)
        q->pulse_time = 0;
}

/* TakeDamage from a monster: who flies off, and when [R], each as its script has it. this.actor.onPickup() there is
 * the Actor binding (actor_on_pickup), so a person keeps the state its smash() gave it and its flight's countdown
 * deletes it (3 s for a window person, 3.5 a runner, 4 a walker, a mime ...) */
static int take_damage(People *p, Person *q, float monster_heading, const char *move, int who)
{
    switch (q->kind) {
    case PK_WINDOW:                                  /* only while it runs off after its fall (7) */
        if (q->state != 7)
            return 0;
        smash(p, q, monster_heading);
        actor_on_pickup(p, q);
        start_anim(p, q, "PickedUp", 0, 1);
        return 1;
    case PK_RUNNER:
        if (q->state == 16)
            return 0;
        smash(p, q, monster_heading);
        actor_on_pickup(p, q);
        start_anim(p, q, "PickedUp", 0, 1);
        return 1;
    case PK_WALKER:
        if (q->state == 6)
            return 0;
        smash(p, q, monster_heading);
        start_anim(p, q, "PickedUp", 0, 1);
        actor_on_pickup(p, q);
        return 1;
    case PK_STATIONARY:
        if (q->state == 2)
            return 0;
        play_effect(p, q);
        start_anim(p, q, "PickedUp", 0, 1);
        actor_on_pickup(p, q);
        smash(p, q, monster_heading);
        return 1;
    case PK_TOURIST:
        if (q->state == 4 || q->is_child)
            return 0;
        start_anim(p, q, "PickedUp", 0, 1);
        actor_on_pickup(p, q);
        smash(p, q, monster_heading);
        return 1;
    case PK_MIME:
    case PK_GUARD:
        if (q->state == 8)
            return 0;
        smash(p, q, monster_heading);
        actor_on_pickup(p, q);
        start_anim(p, q, "PickedUp", 0, 1);
        return 1;
    case PK_BAND:
        if (q->state == 16)
            return 0;
        smash(p, q, monster_heading);
        p->band_flee = 1;
        actor_on_pickup(p, q);
        start_anim(p, q, "PickedUp", 0, 1);
        return 1;
    case PK_CHILD:                                   /* a monster on the ground is thrown back by it */
        if (move && strncmp(move, "Ground", 6) == 0) {
            float v[3] = { 0, -20, 30 };
            emit(p, PE_MONSTER_KNOCK, who, NULL, NULL, v, 0, 1);
        }
        return 0;
    default:                                         /* no script: no TakeDamage */
        return 0;
    }
}

/* ---------------------------------------------------------------- construction */

static int script_kind(const char *script)
{
    static const struct { const char *name; int kind; } k[] = {
        { "Person_Window", PK_WINDOW }, { "Person_Runner", PK_RUNNER }, { "WalkingAroundPerson", PK_WALKER },
        { "StationaryPerson", PK_STATIONARY }, { "Tourist", PK_TOURIST }, { "Person_Child", PK_CHILD },
        { "Mime.", PK_MIME }, { "BandMember.", PK_BAND }, { "RoyalGuard.", PK_GUARD },
    };
    for (size_t i = 0; script && i < sizeof k / sizeof k[0]; i++)
        if (strncmp(script, k[i].name, strlen(k[i].name)) == 0)
            return k[i].kind;
    return PK_OTHER;
}

static Person *new_person(People *p, Group tmpl, const float pos[3], float heading)
{
    if (p->n == p->cap) {
        int cap = p->cap ? 2 * p->cap : 64;
        Person *v = realloc(p->v, sizeof *v * (size_t)cap);
        if (!v)
            return NULL;
        p->v = v;
        p->cap = cap;
    }
    Person *q = &p->v[p->n++];
    memset(q, 0, sizeof *q);
    q->tmpl = tmpl;
    q->name = tmpl.at ? group_name(tmpl) : "";
    q->kind = script_kind(tmpl.at ? group_str(tmpl, "Script|ScriptName") : NULL);
    memcpy(q->pos, pos, sizeof q->pos);
    q->heading = heading;
    q->building = q->storey = q->column = -1;
    q->pulse_delay = -1;                             /* [R] the Actor's +0xF8: -1, no pulses */
    q->flags = OF_ACTOR;                             /* [R] 0x30E3E0 */
    const char *flags = tstr(q, "Flags");            /* [R] the template's Flags (0x30DA24, names at 0x3F5F70) */
    if (flags && strstr(flags, "Pickupable"))
        q->flags |= OF_PICKUP;
    if (flags && strstr(flags, "PushOutMonsters"))
        q->flags |= OF_PUSH_OUT;
    const char *def = tstr(q, "DefaultAnim");        /* the native actor starts in its DefaultAnim */
    start_anim(p, q, def ? def : "Idle", 0, 0);
    /* [R] the world object's init (0x30D280): its AmbientEffect (the instance's, else the template's; no ActorInstance|
     * sets one) and, when its template is a goal of the block's challenge (0x3466A0), its ChallengeEffect, each
     * attached to it (0x291ED0(effect, actor, node -1): it follows the actor), kept at +0x108 / +0x110 */
    fx_on(p, q, PFX_AMBIENT, tstr(q, "Effect|AmbientEffect"), q->pos, 1);
    fx_on(p, q, PFX_CHALLENGE, tstr(q, "Effect|ChallengeEffect"), q->pos, 1);
    return q;
}

static void construct(People *p, Person *q)
{
    float up[3] = { q->pos[0], q->pos[1], q->pos[2] + 5 };
    q->fly_duration = 3;
    switch (q->kind) {
    case PK_WINDOW:
        q->state = 1;
        start_anim(p, q, "WindowEmerge", 0, 0);
        q->pulse_delay = 0.25f;
        q->flags |= OF_EFF_NODES;
        break;
    case PK_RUNNER:
        q->flags |= OF_PICKUP | OF_EFF_NODES;
        break;
    case PK_WALKER:
        q->flags |= OF_EFF_NODES;
        start_anim(p, q, "Walk", 1, 1);
        memcpy(q->start, q->pos, sizeof q->start);
        q->walk_range = tnum(q, "WalkRange", 5);
        q->has_idle = (int)tnum(q, "HasIdle", 0);
        q->is_grenadier = (int)tnum(q, "IsGrenadier", 0);
        q->walk_speed = tnum(q, "WalkSpeed", 4);
        q->weapon_off = -1000;
        q->pulse_delay = TIMESTEP;
        sound(p, q, "Sound|AmbientLoopingSound");
        q->state = 0;
        break;
    case PK_MIME:
        q->flags |= OF_EFF_NODES | OF_PICKUP;
        q->state = 4;
        memcpy(q->end_loc, q->pos, sizeof q->end_loc);
        q->stationary = (int)tnum(q, "Stationary", 1);
        q->cur_anim = 8;
        q->reps = 0;
        mime_anim(p, q, 1);
        {                                            /* a step of 0.001: its first mover callback comes at once */
            float end[3] = { q->pos[0] + 0.001f, q->pos[1], q->pos[2] };
            mover_init(q, q->pos, end, 1);
            q->mv.flags = 0;
        }
        mime_facing(q);
        break;
    case PK_BAND:
        p->band_flee = 0;                            /* the script's global, reset by every new member */
        q->state = 1;
        q->pulse_delay = 0.25f;
        q->flags |= OF_EFF_NODES | OF_PICKUP;
        if (tstr(q, "Sound|BandSound") && !p->band_sound) {   /* the band's music, once */
            emit(p, PE_SOUND, who_of(p, q), tstr(q, "Sound|BandSound"), NULL, q->pos, 0, 1);
            q->sub |= 64;
            p->band_sound = 1;
        }
        break;
    case PK_GUARD:
        q->flags |= OF_EFF_NODES | OF_PICKUP;
        q->heading = 180;
        start_anim(p, q, "March", 0, 1);
        q->pulse_delay = 1;
        q->march_dir = 1;
        q->hut = -1;
        q->fly_duration = 0;                         /* never set by the script */
        q->state = 4;
        break;
    case PK_STATIONARY:
        q->flags |= OF_EFF_NODES | OF_PICKUP;
        start_anim(p, q, "Idle", 1, 1);
        fx_on(p, q, PFX_SCRIPT, tstr(q, "Effect|IdleEffect"), up, 0);   /* idleEffect.createAndStart(pos + 5 up) */
        sound(p, q, "Sound|Sound");
        if (tstr(q, "Sound|AmbientSound"))
            q->pulse_delay = 7;
        q->state = 1;
        break;
    case PK_TOURIST:
        q->state = 2;
        q->pulse_delay = 2;
        mover_init(q, q->pos, q->pos, 0);
        q->mv.flags = 8 | 4;
        q->is_child = (int)tnum(q, "IsChild", 0);
        if (q->is_child) {
            float d = tnum(q, "MonsterResponseDist", 0);
            fx_on(p, q, PFX_SCRIPT, tstr(q, "Effect|KidBalloonHoldingFX"), q->pos, 1);   /* createAndStartAttached */
            q->mv.resp2 = d > 0 ? d * d : 0;
        } else {
            q->flags |= OF_EFF_NODES | OF_PICKUP;
            q->mv.resp2 = 0;                         /* setMonsterResponseDist(-1) */
        }
        break;
    default:
        break;
    }
    place_matrix(q->world, q->pos, q->heading, NULL);
}

/* ---------------------------------------------------------------- the API */

People *people_new(PeopleClipSeconds clip_seconds, void *ctx)
{
    People *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->clip_seconds = clip_seconds;
    p->ctx = ctx;
    p->droppable_rate = 0.5f;                        /* the blocks' DroppableDef rate */
    return p;
}

void people_free(People *p)
{
    if (p) {
        free(p->v);
        free(p);
    }
}

void people_set_droppable_rate(People *p, float rate) { p->droppable_rate = rate; }

int people_add(People *p, Group tmpl, const float pos[3], float heading_deg)
{
    Person *q = new_person(p, tmpl, pos, heading_deg);
    if (!q)
        return -1;
    construct(p, q);
    return who_of(p, q);
}

int people_add_window(People *p, Group tmpl, const float at[3], int building, int storey, int column)
{
    int who = people_add(p, tmpl, at, 0);            /* [R 0x318A44 .. 0x318B14] no facing is set: the identity */
    if (who >= 0) {
        p->v[who].building = building;
        p->v[who].storey = storey;
        p->v[who].column = column;
        p->v[who].win_x = at[0];
        p->v[who].win_floor = NAN;                   /* the first people_window_move sets it */
    }
    return who;
}

void people_window_move(People *p, int who, float floor_z, float dx)
{
    if (who < 0 || who >= p->n)
        return;
    Person *q = &p->v[who];
    if (q->kind != PK_WINDOW || q->phase != PP_SCRIPT || q->building < 0)
        return;
    if (isnan(q->win_floor))
        q->win_floor = floor_z;
    q->pos[2] += floor_z - q->win_floor;
    q->win_floor = floor_z;
    q->pos[0] = q->win_x + dx;
    place_matrix(q->world, q->pos, q->heading, NULL);
}

int people_crowd(People *p, const Group *tmpls, int ntmpls, const float centre[3])
{
    if (ntmpls <= 0)
        return 0;
    int n = 4 + (int)(frand(p) * 5.0f), made = 0;
    for (int i = 0; i < n; i++) {
        float s[3] = { centre[0] - 10 + frand(p) * 20, centre[1] - 5 + frand(p) * 10, centre[2] };
        float d[3] = { centre[0] - 75 + frand(p) * 150, centre[1] - 50, centre[2] };
        int pick = (int)(frand(p) * (float)ntmpls);
        int who = people_add(p, tmpls[pick < ntmpls ? pick : ntmpls - 1], s, heading_of(d[0] - s[0], d[1] - s[1]));
        if (who < 0)
            continue;
        Person *q = &p->v[who];
        start_anim(p, q, "Running", 1, 0);
        mover_init(q, s, d, 10);
        made++;
    }
    return made;
}

void people_set_path(People *p, int who, const float (*pts)[3], int n, float speed, uint32_t flags)
{
    if (who < 0 || who >= p->n || n < 1)
        return;
    Person *q = &p->v[who];
    mover_init(q, pts[0], n > 1 ? pts[1] : pts[0], speed);
    for (int i = 2; i < n; i++)
        mover_add(q, pts[i]);
    q->mv.flags = flags;
}

int people_count(const People *p) { return p->n; }
const Person *people_get(const People *p, int who) { return who >= 0 && who < p->n ? &p->v[who] : NULL; }

int people_object(const People *p, int o, float pos[3])
{
    if (o < 0 || o >= p->nobj || p->obj[o].state == 2)
        return 0;
    memcpy(pos, p->obj[o].pos, 3 * sizeof(float));
    return p->obj[o].made;
}

int people_events(People *p, PeopleEvent *out, int max)
{
    int n = p->nev < max ? p->nev : max;
    memcpy(out, p->ev, sizeof *out * (size_t)n);
    memmove(p->ev, p->ev + n, sizeof *p->ev * (size_t)(p->nev - n));
    p->nev -= n;
    return n;
}

void people_set_templates(People *p, PeopleFindTemplate find, void *ctx)
{
    p->find = find;
    p->find_ctx = ctx;
}

void people_set_visible(People *p, PeopleVisible visible, void *ctx)
{
    p->visible = visible;
    p->visible_ctx = ctx;
}

static Group template_ref(People *p, Group from, const char *key)   /* the template an OTRef| of from names */
{
    Group none = { NULL, 0 };
    const char *name = from.at ? group_str(from, key) : NULL;
    return name && p->find ? p->find(p->find_ctx, name) : none;
}

/* Band::createBand: BandMember1 .. n in files of three (the leader alone in front), 3.5 apart along x and 2 along y,
 * from x = 50 to the band's place, at 4, facing -x */
static void create_band(People *p, int o)
{
    float x = 0, y = 0;
    for (int i = 1; i < 10000; i++) {
        char key[32];
        snprintf(key, sizeof key, "OTRef|BandMember%d", i);
        if (!group_str(p->obj[o].t, key))
            break;
        Group t = template_ref(p, p->obj[o].t, key);
        if (!t.at)
            continue;
        if (i == 1) {
            y = 2;
        } else if ((i + 1) % 3 == 0) {
            y = 0;
            x += 3.5f;
        }
        y += 2;
        float start[3] = { 50 + x, -y, 0 }, end[3] = { p->obj[o].pos[0] + x, p->obj[o].pos[1] - y, 0 };
        int who = people_add(p, t, start, 90);
        if (who < 0)
            continue;
        Person *q = &p->v[who];
        start_anim(p, q, "Running", 1, 0);
        mover_init(q, start, end, 4);
        q->mv.flags = 0;
        q->heading = 90;
    }
}

/* RoyalGuardHut::createGuard: out of the hut's place, 3 towards -y at 5, marching along MarchDir */
static void create_guard(People *p, int o)
{
    Group t = template_ref(p, p->obj[o].t, "OTRef|Guard");
    if (!t.at)
        return;
    int who = people_add(p, t, p->obj[o].pos, 180);
    if (who < 0)
        return;
    Person *q = &p->v[who];
    start_anim(p, q, "March", 0, 0);
    q->march_dir = group_num(p->obj[o].t, "MarchDir", 1);   /* setMarchDirection */
    q->hut = o;
    float end[3] = { p->obj[o].pos[0], p->obj[o].pos[1] - 3, p->obj[o].pos[2] };
    mover_init(q, p->obj[o].pos, end, 5);
    q->mv.flags = 0;
    p->obj[o].guard = who;
}

/* ActorSwarm.bsf's getStringProp: the ScriptObject's template (ChildSwarm sets the keys typed) */
static const char *swarm_str(People *p, int o, const char *type, const char *key)
{
    char typed[64];
    snprintf(typed, sizeof typed, "%s%s", type, key);
    const char *s = group_str(p->obj[o].t, typed);
    return s ? s : group_str(p->obj[o].t, key);
}

static void swarm_sound(People *p, int o, const char *key)   /* add3DSfx(getStringProp(key), 0) at its place */
{
    const char *s = swarm_str(p, o, "Sound|", key);
    if (s && p->nev < MAX_EVENTS) {                  /* on the swarm: it goes with it (deleted at its second pulse) */
        emit(p, PE_SOUND, -1, s, NULL, p->obj[o].pos, 0, 1);
        p->ev[p->nev - 1].object = o;
    }
}

/* ActorSwarm::onPulse in its state 0 [R ActorSwarm.scr]: the Message in MessageColor (default set255(220, 0, 0, 255);
 * getColorProp as edible.c's), RecessSound, SwarmSound; then 12 + frand x 8 actors, each of a random one of its
 * Actor0 .. n, at its place +- 5 in x and y, running (startAnimAtRandomOffset("Running", 0)) on a LineMover at 11.5
 * (MonsterResponseDist 0) to a middle point (x pushed 0 .. 5 further from the world's x = 0, y - 8 - 0 .. 5), then to
 * (its x - 45 + 90 frand, its y - 50), flags 1 | 4, facing that way. Person_Child deletes each at its mover's end */
static void swarm_out(People *p, int o)
{
    const float *pos = p->obj[o].pos;
    const char *message = swarm_str(p, o, "", "Message");
    if (message) {
        float c[4] = { 220.0f / 255, 0, 0, 1 };
        Group col = group_sub(p->obj[o].t, "Color|MessageColor");
        if (!col.at)
            col = group_sub(p->obj[o].t, "MessageColor");
        Record r[4];
        uint32_t nc = col.at ? group_items(col, r, 4) : 0;
        for (uint32_t i = 0; nc >= 3 && i < nc && i < 4; i++)
            c[i] = (float)((int)record_num(r[i]) & 0xFF) / 255.0f;
        emit(p, PE_MESSAGE, -1, message, NULL, c, c[3], 1);
    }
    swarm_sound(p, o, "RecessSound");
    swarm_sound(p, o, "SwarmSound");
    const char *names[32];
    int n = 0;
    for (int i = 0; n < 32; i++) {
        char key[32];
        snprintf(key, sizeof key, "Actor%d", i);
        const char *name = swarm_str(p, o, "OTRef|", key);
        if (!name)
            break;
        names[n++] = name;
    }
    if (n == 0)
        return;
    int people = 12 + (int)(frand(p) * 8.0f);
    for (int i = 0; i < people; i++) {
        float s[3] = { pos[0], pos[1], pos[2] };
        s[0] += -5.0f + frand(p) * 10.0f;
        s[1] += -5.0f + frand(p) * 10.0f;
        float m[3] = { s[0], s[1], s[2] };
        if (m[0] < 0)
            m[0] -= frand(p) * 5.0f;
        else
            m[0] += frand(p) * 5.0f;
        m[1] = m[1] - 8.0f + frand(p) * -5.0f;
        float d[3] = { pos[0], pos[1], pos[2] };
        d[0] += -45.0f + frand(p) * 90.0f;
        d[1] -= 50.0f;
        int pick = (int)(frand(p) * (float)n);
        Group t = p->find ? p->find(p->find_ctx, names[pick < n ? pick : n - 1]) : (Group){ NULL, 0 };
        if (!t.at)
            continue;
        int who = people_add(p, t, s, 0);
        if (who < 0)
            continue;
        Person *q = &p->v[who];
        start_anim(p, q, "Running", 1, 0);
        mover_init(q, s, m, 11.5f);
        mover_add(q, d);
        q->mv.flags = 1 | 4;
        q->mv.resp2 = 0;
        q->heading = heading_of(m[0] - s[0], m[1] - s[1]);   /* getMovementFacing, its z only */
        place_matrix(q->world, q->pos, q->heading, NULL);
    }
}

static void objects_update(People *p, float dt)
{
    for (int o = 0; o < p->nobj; o++) {
        if (p->obj[o].state == 2)
            continue;
        if (p->obj[o].swarm) {                       /* ActorSwarm: onInit set a 2 s pulse */
            if ((p->obj[o].timer -= dt) <= 0) {
                p->obj[o].timer = 2;
                if (p->obj[o].state == 0) {
                    swarm_out(p, o);
                    p->obj[o].state = 1;
                } else {
                    p->obj[o].state = 2;             /* its next returns 4 | 8 | 16: deleted */
                }
            }
            continue;
        }
        if (!p->obj[o].hut) {                        /* Band: onInit sets a 0.25 s pulse, which makes the band */
            if ((p->obj[o].timer -= dt) <= 0) {
                create_band(p, o);
                p->obj[o].state = 2;                 /* then it asks to be deleted */
            }
            continue;
        }
        if ((p->obj[o].timer += dt) < 1)             /* the hut pulses every second */
            continue;
        p->obj[o].timer -= 1;
        if (p->obj[o].state == 1) {                  /* hit: its deathLength (never set, 0) runs out at once */
            p->obj[o].state = 2;
            continue;
        }
        if ((p->obj[o].create_time -= 1) <= 0) {
            int g = p->obj[o].guard;
            if (g >= 0 && g < p->n && p->v[g].phase != PP_GONE) {
                p->obj[o].create_time = 6;
            } else {
                create_guard(p, o);
                p->obj[o].create_time = 15;
            }
        }
    }
}

void people_add_band(People *p, Group band, const float dest[3])
{
    if (p->nobj >= MAX_OBJECTS)
        return;
    int o = p->nobj++;
    memset(&p->obj[o], 0, sizeof p->obj[o]);
    p->obj[o].made = ++p->nmade;
    p->obj[o].t = band;
    memcpy(p->obj[o].pos, dest, sizeof p->obj[o].pos);
    p->obj[o].timer = 0.25f;
    p->obj[o].guard = -1;
}

void people_add_swarm(People *p, Group swarm, const float pos[3])
{
    int o = 0;
    while (o < p->nobj && !(p->obj[o].swarm && p->obj[o].state == 2))   /* a deleted swarm's place, else a new one */
        o++;
    if (o == p->nobj) {
        if (p->nobj >= MAX_OBJECTS)
            return;
        p->nobj++;
    }
    memset(&p->obj[o], 0, sizeof p->obj[o]);
    p->obj[o].made = ++p->nmade;
    p->obj[o].swarm = 1;
    p->obj[o].t = swarm;
    memcpy(p->obj[o].pos, pos, sizeof p->obj[o].pos);   /* setPoint */
    p->obj[o].timer = 2;                             /* onInit: BellSound, state 0, setPulseDelay(2.0) */
    p->obj[o].guard = -1;
    swarm_sound(p, o, "BellSound");
}

int people_add_hut(People *p, Group hut, const float pos[3])
{
    if (p->nobj >= MAX_OBJECTS)
        return -1;
    int o = p->nobj++;
    memset(&p->obj[o], 0, sizeof p->obj[o]);
    p->obj[o].made = ++p->nmade;
    p->obj[o].hut = 1;
    p->obj[o].t = hut;
    memcpy(p->obj[o].pos, pos, sizeof p->obj[o].pos);
    p->obj[o].create_time = 4;
    p->obj[o].guard = -1;
    return o;
}

PeopleEaten people_hut_hit(People *p, int hut, const float direction[3], float vel[3])
{
    PeopleEaten e = { 0, 0, 0, 0 };
    if (vel)
        vel[0] = vel[1] = vel[2] = 0;
    if (hut < 0 || hut >= p->nobj || !p->obj[hut].hut || p->obj[hut].state != 0)
        return e;
    if (vel) {                                       /* a SphereMover at -direction (x, y) x 25 */
        vel[0] = -direction[0] * 25;
        vel[1] = -direction[1] * 25;
        vel[2] = direction[2] * 25;
    }
    p->obj[hut].state = 1;
    int g = p->obj[hut].guard;
    if (g >= 0 && g < p->n && p->v[g].phase != PP_GONE)
        p->v[g].hut_destroyed = 1;                   /* eventHutDestroyed */
    Group t = p->obj[hut].t;                         /* gCommon.addStats(hut, slot, 0) */
    e.health = t.at ? group_num(t, "Health", 0) : 0;
    e.energy = t.at ? group_num(t, "Energy", 0) : 0;
    e.points = t.at ? group_num(t, "Points", 0) : 0;
    return e;
}

int people_hut_state(const People *p, int hut)
{
    return hut >= 0 && hut < p->nobj ? p->obj[hut].state : 2;
}

int people_add_streaker(People *p, Group tmpl, const float monster_pos[3], float camera_x)
{
    float dir = camera_x > monster_pos[0] ? -1.0f : 1.0f;   /* away from the camera's side */
    float start[3] = { monster_pos[0], monster_pos[1], monster_pos[2] };
    float out[3] = { start[0], -dir * 1.0f - 3.0f, 0 }, end[3] = { dir * 100.0f, out[1], 0 };
    int who = people_add(p, tmpl, start, -90);
    if (who < 0)
        return -1;
    Person *q = &p->v[who];
    start_anim(p, q, "StreakOut", 1, 0);
    mover_init(q, start, out, 2.241f);
    mover_add(q, end);
    q->mv.flags = 0;
    q->heading = -90;
    return who;
}

static void gone(People *p, Person *q)
{
    /* its effects end with it: the Actor's destructor shuts down its AmbientEffect (+0x108) and ChallengeEffect (+0x110)
     * (vtable +0x30, 0x30E128 .. 0x30E1DC) [R]; the scripts stop their own first: the IdleEffect in playEffect (onPickup,
     * TakeDamage: StationaryPerson.scr 92), the kid's BalloonFX as it runs off (onMoverCallback, Tourist.scr 288; a kid
     * cannot be picked up) [R]. [G: a kid deleted another way would leave its balloon running where it stood (not
     * attached any more, setTurnOffWhenObjectDies not called); here it stops] */
    for (int k = PFX_AMBIENT; k <= PFX_SCRIPT; k++)
        fx_off(p, q, k);
    q->phase = PP_GONE;
    q->flags |= OF_DYING;                            /* requestDelete (vtable +0x58, 0x285DD0) */
    q->mv.active = 0;
    q->building = -1;
    /* [R] the Actor destructor (0x30DF80): in play (0x4D6A80 is 6, a block current), each SpawnsItem<n> of its
     * instance (n = 0, 1, ... while there is one) is made from the block and sent onWalkIn (0x30E114); made after this
     * frame's update here (people_spawn_chain), the list of people being walked */
    if (!q->inst.at || !p->block.at || p->not_in_play)
        return;
    for (int n = 0; n < 16 && p->nspawn < 16; n++) {
        char key[24], name[96];
        snprintf(key, sizeof key, "SpawnsItem%d", n);
        const char *next = group_str(q->inst, key);
        if (!next)
            break;
        snprintf(name, sizeof name, "ActorInstance|%s", next);
        Group g = group_sub(p->block, name);
        if (g.at)
            p->spawn[p->nspawn++] = g;
    }
    q->inst = (Group){ 0 };                          /* (once) */
}

/* the script's onWalkIn [R WalkingAroundPerson.bsf, Mime.bsf, Tourist.bsf; the others have none] */
static void walk_in(People *p, Person *q)
{
    float delayed = q->inst.at ? group_num(q->inst, "DelayedSpawn", tnum(q, "DelayedSpawn", 0)) : tnum(q, "DelayedSpawn", 0);
    switch (q->kind) {
    case PK_WALKER: {                                /* from x 80 off the side it is on, facing in; its walks keep to
                                                      * its starting place, so it walks back there */
        float pos[3] = { q->pos[0] > 0 ? 80.0f : -80.0f, q->pos[1], q->pos[2] };
        memcpy(q->pos, pos, sizeof q->pos);
        q->heading = pos[0] > 0 ? 90.0f : -90.0f;    /* FACE_LEFT, FACE_RIGHT */
        if (delayed > 0)
            q->pulse_delay = delayed;
        q->state = 0;
        break;
    }
    case PK_MIME: {                                  /* from x -80 to its place at 35 x the step, in Anim8 (state 5) */
        float target[3], start[3];
        memcpy(target, q->pos, sizeof target);
        start[0] = -80;
        start[1] = target[1];
        start[2] = target[2];
        q->cur_anim = 8;
        q->reps = 0;
        mime_anim(p, q, 1);
        mover_init(q, start, target, 35 * TIMESTEP);
        q->mv.flags = 0;
        q->state = 5;
        break;
    }
    case PK_TOURIST: {                               /* running in from 100 past its Location, at the street's edge */
        float end[3], start[3] = { 0, -3, 0 };
        memcpy(end, q->pos, sizeof end);
        start[0] = end[0] < 0 ? -100 + end[0] : 100 + end[0];
        float vel = q->inst.at ? group_num(q->inst, "Velocity", tnum(q, "Velocity", 6)) : tnum(q, "Velocity", 6);
        mover_init(q, start, end, vel);
        q->mv.flags = (q->mv.flags | 1 | 4) & ~8u;
        start_anim(p, q, "Running", 1, 0);
        q->state = 0;
        break;
    }
    default:
        break;
    }
}

static void people_spawn_chain(People *p)
{
    for (int i = 0; i < p->nspawn; i++) {
        Group g = p->spawn[i];
        const char *name = group_str(g, "Name");
        Group t = name && p->find ? p->find(p->find_ctx, name) : (Group){ 0 };
        if (!t.at)
            continue;
        Group loc = group_sub(g, "Location");
        Record r[3];
        uint32_t n = loc.at ? group_items(loc, r, 3) : 0;
        float xyz[3];
        for (uint32_t k = 0; k < 3; k++)
            xyz[k] = k < n ? record_num(r[k]) : 0.0f;
        int who = people_add(p, t, xyz, group_num(g, "ZRotation", 0));
        if (who < 0)
            continue;
        p->v[who].inst = g;
        walk_in(p, &p->v[who]);
        place_matrix(p->v[who].world, p->v[who].pos, p->v[who].heading, NULL);
    }
    p->nspawn = 0;
}

void people_set_block(People *p, Group block) { if (p) p->block = block; }
void people_set_instance(People *p, int who, Group inst) { if (p && who >= 0 && who < p->n) p->v[who].inst = inst; }
void people_set_world_state(People *p, int state) { if (p) p->not_in_play = state != 6; }

void people_remove(People *p, int who)
{
    if (who >= 0 && who < p->n && p->v[who].phase != PP_GONE)
        gone(p, &p->v[who]);
}

void people_update(People *p, float dt, const PeopleMonster *m, int nm)
{
    p->m = m;
    p->nm = nm;
    people_spawn_chain(p);                           /* the ones eaten since the last frame (people_remove) */
    for (int i = 0; i < p->ntimers; i++)
        if ((p->timer[i].left -= dt) <= 0) {
            if (p->nev < MAX_EVENTS)
                p->ev[p->nev++] = p->timer[i].ev;
            p->timer[i--] = p->timer[--p->ntimers];
        }
    objects_update(p, dt);
    for (int i = 0; i < p->n; i++) {
        Person *q = &p->v[i];
        if (q->phase == PP_GONE)
            continue;
        int r = 1;
        /* the anim: a play that does not repeat ends in onAnimComplete */
        if (q->anim_loop) {
            if (q->anim_len > 0) {
                q->anim_time = fmodf(q->anim_time + (q->anim_reverse ? -dt : dt), q->anim_len);
                if (q->anim_time < 0)
                    q->anim_time += q->anim_len;
            }
        } else if (!q->anim_done) {
            q->anim_time += dt;
            if (q->anim_time >= q->anim_len) {
                q->anim_time = q->anim_len;
                q->anim_done = 1;                    /* a handler's startAnim clears it */
                r = on_anim_done(p, q);
            }
        }
        /* [R 0x30CEA0] the pulse's clock (+0xFC) runs only with a delay (+0xF8) of 0.01 or more; at the delay it is
         * taken off and onPulse runs, once a frame. setPulseDelay(0) stops the pulses */
        if (r != DELETE_ME && q->pulse_delay >= 0.01f && q->phase != PP_GONE) {
            q->pulse_time += dt;
            if (q->pulse_time >= q->pulse_delay) {
                q->pulse_time = q->pulse_delay > 0 ? q->pulse_time - q->pulse_delay : 0;
                r = on_pulse(p, q);
            }
        }
        if (r != DELETE_ME && q->phase == PP_SCRIPT) {
            int st = mover_step(p, q, dt);
            if (st >= 2)
                r = on_mover(p, q, st);
            place_matrix(q->world, q->pos, q->heading, NULL);
        } else if (r != DELETE_ME && q->phase == PP_FLYING) {
            /* [R] the SpherePhysicsMover's frame (0x28E440, collide.c): sub-steps, gravity, the street's push and
             * impulse (restitution and friction, the spin); the people scripts have no OnHitGround. [G: the objects
             * of the frame's collision world it would meet (0x287430) are left out: people are not in the viewer's
             * collision world] */
            collide_mover_step(NULL, &q->cm, q->cmat, -1, dt, NULL, NULL);
            collide_matrix_to16(q->cmat, q->world);
            memcpy(q->pos, q->cmat + 9, sizeof q->pos);
            memcpy(q->vel, q->cm.vel, sizeof q->vel);
        }
        if (r == DELETE_ME)
            gone(p, q);
    }
    people_spawn_chain(p);                           /* the chains of the ones gone this frame (or since the last) */
}

void people_window_fall(People *p, int who)
{
    if (who < 0 || who >= p->n)
        return;
    Person *q = &p->v[who];
    if (q->phase == PP_SCRIPT && q->kind == PK_WINDOW && q->building >= 0)
        window_fall(p, q);
}

/* ---------------------------------------------------------------- the monster's grab */

int people_probe(const PeopleBox *b, int nb, const float monster_pos[3], const float xyz[3], int mirrored,
                 int *building, int *storey, int *column)
{
    float pt[3] = { monster_pos[0] + (mirrored ? -xyz[0] : xyz[0]), monster_pos[1] + xyz[1] - 1.0f,
                    monster_pos[2] + xyz[2] };
    int best = -1;
    float bd = 0;
    for (int i = 0; i < nb; i++) {                   /* 0x2A3D20 looking along (0, 1, 0): the first front ahead */
        if (b[i].gone || !(b[i].min[0] <= pt[0]) || b[i].max[0] < pt[0])
            continue;
        float d = b[i].min[1] - pt[1];
        if (d < 0)
            continue;
        if (best < 0 || d < bd) {
            best = i;
            bd = d;
        }
    }
    if (best < 0)
        return 0;
    int col = (int)((pt[0] - b[best].min[0]) / 5.0f);   /* 0x2B3900: truncated, as cvt.w.s does */
    int sto = (int)((pt[2] - b[best].min[2] - 6.0f) / 3.0f);
    if (col < 0 || col >= b[best].width || sto < 0)
        return 0;
    *building = best;
    *storey = sto;
    *column = col;
    return 1;
}

int people_window_at(const People *p, int building, int storey, int column)
{
    for (int i = 0; i < p->n; i++) {
        const Person *q = &p->v[i];
        if (q->phase == PP_SCRIPT && q->building >= 0 && q->building == building && q->storey == storey &&
            q->column == column)
            return i;
    }
    return -1;
}

int people_window_retired(const People *p, int building, int storey, int column)
{
    for (int i = 0; i < p->nretired; i++)
        if (p->retired[i].building == building && p->retired[i].storey == storey && p->retired[i].column == column)
            return 1;
    return 0;
}

int people_can_grab(const People *p, const PeopleBox *b, int nb, const float monster_pos[3], const float xyz[3],
                    int mirrored)
{
    int bi, st, col;
    if (!people_probe(b, nb, monster_pos, xyz, mirrored, &bi, &st, &col) || !b[bi].live || st >= b[bi].n)
        return -1;
    return people_window_at(p, bi, b[bi].live[st], col);   /* the probe's storey is a standing one: its id */
}

int people_nearest(const People *p, const float hand[3], const float root[3], const float forward[3], float radius)
{
    int best = -1;
    float bd = 3.4e38f;
    for (int i = 0; i < p->n; i++) {
        const Person *q = &p->v[i];
        if (q->phase == PP_GONE || !(q->flags & OF_PICKUP) || (q->flags & (OF_HELD | OF_DYING | 0x40)))
            continue;
        const float *at = &q->world[12];
        float dx = at[0] - hand[0], dy = at[1] - hand[1], dz = at[2] - hand[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d > radius * radius || d >= bd)
            continue;
        float v[3] = { at[0] - root[0], at[1] - root[1], 0 };   /* in front of the root: (x, y) and its z dropped */
        if (v[0] * forward[0] + v[1] * forward[1] + v[2] * forward[2] <= 0)
            continue;
        best = i;
        bd = d;
    }
    return best;
}

void people_grab(People *p, int who, float blend)
{
    if (who < 0 || who >= p->n || p->v[who].phase == PP_GONE)
        return;
    Person *q = &p->v[who];
    q->flags = (q->flags | OF_HELD) & ~(uint32_t)(OF_PICKUP | OF_PUSH_OUT);   /* 0x2DA310 */
    q->phase = PP_HELD;
    q->mv.active = 0;                                /* its mover becomes the HeldObjectMover */
    q->blend = blend > 0.001f ? blend : 0;
    q->blend_left = q->blend;
    mat_quat(q->world, q->from_q);
    memcpy(q->from_p, &q->world[12], sizeof q->from_p);
    q->off_set = 0;
    actor_on_pickup(p, q);                           /* 0x2B344C / 0x2DA45C: 0x30C5E0, its effects stop */
    if (q->building >= 0 && p->nretired < 64) {      /* grabbed from its window: the slot is retired */
        p->retired[p->nretired].building = q->building;
        p->retired[p->nretired].storey = q->storey;
        p->retired[p->nretired++].column = q->column;
    }
    on_pickup(p, q);                                 /* script event 8 */
    leave_window(q);                                 /* 0x318140: the occupant record lets it go */
}

/* [R] HeldObjectMover (init 0x310ED0, held 0x3109E0): the object's MajorAxis (default 2, z) along the hand's z, the
 * next one ((MajorAxis + 1) % 3) along the hand's y, the last their cross product; at the hand's place plus
 * HoldOffset (MirroredHoldOffset when the monster plays mirrored) in the hand's frame. [R] The init keeps where it was
 * in that frame (its turn +0x68 and place +0x78, from the hand at the grab); while the blend lasts (+0x84 the time
 * left, +0x88 one over the blend) 0x310B50 takes w = 1 - left / blend, turns that offset w of the way to none
 * (0x104C60) and scales its place by 1 - w, on the hold's frame: linear in time, and it follows the hand. */
void people_hold(People *p, int who, const float hand[16], int mirrored, float dt)
{
    if (who < 0 || who >= p->n || p->v[who].phase != PP_HELD)
        return;
    Person *q = &p->v[who];
    int major = (int)tnum(q, "MajorAxis", 2);
    if (major < 0 || major > 2)
        major = 2;
    int i1 = (major + 1) % 3, i2 = (major + 2) % 3;
    const float *hz = hand + 8, *hy = hand + 4;
    float axis[3][3];
    memcpy(axis[major], hz, sizeof axis[0]);
    memcpy(axis[i1], hy, sizeof axis[0]);
    axis[i2][0] = axis[major][1] * axis[i1][2] - axis[major][2] * axis[i1][1];
    axis[i2][1] = axis[major][2] * axis[i1][0] - axis[major][0] * axis[i1][2];
    axis[i2][2] = axis[major][0] * axis[i1][1] - axis[major][1] * axis[i1][0];
    float off[3] = { 0, 0, 0 };
    Group og = q->tmpl.at ? group_sub(q->tmpl, mirrored ? "MirroredHoldOffset" : "HoldOffset") : q->tmpl;
    if (og.at) {
        Record r[3];
        if (group_items(og, r, 3) == 3)
            for (int k = 0; k < 3; k++)
                off[k] = record_num(r[k]);
    }
    float t[16];
    for (int c = 0; c < 3; c++) {
        for (int k = 0; k < 3; k++)
            t[4 * c + k] = axis[c][k];
        t[4 * c + 3] = 0;
    }
    for (int k = 0; k < 3; k++)
        t[12 + k] = hand[12 + k] + off[0] * hand[k] + off[1] * hand[4 + k] + off[2] * hand[8 + k];
    t[15] = 1;
    if (q->blend_left > 0) {
        float tq[4], oq[4], bq[4];
        static const float none[4] = { 0, 0, 0, 1 };
        mat_quat(t, tq);
        if (!q->off_set) {                           /* 0x310ED0: the offset from the hold's frame */
            float c[4] = { -tq[0], -tq[1], -tq[2], tq[3] }, d[3];
            quat_mul(c, q->from_q, q->off_q);
            for (int k = 0; k < 3; k++)
                d[k] = q->from_p[k] - t[12 + k];
            for (int k = 0; k < 3; k++)              /* R_t^T d */
                q->off_p[k] = t[4 * k] * d[0] + t[4 * k + 1] * d[1] + t[4 * k + 2] * d[2];
            q->off_set = 1;
        }
        q->blend_left -= dt;
        float w = q->blend_left > 0 ? 1 - q->blend_left / q->blend : 1;
        slerp(q->off_q, none, w, bq);
        quat_mul(tq, bq, oq);
        quat_mat(oq, q->world);
        for (int k = 0; k < 3; k++)
            q->world[12 + k] = t[12 + k] + (1 - w) * (t[k] * q->off_p[0] + t[4 + k] * q->off_p[1] + t[8 + k] * q->off_p[2]);
    } else {
        memcpy(q->world, t, sizeof t);
    }
    memcpy(q->pos, &q->world[12], sizeof q->pos);
}

PeopleEaten people_eat(People *p, int who)
{
    PeopleEaten e = { 0, 0, 0, 0 };
    if (who < 0 || who >= p->n || p->v[who].phase != PP_HELD)
        return e;
    Person *q = &p->v[who];
    if (!(q->kind == PK_WALKER && q->thrown)) {      /* the scripts' onThrow(monster, 1) */
        q->thrown = 1;
        if (!(q->flags & OF_COUNTED)) {              /* gCommon.addStats(actor, slot, 1), once per object */
            e.health = tnum(q, "Health", 0);
            e.energy = tnum(q, "Energy", 0);
            e.points = tnum(q, "Points", 0);
            q->flags |= OF_COUNTED;
        }
        if (q->kind == PK_STATIONARY) {
            const char *fx = tstr(q, "Effect|EatEffect"), *node = tstr(q, "EatEffectNode");
            if (fx && node) {                        /* on the monster's node: its name + "1" */
                e.eat_fx = fx;
                snprintf(e.eat_node, sizeof e.eat_node, "%s1", node);
            }
            const char *reaction = tstr(q, "ReactionType");
            if (reaction && same_text(reaction, "Priest")) {
                /* Monsters::doPriestReaction after EatReactionDelay: thrown up and back, 30 HP */
                float v[3] = { frand(p) < 0.5f ? -10.0f : 10.0f, -5, 20 };
                float delay = tnum(q, "EatReactionDelay", 0);
                later(p, delay, PE_MONSTER_KNOCK, v, 0);
                later(p, delay, PE_MONSTER_HP, NULL, -30);
                e.priest = 1;
            }
        }
        e.fart = fart_maybe(p);
    }
    gone(p, q);                                      /* 0x2D9DC0: an eaten actor is deleted */
    return e;
}

void people_apply_eaten(const PeopleEaten *e, float *hp, float hp_max, float *energy)
{
    if (hp && *hp > 0) {                             /* 0x2C51A0 */
        float v = *hp + e->health;
        v = v > 1 ? v : 1;
        *hp = v < hp_max ? v : hp_max;
    }
    if (energy) {
        float v = *energy + e->energy;
        *energy = v < 0 ? 0 : v > 100 ? 100 : v;
    }
}

void people_throw(People *p, int who, float monster_heading)
{
    if (who < 0 || who >= p->n || p->v[who].phase != PP_HELD)
        return;
    Person *q = &p->v[who];
    if (q->kind == PK_WALKER && q->thrown) {         /* bThrownProcessed: it only lets go */
        q->flags &= ~(uint32_t)OF_HELD;
        q->phase = PP_SCRIPT;
        return;
    }
    q->thrown = 1;
    float clock = q->pulse_time;
    smash(p, q, monster_heading);                    /* onThrow(monster, 0) */
    if (q->kind == PK_MIME)                          /* Mime's onThrow smashes without its smash(): no setPulseTime */
        q->pulse_time = clock;
}

int people_hit(People *p, int monster, const float c[3], float radius, float monster_heading, const char *move)
{
    int n = 0;
    p->hitter = monster;
    for (int i = 0; i < p->n; i++) {
        Person *q = &p->v[i];
        if (q->phase != PP_SCRIPT)
            continue;
        /* the person's sphere: CollisionRadius (DefActorProps: 2) [R 0x28FD70: about the model's bounding sphere's
         * centre; G: taken 1 up, as people are not in the viewer's collision world (the game's test is the monster
         * against the objects, 0x2E22B0, an armed sphere's blow on an OnlyHitWithEffNodes person)] */
        float rr = tnum(q, "CollisionRadius", 2) + radius;
        float dx = q->pos[0] - c[0], dy = q->pos[1] - c[1], dz = q->pos[2] + 1 - c[2];
        if (dx * dx + dy * dy + dz * dz <= rr * rr)
            n += take_damage(p, q, monster_heading, move, -1);
    }
    p->hitter = 0;
    return n;
}
