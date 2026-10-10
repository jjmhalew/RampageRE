/* shadow.c - the game's shadows (shadow.h; docs/SHADOW_NOTES.md), transcribed from the PS2 code: the lighting keys
 * 0x2B92F0 (and the shadow colour 0x326AB0), the object shadows' ShadowData reader 0x3265D0 and draw 0x325C90, the
 * monster's blobs (ROMonster, vtable 0x44B040: constructor 0x2FF510, update 0x2FDA50 / 0x2FDD00, ground blob 0x2FEA20,
 * wall blob 0x2FDEB0), the ground grid of the buildings' shadows (setup 0x31FC40, mesh 0x31F340, a building's top
 * 0x31F790, the cells 0x31F690, the colours 0x31F540, the draw 0x31F8B0, the colour 0x320040) and the facade darkening
 * (0x31D6B0, 0x31DD90, 0x31DC00, 0x31D9A0, 0x31D560, 0x31DA60, 0x31D7F0, 0x31D8F0; who uses it: building.c's
 * building_light_grid and city.c's building light, docs/BUILDING_LIGHT_NOTES.md).
 * [R] = read in that code, [G] = a guess or a stand-in where it matters.
 *
 * Colours: the game hands the GS bytes where 128 is 1.0. The grid's and the object quads' VU programs (0x3514D0,
 * 0x357B50) store the vertex colour as it is [R]; the blobs' (0x354640) take none, but a constant: the material
 * colour, rgb halved, alpha x 128/255 (colour mode 3, 0x265B28) [R]. So: the grid's alpha byte / 128, the object quads' (halved) bytes / 128, the blobs' / 255. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "shadow.h"

#define SQRT3_INV 0.57735027f
#define FADE_TIME 0.4f        /* gp-0x7210: the blobs fade in and out over 0.4 s [R 0x2FDBF8] */
#define FADE_HEIGHT 5.0f      /* gp-0x7208: the ground blob is gone 5 above its ground [R 0x2FDBCC] */
#define WALL_INSET 0.1f       /* gp-0x720C [R 0x2FDD88] */
#define GRID_X0 (-64.5f)      /* gp-0x7138: the grid's corner [R 0x31F968, 0x31F384] */
#define GRID_Y0 (-2.5f)
#define GRID_STEP 5.0f        /* [R 0x31F380] */
#define GRID_VX 24            /* vertices across, 0 .. 23 [R 0x31F480: 0x18] */
#define GRID_VY 7             /* vertex rows 0 .. 6: six strips [R 0x31F510] */
#define RAMP 0.2f             /* +0x324 of the lighting: 1 / the darkening's ramp [R 0x2B9A80]; MShadowRampDist
                               * (5 in every city) has no key name in the ELF, so it is never read */

/* ---------------------------------------------------------------- the list */
void shadow_list_init(ShadowList *l) { memset(l, 0, sizeof *l); }
void shadow_list_clear(ShadowList *l) { l->nv = 0; l->nb = 0; }
void shadow_list_free(ShadowList *l) { free(l->v); free(l->b); memset(l, 0, sizeof *l); }

static int grow(void **p, int *cap, int need, size_t size)
{
    void *q;
    int n = *cap ? *cap : 64;
    if (need <= *cap)
        return 1;
    while (n < need)
        n *= 2;
    q = realloc(*p, (size_t)n * size);
    if (!q)
        return 0;
    *p = q;
    *cap = n;
    return 1;
}

static int same_tex(const char *a, const char *b) { return a == b || (a && b && strcmp(a, b) == 0); }

/* opens (or continues) a batch and makes room for n more vertices; NULL when out of memory */
static ShadowVert *begin(ShadowList *l, const char *tex, int pass, int depth_test, int alpha_test, int n)
{
    ShadowBatch *b = l->nb ? &l->b[l->nb - 1] : NULL;
    if (!grow((void **)&l->v, &l->vcap, l->nv + n, sizeof *l->v))
        return NULL;
    if (!b || !same_tex(b->texture, tex) || b->pass != pass || b->depth_test != depth_test ||
        b->alpha_test != alpha_test || b->first + b->count != l->nv) {
        if (!grow((void **)&l->b, &l->bcap, l->nb + 1, sizeof *l->b))
            return NULL;
        b = &l->b[l->nb++];
        b->texture = tex;
        b->pass = pass;
        b->depth_test = depth_test;
        b->alpha_test = alpha_test;
        b->first = l->nv;
        b->count = 0;
    }
    b->count += n;
    l->nv += n;
    return &l->v[l->nv - n];
}

static void vert(ShadowVert *v, float x, float y, float z, float u, float t, const float c[4])
{
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
    v->uv[0] = u; v->uv[1] = t;
    memcpy(v->rgba, c, sizeof v->rgba);
}

/* a quad given as a 4-vertex strip q0 q1 q2 q3: triangles (q0, q1, q2), (q2, q1, q3) */
static void strip4(ShadowVert *o, const ShadowVert q[4])
{
    o[0] = q[0]; o[1] = q[1]; o[2] = q[2];
    o[3] = q[2]; o[4] = q[1]; o[5] = q[3];
}

/* a float colour channel to the byte the game packs: x 255 + 0.5, truncated, low 8 bits [R 0x326B10 .. 0x326B78] */
static unsigned byte_of(float f)
{
    float v = 255.0f * f + 0.5f;
    if (v <= 0.0f)
        return 0;
    if (v >= 2147483648.0f)
        v -= 2147483648.0f;   /* the cvt.w.s guard of 0x325E90: past 2^31 it subtracts it and sets the top bit */
    return (unsigned)v & 0xFF;
}

/* ---------------------------------------------------------------- the light and the city's keys */
void shadow_dir(float out[3])
{
    /* +0x308 of the lighting (0x4D2988): (-1, 1, -1) normalised; (1, 0, 0) if it were shorter than 1e-3 [R 0x2B9928] */
    out[0] = -SQRT3_INV;
    out[1] = SQRT3_INV;
    out[2] = -SQRT3_INV;
}

void shadow_lighting_default(ShadowLighting *l)
{
    l->tint[0] = l->tint[1] = l->tint[2] = 0.0f;   /* TShadowTintR/G/B -> +0x328.. */
    l->tint[3] = 0.85f;                            /* TShadowTintA -> +0x334, halved there [R 0x2B9D10] */
    l->ground_intensity = 0.8f;                    /* +0x340 */
    l->monster_intensity = 0.8f;                   /* +0x33C */
    l->mshadow[0] = 0.4f;                          /* +0x314 .. +0x31C */
    l->mshadow[1] = 0.25f;
    l->mshadow[2] = 0.0f;
    l->monster_blob = "shadow_blob";               /* +0x338: a material of shader 0x35 with that texture */
}

/* ---------------------------------------------------------------- 1. object shadows */
void shadow_data_default(ShadowData *d)
{
    d->texture = "shadow_square";
    d->intensity = 0.0f;      /* a missing float key reads as 0 [R 0x326820] */
    d->max_height = 0.0f;
    d->full_height = 0.0f;
    d->ex_proj_height = 0.0f; /* [R 0x326980] */
    d->rect[0] = d->rect[1] = -1.0f;   /* (-1, -1, 1, 1) when ShadowRect is absent [R 0x3269E0] */
    d->rect[2] = d->rect[3] = 1.0f;
}

int shadow_object(ShadowList *out, const ShadowLighting *l, const ShadowData *d, const float m[12])
{
    float dir[3], h, inv, f, a, cx, cy, t, c[4], x[2], y[2];
    unsigned ab, rgb[3];
    ShadowVert q[4], *o;
    int i;
    shadow_dir(dir);
    h = d->max_height - m[11];                     /* RenderShadowData +0xC minus the node's z [R 0x325E54] */
    if (h <= 0.0f)
        return 0;
    inv = d->max_height - d->full_height > 0.001f ? 1.0f / (d->max_height - d->full_height) : 1e10f;  /* +0x10 */
    f = h * inv;
    if (f < 1.0f)
        f = 1.0f;     /* [R 0x325E74: max.s, not min.s] so the shadow never fades: full until MaxHeight, then gone */
    /* alpha byte = 255 x f x Intensity x (TShadowTintA / 2) + 0.5, low 8 bits [R 0x325E7C .. 0x325EC0] */
    a = f * d->intensity * (l->tint[3] * 0.5f);
    ab = byte_of(a);
    if (!ab)
        return 0;
    for (i = 0; i < 3; i++)
        rgb[i] = byte_of(l->tint[i]);              /* manager +0xD14 [R 0x326AB0] */
    /* each byte halved into the vertex colour [R 0x326198 ..]: then 128 = 1.0 */
    for (i = 0; i < 3; i++)
        c[i] = (float)(rgb[i] >> 1) / 128.0f;
    c[3] = (float)(ab >> 1) / 128.0f;
    /* the centre slides along the light from the node's position raised by ExProjHeight down to z = 0
     * [R 0x325EE0 .. 0x325F3C] */
    t = (m[11] + d->ex_proj_height) * (-1.0f / dir[2]);
    cx = m[9] + t * dir[0];
    cy = m[10] + t * dir[1];
    x[0] = d->rect[0]; y[0] = d->rect[1]; x[1] = d->rect[2]; y[1] = d->rect[3];
    /* corners (x0 y0) (x1 y0) (x1 y1) (x0 y1) through the node's x and y axes, flat at z = 0.01 (gp-0x712C), UVs
     * (0 0) (1 0) (1 1) (0 1) [R 0x325F40 .. 0x326060, 0x3260FC, 0x326200 ..] */
    vert(&q[0], cx + x[0] * m[0] + y[0] * m[3], cy + x[0] * m[1] + y[0] * m[4], 0.01f, 0, 0, c);
    vert(&q[1], cx + x[1] * m[0] + y[0] * m[3], cy + x[1] * m[1] + y[0] * m[4], 0.01f, 1, 0, c);
    vert(&q[2], cx + x[1] * m[0] + y[1] * m[3], cy + x[1] * m[1] + y[1] * m[4], 0.01f, 1, 1, c);
    vert(&q[3], cx + x[0] * m[0] + y[1] * m[3], cy + x[0] * m[1] + y[1] * m[4], 0.01f, 0, 1, c);
    /* primitive 6: a quad list the buffer turns into strips through the order 0 1 3 2 (gp-0x78D4) [R 0x326108] */
    o = begin(out, d->texture, SHADOW_PASS_DECAL, 1, 0, 6);  /* depth test on, no write [G: shader 0x4D] */
    if (!o)
        return 0;
    {
        ShadowVert s[4];
        s[0] = q[0]; s[1] = q[1]; s[2] = q[3]; s[3] = q[2];
        strip4(o, s);
    }
    return 1;
}

/* ---------------------------------------------------------------- 2 and 3. the monster */
void shadow_monster_init(ShadowMonster *s) { memset(s, 0, sizeof *s); }   /* [R 0x2FF568 ..] */

void shadow_monster_update(ShadowMonster *s, float dt, const float centre[3], const float body[3], float radius,
                           int mounted, const float mount_box[6], const float *roof_box, int blocked)
{
    float step = dt / FADE_TIME;
    int on = 0;
    /* the ground blob (0x2FDA50) */
    if (!mounted) {                                 /* +0xC644 of the monster [R 0x2FDA80] */
        float r = 0.999f * radius;                  /* [R 0x2FDAAC] */
        s->ground[0] = centre[0];
        s->ground[1] = centre[1];
        s->ground[2] = 0.0f;                        /* the street [R 0x2FDB64] */
        /* over a roof only when the blob (the radius, all round) lies inside it [R 0x2FDAC4 .. 0x2FDB38] */
        if (roof_box && centre[0] + r <= roof_box[3] && !(centre[0] - r < roof_box[0]) &&
            centre[1] + r <= roof_box[4] && !(centre[1] - r < roof_box[1]))
            s->ground[2] = roof_box[5];
        if (blocked) {                              /* [R 0x2FDB98: off at once] */
            s->ground_fade = 0.0f;
            s->ground_on = 0;
        } else
            on = 1;
    }
    if (on) {
        float target = 1.0f - (centre[2] - s->ground[2]) / FADE_HEIGHT, cur = s->ground_fade;
        s->ground_on = 1;
        if (!(target <= cur))
            cur = fminf(target, cur + step);        /* [R 0x2FDC10: min.s] */
        else if (target < cur)
            cur = fmaxf(target, cur - step);        /* [R 0x2FDC44: max.s] */
        if (cur < 0.0f) {
            s->ground_on = 0;
            cur = 0.0f;
        } else if (cur > 1.0f)
            cur = 1.0f;
        s->ground_fade = cur;
    } else if (s->ground_on) {                      /* fade out [R 0x2FDC98] */
        s->ground_fade -= step;
        if (s->ground_fade <= 0.0f) {
            s->ground_fade = 0.0f;
            s->ground_on = 0;
        }
    }
    /* the wall blob (0x2FDD00) */
    memcpy(s->wall_pos, body, sizeof s->wall_pos);  /* +0x34, every frame [R 0x2FDD14] */
    if (mounted && mount_box) {
        memcpy(s->wall_box, mount_box, sizeof s->wall_box);   /* +0x1DC .. of the building + its position */
        s->wall_box[3] += WALL_INSET;                         /* max x + 0.1, max y - 0.1 [R 0x2FDD84] */
        s->wall_box[4] -= WALL_INSET;
        s->wall_on = 1;
        s->wall_fade = fminf(1.0f, s->wall_fade + step);      /* [R 0x2FDE48] */
    } else if (s->wall_on) {
        s->wall_fade -= step;
        if (s->wall_fade <= 0.0f) {
            s->wall_fade = 0.0f;
            s->wall_on = 0;
        }
    }
}

/* the wall's quad, clipped to the face: a along the face (x or y) from a0 to a1, z from z0 to z1 */
static void wall_quad(ShadowList *out, const char *tex, int plane_x, float plane, float a0, float a1, float u0,
                      float u1, float z0, float z1, float v0, float v1, const float c[4])
{
    ShadowVert q[4], *o = begin(out, tex, SHADOW_PASS_WALL, 0, 0, 6);
    if (!o)
        return;
    if (plane_x) {
        vert(&q[0], plane, a0, z0, u0, v0, c);
        vert(&q[1], plane, a1, z0, u1, v0, c);
        vert(&q[2], plane, a0, z1, u0, v1, c);
        vert(&q[3], plane, a1, z1, u1, v1, c);
    } else {
        vert(&q[0], a0, plane, z0, u0, v0, c);
        vert(&q[1], a1, plane, z0, u1, v0, c);
        vert(&q[2], a0, plane, z1, u0, v1, c);
        vert(&q[3], a1, plane, z1, u1, v1, c);
    }
    strip4(o, q);   /* primitive 4, a strip of 2 triangles [R 0x2FE0CC] */
}

void shadow_monster_draw(ShadowList *out, const ShadowLighting *l, const ShadowMonster *s, const float rot[9],
                         float radius)
{
    float c[4];
    int i;
    for (i = 0; i < 3; i++)
        c[i] = (float)byte_of(l->tint[i]) / 255.0f;   /* +0x58: TShadowTint RGB [R 0x2FF578] */
    /* the ground blob (0x2FEA20, called while +0x18 is set) */
    if (s->ground_on) {
        float hx = 1.2f * radius, hy = 0.8f * radius, z = 0.01f;   /* [R 0x2FEB64, 0x2FEB70, 0x2FEBF4] */
        float t[3], lx[4], ly[4];
        ShadowVert q[4], *o;
        c[3] = (float)byte_of(s->ground_fade * l->monster_intensity) / 255.0f;   /* [R 0x2FEAD4 ..] */
        /* the world matrix: the monster's 3x3, at the ground point raised 0.01 [R 0x2FEA58 .. 0x2FEAC4] */
        t[0] = s->ground[0];
        t[1] = s->ground[1];
        t[2] = s->ground[2] + 0.01f;
        lx[0] = -hx; ly[0] = -hy;   /* UV (0 0) */
        lx[1] = hx;  ly[1] = -hy;   /* (1 0) */
        lx[2] = -hx; ly[2] = hy;    /* (0 1) */
        lx[3] = hx;  ly[3] = hy;    /* (1 1) */
        for (i = 0; i < 4; i++)
            vert(&q[i], t[0] + lx[i] * rot[0] + ly[i] * rot[3] + z * rot[6],
                 t[1] + lx[i] * rot[1] + ly[i] * rot[4] + z * rot[7],
                 t[2] + lx[i] * rot[2] + ly[i] * rot[5] + z * rot[8], (float)(i & 1), (float)(i >> 1), c);
        o = begin(out, l->monster_blob, SHADOW_PASS_DECAL, 1, 0, 6);   /* [G: depth state left as it was] */
        if (o)
            strip4(o, q);
    }
    /* the wall blob (0x2FDEB0, called while +0x2C is set) */
    if (s->wall_on) {
        const float *b = s->wall_box, *p = s->wall_pos;
        float z1 = fminf(p[2] + 7.0f, b[5]);            /* f27 [R 0x2FDEF8] */
        float z0 = p[2] - 5.0f, v0, v1, a0, a1;          /* f26 [R 0x2FDF28] */
        if (z0 < 0.0f)
            z0 = 0.0f;
        if (!(z0 < z1))
            return;
        /* the texture is centred on the body point: 5 below to 7 above, 5 to the left to 2 to the right, clipped to
         * the face [R 0x2FDF4C .. 0x2FDFA8, 0x2FE0D0 .. 0x2FE108] */
        v0 = 0.5f - 0.5f * 0.2f * (p[2] - z0);
        v1 = 0.5f + 0.5f * (1.0f / 7.0f) * (z1 - p[2]);
        c[3] = (float)byte_of(s->wall_fade * l->monster_intensity) / 255.0f;   /* [R 0x2FDFB4 ..] */
        /* the front face, y = box min y [R 0x2FE070 ..] */
        a0 = p[0] - 5.0f;
        if (a0 < b[0])
            a0 = b[0];
        a1 = p[0] + 2.0f;
        if (!(a1 <= b[3]))
            a1 = b[3];
        if (a0 < a1)
            wall_quad(out, l->monster_blob, 0, b[1], a0, a1, 0.5f - 0.1f * (p[0] - a0), 0.5f + 0.25f * (a1 - p[0]),
                      z0, z1, v0, v1, c);
        /* the right side face, x = box max x, when the body is right of it [R 0x2FE508 ..] */
        if (!(p[0] <= b[3])) {
            a0 = p[1] - 5.0f;
            if (a0 < b[1])
                a0 = b[1];
            a1 = p[1] + 2.0f;
            if (!(a1 <= b[4]))
                a1 = b[4];
            if (a0 < a1)
                wall_quad(out, l->monster_blob, 1, b[3], a0, a1, 0.5f - 0.1f * (p[1] - a0),
                          0.5f + 0.25f * (a1 - p[1]), z0, z1, v0, v1, c);
        }
    }
}

/* ---------------------------------------------------------------- 4. the ground grid */
/* the stand-in for the game's ray against the building's model (0x27FFF0 on +0x74 with its matrix): its box */
static int ray_box(const float b[6], const float o[3], const float d[3], float maxdist, float *t)
{
    float t0 = 0.0f, t1 = maxdist;
    int i;
    for (i = 0; i < 3; i++) {
        if (fabsf(d[i]) < 1e-12f) {
            if (o[i] < b[i] || o[i] > b[i + 3])
                return 0;
        } else {
            float a = (b[i] - o[i]) / d[i], e = (b[i + 3] - o[i]) / d[i];
            if (a > e) { float x = a; a = e; e = x; }
            if (a > t0) t0 = a;
            if (e < t1) t1 = e;
            if (t0 > t1)
                return 0;
        }
    }
    *t = t0;
    return 1;
}

void shadow_ground_init(ShadowGround *g, int enabled, int nbuildings, const float (*boxes)[6], ShadowRayFn ray,
                        void *ctx)
{
    float dir[3], up[3];
    int b, r, c;
    memset(g, 0, sizeof *g);
    g->enabled = enabled != 0;                      /* +0x0 [R 0x31FCE0] */
    if (!g->enabled)
        return;
    if (nbuildings > SHADOW_MAX_BUILDINGS)
        nbuildings = SHADOW_MAX_BUILDINGS;
    g->nbuildings = nbuildings;
    for (b = 0; b < nbuildings; b++)
        g->top[b] = boxes[b][5];                    /* +0xA68: +0x1F0, the box's top [R 0x31FD38] */
    shadow_dir(dir);
    up[0] = -dir[0]; up[1] = -dir[1]; up[2] = -dir[2];   /* towards the light [R 0x31FDCC ..] */
    for (r = 0; r < SHADOW_GRID_ROWS; r++)
        for (c = 0; c < SHADOW_GRID_COLS; c++) {
            float o[3];
            o[0] = GRID_X0 + GRID_STEP * (float)(c + 1);  /* -59.5 + 5c [R 0x31FE00] */
            o[1] = GRID_Y0 + GRID_STEP * (float)(r + 1);  /* 2.5 + 5r [R 0x31FE68] */
            o[2] = -0.001f;                               /* [R 0x31FD80] */
            g->cell[r][c].n = 0;
            g->cell[r][c].lit = 1;
            for (b = 0; b < nbuildings; b++) {
                float t;
                int hit = ray ? ray(ctx, b, o, up, 100.0f, &t) : ray_box(boxes[b], o, up, 100.0f, &t);
                if (hit && g->cell[r][c].n != 4) {        /* at most four [R 0x31FED0] */
                    int n = g->cell[r][c].n++;
                    g->cell[r][c].who[n] = (unsigned char)b;
                    g->cell[r][c].lit = 0;
                    g->cell[r][c].hit[n] = t * up[2];      /* the height on the building the ray met */
                }
            }
        }
    g->dirty[0] = g->dirty[1] = -1000.0f;           /* all of it [R 0x31FF5C] */
    g->dirty[2] = g->dirty[3] = 1000.0f;
}

void shadow_ground_top(ShadowGround *g, int b, const float box[6])
{
    float dir[3], k, p[4];
    if (!g->enabled || b < 0 || b >= g->nbuildings || g->top[b] == box[5])   /* [R 0x31F7A0, 0x31F7C0] */
        return;
    shadow_dir(dir);
    /* the old shadow's reach: the box's min x moved along the light by the old top, its max y likewise; the new top
     * is kept [R 0x31F7CC .. 0x31F834] */
    k = -g->top[b] / dir[2];
    g->top[b] = box[5];
    p[0] = box[0] + dir[0] * k;
    p[1] = box[1];
    p[2] = box[3];
    p[3] = box[4] + dir[1] * k;
    /* both corners into the dirty rectangle (0x31F860) */
    g->dirty[0] = fminf(g->dirty[0], fminf(p[0], p[2]));
    g->dirty[1] = fminf(g->dirty[1], fminf(p[1], p[3]));
    g->dirty[2] = fmaxf(g->dirty[2], fmaxf(p[0], p[2]));
    g->dirty[3] = fmaxf(g->dirty[3], fmaxf(p[1], p[3]));
}

void shadow_ground_draw(ShadowList *out, ShadowGround *g, const ShadowLighting *l)
{
    float col[GRID_VY][GRID_VX], c[4];
    unsigned ab;
    int i, j;
    if (!g->enabled)
        return;
    /* the dirty cells again (0x31F8B0 -> 0x31F690): a sample is lit unless a building it met still reaches the
     * height the ray met it at [R 0x31F6E4 .. 0x31F738] */
    if (!(g->dirty[2] < g->dirty[0]) && !(g->dirty[3] < g->dirty[1])) {
        int c0 = (int)(0.2f * (g->dirty[0] - GRID_X0) - 0.001f), r0 = (int)(0.2f * (g->dirty[1] - GRID_Y0) - 0.001f);
        int c1 = (int)(0.2f * (g->dirty[2] - GRID_X0) + 0.999f), r1 = (int)(0.2f * (g->dirty[3] - GRID_Y0) + 0.999f);
        int r, cc, k;
        if (c0 <= 0) c0 = 1;                         /* [R 0x31FA54 ..] */
        if (r0 <= 0) r0 = 1;
        if (c1 > SHADOW_GRID_COLS) c1 = SHADOW_GRID_COLS;
        if (r1 > SHADOW_GRID_ROWS) r1 = SHADOW_GRID_ROWS;
        for (r = r0; r <= r1; r++)
            for (cc = c0; cc <= c1; cc++) {
                ShadowCell *s = &g->cell[r - 1][cc - 1];
                s->lit = 1;
                for (k = 0; k < s->n; k++)
                    if (!(g->top[s->who[k]] < s->hit[k])) {
                        s->lit = 0;
                        break;
                    }
            }
        g->dirty[0] = g->dirty[1] = 1.7e38f;         /* empty again [R 0x31FAE4] */
        g->dirty[2] = g->dirty[3] = -1.7e38f;
    }
    /* the colour (0x320040): TShadowTint RGB, alpha = TShadowTintA / 2 x GroundShadowIntensity as a byte, given to the
     * GS as it is [R 0x320148 .. 0x3201D4] */
    ab = byte_of(l->tint[3] * 0.5f * l->ground_intensity);
    for (i = 0; i < 3; i++)
        c[i] = fminf((float)byte_of(l->tint[i]) / 128.0f, 1.0f);
    c[3] = fminf((float)ab / 128.0f, 1.0f);
    /* vertex (i, j) is the sample (i - 1, j - 1); the outer ring is never shadowed [R 0x31F340, 0x31F540] */
    for (j = 0; j < GRID_VY; j++)
        for (i = 0; i < GRID_VX; i++)
            col[j][i] = (i >= 1 && i <= SHADOW_GRID_COLS && j >= 1 && j <= SHADOW_GRID_ROWS &&
                         !g->cell[j - 1][i - 1].lit) ? 1.0f : 0.0f;
    /* six strips of 23 cells at z = 0 (0x12A vertices, 0x128 triangles, primitive 4) [R 0x31F340, 0x31FBB0]; a cell
     * is the strip's triangles (b_i, t_i, b_i+1) and (t_i, b_i+1, t_i+1); the all-clear ones are left out */
    for (j = 0; j < GRID_VY - 1; j++)
        for (i = 0; i < GRID_VX - 1; i++) {
            static const int tri[6][2] = { {0, 0}, {0, 1}, {1, 0}, {0, 1}, {1, 0}, {1, 1} };   /* (di, dj) */
            static const float none[4] = { 0, 0, 0, 0 };
            ShadowVert *o;
            int k;
            if (col[j][i] == 0 && col[j + 1][i] == 0 && col[j][i + 1] == 0 && col[j + 1][i + 1] == 0)
                continue;
            o = begin(out, NULL, SHADOW_PASS_GROUND, 0, 1, 6);   /* ZTST ALWAYS, ATST GREATER 1 [R 0x31FB40 ..] */
            if (!o)
                return;
            for (k = 0; k < 6; k++) {
                int vi = i + tri[k][0], vj = j + tri[k][1];
                vert(&o[k], GRID_X0 + GRID_STEP * (float)vi, GRID_Y0 + GRID_STEP * (float)vj, 0.0f, 0, 0,
                     col[vj][vi] != 0 ? c : none);
            }
        }
}

/* ---------------------------------------------------------------- facade darkening */
static float dot3(const float *n, const float *p) { return n[0] * p[0] + n[1] * p[1] + n[2] * p[2]; }

/* a neighbour's shadow on the right face x = fx (0x31D9A0) */
static void right_box(ShadowFacade *f, const float nb[6], float fx)
{
    float dir[3], t, *e;
    if (f->nright >= 2)
        return;
    shadow_dir(dir);
    t = (fx - nb[0]) / dir[0];
    e = f->right[f->nright++];
    e[0] = fx; e[1] = nb[1] + dir[1] * t; e[2] = 0.0f;
    e[3] = fx; e[4] = nb[4] + dir[1] * t; e[5] = nb[5] + dir[2] * t;
}

void shadow_facade_build(ShadowFacade *f, const float own[6], const float *nb_a, const float *nb_b)
{
    float dir[3];
    memset(f, 0, sizeof *f);
    shadow_dir(dir);
    f->front_y = own[1] + 0.1f;                     /* +0xBC [R 0x31D6FC] */
    f->right_x = own[3] - 1.0f;                     /* +0xC0 [R 0x31D708] */
    if (nb_a) {
        float b[6], fy = own[1], t, x0, x1, z0, z1, *n, len;
        memcpy(b, nb_a, sizeof b);
        b[5] += 2.0f;                               /* its top + 2 (gp-0x7144) [R 0x31D748] */
        /* 0x31DD90: the neighbour's left edge (x = its min x, up to its top) carried along the light onto the front
         * plane y = own min y, from its front corner (x0, z0) and its back corner (x1, z1) */
        t = (fy - b[1]) / dir[1];
        x0 = b[0] + dir[0] * t;                     /* +0x34 */
        z0 = b[5] + dir[2] * t;                     /* +0x3C */
        t = (fy - b[4]) / dir[1];
        x1 = b[0] + dir[0] * t;                     /* +0x38 */
        z1 = b[5] + dir[2] * t;                     /* +0x40 */
        f->plane[0][0] = 1; f->plane[0][3] = x0;                 /* x >= x0 */
        f->plane[1][0] = -1; f->plane[1][3] = -x1;               /* x <= x1 */
        n = f->plane[2];                                          /* under the sloped top */
        n[0] = z1 - z0; n[1] = 0; n[2] = -(x1 - x0);
        len = n[0] * n[0] + n[2] * n[2];
        if (len < 1e-6f) {
            n[0] = 1; n[2] = 0;
        } else {
            len = 1.0f / sqrtf(len);
            n[0] *= len; n[2] *= len;
        }
        n[3] = n[0] * x1 + n[1] * fy + n[2] * z1;
        /* 0x31DC00: it counts only when that span is 1e-3 wide or more and meets the face's x range widened by 2
         * (the polygon it also builds at +0x44 is not ported: its reader was not traced) */
        if (x1 - x0 >= 0.001f) {
            float lo = own[0] - 2.0f, hi = own[3] + 2.0f;
            if ((lo <= x0 || lo < x1) && !(hi < x1 && hi <= x0))
                f->front = 1;
        }
        if (!(nb_a[0] < own[3]))                     /* it lies to the right [R 0x31D79C] */
            right_box(f, nb_a, own[3]);
    }
    if (nb_b)
        right_box(f, nb_b, own[3]);
}

/* colour -= MShadowRGB x min(depth / 5, 1), not below 0 [R 0x31D5D8 .. 0x31D678, 0x31DB48 .., 0x31D828 ..] */
static int shade(const ShadowLighting *l, float depth, float rgb[3])
{
    float k = fminf(depth * RAMP, 1.0f);
    int i;
    for (i = 0; i < 3; i++) {
        rgb[i] -= l->mshadow[i] * k;
        if (rgb[i] < 0)
            rgb[i] = 0;
    }
    return 1;
}

int shadow_facade_front(const ShadowFacade *f, const ShadowLighting *l, const float p[3], float rgb[3])
{
    float e0, e2;
    if (!f->front || dot3(f->plane[1], p) < f->plane[1][3])   /* 0x31DA60 */
        return 0;
    e0 = dot3(f->plane[0], p) - f->plane[0][3];
    e2 = dot3(f->plane[2], p) - f->plane[2][3];
    if (e0 < 0 || e2 < 0)
        return 0;
    return shade(l, fminf(e0, e2), rgb);
}

int shadow_facade_right(const ShadowFacade *f, const ShadowLighting *l, const float p[3], float rgb[3])
{
    float depth = 1e10f;                             /* 0x501502F9 */
    int i, in = 0;
    for (i = 0; i < f->nright; i++) {                /* 0x31D8F0: y and z only, x is not tested */
        const float *b = f->right[i];
        float dy = p[1] - b[1], dz = b[5] - p[2];
        if (dy < 0 || b[4] - p[1] < 0 || dz < 0)
            continue;
        depth = fminf(fminf(depth, dy), dz);
        in = 1;
    }
    return in ? shade(l, depth, rgb) : 0;
}

int shadow_facade_darken(const ShadowFacade *f, const ShadowLighting *l, const float p[3], float rgb[3])
{
    if (p[1] <= f->front_y)                          /* the front face [R 0x31D56C] */
        return shadow_facade_front(f, l, p, rgb);
    if (p[0] <= f->right_x)                          /* [R 0x31D590] */
        return 0;
    return shadow_facade_right(f, l, p, rgb);
}
