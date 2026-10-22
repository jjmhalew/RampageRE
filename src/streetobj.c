/* streetobj.c - the street and roof objects: StreetObject.bsf, RoofItem.bsf, MerryGoRound.bsf, Sign.bsf, the
 * billboards (Billboard.bsf, Billboard_2Part.bsf, Billboard_George.bsf) and AnimatedSign.bsf (streetobj.h,
 * docs/STREETOBJ_NOTES.md). [R] read in the scripts (build/scripts/StreetObject.scr, RoofItem.scr, ...) or the ELF,
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
    float amount;                                    /* the blow's damage, for a TakeDamage that reads it (Crane) */
    float dir[3];                                    /* the blow's direction, TakeDamage's (the fountain's) */
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
static const char *const TYPED[] = { "", "Effect|", "Sound|", "Shape|", "Color|", "Script|", "OTRef|" };

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
/* getNearestMonster [R 0x2B2CC0, the RScriptObject binding 0x37 of 0x2AA080, the same code as the Actor's 0x2B26F0]:
 * the nearest player monster by 3D distance (0x2B27E8 is sqrt.s), starting at 1000: none farther than 1000, so in a
 * block any; -1 with none */
static int nearest_monster(const StreetObjs *s, const float p[3])
{
    int best = -1;
    float bd = 1000;
    for (int i = 0; i < s->nm; i++) {
        float d[3] = { s->m[i].pos[0] - p[0], s->m[i].pos[1] - p[1], s->m[i].pos[2] - p[2] };
        float q = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
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

/* ---------------------------------------------------------------- MerryGoRound.bsf */

/* Construct [R]: state 1, a pulse every 0.2 s (currPulseTime 0.2), the object flag 0x2000000 (as the trains': no code
 * tests it, rail.h), and a RotationMover of (0, 0, 0) set on it: it stands till its first pulse */
static void merry_construct(StreetObj *o)
{
    o->state = 1;
    set_pulse(o, 0.2f);
    o->spin_step = 0.2f;
    o->oflags |= 0x2000000;
    o->spin = 0;
    o->grabbed = -1;
}

/* the RotationMover's step [R 0x31C540]: its rate x the world's step (gp-0x6CFC +4) added to the angles read back
 * from the matrix (0x101220: z = atan2, within -pi .. pi, as getFacing().z), the matrix made again from them
 * (0x101330: Rz Rx Ry). The ride stands upright, so x and y stay 0 */
static void merry_turn(StreetObj *o)
{
    float e[3] = { 0, 0, o->facing[2] + o->spin * TIMESTEP };
    e[2] = atan2f(sinf(e[2]), cosf(e[2]));
    set_facing(o, e);
}

/* the monster's side of the script's calls: queueFacingZRequest, queuePushRequest, setVel(v, 0), playReaction */
static StreetEvent *monster_call(StreetObjs *s, StreetObj *o, int type, int m)
{
    StreetEvent *e = emit(s, type, o, NULL, NULL);
    e->monster = m;
    return e;
}

/* onPulse [R] */
static void merry_pulse(StreetObjs *s, StreetObj *o)
{
    if (o->state == 2) {                             /* slowing: 30% of its turn a second, to a quarter turn */
        if (fabsf(o->spin) <= PI / 2)
            o->state = 1;
        else
            o->spin -= o->spin * 0.3f * o->spin_step;   /* rMover.setRotation(angleVel3) */
    } else if (o->state == 3) {                      /* a monster on it, every frame */
        if (o->grabbed < 0 || o->grabbed >= s->nm)  /* grabbedMonster.isValid() */
            o->state = 2;
        else {
            int m = o->grabbed;
            float angle = o->facing[2];              /* getFacing().z */
            o->grab -= o->spin_step;
            if (o->grab > 0) {                       /* faster, the monster at its centre facing its turn */
                o->spin *= 1.02f;                    /* scaleMerryGoRoundRotation(1.02) */
                monster_call(s, o, SE_MONSTER_FACING, m)->amount = o->facing[2];
                monster_call(s, o, SE_MONSTER_PUSH, m);
            } else {                                 /* flung off along (cos, sin) of its turn, 1.2 x its rate */
                float k = o->spin * 1.2f;
                StreetEvent *e = monster_call(s, o, SE_MONSTER_VEL, m);
                e->v[0] = cosf(angle) * k;
                e->v[1] = sinf(angle) * k;
                e->v[2] = 1.2f * k;
                monster_call(s, o, SE_MONSTER_REACTION, m)->name = "React_Ground_Slip_Heavy";
                monster_call(s, o, SE_MONSTER_FACING, m)->amount = s->m[m].facing + PI;   /* turned round */
                o->spin *= 0.3f;                     /* scaleMerryGoRoundRotation(0.3) */
                set_pulse(o, 0.2f);
                o->spin_step = 0.2f;
                o->no_hit = 2.0f;
                o->state = 2;
            }
        }
    } else if (o->state == 1 && fabsf(o->spin) < PI / 2)
        o->spin += PI / 2;                           /* rotateMerryGoRound(pi / 2): idling */
}

/* TakeDamage(who, amount, type, direction) [R]: any type, from anything; each one takes currPulseTime off
 * durationNoHit, and only with that run out (and no monster on it) does it do anything: 2 s more, and for a monster in
 * a move whose name starts with "Ground" (getMoveName().fFind("Ground") == 0) the spin and the ride */
static void merry_damage(StreetObjs *s, StreetObj *o, int mi, const StreetMonster *who)
{
    o->no_hit -= o->spin_step;
    if (o->no_hit > 0 || o->state == 3)
        return;
    o->no_hit = 2.0f;
    if (!who || mi < 0)
        return;                                      /* gWorld.coerceToMonster(who) not valid */
    int ground = who->move && strncmp(who->move, "Ground", 6) == 0;
    if (fabsf(o->spin) < PI * 6 && ground) {
        float r = frand(s);
        if (r < 0.5f)
            r += 0.5f;
        o->spin += PI * 4 * r;                       /* rotateMerryGoRound(4 pi x 0.5 .. 1) */
        effect(s, o, ostr(o, "AttachedEffect"), pos_of(o), 0, NULL, NULL, 0, 0);   /* the twirl, where it stands */
        effect(s, o, gstr(o->tmpl, "HitEffect"), NULL, 1, NULL, NULL, 0, 0);   /* getTemplateStringProp: on it */
    }
    if (o->state == 1)
        o->state = 2;
    if (o->state == 2 && ground) {                   /* it takes the monster on: a pulse every frame for 1.3 s */
        o->grab = 1.3f;
        o->spin_step = TIMESTEP;                     /* gWorld.getTimeStep() */
        set_pulse(o, o->spin_step);
        o->grabbed = mi;
        o->state = 3;
        merry_pulse(s, o);
    }
}

/* ---------------------------------------------------------------- Sign.bsf */

/* the signs hung on the buildings' fronts (65 templates: CasinoVerticalSign, SignParking1, CasinoHorizontalSign,
 * GoldrushSign, LuckysSign, ...) [R Sign.scr]. Construct: flags 0, no hits, and a fallRot onPulse never reads (its x
 * sign one draw of gWorld.frand); no pulse until it comes loose */
static void sign_construct(StreetObjs *s, StreetObj *o)
{
    (void)o;
    (void)frand(s);                                  /* gWorld.frand() < 0.5: fallRot.x's sign, unused */
}

/* it comes loose: flags 1, a pulse every frame (setPulseDelay(gWorld.getTimeStep())) */
static void sign_loose(StreetObj *o)
{
    o->flags |= 1;
    set_pulse(o, TIMESTEP);
}

/* onPulse [R]: down 10 and towards the street (-y) 3 a second, turned (pi/10, 3pi/20, 0) a second about its own
 * axes; at z <= -0.25 its GroundEffect there, and it returns 4|8|16: deleted */
static void sign_pulse(StreetObjs *s, StreetObj *o)
{
    o->world[14] -= 10 * o->pulse_delay;
    o->world[13] += -3 * o->pulse_delay;
    float rot[3] = { PI * 0.1f * TIMESTEP, PI * 0.15f * TIMESTEP, 0 };
    rotate_local(o, rot);
    if (o->world[14] <= -0.25f) {
        effect(s, o, gstr(o->tmpl, "GroundEffect"), pos_of(o), 0, NULL, NULL, 0, 0);
        request_delete(s, o);
    }
}

/* onBuildingDestruction [R]: still up, it comes loose in its DetachEffect (the electric signs' sparks), where it is,
 * no longer IsObstacle nor OnlyHitWithEffNodes */
static void sign_building_gone(StreetObjs *s, StreetObj *o)
{
    if (o->flags & 1)
        return;
    sign_loose(o);
    effect(s, o, gstr(o->tmpl, "DetachEffect"), pos_of(o), 0, NULL, NULL, 0, 0);
    o->oflags &= ~(uint32_t)(SF_OBSTACLE | SF_EFF_NODES);
}

/* TakeDamage(who, amount, type, direction) [R]: from a monster, any type; at Hits (0; the logo signs 4) and with
 * TakeDamage (1) it comes loose, its HitEffect on it, no longer IsObstacle nor OnlyHitWithEffNodes, and it counts */
static void sign_damage(StreetObjs *s, StreetObj *o, int mi, const StreetMonster *who)
{
    if (!who || mi < 0)
        return;                                      /* gWorld.coerceToMonster(who) not valid */
    o->num_hits++;
    if ((o->flags & 1) || (int)onum(o, "TakeDamage", 1) == 0 || o->num_hits < (int)onum(o, "Hits", 0))
        return;
    sign_loose(o);
    effect(s, o, gstr(o->tmpl, "HitEffect"), NULL, 1, NULL, NULL, 0, 0);
    o->oflags &= ~(uint32_t)(SF_OBSTACLE | SF_EFF_NODES);
    add_stats(s, o, mi);
}

/* ---------------------------------------------------------------- Billboard.bsf, Billboard_2Part.bsf */

/* the billboards on the roofs and the Las Vegas marquees (32 templates: ScumSodaBillboard, BillboardBeans, the
 * BillboardBreakaway* with their fronts, MidwayBillboard, BillboardKenoMarquee, BirdsOnlyInvisBillboard1, ...)
 * [R Billboard.scr]. Construct: Type (getStringProp, "Generic"), requiredHits 3, no pulse */
static int is_type(const StreetObj *o, const char *t) { return same_text(o->type, t); }

static void billboard_construct(StreetObj *o, const char *type)
{
    o->type = type ? type : "Generic";
    o->required = 3;
    o->bird_fx = o->bubble_fx = -1;
}

/* getShapeBounds' top [G: the model's box in its own frame; the collision box's without one] */
static float shape_top(StreetObjs *s, const StreetObj *o)
{
    float mn[3], mx[3];
    if (o->shape && s->host.shape_box && s->host.shape_box(s->host.ctx, o->shape, mn, mx))
        return mx[2];
    return o->box_max[2];
}

static StreetObj *attachment(StreetObjs *s, const StreetObj *o, int i)   /* attachments[i], while isValid */
{
    int k = i < o->nattach ? o->attach[i] : -1;
    return k >= 0 && k < s->n && s->o[k].alive && !s->deleting[k] ? &s->o[k] : NULL;
}

/* onInit [R]: the pigeons on its top (one gWorld.frand(), always above -0.33: with useBirdFx 1, any case of the key),
 * 0.5 behind its place; then its attachments are made (createScriptObject), which the caller did
 * (streetobj_add_attachment), so here a Billboard_2Part front's Construct draws its fRotDir (+1 when
 * gWorld.frand() < 0.5), after the birds' and in attachment order */
static void billboard_init(StreetObjs *s, StreetObj *o)
{
    float at[3] = { o->world[12], o->world[13] + 0.5f, o->world[14] + shape_top(s, o) };
    if (frand(s) > -0.33f && (int)onum(o, "useBirdFx", 1) == 1) {
        o->bird_fx = effect(s, o, "Pigeon_walk", at, 0, NULL, NULL, 0, 0);
        memcpy(o->bird_at, at, sizeof at);
    }
    for (int i = 0; i < o->nattach; i++)
        if (o->attach[i] >= 0 && o->attach[i] < s->n && s->o[o->attach[i]].kind == SK_2PART)
            s->o[o->attach[i]].rot_dir = frand(s) < 0.5f ? 1.0f : -1.0f;
}

/* the birds fly off (birdEffect.stop(), Pigeon_fly where they sat, or [the hit] on its top now) */
static void birds_fly(StreetObjs *s, StreetObj *o, int from_top)
{
    if (o->bird_fx < 0)
        return;
    float at[3] = { o->world[12], o->world[13], o->world[14] + shape_top(s, o) };
    effect(s, o, "Pigeon_fly", from_top ? at : o->bird_at, 0, NULL, NULL, 0, 0);
    effect_stop(s, o, o->bird_fx);
    o->bird_fx = -1;
}

/* Billboard_2Part.bsf's onStartFall [R]: a pulse every frame and a SplineMover at 10 a second from where it is to 3
 * out (-y) and 3 up, then down to z -3 [G: straight between the points]; deleted at its end (onMoverCallback 2). A
 * front still falling starts again from where it is: the billboard's next blow calls it for each one still valid */
static void part_start_fall(StreetObj *o)
{
    if (o->kind != SK_2PART)
        return;
    set_pulse(o, TIMESTEP);
    memcpy(o->path[0], pos_of(o), sizeof o->path[0]);
    memcpy(o->path[1], o->path[0], sizeof o->path[1]);
    o->path[1][1] -= 3;
    o->path[1][2] += 3;
    memcpy(o->path[2], o->path[1], sizeof o->path[2]);
    o->path[2][2] = -3;
    o->path_at = 0;
    o->on_path = 1;
}
static void part_move(StreetObjs *s, StreetObj *o)
{
    float d1[3], d2[3];
    for (int k = 0; k < 3; k++) {
        d1[k] = o->path[1][k] - o->path[0][k];
        d2[k] = o->path[2][k] - o->path[1][k];
    }
    float l1 = len3(d1), l2 = len3(d2), t = (o->path_at += 10 * TIMESTEP);
    for (int k = 0; k < 3; k++)
        o->world[12 + k] = t < l1 ? o->path[0][k] + d1[k] * t / l1 :
                           t < l1 + l2 ? o->path[1][k] + d2[k] * (t - l1) / l2 : o->path[2][k];
    o->moved = 1;
    if (t >= l1 + l2)
        request_delete(s, o);
}
/* onPulse [R]: turning about its own y at 2 pi a second, fRotDir (+1 when Construct's gWorld.frand() < 0.5, else -1) */
static void part_pulse(StreetObj *o)
{
    float f[3] = { o->facing[0], o->facing[1] + o->rot_dir * 2 * PI * TIMESTEP, o->facing[2] };
    set_facing(o, f);
}

/* the attachments go: AtchDestFx where each is, then deleted, or for a 2Part its front falls (onStartFall) */
static void billboard_drop(StreetObjs *s, StreetObj *o)
{
    const char *fx = gstr(o->tmpl, "AtchDestFx");
    for (int i = 0; i < o->nattach; i++) {
        StreetObj *a = attachment(s, o, i);
        if (!a)
            continue;
        effect(s, a, fx, pos_of(a), 0, NULL, NULL, 0, 0);
        if (!is_type(o, "2Part"))
            request_delete(s, a);
        else
            part_start_fall(a);
    }
}
static void billboard_atch_fx(StreetObjs *s, StreetObj *o, int which, int *any)   /* Atch<i>DestFx<which> */
{
    for (int i = 0; i < o->nattach; i++) {
        char key[32];
        snprintf(key, sizeof key, "Atch%dDestFx%d", i + 1, which);
        const char *fx = gstr(o->tmpl, key);
        if (!fx)
            continue;
        StreetObj *a = attachment(s, o, i);
        if (a)
            effect(s, a, fx, NULL, 1, NULL, NULL, 0, 0);
        if (any)
            *any = 1;
    }
}

/* TakeDamage(who, ...) [R], from a monster only, any type (the templates are OnlyHitWithEffNodes: a blow, once an
 * attack). The first: its SoundDestroy, DestShape, DestEffect on it (not BirdsOnly), each attachment's Atch<i>DestFx1
 * on it (then state 2: the next blow takes them), else (and always for a 2Part) the attachments go; the birds fly.
 * State 2: SoundDestroy, Atch<i>DestFx2, the attachments go, state 1. State 1: three more, each 0.5 s more shaking,
 * a pulse every frame and addStats (counted once); at the third it starts to fall (fFalling 4) in
 * Collapse_Floor_redbrick_comp (not BirdsOnly) */
static void billboard_damage(StreetObjs *s, StreetObj *o, int mi, const StreetMonster *who)
{
    if (!who || mi < 0)
        return;
    if (o->state == 0) {
        int kept = 0;
        sound(s, o, ostr(o, "SoundDestroy"), 1);
        o->state = 1;
        set_shape(s, o, gstr(o->tmpl, "DestShape"));
        if (!is_type(o, "BirdsOnly"))
            effect(s, o, gstr(o->tmpl, "DestEffect"), NULL, 1, NULL, NULL, 0, 0);
        billboard_atch_fx(s, o, 1, &kept);
        if (kept)
            o->state = 2;
        if (!kept || is_type(o, "2Part"))
            billboard_drop(s, o);
        birds_fly(s, o, 1);
    } else if (o->state == 2) {
        o->state = 1;
        sound(s, o, ostr(o, "SoundDestroy"), 1);
        billboard_atch_fx(s, o, 2, NULL);
        billboard_drop(s, o);
    } else if (o->state == 1 && o->required > 0) {
        set_pulse(o, TIMESTEP);
        o->required--;
        o->shake += 0.5f;
        add_stats(s, o, mi);
        if (o->required == 0) {
            o->falling = 4;
            if (!is_type(o, "BirdsOnly"))
                effect(s, o, "Collapse_Floor_redbrick_comp", pos_of(o), 0, NULL, NULL, 0, 0);
        }
    }
}

/* onBuildingDestruction [R]: (before it has fallen) the attachments go when it is still whole or a Midway one, the
 * birds fly from where they sat, and it falls (fFalling 4, every frame) */
static void billboard_building_gone(StreetObjs *s, StreetObj *o)
{
    if (o->state >= 4)
        return;
    if (o->state < 1 || is_type(o, "Midway")) {
        const char *fx = gstr(o->tmpl, "AtchDestFx");
        for (int i = 0; i < o->nattach; i++) {
            StreetObj *a = attachment(s, o, i);
            if (a) {
                effect(s, a, fx, pos_of(a), 0, NULL, NULL, 0, 0);
                request_delete(s, a);
            }
        }
    }
    birds_fly(s, o, 0);
    o->state = 1;
    o->required = 0;
    set_pulse(o, TIMESTEP);
    o->falling = 4;
}

/* the fall (Billboard.bsf's onPulse with no hits left, Billboard_George.bsf's processDestroy) [R]: over about x at
 * DestFallDir x pi a second; at +-pi/2 (George's then set to pi x DestFallDir) its DestFallEffect on it, and it goes
 * (4|8|16), unless fFalling still runs: then it sits at its building's top (it goes down with it) until that is
 * out, 4 s from the start. Returns whether it goes */
static int billboard_fall(StreetObjs *s, StreetObj *o, int george)
{
    int go = 0;
    if (!o->fall_done) {
        float fdr = onum(o, "DestFallDir", 1), f[3] = { o->facing[0] + fdr * TIMESTEP * PI, o->facing[1],
                                                       o->facing[2] };
        if (f[0] >= PI / 2 || f[0] <= -PI / 2) {
            go = 1;
            o->fall_done = 1;
            effect(s, o, gstr(o->tmpl, "DestFallEffect"), NULL, 1, NULL, NULL, 0, 0);
            if (george)
                f[0] = PI * fdr;
            else
                o->state = 5;
            set_pulse(o, 0);
        }
        set_facing(o, f);
    }
    if (o->falling > 0) {
        o->falling -= TIMESTEP;
        o->world[14] = building_height(s, o);
        o->moved = 1;
        go = o->falling <= 0;
        if (!go)
            set_pulse(o, TIMESTEP);
        else if (george)
            o->state = 4;
    }
    return go;
}
/* the shaking: each frame a step of -1/16 .. 1/16 in x and the same in z, not undone (it wanders) [R] */
static float billboard_shake(StreetObjs *s, StreetObj *o)
{
    o->shake -= TIMESTEP;
    if (o->shake < 0)
        o->shake = 0;
    float sk = frand(s) * 0.125f - 0.125f / 2;
    o->world[12] += sk;
    o->world[14] += sk;
    o->moved = 1;
    return sk;
}

/* onPulse [R] */
static void billboard_pulse(StreetObjs *s, StreetObj *o)
{
    if (o->required <= 0) {
        if (billboard_fall(s, o, 0))
            request_delete(s, o);
    } else if (o->shake > 0)
        billboard_shake(s, o);
}

/* ---------------------------------------------------------------- Billboard_George.bsf */

/* GeorgeBillboard (New York): George pours Scum Soda into his mouth, his hand (the bottle, Attachment2) and jaw
 * (Attachment1) turned every frame (Process) [R Billboard_George.scr]. Construct: Type (the template's), requiredHits
 * 3, setFlags(0x8000): OnlyHitWithEffNodes alone */
static void george_construct(StreetObj *o)
{
    billboard_construct(o, gstr(o->tmpl, "Type"));
    o->dir = 1;
    o->oflags = SF_EFF_NODES | (o->oflags & SF_ATTACHED);   /* the building's hold is set apart (0x309E20) */
}
/* onInit [R]: the pigeons on its top when gWorld.frand() > 0.33; the jaw and the bottle where the caller made them */
static void george_init(StreetObjs *s, StreetObj *o)
{
    float at[3] = { o->world[12], o->world[13], o->world[14] + shape_top(s, o) };
    if (frand(s) > 0.33f) {
        o->bird_fx = effect(s, o, "Pigeon_walk", at, 0, NULL, NULL, 0, 0);
        memcpy(o->bird_at, at, sizeof at);
    }
    for (int i = 0; i < 2; i++) {
        StreetObj *a = attachment(s, o, i);
        if (a)
            memcpy(i ? o->hand_loc : o->jaw_loc, pos_of(a), sizeof o->jaw_loc);
    }
}
static void turn_y(StreetObj *a, float y)            /* setFacing((0, y, 0)) */
{
    float f[3] = { 0, y, 0 };
    if (a)
        set_facing(a, f);
}
static void put(StreetObj *a, const float p[3])     /* setPoint */
{
    if (a) {
        memcpy(a->world + 12, p, 3 * sizeof(float));
        a->moved = 1;
    }
}
static const float SODA_POUR[3] = { 22.25f, 18.75f, 42.95f }, SODA_MOUTH[3] = { 21.5f, 18.35f, 39.75f };

/* processUndamaged [R]: the hand up to pi/3.5 at that a second (soda_pour at its top, then 1 s still), down again
 * (0.75 s still at the bottom); the jaw with it to pi/4, turned pi/10 back */
static void george_undamaged(StreetObjs *s, StreetObj *o)
{
    if (o->delay > 0) {
        o->delay -= TIMESTEP;
        return;
    }
    o->hand += TIMESTEP * (PI / 3.5f) * o->dir;
    if (o->hand >= PI / 3.5f) {
        o->hand = PI / 3.5f;
        o->dir = -1;
        o->delay = 1;
        effect(s, o, "soda_pour", SODA_POUR, 0, NULL, NULL, 0, 0);
    } else if (o->hand <= 0) {
        o->hand = 0;
        o->dir = 1;
        o->delay = 0.75f;
    }
    turn_y(attachment(s, o, 1), -o->hand);
    o->jaw += TIMESTEP * (PI / 4) * o->dir;
    if (o->jaw >= PI / 4)
        o->jaw = PI / 4;
    else if (o->jaw <= 0)
        o->jaw = 0;
    turn_y(attachment(s, o, 0), o->jaw - PI / 10);
}
/* processDamaged [R]: the hand up to 0.62 pi at 2 pi a second (soda_pour while it is there), 3 out and 1 down over
 * 0.25 s, the jaw 1.85 down at 5.5 a second; all three there, the bubbles at George's mouth, once */
static void george_damaged(StreetObjs *s, StreetObj *o)
{
    if (o->delay >= 3)
        return;
    o->delay = 0;
    o->hand += TIMESTEP * PI * 2;
    if (o->hand >= PI * 0.62f) {
        o->hand = PI * 0.62f;
        o->delay += 1;
        effect(s, o, "soda_pour", SODA_POUR, 0, NULL, NULL, 0, 0);
    }
    StreetObj *hand = attachment(s, o, 1), *jaw = attachment(s, o, 0);
    turn_y(hand, -o->hand);
    o->hand2 += TIMESTEP;
    if (o->hand2 > 0.25f) {
        o->hand2 = 0.25f;
        o->delay += 1;
    }
    float f = o->hand2 / 0.25f, p[3] = { o->hand_loc[0] - 3 * f, o->hand_loc[1], o->hand_loc[2] - f };
    put(hand, p);
    o->jaw += TIMESTEP * 5.5f;
    if (o->jaw > 1.85f) {
        o->jaw = 1.85f;
        o->delay += 1;
    }
    float q[3] = { o->jaw_loc[0], o->jaw_loc[1], o->jaw_loc[2] - o->jaw };
    put(jaw, q);
    if (o->delay >= 3)
        o->bubble_fx = effect(s, o, "soda_george_mouth", SODA_MOUTH, 0, NULL, NULL, 0, 0);
}
/* processDestroy [R]: the fall with no hits left; else the shaking, the hand and jaw with it */
static void george_destroy(StreetObjs *s, StreetObj *o)
{
    if (o->required <= 0) {
        if (billboard_fall(s, o, 1))
            request_delete(s, o);
    } else if (o->shake > 0) {
        float sk = billboard_shake(s, o);
        for (int i = 0; i < 2; i++) {
            StreetObj *a = attachment(s, o, i);
            if (a) {
                float p[3] = { a->world[12] + sk, a->world[13], a->world[14] + sk };
                put(a, p);
            }
        }
    }
}
static void george_process(StreetObjs *s, StreetObj *o)   /* Process, every frame */
{
    if (o->state == 0)
        george_undamaged(s, o);
    else if (o->state == 1)
        george_damaged(s, o);
    else if (o->state == 3)
        george_destroy(s, o);
}
/* the hand and the jaw go (AtchDestFx where each is, with fx); with fx the bubbles stop too: a building falling once
 * the sign was hit leaves them running where they were, in the air [R onBuildingDestruction: both under state < 1] */
static void george_drop(StreetObjs *s, StreetObj *o, int fx)
{
    const char *name = fx ? gstr(o->tmpl, "AtchDestFx") : NULL;
    for (int i = 1; i >= 0; i--) {
        StreetObj *a = attachment(s, o, i);
        if (a) {
            effect(s, a, name, pos_of(a), 0, NULL, NULL, 0, 0);
            request_delete(s, a);
        }
    }
    if (fx && o->bubble_fx >= 0)
        effect_stop(s, o, o->bubble_fx);
    o->bubble_fx = -1;
}
/* TakeDamage [R]: from anything (the score only from a monster), not once its building has gone. The first blow:
 * DestShape, DestEffect on it, the birds fly, state 1 (processDamaged). Then state 3: two more shake it 0.5 s each,
 * the third counts, and the hand and the jaw go: it falls */
static void george_damage(StreetObjs *s, StreetObj *o, int mi, const StreetMonster *who)
{
    if (o->fallen)
        return;
    if (o->state == 0) {
        o->state = 1;
        o->delay = 0;
        o->hand = 0;
        set_shape(s, o, gstr(o->tmpl, "DestShape"));
        effect(s, o, gstr(o->tmpl, "DestEffect"), NULL, 1, NULL, NULL, 0, 0);
        birds_fly(s, o, 1);
    } else if (o->state == 1 || o->state == 3) {
        o->state = 3;
        if (o->required > 1) {
            o->required--;
            o->shake += 0.5f;
        } else {
            if (who && mi >= 0)
                add_stats(s, o, mi);
            o->required--;
            george_drop(s, o, 1);
        }
    }
}
/* onBuildingDestruction [R]: it falls with its building (fFalling 4); the hand and the jaw go (their AtchDestFx only
 * while it was whole), the birds fly from where they sat */
static void george_building_gone(StreetObjs *s, StreetObj *o)
{
    if (o->state < 4) {
        o->required = 0;
        o->falling = 4;
        george_drop(s, o, o->state < 1);
        birds_fly(s, o, 0);
        o->state = 3;
    }
    o->fallen = 1;
}

/* ---------------------------------------------------------------- AnimatedSign.bsf */

/* VegasCowboySign: an Actor on its building's front, its own clip (Sign_Cowboy's arm) on a loop [R AnimatedSign.scr].
 * Construct: flags 0 and a fallRot onPulse never reads (one gWorld.frand: its x sign). A monster's blow (it is
 * OnlyHitWithEffNodes), a thrown object or its building coming down sets it loose: its HitEffect on it (the blow
 * counts too), or the DetachEffect where it is; then it falls 10 a second, turning (pi/10, 3pi/20, 0) a second about
 * its own axes, its GroundEffect at z <= -0.25 and gone. Unlike Sign.bsf no drift towards the street, and its flags
 * stay */
static void animsign_loose(StreetObjs *s, StreetObj *o, const char *fx, int on_it)
{
    if (o->flags & 1)
        return;
    o->flags |= 1;
    set_pulse(o, TIMESTEP);
    effect(s, o, gstr(o->tmpl, fx), on_it ? NULL : pos_of(o), on_it, NULL, NULL, 0, 0);
}
static void animsign_pulse(StreetObjs *s, StreetObj *o)
{
    o->world[14] -= 10 * TIMESTEP;
    float rot[3] = { PI * 0.1f * TIMESTEP, PI * 0.15f * TIMESTEP, 0 };
    rotate_local(o, rot);
    if (o->world[14] <= -0.25f) {
        effect(s, o, gstr(o->tmpl, "GroundEffect"), pos_of(o), 0, NULL, NULL, 0, 0);
        request_delete(s, o);
    }
}
static void animsign_damage(StreetObjs *s, StreetObj *o, int mi, const StreetMonster *who)
{
    if (!who || mi < 0 || (o->flags & 1))
        return;
    animsign_loose(s, o, "HitEffect", 1);
    add_stats(s, o, mi);
}

/* ---------------------------------------------------------------- DamagingEffect.bsf */

/* the geysers (Fountain2, Fountain2_silent in Las Vegas; Fountain3, which a broken fountain or hydrant leaves) and
 * the puddles (PukePuddle, SoapPuddle, ChocolatePuddle) [R DamagingEffect.scr]. Construct: a pulse after the
 * template's DeleteSelfTime (0: none, so it stays), which deletes it; its Effect (getStringProp) on it, turned off
 * when it goes */
static void damaging_construct(StreetObjs *s, StreetObj *o)
{
    set_pulse(o, onum(o, "DeleteSelfTime", 0));
    effect(s, o, ostr(o, "Effect"), NULL, 1, NULL, NULL, 0, 1);
}
/* TakeDamage [R], from a monster within HitDistance (1.5) of it along x and along y, any type (a touch every frame):
 * its PlayReaction while the monster's move is a Ground one (React_Geyser; else SetReaction, which none of the books
 * sets: not here); its ReactionSound on
 * the monster; with SetMonsterVelocity, the monster thrown at (10 away from it along x, -5 x frand, the
 * MonsterZVelocity: 20, the books' MonsterZVel not being that key) */
static void damaging_damage(StreetObjs *s, StreetObj *o, int mi, const StreetMonster *who)
{
    if (!who || mi < 0)
        return;
    float hit = onum(o, "HitDistance", 1.5f);
    if (fabsf(o->world[12] - who->pos[0]) > hit || fabsf(o->world[13] - who->pos[1]) > hit)
        return;
    const char *react = ostr(o, "PlayReaction");
    if (react && who->move && strncmp(who->move, "Ground", 6) == 0)
        monster_call(s, o, SE_MONSTER_REACTION, mi)->name = react;
    const char *snd = ostr(o, "ReactionSound");
    if (snd)
        {                                            /* m.add3DSfx(reactSound, 0): the monster's own */
            StreetEvent *e = emit(s, SE_SOUND, o, snd, who->pos);
            e->level = 0;
            e->monster = mi;
        }
    if ((int)onum(o, "SetMonsterVelocity", 0)) {
        StreetEvent *e = monster_call(s, o, SE_MONSTER_VEL, mi);
        e->v[0] = who->pos[0] < o->world[12] ? -10 : 10;
        e->v[1] = -5 * frand(s);
        e->v[2] = onum(o, "MonsterZVelocity", 20);
    }
}

/* ---------------------------------------------------------------- Fountain.bsf */

static int add_kind(StreetObjs *s, int kind, Group tmpl, Group inst, const float world[16], int building, int attack);

/* the fountains' sculptures (Fountain1) and the fire hydrants (FireHydrant1) [R Fountain.scr]. Construct: state 0,
 * setFlags(0x8000): only an armed blow, once an attack. onInit: with AttachFountainFX its "fountain" effect at its
 * model's nodes 1 .. 4, turned off when it goes */
static void fountain_construct(StreetObj *o)
{
    o->oflags = SF_EFF_NODES | (o->oflags & SF_ATTACHED);
}
static void fountain_init(StreetObjs *s, StreetObj *o)
{
    for (int n = 1; n <= 4 && (int)onum(o, "AttachFountainFX", 0); n++)
        effect(s, o, "fountain", NULL, 1, NULL, NULL, n, 1);
}
/* broken, once (TakeDamage from anything, or TakeImpact): its Geyser (an OTRef: Fountain3, a DamagingEffect.bsf
 * object of 15 s) made where it stood; it is put 3 up and off on a SphereMover at the blow's way x 10 along x and y
 * and 25 up (a thrown object's: straight up), a pulse a second, and a monster's blow counts */
static void fountain_break(StreetObjs *s, StreetObj *o, int mi, const float dir[3])
{
    if (o->state != 0)
        return;
    o->state = 1;
    const char *g = ostr(o, "Geyser");
    Group gt = g && s->host.find ? s->host.find(s->host.ctx, g) : (Group){ 0 };
    if (gt.at) {
        float w[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, o->world[12], o->world[13], o->world[14], 1 };
        int k = add_kind(s, streetobj_kind_of(gt), gt, (Group){ 0 }, w, -1, 0);
        if (k >= 0)
            s->o[k].made = 1;                        /* no building's: made after the block's start */
    }
    o->world[14] = 3;
    float r = collide_mover_radius(&o->cshape);
    if (r < 0.05f)
        r = o->cshape.radius > 0.05f ? o->cshape.radius : 1;
    collide_mover_init(&o->cm, r, onum(o, "Gravity", 9.81f), onum(o, "Mass", 1), onum(o, "Restitution", 0.8f),
                       onum(o, "SlidingFriction", 0.4f), onum(o, "InertiaSolidity", 1));
    static const float none[3] = { 0, 0, 0 };
    collide_mover_spin(&o->cm, none);
    o->cm.vel[0] = dir[0] * 10;
    o->cm.vel[1] = dir[1] * 10;
    o->cm.vel[2] = 25;
    if (getenv("RAMPAGE_TRACE"))
        printf("  fountain %s at %.1f %.1f breaks off along %.2f %.2f %.2f\n", o->name ? o->name : "?", o->world[12],
               o->world[13], dir[0], dir[1], dir[2]);
    collide_matrix_from16(o->cmat, o->world);
    o->flying = 1;
    o->moved = 1;
    set_pulse(o, 1);
    if (mi >= 0)
        add_stats(s, o, mi);
}
/* onPulse [R]: at the fourth, Impact_concrete_Big where it is, and it goes */
static void fountain_pulse(StreetObjs *s, StreetObj *o)
{
    o->elapsed += 1;
    if (o->elapsed >= 4) {
        effect(s, o, "Impact_concrete_Big", pos_of(o), 0, NULL, NULL, 0, 0);
        request_delete(s, o);
    }
}

/* ---------------------------------------------------------------- the module */

StreetObjs *streetobj_new(const StreetHost *host)
{
    StreetObjs *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    if (host)
        s->host = *host;
    s->frame = 1;                                    /* (the objects' last frame starts at 0: none) */
    return s;
}
void streetobj_free(StreetObjs *s) { free(s); }

/* ---------------------------------------------------------------- WaterTower.bsf, Crane.bsf, KiteAnchor.bsf */

/* getNodePosition(n): its model's node n (1 or 2) in the world now; 0 none */
static int node_pos(const StreetObjs *s, const StreetObj *o, int n, float out[3])
{
    if (n < 1 || n > o->nnode)
        return 0;
    streetobj_point(s, idx(s, o), o->node[n - 1], out);
    return 1;
}
static void set_point(StreetObj *o, const float p[3])   /* setPoint */
{
    memcpy(o->world + 12, p, 3 * sizeof(float));
    o->moved = 1;
}
/* gWorld.createLineMover(): init(object, its place, to, speed, 0), setMover */
static void line_mover(StreetObj *o, const float to[3], float speed)
{
    memcpy(o->lm_to, to, sizeof o->lm_to);
    o->lm_speed = speed;
    o->lm_on = 1;
}
/* the LineMover's step, straight at its speed [G: no easing]; 1 at its end (onMoverCallback) */
static int line_step(StreetObj *o)
{
    float d[3], l = 0, p[3];
    for (int k = 0; k < 3; k++) {
        d[k] = o->lm_to[k] - o->world[12 + k];
        l += d[k] * d[k];
    }
    l = sqrtf(l);
    float step = o->lm_speed * TIMESTEP;
    if (l <= step) {
        set_point(o, o->lm_to);
        o->lm_on = 0;
        return 1;
    }
    for (int k = 0; k < 3; k++)
        p[k] = o->world[12 + k] + d[k] / l * step;
    set_point(o, p);
    return 0;
}

/* WaterTower.bsf [R]: a water tower's legs on a roof, its tank (OTRef|Attachment WaterTowerTop1, a RoofItem) made by
 * the caller at its node 1 (attachment 0). A monster's blow (it counts) or a thrown object: TankHitEffect1
 * (WaterTower_Comp) where it stands and the tank goes; SudsDelay (3 s) later its SudsEffect (Suds,
 * Chocolate_tower_hit) 7 towards the street (where no building holds that point, at it), SudsZOffset and
 * SudsYOffset off, its SlipObject (SoapPuddle, ChocolatePuddle) made below it at y -2, and it goes. Its building
 * coming down: DestFallEffect and TankHitEffect1 where it is, the suds stop, the tank goes, and it sinks 6 a second
 * for 2 s and goes */
static void tower_construct(StreetObj *o)
{
    o->oflags |= SF_PUSH_OUT | SF_EFF_NODES;
    o->delay = onum(o, "SudsDelay", 3);
    o->hit_fx = -1;
}
static void tower_hit(StreetObjs *s, StreetObj *o, int mi)   /* TakeDamage from a monster, TakeImpact */
{
    if (o->state != 0)
        return;
    o->state = 1;
    effect(s, o, gstr(o->tmpl, "TankHitEffect1"), NULL, 0, NULL, NULL, 0, 0);
    StreetObj *t = attachment(s, o, 0);
    if (t)
        request_delete(s, t);
    set_pulse(o, o->delay);
    add_stats(s, o, mi);
}
static void tower_pulse(StreetObjs *s, StreetObj *o)
{
    if (o->state == 1) {
        const char *fx = gstr(o->tmpl, "SudsEffect");
        if (fx) {
            float p[3] = { o->world[12], o->world[13] - 7, o->world[14] + onum(o, "SudsZOffset", 0) };
            if (building_at(s, p) < 0)
                p[1] += 7;
            p[1] += onum(o, "SudsYOffset", 0);
            o->hit_fx = effect(s, o, fx, p, 0, NULL, NULL, 0, 0);
            const char *slip = ostr(o, "SlipObject");
            Group gt = slip && s->host.find ? s->host.find(s->host.ctx, slip) : (Group){ 0 };
            if (gt.at) {
                float w[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, p[0], -2, 0, 1 };
                int k = add_kind(s, streetobj_kind_of(gt), gt, (Group){ 0 }, w, -1, 0);
                if (k >= 0)
                    s->o[k].made = 1;
            }
        }
        request_delete(s, o);                        /* 4 | 8 | 16 */
    } else if (o->state == 2) {
        o->elapsed -= TIMESTEP;
        float p[3] = { o->world[12], o->world[13], o->world[14] - 6 * TIMESTEP };
        set_point(o, p);
        if (o->elapsed <= 0)
            request_delete(s, o);
    }
}
static void tower_building_gone(StreetObjs *s, StreetObj *o)
{
    o->state = 2;
    if (o->hit_fx >= 0)
        effect_stop(s, o, o->hit_fx);
    effect(s, o, gstr(o->tmpl, "DestFallEffect"), NULL, 0, NULL, NULL, 0, 0);
    effect(s, o, gstr(o->tmpl, "TankHitEffect1"), NULL, 0, NULL, NULL, 0, 0);
    StreetObj *t = attachment(s, o, 0);
    if (t)
        request_delete(s, t);
    o->elapsed = 2;
    set_pulse(o, TIMESTEP);
}

/* Crane.bsf [R]: a construction crane on a roof turning its jib (RotationVel a second within MinRotation ..
 * MaxRotation x pi; a pulse of 0.5 s starts a turn 1 time in 20, the other way) and running its cargo (OTRef|
 * Attatchment Crane1_Cargo, made by the caller at node 1: attachment 0) between its nodes 1 and 2 (SlideVel a second;
 * 1 time in 10). A monster's blow: HitSound, HitFX on node 1 and the blow's damage off its HitPoints; still standing
 * it rocks away from the nearest monster (a quarter-wave of 0.09 / 4 cos(pi t / 2) a frame about the axis across,
 * back to upright), else it counts and topples away from it (pi/4 a second, CrackingSound at once), and past pi/2
 * falls, along fallDir (the way from the monster; at first a random one) 0.15 and down 0.4 a frame, still turning,
 * its cargo gone after 2 s; node 1 below 1.5: CrashFX at node 1 9 nearer the street and at four points round it,
 * and 1.3 s later it goes with its cargo. Its building coming down: DestFallEffect and it falls at once */
static void crane_construct(StreetObjs *s, StreetObj *o)
{
    o->axis[0] = 1;
    o->axis[1] = o->axis[2] = 0;
    o->die_delay = 1.3f;
    o->rot_dir = -1 + (float)(frand(s) < 0.5f) * 2;
    o->cargo_dir = 1;
    set_pulse(o, 0.5f);
    o->oflags |= SF_PUSH_OUT | SF_EFF_NODES;
    o->fall_dir[0] = frand(s) - 0.5f;
    o->fall_dir[1] = frand(s) - 0.5f;
    o->fall_dir[2] = 0;
    float l = sqrtf(o->fall_dir[0] * o->fall_dir[0] + o->fall_dir[1] * o->fall_dir[1]);
    if (l > 1e-6f) {
        o->fall_dir[0] /= l;
        o->fall_dir[1] /= l;
    }
    o->hp = onum(o, "HitPoints", 40);
    float v;
    o->rot_min = gnum(o->inst, "MinRotation", &v) ? v : gnum(o->tmpl, "MinRotation", &v) ? v : -1;
    o->rot_max = gnum(o->inst, "MaxRotation", &v) ? v : gnum(o->tmpl, "MaxRotation", &v) ? v : 1;
    o->rot_vel = gnum(o->tmpl, "RotationVel", &v) ? v : 1;
    o->slide_vel = gnum(o->tmpl, "SlideVel", &v) ? v : 1;
    o->cargo_rot = PI * (gnum(o->tmpl, "CargoZRotOffset", &v) ? v : 0);
    o->rot_min *= PI;
    o->rot_max *= PI;
}
static void crane_cargo(StreetObjs *s, StreetObj *o)   /* updateCargoLocation */
{
    StreetObj *c = attachment(s, o, 0);
    float a[3], b[3];
    if (!c || !node_pos(s, o, 1, a) || !node_pos(s, o, 2, b))
        return;
    float p[3];
    for (int k = 0; k < 3; k++)
        p[k] = a[k] + (b[k] - a[k]) * o->cargo_at;
    set_point(c, p);
}
/* the axis across the way to the nearest monster (its way x z, in the crane's own frame); 0 none */
static int crane_axis(StreetObjs *s, StreetObj *o)
{
    int m = nearest_monster(s, pos_of(o));
    if (m < 0)
        return 0;
    float d[3] = { s->m[m].pos[0] - o->world[12], s->m[m].pos[1] - o->world[13], s->m[m].pos[2] - o->world[14] };
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l > 1e-6f)
        for (int k = 0; k < 3; k++)
            d[k] /= l;
    float w[3] = { d[1], -d[0], 0 };                 /* d x (0, 0, 1) */
    world_to_object(o, w, o->axis);
    return 1;
}
static void crane_rebound(StreetObjs *s, StreetObj *o)   /* doRebound */
{
    if (o->state >= 4 || (o->flags & 2))
        return;
    if (o->total_angle != 0) {
        rotate_aa(o, -o->total_angle, o->axis);
        o->total_angle = 0;
    }
    crane_axis(s, o);
    o->rb_time = 0;
    o->flags |= 2;
}
static void crane_fall_start(StreetObj *o)
{
    float l = sqrtf(o->fall_dir[0] * o->fall_dir[0] + o->fall_dir[1] * o->fall_dir[1] + o->fall_dir[2] * o->fall_dir[2]);
    for (int k = 0; k < 3; k++)
        o->vel[k] = l > 1e-6f ? o->fall_dir[k] / l * 0.15f : 0;
    o->vel[2] = -(6.0f * (1.0f / 15.0f));
    o->state = 4;
}
static void crane_process(StreetObjs *s, StreetObj *o)   /* Process, every frame */
{
    if (o->state < 4 && (o->flags & 2)) {
        if (o->total_angle >= 0) {
            o->rb_time += TIMESTEP;
            float da = 0.09f / (2 * 2) * cosf(PI * o->rb_time / 2);
            o->total_angle += da;
            rotate_aa(o, da, o->axis);
        } else {
            rotate_aa(o, -o->total_angle, o->axis);
            o->total_angle = 0;
            o->rb_time = 0;
            o->flags &= ~2;
        }
        crane_cargo(s, o);
    }
    if (o->state == 5) {                             /* toppling */
        if (!(o->flags & 1)) {
            crane_axis(s, o);
            o->flags |= 1;
        }
        rotate_aa(o, PI / 4 * TIMESTEP, o->axis);
        o->total_angle += PI / 4 * TIMESTEP;
        crane_cargo(s, o);
        if (o->total_angle < PI / 8) {
            o->crack_delay -= TIMESTEP;
            if (o->crack_delay < 0) {
                sound(s, o, ostr(o, "CrackingSound"), 0);
                o->crack_delay = 99999;
            }
        } else if (o->total_angle > PI / 2)
            crane_fall_start(o);
        return;
    }
    if (o->state == 4) {                             /* falling */
        o->elapsed -= 1.0f / 15.0f;
        float p[3] = { o->world[12] + o->vel[0], o->world[13] + o->vel[1], o->world[14] + o->vel[2] };
        set_point(o, p);
        rotate_aa(o, 0.25f * PI / 4 * TIMESTEP, o->axis);
        o->total_angle += 0.25f * PI / 4 * TIMESTEP;
        crane_cargo(s, o);
        StreetObj *c = attachment(s, o, 0);
        if (c && o->elapsed <= 0)
            request_delete(s, c);
        float n[3];
        if (node_pos(s, o, 1, n) && n[2] < 1.5f)
            o->state = 6;
        return;
    }
    if (o->state == 6) {                             /* down */
        if (!(o->flags & 8)) {
            const char *fx = ostr(o, "CrashFX");
            float p[3];
            if (fx && node_pos(s, o, 1, p)) {
                p[1] -= 9;
                float q[4][3] = { { p[0] - 3, p[1] + frand(s) / 4, p[2] + 2.5f },
                                  { p[0] + 3, p[1] + frand(s) / 4, p[2] + 2.5f },
                                  { p[0] + 3, p[1] + frand(s) / 4, p[2] - 2.5f },
                                  { p[0] - 3, p[1] + frand(s) / 4, p[2] - 2.5f } };
                effect(s, o, fx, p, 0, NULL, NULL, 0, 0);
                for (int k = 0; k < 4; k++)
                    effect(s, o, fx, q[k], 0, NULL, NULL, 0, 0);
            }
            o->die_delay = 1.3f;
            o->flags |= 8;
        } else if ((o->die_delay -= TIMESTEP) < 0) {
            request_delete(s, o);
            StreetObj *c = attachment(s, o, 0);
            if (c)
                request_delete(s, c);
        }
        return;
    }
    int cargo = 0;
    if (o->state & 2) {                              /* the cargo running */
        o->cargo_at += o->slide_vel * o->cargo_dir * TIMESTEP;
        if (o->cargo_at >= 1) {
            o->cargo_at = 1;
            o->state &= ~2;
        } else if (o->cargo_at <= 0) {
            o->cargo_at = 0;
            o->state &= ~2;
        }
        cargo = 1;
    }
    if (o->state & 1) {                              /* the jib turning */
        float fc[3] = { o->facing[0], o->facing[1], o->facing[2] + o->rot_dir * TIMESTEP * o->rot_vel };
        if (fc[2] >= o->rot_max) {
            fc[2] = o->rot_max;
            o->state &= ~1;
        } else if (fc[2] <= o->rot_min) {
            fc[2] = o->rot_min;
            o->state &= ~1;
        }
        set_facing(o, fc);
        StreetObj *c = attachment(s, o, 0);
        if (c) {
            float r[3] = { fc[0], fc[1], fc[2] + o->cargo_rot };
            set_facing(c, r);
            cargo = 1;
        }
    }
    if (cargo)
        crane_cargo(s, o);
}
static void crane_pulse(StreetObjs *s, StreetObj *o)
{
    if (o->state & 4)
        return;
    if (!(o->state & 1) && frand(s) < 0.05f) {
        o->state |= 1;
        o->rot_dir = -o->rot_dir;
        return;
    }
    if (!(o->state & 2) && frand(s) < 0.1f) {
        o->state |= 2;
        o->cargo_dir = -o->cargo_dir;
    }
}
static void crane_damage(StreetObjs *s, StreetObj *o, int mi, const StreetMonster *who, float amount)
{
    if (!who || mi < 0)
        return;
    for (int k = 0; k < 3; k++)
        o->fall_dir[k] = o->world[12 + k] - who->pos[k];
    sound(s, o, ostr(o, "HitSound"), 1);
    effect(s, o, ostr(o, "HitFX"), NULL, 1, NULL, NULL, 1, 0);
    o->hp -= amount;
    if (o->hp < 0) {
        add_stats(s, o, mi);
        o->elapsed = 2;
        set_pulse(o, 1.0f / 15.0f);
        o->state = 5;
    } else
        crane_rebound(s, o);
}
static void crane_building_gone(StreetObjs *s, StreetObj *o)
{
    effect(s, o, gstr(o->tmpl, "DestFallEffect"), NULL, 0, NULL, NULL, 0, 0);
    o->elapsed = 2;
    set_pulse(o, 1.0f / 15.0f);
    crane_fall_start(o);
}

/* KiteAnchor.bsf [R]: an InvisShape on the street of SF04 flying its kite: onInit its KiteFX (kite, kite_yellow)
 * on it at (xoffset, yoffset, zoffset); a monster's blow or its building coming down lets it go: up to z 80 at 4 a
 * second (a LineMover), gone at the top (the kite, not turned off with it, stays up there). Its shape is the
 * InvisShape model's sphere (no CollisionBox, CollisionRadius or AutoCollisionBox: 0x28FD70 -> 0x184CB0), radius
 * 0.034 at its place, so a blow lets it go only when one of the monster's spheres takes in that point [R]; on SF04's
 * roof (z 21 and 23, the top at 22.5) a standing monster's fists pass some 4 above them */
static void kite_init(StreetObjs *s, StreetObj *o)
{
    float off[3] = { onum(o, "xoffset", 0), onum(o, "yoffset", 0), onum(o, "zoffset", 0) };
    effect(s, o, ostr(o, "KiteFX"), NULL, 1, off, NULL, 0, 0);
}
static void kite_fly(StreetObj *o)
{
    if (o->flags & 1)
        return;
    float to[3] = { o->world[12], o->world[13], 80 };
    line_mover(o, to, 4);
    o->flags |= 1;
}

/* ForkLift.bsf [R] (ForkLift1 in NY02, ForkLiftHK1 in HK01): a forklift driving its round of the yard on LineMovers,
 * turning between them at pi a second (startRotation: from curRotZ, which the first turn has not set: it snaps):
 * from (Loc1X, Loc1Y) (23.7, 4) facing back to y -3 at 6, along x to Loc3X (-33) at 8, to y Loc4Y (4) at 6, with
 * DoMove4_5 (1) out to x -24.5 and back at 5 (without, it stops there), to y -3 at 6, along x to Loc1X at 8, to y
 * Loc1Y and back to y -3 at 6, and round again. Pickupable; held (onPickup) it stops for good, DamageShape; a monster's
 * blows take its HitPoints (1): HitEffect1 on it, and at the last (not held) DamageShape, HitEffect2 on it (with none:
 * the score, 2) and it flies off 25 a second away from the monster; thrown, landing or hit by a thrown object, or
 * after that flight: 1.5 s later it goes (ExplodeFX on it in its last quarter second). [G: not ported: the custom
 * collision the mover's callback 4 does on the x legs, a monster beside it pushed 0.3 towards the street] */
static void fork_construct(StreetObj *o)
{
    o->oflags |= SF_PUSH_OUT | SF_EFF_NODES | SF_PICKUP;
    o->hp = (float)(int)onum(o, "HitPoints", 2);
    set_pulse(o, 0.1f);
    o->die_delay = 1.5f;
}
/* LineMover.init(object, from, to, speed, 0): it starts at `from` */
static void fork_move(StreetObj *o, float fx, float fy, float tx, float ty, float speed)
{
    float from[3] = { fx, fy, 0 }, to[3] = { tx, ty, 0 };
    set_point(o, from);
    line_mover(o, to, speed);
}
static void fork_turn(StreetObj *o, float to)       /* startRotation */
{
    o->rot_start = o->facing[2];
    o->rot_end = to;
    set_pulse(o, TIMESTEP);
    o->saved = o->state;
    o->state = 10;
}
/* onMoverCallback(2): the leg or the turn done, the next; flags 1: turned, now the leg */
static void fork_next(StreetObj *o)
{
    float x = o->world[12], y = o->world[13];
    float loc1x = onum(o, "Loc1X", 23.7f), loc1y = onum(o, "Loc1Y", 4), loc3x = onum(o, "Loc3X", -33),
          loc4y = onum(o, "Loc4Y", 4);
    int turned = o->flags & 1;
    if (o->state == 2) {
        if (!turned) { fork_turn(o, PI / 2); o->flags |= 1; return; }
        o->oflags &= ~(uint32_t)SF_PUSH_OUT;
        fork_move(o, x, y, loc3x, y, 8);
        o->state = 3;
    } else if (o->state == 3) {
        o->oflags |= SF_PUSH_OUT;
        if (!turned) { fork_turn(o, 0); o->flags |= 1; return; }
        fork_move(o, x, y, x, loc4y, 6);
        o->state = 4;
    } else if (o->state == 4) {
        if (!onum(o, "DoMove4_5", 1)) {
            o->state = 6;                            /* (no leg: it stops here) */
            return;
        }
        if (!turned) { fork_turn(o, -PI / 2); o->flags |= 1; return; }
        fork_move(o, -33, 4, -24.5f, 4, 5);
        o->state = 5;
    } else if (o->state == 5) {
        if (!turned) { fork_turn(o, -PI / 2); o->flags |= 1; return; }
        fork_move(o, -24.5f, 4, -33, 4, 5);
        o->state = 6;
    } else if (o->state == 6) {
        if (!turned) { fork_turn(o, 0); o->flags |= 1; return; }
        fork_move(o, x, y, x, -3, 6);
        o->state = 7;
    } else if (o->state == 7) {
        if (!turned) { fork_turn(o, -PI / 2); o->flags |= 1; return; }
        o->oflags &= ~(uint32_t)SF_PUSH_OUT;
        fork_move(o, x, y, loc1x, y, 8);
        o->state = 8;
    } else if (o->state == 8) {
        o->oflags |= SF_PUSH_OUT;
        if (!turned) { fork_turn(o, 0); o->flags |= 1; return; }
        fork_move(o, x, -3, x, loc1y, 6);
        o->state = 9;
    } else if (o->state == 9) {
        if (!turned) { fork_turn(o, 0); o->flags |= 1; return; }
        fork_move(o, x, y, x, -3, 6);
        o->state = 2;
    } else
        return;
    o->flags &= ~1;
}
static void fork_pulse(StreetObjs *s, StreetObj *o)
{
    if (o->state == 0) {
        float p[3] = { onum(o, "Loc1X", 23.7f), onum(o, "Loc1Y", 4), 0 }, e[3] = { 0, 0, PI };
        set_point(o, p);
        set_facing(o, e);
        o->oflags |= SF_PUSH_OUT;
        fork_move(o, p[0], p[1], p[0], -3, 6);
        o->state = 2;
    } else if (o->state == 10) {                     /* turning */
        float rate = (o->rot_end < o->rot_start ? -1 : 1) * o->pulse_delay * PI;
        o->rot_cur += rate;
        float e[3] = { 0, 0, o->rot_cur };
        if ((o->rot_end >= o->rot_start && o->rot_cur >= o->rot_end) ||
            (o->rot_end <= o->rot_start && o->rot_cur <= o->rot_end)) {
            o->rot_cur = e[2] = o->rot_end;
            set_facing(o, e);
            set_pulse(o, 0);
            o->state = o->saved;
            fork_next(o);
        } else
            set_facing(o, e);
    } else if (o->state == 11) {                     /* going */
        o->die_delay -= o->pulse_delay;
        if (o->die_delay > 0 && o->die_delay < 0.25f)
            effect(s, o, ostr(o, "ExplodeFX"), NULL, 1, NULL, NULL, 0, 0);
        else if (o->die_delay < 0)
            request_delete(s, o);
    }
}
/* onThrow, OnHitGround, TakeImpact [R]: setPulseDelay(0.1), state 11; the LineMover is left as it is, so one hit on the
 * street by a thrown object drives on for its last 1.5 s (state 11's mover callbacks do nothing); thrown, its flight
 * has the mover's place (stop: the port's line off) */
static void fork_die(StreetObj *o, int stop)
{
    set_pulse(o, 0.1f);
    o->state = 11;
    if (stop)
        o->lm_on = 0;
}
static void fork_damage(StreetObjs *s, StreetObj *o, int mi, const StreetMonster *who)
{
    if (!who || mi < 0)
        return;
    o->hp -= 1;
    if (o->hp < 1 && o->state != 1) {
        set_shape(s, o, ostr(o, "DamageShape"));
        const char *fx = ostr(o, "HitEffect2");
        if (fx)
            effect(s, o, fx, NULL, 1, NULL, NULL, 0, 0);
        else
            add_stats(s, o, mi);
        float d[3] = { o->world[12] - who->pos[0], o->world[13] - who->pos[1], o->world[14] - who->pos[2] };
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; k++)
            d[k] = l > 1e-6f ? d[k] / l * 25 : 0;
        o->lm_on = 0;
        streetobj_fly(s, idx(s, o), -1, 0, d);       /* a SphereMover */
        set_pulse(o, 0.1f);
        o->state = 11;
    } else
        effect(s, o, ostr(o, "HitEffect1"), NULL, 1, NULL, NULL, 0, 0);
}

int streetobj_kind_of(Group tmpl)
{
    const char *sc = gstr(tmpl, "ScriptName");
    float up;
    if (gnum(tmpl, "IsMonsterUpgrade", &up) && up != 0)
        return -1;
    return same_text(sc, "StreetObject.bsf") ? SK_STREET : same_text(sc, "RoofItem.bsf") ? SK_ROOF :
           same_text(sc, "MerryGoRound.bsf") ? SK_MERRY : same_text(sc, "Sign.bsf") ? SK_SIGN :
           same_text(sc, "Billboard.bsf") ? SK_BILLBOARD : same_text(sc, "Billboard_George.bsf") ? SK_GEORGE :
           same_text(sc, "AnimatedSign.bsf") ? SK_ANIMSIGN : same_text(sc, "Fountain.bsf") ? SK_FOUNTAIN :
           same_text(sc, "DamagingEffect.bsf") ? SK_DAMAGING : same_text(sc, "WaterTower.bsf") ? SK_TOWER :
           same_text(sc, "Crane.bsf") ? SK_CRANE : same_text(sc, "KiteAnchor.bsf") ? SK_KITE :
           same_text(sc, "ForkLift.bsf") ? SK_FORK : -1;
}

/* a book's collision keys (WSimpleObject 0x28FD70): CollisionBox { min x y z, max x y z } */
static const float *book_box(Group g, float b[6])
{
    return gpoint(g, "CollisionBox|CollisionBox", b, 6) || gpoint(g, "CollisionBox", b, 6) ? b : NULL;
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

static int add_kind(StreetObjs *s, int kind, Group tmpl, Group inst, const float world[16], int building, int attack)
{
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
    else if (kind == SK_ROOF)
        roof_construct(s, o);
    else if (kind == SK_MERRY)
        merry_construct(o);
    else if (kind == SK_SIGN)
        sign_construct(s, o);
    else if (kind == SK_BILLBOARD)
        billboard_construct(o, ostr(o, "Type"));
    else if (kind == SK_GEORGE)
        george_construct(o);
    else if (kind == SK_2PART)
        o->rot_dir = 1;                              /* its Construct's draw: billboard_init, which makes it */
    else if (kind == SK_ANIMSIGN)
        (void)frand(s);                              /* fallRot.x's sign, unused */
    else if (kind == SK_FOUNTAIN)
        fountain_construct(o);
    else if (kind == SK_DAMAGING)
        damaging_construct(s, o);
    else if (kind == SK_TOWER)
        tower_construct(o);
    else if (kind == SK_CRANE)
        crane_construct(s, o);
    else if (kind == SK_KITE)
        o->oflags |= SF_EFF_NODES;
    else if (kind == SK_FORK)
        fork_construct(o);
    return i;
}

int streetobj_add(StreetObjs *s, Group tmpl, Group inst, const float world[16], int building, int attack)
{
    return add_kind(s, tmpl.at ? streetobj_kind_of(tmpl) : -1, tmpl, inst, world, building, attack);
}

void streetobj_nodes(StreetObjs *s, int i, const float node[][3], int n)
{
    if (i < 0 || i >= s->n)
        return;
    s->o[i].nnode = n < 2 ? n : 2;
    for (int k = 0; k < s->o[i].nnode; k++)
        memcpy(s->o[i].node[k], node[k], sizeof s->o[i].node[k]);
}

int streetobj_add_attachment(StreetObjs *s, int parent, Group tmpl, const float world[16])
{
    if (parent < 0 || parent >= s->n || s->o[parent].nattach >= 5)
        return -1;
    int kind = same_text(gstr(tmpl, "ScriptName"), "Billboard_2Part.bsf") ? SK_2PART :
               same_text(gstr(tmpl, "ScriptName"), "RoofItem.bsf") ? SK_ROOF : SK_ATTACH;   /* a water tower's tank */
    int i = add_kind(s, kind, tmpl, (Group){ 0 }, world, -1, 0);
    if (i >= 0) {
        s->o[i].building = -2;                       /* a script's object: no building's (0x309E20 is the block's) */
        s->o[parent].attach[s->o[parent].nattach++] = i;
    }
    return i;
}

void streetobj_start(StreetObjs *s, const StreetBuilding *b, int nb)
{
    s->nb = b ? (nb < MAX_BUILDINGS ? nb : MAX_BUILDINGS) : 0;
    if (s->nb)
        memcpy(s->b, b, sizeof *s->b * (size_t)s->nb);
    for (int i = 0; i < s->n; i++) {
        StreetObj *o = &s->o[i];
        if (o->building == -2)                      /* a billboard's attachment */
            o->building = -1;
        else if (o->building < 0 && (o->building = building_at(s, pos_of(o))) >= 0)
            o->oflags |= SF_ATTACHED;
        if (getenv("RAMPAGE_STREET"))                /* the list, for finding one */
            printf("street object %d %s (%s) at %.2f %.2f %.2f, box %.1f %.1f %.1f .. %.1f %.1f %.1f, flags %#x, "
                   "building %d\n", i, o->name ? o->name : "?",
                   o->kind == SK_ROOF ? "RoofItem" : o->kind == SK_MERRY ? "MerryGoRound" :
                   o->kind == SK_SIGN ? "Sign" : o->kind == SK_BILLBOARD ? "Billboard" :
                   o->kind == SK_GEORGE ? "Billboard_George" : o->kind == SK_2PART ? "Billboard_2Part" :
                   o->kind == SK_ATTACH ? "attachment" : o->kind == SK_ANIMSIGN ? "AnimatedSign" :
                   o->kind == SK_FOUNTAIN ? "Fountain" : o->kind == SK_DAMAGING ? "DamagingEffect" :
                   o->kind == SK_TOWER ? "WaterTower" : o->kind == SK_CRANE ? "Crane" :
                   o->kind == SK_KITE ? "KiteAnchor" : o->kind == SK_FORK ? "ForkLift" : "StreetObject",
                   o->world[12], o->world[13], o->world[14], o->box_min[0], o->box_min[1], o->box_min[2],
                   o->box_max[0], o->box_max[1], o->box_max[2], (unsigned)o->oflags, o->building);
    }
    for (int i = 0; i < s->n; i++)                   /* onInit, the block begun */
        if (s->o[i].kind == SK_BILLBOARD)
            billboard_init(s, &s->o[i]);
        else if (s->o[i].kind == SK_GEORGE)
            george_init(s, &s->o[i]);
        else if (s->o[i].kind == SK_FOUNTAIN)
            fountain_init(s, &s->o[i]);
        else if (s->o[i].kind == SK_KITE)
            kite_init(s, &s->o[i]);
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
    else if ((c.nmet || c.ground) && o->kind == SK_FORK && o->state != 11)
        fork_die(o, 1);                              /* ForkLift's TakeImpact, OnHitGround */
}

static void step(StreetObjs *s)
{
    for (int i = 0; i < s->n; i++) {
        StreetObj *o = &s->o[i];
        if (!o->alive)
            continue;
        if (o->kind == SK_MERRY)                     /* its RotationMover, before the pulse (as rail.c's movers) */
            merry_turn(o);
        if (o->pulse_delay >= 0.01f) {               /* onPulse [R 0x309BA0]: the clock runs only with a delay of */
            o->pulse += TIMESTEP;                    /* 0.01 or more; at the delay it is taken off, once a frame */
            if (o->pulse >= o->pulse_delay) {
                o->pulse -= o->pulse_delay;
                if (o->kind == SK_STREET)
                    street_pulse(s, o);
                else if (o->kind == SK_ROOF)
                    roof_pulse(s, o);
                else if (o->kind == SK_MERRY)
                    merry_pulse(s, o);
                else if (o->kind == SK_SIGN)
                    sign_pulse(s, o);
                else if (o->kind == SK_BILLBOARD)
                    billboard_pulse(s, o);
                else if (o->kind == SK_2PART)
                    part_pulse(o);
                else if (o->kind == SK_ANIMSIGN)
                    animsign_pulse(s, o);
                else if (o->kind == SK_FOUNTAIN)
                    fountain_pulse(s, o);
                else if (o->kind == SK_DAMAGING)
                    request_delete(s, o);            /* DamagingEffect's onPulse: its DeleteSelfTime is up */
                else if (o->kind == SK_TOWER)
                    tower_pulse(s, o);
                else if (o->kind == SK_CRANE)
                    crane_pulse(s, o);
                else if (o->kind == SK_FORK)
                    fork_pulse(s, o);
            }
        }
        if (o->kind == SK_GEORGE)                    /* Process: every frame */
            george_process(s, o);
        else if (o->kind == SK_CRANE && !s->deleting[i])
            crane_process(s, o);
        if (o->lm_on && !s->deleting[i] && !(o->oflags & SF_HELD) && !o->flying && line_step(o)) {
            if (o->kind == SK_KITE)                  /* onMoverCallback */
                request_delete(s, o);
            else if (o->kind == SK_FORK)
                fork_next(o);
        }
        if (o->kind == SK_2PART && o->on_path && !s->deleting[i])   /* its SplineMover */
            part_move(s, o);
        /* 0x309AC0 [R]: attached (0x4000000) to a building whose collapsing storey is set: onBuildingDestruction,
         * once (the flag goes, and with it the building's sway on the object, 0x3098E0) */
        if ((o->oflags & SF_ATTACHED) && o->building >= 0 && o->building < s->nb && s->b[o->building].falling) {
            o->oflags &= ~(uint32_t)SF_ATTACHED;
            if (o->kind == SK_ROOF)
                roof_building_gone(s, o);           /* StreetObject.bsf has none */
            else if (o->kind == SK_SIGN)
                sign_building_gone(s, o);
            else if (o->kind == SK_BILLBOARD)
                billboard_building_gone(s, o);
            else if (o->kind == SK_GEORGE)
                george_building_gone(s, o);
            else if (o->kind == SK_ANIMSIGN)
                animsign_loose(s, o, "DetachEffect", 0);
            else if (o->kind == SK_TOWER)
                tower_building_gone(s, o);
            else if (o->kind == SK_CRANE)
                crane_building_gone(s, o);
            else if (o->kind == SK_KITE)
                kite_fly(o);
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
    s->acc += dt;
    if (s->acc > 0.25f)
        s->acc = 0.25f;
    while (s->acc >= TIMESTEP) {                     /* the scripts count in frames: run them at the game's step */
        s->acc -= TIMESTEP;
        /* one TakeDamage an object a frame, the game's frame: its world step [R 0x2E22B0 runs in the monster's update,
         * at the world's 30 a second]; the touches and blows the host passes between two steps are one frame's, so a
         * faster screen does not touch twice as often (Monster_slip from a geyser, a merry-go-round's count) */
        s->frame++;
        for (int i = 0; i < s->n; i++) {             /* (for the drawing between the steps) */
            memcpy(s->o[i].prev, s->o[i].world, sizeof s->o[i].prev);
            s->o[i].prev_ok = 1;
        }
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
/* the sphere against the object's shape; dir (when given) TakeDamage's direction: the push that moves the sphere out,
 * from the object towards the blow, made 1 long, (1, 0, 0) below 1e-6 (0x2E22B0: 0x280470's output, 0x2E25F8 ..
 * 0x2E267C) [R] */
static int sphere_meets(const StreetObj *o, const float c[3], float radius, float dir[3])
{
    CollideObj co;
    float n[3], pt[3], push[3];
    as_collide(o, &co);
    if (!collide_obj_sphere(&co, c, radius, n, pt, push))
        return 0;
    if (dir) {
        float l = sqrtf(push[0] * push[0] + push[1] * push[1] + push[2] * push[2]);
        for (int k = 0; k < 3; k++)
            dir[k] = l < 1e-6f ? (k == 0) : push[k] / l;
    }
    return 1;
}
static void take_damage(StreetObjs *s, StreetObj *o, int mi, const StreetMonster *who, int type)
{
    o->frame = s->frame;
    if (o->oflags & SF_HELD)                         /* [R] 0x309A80: not while held */
        return;
    if (o->kind == SK_STREET)
        street_damage(s, o, mi, who, type);
    else if (o->kind == SK_ROOF)
        roof_damage(s, o, mi, who);
    else if (o->kind == SK_MERRY)
        merry_damage(s, o, mi, who);
    else if (o->kind == SK_SIGN)
        sign_damage(s, o, mi, who);
    else if (o->kind == SK_BILLBOARD)
        billboard_damage(s, o, mi, who);
    else if (o->kind == SK_GEORGE)
        george_damage(s, o, mi, who);
    else if (o->kind == SK_ANIMSIGN)
        animsign_damage(s, o, mi, who);              /* a billboard's front or attachment: no TakeDamage */
    else if (o->kind == SK_DAMAGING)
        damaging_damage(s, o, mi, who);
    else if (o->kind == SK_TOWER && who && mi >= 0)  /* from a monster */
        tower_hit(s, o, mi);
    else if (o->kind == SK_CRANE)
        crane_damage(s, o, mi, who, s->amount);
    else if (o->kind == SK_KITE && who && mi >= 0)
        kite_fly(o);
    else if (o->kind == SK_FORK)
        fork_damage(s, o, mi, who);
    else if (o->kind == SK_FOUNTAIN)                 /* Fountain.bsf: TakeDamage's direction x 10 across, 25 up */
        fountain_break(s, o, who ? mi : -1, s->dir);
    if (s->deleting[idx(s, o)])
        delete_now(s, o);
}

int streetobj_hit(StreetObjs *s, int monster, const StreetMonster *who, const float centre[3], float radius,
                  float amount, unsigned record)
{
    s->amount = amount;                              /* (Crane.bsf's TakeDamage reads it) */
    int n = 0;
    for (int i = 0; i < s->n; i++) {
        StreetObj *o = &s->o[i];
        if (!o->alive || o->frame == s->frame || o->kind == SK_ATTACH || o->kind == SK_2PART ||
            !sphere_meets(o, centre, radius, s->dir))
            continue;                                /* (an object without a TakeDamage is not hit) */
        if ((o->oflags & SF_EFF_NODES) && !collide_hit_once(&o->hits, monster, record))
            continue;                                /* [R] 0x2E33A0: on the record's hit list */
        if (getenv("RAMPAGE_TRACE"))
            printf("  street object %d (%s) hit\n", i, o->name ? o->name : "?");
        take_damage(s, o, monster, who, 0);
        n++;
    }
    return n;
}

void streetobj_shot(StreetObjs *s, int i, const float dir[3])
{
    if (i < 0 || i >= s->n || !s->o[i].alive || s->o[i].kind != SK_FOUNTAIN || s->o[i].frame == s->frame)
        return;
    memcpy(s->dir, dir, sizeof s->dir);
    take_damage(s, &s->o[i], -1, NULL, 0);
}

int streetobj_touch(StreetObjs *s, int monster, const StreetMonster *who, const float centre[3], float radius)
{
    int n = 0;
    for (int i = 0; i < s->n; i++) {
        StreetObj *o = &s->o[i];
        if (!o->alive || (o->oflags & SF_EFF_NODES) || o->frame == s->frame || o->kind == SK_ATTACH ||
            o->kind == SK_2PART || !sphere_meets(o, centre, radius, s->dir))
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
    if (o->alive && o->kind == SK_SIGN) {            /* Sign.bsf's: loose, its HitEffect on it; no score */
        if (!(o->flags & 1)) {
            sign_loose(o);
            effect(s, o, gstr(o->tmpl, "HitEffect"), NULL, 1, NULL, NULL, 0, 0);
        }
        return 1;
    }
    if (o->alive && o->kind == SK_FOUNTAIN) {        /* Fountain.bsf's: broken, straight up, no score */
        static const float up[3] = { 0, 0, 0 };
        fountain_break(s, o, -1, up);
        return 1;
    }
    if (o->alive && o->kind == SK_FORK && !(o->oflags & SF_HELD)) {   /* ForkLift.bsf's: going */
        fork_die(o, 0);
        return 1;
    }
    if (o->alive && o->kind == SK_TOWER) {           /* WaterTower.bsf's: as a blow, no score */
        tower_hit(s, o, -1);
        return 1;
    }
    if (o->alive && o->kind == SK_ANIMSIGN) {        /* AnimatedSign.bsf's: the same, its HitEffect on it */
        animsign_loose(s, o, "HitEffect", 1);
        return 1;
    }
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
        if (!o->alive || o->kind != SK_STREET || (o->oflags & SF_HELD) || !sphere_meets(o, centre, radius, NULL))
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
    int best = -1, seen = 0;
    (void)pos;
    for (int i = s->n - 1; i >= 0 && best < 0; i--) {
        const StreetObj *o = &s->o[i];
        if (o->kind == SK_ROOF && same_text(o->name, name)) {
            seen = 1;
            if (o->alive)
                best = i;
        }
    }
    if (best < 0)                                    /* -2: it was here and is gone (isValid() 0: nothing) */
        return seen ? -2 : -1;
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

void streetobj_grab(StreetObjs *s, int i, float blend)
{
    if (i < 0 || i >= s->n || !s->o[i].alive)
        return;
    StreetObj *o = &s->o[i];
    float rot[9];                                    /* the HeldObjectMover's blend in (army.h), from where it is */
    for (int c = 0; c < 3; c++)
        memcpy(rot + 3 * c, o->world + 4 * c, 3 * sizeof(float));
    army_hold_blend_start(&o->hold, rot, o->world + 12, blend);
    o->oflags = (o->oflags | SF_HELD) & ~(uint32_t)(SF_PICKUP | SF_PUSH_OUT);   /* [R] 0x2DA310 */
    o->flying = 0;
    o->oflags &= ~(uint32_t)SF_ATTACHED;             /* its mover is the hand's now */
    if (o->kind == SK_STREET)
        streetobj_pickup(s, i);                      /* script event 8 */
    else if (o->kind == SK_FORK) {                   /* ForkLift's onPickup: it stops, DamageShape */
        o->state = 1;
        o->lm_on = 0;
        set_shape(s, o, ostr(o, "DamageShape"));
        set_pulse(o, 0.1f);
    }
}

/* [R] the HeldObjectMover (0x310ED0): the MajorAxis row (default 2) is the hand's z, the next its y, the third their
 * cross; at the hand's place plus HoldOffset (MirroredHoldOffset when mirrored) in the hand's frame; blended in from
 * where it was (army_hold_blend, 0x310B50: 0.1 s from doGrabObject) */
void streetobj_hold(StreetObjs *s, int i, const float hand[16], int mirrored, float dt)
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
    float pos[3];
    for (int k = 0; k < 3; k++)
        pos[k] = hand[12 + k] + hand[k] * off[0] + hand[4 + k] * off[1] + hand[8 + k] * off[2];
    army_hold_blend(&o->hold, ax, pos, dt);
    for (int c = 0; c < 3; c++) {
        for (int k = 0; k < 3; k++)
            o->world[c * 4 + k] = ax[c * 3 + k];
        o->world[c * 4 + 3] = 0;
    }
    for (int k = 0; k < 3; k++)
        o->world[12 + k] = pos[k];
    o->world[15] = 1;
    o->moved = 1;
    memcpy(o->prev, o->world, sizeof o->prev);       /* moved by the frame, not the step: drawn where it is */
}

void streetobj_fly(StreetObjs *s, int i, int monster, int eat, const float vel[3])
{
    if (i < 0 || i >= s->n || !s->o[i].alive)
        return;
    StreetObj *o = &s->o[i];
    o->oflags &= ~(uint32_t)SF_HELD;
    if (!eat)                                        /* [R 0x2DA088] a sphere at its origin: a parked car meets the
                                                      * buildings (collide_shape_flight) */
        collide_shape_flight(&o->cshape);
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
    else if (o->kind == SK_FORK && monster >= 0)    /* onThrow (not the flight its last blow starts) */
        fork_die(o, 1);
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
    memcpy(o->prev, o->world, sizeof o->prev);
}

/* ---------------------------------------------------------------- reading */

int streetobj_count(const StreetObjs *s) { return s->n; }
const StreetObj *streetobj_get(const StreetObjs *s, int i) { return i >= 0 && i < s->n ? &s->o[i] : NULL; }

void streetobj_draw_world(const StreetObjs *s, int i, float out[16])
{
    const StreetObj *o = &s->o[i];
    const float *a = o->prev, *b = o->world;
    float d[3] = { b[12] - a[12], b[13] - a[13], b[14] - a[14] };
    if (!o->prev_ok || (o->oflags & SF_HELD) || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 8 * 8)
        memcpy(out, b, 16 * sizeof *out);
    else
        collide_matrix_between(out, a, b, s->acc / TIMESTEP);
}

int streetobj_events(StreetObjs *s, StreetEvent *out, int max)
{
    int n = s->nev < max ? s->nev : max;
    memcpy(out, s->ev, sizeof *out * (size_t)n);
    memmove(s->ev, s->ev + n, sizeof *s->ev * (size_t)(s->nev - n));
    s->nev -= n;
    return n;
}
