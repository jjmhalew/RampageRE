/* look.c - the face queue, the head turn and the velocity's friction (look.h, docs/LOOK_NOTES.md).
 * [R] read in the ELF, [G] guessed. */
#include <math.h>
#include <string.h>
#include "look.h"

#define PI 3.14159265f

/* ---- 1. the face ---------------------------------------------------------------------------------------------- */

void face_init(FaceQueue *q)
{
    memset(q, 0, sizeof *q);
}

static FaceRec rec_of(const void *node, const char *name, int head, int loop, int plays)
{
    FaceRec r;
    r.node = node;
    r.name = name;
    r.head = head != 0;                              /* [R] 0x2E8FB0 / 0x2E9350: the record's mask (+0xC) or none */
    r.loop = loop != 0;                              /* [R] +0x1C */
    r.plays = plays;                                 /* [R] +0x20 */
    return r;
}

/* [R] 0x2E9350 (setFacialAnim after the name lookup 0x2E9200: hash % 97 at monster +0x124, any case): a record off
 * the free list (+0xBBE0; none free: return at once), filled, its clip started on the face layer (0x2FF820: the
 * node's clip at the node's rate, blended over node +0x14 when > 0) with its events (0x2F45D0, +0x7DE8); then the
 * record at the front of the list (+0xBBD8) goes back to the free list and the new one takes the front. */
int face_set(FaceQueue *q, const void *node, const char *name, int head, int loop, int plays)
{
    if (!node || q->n >= FACE_RECORDS)
        return FACE_NONE;
    if (q->n == 0)
        q->n = 1;
    q->list[0] = rec_of(node, name, head, loop, plays);
    return FACE_START;
}

/* [R] 0x2E8FB0 (queueFacialAnim, Monster script method 0x7E -> 0x2ABAC0): the same lookup and record, appended at the
 * back (+0xBBDC); nothing is started. [R] Its head flag is written into the face layer's mask slot (+0xBBE8 + 4 x
 * the animation player's +0x72D0) at once, as setFacialAnim's is, so the face playing now would take the queued
 * one's head layer [G: not ported; no script calls queueFacialAnim (Monsters.scr and every .bsf: setFacialAnim only),
 * so the data never exercises it]. */
int face_queue(FaceQueue *q, const void *node, const char *name, int head, int loop, int plays)
{
    if (!node || q->n >= FACE_RECORDS)
        return 0;
    q->list[q->n++] = rec_of(node, name, head, loop, plays);   /* [R] with an empty list it becomes the front
                                                                   without being started (never: a face always plays) */
    return 1;
}

/* [R] 0x2E95D0, from the face layer when its clip ends (0x2FF880): the front record plays again while it loops or,
 * counted down, has plays left; otherwise it goes back to the free list and the next one starts, or with none
 * Monsters.bsf's DefaultFacialAnim runs (0x2E75C0 "DefaultFacialAnim": setFacialAnim("Expression_Idle", 0, 1, 0)). A
 * looping face never ends, so a queued one waits behind it for ever. */
int face_clip_end(FaceQueue *q)
{
    if (q->n == 0)
        return FACE_DEFAULT;
    FaceRec *r = &q->list[0];
    if (r->loop || --r->plays > 0)
        return FACE_RESTART;
    memmove(q->list, q->list + 1, sizeof *q->list * (size_t)(q->n - 1));
    q->n--;
    return q->n > 0 ? FACE_START : FACE_DEFAULT;
}

/* [R] Monsters.scr faceAnim: isLooping is 0 whatever param2 says; headLayer = param1 != 0; count = (int)param3; with
 * param2 > 1 gWorld.installObjectTimer("Monsters::appendBelch", monster, param2, 0, 1): one call that many seconds
 * later, setFacialAnim("Expression_Burp", 0, 0, 1). George's Chew after a throw: faceAnim(10, 0, 5, 1). */
const char *face_anim_event(FaceQueue *q, const float p[4], int *head, int *plays)
{
    static const struct { int id; const char *name; } FACES[] = {
        { 15, "Expression_Barf" }, { 1, "Expression_Burp" }, { 2, "Expression_Choke" }, { 3, "Expression_Yuck" },
        { 10, "Expression_Chew" }, { 13, "Expression_Chomp" }, { 5, "Expression_Flex" }, { 12, "Expression_Roar" },
        { 11, "Expression_Smile" }, { 8, "Expression_Spew" }, { 9, "Expression_Zap" }, { 18, "React_Intro" },
        { 19, "React_Victory" }, { 20, "Expression_Intro" } };
    const char *name = NULL;
    for (size_t k = 0; k < sizeof FACES / sizeof *FACES; k++)
        if (p[0] == (float)FACES[k].id)
            name = FACES[k].name;
    *head = p[1] != 0;
    *plays = (int)p[3];
    if (p[2] > 1.0f && q->nbelch < 4)                /* [R] the timer is installed even for an unknown number */
        q->belch[q->nbelch++] = p[2];
    return name;                                     /* [R] unknown: setFacialAnim("") finds no node, nothing plays */
}

int face_tick(FaceQueue *q, float dt)
{
    int fired = 0;
    for (int k = 0; k < q->nbelch;) {
        q->belch[k] -= dt;
        if (q->belch[k] <= 0) {
            q->belch[k] = q->belch[--q->nbelch];
            fired = 1;
        } else
            k++;
    }
    return fired;
}

/* ---- 2. the head ---------------------------------------------------------------------------------------------- */

void look_limits_default(LookLimits *l)
{
    l->yaw_rate = PI / 2;                            /* [R] MONSTER_DATA +0x174 (George, Lizzie, Ralph ...) */
    l->pitch_rate = PI / 2;                          /* [R] +0x178 */
    l->yaw_max = 1.4137170f;                         /* [R] +0xA08, 81 degrees */
    l->pitch_pos = l->pitch_neg = 0.7853980f;        /* [R] +0xA0C, +0xA10, 45 degrees */
}

static float wrap(float a)                           /* [R] once by 2 pi into -pi .. pi (0x2FBC90, 0x2FBE30) */
{
    if (a > PI)
        return a - 2 * PI;
    if (a < -PI)
        return a + 2 * PI;
    return a;
}

static float clampf(float a, float lo, float hi) { return a > hi ? hi : a < lo ? lo : a; }

/* [R] 0x2E70C0 in the command 0x2E6580: with node flag 0x200 every head animator gets +0x119 = the right stick off
 * centre and 0x2FBE30(pitch = ry x (+0xA0C if ry > 0 else +0xA10), yaw = -rx x +0xA08), the sticks being the clip
 * request's +0x24 / +0x28 / 255; without it +0x119 = 0 for all. 0x2FBE30 takes them while tracking is off or the
 * stick holds it: wrapped, the pitch kept to -A10 .. A0C, the yaw to +-A08. */
void look_stick(HeadLook *h, const LookLimits *l, int look_state, float rx, float ry)
{
    if (!look_state) {
        h->manual = 0;
        return;
    }
    h->manual = rx != 0 || ry != 0;
    if (h->enabled && !h->manual)
        return;
    float p = ry * (ry > 0 ? l->pitch_pos : l->pitch_neg), y = -rx * l->yaw_max;
    h->pitch_to = clampf(wrap(p), -l->pitch_neg, l->pitch_pos);
    h->yaw_to = clampf(wrap(y), -l->yaw_max, l->yaw_max);
}

/* [R] 0x2DE9C4: +0x118 = (the monster's target handle +0xADC holds an object, 0x2FF760) and (node flag 0x20); then
 * vtable +0x8 (0x2FBC90): off (or held) and not held: both targets 0. The handle is only ever cleared (0x2FF790) and
 * no node of the 30 monsters has 0x20, so tracking is never on. Returns 1 when look_track should run. */
int look_update(HeadLook *h, const LookLimits *l, int has_target, uint32_t node_flags)
{
    (void)l;
    h->enabled = has_target && (node_flags & 0x20);
    if (h->enabled && !h->manual)
        return 1;
    if (!h->manual)
        h->yaw_to = h->pitch_to = 0;
    return 0;
}

/* [R] 0x2FB950 + the rest of 0x2FBC90. Headings are measured from +y towards -x (atan2(-x, y), the asin cases at
 * 0x2FB9F0), elevations as pi / 2 - acos(z / |v|). yaw = heading(target - neck) - heading(fwd), pitch =
 * elevation(target - head) - elevation(fwd); then the pitch target is wrapped and kept to -A10 .. A0C, while the
 * clamp to +-A08 falls on the yaw now (+0xBC), not its target (+0xC0), which is not wrapped either [R, kept]. The
 * places are the last built pose's [R: +0x80 .. +0x8C and +0xB0 .. +0xB8 are stored only by the pose build 0x2FBF90
 * (0x2FC2A8 .., 0x2FC36C ..), which MonsterXFormTree runs when it draws; 0x2FB950 reads them in the update]. */
void look_track(HeadLook *h, const LookLimits *l, const float target[3], const float neck[3], const float head[3],
                const float fwd[3])
{
    float hf = 0, ef, hd = 0, ed = 0;
    if (sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]) > 0.001f)
        hf = atan2f(-fwd[0], fwd[1]);
    ef = PI / 2 - acosf(clampf(fwd[2], -1, 1));
    float d[3] = { target[0] - neck[0], target[1] - neck[1], target[2] - neck[2] };
    if (sqrtf(d[0] * d[0] + d[1] * d[1]) > 0.001f)
        hd = atan2f(-d[0], d[1]);
    float e[3] = { target[0] - head[0], target[1] - head[1], target[2] - head[2] };
    float le = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    if (le > 0)
        ed = PI / 2 - acosf(clampf(e[2] / le, -1, 1));
    h->yaw_to = hd - hf;
    h->pitch_to = clampf(wrap(ed - ef), -l->pitch_neg, l->pitch_pos);
    h->yaw = clampf(wrap(h->yaw), -l->yaw_max, l->yaw_max);
}

static float approach(float now, float to, float step)
{
    if (now < to)
        return now + step > to ? to : now + step;
    if (now > to)
        return now - step < to ? to : now - step;
    return now;
}

/* [R] 0x2FBF90: the head bone (+0x1C) eases the pitch at +0x178 x dt, the neck (+0x20) the yaw at +0x174 x dt,
 * each halved while the stick holds it (+0x119); never past the target. No weight: the turn is the angles alone. */
void look_step(HeadLook *h, const LookLimits *l, float dt)
{
    float k = h->manual ? 0.5f : 1.0f;
    h->pitch = approach(h->pitch, h->pitch_to, l->pitch_rate * dt * k);
    h->yaw = approach(h->yaw, h->yaw_to, l->yaw_rate * dt * k);
}

static Mat4 rz(float a)                              /* column vectors: about z, counter-clockwise */
{
    Mat4 m = mat_identity();
    float c = cosf(a), s = sinf(a);
    m.m[0] = c; m.m[1] = s; m.m[4] = -s; m.m[5] = c;
    return m;
}

static Mat4 rigid_inverse(const Mat4 *m)
{
    Mat4 o = mat_identity();
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            o.m[4 * r + c] = m->m[4 * c + r];
    for (int r = 0; r < 3; r++)
        o.m[12 + r] = -(o.m[r] * m->m[12] + o.m[4 + r] * m->m[13] + o.m[8 + r] * m->m[14]);
    return o;
}

/* [R] 0x2FBF90 per bone as the hierarchy is built (row vectors, 3 x 3 only, the place kept): the neck's model matrix
 * M x Rz(yaw) (0x2FC7DC), the head's Rz(-pitch) x M (0x2FC508). Here on the finished pose: the neck turns about the
 * model's up through its own place, the head's new model matrix is the turned neck x its old local, then turned
 * about its own z; the nodes below follow (as 0x2F9320 does for the IK). */
void look_pose(Mat4 *node, const Tree *tree, int neck, int head, float yaw, float pitch)
{
    if (!tree || tree->n > 256 || head < 0 || (uint32_t)head >= tree->n || (yaw == 0 && pitch == 0))
        return;
    if (neck >= 0 && (uint32_t)neck >= tree->n)
        return;
    static Mat4 old[256];
    static uint8_t moved[256];
    memcpy(old, node, sizeof *node * tree->n);
    memset(moved, 0, tree->n);
    if (neck >= 0 && neck != head) {
        Mat4 m = node[neck];
        float t[3] = { m.m[12], m.m[13], m.m[14] };
        m.m[12] = m.m[13] = m.m[14] = 0;
        m = mat_mul(rz(yaw), m);
        m.m[12] = t[0]; m.m[13] = t[1]; m.m[14] = t[2];
        node[neck] = m;
        moved[neck] = 1;
        /* the head rides on the turned neck (its parent, or further down) */
        node[head] = mat_mul(node[neck], mat_mul(rigid_inverse(&old[neck]), old[head]));
        node[head] = mat_mul(node[head], rz(-pitch));
    } else {                                         /* [R] one bone: Rz(yaw) about up, then -pitch about its z */
        Mat4 m = node[head];
        float t[3] = { m.m[12], m.m[13], m.m[14] };
        m.m[12] = m.m[13] = m.m[14] = 0;
        m = mat_mul(mat_mul(rz(yaw), m), rz(-pitch));
        m.m[12] = t[0]; m.m[13] = t[1]; m.m[14] = t[2];
        node[head] = m;
    }
    moved[head] = 1;
    for (uint32_t i = 0; i < tree->n; i++) {         /* below them: each keeps its place relative to its parent */
        int p = tree->nodes[i].parent;
        if (moved[i] || p < 0 || (uint32_t)p >= i || !moved[p])
            continue;
        node[i] = mat_mul(node[p], mat_mul(rigid_inverse(&old[p]), old[i]));
        moved[i] = 1;
    }
}

/* ---- 3. the velocity ------------------------------------------------------------------------------------------ */

/* [R] 0x2DF6F0 (modes 0, 2 and 4; the transition timer +0xBA4C of transitionToMount runs the same friction): +0xBD38
 * = 0 unless +0x110; with |v|^2 >= 1e-6 the place moves by v x dt, then d = v / |v| (0x2DFD40) and v - d x
 * (+0xF4) x dt is the new velocity, z only with node flags 0x80002 (otherwise its z stays d's z in the test below:
 * the game's own quirk, kept). Its dot with the old one <= 0: the velocity becomes 0 (0x409FF8). */
void monster_ground_velocity(float vel[3], float pos[3], float friction, float dt, uint32_t node_flags,
                             int slot_pending)
{
    int with_z = (node_flags & MONSTER_FRICTION_Z) != 0;
    if (!slot_pending)
        vel[2] = 0;
    float l2 = vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2];
    if (l2 < 1e-6f)
        return;
    if (pos) {
        pos[0] += vel[0] * dt;
        pos[1] += vel[1] * dt;
        if (with_z)
            pos[2] += vel[2] * dt;
    }
    float r = 1 / sqrtf(l2), d[3] = { vel[0] * r, vel[1] * r, vel[2] * r };
    float n[3] = { vel[0] - d[0] * friction * dt, vel[1] - d[1] * friction * dt,
                   with_z ? vel[2] - d[2] * friction * dt : d[2] };
    if (n[0] * vel[0] + n[1] * vel[1] + n[2] * vel[2] <= 0) {
        vel[0] = vel[1] = vel[2] = 0;
        return;
    }
    vel[0] = n[0];
    vel[1] = n[1];
    if (with_z)
        vel[2] = n[2];
}

/* [R] 0x2DEBD8 (mode 1): with |v|^2 >= 1e-6, v -= v / |v| x 0.002 x |v|^2 x dt (0x2DECC4, all three axes); the place
 * moves by v x dt (0x2DF088); then 60 x dt off the up velocity unless +0xAB8 (0x2DF118). */
void monster_air_velocity(float vel[3], float pos[3], float dt, int no_gravity)
{
    float l2 = vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2];
    if (l2 >= 1e-6f) {
        float k = MONSTER_AIR_DRAG * l2 * dt / sqrtf(l2);
        for (int i = 0; i < 3; i++)
            vel[i] -= vel[i] * k;
    }
    if (pos)
        for (int i = 0; i < 3; i++)
            pos[i] += vel[i] * dt;
    if (!no_gravity)
        vel[2] -= 60.0f * dt;
}

/* [R] 0x2E0330: z against 0x2E8530 (the ground under the monster): within 0.001 it is the ground; otherwise, gravity
 * on, 0.3 x ground + 0.7 x z, per frame (not scaled by dt) */
float monster_ground_follow(float z, float ground, int no_gravity)
{
    if (fabsf(ground - z) < 0.001f)
        return ground;
    return no_gravity ? z : 0.3f * ground + 0.7f * z;
}

/* [R] 0x2D77D8 (0x2D6A50 on the ground; dist the attack record's +0x30 x its modifiers, time its +0x34) */
float monster_knock_ground(float vel[3], const float dir[3], float dist, float time)
{
    if (time <= 0)
        return MONSTER_FRICTION;
    float v = 2 * dist / time;
    for (int i = 0; i < 3; i++)
        vel[i] = dir[i] * v;
    return v / time;
}

/* [R] 0x2D7558 (mode 1 or 2): dir x dist / time, z = tan(+0x38 of the attack record) x |that|, then a spin (+0xBD3C ..
 * +0xBD50, not ported) and +0xF4 = 50 (0x2D77C8). +0x38 (0 or 1 in the data) goes to tanf (0x1576D8 at 0x2D7600) as
 * it is: radians, so 1 lifts the victim at tan 1 = 1.56 x its speed [R]. */
float monster_knock_air(float vel[3], const float dir[3], float dist, float time, float angle)
{
    if (time <= 0)
        return MONSTER_FRICTION;
    float v = dist / time;
    for (int i = 0; i < 3; i++)
        vel[i] = dir[i] * v;
    vel[2] = tanf(angle) * sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    return MONSTER_FRICTION;
}

/* [R] 0x2D7928: each axis of the knock replaces the velocity's when it is larger in size; +0xF4 = 50 (0x2D79E0) */
float monster_knock_shot(float vel[3], const float knock[3])
{
    for (int i = 0; i < 3; i++)
        if (fabsf(knock[i]) > fabsf(vel[i]))
            vel[i] = knock[i];
    return MONSTER_FRICTION;
}
