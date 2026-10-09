/* climb.c - a monster on a building's face (climb.h; docs/FORMAT_MONSTER.md "Climbing"). */
#include <math.h>
#include <string.h>
#include "climb.h"
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

/* 0x2DDC40: can the monster be at pt on building bi? The box pt +- (4.1, 4.1, 4.5) must not reach into another
 * building (-5), the building must not be falling (-8). The game also asks the cells' flag 0x1000 (-6), the
 * building's attachments and other objects (-2), other monsters (-3) and the camera's reach (-9): none here. */
static int place_ok(const Scene *s, int bi, const float pt[3])
{
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
    return 1;
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
    int r = place_ok(s, spot.building, spot.pt);
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

int climb_can_climb(const Mount *m, const Scene *s, const Climber *c, int dv, int dh)
{
    (void)c;
    const Building *b = building_of(s, m->at.building);
    if (!m->mounted || !b)
        return -4;
    float pt[3];
    memcpy(pt, m->at.pt, sizeof pt);
    if (dh) {                                        /* along the face: hop to the next rail, or swing round a corner */
        int to = m->at.rail + dh, kind = rail_kind(b, to);
        if ((kind & (dh < 0 ? 3 : 5)) == 0)
            return -7;
        for (uint32_t i = 0; i < s->nbuildings; i++) {   /* a building tall enough close beside: -5 */
            const Building *o = &s->buildings[i].b;
            if ((int)i == m->at.building || !standing(o) || o->max[2] < pt[2] || pt[1] < o->min[1] - 8 ||
                pt[1] > o->max[1] + 8)
                continue;
            if (dh < 0 ? o->max[0] <= pt[0] && o->max[0] > pt[0] - 8 : o->min[0] >= pt[0] && o->min[0] < pt[0] + 8)
                return -5;
        }
        pt[0] += RAIL * (float)dh;
        if (m->from_top)
            pt[2] -= REACH;
        int r = place_ok(s, m->at.building, pt);
        if (r != 1)
            return r;
        return to == (dh < 0 ? 0 : b->width) || m->at.rail == (dh < 0 ? b->width : 0) ? 4 : 3;
    }
    if (dv > 0) {                                    /* up: on climbing, or over the top onto the roof */
        if (pt[2] + 4 <= b->max[2]) {
            pt[2] += 0.75f;
            int r = place_ok(s, m->at.building, pt);
            return r == 1 || r == -9 ? 2 : r;
        }
        float top[3] = { pt[0], pt[1] + 2.5f, b->max[2] + 7.5f };
        int r = place_ok(s, m->at.building, top);
        return r == 1 ? 5 : r;
    }
    if (dv < 0) {                                    /* down: on climbing, or its feet are at the bottom: -1 */
        if (pt[2] - 7.5f <= b->min[2])
            return -1;
        pt[2] -= 0.75f;
        int r = place_ok(s, m->at.building, pt);
        return r == 1 || r == -9 || r == -5 ? 2 : r;
    }
    return 0;
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

void climb_clip_started(Mount *m)
{
    m->rail_changed = 0;
    m->from_top = 0;
}

void climb_place(Mount *m, const Scene *s, uint32_t flags, const float delta[3], float dt, float pos[3], float *heading)
{
    const Building *b = building_of(s, m->at.building);
    if (!b)
        return;
    float wall = climb_wall_heading(m, s);
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
            pos[2] = (flags & 0x100000) ? b->max[2] - REACH : fminf(pos[2], b->max[2] - REACH);
        }
        return;
    }
    if (m->mounted && (flags & 0x400) && !m->rail_changed)
        return;
    *heading = wall;
    if (m->mounted) {                                /* on the face: at its place, up and down by the clip */
        pos[0] = m->at.pt[0];
        pos[1] = m->at.pt[1];
        pos[2] = fmaxf(b->min[2], fminf(pos[2], b->max[2] - REACH));
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
    int r = place_ok(s, m->at.building, m->at.pt);
    if (r == 1 || r == -9)
        return -1;
    return r == -8 ? 73 : 31;                        /* knocked off: the falling building's slot, or the plain one */
}
