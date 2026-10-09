/* footstep.c - the monster's footfalls and landings (footstep.h, docs/FOOTSTEP_NOTES.md).
 * [R] read in the ELF, [G] guessed. */
#include <math.h>
#include <string.h>
#include "footstep.h"

#define STILL 1e-6f                                  /* 0x358637BD: |getVelocity|^2 below it is "not moving" */
#define NODE_FOOT_LOCK (0x50u | 0x1080u)             /* the state flags the foot object locks the feet in */
#define SLIP_GONE 1e-6f                              /* 0x358637BE: 0x33BF10's slip^2 at which the anchor is let go */
#define SNAP 0.001f                                  /* 0x3A83126F: 0x33CF90 snaps the share to 0 or 1 within it */
#define DRAG_SHARE 0.333f                            /* 0x3EAA7EFA: the arc's height per unit of the drag */
#define PI_F 3.14159265f                             /* 0x40490FDB */

/* ---- 1. the names --------------------------------------------------------------------------------------------- */

static void put(char *dst, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n > 63)
        n = 63;
    memcpy(dst, src ? src : "", n);
    dst[n] = 0;
}

void foot_props_default(FootProps *p)
{
    put(p->ground_step, "footfall.edf");
    put(p->water_step, "footfall_water.edf");
    put(p->ground_impact, "footfall_big.edf");
    put(p->water_impact, "footfall_big_water.edf");
}

/* [R] 0x2F5D80 (from 0x2F6168): MonsterFx of the book (+0xAD0 of the monster's setup record) is looked up (0x1B0210);
 * each of the four keys (the table 0x3F5BCC .. 0x3F5BD8) is a name (0x1B4490), loaded as an effect definition
 * (0x198DC0 on the library 0x4861C0) and kept refcounted at +0x140 / +0x144 / +0x150 / +0x154 of the record at
 * monster +0x7DFC. A missing key leaves its pointer NULL [R: the initialiser 0x2F65D8 zeroes them], and every
 * starter returns at once on NULL. */
int foot_props_read(FootProps *p, Group monster_fx)
{
    if (!monster_fx.at)
        return 0;
    static const char *const KEY[4] = { "GroundFootStepFx", "WaterFootStepFx", "GroundImpactFx", "WaterImpactFx" };
    char *dst[4] = { p->ground_step, p->water_step, p->ground_impact, p->water_impact };
    int n = 0;
    for (int i = 0; i < 4; i++) {
        const char *s = group_str(monster_fx, KEY[i]);
        put(dst[i], s);
        n += s != NULL;
    }
    return n;
}

/* [R] 0x301AE0 keeps the nodes named "Bip01 L Foot" (gp-0x71E8) and "Bip01 R Foot" (gp-0x71EC) at the animation
 * player's +0x72C0 / +0x72C4 (monster +0x7DA8 / +0x7DAC); 0x2E5C40 takes +0x7DA8 for foot 0 and +0x7DAC otherwise,
 * after the callers' mirror swap. The viewer poses the clip unmirrored and reflects the drawing, and its
 * scene_monster_node gives a mirrored monster's node of the other side reflected: so the clip's foot f is the
 * drawn bone of f ^ mirrored [G: the equivalence, from the two conventions]. */
const char *foot_bone(int foot, int mirrored)
{
    return ((foot != 0) != (mirrored != 0)) ? "Bip01 R Foot" : "Bip01 L Foot";
}

/* ---- 2. the steps --------------------------------------------------------------------------------------------- */

void foot_init(FootState *s, int disable_foot_drag)
{
    memset(s, 0, sizeof *s);                         /* [R] 0x33D4E0: the records zeroed */
    s->anchor = -1;                                  /* [R] +0x6C = -1 */
    s->no_drag = disable_foot_drag != 0;             /* [R] +0x69: MiscData|DisableFootDrag != 0 */
    s->keep[0] = s->keep[1] = 0.5f;                  /* [R] +0x54, +0x58 = 0.5 */
    s->keep[2] = s->keep[3] = 0.7f;                  /* [R] +0x5C, +0x60 = 0.7 */
    s->rtw = 0.7f;                                   /* [R] +0x64 = 0.7 until the book's */
    s->drag_scale = 1;                               /* [R] 0x2E9E54: monster +0xAC4 = 1 until the book's */
    s->links = 3;
}

/* [R] 0x33D4E0: +0x50 = -(row z of the node's inverse bind rotation . its translation), the node at monster +0x7DAC
 * ("Bip01 R Foot") in the skeleton's bind pose (+0x3C, 0x50 bytes a node, the inverse bind matrix from +0x4: 0x174B20
 * transposes it, 0x174A90 turns the translation back) */
float foot_ankle(const Tree *t)
{
    static const char tail[] = "Bip01 R Foot";
    for (uint32_t i = 0; t && i < t->n; i++) {
        const char *n = t->nodes[i].name;
        size_t a = n ? strlen(n) : 0, b = sizeof tail - 1;
        if (a < b || strcmp(n + a - b, tail) || !t->nodes[i].has_bind)
            continue;
        const float *m = t->nodes[i].inverse_bind;   /* column-major: the game's rows are its columns */
        return -(m[8] * m[12] + m[9] * m[13] + m[10] * m[14]);
    }
    return 0;
}

/* [R] 0x2E5640 (from the animation player's setup 0x301AE0): no MiscData, no foot IK; Use3LinkFootIK != 0 (an int)
 * the 3-link (0x2F7D70, vtable 0x44AF60), else the 2-link StdIKPairAnimator (0x2F8ED0, 0x44AFA0). The setup 0x2E98C0
 * reads FootIKDragHeightScale (a float) to +0xAC4, 0x33D4E0 RTWSmoothFactor (a float) to +0x64; a key of another
 * type is not taken. */
void foot_ik_props(FootState *s, Group misc, const Tree *skeleton)
{
    s->ankle = foot_ankle(skeleton);
    if (!misc.at) {
        s->links = 0;
        return;
    }
    s->links = group_num(misc, "Use3LinkFootIK", 0) != 0 ? 4 : 3;
    s->drag_scale = group_num(misc, "FootIKDragHeightScale", s->drag_scale);
    s->rtw = group_num(misc, "RTWSmoothFactor", s->rtw);
}

/* the record a script's foot names: [R] 0x33CDA0 / 0x33CCC0 / 0x33CA10 all swap it when mirrored (+0x7DA1):
 * (foot + 1) & 1; unmirrored it is taken as given (0x33CDA0 indexes with it as is; 0x33CCC0 tests it against 0) */
static int rec_index(int foot, int mirrored)
{
    return mirrored ? (foot + 1) & 1 : foot != 0;
}

/* [R] 0x33CDA0: record (foot) +0x24 &= ~3, +0x0 = 0. The anchor (+0x6C) is left as it is. */
void foot_step(FootState *s, int foot, int mirrored)
{
    FootRec *r = &s->rec[rec_index(foot, mirrored)];
    r->flags &= ~(FOOT_PLANTED | FOOT_DRAGGED);
    r->weight = 0;
}

static int still(const FootPose *m)
{
    return m->vel[0] * m->vel[0] + m->vel[1] * m->vel[1] + m->vel[2] * m->vel[2] < STILL;
}

/* [R] 0x33CCC0: unless the debug byte 0x4D6A9A (never set here) and only while |getVelocity|^2 < 1e-6: the foot's
 * place (0x2E5C40: its node's model-space place x the dynamic scale +0xA78, through the monster's matrix), z = the
 * monster's ground (0x2E8530, cached per frame at +0xA48), then 0x2DC350 (record 1) or 0x2DC3D0 (record 0): with a
 * water (0x2D5F00) the place becomes (x, y, the water's +0x34) and the water flag is passed; 0x2F4A20 / 0x2F4B70
 * take +0x144 (water) or +0x140 (ground), give up on NULL, make the foot's emitter (+0x14C / +0x148) anew from that
 * definition only when it held another one (0x208220, the old freed by 0x207A40), and then place it (+0x8C), give
 * it the identity frame (+0x90, gp-0x70F0) and restart it (+0x2C). No sound of its own (the effects carry theirs),
 * no shake, no damage, no scale, no water push. */
int foot_plant(FootState *s, const FootProps *p, int foot, const FootPose *m, FootFx *fx)
{
    (void)s;                                         /* [R] the records are not touched */
    if (!still(m))
        return 0;
    int r = rec_index(foot, m->mirrored);
    const char *name = m->in_water ? p->water_step : p->ground_step;
    if (!name[0])
        return 0;
    fx->name = name;
    fx->pos[0] = m->foot[foot != 0][0];              /* the clip's foot: foot_bone(foot, mirrored) gave it */
    fx->pos[1] = m->foot[foot != 0][1];
    fx->pos[2] = m->in_water ? m->water_z : m->ground;
    fx->water = m->in_water != 0;
    fx->slot = r;
    return 1;
}

/* [R] 0x33CCA0 (first thing in the state command 0x2E6580, a1 the command, its +0 the new node): with a new node,
 * +0x74 = monster +0x10C, which 0x2E6848 only afterwards sets to the new node: the state being left. The clip's
 * events (cancelFootStep among them) start after it (0x2F45D0 at 0x2E6858). */
void foot_command(FootState *s, uint32_t leaving_flags)
{
    s->node_flags = leaving_flags;
}

/* [R] 0x33CA10: only for foot 0 (the argument as given), unless the debug byte; the foot object's state node
 * (+0x74, the state left: 0x33CCA0) must exist and have flags 0x50 or 0x1080, and the monster be still.
 * s2 = record 0 (1 mirrored), s1 the other; both feet's places (0x2E5C40) into the monster's frame (minus +0x2C, then
 * 0x101190: the dots with its axes); if s2's y < s1's, they swap: s2 is the foot further forward. s2: weight 1,
 * flags |= 1, its place, z = the ground (0x2E8530) + the ankle (+0x50). s1: with DisableFootDrag weight 0 (flags
 * kept); otherwise weight 1, flags |= 2, the drag set up (0x33C5A0, drag_setup), flags &= ~1. +0x6C = s2. */
static void drag_setup(FootState *s, int r, const FootPose *m);
int foot_cancel(FootState *s, int foot, const FootPose *m)
{
    if (foot != 0 || !(s->node_flags & NODE_FOOT_LOCK) || !still(m))
        return -1;
    int a = m->mirrored ? 1 : 0, b = a ^ 1;
    float ya = 0, yb = 0;
    for (int k = 0; k < 3; k++) {                    /* record r is the clip's foot r ^ mirrored */
        ya += (m->foot[a ^ (m->mirrored != 0)][k] - m->place[k]) * m->fwd[k];
        yb += (m->foot[b ^ (m->mirrored != 0)][k] - m->place[k]) * m->fwd[k];
    }
    if (ya < yb) {
        int t = a;
        a = b;
        b = t;
    }
    FootRec *pl = &s->rec[a], *dr = &s->rec[b];
    pl->flags |= FOOT_PLANTED;
    pl->weight = 1;
    memcpy(pl->at, m->foot[a ^ (m->mirrored != 0)], sizeof pl->at);
    pl->at[2] = m->ground + s->ankle;
    if (s->no_drag) {
        dr->weight = 0;
    } else {
        dr->weight = 1;
        dr->flags |= FOOT_DRAGGED;
        drag_setup(s, b, m);
        dr->flags &= ~FOOT_PLANTED;
    }
    s->anchor = a;
    return a;
}

/* ---- 2b. the foot lock and the foot IK's targets --------------------------------------------------------------- */

/* the monster's matrix (0x2E5C40's, without the dynamic scale): model x = (fwd y, -fwd x), y = fwd, z up */
static void to_world(const float place[3], const float fwd[3], const float p[3], float out[3])
{
    out[0] = place[0] + p[0] * fwd[1] + p[1] * fwd[0];
    out[1] = place[1] - p[0] * fwd[0] + p[1] * fwd[1];
    out[2] = place[2] + p[2];
}

static void heading_fwd(float heading, float fwd[3])
{
    float h = heading / 57.2957795f;
    fwd[0] = -sinf(h);
    fwd[1] = cosf(h);
    fwd[2] = 0;
}

/* [R] 0x33C5A0 (record r dragged, the other planted): the planted foot as the clip now poses it (0x33C290 mirrored,
 * else the clip's own query, animation player +0x2A24 vtable +0x34, time offset 0), through the monster's matrix, is
 * short of where it was just planted by d (x, y); the dragged foot as the clip poses it, through the matrix moved by d,
 * is how far from where it is now (0x2E5C40)? 0.333 of that (x, y), at most 3 x the ankle (gp-0x6C68, worked out the
 * first time and kept: gp-0x6C64), times FootIKDragHeightScale (+0xAC4), is the arc's height (+0x8). So the arc
 * scales with how far the foot will be dragged. */
static void drag_setup(FootState *s, int r, const FootPose *m)
{
    int p = r ^ 1, mi = m->mirrored != 0;
    float lift = 0;
    if (m->clip_ok) {
        float wp[3], wq[3];
        to_world(m->place, m->fwd, m->clip_foot[p ^ mi], wp);
        to_world(m->place, m->fwd, m->clip_foot[r ^ mi], wq);
        float ex = m->foot[r ^ mi][0] - (wq[0] + s->rec[p].at[0] - wp[0]);
        float ey = m->foot[r ^ mi][1] - (wq[1] + s->rec[p].at[1] - wp[1]);
        lift = DRAG_SHARE * sqrtf(ex * ex + ey * ey);
    }
    if (!s->lift_cap_set) {                          /* [R] once for the game [G: kept per monster here] */
        s->lift_cap = 3 * s->ankle;
        s->lift_cap_set = 1;
    }
    if (!(lift <= s->lift_cap))
        lift = s->lift_cap;
    s->rec[r].lift = lift * s->drag_scale;
}

/* [R] 0x33BF10 (a1 the matrix the frame's move made, a2 the matrix out; 1 = take it): nothing under the debug byte
 * 0x4D6A9A; with an anchor (+0x6C): first a footfall at its record's place when the +0x68 byte is set (never: no
 * code sets it, FOOTSTEP_NOTES.md); the anchor let go (-1) when the state now (monster +0x10C) travels (0x50 or
 * 0x1080: 0x33BFE8 .. 0x33C000 go to the drop at 0x33C018) or its record is no longer planted (footStep); else the
 * anchor's foot as the clip alone poses it now (0x33C290 mirrored, else
 * the player's clip query) through the matrix, against the record's place, (x, y): within 1e-6 squared the anchor is
 * let go; else the place moves by (1 - RTWSmoothFactor) of it, the rotation kept. */
int foot_lock(FootState *s, uint32_t node_flags, const float foot[2][3], float place[3], float heading)
{
    int a = s->anchor;
    if (a < 0 || a > 1)
        return 0;
    if ((node_flags & NODE_FOOT_LOCK) || !(s->rec[a].flags & FOOT_PLANTED)) {
        s->anchor = -1;
        return 0;
    }
    float fwd[3], w[3];
    heading_fwd(heading, fwd);
    to_world(place, fwd, foot[a], w);
    float dx = s->rec[a].at[0] - w[0], dy = s->rec[a].at[1] - w[1];
    if (dx * dx + dy * dy <= SLIP_GONE) {
        s->anchor = -1;
        return 0;
    }
    place[0] += (1 - s->rtw) * dx;
    place[1] += (1 - s->rtw) * dy;
    return 1;
}

/* [R] 0x33CF90 (record r, every frame from 0x33D450 while the monster has a foot IK):
 *  - once eased (+0x24 & 0x10): +0x4 = weight x (1 - k) + +0x4 x k, k = +0x58 planted, else +0x60 (the share kept),
 *    then 0 within 0.001 of 0 and 1 within 0.001 of 1;
 *  - planted: z = the ground (0x2E8530) + the ankle (+0x50); its leg's first node (0x2F8F50 link 0, through the
 *    monster's matrix) to the place, beyond the leg's length (0x2F9060) brought back to it;
 *  - else the place is the foot as posed (0x2E5C40); dragged: once the clip blend is over (player +0x4E58 <= 0) the
 *    drag ends (flags &= ~2, weight 0), else z is raised to at least ground + ankle + sin(pi (1 - left / time)) x +0x8;
 *  - once held (+0x24 & 4): +0x18 = place + k (+0x18 - place), k = +0x54 planted, else +0x5C. */
static void record_update(FootState *s, int r, const FootFrame *m, const float fwd[3])
{
    FootRec *f = &s->rec[r];
    int planted = (f->flags & FOOT_PLANTED) != 0;
    if (f->flags & FOOT_EASING) {
        float k = planted ? s->keep[1] : s->keep[3];
        f->shown = f->weight * (1 - k) + f->shown * k;
        if (fabsf(f->shown) <= SNAP)
            f->shown = 0;
        if (fabsf(f->shown - 1) < SNAP)
            f->shown = 1;
    }
    if (planted) {
        f->at[2] = m->ground + s->ankle;
        float hip[3], d[3];
        to_world(m->place, fwd, m->hip[r], hip);
        for (int k = 0; k < 3; k++)
            d[k] = f->at[k] - hip[k];
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(dist <= m->reach[r]))
            for (int k = 0; k < 3; k++)
                f->at[k] = hip[k] + d[k] * (m->reach[r] / dist);
    } else {
        to_world(m->place, fwd, m->foot[r], f->at);
        if (f->flags & FOOT_DRAGGED) {
            if (m->blend_left <= 0) {
                f->flags &= ~FOOT_DRAGGED;
                f->weight = 0;
            } else {
                float done = 1 - m->blend_left * (m->blend_time > 0 ? 1 / m->blend_time : 0);
                float z = m->ground + s->ankle + sinf(PI_F * done) * f->lift;
                if (f->at[2] < z)
                    f->at[2] = z;
            }
        }
    }
    if (f->flags & FOOT_HELD) {
        float k = planted ? s->keep[0] : s->keep[2];
        for (int j = 0; j < 3; j++)
            f->held[j] = f->at[j] + k * (f->held[j] - f->at[j]);
    }
}

/* [R] 0x33D450: 0x33CF90 for records 0 and 1, then 0x33D490: 0x33CDE0 for 0 and 1 (also run by the state command
 * 0x2E6580): the first time, +0x4 = weight (flags |= 0x10) and +0x18 = the place (flags |= 4); the target is +0x18
 * in the monster's frame (0x101190: the dots with its axes) with the rotation of the posed foot node (monster +0x7DA8
 * for 0, +0x7DAC for 1: that is ik_feet's) and the share +0x4 (0x2F8FE0: chain +0x48; 0x2F8FF0: chain +0x18). The
 * IK itself runs right after (0x2E1338: vtable +0x8). */
void foot_update(FootState *s, const FootFrame *m, float w[2], float target[2][3])
{
    float fwd[3];
    heading_fwd(m->heading, fwd);
    for (int r = 0; r < 2; r++) {
        w[r] = 0;
        memset(target[r], 0, sizeof target[r]);
    }
    if (!s->links)
        return;
    for (int r = 0; r < 2; r++)
        if (m->ok[r])
            record_update(s, r, m, fwd);
    for (int r = 0; r < 2; r++) {
        FootRec *f = &s->rec[r];
        if (!m->ok[r])
            continue;
        if (!(f->flags & FOOT_EASING)) {
            f->shown = f->weight;
            f->flags |= FOOT_EASING;
        }
        if (!(f->flags & FOOT_HELD)) {
            memcpy(f->held, f->at, sizeof f->held);
            f->flags |= FOOT_HELD;
        }
        float d[3] = { f->held[0] - m->place[0], f->held[1] - m->place[1], f->held[2] - m->place[2] };
        target[r][0] = d[0] * fwd[1] - d[1] * fwd[0];    /* . model x (fwd y, -fwd x) */
        target[r][1] = d[0] * fwd[0] + d[1] * fwd[1];    /* . model y (fwd) */
        target[r][2] = d[2];
        w[r] = f->shown;
    }
}

/* ---- 3. the landing ------------------------------------------------------------------------------------------- */

/* [R] 0x2DC450 (the landing, from 0x2E2980): the place of the node at animation player +0x72C8 ("Bip01 Spine1",
 * 0x2E6000), z = the water's +0x34 with a water (0x2D5F00), else the ground (0x2E8530); 0x2F48D0 takes +0x154
 * (water) or +0x150 (ground) into the one emitter +0x158 as the steps do. Always, whatever the fall speed. */
int foot_impact(const FootProps *p, const float spine[3], float ground, int in_water, float water_z, FootFx *fx)
{
    const char *name = in_water ? p->water_impact : p->ground_impact;
    if (!name[0])
        return 0;
    fx->name = name;
    fx->pos[0] = spine[0];
    fx->pos[1] = spine[1];
    fx->pos[2] = in_water ? water_z : ground;
    fx->water = in_water != 0;
    fx->slot = 2;
    return 1;
}
