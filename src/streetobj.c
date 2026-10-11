/* streetobj.c - the street and roof objects: StreetObject.bsf and RoofItem.bsf (streetobj.h,
 * docs/STREETOBJ_NOTES.md). [R] read in the scripts (build/scripts/StreetObject.scr, RoofItem.scr) or the ELF,
 * [G] guessed. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "streetobj.h"
#include "wrand.h"

#define PI 3.14159265f
#define TIMESTEP (1.0f / 30.0f)                      /* gWorld.getTimeStep(): its step, 1/30 [R 0x2B7800] */
#define MAX_OBJS 256
#define MAX_EVENTS 256
#define MAX_BUILDINGS 64
#define MAX_STOPS 4                                  /* effects an object turns off when it dies */

struct StreetObjs {
    StreetHost host;
    StreetObj o[MAX_OBJS];
    int n;
    /* per object: the effects it turns off when it dies (setTurnOffWhenObjectDies(1)) and whether it is going */
    int stop[MAX_OBJS][MAX_STOPS], nstop[MAX_OBJS];
    uint8_t deleting[MAX_OBJS];
    StreetEvent ev[MAX_EVENTS];
    int nev;
    StreetBuilding b[MAX_BUILDINGS];
    int nb;
    const StreetMonster *m;
    int nm;
    float acc;
    int next_id, frame;
    uint32_t seed;
    CollideWorld *cw;                                /* the frame's collision world (streetobj_collide_add) */
};

/* ---------------------------------------------------------------- small things */

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

/* gWorld.frand() (0x279F98): the world's Park-Miller, gWorld +0x828 (wrand.c) [R] */
static float frand(StreetObjs *s)
{
    (void)s;
    return wrand_frand();
}

static float len3(const float a[3]) { return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
/* Point3.fastNormalize: [R] the game's normalize of a null vector gives (1, 0, 0) (as army.c) */
static void norm3(float a[3])
{
    float l = len3(a);
    if (l < 1e-6f) {
        a[0] = 1;
        a[1] = a[2] = 0;
    } else {
        a[0] /= l;
        a[1] /= l;
        a[2] /= l;
    }
}

/* 3 x 3 column-major: m[c * 3 + r] */
static void mul33(const float a[9], const float b[9], float out[9])
{
    float t[9];
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            t[c * 3 + r] = a[r] * b[c * 3] + a[3 + r] * b[c * 3 + 1] + a[6 + r] * b[c * 3 + 2];
    memcpy(out, t, sizeof t);
}
static void rot_x(float r[9], float a)
{
    float c = cosf(a), s = sinf(a);
    float m[9] = { 1, 0, 0, 0, c, s, 0, -s, c };
    memcpy(r, m, sizeof m);
}
static void rot_y(float r[9], float a)
{
    float c = cosf(a), s = sinf(a);
    float m[9] = { c, 0, -s, 0, 1, 0, s, 0, c };
    memcpy(r, m, sizeof m);
}
static void rot_z(float r[9], float a)
{
    float c = cosf(a), s = sinf(a);
    float m[9] = { c, s, 0, -s, c, 0, 0, 0, 1 };
    memcpy(r, m, sizeof m);
}
static void update_world(StreetObj *o)
{
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++)
            o->world[c * 4 + r] = o->rot[c * 3 + r];
        o->world[c * 4 + 3] = 0;
    }
    o->world[15] = 1;
}
static const float *pos_of(const StreetObj *o) { return o->world + 12; }
static void world_to_object(const StreetObj *o, const float w[3], float l[3])   /* worldToObjectVec: rot^T w */
{
    for (int r = 0; r < 3; r++)
        l[r] = o->rot[r * 3] * w[0] + o->rot[r * 3 + 1] * w[1] + o->rot[r * 3 + 2] * w[2];
}

/* WorldObject.rotateAA(angle, axis) (case 0xE, 0x278320) [R]: R = I + [angle axis]x (0x101F70), its axes made
 * square again (0x1017F0), in its own frame: right-handed, so a tree leans away from the monster that pushed it;
 * the turn is atan(angle), and a rotateAA(-total) does not quite undo it */
static void rotate_aa(StreetObj *o, float angle, const float axis[3])
{
    float tx = angle * axis[0], ty = angle * axis[1], tz = angle * axis[2];
    float r[9] = { 1, tz, -ty, -tz, 1, tx, ty, -tx, 1 };
    for (int c = 0; c < 3; c++) {                    /* Gram-Schmidt, axis by axis */
        float *a = r + 3 * c;
        for (int p = 0; p < c; p++) {
            const float *b = r + 3 * p;
            float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            for (int k = 0; k < 3; k++)
                a[k] -= d * b[k];
        }
        float l = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (l > 1e-3f)
            for (int k = 0; k < 3; k++)
                a[k] /= l;
    }
    mul33(o->rot, r, o->rot);
    update_world(o);
    o->moved = 1;
}
/* WorldObject.rotate(e) (case 0xD, 0x278258) [R]: rot = rot x Rz(e[2]) x Rx(e[0]) x Ry(e[1]), about its own axes */
static void rotate_local(StreetObj *o, const float e[3])
{
    float r[9];
    rot_z(r, e[2]);
    mul33(o->rot, r, o->rot);
    rot_x(r, e[0]);
    mul33(o->rot, r, o->rot);
    rot_y(r, e[1]);
    mul33(o->rot, r, o->rot);
    update_world(o);
    o->moved = 1;
}
/* WorldObject.setFacing(e): rot = Rz(e[2]) x Rx(e[0]) x Ry(e[1]) [R 0x277E78 -> 0x101330(m, z, x, y)] */
static void set_facing(StreetObj *o, const float e[3])
{
    float rz[9], rx[9], ry[9];
    memcpy(o->facing, e, sizeof o->facing);
    rot_z(rz, e[2]);
    rot_x(rx, e[0]);
    rot_y(ry, e[1]);
    mul33(rz, rx, o->rot);
    mul33(o->rot, ry, o->rot);
    update_world(o);
    o->moved = 1;
}

/* setPulseDelay (0x2AA2A0): only the delay; the pulse's clock (o->pulse) runs on [R] */
static void set_pulse(StreetObj *o, float delay)
{
    o->pulse_delay = delay;
}

/* ---------------------------------------------------------------- properties */

/* getStringProp / getFloatProp / getIntProp: the instance book (obj +0x7C) first, then the template [R 0x2C51A0, as
 * army.c]. The books keep a record's type before a '|' ("Effect|HitEffect"); the scripts ask by the bare name */
static const char *const TYPED[] = { "", "Effect|", "Sound|", "Shape|", "Color|", "Script|" };

static int find_rec(Group g, const char *key, Record *out, Group *where)
{
    char k[96];
    if (!g.at || !key)
        return 0;
    for (size_t i = 0; i < sizeof TYPED / sizeof *TYPED; i++) {
        snprintf(k, sizeof k, "%s%s", TYPED[i], key);
        if (group_get(g, k, out)) {
            if (where)
                *where = g;
            return 1;
        }
    }
    return 0;
}
static const char *gstr(Group g, const char *key)
{
    Record r;
    Group w;
    if (!find_rec(g, key, &r, &w) || (r.kind & 0xFF) != PK_NAME)
        return NULL;
    const char *s = record_str(w, r);
    return s && *s ? s : NULL;
}
static int gnum(Group g, const char *key, float *out)
{
    Record r;
    if (!find_rec(g, key, &r, NULL) || (r.kind & 0xFF) > PK_FLOAT)
        return 0;
    *out = record_num(r);
    return 1;
}
static int gpoint(Group g, const char *key, float *out, int n)   /* a group of n numbers (Offset { x y z }) */
{
    Record r, it[8];
    Group w;
    if (!find_rec(g, key, &r, &w) || (r.kind & 0xFF) != PK_GROUP)
        return 0;
    Group sub = record_group(w, r);
    if ((int)group_items(sub, it, 8) < n)
        return 0;
    for (int k = 0; k < n; k++)
        out[k] = record_num(it[k]);
    return 1;
}
static const char *ostr(const StreetObj *o, const char *key)   /* getStringProp: "StringNotFound" is NULL */
{
    const char *s = gstr(o->inst, key);
    return s ? s : gstr(o->tmpl, key);
}
static float onum(const StreetObj *o, const char *key, float def)
{
    float v;
    return gnum(o->inst, key, &v) || gnum(o->tmpl, key, &v) ? v : def;
}
static int opoint(const StreetObj *o, const char *key, float out[3])
{
    return gpoint(o->inst, key, out, 3) || gpoint(o->tmpl, key, out, 3);
}

/* [R] the template's Flags names (0x335ED0, the table 0x3F5F20) */
static uint32_t template_flags(Group t)
{
    static const struct { const char *name; uint32_t bit; } NAMES[] = {
        { "IsBuilding", 0x2000 }, { "PushOutMonsters", SF_PUSH_OUT }, { "OnlyHitWithEffNodes", SF_EFF_NODES },
        { "Pickupable", SF_PICKUP }, { "BeingHeld", SF_HELD }, { "IsObstacle", SF_OBSTACLE },
    };
    const char *f = gstr(t, "Flags");
    uint32_t bits = 0;
    for (size_t i = 0; f && i < sizeof NAMES / sizeof *NAMES; i++)
        if (strstr(f, NAMES[i].name))
            bits |= NAMES[i].bit;
    return bits;
}

/* ---------------------------------------------------------------- events */

static int idx(const StreetObjs *s, const StreetObj *o) { return o ? (int)(o - s->o) : -1; }

static StreetEvent *emit(StreetObjs *s, int type, const StreetObj *o, const char *name, const float pos[3])
{
    static StreetEvent spare;
    StreetEvent *e = s->nev < MAX_EVENTS ? &s->ev[s->nev++] : &spare;
    memset(e, 0, sizeof *e);
    e->type = type;
    e->obj = idx(s, o);
    e->monster = -1;
    e->node_id = -1;
    e->name = name;
    if (pos)
        memcpy(e->pos, pos, sizeof e->pos);
    else if (o)
        memcpy(e->pos, pos_of(o), sizeof e->pos);
    return e;
}

void streetobj_point(const StreetObjs *s, int i, const float off[3], float out[3])
{
    const StreetObj *o = &s->o[i];
    for (int r = 0; r < 3; r++)
        out[r] = o->world[12 + r] + o->rot[r] * off[0] + o->rot[3 + r] * off[1] + o->rot[6 + r] * off[2];
}

/* FollowEffect.createAndStart(pos, fx) (follow 0) or createAndStartAttached... (follow 1: on the object, at off in its
 * frame, or at its model's node); the handle, -1 none. stop_on_death: setTurnOffWhenObjectDies(1) */
static int effect(StreetObjs *s, StreetObj *o, const char *name, const float at[3], int follow, const float off[3],
                  const char *node, int node_id, int stop_on_death)
{
    if (!name)
        return -1;
    static const float zero[3] = { 0, 0, 0 };
    float p[3];
    if (follow)
        streetobj_point(s, idx(s, o), off ? off : zero, p);
    else
        memcpy(p, at ? at : pos_of(o), sizeof p);
    StreetEvent *e = emit(s, SE_EFFECT, o, name, p);
    e->id = ++s->next_id;
    e->level = follow;
    e->node = follow ? node : NULL;
    e->node_id = follow && node_id > 0 ? node_id : -1;
    if (follow && off)
        memcpy(e->off, off, sizeof e->off);
    int i = idx(s, o);
    if (stop_on_death && s->nstop[i] < MAX_STOPS)
        s->stop[i][s->nstop[i]++] = e->id;
    return e->id;
}
static void effect_stop(StreetObjs *s, StreetObj *o, int id)
{
    if (id <= 0)
        return;
    emit(s, SE_EFFECT_STOP, o, NULL, NULL)->id = id;
    int i = idx(s, o);
    for (int k = 0; k < s->nstop[i]; k++)
        if (s->stop[i][k] == id)
            s->stop[i][k--] = s->stop[i][--s->nstop[i]];
}
static void sound(StreetObjs *s, StreetObj *o, const char *name, int level)   /* add3DSfx(name, level) */
{
    if (name)
        emit(s, SE_SOUND, o, name, NULL)->level = level;
}
static void set_shape(StreetObjs *s, StreetObj *o, const char *shape)   /* setShape */
{
    if (!shape)
        return;
    o->shape = shape;
    emit(s, SE_SHAPE, o, shape, NULL);
}
/* gCommon.addStats(obj, slot, 0) [R 0x2C51A0]: once an object (flag 0x40000000) */
static void add_stats(StreetObjs *s, StreetObj *o, int monster)
{
    if (o->counted || monster < 0)
        return;
    o->counted = 1;
    StreetEvent *e = emit(s, SE_STATS, o, o->name, NULL);
    e->monster = monster;
    e->tmpl = o->tmpl;
    e->amount = (float)(int)onum(o, "Points", 0);
}
/* requestDelete (0x285DD0) marks the object (0x800) for the world's walk, a frame later [R]; here it goes at the end
 * of its step (the rest of the script's call still runs); the effects it turns off go with it */
static void request_delete(StreetObjs *s, StreetObj *o)
{
    s->deleting[idx(s, o)] = 1;
}
static void delete_now(StreetObjs *s, StreetObj *o)
{
    int i = idx(s, o);
    if (!o->alive)
        return;
    while (s->nstop[i] > 0)
        effect_stop(s, o, s->stop[i][0]);
    o->alive = 0;
    o->moved = 1;
    s->deleting[i] = 0;
    emit(s, SE_REMOVED, o, o->name, NULL);
}

/* findBuildingByPosition (0x2A3D20 with no direction) [R]: the last building of the list whose box holds the point
 * along x and y (both ends in); z is not looked at */
static int building_at(const StreetObjs *s, const float p[3])
{
    int found = -1;
    for (int i = 0; i < s->nb; i++)
        if (s->b[i].min[0] <= p[0] && p[0] <= s->b[i].max[0] && s->b[i].min[1] <= p[1] && p[1] <= s->b[i].max[1])
            found = i;
    return found;
}
/* getBuildingHeight (0x309890) [R]: the attached building's +0x1F0, its box's top now; 0 without one */
static float building_height(const StreetObjs *s, const StreetObj *o)
{
    return o->building >= 0 && o->building < s->nb ? s->b[o->building].max[2] : 0;
}
static int nearest_monster(const StreetObjs *s, const float p[3])   /* getNearestMonster: -1 none */
{
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < s->nm; i++) {
        float d[3] = { s->m[i].pos[0] - p[0], s->m[i].pos[1] - p[1], s->m[i].pos[2] - p[2] };
        float q = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (q < bd) {
            bd = q;
            best = i;
        }
    }
    return best;
}

/* ---------------------------------------------------------------- StreetObject.bsf */

/* Construct [R] */
static void street_construct(StreetObjs *s, StreetObj *o)
{
    if (onum(o, "OnlyHitWithEffNodes", 1) != 0)
        o->oflags |= SF_EFF_NODES;
    if (onum(o, "IsSwaying", 0) != 0)
        o->oflags &= ~(uint32_t)SF_EFF_NODES;
    if (ostr(o, "AttachEffect"))
        o->attach_delay = onum(o, "AttachEffectDelay", 0);
    if (onum(o, "Oscillate", 0) != 0) {
        o->osc_base = onum(o, "OscillateScale", 1);
        o->osc_scale = o->osc_base;
        o->osc = 5 * frand(s);
    }
    set_pulse(o, TIMESTEP + o->attach_delay);       /* the last setPulseDelay of Construct wins */
}

/* oscillateObject [R]: the facing's x and y rock as (sin 2t, cos 3t) x 0.0625 x the scale, which falls back to its
 * base by 1 a second after a hit raised it */
static void street_oscillate(StreetObj *o)
{
    float dt = TIMESTEP;
    if (o->osc_scale > o->osc_base)
        o->osc_scale -= 1.0f * dt;
    o->osc += dt;
    float e[3] = { sinf(o->osc * 2) * 0.0625f * o->osc_scale, cosf(o->osc * 3) * 0.0625f * o->osc_scale, o->facing[2] };
    set_facing(o, e);
}

/* onPulse [R] */
static void street_pulse(StreetObjs *s, StreetObj *o)
{
    if (o->state == 0) {
        o->attach_delay -= o->pulse_delay;
        const char *fx = ostr(o, "AttachEffect");
        if (o->attach_delay <= 0 && !o->attach_fx && fx) {
            float off[3] = { -1000, -1000, -1000 };   /* def_noOffset */
            int has_off = opoint(o, "Offset", off) && !(off[0] == -1000 && off[1] == -1000 && off[2] == -1000);
            int node = (int)onum(o, "NodeId", 0);
            const char *node_name = ostr(o, "NodeName");
            if (has_off)                             /* every one turns off when the object dies */
                effect(s, o, fx, NULL, 1, off, NULL, 0, 1);
            else if (node > 0)
                effect(s, o, fx, NULL, 1, NULL, NULL, node, 1);
            else if (onum(o, "Attached", 1) == 0)
                effect(s, o, fx, pos_of(o), 0, NULL, NULL, 0, 1);
            else if (node_name)
                effect(s, o, fx, NULL, 1, NULL, node_name, 0, 1);
            else
                effect(s, o, fx, NULL, 1, NULL, NULL, 0, 1);
            o->attach_fx = 1;                        /* bAttachedEffect */
        }
        set_pulse(o, onum(o, "IsCustomOscillation", 0) == 0 ? 0 : TIMESTEP);
        o->state = 1;
    }
    if (o->state == 1) {
        if (onum(o, "IsCustomOscillation", 0) != 0)
            street_oscillate(o);
    } else if (o->state == 2) {                      /* the sway */
        int mi = nearest_monster(s, pos_of(o));
        if (mi < 0)
            return;
        const float *mp = s->m[mi].pos, *op = pos_of(o);
        float tw[3] = { op[0] - mp[0], op[1] - mp[1], op[2] - mp[2] }, tangent[3];
        world_to_object(o, tw, tangent);
        float cross[3] = { o->axis[1], -o->axis[0], 0 };
        float cos_sway = cosf(PI * o->sway_time / 1.2f);
        float dist2 = tw[0] * tw[0] + tw[1] * tw[1] + tw[2] * tw[2];
        float dot = cross[0] * tangent[0] + cross[1] * tangent[1] + cross[2] * tangent[2];
        if (dist2 < onum(o, "SwayRestitutionDist2", 49) && cos_sway <= 0 && dot > 0)
            return;                                  /* the monster holds it leaning */
        if (o->total_angle >= 0) {
            o->sway_time += TIMESTEP;
            float d = o->sway_intensity * 0.025f / (1.2f * 1.2f) * cos_sway;
            o->total_angle += d;
            rotate_aa(o, d, o->axis);
        } else {
            rotate_aa(o, -o->total_angle, o->axis);
            o->total_angle = 0;
            o->sway_time = 0;
            set_pulse(o, 0);
            o->state = 1;
        }
    } else if (o->state == 3) {                      /* the fall (FallOnHit) */
        rotate_aa(o, 1.4f * o->pulse_delay, o->axis);
        o->total_angle += 1.4f * o->pulse_delay;
        if (o->total_angle > PI / 2) {
            rotate_aa(o, -fabsf(PI / 2 - o->total_angle), o->axis);
            o->state = 4;
        }
    } else if (o->state == 4) {                      /* down: its HitEffect again, then gone DieDuration later */
        if (!(o->flags & 1)) {
            const char *fx = ostr(o, "HitEffect");
            if (onum(o, "HitEffectAttached", 1) != 0)
                effect(s, o, fx, NULL, 1, NULL, NULL, 0, 0);
            else
                effect(s, o, fx, pos_of(o), 0, NULL, NULL, 0, 0);
            o->flags |= 1;
            set_pulse(o, onum(o, "DieDuration", 1));
        } else
            request_delete(s, o);
    }
}

/* the HitEffect of a blow: attached, or (by_prop and HitEffectAttached 0) where it stands; once with
 * PlayHitEffectOnlyOnce; it goes with the object with TurnOffHitEffectOnDestroy */
static void street_hit_effect(StreetObjs *s, StreetObj *o, int by_prop)
{
    const char *fx = ostr(o, "HitEffect");
    if (!fx || (onum(o, "PlayHitEffectOnlyOnce", 0) != 0 && o->hit_played))
        return;
    int off = onum(o, "TurnOffHitEffectOnDestroy", 0) != 0;
    if (!by_prop || onum(o, "HitEffectAttached", 1) != 0)
        effect(s, o, fx, NULL, 1, NULL, NULL, 0, off);
    else
        effect(s, o, fx, pos_of(o), 0, NULL, NULL, 0, off);
    o->hit_played = 1;
}

/* TakeDamage(who, amount, type, direction) [R], from monster mi at who (the upgrade's part is challenge.c's) */
static void street_damage(StreetObjs *s, StreetObj *o, int mi, const StreetMonster *who, int type)
{
    if (!who)
        return;                                      /* gWorld.coerceToMonster(who) not valid: nothing */
    const float *mp = who->pos, *op = pos_of(o);
    if (type == 0 || onum(o, "IsDamagedOnCollision", 0) != 0) {
        o->num_hits++;
        if (o->num_hits >= (int)onum(o, "NumHits", 0)) {
            add_stats(s, o, mi);
            sound(s, o, ostr(o, "HitSound"), 1);
            street_hit_effect(s, o, 1);
            set_shape(s, o, ostr(o, "DamageShape"));
            if (onum(o, "OscillateWithDamage", 0) != 0)
                o->osc_scale = 6;
            else if (onum(o, "FallOnHit", 0) != 0) {
                if (o->state < 3) {                  /* it tips over away from the monster */
                    float a[3] = { mp[0] - op[0], mp[1] - op[1], mp[2] - op[2] }, w[3];
                    w[0] = a[1];
                    w[1] = -a[0];
                    w[2] = 0;
                    norm3(w);
                    world_to_object(o, w, o->axis);
                    norm3(o->axis);
                    set_pulse(o, TIMESTEP);
                    o->state = 3;
                }
            } else
                request_delete(s, o);
        } else if (onum(o, "PlayHitEffectOnFirstHit", 1) != 0)
            street_hit_effect(s, o, 0);              /* always attached here */
    } else if (type == 1) {
        if (onum(o, "PlayHitEffectOnCollision", 0) != 0 &&
            !(onum(o, "PlayHitEffectOnlyOnce", 0) != 0 && o->hit_played)) {
            set_shape(s, o, ostr(o, "DamageShape"));
            const char *fx = ostr(o, "HitEffect");
            if (fx) {                                /* [R] the turn-off only on the unattached one */
                if (onum(o, "HitEffectAttached", 1) != 0)
                    effect(s, o, fx, NULL, 1, NULL, NULL, 0, 0);
                else
                    effect(s, o, fx, op, 0, NULL, NULL, 0, onum(o, "TurnOffHitEffectOnDestroy", 0) != 0);
                o->hit_played = 1;
            }
        }
        if (onum(o, "IsSwaying", 0) != 0 && o->state < 2) {
            float dm[3] = { mp[0] - o->prev_mpos[0], mp[1] - o->prev_mpos[1], mp[2] - o->prev_mpos[2] };
            if (dm[0] * dm[0] + dm[1] * dm[1] + dm[2] * dm[2] > 0) {   /* it moved since its last touch */
                float fz = who->facing + PI / 2, tangent[3] = { -cosf(fz), -sinf(fz), 0 };
                float a[3] = { mp[0] - op[0], mp[1] - op[1], mp[2] - op[2] }, w[3];
                norm3(a);
                float k = tangent[0] * a[0] + tangent[1] * a[1] + tangent[2] * a[2];
                o->sway_intensity = k * k * k * k;  /* walking straight at it: 1 */
                w[0] = a[1];
                w[1] = -a[0];
                w[2] = 0;
                world_to_object(o, w, o->axis);
                norm3(o->axis);
                set_pulse(o, TIMESTEP);
                o->state = 2;
            }
        } else if (onum(o, "IsCustomOscillation", 0) != 0)
            o->osc_scale = onum(o, "CustomOscillationScale", 0) * 2;
    }
    memcpy(o->prev_mpos, mp, sizeof o->prev_mpos);
}

/* ---------------------------------------------------------------- RoofItem.bsf */

/* Construct [R] */
static void roof_construct(StreetObjs *s, StreetObj *o)
{
    o->state = 1;
    o->y_per_frame = 0.2f;
    const char *fx = ostr(o, "AttachEffect");
    if (fx) {
        float off[3] = { 0, 0, onum(o, "AttachEffectZOffset", 0) };
        o->attach_fx = effect(s, o, fx, NULL, 1, off, NULL, 0, 1);
    }
    if (onum(o, "SpansTwoBuildings", 0) != 0)
        set_pulse(o, TIMESTEP * 5);
}

/* onBuildingDestruction [R]: a standing one falls off at once, to a random side */
static void roof_building_gone(StreetObjs *s, StreetObj *o)
{
    if (o->state != 1)
        return;
    o->ydir = frand(s) > 0.5f ? 1.0f : -1.0f;
    set_pulse(o, TIMESTEP);
    o->latency = 0;
    o->state = 2;
}

/* the HitEffect: attached at (0, 0, HitZOffset) (DestFallEffectAttached) or there; the handle */
static int roof_hit_effect(StreetObjs *s, StreetObj *o)
{
    const char *fx = ostr(o, "HitEffect");
    if (!fx)
        return 0;
    float z = onum(o, "HitZOffset", 0);
    int off = onum(o, "TurnOffHitEffectOnDestroy", 0) != 0;
    if (onum(o, "DestFallEffectAttached", 0) != 0) {
        float d[3] = { 0, 0, z };
        o->hit_fx = effect(s, o, fx, NULL, 1, d, NULL, 0, off);
    } else {
        float at[3] = { pos_of(o)[0], pos_of(o)[1], pos_of(o)[2] + z };
        o->hit_fx = effect(s, o, fx, at, 0, NULL, NULL, 0, off);
    }
    return 1;
}

/* fallOff [R]: its HitSound and DamageShape; then every frame down 6 a second until the street, for the first 0.5 s
 * also 2.5 along +y in all, turned by (pi/2, ydir pi/2, +-pi/3) over the frames a fall to the street at 6 takes */
static void roof_fall_off(StreetObjs *s, StreetObj *o)
{
    sound(s, o, ostr(o, "HitSound"), 1);
    set_shape(s, o, ostr(o, "DamageShape"));
    o->state = 3;
    o->elapsed = 3;
    set_pulse(o, TIMESTEP);
    float fall = pos_of(o)[2], n = fall / 6 / TIMESTEP;
    if (n == 0)
        n = 0.01f;                                   /* [R] after its dbgAssert "falling 0 dist" */
    o->rpf[0] = PI / 2 / n;                          /* [R] - -(pi) / 2 / numFrames */
    o->rpf[1] = o->ydir * (PI / 2 / n);
    o->rpf[2] = PI / 3 / n;
    if (frand(s) > 0.5f)
        o->rpf[2] = -o->rpf[2];
    o->move_time = 0.5f;
    o->y_per_frame = 2.5f / (o->move_time / TIMESTEP);
}

/* onPulse [R] */
static void roof_pulse(StreetObjs *s, StreetObj *o)
{
    if (o->state == 2) {
        if (!(o->flags & 1)) {
            if (roof_hit_effect(s, o) && onum(o, "DeleteOnHit", 0) != 0)
                request_delete(s, o);                /* the chimneys: gone in their smoke */
            o->flags |= 1;
        }
        o->latency -= o->pulse_delay;
        if (o->latency < 0) {
            roof_fall_off(s, o);
            if (onum(o, "TurnOffHitEffectOnDestroy", 0) != 0)
                effect_stop(s, o, o->hit_fx);
        }
    }
    if (o->state == 3) {
        o->elapsed -= o->pulse_delay;
        o->move_time -= o->pulse_delay;
        o->world[14] -= 6 * o->pulse_delay;
        if (o->move_time > 0)
            o->world[13] += o->y_per_frame;
        o->moved = 1;
        rotate_local(o, o->rpf);
        if (o->world[14] <= 0)
            request_delete(s, o);                    /* return 4 | 8 | 16: deleted */
    }
    if (o->state == 1 && onum(o, "SpansTwoBuildings", 0) != 0) {   /* across two: either one falling */
        float d = onum(o, "BuildingDistance", 5), p[3] = { pos_of(o)[0] - d, pos_of(o)[1], pos_of(o)[2] };
        int b = building_at(s, p);
        if (b >= 0) {
            if (s->b[b].falling)
                roof_building_gone(s, o);
            else {
                p[0] += 2 * d;
                b = building_at(s, p);
                if (b >= 0) {
                    if (s->b[b].falling)
                        roof_building_gone(s, o);
                } else
                    set_pulse(o, 0);
            }
        } else
            set_pulse(o, 0);
    }
}

/* TakeDamage(who, amount, type, direction) [R]: who NULL from forceDamage (flags 2) */
static void roof_damage(StreetObjs *s, StreetObj *o, int mi, const StreetMonster *who)
{
    if (!((o->flags & 2) || who) || onum(o, "TakeDamage", 1) == 0)
        return;
    o->num_hits++;
    float zdist = 0, z = pos_of(o)[2];
    if (who && !(o->oflags & SF_EFF_NODES))
        zdist = z - who->pos[2];
    int reach = fabsf(zdist) < 2 || z < building_height(s, o);   /* on its roof, or the roof came down below it */
    if (o->state == 1 && o->num_hits >= (int)onum(o, "NumHits", 0) && reach) {
        o->ydir = who && who->pos[0] < pos_of(o)[0] ? 1.0f : -1.0f;
        set_pulse(o, TIMESTEP);
        o->latency = onum(o, "FallLatency", 0.4f);
        o->state = 2;
        set_shape(s, o, ostr(o, "DamageShape"));
        const char *mouth = ostr(o, "AttachToMonsterMouthEffect"), *shoulder = ostr(o, "AttachToMonsterShoulderEffect");
        if (who && mouth) {                          /* the soot */
            StreetEvent *e = emit(s, SE_MONSTER_EFFECT, o, mouth, who->pos);
            e->monster = mi;
            e->node = "fxNode_Belch";
        }
        if (who && shoulder && mouth) {              /* [R] the loop starts mouthEffect, not shoulderEffect, on
                                                      * fxNode_Shoulder1 .. 20 until one is missing */
            StreetEvent *e = emit(s, SE_MONSTER_EFFECT, o, mouth, who->pos);
            e->monster = mi;
            e->node = "fxNode_Shoulder";
            e->level = 1;
        }
        if (who)
            add_stats(s, o, mi);
    } else if (onum(o, "PlayHitEffectOnFirstHit", 1) != 0 && o->state == 1 && reach)
        roof_hit_effect(s, o);
}

/* ---------------------------------------------------------------- the module */

StreetObjs *streetobj_new(const StreetHost *host)
{
    StreetObjs *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    if (host)
        s->host = *host;
    return s;
}
void streetobj_free(StreetObjs *s) { free(s); }

int streetobj_kind_of(Group tmpl)
{
    const char *sc = gstr(tmpl, "ScriptName");
    float up;
    if (gnum(tmpl, "IsMonsterUpgrade", &up) && up != 0)
        return -1;
    return same_text(sc, "StreetObject.bsf") ? SK_STREET : same_text(sc, "RoofItem.bsf") ? SK_ROOF : -1;
}

/* a book's collision keys (WSimpleObject 0x28FD70): CollisionBox { min x y z, max x y z } */
static const float *book_box(Group g, float b[6])
{
    return gpoint(g, "CollisionBox", b, 6) ? b : NULL;
}

/* its collision shape (0x28FD70, collide_shape_pick): the template's CollisionBox, CollisionRadius or AutoCollisionBox
 * with the model's box (the host measures it), then the instance's; else the sphere the model keeps (0x184CB0, the
 * host's shape_sphere; without one the box's bounding sphere; no model: radius 1 at the origin). box_min / box_max:
 * the shape's box (a sphere's bounding cube) */
static void object_box(StreetObjs *s, StreetObj *o)
{
    float tb[6], ib[6], mb[6], c[3] = { 0, 0, 0 }, r = 1;
    int have = o->shape && s->host.shape_box && s->host.shape_box(s->host.ctx, o->shape, mb, mb + 3);
    if (have) {
        float h[3];
        for (int k = 0; k < 3; k++) {
            c[k] = (mb[k] + mb[3 + k]) / 2;
            h[k] = (mb[3 + k] - mb[k]) / 2;
        }
        r = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    }
    /* [R 0x28FD70 -> 0x184CB0] the sphere the model keeps, the union of its parts' spheres (0x185AA0) */
    if (have && s->host.shape_sphere)
        s->host.shape_sphere(s->host.ctx, o->shape, c, &r);
    Group t = o->tmpl, in = o->inst;
    collide_shape_pick(&o->cshape, have ? c : NULL, r, t.at ? book_box(t, tb) : NULL,
                       t.at ? group_num(t, "CollisionRadius", -1) : -1, t.at && group_num(t, "AutoCollisionBox", 0) != 0,
                       in.at ? book_box(in, ib) : NULL, in.at ? group_num(in, "CollisionRadius", -1) : -1,
                       in.at && group_num(in, "AutoCollisionBox", 0) != 0, have ? mb : NULL);
    for (int k = 0; k < 3; k++) {
        o->box_min[k] = o->cshape.centre[k] - o->cshape.half[k];
        o->box_max[k] = o->cshape.centre[k] + o->cshape.half[k];
    }
}

int streetobj_add(StreetObjs *s, Group tmpl, Group inst, const float world[16], int building, int attack)
{
    int kind = tmpl.at ? streetobj_kind_of(tmpl) : -1;
    if (kind < 0 || s->n >= MAX_OBJS)
        return -1;
    int i = s->n++;
    StreetObj *o = &s->o[i];
    memset(o, 0, sizeof *o);
    s->nstop[i] = 0;
    s->deleting[i] = 0;
    o->name = group_name(tmpl);
    o->tmpl = tmpl;
    o->inst = inst;
    o->kind = kind;
    o->alive = 1;
    memcpy(o->world, world, sizeof o->world);
    memcpy(o->rest, world, sizeof o->rest);
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            o->rot[c * 3 + r] = world[c * 4 + r];
    o->facing[2] = atan2f(world[1], world[0]);      /* its turn about z */
    o->shape = gstr(tmpl, "Shape");
    o->oflags = template_flags(tmpl);
    o->building = building;
    o->cobj = -1;
    if (building >= 0)
        o->oflags |= SF_ATTACHED;                    /* the building that made it (+0x104 already set) */
    object_box(s, o);
    if (attack)                                      /* [R 0x3050F0's AttackObjectInstance| list -> 0x315330] */
        army_osc_init(&o->wobble, tmpl, inst);
    if (kind == SK_STREET)
        street_construct(s, o);
    else
        roof_construct(s, o);
    return i;
}

void streetobj_start(StreetObjs *s, const StreetBuilding *b, int nb)
{
    s->nb = b ? (nb < MAX_BUILDINGS ? nb : MAX_BUILDINGS) : 0;
    if (s->nb)
        memcpy(s->b, b, sizeof *s->b * (size_t)s->nb);
    for (int i = 0; i < s->n; i++) {
        StreetObj *o = &s->o[i];
        if (o->building < 0 && (o->building = building_at(s, pos_of(o))) >= 0)
            o->oflags |= SF_ATTACHED;
        if (getenv("RAMPAGE_STREET"))                /* the list, for finding one */
            printf("street object %d %s (%s) at %.2f %.2f %.2f, box %.1f %.1f %.1f .. %.1f %.1f %.1f, flags %#x, "
                   "building %d\n", i, o->name ? o->name : "?", o->kind == SK_ROOF ? "RoofItem" : "StreetObject",
                   o->world[12], o->world[13], o->world[14], o->box_min[0], o->box_min[1], o->box_min[2],
                   o->box_max[0], o->box_max[1], o->box_max[2], (unsigned)o->oflags, o->building);
    }
}

/* the flight's contacts (collide_mover_step): the street, and the objects of the frame's world its shape reaches */
typedef struct { StreetObjs *s; StreetObj *o; int ground, met[8], nmet; float at[8][3]; } FlyCtx;

static void fly_contact(void *user, int other, const float p[3], const float n[3])
{
    FlyCtx *c = user;
    (void)n;
    if (other < 0) {
        c->ground = 1;
        return;
    }
    for (int k = 0; k < c->nmet; k++)
        if (c->met[k] == other)
            return;
    if (c->nmet < 8) {
        memcpy(c->at[c->nmet], p, sizeof c->at[0]);
        c->met[c->nmet++] = other;
    }
}

/* [R] a thrown object's frame: its SpherePhysicsMover (0x28E440, collide.c) at the game's step; the objects it meets
 * get TakeImpact (SE_IMPACT; a street object of this module its own TakeImpact) and so does it, the street calls
 * OnHitGround (StreetObject.bsf: its HitEffect where it is, requestDelete). RoofItem.bsf has neither: it flies on */
static void fly_step(StreetObjs *s, StreetObj *o)
{
    int i = idx(s, o);
    FlyCtx c = { s, o, 0, { 0 }, 0, { { 0 } } };
    int self = s->cw ? collide_check(s->cw, o->cobj, COLLIDE_TAG(COLLIDE_OWN_STREET, i), 0) : -1;
    collide_mover_step_ex(s->cw, &o->cm, o->cmat, self, TIMESTEP, COLLIDE_F_MONSTER, fly_contact, &c);
    collide_matrix_to16(o->cmat, o->world);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++)
            o->rot[r * 3 + k] = o->world[r * 4 + k];
    o->moved = 1;
    for (int k = 0; k < c.nmet && s->cw; k++) {
        const CollideObj *w = &s->cw->o[c.met[k]];
        if (COLLIDE_TAG_OWNER(w->tag) == COLLIDE_OWN_STREET) {
            if (COLLIDE_TAG_INDEX(w->tag) != i)
                streetobj_take_impact(s, COLLIDE_TAG_INDEX(w->tag));
        } else {
            StreetEvent *e = emit(s, SE_IMPACT, o, o->name, c.at[k]);
            if (e) {
                e->id = w->tag;
                e->level = w->kind;
                e->amount = o->cm.mass;
            }
        }
    }
    if (c.nmet && o->kind == SK_STREET)              /* its own TakeImpact */
        streetobj_take_impact(s, i);
    else if (c.ground && o->kind == SK_STREET && o->alive)
        streetobj_hit_ground(s, i);
}

static void step(StreetObjs *s)
{
    for (int i = 0; i < s->n; i++) {
        StreetObj *o = &s->o[i];
        if (!o->alive)
            continue;
        if (o->pulse_delay >= 0.01f) {               /* onPulse [R 0x309BA0]: the clock runs only with a delay of */
            o->pulse += TIMESTEP;                    /* 0.01 or more; at the delay it is taken off, once a frame */
            if (o->pulse >= o->pulse_delay) {
                o->pulse -= o->pulse_delay;
                if (o->kind == SK_STREET)
                    street_pulse(s, o);
                else
                    roof_pulse(s, o);
            }
        }
        /* 0x309AC0 [R]: attached (0x4000000) to a building whose collapsing storey is set: onBuildingDestruction,
         * once (the flag goes, and with it the building's sway on the object, 0x3098E0) */
        if ((o->oflags & SF_ATTACHED) && o->building >= 0 && o->building < s->nb && s->b[o->building].falling) {
            o->oflags &= ~(uint32_t)SF_ATTACHED;
            if (o->kind == SK_ROOF)
                roof_building_gone(s, o);           /* StreetObject.bsf has none */
        }
        if (o->flying && !s->deleting[i])
            fly_step(s, o);
        float d[3];                                  /* [R 0x314EF0] an AttackObject's wobble, after 0x309AC0: onto
                                                      * the turn it has (the sway's, the fall's), made again */
        if (!s->deleting[i] && army_osc_step(&o->wobble, TIMESTEP, d)) {
            army_osc_turn(o->world, d);
            for (int c = 0; c < 3; c++)
                for (int r = 0; r < 3; r++)
                    o->rot[c * 3 + r] = o->world[c * 4 + r];
            o->moved = 1;
        }
        if (s->deleting[i])
            delete_now(s, o);
    }
}

void streetobj_update(StreetObjs *s, float dt, const StreetMonster *m, int nm, const StreetBuilding *b, int nb)
{
    s->m = m;
    s->nm = m ? nm : 0;
    if (b) {
        s->nb = nb < MAX_BUILDINGS ? nb : MAX_BUILDINGS;
        memcpy(s->b, b, sizeof *s->b * (size_t)s->nb);
    }
    s->frame++;                                      /* one TakeDamage an object a frame */
    s->acc += dt;
    if (s->acc > 0.25f)
        s->acc = 0.25f;
    while (s->acc >= TIMESTEP) {                     /* the scripts count in frames: run them at the game's step */
        s->acc -= TIMESTEP;
        step(s);
    }
    for (int i = 0; i < s->n; i++)                   /* a hit's requestDelete between the steps */
        if (s->deleting[i])
            delete_now(s, &s->o[i]);
}

/* ---------------------------------------------------------------- the monster's side */


/* the object as the collision world has it (WCollisionInfo at its matrix) */
static void as_collide(const StreetObj *o, CollideObj *c)
{
    memset(c, 0, sizeof *c);
    c->shape = o->cshape;
    collide_matrix_from16(c->m, o->world);
    c->flags = COLLIDE_F_WORLD | (o->oflags & ~(uint32_t)SF_HELD);
    c->building = -1;
}

/* the sphere against the object's shape (0x280470: the shape's sphere test, collide_obj_sphere) */
static int sphere_meets(const StreetObj *o, const float c[3], float radius)
{
    CollideObj co;
    float n[3], pt[3], push[3];
    as_collide(o, &co);
    return collide_obj_sphere(&co, c, radius, n, pt, push);
}
static void take_damage(StreetObjs *s, StreetObj *o, int mi, const StreetMonster *who, int type)
{
    o->frame = s->frame;
    if (o->oflags & SF_HELD)                         /* [R] 0x309A80: not while held */
        return;
    if (o->kind == SK_STREET)
        street_damage(s, o, mi, who, type);
    else
        roof_damage(s, o, mi, who);
    if (s->deleting[idx(s, o)])
        delete_now(s, o);
}

int streetobj_hit(StreetObjs *s, int monster, const StreetMonster *who, const float centre[3], float radius,
                  float amount, unsigned record)
{
    (void)amount;                                    /* neither script looks at it */
    int n = 0;
    for (int i = 0; i < s->n; i++) {
        StreetObj *o = &s->o[i];
        if (!o->alive || o->frame == s->frame || !sphere_meets(o, centre, radius))
            continue;
        if ((o->oflags & SF_EFF_NODES) && !collide_hit_once(&o->hits, monster, record))
            continue;                                /* [R] 0x2E33A0: on the record's hit list */
        if (getenv("RAMPAGE_TRACE"))
            printf("  street object %d (%s) hit\n", i, o->name ? o->name : "?");
        take_damage(s, o, monster, who, 0);
        n++;
    }
    return n;
}

int streetobj_touch(StreetObjs *s, int monster, const StreetMonster *who, const float centre[3], float radius)
{
    int n = 0;
    for (int i = 0; i < s->n; i++) {
        StreetObj *o = &s->o[i];
        if (!o->alive || (o->oflags & SF_EFF_NODES) || o->frame == s->frame || !sphere_meets(o, centre, radius))
            continue;
        if (getenv("RAMPAGE_TRACE"))
            printf("  street object %d (%s) touched\n", i, o->name ? o->name : "?");
        take_damage(s, o, monster, who, 1);
        n++;
    }
    return n;
}

int streetobj_take_impact(StreetObjs *s, int i)
{
    if (i < 0 || i >= s->n)
        return 0;
    StreetObj *o = &s->o[i];                         /* TakeImpact [R] */
    if (!o->alive || o->kind != SK_STREET || (o->oflags & SF_HELD))
        return 0;
    if (getenv("RAMPAGE_TRACE"))
        printf("  street object %d (%s) TakeImpact\n", i, o->name ? o->name : "?");
    effect(s, o, ostr(o, "HitEffect"), NULL, 1, NULL, NULL, 0, 0);
    delete_now(s, o);
    return 1;
}

void streetobj_collide_add(StreetObjs *s, CollideWorld *w)
{
    s->cw = w;
    for (int i = 0; i < s->n; i++) {
        StreetObj *o = &s->o[i];
        o->cobj = -1;
        if (!w || !o->alive || s->deleting[i])
            continue;
        CollideObj c;
        as_collide(o, &c);
        /* [R 0x2DA310] a held object stays in the world with its shape: PushOutMonsters cleared (the monster's box no longer
         * meets it), flying objects and shots still do (flag 4 stays: no removeObject; a shot's own owner is no hit) */
        if (o->oflags & SF_HELD)
            c.flags &= ~(uint32_t)COLLIDE_F_PUSHOUT;
        o->cobj = collide_add(w, &c.shape, c.m, c.flags, COLLIDE_TAG(COLLIDE_OWN_STREET, i), 0);
    }
}

int streetobj_impact(StreetObjs *s, const float centre[3], float radius)
{
    int n = 0;
    for (int i = 0; i < s->n; i++) {                 /* TakeImpact [R] */
        StreetObj *o = &s->o[i];
        if (!o->alive || o->kind != SK_STREET || (o->oflags & SF_HELD) || !sphere_meets(o, centre, radius))
            continue;
        effect(s, o, ostr(o, "HitEffect"), NULL, 1, NULL, NULL, 0, 0);
        delete_now(s, o);
        n++;
    }
    return n;
}

int streetobj_force_damage(StreetObjs *s, const char *name, const float pos[3])
{
    /* gWorld.findWorldObject(name) (0x279E70 -> 0x2852A0 -> 0x284930) [R]: by name alone (h = h x 131 + c, case
     * folded, 127 buckets; the stored hash +0x8C and the name +0x88 compared case-blind), the first of its chain;
     * addObject puts a named object (flag 0x1000) at its chain's head (0x284A90), so the newest one wins */
    int best = -1;
    (void)pos;
    for (int i = s->n - 1; i >= 0 && best < 0; i--) {
        const StreetObj *o = &s->o[i];
        if (o->alive && o->kind == SK_ROOF && same_text(o->name, name))
            best = i;
    }
    if (best < 0)
        return -1;
    StreetObj *o = &s->o[best];                      /* forceDamage [R]: flags 2, 99 hits, TakeDamage(none, 1000, 0) */
    o->flags |= 2;
    o->num_hits = 99;
    roof_damage(s, o, -1, NULL);
    return best;
}

/* ---------------------------------------------------------------- the pick-up */

void streetobj_pickup(StreetObjs *s, int i)          /* onPickup [R] */
{
    if (i < 0 || i >= s->n || !s->o[i].alive || s->o[i].kind != SK_STREET)
        return;
    StreetObj *o = &s->o[i];
    set_shape(s, o, ostr(o, "DamageShape"));
    sound(s, o, ostr(o, "PickupSound"), 0);
    effect(s, o, ostr(o, "HitEffect"), NULL, 1, NULL, NULL, 0, 0);
    o->oflags &= ~(uint32_t)SF_PUSH_OUT;
    o->oflags |= SF_HELD;                            /* the engine's BeingHeld */
}

void streetobj_throw(StreetObjs *s, int i, int monster)   /* onThrow [R] */
{
    if (i < 0 || i >= s->n || !s->o[i].alive || s->o[i].kind != SK_STREET)
        return;
    StreetObj *o = &s->o[i];
    add_stats(s, o, monster);
    o->oflags &= ~(uint32_t)(SF_PICKUP | SF_HELD);
}

void streetobj_hit_ground(StreetObjs *s, int i)      /* OnHitGround [R] */
{
    if (i < 0 || i >= s->n || !s->o[i].alive || s->o[i].kind != SK_STREET)
        return;
    StreetObj *o = &s->o[i];
    effect(s, o, ostr(o, "HitEffect"), pos_of(o), 0, NULL, NULL, 0, 0);
    delete_now(s, o);
}

int streetobj_nearest_pickup(const StreetObjs *s, const float hand[3], const float root[3], const float forward[3],
                             float radius)
{
    int best = -1;
    float bd = 3.4e38f;
    for (int i = 0; i < s->n; i++) {
        const StreetObj *o = &s->o[i];
        if (!o->alive || s->deleting[i] || o->flying || !(o->oflags & SF_PICKUP) || (o->oflags & SF_HELD))
            continue;
        const float *at = pos_of(o);
        float dx = at[0] - hand[0], dy = at[1] - hand[1], dz = at[2] - hand[2], d = dx * dx + dy * dy + dz * dz;
        if (d > radius * radius || d >= bd)
            continue;
        if ((at[0] - root[0]) * forward[0] + (at[1] - root[1]) * forward[1] <= 0)   /* in front of the root */
            continue;
        best = i;
        bd = d;
    }
    return best;
}

void streetobj_grab(StreetObjs *s, int i)
{
    if (i < 0 || i >= s->n || !s->o[i].alive)
        return;
    StreetObj *o = &s->o[i];
    o->oflags = (o->oflags | SF_HELD) & ~(uint32_t)(SF_PICKUP | SF_PUSH_OUT);   /* [R] 0x2DA310 */
    o->flying = 0;
    o->oflags &= ~(uint32_t)SF_ATTACHED;             /* its mover is the hand's now */
    if (o->kind == SK_STREET)
        streetobj_pickup(s, i);                      /* script event 8 */
}

/* [R] the HeldObjectMover (0x310ED0): the MajorAxis row (default 2) is the hand's z, the next its y, the third their
 * cross; at the hand's place plus HoldOffset (MirroredHoldOffset when mirrored) in the hand's frame. [G: its blend
 * in (0x310B50, 0.1 s from the ground) is left out, as vehicle.c's] */
void streetobj_hold(StreetObjs *s, int i, const float hand[16], int mirrored)
{
    if (i < 0 || i >= s->n || !s->o[i].alive || !(s->o[i].oflags & SF_HELD))
        return;
    StreetObj *o = &s->o[i];
    float off[3] = { 0, 0, 0 };
    Group g = group_sub(o->tmpl, mirrored ? "MirroredHoldOffset" : "HoldOffset");
    Record r[3];
    if (g.at && group_items(g, r, 3) == 3)
        for (int k = 0; k < 3; k++)
            off[k] = record_num(r[k]);
    int major = (int)onum(o, "MajorAxis", 2);
    if (major < 0 || major > 2)
        major = 2;
    int i1 = (major + 1) % 3, i2 = (major + 2) % 3;
    float *ax = o->rot;
    for (int k = 0; k < 3; k++) {
        ax[major * 3 + k] = hand[8 + k];
        ax[i1 * 3 + k] = hand[4 + k];
    }
    ax[i2 * 3 + 0] = ax[major * 3 + 1] * ax[i1 * 3 + 2] - ax[major * 3 + 2] * ax[i1 * 3 + 1];
    ax[i2 * 3 + 1] = ax[major * 3 + 2] * ax[i1 * 3 + 0] - ax[major * 3 + 0] * ax[i1 * 3 + 2];
    ax[i2 * 3 + 2] = ax[major * 3 + 0] * ax[i1 * 3 + 1] - ax[major * 3 + 1] * ax[i1 * 3 + 0];
    for (int c = 0; c < 3; c++) {
        for (int k = 0; k < 3; k++)
            o->world[c * 4 + k] = ax[c * 3 + k];
        o->world[c * 4 + 3] = 0;
    }
    for (int k = 0; k < 3; k++)
        o->world[12 + k] = hand[12 + k] + hand[k] * off[0] + hand[4 + k] * off[1] + hand[8 + k] * off[2];
    o->world[15] = 1;
    o->moved = 1;
}

void streetobj_fly(StreetObjs *s, int i, int monster, int eat, const float vel[3])
{
    if (i < 0 || i >= s->n || !s->o[i].alive)
        return;
    StreetObj *o = &s->o[i];
    o->oflags &= ~(uint32_t)SF_HELD;
    /* [R] 0x28DE70 / 0x28F220: the template's Gravity (DefObjectProps 35), Mass 1, Restitution 0.8, SlidingFriction
     * 0.4, InertiaSolidity 1 unless set; its radius sqrt(+0x34) of its shape; no spin */
    float r = collide_mover_radius(&o->cshape);
    if (r < 0.05f)
        r = o->cshape.radius > 0.05f ? o->cshape.radius : 1;
    collide_mover_init(&o->cm, r, onum(o, "Gravity", 9.81f), onum(o, "Mass", 1), onum(o, "Restitution", 0.8f),
                       onum(o, "SlidingFriction", 0.4f), onum(o, "InertiaSolidity", 1));
    static const float none[3] = { 0, 0, 0 };
    collide_mover_spin(&o->cm, none);
    if (vel)
        memcpy(o->cm.vel, vel, sizeof o->cm.vel);
    collide_matrix_from16(o->cmat, o->world);
    o->flying = 1;
    (void)eat;                                       /* onThrow's bWasEaten: StreetObject.bsf does not look */
    if (o->kind == SK_STREET)
        streetobj_throw(s, i, monster);
}

void streetobj_place(StreetObjs *s, int i, const float world[16])
{
    if (i < 0 || i >= s->n)
        return;
    StreetObj *o = &s->o[i];
    memcpy(o->world, world, sizeof o->world);
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            o->rot[c * 3 + r] = world[c * 4 + r];
    o->moved = 1;
}

/* ---------------------------------------------------------------- reading */

int streetobj_count(const StreetObjs *s) { return s->n; }
const StreetObj *streetobj_get(const StreetObjs *s, int i) { return i >= 0 && i < s->n ? &s->o[i] : NULL; }

int streetobj_events(StreetObjs *s, StreetEvent *out, int max)
{
    int n = s->nev < max ? s->nev : max;
    memcpy(out, s->ev, sizeof *out * (size_t)n);
    memmove(s->ev, s->ev + n, sizeof *s->ev * (size_t)(s->nev - n));
    s->nev -= n;
    return n;
}
