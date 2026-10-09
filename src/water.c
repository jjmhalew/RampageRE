/* water.c - the game's water surfaces (water.h; docs/WATER_NOTES.md). A port of the WaterObject code: the reader
 * 0x338CC0, the grid 0x338510 (the far-off flat quad 0x3381B0), the frame 0x336DC0, its waves 0x337C40 / 0x339BB0,
 * the pressing 0x336960 / 0x3368F0 (rectangle 0x336C30, disc 0x336B40, box 0x336A30), the monster's push 0x3369E0,
 * the floor 0x337E00, the draw ROWater 0x339D00 and its VU1 program 0x37C120 (shader 0x7E of the table at 0x40F910).
 * [R] = read in that code, [G] = a guess or a stand-in where it matters.
 *
 * The game's object (WaterObject, 0x1B4 bytes) and what it is here:
 *   +0x08 its matrix (identity, translation +0x2C = Origin + (0, offset, 0)), +0x104 its box, +0x144 flat,
 *   +0x148 the grid's xy, +0x14C / +0x150 the two height buffers, +0x170 the older (overwritten with the newer each
 *   frame), +0x174 the newer, swapped at the end of the frame, +0x154 / +0x15C the CylWaves (64 bytes each),
 *   +0x160 / +0x168 the BoxWaves (80 bytes), +0x16C the material, +0x178 / +0x17C the vertices per row / column,
 *   +0x180 the strip's triangle count, +0x184 / +0x188 the half sizes, +0x18C TileSize, +0x194 / +0x198 the floor's
 *   inner half sizes, +0x19C MinGroundHeight, +0x1A0 / +0x1A4 1 / the floor's slope widths, +0x1A8 their ratio,
 *   +0x1AC Damp, +0x1B0 WaveSpeed, +0xF8 the normals, +0xFC the vertex buffer, +0x100 the strip's index buffer. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "water.h"

struct Water {
    int cols, rows;                                  /* +0x178, +0x17C: NumCols + 1, NumRows + 1 */
    float tile;                                      /* +0x18C */
    float half[2];                                   /* +0x184, +0x188 */
    float inner[2];                                  /* +0x194, +0x198 */
    float min_ground;                                /* +0x19C */
    float damp, speed;                               /* +0x1AC, +0x1B0 */
    float origin[3];                                 /* +0x2C: the translation, world */
    int flat;                                        /* +0x144 */
    int nwaves; WaterWave waves[WATER_MAX_WAVES];    /* +0x154, +0x160 */
    float *xy;                                       /* +0x148: local x, y per vertex */
    float *older, *newer;                            /* +0x170, +0x174 */
    float *nrm;                                      /* +0xF8 */
    float *pos, *grid_uv;                            /* world xyz; the grid's own uv (0x338510) */
    uint32_t *tris; int ntris;
    float time;                                      /* the clock at gp-0x6CFC +0x30 */
    char tex[64];
};

static int name_is(const char *name, const char *type)   /* "CylWave|..." against "CylWave", any case [R 0x339310] */
{
    size_t n = strlen(type);
    for (size_t i = 0; i < n; i++) {
        char a = name[i], b = type[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b)
            return 0;
    }
    return name[n] == '|' || name[n] == 0;
}

/* ---- reading (0x338CC0) */
int water_read(Group g, WaterDef *def)
{
    if (!def)
        return -1;
    memset(def, 0, sizeof *def);
    if (!g.at)
        return -1;
    Record r;
    if (!group_get(g, "NumCols", &r) || !group_get(g, "NumRows", &r))
        return -1;
    def->num_cols = (int)group_num(g, "NumCols", 0);    /* +0x178 = NumCols + 1 [R] */
    def->num_rows = (int)group_num(g, "NumRows", 0);    /* +0x17C = NumRows + 1 [R] */
    if (def->num_cols < 1 || def->num_rows < 1 || def->num_cols > 1024 || def->num_rows > 1024)
        return -1;
    def->tile = group_num(g, "TileSize", 1);            /* +0x18C [R]; the default is a stand-in [G] */
    def->damp = group_num(g, "Damp", 0.98f);            /* +0x1AC [R] */
    def->wave_speed = group_num(g, "WaveSpeed", 5);     /* +0x1B0 [R] */
    Group o = group_sub(g, "Origin");                   /* three list values through 0x28DB00 [R] */
    if (o.at) {
        Record v[3];
        uint32_t n = group_items(o, v, 3);
        for (uint32_t i = 0; i < n; i++)
            def->origin[i] = record_num(v[i]);
    }
    def->has_min_ground = group_get(g, "MinGroundHeight", &r);
    def->min_ground = def->has_min_ground ? group_num(g, "MinGroundHeight", 0) : 0;
    def->min_ground_xscale = group_num(g, "MinGroundHeightXScale", 1);
    def->min_ground_yscale = group_num(g, "MinGroundHeightYScale", 1);
    const char *env = group_str(g, "Texture|EnvMap");   /* the key "EnvMap" through the object (0x3391F8) [R] */
    if (env) {
        strncpy(def->env_map, env, sizeof def->env_map - 1);
        def->env_map[sizeof def->env_map - 1] = 0;
    }
    /* WaveList: each CylWave| holds exactly 4 SineWave| of exactly 4 values, a BoxWave| 5 of 4; anything else is
     * skipped [R 0x3392AC .. 0x3397E8]. The game keeps the two kinds in two arrays and runs the CylWaves first */
    Group wl = group_sub(g, "WaveList");
    for (int pass = 0; pass < 2 && wl.at; pass++) {
        const char *type = pass == 0 ? "CylWave" : "BoxWave";
        int need = pass == 0 ? 4 : 5;
        for (uint32_t i = 0, n = group_count(wl); i < n && def->nwaves < WATER_MAX_WAVES; i++) {
            Record w = group_record(wl, i);
            if ((w.kind & 0xFF) != PK_GROUP || !name_is(w.name, type))
                continue;
            Group wg = record_group(wl, w);
            if (group_count(wg) != (uint32_t)need)
                continue;
            WaterWave wave;
            memset(&wave, 0, sizeof wave);
            wave.kind = pass == 0 ? WATER_CYLWAVE : WATER_BOXWAVE;
            int ok = 1;
            for (int k = 0; k < need && ok; k++) {
                Record s = group_record(wg, (uint32_t)k);
                Group sg = record_group(wg, s);
                Record v[5];
                if ((s.kind & 0xFF) != PK_GROUP || group_count(sg) != 4 || group_items(sg, v, 4) != 4) {
                    ok = 0;
                    break;
                }
                wave.s[k].freq = record_num(v[0]);
                wave.s[k].phase = record_num(v[1]);
                wave.s[k].amp = record_num(v[2]);
                wave.s[k].offset = record_num(v[3]);
            }
            if (ok)
                def->waves[def->nwaves++] = wave;
        }
    }
    return 0;
}

/* ---- setting up */
void water_free(Water *w)
{
    if (!w)
        return;
    free(w->xy); free(w->older); free(w->newer); free(w->nrm); free(w->pos); free(w->grid_uv); free(w->tris);
    free(w);
}

static void build_positions(Water *w);

Water *water_new(const WaterDef *def, const float offset[3], int flat)
{
    if (!def || def->num_cols < 1 || def->num_rows < 1 || def->num_cols > 1024 || def->num_rows > 1024)
        return NULL;
    Water *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->flat = flat != 0;                             /* +0x144 = (the offset > 0) [R 0x338E84] */
    w->tile = def->tile;
    w->damp = def->damp;
    w->speed = def->wave_speed;
    for (int k = 0; k < 3; k++)                      /* the matrix: identity, Origin (+ the offset on y) [R 0x338D30] */
        w->origin[k] = def->origin[k] + (offset ? offset[k] : 0);
    /* the half sizes: NumCols x TileSize / 2 [R 0x338F88] */
    w->half[0] = (float)def->num_cols * def->tile * 0.5f;
    w->half[1] = (float)def->num_rows * def->tile * 0.5f;
    /* the floor: MinGroundHeight over the inner rectangle, sloping to 0 at the edge; without it, 0 [R 0x338FDC] */
    if (def->has_min_ground) {
        w->min_ground = def->min_ground;
        w->inner[0] = def->min_ground_xscale * w->half[0];
        w->inner[1] = def->min_ground_yscale * w->half[1];
    } else {
        w->min_ground = 0;
        w->inner[0] = w->half[0];
        w->inner[1] = w->half[1];
    }
    strncpy(w->tex, def->env_map, sizeof w->tex - 1);
    if (!w->flat) {                                  /* a flat one reads no waves [R 0x3392A0] */
        w->nwaves = def->nwaves < WATER_MAX_WAVES ? def->nwaves : WATER_MAX_WAVES;
        for (int i = 0; i < w->nwaves; i++) {
            WaterWave *v = &w->waves[i];
            *v = def->waves[i];
            /* the centre waves' amplitude in half sizes [R 0x3394B8 CylWave sines 2, 3; 0x339730 BoxWave 3, 4] */
            int cx = v->kind == WATER_CYLWAVE ? 2 : 3;
            v->s[cx].amp *= w->half[0];
            v->s[cx + 1].amp *= w->half[1];
        }
    }

    /* the grid (0x338510): row by row, x = TileSize col - half x, y = TileSize row - half y, all still; the far-off
     * one (0x3381B0) is one quad, 2 x 2 vertices at +-half */
    w->cols = w->flat ? 2 : def->num_cols + 1;
    w->rows = w->flat ? 2 : def->num_rows + 1;
    int n = w->cols * w->rows;
    w->xy = malloc(2 * (size_t)n * sizeof *w->xy);
    w->older = calloc((size_t)n, sizeof *w->older);
    w->newer = calloc((size_t)n, sizeof *w->newer);
    w->nrm = malloc(3 * (size_t)n * sizeof *w->nrm);
    w->pos = malloc(3 * (size_t)n * sizeof *w->pos);
    w->grid_uv = malloc(2 * (size_t)n * sizeof *w->grid_uv);
    w->ntris = 2 * (w->cols - 1) * (w->rows - 1);
    w->tris = malloc(3 * (size_t)w->ntris * sizeof *w->tris);
    if (!w->xy || !w->older || !w->newer || !w->nrm || !w->pos || !w->grid_uv || !w->tris) {
        water_free(w);
        return NULL;
    }
    for (int r = 0; r < w->rows; r++)
        for (int c = 0; c < w->cols; c++) {
            int i = r * w->cols + c;
            if (w->flat) {
                w->xy[2 * i] = 2 * w->half[0] * (float)c - w->half[0];      /* [R 0x338350] */
                w->xy[2 * i + 1] = 2 * w->half[1] * (float)r - w->half[1];
            } else {
                w->xy[2 * i] = w->tile * (float)c - w->half[0];             /* [R 0x338714] */
                w->xy[2 * i + 1] = w->tile * (float)r - w->half[1];
            }
            w->grid_uv[2 * i] = (float)c / (float)(w->cols - 1);
            w->grid_uv[2 * i + 1] = (float)r / (float)(w->rows - 1);
            w->nrm[3 * i] = 0; w->nrm[3 * i + 1] = 0; w->nrm[3 * i + 2] = 1;  /* +0xF8 = (0, 0, 1) [R 0x33875C] */
        }
    /* the game draws a strip per row pair, the rows run on without degenerate joins (0x338838: (row + 1, col),
     * (row, col) for every col, row after row); here each cell is two triangles, wound as the strip's are seen from
     * above (its joins between rows are left out) [G] */
    int t = 0;
    for (int r = 0; r + 1 < w->rows; r++)
        for (int c = 0; c + 1 < w->cols; c++) {
            uint32_t a = (uint32_t)(r * w->cols + c), b = a + 1, d = a + (uint32_t)w->cols, e = d + 1;
            w->tris[t++] = d; w->tris[t++] = a; w->tris[t++] = e;
            w->tris[t++] = a; w->tris[t++] = b; w->tris[t++] = e;
        }
    build_positions(w);
    return w;
}

/* ---- the waves (0x337C40) */
static float sine(const WaterSine *s, float t) { return s->amp * sinf(s->freq * t + s->phase) + s->offset; }

/* the grid rectangle about centre c with half sizes e (0x336C30): 0 if it misses the inner vertices. The bounds
 * are vertex indices; x runs x0 <= x < x1, y runs y0 <= y <= y1, as the callers loop [R] */
static int rect(const Water *w, const float c[2], const float e[2], int out[4])
{
    int hc = w->cols >> 1, hr = w->rows >> 1;
    out[0] = hc + (int)((c[0] - e[0]) / w->tile) - 1;      /* cvt.w.s: towards zero, as (int) */
    out[1] = hr + (int)((c[1] - e[1]) / w->tile) - 1;
    if (!(out[0] < w->cols - 1) || !(out[1] < w->rows - 1))
        return 0;
    if (out[0] < 2) out[0] = 1;
    if (out[1] < 2) out[1] = 1;
    out[2] = hc + (int)((c[0] + e[0]) / w->tile) + 1;
    out[3] = hr + (int)((c[1] + e[1]) / w->tile) + 1;
    if (out[2] <= 0 || out[3] <= 0)
        return 0;
    if (out[2] > w->cols - 2) out[2] = w->cols - 2;
    if (out[3] > w->rows - 2) out[3] = w->rows - 2;
    return 1;
}

/* a disc (0x336960 -> 0x336B40): both buffers down to depth where (x - cx)^2 + (y - cy)^2 <= r^2 [R] */
static void press_disc(Water *w, const float c[2], float radius, float depth)
{
    float e[2] = { radius, radius };
    int b[4];
    if (w->flat || !rect(w, c, e, b))
        return;
    float r2 = radius * radius;
    for (int y = b[1]; y <= b[3]; y++)
        for (int x = b[0]; x < b[2]; x++) {
            int i = y * w->cols + x;
            float dx = w->xy[2 * i] - c[0], dy = w->xy[2 * i + 1] - c[1];
            if (dx * dx + dy * dy <= r2) {
                if (!(w->newer[i] <= depth)) w->newer[i] = depth;
                if (!(w->older[i] <= depth)) w->older[i] = depth;
            }
        }
}

/* a box (0x3368F0 -> 0x336A30): the same within |x - cx| <= ex, |y - cy| <= ey; the row test reads the row's y [R] */
static void press_box(Water *w, const float c[2], const float e[2], float depth)
{
    int b[4];
    if (w->flat || !rect(w, c, e, b))
        return;
    for (int y = b[1]; y <= b[3]; y++) {
        if (!(fabsf(c[1] - w->xy[2 * (y * w->cols) + 1]) <= e[1]))
            continue;
        for (int x = b[0]; x < b[2]; x++) {
            int i = y * w->cols + x;
            if (fabsf(c[0] - w->xy[2 * i]) <= e[0]) {
                if (!(w->newer[i] <= depth)) w->newer[i] = depth;
                if (!(w->older[i] <= depth)) w->older[i] = depth;
            }
        }
    }
}

static void run_waves(Water *w, float t)
{
    for (int k = 0; k < w->nwaves; k++) {
        const WaterWave *v = &w->waves[k];
        if (v->kind == WATER_CYLWAVE) {              /* [R 0x337C68] */
            float c[2] = { sine(&v->s[2], t), sine(&v->s[3], t) };
            press_disc(w, c, sine(&v->s[1], t), -sine(&v->s[0], t));
        } else {                                     /* [R 0x339BB0] */
            float c[2] = { sine(&v->s[3], t), sine(&v->s[4], t) };
            float e[2] = { sine(&v->s[1], t), sine(&v->s[2], t) };
            press_box(w, c, e, -sine(&v->s[0], t));
        }
    }
}

/* ---- the frame */
static void build_positions(Water *w)
{
    const float *h = w->newer;
    for (int i = 0, n = w->cols * w->rows; i < n; i++) {
        w->pos[3 * i] = w->origin[0] + w->xy[2 * i];
        w->pos[3 * i + 1] = w->origin[1] + w->xy[2 * i + 1];
        w->pos[3 * i + 2] = w->origin[2] + h[i];
    }
}

void water_step(Water *w, float dt)
{
    if (!w || !(dt > 0))
        return;
    w->time += dt;
    if (w->flat)                                     /* 0x338180: nothing to do [R] */
        return;
    run_waves(w, w->time);                           /* 0x337C40 at the clock's +0x30 [R] */
    /* k = WaveSpeed^2 dt^2 / TileSize^2 (dt: the clock's +0x4) [R 0x336DE4]; the scheme blows up past 0.5, which
     * the game's 1/60 s never nears (0.007); held there for a long frame [G] */
    float k = w->speed * w->speed * dt * dt / (w->tile * w->tile);
    if (k > 0.5f)
        k = 0.5f;
    float centre = 2 - 4 * k;
    int C = w->cols;
    /* the interior: older = Damp (k (4 neighbours of newer) + (2 - 4k) newer - older) [R 0x336E78] */
    for (int r = 1; r < w->rows - 1; r++)
        for (int c = 1; c < C - 1; c++) {
            int i = r * C + c;
            const float *h = w->newer;
            float sum = h[i - C] + h[i + C] + h[i - 1] + h[i + 1];
            w->older[i] = w->damp * (k * sum + centre * h[i] - w->older[i]);
        }
    /* the normals from the new heights, interior only: (h[x-1] - h[x+1], h[y-1] - h[y+1], 2 TileSize), (1, 0, 0) if
     * it is shorter than 1e-3 [R 0x337274] */
    for (int r = 1; r < w->rows - 1; r++)
        for (int c = 1; c < C - 1; c++) {
            int i = r * C + c;
            const float *h = w->older;
            float *n = &w->nrm[3 * i];
            n[0] = h[i - 1] - h[i + 1];
            n[1] = h[i - C] - h[i + C];
            n[2] = 2 * w->tile;
            float l2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
            if (l2 < 1e-6f) {
                n[0] = 1; n[1] = 0; n[2] = 0;
            } else {
                float s = 1 / sqrtf(l2);
                n[0] *= s; n[1] *= s; n[2] *= s;
            }
        }
    float *t = w->older;                             /* the swap [R 0x337C08] */
    w->older = w->newer;
    w->newer = t;
    build_positions(w);                              /* the vertex buffer's z [R 0x336F68] */
}

void water_set_time(Water *w, float t) { if (w) w->time = t; }
float water_time(const Water *w) { return w ? w->time : 0; }

void water_disturb(Water *w, const float pos[3], float radius, float depth)
{
    if (!w || !pos)
        return;
    float c[2] = { pos[0] - w->origin[0], pos[1] - w->origin[1] };   /* 0x3369E0: minus the translation [R] */
    press_disc(w, c, radius, depth);
}

/* ---- the floor (0x337E00). The game walks the eight regions about the inner rectangle; inside the slope it is
 * MinGroundHeight x (1 - the larger of the distances past the inner edge over the slope's width, x and y), which
 * is what its corner tests (+0x1A8 = the y width / the x width) pick [R]. A slope narrower than 0.001 counts as no
 * slope (+0x1A0 / +0x1A4 = 0) [R 0x3390DC] */
int water_contains(const Water *w, float x, float y)
{
    if (!w)
        return 0;
    float dx = x - w->origin[0], dy = y - w->origin[1];
    return !(dx < -w->half[0]) && !(dy < -w->half[1]) && dx <= w->half[0] && dy <= w->half[1];
}

float water_floor(const Water *w, float x, float y)
{
    if (!w)
        return 0;
    if (!water_contains(w, x, y))
        return w->origin[2];
    float ax = fabsf(x - w->origin[0]) - w->inner[0], ay = fabsf(y - w->origin[1]) - w->inner[1];
    float bx = w->half[0] - w->inner[0], by = w->half[1] - w->inner[1];
    float fx = ax > 0 && bx > 0.001f ? ax / bx : 0, fy = ay > 0 && by > 0.001f ? ay / by : 0;
    float f = 1 - (fx > fy ? fx : fy);
    return w->origin[2] + w->min_ground * f;
}

/* ---- the mesh */
int water_cols(const Water *w) { return w ? w->cols : 0; }
int water_rows(const Water *w) { return w ? w->rows : 0; }
int water_vertex_count(const Water *w) { return w ? w->cols * w->rows : 0; }
const float *water_positions(const Water *w) { return w ? w->pos : NULL; }
const float *water_normals(const Water *w) { return w ? w->nrm : NULL; }
const float *water_grid_uv(const Water *w) { return w ? w->grid_uv : NULL; }
const uint32_t *water_triangles(const Water *w, int *ntris) { if (ntris) *ntris = w ? w->ntris : 0; return w ? w->tris : NULL; }
const char *water_texture(const Water *w) { return w && w->tex[0] ? w->tex : NULL; }

/* the VU1 program 0x37C120 (and its clipping twin 0x3906B0), per vertex [R]:
 *   N = the vertex colour / 127, the normal the CPU wrote there as signed bytes (n x 127, truncated: 0x3373FC)
 *   V = position + (+0x2C - the camera's position), uploaded to VU1 0x90 with render flag 8 (0x339D34): the ray from
 *       the camera, world space (the matrix has no rotation)
 *   R = (V - 2 (N.V) N) / |V|
 *   a = 1 + (|Rx| + Ry) / s if Ry <= 0, else 3 - (|Rx| - Ry) / s, s = |Rx| + |Ry| + 1e-9; a = 8 - a if Rx > 0
 *   S = a / 8, T = 1 - Rz (then x Q for the GS, which interpolates in perspective as GL does)
 * so S goes once round (-Y 0, -X 0.25, +Y 0.5, +X 0.75) and T from the horizon (1) up (0) */
void water_env_uv(const Water *w, const float cam[3], float *uv)
{
    if (!w || !cam || !uv)
        return;
    for (int i = 0, n = w->cols * w->rows; i < n; i++) {
        const float *p = &w->pos[3 * i], *m = &w->nrm[3 * i];
        float N[3], V[3];
        for (int k = 0; k < 3; k++) {
            N[k] = (float)(int)(m[k] * 127.0f) * (1.0f / 127.0f);
            V[k] = p[k] - cam[k];
        }
        float l2 = V[0] * V[0] + V[1] * V[1] + V[2] * V[2];
        float q = l2 > 0 ? 1 / sqrtf(l2) : 0;
        float d = 2 * (N[0] * V[0] + N[1] * V[1] + N[2] * V[2]);
        float R[3] = { (V[0] - d * N[0]) * q, (V[1] - d * N[1]) * q, (V[2] - d * N[2]) * q };
        float ax = fabsf(R[0]), s = ax + fabsf(R[1]) + 1e-9f;
        float a = R[1] <= 0 ? 1 + (ax + R[1]) / s : 3 - (ax - R[1]) / s;
        if (R[0] > 0)
            a = 8 - a;
        uv[2 * i] = a * 0.125f;
        uv[2 * i + 1] = 1 - R[2];
    }
}
