/* fx.c - the game's particle effects (docs/FX_NOTES.md), transcribed from the PS2 code: the EDF reader (0x1ED400,
 * coded lines; 0x1FA4D0, one emitter's keys; 0x1F97D0, what is derived at load), the graphs (factory 0x20D630), an
 * emitter's frame (ParticleSystemFx::advance 0x1F2900 calls the emitter update chosen per definition, 0x20EFD0 /
 * 0x213F30 / 0x216470, through the member pointer at definition +0x180) and how a particle is drawn (0x2214D0 builds
 * the VU1 packets of the billboards, 0x21C7E0 the streaks). [R] = read in that code, [G] = a guess where it matters.
 *
 * The game keeps an emitter's particles in the emitter's frame when +EmitterRelativeParticles and in the world otherwise
 * (only the relative ones go through the emitter's matrix at the draw [R 0x222400..0x222520]); here everything is
 * simulated in world axes relative to the effect's origin, unscaled, and scaled on output: when the effect moves
 * (fx_move, fx_place) the others are moved back by as much, so they stay where they were, and the relative ones turn
 * with the new frame.
 * The game advances effects once per frame with gravity added per frame [R 0x2105E0], so fx_update runs fixed 1/60 s
 * frames: the effects world's dt is 1 / the display rate (0x309210), 60 here (50 for a PAL game) [R]. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "fx.h"
#include "iso.h"

#define FX_PI 3.14159265f
#define FX_STEP (1.0f / 60.0f)
#define MAX_PARTS 8192
#define MAX_EMITS 1024
#define MAX_FX 256
#define MAX_TEX 8
#define MAX_CHILD 4
#define MAX_SHAPE_MESH 8

/* ---------------------------------------------------------------- definitions */
/* emitter flags (definition +0x0) [R 0x1FBEE8.. the flag names and their bits] */
enum {
    EF_AUTOSTART = 0x1, EF_CYCLELOOPS = 0x2, EF_LIFESIZELINKED = 0x4, EF_STATIONARY = 0x8, EF_RELATIVE = 0x10,
    EF_BREAKSREL = 0x20, EF_ENDOFLIFE = 0x40, EF_EMITEMITTERS = 0x80, EF_RELDISTLIMIT = 0x100, EF_PCTSTRETCH = 0x200,
    EF_COLLISIONROD = 0x400, EF_PREDICTABLE = 0x800, EF_RANDOMXF = 0x1000, EF_ONETIME = 0x2000, EF_DATAPOINTS = 0x4000,
    EF_ELLIPSOID = 0x8000, EF_SURFACE = 0x10000, EF_WORLDMATRIX = 0x20000, EF_DATASETGEN = 0x40000,
    EF_XF_AGE = 0x800000, EF_XF_ROTX = 0x1000000, EF_XF_ROTY = 0x2000000, EF_XF_ROTZ = 0x4000000
};
/* particle flags (definition +0xD0) */
enum {
    PF_TESTZ = 0x1, PF_WRITEZ = 0x2, PF_ADDITIVE = 0x4, PF_LOOPTEX = 0x8, PF_GROUND = 0x10, PF_BUILDINGS = 0x20,
    PF_OBJECTS = 0x40, PF_TINY = 0x80, PF_NORENDER = 0x100, PF_NOBILLBOARD = 0x200, PF_WATER = 0x400,
    PF_RANDOMTILE = 0x1000, PF_RANDOMROT = 0x2000, PF_DISCRETEROT = 0x4000, PF_CYCLEINAREA = 0x20000,
    PF_LINEARAIR = 0x40000, PF_STRETCHALT = 0x80000, PF_CROSSHATCH = 0x100000, PF_DIEWITHEMITTER = 0x200000,
    PF_REALIGN = 0x1000000
};
enum { PT_NORMAL, PT_STRETCH, PT_XFORM };                         /* enum.particle_type -> +0xD8 */
enum { BL_NORMAL, BL_LIGHTEN, BL_DARKEN, BL_DIFFERENCE, BL_MULTIPLY, BL_NONE };   /* enum.emvc.*_blend */

/* the graphs of an emitter definition, in the order of the key table 0x1FA784.. (definition offset in comments) */
enum {
    G_GEN,                        /* +0x24 graph.gen_rate */
    G_AREA,                       /* +0x28..0x30 graph.start_area.x/y/z */
    G_VFS = G_AREA + 3,           /* +0x34 graph.vel_from_start */
    G_VEL,                        /* +0x38..0x40 graph.em_vel.x/y/z */
    G_VELR = G_VEL + 3,           /* +0x44..0x4C graph.em_vel_r.x/y/z */
    G_OFS = G_VELR + 3,           /* +0x50..0x58 graph.em_pt_offset.x/y/z */
    G_LIFE = G_OFS + 3,           /* +0x5C, 0x60 graph.p_lifetime.min/max */
    G_SIZE = G_LIFE + 2,          /* +0x64, 0x68 graph.p_size.min/max */
    G_PVM = G_SIZE + 2,           /* +0x6C..0x74 graph.p_vel_mult.x/y/z */
    G_START = G_PVM + 3,          /* +0x84 graph.em_start_time */
    G_EMVC,                       /* +0xC..0x18 graph.emvc.r/g/b/a */
    G_VC = G_EMVC + 4,            /* +0x128..0x134 graph.vertex_color.r/g/b/a */
    G_SIZEMUL = G_VC + 4,         /* +0x138 graph.size_mul */
    G_TILE,                       /* +0xF8 graph.tile_advance */
    G_ROT,                        /* +0x108 graph.rotation */
    G_SRD,                        /* +0x10C graph.start_rot_distribution */
    G_ORBIT_IN, G_ORBIT_ALONG,    /* +0x88, 0x8C graph.orbit_accel_in/along */
    G_COUNT
};

enum { GR_NONE, GR_CONST, GR_POLY, GR_LERP, GR_SINE };
typedef struct {
    uint8_t kind, n;      /* poly: degree; lininterp: points */
    float c[6];           /* const c0; poly c0..cn (ascending powers); sine amplitude, frequency, phase, offset */
    uint32_t at;          /* lininterp: first point in FxLib.pts (x, 1 / (x - previous x), y) */
} Graph;

typedef struct {
    uint32_t eflags, enorm, pflags, pnorm;   /* +0x0, +0x8, +0xD0, +0xD4 */
    int ptype, pmode, cblend, ablend;        /* +0xD8, +0xDC, +0x1C, +0x20 */
    int gnd, spawn, ntiles[2], tile_step, repeat, fwd_axis, nshapes, ntex;
    int shape[MAX_TEX];                      /* shapes.name (+0x120 count, +0x124): indices into FxLib.shapes */
    char sound[2][48];                       /* enum.sound_loop_2d (+0xA4), enum.sound_2d (+0xB0); "" none */
    float cycle[2], shutdown, grav, air, stretch_len, stretch_pct, destretch_pct, offset[3];
    int rgb_const, a_const;                  /* the vertex colour graphs do not change (+0xD0 0x400000 / 0x800000) */
    Graph g[G_COUNT];
    const Bundle *texb[MAX_TEX];
    uint32_t tex[MAX_TEX];
} EmDef;

enum { FX_NONE, FX_PARTICLE, FX_COMPOSITE, FX_SHAKE };
typedef struct {
    char name[48];
    int type, nem, nchild;
    EmDef *em;
    char child_name[MAX_CHILD][48];
    int child[MAX_CHILD];
} Effect;

typedef struct {                                 /* an XForm particle's model: "<name>.pwk", its meshes "<name>|.." */
    char name[48];
    const Bundle *b;
    int nmesh;
    const BdpEntry *mesh[MAX_SHAPE_MESH];
} Shape;

struct FxLib {
    char *words[342];
    Effect *fx;
    int nfx;
    Shape *shapes;
    int nshapes;
    float *pts;
    uint32_t npts, cap;
    const Bundle *const *bundles;
    int nbundles;
};

/* ---------------------------------------------------------------- graphs (0x20D630) */
static float sine_table[129];

static void sine_init(void)                 /* GraphSineTable: 129 values of sin over a turn [R 0x20DFD0] */
{
    if (sine_table[32] != 0)
        return;
    for (int i = 0; i <= 128; i++)
        sine_table[i] = sinf(2 * FX_PI * i / 128.0f);
}

static const char *skip_word(const char *s)     /* past the type word and the blanks after it [R 0x20D670] */
{
    while (*s && *s != ' ' && *s != '\t')
        s++;
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static int read_floats(const char **s, float *out, int max)
{
    int n = 0;
    while (n < max) {
        char *end;
        float v = strtof(*s, &end);
        if (end == *s)
            break;
        out[n++] = v;
        *s = end;
    }
    return n;
}

static int lib_points(FxLib *L, int n)
{
    if (L->npts + 3u * n > L->cap) {
        uint32_t cap = L->cap ? L->cap * 2 : 4096;
        while (cap < L->npts + 3u * n)
            cap *= 2;
        float *p = realloc(L->pts, cap * sizeof *p);
        if (!p)
            return -1;
        L->pts = p;
        L->cap = cap;
    }
    int at = (int)L->npts;
    L->npts += 3u * n;
    return at;
}

/* "const v", "poly n c0 .. cn" (or polynomial; n = 0 is a constant), "lininterp n x0 y0 x1 y1 .." (or
 * linearinterpolation), "sin a f p o" (or sine). The factory looks for the words with strstr in this order; a
 * reciprocal is not used by any effect and becomes 0 [R 0x20D630..0x20E0F8] */
static int graph_parse_into(FxLib *L, Graph *g, const char *s);
/* [R 0x1EDC60] a line the factory cannot read (0x20D630 answers NULL) leaves the slot as it was */
static void graph_parse(FxLib *L, Graph *g, const char *s)
{
    Graph t;
    if (graph_parse_into(L, &t, s))
        *g = t;
}
static int graph_parse_into(FxLib *L, Graph *g, const char *s)
{
    memset(g, 0, sizeof *g);
    if (strstr(s, "const")) {
        g->kind = GR_CONST;
        g->c[0] = (float)atof(skip_word(strstr(s, "const")));
    } else if (strstr(s, "reciprocal") || strstr(s, "reci")) {
        g->kind = GR_CONST;
    } else if (strstr(s, "poly")) {
        const char *p = skip_word(strstr(s, "poly"));
        char *end;
        long n = strtol(p, &end, 10);
        if (end == p || n < 0 || n > 5) {
            g->kind = GR_CONST;
            return 1;
        }
        p = end;
        if (n == 0) {
            g->kind = GR_CONST;
            g->c[0] = (float)atof(p);
            return 1;
        }
        g->kind = GR_POLY;
        g->n = (uint8_t)n;
        read_floats(&p, g->c, (int)n + 1);         /* missing coefficients stay 0 [R 0x20DAD0..] */
    } else if (strstr(s, "lininterp") || strstr(s, "linearinterpolation")) {
        const char *p = skip_word(strstr(s, "lin"));
        char *end;
        long n = strtol(p, &end, 10);
        if (end == p || n <= 0 || n > 255) {
            g->kind = GR_CONST;
            return 1;
        }
        p = end;
        int at = lib_points(L, (int)n);
        if (at < 0) {
            g->kind = GR_CONST;
            return 1;
        }
        g->kind = GR_LERP;
        g->n = (uint8_t)n;
        g->at = (uint32_t)at;
        float *pt = L->pts + at;
        for (int i = 0; i < n; i++, pt += 3) {      /* 0x20BA80: a missing x is the last + 0.1, a missing y the last y */
            float xy[2];
            int got = read_floats(&p, xy, 2);
            if (got < 1)
                xy[0] = i ? pt[-3] + 0.1f : 0;
            if (got < 2)
                xy[1] = i ? pt[-1] : 0;
            float dx = i ? xy[0] - pt[-3] : 1000;
            if (dx == 0)
                dx = 1000;
            pt[0] = xy[0];
            pt[1] = 1 / dx;
            pt[2] = xy[1];
        }
    } else if (strstr(s, "sin")) {
        const char *p = skip_word(strstr(s, "sin"));
        g->kind = GR_SINE;
        int got = read_floats(&p, g->c, 4);
        if (got <= 1) {                               /* "sin" with fewer than two numbers: 1 sin(t) [R 0x20E06C] */
            memset(g->c, 0, sizeof g->c);
            g->c[0] = 1;
            g->c[1] = 1;
        }
        sine_init();
    } else
        return 0;                                     /* not a graph: rejected [R 0x20E0D8] */
    return 1;
}

static float graph_eval(const FxLib *L, const Graph *g, float t)
{
    switch (g->kind) {
    case GR_CONST:
        return g->c[0];
    case GR_POLY: {                                    /* GraphLinear 0x20EDC0, Quadratic.. Quintic: c0 + c1 t + .. */
        float v = g->c[g->n];
        for (int i = g->n - 1; i >= 0; i--)
            v = v * t + g->c[i];
        return v;
    }
    case GR_LERP: {                                    /* 0x20B980: held at both ends, straight lines between */
        const float *p = L->pts + g->at;
        int n = g->n;
        if (t <= p[0])
            return p[2];
        if (t >= p[3 * (n - 1)])
            return p[3 * (n - 1) + 2];
        int i = 1;
        while (i < n - 1 && !(t <= p[3 * i]))
            i++;
        float f = (t - p[3 * (i - 1)]) * p[3 * i + 1];
        return (1 - f) * p[3 * (i - 1) + 2] + f * p[3 * i + 2];
    }
    case GR_SINE: {                                    /* 0x20B1E0: a sin(f t + p) + o through the 128-step table */
        float u = (g->c[1] * t + g->c[2]) * (1 / (2 * FX_PI));
        float x = (u - floorf(u)) * 128;
        int i = (int)x;
        if (i > 127)
            i = 127;
        float f = x - i;
        return g->c[0] * ((1 - f) * sine_table[i] + f * sine_table[i + 1]) + g->c[3];
    }
    }
    return 0;
}

static void graph_const(Graph *g, float v)
{
    memset(g, 0, sizeof *g);
    g->kind = GR_CONST;
    g->c[0] = v;
}

static int graph_is_const(const Graph *g) { return g->kind == GR_CONST; }

/* ---------------------------------------------------------------- the coded text (0x1ED400) */
/* one coded line to its words: "AA".."ZZ" a dictionary word, "?text?" a literal inside the line, "?text" a literal to
 * the end, "=" itself. key gets the words before the "=", val the ones after, each joined by one space; without a "="
 * key holds the whole line. Returns 1 if there was a "=". */
static int decode_line(const FxLib *L, const char *s, int n, char *key, int keycap, char *val, int valcap)
{
    char *dst = key;
    int cap = keycap, len = 0, eq = 0;
    key[0] = val[0] = 0;
    for (int i = 0; i < n;) {
        const char *word;
        int wl;
        if (s[i] == '=') {
            if (!eq) {
                eq = 1;
                dst = val;
                cap = valcap;
                len = 0;
            }
            i++;
            continue;
        }
        if (s[i] == '?') {
            int j = i + 1;
            while (j < n && s[j] != '?')
                j++;
            word = s + i + 1;
            wl = j - i - 1;
            i = j < n ? j + 1 : n;
        } else if (i + 1 < n && s[i] >= 'A' && s[i] <= 'Z' && s[i + 1] >= 'A' && s[i + 1] <= 'Z') {
            int w = (s[i] - 'A') * 26 + (s[i + 1] - 'A');
            word = w < 342 && L->words[w] ? L->words[w] : "";
            wl = (int)strlen(word);
            i += 2;
        } else {
            i++;
            continue;
        }
        if (len && len + 1 < cap)
            dst[len++] = ' ';
        for (int k = 0; k < wl && len + 1 < cap; k++)
            dst[len++] = word[k];
        dst[len] = 0;
    }
    return eq;
}

/* ---------------------------------------------------------------- one emitter's keys (0x1FA4D0) */
typedef struct { const char *name; int field, bit; } FlagName;
enum { FL_E, FL_EN, FL_P, FL_PN, FL_STRETCH };
/* the names in the order 0x1FBEE8.. tries them: a token is +Name or -Name, compared over the name's length, so the
 * order matters; a token without the sign is skipped [R 0x1FBEAC, 0x1FD930] */
static const FlagName FLAGS[] = {
    { "StretchEmissions", FL_STRETCH, 0 }, { "AutoStart", FL_E, EF_AUTOSTART }, { "CycleLoops", FL_E, EF_CYCLELOOPS },
    { "LifetimeAndSizeLinked", FL_E, EF_LIFESIZELINKED }, { "Stationary", FL_E, EF_STATIONARY },
    { "EmitterRelativeParticles", FL_E, EF_RELATIVE }, { "BreaksRelativityUponShutdown", FL_E, EF_BREAKSREL },
    { "EndOfLifeUponShutdown", FL_E, EF_ENDOFLIFE }, { "EmitEmitters", FL_E, EF_EMITEMITTERS },
    { "RelativeDistanceLimit", FL_E, EF_RELDISTLIMIT }, { "UsesPercentStretchLength", FL_E, EF_PCTSTRETCH },
    { "ProvidesCollisionRod", FL_E, EF_COLLISIONROD }, { "PredictableEmissionRate", FL_E, EF_PREDICTABLE },
    { "RandomizeEmitterXF", FL_E, EF_RANDOMXF }, { "OneTimeEmission", FL_E, EF_ONETIME },
    { "RenderAsDatapoints", FL_E, EF_DATAPOINTS }, { "EllipsoidalStartArea", FL_E, EF_ELLIPSOID },
    { "SurfaceStart", FL_E, EF_SURFACE }, { "UseWorldMatrix", FL_E, EF_WORLDMATRIX },
    { "UsesDatasetGenerator", FL_E, EF_DATASETGEN }, { "NormalizedAll", FL_EN, 0x3FF }, { "NormalizedEmAll", FL_EN, 0x3FF },
    { "NormalizedVertexAlpha", FL_EN, 0x200 }, { "NormalizedGenerationRate", FL_EN, 0x1 },
    { "NormalizedStartArea", FL_EN, 0x2 }, { "NormalizedVelFromStartCenter", FL_EN, 0x4 },
    { "NormalizedEmVelocity", FL_EN, 0x8 }, { "NormalizedEmVelocityRandom", FL_EN, 0x10 },
    { "NormalizedEmOffset", FL_EN, 0x20 }, { "NormalizedParticleLifetime", FL_EN, 0x40 },
    { "NormalizedParticleSize", FL_EN, 0x80 }, { "TestZBuffer", FL_P, PF_TESTZ }, { "WriteZBuffer", FL_P, PF_WRITEZ },
    { "AdditiveAlpha", FL_P, PF_ADDITIVE }, { "RandomizeInitialRotation", FL_P, PF_RANDOMROT },
    { "LoopingTextureAnimation", FL_P, PF_LOOPTEX }, { "CollidesWithGround", FL_P, PF_GROUND },
    { "CollidesWithBuildings", FL_P, PF_BUILDINGS }, { "CollidesWithObjects", FL_P, PF_OBJECTS },
    { "CollidesWithWater", FL_P, PF_WATER }, { "UsesTinyParticles", FL_P, PF_TINY }, { "NormalizedPaAll", FL_PN, 0x3F },
    { "NormalizedSizeMultiplier", FL_PN, 0x4 }, { "UsesStretchInformation", FL_STRETCH, 0 },
    { "NoRender", FL_P, PF_NORENDER }, { "DontBillboard", FL_P, PF_NOBILLBOARD },
    { "NormalizedTextureCenter", FL_PN, 0x10 }, { "NormalizedTileAdvance", FL_PN, 0x10 },
    { "NormalizedTextureRadius", FL_PN, 0x20 }, { "NormalizedRotation", FL_PN, 0x8 },
    { "RandomStartTile", FL_P, PF_RANDOMTILE }, { "RandomStartRotation", FL_P, PF_RANDOMROT },
    { "UseDiscreteRotation", FL_P, PF_DISCRETEROT }, { "CycleWithinStartArea", FL_P, PF_CYCLEINAREA },
    { "LinearAirResistance", FL_P, PF_LINEARAIR }, { "StretchAlternateAlignment", FL_P, PF_STRETCHALT },
    { "StretchCrosshatchRendering", FL_P, PF_CROSSHATCH }, { "ParticlesDieWithEmitter", FL_P, PF_DIEWITHEMITTER },
    { "StretchRealignDirection", FL_P, PF_REALIGN }, { "XF_AgeNotDistribution", FL_E, EF_XF_AGE },
    { "XF_RotatesAroundX", FL_E, EF_XF_ROTX }, { "XF_RotatesAroundY", FL_E, EF_XF_ROTY },
    { "XF_RotatesAroundZ", FL_E, EF_XF_ROTZ },
};

static void parse_flags(EmDef *d, const char *s)
{
    while (*s) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        const char *tok = s;
        while (*s && *s != ' ' && *s != '\t')
            s++;
        if (*tok != '+' && *tok != '-')
            continue;
        int on = *tok == '+';
        tok++;
        for (size_t i = 0; i < sizeof FLAGS / sizeof *FLAGS; i++) {
            const FlagName *f = &FLAGS[i];
            if (_strnicmp(tok, f->name, strlen(f->name)) != 0)
                continue;
            uint32_t *field = f->field == FL_E ? &d->eflags : f->field == FL_EN ? &d->enorm :
                              f->field == FL_P ? &d->pflags : &d->pnorm;
            if (f->field == FL_STRETCH) {
                if (on)
                    d->ptype = PT_STRETCH;
            } else if (on) {
                *field |= (uint32_t)f->bit;
            } else {
                *field &= ~(uint32_t)f->bit;
            }
            break;
        }
    }
}

/* an enum value: the first name it starts with, over the name's length, in the game's order; -1 if none */
static int parse_enum(const char *s, const char *const *names, const int *values, int n)
{
    for (int i = 0; i < n; i++)
        if (_strnicmp(s, names[i], strlen(names[i])) == 0)
            return values[i];
    return -1;
}

static const char *const TYPE_NAMES[] = { "Normal", "Stretch", "XForm", "Shape" };
static const int TYPE_VALUES[] = { PT_NORMAL, PT_STRETCH, PT_XFORM, PT_XFORM };
static const char *const MODE_NAMES[] = { "Normal", "RotatesOnly", "TilesNoRotation", "Tiles", "StreamsNoRotation", "Streams" };
static const int MODE_VALUES[] = { 0, 2, 4, 6, 8, 10 };
static const char *const BLEND_NAMES[] = { "Normal", "Lighten", "Darken", "Difference", "Multiply", "None" };
static const int BLEND_VALUES[] = { 0, 1, 2, 3, 4, 5 };
/* ground responses and their index into the table at 0x3B2170 [R 0x1FD9C0..] */
static const char *const COL_NAMES[] = {
    "Nothing", "Bounce", "HalfBounce", "QuarterBounce", "Slide", "Stop", "DeleteCheckCenter", "DeleteCheckEntire",
    "Delete", "AgeHalfEnd", "AgeQuarterEnd", "AgeHalf", "AgeQuarter", "BounceAgeHalfEnd", "BounceAgeQuarterEnd",
    "BounceAgeHalf", "BounceAgeQuarter", "HalfBounceAgeHalfEnd", "HalfBounceAgeQuarterEnd", "HalfBounceAgeHalf",
    "HalfBounceAgeQuarter", "QuarterBounceAgeHalfEnd", "QuarterBounceAgeQuarterEnd", "QuarterBounceAgeHalf",
    "QuarterBounceAgeQuarter" };
static const int COL_VALUES[] = { 0, 1, 2, 3, 4, 5, 7, 8, 6, 11, 12, 9, 10, 15, 16, 13, 14, 19, 20, 17, 18, 23, 24, 21, 22 };
/* the table at 0x3B2170 (28 bytes each, the floats): how far into the ground the particle may go in radii, what is
 * kept of the speed into the ground (reflected) and along it, then the new age = a + b age + c lifetime */
static const float COL_TABLE[25][6] = {
    { 0, -1, 1, 0, 1, 0 }, { 1, 1, 1, 0, 1, 0 }, { 1, .5f, 1, 0, 1, 0 }, { 1, .25f, 1, 0, 1, 0 }, { 1, 0, 1, 0, 1, 0 },
    { 1, 0, 0, 0, 1, 0 }, { 1, 0, 0, 0, 0, 2 }, { 0, 0, 0, 0, 0, 2 }, { -1, 0, 0, 0, 0, 2 },
    { 1, -1, 1, 0, 1, .5f }, { 1, -1, 1, 0, 1, .25f }, { 1, -1, 1, 0, .5f, .5f }, { 1, -1, 1, 0, .75f, .25f },
    { 1, 1, 1, 0, 1, .5f }, { 1, 1, 1, 0, 1, .25f }, { 1, 1, 1, 0, .5f, .5f }, { 1, 1, 1, 0, .75f, .25f },
    { 1, .5f, 1, 0, 1, .5f }, { 1, .5f, 1, 0, 1, .25f }, { 1, .5f, 1, 0, .5f, .5f }, { 1, .5f, 1, 0, .75f, .25f },
    { 1, .25f, 1, 0, 1, .5f }, { 1, .25f, 1, 0, 1, .25f }, { 1, .25f, 1, 0, .5f, .5f }, { 1, .25f, 1, 0, .75f, .25f },
};

/* the graph keys: name and slot (0x1FA784.. builds this table on the stack: key, target, "required") */
typedef struct { const char *key; int slot; } GraphKey;
static const GraphKey GRAPH_KEYS[] = {
    { "graph.gen_rate", G_GEN }, { "graph.start_area.x", G_AREA }, { "graph.start_area.y", G_AREA + 1 },
    { "graph.start_area.z", G_AREA + 2 }, { "graph.vel_from_start", G_VFS }, { "graph.em_vel.x", G_VEL },
    { "graph.em_vel.y", G_VEL + 1 }, { "graph.em_vel.z", G_VEL + 2 }, { "graph.em_vel_r.x", G_VELR },
    { "graph.em_vel_r.y", G_VELR + 1 }, { "graph.em_vel_r.z", G_VELR + 2 }, { "graph.em_pt_offset.x", G_OFS },
    { "graph.em_pt_offset.y", G_OFS + 1 }, { "graph.em_pt_offset.z", G_OFS + 2 }, { "graph.p_lifetime.min", G_LIFE },
    { "graph.p_lifetime.max", G_LIFE + 1 }, { "graph.p_size.min", G_SIZE }, { "graph.p_size.max", G_SIZE + 1 },
    { "graph.p_vel_mult.x", G_PVM }, { "graph.p_vel_mult.y", G_PVM + 1 }, { "graph.p_vel_mult.z", G_PVM + 2 },
    { "graph.em_start_time", G_START }, { "graph.emvc.r", G_EMVC }, { "graph.emvc.g", G_EMVC + 1 },
    { "graph.emvc.b", G_EMVC + 2 }, { "graph.emvc.a", G_EMVC + 3 }, { "graph.vertex_color.r", G_VC },
    { "graph.vertex_color.g", G_VC + 1 }, { "graph.vertex_color.b", G_VC + 2 }, { "graph.vertex_color.a", G_VC + 3 },
    { "graph.vertex_alpha", G_VC + 3 }, { "graph.size_mul", G_SIZEMUL }, { "graph.tile_advance", G_TILE },
    { "graph.rotation", G_ROT }, { "graph.start_rot_distribution", G_SRD }, { "graph.orbit_accel_in", G_ORBIT_IN },
    { "graph.orbit_accel_along", G_ORBIT_ALONG },
};

/* the defaults 0x1FA4D0 and the constructor 0x200690 set before the keys are read */
static void emdef_init(EmDef *d)
{
    memset(d, 0, sizeof *d);
    d->eflags = EF_AUTOSTART | EF_BREAKSREL | EF_PCTSTRETCH;    /* 0x221 */
    d->enorm = 0x3FF;                                           /* every emitter graph over the cycle (0..1) */
    d->pflags = PF_TESTZ | PF_DISCRETEROT;                      /* 0x80004001 */
    d->pnorm = 0x3F;                                            /* every particle graph over the lifetime (0..1) */
    d->shutdown = 1;
    d->stretch_len = 1;
    d->stretch_pct = 0.3f;
    d->destretch_pct = 0.7f;
    d->spawn = -1;
    d->fwd_axis = 2;
    d->ntiles[0] = d->ntiles[1] = 1;
    d->tile_step = 1;
    d->repeat = 1;
    d->cycle[0] = d->cycle[1] = 1;
}

static void find_texture(const FxLib *L, const char *name, const Bundle **b, uint32_t *tex)
{
    *b = NULL;
    *tex = 0;
    for (int i = 0; i < L->nbundles; i++) {
        int n = bdp_find_name(L->bundles[i], name);
        if (n >= 0 && bdp_entry_of(L->bundles[i], (uint32_t)n, ET_TEXTURE)) {
            *b = L->bundles[i];
            *tex = (uint32_t)n;
            return;
        }
    }
}

/* a shape by name, added to the library the first time: the model "<name>.pwk" (type 16) of the first bundle that has
 * one, and that bundle's meshes "<name>|<object>" (the game resolves shapes.name to SimpleShapes at load, refcounted
 * [R 0x1F91F8]; 0x19B300 adds ".pwk", reuses a loaded one (a case-blind hash), else a loose file, else the loaded
 * bundles (0x19BA60 -> 0x1D2930) [R]) */
static int lib_shape(FxLib *L, const char *name)
{
    for (int i = 0; i < L->nshapes; i++)
        if (_stricmp(L->shapes[i].name, name) == 0)
            return i;
    Shape *sh = realloc(L->shapes, (size_t)(L->nshapes + 1) * sizeof *sh);
    if (!sh)
        return -1;
    L->shapes = sh;
    sh = &L->shapes[L->nshapes];
    memset(sh, 0, sizeof *sh);
    snprintf(sh->name, sizeof sh->name, "%s", name);
    char pwk[64];
    snprintf(pwk, sizeof pwk, "%s.pwk", name);
    size_t nl = strlen(name);
    for (int bi = 0; bi < L->nbundles && !sh->b; bi++) {
        const Bundle *b = L->bundles[bi];
        int n = bdp_find_name(b, pwk);
        if (n < 0 || !bdp_entry_of(b, (uint32_t)n, ET_PWK))
            continue;
        sh->b = b;
        for (uint32_t k = 0; k < b->nentries && sh->nmesh < MAX_SHAPE_MESH; k++) {
            const char *en = bdp_name(b, b->entries[k].name);
            if (b->entries[k].type == ET_MESH && _strnicmp(en, name, nl) == 0 && en[nl] == '|')
                sh->mesh[sh->nmesh++] = &b->entries[k];
        }
    }
    return L->nshapes++;
}

static void emdef_key(FxLib *L, EmDef *d, const char *key, const char *val)
{
    for (size_t i = 0; i < sizeof GRAPH_KEYS / sizeof *GRAPH_KEYS; i++)
        if (_stricmp(key, GRAPH_KEYS[i].key) == 0) {
            graph_parse(L, &d->g[GRAPH_KEYS[i].slot], val);
            return;
        }
    if (_strnicmp(key, "flags.", 6) == 0)
        parse_flags(d, val);
    else if (_stricmp(key, "range.cycle_length") == 0) {
        const char *p = val;
        int n = read_floats(&p, d->cycle, 2);
        if (n == 1)
            d->cycle[1] = d->cycle[0];
    } else if (_stricmp(key, "vector.em_pt_offset") == 0) {
        const char *p = val;
        read_floats(&p, d->offset, 3);
    } else if (_stricmp(key, "rgb.vertex_color") == 0) {           /* the old form: 0..255 [R 0x1FBC40] */
        const char *p = val;
        float c[3] = { 255, 255, 255 };
        read_floats(&p, c, 3);
        for (int i = 0; i < 3; i++)
            graph_const(&d->g[G_VC + i], c[i] / 255);
    } else if (_stricmp(key, "val.cycle_shutdown_time") == 0) d->shutdown = (float)atof(val);
    else if (_stricmp(key, "val.grav_multiplier") == 0) d->grav = (float)atof(val);
    else if (_stricmp(key, "val.air_resistance") == 0) d->air = (float)atof(val);
    else if (_stricmp(key, "val.stretchlength") == 0) d->stretch_len = (float)atof(val);
    else if (_stricmp(key, "val.stretch_percent_time") == 0) d->stretch_pct = (float)atof(val);
    else if (_stricmp(key, "val.destretch_percent_time") == 0) d->destretch_pct = (float)atof(val);
    else if (_stricmp(key, "int.num_tiles.u") == 0) d->ntiles[0] = atoi(val);
    else if (_stricmp(key, "int.num_tiles.v") == 0) d->ntiles[1] = atoi(val);
    else if (_stricmp(key, "int.start_tile_step") == 0) d->tile_step = atoi(val);
    else if (_stricmp(key, "int.multiple_render") == 0) d->repeat = atoi(val);
    else if (_stricmp(key, "int.emitter_to_spawn") == 0) d->spawn = atoi(val);
    else if (_stricmp(key, "int.forward_axis") == 0) d->fwd_axis = atoi(val);
    else if (_stricmp(key, "enum.particle_type") == 0) {
        int v = parse_enum(val, TYPE_NAMES, TYPE_VALUES, 4);
        if (v >= 0) d->ptype = v;
    } else if (_stricmp(key, "enum.particle_mode") == 0) {
        int v = parse_enum(val, MODE_NAMES, MODE_VALUES, 6);
        if (v >= 0) d->pmode = v;
    } else if (_stricmp(key, "enum.emvc.color_blend") == 0) {
        int v = parse_enum(val, BLEND_NAMES, BLEND_VALUES, 6);
        if (v >= 0) d->cblend = v;
    } else if (_stricmp(key, "enum.emvc.alpha_blend") == 0) {
        int v = parse_enum(val, BLEND_NAMES, BLEND_VALUES, 6);
        if (v >= 0) d->ablend = v;
    } else if (_stricmp(key, "enum.gnd_col_resp") == 0) {
        int v = parse_enum(val, COL_NAMES, COL_VALUES, 25);
        if (v >= 0) d->gnd = v;
    } else if (_stricmp(key, "textures.name") == 0) {          /* a list of texture names (+0x118 count, +0x11C) */
        const char *s = val;
        d->ntex = 0;
        while (*s && d->ntex < MAX_TEX) {
            while (*s == ' ')
                s++;
            char name[64];
            int n = 0;
            while (*s && *s != ' ') {
                if (n < 63)
                    name[n++] = *s;
                s++;
            }
            name[n] = 0;
            if (n) {
                find_texture(L, name, &d->texb[d->ntex], &d->tex[d->ntex]);
                d->ntex++;
            }
        }
    } else if (_stricmp(key, "shapes.name") == 0) {          /* a list of model names, as textures.name */
        const char *s = val;
        d->nshapes = 0;
        while (*s && d->nshapes < MAX_TEX) {
            while (*s == ' ')
                s++;
            char name[48];
            int n = 0;
            while (*s && *s != ' ') {
                if (n < 47)
                    name[n++] = *s;
                s++;
            }
            name[n] = 0;
            if (n) {
                int k = lib_shape(L, name);
                if (k >= 0)
                    d->shape[d->nshapes++] = k;
            }
        }
    } else if (_stricmp(key, "enum.sound_loop_2d") == 0 || _stricmp(key, "enum.sound_2d") == 0) {
        /* the name after the '|' ("Rampage|fireball" -> "fireball") [R 0x1FE1D4]; "No Sound" is none [R 0x1F84D4] */
        const char *bar = strchr(val, '|');
        char *dst = d->sound[_stricmp(key, "enum.sound_2d") == 0];
        snprintf(dst, sizeof d->sound[0], "%s", bar ? bar + 1 : val);
        if (_stricmp(dst, "No Sound") == 0)
            dst[0] = 0;
    }
    /* not used here: enum.obj_col_resp / con_col_resp (all "nothing"), int.num_emitters_reserved,
     * int.emitterDatasetType, graph.containment.*, graph.texture_center / radius (no effect has them), val.cost_* */
}

/* what 0x1FA4D0's end and 0x1F97D0 fill in: graphs never given, the derived flags [R 0x1FE7B8..0x1FF9A0] */
static void emdef_finish(EmDef *d)
{
    for (int i = 0; i < 3; i++)
        if (d->g[G_OFS + i].kind == GR_NONE)
            graph_const(&d->g[G_OFS + i], d->offset[i]);           /* vector.em_pt_offset, else 0 [R 0x1FE848] */
    if (d->g[G_START].kind == GR_NONE)
        graph_const(&d->g[G_START], 0);
    for (int i = 0; i < 4; i++) {
        if (d->g[G_EMVC + i].kind == GR_NONE)
            graph_const(&d->g[G_EMVC + i], 1);                       /* [R 0x1FF4C0: all three 1] */
        if (d->g[G_VC + i].kind == GR_NONE)
            graph_const(&d->g[G_VC + i], 1);                         /* required: a file without them fails */
    }
    if (d->g[G_SIZEMUL].kind == GR_NONE)
        graph_const(&d->g[G_SIZEMUL], 1);
    if (d->g[G_ROT].kind == GR_NONE)
        graph_const(&d->g[G_ROT], 0);
    if (d->g[G_SRD].kind == GR_NONE)
        graph_const(&d->g[G_SRD], 0);
    if (d->g[G_TILE].kind == GR_NONE)
        graph_const(&d->g[G_TILE], 0);
    d->rgb_const = graph_is_const(&d->g[G_VC]) && graph_is_const(&d->g[G_VC + 1]) && graph_is_const(&d->g[G_VC + 2]);
    d->a_const = graph_is_const(&d->g[G_VC + 3]);
    if (d->spawn < 0)
        d->eflags &= ~(uint32_t)EF_EMITEMITTERS;                     /* [R 0x1FF0D0] */
    for (int i = 0; i < 2; i++) {
        if (d->ntiles[i] < 0)
            d->ntiles[i] = -d->ntiles[i];                            /* one says -4 (unsigned there, a NoRender) */
        if (d->ntiles[i] == 0)
            d->ntiles[i] = 1;
    }
    if (d->tile_step < 1)
        d->tile_step = 1;
    if (d->repeat < 0)
        d->repeat = 0;
    d->pflags &= ~(uint32_t)PF_TINY;   /* [R 0x1F9800] UsesTinyParticles is cleared at load: drawn as the others */
}

/* ---------------------------------------------------------------- an effect file */
static int parse_effect(FxLib *L, Effect *e, const uint8_t *data, uint32_t size)
{
    char key[256], val[1024];
    int depth = 0;
    EmDef *cur = NULL;
    uint32_t i = 0;
    while (i < size) {
        uint32_t j = i;
        while (j < size && data[j] != '\n')
            j++;
        int n = (int)(j - i);
        while (n > 0 && (data[i + n - 1] == '\r' || data[i + n - 1] == 0))
            n--;
        int eq = decode_line(L, (const char *)data + i, n, key, sizeof key, val, sizeof val);
        i = j + 1;
        if (!eq) {
            if (strcmp(key, "{") == 0) {
                EmDef *em = realloc(e->em, (size_t)(e->nem + 1) * sizeof *em);
                if (!em)
                    return -1;
                e->em = em;
                cur = &e->em[e->nem++];
                emdef_init(cur);
                depth = 1;
            } else if (strcmp(key, "}") == 0 && depth) {
                emdef_finish(cur);
                cur = NULL;
                depth = 0;
            }
            continue;
        }
        if (depth) {
            emdef_key(L, cur, key, val);
        } else if (_stricmp(key, "type") == 0) {
            e->type = _stricmp(val, "particle") == 0 ? FX_PARTICLE : _stricmp(val, "composite") == 0 ? FX_COMPOSITE :
                      _stricmp(val, "camerashake") == 0 ? FX_SHAKE : FX_NONE;
        } else if (_strnicmp(key, "edf.name.", 9) == 0) {           /* CompositeFx: its parts by name */
            int k = atoi(key + 9);
            if (k >= 0 && k < MAX_CHILD) {
                snprintf(e->child_name[k], sizeof e->child_name[k], "%s", val);
                if (k + 1 > e->nchild)
                    e->nchild = k + 1;
            }
        }
    }
    if (cur)
        emdef_finish(cur);
    return 0;
}

/* the file offset of address a by the ELF's program headers (RAMPAGE.ELF, split_elf.py's copy with its sections at
 * 0x1000 on, or the disc's SLES_538.00, one segment at 0x80); 0 = not in the file */
static uint32_t elf_offset(const uint8_t *elf, uint32_t size, uint32_t a)
{
    if (size < 52 || memcmp(elf, "\x7f" "ELF", 4) != 0)
        return 0;
    uint32_t ph = rd32(elf + 28), ent = rd16(elf + 42), n = rd16(elf + 44);
    for (uint32_t i = 0; i < n && ent >= 32 && ph + (i + 1) * ent <= size; i++) {
        const uint8_t *p = elf + ph + i * ent;
        uint32_t off = rd32(p + 4), va = rd32(p + 8), filesz = rd32(p + 16);
        if (rd32(p) == 1 && a >= va && a - va < filesz)  /* PT_LOAD */
            return off + (a - va) < size ? off + (a - va) : 0;
    }
    return 0;
}

static int load_words(FxLib *L, const char *data_path)
{
    uint32_t size = 0;
    uint8_t *elf = data_elf(data_path, &size);          /* beside the VOL, or in the disc image (iso.c) */
    if (!elf)
        return -1;
    int ok = 0;
    const uint32_t table = elf_offset(elf, size, 0x3B1C10);
    if (table && table + 4 * 342 <= size) {
        ok = 1;
        for (int i = 0; i < 342 && ok; i++) {
            uint32_t at = elf_offset(elf, size, rd32(elf + table + 4 * i));
            if (!at) {
                ok = 0;
                break;
            }
            const char *s = (const char *)elf + at;
            size_t n = 0, left = (size_t)(size - at);
            while (n < left && s[n])
                n++;
            L->words[i] = malloc(n + 1);
            if (!L->words[i]) {
                ok = 0;
                break;
            }
            memcpy(L->words[i], s, n);
            L->words[i][n] = 0;
        }
        /* the first three words are "Text", "EDF", "file" */
        if (ok && (strcmp(L->words[0], "Text") != 0 || strcmp(L->words[11], "particle") != 0))
            ok = 0;
    }
    free(elf);
    return ok ? 0 : -1;
}

void fx_close(FxLib *L)
{
    if (!L)
        return;
    for (int i = 0; i < 342; i++)
        free(L->words[i]);
    for (int i = 0; i < L->nfx; i++)
        free(L->fx[i].em);
    free(L->fx);
    free(L->shapes);
    free(L->pts);
    free(L);
}

int fx_count(const FxLib *L) { return L ? L->nfx : 0; }
const char *fx_name(const FxLib *L, int e) { return L && e >= 0 && e < L->nfx ? L->fx[e].name : ""; }

int fx_find(const FxLib *L, const char *name)
{
    if (!L || !name)
        return -1;
    char n[64];
    snprintf(n, sizeof n, "%s", name);
    size_t len = strlen(n);
    if (len > 4 && _stricmp(n + len - 4, ".edf") == 0)
        n[len - 4] = 0;
    for (int i = 0; i < L->nfx; i++)
        if (_stricmp(L->fx[i].name, n) == 0)
            return i;
    return -1;
}

FxLib *fx_open(const char *data_path, const Bundle *const *bundles, int nbundles)
{
    FxLib *L = calloc(1, sizeof *L);
    if (!L)
        return NULL;
    if (load_words(L, data_path) != 0) {
        fx_close(L);
        return NULL;
    }
    L->bundles = bundles;
    L->nbundles = nbundles;
    for (int bi = 0; bi < nbundles; bi++) {
        const Bundle *b = bundles[bi];
        for (uint32_t k = 0; k < b->nentries; k++) {
            const BdpEntry *en = &b->entries[k];
            if (en->type != ET_EFFECT || !en->data)
                continue;
            const char *name = bdp_name(b, en->name);
            if (fx_find(L, name) >= 0)
                continue;
            Effect *fx = realloc(L->fx, (size_t)(L->nfx + 1) * sizeof *fx);
            if (!fx)
                break;
            L->fx = fx;
            Effect *e = &L->fx[L->nfx];
            memset(e, 0, sizeof *e);
            snprintf(e->name, sizeof e->name, "%s", name);
            L->nfx++;
            parse_effect(L, e, en->data, en->size);
        }
    }
    for (int i = 0; i < L->nfx; i++)
        for (int k = 0; k < L->fx[i].nchild; k++)
            L->fx[i].child[k] = fx_find(L, L->fx[i].child_name[k]);
    return L;
}

/* ---------------------------------------------------------------- the world */
typedef struct {
    float pos[3], vel[3];          /* relative to the effect's origin, world axes, unscaled (+0x0, +0xC) */
    float age, life, inv_life;     /* +0x18, +0x1C, +0x20 */
    float size;                    /* +0x24 */
    float emvc[4];                 /* +0x34..+0x40: the emitter's colour when it was made */
    float spin, ang0;              /* +0x30 radians a second, +0x2A start angle */
    float stretch, sdir[3];        /* +0x4C length, +0x50 direction of a streak */
    /* XForm: the start orientation (+0x4C, a quaternion there; here the matrix whose columns are the mesh's axes in
     * the world), the tumbling axis in the mesh's frame (+0x5C) and its speed in radians a second (+0x68) */
    float xr0[3][3], xaxis[3], xrate;
    int next, child, em;
    uint16_t tile0;                /* +0x2C */
    uint8_t tex, used, shape;      /* +0x28, +0x29 */
} Part;

typedef struct {
    const EmDef *d;
    const Effect *owner;                   /* the effect whose emitter list d is in (EmitEmitters picks from it) */
    int used, fx, shut, parent, head;
    int sound[2];                          /* the FxSoundFn handles of its two sounds (+0x0, +0x1 their owners) */
    float age, cycle, inv_cycle, acc;      /* +0x1C, +0x20, +0x24, +0x48 */
    float origin[3], R[3][3];              /* where it emits from (relative, unscaled), its axes (rows) */
} Emit;

/* ParticleSystemFx +0x9C .. +0xA4 [R 0x1F2750, 0x1F2E60..0x1F2F08]: an effect with a collision rod (fx_rod) keeps its
 * rod emitter (emitter 0's instance) and the rod's head and tail: the head grows by the rod's speed x dt up to its
 * length, the tail stays 0 while that emitter is neither shut down nor past its cycle and grows by as much after; a
 * hit cuts the head (fx_cut, its vtable +0x80, 0x1F2520). The head is the distance the RelativeDistanceLimit emitters'
 * particles may go (vtable +0xB8, 0x1F1CF0: +0xA0) */
typedef struct {
    int used, gen, nemit;      /* nemit: its live emitters */
    const Effect *e;
    float origin[3], R[3][3], scale;
    int rod;                   /* the rod emitter (w->em), -1 none (+0x9C) */
    float rod_speed, rod_len, head, tail;   /* the rod's (unscaled), +0xA0, +0xA4 */
} Fx;

struct FxWorld {
    const FxLib *lib;
    uint32_t seed;
    float accum;
    int free_part, nparts;
    Part p[MAX_PARTS];
    Emit em[MAX_EMITS];
    Fx fx[MAX_FX];
    FxShakeFn shake;
    void *shake_ctx;
    FxSoundFn sound;
    void *sound_ctx;
};

/* Park-Miller, x 16807 mod 2^31 - 1 [R 0x106BD0]; the float is x * 2^-31 [R 0x213A90] */
static uint32_t rnd(FxWorld *w)
{
    w->seed = (uint32_t)(((uint64_t)w->seed * 16807u) % 2147483647u);
    if (w->seed == 0)
        w->seed = 1;
    return w->seed;
}
static float rnd01(FxWorld *w) { return (float)rnd(w) * (1.0f / 2147483648.0f); }

FxWorld *fx_world(const FxLib *lib)
{
    if (!lib)
        return NULL;
    FxWorld *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->lib = lib;
    w->seed = 1;
    fx_clear(w);
    return w;
}

void fx_world_free(FxWorld *w) { free(w); }

void fx_on_shake(FxWorld *w, FxShakeFn fn, void *ctx)
{
    w->shake = fn;
    w->shake_ctx = ctx;
}

static void emit_sounds_stop(FxWorld *w, Emit *e);

void fx_on_sound(FxWorld *w, FxSoundFn fn, void *ctx)
{
    w->sound = fn;
    w->sound_ctx = ctx;
}

void fx_clear(FxWorld *w)
{
    for (int i = 0; i < MAX_EMITS; i++)
        if (w->em[i].used)
            emit_sounds_stop(w, &w->em[i]);
    memset(w->p, 0, sizeof w->p);
    memset(w->em, 0, sizeof w->em);
    for (int i = 0; i < MAX_FX; i++)
        w->fx[i].used = 0;
    for (int i = 0; i < MAX_PARTS; i++)
        w->p[i].next = i + 1 < MAX_PARTS ? i + 1 : -1;
    w->free_part = 0;
    w->nparts = 0;
    w->accum = 0;
}

int fx_particles(const FxWorld *w) { return w ? w->nparts : 0; }

static void vec_rows(const float R[3][3], const float v[3], float out[3])   /* v in the frame, out in world axes */
{
    for (int i = 0; i < 3; i++)
        out[i] = v[0] * R[0][i] + v[1] * R[1][i] + v[2] * R[2][i];
}

static float len3(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static float em_graph(const FxWorld *w, const Emit *e, int slot, float t) { return graph_eval(w->lib, &e->d->g[slot], t); }

/* the effect's collision rod (ParticleSystemDef, derived at load by 0x1F9290) [R]: only when emitter 0 has AutoStart
 * and +ProvidesCollisionRod; its direction em_vel(0) made 1 long (+0x40), its speed |em_vel(0)| (+0x50), its length
 * speed x p_lifetime.max(0), times destretch_percent_time when above 0 (+0x54, 0x1FB484), its radius |start_area(0)|
 * + max(p_size) x size_mul (+0x4C); the stream handle's cycle the emitter's longest range.cycle_length (0x2270A0) */
static int rod_of(const FxLib *L, const Effect *f, FxRod *r)
{
    if (f->type != FX_PARTICLE || f->nem < 1)
        return 0;
    const EmDef *d = &f->em[0];
    if ((d->eflags & (EF_AUTOSTART | EF_COLLISIONROD)) != (EF_AUTOSTART | EF_COLLISIONROD))
        return 0;
    float v[3], a[3];
    for (int k = 0; k < 3; k++) {
        v[k] = graph_eval(L, &d->g[G_VEL + k], 0);
        a[k] = graph_eval(L, &d->g[G_AREA + k], 0);
    }
    r->speed = len3(v);
    for (int k = 0; k < 3; k++)
        r->dir[k] = r->speed > 0 ? v[k] / r->speed : 0;
    r->length = r->speed * graph_eval(L, &d->g[G_LIFE + 1], 0);
    if (d->destretch_pct > 0)
        r->length *= d->destretch_pct;
    float s0 = graph_eval(L, &d->g[G_SIZE], 0), s1 = graph_eval(L, &d->g[G_SIZE + 1], 0);
    r->radius = len3(a) + (s0 > s1 ? s0 : s1) * graph_eval(L, &d->g[G_SIZEMUL], 0);
    r->cycle = d->cycle[0] > d->cycle[1] ? d->cycle[0] : d->cycle[1];
    r->loops = (d->eflags & EF_CYCLELOOPS) != 0;
    return 1;
}

int fx_rod(const FxLib *L, const char *name, FxRod *r)
{
    int i = fx_find(L, name);
    return i >= 0 && r ? rod_of(L, &L->fx[i], r) : 0;
}

/* a new emitter of an effect (0x1F7F90): its cycle length from range.cycle_length, a start age from
 * graph.em_start_time at a random point (minus a frame: the first advance brings it to it) */
static int emit_new(FxWorld *w, int fx, const Effect *owner, const EmDef *d, int parent)
{
    int i = 0;
    while (i < MAX_EMITS && w->em[i].used)
        i++;
    if (i == MAX_EMITS)
        return -1;
    Emit *e = &w->em[i];
    memset(e, 0, sizeof *e);
    e->used = 1;
    w->fx[fx].nemit++;
    e->d = d;
    e->owner = owner;
    e->fx = fx;
    e->parent = parent;
    e->head = -1;
    e->cycle = d->cycle[0] + rnd01(w) * (d->cycle[1] - d->cycle[0]);
    if (e->cycle < 1e-4f)
        e->cycle = 1e-4f;
    e->inv_cycle = 1 / e->cycle;
    e->age = e->cycle * graph_eval(w->lib, &d->g[G_START], rnd01(w)) - FX_STEP;
    if (d->eflags & EF_WORLDMATRIX) {
        memset(e->R, 0, sizeof e->R);
        e->R[0][0] = e->R[1][1] = e->R[2][2] = 1;
    } else {
        memcpy(e->R, w->fx[fx].R, sizeof e->R);
    }
    if (d->eflags & EF_RANDOMXF) {     /* three random angles 2 pi r1, r2, r3 (0x1F8164 -> 0x101330) [R] */
        float a = 2 * FX_PI * rnd01(w), b = 2 * FX_PI * rnd01(w), c = 2 * FX_PI * rnd01(w);
        float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
        float X[3][3] = { { ca * cc - sa * sb * sc, sa * cc + ca * sb * sc, -cb * sc }, { -sa * cb, ca * cb, sb },
                          { ca * sc + sa * sb * cc, sa * sc - ca * sb * cc, cb * cc } }, R[3][3];
        for (int r = 0; r < 3; r++)
            vec_rows(e->R, X[r], R[r]);
        memcpy(e->R, R, sizeof R);
    }
    if (parent >= 0)
        memcpy(e->origin, w->p[parent].pos, sizeof e->origin);
    for (int k = 0; k < 2; k++)                 /* its sounds, at the effect's place [R 0x1F84C0..0x1F8558] */
        if (d->sound[k][0] && w->sound)
            e->sound[k] = w->sound(w->sound_ctx, FXS_START, d->sound[k], w->fx[fx].origin, 0);
    return i;
}

/* the emitter's sounds are killed when it shuts down or ends [R 0x1F7CD0, 0x1F7E40: 0x26D530 on both owners] */
static void emit_sounds_stop(FxWorld *w, Emit *e)
{
    for (int k = 0; k < 2; k++) {
        if (e->sound[k] && w->sound)
            w->sound(w->sound_ctx, FXS_STOP, NULL, w->fx[e->fx].origin, e->sound[k]);
        e->sound[k] = 0;
    }
}

/* where in the start area a particle starts (0x212A20): a box (half extents a), an ellipsoid (+EllipsoidalStartArea,
 * 16 tries at most), their surface (+SurfaceStart; on a box a face by its area) */
static void start_area(FxWorld *w, const EmDef *d, const float a[3], float out[3])
{
    int zero[3], nz = 0;
    for (int i = 0; i < 3; i++) {
        zero[i] = fabsf(a[i]) <= 0.001f;
        nz += zero[i];
    }
    if (nz == 3) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    if (d->eflags & EF_ELLIPSOID) {
        float u[3], l = 0;
        int tries = 16;
        for (;;) {
            for (int i = 0; i < 3; i++)
                u[i] = zero[i] ? 0 : 2 * rnd01(w) - 1;
            l = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
            if (l <= 1 && (!(d->eflags & EF_SURFACE) || l >= 1e-6f))
                break;
            if (--tries == 0) {
                u[0] = u[1] = 0;
                u[2] = 0.2f;
                l = 0.04f;
                break;
            }
        }
        if (d->eflags & EF_SURFACE) {
            if (l < 1e-6f) {
                u[0] = 1;
                u[1] = u[2] = 0;
            } else {
                float k = 1 / sqrtf(l);
                u[0] *= k;
                u[1] *= k;
                u[2] *= k;
            }
        }
        for (int i = 0; i < 3; i++)
            out[i] = u[i] * a[i];
    } else if (d->eflags & EF_SURFACE) {
        memcpy(out, a, 3 * sizeof *out);
        if (nz == 2) {                                     /* a line: one of its two ends */
            if (rnd(w) & 8)
                for (int i = 0; i < 3; i++)
                    out[i] = -out[i];
            return;
        }
        float A0 = a[1] * a[2], A1 = a[0] * a[2], A2 = a[0] * a[1];
        float r = (A0 + A1 + A2) * rnd01(w);
        int f = r <= A0 ? 0 : r <= A0 + A1 ? 1 : 2;
        if (!(rnd(w) & 0x80))
            out[f] = -out[f];
        out[(f + 1) % 3] *= 2 * rnd01(w) - 1;
        out[(f + 2) % 3] *= 2 * rnd01(w) - 1;
    } else {
        for (int i = 0; i < 3; i++)
            out[i] = a[i] * (2 * rnd01(w) - 1);
    }
}

/* an emitter's shutdown (0x1F7CD0): it emits no more, its sounds end, and with EndOfLifeUponShutdown its particles
 * age out */
static void emit_shutdown(FxWorld *w, Emit *e)
{
    e->shut = 1;
    emit_sounds_stop(w, e);
    if (e->d->eflags & EF_ENDOFLIFE)
        for (int p = e->head; p >= 0; p = w->p[p].next)
            w->p[p].age = w->p[p].life + 1;
}

static void part_free(FxWorld *w, int i)
{
    Part *p = &w->p[i];
    if (p->child >= 0 && w->em[p->child].used) {   /* [R 0x1F21F0] its emitter (EmitEmitters): shut down (0x1F7CD0) */
        Emit *ce = &w->em[p->child];
        ce->parent = -1;
        emit_shutdown(w, ce);
    }
    p->used = 0;
    p->next = w->free_part;
    w->free_part = i;
    w->nparts--;
}

/* the emitter's frame values, read once per frame before its particles are made [R 0x20F454..0x20F5A4] */
typedef struct { float vfs, lmin, lrange, smin, srange, vel[3], velr[3], ofs[3], area[3], emvc[4]; } EmitFrame;

/* M = the rotation by angle about the unit axis a (M x v turns v; rows of M in M[row]) */
static void axis_angle(const float a[3], float angle, float M[3][3])
{
    float c = cosf(angle), s = sinf(angle), t = 1 - c;
    M[0][0] = c + t * a[0] * a[0];        M[0][1] = t * a[0] * a[1] - s * a[2]; M[0][2] = t * a[0] * a[2] + s * a[1];
    M[1][0] = t * a[0] * a[1] + s * a[2]; M[1][1] = c + t * a[1] * a[1];        M[1][2] = t * a[1] * a[2] - s * a[0];
    M[2][0] = t * a[0] * a[2] - s * a[1]; M[2][1] = t * a[1] * a[2] + s * a[0]; M[2][2] = c + t * a[2] * a[2];
}

/* a random axis among the XF_RotatesAround* ones: each enabled component 2r - 1 (the third random number on x, the
 * first on z), the mask itself when that comes out near 0, normalised; (1, 0, 0) when nothing is left [R 0x211B48..
 * 0x211D24, 0x211F38..0x21210C] */
static void xform_axis(FxWorld *w, const float mask[3], int naxes, float out[3])
{
    if (naxes == 1) {
        memcpy(out, mask, 3 * sizeof *out);
        return;
    }
    float r1 = rnd01(w), r2 = rnd01(w), r3 = rnd01(w);
    out[0] = mask[0] * (2 * r3 - 1);
    out[1] = mask[1] * (2 * r2 - 1);
    out[2] = mask[2] * (2 * r1 - 1);
    if (out[0] * out[0] + out[1] * out[1] + out[2] * out[2] < 1e-6f)
        memcpy(out, mask, 3 * sizeof *out);
    float l2 = out[0] * out[0] + out[1] * out[1] + out[2] * out[2];
    if (l2 < 1e-6f) {
        out[0] = 1;
        out[1] = out[2] = 0;
    } else {
        float k = 1 / sqrtf(l2);
        for (int i = 0; i < 3; i++)
            out[i] *= k;
    }
}

/* an XForm particle's orientation and tumble (0x2119D0): with +RandomStartRotation and some XF_RotatesAround* axis, a
 * start turn of 2 pi r about a random axis of those, else none, in the emitter's frame and so in the world's; a tumble
 * about another such axis at start_rot_distribution(r) radians a second (at the emitter's cycle point with
 * +XF_AgeNotDistribution), none without an axis or when that graph is the constant 0 */
static void xform_start(FxWorld *w, const Emit *e, Part *p)
{
    const EmDef *d = e->d;
    float mask[3] = { (d->eflags & EF_XF_ROTX) ? 1.0f : 0.0f, (d->eflags & EF_XF_ROTY) ? 1.0f : 0.0f,
                      (d->eflags & EF_XF_ROTZ) ? 1.0f : 0.0f };
    int naxes = (int)(mask[0] + mask[1] + mask[2]);
    float R0[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    if ((d->pflags & PF_RANDOMROT) && naxes) {
        float a[3];
        xform_axis(w, mask, naxes, a);
        axis_angle(a, 2 * FX_PI * rnd01(w), R0);
    }
    for (int r = 0; r < 3; r++)                            /* into the world: the emitter's axes x R0 [R 0x211DC0] */
        for (int c = 0; c < 3; c++)
            p->xr0[r][c] = e->R[0][r] * R0[0][c] + e->R[1][r] * R0[1][c] + e->R[2][r] * R0[2][c];
    p->xaxis[0] = p->xaxis[1] = 0;
    p->xaxis[2] = 1;                                       /* the default axis at 0x40A028 [R 0x211EE8] */
    p->xrate = 0;
    const Graph *g = &d->g[G_SRD];
    if (!naxes || (g->kind == GR_CONST && g->c[0] == 0))
        return;
    xform_axis(w, mask, naxes, p->xaxis);
    if (d->eflags & EF_RELATIVE) {                       /* kept raw (+0x5C); a relative one's turns with its */
        float t[3];                                      /* emitter at the draw, here taken into the world now */
        vec_rows(e->R, p->xaxis, t);
        memcpy(p->xaxis, t, sizeof t);
    }
    p->xrate = graph_eval(w->lib, g, (d->eflags & EF_XF_AGE) ? e->age * e->inv_cycle : rnd01(w));
}

static void emit_one(FxWorld *w, int ei, const EmitFrame *f, float dt)
{
    Emit *e = &w->em[ei];
    const EmDef *d = e->d;
    if (w->free_part < 0)
        return;                                            /* the pool is empty: nothing made [R 0x212460] */
    int pi = w->free_part;
    Part *p = &w->p[pi];
    w->free_part = p->next;
    w->nparts++;
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->em = ei;
    p->child = -1;

    float p0[3], local[3], v[3], world[3];
    start_area(w, d, f->area, p0);                          /* 0x212A20 */
    for (int i = 0; i < 3; i++)
        local[i] = p0[i] + f->ofs[i];                       /* + graph.em_pt_offset */
    vec_rows(e->R, local, world);
    for (int i = 0; i < 3; i++)
        p->pos[i] = e->origin[i] + world[i];
    float l2 = p0[0] * p0[0] + p0[1] * p0[1] + p0[2] * p0[2];
    if (l2 >= 1e-6f) {                                      /* away from the area's centre [R 0x212420, 0x2123D0] */
        float k = f->vfs / sqrtf(l2);
        for (int i = 0; i < 3; i++)
            p0[i] *= k;
    } else {
        p0[0] = p0[1] = p0[2] = 0;
    }
    for (int i = 0; i < 3; i++)
        v[i] = p0[i] + f->vel[i] + f->velr[i] * (2 * rnd01(w) - 1);   /* em_vel + em_vel_r x (-1..1) [R 0x20F6C0] */
    vec_rows(e->R, v, p->vel);
    if (e->parent >= 0 && !(d->eflags & (EF_STATIONARY | EF_RELATIVE)))
        for (int i = 0; i < 3; i++)
            p->vel[i] += w->p[e->parent].vel[i];            /* a carried emitter passes its carrier's speed on [R 0x212310] */
    if (d->ptype == PT_STRETCH) {
        float l = len3(p->vel);
        if (l * l < 1e-6f) {
            p->sdir[0] = 1;
        } else {
            for (int i = 0; i < 3; i++)
                p->sdir[i] = p->vel[i] / l;
        }
    } else if (!(d->eflags & EF_PREDICTABLE)) {             /* made somewhere inside the frame [R 0x20F7E8] */
        float back = -dt * rnd01(w);
        p->age = back;
        for (int i = 0; i < 3; i++)
            p->pos[i] += p->vel[i] * back;
    }
    float r = rnd01(w);
    p->life = f->lmin + r * f->lrange;
    p->inv_life = 1 / p->life;
    memcpy(p->emvc, f->emvc, sizeof p->emvc);
    if (!(d->eflags & EF_LIFESIZELINKED))
        r = rnd01(w);
    p->size = f->smin + r * f->srange;
    p->tex = (uint8_t)(d->ntex ? rnd(w) % (uint32_t)d->ntex : 0);   /* 0x2193A0 */
    if ((d->pmode & 4) && (d->pflags & PF_RANDOMTILE)) {    /* 0x219410: a start tile, a multiple of the step */
        int n = d->ntiles[0] * d->ntiles[1] / d->tile_step;
        p->tile0 = (uint16_t)((n > 0 ? rnd(w) % (uint32_t)n : 0) * (uint32_t)d->tile_step);
    }
    /* 0x219530 (the update 0x216470): a start angle and a spin; 0x212240 (0x20EFD0, 0x213F30) sets neither. Which
     * update an emitter gets is 0x21AD40's template match against the table at 0x4CAE10, which start-up code fills
     * through no address the ELF names [G: an XForm emitter as 0x212240; with 0x219530 the RandomStartRotation ones
     * would draw two more random numbers, the angle and the spin, which a mesh does not use] */
    if ((d->pmode & 2) && (d->pflags & PF_RANDOMROT) && d->ptype != PT_XFORM) {
        p->ang0 = (float)((rnd(w) >> 8) & 0xFF) * (2 * FX_PI / 256);
        p->spin = graph_eval(w->lib, &d->g[G_SRD], rnd01(w));
    }
    if (d->ptype == PT_XFORM) {
        p->shape = (uint8_t)(d->nshapes ? rnd(w) % (uint32_t)d->nshapes : 0);   /* [R 0x212250] */
        xform_start(w, e, p);
    }
    if ((d->eflags & EF_EMITEMITTERS) && d->spawn < e->owner->nem)   /* the particle carries an emitter of the
                                                                           * same effect (int.emitter_to_spawn) */
        p->child = emit_new(w, e->fx, e->owner, &e->owner->em[d->spawn], pi);
    /* into the list sorted by texture, before the others of its texture [R 0x211970] */
    int *link = &e->head;
    while (*link >= 0 && w->p[*link].tex < p->tex)
        link = &w->p[*link].next;
    p->next = *link;
    *link = pi;
}

/* +RelativeDistanceLimit (0x211670, 0x2117D4, at the end of the move): the particle goes when it is limit (the effect's
 * rod head, fx_cut) or more from its emitter [R]: a relative streak by its tail's distance, its length cut so that its
 * head stops at the limit; another streak by how far its head has run (age x speed, compared squared); the rest by
 * their distance (the game takes |pos| in the emitter's frame; no effect has a non-relative one). Returns 1: gone. */
static int distance_limit(const EmDef *d, const Emit *e, Part *p, float limit)
{
    float r[3] = { p->pos[0] - e->origin[0], p->pos[1] - e->origin[1], p->pos[2] - e->origin[2] }, dist = len3(r);
    if (d->ptype != PT_STRETCH)
        return !(dist < limit);
    if (!(d->eflags & EF_RELATIVE)) {
        float v2 = p->vel[0] * p->vel[0] + p->vel[1] * p->vel[1] + p->vel[2] * p->vel[2];
        return !(p->age * p->age * v2 < limit * limit);
    }
    if (!(dist < limit))
        return 1;
    if (dist + p->stretch > limit)
        p->stretch = limit - dist;
    return 0;
}

/* a particle's frame after the emitter's (0x20F9B8..0x20FD30): age, move (0x211110), the ground (0x2106B0), gravity
 * (0x2105E0), the air (0x210450). Returns 1 when it is gone. */
static int part_step(FxWorld *w, Emit *e, Part *p, float dt, float ground, float limit)
{
    const EmDef *d = e->d;
    p->age += dt;
    if (p->age > p->life + 1e-4f)
        return 1;
    float t = p->age * p->inv_life, delta[3];
    for (int i = 0; i < 3; i++) {
        delta[i] = dt * p->vel[i];
        if (d->g[G_PVM + i].kind != GR_NONE)
            delta[i] *= graph_eval(w->lib, &d->g[G_PVM + i], t);    /* graph.p_vel_mult over the life */
    }
    if (d->ptype == PT_STRETCH) {                          /* the head runs ahead, then the tail follows [R 0x211414] */
        float speed = len3(p->vel);
        if (t < d->stretch_pct)
            p->stretch += dt * speed;
        else
            for (int i = 0; i < 3; i++)
                p->pos[i] += delta[i];
        if ((d->pflags & PF_REALIGN) && speed * speed > 1e-6f)
            for (int i = 0; i < 3; i++)
                p->sdir[i] = p->vel[i] / speed;
        if (t > d->destretch_pct) {
            p->stretch -= dt * speed;
            if (p->stretch < 0)
                p->stretch = 0;
        }
    } else {
        for (int i = 0; i < 3; i++)
            p->pos[i] += delta[i];
    }
    if ((d->eflags & EF_RELDISTLIMIT) && distance_limit(d, e, p, limit))
        return 1;
    if (d->pflags & PF_GROUND) {                           /* the ground plane, its normal +Z */
        const float *c = COL_TABLE[d->gnd];
        float h = p->pos[2] - ground;
        float r = 0.5f * p->size * graph_eval(w->lib, &d->g[G_SIZEMUL], t);
        int hit = c[0] >= 0 ? h - c[0] * r < 0 : h + r < 0;   /* h - c0 r < 0 (0x210918), c0 -1: all of it under [R] */
        if (hit && p->vel[2] <= 0.01f) {
            p->vel[2] *= -c[1];                             /* what goes into the ground comes back x c1 */
            p->vel[0] *= c[2];
            p->vel[1] *= c[2];
            if (c[0] >= 0)
                p->pos[2] += c[0] * r - h;
            p->age = c[3] + c[4] * p->age + c[5] * p->life;
            if (p->age > p->life)
                return 1;
        }
    }
    p->vel[2] += -196.2f * d->grav;                        /* per frame [R 0x2105E0: 0xC3443334 x grav_multiplier] */
    float k;                                               /* 0x2118E0 / 0x210450 */
    if (d->pflags & PF_LINEARAIR)
        k = 1 - dt * d->air;
    else
        k = 1 - dt * d->air * len3(p->vel);
    if (k < 0)
        k = 0;
    for (int i = 0; i < 3; i++)
        p->vel[i] *= k;
    return 0;
}

/* an emitter's frame (0x20EFD0): its age and cycle, how many particles to make, make them, move the old ones.
 * Returns 1 when it is finished. */
static int emit_step(FxWorld *w, int ei, float dt, float ground)
{
    Emit *e = &w->em[ei];
    const EmDef *d = e->d;
    e->age += dt;
    if (e->age > e->cycle && !e->shut && (d->eflags & EF_CYCLELOOPS))
        e->age -= e->cycle;
    if (e->age > e->cycle && (d->pflags & PF_DIEWITHEMITTER))
        while (e->head >= 0) {                              /* [R 0x20F13C] */
            int n = w->p[e->head].next;
            part_free(w, e->head);
            e->head = n;
        }
    if (e->parent >= 0)
        memcpy(e->origin, w->p[e->parent].pos, sizeof e->origin);
    float t = e->age * e->inv_cycle;                        /* every emitter graph runs over the cycle */
    if (d->eflags & EF_ONETIME) {                           /* all of gen_rate(0) on the first frame [R 0x20F2D8] */
        if (e->age <= dt) {
            e->acc = graph_eval(w->lib, &d->g[G_GEN], 0);
            e->age += 0.001f;
        } else {
            e->acc = 0;
        }
    } else {
        float rate = e->age <= e->cycle && !e->shut ? graph_eval(w->lib, &d->g[G_GEN], t) : 0;
        if (e->acc < 0)
            e->acc = 0;
        if (d->eflags & EF_PREDICTABLE)
            e->acc += rate * dt;
        else
            e->acc = rate * dt + rnd01(w);                  /* a random rounding of rate x dt [R 0x20F408] */
    }
    if (!e->shut && e->acc >= 1) {
        EmitFrame f;
        f.vfs = em_graph(w, e, G_VFS, t);
        f.lmin = em_graph(w, e, G_LIFE, t);
        float lmax = em_graph(w, e, G_LIFE + 1, t);
        if (f.lmin < 0.04f) f.lmin = 0.04f;
        if (lmax < 0.04f) lmax = 0.04f;
        f.lrange = lmax - f.lmin;
        f.smin = em_graph(w, e, G_SIZE, t);
        f.srange = em_graph(w, e, G_SIZE + 1, t) - f.smin;
        for (int i = 0; i < 3; i++) {
            f.vel[i] = em_graph(w, e, G_VEL + i, t);
            f.velr[i] = em_graph(w, e, G_VELR + i, t);
            f.ofs[i] = em_graph(w, e, G_OFS + i, t);
            f.area[i] = em_graph(w, e, G_AREA + i, t);
        }
        for (int i = 0; i < 4; i++)                         /* 0x213420 */
            f.emvc[i] = em_graph(w, e, G_EMVC + i, t);
        while (e->acc >= 1) {
            e->acc -= 1;
            emit_one(w, ei, &f, dt);
            e = &w->em[ei];
        }
    }
    int *link = &e->head;
    float limit = w->fx[e->fx].head;
    while (*link >= 0) {
        int pi = *link;
        if (part_step(w, e, &w->p[pi], dt, ground, limit)) {
            *link = w->p[pi].next;
            part_free(w, pi);
        } else {
            link = &w->p[pi].next;
        }
    }
    return e->head < 0 && (e->shut || (!(d->eflags & EF_CYCLELOOPS) && e->age > e->cycle));
}

static void spawn_effect(FxWorld *w, int slot, int effect, int depth)
{
    const Effect *ef = &w->lib->fx[effect];
    if (ef->type == FX_SHAKE) {
        if (w->shake)
            w->shake(w->shake_ctx, ef->name, w->fx[slot].origin);
    } else if (ef->type == FX_COMPOSITE && depth < 4) {   /* CompositeFx: every part where it is */
        for (int k = 0; k < ef->nchild; k++)
            if (ef->child[k] >= 0)
                spawn_effect(w, slot, ef->child[k], depth + 1);
    } else if (ef->type == FX_PARTICLE) {
        Fx *f = &w->fx[slot];
        FxRod r;
        for (int i = 0; i < ef->nem; i++) {
            if (!(ef->em[i].eflags & EF_AUTOSTART))         /* -AutoStart ones wait for EmitEmitters */
                continue;
            int ei = emit_new(w, slot, ef, &ef->em[i], -1);
            if (i == 0 && ei >= 0 && f->rod < 0 && rod_of(w->lib, ef, &r)) {   /* [R 0x1F2734..0x1F2758] */
                f->rod = ei;
                f->rod_speed = r.speed;
                f->rod_len = r.length;
                f->head = f->tail = 0;
            }
        }
    }
}

int fx_spawn(FxWorld *w, const char *effect_name, const float pos[3], const float dir[3], float scale)
{
    if (!w)
        return -1;
    int effect = fx_find(w->lib, effect_name);
    if (effect < 0)
        return -1;
    int slot = 0;
    while (slot < MAX_FX && w->fx[slot].used)
        slot++;
    if (slot == MAX_FX)
        return -1;
    Fx *f = &w->fx[slot];
    f->used = 1;
    f->nemit = 0;
    f->gen = (f->gen + 1) & 0x7FFFFF;
    f->e = &w->lib->fx[effect];
    memcpy(f->origin, pos, sizeof f->origin);
    f->scale = scale > 0 ? scale : 1;
    f->rod = -1;
    f->head = f->tail = 0;
    /* the effect's frame: Y along dir, Z the world's up made square to it. The game's callers hand the effect a
     * whole matrix of their own; a building cell's is its face's from 0x430F10 (0x30F710; the front's the identity:
     * dir +Y here, the effects' -Y out of the facade) [R]; the others are the callers' (fx_spawn_frame for a frame) */
    float Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 }, X[3];
    if (dir && len3(dir) > 1e-6f) {
        float l = len3(dir);
        for (int i = 0; i < 3; i++)
            Y[i] = dir[i] / l;
    }
    float dz = Y[2];
    for (int i = 0; i < 3; i++)
        Z[i] -= dz * Y[i];
    if (len3(Z) < 1e-3f) {                                  /* straight up or down: X stays the world's X */
        Z[0] = 0; Z[1] = -Y[2]; Z[2] = Y[1];
    }
    float lz = len3(Z);
    for (int i = 0; i < 3; i++)
        Z[i] /= lz;
    X[0] = Y[1] * Z[2] - Y[2] * Z[1];
    X[1] = Y[2] * Z[0] - Y[0] * Z[2];
    X[2] = Y[0] * Z[1] - Y[1] * Z[0];
    memcpy(f->R[0], X, sizeof X);
    memcpy(f->R[1], Y, sizeof Y);
    memcpy(f->R[2], Z, sizeof Z);
    spawn_effect(w, slot, effect, 0);
    if (!f->nemit) {
        f->used = 0;
        return -1;
    }
    return slot | f->gen << 8;
}

int fx_spawn_frame(FxWorld *w, const char *effect_name, const float pos[3], const float R[3][3], float scale)
{
    int id = fx_spawn(w, effect_name, pos, NULL, scale);
    if (id >= 0 && R)
        fx_place(w, id, pos, R);
    return id;
}

static Fx *fx_of(const FxWorld *w, int id)
{
    if (!w || id < 0 || (id & 0xFF) >= MAX_FX)
        return NULL;
    const Fx *f = &w->fx[id & 0xFF];
    return f->used && f->gen == id >> 8 ? (Fx *)f : NULL;
}

int fx_alive(const FxWorld *w, int id) { return fx_of(w, id) != NULL; }

void fx_move(FxWorld *w, int id, const float pos[3])
{
    Fx *f = fx_of(w, id);
    if (!f)
        return;
    int slot = (int)(f - w->fx);
    float back[3];                                    /* the world's particles stay where they are */
    for (int i = 0; i < 3; i++)
        back[i] = (pos[i] - f->origin[i]) / f->scale;
    for (int i = 0; i < MAX_EMITS; i++) {
        const Emit *e = &w->em[i];
        if (!e->used || e->fx != slot || (e->d->eflags & EF_RELATIVE))
            continue;
        for (int p = e->head; p >= 0; p = w->p[p].next)
            for (int k = 0; k < 3; k++)
                w->p[p].pos[k] -= back[k];
    }
    memcpy(f->origin, pos, sizeof f->origin);
    for (int i = 0; i < MAX_EMITS && w->sound; i++)       /* its sounds follow it (add3DSfx keeps the place's address) */
        for (int k = 0; k < 2; k++)
            if (w->em[i].used && w->em[i].fx == slot && w->em[i].sound[k])
                w->sound(w->sound_ctx, FXS_MOVE, NULL, pos, w->em[i].sound[k]);
}

void fx_place(FxWorld *w, int id, const float pos[3], const float R[3][3])
{
    Fx *f = fx_of(w, id);
    if (!f)
        return;
    fx_move(w, id, pos);
    if (!R)
        return;
    memcpy(f->R, R, sizeof f->R);
    int slot = (int)(f - w->fx);
    for (int i = 0; i < MAX_EMITS; i++) {              /* its emitters' axes, but for the world-axis and random ones */
        Emit *e = &w->em[i];
        if (!e->used || e->fx != slot || (e->d->eflags & (EF_WORLDMATRIX | EF_RANDOMXF)))
            continue;
        if (e->d->eflags & EF_RELATIVE) {             /* its particles are in its frame: they turn with it */
            float T[3][3];                            /* T v = the new axes x (the old axes' components of v) */
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++)
                    T[r][c] = R[0][r] * e->R[0][c] + R[1][r] * e->R[1][c] + R[2][r] * e->R[2][c];
            for (int p = e->head; p >= 0; p = w->p[p].next) {
                Part *q = &w->p[p];
                float *v[4] = { q->pos, q->vel, q->sdir, q->xaxis }, t[3];
                for (int k = 0; k < 4; k++) {
                    float o[3] = { v[k][0] - (k ? 0 : e->origin[0]), v[k][1] - (k ? 0 : e->origin[1]),
                                   v[k][2] - (k ? 0 : e->origin[2]) };
                    for (int j = 0; j < 3; j++)
                        t[j] = T[j][0] * o[0] + T[j][1] * o[1] + T[j][2] * o[2] + (k ? 0 : e->origin[j]);
                    memcpy(v[k], t, sizeof t);
                }
                float X[3][3];
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < 3; c++)
                        X[r][c] = T[r][0] * q->xr0[0][c] + T[r][1] * q->xr0[1][c] + T[r][2] * q->xr0[2][c];
                memcpy(q->xr0, X, sizeof X);
            }
        }
        memcpy(e->R, R, sizeof e->R);
    }
}

void fx_kill(FxWorld *w, int id)
{
    Fx *f = fx_of(w, id);
    if (!f)
        return;
    int slot = (int)(f - w->fx);
    for (int i = 0; i < MAX_EMITS; i++) {
        Emit *e = &w->em[i];
        if (!e->used || e->fx != slot)
            continue;
        emit_sounds_stop(w, e);
        for (int p = e->head, next; p >= 0; p = next) {
            next = w->p[p].next;
            part_free(w, p);
        }
        e->head = -1;
        e->used = 0;
    }
    f->nemit = 0;
    f->used = 0;
}

/* 0x1F7CD0: an emitter shuts down: it stops making particles; +EndOfLifeUponShutdown ends the live ones */
void fx_stop(FxWorld *w, int id)
{
    Fx *f = fx_of(w, id);
    if (!f)
        return;
    int slot = (int)(f - w->fx);
    for (int i = 0; i < MAX_EMITS; i++) {
        Emit *e = &w->em[i];
        if (e->used && e->fx == slot)
            emit_shutdown(w, e);
    }
}

void fx_update(FxWorld *w, float dt, float ground_z)
{
    if (!w || dt <= 0)
        return;
    w->accum += dt;
    if (w->accum > 0.25f)
        w->accum = 0.25f;
    while (w->accum >= FX_STEP) {
        w->accum -= FX_STEP;
        for (int i = 0; i < MAX_EMITS; i++) {
            if (!w->em[i].used)
                continue;
            Fx *f = &w->fx[w->em[i].fx];
            float ground = (ground_z - f->origin[2]) / f->scale;
            if (emit_step(w, i, FX_STEP, ground)) {
                if (f->rod == i) {
                    if (f->tail < f->head)               /* the rod's emitter stays while its tail is short of the
                                                          * head [R 0x1F2CE8] */
                        continue;
                    f->rod = -1;                         /* [R 0x1F2E40] */
                    f->head = f->tail = 0;
                }
                emit_sounds_stop(w, &w->em[i]);
                w->em[i].used = 0;
                if (--f->nemit <= 0)
                    f->used = 0;                         /* the effect is over when its last emitter is */
            }
        }
        for (int k = 0; k < MAX_FX; k++) {               /* the rods, after the emitters [R 0x1F2E60..0x1F2F08] */
            Fx *f = &w->fx[k];
            if (!f->used || f->rod < 0)
                continue;
            const Emit *e = &w->em[f->rod];
            f->head += f->rod_speed * FX_STEP;
            if (f->head > f->rod_len)
                f->head = f->rod_len;
            f->tail = e->shut || e->age > e->cycle ? f->tail + f->rod_speed * FX_STEP : 0;
        }
    }
}

/* the rod's vtable +0x80 (0x1F2520): a hit t along it (world units) is its head now; one short of the tail drops the
 * rod (its head then stays) */
void fx_cut(FxWorld *w, int id, float t)
{
    Fx *f = fx_of(w, id);
    if (!f)
        return;
    f->head = t / f->scale;
    if (f->head < f->tail)
        f->rod = -1;
}

/* ---------------------------------------------------------------- drawing (0x2214D0, 0x21C7E0) */
static float blend(int mode, float v, float e)              /* the emitter colour on the particle's [R 0x21C638..] */
{
    switch (mode) {
    case BL_LIGHTEN: v += e; break;
    case BL_DARKEN: v = e - v; break;
    case BL_DIFFERENCE: v -= e; break;
    case BL_MULTIPLY: v *= e; break;
    default: break;
    }
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static int quads_of(const FxWorld *w, int slot, FxQuad *out, int max)
{
    if (!w)
        return 0;
    const FxLib *L = w->lib;
    int n = 0;
    for (int ei = 0; ei < MAX_EMITS; ei++) {
        const Emit *e = &w->em[ei];
        if (!e->used || (slot >= 0 && e->fx != slot))
            continue;
        const Fx *f = &w->fx[e->fx];
        const EmDef *d = e->d;
        if ((d->pflags & PF_NORENDER) || d->ptype == PT_XFORM || d->repeat <= 0)
            continue;                                   /* XForm particles are meshes (shapes.name): not drawn */
        for (int pi = e->head; pi >= 0; pi = w->p[pi].next) {
            const Part *p = &w->p[pi];
            float t = p->age * p->inv_life;
            if (t < 0)
                t = 0;
            float tp = (d->pnorm & 4) ? t : p->age;
            FxQuad q;
            memset(&q, 0, sizeof q);
            /* [R 0x22233C] a billboard's axes are size x size_mul along the camera's right and up: the effect's matrix
             * (its scale) does not reach them; a relative flat quad's does, and a streak's length but not its width
             * (0x1F769C: size x size_mul, laid in camera space after the transform) */
            float size = p->size * graph_eval(L, &d->g[G_SIZEMUL], tp), size_s = size * f->scale;
            float v[4];                                 /* the colour [R 0x21C5BC.., 0x222680..] */
            for (int i = 0; i < 4; i++)
                v[i] = graph_eval(L, &d->g[G_VC + i], (d->pnorm & (i < 3 ? 1 : 2)) ? t : p->age);
            for (int i = 0; i < 3; i++)
                q.col[i] = d->cblend == BL_NONE && d->rgb_const ? p->emvc[i] : blend(d->cblend, v[i], p->emvc[i]);
            q.col[3] = d->ablend == BL_NONE && d->a_const ? p->emvc[3] : blend(d->ablend, v[3], p->emvc[3]);
            float rot = 0;
            if (d->pmode & 2)                           /* RotatesOnly, Tiles [R 0x2225E0] */
                rot = graph_eval(L, &d->g[G_ROT], (d->pnorm & 8) ? t : p->age) + p->spin * p->age + p->ang0;
            /* [R 0x221E50, 0x3B2480, 0x3B24D0] the quad's corners (+-0.5 along the camera's right, +-0.5 up) take
             * u = 0.5 + x, v = 0.5 - y of texture_center +- texture_radius / 2 (4.12 fixed point; 0.5 and 1, as no
             * file sets them): v 0 at the top */
            q.uv[0] = q.uv[1] = 0;
            q.uv[2] = q.uv[3] = 1;
            if (d->pmode & 4) {                         /* Tiles, TilesNoRotation [R 0x223488] */
                int adv = (int)floorf(graph_eval(L, &d->g[G_TILE], (d->pnorm & 0x10) ? t : p->age));
                unsigned idx = (d->pflags & PF_CYCLEINAREA) ? (unsigned)adv % (unsigned)d->tile_step + p->tile0 :
                               p->tile0 + (unsigned)adv;
                unsigned nu = (unsigned)d->ntiles[0], nv = (unsigned)d->ntiles[1];
                unsigned col = idx % nu, row = (idx / nu) % nv;
                q.uv[0] = (float)col / nu;
                q.uv[1] = (float)row / nv;
                q.uv[2] = (float)(col + 1) / nu;
                q.uv[3] = (float)(row + 1) / nv;
            }
            for (int i = 0; i < 3; i++)
                q.pos[i] = f->origin[i] + p->pos[i] * f->scale;
            if (d->ptype == PT_STRETCH) {               /* from the tail (pos) to the head [R 0x1F7500] */
                q.kind = FXQ_STRETCH;
                float half = 0.5f * p->stretch * f->scale;
                for (int i = 0; i < 3; i++) {
                    q.axis[0][i] = p->sdir[i] * half;
                    q.pos[i] += q.axis[0][i];
                }
                q.size[0] = 2 * half;
                q.size[1] = size;
            } else if (d->pflags & PF_NOBILLBOARD) {
                /* [R 0x222C08] DontBillboard: not relative, the world's x and y (the draw's model matrix), forward_axis
                 * unused; relative, rows 0 and 1 of the emitter's frame x B (forward_axis 2: identity; 1: rows (1, 0,
                 * 0), (0, 0, 1), (0, -1, 0); 0: (0, 1, 0), (-1, 0, 0), (0, 0, 1)), the effect's scale with them; then
                 * the spin (0x223344): rows (c, s, 0), (-s, c, 0), (0, 0, 1) times those axes (0x101A90), s the sine
                 * at +0x478 and c the cosine at +0x47C (0x105E30's series) [R] */
                float A[2][3] = { { 1, 0, 0 }, { 0, 1, 0 } }, sz = size;
                if (d->eflags & EF_RELATIVE) {
                    const float (*M)[3] = (const float (*)[3])e->R;
                    for (int r = 0; r < 2; r++) {
                        if (d->fwd_axis == 1) {
                            A[r][0] = M[r][0]; A[r][1] = -M[r][2]; A[r][2] = M[r][1];
                        } else if (d->fwd_axis == 0) {
                            A[r][0] = -M[r][1]; A[r][1] = M[r][0]; A[r][2] = M[r][2];
                        } else
                            memcpy(A[r], M[r], sizeof A[r]);
                    }
                    sz = size_s;
                }
                float c = cosf(rot) * sz * 0.5f, sn = sinf(rot) * sz * 0.5f;
                q.kind = FXQ_FLAT;
                for (int i = 0; i < 3; i++) {
                    q.axis[0][i] = c * A[0][i] + sn * A[1][i];
                    q.axis[1][i] = -sn * A[0][i] + c * A[1][i];
                }
                q.size[0] = q.size[1] = sz;
            } else {
                q.kind = FXQ_BILLBOARD;
                q.size[0] = q.size[1] = size;
                q.rot = rot;
            }
            q.b = d->ntex ? d->texb[p->tex] : NULL;
            q.tex = d->ntex ? d->tex[p->tex] : 0;
            q.additive = (d->pflags & PF_ADDITIVE) != 0;
            q.ztest = (d->pflags & PF_TESTZ) != 0;
            for (int k = 0; k < d->repeat && n < max; k++)   /* int.multiple_render: drawn that many times */
                out[n++] = q;
            if (n >= max)
                return n;
        }
    }
    return n;
}

int fx_quads(const FxWorld *w, FxQuad *out, int max) { return quads_of(w, -1, out, max); }

int fx_quads_of(const FxWorld *w, int id, FxQuad *out, int max)
{
    const Fx *f = fx_of(w, id);
    return f ? quads_of(w, (int)(f - w->fx), out, max) : 0;
}

/* ---------------------------------------------------------------- XForm particles (0x21B330, 0x21DC20) */
int fx_shape_count(const FxLib *L) { return L ? L->nshapes : 0; }
const char *fx_shape_name(const FxLib *L, int i) { return L && i >= 0 && i < L->nshapes ? L->shapes[i].name : ""; }
const Bundle *fx_shape_bundle(const FxLib *L, int i) { return L && i >= 0 && i < L->nshapes ? L->shapes[i].b : NULL; }

int fx_shape_meshes(const FxLib *L, int i, const BdpEntry **out, int max)
{
    if (!L || i < 0 || i >= L->nshapes)
        return 0;
    int n = 0;
    for (; n < L->shapes[i].nmesh && n < max; n++)
        out[n] = L->shapes[i].mesh[n];
    return n;
}

/* per particle [R 0x21B780..0x21BAA8]: s = size_mul(t) (t over the life with NormalizedSizeMultiplier); s <= 0 is not
 * drawn, else the renderer state at 0x4CB52C gets s, which only scales the shape's culling sphere (0x16DDA8; its
 * copy at 0x4CC040 never reaches VU1, the upload 0x263210 ending at 0x4CC03F), not the drawn matrix; the turn = the tumble about its world-fixed axis by rate x age, after the start
 * orientation (0x185A50 axis-angle, 0x105340 to a quaternion, 0x196F30 the product, 0x104BB0 to a matrix) [R];
 * the place the particle's; then the shape draws (its vtable +0x10, SimpleShape 0x184BB0). The particle size is only
 * the ground collision's radius. */
int fx_meshes(const FxWorld *w, FxMesh *out, int max)
{
    if (!w)
        return 0;
    const FxLib *L = w->lib;
    int n = 0;
    for (int ei = 0; ei < MAX_EMITS && n < max; ei++) {
        const Emit *e = &w->em[ei];
        if (!e->used || e->d->ptype != PT_XFORM || (e->d->pflags & PF_NORENDER) || !e->d->nshapes)
            continue;
        const EmDef *d = e->d;
        const Fx *f = &w->fx[e->fx];
        for (int pi = e->head; pi >= 0 && n < max; pi = w->p[pi].next) {
            const Part *p = &w->p[pi];
            float t = p->age * p->inv_life;
            float s = graph_eval(L, &d->g[G_SIZEMUL], (d->pnorm & 4) ? t : p->age);
            int shape = d->shape[p->shape < d->nshapes ? p->shape : 0];
            if (s <= 0 || !L->shapes[shape].nmesh)
                continue;
            float T[3][3], R[3][3];
            axis_angle(p->xaxis, p->xrate * (p->age > 0 ? p->age : 0), T);
            for (int r = 0; r < 3; r++)                 /* [R 0x21B8EC] mat(Q x quat(axis, -rate age)): T x xr0 */
                for (int c = 0; c < 3; c++)
                    R[r][c] = T[r][0] * p->xr0[0][c] + T[r][1] * p->xr0[1][c] + T[r][2] * p->xr0[2][c];
            FxMesh *m = &out[n++];
            s = f->scale;                               /* the effect's own scale only */
            for (int c = 0; c < 3; c++) {
                for (int r = 0; r < 3; r++)
                    m->m[4 * c + r] = R[r][c] * s;
                m->m[4 * c + 3] = 0;
            }
            for (int r = 0; r < 3; r++)
                m->m[12 + r] = f->origin[r] + p->pos[r] * f->scale;
            m->m[15] = 1;
            m->shape = shape;
        }
    }
    return n;
}
