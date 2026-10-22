/* climb.c - a monster on a building's face (climb.h; docs/FORMAT_MONSTER.md "Climbing"). */
#include <math.h>
#include <string.h>
#include "climb.h"
#include "ik.h"
#include "moves.h"

#define RAIL 5.0f
#define REACH 11.5f                                  /* 3 + 4.5 + 4: how far above its place a face must go on */

static const Building *building_of(const Scene *s, int i)
{
    return i >= 0 && (uint32_t)i < s->nbuildings ? &s->buildings[i].b : NULL;
}

static int standing(const Building *b)               /* states 1 .. 3 */
{
    return b->state == BUILDING_STANDING || b->state == BUILDING_FLOOR_FALLING;
}

/* 0x2DDC40 [R]: can the monster be at pt on building bi? The box pt +- (4.1, 4.1, 4.5) must not reach into another
 * building (-5), the building must not be falling (-8), the cells of its front the box covers must not have flag
 * 0x1000 (-6: 0x2DDF60 .. 0x2DE0C4; FloorFlags, 0x2CC0C0, holds only 0 or 1 in every city and no script sets it, so
 * it never does), nor may the box reach an adornment of that building flagged 2 and not knocked off (-2: 0x2DE120,
 * the adornment's box through its matrix, its two corners), then the objects in the grid (flags 0x40040): another
 * monster (-3: object flag 0x40; one on a face counts by its mount point, one off a face by its place and only while
 * this one is off a face too; within 8.2 across and 9 up and down of pt) or an obstacle (-2: an electric side sign),
 * and a player's place must be in the camera's reach (-9: 0x336230, at most 70 high and 30 along x from the middle of
 * the players; not asked of an AI, +0x108) */
static int place_ok(const Scene *s, int bi, const float pt[3], const Climber *c)
{
    float focus_x = c->focus_x;
    float lo[3] = { pt[0] - 4.1f, pt[1] - 4.1f, pt[2] - 4.5f }, hi[3] = { pt[0] + 4.1f, pt[1] + 4.1f, pt[2] + 4.5f };
    for (uint32_t i = 0; i < s->nbuildings; i++) {
        const Building *o = &s->buildings[i].b;
        if ((int)i == bi || o->state == BUILDING_GONE)
            continue;
        int overlap = 1;
        for (int k = 0; k < 3; k++)
            overlap &= lo[k] < o->max[k] && hi[k] > o->min[k];
        if (overlap)
            return -5;
    }
    const Building *b = building_of(s, bi);
    if (!b || b->state == BUILDING_FALLING || b->state == BUILDING_GONE)
        return -8;
    const SceneBuilding *sb = &s->buildings[bi];
    for (int i = 0; i < sb->nadorn; i++) {
        const Adornment *a = &sb->adorn[i];
        if (!(a->flags & 2) || a->gone || a->inst < 0 || (uint32_t)a->inst >= s->ninstances)
            continue;
        const float *m = s->instances[a->inst].base.m;
        float A[3], B[3];
        for (int k = 0; k < 3; k++) {
            A[k] = m[k] * a->lo[0] + m[4 + k] * a->lo[1] + m[8 + k] * a->lo[2] + m[12 + k];
            B[k] = m[k] * a->hi[0] + m[4 + k] * a->hi[1] + m[8 + k] * a->hi[2] + m[12 + k];
        }
        if (lo[0] <= B[0] && lo[1] <= B[1] && lo[2] <= B[2] && !(hi[0] < A[0]) && !(hi[1] < A[1]) && !(hi[2] < A[2]))
            return -2;
    }
    for (int i = 0; i < c->nother; i++) {
        const float *op = c->other[i].pt, *olo = c->other[i].lo, *ohi = c->other[i].hi;
        int overlap = 1;                             /* (the grid's query: its object meets the box) */
        for (int k = 0; k < 3; k++)
            overlap &= lo[k] < ohi[k] && hi[k] > olo[k];
        if (!overlap || (c->mounted && !c->other[i].mounted))
            continue;
        float dx = op[0] - pt[0], dy = op[1] - pt[1];
        if (dx * dx + dy * dy < 67.24f && op[2] - 4.5f <= pt[2] + 4.5f && op[2] + 4.5f >= pt[2] - 4.5f)
            return -3;
    }
    for (uint32_t i = 0; s->obstacles && i < s->nobstacles; i++) {
        const float *o = s->obstacles[i];
        int overlap = 1;
        for (int k = 0; k < 3; k++)
            overlap &= lo[k] < o[3 + k] && hi[k] > o[k];
        if (overlap)
            return -2;
    }
    if (pt[2] > 70 || fabsf(pt[0] - focus_x) > 30)
        return -9;
    return 1;
}

int climb_place_ok(const Scene *s, int bi, const float pt[3], const Climber *c)
{
    return place_ok(s, bi, pt, c);
}

/* 0x2DD710: on a side in a spider state, the building of its row across the 10-unit alley that side looks onto */
static const Building *across(const Mount *m, const Scene *s)
{
    const Building *b = building_of(s, m->at.building);
    if (!b || !m->mounted || !m->spider || (m->at.rail != 0 && m->at.rail != b->width))
        return NULL;
    for (uint32_t i = 0; i < s->nbuildings; i++) {
        const Building *o = &s->buildings[i].b;
        if (o == b || o->row != b->row)
            continue;
        float gap = m->at.rail == 0 ? b->min[0] - o->max[0] : o->min[0] - b->max[0];
        if (fabsf(gap - 10) < 0.001f)
            return o;
    }
    return NULL;
}

static float top_of(const Mount *m, const Scene *s, const Building *b)   /* maxZ', with the spider's neighbour */
{
    const Building *o = across(m, s);
    return o ? fminf(b->max[2], o->max[2]) : b->max[2];
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* 0x2A4470: the nearest spot on a face of that side the monster looks at */
static int find_spot(const Mount *m, const Scene *s, const Climber *c, int side, float zoff, MountSpot *out)
{
    float h = c->heading / 57.2957795f, rx = cosf(h), ry = sinf(h), fx = -ry, fy = rx;
    float px = c->pos[0] + c->probe[0] * rx + c->probe[1] * fx, py = c->pos[1] + c->probe[0] * ry + c->probe[1] * fy;
    float r = c->half_width, best = 1e30f;
    int found = 0;
    for (uint32_t i = 0; i < s->nbuildings; i++) {
        const Building *b = &s->buildings[i].b;
        if (!standing(b) || (int)i == m->at.building)
            continue;
        float df = b->min[1] - py, dl = b->min[0] - px, dr = px - b->max[0];
        int in_x = px >= b->min[0] && px <= b->max[0], rail = -1, k = 6;
        float side_reach = m->mounted ? 5.5f : 1.1f * r;
        if (side == FACE_FRONT && df > 0 && df <= 1.5f * r && in_x)
            rail = clampi((int)floorf((px - b->min[0]) / RAIL + 0.5f), 1, b->width - 1);
        else if (side == FACE_LEFT && dl > 0 && py >= b->min[1] && dl <= side_reach)
            rail = 0;
        else if (side == FACE_RIGHT && dr > 0 && py >= b->min[1] && dr <= side_reach)
            rail = b->width;
        else if (side == FACE_ROOF && in_x && -df >= 0 && -df <= 1.5f * r && b->max[2] - c->pos[2] <= 1) {
            rail = clampi((int)floorf((px - b->min[0]) / RAIL + 0.5f), 1, b->width - 1);
            k = -8;
        }
        if (rail < 0)
            continue;
        if (side != FACE_ROOF && b->max[2] < c->pos[2] + REACH + zoff)
            continue;                                /* not tall enough */
        int storey = clampi((int)floorf((c->pos[2] + (float)k - b->min[2] - 6) / 3 + 0.5f), 0, b->n);
        MountSpot spot = { (int)i, { b->min[0] + RAIL * (float)rail, b->min[1], b->min[2] + 6 + 3 * (float)storey },
                           rail, storey };
        float d = 0;
        for (int a = 0; a < 3; a++)
            d += (spot.pt[a] - c->pos[a]) * (spot.pt[a] - c->pos[a]);
        if (d < best) {
            best = d;
            *out = spot;
            found = 1;
        }
    }
    return found;
}

int climb_can_mount(Mount *m, const Scene *s, const Climber *c, int side, float zoff)
{
    if (side != FACE_LEFT && side != FACE_RIGHT && side != FACE_FRONT && side != FACE_ROOF)
        return -7;
    MountSpot spot;
    if (!find_spot(m, s, c, side, zoff, &spot))
        return -7;
    int r = place_ok(s, spot.building, spot.pt, c);
    if (r == 1) {
        m->cand = spot;
        m->have_cand = 1;
        m->from_roof = side == FACE_ROOF;
    }
    return r;
}

/* 0x2D6560: what rail + dh is: 1 the front, 2 the left side, 4 the right side, 0 off the building */
static int rail_kind(const Building *b, int rail)
{
    return rail >= 1 && rail <= b->width - 1 ? 1 : rail == 0 ? 2 : rail == b->width ? 4 : 0;
}

int climb_can_climb(const Mount *m, const Scene *s, const Climber *c, int dv, int dh, int spider)
{
    const Building *b = building_of(s, m->at.building);
    if (!m->mounted || !b)
        return -4;
    float pt[3];
    memcpy(pt, m->at.pt, sizeof pt);
    if (dh) {                                        /* along the face: hop to the next rail, or swing round a corner */
        int to = m->at.rail + dh, kind = rail_kind(b, to);
        if ((kind & (dh < 0 ? 3 : 5)) == 0)
            return -7;
        /* a building close beside that way: -5 [R 0x2DD160 .. 0x2DD198, 0x2DD298 .. 0x2DD2D0 -> 0x2A3BA0]: a ray
         * along x from the place, 1 towards the street (1 into the building when the step turns onto the end rail),
         * meets the first building whose y range holds it; it counts when its top is at least 4.5 below the place,
         * its gap within 8: from the place, or round a corner from this building's own edge */
        int corner = (kind & (dh < 0 ? 2 : 4)) != 0, near = -1;
        float y = pt[1] + (corner ? 1.0f : -1.0f), nx = dh < 0 ? -1e30f : 1e30f;
        for (uint32_t i = 0; i < s->nbuildings; i++) {
            const Building *o = &s->buildings[i].b;
            if ((int)i == m->at.building || !standing(o) || y < o->min[1] || y > o->max[1])
                continue;
            if (dh < 0 ? o->min[0] < pt[0] && o->max[0] > nx : o->max[0] > pt[0] && o->min[0] < nx) {   /* (one the
                                                      * ray starts in: met at once, its gap below 0) */
                nx = dh < 0 ? o->max[0] : o->min[0];
                near = (int)i;
            }
        }
        if (near >= 0 && s->buildings[near].b.max[2] >= pt[2] - 4.5f) {
            float from = corner ? (dh < 0 ? b->min[0] : b->max[0]) : pt[0];
            if ((dh < 0 ? from - nx : nx - from) < 8)
                return -5;
        }
        pt[0] += RAIL * (float)dh;
        if (m->from_top)
            pt[2] -= REACH;
        int r = place_ok(s, m->at.building, pt, c);
        if (r != 1)
            return r;
        return to == (dh < 0 ? 0 : b->width) || m->at.rail == (dh < 0 ? b->width : 0) ? 4 : 3;
    }
    if (dv > 0) {                                    /* up: on climbing, or over the top onto the roof */
        float roof = spider ? top_of(m, s, b) : b->max[2];
        if (pt[2] + 4 <= roof) {
            pt[2] += 0.75f;
            int r = place_ok(s, m->at.building, pt, c);
            return r == 1 || r == -9 ? 2 : r;
        }
        float top[3] = { pt[0], pt[1] + 2.5f, roof + 7.5f };
        int r = place_ok(s, m->at.building, top, c);
        return r == 1 ? 5 : r;
    }
    if (dv < 0) {                                    /* down: on climbing, or its feet are at the bottom: -1 */
        if (pt[2] - 7.5f <= b->min[2])
            return -1;
        pt[2] -= 0.75f;
        int r = place_ok(s, m->at.building, pt, c);
        return r == 1 || r == -9 || r == -5 ? 2 : r;
    }
    return 0;
}

/* 0x2A41F0: the spot on the next building of its row that way (right: +x), 10 (9.75 .. 10.25) from its place along
 * the ground: from the front's end rail onto the front of a building it touches, or from a side across the alley onto
 * the other's side. The building must stand at least 4 above the place; the storey is kept. */
static int transfer_spot(const Mount *m, const Scene *s, int right, MountSpot *out)
{
    const Building *b = building_of(s, m->at.building);
    if (!b)
        return 0;
    float best = 1e30f;
    int found = 0;
    for (uint32_t i = 0; i < s->nbuildings; i++) {
        const Building *o = &s->buildings[i].b;
        if ((int)i == m->at.building || !standing(o) || o->row != b->row || m->at.pt[2] + 4 - o->max[2] > 0)
            continue;
        int rail;
        if (o->min[0] - m->at.pt[0] > 0) {           /* to the right: from the left side never */
            if (!right || m->at.rail == 0)
                continue;
            rail = m->at.rail == b->width ? 0 : 1;
        } else if (o->max[0] - m->at.pt[0] < 0) {    /* to the left: from the right side never */
            if (right || m->at.rail == b->width)
                continue;
            rail = m->at.rail == 0 ? o->width : o->width - 1;
        } else
            continue;
        float pt[3] = { o->min[0] + RAIL * (float)rail, o->min[1], m->at.pt[2] };
        float d = (pt[0] - m->at.pt[0]) * (pt[0] - m->at.pt[0]) + (pt[1] - m->at.pt[1]) * (pt[1] - m->at.pt[1]);
        if (d > 10.25f * 10.25f || d < 9.75f * 9.75f || d >= best)
            continue;
        best = d;
        MountSpot spot = { (int)i, { pt[0], pt[1], pt[2] }, rail, m->at.storey };
        *out = spot;
        found = 1;
    }
    return found;
}

int climb_can_transfer(Mount *m, const Scene *s, const Climber *c, int right)
{
    MountSpot spot;
    if (!m->mounted || !transfer_spot(m, s, right, &spot))
        return -7;
    int r = place_ok(s, spot.building, spot.pt, c);
    if (r == 1) {
        m->xfer = spot;
        m->have_xfer = 1;
    }
    return r;
}

void climb_transfer(Mount *m)
{
    if (m->have_xfer)
        m->at = m->xfer;
}

void climb_prepare(Mount *m)
{
    if (!m->have_cand) {
        m->at.building = -1;
        m->mounted = m->from_roof = 0;
        return;
    }
    m->at = m->cand;
    m->mounted = 0;
}

float climb_wall_heading(const Mount *m, const Scene *s)
{
    const Building *b = building_of(s, m->at.building);
    if (!b)
        return 0;
    float h = m->at.rail == 0 ? 270.0f : m->at.rail >= b->width ? 90.0f : 0.0f;   /* facing +x, -x, +y */
    if (!m->mounted && m->from_roof)
        h += 180;
    return fmodf(h, 360);
}

void climb_mount(Mount *m, const Climber *c, float t)
{
    if (m->at.building < 0)
        return;
    m->mounted = 1;
    m->from_roof = 0;
    m->have_cand = 0;
    if (t <= 0.001f)
        return;                                      /* transitionToMount is only called for a time */
    m->move_time = m->move_left = t;
    memcpy(m->move_from, c->pos, sizeof m->move_from);
    m->move_to[0] = m->at.pt[0];
    m->move_to[1] = m->at.pt[1];
    m->move_to[2] = c->pos[2];
    m->heading_from = c->heading;
}

void climb_unmount(Mount *m)
{
    m->at.building = -1;
    m->mounted = m->from_roof = 0;
    m->move_left = 0;
}

void climb_change_rail(Mount *m, int dir)
{
    m->at.rail += dir;
    m->at.pt[0] += RAIL * (float)dir;
    m->rail_changed = 1;
}

void climb_clip_started(Mount *m, const char *state)
{
    m->rail_changed = 0;
    m->from_top = 0;
    m->spider = state && (!strcmp(state, "Spider_Climb_Down") || !strcmp(state, "Spider_Climb_Up") ||
                          !strcmp(state, "Spider_Idle"));      /* the names at 0x3F5938 */
}

void climb_place(Mount *m, const Scene *s, uint32_t flags, const float delta[3], float dt, float pos[3], float *heading)
{
    const Building *b = building_of(s, m->at.building);
    if (!b)
        return;
    float wall = climb_wall_heading(m, s), top = top_of(m, s, b), bottom = b->min[2];
    const Building *o = across(m, s);
    if (o)
        bottom = fmaxf(bottom, o->min[2]);
    if (m->move_left > 0) {                          /* transitionToMount: from where it was to its place */
        for (int k = 0; k < 3; k++)
            m->move_from[k] += delta[k];             /* the clip's root still moves the start */
        m->move_left -= dt;
        float f = m->move_left > 0 ? m->move_left / m->move_time : 0;
        for (int k = 0; k < 3; k++)
            pos[k] = m->move_to[k] + (m->move_from[k] - m->move_to[k]) * f;
        float turn = fmodf(m->heading_from - wall + 540, 360) - 180;
        *heading = wall + turn * f;
    }
    if (m->mounted)                                  /* 0x2E14F0: its place's height, from where the clip took it */
        m->at.pt[2] = fmaxf(pos[2] + 7.5f, 7.5f);    /* (before it is held below the top: a stroke past it mantles) */
    if ((flags & 0x40400) == 0x40400)                /* the clip has it all */
        return;
    if (m->from_roof) {                              /* coming over the roof's edge */
        *heading = wall;
        if (m->from_top) {
            pos[0] = m->at.pt[0];
            pos[1] = m->at.pt[1];
        }
        pos[2] = b->max[2];
        return;
    }
    if (m->mounted && (flags & 0x40000)) {           /* a clip that bakes: free, but kept below the top */
        if ((flags & 0x120000) && !m->rail_changed) {
            pos[0] = m->at.pt[0];
            pos[1] = m->at.pt[1];
            pos[2] = (flags & 0x100000) ? top - REACH : fminf(pos[2], top - REACH);
        }
        return;
    }
    if (m->mounted && (flags & 0x400) && !m->rail_changed)
        return;
    *heading = wall;
    if (m->mounted) {                                /* on the face: at its place, up and down by the clip */
        pos[0] = m->at.pt[0];
        pos[1] = m->at.pt[1];
        pos[2] = fmaxf(bottom, fminf(pos[2], top - REACH));
    } else if (m->at.rail >= 1 && m->at.rail < b->width) {
        pos[0] = m->at.pt[0];                        /* getting on: lined up with the rail */
    } else {
        pos[1] = m->at.pt[1];
    }
}

int climb_check(Mount *m, const Scene *s, const Climber *c, int mode)
{
    if (m->mounted && mode != MODE_BUILDING) {
        climb_unmount(m);
        return -1;
    }
    const Building *b = building_of(s, m->at.building);
    if (!m->mounted || !b)
        return -1;
    m->at.storey = clampi((int)((c->pos[2] + 3) / 3), 0, b->n - 1 > 0 ? b->n - 1 : 0);
    const Building *o = across(m, s);                /* braced across the alley (0x2E15D8 .. 0x2E1610): no higher than
                                                      * that building's top storey */
    if (o && m->at.storey > o->n - 1)
        m->at.storey = o->n - 1;
    int r = place_ok(s, m->at.building, m->at.pt, c);
    if (o && o->state >= BUILDING_FALLING)           /* that building coming down knocks it off as its own would
                                                      * (0x2E1640 .. 0x2E1658: -8, slot 73) */
        r = -8;
    if (!c->in_water && (r == 1 || r == -9))         /* climbing down into a water: let go [R 0x2E1660] */
        return -1;
    return r == -8 ? 73 : 31;                        /* knocked off: the falling building's slot, or the plain one */
}

/* ---------------------------------------------------------------- the hands */

float hand_weight(const Hand *h)
{
    if (h->remaining <= 0)
        return h->target;
    if (h->remaining >= h->duration)
        return h->start;
    float u = h->remaining / h->duration, s = (3 - 2 * u) * u * u;
    return h->target * (1 - s) + h->start * s;
}

int hand_attach(Hand *h, float time, const float pos[3], const float q[4])
{
    if (time <= 0) {
        if (h->attaching || h->planted)
            return 0;
        time = 0.2f;
    }
    h->attaching = 1;
    h->start = hand_weight(h);
    h->target = 1;
    h->remaining = h->duration = h->start >= 1 ? 0 : time;
    memcpy(h->tp, pos, sizeof h->tp);
    memcpy(h->tq, q, sizeof h->tq);
    return 1;
}

void hand_detach(Hand *h, float time)
{
    h->start = hand_weight(h);
    h->target = 0;
    h->attaching = h->planted = 0;
    h->remaining = h->duration = h->start <= 0 ? 0 : time;
}

void climb_hand_snap(const Mount *m, const Scene *s, int hand, float thick, float pos[3])
{
    const Building *b = building_of(s, m->at.building);
    if (!b)
        return;
    const float *M = m->at.pt;
    if (m->at.rail == 0) {
        pos[0] = hand == 1 ? M[0] : M[0] - thick;
        pos[1] = hand == 1 ? M[1] : M[1] + 2.5f;
    } else if (m->at.rail == b->width) {
        pos[0] = hand == 0 ? M[0] : M[0] + thick;
        pos[1] = hand == 0 ? M[1] : M[1] + 2.5f;
    } else {
        pos[1] = M[1] - thick;
        pos[0] = fmaxf(b->min[0] + 2 * thick, fminf(pos[0], b->max[0] - 2 * thick));
    }
    float lo = b->min[2], hi = b->max[2];
    const Building *o = across(m, s);
    if (o) {
        lo = fmaxf(lo, o->min[2]);
        hi = fminf(hi, o->max[2]);
    }
    pos[2] = fmaxf(lo + 1.5f, fminf(pos[2], hi - 1.5f));
}

void hand_update(Hand *h, int hand, const Mount *m, const Scene *s, float thick, const float anim[3], float dt)
{
    int follow = 0;
    if (h->remaining > 0) {
        h->remaining -= dt;
        if (h->remaining <= 0) {
            h->remaining = 0;
            if (h->target == 1) {
                h->planted = 1;
                h->attaching = 0;
            }
        } else
            follow = h->attaching;
    } else
        follow = h->target == 1 && h->attaching;
    if (follow) {
        memcpy(h->tp, anim, sizeof h->tp);
        climb_hand_snap(m, s, hand, thick, h->tp);
    }
    if (h->planted)
        climb_hand_snap(m, s, hand, thick, h->tp);
    if (h->valid) {                                  /* 0.3 (gp - 0x7130) of the last place stays, a frame of the
                                                      * monster's update: a world step, 1/30 s [R 0x2B7800] */
        float keep = powf(0.3f, dt * 30);
        for (int k = 0; k < 3; k++)
            h->sp[k] = h->tp[k] + (h->sp[k] - h->tp[k]) * keep;
        ik_slerp(h->tq, h->sq, keep, h->sq);
    }
    /* 0x322960: what the solver gets */
    if (hand_weight(h) <= 0)
        h->valid = 0;
    else if (!h->valid) {
        memcpy(h->sq, h->tq, sizeof h->sq);
        memcpy(h->sp, h->tp, sizeof h->sp);
        h->valid = 1;
    }
}
