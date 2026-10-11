/* army.c - the army, its weapons, the droppables and the score (army.h; docs/ARMY_NOTES.md).
 * [R] marks what is read from the game's code or scripts, [G] what is a guess or a stand-in. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "army.h"
#include "wrand.h"

#define PI 3.14159265f
#define TIMESTEP (1.0f / 30.0f)                      /* gWorld.getTimeStep(): its step, 1/30 [R 0x2B7800] */
#define DELETE_ME (4 | 8 | 16)                       /* [R] what a script handler returns to have its object deleted */
#define MAX_EVENTS 512
#define MAX_SHOTS 128
#define MAX_SPAWN 32
#define MAX_PERSONS 16
#define MAX_BUILDINGS 64
#define NOT_FOUND "StringNotFound"

/* object flags (+0x4C), names from the table at 0x3F5F70 [R] */
enum { OF_MONSTER = 0x40, OF_PUSH_OUT = 0x4000, OF_EFF_NODES = 0x8000, OF_PICKUP = 0x10000, OF_HELD = 0x20000,
       OF_ALPHA = 0x8000000 };

typedef struct {                                     /* an AIInstance record, 0x24 bytes (0x335320, 0x3350F0) [R] */
    Group g;                                         /* +0 */
    const char *name;                                /* +4 OTRef|Name */
    float min_delay, respawn, offset;                /* +8, +C, +10 */
    int left;                                        /* +14 MaxOccurances: spawns left */
    float timer;                                     /* +18 */
    int alive, enabled;                              /* +1C, +20 */
} Spawner;

typedef struct { int who, on, monster; float world[16]; ArmyWeapon w; int has; } PersonGun;   /* who: person + 1, 0 free */
typedef struct { float step, sum; int skip; } Chain; /* a building's +0x194, +0x198, +0x19C [R] */

struct Army {
    ArmyHost host;
    ArmyUnit *u; int n, cap;
    ArmyShot shot[MAX_SHOTS]; int nshots, next_shot;
    ArmyEvent ev[MAX_EVENTS]; int nev, next_fx;
    Spawner sp[MAX_SPAWN]; int nsp, active;
    const char *saved_name;                          /* gAIMgr +0x54 */
    /* Swat.bsf's globals, per GroupIndex */
    int flee[4], instances[4], fallback_played[4], fire_played[4], move_played[4];
    float fallback_ts, fire_ts, move_ts, emerge_ts, now;
    int window_first, window_boss;                   /* Military_Window.bsf's isFirst, makeBoss */
    PersonGun person[MAX_PERSONS];
    ArmyStats stats[ARMY_MONSTERS];
    Chain chain[MAX_BUILDINGS];
    /* this frame */
    const ArmyMonster *m; int nm;
    const ArmyBox *b; int nb;
    float dt;
    float view[2];                                   /* about the camera's x (army_set_view) */
    CollideWorld *cw;                                /* the frame's collision world (army_set_collide), NULL none */
};

/* ---------------------------------------------------------------- small things */

/* gWorld.frand() (0x279F98): the world's Park-Miller, gWorld +0x828 (wrand.c) [R] */
static float frand(Army *a)
{
    (void)a;
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

static int contains(const char *s, const char *what)   /* string.fFind(what) >= 0 */
{
    return s && what && strstr(s, what) != NULL;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float len3(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static void norm3(float v[3])
{
    float l = len3(v);
    if (l < 1e-6f) {
        v[0] = 1;                                    /* [R] the game's normalize of a null vector gives (1, 0, 0) */
        v[1] = v[2] = 0;
    } else {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

void army_frame_point(float F[3][3], const float v[3], int k)
{
    int i = (k + 1) % 3, j = (k + 2) % 3;
    memcpy(F[k], v, sizeof F[k]);
    F[i][0] = fabsf(v[0]) <= 0.7f ? 1.0f : 0.0f;    /* 0x40A008 (1, 0, 0), else 0x40A018 (0, 1, 0) */
    F[i][1] = 1 - F[i][0];
    F[i][2] = 0;
    F[j][0] = F[k][1] * F[i][2] - F[k][2] * F[i][1];
    F[j][1] = F[k][2] * F[i][0] - F[k][0] * F[i][2];
    F[j][2] = F[k][0] * F[i][1] - F[k][1] * F[i][0];
    norm3(F[j]);
    F[i][0] = F[j][1] * F[k][2] - F[j][2] * F[k][1];
    F[i][1] = F[j][2] * F[k][0] - F[j][0] * F[k][2];
    F[i][2] = F[j][0] * F[k][1] - F[j][1] * F[k][0];
}

void army_frame_turn(float F[3][3], float facing, float yaw, float pitch)
{
    float cf = cosf(-facing), sf = sinf(-facing), cp = cosf(pitch), sp = sinf(pitch);
    float h = facing + yaw, ch = cosf(h), sh = sinf(h);
    for (int r = 0; r < 3; r++) {
        float x = F[r][0] * cf - F[r][1] * sf, y = F[r][0] * sf + F[r][1] * cf, z = F[r][2];
        float y2 = y * cp - z * sp, z2 = y * sp + z * cp;
        F[r][0] = x * ch - y2 * sh;
        F[r][1] = x * sh + y2 * ch;
        F[r][2] = z2;
    }
}

void army_beam_axis(float F[3][3], int have_frame, const float dir[3], const float *rod, float out[3])
{
    float r[3] = { 1, 0, 0 };
    if (rod)
        memcpy(r, rod, sizeof r);
    if (!have_frame) {
        int k = 0;
        for (int c = 1; c < 3; c++)
            if (fabsf(r[c]) > fabsf(r[k]))
                k = c;
        float d[3] = { dir[0], dir[1], dir[2] };
        norm3(d);
        if (r[k] < 0)
            for (int c = 0; c < 3; c++)
                d[c] = -d[c];
        army_frame_point(F, d, k);
    }
    for (int c = 0; c < 3; c++)
        out[c] = r[0] * F[0][c] + r[1] * F[1][c] + r[2] * F[2][c];
    norm3(out);
}

static float dist3(const float p[3], const float q[3])
{
    float d[3] = { p[0] - q[0], p[1] - q[1], p[2] - q[2] };
    return len3(d);
}

/* a property as getStringProp / getFloatProp find it: the instance book (obj +0x7C: the AIInstance group) first,
 * then the template (+0x80) [R: 0x2C51A0 does this]. The books keep a record's type before a '|' ("Effect|HitEffect",
 * "Sound|VoiceMove", "OTRef|TroopType1"); the scripts ask by the bare name, which finds it [R 0x1B4490: a record's
 * name (+0x4) is what follows its "Type|", compared in any case], so the types the army's books use are tried */
static const char *const TYPED[] = { "", "Effect|", "Sound|", "Shape|", "OTRef|", "Anim|", "Script|" };

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
        if (s)
            return s;
    }
    return NULL;
}
static float gnum(Group g, const char *key, float def)
{
    Record r;
    return find_rec(g, key, &r) && (r.kind & 0xFF) <= PK_FLOAT ? record_num(r) : def;
}
/* getStringProp (instance, then template); getTemplateStringProp (template only); "" counts as absent */
static const char *ustr(const ArmyUnit *u, const char *key)
{
    const char *s = gstr(u->inst, key);
    if (!s)
        s = gstr(u->tmpl, key);
    return s && *s ? s : NULL;
}
static const char *tstr(const ArmyUnit *u, const char *key)
{
    const char *s = gstr(u->tmpl, key);
    return s && *s ? s : NULL;
}
static float unum(const ArmyUnit *u, const char *key, float def)
{
    Record r;
    if (find_rec(u->inst, key, &r))
        return record_num(r);
    return gnum(u->tmpl, key, def);
}
static float tnum(const ArmyUnit *u, const char *key, float def) { return gnum(u->tmpl, key, def); }

static int who(const Army *a, const ArmyUnit *u) { return (int)(u - a->u); }

static ArmyEvent *emit(Army *a, int type, int unit, const char *name, const float v[3])
{
    static ArmyEvent spare;
    if (a->nev >= MAX_EVENTS) {
        memset(&spare, 0, sizeof spare);
        return &spare;
    }
    ArmyEvent *e = &a->ev[a->nev++];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->unit = unit;
    e->shot = -1;
    e->monster = -1;
    e->id = -1;
    e->name = name;
    if (v) {
        memcpy(e->v, v, sizeof e->v);
        memcpy(e->pos, v, sizeof e->pos);
    }
    return e;
}

/* FollowEffect.createAndStart(pos, name): an effect left where it started; returns its id */
static int effect_at(Army *a, const char *name, const float pos[3])
{
    if (!name || !*name)
        return -1;
    ArmyEvent *e = emit(a, AE_EFFECT, -1, name, pos);
    e->id = a->next_fx++;
    e->dir[1] = 1;
    return e->id;
}
/* FollowEffect.createAndStartAttached(object, name): it follows the unit (offset from its place) */
static int effect_on(Army *a, ArmyUnit *u, const char *name, const float offset[3])
{
    if (!name || !*name)
        return -1;
    float p[3] = { u->pos[0], u->pos[1], u->pos[2] };
    if (offset)
        for (int k = 0; k < 3; k++)
            p[k] += offset[k];
    ArmyEvent *e = emit(a, AE_EFFECT, who(a, u), name, p);
    e->id = a->next_fx++;
    e->dir[1] = 1;
    return e->id;
}
static void effect_stop(Army *a, int id)
{
    if (id >= 0)
        emit(a, AE_EFFECT_STOP, -1, NULL, NULL)->id = id;
}
static void sound(Army *a, ArmyUnit *u, const char *name)   /* add3DSfx */
{
    if (name && *name)
        emit(a, AE_SOUND, who(a, u), name, u->pos);
}

/* getNearestMonster [R: the Actor binding 0x42 (0x2A8FA0 -> 0x2B26F0) and the RScriptObject / AttackObject one 0x37
 * (0x2AA080 -> 0x2B2CC0) are the same code]: the nearest player monster by 3D distance (0x2B27E8 is sqrt.s), starting at
 * 1000: none farther than 1000, so in a block any; -1 with none [G: the game then hands the script gWorld +0x3C (0x4D17DC, the
 * monsters' count to 0x2BD940) as the monster, read by the result's setter 0x27C8D0; what the script sees is not
 * followed] */
static int nearest_monster(const Army *a, const float pos[3])
{
    int best = -1;
    float bd = 1000;
    for (int i = 0; i < a->nm; i++) {
        float e[3] = { a->m[i].pos[0] - pos[0], a->m[i].pos[1] - pos[1], a->m[i].pos[2] - pos[2] };
        float d = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* the mover's target (flag 8; LineMover.getTarget, +0x8C) [R 0x316060]: given none (+0x84), the nearest player monster
 * by squared 3D distance with no limit (from FLT_MAX); -1 none */
static int mover_target(const Army *a, const float pos[3])
{
    int best = -1;
    float bd = 3.4028235e38f;
    for (int i = 0; i < a->nm; i++) {
        float d = dist3(a->m[i].pos, pos);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* [R 0x2DB030] the monster's target point: 0x2E6000's place of `Bip01 Spine1` (node +0x7DB0, its model place at
 * +0xB18 + 0x30 node, scaled by +0xA78, turned and moved by the monster's matrix) plus M (+0xBD54), which only the
 * unused script binding setTargetOffset sets (0 at 0x2DA6CC, 0x2E805C, 0x2E9A34), then z = the feet's + 6
 * (0x2DB0D4 .. 0x2DB108) */
static void monster_target(const ArmyMonster *m, float c[3])
{
    c[0] = m->target[0];
    c[1] = m->target[1];
    c[2] = m->pos[2] + 6;
}

/* one sphere merged into another (0x103770; radii squared): a centre within 0.001 keeps the larger, one inside the
 * other the outer, else the sphere round both */
static void sphere_merge(float c[3], float *r2, const float b[3], float rb2)
{
    if (rb2 < 0)
        return;
    if (*r2 < 0) {
        memcpy(c, b, 3 * sizeof *c);
        *r2 = rb2;
        return;
    }
    float d[3] = { b[0] - c[0], b[1] - c[1], b[2] - c[2] };
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), ra = sqrtf(*r2), rb = sqrtf(rb2);
    if (dist < 0.001f) {
        if (rb2 > *r2) *r2 = rb2;
    } else if ((dist + rb) * (dist + rb) < *r2) {
    } else if ((dist + ra) * (dist + ra) <= rb2) {
        memcpy(c, b, 3 * sizeof *c);
        *r2 = rb2;
    } else {
        float R = 0.5f * (dist + ra + rb), t = (R - ra) / dist;
        if (t < 0) t = 0;
        for (int k = 0; k < 3; k++)
            c[k] += d[k] * t;
        *r2 = R * R;
    }
}

/* a shot's homing point: the target's shape centre (0x293260 -> 0x287F00). A monster's shape is its WCISpheres
 * (+0x68C, ctor 0x2EB6E0): the bounding sphere of its collision spheres, merged in their order from the posed bones
 * (0x2DA9E0 -> 0x29A880 -> 0x103770) [R]. Without spheres the box's middle stands in */
static void monster_centre(const ArmyMonster *m, float c[3])
{
    if (m->nspheres > 0) {
        float r2 = m->spheres[0][3] * m->spheres[0][3];
        memcpy(c, m->spheres[0], 3 * sizeof *c);
        for (int i = 1; i < m->nspheres; i++)
            sphere_merge(c, &r2, m->spheres[i], m->spheres[i][3] * m->spheres[i][3]);
        return;
    }
    if (m->box_max[2] > m->box_min[2])
        for (int k = 0; k < 3; k++)
            c[k] = (m->box_min[k] + m->box_max[k]) / 2;
    else {
        c[0] = m->pos[0];
        c[1] = m->pos[1];
        c[2] = m->pos[2] + 4.25f;                    /* [R] George's box: 4.25 up (MONSTER_DATA +0x19C) */
    }
}

void army_facing_turn(const float f[3], float *w)
{
    float h = f[2], ax = f[0], ay = f[1];
    float ch = cosf(h), sh = sinf(h), cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay);
    float r[9] = { ch * cy - sh * sx * sy, sh * cy + ch * sx * sy, -cx * sy,   /* Rz(h) Rx(ax) Ry(ay) */
                   -sh * cx, ch * cx, sx,
                   ch * sy + sh * sx * cy, sh * sy - ch * sx * cy, cx * cy };
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 3; k++)
            w[4 * c + k] = r[3 * c + k];
}
void army_turn_facing(const float *w, float f[3])
{
    f[0] = asinf(clampf(w[6], -1, 1));
    f[2] = atan2f(-w[4], w[5]);
    f[1] = atan2f(-w[2], w[10]);
}

/* ---------------------------------------------------------------- the AttackObject's wobble (0x315330, 0x314EF0) */

static int osc_key(Group g, const char *key, float *out)   /* 0x1B43C0, a float record only */
{
    Record r;
    if (!g.at || !group_get(g, key, &r) || (r.kind & 0xFF) != PK_FLOAT)
        return 0;
    *out = record_num(r);
    return 1;
}
void army_osc_init(ArmyOsc *o, Group tmpl, Group inst)
{
    memset(o, 0, sizeof *o);
    if (!osc_key(tmpl, "OscillationScale", &o->scale))   /* +0x190: the template's (obj +0x80) */
        o->scale = 0;
    if (inst.at && o->scale <= 0 && !osc_key(inst, "OscillationScale", &o->scale))   /* the instance's (+0x7C) */
        o->scale = 0;
    osc_key(tmpl, "OscillationOffset", &o->phase);  /* +0x18C: the template's, both times (0x3158E0, 0x315934) */
    if (o->phase < 0)
        o->phase = 2 * (PI * wrand_frand());
}
int army_osc_step(ArmyOsc *o, float dt, float d[3])
{
    d[0] = d[1] = d[2] = 0;
    if (o->scale <= 0)
        return 0;
    o->phase += dt;
    float now[3] = { sinf(o->phase * 2) * o->scale, cosf(o->phase * 3) * o->scale, 0 };
    for (int k = 0; k < 3; k++) {
        d[k] = now[k] - o->last[k];
        o->last[k] = now[k];
    }
    return 1;
}
void army_osc_turn(float *w, const float d[3])
{
    float f[3];
    army_turn_facing(w, f);
    for (int k = 0; k < 3; k++)
        f[k] += d[k];
    army_facing_turn(f, w);
}

/* a unit's world matrix: its place, turned by facing about z, then by tilt (x, y) */
static void unit_matrix(ArmyUnit *u)
{
    float f[3] = { u->tilt[0], u->tilt[1], u->facing };
    army_facing_turn(f, u->world);
    u->world[3] = u->world[7] = u->world[11] = 0;
    u->world[12] = u->pos[0];
    u->world[13] = u->pos[1];
    u->world[14] = u->pos[2];
    u->world[15] = 1;
}

static float facing_of(float dx, float dy) { return atan2f(-dx, dy); }   /* the facing z that looks along (dx, dy) */

/* ---------------------------------------------------------------- the actor's look (0x30AAD0, as people.c) */

static void start_anim(Army *a, ArmyUnit *u, const char *name, int random_offset)
{
    snprintf(u->anim, sizeof u->anim, "%s", name);
    char key[64];
    snprintf(key, sizeof key, "ActorAnimShape|%s", name);
    Group look = group_sub(u->tmpl, key);
    float len = 0;
    u->anim_loop = 0;
    u->clip = NULL;
    if (look.at) {
        u->anim_loop = group_num(look, "AnimRepeat", 0) != 0;
        len = fabsf(group_num(look, "Duration", 0));
        const char *shape = group_str(look, "Shape|ShapeName");
        if (!shape)
            shape = gstr(u->tmpl, "ShapeName");
        if (shape)
            u->shape = shape;
        u->clip = group_str(look, "Anim|AnimName");
        if (len <= 0 && a->host.clip_seconds)
            len = a->host.clip_seconds(a->host.ctx, u->clip, u->shape);
    }
    u->anim_len = len;
    u->anim_time = random_offset ? frand(a) * len : 0;
    u->anim_done = 0;
}

/* ---------------------------------------------------------------- the weapons */

/* [R] DamageType names (0x3F5E40, 12 x {int, name}; 0x1B0260 looks the name up): unknown names keep the default 11 */
static const char *const DAMAGE_TYPE[12] = { "Concussive", "Collision", "Explosive", "Edged", "Blunt", "Poison",
                                             "Electrical", "Energy", "Nuclear", "Freezing", "Heat", "Unresistable" };

/* the definition's reader over one group (0x297DC0; projectiles 0x296830 + 0x33B860, beams 0x2960D0 + 0x33B5F0):
 * a key that is absent keeps what the weapon had */
static void weapon_read(ArmyWeapon *w, Group g)
{
    if (!g.at)
        return;
    Record r;
    const char *s;
    if (group_get(g, "Ammo", &r)) {
        w->ammo_max = (int)record_num(r);
    }
    if (group_get(g, "StartAmmo", &r))
        w->start_ammo = (int)record_num(r);
    w->firing_offset = group_num(g, "FiringOffset", w->firing_offset);
    w->refire = group_num(g, "RefireDelay", w->refire);
    w->charge_time = group_num(g, "ChargeTime", w->charge_time);
    w->gravity = group_num(g, "Gravity", w->gravity);
    w->velocity = group_num(g, "Velocity", w->velocity);
    w->range = group_num(g, "MaxFiringRange", w->range);
    if ((s = group_str(g, "DamageType")) != NULL)
        for (int i = 0; i < 12; i++)
            if (same_text(s, DAMAGE_TYPE[i]))
                w->damage_type = i;
    if ((s = group_str(g, "WeaponNode")) != NULL)
        w->node = s;
    if ((s = group_str(g, "Direction")) != NULL) {   /* [R 0x30F4B4] the actor's aim: the first letter - 'X' */
        int c = (*s == '-' ? s[1] : *s) & ~0x20;
        w->aim_axis = c >= 'X' && c <= 'Z' ? c - 'X' : 0;
    }
    if ((s = gstr(g, "FireEffect")) != NULL)
        w->fire_fx = s;
    if ((s = gstr(g, "ChargeEffect")) != NULL)
        w->charge_fx = s;
    if ((s = gstr(g, "ImpactEffect")) != NULL)
        w->impact_fx = s;
    w->duration = group_num(g, "Duration", w->duration);
    w->damage = group_num(g, "ImpactDamage", w->damage);
    w->h_knock = group_num(g, "HorizontalKnockback", w->h_knock);
    w->v_knock = group_num(g, "VerticalKnockback", w->v_knock);
    if (group_get(g, "ReactionLevel", &r))
        w->reaction = (int)record_num(r);
    if (w->type == 0) {                              /* RProjectileWeapon */
        w->impact_delay = group_num(g, "ImpactDelay", w->impact_delay);
        w->mobility = group_num(g, "Mobility", w->mobility);
        w->accel = group_num(g, "Acceleration", w->accel);
        w->init_vel = group_num(g, "InitialVelocityPercent", w->init_vel);
        w->vis_range = group_num(g, "ImpactVisRange", w->vis_range);
        if ((s = group_str(g, "Direction")) != NULL) {   /* "X", "Y", "Z", "-Z": +0x40 negate, +0x3C = c - 'X' */
            w->axis_neg = *s == '-';
            if (*s == '-')
                s++;
            w->axis = (*s >= 'X' && *s <= 'Z') ? *s - 'X' : 2;
        }
    } else {                                         /* RBeamWeapon */
        w->dps = group_num(g, "DamagePerSec", w->dps);
        w->radius = group_num(g, "CollisionRadius", w->radius);
    }
}

/* a weapon of the group g (a Weapon| group of a template): WeaponName finds its book group in Weapons.bdp; the type
 * is the template's WeaponType, else the book's; the book is read first and the template's group over it [R 0x299EC8] */
static int weapon_make(Army *a, ArmyWeapon *w, Group g)
{
    memset(w, 0, sizeof *w);
    w->name = group_str(g, "WeaponName");
    Group book = { 0 };
    if (w->name && a->host.find)
        book = a->host.find(a->host.ctx, "Weapons", w->name);
    const char *type = group_str(g, "WeaponType");
    if (!type && book.at)
        type = group_str(book, "WeaponType");
    if (!type)
        return 0;
    w->type = same_text(type, "RProjectileWeapon") ? 0 : same_text(type, "RBeamWeapon") ? 1 : 2;
    /* [R] the defaults (0x296BC0, 0x296230, 0x33B3E0): range 10, DamageType 11, node -1, StartAmmo -1, Acceleration
     * 1e8, InitialVelocityPercent 1, Direction Z, ImpactVisRange 1, ReactionLevel -1; the rest 0 */
    w->range = 10;
    w->damage_type = 11;
    w->start_ammo = -1;
    w->accel = 1e8f;
    w->init_vel = 1;
    w->axis = 2;
    w->vis_range = 1;
    w->reaction = -1;
    weapon_read(w, book);
    weapon_read(w, g);
    if (w->ammo_max != 0)
        w->def_flags |= 2;                           /* [R] 0x298074: a weapon with Ammo counts its shots */
    /* [R] 0x2978D0: ammo = StartAmmo if >= 0, else Ammo; rate 1; the countdown starts at ChargeTime + FiringOffset */
    w->ammo = w->start_ammo >= 0 ? w->start_ammo : w->ammo_max;
    w->scale = 1;
    w->timer = w->charge_time + w->firing_offset;
    w->beam_left = w->duration;                      /* [R] 0x295E30 */
    w->shot = -1;
    w->first = 0;
    return 1;
}

/* [R 0x299640] the cluster's keys on a weapon of it: the rates as read (radians a second); the ranges in degrees: Range
 * r gives -r .. r (negative or missing: the default, turn +-360, pitch +-40), Min / MaxRange each end */
void army_cluster_read(ArmyWeapon *w, Group cl)
{
    float rr = group_num(cl, "RotationRange", -1), er = group_num(cl, "ElevationRange", -1);
    w->rot_range = (rr >= 0 ? rr : 360) * PI / 180;
    w->elev_range = (er >= 0 ? er : 40) * PI / 180;
    w->rot_min = group_num(cl, "RotationMinRange", -w->rot_range * 180 / PI) * PI / 180;
    w->rot_max = group_num(cl, "RotationMaxRange", w->rot_range * 180 / PI) * PI / 180;
    w->elev_min = group_num(cl, "ElevationMinRange", -w->elev_range * 180 / PI) * PI / 180;
    w->elev_max = group_num(cl, "ElevationMaxRange", w->elev_range * 180 / PI) * PI / 180;
    w->rot_rate = group_num(cl, "RotationRate", 1);
    w->elev_rate = group_num(cl, "ElevationRate", 0.5f);
    w->clear_rot = group_num(cl, "RequiresRotationClearToFire", 1) != 0;
    w->clear_elev = group_num(cl, "RequiresElevationClearToFire", 1) != 0;
    w->recenter = group_num(cl, "ReCenterWhenNoTarget", 0) != 0;
    w->rot_node = group_str(cl, "RotationControlNode");
    w->elev_node = group_str(cl, "ElevationControlNode");
}

/* the template's weapons: WeaponSystem { WeaponCluster { turret, Weapon| {...} ... } } (objects), Weapon|<name>
 * (actors) */
static void unit_weapons(Army *a, ArmyUnit *u)
{
    Record r[32];
    u->nw = 0;
    Group sys = group_sub(u->tmpl, "WeaponSystem|WeaponSystem");
    if (!sys.at)
        sys = group_sub(u->tmpl, "WeaponSystem|");
    if (sys.at) {
        uint32_t nc = group_all(sys, "WeaponCluster|", r, 32);
        for (uint32_t c = 0; c < nc && u->nw < ARMY_WEAPONS; c++) {
            Group cl = record_group(sys, r[c]);
            Record wr[8];
            uint32_t nw = group_all(cl, "Weapon|", wr, 8);
            for (uint32_t i = 0; i < nw && u->nw < ARMY_WEAPONS; i++)
                if (weapon_make(a, &u->w[u->nw], record_group(cl, wr[i])))
                    army_cluster_read(&u->w[u->nw++], cl);
        }
        return;
    }
    uint32_t n = group_all(u->tmpl, "Weapon|", r, 32);
    for (uint32_t i = 0; i < n && u->nw < ARMY_WEAPONS; i++)
        if (weapon_make(a, &u->w[u->nw], record_group(u->tmpl, r[i])))
            u->nw++;
}

/* ---- the shots */

static ArmyShot *new_shot(Army *a)
{
    for (int i = 0; i < a->nshots; i++)
        if (!a->shot[i].alive) {
            memset(&a->shot[i], 0, sizeof a->shot[i]);
            a->shot[i].id = a->next_shot++;
            a->shot[i].alive = 1;
            return &a->shot[i];
        }
    if (a->nshots >= MAX_SHOTS)
        return NULL;
    ArmyShot *s = &a->shot[a->nshots++];
    memset(s, 0, sizeof *s);
    s->id = a->next_shot++;
    s->alive = 1;
    return s;
}

/* the first thing a ray from p along the unit d meets within len: the world's ray (gp - 0x6D60 vtable +0x18,
 * 0x283730, collide_ray): the street first, then every object with a flag of include and none of exclude, the
 * nearest. A shot's (0x293CB4): include the shot's +0x134, exclude its +0x138 | 8, and a hit on its owner (+0xC4) is
 * no hit [R 0x293CD8]; a beam's (0x295800): include -1, exclude 8 with the owner flagged 8 around the cast [R].
 * Returns the distance, -1 none; *monster the monster it hit (an object tagged COLLIDE_OWN_MONSTER) or -1, *obj the
 * world's object (-1 the street or none), nrm the surface */
static float ray_hit(Army *a, const float p[3], const float d[3], float len, uint32_t include, uint32_t exclude,
                     int owner, int beam, int *monster, int *obj, float nrm[3])
{
    static CollideWorld none;                        /* no world: the street alone */
    CollideWorld *w = a->cw ? a->cw : &none;
    *monster = *obj = -1;
    int oo = owner >= 0 && owner < a->n ? collide_check(w, a->u[owner].cobj, COLLIDE_TAG(COLLIDE_OWN_ARMY, owner), 0) : -1;
    uint32_t keep = oo >= 0 ? w->o[oo].flags : 0;
    if (beam && oo >= 0)
        w->o[oo].flags |= COLLIDE_F_RAYSKIP;
    CollideHit h;
    int got = collide_ray(w, p, d, len, include, exclude | COLLIDE_F_RAYSKIP, 0, &h);
    if (oo >= 0)
        w->o[oo].flags = keep;
    if (!got || (!beam && oo >= 0 && h.obj == oo))
        return -1;
    memcpy(nrm, h.normal, sizeof h.normal);
    *obj = h.obj;
    if (h.obj >= 0 && COLLIDE_TAG_OWNER(w->o[h.obj].tag) == COLLIDE_OWN_MONSTER) {
        int m = COLLIDE_TAG_INDEX(w->o[h.obj].tag);
        *monster = m < a->nm ? m : -1;
    }
    return h.t;
}

/* [R] the impact on a monster (RProjectile 0x33BCA0, RBeam 0x33B440): the hit object flagged 0x40 (a monster) gets
 * the knock: the shot's direction in x, y, made 1 long and times HorizontalKnockback, and VerticalKnockback up (a
 * missing direction takes the constant at 0x409FF8); with a ReactionLevel >= 0 it goes to 0x2D9650 (the monster's
 * weapon hit: damage, type, owner, the knock, the level), else to the monster's TakeDamage (vtable +0x34) */
static void hit_monster(Army *a, int monster, const char *weapon, int owner, float damage, int type, int level,
                        float hk, float vk, const float dir[3], const float at[3])
{
    float k[3] = { dir[0], dir[1], 0 };
    float l = sqrtf(k[0] * k[0] + k[1] * k[1]);
    if (l < 1e-4f) {
        k[0] = 1;
        k[1] = 0;
    } else {
        k[0] /= l;
        k[1] /= l;
    }
    if (getenv("RAMPAGE_TRACE"))                     /* who hit it: a unit's template, or a person (the punks) */
        printf("  %.2f: %s's %s hits monster %d: %.2f\n", a->now,
               owner >= 0 && owner < a->n && a->u[owner].name ? a->u[owner].name : "person", weapon ? weapon : "?",
               monster, damage);
    ArmyEvent *e = emit(a, AE_HIT, owner, weapon, NULL);
    e->monster = monster;
    e->amount = damage;
    e->level = level;
    e->id = type;
    e->v[0] = k[0] * hk;
    e->v[1] = k[1] * hk;
    e->v[2] = vk;
    memcpy(e->pos, at, sizeof e->pos);
    memcpy(e->dir, dir, sizeof e->dir);
}

/* RProjectileWeapon's fire (0x2963C0): [R] one shot from the firing matrix, along its Direction axis (negated with
 * '-'), its record filled from the definition (0x2962C0 + 0x33B770: Duration, ImpactDamage x the damage scale,
 * ImpactVisRange, ImpactDelay, Gravity, Mobility, Velocity, Acceleration, InitialVelocityPercent, the knocks and the
 * ReactionLevel), its look the FireEffect, then the countdown restarts at max(RefireDelay, ChargeTime) */
static void fire_projectile(Army *a, ArmyUnit *u, ArmyWeapon *w, const float from[3], const float dir0[3], int target)
{
    ArmyShot *s = new_shot(a);
    if (!s)
        return;
    s->weapon = w->name;
    s->effect = w->fire_fx;
    s->impact_fx = w->impact_fx;
    s->owner = u ? who(a, u) : -1;
    s->target = target;
    memcpy(s->pos, from, sizeof s->pos);
    memcpy(s->dir, dir0, sizeof s->dir);
    norm3(s->dir);
    if (getenv("RAMPAGE_TRACE"))                     /* a unit's, or a person's (the punks' bottles) */
        printf("  %.2f: %s fires %s from %.1f %.1f %.1f along %.2f %.2f %.2f\n", a->now,
               u ? u->name ? u->name : "?" : "person",
               w->name ? w->name : "?", from[0], from[1], from[2], s->dir[0], s->dir[1], s->dir[2]);
    s->damage = w->damage * w->scale;
    s->duration = w->duration;
    s->vis_range = w->vis_range;
    s->impact_delay = w->impact_delay;
    s->gravity = w->gravity;
    s->mobility = w->mobility;
    s->velocity = w->velocity;
    s->accel = w->accel;
    s->h_knock = w->h_knock;
    s->v_knock = w->v_knock;
    s->reaction = w->reaction;
    s->damage_type = w->damage_type;
    s->include = COLLIDE_F_WORLD;                    /* [R] 0x2953E0: the ray's masks, include 4, exclude 0 */
    s->exclude = 0;
    if (s->damage_type == 8)                         /* [R] 0x33B770: DamageType 8 meets only the buildings */
        s->include = COLLIDE_F_BUILDING;
    if (s->damage_type == 4) {                       /* [R] 0x33B770: Blunt counts as 0 and meets buildings and monsters */
        s->damage_type = 0;
        s->include = COLLIDE_F_BUILDING | COLLIDE_F_MONSTER;
    }
    s->phase = frand(a) * 2 * PI;                    /* [R] 0x29463C: +0xC0 */
    s->speed = s->velocity * w->init_vel;            /* [R] 0x2949B8: Velocity x InitialVelocityPercent */
    if (s->effect) {
        ArmyEvent *e = emit(a, AE_EFFECT, -1, s->effect, s->pos);
        e->shot = s->id;
        e->id = a->next_fx++;
        memcpy(e->dir, s->dir, sizeof e->dir);
    }
}

/* the shot each frame (0x293120): [R]
 *  - its age grows; past Duration it is gone (once its effects are done, 0x2931D0);
 *  - towards its target (+0xD0: 0x287F00, the target's place) it turns: the change of its unit direction is at most
 *    Mobility x dt long (0x2936EC), with the target circling by max(5, 0.2 x the distance) when flag 0x10 is set;
 *  - it steps (0x294300): speed = min(Velocity, speed + Acceleration x dt); the velocity loses 9.81 x Gravity x dt
 *    in z; the direction and speed are the velocity's; it moves by velocity x dt;
 *  - the step's segment is tested (after ImpactDelay seconds, 0x293C4C) and the first thing met is hit: a monster
 *    gets the impact (0x33BCA0), the ImpactEffect starts where it hit (when within ImpactVisRange of the view,
 *    0x16C0A0: not tested here), and the shot is done (flag 2) */
static void shot_update(Army *a, ArmyShot *s, float dt)
{
    s->age += dt;
    if (s->age >= s->duration) {
        s->alive = 0;
        return;
    }
    if (s->target >= 0 && s->target < a->nm) {
        float t[3], to[3];
        monster_centre(&a->m[s->target], t);
        for (int k = 0; k < 3; k++)
            to[k] = t[k] - s->pos[k];
        norm3(to);
        float d[3] = { s->dir[0] - to[0], s->dir[1] - to[1], s->dir[2] - to[2] };
        float dl = len3(d), most = s->mobility * dt;
        if (dl < most)
            memcpy(s->dir, to, sizeof s->dir);
        else {
            for (int k = 0; k < 3; k++)
                s->dir[k] -= d[k] * most / dl;
            norm3(s->dir);
        }
    }
    s->speed = fminf(s->velocity, s->speed + s->accel * dt);
    float v[3] = { s->dir[0] * s->speed, s->dir[1] * s->speed, s->dir[2] * s->speed - 9.81f * s->gravity * dt };
    s->speed = len3(v);
    if (s->speed > 0)
        for (int k = 0; k < 3; k++)
            s->dir[k] = v[k] / s->speed;
    float step = s->speed * dt;
    if (step > 0.001f && s->age >= s->impact_delay) {
        int mon, obj;
        float nrm[3];
        float t = ray_hit(a, s->pos, s->dir, step + 0.001f, s->include, s->exclude, s->owner, 0, &mon, &obj, nrm);
        if (t >= 0) {
            float at[3] = { s->pos[0] + s->dir[0] * t, s->pos[1] + s->dir[1] * t, s->pos[2] + s->dir[2] * t };
            if (getenv("RAMPAGE_TRACE")) {           /* what it met: the street, a monster, a module's object */
                int tag = obj >= 0 && a->cw ? a->cw->o[obj].tag : 0;
                printf("  shot %s hit %s %d.%d at %.1f %.1f %.1f\n", s->weapon ? s->weapon : "?",
                       obj < 0 ? "street" : COLLIDE_TAG_OWNER(tag) == COLLIDE_OWN_BUILDING ? "building" :
                       COLLIDE_TAG_OWNER(tag) == COLLIDE_OWN_MONSTER ? "monster" : "object",
                       obj < 0 ? -1 : COLLIDE_TAG_OWNER(tag), obj < 0 ? -1 : COLLIDE_TAG_INDEX(tag), at[0], at[1], at[2]);
            }
            if (mon >= 0)
                hit_monster(a, mon, s->weapon, s->owner, s->damage, s->damage_type, s->reaction, s->h_knock,
                            s->v_knock, s->dir, at);
            if (s->impact_fx) {
                ArmyEvent *e = emit(a, AE_EFFECT, -1, s->impact_fx, at);
                e->id = a->next_fx++;
                memcpy(e->dir, nrm, sizeof e->dir);
            }
            memcpy(s->pos, at, sizeof s->pos);
            s->alive = 0;
            return;
        }
    }
    for (int k = 0; k < 3; k++)
        s->pos[k] += v[k] * dt;
}

/* the beam while it fires (0x295750): [R] from the weapon along its rod through the firing frame, the first thing
 * met: the beam stops there (0x226DF0), and a hit object takes, on the beam's first touch (+0x1B8), ImpactDamage x
 * the damage scale, after that DamagePerSec x dt each frame (if DamagePerSec > 0), through the impact 0x33B440; the
 * ImpactEffect sits where it touches. frame: the weapon's firing frame (NULL: made from dir) */
static void beam_update(Army *a, ArmyUnit *u, ArmyWeapon *w, const float from[3], const float dir[3],
                        const float frame[3][3], float dt)
{
    ArmyShot *s = NULL;
    for (int i = 0; i < a->nshots; i++)
        if (a->shot[i].alive && a->shot[i].id == w->shot)
            s = &a->shot[i];
    if (!s)
        return;
    memcpy(s->pos, from, sizeof s->pos);
    s->age += dt;
    /* [R 0x2270A0, 0x226CC0, 0x226DF0, 0x295750] the reach is the FireEffect's collision rod, a stream kept from
     * frame to frame: the clock (the age) runs on; the tail stays at 0 while the clock is within the emitter's cycle,
     * then grows by the rod's speed (the loop byte +0x42 that would wrap the clock is never written: no beam loops);
     * the head grows by the speed up to the rod's length; a tail at the head ends the stream for the burst (both 0).
     * The cast runs from the tail, head - tail long when that is more than 0.001, and a hit at t cuts the head to
     * tail + t, so it grows again from there. The axis: the rod's direction through the weapon's firing matrix
     * (0x226950 with 0x2971A0's frame, which the effect is placed in too, 0x2975A4). No rod, no cast.
     * MaxFiringRange only gates the firing */
    float head, tail;
    float speed, length, cycle, rd[3];
    int loops, rod = a->host.rod && w->fire_fx && a->host.rod(a->host.ctx, w->fire_fx, rd, &speed, &length, &cycle,
                                                               &loops);
    if (frame)
        memcpy(s->R, frame, sizeof s->R);
    army_beam_axis(s->R, frame != NULL, dir, rod ? rd : NULL, s->dir);
    if (a->host.rod) {
        head = tail = 0;
        if (rod) {
            tail = s->age <= cycle ? 0 : s->tail + speed * dt;
            head = fminf(s->head + speed * dt, length);
            if (tail >= head)
                head = tail = 0;
        }
    } else {
        tail = 0;
        head = fminf(w->velocity > 0 ? w->velocity * s->age : w->range, w->range);
    }
    float from_t[3] = { s->pos[0] + s->dir[0] * tail, s->pos[1] + s->dir[1] * tail, s->pos[2] + s->dir[2] * tail };
    int mon, obj;
    float nrm[3];
    float t = head - tail > 0.001f ?
              ray_hit(a, from_t, s->dir, head - tail, ~0u, 0, u ? who(a, u) : -1, 1, &mon, &obj, nrm) : -1;
    if (t >= 0)
        head = tail + t;                             /* [R 0x226DF0] the cut */
    s->head = head;
    s->tail = tail;
    s->length = head;
    if (t < 0)
        return;
    /* [R 0x2958CC] the impact at the weapon's place + t along: the tail not added */
    float at[3] = { s->pos[0] + s->dir[0] * t, s->pos[1] + s->dir[1] * t, s->pos[2] + s->dir[2] * t };
    if (mon >= 0) {
        float dmg = w->first ? w->scale * w->damage : w->dps > 0 ? w->scale * w->dps * dt : 0;
        if (dmg > 0)
            hit_monster(a, mon, w->name, u ? who(a, u) : -1, dmg, w->damage_type, w->reaction, w->h_knock, w->v_knock,
                        s->dir, at);
    }
    if (w->first && w->impact_fx) {
        ArmyEvent *e = emit(a, AE_EFFECT, -1, w->impact_fx, at);
        e->id = a->next_fx++;
        memcpy(e->dir, nrm, sizeof e->dir);
    }
    w->first = 0;
}

/* a weapon each frame (the common countdown 0x2971A0, the charge 0x297760 and the fire 0x2963C0 / 0x295C70) [R]:
 *  - the countdown falls by the rate x dt while the weapon has ammo or does not count it (+0x4 & 2); while it may not
 *    fire it stops at ChargeTime + FiringOffset, so from a permission to the shot is FiringOffset + ChargeTime;
 *  - at ChargeTime or less (with ammo) the ChargeEffect starts, once;
 *  - at 0, with ammo: one shot (or the beam starts for Duration), ammo - 1, and the countdown starts again at
 *    max(RefireDelay, ChargeTime);
 *  - a beam that has run its Duration stops (0x295C20) and the countdown restarts the same way.
 * may: what the weapon system allows [R 0x29ADE0, 0x299010]: a target within MaxFiringRange and, for a cluster, on
 * target (weapon_aim); an actor's while enableWeapon and setWeaponTarget hold */
static void weapon_tick(Army *a, ArmyUnit *u, ArmyWeapon *w, int may, const float from[3], const float dir[3],
                        const float frame[3][3], int target, float dt)
{
    if (w->type == 2)
        return;
    if (w->type == 1 && w->firing) {
        w->beam_left -= dt;
        if (w->beam_left <= 0) {
            w->timer = fmaxf(w->refire, w->charge_time);
            w->firing = w->charging = 0;
            for (int i = 0; i < a->nshots; i++)
                if (a->shot[i].alive && a->shot[i].id == w->shot)
                    a->shot[i].alive = 0;
            w->shot = -1;
        } else
            beam_update(a, u, w, from, dir, frame, dt);
    }
    if (w->ammo > 0 || !(w->def_flags & 2)) {
        w->timer -= w->scale * dt;
        float floor = may ? 0 : w->charge_time + w->firing_offset;
        if (w->timer < floor)
            w->timer = floor;
    }
    if (!w->on || !may || w->firing)
        return;
    if (w->timer <= w->charge_time && !w->charging && w->ammo > 0) {
        w->charging = 1;
        if (w->charge_fx)
            effect_at(a, w->charge_fx, from);
    }
    if (w->timer > 0 || w->ammo <= 0)
        return;
    w->ammo--;
    w->charging = 0;
    if (w->type == 0) {
        fire_projectile(a, u, w, from, dir, target);
        w->timer = fmaxf(w->refire, w->charge_time);
    } else {
        ArmyShot *s = new_shot(a);
        if (getenv("RAMPAGE_TRACE"))
            printf("  %.2f: %s fires %s (a beam)\n", a->now, u && u->name ? u->name : "person", w->name ? w->name : "?");
        w->beam_left = w->duration;                  /* [R] 0x295CDC */
        w->firing = 1;
        w->first = 1;
        if (s) {
            s->beam = 1;
            s->weapon = w->name;
            s->effect = w->fire_fx;
            s->owner = u ? who(a, u) : -1;
            s->target = target;
            s->duration = w->duration;
            memcpy(s->pos, from, sizeof s->pos);
            /* [R 0x2975A4] its effect placed in the firing frame, the beam along the rod through it */
            float rd[3], speed, length, cycle;
            int loops, rod = a->host.rod && w->fire_fx &&
                             a->host.rod(a->host.ctx, w->fire_fx, rd, &speed, &length, &cycle, &loops);
            if (frame)
                memcpy(s->R, frame, sizeof s->R);
            army_beam_axis(s->R, frame != NULL, dir, rod ? rd : NULL, s->dir);
            w->shot = s->id;
            if (s->effect) {
                ArmyEvent *e = emit(a, AE_EFFECT, -1, s->effect, from);
                e->shot = s->id;
                e->id = a->next_fx++;
                memcpy(e->dir, s->dir, sizeof e->dir);
            }
        }
    }
}

/* [R 0x293050] the aim of a shot that falls: at the target lifted by 1/2 g (L / V)^2 (L the straight distance, V the
 * Velocity, g = 9.81 x Gravity; no lift without gravity), straight from p (actors 0x30EC10 and clusters 0x29ADE0) */
static void lob(const float p[3], const float t[3], float v, float g, float dir[3])
{
    float d[3] = { t[0] - p[0], t[1] - p[1], t[2] - p[2] };
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (g > 0 && v > 0)
        d[2] += 0.5f * g * (l / v) * (l / v);
    memcpy(dir, d, sizeof d);
    norm3(dir);
}

/* [R 0x29B3B8 .. 0x29B7D8] the cluster's correction of its pitch for the object's own tilt: x the angle of the
 * object's x row above the level (acos, 0x105C60, of the row against its level part made 1 long; signed by the row's
 * z), y minus that of its y row; y negated while the turn (+0x54, before this frame's step) is beyond +-pi / 2, x while
 * it is below 0; the larger of the two plus half the smaller. 0 for an upright frame */
static float row_rise(const float *row)
{
    float h[3] = { row[0], row[1], 0 }, l = sqrtf(h[0] * h[0] + h[1] * h[1]);
    if (l < 1e-6f)
        h[0] = 1, h[1] = 0;
    else
        h[0] /= l, h[1] /= l;
    float a = acosf(clampf(h[0] * row[0] + h[1] * row[1], -1, 1));
    return row[2] < 0 ? -a : a;
}
static float cluster_tilt(const float *world, float turn)
{
    float x = row_rise(world), y = -row_rise(world + 4);
    if (fabsf(turn) > PI / 2)
        y = -y;
    if (turn < 0)
        x = -x;
    return fabsf(x) <= fabsf(y) ? y + 0.5f * x : x + 0.5f * y;
}

int army_cluster_turn(ArmyWeapon *w, const float world[16], const float from[3], const float *target, float dt)
{
    float sy = w->rot_rate * dt, sp = w->elev_rate * dt;
    if (!target) {
        /* [R 0x29ADE0] ReCenterWhenNoTarget (flag 0x10) without a target: the same steps towards 0, 0 */
        if (w->recenter) {
            w->yaw += clampf(-w->yaw, -sy, sy);
            w->pitch += clampf(-w->pitch, -sp, sp);
        }
        return 0;
    }
    /* [R] the cluster (0x29ADE0) aims at the target lifted for the shot's fall (0x293050: Velocity, Gravity) */
    float aim[3];
    lob(from, target, w->velocity, 9.81f * w->gravity, aim);
    /* [R 0x29B1AC] the turn: the aim's angle less that of the object's +y row, wrapped to +-pi; [R 0x29B7F8] the
     * pitch: atan2 of the rise over the level distance, plus the object's own tilt (cluster_tilt) */
    float want_yaw = atan2f(-aim[0], aim[1]) - atan2f(-world[4], world[5]);
    while (want_yaw > PI) want_yaw -= 2 * PI;
    while (want_yaw < -PI) want_yaw += 2 * PI;
    float want_pitch = asinf(clampf(aim[2], -1, 1)) + cluster_tilt(world, w->yaw);
    /* [R] 0x298E70 / 0x298F40: each kept within its ends (0x299640: degrees x pi / 180) */
    if (w->rot_max - w->rot_min < 2 * PI - 1e-3f)
        want_yaw = clampf(want_yaw, w->rot_min, w->rot_max);
    want_pitch = clampf(want_pitch, w->elev_min, w->elev_max);
    /* [R 0x29BA50] a step of rate x dt, onto the target once it is that close; on target when the turn is within
     * pi / 50 and the pitch within pi / 100, or when the cluster does not require it */
    w->yaw += clampf(want_yaw - w->yaw, -sy, sy);
    w->pitch += clampf(want_pitch - w->pitch, -sp, sp);
    return (!w->clear_rot || fabsf(want_yaw - w->yaw) <= PI / 50) &&
           (!w->clear_elev || fabsf(want_pitch - w->pitch) < PI / 100);
}

/* where a unit's weapon fires from and which way: the WeaponNode as drawn when the host knows it, along the
 * definition's axis (objects: their cluster turned onto the target at RotationRate / ElevationRate radians a second
 * within its ranges, 0x29ADE0 / 0x29BA50 [R]); actors aim their Direction axis straight at the target, lifted for a
 * shot that falls (0x30EC10 [R]). Without the node (the host has none) the weapon is 2 above the unit's place.
 * Returns whether the cluster is on the target */
static int weapon_aim(Army *a, ArmyUnit *u, ArmyWeapon *w, const float tp[3], int has_target, float from[3],
                      float dir[3], float F[3][3], float dt)
{
    float node[16];
    int have_node = a->host.node_world && w->node && a->host.node_world(a->host.ctx, u, w->node, node);
    if (have_node)
        memcpy(from, node + 12, 3 * sizeof(float));
    else {
        from[0] = u->pos[0];
        from[1] = u->pos[1];
        from[2] = u->pos[2] + (u->kind == AK_TROOP || u->kind == AK_WINDOW ? 1.5f : 2.0f);
    }
    /* [R 0x2971A0] the firing frame: the identity (+0xBC, gp-0x70F0) times the WeaponNode's world; an actor's is the
     * node's world (its own, else the actor's) with its aimed axis turned onto the target (0x30EC10) */
    const float *m = have_node ? node : u->world;
    for (int r = 0; r < 3; r++) {
        memcpy(F[r], m + 4 * r, sizeof F[r]);
        norm3(F[r]);
    }
    int actor = u->kind == AK_TROOP || u->kind == AK_WINDOW || u->kind == AK_OTHER;
    if (actor) {
        if (!has_target) {
            dir[0] = -sinf(u->facing);
            dir[1] = cosf(u->facing);
            dir[2] = 0;
            return 0;
        }
        lob(from, tp, w->velocity, 9.81f * w->gravity, dir);
        army_frame_point(F, dir, w->aim_axis);       /* [R 0x30F010] */
        return 1;
    }
    if (w->axis_neg && w->axis == 2) {              /* bombs: straight down from their node */
        if (have_node) {
            dir[0] = -node[8];
            dir[1] = -node[9];
            dir[2] = -node[10];
        } else {
            dir[0] = dir[1] = 0;
            dir[2] = -1;
        }
        return has_target;
    }
    /* a turret: yaw about the unit's z, pitch up, from its front (+y in its frame) */
    int on = army_cluster_turn(w, u->world, from, has_target ? tp : NULL, dt);
    float h = u->facing + w->yaw;
    float f[3] = { -sinf(h) * cosf(w->pitch), cosf(h) * cosf(w->pitch), sinf(w->pitch) };
    memcpy(dir, f, sizeof f);
    /* [R 0x2971A0, 0x298980] the firing frame is the WeaponNode's world as drawn, which carries the cluster's turn
     * where the shape has its control nodes (a frame behind: the shape is posed after the update, and the node's
     * world the object hands out, vtable +0x4C, is its last pose); [R 0x2963C0] a shot leaves along that frame's
     * Direction axis (+0x3C, negated by +0x40). Where the shape is not drawn with the cluster's nodes the turn is put
     * on the frame here and the shot leaves along the turn's way [G: a stand-in for the nodes the game turns; where
     * both are known the drawn axis agrees with it within 0 deg on the tanks, 4 on the rocking PirateShip] */
    float cn[16];
    const char *ctl = w->rot_node ? w->rot_node : w->elev_node;
    int drawn = have_node && ctl && a->host.node_world(a->host.ctx, u, ctl, cn);
    if (have_node && (drawn || (!w->rot_rate && !w->elev_rate))) {
        for (int k = 0; k < 3; k++)
            dir[k] = node[4 * w->axis + k] * (w->axis_neg ? -1.0f : 1.0f);
    } else
        army_frame_turn(F, u->facing, w->yaw, w->pitch);
    return on;
}

/* every weapon of a unit this frame, at its target (u->target, or the point lock for StationaryAttackObject) */
static void unit_weapons_tick(Army *a, ArmyUnit *u, float dt, const float *point)
{
    float tp[3] = { 0, 0, 0 };
    int has = 0;
    if (point) {
        memcpy(tp, point, sizeof tp);
        has = 1;
    } else if (u->target >= 0 && u->target < a->nm) {
        monster_target(&a->m[u->target], tp);
        has = 1;
    }
    for (int i = 0; i < u->nw; i++) {
        ArmyWeapon *w = &u->w[i];
        float from[3], dir[3], F[3][3];
        int aimed = weapon_aim(a, u, w, tp, has, from, dir, F, dt);
        int may = has && u->weapons_on && w->on && aimed && dist3(from, tp) <= w->range;
        weapon_tick(a, u, w, may, from, dir, (const float (*)[3])F, point ? -1 : u->target, dt);
    }
}

static void weapons_enable(ArmyUnit *u, int on)      /* enableWeapons / enableWeapon */
{
    u->weapons_on = on;
    for (int i = 0; i < u->nw; i++)
        u->w[i].on = on;
}
/* setWeaponAmmo(n, -, index): the weapon's vtable +0x84 (0x296F50): ammo = min(n, Ammo) [R] */
static void weapon_ammo(ArmyUnit *u, int n, int index)
{
    if (index >= 0 && index < u->nw)
        u->w[index].ammo = n < u->w[index].ammo_max ? n : u->w[index].ammo_max;
}
/* reloadWeapon: the weapon's vtable +0x80 (0x296F70) [R]: ammo = Ammo (0 stays 0), the countdown at least ChargeTime +
 * FiringOffset (the charge effect restarted, not here) */
static void weapons_reload(ArmyUnit *u)
{
    for (int i = 0; i < u->nw; i++) {
        ArmyWeapon *w = &u->w[i];
        float t = w->charge_time + w->firing_offset;
        w->ammo = w->ammo_max;
        if (w->timer < t)
            w->timer = t;
    }
}

/* ---------------------------------------------------------------- the score (gCommon.addStats, 0x2C51A0) */

/* [R] Player.addPoints (0x33F530): points and kills of category t (0 destruction, 1 civilian, 2 vehicle, 3 bonus,
 * 4 time bonus, 5 challenge), and of 6, the total */
static void add_points(Army *a, int slot, int t, float pts, const char *name)
{
    if (slot < 0 || slot >= ARMY_MONSTERS || t < 0 || t >= 7)
        return;
    ArmyStats *st = &a->stats[slot];
    st->points[t] += pts;
    st->kills[t]++;
    if (t != 6) {
        st->points[6] += pts;
        st->kills[6]++;
    }
    ArmyEvent *e = emit(a, AE_SCORE, -1, name, NULL);
    e->monster = slot;
    e->amount = pts;
    e->level = t;
}

/* [R] addStats(obj, slot, type): once per object (flag 0x40000000); the monster (while its HP > 0) gains Health
 * (HP = max(1, HP + Health) within its most) and Energy (within 0 .. 100, a rage at 100), as AE_MONSTER_HP /
 * AE_ENERGY for the host to apply; the player's category `type` gets the object's Points (an int) */
static void add_stats(Army *a, ArmyUnit *u, int slot, int type)
{
    if (u->counted)
        return;
    u->counted = 1;
    float health = unum(u, "Health", 0), energy = unum(u, "Energy", 0);
    int points = (int)unum(u, "Points", 0);
    if (slot >= 0 && slot < ARMY_MONSTERS) {
        ArmyEvent *e;
        if (health != 0) {
            e = emit(a, AE_STATS_HEALTH, who(a, u), u->name, NULL);
            e->monster = slot;
            e->amount = health;
        }
        if (energy != 0) {
            e = emit(a, AE_STATS_ENERGY, who(a, u), u->name, NULL);
            e->monster = slot;
            e->amount = energy;
        }
    }
    add_points(a, slot, type, (float)points, u->name);
}

const ArmyStats *army_stats(const Army *a, int monster)
{
    return monster >= 0 && monster < ARMY_MONSTERS ? &a->stats[monster] : NULL;
}

void army_add_stats(Army *a, int monster, Group tmpl, int type)
{
    ArmyUnit tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.tmpl = tmpl;
    tmp.name = group_name(tmpl);
    /* not a unit of ours: only the score, the gains go out as events with unit -1 */
    float health = gnum(tmpl, "Health", 0), energy = gnum(tmpl, "Energy", 0);
    if (monster >= 0 && monster < ARMY_MONSTERS) {
        ArmyEvent *e;
        if (health != 0) {
            e = emit(a, AE_STATS_HEALTH, -1, tmp.name, NULL);
            e->monster = monster;
            e->amount = health;
        }
        if (energy != 0) {
            e = emit(a, AE_STATS_ENERGY, -1, tmp.name, NULL);
            e->monster = monster;
            e->amount = energy;
        }
    }
    add_points(a, monster, type, (float)(int)gnum(tmpl, "Points", 0), tmp.name);
}

/* the buildings' Scoring (DefBuildingProps, 0x2BA4B0) [R]: see army.h */
static Group scoring_sub(Group scoring, const char *name)
{
    Group g = group_sub(scoring, name);
    if (!g.at) {                                     /* Chicago and NewYork write cell / floor in lower case */
        char low[32];
        size_t i;
        for (i = 0; name[i] && i < sizeof low - 1; i++)
            low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
        low[i] = 0;
        g = group_sub(scoring, low);
    }
    return g;
}
static const char *const STATE_KEY[4] = { "None", "Light", "Medium", "Heavy" };
static const char *state_key(int state) { return state >= 1 && state <= 3 ? STATE_KEY[state] : STATE_KEY[0]; }

float army_score_building(Army *a, int monster, int building, Group building_props, int event, int state,
                          const char *monster_move, int width, int storeys, const int *front_states, int nfront,
                          const float at[3])
{
    if (building < 0 || building >= MAX_BUILDINGS)
        return 0;
    Chain *c = &a->chain[building];
    if (monster < 0) {                               /* [R] no attacker (+0x190 = 0): the chain is reset */
        c->step = c->sum = 0;
        c->skip = 0;
        return 0;
    }
    Group scoring = group_sub(building_props, "Scoring");
    if (!scoring.at)
        return 0;
    float pts = 0;
    int whole = 0;
    if (event == ARMY_SCORE_CELL) {
        Group s = scoring_sub(scoring, "Cell");
        if (!s.at)
            return 0;
        if (c->skip)
            c->skip = 0;                             /* the hit that dropped a floor paid the floor */
        else {
            pts = group_num(s, state_key(state), 0);
            c->step = c->sum = 0;
        }
    } else if (event == ARMY_SCORE_FLOOR) {
        Group s = scoring_sub(scoring, "Floor");
        if (!s.at)
            return 0;
        if (c->sum == 0) {
            pts = group_num(s, "Base", 0) * width;
            Group mf = group_sub(scoring, "MultiFloor");
            if (mf.at) {
                /* the step is MultiFloor's entry named by the attacker's state (+0x10C's name), else its Base; no
                 * state is named Charge / Flurry / Roof / Rage, so it is Base [R + G] */
                Record r;
                c->step = monster_move && group_get(mf, monster_move, &r) ? record_num(r) : group_num(mf, "Base", 0);
            }
        } else if (c->sum == c->step)
            emit(a, AE_SOUND, -1, "v_combo", at);    /* the second floor */
        pts += c->sum;
        c->sum += c->step;
        c->skip = 1;
    } else {
        Group s = scoring_sub(scoring, "Building");
        if (!s.at)
            return 0;
        whole = 1;
        pts = group_num(s, "Base", 0) * width * storeys;
        for (int i = 0; i < nfront; i++)
            pts += group_num(s, state_key(front_states[i]), 0);
        if (c->skip) {
            Group k = scoring_sub(scoring, c->step < c->sum ? "MultiFloor" : "Floor");
            pts += group_num(k, "Collapse", 0);
            emit(a, AE_SOUND, -1, "v_super", at);
        }
        c->skip = 1;
    }
    if (pts != 0) {
        add_points(a, monster, 0, pts, NULL);
        float fl = group_num(scoring, "Floating", 0);
        if (fl > 0 && pts >= fl && at) {             /* a floating number (0x331D50, scale 0.8) at y - 5 */
            ArmyEvent *e = emit(a, AE_FLOAT, -1, NULL, at);
            e->pos[1] -= 5;
            e->amount = pts;
            e->monster = monster;
            e->level = whole;                        /* colour 0xFF00F0F0 for a building, else 0xFFF0F0F0 */
        }
    }
    return pts;
}

/* ---------------------------------------------------------------- the spawner (gAIMgr, 0x4D7800) */

static int unit_new(Army *a, const char *name, Group tmpl, Group inst);
static void unit_init(Army *a, ArmyUnit *u);
static int kind_of(const char *script);

/* [R 0x304450, the current block's objects (0x302DE0)] its AttackObjectInstance| list: unless the instance or its
 * template has NoLoadOnActiveBlock, or a SpawnDelay (then gAIMgr makes it later, 0x334A20: only the bikers' actors
 * have one), each is an AttackObject from the pool 0x4D5EB0 (0x283DA0) given its init (vtable +0x14, 0x304FA0 ->
 * 0x315330 with f12 0: the world object's 0x287AB0 places it at Location turned by ZRotation degrees about z and
 * keeps the group as its instance book (+0x7C), the script's Construct, the WeaponSystem 0x29C020, the wobble's keys)
 * and added to the world (gWorld vtable +0x3C); onInit at the block's start. Of their templates only the PirateShip
 * (StationaryAttackObject.bsf, LV05_JollyRogers) is a unit here: the trees are streetobj.c's, the ambient bosses
 * boss.c's */
static void block_attack_objects(Army *a, Group block_def)
{
    Group where;
    Record r[256];
    uint32_t n = group_values(block_def, "AttackObjectInstance|", &where, r, 256);
    for (uint32_t i = 0; i < n && a->host.find; i++) {
        Group g = record_group(where, r[i]);
        const char *name = group_str(g, "Name");
        Group t = name ? a->host.find(a->host.ctx, "Objects1_Props", name) : (Group){ 0 };
        Record x;
        if (!t.at || kind_of(gstr(t, "ScriptName")) != AK_TURRET || group_get(g, "NoLoadOnActiveBlock", &x) ||
            group_get(t, "NoLoadOnActiveBlock", &x) || group_num(g, "SpawnDelay", group_num(t, "SpawnDelay", 0)) != 0)
            continue;
        int k = unit_new(a, name, t, g);
        if (k < 0)
            continue;
        ArmyUnit *u = &a->u[k];
        Group loc = group_sub(g, "Location");
        Record lr[3];
        uint32_t nl = loc.at ? group_items(loc, lr, 3) : 0;
        for (uint32_t q = 0; q < 3; q++)
            u->pos[q] = q < nl ? record_num(lr[q]) : 0;
        u->facing = group_num(g, "ZRotation", 0) * PI / 180;
        unit_init(a, u);                             /* Construct (+ onInit: its lTarget, the point kept here) */
    }
}

/* [R] 0x334C40: the block's AIDef (here, or up its book's chain); each AIInstance child a record (0x3350F0):
 * MinDelay 10, RespawnDelay 10, OffsetDelay 5, MaxOccurances 9999 unless set; the first spawn in
 * MinDelay + OffsetDelay x rand seconds */
int army_load_block(Army *a, Group block_def)
{
    a->nsp = 0;
    a->active = 0;
    block_attack_objects(a, block_def);
    Group ai = group_sub(block_def, "AIDef");
    if (!ai.at)
        return 0;
    Record r[64];
    uint32_t n = group_all(ai, "AIInstance", r, 64);
    for (uint32_t i = 0; i < n && a->nsp < MAX_SPAWN; i++) {
        Group g = record_group(ai, r[i]);
        Spawner *s = &a->sp[a->nsp];
        memset(s, 0, sizeof *s);
        s->g = g;
        s->name = gstr(g, "Name");
        if (!s->name)
            continue;
        if (a->host.find) {                          /* a boss is boss.c's (Boss_*.bsf), not a unit */
            Group t = a->host.find(a->host.ctx, "Objects1_Props", s->name);
            const char *sc = t.at ? gstr(t, "ScriptName") : NULL;
            if (sc && (strncmp(sc, "Boss_", 5) == 0 || _stricmp(sc, "freighter.bsf") == 0))
                continue;                            /* (and a freighter is rail.c's) */
        }
        s->min_delay = group_num(g, "MinDelay", 10);
        s->respawn = group_num(g, "RespawnDelay", 10);
        s->offset = group_num(g, "OffsetDelay", 5);
        s->left = (int)group_num(g, "MaxOccurances", 9999);
        s->timer = s->min_delay + s->offset * frand(a);
        s->enabled = 1;
        a->nsp++;
    }
    return a->nsp;
}

int army_weapon_make(Army *a, ArmyWeapon *w, Group weapon_group) { return weapon_make(a, w, weapon_group); }
void army_weapon_tick(Army *a, ArmyWeapon *w, int may, const float from[3], const float dir[3],
                      const float frame[3][3], int target, float dt)
{
    weapon_tick(a, NULL, w, may, from, dir, frame, target, dt);
}

void army_activate(Army *a, int on) { a->active = on; }   /* [R] 0x2A6208: bit 8 of the manager's flags */

/* [R] gAIMgr.removeAIObject (0x332A50): every record of that template name has one fewer alive */
static void remove_ai(Army *a, ArmyUnit *u)
{
    for (int i = 0; i < a->nsp; i++)
        if (same_text(a->sp[i].name, u->name))
            a->sp[i].alive--;
}
/* [R] gAIMgr.enableRespawn (0x3329B0): every record of that name on or off; its timer runs on either way */
static void enable_respawn(Army *a, const char *name, int on)
{
    for (int i = 0; i < a->nsp; i++)
        if (same_text(a->sp[i].name, name))
            a->sp[i].enabled = on != 0;
}

/* [R] the update 0x3333F0, only while active: a record with nothing alive and spawns left counts its timer down
 * (enabled or not); enabled and at 0 it spawns its template as an AttackObject (an Actor when IsActor, which no
 * book sets), whose onInit gets the AIInstance group as its instance book, and restarts at RespawnDelay +
 * OffsetDelay x rand. At most one of each record alive; MaxOccurances spawns in all */
static void spawner_update(Army *a, float dt)
{
    if (!a->active)
        return;
    for (int i = 0; i < a->nsp; i++) {
        Spawner *s = &a->sp[i];
        if (s->alive != 0 || s->left == 0)
            continue;
        s->timer -= dt;
        if (!s->enabled || s->timer > 0)
            continue;
        s->timer = s->respawn + s->offset * frand(a);
        s->alive++;
        s->left--;
        Group t = a->host.find ? a->host.find(a->host.ctx, "Objects1_Props", s->name) : (Group){ 0 };
        if (!t.at)
            continue;                                /* [R] alive stays 1: that record never spawns again */
        int k = unit_new(a, s->name, t, s->g);
        if (k >= 0) {
            a->u[k].spawner = i;
            unit_init(a, &a->u[k]);
        }
    }
}

/* ---------------------------------------------------------------- the movers */

/* LineMover (0x315E10, as people.c): [R] from point to point at speed; 2 at the end, 3 at a point, 4 a monster
 * within MonsterResponseDist while it moves; flag 1 faces the way it goes, 8 the nearest monster (its target) */
static void line_init(ArmyUnit *u, const float p[3], const float q[3], float speed)
{
    memcpy(u->mv.pts[0], p, sizeof u->mv.pts[0]);
    memcpy(u->mv.pts[1], q, sizeof u->mv.pts[1]);
    u->mv.n = 2;
    u->mv.seg = 0;
    u->mv.t = 0;
    u->mv.speed = speed;
    u->mv.active = 1;
    u->mv.kind = 0;
    memcpy(u->pos, p, sizeof u->pos);
}

/* SplineMover (helicopters, droppables; vtable 0x44B800, update 0x33ACA0) [R, as boss.c]: points with their
 * distances along the path (addPoint(p, d), as the scripts give them), a Hermite curve with Kochanek-Bartels tangents
 * (0x13B2F0, T = B = C = 0 for addPoint); its place grows by the velocity; flag 2 faces along the tangent */
static void spline_init(ArmyUnit *u, float vel)
{
    u->mv.n = 0;
    u->mv.seg = 0;
    u->mv.t = 0;
    u->mv.s = 0;
    u->mv.ready = 0;
    u->mv.speed = vel;
    u->mv.active = 1;
    u->mv.kind = 1;
}
static void spline_add(ArmyUnit *u, const float p[3], float d)
{
    if (u->mv.n < 8) {
        memcpy(u->mv.pts[u->mv.n], p, sizeof u->mv.pts[0]);
        u->mv.dist[u->mv.n++] = d;
        u->mv.ready = 0;
    }
}
static void spline_prepare(ArmyUnit *u)
{
    int n = u->mv.n;
    u->mv.closed = n > 2 && memcmp(u->mv.pts[0], u->mv.pts[n - 1], sizeof u->mv.pts[0]) == 0;
    for (int i = 0; i < n; i++) {
        int pv = i - 1, nx = i + 1;
        if (pv < 0)
            pv = u->mv.closed ? n - 2 : 0;
        if (nx >= n)
            nx = u->mv.closed ? 1 : n - 1;
        for (int k = 0; k < 3; k++)
            u->mv.m[i][k] = 0.5f * ((u->mv.pts[i][k] - u->mv.pts[pv][k]) + (u->mv.pts[nx][k] - u->mv.pts[i][k]));
    }
    u->mv.ready = 1;
}
/* [R 0x13B780] the curve between the two points whose distances hold s (before the first: it; past the last: it) */
static void spline_eval(ArmyUnit *u, float s, float pos[3], float tan[3])
{
    if (!u->mv.ready)
        spline_prepare(u);
    int n = u->mv.n;
    memset(tan, 0, 3 * sizeof(float));
    if (s <= u->mv.dist[0] || n == 1) {
        memcpy(pos, u->mv.pts[0], 3 * sizeof(float));
        if (n > 1)
            memcpy(tan, u->mv.m[0], 3 * sizeof(float));
        return;
    }
    if (s >= u->mv.dist[n - 1]) {
        memcpy(pos, u->mv.pts[n - 1], 3 * sizeof(float));
        memcpy(tan, u->mv.m[n - 1], 3 * sizeof(float));
        return;
    }
    int i = 0;
    while (i < n - 2 && s >= u->mv.dist[i + 1])
        i++;
    float len = u->mv.dist[i + 1] - u->mv.dist[i], t = len > 0 ? (s - u->mv.dist[i]) / len : 1;
    float t2 = t * t, t3 = t2 * t;
    float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
    float d00 = 6 * t2 - 6 * t, d10 = 3 * t2 - 4 * t + 1, d01 = -6 * t2 + 6 * t, d11 = 3 * t2 - 2 * t;
    for (int k = 0; k < 3; k++) {
        float p0 = u->mv.pts[i][k], p1 = u->mv.pts[i + 1][k], m0 = u->mv.m[i][k], m1 = u->mv.m[i + 1][k];
        pos[k] = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
        tan[k] = d00 * p0 + d10 * m0 + d01 * p1 + d11 * m1;
    }
}
/* [R 0x33ACA0] flag 4 holds it; past the last point it loops with flag 1, else stays at the end answering 2 every
 * frame until the script gives it another path */
static int spline_step(ArmyUnit *u, float dt)
{
    int n = u->mv.n;
    if ((u->mv.flags & 4) || n == 0)
        return 1;
    int r = 1;
    float end = u->mv.dist[n - 1], start = u->mv.dist[0];
    u->mv.s += u->mv.speed * dt;
    if (u->mv.s > end) {
        if (u->mv.flags & 1)
            u->mv.s -= end - start;
        else {
            u->mv.s = end;
            r = 2;
        }
    }
    float tan[3];
    spline_eval(u, u->mv.s, u->pos, tan);
    if ((u->mv.flags & 2) && (tan[0] != 0 || tan[1] != 0)) {
        u->facing = facing_of(tan[0], tan[1]);
        u->tilt[0] = u->tilt[1] = 0;                 /* [R 0x33AD58] the frame made afresh (its pitch along the
                                                      * tangent not here): the wobble's change since the last frame
                                                      * is all it gets (0x314EF0) */
    }
    return r;
}

static int mover_step(Army *a, ArmyUnit *u, float dt)
{
    if (!u->mv.active || u->mv.n < 1 || (u->mv.kind == 0 && u->mv.n < 2))
        return 0;
    if (u->mv.kind == 1)
        return spline_step(u, dt);
    int r = 1;
    float *p = u->mv.pts[u->mv.seg], *q = u->mv.pts[u->mv.seg + 1];
    float d[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] };
    float len = len3(d);
    if (len > 0)
        u->mv.t += dt * u->mv.speed / len;
    else if (u->mv.speed > 0 || u->mv.kind == 1)
        u->mv.t = 2;
    if (u->mv.t > 1) {
        if (u->mv.seg < u->mv.n - 2) {
            u->mv.seg++;
            u->mv.t = 0;
            r = 3;
        } else {
            u->mv.t = 1;
            r = 2;
            u->mv.active = 0;
        }
    }
    p = u->mv.pts[u->mv.seg];
    q = u->mv.pts[u->mv.seg + 1];
    for (int k = 0; k < 3; k++)
        u->pos[k] = p[k] + (q[k] - p[k]) * u->mv.t;
    int face_path = u->mv.kind == 1 ? (u->mv.flags & 2) != 0 : (u->mv.flags & 1) != 0;
    if (face_path && (q[0] != p[0] || q[1] != p[1]))
        u->facing = facing_of(q[0] - p[0], q[1] - p[1]);
    if (u->mv.kind == 0 && (u->mv.flags & 8)) {
        int m = mover_target(a, u->pos);
        if (m >= 0)
            u->facing = facing_of(a->m[m].pos[0] - u->pos[0], a->m[m].pos[1] - u->pos[1]);
    }
    if (r == 1 && u->mv.resp2 > 0)
        for (int i = 0; i < a->nm; i++) {
            float dd = dist3(u->pos, a->m[i].pos);
            if (dd * dd <= u->mv.resp2)
                r = 4;
        }
    return r;
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

/* the unit's shape (0x28FD70, collide_shape_pick): the template's CollisionBox, CollisionRadius (DefActorProps: 2)
 * or AutoCollisionBox with the model's box, then the instance's; else the sphere the model keeps (the host's
 * shape_sphere; a host without it: its box's sphere; without a box radius 1 at the origin) */
static void unit_shape_read(const Army *a, const ArmyUnit *u, CollideShape *s)
{
    float tb[6], ib[6], mb[6], c[3] = { 0, 0, 0 }, r = 1;
    int have = u->shape && a->host.shape_box && a->host.shape_box(a->host.ctx, u->shape, mb, mb + 3);
    if (have) {
        float h[3];
        for (int k = 0; k < 3; k++) {
            c[k] = (mb[k] + mb[3 + k]) / 2;
            h[k] = (mb[3 + k] - mb[k]) / 2;
        }
        r = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    }
    /* [R 0x28FD70 -> 0x184CB0] the sphere the model keeps, the union of its parts' spheres (0x185AA0) */
    if (have && a->host.shape_sphere)
        a->host.shape_sphere(a->host.ctx, u->shape, c, &r);
    Group t = u->tmpl, in = u->inst;
    collide_shape_pick(s, have ? c : NULL, r, t.at ? book_box(t, tb) : NULL, t.at ? gnum(t, "CollisionRadius", -1) : -1,
                       t.at && gnum(t, "AutoCollisionBox", 0) != 0, in.at ? book_box(in, ib) : NULL,
                       in.at ? group_num(in, "CollisionRadius", -1) : -1,
                       in.at && group_num(in, "AutoCollisionBox", 0) != 0, have ? mb : NULL);
}

static void unit_shape(const Army *a, ArmyUnit *u, CollideShape *s)   /* read once, at its first use */
{
    if (!u->cshape_ok) {
        unit_shape_read(a, u, &u->cshape);
        u->cshape_ok = 1;
    }
    *s = u->cshape;
}

/* [R 0x2E22B0 -> 0x280470] a monster's sphere against the unit's shape at its matrix (collide_obj_sphere): the
 * monster's attacks and a droppable's touch alike */
static int unit_meets(const Army *a, ArmyUnit *u, const float c[3], float radius)
{
    CollideObj co;
    float n[3], pt[3], push[3];
    memset(&co, 0, sizeof co);
    unit_shape(a, u, &co.shape);
    collide_matrix_from16(co.m, u->world);
    co.flags = COLLIDE_F_WORLD;
    co.building = -1;
    return collide_obj_sphere(&co, c, radius, n, pt, push);
}

static void unit_take_impact(Army *a, ArmyUnit *u);

typedef struct { Army *a; ArmyUnit *u; } FlightCtx;

/* a contact of the flight (collide_mover_step): the street is OnHitGround (counted by the step); an object is
 * TakeImpact on both [R 0x287430]: on a unit of ours here, on anything else through AE_IMPACT */
static void flight_contact(void *user, int other, const float p[3], const float n[3])
{
    FlightCtx *c = user;
    Army *a = c->a;
    if (other < 0 && getenv("RAMPAGE_TRACE") && c->u->cm.vel[2] > 0.5f)   /* a bounce (the impulse is in) */
        printf("  %s bounced off the street at %.1f %.1f %.1f\n", c->u->name ? c->u->name : "?", p[0], p[1], p[2]);
    if (other < 0 || !a->cw)
        return;
    const CollideObj *o = &a->cw->o[other];
    if (getenv("RAMPAGE_TRACE"))
        printf("  %s met %d.%d at %.1f %.1f %.1f\n", c->u->name ? c->u->name : "?", COLLIDE_TAG_OWNER(o->tag),
               COLLIDE_TAG_INDEX(o->tag), p[0], p[1], p[2]);
    unit_take_impact(a, c->u);
    if (COLLIDE_TAG_OWNER(o->tag) == COLLIDE_OWN_ARMY) {
        int j = COLLIDE_TAG_INDEX(o->tag);
        if (j < a->n && &a->u[j] != c->u)
            unit_take_impact(a, &a->u[j]);
        return;
    }
    ArmyEvent *e = emit(a, AE_IMPACT, who(a, c->u), c->u->name, p);
    e->id = o->tag;
    e->level = o->kind;
    memcpy(e->dir, n, sizeof e->dir);
    e->amount = c->u->cm.mass;
}

/* the SphereMover of a flight (setVelocity, smashObject's SpherePhysicsMover): the game's (collide.c, 0x28E440) with
 * the template's Gravity (DefObjectProps: 35, else 9.81), Mass, Restitution, SlidingFriction and InertiaSolidity
 * (0x28F220); started from the unit's place, velocity and spin (setRotation's rates as they are: an angular velocity
 * about the world's axes [R 0x2B2F50 -> 0x28DD80]); the street and the frame's objects (TakeImpact). Returns the
 * street contacts this frame (OnHitGround, script event 11, every sub-step it touches) */
static int sphere_step(Army *a, ArmyUnit *u, float dt)
{
    if (!u->cm_on) {
        CollideShape sh;
        unit_shape(a, u, &sh);
        float r = collide_mover_radius(&sh);
        if (r < 0.05f)
            r = sh.radius > 0.05f ? sh.radius : 1;
        collide_mover_init(&u->cm, r, tnum(u, "Gravity", 9.81f), tnum(u, "Mass", 1), tnum(u, "Restitution", 0.8f),
                           tnum(u, "SlidingFriction", 0.4f), tnum(u, "InertiaSolidity", 1));
        collide_mover_spin(&u->cm, u->spin);         /* [R 0x2B2F50 -> 0x28DD80] the rates as given: a world axis */
        collide_matrix_from16(u->cmat, u->world);
        u->cm_on = 1;
    }
    memcpy(u->cm.vel, u->vel, sizeof u->cm.vel);    /* the scripts may set it (setVelocity) */
    int i = who(a, u);
    int self = collide_check(a->cw, u->cobj, COLLIDE_TAG(COLLIDE_OWN_ARMY, i), 0);
    FlightCtx c = { a, u };
    int hit = collide_mover_step_ex(a->cw, &u->cm, u->cmat, self, dt, COLLIDE_F_MONSTER, flight_contact, &c);
    memcpy(u->vel, u->cm.vel, sizeof u->vel);
    collide_matrix_to16(u->cmat, u->world);
    u->pos[0] = u->cmat[9];
    u->pos[1] = u->cmat[10];
    u->pos[2] = u->cmat[11];
    u->facing = atan2f(u->cmat[1], u->cmat[0]);     /* unit_matrix's x axis is (cos, sin) x cos tilt */
    return hit;
}

/* [R] TakeDamage's knock-away of a wreck (Vehicle_Attack, Vehicle_Helicopter): the way from the monster, made 1
 * long, times (12, 5, 1.5), z made positive */
static void wreck_away(ArmyUnit *u, const float monster_pos[3])
{
    float d[3] = { u->pos[0] - monster_pos[0], u->pos[1] - monster_pos[1], u->pos[2] - monster_pos[2] };
    norm3(d);
    u->vel[0] = d[0] * 12;
    u->vel[1] = d[1] * 5;
    u->vel[2] = fabsf(d[2] * 1.5f);
    u->mv.active = 0;
    u->mv.kind = 2;
    u->cm_on = 0;
}

/* the AttackMover (vtable 0x44B430: init 0x313D90, update 0x313F80 ...): see attack_step below */
static void attack_init(Army *a, ArmyUnit *u);
static void attack_step(Army *a, ArmyUnit *u, float dt);

/* smashObject (0x2B2F50, as people.c): an actor flies off on a SpherePhysicsMover, 15 along a yaw in the 120 degree
 * cone towards the camera (narrowed on the side the monster faces) and 15 x (0.8 .. 1.3) up [R] */
static void smash_object(Army *a, ArmyUnit *u, float monster_heading)
{
    const float c = 3 * PI / 2, lo = 20 * PI / 180, wd = 120 * PI / 180;
    float A = sinf(c), B = cosf(c), A2 = sinf(monster_heading + PI / 2), B2 = cosf(monster_heading + PI / 2);
    float d = -(B2 * B + A2 * A), cross = B2 * A - A2 * B;
    float width = lo + (wd - lo) * (1 - d) / 2, a0 = c - wd / 2, a1 = c + wd / 2;
    if (cross < 0)
        a0 = a1 - width;
    else
        a1 = a0 + width;
    float yaw = a0 + (a1 - a0) * frand(a);
    u->vel[0] = 15 * cosf(yaw);
    u->vel[1] = 15 * sinf(yaw);
    u->vel[2] = 15 * (0.8f + 0.5f * frand(a));
    u->spin[0] = (frand(a) < 0.5f ? 1 : -1) * (2 * PI + PI / 6 * frand(a));
    u->spin[1] = (frand(a) < 0.5f ? 1 : -1) * (PI / 16 + PI / 16 * frand(a));
    u->spin[2] = (frand(a) < 0.5f ? 1 : -1) * (PI / 16 + PI / 16 * frand(a));
    u->pos[0] = u->world[12];
    u->pos[1] = u->world[13];
    u->pos[2] = u->world[14];
    u->mv.active = 0;
    u->mv.kind = 2;
    u->cm_on = 0;
    u->oflags &= ~(uint32_t)OF_HELD;
    u->phase = AU_DYING;
}

/* ---------------------------------------------------------------- the units */

static int kind_of(const char *script)
{
    static const struct { const char *s; int k; } K[] = {
        { "Vehicle_Attack.bsf", AK_TANK }, { "Vehicle_Helicopter.bsf", AK_HELI }, { "CombatTruck.bsf", AK_TRUCK },
        { "Swat.bsf", AK_TROOP }, { "Military_Window.bsf", AK_WINDOW }, { "StationaryAttackObject.bsf", AK_TURRET },
        { "Bomber.bsf", AK_BOMBER }, { "Droppable.bsf", AK_DROP } };
    for (size_t i = 0; i < sizeof K / sizeof *K; i++)
        if (same_text(script, K[i].s))
            return K[i].k;
    return AK_OTHER;
}

static int unit_new(Army *a, const char *name, Group tmpl, Group inst)
{
    int k = -1;
    for (int i = 0; i < a->n; i++)
        if (a->u[i].phase == AU_GONE) {
            k = i;
            break;
        }
    if (k < 0) {                                     /* a fixed pool: the scripts keep pointers across a spawn */
        if (a->n == a->cap)
            return -1;
        k = a->n++;
    }
    ArmyUnit *u = &a->u[k];
    memset(u, 0, sizeof *u);
    u->name = name;
    u->tmpl = tmpl;
    u->inst = inst;
    u->kind = kind_of(gstr(tmpl, "ScriptName"));
    u->phase = AU_ALIVE;
    u->shape = gstr(tmpl, "Shape");
    if (!u->shape)
        u->shape = gstr(tmpl, "ShapeName");
    u->target = -1;
    u->spawner = -1;
    u->friend_of = -1;
    u->person = -1;
    u->cobj = -1;
    u->pulse_delay = 0.25f;
    /* [R 0x315330 through the vtable's init 0x304FA0, before onInit] the wobble's keys, the instance book the
     * AIInstance; the Actors (0x44B300) have none [R: 0x314EF0 is in the AttackObject's vtable alone, the Actor's
     * +0x10 is 0x30CD20], and none of their templates has the keys */
    army_osc_init(&u->osc, tmpl, inst);
    unit_weapons(a, u);
    unit_matrix(u);
    return k;
}

static void unit_remove(Army *a, ArmyUnit *u)        /* requestDelete */
{
    if (u->phase == AU_GONE)
        return;
    u->phase = AU_GONE;
    for (int i = 0; i < u->nw; i++)
        if (u->w[i].shot >= 0)
            for (int s = 0; s < a->nshots; s++)
                if (a->shot[s].alive && a->shot[s].id == u->w[i].shot)
                    a->shot[s].alive = 0;
    emit(a, AE_REMOVED, who(a, u), u->name, u->pos);
}

/* the hit effects of the vehicle scripts: "HitEffect" + n */
static const char *hit_effect_n(const ArmyUnit *u, int n, int template_only)
{
    char k[32];
    snprintf(k, sizeof k, "HitEffect%d", n);
    return template_only ? tstr(u, k) : ustr(u, k);
}

/* ======== Vehicle_Attack.bsf (tanks), pulses every 1/15 s */

static void tank_init(Army *a, ArmyUnit *u)          /* Construct + onInit */
{
    u->explode_time = 0;
    u->flags = 2 | 1;
    u->oflags |= OF_PUSH_OUT | OF_EFF_NODES;
    u->hp_start = u->hp = unum(u, "HitPoints", 51);
    u->state = 0;                                    /* bOnScreen */
    u->aux[0] = -1;                                  /* nTreadSoundId */
    u->pulse_delay = 1.0f / 15;
    u->life = unum(u, "Lifespan", 1000);
    u->life_state = 0;
    float pos[3] = { -100, -7.5f, 0 }, facing = -PI / 2;
    if (frand(a) < 0.5f) {
        pos[0] = -pos[0];
        facing = -facing;
    }
    memcpy(u->pos, pos, sizeof pos);
    u->facing = facing;
    float end[3] = { -pos[0], pos[1], pos[2] };
    line_init(u, pos, end, 12);
    sound(a, u, ustr(u, "MovementSound"));
    weapons_enable(u, 1);
}

static void tank_lifespan(ArmyUnit *u)               /* processLifespan */
{
    if (u->flags > (1 | 2))
        return;
    u->life -= 1.0f / 15;
    if (u->life_state == 0) {
        if (u->life <= 0) {
            u->life_state = 1;
            u->pulse_delay = TIMESTEP;
            u->mv.active = 0;                        /* setMover(an empty LineMover) */
            u->mv.kind = 0;
            weapons_enable(u, 0);
        }
    } else if (u->life_state == 1) {
        float z = u->facing;
        if (u->pos[0] > 0) {
            z -= PI * u->pulse_delay;
            if (z <= -PI / 2) {
                z = -PI / 2;
                u->life_state = 2;
            }
        } else {
            z += PI * u->pulse_delay;
            if (z >= PI / 2) {
                z = PI / 2;
                u->life_state = 2;
            }
        }
        u->facing = z;
    } else if (u->life_state == 2) {
        u->life_state = 3;
        float end[3] = { 100, -7.5f, 0 };
        if (u->pos[0] < 0)
            end[0] = -end[0];
        line_init(u, u->pos, end, 10);
    }
}

static int tank_pulse(Army *a, ArmyUnit *u)
{
    if (u->state == 0) {                             /* not yet on screen */
        float x = u->pos[0] > 0 ? u->pos[0] + 10 : u->pos[0] - 10;
        float p[3] = { x, u->pos[1], u->pos[2] };
        if (a->host.visible ? a->host.visible(a->host.ctx, p, 1) : x >= a->view[0] && x <= a->view[1]) {
                                                     /* gWorld.isSphereVisible(pos, 1) */
            u->state = 1;
            attack_init(a, u);                       /* createAttackMover; gCameraMgr.addInterestObject */
        }
        return 1;
    }
    tank_lifespan(u);
    if (u->flags & 8) {
        u->explode_time += 1.0f / 15;
        if (u->explode_time >= 1.5f) {
            u->flags = 16;
            u->explode_time = 0;
            effect_stop(a, (int)u->aux[1]);
            u->aux[1] = (float)effect_at(a, tstr(u, "HitEffect"), u->pos);
        }
        return 1;
    } else if (u->flags & 16) {
        u->explode_time += 1.0f / 15;
        if (u->explode_time >= 1.0f) {
            remove_ai(a, u);
            return DELETE_ME;
        }
    } else if (u->flags & 4) {                       /* the hop when hit */
        u->bounce_vel[2] -= 50.0f / 15;
        float p[3];
        for (int k = 0; k < 3; k++)
            p[k] = u->pos[k] + u->bounce_vel[k] * TIMESTEP;
        if (p[2] < 0) {
            if (--u->bounce == 0) {
                u->flags &= ~4;
                u->pulse_delay = 1.0f / 15;
            }
            u->bounce_vel[0] = -5;
            u->bounce_vel[1] = 0;
            u->bounce_vel[2] = 7.5f * (float)u->bounce;
            p[2] = 0;
            effect_on(a, u, "Footfall", NULL);
        }
        memcpy(u->pos, p, sizeof p);
    }
    return 1;
}

static void tank_damage(Army *a, ArmyUnit *u, int monster, float amount)
{
    if (u->hp <= 0 || monster < 0)
        return;
    const float *mp = a->m[monster].pos;
    u->hp -= amount;
    effect_stop(a, (int)u->aux[1]);
    if (!(u->flags & 4)) {
        u->flags |= 4;
        u->bounce_vel[0] = -5;
        u->bounce_vel[1] = 0;
        u->bounce_vel[2] = 20;
        u->bounce = 2;
        u->pulse_delay = TIMESTEP;
        if (mp[0] < u->pos[0])
            u->bounce_vel[0] = -u->bounce_vel[0];
    }
    if (u->hp <= 0 && !(u->flags & 8)) {
        const char *d = ustr(u, "DestShape");
        if (d)
            u->shape = d;
        u->flags = 8;
        u->oflags &= ~(uint32_t)(OF_PUSH_OUT | OF_EFF_NODES);
        wreck_away(u, mp);
        weapons_enable(u, 0);
        add_stats(a, u, monster, 2);
    }
    int n = (int)(u->hp / (u->hp_start / 3));
    const char *fx = hit_effect_n(u, n, 0);
    if (fx)
        u->aux[1] = (float)(u->hp <= 0 ? effect_at(a, fx, u->pos) : effect_on(a, u, fx, NULL));
    fx = ustr(u, "HitEffect");
    if (!(u->flags & 8) && fx)
        effect_on(a, u, fx, NULL);
}

/* ======== Vehicle_Helicopter.bsf: 0 looking, 2 closing in, 1 attacking, 3 leaving, 5 falling */

static void heli_path(Army *a, ArmyUnit *u)          /* createPath */
{
    u->sub = 0;                                      /* nFlightState */
    float maxh = 0;
    for (int i = 0; i < a->nm; i++)
        if (a->m[i].pos[2] > maxh)
            maxh = a->m[i].pos[2];
    float opos = u->pos[0], xpos = 100.0f + (u->is_cop ? 9.0f : 0.0f);   /* is_cop: bIsFriend */
    spline_init(u, u->mv.speed > 0 ? u->mv.speed : 12);
    u->mv.flags = 2;
    for (int i = 0; i < 8; i++) {
        float h = maxh > 12 ? maxh + frand(a) * 9 : 12 + frand(a) * 6;
        if (u->is_cop)
            h += 2;
        float p[3] = { xpos - (float)i * (200.0f / 8), -10, h };
        if (opos < 0)
            p[0] = -p[0];
        spline_add(u, p, (float)i * (200.0f / 8));
    }
}

static void heli_init(Army *a, ArmyUnit *u)          /* onInit (is_cop = bIsFriend, side = bHeadingLeft) */
{
    float pos[3] = { -100, -12.5f, 20 }, facing = -PI / 2;
    if (u->is_cop) {
        pos[0] = -112;
        if (!u->side) {
            pos[0] = -pos[0];
            facing = -facing;
        }
    } else if (frand(a) < 0.5f) {
        pos[0] = -pos[0];
        facing = -facing;
    }
    if (unum(u, "IsHoming", 0) != 0) {
        weapon_ammo(u, 0, 0);
        weapon_ammo(u, 0, 1);
    }
    u->state_time = 0;
    u->state = 0;
    u->explode_time = 0;
    u->ihp = (int)unum(u, "HitPoints", 1);
    u->aux[2] = unum(u, "TargetRange", 35);
    u->aux[3] = unum(u, "TargetXRange", 15);
    u->aux[4] = unum(u, "AttackDuration", 5);
    u->aux[5] = unum(u, "Acceleration", 30);
    u->aux[6] = unum(u, "Velocity", 12);         /* fMaxVelocity */
    u->aux[7] = u->aux[6];                       /* fVelocity */
    u->count = 0;                                    /* bReportDestruction */
    u->life = unum(u, "Lifespan", 1000);
    u->life_state = 0;
    u->pulse_time = 0;
    u->oflags |= OF_EFF_NODES | OF_ALPHA;
    memcpy(u->pos, pos, sizeof pos);
    u->facing = facing;
    u->mv.speed = 12;
    heli_path(a, u);
    u->pulse_delay = 0.25f;
    u->target = -1;                                  /* setTarget(nullTarget) */
    const char *fx = ustr(u, "AttachEffect");
    if (fx)
        u->aux[0] = (float)effect_on(a, u, fx, NULL);   /* (AttachEffectNode: by that node) */
    else
        u->aux[0] = -1;
    if (!u->is_cop) {
        float chance = unum(u, "FriendChance", 0);   /* the AIInstance's (instance book) */
        if (frand(a) <= chance) {
            const char *name = ustr(u, "Name");
            if (!name)
                name = u->name;
            Group t = a->host.find ? a->host.find(a->host.ctx, "Objects1_Props", name) : u->tmpl;
            int k = unit_new(a, name, t.at ? t : u->tmpl, (Group){ 0 });   /* gWorld.createAttackObject(name) */
            if (k >= 0) {
                ArmyUnit *f = &a->u[k];
                f->is_cop = 1;                       /* setIsFriend(pos.x < 0): the wingman */
                f->side = pos[0] < 0;
                f->friend_of = who(a, u);
                u->friend_of = k;
                heli_init(a, f);                     /* callOnInit */
            }
        }
    }
    weapons_enable(u, 1);
}

static int heli_pulse(Army *a, ArmyUnit *u)
{
    float tic = u->pulse_delay;
    if (u->state != 5) {                             /* processLifespan */
        u->life -= tic;
        if (u->life <= 0)
            u->life_state = 1;
    }
    u->state_time += tic;
    ArmyUnit *f = u->friend_of >= 0 && !u->is_cop ? &a->u[u->friend_of] : NULL;   /* the wingman, if this leads */
    if (f && (f->phase == AU_GONE || f->kind != AK_HELI))
        f = NULL;
    if (u->state == 0) {
        if (u->is_cop && u->sub == 2) {              /* the leader found a monster */
            u->state = 2;
            u->state_time = 0;
            u->target = u->troop_i < a->nm ? u->troop_i : -1;
            u->mv.flags &= ~2u;
            return 1;
        }
        for (int i = 0; i < a->nm; i++) {
            const float *mp = a->m[i].pos;
            if (dist3(mp, u->pos) <= u->aux[2] || fabsf(mp[0] - u->pos[0]) <= u->aux[3]) {
                u->state = 2;
                u->state_time = 0;
                u->target = i;
                u->mv.flags &= ~2u;
                sound(a, u, tstr(u, "VoiceSound"));
                if (f) {                             /* friend.stopFlight(i, 2) */
                    f->troop_i = i;
                    f->sub = 2;
                }
                break;
            }
        }
    } else if (u->state == 2) {
        if (u->is_cop && u->sub == 1) {
            u->state = 1;
            u->state_time = 0;
            return 1;
        }
        if (u->target >= 0 && u->target < a->nm) {
            const float *mp = a->m[u->target].pos;
            if (dist3(mp, u->pos) <= u->aux[2] * 0.6f || fabsf(mp[0] - u->pos[0]) <= u->aux[3] * 0.6f) {
                u->state = 1;
                u->state_time = 0;
                if (unum(u, "IsHoming", 0) != 0) {
                    weapon_ammo(u, 1, 0);
                    weapon_ammo(u, 2, 1);
                }
                if (f)                               /* friend.stopFlight(-1, 1) */
                    f->sub = 1;
            }
        }
        u->tilt[0] *= 0.9f;                          /* facing.x x 0.9 */
    } else if (u->state == 1) {
        u->aux[7] = fmaxf(0, u->aux[7] - u->aux[5] * tic);
        u->mv.speed = u->aux[7];
        if (u->state_time > u->aux[4]) {
            u->state = 3;
            u->target = -1;
        }
    } else if (u->state == 3) {
        u->aux[7] = fminf(u->aux[6], u->aux[7] + u->aux[5] * tic);
        u->mv.speed = u->aux[7];
    } else if (u->state == 5) {
        u->explode_time += tic;
        if (u->pos[2] <= 4) {
            effect_stop(a, (int)u->aux[1]);
            effect_at(a, tstr(u, "HitEffect0"), u->pos);
            effect_at(a, tstr(u, "ExplodeEffect"), u->pos);
            effect_stop(a, (int)u->aux[0]);
            if (u->is_cop) {
                if (u->count)
                    remove_ai(a, u);
            } else if (f)
                f->count = 1;                        /* friend.reportDestruction: the wingman reports for both */
            else
                remove_ai(a, u);
            return DELETE_ME;
        }
    }
    return 1;
}

static int heli_mover(Army *a, ArmyUnit *u, int state)
{
    if (state != 2)
        return 1;
    if (u->life_state) {
        remove_ai(a, u);
        return DELETE_ME;
    }
    u->state = 0;
    if (unum(u, "DeleteAtEnd", 0) != 0) {
        remove_ai(a, u);
        return DELETE_ME;
    }
    heli_path(a, u);
    return 1;
}

static void heli_damage(Army *a, ArmyUnit *u, int monster)
{
    if (u->ihp <= 0 || monster < 0)
        return;
    u->ihp--;
    effect_stop(a, (int)u->aux[1]);
    if (u->ihp == 0 && u->state != 5) {
        effect_stop(a, (int)u->aux[0]);
        const char *d = ustr(u, "DestShape");
        if (d) {
            u->oflags &= ~(uint32_t)OF_ALPHA;
            u->shape = d;
        }
        u->state = 5;
        u->oflags &= ~(uint32_t)(OF_PUSH_OUT | OF_EFF_NODES);
        u->pulse_delay = TIMESTEP;
        wreck_away(u, a->m[monster].pos);
        weapon_ammo(u, 0, 0);
        add_stats(a, u, monster, 2);
    }
    const char *fx = hit_effect_n(u, u->ihp, 1);
    if (fx)
        u->aux[1] = (float)effect_on(a, u, fx, NULL);
}

/* ======== CombatTruck.bsf: 1 made, 2 driving in, 4 dropping troops, 5 leaving, 10 driving off, 6 dying, 7 the
 * police car's skid. is_cop holds isTruck, side bDriveAway, count numTroopTypes */

static int unit_spawn_actor(Army *a, const char *name, const float pos[3]);

static const char *own_name(const ArmyUnit *u)       /* getStringProp("Name"): the AIInstance's, else the template's */
{
    const char *s = ustr(u, "Name");
    return s ? s : u->name;
}

static void truck_init(Army *a, ArmyUnit *u)         /* Construct + onInit */
{
    u->ihp = (int)unum(u, "HitPoints", 2);
    u->side = (int)unum(u, "DriveAway", 1);
    u->aux[0] = 2;                                   /* dieDuration */
    u->is_cop = (int)unum(u, "IsTruck", 0);
    u->oflags |= u->is_cop ? OF_EFF_NODES : OF_EFF_NODES | OF_PICKUP;
    const char *amb = tstr(u, "AmbientEffect");
    u->aux[1] = -1;
    if (amb) {
        float off[3] = { 0, unum(u, "AmbientEffectYOffset", 0), unum(u, "AmbientEffectZOffset", 0) };
        u->aux[1] = (float)effect_on(a, u, amb, off);
    }
    sound(a, u, tstr(u, "EngineSound"));
    u->state_time = 0;
    u->state = 1;
    u->count = 0;
    for (int i = 0; i < 4; i++) {                    /* TroopType1 .. with numTroop<n>Spawned (5) */
        char k[32];
        snprintf(k, sizeof k, "TroopType%d", i + 1);
        if (!ustr(u, k))
            break;
        snprintf(k, sizeof k, "numTroop%dSpawned", i + 1);
        u->troops_left[i] = (int)unum(u, k, 5);
        u->count = i + 1;
    }
    enable_respawn(a, own_name(u), 0);
    /* onInit */
    u->aux[2] = frand(a) > 0.5f ? 1.0f : -1.0f;      /* whichSideOfScreen */
    u->pos[0] = 100 * u->aux[2];
    u->pos[1] = -8;
    u->pos[2] = 0;
    u->facing = PI / (2 * u->aux[2]);
    int m = nearest_monster(a, u->pos);
    float mx = m >= 0 ? a->m[m].pos[0] : 0;
    float end[3] = { mx + 8 * u->aux[2], -8, 0 };
    if (fabsf(end[0]) > 40)
        end[0] = 40 * u->aux[2];
    line_init(u, u->pos, end, 20);
    u->mv.resp2 = unum(u, "MonsterResponseDist", -1);
    u->mv.resp2 *= u->mv.resp2;
    u->state = 2;
    u->pulse_delay = 1;
}

static void truck_damaged_shape(Army *a, ArmyUnit *u)   /* applyDamagedShape */
{
    if (u->flags & 32)
        return;
    if (ustr(u, "DestShape"))
        u->shape = tstr(u, "DestShape") ? tstr(u, "DestShape") : ustr(u, "DestShape");
    effect_on(a, u, ustr(u, "DamageSound"), NULL);
    u->flags |= 32;
}

/* createTroop: an actor of TroopType<n> at the truck, (ExitTruck out of a truck, else Running) to its place at 10;
 * the truck's name saved for the troop's Construct (gAIMgr.setSavedObjectName / clearSavedObjectName) */
static void truck_troop(Army *a, int self, const float to[3], int type, int run_sound)
{
    ArmyUnit *u = &a->u[self];
    char k[32];
    snprintf(k, sizeof k, "TroopType%d", type);
    const char *name = ustr(u, k);
    if (!name)
        return;
    a->saved_name = own_name(u);
    int t = unit_spawn_actor(a, name, u->pos);
    a->saved_name = NULL;
    if (t < 0)
        return;
    ArmyUnit *s = &a->u[t];
    if (u->is_cop) {
        start_anim(a, s, "ExitTruck", 0);
        s->facing = -u->facing;
    } else
        start_anim(a, s, "Running", 1);
    line_init(s, u->pos, to, 10);
    s->mv.flags = 0;
    if (run_sound)
        sound(a, s, ustr(u, "RunSound"));
    sound(a, s, ustr(u, "DeploySound"));
}

static int truck_pulse(Army *a, ArmyUnit *u)
{
    if (u->state == 4) {
        if (!(u->flags & 16)) {
            u->troop_i = 0;                          /* i */
            u->sub = 0;                              /* j */
            u->aux[4] = -5;                          /* y_offset1 */
            u->aux[3] = 2.5f;                        /* x_offset1 */
            if (u->is_cop)
                u->oflags |= OF_PUSH_OUT;
            u->aux[5] = 0;                           /* pushTime */
            u->pulse_delay = 0.5f;
            u->flags |= 16 | 64;
        }
        if (u->sub < u->count) {
            if (u->troop_i < u->troops_left[u->sub]) {
                int mi = a->nm > 0 ? (int)(frand(a) * (float)a->nm) : -1;
                float tp[3] = { 0, 0, 0 };
                if (mi >= 0)
                    memcpy(tp, a->m[mi].pos, sizeof tp);
                u->aux[4] = u->troop_i % 2 == 0 ? -5 : -3.75f;
                if (!u->side)
                    u->aux[4] += 4;
                u->aux[3] += 1.5f * u->aux[2];
                tp[0] = u->pos[0] + 4 * u->aux[2] + u->aux[3];
                tp[1] = u->pos[1] - u->aux[4];
                tp[2] = 0;
                int last = u->sub == u->count - 1 && u->troop_i == u->troops_left[u->sub] - 1;
                truck_troop(a, who(a, u), tp, u->sub + 1, last);
                u->troop_i++;
            }
            if (u->troop_i >= u->troops_left[u->sub]) {
                u->sub++;
                u->troop_i = 0;
            }
        } else {
            u->pulse_delay = 1;
            u->flags &= ~16;
            if (u->is_cop)
                u->state = 5;
            else {
                u->state = 3;
                u->pulse_delay = 0;
            }
        }
    } else if (u->state == 6) {
        u->aux[0] -= u->pulse_delay;
        truck_damaged_shape(a, u);
        if (u->aux[0] < 0.25f && u->aux[0] > 0)
            effect_on(a, u, tstr(u, "DamageEffect"), NULL);
        else if (u->aux[0] < 0) {
            effect_at(a, tstr(u, "ExplodeEffect"), u->pos);
            remove_ai(a, u);
            if (!(u->flags & 64))
                enable_respawn(a, own_name(u), 1);
            return DELETE_ME;
        }
    } else if (u->state == 5) {
        if (u->is_cop)
            u->oflags &= ~(uint32_t)OF_PUSH_OUT;
        float end[3] = { -100 * u->aux[2], -5, 0 };
        line_init(u, u->pos, end, 15);
        sound(a, u, tstr(u, "EngineSound"));
        u->state = 10;
    }
    return 1;
}

static int truck_mover(Army *a, ArmyUnit *u, int state)
{
    if (u->state == 6)
        return 1;
    if (state == 4 && u->is_cop && (u->state == 10 || u->state == 2)) {
        /* pushMonster: a monster on the ground within MonsterResponseDist is pushed along (queuePushRequest) */
        for (int i = 0; i < a->nm; i++) {
            float dd = dist3(a->m[i].pos, u->pos);
            if (dd * dd <= u->mv.resp2 && contains(a->m[i].move, "Ground")) {
                float d[3] = { a->m[i].pos[0] - u->pos[0], a->m[i].pos[1] - u->pos[1], a->m[i].pos[2] - u->pos[2] };
                norm3(d);
                float tx = cosf(u->facing + PI / 2) < 0 ? -1.0f : 1.0f;
                u->aux[5] += TIMESTEP;
                float p[3] = { a->m[i].pos[0], a->m[i].pos[1] + 0.5f, a->m[i].pos[2] };
                if (d[0] * tx > 0.9238795f && u->aux[5] < 0.7f)
                    p[0] += tx * 0.5f;
                ArmyEvent *e = emit(a, AE_PUSH, who(a, u), NULL, p);
                e->monster = i;
            }
        }
    }
    if (state == 2) {
        if (u->state == 2) {
            if (u->is_cop)
                u->oflags |= OF_PUSH_OUT;
            sound(a, u, tstr(u, "StopSound"));
            if (u->side) {
                u->mv.speed = 0;
                u->state = 4;
                u->pulse_delay = 0.25f;
            } else {                                 /* the police car's skid */
                u->facing = 0.8f * PI;
                float end[3] = { u->pos[0] - 3, u->pos[1] + 4, u->pos[2] };
                line_init(u, u->pos, end, 8);
                sound(a, u, tstr(u, "SkidSound"));
                u->state = 7;
            }
        } else if (u->state == 7) {
            u->mv.speed = 0;
            u->oflags |= OF_PICKUP;
            u->pulse_delay = 0.25f;
            u->state = 4;
        } else if (u->state == 10) {
            remove_ai(a, u);
            return DELETE_ME;
        }
    }
    return 1;
}

static void truck_damage(Army *a, ArmyUnit *u, int monster)
{
    if (monster < 0)
        return;
    u->ihp--;
    effect_stop(a, (int)u->aux[1]);
    if (u->ihp <= 0) {
        truck_damaged_shape(a, u);
        effect_on(a, u, tstr(u, "HitEffect2"), NULL);
        add_stats(a, u, monster, 2);
        float d[3] = { u->pos[0] - a->m[monster].pos[0], u->pos[1] - a->m[monster].pos[1], 0 };
        d[2] = fabsf(u->pos[2] - a->m[monster].pos[2]);
        norm3(d);
        for (int k = 0; k < 3; k++)
            u->vel[k] = d[k] * 15;
        u->mv.active = 0;
        u->mv.kind = 2;
        u->cm_on = 0;
        u->pulse_delay = 0.25f;
        u->state = 6;
    } else
        effect_on(a, u, tstr(u, "HitEffect1"), NULL);
}

static void truck_pickup(Army *a, ArmyUnit *u)       /* onPickup */
{
    u->oflags &= ~(uint32_t)(OF_PUSH_OUT | OF_PICKUP);
    truck_damaged_shape(a, u);
    effect_on(a, u, ustr(u, "PickupSound"), NULL);
}

static void truck_thrown(Army *a, ArmyUnit *u, int monster)   /* onThrow */
{
    u->oflags &= ~(uint32_t)OF_PICKUP;
    u->pulse_delay = 0.125f;
    effect_on(a, u, tstr(u, "HitEffect2"), NULL);
    effect_on(a, u, ustr(u, "ThrowSound"), NULL);
    u->state = 6;
    add_stats(a, u, monster, 2);
}

/* ======== Swat.bsf: 2 out of the truck, 4 running, 16 ready, 8 firing, 128 / 256 the grenadier's throw, 32 held,
 * 64 falling back, 512 smashed */

static int group_of(const ArmyUnit *u) { int g = u->group_index; return g < 0 || g > 3 ? 0 : g; }

static void troop_init(Army *a, ArmyUnit *u)         /* Construct */
{
    u->state = 2;
    u->pulse_delay = 0.25f;
    u->oflags |= OF_EFF_NODES | OF_PICKUP;
    u->vehicle = a->saved_name;                      /* vehicleType = gAIMgr.getSavedObjectName() */
    u->is_grenadier = (int)unum(u, "IsGrenadier", 0);
    u->is_cop = (int)unum(u, "IsCop", 0);
    u->group_index = (int)unum(u, "GroupIndex", 0);
    if (!u->is_cop)
        a->instances[group_of(u)]++;
    u->reload = 0;                                   /* reloadDelay */
    u->aux[1] = 0;                                   /* copReloadDelay */
    u->voice[0] = 4;                                 /* voiceFireDelay */
    u->voice[1] = 0;                                 /* voiceArnoldDelay */
    u->voice[2] = 0;                                 /* voiceCopDelay */
    u->flags |= 1024;
    if (!u->is_cop)
        a->flee[group_of(u)] = 0;
    if (!u->mv.active)
        line_init(u, u->pos, u->pos, 0);
    float dt = fabsf(a->now - a->move_ts);
    if (!u->is_cop && a->move_played[group_of(u)] == 0 && dt > 3) {
        sound(a, u, ustr(u, "VoiceMove"));
        a->move_played[group_of(u)] = 1;
        a->move_ts = a->now;
    }
    const char *def = tstr(u, "DefaultAnim");
    if (def)
        start_anim(a, u, def, 0);
}

static void troop_notify(Army *a, ArmyUnit *u)       /* notifyGroup */
{
    if (u->flags & 2048)
        return;
    if (!u->is_cop) {
        int g = group_of(u);
        if (--a->instances[g] <= 0) {
            a->fallback_played[g] = a->fire_played[g] = a->move_played[g] = 0;
            enable_respawn(a, u->vehicle, 1);
        }
    } else
        enable_respawn(a, u->vehicle, 1);
    u->flags |= 2048;
}

static int troop_pulse(Army *a, ArmyUnit *u)
{
    u->voice[1] -= u->pulse_delay;
    if (u->state == 32 || u->state == 64) {
        u->pulse_delay = 1;
        return 1;
    }
    if (u->is_grenadier && u->state != 512)
        u->reload -= u->pulse_delay;
    int g = group_of(u);
    if (a->flee[g] && (u->flags & 1024) && !u->is_cop && u->state != 512) {
        float dt = fabsf(a->fallback_ts - a->now);
        if (a->fallback_played[g] == 0 && dt > 3) {
            sound(a, u, ustr(u, frand(a) > 0.5f ? "VoiceFallback" : "VoiceCodeRed"));
            a->fallback_played[g] = 1;
            a->fallback_ts = a->now;
        }
        u->state = 64;
        int m = nearest_monster(a, u->pos);
        float dir = m >= 0 && u->pos[0] < a->m[m].pos[0] ? -1.0f : 1.0f;
        float r = frand(a);
        float end[3] = { u->pos[0] + dir * r * 100, -10 - 50 * frand(a), 0 };
        start_anim(a, u, "Running", 1);
        line_init(u, u->pos, end, 10);
        u->mv.flags = 1 | 4;
        weapons_enable(u, 0);
        return 1;
    } else if (u->state == 512) {
        troop_notify(a, u);
        return DELETE_ME;
    } else if (u->state == 16 || u->state == 8) {
        u->pulse_delay = 0.25f;
        u->voice[0] -= u->pulse_delay;
        float dt = fabsf(a->fire_ts - a->now);
        if (!u->is_cop && a->fire_played[g] == 0 && u->voice[0] < 0 && dt > 3) {
            sound(a, u, ustr(u, "VoiceFire"));
            a->fire_ts = a->now;
            a->fire_played[g] = 1;
        }
        if (u->is_cop) {
            u->voice[2] -= u->pulse_delay;
            u->aux[1] -= u->pulse_delay;
            if (u->voice[2] < 0) {
                sound(a, u, ustr(u, "VoiceCop"));
                u->voice[2] = unum(u, "DelayCopVoice", 3);
            }
            if (u->aux[1] < 0) {                     /* a clip of 1, 2 or 3 bullets */
                float r = frand(a);
                weapon_ammo(u, r < 0.3f ? 1 : r < 0.6f ? 2 : 3, 0);
                u->aux[1] = unum(u, "CopReloadDelay", 5);
            }
        }
        int m = u->mv.flags & 8 ? mover_target(a, u->pos) : -1;   /* the mover's target */
        float mp[3];
        memcpy(mp, m >= 0 ? a->m[m].pos : u->pos, sizeof mp);
        float dx = mp[0] - u->pos[0];
        if (m >= 0 && fabsf(dx) < tnum(u, "ShootingRange", 20)) {
            if (mp[2] < 10 && !u->is_grenadier) {
                u->target = m;                       /* setWeaponTarget, enableWeapon(1) */
                weapons_enable(u, 1);
                sound(a, u, ustr(u, "ShootSound"));
                u->state = 8;
            }
        } else if (tnum(u, "FollowMonster", 0) != 0) {
            float dest[3] = { u->pos[0], u->pos[1], u->pos[2] };
            float ideal = tnum(u, "IdealShootingRange", 5);
            dest[0] = dx >= 0 ? mp[0] - ideal : mp[0] + ideal;
            line_init(u, u->pos, dest, 10);
            u->mv.flags = 1 | 4;
            start_anim(a, u, "Running", 1);
            u->state = 4;
        } else if (!u->is_grenadier) {
            weapons_enable(u, 0);
            u->state = 16;
        }
    }
    return 1;
}

static int troop_mover(Army *a, ArmyUnit *u, int state)
{
    if (state != 2)
        return 1;
    if (u->state == 64) {
        troop_notify(a, u);
        return DELETE_ME;
    }
    start_anim(a, u, frand(a) < 0.5f ? "StandingGroundFiringAngleU" : "CrouchingGroundFiringAngleU", 1);
    if (u->is_grenadier)
        start_anim(a, u, "Idle", 1);
    sound(a, u, ustr(u, "LoadGunSound"));
    line_init(u, u->pos, u->pos, 0);
    u->mv.flags = 8 | 4;
    u->state = 16;
    return 1;
}

static void troop_anim_done(Army *a, ArmyUnit *u)
{
    int m = u->mv.flags & 8 ? mover_target(a, u->pos) : -1;
    if (u->state == 2) {
        start_anim(a, u, "Running", 1);
        u->state = 4;
    } else if (u->state == 128) {
        if (u->is_grenadier && u->voice[1] < 0) {
            sound(a, u, ustr(u, "VoiceArni"));
            u->voice[1] = unum(u, "DelayArniVoice", 8);
        }
        u->target = m;
        weapons_enable(u, 1);
        start_anim(a, u, "GroundThrowRecover", 1);
        u->state = 256;
    } else if (m >= 0) {
        const float *mp = a->m[m].pos;
        float dx = fabsf(mp[0] - u->pos[0]);
        if (u->state == 256) {
            weapons_enable(u, 0);
            u->state = 16;
            start_anim(a, u, "Idle", 1);
            if (dx < tnum(u, "ShootingRange", 20) && mp[2] < 10 && u->reload <= 0) {
                u->reload = 3;
                start_anim(a, u, "StandingGroundFiringAngleU", 1);
                if (u->is_grenadier)
                    sound(a, u, ustr(u, "VoiceArni"));
                u->state = 128;
            } else {
                start_anim(a, u, "Idle", 1);
                u->state = 16;
            }
        } else if (u->is_grenadier && u->state == 16) {
            /* [R] `reloadDelay <= 0.0 && reloadDelay`: only a reload below 0 passes */
            if (dx < tnum(u, "ShootingRange", 20) && mp[2] < 10 && u->reload < 0) {
                u->reload = 3;
                start_anim(a, u, "StandingGroundFiringAngleU", 1);
                u->state = 128;
            } else {
                start_anim(a, u, "Idle", 1);
                u->state = 16;
            }
        }
    }
}

static void troop_smash(Army *a, ArmyUnit *u, int monster)
{
    smash_object(a, u, monster >= 0 ? a->m[monster].heading : 0);
    sound(a, u, ustr(u, "FlyingSound"));
    u->state = 512;
    u->pulse_delay = 3;
    u->pulse_time = 0;
}

static void troop_pickup(Army *a, ArmyUnit *u)       /* onPickup */
{
    if (u->state == 32)
        return;
    start_anim(a, u, "PickedUp", 1);
    if (!u->is_cop)
        a->flee[group_of(u)] = 1;
    troop_notify(a, u);
    u->state = 32;
    weapons_enable(u, 0);
    sound(a, u, ustr(u, "VoiceGrabbed"));
}

static void troop_damage(Army *a, ArmyUnit *u, int monster)
{
    if (monster < 0 || u->state == 512)
        return;
    if (!u->is_cop)
        a->flee[group_of(u)] = 1;
    weapons_enable(u, 0);
    u->flags &= ~1024;
    troop_smash(a, u, monster);
    /* [R] then onPickup() and PickedUp: the state goes to 32, so a punched troop flies on and its 3 s pulse never
     * deletes it (people.c keeps the same quirk of the window people) */
    troop_pickup(a, u);
    start_anim(a, u, "PickedUp", 1);
}

/* ======== Military_Window.bsf: 1 emerging, 2 looking / firing, 3 going back in, 8 held, 9 smashed */

static const char *const FIRE_DIR[8] = { "RM", "RU", "MU", "LU", "LM", "LD", "MD", "RD" };

static void window_init(Army *a, ArmyUnit *u)        /* Construct */
{
    u->state = 1;
    const char *emerge = ustr(u, "EmergeSound");
    if (emerge) {
        float r = frand(a);
        int m = nearest_monster(a, u->pos);
        float d = m >= 0 ? fabsf(a->m[m].pos[0] - u->pos[0]) : 1e9f;
        if (fabsf(a->now - a->emerge_ts) > 2 && r < 0.2f && d <= 30) {
            sound(a, u, emerge);
            a->emerge_ts = a->now;
        }
    }
    u->reload = 0;                                   /* fReloadTime */
    u->pulse_delay = 0.25f;
    u->count = 0;                                    /* bPlayedLoadSound */
    if (!a->window_boss) {                           /* isFirst / makeBoss (used by nothing else) */
        a->window_first++;
        a->window_boss = 1;
        u->sub = a->window_first;
    } else
        u->sub = 0;
    start_anim(a, u, tstr(u, "DefaultAnim") ? tstr(u, "DefaultAnim") : "WindowEmerge", 0);
}

/* getMonsterDirection (the Actor binding 0x3F -> 0x2B2850) [R]: the eighth of the way to the monster 3 above its feet
 * seen from the window, counter-clockwise from the right in the front's plane (dy unused): RM, RU, MU, LU, LM, LD, MD,
 * RD */
static int monster_direction(const ArmyUnit *u, const float mp[3])
{
    float dx = mp[0] - u->pos[0], dz = mp[2] + 3 - u->pos[2];
    int i = (int)floorf(atan2f(dz, dx) / (PI / 4) + 0.5f);
    return ((i % 8) + 8) % 8;
}

static const ArmyBox *building_at(const Army *a, const float p[3]);

static int window_anim_done(Army *a, ArmyUnit *u)
{
    if (u->state == 1) {
        start_anim(a, u, "WindowLook", 0);
        u->state = 2;
    } else if (u->state == 2) {
        int m = nearest_monster(a, u->pos);
        /* findBuildingByPosition(its place): the building's place, y - 5, against the monster's y [R]; the place is
         * its box's middle [R: the box is laid about it, x +- columns x 5 / 2, y +- depth x 5 / 2 (city.c, the grid
         * 0x2D1A30)] */
        float in[3] = { u->pos[0], u->pos[1] + 0.5f, u->pos[2] };
        const ArmyBox *b = building_at(a, in);
        float by = (b ? (b->min[1] + b->max[1]) / 2 : u->pos[1]) - 5;
        float ydist = m >= 0 ? fabsf(by - a->m[m].pos[1]) : 1e9f;
        if (m >= 0 && ydist <= 6 && (frand(a) < 0.5f || u->state_time < 5)) {
            char k[32];
            snprintf(k, sizeof k, "WindowFiring%s", FIRE_DIR[monster_direction(u, a->m[m].pos)]);
            u->target = m;
            start_anim(a, u, k, 0);
            weapons_enable(u, 1);
            if (u->state_time >= u->reload) {
                weapons_reload(u);
                sound(a, u, ustr(u, "ShootSound"));
                u->reload = u->state_time + 2 + frand(a) * 2;
            }
            if (!u->count) {
                sound(a, u, ustr(u, "LoadGunSound"));
                u->count = 1;
            }
        } else {
            start_anim(a, u, "WindowReEnter", 0);
            u->state = 3;
            weapons_enable(u, 0);
        }
    } else if (u->state == 3) {
        u->state = 0;
        emit(a, AE_WINDOW_CLOSE, -1, NULL, u->pos);  /* closeWindow: it shuts */
        return DELETE_ME;
    }
    return 1;
}

static void window_smash(Army *a, ArmyUnit *u, int monster)
{
    smash_object(a, u, monster >= 0 ? a->m[monster].heading : 0);
    sound(a, u, ustr(u, "FlyingSound"));
    u->state = 9;
    u->pulse_delay = 3;
    u->pulse_time = 0;
}

static void window_pickup(Army *a, ArmyUnit *u)
{
    if (u->state == 8)
        return;
    start_anim(a, u, "PickedUp", 1);
    sound(a, u, tstr(u, "PickupSound"));
    u->state = 8;
    weapons_enable(u, 0);
}

/* ======== StationaryAttackObject.bsf: flags 2 / 1 alive, 4 hopping, 8 exploding, 16 done; it aims at a point that
 * follows the monster with a lag (lTarget: count 1 while it aims there) */

static void turret_init(Army *a, ArmyUnit *u)
{
    (void)a;
    u->oflags |= OF_PUSH_OUT | OF_EFF_NODES;
    u->state_time = 0;
    u->flags = 2;
    u->explode_time = 0;
    u->ihp = (int)unum(u, "HitPoints", 3);
    u->aux[0] = 0;                                   /* fRetargetTime */
    u->sub = 0;                                      /* nTargetState */
    weapons_enable(u, 1);
    u->aux[1] = 0.55f;                               /* fLockTime */
    u->aux[2] = 2;                                   /* refireTime */
    weapon_ammo(u, 0, 0);
    u->aux[3] = 0;                                   /* bVoicePlayedForShot */
    u->aux[4] = -1;
    memcpy(u->lock, u->pos, sizeof u->lock);
    u->count = 0;
}

static void turret_select(Army *a, ArmyUnit *u)      /* selectTarget */
{
    int m = a->nm > 0 ? (int)(frand(a) * (float)a->nm) : -1;
    if (m >= 0) {
        u->target = m;
        u->sub = 2;
        memcpy(u->lock, a->m[m].pos, sizeof u->lock);
        u->count = 0;
    }
}

static int turret_process(Army *a, ArmyUnit *u, float dt)   /* Process, every frame */
{
    u->aux[2] -= dt;
    if (u->aux[2] <= 0.5f && !u->aux[3]) {
        const char *v = ustr(u, "FireSound");
        if (v) {
            sound(a, u, v);
            u->aux[3] = 1;
        }
    } else if (u->aux[2] <= 0) {
        weapon_ammo(u, 1, 0);
        u->aux[2] = unum(u, "WeaponRefireDelay", 5);
        u->aux[3] = 0;
    }
    if (u->flags <= (2 | 1)) {
        if (u->sub == 1) {
            if (u->target >= 0 && u->target < a->nm) {
                float d[2] = { a->m[u->target].pos[0] - u->lock[0], a->m[u->target].pos[1] - u->lock[1] };
                if (sqrtf(d[0] * d[0] + d[1] * d[1]) > 3) {
                    u->aux[0] -= dt;
                    if (u->aux[0] <= 0) {
                        u->sub = 2;
                        turret_select(a, u);
                    }
                } else
                    u->aux[0] = 1.05f;
            } else
                u->sub = 0;
        } else if (u->sub == 2) {
            if (u->target >= 0 && u->target < a->nm) {
                const float *mp = a->m[u->target].pos;
                float d[2] = { mp[0] - u->lock[0], mp[1] - u->lock[1] };
                if (sqrtf(d[0] * d[0] + d[1] * d[1]) < 0.5f) {
                    u->aux[1] -= dt;
                    if (u->aux[1] <= 0) {
                        u->sub = 1;
                        memcpy(u->lock, mp, sizeof u->lock);
                        u->lock[2] += unum(u, "TargetOffsetZ", 3);
                        u->count = 1;                /* setTarget(lTarget) */
                        u->aux[0] = 1.05f;
                    }
                } else {
                    u->aux[1] = 0.55f;
                    memcpy(u->lock, mp, sizeof u->lock);
                }
            } else
                u->sub = 0;
        } else if (u->sub == 0)
            turret_select(a, u);
    } else if (u->flags & 8) {
        u->explode_time += dt;
        if (u->explode_time >= 1.5f) {
            u->flags = 16;
            u->explode_time = 0;
            effect_stop(a, (int)u->aux[4]);
            u->aux[4] = (float)effect_at(a, tstr(u, "HitEffect"), u->pos);
        }
        return 1;
    } else if (u->flags & 16) {
        u->explode_time += dt;
        if (u->explode_time >= 1) {
            remove_ai(a, u);
            return DELETE_ME;
        }
    } else if ((u->flags & 4) && unum(u, "IsSinkable", 0) == 0) {
        u->bounce_vel[2] -= 50 * dt;
        float p[3];
        for (int k = 0; k < 3; k++)
            p[k] = u->pos[k] + u->bounce_vel[k] * dt;
        if (p[2] < 0) {
            if (--u->bounce == 0)
                u->flags &= ~4;
            u->bounce_vel[0] = -5;
            u->bounce_vel[1] = 0;
            u->bounce_vel[2] = 7.5f * (float)u->bounce;
            p[2] = 0;
            effect_on(a, u, "Footfall", NULL);
        }
        memcpy(u->pos, p, sizeof p);
    }
    return 1;
}

static void turret_damage(Army *a, ArmyUnit *u, int monster, const float direction[3])
{
    if (u->ihp <= 0 || monster < 0)
        return;
    u->ihp--;
    if (getenv("RAMPAGE_TRACE"))
        printf("%s hit by monster %d: %d hits left\n", u->name, monster, u->ihp);
    effect_stop(a, (int)u->aux[4]);
    if (!(u->flags & 4)) {
        u->flags |= 4;
        u->bounce_vel[0] = -5;
        u->bounce_vel[1] = 0;
        u->bounce_vel[2] = 20;
        u->bounce = 2;
        if (a->m[monster].pos[0] < u->pos[0])
            u->bounce_vel[0] = 5;
    }
    if (u->ihp == 0 && !(u->flags & 8)) {
        const char *d = ustr(u, "DestShape") ? ustr(u, "DestShape") : tstr(u, "DestShape");
        if (d)
            u->shape = d;
        u->flags = 8;
        u->oflags &= ~(uint32_t)(OF_PUSH_OUT | OF_EFF_NODES);
        weapons_enable(u, 0);
        if (unum(u, "IsSinkable", 0) != 0) {
            float end[3] = { u->pos[0], u->pos[1], u->pos[2] - 10 };
            line_init(u, u->pos, end, unum(u, "SinkRate", 1));
        } else {
            u->vel[0] = direction[0] * 12;
            u->vel[1] = direction[1] * 5;
            u->vel[2] = fabsf(direction[2] * 1.5f);
            u->mv.active = 0;
            u->mv.kind = 2;
            u->cm_on = 0;
        }
        add_stats(a, u, monster, 0);
    }
    const char *fx = hit_effect_n(u, u->ihp, 1);
    if (fx)
        u->aux[4] = (float)(u->ihp == 0 ? effect_at(a, fx, u->pos) : effect_on(a, u, fx, NULL));
    fx = tstr(u, "HitEffect");
    if (!(u->flags & 8) && fx)
        effect_on(a, u, fx, NULL);
}

/* ======== Bomber.bsf: 1 flying over (bombing), 4 turned away, 2 falling, 3 gone */

static void bomber_init(Army *a, ArmyUnit *u)
{
    u->oflags |= OF_EFF_NODES;
    float row = tnum(u, "BuildingRow", 0), dir = -1, ypos = 5, zpos = unum(u, "Height", 42);
    if (row == 1) {
        dir = 1;
        ypos = 15;
        zpos += 5;
    }
    float start[3] = { 120 * dir, ypos, zpos }, end[3] = { -120 * dir, ypos, zpos };
    line_init(u, start, end, 20);
    u->facing = PI / 2 * dir;
    const char *sh = ustr(u, dir == 1 ? "ShadowEffect2" : "ShadowEffect1");
    float off[3] = { 0, 0, -u->pos[2] };
    u->aux[0] = sh ? (float)effect_on(a, u, sh, off) : -1;
    u->ihp = (int)tnum(u, "HitPoints", 1);
    u->state = 1;
    const char *type = tstr(u, "Type");
    u->pulse_delay = same_text(type, "Carpet") ? 1.0f : 0.25f;
    u->aux[4] = 5;                                   /* FlyTime */
    u->life = unum(u, "Lifespan", 1000);
    u->life_state = 0;
    u->count = 0;                                    /* bPlayedSound */
    weapons_enable(u, 1);
}

static int bomber_pulse(Army *a, ArmyUnit *u)
{
    if (u->state == 2)
        return 1;
    if (u->state == 3)
        return DELETE_ME;
    u->life -= u->pulse_delay;
    if (u->life_state == 0 && u->life <= 0)
        u->life_state = 1;
    if (u->state == 1) {
        int m = nearest_monster(a, u->pos);          /* updateBombStatus */
        if (m >= 0) {
            const char *type = tstr(u, "Type");
            const float *mp = a->m[m].pos;
            if ((same_text(type, "Targeted") && fabsf(mp[0] - u->pos[0]) < 4.8f && fabsf(mp[1] - u->pos[1]) <= 15) ||
                same_text(type, "Carpet")) {
                u->target = m;
                weapons_reload(u);
            }
        }
        /* gWorld.getCameraPos().x [R: id 0x41, 0x2AF220: gWorld +0x684, the eye the objects' render distance is
         * taken from (0x290FBC)]: the host sets the view about the camera's x (army_set_view) */
        float mid = (a->view[0] + a->view[1]) / 2;
        if (!u->count && u->pos[0] > mid - 50 && u->pos[0] < mid + 50) {
            u->count = 1;
            sound(a, u, tstr(u, "FlybySound"));
        }
    }
    if (u->state == 4) {                             /* updateFlyingStatus */
        u->aux[4] -= u->pulse_delay;
        if (u->aux[4] <= 0) {
            u->aux[4] = 5;
            u->state = 1;
            float end[3] = { -u->pos[0], u->pos[1], u->pos[2] };
            u->facing = -u->facing;
            line_init(u, u->pos, end, 20);
            effect_stop(a, (int)u->aux[0]);
            float off[3] = { 0, 0, -u->pos[2] };
            u->aux[0] = (float)effect_on(a, u, ustr(u, u->pos[0] >= 0 ? "ShadowEffect2" : "ShadowEffect1"), off);
        }
    }
    return 1;
}

static int bomber_mover(Army *a, ArmyUnit *u, int state)
{
    if (state == 2) {
        if (u->life_state) {
            remove_ai(a, u);
            return DELETE_ME;
        }
        if (u->state == 1)
            u->state = 4;
    }
    return 1;
}

static void bomber_damage(Army *a, ArmyUnit *u, int monster)
{
    if (u->state != 1 || monster < 0)
        return;
    const char *fx = tstr(u, "HitEffect");
    if (--u->ihp != 0) {
        effect_on(a, u, fx, NULL);
        return;
    }
    u->state = 2;
    u->pulse_delay = TIMESTEP;
    float t[3] = { cosf(u->facing + PI / 2) < 0 ? -1.0f : 1.0f, 0.1f, -0.15f };
    for (int k = 0; k < 3; k++)
        u->vel[k] = t[k] * 20;
    u->spin[0] = PI / 4;                             /* setRotation(pi / 4, 0, 0): [R 0x28DD80] pi / 4 a second
                                                      * about the world's x */
    u->mv.active = 0;
    u->mv.kind = 2;
    u->cm_on = 0;
    effect_stop(a, (int)u->aux[0]);
    float o1[3] = { 0, 4, 0 }, o2[3] = { 0, -4, 0 };
    effect_on(a, u, fx, NULL);
    effect_on(a, u, fx, o1);
    effect_on(a, u, fx, o2);
    add_stats(a, u, monster, 2);
    remove_ai(a, u);
}

static void bomber_ground(Army *a, ArmyUnit *u)      /* onHitGround / TakeImpact */
{
    if (u->flags & 1)
        return;
    effect_on(a, u, ustr(u, "DieEffect"), NULL);
    u->pulse_delay = 0.35f;
    u->pulse_time = 0;
    u->flags |= 1;
    u->state = 3;
}

/* ======== Droppable.bsf: falls from a window along three points at 12 per second, turning a quarter on the way */

/* findBuildingByPosition (the binding 0x2AEAA0, 0x2A3D20 with no direction) [R]: of the world's buildings the last
 * whose box holds the point in x and y, ends included; z and the state are not tested, so rubble counts */
static const ArmyBox *building_at(const Army *a, const float p[3])
{
    const ArmyBox *found = NULL;
    for (int i = 0; i < a->nb; i++) {
        const ArmyBox *b = &a->b[i];
        if (p[0] >= b->min[0] && p[0] <= b->max[0] && p[1] >= b->min[1] && p[1] <= b->max[1])
            found = b;
    }
    return found;
}

static void drop_init(Army *a, ArmyUnit *u)          /* onInit + createDropPath + getCellCenter */
{
    float start[3] = { u->pos[0], u->pos[1], u->pos[2] };
    float look[3] = { start[0], start[1] + 3, start[2] };   /* findBuildingByPosition(start + 3 in y) */
    const ArmyBox *b = building_at(a, look);
    spline_init(u, 12);
    u->mv.flags = 0;
    float best = 1000, mp[3] = { 0, 0, 0 };
    for (int i = 0; i < a->nm; i++) {
        float d = fabsf(a->m[i].pos[0] - start[0]);
        if (d < best && a->m[i].pos[2] < start[2]) {
            best = d;
            memcpy(mp, a->m[i].pos, sizeof mp);
        }
    }
    /* getCellCenter: the first column edge (left + 5 i, left = the building's x - columns x 5 / 2) at or right of
     * the drop, else the drop's own x [R]; the building's place is its box's middle (as above) [R] */
    float cell = start[0];
    if (b) {
        float left = (b->min[0] + b->max[0]) / 2 - (float)b->width * 5 / 2;
        for (int i = 0; i < b->width; i++)
            if (left + 5.0f * (float)i >= start[0]) {
                cell = left + 5.0f * (float)i;
                break;
            }
    }
    /* [R] the monster side picks + or - half a column; with no monster below, mp is (0, 0, 0) */
    float end[3] = { mp[0] > start[0] ? cell + 2.5f : cell - 2.5f, start[1] - 5, 1 };
    const ArmyBox *front = building_at(a, end);
    if (front)                                       /* a building in front: it lands on its roof */
        end[2] = (float)front->storeys * 3 + 1.5f + 6;
    float peek[3] = { start[0] + 0.6f * (end[0] - start[0]), start[1] - 3, start[2] + 1.5f };
    spline_add(u, start, 0);
    spline_add(u, peek, dist3(peek, start));
    spline_add(u, end, dist3(end, peek));
    /* p3RotRate: a quarter turn about y over the frames of a fall at 12 from start + 3 in y ... to z 1 */
    float frames = (start[2] - 1) / 12 / TIMESTEP;
    if (frames == 0)
        frames = 0.01f;
    u->spin[1] = (PI / 2) / frames / TIMESTEP;       /* per frame in the game (Process), per second here */
    if (frand(a) > 0.5f)
        u->spin[1] = -u->spin[1];
}

/* TakeDamage(monster): within 2.5 along x the monster takes it on the head: HitEffect, HitMonsterSound,
 * setReaction(19), gone [R]. When the game calls it [R 0x2E22B0]: the monster's object test, each frame one of its
 * collision spheres meets the droppable's shape (armed or not), and an attack that reaches it; here the same
 * (touches, army_hit), with the box against its place for a monster without spheres */
static int drop_damage(Army *a, ArmyUnit *u, int monster)
{
    if (monster < 0 || fabsf(a->m[monster].pos[0] - u->pos[0]) >= 2.5f)
        return 1;
    effect_at(a, ustr(u, "HitEffect"), u->pos);
    sound(a, u, ustr(u, "HitMonsterSound"));
    ArmyEvent *e = emit(a, AE_REACTION, who(a, u), u->name, u->pos);
    e->monster = monster;
    e->level = 19;
    return DELETE_ME;
}

/* ---------------------------------------------------------------- the AttackMover (tanks) */

/* [R] init 0x314B40: Velocity the most speed (+0x50), ApproachRange^2 (+0x44, 625 unless set), FleeRange^2 (+0x48,
 * 225), MovementSound (+0x3C); acceleration 8 (+0x4C), 10 driven on before a turn (+0x54), turning at pi / 2 a
 * second (+0x58); the way it goes (+0x6C) +1 when its facing is <= 0 (facing -pi / 2 looks along +x) */
static void attack_init(Army *a, ArmyUnit *u)
{
    if (u->am.tread >= 0)                            /* the script's tread sound stops */
        emit(a, AE_SOUND, who(a, u), ustr(u, "MovementSound"), u->pos)->level = 0;
    u->mv.active = 0;
    u->mv.kind = 3;
    u->am.vmax = unum(u, "Velocity", 0);
    float r = unum(u, "ApproachRange", 25);
    u->am.app2 = r * r;
    r = unum(u, "FleeRange", 15);
    u->am.flee2 = r * r;
    u->am.speed = 0;
    u->am.timer = 0;
    u->am.state = 1;
    u->am.sign = u->facing <= 0 ? 1.0f : -1.0f;
    u->am.driven = 0;
    u->am.tread = 0;
    u->am.target = -1;
}

/* [R] the update 0x313F80, each frame:
 *  - the target (0x313DB0): the monster in front (along the way it goes) nearest along x, else the one it had, else
 *    monster 0; it is the weapons' target too (0x314EC0, the WeaponSystem's +4);
 *  - nothing while the tank is off the street (z >= 0.1: the hop when hit);
 *  - state 1: a target behind turns it round (state 2, to its facing - pi x the way); otherwise by dx^2 (along x
 *    only): farther than ApproachRange, after 2 s (+0x68 counting up) it speeds up by 8 / s to Velocity; nearer than
 *    FleeRange, after the count runs back to 0, it backs off down to -Velocity; between, it brakes to 0;
 *  - it moves along its own forward axis only; a step towards a monster within 10 (and 5 in y) is not taken;
 *  - state 2: it drives on 10 (speeding up, no cap), then turns on the spot at pi / 2 a second, and goes the other
 *    way (state 1);
 *  - the MovementSound plays while |speed| > 0.01 */
static void attack_step(Army *a, ArmyUnit *u, float dt)
{
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < a->nm; i++) {
        float dx = a->m[i].pos[0] - u->pos[0];
        if (dx * u->am.sign > 0 && fabsf(dx) < bd) {
            bd = fabsf(dx);
            best = i;
        }
    }
    if (best < 0)
        best = u->am.target >= 0 && u->am.target < a->nm ? u->am.target : a->nm > 0 ? 0 : -1;
    u->am.target = best;
    u->target = best;
    if (u->pos[2] >= 0.1f || best < 0)
        return;
    float fwd[2] = { -sinf(u->facing), cosf(u->facing) };
    float dx = a->m[best].pos[0] - u->pos[0];
    if (u->am.state == 1) {
        if (dx * u->am.sign < 0) {
            u->am.state = 2;
            u->am.goal = u->facing - PI * u->am.sign;
            u->am.driven = 0;
        } else {
            float d2 = dx * dx;
            if (d2 > u->am.app2) {
                u->am.timer = fminf(2, u->am.timer + dt);
                if (u->am.timer >= 2)
                    u->am.speed = fminf(u->am.vmax, u->am.speed + 8 * dt);
            } else if (d2 < u->am.flee2) {
                u->am.timer = fmaxf(0, u->am.timer - dt);
                if (u->am.timer <= 0)
                    u->am.speed = fmaxf(-u->am.vmax, u->am.speed - 8 * dt);
            } else {
                float s = 8 * dt;
                u->am.speed = fabsf(u->am.speed) <= s ? 0 : u->am.speed - (u->am.speed > 0 ? s : -s);
            }
        }
    }
    if (u->am.state == 2) {
        if (u->am.driven < 10) {
            u->am.speed += 8 * dt;
            u->am.driven += fabsf(u->am.speed * dt);
        } else {
            u->am.speed = 0;
            float d = u->am.goal - u->facing, s = PI / 2 * dt;
            if (fabsf(d) <= s) {
                u->facing = u->am.goal;
                while (u->facing > PI) u->facing -= 2 * PI;
                while (u->facing < -PI) u->facing += 2 * PI;
                u->am.sign = -u->am.sign;
                u->am.state = 1;
                u->am.timer = 0;
            } else
                u->facing += d > 0 ? s : -s;
        }
    }
    float step[2] = { fwd[0] * u->am.speed * dt, fwd[1] * u->am.speed * dt };
    int blocked = 0;
    for (int i = 0; i < a->nm; i++) {
        float mx = a->m[i].pos[0] - u->pos[0];
        if (dist3(a->m[i].pos, u->pos) < 10 && fabsf(a->m[i].pos[1] - u->pos[1]) < 5 && mx * step[0] > 0)
            blocked = 1;
    }
    if (!blocked) {
        u->pos[0] += step[0];
        u->pos[1] += step[1];
    }
    int moving = fabsf(u->am.speed) > 0.01f;
    if (moving != u->am.tread) {                     /* 0x287860 / 0x2877A0 */
        u->am.tread = moving;
        emit(a, AE_SOUND, who(a, u), ustr(u, "MovementSound"), u->pos)->level = moving;
    }
}

/* ---------------------------------------------------------------- the units' lives */

static void unit_init(Army *a, ArmyUnit *u)
{
    switch (u->kind) {
    case AK_TANK: tank_init(a, u); break;
    case AK_HELI: heli_init(a, u); break;
    case AK_TRUCK: truck_init(a, u); break;
    case AK_TROOP: troop_init(a, u); break;
    case AK_WINDOW: window_init(a, u); break;
    case AK_TURRET: turret_init(a, u); break;
    case AK_BOMBER: bomber_init(a, u); break;
    case AK_DROP: drop_init(a, u); break;
    default: {
        const char *def = tstr(u, "DefaultAnim");
        if (def)
            start_anim(a, u, def, 1);
    } break;
    }
    unit_matrix(u);
}

static int unit_spawn_actor(Army *a, const char *name, const float pos[3])   /* gWorld.createActor(name) */
{
    Group t = a->host.find ? a->host.find(a->host.ctx, "Objects1_Props", name) : (Group){ 0 };
    if (!t.at)
        return -1;
    int k = unit_new(a, name, t, (Group){ 0 });
    if (k < 0)
        return -1;
    memcpy(a->u[k].pos, pos, 3 * sizeof(float));
    unit_init(a, &a->u[k]);
    return k;
}

static int on_mover(Army *a, ArmyUnit *u, int state)
{
    switch (u->kind) {
    case AK_TANK:
        if (state == 2) {
            remove_ai(a, u);
            return DELETE_ME;
        }
        return 1;
    case AK_HELI: return heli_mover(a, u, state);
    case AK_TRUCK: return truck_mover(a, u, state);
    case AK_TROOP: return troop_mover(a, u, state);
    case AK_BOMBER: return bomber_mover(a, u, state);
    case AK_TURRET: return 1;                        /* no onMoverCallback: the sinking ends where it is; Process
                                                      * deletes it 2.5 s after the last hit (flags 8, then 16) */
    case AK_DROP:
        if (state == 2) {
            effect_at(a, ustr(u, "GroundEffect"), u->pos);
            return DELETE_ME;
        }
        return 1;
    default: return 1;
    }
}

static int on_pulse(Army *a, ArmyUnit *u)
{
    switch (u->kind) {
    case AK_TANK: return tank_pulse(a, u);
    case AK_HELI: return heli_pulse(a, u);
    case AK_TRUCK: return truck_pulse(a, u);
    case AK_TROOP: return troop_pulse(a, u);
    case AK_WINDOW:
        u->state_time += 0.25f;
        return u->state == 9 ? DELETE_ME : 1;
    case AK_BOMBER: return bomber_pulse(a, u);
    default: return 1;
    }
}

/* [R 0x2E22B0] a droppable meets a monster when one of its collision spheres, armed or not, meets the droppable's
 * shape (0x28FD70: the Droppable_* books set none, so the model's sphere). A monster without spheres: its box
 * against the unit's place */
static int touches(const Army *a, const ArmyMonster *m, ArmyUnit *u)
{
    for (int s = 0; s < m->nspheres; s++)
        if (unit_meets(a, u, m->spheres[s], m->spheres[s][3]))
            return 1;
    const float *p = u->pos;
    if (m->nspheres == 0 && m->box_max[2] > m->box_min[2])
        return p[0] >= m->box_min[0] && p[0] <= m->box_max[0] && p[1] >= m->box_min[1] && p[1] <= m->box_max[1] &&
               p[2] >= m->box_min[2] && p[2] <= m->box_max[2];
    return 0;
}

static void unit_update(Army *a, int i, float dt)
{
    ArmyUnit *u = &a->u[i];
    if (u->phase == AU_GONE)
        return;
    if (u->phase == AU_HELD) {                       /* the HeldObjectMover: army_hold places it */
        u->anim_time += dt;
        return;
    }
    int r = 1;
    /* the mover */
    if (u->mv.kind != 2)
        u->cm_on = 0;                                /* a flight is over once another mover takes it */
    if (u->mv.kind == 2)
        r = sphere_step(a, u, dt) ? -2 : 1;          /* -2: OnHitGround (script event 11): once a frame here, every
                                                      * sub-step in the game (0x28E440) [R: the same for the two
                                                      * scripts that have one: CombatTruck's sets state 6, Bomber's
                                                      * is guarded] */
    else if (u->mv.kind == 3)
        attack_step(a, u, dt);
    else {
        int cb = mover_step(a, u, dt);
        if (cb >= 2)
            r = on_mover(a, u, cb);
    }
    if (r == -2) {
        r = 1;
        if (u->kind == AK_TRUCK)
            u->state = 6;
        else if (u->kind == AK_BOMBER)
            bomber_ground(a, u);
    }
    /* the look: a play that does not repeat ends in onAnimComplete */
    if (r != DELETE_ME && u->anim[0]) {
        u->anim_time += dt;
        if (u->anim_len > 0 && u->anim_time >= u->anim_len) {
            if (u->anim_loop)
                u->anim_time = fmodf(u->anim_time, u->anim_len);
            else if (!u->anim_done) {
                u->anim_done = 1;
                u->anim_time = u->anim_len;
                if (u->kind == AK_TROOP)
                    troop_anim_done(a, u);
                else if (u->kind == AK_WINDOW)
                    r = window_anim_done(a, u);
            }
        }
    }
    /* the pulse (setPulseDelay; 0: every frame) */
    if (r != DELETE_ME) {
        u->pulse_time += dt;
        if (u->pulse_time >= u->pulse_delay) {
            u->pulse_time = u->pulse_delay > 0 ? fmodf(u->pulse_time, u->pulse_delay) : 0;
            r = on_pulse(a, u);
        }
    }
    if (r != DELETE_ME && u->kind == AK_TURRET)
        r = turret_process(a, u, dt);
    /* a droppable meeting a monster */
    if (r != DELETE_ME && u->kind == AK_DROP)
        for (int m = 0; m < a->nm && r != DELETE_ME; m++)
            if (touches(a, &a->m[m], u))
                r = drop_damage(a, u, m);
    if (r == DELETE_ME) {
        unit_remove(a, u);
        return;
    }
    if (u->kind == AK_DROP)
        u->tilt[1] += u->spin[1] * dt;               /* Process: rotate(p3RotRate) every frame */
    float d[3];
    int flight = u->mv.kind == 2 && u->cm_on;
    if (army_osc_step(&u->osc, dt, d)) {             /* [R 0x314EF0] the wobble, after the update (0x309AC0) */
        if (flight) {                                /* onto the mover's frame, which it steps on from */
            army_osc_turn(u->world, d);
            collide_matrix_from16(u->cmat, u->world);
        } else {                                     /* the facing: kept, it carries the whole wobble; a spline's
                                                      * frame made afresh only this frame's change */
            u->tilt[0] += d[0];
            u->tilt[1] += d[1];
            u->facing += d[2];
        }
    }
    if (!flight)                                     /* a flight's matrix is its mover's */
        unit_matrix(u);
    if (u->phase == AU_ALIVE && u->nw > 0)
        unit_weapons_tick(a, u, dt, u->kind == AK_TURRET && u->count ? u->lock : NULL);
}

/* ---------------------------------------------------------------- the API */

#define MAX_UNITS 160

Army *army_new(const ArmyHost *host)
{
    Army *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->u = calloc(MAX_UNITS, sizeof *a->u);
    if (!a->u) {
        free(a);
        return NULL;
    }
    a->cap = MAX_UNITS;
    if (host)
        a->host = *host;
    a->view[0] = -45;                                /* until the host sets it: the street about x = 0 */
    a->view[1] = 45;
    a->fallback_ts = a->fire_ts = a->move_ts = -3;   /* [R] the scripts' sfx_*PrevTS = -3 */
    a->emerge_ts = -2;
    return a;
}

void army_free(Army *a)
{
    if (!a)
        return;
    free(a->u);
    free(a);
}

void army_set_view(Army *a, float xmin, float xmax) { a->view[0] = xmin; a->view[1] = xmax; }

void army_set_collide(Army *a, CollideWorld *w) { a->cw = w; }

void army_collide_add(Army *a, CollideWorld *w)
{
    for (int i = 0; i < a->n; i++) {
        ArmyUnit *u = &a->u[i];
        u->cobj = -1;
        if (!w || u->phase == AU_GONE)
            continue;
        CollideShape sh;
        float m[12];
        unit_shape(a, u, &sh);
        collide_matrix_from16(m, u->world);
        uint32_t fl = COLLIDE_F_WORLD | (u->oflags & ~(uint32_t)(OF_HELD | OF_MONSTER));
        /* [R 0x2DA310] a held object stays in the world with its shape: PushOutMonsters cleared (the monster's box no longer
         * meets it), flying objects and shots still do (flag 4 stays: no removeObject; a shot's own owner is no hit) */
        if (u->phase == AU_HELD)
            fl &= ~(uint32_t)COLLIDE_F_PUSHOUT;
        u->cobj = collide_add(w, &sh, m, fl, COLLIDE_TAG(COLLIDE_OWN_ARMY, i), 0);
        if (u->mv.kind == 2 && u->cm_on)             /* flying: another flying object exchanges impulses with it (0x285E90) */
            collide_set_mover(w, u->cobj, &u->cm, u->cmat, u->vel);
    }
}

/* TakeImpact (the world's 0x287430, vtable +0x38): Bomber.bsf's is onHitGround's; no other script of ours has one */
static void unit_take_impact(Army *a, ArmyUnit *u)
{
    if (u->phase != AU_GONE && u->kind == AK_BOMBER)
        bomber_ground(a, u);
}

void army_impact(Army *a, int unit)
{
    if (unit >= 0 && unit < a->n)
        unit_take_impact(a, &a->u[unit]);
}

void army_clear(Army *a)
{
    a->n = 0;
    a->nshots = 0;
    a->nev = 0;
    memset(a->person, 0, sizeof a->person);
    memset(a->chain, 0, sizeof a->chain);
    memset(a->flee, 0, sizeof a->flee);
    memset(a->instances, 0, sizeof a->instances);
}

static void frame_inputs(Army *a, const ArmyMonster *m, int nm, const ArmyBox *b, int nb)
{
    a->m = m;
    a->nm = m ? (nm < ARMY_MONSTERS ? nm : ARMY_MONSTERS) : 0;
    a->b = b;
    a->nb = b ? nb : 0;
}

int army_spawn(Army *a, const char *tmpl_name, const float pos[3], float facing)
{
    Group t = a->host.find ? a->host.find(a->host.ctx, "Objects1_Props", tmpl_name) : (Group){ 0 };
    if (!t.at)
        return -1;
    int k = unit_new(a, tmpl_name, t, (Group){ 0 });
    if (k < 0)
        return -1;
    if (pos)
        memcpy(a->u[k].pos, pos, sizeof a->u[k].pos);
    a->u[k].facing = facing;
    unit_init(a, &a->u[k]);
    return k;
}

int army_spawn_window(Army *a, const char *tmpl_name, const float pos[3])
{
    return army_spawn(a, tmpl_name, pos, PI);        /* facing the street (-y) */
}

int army_drop(Army *a, const char *tmpl_name, const float from[3], const ArmyMonster *m, int nm, const ArmyBox *b,
              int nb)
{
    frame_inputs(a, m, nm, b, nb);
    return army_spawn(a, tmpl_name, from, 0);
}

/* ---- the people's weapons (PE_WEAPON: the street punk's bottle) */

/* the person's weapon: its own (who + 1), else (make) a free one */
static PersonGun *person_gun(Army *a, int person, int make)
{
    PersonGun *free = NULL;
    for (int i = 0; i < MAX_PERSONS; i++) {
        if (a->person[i].who == person + 1)
            return &a->person[i];
        if (!free && !a->person[i].who)
            free = &a->person[i];
    }
    if (!make)
        return NULL;
    if (!free) {                                     /* all taken: one whose weapon is off */
        for (int i = 0; i < MAX_PERSONS && !free; i++)
            if (!a->person[i].on)
                free = &a->person[i];
        if (!free)
            return NULL;
    }
    memset(free, 0, sizeof *free);
    free->who = person + 1;
    return free;
}

void army_person_forget(Army *a, int person)
{
    PersonGun *g = person >= 0 ? person_gun(a, person, 0) : NULL;
    if (g)
        memset(g, 0, sizeof *g);
}

void army_person_weapon(Army *a, int person, const char *weapon, const float person_world[16], int target_monster)
{
    if (person < 0)
        return;
    PersonGun *g = person_gun(a, person, weapon != NULL);
    if (!g)
        return;
    if (!weapon) {                                   /* enableWeapon(0): it keeps counting down (0x30EC10) */
        g->on = 0;
        g->w.on = 0;
        return;
    }
    if (!g->has || !same_text(g->w.name, weapon)) {
        Group book = a->host.find ? a->host.find(a->host.ctx, "Weapons", weapon) : (Group){ 0 };
        memset(&g->w, 0, sizeof g->w);
        g->has = 0;
        if (book.at) {
            /* the weapon as weapon_make builds it, from the book's group alone (people.c passes the name only) */
            ArmyWeapon *w = &g->w;
            const char *type = group_str(book, "WeaponType");
            w->type = type && same_text(type, "RBeamWeapon") ? 1 : type && same_text(type, "RProjectileWeapon") ? 0 : 2;
            w->name = weapon;
            w->range = 10;
            w->damage_type = 11;
            w->start_ammo = -1;
            w->accel = 1e8f;
            w->init_vel = 1;
            w->axis = 2;
            w->vis_range = 1;
            w->reaction = -1;
            weapon_read(w, book);
            if (w->ammo_max != 0)
                w->def_flags |= 2;
            w->ammo = w->start_ammo >= 0 ? w->start_ammo : w->ammo_max;
            w->scale = 1;
            w->timer = w->charge_time + w->firing_offset;
            w->shot = -1;
            g->has = 1;
        }
    }
    g->on = 1;
    g->w.on = 1;
    g->monster = target_monster;
    if (person_world)
        memcpy(g->world, person_world, sizeof g->world);
}

void army_person_moved(Army *a, int person, const float person_world[16])
{
    PersonGun *g = person >= 0 && person_world ? person_gun(a, person, 0) : NULL;
    if (g)
        memcpy(g->world, person_world, 16 * sizeof(float));
}

/* [R 0x30EC10] the actor's weapon is updated every frame, enabled or not (its vtable +0x6C with the enable byte
 * +0x0): a disabled one counts down to its floor (ChargeTime + FiringOffset, weapon_tick) and does not fire */
static void persons_update(Army *a, float dt)
{
    for (int i = 0; i < MAX_PERSONS; i++) {
        PersonGun *g = &a->person[i];
        if (!g->who || !g->has)
            continue;
        int m = g->monster >= 0 && g->monster < a->nm ? g->monster : -1;
        float from[3] = { g->world[12], g->world[13], g->world[14] + 1.5f }, tp[3] = { 0, 0, 0 }, dir[3];
        if (m >= 0)
            monster_target(&a->m[m], tp);
        lob(from, tp, g->w.velocity, 9.81f * g->w.gravity, dir);   /* the throw's aim: see weapon_aim */
        weapon_tick(a, NULL, &g->w, g->on && m >= 0 && dist3(from, tp) <= g->w.range, from, dir, NULL, m, dt);
    }
}

void army_update(Army *a, float dt, const ArmyMonster *m, int nm, const ArmyBox *b, int nb)
{
    frame_inputs(a, m, nm, b, nb);
    a->dt = dt;
    a->now += dt;
    spawner_update(a, dt);
    for (int i = 0; i < a->n; i++)                   /* (units made this frame are run too) */
        unit_update(a, i, dt);
    persons_update(a, dt);
    for (int i = 0; i < a->nshots; i++)
        if (a->shot[i].alive && !a->shot[i].beam)
            shot_update(a, &a->shot[i], dt);
    while (a->n > 0 && a->u[a->n - 1].phase == AU_GONE)
        a->n--;
}

int army_count(const Army *a) { return a->n; }
const ArmyUnit *army_unit(const Army *a, int i) { return i >= 0 && i < a->n ? &a->u[i] : NULL; }
int army_shots(const Army *a) { return a->nshots; }
const ArmyShot *army_shot(const Army *a, int i) { return i >= 0 && i < a->nshots ? &a->shot[i] : NULL; }

int army_events(Army *a, ArmyEvent *out, int max)
{
    int n = a->nev < max ? a->nev : max;
    if (out && n > 0)
        memcpy(out, a->ev, (size_t)n * sizeof *out);
    a->nev = 0;
    return n;
}

/* ---- the monster's side */


/* TakeDamage(monster, amount, type, direction) of each unit an armed attack sphere reaches */

/* [R 0x2E22B0] an armed sphere: TakeDamage(monster, the attack's damage, 0); every unit but a droppable has
 * OnlyHitWithEffNodes (0x8000, set by its script) and goes on the attack's hit list, one blow an attack */
int army_hit(Army *a, int monster, const float centre[3], float radius, float amount, unsigned record)
{
    if (monster < 0 || monster >= a->nm)
        return 0;
    int n = 0;
    for (int i = 0; i < a->n; i++) {
        ArmyUnit *u = &a->u[i];
        if (u->phase != AU_ALIVE || !unit_meets(a, u, centre, radius))
            continue;
        if (u->kind != AK_DROP && !collide_hit_once(&u->hits, monster, record))
            continue;
        float dir[3] = { u->pos[0] - a->m[monster].pos[0], u->pos[1] - a->m[monster].pos[1],
                         u->pos[2] - a->m[monster].pos[2] };
        norm3(dir);
        n++;
        switch (u->kind) {
        case AK_TANK: tank_damage(a, u, monster, amount); break;
        case AK_HELI: heli_damage(a, u, monster); break;
        case AK_TRUCK: truck_damage(a, u, monster); break;
        case AK_TROOP: troop_damage(a, u, monster); break;
        case AK_TURRET: turret_damage(a, u, monster, dir); break;
        case AK_BOMBER: bomber_damage(a, u, monster); break;
        case AK_DROP:
            if (drop_damage(a, u, monster) == DELETE_ME)
                unit_remove(a, u);
            break;
        default: n--; break;                         /* Military_Window has no TakeDamage */
        }
    }
    return n;
}

int army_nearest_pickup(const Army *a, const float hand[3], const float root[3], const float forward[3], float radius)
{
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < a->n; i++) {
        const ArmyUnit *u = &a->u[i];
        if (u->phase != AU_ALIVE || !(u->oflags & OF_PICKUP) || (u->oflags & OF_HELD))
            continue;
        float d = dist3(u->pos, hand);
        float f = (u->pos[0] - root[0]) * forward[0] + (u->pos[1] - root[1]) * forward[1] +
                  (u->pos[2] - root[2]) * forward[2];
        if (d <= radius && f > 0 && d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* [R] the hold (0x2DA310): BeingHeld, not Pickupable nor PushOutMonsters, the script's onPickup */
void army_grab(Army *a, int unit, int monster)
{
    (void)monster;
    if (unit < 0 || unit >= a->n || a->u[unit].phase != AU_ALIVE)
        return;
    ArmyUnit *u = &a->u[unit];
    u->oflags = (u->oflags | OF_HELD) & ~(uint32_t)(OF_PICKUP | OF_PUSH_OUT);
    u->phase = AU_HELD;
    u->mv.active = 0;
    if (u->mv.kind != 1)
        u->mv.kind = 0;
    weapons_enable(u, 0);
    if (u->kind == AK_TROOP)
        troop_pickup(a, u);
    else if (u->kind == AK_WINDOW)
        window_pickup(a, u);
    else if (u->kind == AK_TRUCK)
        truck_pickup(a, u);
}

/* [R] the HeldObjectMover (init 0x310ED0, held 0x3109E0), as people.c's: the unit's MajorAxis (default 2) along the
 * hand's z, the next one along its y, the last their cross product; at the hand plus HoldOffset (MirroredHoldOffset
 * while the monster plays mirrored) in the hand's frame. [G: its blend in from where the unit was (+0x84, 0x310B50:
 * 0.1 s from doGrabObject) is not ported; the unit is in the hand at once] */
void army_hold(Army *a, int unit, const float hand[16], int mirrored)
{
    if (unit < 0 || unit >= a->n || a->u[unit].phase != AU_HELD)
        return;
    ArmyUnit *u = &a->u[unit];
    int major = (int)tnum(u, "MajorAxis", 2);
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
    Group og = group_sub(u->tmpl, mirrored ? "MirroredHoldOffset" : "HoldOffset");
    Record r[3];
    if (og.at && group_items(og, r, 3) == 3)
        for (int k = 0; k < 3; k++)
            off[k] = record_num(r[k]);
    for (int c = 0; c < 3; c++) {
        memcpy(u->world + 4 * c, axis[c], sizeof axis[c]);
        u->world[4 * c + 3] = 0;
    }
    for (int k = 0; k < 3; k++)
        u->world[12 + k] = u->pos[k] = hand[12 + k] + off[0] * hand[k] + off[1] * hand[4 + k] + off[2] * hand[8 + k];
    u->world[15] = 1;
}

/* [R] throwObject (0x2D9DC0): onThrow(monster, eat) always; an actor eaten is counted (addStats type 1; a fart one
 * time in five, 2 s later) and gone, an actor not eaten is smashed; an object gets the clip's velocity */
void army_throw(Army *a, int unit, int monster, int eat, const float vel[3], float monster_heading)
{
    if (unit < 0 || unit >= a->n || a->u[unit].phase != AU_HELD)
        return;
    ArmyUnit *u = &a->u[unit];
    u->oflags &= ~(uint32_t)OF_HELD;
    u->phase = AU_ALIVE;
    if (u->kind == AK_TROOP || u->kind == AK_WINDOW) {
        if (eat) {
            add_stats(a, u, monster, 1);
            if (frand(a) <= 0.2f) {
                ArmyEvent *e = emit(a, AE_FART, who(a, u), NULL, u->pos);
                e->monster = monster;
                e->amount = 2;
            }
            if (u->kind == AK_TROOP)
                troop_notify(a, u);
            unit_remove(a, u);
        } else if (u->kind == AK_TROOP)
            troop_smash(a, u, monster);
        else
            window_smash(a, u, monster);
        (void)monster_heading;
        return;
    }
    if (u->kind == AK_TRUCK)
        truck_thrown(a, u, monster);
    if (vel)
        memcpy(u->vel, vel, sizeof u->vel);
    u->mv.active = 0;
    u->mv.kind = 2;
    u->cm_on = 0;
}

/* [R] the monster's weapon hit 0x2D6A50 (from 0x2D9650): the slot it starts, see army.h */
int army_reaction_slot(int level, const float knock[3], int mode, int behind)
{
    if (level < 0 || level == 4)
        return -1;
    if (mode == 2)
        return behind ? 44 : 42;
    if (mode == 1)
        return behind ? 45 : 43;
    if (mode != 0 && mode != 5)
        return -1;
    float l = len3(knock), z = l > 0 ? knock[2] / l : 0;
    if (z > 0.707f)
        return (behind ? 27 : 24) + level;
    if (z < -0.707f)
        return (behind ? 21 : 18) + level;
    if (level == 2 && knock[2] <= 0.001f)
        return behind ? 72 : 71;
    return (behind ? 3 : 0) + level;
}

int army_hit_behind(const float monster_pos[3], float monster_heading, const float at[3])
{
    float fwd[2] = { -sinf(monster_heading), cosf(monster_heading) };
    return (at[0] - monster_pos[0]) * fwd[0] + (at[1] - monster_pos[1]) * fwd[1] < 0;
}
