/* cloth.c - the game's cloth simulation (cloth.h; docs/CLOTH_NOTES.md). A port of the ClothInstance code
 * 0x1D5920 .. 0x1DAAA0: setup 0x1DA4B0, the frame 0x1D90E0, the rest pose 0x1D9B80, the pins 0x1D8360, a substep
 * 0x1D7710, the stretch limit and velocities 0x1D8CA0, the triangle normals 0x1D7490, the particle normals 0x1D6E30,
 * the reset and its blend 0x1D67B0 / 0x1D89C0, and the flags' wind of the ClothObject update 0x3473B0.
 * [R] = read in that code, [G] = a guess or a stand-in where it matters.
 *
 * The game's instance (0x9C bytes) and what it is here:
 *   +0x04 entry, +0x08 a 0x20-byte record per part {+0 particle states, +4 kept positions, +8 triangle normals,
 *   +0xC pins' mean velocity, +0x18 pins' top speed squared, +0x1C 1 / pins}, +0x10 the skin matrix per skeleton node,
 *   +0x1C the object matrix of the last frame (4x3), +0x60 the force added this frame, +0x7C the skeleton,
 *   +0x80 resume after the blend, +0x81 started, +0x82 no pins, +0x83 simulating, +0x84 blend time left, +0x88 1 / its
 *   length, +0x8C scale (1). A particle state is 0x44 bytes: +0 velocity, +0xC force, +0x18 position, +0x24 last
 *   frame's position, +0x30 normal, +0x3C stiffness, +0x40 held. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cloth.h"

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static float rdf(const uint8_t *p) { union { uint32_t u; float f; } v; v.u = rd32(p); return v.f; }

typedef struct { uint32_t a, b; float rest, rest2, damp, k; } Spring;   /* 24 bytes [R] */
typedef struct { uint32_t t0, t1, p[4]; float rest, k; } Bend;          /* 32 bytes: two triangles, four particles [R] */

typedef struct {
    /* the part's settings (0x8C bytes; names from the text format's keywords at 0x41C458, in field order) [R] */
    float gravity;                                   /* +0x00 GRAVITY: 9.8 on the flags, 0 or 100 on the monsters */
    float drag;                                      /* +0x1C DRAG */
    float mass, inv_mass;                            /* +0x20 PARTICLE_MASS (1); +0x5C its inverse, set by 0x1DA4B0 */
    float max_dt;                                    /* +0x34 MAX_TIME_DELTA: the substep, 1/60 */
    float rel_vel;                                   /* +0x38 RELATIVE_VEL_SCALE: 0.85 */
    float extra_damp;                                /* +0x3C EXTRA_DAMP: 1 */
    float max_vel, max_vel_time;                     /* +0x44 MAX_VEL (100, 150), +0x48 MAX_VEL_TRANS_TIME (1/30) */
    float extra_damp_vel2;                           /* +0x58: EXTRA_DAMP_VEL (+0x40, 10) squared, stored (100) */
    uint32_t iters;                                  /* +0x4C NUM_INV_DYN_ITERS: 2 or 3 */
    uint8_t adjust_vel, do_collisions, bend_springs, is_stiff, use_extra_damp;   /* +0x50 .. +0x54 */
    int first, n;                                    /* its particles in the cloth's arrays */
    int npins; uint32_t *pins;
    int nsprings; Spring *springs;
    int nbends; Bend *bends;
    int ntris; uint32_t *tris; float *trin;          /* trin: the unit normal per triangle (instance part +0x8) */
    float avg_vel[3];                                /* part +0xC: the pins' mean velocity this frame */
    float max_speed2;                                /* part +0x18: the fastest pin's speed squared */
    float inv_pins;                                  /* part +0x1C */
    float *snap;                                     /* part +0x4: positions kept by a reset, for the blend */
} Part;

struct Cloth {
    int nparts; Part *parts;
    int n;
    float *bind, *bind_nrm, *stiff, *wt;             /* per particle: entry +0x00, +0x0C, +0x18, +0x1C (4 weights) */
    uint16_t *node; uint8_t *nb, *held;              /* +0x2C (4 nodes), +0x34 bone count, +0x36 held */
    float *vel, *force, *pos, *prev, *nrm;
    uint32_t nbones, *bones; int nodes;
    uint32_t *alltris; int nalltris;
    float world[16];                                 /* this frame's object matrix (0x1D90E0's second argument) */
    float m1c[16];                                   /* instance +0x1C: the one of the frame before, then this one */
    float acc[3];                                    /* instance +0x60 */
    int started, simulating, resume;                 /* +0x81, +0x83, +0x80 */
    float freeze, inv_freeze;                        /* +0x84, +0x88 */
};

/* ---- small vector and matrix helpers (column-major 4x4; the game's 4x3 row matrices have the same layout) */
static void xf_point(const float *m, const float *p, float *o)
{
    float x = p[0], y = p[1], z = p[2];
    o[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
    o[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
    o[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

static void xf_dir(const float *m, const float *p, float *o)
{
    float x = p[0], y = p[1], z = p[2];
    o[0] = m[0] * x + m[4] * y + m[8] * z;
    o[1] = m[1] * x + m[5] * y + m[9] * z;
    o[2] = m[2] * x + m[6] * y + m[10] * z;
}

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void identity(float *m)
{
    memset(m, 0, 16 * sizeof *m);
    m[0] = m[5] = m[10] = m[15] = 1;
}

static void mat_mul(const float *a, const float *b, float *o)   /* o = a b (b first) */
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++)
            r[4 * c + k] = a[k] * b[4 * c] + a[4 + k] * b[4 * c + 1] + a[8 + k] * b[4 * c + 2] + a[12 + k] * b[4 * c + 3];
    memcpy(o, r, sizeof r);
}

static void mat_inverse_affine(const float *m, float *o)       /* the 3x3 part inverted, then the translation */
{
    float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    float A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
    float det = a * A + b * B + c * C, s = det != 0 ? 1 / det : 0;
    float r[16];
    r[0] = A * s;                  r[4] = (c * h - b * i) * s;  r[8] = (b * f - c * e) * s;
    r[1] = B * s;                  r[5] = (a * i - c * g) * s;  r[9] = (c * d - a * f) * s;
    r[2] = C * s;                  r[6] = (b * g - a * h) * s;  r[10] = (a * e - b * d) * s;
    r[3] = r[7] = r[11] = 0; r[15] = 1;
    for (int k = 0; k < 3; k++)
        r[12 + k] = -(r[k] * m[12] + r[4 + k] * m[13] + r[8 + k] * m[14]);
    memcpy(o, r, sizeof r);
}

/* ---- parsing (the entry is used in place by the game; 0x1DB840 turns its offsets into pointers) */
static int in_range(uint32_t off, uint64_t len, uint32_t size) { return (uint64_t)off + len <= size; }

static void free_parts(Cloth *c)
{
    for (int k = 0; c->parts && k < c->nparts; k++) {
        Part *p = &c->parts[k];
        free(p->pins); free(p->springs); free(p->bends); free(p->tris); free(p->trin); free(p->snap);
    }
    free(c->parts);
}

void cloth_free(Cloth *c)
{
    if (!c)
        return;
    free_parts(c);
    free(c->bind); free(c->bind_nrm); free(c->stiff); free(c->wt); free(c->node); free(c->nb); free(c->held);
    free(c->vel); free(c->force); free(c->pos); free(c->prev); free(c->nrm);
    free(c->bones); free(c->alltris);
    free(c);
}

static void tri_normals(Cloth *c, const Part *p);
static void particle_normals(Cloth *c, const float (*skin)[16]);

Cloth *cloth_new(const uint8_t *d, uint32_t size)
{
    if (!d || size < 0x4C)
        return NULL;
    uint32_t nparts = rd32(d + 0x34), parts_at = rd32(d + 0x38), nbones = rd32(d + 0x3C), bones_at = rd32(d + 0x40);
    if (nparts == 0 || nparts > 4096 || !in_range(parts_at, 0x8Cull * nparts, size) || nbones > 4096 ||
        !in_range(bones_at, 4ull * nbones, size))
        return NULL;
    Cloth *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->nparts = (int)nparts;
    c->parts = calloc(nparts, sizeof *c->parts);
    c->nbones = nbones;
    c->bones = malloc((nbones ? nbones : 1) * sizeof *c->bones);
    if (!c->parts || !c->bones)
        goto fail;
    for (uint32_t i = 0; i < nbones; i++)
        c->bones[i] = rd32(d + bones_at + 4 * i);
    /* the entry's +0x44 / +0x48 are a count and array of 0x18-byte collision spheres {f32[3] offset, f32, f32, u32
     * node} (0x1D9518, 0x1D87F0); the count is 0 on all 44 entries, so there is nothing to collide with [R] */
    uint64_t total = 0, ntris = 0;
    for (uint32_t k = 0; k < nparts; k++) {
        const uint8_t *r = d + parts_at + 0x8C * k;
        if (rd32(r + 0x80) != rd32(r + 0x84) || rd32(r + 0x84) > 65536)
            goto fail;
        total += rd32(r + 0x84);
        ntris += rd32(r + 0x78);
    }
    if (total == 0 || total > 1u << 20 || ntris > 1u << 20)
        goto fail;
    c->n = (int)total;
    c->bind = calloc(3 * total, sizeof(float));      c->bind_nrm = calloc(3 * total, sizeof(float));
    c->stiff = calloc(total, sizeof(float));         c->wt = calloc(4 * total, sizeof(float));
    c->node = calloc(4 * total, sizeof(uint16_t));   c->nb = calloc(total, 1);  c->held = calloc(total, 1);
    c->vel = calloc(3 * total, sizeof(float));       c->force = calloc(3 * total, sizeof(float));
    c->pos = calloc(3 * total, sizeof(float));       c->prev = calloc(3 * total, sizeof(float));
    c->nrm = calloc(3 * total, sizeof(float));
    c->alltris = calloc(3 * (ntris ? ntris : 1), sizeof(uint32_t));
    if (!c->bind || !c->bind_nrm || !c->stiff || !c->wt || !c->node || !c->nb || !c->held || !c->vel || !c->force ||
        !c->pos || !c->prev || !c->nrm || !c->alltris)
        goto fail;

    int first = 0;
    for (uint32_t k = 0; k < nparts; k++) {
        const uint8_t *r = d + parts_at + 0x8C * k;
        Part *p = &c->parts[k];
        p->gravity = rdf(r + 0x00);
        p->drag = rdf(r + 0x1C);
        p->mass = rdf(r + 0x20);
        p->inv_mass = p->mass != 0 ? 1 / p->mass : 0;           /* 0x1DA79C: +0x5C = 1 / +0x20 */
        p->max_dt = rdf(r + 0x34);
        p->rel_vel = rdf(r + 0x38);
        p->extra_damp = rdf(r + 0x3C);
        p->max_vel = rdf(r + 0x44);
        p->max_vel_time = rdf(r + 0x48);
        p->iters = rd32(r + 0x4C);
        p->adjust_vel = r[0x50]; p->do_collisions = r[0x51]; p->bend_springs = r[0x52];
        p->is_stiff = r[0x53]; p->use_extra_damp = r[0x54];
        p->extra_damp_vel2 = rdf(r + 0x58);
        uint32_t np = rd32(r + 0x60), pins_at = rd32(r + 0x64), ns = rd32(r + 0x68), springs_at = rd32(r + 0x6C);
        uint32_t nbd = rd32(r + 0x70), bends_at = rd32(r + 0x74), nt = rd32(r + 0x78), tris_at = rd32(r + 0x7C);
        uint32_t npart = rd32(r + 0x84), parts_p = rd32(r + 0x88);
        if (!in_range(pins_at, 4ull * np, size) || !in_range(springs_at, 24ull * ns, size) ||
            !in_range(bends_at, 32ull * nbd, size) || !in_range(tris_at, 12ull * nt, size) ||
            !in_range(parts_p, 64ull * npart, size))
            goto fail;
        p->first = first;
        p->n = (int)npart;
        p->npins = (int)np; p->nsprings = (int)ns; p->nbends = (int)nbd; p->ntris = (int)nt;
        p->pins = malloc((np ? np : 1) * sizeof *p->pins);
        p->springs = malloc((ns ? ns : 1) * sizeof *p->springs);
        p->bends = malloc((nbd ? nbd : 1) * sizeof *p->bends);
        p->tris = malloc((nt ? 3 * nt : 1) * sizeof *p->tris);
        p->trin = calloc(nt ? 3 * nt : 1, sizeof *p->trin);
        p->snap = calloc(3 * npart, sizeof *p->snap);
        if (!p->pins || !p->springs || !p->bends || !p->tris || !p->trin || !p->snap)
            goto fail;
        for (uint32_t i = 0; i < np; i++) {
            uint32_t q = rd32(d + pins_at + 4 * i);
            if (q >= npart)
                goto fail;
            p->pins[i] = (uint32_t)first + q;
        }
        for (uint32_t i = 0; i < ns; i++) {
            const uint8_t *s = d + springs_at + 24 * i;
            Spring *sp = &p->springs[i];
            sp->a = rd32(s); sp->b = rd32(s + 4);
            if (sp->a >= npart || sp->b >= npart)
                goto fail;
            sp->a += (uint32_t)first; sp->b += (uint32_t)first;
            sp->rest = rdf(s + 8); sp->rest2 = rdf(s + 12); sp->damp = rdf(s + 16); sp->k = rdf(s + 20);
        }
        for (uint32_t i = 0; i < nbd; i++) {
            const uint8_t *s = d + bends_at + 32 * i;
            Bend *b = &p->bends[i];
            b->t0 = rd32(s); b->t1 = rd32(s + 4);
            if (b->t0 >= nt || b->t1 >= nt)
                goto fail;
            for (int j = 0; j < 4; j++) {
                uint32_t q = rd32(s + 8 + 4 * j);
                if (q >= npart)
                    goto fail;
                b->p[j] = (uint32_t)first + q;
            }
            b->rest = rdf(s + 24); b->k = rdf(s + 28);
        }
        for (uint32_t i = 0; i < 3 * nt; i++) {
            uint32_t q = rd32(d + tris_at + 4 * i);
            if (q >= npart)
                goto fail;
            p->tris[i] = (uint32_t)first + q;
            c->alltris[3 * c->nalltris + i] = (uint32_t)first + q;
        }
        c->nalltris += (int)nt;
        for (uint32_t i = 0; i < npart; i++) {
            const uint8_t *q = d + parts_p + 64 * i;
            int g = first + (int)i;
            for (int j = 0; j < 3; j++) {
                c->bind[3 * g + j] = rdf(q + 4 * j);
                c->bind_nrm[3 * g + j] = rdf(q + 0x0C + 4 * j);
            }
            c->stiff[g] = rdf(q + 0x18);                /* state +0x3C: the spring to the rest shape (IS_STIFF) */
            int nbn = q[0x34] > 4 ? 4 : q[0x34];
            c->nb[g] = (uint8_t)nbn;
            for (int j = 0; j < 4; j++) {
                c->wt[4 * g + j] = rdf(q + 0x1C + 4 * j);
                c->node[4 * g + j] = rd16(q + 0x2C + 2 * j);
            }
            for (int j = 0; j < nbn; j++)
                if (c->node[4 * g + j] + 1 > c->nodes)
                    c->nodes = c->node[4 * g + j] + 1;
            c->held[g] = (uint8_t)(rd16(q + 0x36) & 1);   /* state +0x40 = lhu +0x36; the code tests bit 0 */
        }
        p->inv_pins = np ? 1.0f / (float)np : 0;         /* 0x1DA994; with no pins the game sets +0x82 */
        first += (int)npart;
    }
    /* 0x1DA6EC: every particle still at its bind position */
    memcpy(c->pos, c->bind, 3 * total * sizeof(float));
    memcpy(c->prev, c->bind, 3 * total * sizeof(float));
    memcpy(c->nrm, c->bind_nrm, 3 * total * sizeof(float));
    for (int k = 0; k < c->nparts; k++)
        if (c->parts[k].nbends)
            tri_normals(c, &c->parts[k]);            /* 0x1DA9C0 */
    identity(c->world);
    identity(c->m1c);
    c->simulating = 1;                               /* 0x1DA604: +0x83 = 1, +0x81 = +0x80 = +0x82 = 0 */
    return c;
fail:
    cloth_free(c);
    return NULL;
}

/* ---- accessors */
int cloth_count(const Cloth *c) { return c ? c->n : 0; }
int cloth_parts(const Cloth *c) { return c ? c->nparts : 0; }
void cloth_part_range(const Cloth *c, int part, int *first, int *count)
{
    int ok = c && part >= 0 && part < c->nparts;
    if (first) *first = ok ? c->parts[part].first : 0;
    if (count) *count = ok ? c->parts[part].n : 0;
}
const uint32_t *cloth_bones(const Cloth *c, int *count) { if (count) *count = c ? (int)c->nbones : 0; return c ? c->bones : NULL; }
int cloth_nodes(const Cloth *c) { return c ? c->nodes : 0; }
const float *cloth_positions(const Cloth *c) { return c ? c->pos : NULL; }
const float *cloth_normals(const Cloth *c) { return c ? c->nrm : NULL; }
const float *cloth_bind_positions(const Cloth *c) { return c ? c->bind : NULL; }
int cloth_held(const Cloth *c, int i) { return c && i >= 0 && i < c->n ? c->held[i] : 0; }
const uint32_t *cloth_triangles(const Cloth *c, int *count) { if (count) *count = c ? c->nalltris : 0; return c ? c->alltris : NULL; }

void cloth_set_world(Cloth *c, const float world[16])
{
    if (c && world)
        memcpy(c->world, world, sizeof c->world);
}

/* ---- where the bones put a particle: sum over its bones of weight x (skin matrix x bind position) [R] */
static void skinned(const Cloth *c, int i, const float (*skin)[16], float *o)
{
    o[0] = o[1] = o[2] = 0;
    for (int b = 0; b < c->nb[i]; b++) {
        float t[3], w = c->wt[4 * i + b];
        xf_point(skin[c->node[4 * i + b]], c->bind + 3 * i, t);
        o[0] += w * t[0]; o[1] += w * t[1]; o[2] += w * t[2];
    }
}

/* a particle's place held by its bones, or by the object matrix m when it has none (a flag) [R]. Without a skeleton
 * a particle with bones goes by m as well [G]: the game always has one for those */
static void held_place(const Cloth *c, int i, const float (*skin)[16], const float *m, float *o)
{
    if (skin && c->nb[i])
        skinned(c, i, skin, o);
    else
        xf_point(m, c->bind + 3 * i, o);
}

/* 0x1D7490: each triangle's unit normal (p1 - p0) x (p2 - p1); (1, 0, 0) for one with no area [R] */
static void tri_normals(Cloth *c, const Part *p)
{
    for (int t = 0; t < p->ntris; t++) {
        const float *p0 = c->pos + 3 * p->tris[3 * t], *p1 = c->pos + 3 * p->tris[3 * t + 1], *p2 = c->pos + 3 * p->tris[3 * t + 2];
        float a[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, b[3] = { p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2] };
        float *n = p->trin + 3 * t;
        n[0] = a[1] * b[2] - a[2] * b[1];
        n[1] = a[2] * b[0] - a[0] * b[2];
        n[2] = a[0] * b[1] - a[1] * b[0];
        float l2 = dot3(n, n);
        if (l2 < 1e-6f) {
            n[0] = 1; n[1] = n[2] = 0;
        } else {
            float s = 1 / sqrtf(l2);
            n[0] *= s; n[1] *= s; n[2] *= s;
        }
    }
}

/* 0x1D6E30 (at drawing): a free particle's normal is the sum of its triangles' unit normals, normalised; a pin's is
 * its bind normal turned by its bones' skin matrices (normalised with two bones or more) or by the object [R] */
static void particle_normals(Cloth *c, const float (*skin)[16])
{
    for (int k = 0; k < c->nparts; k++) {
        Part *p = &c->parts[k];
        if (!p->nbends)
            tri_normals(c, p);
        for (int i = p->first; i < p->first + p->n; i++)
            if (!c->held[i])
                c->nrm[3 * i] = c->nrm[3 * i + 1] = c->nrm[3 * i + 2] = 0;
        for (int t = 0; t < 3 * p->ntris; t++) {
            uint32_t i = p->tris[t];
            if (!c->held[i])
                for (int j = 0; j < 3; j++)
                    c->nrm[3 * i + j] += p->trin[3 * (t / 3) + j];
        }
        for (int q = 0; q < p->npins; q++) {
            int i = (int)p->pins[q];
            float *n = c->nrm + 3 * i;
            if (skin && c->nb[i]) {
                n[0] = n[1] = n[2] = 0;
                for (int b = 0; b < c->nb[i]; b++) {
                    float t[3], w = c->wt[4 * i + b];
                    xf_dir(skin[c->node[4 * i + b]], c->bind_nrm + 3 * i, t);
                    n[0] += w * t[0]; n[1] += w * t[1]; n[2] += w * t[2];
                }
                if (c->nb[i] >= 2) {
                    float l2 = dot3(n, n), s = l2 > 0 ? 1 / sqrtf(l2) : 0;
                    n[0] *= s; n[1] *= s; n[2] *= s;
                }
            } else
                xf_dir(c->m1c, c->bind_nrm + 3 * i, n);
        }
        for (int i = p->first; i < p->first + p->n; i++)
            if (!c->held[i]) {
                float *n = c->nrm + 3 * i, l2 = dot3(n, n), s = l2 > 0 ? 1 / sqrtf(l2) : 0;   /* the game: rsqrt [G] at 0 */
                n[0] *= s; n[1] *= s; n[2] *= s;
            }
    }
}

/* 0x1D9B80: every particle where its bones (or the object matrix) put it, at rest; the pins' velocity and top speed
 * cleared. The game puts a particle without bones at 0 when there is a skeleton [R]; none is so [G] */
static void rest(Cloth *c, const float (*skin)[16])
{
    for (int i = 0; i < c->n; i++) {
        held_place(c, i, skin, c->world, c->pos + 3 * i);
        memcpy(c->prev + 3 * i, c->pos + 3 * i, 3 * sizeof(float));
        c->vel[3 * i] = c->vel[3 * i + 1] = c->vel[3 * i + 2] = 0;
    }
    for (int k = 0; k < c->nparts; k++) {
        Part *p = &c->parts[k];
        if (p->nbends)
            tri_normals(c, p);
        p->avg_vel[0] = p->avg_vel[1] = p->avg_vel[2] = 0;
        p->max_speed2 = 0;
    }
}

/* 0x1D8360: the pins onto their bones (or the object matrix of the frame before: +0x1C is not yet this frame's);
 * their mean velocity over the frame and the fastest one's speed squared [R] */
static void pins(Cloth *c, const float (*skin)[16], float inv_dt)
{
    for (int k = 0; k < c->nparts; k++) {
        Part *p = &c->parts[k];
        float sum[3] = { 0, 0, 0 };
        p->max_speed2 = 0;
        for (int q = 0; q < p->npins; q++) {
            int i = (int)p->pins[q];
            float *x = c->pos + 3 * i, *o = c->prev + 3 * i;
            held_place(c, i, skin, c->m1c, x);
            float dl[3] = { x[0] - o[0], x[1] - o[1], x[2] - o[2] };
            float v[3] = { dl[0] * inv_dt, dl[1] * inv_dt, dl[2] * inv_dt }, v2 = dot3(v, v);
            if (v2 > p->max_speed2)
                p->max_speed2 = v2;
            sum[0] += dl[0]; sum[1] += dl[1]; sum[2] += dl[2];
        }
        float s = p->inv_pins * inv_dt;
        p->avg_vel[0] = sum[0] * s; p->avg_vel[1] = sum[1] * s; p->avg_vel[2] = sum[2] * s;
    }
}

/* the bend's angle: acos of the two triangles' normals' cosine by the game's rational approximation (0x1D7E50) */
static float bend_acos(float x)
{
    if (x <= -0.999f)
        return 3.14159265f;
    if (!(x < 0.999f))
        return 0;
    float x2 = x * x, x4 = x2 * x2;
    float num = -6.9086f + 12.8558f * x2 - 5.9494f * x4;      /* 0xC0DD133C, 0x414DB0BA, 0xC0BE616B */
    float den = -6.9253f + 14.2031f * x2 - 8.2793f * x4 + x4 * x2;   /* 0xC0DD9BFD, 0x41633FB2, 0xC10477D4 */
    return 1.57079633f - x * num / den;
}

/* 0x1D7710: one substep of h seconds on part k [R] */
static void substep(Cloth *c, int k, float h, const float (*skin)[16])
{
    Part *p = &c->parts[k];
    float g = p->gravity * p->mass;
    float rv[3] = { p->avg_vel[0] * p->rel_vel, p->avg_vel[1] * p->rel_vel, p->avg_vel[2] * p->rel_vel };
    int end = p->first + p->n;
    /* the force on each free particle: this frame's added force, gravity down z, and drag quadratic in the speed
     * relative to the air, which moves with RELATIVE_VEL_SCALE of the pins' mean velocity */
    for (int i = p->first; i < end; i++) {
        if (c->held[i])
            continue;
        float *f = c->force + 3 * i, *v = c->vel + 3 * i;
        f[0] = c->acc[0]; f[1] = c->acc[1]; f[2] = c->acc[2] - g;
        float r[3] = { v[0] - rv[0], v[1] - rv[1], v[2] - rv[2] }, d2 = dot3(r, r);
        if (d2 > 1e-6f) {
            float s = p->drag * -sqrtf(d2);
            f[0] += r[0] * s; f[1] += r[1] * s; f[2] += r[2] * s;
        }
    }
    if (p->is_stiff) {
        /* IS_STIFF (the monsters): a spring of the particle's own stiffness (100 .. 500, rising away from the pins)
         * to where its bones put it; the springs between particles only limit the stretch (0x1D8CA0) */
        for (int i = p->first; i < end; i++) {
            if (c->held[i])
                continue;
            float t[3], *x = c->pos + 3 * i, *f = c->force + 3 * i;
            held_place(c, i, skin, c->m1c, t);
            f[0] += c->stiff[i] * (t[0] - x[0]);
            f[1] += c->stiff[i] * (t[1] - x[1]);
            f[2] += c->stiff[i] * (t[2] - x[2]);
        }
    } else {
        /* damped springs (the flags): along the spring, TENSION x stretch + DAMP x (relative velocity . offset) */
        for (int s = 0; s < p->nsprings; s++) {
            const Spring *sp = &p->springs[s];
            float *xa = c->pos + 3 * sp->a, *xb = c->pos + 3 * sp->b, *va = c->vel + 3 * sp->a, *vb = c->vel + 3 * sp->b;
            float dl[3] = { xb[0] - xa[0], xb[1] - xa[1], xb[2] - xa[2] }, l2 = dot3(dl, dl);
            if (l2 <= 1e-6f)
                continue;
            float inv = 1 / sqrtf(l2);
            float dv[3] = { vb[0] - va[0], vb[1] - va[1], vb[2] - va[2] };
            float m = inv * (dot3(dv, dl) * sp->damp + sp->k * (inv * l2 - sp->rest));
            float fs[3] = { dl[0] * m, dl[1] * m, dl[2] * m };
            if (!c->held[sp->a])
                for (int j = 0; j < 3; j++) c->force[3 * sp->a + j] += fs[j];
            if (!c->held[sp->b])
                for (int j = 0; j < 3; j++) c->force[3 * sp->b + j] -= fs[j];
        }
    }
    /* bends (the flags' CALC_BEND_SPRINGS): the angle between two triangles' normals, signed by which side of the
     * hinge p[0] lies (from p[2]), pushed toward the rest angle along the normals' sum: + on p[0], p[1], - on p[2],
     * p[3]. BEND (20) is the stiffness; the rest angles are 0 */
    for (int b = 0; b < p->nbends; b++) {
        const Bend *bd = &p->bends[b];
        const float *n0 = p->trin + 3 * bd->t0, *n1 = p->trin + 3 * bd->t1;
        float sum[3] = { n0[0] + n1[0], n0[1] + n1[1], n0[2] + n1[2] };
        float ang = bend_acos(dot3(n0, n1));
        const float *xa = c->pos + 3 * bd->p[0], *xc = c->pos + 3 * bd->p[2];
        float dl[3] = { xa[0] - xc[0], xa[1] - xc[1], xa[2] - xc[2] };
        if (dot3(sum, dl) <= 0)
            ang = -ang;
        float m = (bd->rest - ang) * bd->k;
        if (fabsf(m) <= 0.001f)
            continue;
        float l2 = dot3(sum, sum);
        if (l2 <= 0)
            continue;                                /* the game takes rsqrt regardless [G] */
        m /= sqrtf(l2);
        float fs[3] = { sum[0] * m, sum[1] * m, sum[2] * m };
        for (int j = 0; j < 4; j++) {
            uint32_t i = bd->p[j];
            if (c->held[i])
                continue;
            float sg = j < 2 ? 1.0f : -1.0f;
            c->force[3 * i] += sg * fs[0]; c->force[3 * i + 1] += sg * fs[1]; c->force[3 * i + 2] += sg * fs[2];
        }
    }
    /* semi-implicit Euler: the velocity from the force, then the position from the new velocity. USE_EXTRA_DAMP
     * (set on no part) scales slow particles' velocity by EXTRA_DAMP (0x1D8180) */
    float hm = h * p->inv_mass;
    for (int i = p->first; i < end; i++) {
        if (c->held[i])
            continue;
        float *v = c->vel + 3 * i, *f = c->force + 3 * i, *x = c->pos + 3 * i;
        v[0] += f[0] * hm; v[1] += f[1] * hm; v[2] += f[2] * hm;
        if (p->use_extra_damp && dot3(v, v) <= p->extra_damp_vel2) {
            v[0] *= p->extra_damp; v[1] *= p->extra_damp; v[2] *= p->extra_damp;
        }
        x[0] += v[0] * h; x[1] += v[1] * h; x[2] += v[2] * h;
    }
    if (p->nbends)
        tri_normals(c, p);
}

/* 0x1D8CA0: NUM_INV_DYN_ITERS passes over the springs cutting back any stretch past the rest length (the square-root
 * free 0.5 - r^2 / (d^2 + r^2), all of it on the free end when the other is held: the code does not check that one),
 * then with ADJUST_VEL each free particle's velocity read back from its move over the frame [R] */
static void inverse_dynamics(Cloth *c, float inv_dt)
{
    for (int k = 0; k < c->nparts; k++) {
        Part *p = &c->parts[k];
        for (uint32_t it = 0; it < p->iters; it++)
            for (int s = 0; s < p->nsprings; s++) {
                const Spring *sp = &p->springs[s];
                float *xa = c->pos + 3 * sp->a, *xb = c->pos + 3 * sp->b;
                float dl[3] = { xb[0] - xa[0], xb[1] - xa[1], xb[2] - xa[2] }, l2 = dot3(dl, dl);
                if (l2 <= sp->rest2)
                    continue;
                float f = 0.5f - sp->rest2 / (l2 + sp->rest2);
                if (c->held[sp->a]) {
                    f *= 2;
                    for (int j = 0; j < 3; j++) xb[j] -= dl[j] * f;
                } else if (c->held[sp->b]) {
                    f *= 2;
                    for (int j = 0; j < 3; j++) xa[j] += dl[j] * f;
                } else
                    for (int j = 0; j < 3; j++) {
                        xa[j] += dl[j] * f;
                        xb[j] -= dl[j] * f;
                    }
            }
        for (int i = p->first; i < p->first + p->n; i++) {
            float *x = c->pos + 3 * i, *o = c->prev + 3 * i;
            if (p->adjust_vel && !c->held[i])
                for (int j = 0; j < 3; j++)
                    c->vel[3 * i + j] = (x[j] - o[j]) * inv_dt;
            memcpy(o, x, 3 * sizeof(float));
        }
    }
}

/* 0x1D67B0 (flag, time): stop simulating; with time >= 0.001 keep the positions and blend them away over that time
 * (0x1D89C0), resuming the simulation afterwards if flag */
static void start_blend(Cloth *c, int resume, float t)
{
    c->resume = resume;
    c->simulating = 0;
    if (t < 0.001f) {
        c->freeze = 0;
        return;
    }
    for (int k = 0; k < c->nparts; k++)
        memcpy(c->parts[k].snap, c->pos + 3 * c->parts[k].first, 3 * (size_t)c->parts[k].n * sizeof(float));
    c->freeze = t;
    c->inv_freeze = 1 / t;
}

/* 0x1D89C0: the kept positions carried along with the object (D = this frame's matrix x the last one's inverse:
 * 0x174B80 .. 0x174A90 invert it [G] as a general affine matrix), each free particle a mix of its rest place and the
 * kept one, the kept one's share the blend time left */
static void blend(Cloth *c, const float *D)
{
    float f1 = c->freeze * c->inv_freeze, f0 = 1 - f1;
    for (int k = 0; k < c->nparts; k++) {
        Part *p = &c->parts[k];
        for (int i = 0; i < p->n; i++) {
            int g = p->first + i;
            if (c->held[g])
                continue;
            float *s = p->snap + 3 * i, q[3], *x = c->pos + 3 * g;
            xf_point(D, s, q);
            for (int j = 0; j < 3; j++) {
                x[j] = x[j] * f0 + q[j] * f1;
                s[j] = q[j];
            }
        }
    }
}

/* 0x1D90E0: one frame */
static void update(Cloth *c, const float (*skin)[16], float dt)
{
    if (!c->started) {                               /* the first frame only places it */
        rest(c, skin);
        memcpy(c->m1c, c->world, sizeof c->m1c);
        c->started = 1;
        particle_normals(c, skin);
        return;
    }
    if (!c->simulating) {                            /* after a reset: at rest, blending from the kept positions */
        rest(c, skin);
        if (c->freeze > 0) {
            c->freeze -= dt;
            if (c->freeze <= 0) {
                if (c->resume)
                    c->simulating = 1;
                c->resume = 0;
                c->freeze = 0;
            } else {
                float inv[16], D[16];
                mat_inverse_affine(c->m1c, inv);
                mat_mul(c->world, inv, D);
                blend(c, D);
            }
        }
        memcpy(c->m1c, c->world, sizeof c->m1c);
        particle_normals(c, skin);
        return;                                      /* the added force stays: the game clears it only below [R] */
    }
    float inv_dt = dt > 0 ? 1 / dt : 0;
    /* the skin matrices: the game makes them here for the entry's bone list from the skeleton (node x world, then the
     * inverse bind x that: 0x1D93E8, 0x1D9768); here the caller hands them in */
    pins(c, skin, inv_dt);
    for (int k = 0; k < c->nparts; k++) {
        Part *p = &c->parts[k];
        if (p->max_speed2 > p->max_vel * p->max_vel) {   /* a pin went faster than MAX_VEL: snap to rest, blend */
            start_blend(c, 0, p->max_vel_time);
            c->resume = 1;
            update(c, skin, dt);
            return;
        }
    }
    memcpy(c->m1c, c->world, sizeof c->m1c);
    for (int k = 0; k < c->nparts; k++) {            /* substeps of MAX_TIME_DELTA, the rest in one more (> 1e-4) */
        float step = c->parts[k].max_dt, t = dt;
        if (step > 0 && dt > step)
            do {
                substep(c, k, step, skin);
                t -= step;
            } while (t > step);
        if (t > 1e-4f)
            substep(c, k, t, skin);
    }
    inverse_dynamics(c, inv_dt);
    /* 0x1D87F0, the collisions with the entry's spheres, would come here: no entry has any */
    particle_normals(c, skin);
    c->acc[0] = c->acc[1] = c->acc[2] = 0;
}

void cloth_reset(Cloth *c, const float (*skin)[16])
{
    if (!c)
        return;
    rest(c, skin);
    memcpy(c->m1c, c->world, sizeof c->m1c);
    c->started = c->simulating = 1;
    c->resume = 0;
    c->freeze = 0;
    c->acc[0] = c->acc[1] = c->acc[2] = 0;
    particle_normals(c, skin);
}

void cloth_step(Cloth *c, float dt, const float (*skin)[16], const float wind[3])
{
    if (!c || !(dt > 0))
        return;                                      /* [G] with dt 0 the game would zero the velocities */
    if (wind) {                                      /* 0x1D6740 */
        c->acc[0] += wind[0]; c->acc[1] += wind[1]; c->acc[2] += wind[2];
    }
    update(c, skin, dt);
}

void cloth_to_local(const Cloth *c, float *pos_out, float *nrm_out)
{
    if (!c)
        return;
    float inv[16];
    mat_inverse_affine(c->world, inv);
    for (int i = 0; i < c->n; i++) {
        if (pos_out)
            xf_point(inv, c->pos + 3 * i, pos_out + 3 * i);
        if (nrm_out) {
            float *n = nrm_out + 3 * i, l2;
            xf_dir(inv, c->nrm + 3 * i, n);
            l2 = dot3(n, n);
            if (l2 > 0) {
                float s = 1 / sqrtf(l2);
                n[0] *= s; n[1] *= s; n[2] *= s;
            }
        }
    }
}

/* ---- mesh vertices to particles */
static float dist2(const float *a, const float *b)
{
    float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return dot3(d, d);
}

int cloth_nearest(const Cloth *c, const float pos[3])
{
    if (!c || !pos)
        return -1;
    int best = -1;
    float bd = 0;
    for (int i = 0; i < c->n; i++) {
        float d = dist2(c->bind + 3 * i, pos);
        if (best < 0 || d < bd) {
            best = i;
            bd = d;
        }
    }
    return best;
}

int cloth_triangle(const Cloth *c, const float p0[3], const float p1[3], const float p2[3], int out[3])
{
    const float *q[3] = { p0, p1, p2 };
    const float eps = 1e-8f;
    if (!c || !p0 || !p1 || !p2 || !out)
        return 0;
    for (int t = 0; t < c->nalltris; t++) {
        int got[3], used = 0, ok = 1;
        for (int k = 0; k < 3 && ok; k++) {
            ok = 0;
            for (int j = 0; j < 3; j++) {
                uint32_t i = c->alltris[3 * t + j];
                if (!(used >> j & 1) && dist2(c->bind + 3 * i, q[k]) <= eps) {
                    got[k] = (int)i;
                    used |= 1 << j;
                    ok = 1;
                    break;
                }
            }
        }
        if (ok) {
            memcpy(out, got, sizeof got);
            return 1;
        }
    }
    for (int k = 0; k < 3; k++)
        out[k] = cloth_nearest(c, q[k]);
    return 0;
}

/* ---- the flags' wind (0x3473B0) */

/* 0x13C220: smoothed value noise in -1 .. 1 at x, the integer lattice hashed by the classic
 * 1 - ((n (n^2 15731 + 789221) + 1376312589) & 0x7FFFFFFF) / 2^30 with n = k ^ (k << 13), each lattice value
 * smoothed 1/4, 1/2, 1/4 with its neighbours and linearly interpolated */
static float lattice(int32_t k)
{
    uint32_t n = (uint32_t)k;
    n = (n << 13) ^ n;
    uint32_t h = (n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7FFFFFFFu;
    return 1.0f - (float)h / 1073741824.0f;
}

static float noise1(float x)
{
    int32_t i = (int32_t)x;                          /* cvt.w.s: toward zero */
    float f = x - (float)i;
    float s0 = 0.5f * lattice(i) + 0.25f * lattice(i - 1) + 0.25f * lattice(i + 1);
    float s1 = 0.5f * lattice(i + 1) + 0.25f * lattice(i) + 0.25f * lattice(i + 2);
    return s0 * (1 - f) + s1 * f;
}

/* 0x13C160: the octaves (the count at 0x44CA54, 5), each half as strong at twice the frequency */
float cloth_noise(float x)
{
    float total = noise1(x), amp = 1, freq = 1;
    for (int o = 1; o <= 5; o++) {
        amp *= 0.5f;
        freq *= 2;
        total += amp * noise1(x * freq);
    }
    return total;
}

/* 0x3473B0: t = TimeOffset + the game's clock (+0x30 of the clock at gp-0x6CFC [G]: seconds). The direction swings
 * between the min and max angle with |noise(t DirFreq)|, as (-sin a, cos a, 0); its strength HorizontalWindForce x
 * |noise(t ForceFreq)|; up z VerticalWindForce x |noise(t VerticalWindFreq)| [R] */
void cloth_flag_wind(const ClothWind *w, float t, float out[3])
{
    if (!w || !out)
        return;
    float tt = w->time_offset + t;
    float a = fabsf(cloth_noise(tt * w->dir_freq));
    float ang = a * w->max_angle + (1 - a) * w->min_angle;
    float m = w->force * fabsf(cloth_noise(tt * w->force_freq));
    out[0] = -sinf(ang) * m;
    out[1] = cosf(ang) * m;
    out[2] = w->vert_force * fabsf(cloth_noise(tt * w->vert_freq));
}
