/* footstep.c - the monster's footfalls and landings (footstep.h, docs/FOOTSTEP_NOTES.md).
 * [R] read in the ELF, [G] guessed. */
#include <string.h>
#include "footstep.h"

#define STILL 1e-6f                                  /* 0x358637BD: |getVelocity|^2 below it is "not moving" */
#define NODE_FOOT_LOCK (0x50u | 0x1080u)             /* the state flags the foot object locks the feet in */

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

/* [R] 0x33CA10: only for foot 0 (the argument as given), unless the debug byte; the foot object's state node
 * (+0x74, set from monster +0x10C by 0x33CCA0) must exist and have flags 0x50 or 0x1080, and the monster be still.
 * s2 = record 0 (1 mirrored), s1 the other; both feet's places (0x2E5C40) into the monster's frame (minus +0x2C, then
 * 0x101190: the dots with its axes); if s2's y < s1's, they swap: s2 is the foot further forward. s2: weight 1,
 * flags |= 1, its place, z = the ground (0x2E8530) + the ankle (+0x50). s1: with DisableFootDrag weight 0 (flags
 * kept); otherwise weight 1, flags |= 2, the drag set up (0x33C5A0, not ported), flags &= ~1. +0x6C = s2. */
int foot_cancel(FootState *s, int foot, const FootPose *m)
{
    if (foot != 0 || !(m->node_flags & NODE_FOOT_LOCK) || !still(m))
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
        dr->flags &= ~FOOT_PLANTED;
    }
    s->anchor = a;
    return a;
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
