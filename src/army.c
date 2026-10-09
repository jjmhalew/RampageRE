/* army.c - the army, its weapons, the droppables and the score (army.h; docs/ARMY_NOTES.md).
 * [R] marks what is read from the game's code or scripts, [G] what is a guess or a stand-in. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "army.h"

#define PI 3.14159265f
#define TIMESTEP (1.0f / 60.0f)                      /* [G] gWorld.getTimeStep(): one frame */
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

typedef struct { int on, monster; float world[16]; ArmyWeapon w; int has; } PersonGun;
typedef struct { float step, sum; int skip; } Chain; /* a building's +0x194, +0x198, +0x19C [R] */

struct Army {
    ArmyHost host;
    ArmyUnit *u; int n, cap;
    ArmyShot shot[MAX_SHOTS]; int nshots, next_shot;
    ArmyEvent ev[MAX_EVENTS]; int nev, next_fx;
    uint32_t rng;
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
    float view[2];                                   /* [G] the street the camera sees, along x */
};

/* ---------------------------------------------------------------- small things */

static float frand(Army *a)                          /* [G] gWorld.frand(): [0, 1), not the game's Park-Miller (0x106BD0) */
{
    a->rng = a->rng * 1664525u + 1013904223u;
    return (float)(a->rng >> 8) / 16777216.0f;
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
static float dist3(const float p[3], const float q[3])
{
    float d[3] = { p[0] - q[0], p[1] - q[1], p[2] - q[2] };
    return len3(d);
}

/* a property as getStringProp / getFloatProp find it: the instance book (obj +0x7C: the AIInstance group) first,
 * then the template (+0x80) [R: 0x2C51A0 does this]. The books keep a record's type before a '|' ("Effect|HitEffect",
 * "Sound|VoiceMove", "OTRef|TroopType1"); the scripts ask by the bare name, so the typed names are tried too [G] */
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

static int nearest_monster(const Army *a, const float pos[3])   /* getNearestMonster */
{
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < a->nm; i++) {
        float d = dist3(a->m[i].pos, pos);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

static void monster_centre(const ArmyMonster *m, float c[3])   /* [G] where a shot aims: the box's middle */
{
    if (m->box_max[2] > m->box_min[2])
        for (int k = 0; k < 3; k++)
            c[k] = (m->box_min[k] + m->box_max[k]) / 2;
    else {
        c[0] = m->pos[0];
        c[1] = m->pos[1];
        c[2] = m->pos[2] + 4.25f;                    /* [R] George's box: 4.25 up (MONSTER_DATA +0x19C) */
    }
}

/* a unit's world matrix: its place, turned by facing about z, then by tilt (x, y) */
static void unit_matrix(ArmyUnit *u)
{
    float h = u->facing, ax = u->tilt[0], ay = u->tilt[1];
    float ch = cosf(h), sh = sinf(h), cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay);
    float r[9] = { ch * cy, sh * cy, -sy,
                   ch * sy * sx - sh * cx, sh * sy * sx + ch * cx, cy * sx,
                   ch * sy * cx + sh * sx, sh * sy * cx - ch * sx, cy * cx };
    for (int c = 0; c < 3; c++) {
        for (int k = 0; k < 3; k++)
            u->world[4 * c + k] = r[3 * c + k];
        u->world[4 * c + 3] = 0;
    }
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
                if (weapon_make(a, &u->w[u->nw], record_group(cl, wr[i]))) {
                    ArmyWeapon *w = &u->w[u->nw++];
                    w->rot_range = group_num(cl, "RotationRange", 0) * PI / 180;
                    w->rot_rate = group_num(cl, "RotationRate", 1);
                    w->elev_range = group_num(cl, "ElevationRange", 0) * PI / 180;
                    w->elev_rate = group_num(cl, "ElevationRate", 1);
                }
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

/* the first thing a ray from p along the unit d meets within len: a monster's sphere (or its box), a standing
 * building's box, the street (z = 0). [G] The game asks its collision world (gp-0x6D60 vtable +0xC +0x18, mask 8,
 * flags 4 / 0x2000 / 0x2040), not read; the owner is passed over [R 0x293CD0]. Returns the distance, -1 none;
 * *monster the monster it hit or -1, nrm the surface */
static float ray_hit(const Army *a, const float p[3], const float d[3], float len, int *monster, float nrm[3])
{
    float best = -1;
    *monster = -1;
    nrm[0] = -d[0];
    nrm[1] = -d[1];
    nrm[2] = -d[2];
    for (int i = 0; i < a->nm; i++) {
        const ArmyMonster *m = &a->m[i];
        for (int s = 0; s < m->nspheres; s++) {
            float oc[3] = { p[0] - m->spheres[s][0], p[1] - m->spheres[s][1], p[2] - m->spheres[s][2] };
            float bq = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2];
            float c = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - m->spheres[s][3] * m->spheres[s][3];
            float disc = bq * bq - c;
            if (disc < 0)
                continue;
            float t = -bq - sqrtf(disc);
            if (t < 0)
                t = c <= 0 ? 0 : -1;
            if (t >= 0 && t <= len && (best < 0 || t < best)) {
                best = t;
                *monster = i;
                for (int k = 0; k < 3; k++)
                    nrm[k] = p[k] + d[k] * t - m->spheres[s][k];
                norm3(nrm);
            }
        }
        if (m->nspheres == 0 && m->box_max[2] > m->box_min[2]) {
            float t0 = 0, t1 = len;
            int ok = 1;
            for (int k = 0; k < 3 && ok; k++) {
                if (fabsf(d[k]) < 1e-8f) {
                    if (p[k] < m->box_min[k] || p[k] > m->box_max[k])
                        ok = 0;
                } else {
                    float ta = (m->box_min[k] - p[k]) / d[k], tb = (m->box_max[k] - p[k]) / d[k];
                    if (ta > tb) { float x = ta; ta = tb; tb = x; }
                    if (ta > t0) t0 = ta;
                    if (tb < t1) t1 = tb;
                    if (t0 > t1) ok = 0;
                }
            }
            if (ok && (best < 0 || t0 < best)) {
                best = t0;
                *monster = i;
            }
        }
    }
    for (int i = 0; i < a->nb; i++) {
        const ArmyBox *bx = &a->b[i];
        if (bx->gone)
            continue;
        /* [G] a shot that starts in or at a building (a soldier in its window) leaves it unhindered */
        if (p[0] >= bx->min[0] - 0.5f && p[0] <= bx->max[0] + 0.5f && p[1] >= bx->min[1] - 0.5f &&
            p[1] <= bx->max[1] + 0.5f && p[2] >= bx->min[2] - 0.5f && p[2] <= bx->max[2] + 0.5f)
            continue;
        float t0 = 0, t1 = len;
        int ok = 1, axis = -1;
        for (int k = 0; k < 3 && ok; k++) {
            if (fabsf(d[k]) < 1e-8f) {
                if (p[k] < bx->min[k] || p[k] > bx->max[k])
                    ok = 0;
            } else {
                float ta = (bx->min[k] - p[k]) / d[k], tb = (bx->max[k] - p[k]) / d[k];
                if (ta > tb) { float x = ta; ta = tb; tb = x; }
                if (ta > t0) { t0 = ta; axis = k; }
                if (tb < t1) t1 = tb;
                if (t0 > t1) ok = 0;
            }
        }
        if (ok && (best < 0 || t0 < best)) {
            best = t0;
            *monster = -1;
            nrm[0] = nrm[1] = nrm[2] = 0;
            if (axis >= 0)
                nrm[axis] = d[axis] > 0 ? -1.0f : 1.0f;
        }
    }
    if (d[2] < 0 && p[2] >= 0) {
        float t = -p[2] / d[2];
        if (t <= len && (best < 0 || t < best)) {
            best = t;
            *monster = -1;
            nrm[0] = nrm[1] = 0;
            nrm[2] = 1;
        }
    }
    return best;
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
    if (s->damage_type == 4)                         /* [R] 0x33B770: Blunt counts as 0 (collision flags 0x2040) */
        s->damage_type = 0;
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
        int mon;
        float nrm[3];
        float t = ray_hit(a, s->pos, s->dir, step + 0.001f, &mon, nrm);
        if (t >= 0) {
            float at[3] = { s->pos[0] + s->dir[0] * t, s->pos[1] + s->dir[1] * t, s->pos[2] + s->dir[2] * t };
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

/* the beam while it fires (0x295750): [R] from the weapon along its direction, the first thing met: the beam stops
 * there (0x226DF0), and a hit object takes, on the beam's first touch (+0x1B8), ImpactDamage x the damage scale,
 * after that DamagePerSec x dt each frame (if DamagePerSec > 0), through the impact 0x33B440; the ImpactEffect
 * sits where it touches. [G] The beam's own reach is its effect's (a stream, 0x226950): taken as growing at
 * Velocity per second up to MaxFiringRange */
static void beam_update(Army *a, ArmyUnit *u, ArmyWeapon *w, const float from[3], const float dir[3], float dt)
{
    ArmyShot *s = NULL;
    for (int i = 0; i < a->nshots; i++)
        if (a->shot[i].alive && a->shot[i].id == w->shot)
            s = &a->shot[i];
    if (!s)
        return;
    memcpy(s->pos, from, sizeof s->pos);
    memcpy(s->dir, dir, sizeof s->dir);
    norm3(s->dir);
    s->age += dt;
    float reach = fminf(w->velocity > 0 ? w->velocity * s->age : w->range, w->range);
    int mon;
    float nrm[3];
    float t = reach > 0.001f ? ray_hit(a, s->pos, s->dir, reach, &mon, nrm) : -1;
    s->length = t >= 0 ? t : reach;
    if (t < 0)
        return;
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
 * may: [G] what the weapon system (WeaponCluster, not read) allows: a target within MaxFiringRange, the turret
 * turned onto it */
static void weapon_tick(Army *a, ArmyUnit *u, ArmyWeapon *w, int may, const float from[3], const float dir[3],
                        int target, float dt)
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
            beam_update(a, u, w, from, dir, dt);
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
            memcpy(s->dir, dir, sizeof s->dir);
            w->shot = s->id;
            if (s->effect) {
                ArmyEvent *e = emit(a, AE_EFFECT, -1, s->effect, from);
                e->shot = s->id;
                e->id = a->next_fx++;
                memcpy(e->dir, dir, sizeof e->dir);
            }
        }
    }
}

/* [G] a ballistic aim: the lower launch angle that carries a shot of speed v and gravity g from p to t, else 45 */
static void lob(const float p[3], const float t[3], float v, float g, float dir[3])
{
    float dx = t[0] - p[0], dy = t[1] - p[1], h = t[2] - p[2], d = sqrtf(dx * dx + dy * dy);
    if (d < 1e-3f || v <= 0) {
        dir[0] = dx;
        dir[1] = dy;
        dir[2] = h;
        norm3(dir);
        return;
    }
    float ang;
    if (g <= 0)
        ang = atan2f(h, d);
    else {
        float disc = v * v * v * v - g * (g * d * d + 2 * h * v * v);
        ang = disc >= 0 ? atanf((v * v - sqrtf(disc)) / (g * d)) : PI / 4;
    }
    dir[0] = dx / d * cosf(ang);
    dir[1] = dy / d * cosf(ang);
    dir[2] = sinf(ang);
}

/* where a unit's weapon fires from and which way: the WeaponNode as drawn when the host knows it, along the
 * definition's axis (objects: their turret turned onto the target, [G] at RotationRate / ElevationRate radians per
 * second within the ranges); actors and stand-ins aim straight at the target (a lob for shots that fall) [G].
 * Returns whether the turret is on the target */
static int weapon_aim(Army *a, ArmyUnit *u, ArmyWeapon *w, const float tp[3], int has_target, float from[3],
                      float dir[3], float dt)
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
    int actor = u->kind == AK_TROOP || u->kind == AK_WINDOW || u->kind == AK_OTHER;
    if (actor || u->kind == AK_TURRET) {
        if (!has_target) {
            dir[0] = -sinf(u->facing);
            dir[1] = cosf(u->facing);
            dir[2] = 0;
            return 0;
        }
        lob(from, tp, w->velocity, 9.81f * w->gravity, dir);
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
    int on = 0;
    if (has_target) {
        /* [R] the cluster (0x29ADE0) aims at the target lifted for the shot's fall (0x293050: Velocity, Gravity) */
        float aim[3];
        lob(from, tp, w->velocity, 9.81f * w->gravity, aim);
        float want_yaw = atan2f(-aim[0], aim[1]) - u->facing;
        while (want_yaw > PI) want_yaw -= 2 * PI;
        while (want_yaw < -PI) want_yaw += 2 * PI;
        float want_pitch = asinf(clampf(aim[2], -1, 1));
        /* [R] 0x298E70 / 0x298F40: each kept within -Range .. +Range (0x299750: degrees x pi / 180) */
        if (w->rot_range > 0 && w->rot_range < PI)
            want_yaw = clampf(want_yaw, -w->rot_range, w->rot_range);
        if (w->elev_range > 0)
            want_pitch = clampf(want_pitch, -w->elev_range, w->elev_range);
        float sy = w->rot_rate * dt, sp = w->elev_rate * dt;
        w->yaw += clampf(want_yaw - w->yaw, -sy, sy);
        w->pitch += clampf(want_pitch - w->pitch, -sp, sp);
        on = fabsf(want_yaw - w->yaw) < 0.05f && fabsf(want_pitch - w->pitch) < 0.05f;
    }
    float h = u->facing + w->yaw;
    float f[3] = { -sinf(h) * cosf(w->pitch), cosf(h) * cosf(w->pitch), sinf(w->pitch) };
    memcpy(dir, f, sizeof f);
    if (have_node && !w->rot_range && !w->elev_range) {
        for (int k = 0; k < 3; k++)
            dir[k] = node[4 * w->axis + k] * (w->axis_neg ? -1.0f : 1.0f);
    }
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
        monster_centre(&a->m[u->target], tp);
        has = 1;
    }
    for (int i = 0; i < u->nw; i++) {
        ArmyWeapon *w = &u->w[i];
        float from[3], dir[3];
        int aimed = weapon_aim(a, u, w, tp, has, from, dir, dt);
        int may = has && u->weapons_on && w->on && aimed && dist3(from, tp) <= w->range;
        weapon_tick(a, u, w, may, from, dir, point ? -1 : u->target, dt);
    }
}

static void weapons_enable(ArmyUnit *u, int on)      /* enableWeapons / enableWeapon */
{
    u->weapons_on = on;
    for (int i = 0; i < u->nw; i++)
        u->w[i].on = on;
}
/* setWeaponAmmo(n, -, index) [G: the binding's body (AttackObject id 0x3A) not read: the weapon's ammo] */
static void weapon_ammo(ArmyUnit *u, int n, int index)
{
    if (index >= 0 && index < u->nw)
        u->w[index].ammo = n;
}
static void weapons_reload(ArmyUnit *u)              /* reloadWeapon [G: ammo back to Ammo] */
{
    for (int i = 0; i < u->nw; i++)
        u->w[i].ammo = u->w[i].ammo_max > 0 ? u->w[i].ammo_max : 1;
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

/* [R] 0x334C40: the block's AIDef (here, or up its book's chain); each AIInstance child a record (0x3350F0):
 * MinDelay 10, RespawnDelay 10, OffsetDelay 5, MaxOccurances 9999 unless set; the first spawn in
 * MinDelay + OffsetDelay x rand seconds */
int army_load_block(Army *a, Group block_def)
{
    a->nsp = 0;
    a->active = 0;
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
void army_weapon_tick(Army *a, ArmyWeapon *w, int may, const float from[3], const float dir[3], int target, float dt)
{
    weapon_tick(a, NULL, w, may, from, dir, target, dt);
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

/* SplineMover (helicopters, droppables): points with their distances along the path (addPoint(p, d)) at a
 * velocity. [G] The game's spline (gWorld.createSplineMover, not read) is taken as straight runs between the points;
 * flag 2 faces the way it goes */
static void spline_init(ArmyUnit *u, float vel)
{
    u->mv.n = 0;
    u->mv.seg = 0;
    u->mv.t = 0;
    u->mv.speed = vel;
    u->mv.active = 1;
    u->mv.kind = 1;
}
static void spline_add(ArmyUnit *u, const float p[3], float d)
{
    if (u->mv.n < 8) {
        memcpy(u->mv.pts[u->mv.n], p, sizeof u->mv.pts[0]);
        u->mv.dist[u->mv.n++] = d;
    }
}

static int mover_step(Army *a, ArmyUnit *u, float dt)
{
    if (!u->mv.active || u->mv.n < 2)
        return 0;
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
        int m = nearest_monster(a, u->pos);
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

/* SphereMover (a wreck flying off: setVelocity): [G] the template's Gravity (DefObjectProps: 35) down, a bounce on
 * the street with its Restitution (0.2 for the vehicles), at rest when slow; 1 when it first meets the street */
static int sphere_step(ArmyUnit *u, float dt)
{
    float g = tnum(u, "Gravity", 9.81f), e = tnum(u, "Restitution", 0.8f);
    int hit = 0;
    u->vel[2] -= g * dt;
    for (int k = 0; k < 3; k++)
        u->pos[k] += u->vel[k] * dt;
    u->tilt[0] += u->spin[0] * dt;
    u->tilt[1] += u->spin[1] * dt;
    u->facing += u->spin[2] * dt;
    if (u->pos[2] < 0) {
        u->pos[2] = 0;
        if (u->vel[2] < 0) {
            hit = 1;
            u->vel[2] = -u->vel[2] * e;
            u->vel[0] *= 0.7f;
            u->vel[1] *= 0.7f;
            u->spin[0] *= 0.5f;
            u->spin[1] *= 0.5f;
            u->spin[2] *= 0.5f;
            if (u->vel[2] < 1) {
                u->vel[0] = u->vel[1] = u->vel[2] = 0;
                u->spin[0] = u->spin[1] = u->spin[2] = 0;
            }
        }
    }
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
    u->pulse_delay = 0.25f;
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
        if (x >= a->view[0] && x <= a->view[1]) {    /* gWorld.isSphereVisible(pos, 1) [G: the view's x range] */
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
        int m = u->mv.flags & 8 ? nearest_monster(a, u->pos) : -1;   /* the mover's target */
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
    int m = u->mv.flags & 8 ? nearest_monster(a, u->pos) : -1;
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

/* getMonsterDirection: [G] (the Actor binding 0x3F, not read) the eighth of the way to the monster's middle seen
 * from the window, counter-clockwise from the right in the front's plane: RM, RU, MU, LU, LM, LD, MD, RD */
static int monster_direction(const ArmyUnit *u, const float mp[3])
{
    float dx = mp[0] - u->pos[0], dz = mp[2] + 4.25f - u->pos[2];
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
        /* findBuildingByPosition(its place): the building's place, y - 5, against the monster's y [R]; the
         * building's place taken as its box's middle [G] */
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
        return DELETE_ME;                            /* closeWindow */
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
        float mid = (a->view[0] + a->view[1]) / 2;   /* gWorld.getCameraPos().x [G: the middle of the view] */
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
    u->spin[0] = PI / 4;                             /* setRotation(pi / 4, 0, 0) [G: radians per second] */
    u->mv.active = 0;
    u->mv.kind = 2;
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

static const ArmyBox *building_at(const Army *a, const float p[3])   /* findBuildingByPosition [G: its footprint] */
{
    for (int i = 0; i < a->nb; i++) {
        const ArmyBox *b = &a->b[i];
        if (!b->gone && p[0] >= b->min[0] && p[0] <= b->max[0] && p[1] >= b->min[1] && p[1] <= b->max[1])
            return b;
    }
    return NULL;
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
     * the drop, else the drop's own x [R]; the building's place taken as its box's middle [G] */
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
 * setReaction(19), gone [R]. [G] When the game calls it (the world's collision of the falling object with the
 * monster, not read): here when it touches the monster's spheres or box, or the monster's attack reaches it */
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
    case AK_TURRET: return state == 2 ? DELETE_ME : 1;   /* a sunk boat */
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

static int touches(const ArmyMonster *m, const float p[3], float r)   /* a point (radius r) in a monster's shapes */
{
    for (int s = 0; s < m->nspheres; s++) {
        float d[3] = { p[0] - m->spheres[s][0], p[1] - m->spheres[s][1], p[2] - m->spheres[s][2] };
        if (len3(d) <= m->spheres[s][3] + r)
            return 1;
    }
    if (m->nspheres == 0 && m->box_max[2] > m->box_min[2])
        return p[0] >= m->box_min[0] - r && p[0] <= m->box_max[0] + r && p[1] >= m->box_min[1] - r &&
               p[1] <= m->box_max[1] + r && p[2] >= m->box_min[2] - r && p[2] <= m->box_max[2] + r;
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
    if (u->mv.kind == 2)
        r = sphere_step(u, dt) ? -2 : 1;             /* -2: OnHitGround (script event 11) */
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
            if (touches(&a->m[m], u->pos, 0.5f))
                r = drop_damage(a, u, m);
    if (r == DELETE_ME) {
        unit_remove(a, u);
        return;
    }
    if (u->kind == AK_DROP)
        u->tilt[1] += u->spin[1] * dt;               /* Process: rotate(p3RotRate) every frame */
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
    a->rng = 12345;
    a->view[0] = -45;                                /* [G] what the camera sees of the street, along x */
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

void army_seed(Army *a, uint32_t seed) { a->rng = seed ? seed : 1; }
void army_set_view(Army *a, float xmin, float xmax) { a->view[0] = xmin; a->view[1] = xmax; }

void army_clear(Army *a)
{
    a->n = 0;
    a->nshots = 0;
    a->nev = 0;
    for (int i = 0; i < MAX_PERSONS; i++)
        a->person[i].on = 0;
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

void army_person_weapon(Army *a, int person, const char *weapon, const float person_world[16], int target_monster)
{
    if (person < 0)
        return;
    PersonGun *g = &a->person[person % MAX_PERSONS];
    if (!weapon) {                                   /* enableWeapon(0) */
        g->on = 0;
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
    if (person >= 0 && person_world)
        memcpy(a->person[person % MAX_PERSONS].world, person_world, 16 * sizeof(float));
}

static void persons_update(Army *a, float dt)
{
    for (int i = 0; i < MAX_PERSONS; i++) {
        PersonGun *g = &a->person[i];
        if (!g->on || !g->has)
            continue;
        int m = g->monster >= 0 && g->monster < a->nm ? g->monster : -1;
        float from[3] = { g->world[12], g->world[13], g->world[14] + 1.5f }, tp[3] = { 0, 0, 0 }, dir[3];
        if (m >= 0)
            monster_centre(&a->m[m], tp);
        lob(from, tp, g->w.velocity, 9.81f * g->w.gravity, dir);   /* [G] the throw's aim: see weapon_aim */
        weapon_tick(a, NULL, &g->w, m >= 0 && dist3(from, tp) <= g->w.range, from, dir, m, dt);
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

/* a unit's shape for the monster's attack: [G] actors a sphere of CollisionRadius (DefActorProps: 2) 1 up (as
 * people.c); objects their model's box (AutoCollisionBox; the host's, else 4 x 6 x 3) turned with them */
static int unit_touched(const Army *a, const ArmyUnit *u, const float c[3], float radius)
{
    if (u->kind == AK_TROOP || u->kind == AK_WINDOW) {
        float p[3] = { u->pos[0], u->pos[1], u->pos[2] + 1 };
        return dist3(p, c) <= radius + tnum(u, "CollisionRadius", 2);
    }
    float lo[3] = { -2, -3, 0 }, hi[3] = { 2, 3, 3 };
    if (u->kind == AK_DROP) {
        lo[0] = lo[1] = -1;
        lo[2] = -1;
        hi[0] = hi[1] = hi[2] = 1;
    }
    if (a->host.shape_box && u->shape)
        a->host.shape_box(a->host.ctx, u->shape, lo, hi);
    /* the centre in the unit's frame (its rotation is orthonormal: the transpose undoes it) */
    float d[3] = { c[0] - u->world[12], c[1] - u->world[13], c[2] - u->world[14] }, l[3], q = 0;
    for (int k = 0; k < 3; k++)
        l[k] = d[0] * u->world[4 * k] + d[1] * u->world[4 * k + 1] + d[2] * u->world[4 * k + 2];
    for (int k = 0; k < 3; k++) {
        float e = l[k] < lo[k] ? lo[k] - l[k] : l[k] > hi[k] ? l[k] - hi[k] : 0;
        q += e * e;
    }
    return q <= radius * radius;
}

/* TakeDamage(monster, amount, type, direction) of each unit an armed attack sphere reaches */
int army_hit(Army *a, int monster, const float centre[3], float radius, float amount)
{
    if (monster < 0 || monster >= a->nm)
        return 0;
    int n = 0;
    for (int i = 0; i < a->n; i++) {
        ArmyUnit *u = &a->u[i];
        if (u->phase != AU_ALIVE || !unit_touched(a, u, centre, radius))
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

/* [G] in the hand as the hand is (the HeldObjectMover's MajorAxis turn and HoldOffset are left out) */
void army_hold(Army *a, int unit, const float hand_world[16])
{
    if (unit < 0 || unit >= a->n || a->u[unit].phase != AU_HELD)
        return;
    ArmyUnit *u = &a->u[unit];
    memcpy(u->world, hand_world, sizeof u->world);
    u->pos[0] = hand_world[12];
    u->pos[1] = hand_world[13];
    u->pos[2] = hand_world[14];
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
