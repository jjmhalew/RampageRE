/* collide.c - the game's collision world: gWorld's RGridMgr (0x4D0900, gp - 0x6D60) and RSceneryMgr (0x4D0B00,
 * gp - 0x6D5C), the WCollisionInfo shapes, the SpherePhysicsMover and the monster's response (0x2E2C50). See
 * collide.h and docs/COLLIDE_NOTES.md; [R] read in RAMPAGE.ELF, [G] guessed. */
#include "collide.h"
#include <math.h>
#include <string.h>

#define BIG 1.7e38f

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float a[3], const float b[3], float o[3])
{
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static void copy3(float o[3], const float a[3]) { o[0] = a[0]; o[1] = a[1]; o[2] = a[2]; }

/* 0x101190: world direction to the object's frame (by the transposed axes) */
static void to_local(const float m[12], const float v[3], float o[3])
{
    float x = dot3(v, m), y = dot3(v, m + 3), z = dot3(v, m + 6);
    o[0] = x; o[1] = y; o[2] = z;
}
static void to_world_dir(const float m[12], const float l[3], float o[3])
{
    for (int k = 0; k < 3; k++)
        o[k] = l[0] * m[k] + l[1] * m[3 + k] + l[2] * m[6 + k];
}
static void point_local(const float m[12], const float p[3], float o[3])
{
    float d[3] = { p[0] - m[9], p[1] - m[10], p[2] - m[11] };
    to_local(m, d, o);
}
static void point_world(const float m[12], const float l[3], float o[3])
{
    float w[3];
    to_world_dir(m, l, w);
    for (int k = 0; k < 3; k++)
        o[k] = w[k] + m[9 + k];
}

void collide_matrix(float m[12], float heading_deg, const float pos[3])
{
    float h = heading_deg / 57.2957795f, c = cosf(h), s = sinf(h);
    float r[12] = { c, s, 0, -s, c, 0, 0, 0, 1, pos[0], pos[1], pos[2] };
    memcpy(m, r, sizeof r);
}

/* ---------------------------------------------------------------- shapes (WCollisionInfo, vtable 0x449000) */

void collide_shape_sphere(CollideShape *s, const float centre[3], float r)
{
    memset(s, 0, sizeof *s);                         /* 0x281110: type 0; the input (centre, r^2) kept at +0x28 */
    s->type = COLLIDE_SPHERE;
    copy3(s->centre, centre);
    s->radius = r;
    s->half[0] = s->half[1] = s->half[2] = r;
    copy3(s->raw, centre);
    s->raw[3] = r * r;
}

void collide_shape_box(CollideShape *s, const float min[3], const float max[3])
{
    memset(s, 0, sizeof *s);                         /* 0x281170: type 1, half sizes (0x103970), centre (0x1039C0) */
    s->type = COLLIDE_BOX;
    for (int k = 0; k < 3; k++) {
        s->half[k] = (max[k] - min[k]) / 2;
        s->centre[k] = (max[k] + min[k]) / 2;
        s->raw[k] = min[k];
        s->raw[3 + k] = max[k];
    }
    s->radius = sqrtf(dot3(s->half, s->half));
}

void collide_shape_pick(CollideShape *s, const float bound_c[3], float bound_r,
                        const float *tmpl_box, float tmpl_radius, int tmpl_auto,
                        const float *inst_box, float inst_radius, int inst_auto, const float *auto_box)
{
    static const float zero[3] = { 0, 0, 0 };
    const float *bc = bound_c ? bound_c : zero;
    float br = bound_c ? bound_r : 1;
    int set = 0;
    for (int pass = 0; pass < 2; pass++) {           /* the template's book, then the instance's (+0x80, +0x7C) */
        const float *box = pass ? inst_box : tmpl_box;
        float rad = pass ? inst_radius : tmpl_radius;
        int aut = pass ? inst_auto : tmpl_auto;
        if (box) {                                   /* CollisionBox (min x y z, max x y z) */
            collide_shape_box(s, box, box + 3);
            set = 1;
        } else if (rad >= 0) {                       /* CollisionRadius about the model's sphere's centre */
            collide_shape_sphere(s, bc, rad);
            set = 1;
        } else if (aut && auto_box) {                /* AutoCollisionBox: the model's box (its vtable +0x18) */
            collide_shape_box(s, auto_box, auto_box + 3);
            set = 1;
        }
    }
    if (!set)
        collide_shape_sphere(s, bc, br);
}

float collide_mover_radius(const CollideShape *s)
{
    float v = s->raw[3];                             /* +0x34: r^2, or a box's max x */
    return v > 0 ? sqrtf(v) : 0;
}

/* 0x281800 + 0x281230: box (centre d from the shape's box, half b) against the shape's box (half a), both aligned:
 * the axis of the smallest overlap; push moves the first box out, along d's side */
static int box_box(const float d[3], const float a[3], const float b[3], float push[3], float normal[3], float point[3])
{
    float o[3];
    for (int k = 0; k < 3; k++) {
        o[k] = a[k] + b[k] - fabsf(d[k]);
        if (o[k] < 0)
            return 0;
    }
    int ax = o[0] <= o[1] ? (o[0] <= o[2] ? 0 : 2) : (o[1] <= o[2] ? 1 : 2);
    for (int k = 0; k < 3; k++) {
        float sg = d[k] < 0 ? -1.0f : 1.0f;
        push[k] = k == ax ? o[k] * sg : 0;
        normal[k] = k == ax ? sg : 0;
        point[k] = a[k] * sg;                        /* the shape's corner towards it [R] */
    }
    return 1;
}

/* 0x2815F0 + 0x281940: a sphere (centre d from the box's centre, radius r) against an aligned box of half h; push
 * moves the sphere out, point the box's nearest point. Inside on every axis: out by the least depth; outside on one
 * axis: straight out of that face; on two or three: from the nearest edge or corner */
static int sphere_box(const float d[3], const float h[3], float r, float push[3], float normal[3], float point[3])
{
    float ad[3], q[3];
    int inside = 0;
    for (int k = 0; k < 3; k++) {
        ad[k] = fabsf(d[k]);
        if (ad[k] > h[k] + r)
            return 0;
        q[k] = ad[k] - h[k];
        inside += q[k] <= 0;
    }
    float p[3] = { 0, 0, 0 }, n[3] = { 0, 0, 0 }, c[3];
    for (int k = 0; k < 3; k++)
        c[k] = ad[k] < h[k] ? ad[k] : h[k];
    if (inside >= 2) {
        int ax = 0;
        if (inside == 3) {                           /* the least deep */
            ax = q[0] >= q[1] ? (q[0] >= q[2] ? 0 : 2) : (q[1] >= q[2] ? 1 : 2);
        } else
            ax = q[0] > 0 ? 0 : q[1] > 0 ? 1 : 2;   /* the one axis it is outside on */
        p[ax] = r - q[ax];
        n[ax] = 1;
        c[ax] = h[ax];
    } else {
        for (int k = 0; k < 3; k++)
            if (q[k] < 0)
                q[k] = 0;
        float l2 = dot3(q, q);
        if (l2 >= r * r)
            return 0;
        float l = sqrtf(l2);
        if (l <= 0)
            return 0;
        for (int k = 0; k < 3; k++) {
            n[k] = q[k] / l;
            p[k] = n[k] * (r - l);
        }
    }
    for (int k = 0; k < 3; k++) {
        float sg = d[k] < 0 ? -1.0f : 1.0f;
        push[k] = p[k] * sg;
        normal[k] = n[k] * sg;
        point[k] = c[k] * sg;
    }
    return 1;
}

/* 0x2813F0: sphere (centre d from the other's) against sphere */
static int sphere_sphere(const float d[3], float r1, float r2, float push[3], float normal[3], float point[3])
{
    float l2 = dot3(d, d), rr = r1 + r2;
    if (l2 >= rr * rr)
        return 0;
    float l = sqrtf(l2);
    float n[3] = { 0, 0, 1 };
    if (l > 1e-6f)
        for (int k = 0; k < 3; k++)
            n[k] = d[k] / l;
    for (int k = 0; k < 3; k++) {
        normal[k] = n[k];
        push[k] = n[k] * (rr - l);
        point[k] = n[k] * r2;
    }
    return 1;
}

int collide_obj_box(const CollideObj *o, const float centre[3], const float ext[3], float normal[3], float point[3],
                    float push[3])
{
    float lc[3], d[3], ln[3], lp[3], lpu[3];
    point_local(o->m, centre, lc);                   /* the box's centre in the object's frame; its sizes as they are */
    for (int k = 0; k < 3; k++)
        d[k] = lc[k] - o->shape.centre[k];
    int hit;
    if (o->shape.type == COLLIDE_SPHERE) {           /* 0x280D60 type 0: the sphere against the box, the push turned */
        float e[3] = { -d[0], -d[1], -d[2] }, sp[3], sn[3], sq[3];
        hit = sphere_box(e, ext, o->shape.radius, sp, sn, sq);
        if (hit)
            for (int k = 0; k < 3; k++) {
                lpu[k] = -sp[k];
                ln[k] = -sn[k];
                lp[k] = sq[k] + lc[k];
            }
    } else {
        hit = box_box(d, o->shape.half, ext, lpu, ln, lp);
        if (hit)
            for (int k = 0; k < 3; k++)
                lp[k] += o->shape.centre[k];
    }
    if (!hit)
        return 0;
    to_world_dir(o->m, ln, normal);                  /* 0x2801A0: normal and push turned, the point placed */
    to_world_dir(o->m, lpu, push);
    point_world(o->m, lp, point);
    return 1;
}

int collide_obj_sphere(const CollideObj *o, const float c[3], float r, float normal[3], float point[3], float push[3])
{
    float lc[3], d[3], ln[3], lp[3], lpu[3];
    point_local(o->m, c, lc);
    for (int k = 0; k < 3; k++)
        d[k] = lc[k] - o->shape.centre[k];
    int hit = o->shape.type == COLLIDE_SPHERE ? sphere_sphere(d, r, o->shape.radius, lpu, ln, lp)
                                              : sphere_box(d, o->shape.half, r, lpu, ln, lp);
    if (!hit)
        return 0;
    for (int k = 0; k < 3; k++)
        lp[k] += o->shape.centre[k];
    to_world_dir(o->m, ln, normal);
    to_world_dir(o->m, lpu, push);
    point_world(o->m, lp, point);
    return 1;
}

/* 0x280780 (box) and the sphere's ray, in the object's frame: the entry, a ray starting inside meets nothing [G] */
int collide_obj_ray(const CollideObj *o, const float start[3], const float dir[3], float maxt, float *t, float normal[3])
{
    float s[3], d[3], ln[3] = { 0, 0, 0 };
    point_local(o->m, start, s);
    to_local(o->m, dir, d);
    for (int k = 0; k < 3; k++)
        s[k] -= o->shape.centre[k];
    float tt;
    if (o->shape.type == COLLIDE_SPHERE) {
        float a = dot3(d, d), b = dot3(s, d), c = dot3(s, s) - o->shape.radius * o->shape.radius;
        if (a <= 0 || c <= 0)
            return 0;
        float disc = b * b - a * c;
        if (disc < 0)
            return 0;
        tt = (-b - sqrtf(disc)) / a;
        if (tt < 0 || tt > maxt)
            return 0;
        for (int k = 0; k < 3; k++)
            ln[k] = (s[k] + d[k] * tt) / o->shape.radius;
    } else {
        float t0 = -BIG, t1 = BIG;
        int ax = -1;
        for (int k = 0; k < 3; k++) {
            const float h = o->shape.half[k];
            if (fabsf(d[k]) < 1e-12f) {
                if (s[k] < -h || s[k] > h)
                    return 0;
                continue;
            }
            float ta = (-h - s[k]) / d[k], tb = (h - s[k]) / d[k];
            if (ta > tb) { float x = ta; ta = tb; tb = x; }
            if (ta > t0) { t0 = ta; ax = k; }
            if (tb < t1) t1 = tb;
        }
        if (t0 > t1 || ax < 0 || t0 < 0 || t0 > maxt)
            return 0;
        tt = t0;
        ln[ax] = d[ax] > 0 ? -1.0f : 1.0f;
    }
    *t = tt;
    to_world_dir(o->m, ln, normal);
    return 1;
}

static void shape_world_centre(const CollideObj *o, float c[3]) { point_world(o->m, o->shape.centre, c); }

int collide_pair(const CollideObj *a, const CollideObj *b, float normal[3], float point[3], float *depth)
{
    float c[3], push[3];
    shape_world_centre(a, c);
    int hit;
    if (a->shape.type == COLLIDE_BOX && b->shape.type == COLLIDE_SPHERE) {   /* the sphere against the box, turned round */
        float bc[3], n[3], p[3];
        shape_world_centre(b, bc);
        hit = collide_obj_sphere(a, bc, b->shape.radius, n, p, push);
        if (hit) {
            for (int k = 0; k < 3; k++) {
                normal[k] = -n[k];
                point[k] = bc[k] - n[k] * b->shape.radius;
            }
            *depth = sqrtf(dot3(push, push));
        }
        return hit;
    }
    hit = collide_obj_sphere(b, c, a->shape.radius, normal, point, push);   /* a's (bounding) sphere against b */
    if (hit) {
        float l = sqrtf(dot3(normal, normal));
        if (l > 0)
            for (int k = 0; k < 3; k++)
                normal[k] /= l;
        *depth = sqrtf(dot3(push, push));
    }
    return hit;
}

/* ---------------------------------------------------------------- the world */

void collide_begin(CollideWorld *w, float back_row)
{
    w->no = w->nb = 0;
    w->nrow[0] = w->nrow[1] = 0;
    w->back_row = back_row;
}

int collide_add(CollideWorld *w, const CollideShape *s, const float m[12], uint32_t flags, int tag, int kind)
{
    if (w->no >= COLLIDE_MAX_OBJ)
        return -1;
    CollideObj *o = &w->o[w->no];
    o->shape = *s;
    memcpy(o->m, m, sizeof o->m);
    o->flags = flags;
    o->tag = tag;
    o->kind = kind;
    o->building = -1;
    return w->no++;
}

int collide_add_building(CollideWorld *w, const float min[3], const float max[3], int row, int state, int falling, int tag)
{
    if (w->nb >= COLLIDE_MAX_BUILDINGS)
        return -1;
    CollideBuilding *b = &w->b[w->nb];
    copy3(b->min, min);
    copy3(b->max, max);
    b->row = row ? 1 : 0;
    b->state = state;
    b->falling = falling;
    b->tag = tag;
    b->nb[0] = b->nb[1] = b->nb[2] = b->nb[3] = 2;
    CollideShape s;                                  /* its box about its place (0x2BDD18: the grid's box less +0x2C) */
    float c[3] = { (min[0] + max[0]) / 2, (min[1] + max[1]) / 2, 0 }, lo[3], hi[3], m[12];
    for (int k = 0; k < 3; k++) {
        lo[k] = min[k] - c[k];
        hi[k] = max[k] - c[k];
    }
    collide_shape_box(&s, lo, hi);
    collide_matrix(m, 0, c);
    b->obj = state ? collide_add(w, &s, m, 4u | COLLIDE_F_BUILDING, tag, -1) : -1;   /* [G] a gone one's box left out */
    if (b->obj >= 0)
        w->o[b->obj].building = w->nb;
    if (w->nrow[b->row] < COLLIDE_ROW_MAX)
        w->row[b->row][w->nrow[b->row]++] = w->nb;
    return w->nb++;
}

static float bx(const CollideBuilding *b) { return (b->min[0] + b->max[0]) / 2; }   /* +0x2C */
static int roof_ok(const CollideBuilding *b) { return b->state > 0 && b->state < 4 && !b->falling; }

void collide_finish(CollideWorld *w)
{
    if (w->back_row < 0) {                           /* the back row's fronts */
        w->back_row = 10;
        for (int i = 0; i < w->nrow[1]; i++)
            w->back_row = i ? fminf(w->back_row, w->b[w->row[1][i]].min[1]) : w->b[w->row[1][i]].min[1];
    }
    for (int r = 0; r < 2; r++)                      /* 0x2A51B0: by their x */
        for (int i = 1; i < w->nrow[r]; i++)
            for (int j = i; j > 0 && bx(&w->b[w->row[r][j - 1]]) > bx(&w->b[w->row[r][j]]); j--) {
                int t = w->row[r][j];
                w->row[r][j] = w->row[r][j - 1];
                w->row[r][j - 1] = t;
            }
    for (int r = 0; r < 2; r++)                      /* 0x2A4C70: side by side in a row */
        for (int i = 0; i + 1 < w->nrow[r]; i++) {
            CollideBuilding *a = &w->b[w->row[r][i]], *c = &w->b[w->row[r][i + 1]];
            if (!roof_ok(a) || !roof_ok(c))
                continue;
            float gap = c->min[0] - a->max[0];
            if (gap > 2)
                continue;
            if (c->max[2] == a->max[2]) {
                if (gap == 0)
                    a->nb[1] = c->nb[0] = 1;
            } else if (c->max[2] > a->max[2])
                a->nb[1] = 0;
            else
                c->nb[0] = 0;
        }
    int j = 0;                                       /* front against back */
    for (int i = 0; i < w->nrow[0] && j < w->nrow[1]; i++) {
        CollideBuilding *f = &w->b[w->row[0][i]];
        if (!roof_ok(f))
            continue;
        for (; j < w->nrow[1]; j++) {
            CollideBuilding *k = &w->b[w->row[1][j]];
            if (!roof_ok(k) || k->max[0] <= f->min[0] || fabsf(k->min[1] - f->max[1]) >= 0.001f)
                continue;
            if (!(k->min[0] < f->max[0]))
                break;
            if (k->max[0] == f->max[0] && k->min[0] == f->min[0] && k->max[2] == f->max[2])
                f->nb[3] = k->nb[2] = 1;
            else if (k->max[2] > f->max[2])
                f->nb[3] = 0;
            else if (k->max[2] < f->max[2])
                k->nb[2] = 0;
            break;
        }
    }
}

int collide_ray(const CollideWorld *w, const float start[3], const float dir[3], float maxt, uint32_t include,
                uint32_t exclude, int any, CollideHit *hit)
{
    int got = 0;
    hit->obj = -1;
    if (dir[2] <= -0.001f && start[2] > 0) {         /* RSceneryMgr 0x282460: the plane z = 0 */
        float t = -start[2] / dir[2];
        if (t <= maxt && t >= 0) {
            hit->t = t;
            hit->normal[0] = hit->normal[1] = 0;
            hit->normal[2] = 1;
            got = 1;
            maxt = t;
            if (any)
                goto done;
        }
    }
    for (int i = 0; i < w->no; i++) {
        const CollideObj *o = &w->o[i];
        if (!(o->flags & include) || (o->flags & exclude))
            continue;
        float t, n[3];
        if (collide_obj_ray(o, start, dir, maxt, &t, n)) {
            hit->t = t;
            copy3(hit->normal, n);
            hit->obj = i;
            got = 1;
            maxt = t;
            if (any)
                break;
        }
    }
done:
    if (got)
        for (int k = 0; k < 3; k++)
            hit->point[k] = start[k] + dir[k] * hit->t;
    return got;
}

int collide_sphere_query(const CollideWorld *w, const float c[3], float r, uint32_t include, uint32_t exclude,
                         int *out, int nmax)
{
    int n = 0;
    for (int i = 0; i < w->no && n < nmax; i++) {
        const CollideObj *o = &w->o[i];
        if (!(o->flags & include) || (o->flags & exclude))
            continue;
        float oc[3], d[3];
        shape_world_centre(o, oc);
        for (int k = 0; k < 3; k++)
            d[k] = oc[k] - c[k];
        float rr = r + o->shape.radius;
        if (dot3(d, d) < rr * rr)
            out[n++] = i;
    }
    return n;
}

int collide_box_query(const CollideWorld *w, const float min[3], const float max[3], uint32_t include, uint32_t exclude,
                      int *out, int nmax)
{
    float c[3], h[3];
    for (int k = 0; k < 3; k++) {
        c[k] = (min[k] + max[k]) / 2;
        h[k] = (max[k] - min[k]) / 2;
    }
    float r = sqrtf(dot3(h, h));
    int n = 0;
    for (int i = 0; i < w->no && n < nmax; i++) {
        const CollideObj *o = &w->o[i];
        if (!(o->flags & include) || (o->flags & exclude))
            continue;
        float oc[3], d[3];
        shape_world_centre(o, oc);
        for (int k = 0; k < 3; k++)
            d[k] = oc[k] - c[k];
        float rr = r + o->shape.radius;
        if (dot3(d, d) > rr * rr)
            continue;
        int in = 1;
        for (int k = 0; k < 3 && in; k++) {          /* 0x101C20: the turned box's half sizes in the world */
            float e = fabsf(o->m[k]) * o->shape.half[0] + fabsf(o->m[3 + k]) * o->shape.half[1] +
                      fabsf(o->m[6 + k]) * o->shape.half[2];
            in = fabsf(d[k]) <= e + h[k];
        }
        if (in)
            out[n++] = i;
    }
    return n;
}

/* ---------------------------------------------------------------- the buildings (0x2A3D20 .. 0x2A4C70) */

int collide_building_at(const CollideWorld *w, const float p[3], float dx, float dy, int *start)
{
    float A[3], B[3];                                /* where the search starts from */
    int s = -1;
    if (dx == 0 && dy == 0) {
        A[0] = A[1] = A[2] = BIG;
        B[0] = B[1] = B[2] = -BIG;
    } else {
        s = collide_building_at(w, p, 0, 0, NULL);
        if (s >= 0) {
            copy3(A, w->b[s].min);
            copy3(B, w->b[s].max);
        } else {
            copy3(A, p);
            copy3(B, p);
        }
    }
    int best = -1;
    float bmin[3] = { BIG, BIG, BIG }, bmax[3] = { -BIG, -BIG, -BIG };
    for (int i = 0; i < w->nb; i++) {
        if (i == s)
            continue;
        const CollideBuilding *c = &w->b[i];
        int ok;
        if (dx == 0)
            ok = c->min[0] <= p[0] && !(c->max[0] < p[0]);
        else if (dx < 0) {
            float d = A[0] - c->max[0];
            ok = d >= 0 && (best < 0 || d < A[0] - bmax[0]);
        } else {
            float d = c->min[0] - B[0];
            ok = d >= 0 && (best < 0 || d < bmin[0] - B[0]);
        }
        if (!ok)
            continue;
        if (dy == 0)
            ok = c->min[1] <= p[1] && !(c->max[1] < p[1]);
        else if (dy < 0) {
            float d = A[1] - c->max[1];
            ok = d >= 0 && (best < 0 || d < A[1] - bmax[1]);
        } else {
            float d = c->min[1] - B[1];
            ok = d >= 0 && (best < 0 || d < bmin[1] - B[1]);
        }
        if (ok) {
            best = i;
            copy3(bmin, c->min);
            copy3(bmax, c->max);
        }
    }
    if (start)
        *start = s;
    return best;
}

int collide_hit_point(const CollideWorld *w, const float p[3], int side, float q[3])
{
    int b = collide_building_at(w, p, 0, 0, NULL);
    if (b < 0 || w->b[b].min[1] < p[1] - 1) {        /* none at p, or p more than 1 behind its front: that way */
        float dx = side == 0 ? 1.0f : side == 1 ? -1.0f : 0, dy = side == 2 ? 1.0f : 0;
        b = collide_building_at(w, p, dx, dy, NULL);
    }
    if (b < 0)
        return -1;
    const CollideBuilding *c = &w->b[b];
    if (side != 5 && p[2] > c->max[2])
        return -1;
    copy3(q, p);
    if (side == 0) q[0] = c->min[0];
    else if (side == 1) q[0] = c->max[0];
    else if (side == 2) q[1] = c->min[1];
    else if (side == 5) q[2] = c->max[2];
    else return -1;
    float d[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] };
    return dot3(d, d) <= 25 ? b : -1;
}

/* 0x2A36F0: the circle against one row: its fronts at y = rowY, its buildings going back from there */
static int push_row(const CollideWorld *w, float p[3], int row, float r, float r2)
{
    float rowY = row ? (float)(int)w->back_row : 0;
    if (p[1] + r <= rowY)
        return 0;
    const int *list = w->row[row], n = w->nrow[row];
    int hit = 0, t1 = 0;
    for (; t1 < n; t1++)                             /* the first standing one whose middle is not left of p */
        if (w->b[list[t1]].state && bx(&w->b[list[t1]]) >= p[0])
            break;
    float right = 1000, left = -1000;
    if (t1 < n) {
        const CollideBuilding *b = &w->b[list[t1]];
        if (b->max[2] > p[2]) {
            right = b->min[0];
            if (p[1] >= rowY && p[0] >= b->min[0] && p[0] <= b->max[0] && b->max[2] - p[2] < 3) {
                p[1] = fminf(p[1], 10 + rowY - r - 0.001f);   /* by its top: kept to the 10 deep roof */
                right = 1000;
                hit = 1;
            }
        }
    }
    int t0 = t1 - 1;
    for (; t0 >= 0; t0--)                            /* the last standing one whose middle is not right of p */
        if (w->b[list[t0]].state && bx(&w->b[list[t0]]) <= p[0])
            break;
    if (t0 >= 0) {
        const CollideBuilding *b = &w->b[list[t0]];
        if (b->max[2] > p[2]) {
            left = b->max[0];
            if (p[1] >= rowY && p[0] <= b->max[0] && p[0] >= b->min[0] && b->max[2] - p[2] < 3) {
                p[1] = fminf(p[1], 10 + rowY - r - 0.001f);
                left = -1000;
                hit = 1;
            }
        }
    }
    if (!(p[0] < right) || p[0] <= left || right - left <= 0.1f + 2 * r2) {   /* in front of a wall */
        float d = p[1] + r - rowY;
        if (d > 0) {
            p[1] -= 0.001f + 1.001f * d;
            hit = 1;
        }
        return hit;
    }
    if (p[1] < rowY) {                               /* before an alley: its corners */
        const float cx[2] = { left, right };
        for (int k = 0; k < 2; k++) {
            float ex = p[0] - cx[k], ey = p[1] - rowY, l2 = ex * ex + ey * ey;
            if (l2 < r * r) {
                float s = 1.001f * r / sqrtf(l2);
                p[0] = cx[k] + ex * s;
                p[1] = rowY + ey * s;
                return 1;
            }
        }
        return hit;
    }
    float dl = p[0] - r - left, dr = right - r - p[0];   /* in the alley: off its walls */
    if (dl < 0) {
        p[0] -= 1.001f * dl - 0.001f;
        hit = 1;
    } else if (dr < 0) {
        p[0] += 1.001f * dr - 0.001f;
        hit = 1;
    }
    return hit;
}

int collide_buildings_push(const CollideWorld *w, float p[3], float r, float r2)
{
    int back = push_row(w, p, 1, r, r2), front = push_row(w, p, 0, r, r2), hit = front || back;
    if (front && back)                               /* caught between the rows */
        p[1] = fminf(p[1], (float)(int)w->back_row - r - 0.001f);
    if (p[0] > 55) { p[0] = 55; hit = 1; }           /* the street's limits [R 0x2A3638 ..] */
    else if (p[0] < -55) { p[0] = -55; hit = 1; }
    if (p[1] > 21) { p[1] = 21; hit = 1; }
    if (p[1] < -10) { p[1] = -10; hit = 1; }
    return hit;
}

void collide_roof_clamp(const CollideWorld *w, int bi, const float p[3], float m, float m2, float out[3])
{
    const CollideBuilding *b = &w->b[bi];
    float x0 = b->min[0] + (b->nb[0] ? m : m2), x1 = b->max[0] - (b->nb[1] ? m : m2);
    float y0 = b->min[1] + (b->nb[2] ? m : m2), y1 = b->max[1] - (b->nb[3] ? m : m2);
    if (x1 < x0)
        x0 = x1 = bx(b);
    if (y1 < y0)
        y0 = y1 = (b->min[1] + b->max[1]) / 2;
    copy3(out, p);
    if (b->nb[0] != 1) out[0] = fmaxf(out[0], x0);   /* a flush neighbour as high: it walks across */
    if (b->nb[1] != 1) out[0] = fminf(out[0], x1);
    if (b->nb[2] != 1) out[1] = fmaxf(out[1], y0);
    if (b->nb[3] != 1) out[1] = fminf(out[1], y1);
}

/* ---------------------------------------------------------------- the monster (0x2E2C50) */

static void probe_point(const CollideMonster *m, float q[3])   /* 0x2E4D30 */
{
    float h = m->heading / 57.2957795f, c = cosf(h), s = sinf(h);
    q[0] = m->pos[0] + c * m->probe[0] - s * m->probe[1];
    q[1] = m->pos[1] + s * m->probe[0] + c * m->probe[1];
    q[2] = m->pos[2];
}

int collide_building_below(const CollideWorld *w, const CollideMonster *m)
{
    if (m->dead)
        return -1;
    float q[3];
    probe_point(m, q);
    int b = collide_building_at(w, q, 0, 0, NULL);
    return b >= 0 && w->b[b].max[2] - m->pos[2] <= 5 ? b : -1;
}

void collide_monster(const CollideWorld *w, CollideMonster *m, CollideMonsterOut *out)
{
    memset(out, 0, sizeof *out);
    out->below = -1;
    float hw = m->half_w, hh = m->half_h;
    float centre[3] = { m->pos[0], m->pos[1], m->pos[2] + hh }, ext[3] = { hw, hw, hh };
    float lo[3] = { m->pos[0] - hw, m->pos[1] - hw, m->pos[2] - hh }, hi[3] = { m->pos[0] + hw, m->pos[1] + hw, m->pos[2] + hh };
    if (m->mode == 5 || (m->mode == 1 && m->pos[2] <= 3)) {   /* climbing, or low in a jump: the box 3 higher */
        lo[2] += 3;
        hi[2] += 3;
        centre[2] += 3;
    }
    int found[64];
    int nf = collide_box_query(w, lo, hi, COLLIDE_F_TRIGGER | COLLIDE_F_PUSHOUT, COLLIDE_F_MONSTER, found, 64);
    for (int i = 0; i < nf; i++) {
        const CollideObj *o = &w->o[found[i]];
        if (m->mode == 5 && !(o->flags & COLLIDE_F_TRIGGER))
            continue;                                /* on a face only the triggers count */
        float n[3], pt[3], push[3];
        if (!collide_obj_box(o, centre, ext, n, pt, push))
            continue;
        int moved = 0;
        if (o->flags & COLLIDE_F_TRIGGER) {          /* 0x335BF0: the trigger fires (its refire time permitting) */
            if (out->ntouch < COLLIDE_MAX_TOUCH) {
                out->touch[out->ntouch] = found[i];
                copy3(out->touch_normal[out->ntouch++], n);
            }
            moved = (o->flags & COLLIDE_F_PUSHOUT) && m->mode != 5;
        } else
            moved = !m->no_push;
        if (moved) {
            m->pos[0] += push[0];
            m->pos[1] += push[1];
            if (out->npushed < COLLIDE_MAX_TOUCH)
                out->pushed[out->npushed++] = found[i];
        }
    }
    if (m->mode == 0) {                              /* standing over a building: on its roof */
        int b = collide_building_below(w, m);
        out->below = b;
        float q[3];
        probe_point(m, q);
        if (b >= 0 && q[2] > w->b[b].max[2] - 1) {
            float o2[3];
            collide_roof_clamp(w, b, q, 0.1f * hw, hw, o2);
            m->pos[0] += o2[0] - q[0];
            m->pos[1] += o2[1] - q[1];
            out->on_roof = 1;
            return;
        }
    }
    if (m->mode == 5)
        return;
    float q[3], q0[3];
    probe_point(m, q);
    if (m->mode == 1 && m->pos[2] <= 3)
        q[2] += 3;
    copy3(q0, q);
    if (!collide_buildings_push(w, q, hw, hw + (m->wide ? 5.0f : 0)))
        return;
    float d[2] = { q[0] - q0[0], q[1] - q0[1] };
    m->pos[0] += d[0];
    m->pos[1] += d[1];
    out->walled = 1;
    for (int k = 0; k < 2; k++)                      /* moving into it no more */
        if (d[k] != 0) {
            if (m->vel[k] * d[k] < 0)
                m->vel[k] = 0;
            if (m->stick[k] * d[k] < 0)
                m->stick[k] = 0;
        }
}

/* ---------------------------------------------------------------- the SpherePhysicsMover */

void collide_mover_init(CollideMover *mv, float radius, float gravity, float mass, float restitution, float friction,
                        float solidity)
{
    memset(mv, 0, sizeof *mv);
    mv->radius = radius;
    mv->gravity = gravity;
    mv->mass = mass > 0 ? mass : 1;
    mv->restitution = restitution;
    mv->friction = friction;
    mv->axis[2] = 1;
    float in = (0.67f - 0.27f * solidity) * mv->mass * radius * radius;
    mv->inertia[0] = mv->inertia[1] = mv->inertia[2] = in > 1e-6f ? in : 1e-6f;
}

void collide_mover_spin(CollideMover *mv, const float w[3])
{
    float l = sqrtf(dot3(w, w));
    mv->rate = l;
    if (l * l < 1e-6f) {
        mv->axis[0] = 1;
        mv->axis[1] = mv->axis[2] = 0;
    } else
        for (int k = 0; k < 3; k++)
            mv->axis[k] = w[k] / l;
}

static void spin_vec(const CollideMover *mv, float w[3])
{
    for (int k = 0; k < 3; k++)
        w[k] = mv->axis[k] * mv->rate;
}

void collide_mover_impulse(CollideMover *mv, const float m[12], const float j[3], const float p[3])
{
    float r[3] = { p[0] - m[9], p[1] - m[10], p[2] - m[11] }, tq[3], tl[3], dw[3], w[3];
    for (int k = 0; k < 3; k++)
        mv->vel[k] += j[k] / mv->mass;
    cross3(r, j, tq);                                /* the torque, into the object's frame, by its inertia, back */
    to_local(m, tq, tl);
    for (int k = 0; k < 3; k++)
        tl[k] /= mv->inertia[k];
    to_world_dir(m, tl, dw);
    spin_vec(mv, w);
    for (int k = 0; k < 3; k++)
        w[k] += dw[k];
    collide_mover_spin(mv, w);
}

/* r x (I^-1 (r x a)) . b in the object's frame: the impulse's lever */
static float lever(const CollideMover *mv, const float r[3], const float a[3], const float b[3])
{
    float c[3], d[3];
    cross3(r, a, c);
    for (int k = 0; k < 3; k++)
        c[k] /= mv->inertia[k];
    cross3(c, r, d);
    return dot3(d, b);
}

void collide_mover_contact(CollideMover *mv, const float m[12], const float p[3], const float n[3])
{
    float v[3], w[3], r[3], nl[3], u[3], t1[3], t2[3];
    to_local(m, mv->vel, v);
    spin_vec(mv, w);
    to_local(m, w, w);
    point_local(m, p, r);
    to_local(m, n, nl);
    cross3(w, r, u);
    for (int k = 0; k < 3; k++)
        u[k] += v[k];
    float vn = dot3(u, nl);
    if (-vn <= 0)                                    /* parting already */
        return;
    float ax[3] = { 1, 0, 0 };                       /* 0x103E90: a tangent, and the second one */
    if (fabsf(nl[0]) > 0.9f) { ax[0] = 0; ax[1] = 1; }
    cross3(nl, ax, t1);
    float l = sqrtf(dot3(t1, t1));
    for (int k = 0; k < 3; k++)
        t1[k] /= l;
    cross3(nl, t1, t2);
    float im = 1 / mv->mass;
    float jn = -vn / (im + lever(mv, r, nl, nl)) * (1 + mv->restitution);
    float j1 = -dot3(u, t1) / (im + lever(mv, r, t1, t1)), j2 = -dot3(u, t2) / (im + lever(mv, r, t2, t2));
    float lim = mv->friction * jn, jt2 = j1 * j1 + j2 * j2;
    if (lim * lim < jt2) {                           /* Coulomb: at most SlidingFriction x the push */
        float s = sqrtf(lim * lim / jt2);
        j1 *= s;
        j2 *= s;
    }
    float jl[3], jw[3];
    for (int k = 0; k < 3; k++)
        jl[k] = nl[k] * jn + t1[k] * j1 + t2[k] * j2;
    to_world_dir(m, jl, jw);
    collide_mover_impulse(mv, m, jw, p);
}

static void rotate_axis(float m[12], const float a[3], float ang)   /* 0x1014D0 / 0x101A90: about a world axis */
{
    float c = cosf(ang), s = sinf(ang);
    for (int i = 0; i < 3; i++) {
        float *v = m + 3 * i, x[3], d = dot3(a, v);
        cross3(a, v, x);
        for (int k = 0; k < 3; k++)
            v[k] = v[k] * c + x[k] * s + a[k] * d * (1 - c);
    }
}

int collide_mover_step(CollideWorld *w, CollideMover *mv, float m[12], int self, float dt, CollideContactFn fn,
                       void *user)
{
    return collide_mover_step_ex(w, mv, m, self, dt, 0, fn, user);
}

int collide_check(const CollideWorld *w, int obj, int tag, int kind)
{
    return w && obj >= 0 && obj < w->no && w->o[obj].tag == tag && w->o[obj].kind == kind ? obj : -1;
}

void collide_matrix_from16(float m[12], const float w16[16])
{
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 3; r++)
            m[3 * c + r] = w16[4 * c + r];
}

void collide_matrix_to16(const float m[12], float w16[16])
{
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 3; r++)
            w16[4 * c + r] = m[3 * c + r];
        w16[4 * c + 3] = c == 3 ? 1.0f : 0.0f;
    }
}

int collide_mover_step_ex(CollideWorld *w, CollideMover *mv, float m[12], int self, float dt, uint32_t exclude,
                          CollideContactFn fn, void *user)
{
    float r = mv->radius, f23 = 0.75f * r, vmax = dt > 0 ? 50 * f23 / dt : BIG, left = dt;
    int contacts = 0;
    if (dt <= 0)
        return 0;
    do {
        float v2 = dot3(mv->vel, mv->vel), h;
        if (v2 > vmax * vmax) {                      /* at most 50 sub-steps' travel a frame */
            float s = sqrtf(vmax * vmax / v2);
            for (int k = 0; k < 3; k++)
                mv->vel[k] *= s;
            v2 = vmax * vmax;
        }
        h = v2 > 1e-6f ? fminf(f23 / sqrtf(v2), left) : left;
        left -= h;
        if (fabsf(mv->rate) > 0.001f)
            rotate_axis(m, mv->axis, mv->rate * h);
        for (int k = 0; k < 3; k++)
            m[9 + k] += mv->vel[k] * h;
        m[11] -= 0.5f * mv->gravity * h * h;
        mv->vel[2] -= mv->gravity * h;
        if (r > m[11]) {                             /* RSceneryMgr 0x2823F0: the street, out and the impulse */
            const float n[3] = { 0, 0, 1 };
            m[11] += r - m[11] + 0.001f * r;
            float p[3] = { m[9], m[10], m[11] - r };
            collide_mover_contact(mv, m, p, n);
            contacts++;
            if (fn)
                fn(user, -1, p, n);                  /* OnHitGround (vtable +0x40) every sub-step on it */
        }
        if (self >= 0 && self < w->no) {             /* 0x287430: the objects its shape reaches */
            CollideObj *o = &w->o[self];
            memcpy(o->m, m, sizeof o->m);
            float c[3];
            shape_world_centre(o, c);
            int near[32], nn = collide_sphere_query(w, c, o->shape.radius, ~0u, exclude, near, 32);
            for (int i = 0; i < nn; i++) {
                if (near[i] == self)
                    continue;
                float n[3], p[3], depth;
                if (!collide_pair(o, &w->o[near[i]], n, p, &depth) || depth * depth <= 1e-5f)
                    continue;
                collide_mover_contact(mv, m, p, n);  /* [G] the other taken as static (0x285E90's one-mover case) */
                if (fn)
                    fn(user, near[i], p, n);         /* TakeImpact (vtable +0x38) on both */
            }
        }
    } while (left > 0);
    return contacts;
}
