/* boss.c - the bosses (boss.h; docs/BOSS_NOTES.md).
 * [R] marks what is read from the game's code or scripts, [G] what is a guess or a stand-in. The scripts are quoted
 * by their decompiled names (build/scripts/Boss_*.scr): each function here says which handler it is. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boss.h"
#include "wrand.h"

#define PI 3.1415927f
#define DELETE_ME (4 | 8 | 16)                       /* [R] what a script handler returns to have its object deleted */
#define MAX_PARTS 48
#define MAX_EVENTS 512
#define MAX_SPAWN 8
#define MAX_TRIG 16
#define MAX_WOBJ 16
#define MAX_SP 24                                    /* a SplineMover's points */
#define MAX_LP 40                                    /* a LineMover's points */
#define MAX_FX_DIE 8
#define ROOF(storeys) ((float)(storeys) * 3.0f + 3.0f * 0.5f + 6.0f)   /* the scripts' height of a building */

/* object flags (+0x4C), names from the table at 0x3F5F70 [R]; 8 and 0x8000000 have no name there */
enum { OF_PUSH_OUT = 0x4000, OF_EFF_NODES = 0x8000, OF_OBSTACLE = 0x40000, OF_ANIMATE = 0x8000000 };
/* [R] DamageType numbers (0x3F5E40): 0 Concussive, 1 Collision. 999 is the generator's own "my building fell" */
enum { DT_CONCUSSIVE = 0, DT_COLLISION = 1, DT_GONE = 999 };
enum { MV_NONE, MV_SPLINE, MV_LINE };

/* ---------------------------------------------------------------- the movers */

typedef struct {                                     /* SplineMover (vtable 0x44B800, update 0x33ACA0) [R] */
    struct { float p[3], s, k, m[3]; } pt[MAX_SP];   /* point, distance along the path, tangent scale, tangent */
    int n, ready, closed;
    float s, vel;                                    /* +0x48 where it is, +0x44 its velocity (distance / s) */
    unsigned flags;                                  /* +0x54: 1 loop, 2 face along the path, 4 hold still */
    float fwd[3];                                    /* the last tangent (flag 2) */
} Spline;

typedef struct {                                     /* LineMover (0x315E10, docs/PEOPLE_NOTES.md) [R] */
    float p[MAX_LP][3];
    int n, seg;
    float t, speed;
    unsigned flags;                                  /* 1 face the way it goes, 2 loop, 4 [G: rounds its corners] */
} Line;

typedef struct { int fx, trig; } Barrier;            /* Boss_NY_Blimp.bsf's Barrier: FollowEffect + VolumeTrigger */

typedef struct {                                     /* Boss_Tank.bsf's Def */
    int effect, damage_fx, shock_fx, monster;
    float start_hp, hp, retarget, vel, jump_vel, move_delay;
    int destroyed;
    float osc, osc_scale;
    int sw_state;                                    /* shockwaveFireState */
    float sw_recharge, sw_fire;
    int sw_retreat;
    float sw_retreat_t;
    const char *sw_name;
    int weapons_off, hit_retreat;
    float hit_retreat_t, bomb_launch, taunt, leave_delay;
    int leave;
    float accel, max_vel, sw_damage, leave_dur;
} TankDef;
typedef struct { int effect, delete_me; } TankBombDef;   /* Boss_Tank_Bomb.bsf */
typedef struct {                                     /* Boss_Helicopter.bsf */
    int hp, monster, knocked, fly_away, destroyed, flight_state, heading_right, vel_state, engine_l, engine_r;
    float pulse_rate, osc, state_time, firing_delay, vel, max_vel, last_fx;   /* pulse_rate 0: every frame */
} HeliDef;
typedef struct {                                     /* Boss_Jumper.bsf */
    int effect, damage_fx, building, flags, destroyed;
    struct { int obj; float pos[3], radius; } adorn[3];
    float state_time, air_damage, max_hp, hp, hit_osc, leave_delay, pre_jump, jump_delay;
} JumperDef;
typedef struct { int effect, delete_me, allow; float delay; } JumperBombDef;   /* Boss_Jumper_Bomb.bsf */
typedef struct {                                     /* Boss_Chicago_Saucer.bsf */
    int blades, damage_fx, shield_fx, charge_fx, flags, target, fire_mode, fly_away;
    float start_hp, hp, blade_rot, blade_scale, osc, osc_scale, vel, shield_delay, state_time;
    float pts[6][3];
    float leave_delay, ww_delay, ww_chance;
} SaucerDef;
typedef struct {                                     /* Boss_Chicago_Whirlwind.bsf */
    int effect, shots, attached, monster;
    float life, building_time, fire_time, pulse;     /* pulse 0: every frame */
} WindDef;
typedef struct {                                     /* Boss_NY_Blimp.bsf */
    struct { int b; Barrier barrier, top; float prev, fz; int state; } bld[5];   /* fz: its barrier's height */
    int hp, state, destroyed_buildings, destroyed_gens, gen_state[2], moved_top, can_take_damage, punched_right,
        dropped, just_fired, voice_played, engine_fx;
    float rot, pulse, hit_countdown, fall_rot, retarget, bomb_drop, bomb_reload, voice_time;   /* pulse 0: each frame */
} BlimpDef;
typedef struct { int building, effect, hit_fx, gen_id, hp; } GenDef;   /* Boss_NY_Generator.bsf */
typedef struct {                                     /* Boss_Blimp_Ambient.bsf, Boss_Saucer_Ambient.bsf */
    int trips, state, blades;
    float vel, blade_rot, hover_delay, hover_start, hover_loc;
} AmbientDef;

typedef struct Obj {
    BossPart p;
    int mover;                                       /* MV_* */
    Spline sp;
    Line ln;
    int oriented;                                    /* the spline faces it along its path this frame (1), or the
                                                      * LineMover's frame lrot does (2) */
    float lrot[9];                                   /* the LineMover's frame: its x, y and z axes */
    uint32_t oflags;
    float pulse_delay, pulse_time;                   /* setPulseDelay; 0: every frame */
    float lo[3], hi[3];                              /* its collision shape's box in its own frame */
    CollideShape cshape;                             /* its collision shape (WCollisionInfo, 0x28FD70) */
    ArmyWeapon w[4];
    int nw, need_rot[4], need_elev[4];
    float elev_min[4], elev_max[4];
    int weapons_on, target;                          /* setTarget: a monster, or -1 */
    int has_point;                                   /* setTarget(a DummyObject): aim at point */
    float point[3];
    int fx_die[MAX_FX_DIE], nfx_die;                 /* effects with setTurnOffWhenObjectDies(1) */
    int spawner, pending, anim_done;                 /* pending: onInit waits for the first update */
    ArmyOsc osc;                                     /* the AttackObject's wobble (army.h) [R 0x315330, 0x314EF0] */
    union {
        TankDef tank;
        TankBombDef tbomb;
        HeliDef heli;
        JumperDef jumper;
        JumperBombDef jbomb;
        SaucerDef saucer;
        WindDef wind;
        BlimpDef blimp;
        GenDef gen;
        AmbientDef amb;
    } s;
    CollideHitList hits;                             /* the attack records that hit it (0x2E33A0's lists) */
} Obj;

typedef struct {                                     /* an AIInstance record of gAIMgr (0x335320), as army.c */
    Group g;
    const char *name;
    float min_delay, respawn, offset, timer;
    int left, alive;
} Spawner;

typedef struct {                                     /* a VolumeTrigger (initByObject(building, group)) */
    int alive, pushout, building;
    float c[3], e[3], refire, last;
    const char *fire, *name;
} Trigger;
typedef struct { int trig, monster; float n[3]; } TriggerTouch;   /* collide_monster's, for the next update */

typedef struct { const char *name; float pos[3]; Group tmpl; int gone; } WorldObj;

struct Boss {
    BossHost host;
    Obj o[MAX_PARTS];
    int n;
    BossEvent ev[MAX_EVENTS];
    int nev, next_fx;
    Spawner sp[MAX_SPAWN];
    int nsp, active;
    Trigger trig[MAX_TRIG];
    int ntrig;
    TriggerTouch touch[MAX_TRIG * 4];
    int ntouch;
    WorldObj wobj[MAX_WOBJ];
    int nwobj;
    int play[8];                                     /* gWorld.getPlayValue / setPlayValue */
    int num_generators;                              /* Boss_NY_Generator.bsf's numGenerators */
    int main, defeated, city_destroyed;
    float now, view[2];
    /* this frame */
    const BossMonster *m;
    int nm;
    const BossBuilding *bl;
    int nb;
    float dt;
};

/* ---------------------------------------------------------------- small things */

/* gWorld.frand() (0x279F98): the world's Park-Miller, gWorld +0x828 (wrand.c) [R] */
static float frand(Boss *b)
{
    (void)b;
    return wrand_frand();
}
/* gWorld.rand() (0x279F58): (Park-Miller >> 8) & 0xFFFF on the same state [R] */
static int irand(Boss *b)
{
    (void)b;
    return wrand_rand();
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
static int contains(const char *s, const char *what) { return s && what && strstr(s, what) != NULL; }   /* fFind >= 0 */

static float len3(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static void norm3(float v[3])
{
    float l = len3(v);
    if (l < 1e-6f) {                                 /* [R] the game's normalize of a null vector gives (1, 0, 0) */
        v[0] = 1;
        v[1] = v[2] = 0;
    } else {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}
static void scale3(float v[3], float s) { v[0] *= s; v[1] *= s; v[2] *= s; }
static void set3(float v[3], float x, float y, float z) { v[0] = x; v[1] = y; v[2] = z; }
static void copy3(float d[3], const float s[3]) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
static void sub3(float d[3], const float a[3], const float c[3])
{
    d[0] = a[0] - c[0];
    d[1] = a[1] - c[1];
    d[2] = a[2] - c[2];
}
static float dist3(const float p[3], const float q[3])
{
    float d[3];
    sub3(d, p, q);
    return len3(d);
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---------------------------------------------------------------- properties */

/* getStringProp / getFloatProp: the instance book (obj +0x7C) first, then the template [R 0x2C51A0, as army.c]. The
 * books keep a record's type before a '|' ("Effect|HitEffect"); the scripts ask by the bare name. A key with dots
 * (".Path0.NumPoints", Boss_Helicopter) walks down through the groups [G: the reader is not traced] */
static const char *const TYPED[] = { "", "Effect|", "Sound|", "Shape|", "OTRef|", "Anim|", "Script|" };

static Group walk(Group g, const char **key)
{
    const char *k = *key, *dot;
    char part[64];
    if (*k == '.')
        k++;
    while (g.at && (dot = strchr(k, '.')) != NULL) {
        size_t n = (size_t)(dot - k);
        if (n >= sizeof part)
            n = sizeof part - 1;
        memcpy(part, k, n);
        part[n] = 0;
        g = group_sub(g, part);
        k = dot + 1;
    }
    *key = k;
    return g;
}
static int find_rec(Group g, const char *key, Record *out, Group *where)
{
    char k[96];
    if (!g.at || !key)
        return 0;
    g = walk(g, &key);
    if (!g.at)
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
static int gpoint(Group g, const char *key, float out[3])   /* a group of three numbers (Location { x y z }) */
{
    Record r, it[8];
    Group w;
    if (!find_rec(g, key, &r, &w) || (r.kind & 0xFF) != PK_GROUP)
        return 0;
    Group sub = record_group(w, r);
    if (group_items(sub, it, 8) < 3)
        return 0;
    for (int k = 0; k < 3; k++)
        out[k] = record_num(it[k]);
    return 1;
}
static const char *ustr(const Obj *o, const char *key)   /* getStringProp */
{
    const char *s = gstr(o->p.inst, key);
    return s ? s : gstr(o->p.tmpl, key);
}
static const char *tstr(const Obj *o, const char *key) { return gstr(o->p.tmpl, key); }   /* getTemplateStringProp */
static float unum(const Obj *o, const char *key, float def)   /* getFloatProp / getIntProp */
{
    float v;
    return gnum(o->p.inst, key, &v) || gnum(o->p.tmpl, key, &v) ? v : def;
}
static int upoint(const Obj *o, const char *key, float out[3])   /* getPoint3Prop: 0 = absent (the default stays) */
{
    return gpoint(o->p.inst, key, out) || gpoint(o->p.tmpl, key, out);
}
static Group find_tmpl(Boss *b, const char *name)
{
    return b->host.find && name ? b->host.find(b->host.ctx, "Objects1_Props", name) : (Group){ 0 };
}

/* ---------------------------------------------------------------- events */

static int idx(const Boss *b, const Obj *o) { return o ? (int)(o - b->o) : -1; }

static BossEvent *emit(Boss *b, int type, const Obj *o, const char *name, const float pos[3])
{
    static BossEvent spare;
    BossEvent *e = b->nev < MAX_EVENTS ? &b->ev[b->nev++] : &spare;
    memset(e, 0, sizeof *e);
    e->type = type;
    e->obj = idx(b, o);
    e->monster = -1;
    e->building = -1;
    e->id = -1;
    e->level = -1;
    e->name = name;
    if (pos)
        copy3(e->pos, pos);
    else if (o)
        copy3(e->pos, o->p.pos);
    return e;
}

/* FollowEffect.createAndStart(pos, name): left where it started */
static int fx_at(Boss *b, const char *name, const float pos[3])
{
    if (!name)
        return -1;
    BossEvent *e = emit(b, BE_EFFECT, NULL, name, pos);
    e->id = b->next_fx++;
    e->dir[1] = 1;
    return e->id;
}
/* createAndStartAttached / ...WithOffset / ...AttachedToNodeByName / ...AttachedToNode: it follows the part */
static int fx_on(Boss *b, Obj *o, const char *name, const float offset[3], const char *node, int node_index,
                 int die_with)
{
    if (!name)
        return -1;
    float p[3];
    copy3(p, o->p.pos);
    if (offset)
        for (int k = 0; k < 3; k++)
            p[k] += offset[k];
    BossEvent *e = emit(b, BE_EFFECT, o, name, p);
    e->id = b->next_fx++;
    e->node = node;
    e->level = node_index;
    e->dir[1] = 1;
    if (die_with && o->nfx_die < MAX_FX_DIE)          /* setTurnOffWhenObjectDies(1) */
        o->fx_die[o->nfx_die++] = e->id;
    return e->id;
}
static void fx_stop(Boss *b, int id)
{
    if (id >= 0)
        emit(b, BE_EFFECT_STOP, NULL, NULL, NULL)->id = id;
}
static void sfx(Boss *b, Obj *o, const char *name)   /* add3DSfx(name, 0) */
{
    if (name)
        emit(b, BE_SOUND, o, name, NULL)->level = 1;
}
static void camera_rmh(Boss *b, float h)             /* gWorld.fCallScriptF("Levels::changeTwoTiltCameraRMH", h, 0) */
{
    emit(b, BE_CAMERA_RMH, NULL, "Levels::changeTwoTiltCameraRMH", NULL)->amount = h;
}
static void challenge_hud(Boss *b, Obj *o, float frac)   /* gCommon.updateChallengeHud */
{
    o->p.hp_frac = frac;
    emit(b, BE_BOSS_HEALTH, o, NULL, NULL)->amount = frac;
}
static void add_stats(Boss *b, Obj *o, int monster, int type)   /* gCommon.addStats(obj, slot, type) */
{
    BossEvent *e = emit(b, BE_STATS, o, o->p.name, NULL);
    e->monster = monster;
    e->level = type;
    e->tmpl = o->p.tmpl;
}
static void completion(Boss *b, Obj *o, const char *callback)   /* gWorld.iCallScriptV(CompletionCallback) */
{
    b->defeated = 1;
    emit(b, BE_DEFEATED, o, callback, NULL);
}

/* ---- what the scripts do to a monster */
static int m_valid(const Boss *b, int i) { return i >= 0 && i < b->nm; }
static void m_pos(const Boss *b, int i, float out[3])   /* an invalid monster's place reads as 0 [G] */
{
    if (m_valid(b, i))
        copy3(out, b->m[i].pos);
    else
        set3(out, 0, 0, 0);
}
static const char *m_move(const Boss *b, int i) { return m_valid(b, i) && b->m[i].move ? b->m[i].move : ""; }
static int any_monster(Boss *b) { return b->nm > 0 ? (int)(frand(b) * (float)b->nm) : -1; }   /* getMonster(rand n) */
static void m_vel(Boss *b, int i, const float v[3])  /* monster.setVel(v, 0) */
{
    BossEvent *e = emit(b, BE_VEL, NULL, NULL, b->m[i].pos);
    e->monster = i;
    copy3(e->v, v);
}
static void m_reaction(Boss *b, int i, const char *name)   /* monster.playReaction(name) */
{
    emit(b, BE_REACTION, NULL, name, b->m[i].pos)->monster = i;
}
static void m_hp(Boss *b, int i, float amount)       /* monster.addHitPoints(amount) */
{
    BossEvent *e = emit(b, BE_HP, NULL, NULL, b->m[i].pos);
    e->monster = i;
    e->amount = amount;
}
static void m_facing(Boss *b, int i, float z)        /* monster.setFacingZ */
{
    BossEvent *e = emit(b, BE_FACING, NULL, NULL, b->m[i].pos);
    e->monster = i;
    e->amount = z;
}
static void m_effect(Boss *b, int i, const char *name)   /* FollowEffect.createAndStartAttached(monster, name) */
{
    BossEvent *e = emit(b, BE_EFFECT, NULL, name, b->m[i].pos);
    e->monster = i;
    e->id = b->next_fx++;
    e->dir[1] = 1;
}
static void m_centre(const BossMonster *m, float c[3])   /* [G] where a weapon aims: the box's middle (army.c) */
{
    if (m->box_max[2] > m->box_min[2])
        for (int k = 0; k < 3; k++)
            c[k] = (m->box_min[k] + m->box_max[k]) / 2;
    else
        set3(c, m->pos[0], m->pos[1], m->pos[2] + 4.25f);
}

/* ---------------------------------------------------------------- the buildings */

/* building.isValid() [R 0x2795E8]: only a handle check (the object there, its serial the same); a building stays in
 * the world's list when it is rubble or gone, so it stays valid */
static int b_ok(const Boss *b, int i) { return i >= 0 && i < b->nb; }

/* findBuildingByPosition (0x2A3D20 [R], called with a zero direction): of every building in the world's list, gone
 * or not, the last whose box (shape box + place) holds the point in x and y, ends included; z is not tested */
static int bld_by_pos(const Boss *b, const float p[3])
{
    int found = -1;
    for (int i = 0; i < b->nb; i++) {
        const BossBuilding *g = &b->bl[i];
        if (p[0] >= g->min[0] && p[0] <= g->max[0] && p[1] >= g->min[1] && p[1] <= g->max[1])
            found = i;
    }
    return found;
}
/* findBuildingNearPosition(p, r) (0x2B4D50) [R]: of the buildings whose box meets the box p.x +- r, p.y +- r,
 * z 0 .. 1000, the one nearest along x (p.x - its right edge when it stands left of p, its left edge - p.x else;
 * below 1000). [G] only the standing ones */
static int bld_near_pos(const Boss *b, const float p[3], float r)
{
    int best = -1;
    float bd = 1000;
    for (int i = 0; i < b->nb; i++) {
        const BossBuilding *g = &b->bl[i];
        if (!g->standing || g->max[0] < p[0] - r || g->min[0] > p[0] + r || g->max[1] < p[1] - r ||
            g->min[1] > p[1] + r || g->max[2] < 0 || g->min[2] > 1000)
            continue;
        float d = g->pos[0] <= p[0] ? p[0] - g->max[0] : g->min[0] - p[0];
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}
/* findBuildingNearMonster(d) (0x318240) [R]: the monsters' span along x (0x318390: their least and most x, widened
 * to 30 around its middle when under 20 wide, then 10 more each side), widened by d more each side (d < 0 narrows
 * it); from a random building, going a random way round the list, the first standing one whose x lies in
 * (lo, hi] */
static int bld_near_monster(Boss *b, float d)
{
    float lo = 1000, hi = -1000;
    for (int i = 0; i < b->nm; i++) {
        if (b->m[i].pos[0] < lo)
            lo = b->m[i].pos[0];
        if (b->m[i].pos[0] > hi)
            hi = b->m[i].pos[0];
    }
    if (fabsf(hi - lo) < 20) {
        float c = (hi + lo) / 2;
        lo = c - 15;
        hi = c + 15;
    }
    lo -= 10 + d;
    hi += 10 + d;
    if (b->nb <= 0)
        return -1;
    int start = irand(b) % b->nb, step = (irand(b) & 1) * 2 - 1;
    for (int k = 0; k < b->nb; k++) {
        int i = ((start + k * step) % b->nb + b->nb) % b->nb;
        const BossBuilding *g = &b->bl[i];
        if (g->standing && g->pos[0] > lo && g->pos[0] <= hi)
            return i;
    }
    return -1;
}
static int bld_random(Boss *b)                       /* getRandomBuilding (0x2AEC40) [R]: from a random one, standing */
{
    if (b->nb <= 0)
        return -1;
    int start = (int)(frand(b) * (float)b->nb);
    for (int k = 0; k < b->nb; k++) {
        int i = (start + k) % b->nb;
        if (b->bl[i].standing)
            return i;
    }
    return -1;
}
static void b_pos(const Boss *b, int i, float out[3])
{
    if (i >= 0 && i < b->nb)
        copy3(out, b->bl[i].pos);
    else
        set3(out, 0, 0, 0);
}
static int b_storeys(const Boss *b, int i) { return i >= 0 && i < b->nb ? b->bl[i].storeys : 0; }

/* ---------------------------------------------------------------- the SplineMover */

static void sp_clear(Spline *s)                      /* clearPath [R 0x33B0B0]: the points freed, the place along it 0 */
{
    s->n = 0;
    s->s = 0;
    s->ready = 0;
}
static void sp_add(Spline *s, const float p[3], float d, float k)   /* addPoint(p, d): k 0; addPointWithVel(p, d, k): the tension k */
{
    if (s->n >= MAX_SP)
        return;
    copy3(s->pt[s->n].p, p);
    s->pt[s->n].s = d;
    s->pt[s->n].k = k;
    s->n++;
    s->ready = 0;
}
/* [R] 0x13B2F0: each point's tangent is Kochanek-Bartels' 0.5 (1 - T)(1 + B)(1 + C)(p - prev) + 0.5 (1 - T)(1 - B)
 * (1 - C)(next - p) (+0x1C, +0x20, +0x24 per point); the ends take themselves as their missing neighbour, unless the
 * path is closed (its first and last points equal), which wraps round. [R 0x33B010 -> 0x13B660] addPointWithVel's k
 * is the point's T (+0x1C), addPoint's 0; B = C = 0 */
static void sp_prepare(Spline *s)
{
    int n = s->n;
    s->closed = n > 2 && s->pt[0].p[0] == s->pt[n - 1].p[0] && s->pt[0].p[1] == s->pt[n - 1].p[1] &&
                s->pt[0].p[2] == s->pt[n - 1].p[2];
    for (int i = 0; i < n; i++) {
        int pv = i - 1, nx = i + 1;
        if (pv < 0)
            pv = s->closed ? n - 2 : 0;
        if (nx >= n)
            nx = s->closed ? 1 : n - 1;
        for (int k = 0; k < 3; k++)
            s->pt[i].m[k] = 0.5f * (1 - s->pt[i].k) * ((s->pt[i].p[k] - s->pt[pv].p[k]) + (s->pt[nx].p[k] - s->pt[i].p[k]));
    }
    s->ready = 1;
}
/* [R] 0x13B780: the Hermite curve between the two points whose distances hold u (before the first: the first
 * point, past the last: the last), and its tangent */
static void sp_eval(Spline *s, float u, float pos[3], float tan[3])
{
    if (!s->ready)
        sp_prepare(s);
    int n = s->n;
    set3(tan, 0, 0, 0);
    if (n == 0)
        return;
    if (n == 1 || u <= s->pt[0].s) {
        copy3(pos, s->pt[0].p);
        if (n > 1)
            copy3(tan, s->pt[0].m);
        return;
    }
    if (u >= s->pt[n - 1].s) {
        copy3(pos, s->pt[n - 1].p);
        copy3(tan, s->pt[n - 1].m);
        return;
    }
    int i = 0;
    while (i < n - 2 && u >= s->pt[i + 1].s)
        i++;
    float len = s->pt[i + 1].s - s->pt[i].s, t = len > 0 ? (u - s->pt[i].s) / len : 1;
    float t2 = t * t, t3 = t2 * t;
    float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
    float d00 = 6 * t2 - 6 * t, d10 = 3 * t2 - 4 * t + 1, d01 = -6 * t2 + 6 * t, d11 = 3 * t2 - 2 * t;
    for (int k = 0; k < 3; k++) {
        float p0 = s->pt[i].p[k], p1 = s->pt[i + 1].p[k], m0 = s->pt[i].m[k], m1 = s->pt[i + 1].m[k];
        pos[k] = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
        tan[k] = d00 * p0 + d10 * m0 + d01 * p1 + d11 * m1;
    }
}
/* [R] the update (0x33ACA0): flag 4 holds it still (1); otherwise its place along the path grows by velocity x
 * the frame time; past the last point it loops back by the path's length with flag 1 (1), or stays at the end and
 * answers 2 - every frame it is there, until a script gives it another path. The object takes the curve's point;
 * with flag 2 also its turn: +Y along the tangent, +X = tangent x Z, +Z = X x tangent */
static int sp_step(Obj *o, float dt)
{
    Spline *s = &o->sp;
    o->oriented = 0;
    if ((s->flags & 4) || s->n == 0)
        return 1;
    int r = 1;
    float end = s->pt[s->n - 1].s, start = s->pt[0].s;
    s->s += s->vel * dt;
    if (s->s > end) {
        if (s->flags & 1)
            s->s -= end - start;
        else {
            s->s = end;
            r = 2;
        }
    }
    float tan[3];
    sp_eval(s, s->s, o->p.pos, tan);
    if (s->flags & 2) {
        if (len3(tan) < 1e-6f)
            set3(tan, 1, 0, 0);
        norm3(tan);
        copy3(s->fwd, tan);
        o->oriented = 1;
        o->p.facing[2] = atan2f(-tan[0], tan[1]);
    }
    return r;
}

/* ---------------------------------------------------------------- the LineMover (the NY blimp) */

static void ln_init(Obj *o, const float a[3], const float c[3], float speed)   /* LineMover.init(obj, a, c, v, 0) */
{
    Line *l = &o->ln;
    copy3(l->p[0], a);
    copy3(l->p[1], c);
    l->n = 2;
    l->seg = 0;
    l->t = 0;
    l->speed = speed;
    o->mover = MV_LINE;
}
static void ln_add(Obj *o, const float p[3])
{
    if (o->ln.n < MAX_LP)
        copy3(o->ln.p[o->ln.n++], p);
}
static void ln_add_path(Boss *b, Obj *o, const char *path)   /* addPath(name, 0): an Objects1_Props path's points */
{
    Group g = find_tmpl(b, path);
    Record r[3 * MAX_LP];
    uint32_t n = group_items(g, r, 3 * MAX_LP);
    for (uint32_t i = 0; i + 2 < n; i += 3) {
        float p[3] = { record_num(r[i]), record_num(r[i + 1]), record_num(r[i + 2]) };
        ln_add(o, p);
    }
}
/* [R 0x316E60] a leg's frame: X = |d x (0, 0, 1)| (none: (1, 0, 0)), Y = |d|, Z = X x Y, as a quaternion (0x105090);
 * a leg shorter than 1e-3 the identity */
static void leg_quat(const float a[3], const float c[3], float q[4])
{
    float d[3];
    sub3(d, c, a);
    if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 1e-6f) {
        q[0] = q[1] = q[2] = 0;
        q[3] = 1;
        return;
    }
    float y[3], x[3] = { d[1], -d[0], 0 }, z[3];
    copy3(y, d);
    norm3(y);
    if (len3(x) < 1e-6f)
        set3(x, 1, 0, 0);
    norm3(x);
    z[0] = x[1] * y[2] - x[2] * y[1];
    z[1] = x[2] * y[0] - x[0] * y[2];
    z[2] = x[0] * y[1] - x[1] * y[0];
    float m00 = x[0], m11 = y[1], m22 = z[2], tr = m00 + m11 + m22;   /* the columns x, y, z */
    if (tr > 0) {
        float s = sqrtf(tr + 1) * 2;
        q[3] = s / 4;
        q[0] = (y[2] - z[1]) / s;
        q[1] = (z[0] - x[2]) / s;
        q[2] = (x[1] - y[0]) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1 + m00 - m11 - m22) * 2;
        q[3] = (y[2] - z[1]) / s;
        q[0] = s / 4;
        q[1] = (y[0] + x[1]) / s;
        q[2] = (z[0] + x[2]) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1 + m11 - m00 - m22) * 2;
        q[3] = (z[0] - x[2]) / s;
        q[0] = (y[0] + x[1]) / s;
        q[1] = s / 4;
        q[2] = (z[1] + y[2]) / s;
    } else {
        float s = sqrtf(1 + m22 - m00 - m11) * 2;
        q[3] = (x[1] - y[0]) / s;
        q[0] = (z[0] + x[2]) / s;
        q[1] = (z[1] + y[2]) / s;
        q[2] = s / 4;
    }
}

static void leg_slerp(const float a[4], const float b[4], float w, float q[4])
{
    float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = 1;
    if (c < 0) {                                     /* the short way */
        c = -c;
        s = -1;
    }
    float ka = 1 - w, kb = w;
    if (c < 0.9995f) {
        float th = acosf(c), sn = sinf(th);
        ka = sinf((1 - w) * th) / sn;
        kb = sinf(w * th) / sn;
    }
    float l = 0;
    for (int k = 0; k < 4; k++) {
        q[k] = ka * a[k] + kb * s * b[k];
        l += q[k] * q[k];
    }
    l = l > 0 ? 1 / sqrtf(l) : 1;
    for (int k = 0; k < 4; k++)
        q[k] *= l;
}

static void leg_axes(const float q[4], float r[9])  /* 0x104BB0: its x, y and z axes */
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z); r[1] = 2 * (x * y + w * z);     r[2] = 2 * (x * z - w * y);
    r[3] = 2 * (x * y - w * z);     r[4] = 1 - 2 * (x * x + z * z); r[5] = 2 * (y * z + w * x);
    r[6] = 2 * (x * z + w * y);     r[7] = 2 * (y * z - w * x);     r[8] = 1 - 2 * (x * x + y * y);
}

/* [R] 0x315E10 (as people.c): the share of the segment grows by speed x dt / its length; past it the next segment
 * (3), after the last the start again with flag 2, else it stays at the end (2, every frame); flag 1 turns it the
 * way it goes */
static int ln_step(Obj *o, float dt)
{
    Line *l = &o->ln;
    if (l->n < 2)
        return 1;
    int r = 1;
    float *a = l->p[l->seg], *c = l->p[l->seg + 1];
    float d[3];
    sub3(d, c, a);
    float len = len3(d);
    if (len > 0)
        l->t += dt * l->speed / len;
    if (l->t > 1) {
        if (l->seg < l->n - 2) {
            l->seg++;
            l->t = 0;
            r = 3;
        } else if (l->flags & 2) {
            l->seg = 0;
            l->t = 0;
            r = 2;
        } else {
            l->t = 1;
            r = 2;
        }
    }
    a = l->p[l->seg];
    c = l->p[l->seg + 1];
    for (int k = 0; k < 3; k++)
        o->p.pos[k] = a[k] + (c[k] - a[k]) * l->t;
    if (l->flags & 1) {
        /* [R 0x316E60, 0x316A24] the leg's full frame (it pitches along the leg: Path_NYEmpire1's z runs 42.5 ..
         * 50.5), as a quaternion (+0x70) put on the object's matrix (0x104BB0). Flag 4 (flag 8 off): over the
         * segment's second half (+0x68 = 0.5) it slerps into the next leg's (after the last: the first's with flag 2,
         * else its own; 0x3164E8 .. 0x316590) by w = s^2 (3 - 2s), s = (t - 0.5) / 0.5 (below 0.001 none, above
         * 0.999 all); the place stays on the segment */
        float q[4];
        leg_quat(a, c, q);
        if ((l->flags & 4) && !(l->flags & 8) && l->t > 0.5f) {
            int nx = l->seg + 1 < l->n - 1 ? l->seg + 1 : (l->flags & 2) ? 0 : l->seg;
            float sx = (l->t - 0.5f) / 0.5f, w = sx * sx * (3 - 2 * sx);
            if (w > 0.999f)
                w = 1;
            if (w >= 0.001f) {
                float qn[4];
                leg_quat(l->p[nx], l->p[nx + 1], qn);
                leg_slerp(q, qn, w, q);
            }
        }
        leg_axes(q, o->lrot);
        o->oriented = 2;
        if (o->lrot[3] != 0 || o->lrot[4] != 0)      /* the heading the scripts read: its y axis's */
            o->p.facing[2] = atan2f(-o->lrot[3], o->lrot[4]);
    }
    return r;
}

/* ---------------------------------------------------------------- the parts */

static void obj_matrix(Obj *o)
{
    float *w = o->p.world;
    if (o->oriented == 1) {                          /* [R] 0x33AD58: the spline's frame */
        float f[3], up[3] = { 0, 0, 1 }, x[3], z[3];
        copy3(f, o->sp.fwd);
        x[0] = f[1] * up[2] - f[2] * up[1];
        x[1] = f[2] * up[0] - f[0] * up[2];
        x[2] = f[0] * up[1] - f[1] * up[0];
        if (len3(x) < 1e-6f)
            set3(x, 1, 0, 0);
        norm3(x);
        z[0] = x[1] * f[2] - x[2] * f[1];
        z[1] = x[2] * f[0] - x[0] * f[2];
        z[2] = x[0] * f[1] - x[1] * f[0];
        for (int k = 0; k < 3; k++) {
            w[k] = x[k];
            w[4 + k] = f[k];
            w[8 + k] = z[k];
        }
    } else if (o->oriented == 2) {                   /* the LineMover's frame */
        for (int k = 0; k < 3; k++) {
            w[k] = o->lrot[k];
            w[4 + k] = o->lrot[3 + k];
            w[8 + k] = o->lrot[6 + k];
        }
    } else                                           /* setFacing(x, y, z): Rz Rx Ry (0x101330) */
        army_facing_turn(o->p.facing, w);
    w[3] = w[7] = w[11] = 0;
    w[12] = o->p.pos[0];
    w[13] = o->p.pos[1];
    w[14] = o->p.pos[2];
    w[15] = 1;
}

static void start_anim(Boss *b, Obj *o, const char *name)   /* Actor.startAnim (0x30AAD0, as people.c / army.c) */
{
    snprintf(o->p.anim, sizeof o->p.anim, "%s", name);
    char key[64];
    snprintf(key, sizeof key, "ActorAnimShape|%s", name);
    Group look = group_sub(o->p.tmpl, key);
    float len = 0;
    o->p.anim_loop = 0;
    o->p.clip = NULL;
    if (look.at) {
        o->p.anim_loop = group_num(look, "AnimRepeat", 0) != 0;
        len = fabsf(group_num(look, "Duration", 0));
        const char *shape = group_str(look, "Shape|ShapeName");
        if (shape)
            o->p.shape = shape;
        o->p.clip = group_str(look, "Anim|AnimName");
        if (len <= 0 && b->host.clip_seconds)
            len = b->host.clip_seconds(b->host.ctx, o->p.clip, o->p.shape);
    }
    o->p.anim_len = len;
    o->p.anim_time = 0;
    o->anim_done = 0;
}

/* the template's weapons: WeaponSystem { WeaponCluster { turret, Weapon| {...} ... } } (army.c unit_weapons) */
static void obj_weapons(Boss *b, Obj *o)
{
    Record r[8];
    o->nw = 0;
    if (!b->host.weapon_make)
        return;
    Group sys = group_sub(o->p.tmpl, "WeaponSystem|WeaponSystem");
    if (!sys.at)
        sys = group_sub(o->p.tmpl, "WeaponSystem|");
    if (!sys.at)
        return;
    uint32_t nc = group_all(sys, "WeaponCluster|", r, 8);
    for (uint32_t c = 0; c < nc && o->nw < 4; c++) {
        Group cl = record_group(sys, r[c]);
        Record wr[8];
        uint32_t nw = group_all(cl, "Weapon|", wr, 8);
        for (uint32_t i = 0; i < nw && o->nw < 4; i++) {
            ArmyWeapon *w = &o->w[o->nw];
            if (!b->host.weapon_make(b->host.ctx, w, record_group(cl, wr[i])))
                continue;
            int k = o->nw++;
            w->rot_range = group_num(cl, "RotationRange", 0) * PI / 180;
            w->rot_rate = group_num(cl, "RotationRate", 1);
            w->elev_range = group_num(cl, "ElevationRange", 0) * PI / 180;
            w->elev_rate = group_num(cl, "ElevationRate", 1);
            o->elev_min[k] = group_num(cl, "ElevationMinRange", -group_num(cl, "ElevationRange", 90)) * PI / 180;
            o->elev_max[k] = group_num(cl, "ElevationMaxRange", group_num(cl, "ElevationRange", 90)) * PI / 180;
            o->need_rot[k] = group_num(cl, "RequiresRotationClearToFire", 1) != 0;
            o->need_elev[k] = group_num(cl, "RequiresElevationClearToFire", 1) != 0;
            w->on = 1;
        }
    }
}
static void weapon_ammo(Obj *o, int n, int i)        /* setWeaponAmmo(n, 0, i) [G: the weapon's ammo, as army.c] */
{
    if (i >= 0 && i < o->nw)
        o->w[i].ammo = n;
}
static void weapon_reload(Obj *o, int i)             /* reloadWeapon(i, 0) [G: back to its Ammo, as army.c] */
{
    if (i >= 0 && i < o->nw)
        o->w[i].ammo = o->w[i].ammo_max > 0 ? o->w[i].ammo_max : 1;
}
static void weapons_enable(Obj *o, int on) { o->weapons_on = on; }   /* enableWeapons */
static void set_target(Obj *o, int monster)          /* setTarget(monster) / setTarget(nullTarget) */
{
    o->target = monster;
    o->has_point = 0;
}
static void set_target_point(Obj *o, const float p[3])   /* setTarget(a DummyObject) and its setPoint */
{
    o->target = -1;
    o->has_point = 1;
    copy3(o->point, p);
}

/* every weapon this frame, through the army (weapon_tick). Where it fires from: the WeaponNode as drawn when the
 * host knows it, else the part's place; which way: the cluster turned onto the target at RotationRate /
 * ElevationRate [G: radians a second, as army.c] within its ranges, and fired once on it (or at once where
 * Requires...ClearToFire is 0) and within MaxFiringRange; a bomb (Direction -Z) drops along the node's -Z */
static void obj_weapons_tick(Boss *b, Obj *o, float dt)
{
    if (!b->host.weapon_tick || o->nw == 0)
        return;
    float tp[3] = { 0, 0, 0 };
    int has = 0;
    if (o->has_point) {
        copy3(tp, o->point);
        has = 1;
    } else if (m_valid(b, o->target)) {
        m_centre(&b->m[o->target], tp);
        has = 1;
    }
    for (int i = 0; i < o->nw; i++) {
        ArmyWeapon *w = &o->w[i];
        float node[16], from[3], dir[3], F[3][3];
        int have_node = b->host.node_world && w->node && b->host.node_world(b->host.ctx, &o->p, w->node, node);
        for (int r = 0; r < 3 && have_node; r++) {   /* [R 0x2971A0] the firing frame: the node's world */
            copy3(F[r], node + 4 * r);
            float l = sqrtf(F[r][0] * F[r][0] + F[r][1] * F[r][1] + F[r][2] * F[r][2]);
            if (l > 1e-6f)
                for (int k = 0; k < 3; k++)
                    F[r][k] /= l;
        }
        if (have_node)
            copy3(from, node + 12);
        else
            copy3(from, o->p.pos);
        int aimed = 1;
        if (w->axis_neg && w->axis == 2) {
            if (have_node)
                set3(dir, -node[8], -node[9], -node[10]);
            else
                set3(dir, 0, 0, -1);
        } else {
            float want_yaw = 0, want_pitch = 0;
            if (has) {
                float d[3];
                sub3(d, tp, from);
                float h = sqrtf(d[0] * d[0] + d[1] * d[1]);
                want_yaw = atan2f(-d[0], d[1]) - o->p.facing[2];
                while (want_yaw > PI) want_yaw -= 2 * PI;
                while (want_yaw < -PI) want_yaw += 2 * PI;
                want_pitch = atan2f(d[2], h);
                if (w->rot_range > 0 && w->rot_range < PI)
                    want_yaw = clampf(want_yaw, -w->rot_range, w->rot_range);
                want_pitch = clampf(want_pitch, o->elev_min[i], o->elev_max[i]);
            }
            float sy = w->rot_rate * dt, sp = w->elev_rate * dt;
            w->yaw += clampf(want_yaw - w->yaw, -sy, sy);
            w->pitch += clampf(want_pitch - w->pitch, -sp, sp);
            aimed = (!o->need_rot[i] || fabsf(want_yaw - w->yaw) < 0.05f) &&
                    (!o->need_elev[i] || fabsf(want_pitch - w->pitch) < 0.05f);
            float h = o->p.facing[2] + w->yaw;
            set3(dir, -sinf(h) * cosf(w->pitch), cosf(h) * cosf(w->pitch), sinf(w->pitch));
            if (have_node)
                army_frame_turn(F, o->p.facing[2], w->yaw, w->pitch);
        }
        int may = has && o->weapons_on && aimed && dist3(from, tp) <= w->range;
        b->host.weapon_tick(b->host.ctx, w, may, from, dir, have_node ? (const float (*)[3])F : NULL,
                            o->has_point ? -1 : o->target, dt);
    }
}

static int kind_of(const char *script)
{
    static const char *const NAMES[] = { "Boss_Tank.bsf", "Boss_Tank_Bomb.bsf", "Boss_Helicopter.bsf",
                                         "Boss_Jumper.bsf", "Boss_Jumper_Bomb.bsf", "Boss_Chicago_Saucer.bsf",
                                         "Boss_Chicago_Whirlwind.bsf", "Boss_NY_Blimp.bsf", "Boss_NY_Generator.bsf",
                                         "Boss_Blimp_Ambient.bsf", "Boss_Saucer_Ambient.bsf" };
    for (int i = 0; i < (int)(sizeof NAMES / sizeof *NAMES); i++)
        if (same_text(script, NAMES[i]))
            return i;
    return BK_NONE;
}
int boss_is_boss_template(Group tmpl) { return kind_of(gstr(tmpl, "ScriptName")) != BK_NONE; }

/* a book's collision keys (WSimpleObject 0x28FD70): CollisionBox { min x y z, max x y z } */
static const float *book_box(Group g, float bx[6])
{
    Record r[8];
    if (!g.at)
        return NULL;
    Group cb = group_sub(g, "CollisionBox|CollisionBox");
    if (!cb.at)
        cb = group_sub(g, "CollisionBox");
    if (!cb.at || group_items(cb, r, 8) < 6)
        return NULL;
    for (int k = 0; k < 6; k++)
        bx[k] = record_num(r[k]);
    return bx;
}

static Obj *obj_new(Boss *b, const char *name, Group tmpl, Group inst, const float pos[3])
{
    int k = -1;
    for (int i = 0; i < b->n; i++)
        if (!b->o[i].p.alive) {
            k = i;
            break;
        }
    if (k < 0) {
        if (b->n == MAX_PARTS)
            return NULL;
        k = b->n++;
    }
    Obj *o = &b->o[k];
    memset(o, 0, sizeof *o);
    o->p.name = name;
    o->p.tmpl = tmpl;
    o->p.inst = inst;
    o->p.alive = 1;
    o->p.owner = -1;
    o->p.kind = kind_of(gstr(tmpl, "ScriptName"));
    if (o->p.kind == BK_NONE)
        o->p.kind = BK_ROTOR;
    o->p.shape = gstr(tmpl, "Shape");
    if (!o->p.shape)
        o->p.shape = gstr(tmpl, "ShapeName");
    o->p.scale = group_num(tmpl, "RenderScale", 1);
    o->p.hp_frac = 1;
    if (pos)
        copy3(o->p.pos, pos);
    o->target = -1;
    o->spawner = -1;
    o->pulse_delay = 0.25f;                          /* [R as army.c] an AttackObject's pulse */
    o->weapons_on = 1;                               /* [G] Boss_Helicopter never calls enableWeapons, yet fires */
    /* [R 0x315810 .. 0x3159F0, the AttackObject's init] its wobble's keys (army_osc_init). Of the bosses only
     * BossSaucerAmbient has one (0.075) */
    army_osc_init(&o->osc, tmpl, inst);
    /* its shape (0x28FD70, collide_shape_pick): the template's CollisionBox { min x y z, max x y z } (the
     * generators'), CollisionRadius or AutoCollisionBox (the blimp) with the model's box, then the instance's; else
     * the model's bounding sphere [G: taken as its box's; no model: radius 1 at the origin]. lo / hi: its box */
    {
        float tb[6], ib[6], mb[6], c[3] = { 0, 0, 0 }, rr = 1;
        int have = o->p.shape && b->host.shape_box && b->host.shape_box(b->host.ctx, o->p.shape, mb, mb + 3);
        if (have) {
            float h[3];
            for (int k = 0; k < 3; k++) {
                c[k] = (mb[k] + mb[3 + k]) / 2;
                h[k] = (mb[3 + k] - mb[k]) / 2;
            }
            rr = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
        }
        /* [R 0x28FD70 -> 0x184CB0] the sphere the model keeps, the union of its parts' spheres (0x185AA0) */
        if (have && b->host.shape_sphere)
            b->host.shape_sphere(b->host.ctx, o->p.shape, c, &rr);
        collide_shape_pick(&o->cshape, have ? c : NULL, rr, book_box(tmpl, tb), group_num(tmpl, "CollisionRadius", -1),
                           group_num(tmpl, "AutoCollisionBox", 0) != 0, inst.at ? book_box(inst, ib) : NULL,
                           inst.at ? group_num(inst, "CollisionRadius", -1) : -1,
                           inst.at && group_num(inst, "AutoCollisionBox", 0) != 0, have ? mb : NULL);
        for (int k = 0; k < 3; k++) {
            o->lo[k] = o->cshape.centre[k] - o->cshape.half[k];
            o->hi[k] = o->cshape.centre[k] + o->cshape.half[k];
        }
    }
    const char *def = gstr(tmpl, "DefaultAnim");     /* an actor starts in its DefaultAnim */
    char key[64];
    if (def && group_sub(tmpl, (snprintf(key, sizeof key, "ActorAnimShape|%s", def), key)).at)
        start_anim(b, o, def);
    obj_weapons(b, o);
    obj_matrix(o);
    emit(b, BE_SPAWNED, o, name, NULL);
    return o;
}
static Obj *obj_make(Boss *b, const char *name, const float pos[3])   /* createScriptObject / createAttackObject */
{
    Group t = find_tmpl(b, name);
    return t.at ? obj_new(b, name, t, (Group){ 0 }, pos) : NULL;
}

static void obj_remove(Boss *b, Obj *o)              /* requestDelete */
{
    if (!o || !o->p.alive)
        return;
    for (int i = 0; i < o->nfx_die; i++)
        fx_stop(b, o->fx_die[i]);
    o->nfx_die = 0;
    emit(b, BE_SOUND, o, NULL, NULL)->level = 0;
    emit(b, BE_REMOVED, o, o->p.name, NULL);
    o->p.alive = 0;
    if (idx(b, o) == b->main)
        b->main = -1;
}
static Obj *obj_at(Boss *b, int i) { return i >= 0 && i < b->n && b->o[i].p.alive ? &b->o[i] : NULL; }

static const char *numbered(char *buf, size_t n, const char *key, int i)   /* "HitEffect" + i */
{
    snprintf(buf, n, "%s%d", key, i);
    return buf;
}

static float wrap_pi(float a)                        /* [G] getFacing().z read back in (-pi, pi] */
{
    while (a > PI)
        a -= 2 * PI;
    while (a <= -PI)
        a += 2 * PI;
    return a;
}
static void horizontal_knock(float v[3], const float d[3], float across, float up)   /* the scripts' throw */
{
    set3(v, d[0], d[1], 0);
    norm3(v);
    scale3(v, across);
    v[2] = up;
}
static int visible(const Boss *b, const float p[3], float r)   /* gWorld.isSphereVisible (unset: the view's x range) */
{
    if (b->host.visible)
        return b->host.visible(b->host.ctx, p, r);
    return p[0] + r >= b->view[0] && p[0] - r <= b->view[1];
}
/* a knocked-away path (Boss_Helicopter createKnockedAwayPath, Boss_Jumper doHitReaction, Boss_Chicago_Saucer
 * createKnockedAwayPath) [R]: away from the monster, lifted by `lift` over it, 5 a step (10 when shorter than 7.5),
 * each step turned back off a building that stands 6 above where it started (x when it heads away from the middle,
 * y else) and jittered by -2 .. 2 in x and -1 .. 3 in y */
static int knock_points(Boss *b, const float pos[3], const float mp[3], float lift, int steps, int climb, float zcap,
                        float out[][3])
{
    float dir[3], cur[3];
    sub3(dir, pos, mp);
    dir[2] = pos[2] - (mp[2] + lift);
    if (dir[2] < 0)
        dir[2] = -dir[2];
    norm3(dir);
    scale3(dir, 5);
    if (len3(dir) < 7.5f)
        scale3(dir, 2);
    for (int j = 0; climb && dir[2] < 5 && j < 32; j++)   /* [G] the script's loop never ends for a level way */
        dir[2] = dir[2] > 0 ? dir[2] * 2 : 5;
    copy3(cur, pos);
    int n = 0;
    for (int i = 0; i < steps; i++) {
        float p[3] = { cur[0] + dir[0], cur[1] + dir[1], cur[2] + dir[2] };
        int bi = bld_by_pos(b, p);
        if (bi >= 0 && ROOF(b->bl[bi].storeys) >= pos[2] + 6) {
            float dp[3];
            sub3(dp, cur, b->bl[bi].pos);
            if ((cur[0] >= 0 && dp[0] > 0) || (cur[0] <= 0 && dp[0] <= 0))
                dir[0] = -dir[0];
            else
                dir[1] = -dir[1];
            for (int k = 0; k < 3; k++)
                p[k] = cur[k] + dir[k];
        }
        p[0] = p[0] - 2 + frand(b) * 4;
        p[1] = p[1] - 1 + frand(b) * 4;
        copy3(out[n++], p);
        copy3(cur, p);
        if (zcap > 0 && cur[2] > pos[2] + zcap)
            break;
    }
    return n;
}
static void sp_by_distance(Spline *s, float pts[][3], int n)   /* addPoint(p[i], the distance so far) */
{
    float d = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0)
            d += dist3(pts[i], pts[i - 1]);
        sp_add(s, pts[i], d, 0);
    }
}

static void obj_init(Boss *b, Obj *o);

/* ---------------------------------------------------------------- Boss_Tank.bsf */

static void tank_taunt(Boss *b, Obj *o)              /* doTaunt: at most every 5 s */
{
    TankDef *t = &o->s.tank;
    if (t->taunt <= 0) {
        t->taunt = 5;
        sfx(b, o, "V_Boss_Taunt");
    }
}

static void tank_init(Boss *b, Obj *o)               /* Construct + onInit */
{
    TankDef *t = &o->s.tank;
    o->oflags |= OF_EFF_NODES | OF_PUSH_OUT;
    t->start_hp = unum(o, "HitPoints", 51);
    t->hp = t->start_hp;
    t->accel = unum(o, "Acceleration", 20);
    t->max_vel = unum(o, "MaxVelocity", 15);
    t->sw_damage = unum(o, "ShockwaveDamage", 250);
    t->bomb_launch = unum(o, "BombFrequency", 10);
    t->osc_scale = 1;
    t->taunt = 5;
    t->weapons_off = 1;
    t->sw_state = 1;
    t->sw_fire = -1;
    t->leave_dur = unum(o, "LeaveDuration", 20);
    t->leave_delay = t->leave_dur;
    t->monster = -1;
    t->damage_fx = -1;
    set3(o->p.pos, -100, -10, 0);                    /* [R] in from the left, facing +x */
    o->p.facing[2] = -PI / 2;
    t->effect = fx_on(b, o, ustr(o, "AttachEffect"), NULL, NULL, -1, 1);
    t->sw_name = ustr(o, "ShockwaveEffect");
    t->shock_fx = fx_on(b, o, t->sw_name, NULL, NULL, -1, 0);
    camera_rmh(b, 35);
}

static void tank_shockwave(Boss *b, Obj *o, float dt)   /* processShockwave */
{
    TankDef *t = &o->s.tank;
    float pos[3];
    copy3(pos, o->p.pos);
    if (t->sw_state == 0) {                          /* recharging */
        t->sw_recharge -= dt;
        if (t->sw_recharge <= 0) {
            t->sw_state = 1;
            t->shock_fx = fx_on(b, o, t->sw_name, NULL, NULL, -1, 0);
        }
    } else if (t->sw_fire > 0) {                     /* the blast's countdown */
        t->sw_fire -= dt;
        if (t->sw_fire <= 0) {
            t->sw_state = 0;
            t->jump_vel = 12;
            t->sw_retreat = 1;
            t->sw_retreat_t = 1.55f;
            t->sw_recharge = 2.66f;
            tank_taunt(b, o);
            fx_at(b, ustr(o, "ShockwaveBlast"), pos);
            fx_stop(b, t->shock_fx);
            t->shock_fx = -1;
            for (int i = 0; i < b->nm; i++) {       /* 7.5 around it at full strength, fading to nothing 10 further */
                float d[3], v[3];
                sub3(d, b->m[i].pos, pos);
                float dist = len3(d) - 7.5f, sc = 1;
                if (dist > 0)
                    sc = 1 - dist / 10;
                if (sc > 0 && b->m[i].pos[2] < 9) {
                    horizontal_knock(v, d, 25 * sc, 20 + 25 * sc);
                    m_vel(b, i, v);
                    m_reaction(b, i, "React_Airborne");
                    m_hp(b, i, -t->sw_damage * sc);
                }
            }
        }
    } else if (!t->hit_retreat) {                    /* charged: a monster near on the street sets it off */
        int ok = 0;
        for (int i = 0; !ok && i < b->nm; i++) {
            float d[3];
            sub3(d, b->m[i].pos, pos);
            if (fabsf(d[0]) < 15 && fabsf(d[1]) < 10 && b->m[i].pos[2] < 6.0f / 2) {
                t->sw_fire = 1.25f;
                ok = 1;
            }
        }
        if (!ok) {                                   /* else the gun is fed one shot, every frame */
            weapon_ammo(o, 1, 0);
            tank_taunt(b, o);
        }
    }
    if (t->sw_retreat) {                             /* backs off from its monster for 1.55 s, then slows */
        float mp[3];
        m_pos(b, t->monster, mp);
        float fdif = mp[0] - pos[0];
        if (t->sw_retreat_t > 0) {
            t->sw_retreat_t -= dt;
            float a = dt * t->accel / 2;
            if (fdif >= 0)
                a = -a;
            t->vel += a;
        } else {
            t->vel *= 0.9f;
            if (fabsf(t->vel) <= 0.1f) {
                t->sw_retreat = 0;
                weapon_ammo(o, 1, 0);
                return;
            }
        }
        t->vel = clampf(t->vel, -t->max_vel, t->max_vel);
        pos[0] += t->vel * dt;
        copy3(o->p.pos, pos);
    }
    if (t->jump_vel > 0 || pos[2] > 0) {             /* the hop of the blast: 12 up, 24 / s^2 down */
        t->jump_vel -= 2 * dt * 12;
        pos[2] += t->jump_vel * dt;
        if (pos[2] <= 0) {
            pos[2] = 0;
            t->jump_vel = 0;
        }
        copy3(o->p.pos, pos);
    }
}

static void tank_move(Boss *b, Obj *o, float dt)     /* processMoveToTarget */
{
    TankDef *t = &o->s.tank;
    float pos[3], mp[3];
    copy3(pos, o->p.pos);
    m_pos(b, t->monster, mp);
    float fdif = mp[0] - pos[0];
    if (fdif >= 10 * 1.15f || fdif <= -10 * 1.15f) {   /* towards it beyond 11.5 along x, else brakes */
        float a = dt * t->accel;
        if (fdif < 0)
            a = -a;
        t->vel += a;
    } else
        t->vel *= 0.9f;
    if (t->move_delay > 0)
        t->move_delay -= dt;
    else if (t->vel >= 0.1f || t->vel <= -0.1f) {
        t->vel = clampf(t->vel, -t->max_vel, t->max_vel);
        pos[0] += t->vel * dt;
        copy3(o->p.pos, pos);
    } else
        t->move_delay = 3;                           /* stopped: it waits 3 s */
    if (t->hit_retreat) {                            /* hit hard: backs away for 1.25 s, then slows */
        fdif = mp[0] - pos[0];
        if (t->hit_retreat_t > 0) {
            t->hit_retreat_t -= dt;
            float a = dt * t->accel;
            if (fdif >= 0)
                a = -a;
            t->vel += a;
        } else {
            t->vel *= 0.9f;
            if (fabsf(t->vel) <= 0.1f)
                t->hit_retreat = 0;
        }
        t->vel = clampf(t->vel, -t->max_vel, t->max_vel);
        pos[0] += t->vel * dt;
        copy3(o->p.pos, pos);
    }
}

static int tank_flee(Boss *b, Obj *o, float dt)      /* processFleeing */
{
    TankDef *t = &o->s.tank;
    float pos[3];
    copy3(pos, o->p.pos);
    if (t->leave == 1) {                             /* away from its monster; back after ReturnDuration */
        float mp[3];
        m_pos(b, t->monster, mp);
        if (mp[0] > pos[0])
            t->hit_retreat = 1;
        t->weapons_off = 1;
        t->leave = 3;
        t->leave_delay = unum(o, "ReturnDuration", 20);
    }
    float a = dt * t->accel;
    if (t->hit_retreat) {                            /* leaving to the left: it turns to face -x at pi / 3 a second */
        a = -a;
        float f = o->p.facing[2] + PI / 3 * dt;
        if (f >= PI / 2)
            f = PI / 2;
        o->p.facing[2] = f;
    }
    t->vel += a;
    t->vel = clampf(t->vel, -t->max_vel, t->max_vel);
    pos[0] += t->vel * dt;
    if (fabsf(pos[0]) < 95)
        copy3(o->p.pos, pos);
    if (pos[0] > 95 || pos[0] < -95) {
        if (t->destroyed || b->city_destroyed) {
            fx_stop(b, t->shock_fx);
            t->shock_fx = -1;
            return DELETE_ME;
        }
        t->leave_delay -= dt;
        if (t->leave_delay <= 0) {
            t->leave_delay = t->leave_dur + 6;
            t->leave = 0;
        }
    }
    return 1;
}

static int tank_process(Boss *b, Obj *o, float dt)  /* Process, every frame */
{
    TankDef *t = &o->s.tank;
    t->taunt -= dt;
    if (t->destroyed || t->leave != 0)
        return tank_flee(b, o, dt);
    t->retarget -= dt;
    t->leave_delay -= dt;
    if (t->weapons_off) {                            /* its weapons on once 10 past it is on screen */
        float p[3];
        copy3(p, o->p.pos);
        p[0] += p[0] > 0 ? 10 : -10;
        if (visible(b, p, 1)) {
            t->weapons_off = 0;
            weapons_enable(o, 1);
        }
    }
    if (t->leave_delay <= 0) {                       /* LeaveDuration up: it drives off */
        t->leave = 1;
        weapons_enable(o, 0);
    }
    if (t->retarget <= 0) {                          /* every 5 s, half the time, another monster */
        t->retarget = 5;
        if (frand(b) < 0.5f || !m_valid(b, t->monster)) {
            t->monster = any_monster(b);
            set_target(o, t->monster);
        }
    }
    tank_shockwave(b, o, dt);
    if (!t->sw_retreat)
        tank_move(b, o, dt);
    if (t->sw_fire < 0 && !t->hit_retreat && !t->sw_retreat && !b->city_destroyed) {
        t->bomb_launch -= dt;                        /* a BossTankBomb every BombFrequency, then every 10 s */
        if (t->bomb_launch <= 0) {
            tank_taunt(b, o);
            t->bomb_launch = 10;
            Obj *so = obj_make(b, "BossTankBomb", o->p.pos);
            if (so)
                obj_init(b, so);
        }
    }
    float zf = o->p.facing[2];                       /* the hover's wobble */
    t->osc += dt;
    o->p.facing[0] = sinf(t->osc * 2) * 0.0625f * t->osc_scale;
    o->p.facing[1] = cosf(t->osc * 3) * 0.0625f * t->osc_scale;
    o->p.facing[2] = zf;
    return 1;
}

static int tank_damage(Boss *b, Obj *o, int monster, float amount)   /* TakeDamage */
{
    TankDef *t = &o->s.tank;
    if (!m_valid(b, monster))
        return 1;
    if (t->destroyed) {                              /* the wreck shoves a monster on the street that walks into it */
        float px = o->p.pos[0], mx = b->m[monster].pos[0];
        int knock = (t->hit_retreat && mx <= px + 3) || (!t->hit_retreat && mx >= px - 3);
        if (knock && contains(m_move(b, monster), "Ground")) {
            float v[3] = { 0, 15, 0 };
            m_vel(b, monster, v);
        }
        return 1;
    }
    if (t->sw_fire > 0)                              /* a hit puts the blast back to 1.25 s */
        t->sw_fire = 1.25f;
    if (!t->hit_retreat) {                           /* a hit across a quarter of its HP: it retreats and leaves */
        float cur = t->hp, goal = 1;
        if (cur > t->start_hp * 0.75f)
            goal = t->hp - t->start_hp * 0.75f;
        else if (cur > t->start_hp * 0.5f)
            goal = t->hp - t->start_hp * 0.5f;
        else if (cur > t->start_hp * 0.25f)
            goal = t->hp - t->start_hp * 0.25f;
        if (amount >= goal) {
            t->hit_retreat = 1;
            t->hit_retreat_t = 1.25f;
            t->leave_delay = -1;
            t->leave = 1;
            weapons_enable(o, 0);
            sfx(b, o, "V_Boss_Damaged");
        }
    }
    t->hp -= amount;
    challenge_hud(b, o, t->hp / t->start_hp);
    fx_on(b, o, ustr(o, "HitEffect"), NULL, NULL, -1, 0);
    char key[32];
    const char *e = ustr(o, numbered(key, sizeof key, "HitEffect", (int)(t->hp / (t->start_hp / 3))));
    if (e) {
        fx_stop(b, t->damage_fx);
        t->damage_fx = fx_on(b, o, e, NULL, NULL, -1, 0);
    }
    if (t->hp <= 0) {
        add_stats(b, o, monster, 2);
        sfx(b, o, "V_Boss_Defeated");
        weapons_enable(o, 0);
        t->destroyed = 1;
        t->leave = 1;
        t->monster = monster;
        o->oflags &= ~(uint32_t)OF_EFF_NODES;        /* from now on touching it counts */
        const char *cb = ustr(o, "CompletionCallback");
        completion(b, o, cb ? cb : "Error: Boss Tank Completion callback not found!");
    }
    return 1;
}

/* ---------------------------------------------------------------- Boss_Tank_Bomb.bsf */

static void tbomb_init(Boss *b, Obj *o)              /* Construct + onInit: a lob onto the roof a monster is on */
{
    TankBombDef *t = &o->s.tbomb;
    t->effect = -1;
    o->pulse_delay = 0.25f;
    float mx = 0;
    int bi = -1;
    for (int i = 0; bi < 0 && i < b->nm; i++) {
        bi = bld_by_pos(b, b->m[i].pos);
        if (bi >= 0)
            mx = b->m[i].pos[0];
    }
    if (bi < 0)
        bi = bld_near_monster(b, 10);
    if (bi < 0)
        bi = bld_random(b);
    if (bi < 0) {
        t->delete_me = 1;
        return;
    }
    float pos[3], xit[3], end[3], arc[3];
    copy3(pos, o->p.pos);
    copy3(xit, pos);
    xit[2] += 5;
    b_pos(b, bi, end);
    if (mx != 0)
        end[0] = mx;
    end[2] = ROOF(b_storeys(b, bi));
    set3(arc, (pos[0] + end[0]) / 2, (pos[1] + end[1]) / 2, end[2] + end[2] * 0.75f);
    float s1 = dist3(xit, pos), s2 = s1 + dist3(arc, xit), s3 = s2 + dist3(end, arc);
    o->mover = MV_SPLINE;
    sp_clear(&o->sp);
    o->sp.flags = 2;
    o->sp.vel = 20;
    sp_add(&o->sp, pos, 0, 0);
    sp_add(&o->sp, xit, s1, 0);
    sp_add(&o->sp, arc, s2, 0);
    sp_add(&o->sp, end, s3, 0);
    t->effect = fx_on(b, o, ustr(o, "AttachEffect"), NULL, NULL, -1, 0);
}

static int tbomb_mover(Boss *b, Obj *o, int state)  /* onMoverCallback: it lands */
{
    if (state != 2)
        return 1;
    fx_at(b, ustr(o, "ImpactEffect"), o->p.pos);
    for (int i = 0; i < b->nm; i++) {               /* within 7.5, fading out, and within 6 up or down */
        float d[3], v[3];
        sub3(d, b->m[i].pos, o->p.pos);
        float dist = len3(d), sc = 1;
        if (dist > 0)
            sc = 1 - dist / 7.5f;
        if (sc > 0 && fabsf(d[2]) < 6) {
            horizontal_knock(v, d, 15 * sc, 10 + 15 * sc);
            m_vel(b, i, v);
            m_reaction(b, i, "React_Airborne");
            m_hp(b, i, -unum(o, "Damage", 50) * sc);
        }
    }
    fx_stop(b, o->s.tbomb.effect);
    return DELETE_ME;
}

/* ---------------------------------------------------------------- Boss_Helicopter.bsf */

static void heli_flight_path(Boss *b, Obj *o)        /* createFlightPath: one of the AIInstance's Path<n> */
{
    HeliDef *h = &o->s.heli;
    o->sp.flags |= 2;
    h->monster = any_monster(b);
    h->flight_state = 0;
    h->heading_right = o->p.pos[0] < 0;
    int np = (int)unum(o, "NumPaths", -1), n = 0;
    float pts[MAX_SP][3];
    if (np > 0) {
        char base[32], key[64];
        snprintf(base, sizeof base, ".Path%d", irand(b) % np);
        snprintf(key, sizeof key, "%s.NumPoints", base);
        n = (int)unum(o, key, 0);
        if (n > MAX_SP)
            n = MAX_SP;
        for (int i = 0; i < n; i++) {
            set3(pts[i], 0, 0, -1);
            snprintf(key, sizeof key, "%s.Point%d", base, i);
            upoint(o, key, pts[i]);
            if (pts[i][0] >= -40 && pts[i][0] <= 40) {   /* 3 over the buildings near it */
                int bi = bld_near_pos(b, pts[i], 20);
                if (bi >= 0) {
                    float ht = ROOF(b->bl[bi].storeys);
                    if (ht + 3 > pts[i][2])
                        pts[i][2] = ht + 3;
                }
            }
        }
    }
    if (n < 2) {                                     /* [G] no paths (the script asserts): straight across */
        n = 2;
        set3(pts[0], 150, -10, 22);
        set3(pts[1], -150, -10, 22);
    }
    for (int i = 1; i < n - 1; i++) {               /* a climb of over 12 within 50 is eased in from both sides */
        float d[3];
        sub3(d, pts[i], pts[i - 1]);
        if (len3(d) < 50 && d[2] > 12)
            pts[i - 1][2] += d[2] / 3;
        sub3(d, pts[i], pts[i + 1]);
        if (len3(d) < 50 && d[2] > 12)
            pts[i + 1][2] += d[2] / 3;
    }
    sp_clear(&o->sp);
    if (h->heading_right) {                          /* from the left: the path backwards */
        float rev[MAX_SP][3];
        for (int i = 0; i < n; i++)
            copy3(rev[i], pts[n - 1 - i]);
        sp_by_distance(&o->sp, rev, n);
    } else
        sp_by_distance(&o->sp, pts, n);
}

static void heli_init(Boss *b, Obj *o)               /* Construct + onInit */
{
    HeliDef *h = &o->s.heli;
    h->hp = (int)unum(o, "HitPoints", 1);
    o->oflags |= OF_EFF_NODES | OF_ANIMATE;
    o->p.animate = 1;
    h->pulse_rate = 0.25f;
    o->pulse_delay = 0.25f;
    h->max_vel = unum(o, "Velocity", 20);
    h->vel = h->max_vel;
    h->flight_state = -1;
    h->vel_state = 3;
    h->monster = -1;
    sp_clear(&o->sp);                                /* its mover, taken on at the first pulse */
    o->sp.vel = 60;
    o->sp.flags = 2;
    set_target(o, -1);
    float pos[3] = { 75, -15, 25 };
    if (frand(b) < 0.5f)
        pos[0] = -pos[0];
    if (frand(b) < 0.5f)
        pos[1] = 25;
    copy3(o->p.pos, pos);
    weapon_ammo(o, 0, 0);
    weapon_ammo(o, 0, 1);
    h->engine_l = fx_on(b, o, "Scumhawk_Engine", NULL, "Attach_Rotor_left", -1, 1);
    h->engine_r = fx_on(b, o, "Scumhawk_Engine", NULL, "Attach_Rotor_Right", -1, 1);
    camera_rmh(b, 35);
}

static void heli_flee_path(Obj *o)          /* createFleePath: up, round and down behind the city */
{
    float pts[6][3] = { { 0, 0, 0 }, { 100, 0, 75 }, { 0, -40, 75 }, { -34, 10, 24 }, { -10, 76, 54 }, { -4, 165, 0 } };
    copy3(pts[0], o->p.pos);
    if (pts[0][0] < 0)
        pts[1][0] = -pts[1][0];
    sp_clear(&o->sp);
    o->sp.vel = 25;
    sp_by_distance(&o->sp, pts, 6);
    o->sp.flags |= 2;
}

static int heli_pulse(Boss *b, Obj *o, float frame_dt)   /* onPulse */
{
    HeliDef *h = &o->s.heli;
    float dt = h->pulse_rate > 0 ? h->pulse_rate : frame_dt;
    if (h->flight_state == -1) {
        o->mover = MV_SPLINE;
        heli_flight_path(b, o);
    }
    if (h->knocked) {                                /* tumbling: wobbles and spins two turns a second */
        h->osc += dt;
        o->p.facing[0] = sinf(h->osc * 3) * 0.1f;
        o->p.facing[1] = cosf(h->osc * 4) * 0.1f;
        o->p.facing[2] = PI + h->osc * 4 * PI;
        if (!h->fly_away)
            return 1;
    }
    if (h->fly_away) {
        if (h->fly_away < 2) {
            h->fly_away++;
            h->state_time -= dt;
            if (h->state_time <= 0) {
                h->state_time = 0.35f;
                fx_on(b, o, ustr(o, "ExplodeEffect"), NULL, NULL, -1, 0);
            }
        }
        return 1;
    }
    h->firing_delay -= dt;
    if (h->vel_state == 4) {                         /* braking at 40 / s^2 to a hover */
        h->vel -= dt * 40;
        if (h->vel <= 0) {
            h->vel_state = 3;
            h->vel = 0;
        }
        o->sp.vel = h->vel;
    } else if (h->vel_state == 5) {                  /* and away again */
        h->vel += dt * 40;
        if (h->vel >= h->max_vel) {
            h->vel_state = 3;
            h->vel = h->max_vel;
        }
        o->sp.vel = h->vel;
    }
    if (h->firing_delay <= 0 && h->flight_state == 1) {
        h->flight_state = 2;
        h->vel_state = 5;
        set_target(o, -1);
    }
    if (h->flight_state == 0) {                      /* a monster within 50: it stops and fires its missiles */
        for (int i = 0; i < b->nm; i++)
            if (dist3(b->m[i].pos, o->p.pos) < 50) {
                int most = (int)(frand(b) * unum(o, "MaxMissiles", 2));
                set_target(o, i);
                weapon_ammo(o, 1 + most, 0);
                weapon_ammo(o, 1 + (frand(b) > 0.5f), 1);
                h->firing_delay = unum(o, "FiringDelay", 2);
                h->flight_state = 1;
                h->vel_state = 4;
                return 1;
            }
    }
    if (h->destroyed) {                              /* burning on its way out: fire at the rotors every 3 s */
        float since = b->now - h->last_fx;
        if (since > 3 || h->last_fx == 0) {
            h->last_fx = b->now;
            const char *fx = ustr(o, "ExplodeEffect");
            fx_on(b, o, fx, NULL, "Attach_Rotor_Right", -1, 1);
            fx_on(b, o, fx, NULL, "Attach_Rotor_Left", -1, 1);
        }
    }
    return 1;
}

static int heli_mover(Boss *b, Obj *o, int state)   /* onMoverCallback */
{
    HeliDef *h = &o->s.heli;
    if (state != 2)
        return 1;
    if (h->fly_away) {                               /* beaten: off behind the city */
        h->knocked = 0;
        h->fly_away = 0;
        h->destroyed = 1;
        heli_flee_path(o);
    } else if (h->destroyed)
        return DELETE_ME;
    else {                                           /* the end of a path or of a knock: another path */
        h->pulse_rate = 0.25f;
        o->pulse_delay = 0.25f;
        h->knocked = 0;
        h->flight_state = 0;
        heli_flight_path(b, o);
    }
    return 1;
}

static int heli_damage(Boss *b, Obj *o, int monster)   /* TakeDamage: any blow is one of its HitPoints */
{
    HeliDef *h = &o->s.heli;
    if (h->knocked || h->hp <= 0 || !m_valid(b, monster))
        return 1;
    h->pulse_rate = 0;                               /* setPulseDelay(timeStep): every frame */
    o->pulse_delay = 0;
    h->knocked = 1;
    h->vel_state = 3;
    h->vel = h->max_vel;
    h->hp--;
    challenge_hud(b, o, (float)h->hp / unum(o, "HitPoints", 1));
    set_target(o, -1);
    weapon_ammo(o, 0, 0);
    weapon_ammo(o, 0, 1);
    float pts[8][3];
    copy3(pts[0], o->p.pos);                         /* createKnockedAwayPath: 7 steps at its Velocity */
    knock_points(b, o->p.pos, b->m[monster].pos, 9, 7, 1, 0, pts + 1);
    sp_clear(&o->sp);
    o->sp.flags &= ~2u;
    o->sp.vel = h->vel;
    sp_by_distance(&o->sp, pts, 8);
    const char *e = h->hp > 0 ? ustr(o, "HitEffect4") : ustr(o, "ExplodeEffect");
    if (e) {
        fx_on(b, o, e, NULL, "Attach_Rotor_Right", -1, 1);
        fx_on(b, o, e, NULL, "Attach_Rotor_left", -1, 1);
    }
    fx_on(b, o, ustr(o, "HitEffect0"), NULL, NULL, -1, 0);
    fx_on(b, o, ustr(o, "HitEffect1"), NULL, NULL, -1, 0);
    if (h->hp == 0) {
        h->state_time = 0.35f;
        h->fly_away = 1;
        add_stats(b, o, monster, 2);
        const char *cb = ustr(o, "CompletionCallback");
        completion(b, o, cb ? cb : "Error-Function Not Found!");
        sfx(b, o, "V_Boss_Defeated");
    } else
        sfx(b, o, "V_Boss_Damaged");
    return 1;
}

/* ---------------------------------------------------------------- Boss_Jumper.bsf (an Actor) */

/* its flags (nFlags): 1 drop a bomb, 2 knocked by a blow, 4 [R: never set, see jumper_flee], 8 landed, 16 coming
 * down onto the roof, 32 in a jump, 64 the next jump goes anywhere, 128 leaving, 256 gone off screen */

static int path_hits_building(const Boss *b, const float start[3], const float end[3])   /* pathIntersectsBuilding */
{
    float d[3], dir[3];
    sub3(d, end, start);
    copy3(dir, d);
    norm3(dir);
    float len = len3(d);
    for (float step = 0.02f; step < len; step += 3) {
        float p[3] = { start[0] + dir[0] * step, start[1] + dir[1] * step, start[2] + dir[2] * step };
        int bi = bld_by_pos(b, p);
        if (bi >= 0 && p[2] <= ROOF(b->bl[bi].storeys))
            return 1;
    }
    return 0;
}

/* the jump's five points: from pos, 9 up at 5 % of the way (a slow start: 0.05), over the arc, 9 above the end at
 * 96 % (slow again), down onto end (createJumpPath, createLeavePath) */
static void jumper_arc(Boss *b, Obj *o, const float pos[3], const float arc[3], const float end[3])
{
    float dif[3], pos2[3], end2[3];
    sub3(dif, end, pos);
    set3(pos2, pos[0] + dif[0] * 0.05f, pos[1] + dif[1] * 0.05f, pos[2] + 9);
    set3(end2, pos[0] + dif[0] * 0.96f, pos[1] + dif[1] * 0.96f, end[2] + 9);
    float s1 = dist3(pos2, pos), s2 = s1 + dist3(arc, pos2), s3 = s2 + dist3(end2, arc), s4 = s3 + dist3(end, end2);
    o->sp.flags &= ~4u;
    sp_clear(&o->sp);
    o->sp.vel = unum(o, "JumpVelocity", 20);
    sp_add(&o->sp, pos, 0, 0.05f);
    sp_add(&o->sp, pos2, s1, 0);
    sp_add(&o->sp, arc, s2, 0);
    sp_add(&o->sp, end2, s3, 0.05f);
    sp_add(&o->sp, end, s4, 0);
    o->s.jumper.flags &= ~(8 | 16);
    o->s.jumper.flags |= 32;
    sfx(b, o, "Leeper_Servo");
}

static void jumper_jump_path(Boss *b, Obj *o)        /* createJumpPath: onto a roof near the monsters */
{
    JumperDef *j = &o->s.jumper;
    int bi = -1;
    for (int i = 0; !(j->flags & 64) && bi < 0 && i < 3; i++) {
        int tb = bld_near_monster(b, -5);
        if (tb != j->building)
            bi = tb;
    }
    for (int i = 0; !(j->flags & 64) && bi < 0 && i < 3; i++) {
        int tb = bld_near_monster(b, 15);
        if (tb != j->building)
            bi = tb;
    }
    if (bi < 0)
        bi = bld_random(b);
    j->flags &= ~64;
    float pos[3], arc[3], end[3];
    copy3(pos, o->p.pos);
    if (bi >= 0) {
        j->building = bi;
        b_pos(b, bi, end);
        int cells = b->bl[bi].width;
        if (cells >= 3) {                            /* somewhere along a wide roof */
            float off = frand(b) * (float)(cells - 2) * 5 / 2;
            if (frand(b) < 0.5f)
                off = -off;
            end[0] += off;
        }
        end[2] = ROOF(b->bl[bi].storeys);
        set3(arc, (pos[0] + end[0]) / 2, (pos[1] + end[1]) / 2, pos[2] + end[2]);
        arc[2] = (pos[2] < end[2] ? end[2] : pos[2]) + sinf(PI / 3) * 25;
        for (int check = 15; check > 0 && path_hits_building(b, pos, arc); check--)
            arc[2] += 3;
    } else {                                         /* no building: the street */
        j->building = -1;
        set3(end, -45 + frand(b) * 90, 0, 0);
        end[1] = frand(b) * 15;
        if (dist3(end, pos) < 25)
            end[0] = -end[0];
        set3(arc, (pos[0] + end[0]) / 2, (pos[1] + end[1]) / 2, pos[2] + end[2]);
        if (arc[2] < 12)
            arc[2] = 12;
    }
    jumper_arc(b, o, pos, arc, end);
}

static void jumper_leave_path(Boss *b, Obj *o)       /* createLeavePath: up and out over the camera */
{
    float pos[3], arc[3], end[3] = { 0, -30, 75 };
    o->s.jumper.building = -1;
    copy3(pos, o->p.pos);
    set3(arc, (pos[0] + end[0]) / 2, (pos[1] + end[1]) / 2, pos[2] + end[2]);
    jumper_arc(b, o, pos, arc, end);
}

static void jumper_init(Boss *b, Obj *o)             /* Construct (which calls onInit) */
{
    static const char *const ADORN[3] = { "HKPalaceTopAdorn1R", "HKPalaceTopAdorn2", "HKPalaceTopAdorn1L" };
    JumperDef *j = &o->s.jumper;
    o->oflags |= OF_PUSH_OUT | OF_EFF_NODES;
    j->max_hp = unum(o, "HitPoints", 51);
    j->hp = j->max_hp;
    j->leave_delay = unum(o, "LeaveDelay", 20);
    j->building = -1;
    j->damage_fx = -1;
    float pos[3] = { -100, -10, 0 };
    upoint(o, "Location", pos);                      /* HK10's AIInstance: (-50, -10, 0) */
    copy3(o->p.pos, pos);
    o->p.facing[2] = PI;
    o->mover = MV_SPLINE;
    sp_clear(&o->sp);
    o->sp.vel = unum(o, "JumpVelocity", 20);
    j->effect = fx_on(b, o, ustr(o, "AttachEffect"), NULL, NULL, -1, 0);
    camera_rmh(b, 35);
    jumper_jump_path(b, o);
    for (int k = 0; k < 3; k++) {                    /* the palace's roof ornaments it breaks by landing on them */
        j->adorn[k].obj = -1;
        j->adorn[k].radius = 3;
        for (int w = 0; w < b->nwobj; w++)
            if (!b->wobj[w].gone && same_text(b->wobj[w].name, ADORN[k])) {
                j->adorn[k].obj = w;
                copy3(j->adorn[k].pos, b->wobj[w].pos);
                j->adorn[k].radius = group_num(b->wobj[w].tmpl, "HitRadius", 3);
                break;
            }
    }
    sfx(b, o, "Leeper_Hum_Loop");
}

static int jumper_process(Boss *b, Obj *o, float dt)   /* Process, every frame */
{
    JumperDef *j = &o->s.jumper;
    if (j->destroyed && j->destroyed < 2) {
        j->destroyed++;
        j->state_time -= dt;
        if (j->state_time <= 0) {
            j->state_time = 0.35f;
            fx_on(b, o, ustr(o, "HitEffect"), NULL, NULL, -1, 0);
        }
    }
    j->air_damage -= dt;
    j->leave_delay -= dt;
    if (j->flags & 2) {                              /* knocked: wobbling and spinning */
        j->hit_osc += dt;
        o->p.facing[0] = sinf(j->hit_osc * 3) * 0.1f;
        o->p.facing[1] = cosf(j->hit_osc * 4) * 0.1f;
        o->p.facing[2] = PI + j->hit_osc * 4 * PI;
    }
    if (j->flags & (128 | 256)) {
        if ((j->flags & 256) && j->leave_delay <= 0 && !b->city_destroyed) {   /* back after ReturnDuration */
            j->leave_delay = unum(o, "LeaveDelay", 20);
            j->flags &= ~(128 | 256);
            jumper_jump_path(b, o);
        }
        if (j->flags & 256)
            return 1;
    } else if (j->flags & 16) {                      /* settling onto the roof at 24 a second */
        int done = 1;
        float pos[3];
        copy3(pos, o->p.pos);
        if (b_ok(b, j->building)) {
            float ht = b->bl[j->building].height;
            if (pos[2] > ht) {
                done = 0;
                pos[2] -= 24 * dt;
            }
        } else if (pos[2] > 0) {
            pos[2] -= 24 * dt;
            pos[2] = fmaxf(0, pos[2]);
        }
        copy3(o->p.pos, pos);
        if (done) {
            j->flags &= ~(16 | 32);
            j->flags |= 8 | 1;
            start_anim(b, o, "Land");
            j->jump_delay = 2;
            sfx(b, o, "Leeper_Servo");
            sfx(b, o, "Tim_Impact_Test");
        }
    } else if (b_ok(b, j->building) && b->bl[j->building].falling && j->jump_delay > 0) {
        j->jump_delay = 0.001f;                      /* its roof is falling: off at once */
        j->flags &= ~32;
    }
    if (!(j->flags & (2 | 32))) {
        j->leave_delay -= dt;                        /* [R] a second time this frame */
        if (j->leave_delay <= 0 && !(j->flags & 128)) {
            j->leave_delay = unum(o, "ReturnDuration", 20);
            j->flags |= 128;
            j->jump_delay = 0.001f;
        }
        if (j->jump_delay > 0) {
            j->jump_delay -= dt;
            if (j->jump_delay <= 0) {
                start_anim(b, o, "Jump");
                j->pre_jump = 0.25f;
                sfx(b, o, "Leeper_Jump");
            }
        } else if (j->pre_jump > 0) {
            j->pre_jump -= dt;
            if (j->pre_jump <= 0) {                  /* off: steam from its feet, a kick to monsters near */
                j->air_damage = unum(o, "JumpDamageDuration", 1);
                if (j->flags & 128)
                    jumper_leave_path(b, o);
                else
                    jumper_jump_path(b, o);
                const char *e = ustr(o, "JumpEffect");
                if (e)
                    for (int i = 0; i < 3; i++)
                        fx_on(b, o, e, NULL, NULL, 2 + i * 3, 0);
                for (int i = 0; i < b->nm; i++)
                    if (dist3(b->m[i].pos, o->p.pos) < 8 && contains(m_move(b, i), "Ground")) {
                        float v[3] = { 0, -20, 20 };
                        m_vel(b, i, v);
                        m_reaction(b, i, "React_Airborne");
                        m_hp(b, i, -unum(o, "Damage", 20));
                    }
            }
        }
    }
    float pos[3];
    copy3(pos, o->p.pos);
    if (j->flags & 1) {                              /* landed: a burning goo bomb rolls off its feet */
        j->flags &= ~1;
        pos[2] += 1.5f;
        Obj *so = obj_make(b, "BossJumperBomb", pos);
        sfx(b, o, "V_Boss_Taunt");
        if (so) {
            obj_init(b, so);
            if (!b_ok(b, j->building))
                so->s.jbomb.delay = 2;               /* setDamageDelay(2) on the street */
        }
    }
    for (int i = 0; i < 3; i++) {
        int w = j->adorn[i].obj;
        if (w >= 0 && !b->wobj[w].gone && dist3(pos, j->adorn[i].pos) <= j->adorn[i].radius) {
            emit(b, BE_FORCE_DAMAGE, NULL, b->wobj[w].name, b->wobj[w].pos);
            b->wobj[w].gone = 1;
            j->adorn[i].obj = -1;
        }
    }
    return 1;
}

static void jumper_flee(Obj *o)             /* doFleeReaction: through the AIInstance's FleePoint<n> */
{
    JumperDef *j = &o->s.jumper;
    j->flags &= 4;                                   /* [R] "& 4", not "| 4": the flag that deletes it is never set */
    float pts[MAX_SP][3];
    int n = 1;
    copy3(pts[0], o->p.pos);
    o->sp.flags &= ~4u;
    sp_clear(&o->sp);
    o->sp.vel = unum(o, "JumpVelocity", 20) * 1.5f;
    for (int i = 1; n < MAX_SP; i++) {
        char key[32];
        snprintf(key, sizeof key, "FleePoint%d", i);
        if (!upoint(o, key, pts[n]))
            break;
        n++;
    }
    sp_by_distance(&o->sp, pts, n);
}

static int jumper_mover(Boss *b, Obj *o, int state)   /* onMoverCallback */
{
    JumperDef *j = &o->s.jumper;
    if (state != 2)
        return 1;
    if (j->flags & 4) {
        fx_stop(b, j->effect);
        fx_stop(b, j->damage_fx);
        return DELETE_ME;
    } else if (j->destroyed)
        jumper_flee(o);
    else if (j->flags & 2) {                         /* knocked: then off to any roof */
        j->flags |= 64;
        jumper_jump_path(b, o);
        j->flags &= ~2;
        j->jump_delay = 0;
        set3(o->p.facing, 0, 0, PI);
    } else if (!(j->flags & 256) && (j->flags & 128))
        j->flags |= 256;
    else if (!(j->flags & 8) && j->jump_delay <= 0) {   /* the end of a jump: the mover holds, it settles */
        o->sp.flags |= 4;
        j->flags |= 16;
    }
    return 1;
}

static void jumper_anim_done(Boss *b, Obj *o)        /* onAnimComplete */
{
    if (same_text(o->p.anim, "Land")) {
        sfx(b, o, "Leeper_Servo");
        start_anim(b, o, "Idle");
    }
}

static int jumper_damage(Boss *b, Obj *o, int monster, float amount, int type)   /* TakeDamage */
{
    JumperDef *j = &o->s.jumper;
    if (!m_valid(b, monster) || type != DT_CONCUSSIVE || (j->flags & 2))
        return 1;
    j->hp -= amount;
    /* doHitReaction: knocked away slowly (2.25 points a second) up to 15 steps, until 60 above */
    float pts[16][3];
    j->flags |= 2;
    j->hit_osc = 0;
    copy3(pts[0], o->p.pos);
    int n = 1 + knock_points(b, o->p.pos, b->m[monster].pos, 9, 15, 0, 60, pts + 1);
    o->sp.flags &= ~4u;
    sp_clear(&o->sp);
    o->sp.vel = 2.25f;
    for (int i = 0; i < n; i++)
        sp_add(&o->sp, pts[i], (float)i, 0);
    challenge_hud(b, o, j->hp / j->max_hp);
    fx_on(b, o, ustr(o, "HitEffect"), NULL, NULL, -1, 0);
    float hf = j->hp / j->max_hp;
    int he = hf > 0.5f ? 2 : hf > 0.15f ? 1 : 0;
    char key[32];
    const char *e = ustr(o, numbered(key, sizeof key, "HitEffect", he));
    if (e) {
        fx_stop(b, j->damage_fx);
        j->damage_fx = fx_on(b, o, e, NULL, NULL, -1, 0);
    }
    sfx(b, o, j->hp <= 0 ? "V_Boss_Defeated" : "V_Boss_Damaged");
    if (j->hp <= 0 && !j->destroyed) {
        j->state_time = 0.35f;
        j->destroyed = 1;
        add_stats(b, o, monster, 2);
        const char *cb = ustr(o, "CompletionCallback");
        completion(b, o, cb ? cb : "Error-Function Not Found!");
    }
    return 1;
}

/* ---------------------------------------------------------------- Boss_Jumper_Bomb.bsf */

static void jbomb_init(Boss *b, Obj *o)              /* Construct + onInit: rolls off the roof towards the camera */
{
    JumperBombDef *jb = &o->s.jbomb;
    jb->allow = 1;
    jb->delay = unum(o, "DamageDelay", 1);
    o->pulse_delay = 0.25f;
    jb->effect = fx_on(b, o, ustr(o, "AttachEffect"), NULL, NULL, -1, 0);
    o->mover = MV_SPLINE;
    sp_clear(&o->sp);
    o->sp.vel = unum(o, "Velocity", 10);
    float pos[3], xit[3], end[3], out[3];
    copy3(pos, o->p.pos);
    pos[2] -= 2;
    copy3(xit, pos);
    xit[1] -= 9;
    copy3(end, xit);
    end[2] = 1;
    int bi = bld_by_pos(b, xit);
    if (bi >= 0) {                                   /* 9 out is still over a building: onto its roof, or further */
        float bh = ROOF(b->bl[bi].storeys);
        if (pos[2] - bh < 6) {
            xit[1] -= 10;
            end[1] = xit[1];
        } else
            end[2] = bh;
    }
    copy3(out, end);
    out[1] = -20;
    float s1 = dist3(xit, pos), s2 = s1 + dist3(end, xit), s3 = s2 + dist3(out, end);
    sp_add(&o->sp, pos, 0, 0);
    sp_add(&o->sp, xit, s1, 0);
    sp_add(&o->sp, end, s2, 0);
    sp_add(&o->sp, out, s3, 0);
}

static int jbomb_mover(Boss *b, Obj *o, int state)  /* onMoverCallback: at the end of its roll it bursts */
{
    if (state != 2)
        return 1;
    fx_at(b, ustr(o, "ImpactEffect"), o->p.pos);
    fx_stop(b, o->s.jbomb.effect);
    return DELETE_ME;
}

static int jbomb_damage(Boss *b, Obj *o, int monster)   /* TakeDamage: a monster in it is blown out, once */
{
    JumperBombDef *jb = &o->s.jbomb;
    if (!m_valid(b, monster))
        return 1;
    const char *mv = m_move(b, monster);
    if (jb->allow && jb->delay <= 0 && !contains(mv, "Jump")) {
        if (contains(mv, "Side"))
            return 1;
        if (contains(mv, "Front") && o->p.pos[2] > b->m[monster].pos[2] + 3)
            return 1;
        jb->allow = 0;
        float v[3] = { 0, -20, 5 };
        m_vel(b, monster, v);
        m_reaction(b, monster, "Jump_Air_Borne_Fall");
        m_hp(b, monster, -unum(o, "Damage", 50));
    }
    return 1;
}

/* ---------------------------------------------------------------- Boss_Chicago_Saucer.bsf */

/* its flags (nFlags): 1 shield up, 2 at the end of a path (charging, firing), 4 knocked by a blow, 8 beaten and
 * fleeing, 16 leaving, 64 gone off screen */

static void saucer_shield_on(Boss *b, Obj *o)        /* startShield */
{
    SaucerDef *s = &o->s.saucer;
    const char *e = ustr(o, "ShieldEffect");
    if (e && s->shield_fx < 0)
        s->shield_fx = fx_on(b, o, e, NULL, NULL, -1, 1);
    s->flags |= 1;
}
static void saucer_shield_off(Boss *b, Obj *o)       /* stopShield */
{
    SaucerDef *s = &o->s.saucer;
    fx_stop(b, s->shield_fx);
    s->shield_fx = -1;
    s->flags &= ~1;
}

static void saucer_heights(Boss *b, Obj *o, int n)   /* checkPathHeights: 6 .. 12 over the buildings near each point */
{
    SaucerDef *s = &o->s.saucer;
    float mp[3];
    m_pos(b, s->target, mp);
    for (int i = 1; i < n; i++) {
        int bi = bld_near_pos(b, s->pts[i], 12.5f);
        if (bi >= 0) {
            float ht = ROOF(b->bl[bi].storeys);
            if (s->pts[i][2] < ht + 6)
                s->pts[i][2] = ht + 6 + frand(b) * 6;
            else if (s->pts[i][2] < mp[2] + 10)
                s->pts[i][2] = mp[2] + 10 + frand(b) * 6;
        }
    }
}
static void saucer_path(Obj *o, const int *order, const float *at, int n)
{
    sp_clear(&o->sp);
    for (int i = 0; i < n; i++)
        sp_add(&o->sp, o->s.saucer.pts[order[i]], at[i], 0);
}

static void saucer_new_path(Boss *b, Obj *o)         /* createNewPath: round to where a random monster is */
{
    SaucerDef *s = &o->s.saucer;
    s->target = any_monster(b);
    float mp[3], sp[3];
    m_pos(b, s->target, mp);
    copy3(sp, o->p.pos);
    int mside = (mp[0] > 15) - (mp[0] < -15), sside = (sp[0] > 15) - (sp[0] < -15), sfront = sp[1] < -2.5f;
    o->sp.vel = s->vel;
    float z = mp[2];
    copy3(s->pts[0], sp);
    static const int FWD[4] = { 0, 1, 2, 3 }, BACK[4] = { 0, 3, 2, 1 };
    static const float AT4[4] = { 0, 0.25f, 0.5f, 1 }, AT3[3] = { 0, 0.5f, 1 };
    if (mside == sside) {                            /* createSidePath: about the side the monster is on */
        s->pts[1][0] = -32.5f + 7.5f - frand(b) * 15;
        s->pts[1][1] = 0 + 5 - frand(b) * 20;
        s->pts[1][2] = z + 10 + frand(b) * 6;
        s->pts[2][0] = -32.5f + 7.5f - frand(b) * 15;
        s->pts[2][1] = 15 + 7.5f - frand(b) * 12.5f;
        s->pts[2][2] = z + 10 + frand(b) * 6;
        s->pts[3][0] = -50 + 7.5f - frand(b) * 15;
        s->pts[3][1] = 10 + 12.5f - frand(b) * 35;
        s->pts[3][2] = z + 10 + frand(b) * 6;
        if (mp[0] > 15)
            for (int i = 1; i <= 3; i++)
                s->pts[i][0] = -s->pts[i][0];
        saucer_heights(b, o, 4);
        saucer_path(o, frand(b) < 0.5f ? FWD : BACK, AT4, 4);
    } else if (!mside && sfront) {                   /* createFrontPath: across the front */
        s->pts[1][0] = 20 + frand(b) * 15;
        s->pts[1][1] = -2.5f - frand(b) * 10;
        s->pts[1][2] = z + 10 + frand(b) * 6;
        s->pts[2][0] = -10 + frand(b) * 20;
        s->pts[2][1] = -2.5f - frand(b) * 10;
        s->pts[2][2] = z + 10 + frand(b) * 6;
        s->pts[3][0] = -20 - frand(b) * 15;
        s->pts[3][1] = -2.5f - frand(b) * 10;
        s->pts[3][2] = z + 10 + frand(b) * 6;
        if (frand(b) < 0.5f)
            for (int i = 1; i <= 3; i++)
                s->pts[i][0] = -s->pts[i][0];
        saucer_heights(b, o, 4);
        saucer_path(o, FWD, AT4, 4);
    } else if (mside && sfront) {                    /* createFrontToSidePath */
        s->pts[1][0] = 35 - frand(b) * 10;
        s->pts[1][1] = -2.5f - frand(b) * 10;
        s->pts[1][2] = z + 10 + frand(b) * 6;
        s->pts[2][0] = 45 - frand(b) * 15;
        s->pts[2][1] = 10.5f - frand(b) * 15;
        s->pts[2][2] = z + 10 + frand(b) * 6;
        if (mp[0] < -15)
            for (int i = 1; i <= 2; i++)
                s->pts[i][0] = -s->pts[i][0];
        saucer_heights(b, o, 3);
        saucer_path(o, FWD, AT3, 3);
    } else if ((!mside && !sfront) || mside == -sside) {   /* createSideToFrontPath */
        s->pts[1][0] = 15 + frand(b) * 10;
        s->pts[1][1] = -2.5f - frand(b) * 10;
        s->pts[1][2] = z + 10 + frand(b) * 6;
        s->pts[2][0] = 10 - frand(b) * 15;
        s->pts[2][1] = -2.5f - frand(b) * 10;
        s->pts[2][2] = z + 10 + frand(b) * 6;
        if (s->pts[0][0] < -15)
            for (int i = 1; i <= 2; i++)
                s->pts[i][0] = -s->pts[i][0];
        saucer_heights(b, o, 3);
        saucer_path(o, FWD, AT3, 3);
    } else {                                         /* createGoToFrontPath (the script cannot reach it) */
        set3(s->pts[1], 30, -10, z + 10 + frand(b) * 6);
        set3(s->pts[2], 0, -10, z + 10 + frand(b) * 6);
        saucer_heights(b, o, 3);
        saucer_path(o, FWD, AT3, 3);
    }
}

static void saucer_init(Boss *b, Obj *o)             /* Construct + onInit */
{
    SaucerDef *s = &o->s.saucer;
    o->oflags |= OF_EFF_NODES;
    s->osc_scale = 1;
    s->start_hp = unum(o, "HitPoints", 51);
    s->hp = s->start_hp;
    s->vel = 0.125f;
    s->shield_delay = 2;
    s->flags = 1;
    s->ww_delay = unum(o, "WhirlwindLaunchDelay", 3);
    s->ww_chance = unum(o, "WhirlwindChance", 0.33f);
    s->leave_delay = unum(o, "LeaveDelay", 20);
    s->blades = s->damage_fx = s->shield_fx = s->charge_fx = s->target = -1;
    const char *att = ustr(o, "Attachment");         /* its rotor, a part of its own */
    if (att) {
        Obj *bl = obj_make(b, att, o->p.pos);
        if (bl) {
            bl->p.owner = idx(b, o);
            bl->oflags |= 8;
            s->blades = idx(b, bl);
        }
    }
    sfx(b, o, "Cyclo_Hum_Loop");
    float start[3] = { -50, -5, 22 }, end[3] = { 50, -5, 22 };   /* in across the front: 8 s */
    o->mover = MV_SPLINE;
    sp_clear(&o->sp);
    sp_add(&o->sp, start, 0, 0);
    sp_add(&o->sp, end, 1, 0);
    o->sp.vel = s->vel;
    copy3(o->p.pos, start);
    weapons_enable(o, 1);
    weapon_ammo(o, 0, 0);
    weapon_ammo(o, 0, 1);
    weapon_ammo(o, 0, 2);
    saucer_shield_on(b, o);
    camera_rmh(b, 35);
}

static int saucer_process(Boss *b, Obj *o, float dt)   /* Process, every frame */
{
    SaucerDef *s = &o->s.saucer;
    s->osc += dt;
    float occ[3] = { sinf(s->osc * 2) * 0.0625f * s->osc_scale, cosf(s->osc * 3) * 0.0625f * s->osc_scale, 0 };
    copy3(o->p.facing, occ);
    Obj *bl = obj_at(b, s->blades);
    if (bl) {                                        /* the rotor: 1.5 turns a second, faster while it charges */
        if (!(s->flags & 2))
            s->blade_scale = fmaxf(s->blade_scale - dt * 1.25f, 0);
        s->blade_rot = fmodf(s->blade_rot + PI * 3 * dt * (1 + s->blade_scale), 2 * PI);
        copy3(bl->p.pos, o->p.pos);
        float fz = o->p.facing[2] + s->blade_rot;
        copy3(bl->p.facing, occ);
        bl->p.facing[2] += fz;                       /* adjustZRot [G: adds to its turn] */
    }
    if (s->fly_away && s->fly_away < 2) {
        s->fly_away++;
        s->state_time -= dt;
        if (s->state_time <= 0) {
            s->state_time = 0.35f;
            fx_on(b, o, ustr(o, "HitEffect"), NULL, NULL, -1, 0);
        }
    }
    if (s->flags & (16 | 64)) {                      /* leaving, or gone: back after ReturnDuration */
        if (s->flags & 64) {
            s->leave_delay -= dt;
            if (!b->city_destroyed && s->leave_delay <= 0) {
                s->leave_delay = unum(o, "LeaveDelay", 20);
                s->flags &= ~(16 | 64);
                saucer_new_path(b, o);
            }
        }
        return 1;
    }
    s->leave_delay -= dt;
    if (s->leave_delay <= 0 && (s->flags & 2) && !(s->flags & 8) && !s->fly_away) {   /* LeaveDelay up: away */
        s->flags |= 16;
        s->flags &= ~2;
        saucer_shield_off(b, o);
        o->sp.vel = 25;
        float pts[3][3];
        copy3(pts[0], o->p.pos);
        set3(pts[1], o->p.pos[0], o->p.pos[1] - 6, o->p.pos[2] + 10);
        set3(pts[2], 0, -40, 100);
        sp_clear(&o->sp);
        sp_by_distance(&o->sp, pts, 3);
        return 1;
    }
    if ((s->flags & 2) && !s->fly_away) {            /* at the end of a path */
        if (s->flags & 4) {                          /* back from a knock */
            saucer_new_path(b, o);
            s->flags &= ~(2 | 4);
            s->osc_scale = 1;
        } else if (s->state_time == s->ww_delay) {   /* arrived: the shield drops, it charges */
            saucer_shield_off(b, o);
            sfx(b, o, "V_Boss_Taunt");
            sfx(b, o, "cyclo_chargeup");
            const char *e = ustr(o, "ChargeUpEffect");
            if (e)
                s->charge_fx = fx_on(b, o, e, NULL, NULL, -1, 0);
            s->fire_mode = 1 + (frand(b) > s->ww_chance);   /* 1 a whirlwind, 2 lightning */
            if (s->fire_mode == 2) {
                s->state_time /= 2;
                if (!m_valid(b, s->target))
                    s->target = 0;
                float p[3];
                m_pos(b, s->target, p);
                p[2] += 6;
                set_target_point(o, p);
            }
        } else if (s->state_time <= 0) {
            if (s->fire_mode == 1) {
                float wp[3];
                copy3(wp, o->p.pos);
                wp[2] = fmaxf(12, wp[2] - 6);
                Obj *so = obj_make(b, "BossSaucerWhirlwind", wp);
                if (so)
                    obj_init(b, so);
                s->fire_mode = 0;
                s->state_time = s->ww_delay / 2;
            } else if (s->fire_mode == 2) {          /* one bolt, 60 % a second, 60 % of those a third */
                s->fire_mode = 0;
                s->state_time = s->ww_delay / 2;
                weapon_ammo(o, 1, 0);
                if (frand(b) < 0.6f) {
                    weapon_ammo(o, 1, 1);
                    if (frand(b) < 0.6f)
                        weapon_ammo(o, 1, 2);
                }
            } else if (!b->city_destroyed) {         /* done: shield up, off to the next place */
                saucer_new_path(b, o);
                s->flags &= ~2;
                saucer_shield_on(b, o);
            }
        } else if (s->state_time > 0)
            s->blade_scale += dt * 1.25f;
        s->state_time -= dt;
    }
    s->shield_delay -= dt;
    return 1;
}

static int saucer_mover(Boss *b, Obj *o, int state)   /* onMoverCallback */
{
    SaucerDef *s = &o->s.saucer;
    if ((s->flags & 2) || state != 2)
        return 1;
    if (s->flags & 16) {
        if (!(s->flags & 64)) {
            s->leave_delay = unum(o, "ReturnDuration", 20);
            s->flags |= 64;
        }
    } else if (s->fly_away) {                        /* beaten: the flee path behind the city */
        float pts[6][3] = { { 0, 0, 0 }, { 100, 0, 75 }, { 0, -40, 75 }, { -34, 10, 24 }, { -10, 76, 54 },
                            { -4, 165, 0 } };
        s->fly_away = 0;
        s->flags |= 8;
        copy3(pts[0], o->p.pos);
        if (pts[0][0] < 0)
            pts[1][0] = -pts[1][0];
        sp_clear(&o->sp);
        o->sp.vel = 25;
        sp_by_distance(&o->sp, pts, 6);
    } else if (s->flags & 8) {
        obj_remove(b, obj_at(b, s->blades));
        return DELETE_ME;
    } else if (s->flags & 4) {
        s->flags |= 2;
        saucer_shield_on(b, o);
    } else {
        s->flags |= 2;
        s->state_time = s->ww_delay;
    }
    return 1;
}

static int saucer_damage(Boss *b, Obj *o, int monster, float amount)   /* TakeDamage */
{
    SaucerDef *s = &o->s.saucer;
    if (s->flags & 1) {                              /* the shield: sparks, at most every 2 s */
        if (s->shield_delay <= 0) {
            s->shield_delay = 2;
            float off[3] = { 0, 0, -3 };
            fx_on(b, o, ustr(o, "ShieldHitEffect"), off, NULL, -1, 0);
        }
        return 1;
    }
    if ((s->flags & 4) || !m_valid(b, monster))
        return 1;
    s->osc_scale = 20;
    s->flags |= 4;
    s->flags &= ~2;
    s->vel = fminf(s->vel + 0.125f * 0.5f, 0.6f);    /* each blow makes its next paths faster */
    float pts[16][3];                                /* createKnockedAwayPath: 15 steps, 2.5 a second */
    copy3(pts[0], o->p.pos);
    knock_points(b, o->p.pos, b->m[monster].pos, 6, 15, 0, 0, pts + 1);
    sp_clear(&o->sp);
    o->sp.vel = 2.5f;
    for (int i = 0; i < 16; i++)
        sp_add(&o->sp, pts[i], (float)i, 0);
    s->hp -= amount;
    challenge_hud(b, o, s->hp / s->start_hp);
    if (s->hp > 0)
        sfx(b, o, "V_Boss_Damaged");
    fx_stop(b, s->charge_fx);
    s->charge_fx = -1;
    fx_on(b, o, ustr(o, "HitEffect"), NULL, NULL, -1, 0);
    char key[32];
    const char *e = ustr(o, numbered(key, sizeof key, "HitEffect", (int)(s->hp / (s->start_hp / 3))));
    if (e)
        s->damage_fx = fx_on(b, o, e, NULL, NULL, -1, 0);
    if (s->hp <= 0 && !s->fly_away) {
        s->state_time = 0.35f;
        s->fly_away = 1;
        sfx(b, o, "V_Boss_Defeated");
        add_stats(b, o, monster, 2);
        const char *cb = ustr(o, "CompletionCallback");
        completion(b, o, cb ? cb : "Error: Saucer Completion callback not found!");
    }
    return 1;
}

/* ---------------------------------------------------------------- Boss_Chicago_Whirlwind.bsf */

static void wind_init(Boss *b, Obj *o)               /* Construct + onInit + createPath */
{
    WindDef *w = &o->s.wind;
    o->oflags |= OF_OBSTACLE | 8;
    o->pulse_delay = 0.125f;
    w->pulse = 0.125f;
    w->life = 6;
    w->fire_time = 2.5f;
    w->monster = -1;
    w->effect = fx_on(b, o, ustr(o, "AttachEffect"), NULL, NULL, -1, 1);
    sfx(b, o, "Cyclo_Wind_Loop");
    float pos[3], mp[3] = { 0, 0, 0 };
    copy3(pos, o->p.pos);
    int near = 0;
    for (int i = 0; i < b->nm; i++) {
        m_pos(b, i, mp);
        if (dist3(mp, pos) < 50)
            near++;
    }
    if (near == b->nm)
        (void)any_monster(b);                        /* [R] the monster it picks is not used: */
    float dir[3];                                    /* it heads for the last monster's place (mpos) */
    sub3(dir, mp, pos);
    norm3(dir);
    dir[2] = 0;
    scale3(dir, 5);
    if (len3(dir) < 7.5f)
        scale3(dir, 2);
    o->mover = MV_SPLINE;
    sp_clear(&o->sp);
    o->sp.vel = 1;
    sp_add(&o->sp, pos, 0, 0);
    for (int i = 0; i < 5; i++) {                    /* five steps of about 10 down towards it, 1.5 s each */
        if (i == 0)
            pos[2] -= 6;
        else if (mp[2] + 12 < pos[2]) {
            pos[2] -= 4;
            if (pos[2] < 12)
                pos[2] = 12;
        }
        float pt[3], q[3];
        copy3(pt, dir);
        for (int k = 0; k < 3; k++)
            q[k] = pos[k] + pt[k];
        int bi = bld_by_pos(b, q);
        if (bi >= 0 && ROOF(b->bl[bi].storeys) >= pos[2] + 6) {
            if (i == 4)
                break;
            pt[0] = pt[0] >= 0 ? fmaxf(pt[0] - dir[0], pt[0] + dir[0]) : fminf(pt[0] - dir[0], pt[0] + dir[0]);
            pt[1] = fminf(pt[1] - dir[1], pt[1] + dir[1]);
        }
        pt[0] = pt[0] - 2 + frand(b) * 4;
        pt[1] = pt[1] - 1 + frand(b) * 4;
        for (int k = 0; k < 3; k++)
            pos[k] += pt[k];
        sp_add(&o->sp, pos, (float)(i + 1) * 1.5f, 0);
    }
}

static void wind_fire(Boss *b, Obj *o)               /* fireWeapon: lightning at a monster 20 .. 50 away */
{
    WindDef *w = &o->s.wind;
    if (frand(b) > 0.5f * (float)(w->shots + 1))
        return;
    int near = 0, nearest = 0;
    float fd = 1000;
    for (int i = 0; i < b->nm; i++) {
        float d = dist3(b->m[i].pos, o->p.pos);
        if (d < 50 && d >= 20)
            near++;
        if (d < fd && d >= 20) {
            nearest = i;
            fd = d;
        }
    }
    int m = near == b->nm ? any_monster(b) : nearest;
    if (fd >= 20) {
        w->shots++;
        weapon_ammo(o, 1, 0);
        set_target(o, m);
    }
}

static int wind_pulse(Boss *b, Obj *o, float frame_dt)   /* onPulse */
{
    WindDef *w = &o->s.wind;
    float pd = w->pulse > 0 ? w->pulse : frame_dt;
    w->life -= pd;
    if (w->life <= 0) {                              /* 6 s: it throws whoever it holds and is gone */
        if (w->attached && m_valid(b, w->monster)) {
            float d[3];
            m_pos(b, w->monster, d);
            d[2] = 0;
            norm3(d);
            scale3(d, 25 + 10 * frand(b));
            d[2] = 15 + 10 * frand(b);
            m_vel(b, w->monster, d);
            m_reaction(b, w->monster, "Jump_Air_Borne_Fall");
        }
        return DELETE_ME;
    }
    if (w->attached) {                               /* follows its monster, 9 above it */
        float pos[3], mp[3], d[3];
        copy3(pos, o->p.pos);
        pos[2] -= 9;
        m_pos(b, w->monster, mp);
        sub3(d, mp, pos);
        if (len3(d) > 1)
            scale3(d, 0.1f);
        d[2] += 9;
        for (int k = 0; k < 3; k++)
            o->p.pos[k] = pos[k] + d[k];
    } else {
        int bi = bld_by_pos(b, o->p.pos);            /* half a second inside a building: gone */
        if (bi >= 0 && ROOF(b->bl[bi].storeys) >= o->p.pos[2] + 6) {
            w->building_time += 0.125f;
            if (w->building_time >= 0.125f * 4)
                return DELETE_ME;
        } else
            w->building_time = 0;
        w->fire_time -= 0.125f;
        if (w->fire_time <= 0) {
            w->fire_time = 2.5f;
            wind_fire(b, o);
        }
    }
    return 1;
}

static int wind_damage(Boss *b, Obj *o, int monster)   /* TakeDamage (by touch): it catches the monster */
{
    WindDef *w = &o->s.wind;
    if (!m_valid(b, monster))
        return 1;
    if (!m_valid(b, w->monster)) {
        w->attached = 1;
        o->mover = MV_NONE;                          /* setMover(an empty LineMover) */
        o->pulse_delay = 0;
        w->pulse = 0;
        w->monster = monster;
        m_hp(b, monster, -unum(o, "Damage", 10) * (w->life / 6));
        m_reaction(b, monster, "React_Tornado");
    }
    if (m_valid(b, w->monster))                      /* and spins it, pi / 8 a touch */
        m_facing(b, w->monster, b->m[w->monster].heading + PI / 8);
    return 1;
}

/* ---------------------------------------------------------------- the NY barriers (VolumeTrigger, NewYork.scr) */

/* VolumeTrigger.initByObject(building, group): the building instance's "VolumeTrigger|<group>" { Center, Extents (half
 * sizes), DoPushout, MinRefireTime, FireFunc } [R: the book; G: Center in the world, Extents half sizes] */
static int trigger_new(Boss *b, int building, const char *group)
{
    if (building < 0 || building >= b->nb || b->ntrig >= MAX_TRIG)
        return -1;
    char key[64];
    snprintf(key, sizeof key, "VolumeTrigger|%s", group);
    Group g = group_sub(b->bl[building].inst, key);
    if (!g.at)
        return -1;
    Trigger *t = &b->trig[b->ntrig];
    memset(t, 0, sizeof *t);
    if (!gpoint(g, "Center", t->c) || !gpoint(g, "Extents", t->e))
        return -1;
    t->alive = 1;
    t->building = building;
    t->pushout = group_num(g, "DoPushout", 0) != 0;
    t->refire = group_num(g, "MinRefireTime", 0);
    t->last = -1e9f;
    t->fire = group_str(g, "FireFunc");
    t->name = group_str(g, "Name");
    return b->ntrig++;
}
static void trigger_kill(Boss *b, int i)             /* trigger.requestDelete */
{
    if (i >= 0 && i < b->ntrig)
        b->trig[i].alive = 0;
}
static int trigger_valid(const Boss *b, int i) { return i >= 0 && i < b->ntrig && b->trig[i].alive; }

static void barrier_fire(Boss *b, int m)             /* NewYork::BlimpBarrierFire [R] */
{
    if (!b->m[m].climbing) {                         /* thrown back hard towards the camera */
        m_reaction(b, m, "React_Airborne");
        float v[3] = { 0, -100, 30 };
        m_vel(b, m, v);
        m_effect(b, m, "Hazard_Electricity_Explode");
    } else {                                         /* zapped off the wall */
        m_reaction(b, m, contains(m_move(b, m), "Side") ? "React_Side_L_Zap" : "React_Front_Zap");
        m_effect(b, m, "Hazard_Electricity_Explode");
    }
}

/* the monster's box (or its spheres' bounds) in the world [G: the game's trigger test is not read] */
static void m_bounds(const BossMonster *m, float lo[3], float hi[3])
{
    if (m->box_max[2] > m->box_min[2]) {
        copy3(lo, m->box_min);
        copy3(hi, m->box_max);
        return;
    }
    copy3(lo, m->pos);
    copy3(hi, m->pos);
    hi[2] += 8.5f;
    for (int s = 0; s < m->nspheres; s++)
        for (int k = 0; k < 3; k++) {
            lo[k] = fminf(lo[k], m->spheres[s][k] - m->spheres[s][3]);
            hi[k] = fmaxf(hi[k], m->spheres[s][k] + m->spheres[s][3]);
        }
}

/* 0x335BF0 for each touch collide_monster reported (0x2E2C50's box query, include VolumeTrigger | PushOutMonsters):
 * an enabled trigger with a FireFunc whose refire clock has run down fires it (FireFunc(monster, normal); the
 * clock set to MinRefireTime, 0x3361A0 runs it) [R]. The DoPushout push was collide_monster's own */
static void triggers_update(Boss *b)
{
    for (int i = 0; i < b->ntouch; i++) {
        const TriggerTouch *k = &b->touch[i];
        Trigger *t = &b->trig[k->trig];
        if (!t->alive || k->monster < 0 || k->monster >= b->nm || b->now - t->last < t->refire)
            continue;
        t->last = b->now;
        if (getenv("RAMPAGE_TRACE"))
            printf("  trigger %d (%s) fired by monster %d: %s\n", k->trig, t->name ? t->name : "?", k->monster,
                   t->fire ? t->fire : "-");
        if (same_text(t->fire, "NewYork::BlimpBarrierFire"))   /* BlimpBarrierCanFire says yes */
            barrier_fire(b, k->monster);
    }
    b->ntouch = 0;
}

void boss_trigger_touch(Boss *b, int trigger, int monster, const float normal[3])
{
    if (!b || trigger < 0 || trigger >= b->ntrig || b->ntouch >= MAX_TRIG * 4)
        return;
    TriggerTouch *k = &b->touch[b->ntouch++];
    k->trig = trigger;
    k->monster = monster;
    copy3(k->n, normal);
}

void boss_collide_add(Boss *b, CollideWorld *w)
{
    if (!w)
        return;
    for (int i = 0; i < b->n; i++) {
        const Obj *o = &b->o[i];
        if (!o->p.alive || o->pending)
            continue;
        float m[12];
        collide_matrix_from16(m, o->p.world);
        collide_add(w, &o->cshape, m, COLLIDE_F_WORLD | o->oflags, COLLIDE_TAG(COLLIDE_OWN_BOSS, i), 0);
    }
    for (int i = 0; i < b->ntrig; i++) {             /* 0x3360F4: 0x400000, | PushOutMonsters with DoPushout; 4 from
                                                      * addObject (0x284CD0, from 0x2A67C0) as every world object's */
        const Trigger *t = &b->trig[i];
        if (!t->alive)
            continue;
        CollideShape sh;
        float lo[3] = { -t->e[0], -t->e[1], -t->e[2] }, m[12];
        collide_shape_box(&sh, lo, t->e);
        collide_matrix(m, 0, t->c);
        collide_add(w, &sh, m, COLLIDE_F_WORLD | COLLIDE_F_TRIGGER | (t->pushout ? COLLIDE_F_PUSHOUT : 0u),
                    COLLIDE_TAG(COLLIDE_OWN_BOSS, i), 1);
    }
}

/* ---------------------------------------------------------------- Boss_NY_Blimp.bsf */

/* its states: 0 circling the Empire buildings (Path_NYEmpire1), 1 knocked back by a blow, 2 on its way to the top
 * (Path_NYEmpire3 / 4), 3 crossing over the top, 5 turning round, 6 beaten and falling */

static void blimp_pulse_delay(Obj *o, float d)       /* setPulseDelay (the script's own): 0 is gWorld.getTimeStep() */
{
    o->s.blimp.pulse = d;
    o->pulse_delay = d;
}

static void blimp_up(Boss *b, Obj *o, float y, const char *left, const char *right)   /* changeBlimpPathToTop(Guard) */
{
    float end[3] = { -25, y, 38 };
    const char *path = left;
    if (wrap_pi(o->p.facing[2]) < 0) {               /* facing +x: the right side */
        end[0] = -end[0];
        path = right;
    }
    ln_init(o, o->p.pos, end, 35);
    ln_add_path(b, o, path);
    o->ln.flags |= 1;
    o->ln.flags &= ~2u;
    o->s.blimp.state = 2;
}
static void blimp_to_top(Boss *b, Obj *o)
{
    if (o->s.blimp.moved_top)
        return;
    blimp_up(b, o, -40, "Path_NYEmpire3L", "Path_NYEmpire3R");
    o->s.blimp.moved_top = 1;
}

static void blimp_init(Boss *b, Obj *o)              /* Construct + onInit */
{
    static const float AT[5][3] = { { -17.5f, 1, 1 }, { 17.5f, 1, 1 }, { 1, 15, 1 }, { -17.5f, 15, 1 },
                                    { 17.5f, 15, 1 } };
    BlimpDef *k = &o->s.blimp;
    o->oflags |= OF_EFF_NODES;
    o->mover = MV_LINE;                              /* initWithPath("Path_NYEmpire1", 15, 1): round and round */
    o->ln.n = 0;
    ln_add_path(b, o, "Path_NYEmpire1");
    o->ln.seg = 0;
    o->ln.t = 0;
    o->ln.speed = 15;
    o->ln.flags = 1 | 2 | 4;
    if (o->ln.n > 0)
        copy3(o->p.pos, o->ln.p[0]);                 /* [G] it starts at the path's first point */
    k->hp = 3;
    k->retarget = 6.5f;
    k->bomb_drop = 0.25f;
    k->voice_time = 3;
    blimp_pulse_delay(o, 0.25f);
    if (frand(b) < 0.5f) {                           /* which generator lowers which barriers */
        k->gen_state[0] = 4;
        k->gen_state[1] = 2;
    } else {
        k->gen_state[1] = 4;
        k->gen_state[0] = 2;
    }
    float far[3] = { -200, 200, 200 };               /* its weapons' target, a DummyObject out of the way */
    set_target_point(o, far);
    k->engine_fx = fx_on(b, o, "Blimp_Engines", NULL, NULL, -1, 0);
    for (int i = 0; i < 5; i++) {                    /* the Empire buildings: front left, front right, back ... */
        k->bld[i].b = bld_by_pos(b, AT[i]);
        k->bld[i].barrier.fx = k->bld[i].barrier.trig = k->bld[i].top.fx = k->bld[i].top.trig = -1;
    }
    for (int i = 0; i < 5; i++) {                    /* their barriers: a glow and a trigger each */
        int bi = k->bld[i].b;
        if (bi < 0)
            continue;
        float pos[3];
        b_pos(b, bi, pos);
        pos[1] -= 5;
        pos[2] = ROOF(b->bl[bi].storeys) + 1;
        const char *fx = i == 2 ? "Barrier_Top" : "Barrier_Front";
        if (i >= 2) {
            pos[1] -= 1;
            pos[2] = k->bld[i - 1].fz;               /* [R] the one before's height */
        }
        emit(b, BE_NO_AUTOCOLLAPSE, o, NULL, NULL)->building = bi;
        k->bld[i].state = 0;
        k->bld[i].fz = pos[2];
        k->bld[i].barrier.fx = fx_at(b, fx, pos);
        k->bld[i].barrier.trig = trigger_new(b, bi, "Barrier");
        if (i >= 2) {
            pos[1] += 1;
            pos[2] = ROOF(b->bl[bi].storeys) + 1;
            k->bld[i].top.fx = fx_at(b, fx, pos);
            k->bld[i].top.trig = trigger_new(b, bi, "BarrierTop");
        }
    }
    for (int f = 4; f <= 6; f++)                     /* the walls before the generators, broken open */
        for (int i = 3; i <= 4; i++)
            for (int c = 1; c <= 2; c++)
                if (k->bld[i].b >= 0) {
                    BossEvent *e = emit(b, BE_CELL_DAMAGE, o, NULL, NULL);
                    e->building = k->bld[i].b;
                    e->level = 2;
                    e->a = f;
                    e->b = c;
                    e->c = 3;
                }
    camera_rmh(b, 35);
}

/* the highest monster and its place (both status updates; monsterLoc starts at 0) */
static int highest(const Boss *b, float loc[3])
{
    int h = -1;
    set3(loc, 0, 0, 0);
    for (int i = 0; i < b->nm; i++)
        if (loc[2] < b->m[i].pos[2]) {
            h = i;
            copy3(loc, b->m[i].pos);
        }
    return h;
}

static void blimp_beam(Boss *b, Obj *o, float pr)   /* updateBeamStatus: the laser, 6 s after its reticle */
{
    BlimpDef *k = &o->s.blimp;
    if (k->state == 6)
        return;
    float ml[3];
    int hm = highest(b, ml);
    k->retarget -= pr;
    if (k->retarget > 0)
        return;
    int m = -1, can = 0;
    float delay = 6.5f;
    if (!k->moved_top) {                             /* circling: only from the front */
        const float *p = o->p.pos;
        if (p[0] < 58 && p[0] > 0 && p[1] < -6.7f)
            can = 1;
        if (p[0] > -55 && p[0] < 0 && p[1] < -8.25f)
            can = 1;
        m = any_monster(b);
    } else if (ml[2] < 35) {                         /* on top, the monsters below: half as often */
        can = 1;
        delay = 6.5f * 2;
        m = any_monster(b);
    } else {                                         /* on top with a monster up there: half the time that one */
        can = 1;
        if (frand(b) < 0.5f && hm >= 0)
            m = hm;
        else
            m = any_monster(b);
    }
    if (!can || !m_valid(b, m))
        return;
    float p[3], tos[3] = { 0, 0, -3 };
    copy3(p, b->m[m].pos);
    p[2] += 3;
    const char *mv = m_move(b, m), *fx = "Blimp_Reticle";
    if (contains(mv, "Side") || contains(mv, "Front"))
        p[2] += 3;
    if (contains(mv, "Front")) {
        p[1] -= 3;
        tos[1] = 3;
    }
    if (contains(mv, "Ground"))
        fx = "Blimp_Reticle_Ground";
    set_target_point(o, p);
    weapon_reload(o, 0);
    k->retarget = delay;
    float at[3] = { p[0] + tos[0], p[1] + tos[1], p[2] + tos[2] };
    fx_at(b, fx, at);
    k->just_fired = 1;
}

static void blimp_bomb(Boss *b, Obj *o, float pr)    /* updateBombStatus */
{
    BlimpDef *k = &o->s.blimp;
    if (k->state == 6)
        return;
    float ml[3];
    highest(b, ml);
    k->bomb_reload -= pr;
    k->bomb_drop -= pr;
    if (k->bomb_reload <= 0) {
        k->bomb_reload = k->moved_top && ml[2] > 35 ? 2.0f * 2 : 2.0f;
        k->dropped = 0;
    }
    const float *p = o->p.pos;
    if (!k->moved_top) {                             /* circling: one over the middle every 2 .. 3 s */
        if (k->bomb_drop <= 0 && p[0] >= -27.5f && p[0] <= 28) {
            k->bomb_reload = 2.0f * 2;
            k->bomb_drop = 2 + frand(b);
            k->dropped++;
            weapon_reload(o, 1);
            k->just_fired = 1;
        }
    } else if (k->bomb_drop <= 0 && k->dropped < 4) {   /* on top: up to 4 over a monster within 6 along x */
        for (int i = 0; i < b->nm; i++)
            if (p[0] > b->m[i].pos[0] - 6 && p[0] < b->m[i].pos[0] + 6) {
                if (ml[2] < 35) {
                    k->bomb_reload = 2;
                    k->bomb_drop = 0.25f;
                } else {
                    k->bomb_reload = 2.0f * 2;
                    k->bomb_drop = 2.0f / 2;
                }
                k->dropped++;
                weapon_reload(o, 1);
                k->just_fired = 1;
                break;
            }
    }
}

static int blimp_pulse(Boss *b, Obj *o, float frame_dt)   /* onPulse */
{
    BlimpDef *k = &o->s.blimp;
    float pr = k->pulse > 0 ? k->pulse : frame_dt;   /* fCurPulseRate */
    if (k->hit_countdown > 0) {                      /* after a blow: its HitEffect twice more, 0.5 s apart */
        float prev = k->hit_countdown;
        k->hit_countdown -= pr;
        if (k->hit_countdown <= 0 || (prev > 0.5f && k->hit_countdown <= 0.5f))
            fx_on(b, o, tstr(o, "HitEffect"), NULL, NULL, -1, 0);
    }
    k->just_fired = 0;
    blimp_beam(b, o, pr);
    blimp_bomb(b, o, pr);
    if (k->voice_played) {                           /* updateVoiceStatus: a voice at most every 3 s */
        k->voice_time = 3;
        k->voice_played = 0;
    }
    k->voice_time -= pr;
    if (k->just_fired && !k->voice_played && k->voice_time <= 0) {
        sfx(b, o, "V_Boss_Taunt");
        k->voice_played = 1;
    }
    if (k->destroyed_buildings < 2)                  /* the front two collapse at a quarter of their HP */
        for (int i = 0; i < 2; i++) {
            int bi = k->bld[i].b;
            if (k->bld[i].state != 0 || bi < 0)
                continue;
            float frac = b->bl[bi].hp_frac;
            if (b->bl[bi].falling)
                frac = 0;
            if (k->bld[i].prev > 0.25f && frac <= 0.25f) {
                emit(b, BE_COLLAPSE, o, NULL, NULL)->building = bi;
                k->bld[i].state = 1;
                fx_stop(b, k->bld[i].barrier.fx);
                trigger_kill(b, k->bld[i].barrier.trig);
                k->destroyed_buildings++;
                if (trigger_valid(b, k->bld[i].top.trig)) {
                    fx_stop(b, k->bld[i].top.fx);
                    trigger_kill(b, k->bld[i].top.trig);
                }
            }
            k->bld[i].prev = frac;
        }
    for (int i = 2; i < 5; i++) {                    /* a back one falling takes its barriers with it */
        int bi = k->bld[i].b;
        if (k->bld[i].state == 0 && bi >= 0 && b->bl[bi].falling) {
            k->bld[i].state = 1;
            if (trigger_valid(b, k->bld[i].barrier.trig)) {
                fx_stop(b, k->bld[i].barrier.fx);
                trigger_kill(b, k->bld[i].barrier.trig);
            }
            if (trigger_valid(b, k->bld[i].top.trig)) {
                fx_stop(b, k->bld[i].top.fx);
                trigger_kill(b, k->bld[i].top.trig);
            }
        }
    }
    if (k->destroyed_gens < 2)                       /* a generator gone: one set of barriers down */
        for (int i = 0; i < 2; i++)
            if (!(k->gen_state[i] & 1) && b->play[i]) {
                k->gen_state[i] |= 1;
                k->destroyed_gens++;
                for (int q = 2; q < 5; q++) {
                    Barrier *br = (k->gen_state[i] & 2) ? &k->bld[q].top : &k->bld[q].barrier;
                    fx_stop(b, br->fx);
                    br->fx = -1;
                    trigger_kill(b, br->trig);
                    if (!(k->gen_state[i] & 2))
                        blimp_to_top(b, o);          /* the lower ones down: it climbs to the top */
                }
            }
    if (k->state == 5) {                             /* turning round over 0.75 s, then across again */
        k->rot -= frame_dt / 0.75f;
        if (k->rot <= 0) {
            k->state = 3;
            k->rot = 0;
            blimp_pulse_delay(o, 0.25f);
            float end[3];
            copy3(end, o->p.pos);
            end[0] = -end[0];
            ln_init(o, o->p.pos, end, 15);
        }
        o->p.facing[2] = k->punched_right ? PI / 2 + k->rot : -PI / 2 - k->rot;
    }
    if (k->state == 6) {                             /* falling: nose down to 60 degrees */
        if (k->fall_rot > -PI / 3)
            k->fall_rot -= pr / 2;
        o->p.facing[0] = k->fall_rot;
    }
    return 1;
}

static int blimp_mover(Boss *b, Obj *o, int state)   /* onMoverCallback */
{
    BlimpDef *k = &o->s.blimp;
    if (state != 2)
        return 1;
    if (k->state == 1) {
        if (!k->moved_top)
            blimp_to_top(b, o);
        else
            blimp_up(b, o, -68, "Path_NYEmpire4L", "Path_NYEmpire4R");   /* changeBlimpPathToTopGuard */
    } else if (k->state == 2) {                      /* on top: across to the other side */
        k->state = 3;
        k->can_take_damage = 1;
        float end[3];
        copy3(end, o->p.pos);
        end[0] = -end[0];
        ln_init(o, o->p.pos, end, 15);
        o->ln.flags &= ~(2u | 1u);
    } else if (k->state == 3) {
        k->state = 5;
        blimp_pulse_delay(o, 0);
        k->punched_right = o->p.pos[0] >= 0;
        k->rot = PI;
    }
    return 1;
}

static int blimp_damage(Boss *b, Obj *o, int monster, const float dir[3])   /* TakeDamage: three blows */
{
    BlimpDef *k = &o->s.blimp;
    if (k->state == 6 || !((k->state == 0 || k->state >= 3) && m_valid(b, monster)))
        return 1;
    k->hp--;
    if (k->hp >= 0)
        challenge_hud(b, o, (float)k->hp / 3);
    float mv[3];
    copy3(mv, dir);
    if (mv[1] > 0)
        mv[1] = -mv[1];
    const char *hit = tstr(o, "HitEffect");
    if (k->hp != 0) {                                /* knocked 25 back (never into the buildings) */
        if (!k->voice_played && k->voice_time <= 0) {
            sfx(b, o, "V_Boss_Damaged");
            k->voice_played = 1;
        }
        k->state = 1;
        k->hit_countdown = 0.5f * 2;
        fx_on(b, o, hit, NULL, NULL, -1, 0);
        mv[2] = 0.5f;
        norm3(mv);
        scale3(mv, 25);
        for (int i = 0; i < 3; i++)
            mv[i] += o->p.pos[i];
        ln_init(o, o->p.pos, mv, 25);
        o->ln.flags &= ~(1u | 2u);
    } else {                                         /* beaten: it falls into the street at (-12, -10) */
        k->state = 6;
        weapons_enable(o, 0);
        fx_stop(b, k->engine_fx);
        k->engine_fx = -1;
        blimp_pulse_delay(o, 0);
        sfx(b, o, "V_Boss_Defeated");
        k->voice_played = 1;
        mv[2] = 0.75f;
        norm3(mv);
        scale3(mv, 10);
        for (int i = 0; i < 3; i++)
            mv[i] += o->p.pos[i];
        float end[3] = { -12, -10, 0 };
        ln_init(o, o->p.pos, mv, 12.5f);
        ln_add(o, end);
        o->ln.flags &= ~(1u | 2u);
        if (hit) {
            float off[3] = { 0, 4, 0 };
            fx_on(b, o, hit, NULL, NULL, -1, 0);
            fx_on(b, o, hit, off, NULL, -1, 0);
            off[1] = -off[1];
            fx_on(b, o, hit, off, NULL, -1, 0);
        }
        add_stats(b, o, monster, 2);
        completion(b, o, "NewYork::setBossBlockFinished");
    }
    return 1;
}

/* ---------------------------------------------------------------- Boss_NY_Generator.bsf */

static void gen_construct(Boss *b, Obj *o)           /* Construct (at the block's load) */
{
    GenDef *g = &o->s.gen;
    o->oflags |= OF_EFF_NODES;
    g->gen_id = b->num_generators++;
    g->hp = 3;
    g->hit_fx = -1;
    o->pulse_delay = 0.33f;
    g->effect = fx_on(b, o, ustr(o, "AttachEffect"), NULL, NULL, -1, 0);
}
static void gen_init(Boss *b, Obj *o)                /* onInit: the building it stands on, 5 behind it */
{
    float p[3];
    copy3(p, o->p.pos);
    p[1] += 5;
    o->s.gen.building = bld_by_pos(b, p);
}
static int gen_damage(Boss *b, Obj *o, int monster, int type)   /* TakeDamage: three blows, or its building falls */
{
    GenDef *g = &o->s.gen;
    if (!m_valid(b, monster) && type != DT_GONE)
        return 1;
    g->hp--;
    if (type == DT_GONE)
        g->hp = 0;
    fx_on(b, o, tstr(o, "HitEffect"), NULL, NULL, -1, 0);
    if (g->hp == 0) {
        if (g->gen_id >= 0 && g->gen_id < 8)
            b->play[g->gen_id] = 1;                  /* gWorld.setPlayValue(genId, 1): the blimp reads it */
        const char *e = tstr(o, "DestEffect");
        if (e) {
            float p[3];
            copy3(p, o->p.pos);
            p[1] += 3;
            fx_at(b, e, p);
        }
        e = tstr(o, "ExplodeEffect");
        if (e) {
            static const float OFF[4][2] = { { -5, -5 }, { 5, -5 }, { -5, 5 }, { 5, 5 } };
            for (int i = 0; i < 4; i++) {
                float p[3] = { o->p.pos[0] + OFF[i][0], o->p.pos[1], o->p.pos[2] + OFF[i][1] };
                fx_at(b, e, p);
            }
        }
        fx_stop(b, g->hit_fx);
        if (m_valid(b, monster))
            add_stats(b, o, monster, 0);
        fx_stop(b, g->effect);
        return DELETE_ME;
    }
    fx_stop(b, g->hit_fx);
    char key[32];
    g->hit_fx = fx_on(b, o, tstr(o, numbered(key, sizeof key, "HitEffect", g->hp)), NULL, NULL, -1, 0);
    return 1;
}
static int gen_pulse(Boss *b, Obj *o)
{
    int bi = o->s.gen.building;
    if (!b_ok(b, bi) || b->bl[bi].falling || b->bl[bi].destroyed)
        return gen_damage(b, o, -1, DT_GONE);
    return 1;
}

/* ---------------------------------------------------------------- Boss_Blimp_Ambient.bsf, Boss_Saucer_Ambient.bsf */

static void amb_path(Boss *b, Obj *o)                /* createPath: 8 points 37.5 apart, at its height + 0 .. 5 */
{
    AmbientDef *a = &o->s.amb;
    float pts[8][3];
    for (int i = 0; i < 8; i++) {
        set3(pts[i], 150 - (300.0f / 8) * (float)i, o->p.pos[1], o->p.pos[2] + frand(b) * 5);
        if (o->p.pos[0] < 0)
            pts[i][0] = -pts[i][0];
    }
    sp_clear(&o->sp);
    for (int i = 0; i < 8; i++)
        sp_add(&o->sp, pts[i], (float)i * (300.0f / 8), 0);
    if (o->p.kind == BK_AMBIENT_SAUCER) {            /* it will hover at a random x in -30 .. 30 */
        a->state = 0;
        a->hover_start = -30 + frand(b) * 60;
        a->hover_loc = pts[0][0];
    }
}
static void amb_init(Boss *b, Obj *o)                /* Construct + onInit */
{
    AmbientDef *a = &o->s.amb;
    a->trips = (int)unum(o, "NumTrips", 3);
    (void)frand(b);                                  /* [R] the start and end it picks are not used */
    o->mover = MV_SPLINE;
    sp_clear(&o->sp);
    a->blades = -1;
    if (o->p.kind == BK_AMBIENT_BLIMP) {
        a->vel = unum(o, "Velocity", 12);
        if (unum(o, "Animate", 0) != 0) {
            o->oflags |= OF_ANIMATE;
            o->p.animate = 1;
        }
        o->sp.flags = 2;
    } else
        a->vel = 24;
    o->sp.vel = a->vel;
    amb_path(b, o);
    const char *att = o->p.kind == BK_AMBIENT_SAUCER ? ustr(o, "Attachment") : NULL;
    if (att) {
        Obj *bl = obj_make(b, att, o->p.pos);
        if (bl) {
            bl->p.owner = idx(b, o);
            a->blades = idx(b, bl);
        }
    }
}
static int amb_process(Boss *b, Obj *o, float dt)   /* Boss_Saucer_Ambient Process */
{
    AmbientDef *a = &o->s.amb;
    Obj *bl = obj_at(b, a->blades);
    if (bl) {
        a->blade_rot = fmodf(a->blade_rot + PI * 3 * dt, 2 * PI);
        copy3(bl->p.pos, o->p.pos);
        float fz = o->p.facing[2] + a->blade_rot;
        copy3(bl->p.facing, o->p.facing);
        bl->p.facing[2] += fz;
    }
    if (a->state == 0) {                             /* crossing its hover point: it slows to a stop */
        float prev = a->hover_loc;
        a->hover_loc = o->p.pos[0];
        if ((prev >= a->hover_start && a->hover_loc < a->hover_start) ||
            (prev <= a->hover_start && a->hover_loc > a->hover_start)) {
            a->state = 1;
            a->hover_delay = 5 + frand(b) * 3;
            o->sp.vel = 0;
        }
    } else if (a->state == 1) {                      /* hovers 5 .. 8 s */
        a->vel = fmaxf(0, a->vel * 0.95f);
        o->sp.vel = a->vel;
        a->hover_delay -= dt;
        if (a->hover_delay <= 0)
            a->state = 2;
    } else if (a->state == 2) {                      /* and off again, 40 / s^2 up to 24 */
        a->vel = fminf(24, a->vel + 40 * dt);
        o->sp.vel = a->vel;
    }
    return 1;
}
static int amb_mover(Boss *b, Obj *o, int state)    /* onMoverCallback: NumTrips crossings, then 50 % more each */
{
    AmbientDef *a = &o->s.amb;
    if (state != 2)
        return 1;
    if (a->trips > 0 || frand(b) < 0.5f) {
        a->trips--;
        amb_path(b, o);
        return 1;
    }
    return DELETE_ME;                                /* [R] its rotor stays where it was (not deleted) */
}

/* ---------------------------------------------------------------- the dispatch */

static void obj_init(Boss *b, Obj *o)                /* Construct + onInit of a new part */
{
    o->pending = 0;
    switch (o->p.kind) {
    case BK_TANK: tank_init(b, o); break;
    case BK_TANK_BOMB: tbomb_init(b, o); break;
    case BK_HELI: heli_init(b, o); break;
    case BK_JUMPER: jumper_init(b, o); break;
    case BK_JUMPER_BOMB: jbomb_init(b, o); break;
    case BK_SAUCER: saucer_init(b, o); break;
    case BK_WHIRLWIND: wind_init(b, o); break;
    case BK_BLIMP: blimp_init(b, o); break;
    case BK_GENERATOR: gen_init(b, o); break;
    case BK_AMBIENT_BLIMP:
    case BK_AMBIENT_SAUCER: amb_init(b, o); break;
    default: break;
    }
    obj_matrix(o);
}

static int on_mover(Boss *b, Obj *o, int state)
{
    switch (o->p.kind) {
    case BK_TANK_BOMB: return tbomb_mover(b, o, state);
    case BK_HELI: return heli_mover(b, o, state);
    case BK_JUMPER: return jumper_mover(b, o, state);
    case BK_JUMPER_BOMB: return jbomb_mover(b, o, state);
    case BK_SAUCER: return saucer_mover(b, o, state);
    case BK_BLIMP: return blimp_mover(b, o, state);
    case BK_AMBIENT_BLIMP:
    case BK_AMBIENT_SAUCER: return amb_mover(b, o, state);
    default: return 1;
    }
}

static int on_pulse(Boss *b, Obj *o, float dt)       /* the scripts with onPulse */
{
    switch (o->p.kind) {
    case BK_TANK_BOMB: return o->s.tbomb.delete_me ? DELETE_ME : 1;
    case BK_HELI: return heli_pulse(b, o, dt);
    case BK_JUMPER_BOMB:
        o->s.jbomb.delay -= 0.25f;
        return o->s.jbomb.delete_me ? DELETE_ME : 1;
    case BK_WHIRLWIND: return wind_pulse(b, o, dt);
    case BK_BLIMP: return blimp_pulse(b, o, dt);
    case BK_GENERATOR: return gen_pulse(b, o);
    default: return 1;
    }
}
static int has_pulse(int kind)
{
    return kind == BK_TANK_BOMB || kind == BK_HELI || kind == BK_JUMPER_BOMB || kind == BK_WHIRLWIND ||
           kind == BK_BLIMP || kind == BK_GENERATOR;
}

static int on_process(Boss *b, Obj *o, float dt)     /* the scripts with Process: every frame */
{
    switch (o->p.kind) {
    case BK_TANK: return tank_process(b, o, dt);
    case BK_JUMPER: return jumper_process(b, o, dt);
    case BK_SAUCER: return saucer_process(b, o, dt);
    case BK_AMBIENT_SAUCER: return amb_process(b, o, dt);
    default: return 1;
    }
}

static int can_take_damage(int kind)                 /* the scripts with TakeDamage */
{
    return kind == BK_TANK || kind == BK_HELI || kind == BK_JUMPER || kind == BK_JUMPER_BOMB || kind == BK_SAUCER ||
           kind == BK_WHIRLWIND || kind == BK_BLIMP || kind == BK_GENERATOR;
}

static int take_damage(Boss *b, Obj *o, int monster, float amount, int type, const float dir[3])
{
    switch (o->p.kind) {
    case BK_TANK: return tank_damage(b, o, monster, amount);
    case BK_HELI: return heli_damage(b, o, monster);
    case BK_JUMPER: return jumper_damage(b, o, monster, amount, type);
    case BK_JUMPER_BOMB: return jbomb_damage(b, o, monster);
    case BK_SAUCER: return saucer_damage(b, o, monster, amount);
    case BK_WHIRLWIND: return wind_damage(b, o, monster);
    case BK_BLIMP: return blimp_damage(b, o, monster, dir);
    case BK_GENERATOR: return gen_damage(b, o, monster, type);
    default: return 1;
    }
}

/* ---------------------------------------------------------------- touching */

/* the distance from c to a part's box (in its own frame; its turn is orthonormal, its scale left out) */
static float box_distance(const Obj *o, const float c[3])
{
    const float *w = o->p.world;
    float d[3] = { c[0] - w[12], c[1] - w[13], c[2] - w[14] }, l[3], q = 0;
    for (int k = 0; k < 3; k++)
        l[k] = d[0] * w[4 * k] + d[1] * w[4 * k + 1] + d[2] * w[4 * k + 2];
    for (int k = 0; k < 3; k++) {
        float e = l[k] < o->lo[k] ? o->lo[k] - l[k] : l[k] > o->hi[k] ? l[k] - o->hi[k] : 0;
        q += e * e;
    }
    return sqrtf(q);
}
static int touches(const Obj *o, const BossMonster *m)   /* [G] a monster's spheres (or its box) in the part's box */
{
    for (int s = 0; s < m->nspheres; s++)
        if (box_distance(o, m->spheres[s]) <= m->spheres[s][3])
            return 1;
    if (m->nspheres == 0) {
        float lo[3], hi[3], c[3];
        m_bounds(m, lo, hi);
        for (int k = 0; k < 3; k++)
            c[k] = (lo[k] + hi[k]) / 2;
        float r = (hi[0] - lo[0]) / 2;
        return box_distance(o, c) <= r;
    }
    return 0;
}
static void away_from(const Obj *o, const BossMonster *m, float dir[3])   /* TakeDamage's direction [G: as army.c] */
{
    sub3(dir, o->p.pos, m->pos);
    norm3(dir);
}

/* each frame: a part without OnlyHitWithEffNodes takes TakeDamage(monster, 2, Collision) from a monster touching
 * it [R 0x2E22B0: an unarmed sphere against the part's shape, amount 2.0 (0x2E2680), type 1]. A PushOutMonsters part's
 * push is the collision world's (collide_monster), not this */
static void contacts(Boss *b)
{
    for (int i = 0; i < b->n; i++) {
        Obj *o = &b->o[i];
        if (!o->p.alive || o->pending || !can_take_damage(o->p.kind) || (o->oflags & OF_EFF_NODES))
            continue;
        for (int m = 0; m < b->nm && o->p.alive; m++) {
            const BossMonster *mm = &b->m[m];
            if (touches(o, mm)) {
                float dir[3];
                away_from(o, mm, dir);
                if (take_damage(b, o, m, 2, DT_COLLISION, dir) == DELETE_ME)
                    obj_remove(b, o);
            }
        }
    }
}

/* ---------------------------------------------------------------- the spawner (gAIMgr's rules, army.c) */

static void spawner_update(Boss *b, float dt)
{
    if (!b->active)
        return;
    for (int i = 0; i < b->nsp; i++) {
        Spawner *s = &b->sp[i];
        if (s->alive != 0 || s->left == 0)
            continue;
        s->timer -= dt;
        if (s->timer > 0)
            continue;
        s->timer = s->respawn + s->offset * frand(b);
        s->alive++;
        s->left--;
        Group t = find_tmpl(b, s->name);
        if (!t.at)
            continue;
        Obj *o = obj_new(b, s->name, t, s->g, NULL);
        if (!o)
            continue;
        o->spawner = i;
        o->p.is_boss = 1;
        b->main = idx(b, o);
        obj_init(b, o);
    }
}

/* ---------------------------------------------------------------- one part, one frame */

static void obj_update(Boss *b, Obj *o, float dt)
{
    if (!o->p.alive || o->pending || o->p.kind == BK_ROTOR)
        return;
    int r = 1;
    if (o->mover == MV_SPLINE) {
        int cb = sp_step(o, dt);
        if (cb == 2)
            r = on_mover(b, o, cb);
    } else if (o->mover == MV_LINE) {
        o->oriented = 0;
        int cb = ln_step(o, dt);
        if (cb == 2)
            r = on_mover(b, o, cb);
    }
    if (r != DELETE_ME && o->p.alive && o->p.anim[0]) {   /* the actor's play; one that does not repeat ends once */
        o->p.anim_time += dt;
        if (o->p.anim_len > 0 && o->p.anim_time >= o->p.anim_len) {
            if (o->p.anim_loop)
                o->p.anim_time = fmodf(o->p.anim_time, o->p.anim_len);
            else {
                o->p.anim_time = o->p.anim_len;
                if (!o->anim_done) {
                    o->anim_done = 1;
                    if (o->p.kind == BK_JUMPER)
                        jumper_anim_done(b, o);
                }
            }
        }
    }
    if (r != DELETE_ME && o->p.alive && has_pulse(o->p.kind)) {   /* setPulseDelay; 0: every frame */
        o->pulse_time += dt;
        if (o->pulse_delay <= 0 || o->pulse_time >= o->pulse_delay) {
            o->pulse_time = o->pulse_delay > 0 ? fmodf(o->pulse_time, o->pulse_delay) : 0;
            r = on_pulse(b, o, dt);
        }
    }
    if (r != DELETE_ME && o->p.alive)
        r = on_process(b, o, dt);
    if (r == DELETE_ME) {
        obj_remove(b, o);
        return;
    }
    if (!o->p.alive)
        return;
    float d[3];                                      /* [R 0x314F18 .. 0x315168] after the object's own update
                                                      * (0x309AC0: its mover, pulse and script) */
    int wobble = army_osc_step(&o->osc, dt, d);
    if (wobble && !o->oriented)                      /* the facing the script set stays and carries it */
        for (int k = 0; k < 3; k++)
            o->p.facing[k] += d[k];
    obj_matrix(o);
    if (wobble && o->oriented)                       /* a mover's frame: its facing (0x101220) + the change, made
                                                      * again (0x101330) */
        army_osc_turn(o->p.world, d);
    if (o->nw)
        obj_weapons_tick(b, o, dt);
}

/* ---------------------------------------------------------------- the API */

Boss *boss_new(const BossHost *host)
{
    Boss *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    if (host)
        b->host = *host;
    b->main = -1;
    b->view[0] = -45;                                /* [G] what the camera sees of the street, along x (army.c) */
    b->view[1] = 45;
    return b;
}
void boss_free(Boss *b) { free(b); }
void boss_set_view(Boss *b, float xmin, float xmax) { b->view[0] = xmin; b->view[1] = xmax; }
void boss_set_city_destroyed(Boss *b, int on) { b->city_destroyed = on != 0; }
void boss_activate(Boss *b, int on) { b->active = on != 0; }

void boss_clear(Boss *b)
{
    for (int i = 0; i < b->n; i++)
        if (b->o[i].p.alive)
            obj_remove(b, &b->o[i]);
    b->n = 0;
    b->nsp = 0;
    b->active = 0;
    b->ntrig = 0;
    b->nwobj = 0;
    b->num_generators = 0;                           /* [G] the script's global, taken as starting again per block */
    memset(b->play, 0, sizeof b->play);
    b->main = -1;
    b->defeated = 0;
}

static void location(Group g, float out[3])
{
    set3(out, 0, 0, 0);
    gpoint(g, "Location", out);
}

int boss_load_block(Boss *b, Group block_def)
{
    boss_clear(b);
    Record r[64];
    Group ai = group_sub(block_def, "AIDef");
    uint32_t n = ai.at ? group_all(ai, "AIInstance", r, 64) : 0;
    for (uint32_t i = 0; i < n && b->nsp < MAX_SPAWN; i++) {   /* the boss's record (0x334C40's reader, as army.c) */
        Group g = record_group(ai, r[i]);
        const char *name = gstr(g, "Name");
        if (!name || !boss_is_boss_template(find_tmpl(b, name)))
            continue;
        Spawner *s = &b->sp[b->nsp++];
        memset(s, 0, sizeof *s);
        s->g = g;
        s->name = name;
        s->min_delay = group_num(g, "MinDelay", 10);
        s->respawn = group_num(g, "RespawnDelay", 10);
        s->offset = group_num(g, "OffsetDelay", 5);
        s->left = (int)group_num(g, "MaxOccurances", 9999);
        s->timer = s->min_delay + s->offset * frand(b);
    }
    Group where;
    static Record obj[256];
    n = group_values(block_def, "ObjectInstance|", &where, obj, 256);
    for (uint32_t i = 0; i < n; i++) {               /* the generators; the world objects the bosses look for */
        Group g = record_group(where, obj[i]);
        const char *name = group_str(g, "Name");
        Group t = find_tmpl(b, name);
        if (!t.at)
            continue;
        float pos[3];
        location(g, pos);
        if (boss_is_boss_template(t)) {
            Obj *o = obj_new(b, name, t, g, pos);
            if (o) {
                if (o->p.kind == BK_GENERATOR)
                    gen_construct(b, o);
                o->pending = 1;                      /* onInit once the buildings are known */
            }
        } else if (b->nwobj < MAX_WOBJ && group_get(t, "HitRadius", &r[0])) {
            WorldObj *w = &b->wobj[b->nwobj++];
            w->name = name;
            w->tmpl = t;
            w->gone = 0;
            copy3(w->pos, pos);
        }
    }
    return b->nsp;
}

int boss_load_waiting(Boss *b, Group block_def, float y, int slot)
{
    Group where;
    static Record obj[256];
    int made = 0;
    uint32_t n = group_values(block_def, "AttackObjectInstance|", &where, obj, 256);
    for (uint32_t i = 0; i < n; i++) {
        Group g = record_group(where, obj[i]);
        if (group_num(g, slot == 1 ? "LoadWhenWaiting" : "LoadWhenDistant", 0) == 0)
            continue;
        const char *name = group_str(g, "Name");
        Group t = find_tmpl(b, name);
        int kind = kind_of(gstr(t, "ScriptName"));
        if (kind != BK_AMBIENT_BLIMP && kind != BK_AMBIENT_SAUCER)
            continue;
        float pos[3];
        location(g, pos);
        pos[1] += y;
        Obj *o = obj_new(b, name, t, g, pos);
        if (o) {
            o->pending = 1;
            made++;
        }
    }
    return made;
}

int boss_spawn(Boss *b, const char *tmpl_name, Group inst, const float pos[3])
{
    Group t = find_tmpl(b, tmpl_name);
    if (!t.at)
        return -1;
    Obj *o = obj_new(b, tmpl_name, t, inst, pos);
    if (!o)
        return -1;
    o->pending = 1;
    return idx(b, o);
}

void boss_update(Boss *b, float dt, const BossMonster *m, int nm, const BossBuilding *bl, int nb)
{
    b->m = m;
    b->nm = m ? nm : 0;
    b->bl = bl;
    b->nb = bl ? nb : 0;
    b->dt = dt;
    b->now += dt;
    for (int i = 0; i < b->n; i++)                   /* onInit of what was loaded or made outside a frame */
        if (b->o[i].p.alive && b->o[i].pending)
            obj_init(b, &b->o[i]);
    spawner_update(b, dt);
    for (int i = 0; i < b->n; i++)                   /* (parts made this frame run too) */
        obj_update(b, &b->o[i], dt);
    for (int i = 0; i < b->n; i++) {                 /* the rotors, placed by their owners */
        Obj *o = &b->o[i];
        if (o->p.alive && o->p.kind == BK_ROTOR)
            obj_matrix(o);
    }
    triggers_update(b);
    contacts(b);
    while (b->n > 0 && !b->o[b->n - 1].p.alive)
        b->n--;
}

int boss_count(const Boss *b) { return b->n; }
const BossPart *boss_part(const Boss *b, int i) { return i >= 0 && i < b->n ? &b->o[i].p : NULL; }
int boss_main(const Boss *b) { return b->main; }
int boss_defeated(const Boss *b) { return b->defeated; }

int boss_events(Boss *b, BossEvent *out, int max)
{
    int n = b->nev < max ? b->nev : max;
    if (out && n > 0)
        memcpy(out, b->ev, (size_t)n * sizeof *out);
    if (n < b->nev)
        memmove(b->ev, b->ev + n, (size_t)(b->nev - n) * sizeof *b->ev);
    b->nev -= n;
    return n;
}

/* [R 0x2E22B0] an armed sphere: TakeDamage(monster, the attack's damage, 0) on each part it meets; a part with 0x8000
 * (OnlyHitWithEffNodes: the Tank, Heli, Jumper, Saucer, Blimp, Generator) goes on the attack's hit list, once */
int boss_hit(Boss *b, int monster, const float centre[3], float radius, float amount, unsigned record)
{
    if (monster < 0 || monster >= b->nm)
        return 0;
    int n = 0;
    for (int i = 0; i < b->n; i++) {
        Obj *o = &b->o[i];
        if (!o->p.alive || o->pending || !can_take_damage(o->p.kind) || box_distance(o, centre) > radius)
            continue;
        if ((o->oflags & OF_EFF_NODES) && !collide_hit_once(&o->hits, monster, record))
            continue;
        float dir[3];
        away_from(o, &b->m[monster], dir);
        n++;
        if (take_damage(b, o, monster, amount, DT_CONCUSSIVE, dir) == DELETE_ME)
            obj_remove(b, o);
    }
    return n;
}
