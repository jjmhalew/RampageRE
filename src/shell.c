/* shell.c - the game's front end (shell.h; docs/SHELL_NOTES.md), transcribed from the PS2 game: the layouts (.ifc,
 * bundle type 24) of Shell.bdp (ControlRoom, AttractMode, MainMenu, MonsterSelect, MonsterSheet, CitySelect,
 * Options), VShell.bdp (Victory), MemoryCard.bdp (MemoryCard) and Loading_<City>.bdp / Loading.bdp (Loading,
 * Loading_Shell); the logic of their scripts (build/scripts/ControlRoom.scr, AttractMode.scr, MainMenu.scr,
 * MonsterSelect.scr, MonsterSheet.scr, CitySelect.scr, Options.scr, Loading.scr, Victory.scr, Shell.scr and the include
 * files ObjectFader.h, ButtonSetup.h, GrowShrink.h); the native code behind them: the gShell bindings (lookup 0x306BF0,
 * dispatcher 0x3062B0, jump table 0x432F50), the shell object 0x4D51F0 (activation 0x308950, update 0x308710), the
 * Common bindings (lookup 0x2C3840, table 0x430450) and GameStatus (0x4D7990, getMonsterFlags 0x33D760), the SaveGame
 * popup 0x4D7E80 (the profile list 0x340B40 / 0x3410E0 / 0x341250, the name entry 0x340980 / 0x340320, the messages
 * 0x3401E0 / 0x33FBB0, the event split 0x341880) and the loading screen 0x4D8250 (showLoading 0x3485E0, the dots
 * 0x348B80). [R] = read there, [G] = a guess where it matters.
 *
 * The GUI core (fonts, texts, the .ifc reader, rects, list boxes, the fader) is hud.c's, copied and widened here (hud.c
 * keeps it static): each object's frame is a full 2D affine map instead of a position and a uniform scale, so turned
 * and mirrored objects (<x>), GuiObject.yaw and setScaleX draw as the game has them; the shapes and effects of the
 * layouts are listed for the viewer to draw in 3D (shell_3d), their lights with them (the scripts find all of them by
 * name); and objects outside a 2D root (<DD>) are the GUI's 3D objects, whose positions are in screen widths (below). Unchanged from hud.c otherwise: if hud.c's core
 * moves into a module of its own one day, this copy can go. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "shell.h"

#define SH_PI 3.14159265f
#define MAX_TEXT 640
#define MAX_FONTS 8
#define MAX_BUNDLES 8
#define MAX_LINES 16
#define MAX_FADERS 12
#define MAX_EVENTS 64
#define MAX_QUADS 6144

/* ---------------------------------------------------------------- colours (hud.c) */
/* the files write RRGGBBAA [R hud.c]; the engine's own colours (fonts) are AABBGGRR */
static void col_rgba(uint32_t c, float out[4])
{
    out[0] = (float)(c >> 24 & 255) / 255.0f;
    out[1] = (float)(c >> 16 & 255) / 255.0f;
    out[2] = (float)(c >> 8 & 255) / 255.0f;
    out[3] = (float)(c & 255) / 255.0f;
}
static void col_abgr(uint32_t c, float out[4])
{
    out[0] = (float)(c & 255) / 255.0f;
    out[1] = (float)(c >> 8 & 255) / 255.0f;
    out[2] = (float)(c >> 16 & 255) / 255.0f;
    out[3] = (float)(c >> 24 & 255) / 255.0f;
}
static void col_set(float out[4], float r, float g, float b, float a) { out[0] = r; out[1] = g; out[2] = b; out[3] = a; }
/* Color.set255(r, g, b, a) */
static void col_255(float out[4], float r, float g, float b, float a)
{
    col_set(out, r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

/* ---------------------------------------------------------------- the resources: bundles, fonts, texts (hud.c) */
typedef struct { const Bundle *b; uint32_t name; float w, h, us, vs; } TexRef;   /* w h: the used size */
typedef struct { int ok; float u0, u1, v0, v1, a, w, c, h; } Glyph;
typedef struct {
    int ok;
    TexRef tex;
    float cell_h, avg_h;
    int spacing, linespacing, shadow_dx, shadow_dy, slant;
    uint32_t shadow_col, col;
    Glyph g[256];
} Font;
typedef struct {
    const Bundle *bundles[MAX_BUNDLES];
    int nb;
    Font fonts[MAX_FONTS];
    char font_names[MAX_FONTS][32];
    int nfonts;
    int lang;
    float time;                 /* seconds since shell_open: the pulses */
    int video_pic;              /* the Video quads show the movie's picture (shell_movie_picture) */
} Gui;

static int tex_find(const Gui *g, const char *name, TexRef *out)
{
    if (!name || !*name)
        return 0;
    for (int i = 0; i < g->nb; i++) {
        int n = bdp_find_name(g->bundles[i], name);
        const BdpEntry *e = n >= 0 ? bdp_entry_of(g->bundles[i], (uint32_t)n, ET_TEXTURE) : NULL;
        if (!e || e->size < 0x20)
            continue;
        /* texture header: 0x08 u16 width, 0x0A height, 0x14 used width, 0x16 used height [R tools/tex.py] */
        int w = rd16(e->data + 0x8), h = rd16(e->data + 0xA);
        out->b = g->bundles[i];
        out->name = (uint32_t)n;
        out->w = rd16(e->data + 0x14);
        out->h = rd16(e->data + 0x16);
        if (out->w <= 0 || out->w > w) out->w = (float)w;
        if (out->h <= 0 || out->h > h) out->h = (float)h;
        out->us = w ? out->w / (float)w : 1;
        out->vs = h ? out->h / (float)h : 1;
        return 1;
    }
    return 0;
}

/* OFontData::load 0x1B7CD0 [R; hud.c has the layout of the file] */
static int font_load(Font *f, const Gui *gui, const char *name)
{
    memset(f, 0, sizeof *f);
    char want[80];
    snprintf(want, sizeof want, "%s.fdf", name);
    const BdpEntry *e = NULL;
    for (int i = 0; i < gui->nb && !e; i++) {
        int n = bdp_find_name(gui->bundles[i], want);
        if (n >= 0)
            e = bdp_entry_of(gui->bundles[i], (uint32_t)n, ET_RAW);
    }
    if (!e || e->size < 0x358)
        return 0;
    const uint8_t *d = e->data;
    char bmp[0x101];
    memcpy(bmp, d + 0x100, 0x100);
    bmp[0x100] = 0;
    char *slash = strrchr(bmp, '\\'), *dot;
    char *base = slash ? slash + 1 : bmp;
    if ((dot = strchr(base, '.')) != NULL)
        *dot = 0;
    if (!tex_find(gui, base, &f->tex))
        return 0;
    uint32_t rows = rd32(d + 0x304), nchars = rd32(d + 0x308), nalias = rd32(d + 0x30C);
    f->cell_h = f->tex.h / (float)(rows ? rows : 1);
    f->spacing = (int)rdf(d + 0x310);
    f->linespacing = (int)rdf(d + 0x314);
    f->shadow_dx = (int)rdf(d + 0x318);
    f->shadow_dy = (int)rdf(d + 0x31C);
    f->shadow_col = rd32(d + 0x320);
    f->col = rd32(d + 0x324);
    f->slant = (int)rdf(d + 0x32C);
    uint32_t at = 0x358 + nchars + nalias;
    if (at > e->size)
        return 0;
    float lower_n = 0, lower_h = 0, upper_n = 0, upper_h = 0;
    for (uint32_t r = at; r + 0x24 <= e->size; r += 0x24) {
        unsigned ch = d[r + 0x20];
        if ((ch >= 0x81 && ch <= 0x9F) || (ch >= 0xE0 && ch <= 0xEF))
            continue;
        Glyph *g = &f->g[ch];
        g->ok = 1;
        g->u0 = rdf(d + r) * f->tex.us;
        g->u1 = rdf(d + r + 4) * f->tex.us;
        g->v0 = rdf(d + r + 8) * f->tex.vs;
        g->v1 = rdf(d + r + 12) * f->tex.vs;
        g->a = rdf(d + r + 16);
        g->w = rdf(d + r + 20);
        g->c = rdf(d + r + 24);
        g->h = rdf(d + r + 28);
        if (g->h < 0)
            g->h = f->cell_h;
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z')) { lower_n += 1; lower_h += g->h; }
        else if (ch >= 'A' && ch <= 'Z') { upper_n += 1; upper_h += g->h; }
    }
    f->avg_h = lower_n > 0 ? lower_h / lower_n : upper_n > 0 ? upper_h / upper_n : f->cell_h;
    const uint8_t *s = d + 0x358 + nchars, *end = s + nalias;
    while (s < end && *s) {
        int dst = *s++, dst_wide = (dst >= 0x81 && dst <= 0x9F) || (dst >= 0xE0 && dst <= 0xEF);
        if (dst_wide && s < end) s++;
        if (s >= end || !*s) break;
        int src = *s++, src_wide = (src >= 0x81 && src <= 0x9F) || (src >= 0xE0 && src <= 0xEF);
        if (src_wide && s < end) s++;
        if (!dst_wide && !src_wide && f->g[src].ok)
            f->g[dst] = f->g[src];
    }
    f->ok = 1;
    return 1;
}

static const Font *find_font(Gui *g, const char *name)
{
    for (int i = 0; i < g->nfonts; i++)
        if (_stricmp(g->font_names[i], name) == 0)
            return &g->fonts[i];
    if (g->nfonts >= MAX_FONTS || !font_load(&g->fonts[g->nfonts], g, name))
        return NULL;
    snprintf(g->font_names[g->nfonts], sizeof g->font_names[0], "%s", name);
    return &g->fonts[g->nfonts++];
}

/* gApp.localizeString(key): the .loc tables (type 23) of the bundles, keys in any case [hud.c] */
static const char *loc_find(const Gui *g, const char *key)
{
    if (!key || !*key)
        return NULL;
    int col = 1 + (g->lang >= 0 && g->lang < 9 ? g->lang : 0);
    for (int i = 0; i < g->nb; i++) {
        const Bundle *b = g->bundles[i];
        for (uint32_t k = 0; k < b->nentries; k++) {
            const BdpEntry *e = &b->entries[k];
            if (e->type != ET_LOC || !e->data || e->size < 0x54)
                continue;
            const uint8_t *d = e->data;
            uint32_t n = rd32(d + 0x24), keys = rd32(d + 0x28), vals = rd32(d + 0x28 + 4 * col);
            if (keys + 4 * n > e->size || vals + 4 * n > e->size)
                continue;
            for (uint32_t r = 0; r < n; r++) {
                uint32_t ko = rd32(d + keys + 4 * r), vo = rd32(d + vals + 4 * r);
                if (ko < e->size && vo < e->size && _stricmp((const char *)d + ko, key) == 0)
                    return (const char *)d + vo;
            }
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- frames and the draw list */
/* an affine map of the plane, row vectors as the layouts write <x>: (u, v) -> (u a + v c + x, u b + v d + y) */
typedef struct { float a, b, c, d, x, y; } Frame;
static const Frame FR_ID = { 1, 0, 0, 1, 0, 0 };
static void fr_apply(const Frame *f, float u, float v, float *x, float *y)
{
    *x = u * f->a + v * f->c + f->x;
    *y = u * f->b + v * f->d + f->y;
}
/* first `local`, then `parent` */
static Frame fr_mul(const Frame *local, const Frame *parent)
{
    Frame g;
    g.a = local->a * parent->a + local->b * parent->c;
    g.b = local->a * parent->b + local->b * parent->d;
    g.c = local->c * parent->a + local->d * parent->c;
    g.d = local->c * parent->b + local->d * parent->d;
    fr_apply(parent, local->x, local->y, &g.x, &g.y);
    return g;
}

typedef struct {
    ShellQuad *q; int n, max;
    const struct Node **d3; const void **l3; int n3, max3;   /* the 3D objects in the order of their markers, and their
                                                      * layouts (shell_3d) */
    const void *layout;                              /* the layout being emitted */
    float skew;                                      /* the top edge moved along x (a slanted glyph) */
    int additive;                                    /* M 0x80: the additive shader [R 0x17D100] */
} Emit;

/* a picture: the rectangle (x0, y0) .. (x1, y1) of frame f with the texture's (u0, v0) .. (u1, v1) on it */
static void emit_quad(Emit *e, const Frame *f, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                      float v1, const float col[4], const Bundle *b, uint32_t tex)
{
    if (e->n >= e->max || col[3] <= 0.0f)
        return;
    ShellQuad *q = &e->q[e->n++];
    fr_apply(f, x0 + e->skew, y0, &q->x[0], &q->y[0]);
    fr_apply(f, x1 + e->skew, y0, &q->x[1], &q->y[1]);
    fr_apply(f, x1, y1, &q->x[2], &q->y[2]);
    fr_apply(f, x0, y1, &q->x[3], &q->y[3]);
    q->u[0] = u0; q->v[0] = v0; q->u[1] = u1; q->v[1] = v0;
    q->u[2] = u1; q->v[2] = v1; q->u[3] = u0; q->v[3] = v1;
    memcpy(q->col, col, sizeof q->col);
    q->b = b;
    q->tex = tex;
    q->additive = e->additive;
}

/* ---------------------------------------------------------------- texts (hud.c) */
typedef struct { const char *s; int n; float w, h; } TLine;

static int text_icon(const Gui *g, const Font *f, const char *s, int n, TexRef *t, int *ok, float *size)
{
    int i = 1, k = 0;
    char buf[48];
    while (i < n && s[i] != '\v') { if (k < 47) buf[k++] = s[i]; i++; }
    buf[k] = 0;
    if (i < n) i++;
    char *end;
    float sc = strtof(buf, &end);
    if (end == buf) sc = 1;
    *size = f->cell_h * sc;
    *ok = tex_find(g, end, t);
    return i;
}

static void run_measure(const Gui *g, const Font *f, const char *s, int n, int spacing, float slant, float *w_out,
                        float *h_out)
{
    float w = 0, h = 0;
    int any = 0;
    for (int i = 0; i < n;) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\v') {
            TexRef t;
            int ok;
            float size;
            i += text_icon(g, f, s + i, n - i, &t, &ok, &size);
            w += size + (float)spacing;
            any = 1;
            continue;
        }
        i++;
        const Glyph *gl = &f->g[ch];
        if (!gl->ok)
            continue;
        w += gl->a + gl->w + gl->c + (float)spacing;
        if (gl->h > h) h = gl->h;
        any = 1;
    }
    *w_out = any ? w + slant - (float)spacing : 0;
    *h_out = any && h > 0 ? h : f->avg_h;
}

/* the lines of a text (0x1B5AA0) [R]: its '\n's, and with a wrap width (OFont +0x8, in font pixels after s) a line ends
 * once the sum of a + w + c + spacing reaches it: at the last break before (a space, or just after a '-'), else
 * before the character that overflows (one at least); the next line skips the spaces. The width of a line is its
 * measure (run_measure, with the slant) */
static int text_lines(const Gui *g, const Font *f, const char *s, int spacing, float slant, float wrap, TLine *L,
                      int max)
{
    int nl = 0;
    const char *p = s;
    while (nl < max) {
        const char *q = strchr(p, '\n');
        int n = q ? (int)(q - p) : (int)strlen(p), start = 0;
        do {
            int end = n;
            if (wrap > 0) {
                float acc = 0;
                int brk = -1;
                for (int i = start; i < n;) {
                    int j = i;
                    float adv = 0;
                    if (p[i] == '\v') {
                        TexRef t;
                        int ok;
                        float size;
                        i += text_icon(g, f, p + i, n - i, &t, &ok, &size);
                        adv = size + (float)spacing;
                    } else {
                        const Glyph *gl = &f->g[(unsigned char)p[i++]];
                        if (gl->ok)
                            adv = gl->a + gl->w + gl->c + (float)spacing;
                    }
                    if (p[j] == ' ')
                        brk = j;
                    acc += adv;
                    if (acc >= wrap && (brk > start || j > start)) {
                        end = brk > start ? brk : j;
                        break;
                    }
                    if (p[j] == '-')
                        brk = i;
                }
            }
            TLine *l = &L[nl++];
            l->s = p + start;
            l->n = end - start;
            run_measure(g, f, l->s, l->n, spacing, slant, &l->w, &l->h);
            start = end;
            while (start < n && p[start] == ' ') start++;
        } while (start < n && nl < max);
        if (!q)
            break;
        p = q + 1;
    }
    return nl;
}

typedef struct {
    const Font *f;
    int justify, origin, spacing, mode;
    float linesp, maxwidth, wrap, slant;            /* in font pixels (linesp, maxwidth: before s) */
} TextStyle;

static void text_extent(const Gui *g, const TextStyle *ts, const char *s, float *w, float *h)
{
    TLine L[MAX_LINES];
    int nl = text_lines(g, ts->f, s, ts->spacing, ts->slant, ts->wrap, L, MAX_LINES);
    float wmax = 0, hsum = 0;
    for (int i = 0; i < nl; i++) {
        if (L[i].w > wmax) wmax = L[i].w;
        hsum += L[i].h;
    }
    float k = ts->maxwidth > 0 && wmax > ts->maxwidth ? ts->maxwidth / wmax : 1.0f;
    *w = wmax * k;
    *h = hsum * k + ts->linesp * (float)(nl - 1);
}

/* the string draw (0x1B6A80, glyphs 0x1B64B0) [R hud.c], in frame f (font pixels) */
static void text_emit(Emit *e, const Gui *g, const TextStyle *ts, const char *s, const Frame *f, const float col[4],
                      const float shadow[4], int sdx, int sdy)
{
    const Font *fo = ts->f;
    if (!fo || !fo->ok || !s || !*s)
        return;
    TLine L[MAX_LINES];
    int nl = text_lines(g, fo, s, ts->spacing, ts->slant, ts->wrap, L, MAX_LINES);
    float wmax = 0, hsum = 0;
    for (int i = 0; i < nl; i++) {
        if (L[i].w > wmax) wmax = L[i].w;
        hsum += L[i].h;
    }
    float k = 1.0f;
    if (ts->maxwidth > 0 && wmax > ts->maxwidth)
        k = ts->maxwidth / wmax;
    float total_h = hsum * k + ts->linesp * (float)(nl - 1);
    float y = ts->origin == 1 ? -0.5f * total_h : ts->origin == 2 ? -total_h : 0.0f;
    float cell = fo->cell_h * k, slant = ts->slant * k, spacing = (float)ts->spacing;
    for (int l = 0; l < nl; l++) {
        float h = L[l].h * k;
        y += h - cell;
        float yoff = h <= cell ? 0.0f : 0.5f * (fo->avg_h * k - h);
        /* [R 0x1B6CB4 .. 0x1B6E18] j places the block (W its widest line), m each line in it; m 3: each line by j */
        float W = wmax * k, lw = L[l].w * k, x;
        if (ts->mode == 3)
            x = ts->justify == 1 ? -0.5f * lw : ts->justify == 2 ? -lw : 0.0f;
        else
            x = (ts->justify == 1 ? -0.5f * W : ts->justify == 2 ? -W : 0.0f) +
                (ts->mode == 1 ? 0.5f * (W - lw) : ts->mode == 2 ? W - lw : 0.0f);
        if (slant < 0)
            x -= slant;
        for (int pass = 0; pass < 2; pass++) {
            const float *c = pass == 0 ? shadow : col;
            float dx = 0, dy = 0;
            if (pass == 0) {
                if (!shadow || (sdx == 0 && sdy == 0))
                    continue;
                dx = (float)sdx * k;
                dy = (float)sdy * k;
            }
            float cx = x + dx, top = y + yoff + dy, bot = top + cell;
            for (int i = 0; i < L[l].n;) {
                unsigned char ch = (unsigned char)L[l].s[i];
                if (ch == '\v') {
                    TexRef t;
                    int ok;
                    float size;
                    i += text_icon(g, fo, L[l].s + i, L[l].n - i, &t, &ok, &size);
                    if (ok && pass == 1) {
                        float ic[4] = { 1, 1, 1, c[3] }, it = top + 0.5f * (cell - size * k);
                        emit_quad(e, f, cx, it, cx + size * k, it + size * k, 0, 0, t.us, t.vs, ic, t.b, t.name);
                    }
                    cx += k * (size + spacing);
                    continue;
                }
                i++;
                const Glyph *gl = &fo->g[ch];
                if (!gl->ok)
                    continue;
                if (ch == ' ') {
                    cx += k * (gl->a + gl->w + gl->c + spacing);
                    continue;
                }
                cx += gl->a * k;
                e->skew = slant;                         /* the slant moves the top edge (an italic) */
                emit_quad(e, f, cx, top, cx + gl->w * k, bot, gl->u0, gl->v0, gl->u1, gl->v1, c, fo->tex.b,
                          fo->tex.name);
                e->skew = 0;
                cx += k * (spacing + gl->w + gl->c);
            }
        }
        y += cell + ts->linesp;
    }
}

/* ---------------------------------------------------------------- the layout tree (.ifc) */
/* N_FX: the layouts' effects (<e>), shapes (<s>), light sets (<ls>, <lt>) and child interfaces (<X>): objects the
 * scripts find (GuiEffect, GuiShape, GuiLight); the shapes and effects are drawn by the viewer (shell_3d) */
enum { N_GROUP, N_2D, N_QUAD, N_TEXT, N_SLIDER, N_RECT, N_LIST, N_LINE, N_FX };
typedef struct Node {
    int kind;
    char name[32];
    float p[3], m;              /* <p> x y z, <m> scale (GuiObject.setScale) */
    float rot[4]; int has_rot;  /* <x>'s 3x3 (its 2D part) over its scale: a turn and / or a mirror; GuiObject.yaw */
    float sx;                   /* GuiObject.setScaleX: the object's x axis by this [G: on top of m] */
    float d[2]; int has_d;
    float col[4];
    unsigned F, Q, M;
    TexRef tex; int has_tex;
    char text[MAX_TEXT];
    const Font *font;
    int justify, origin, spacing, has_spacing, linesp, has_linesp;
    float tscale, maxwidth, wrap;
    int tmode, has_tslant; float tslant;   /* <g> m (3: each line by j) and i */
    float tcol[4]; int has_tcol;
    float shadow[4]; int sh_dx, sh_dy, has_shadow;   /* <sh>: a text's own shadow (colour AABBGGRR [G], dx, dy) */
    float val, vmin, vmax;
    int mask_bar;
    float bar_col[4], mask_col[4];
    TexRef side, corner; int has_side, has_corner;
    float bkgd[4], border[4], B, bbias;
    float hl_col[4], norm_col[4], dis_col[4], sel_col[4];
    int has_hl, has_norm, has_dis;
    TexRef hl_tex, endcap; int has_hl_tex, has_endcap;
    float hl_d[2];
    int sel, pulse, top;        /* list box: the selection, setHighlightPulse, the first item shown */
    float ep[4]; int has_ep;
    float lw;                                            /* a line's <width>: 3 without (GuiLine ctor 0x23BA10) */
    int visible;
    int video;                  /* the Video quad: the movie's picture when there is one */
    /* the 3D objects (N_FX): what they are, their files and their state */
    int fx;                     /* FX_EFFECT <e>, FX_SHAPE <s>, FX_LIGHTSET <ls>, FX_LIGHT <lt>; 0 the rest */
    char res[40];               /* the shape's model (m=, "CITYSELECT_GLOBE.PWK"), the effect's edf= */
    char attach[32];            /* <k>: the parent shape's node the effect sits on */
    char lights[32];            /* <la>: the light set of the interface that lights it */
    float r3[9], m3; int r3_own;   /* its own 3x3 by rows (<x>, setIdentity, setLookAt) and the <m> after it; 0: the
                                 * 2D frame's turn and m */
    unsigned eflags;            /* GuiEffect +0xB4 (FXE_*) */
    int drawn, gen;             /* draws since its effect was set (+0xAC); setEffect's count */
    float amb[4], dif[4], spec[4];   /* <ls> A, <lt> A D S (RRGGBBAA) */
    float w3[16];               /* its world when it was last drawn (shell_3d) */
    float fade;
    struct Node *parent, *child, *next;
} Node;

typedef struct {
    Node *nodes;
    int n, cap;
    Node *root;
    const Bundle *b;            /* the bundle it is from (its 3D objects' files) */
} Layout;

/* GuiEffect's flags (+0xB4) [R 0x237520 .. 0x2377E0]: 1 started by the script alone (auto="false", <E> 0x1), 0x200
 * running (until its instance is finished, a stopped one too), 0x400 to start (on the first update after it was
 * drawn), 0x800 to stop; FXE_SHUT (the port's): it was stopped and plays out */
enum { FXE_MANUAL = 0x1, FXE_RUN = 0x200, FXE_START = 0x400, FXE_STOP = 0x800, FXE_SHUT = 0x1000 };
enum { FX_OTHER, FX_EFFECT, FX_SHAPE, FX_LIGHTSET, FX_LIGHT };

typedef struct { const char *p, *end; } Xml;
typedef struct { char k[16], v[64]; } Attr;

static void xml_skip_ws(Xml *x)
{
    while (x->p < x->end && (*x->p == ' ' || *x->p == '\t' || *x->p == '\r' || *x->p == '\n'))
        x->p++;
}

static int xml_name(Xml *x, char *out, int max)
{
    int n = 0;
    while (x->p < x->end && *x->p != ' ' && *x->p != '>' && *x->p != '/' && *x->p != '=' && *x->p != '"') {
        if (n < max - 1) out[n++] = *x->p;
        x->p++;
    }
    out[n] = 0;
    return n;
}

static void parse_floats(const char *s, float *out, int max)
{
    char *e;
    for (int i = 0; i < max; i++) {
        float v = strtof(s, &e);
        if (e == s) break;
        out[i] = v;
        s = e;
    }
}

static void node_text(Node *nd, const char *s)
{
    if (nd)
        snprintf(nd->text, sizeof nd->text, "%s", s ? s : "");
}
/* GuiText.localizeString: a key without a text keeps the key [G] */
static void node_localize(const Gui *g, Node *nd, const char *key)
{
    /* [R 0x235800] a key starting with a digit is the text itself; one the strings lack shows "0x key x0" */
    if (key && isdigit((unsigned char)key[0])) {
        node_text(nd, key);
        return;
    }
    const char *s = loc_find(g, key);
    if (s) {
        node_text(nd, s);
        return;
    }
    char buf[MAX_TEXT];
    snprintf(buf, sizeof buf, "0x %s x0", key ? key : "");
    node_text(nd, buf);
}

/* the <g> attributes (0x22D960) [R]: only the first letter counts, either case; a fresh OFont each time. j and o
 * share one parser (0x2350E0): l / c / r set the justify (+0x4), t / m / b the origin (+0x5), a longer value only as
 * left, center, right, top, middle, bottom; s the scale (+0xC); c the char spacing (+0x10), l the line spacing
 * (+0x11); w the word wrap's width (+0x8); i the slant (+0x44, over the font's); m how each line sits in the block
 * (+0x7: l 0, c 1, r 2; 3 each line by j) */
static int g_word(const char *v, const char *word)
{
    if (!v[1])
        return tolower((unsigned char)v[0]) == word[0];
    for (; *v && *word; v++, word++)
        if (tolower((unsigned char)*v) != *word)
            return 0;
    return !*v && !*word;
}
static void apply_g(Node *nd, const char *attr, const char *val)
{
    static const char *const H[3] = { "left", "center", "right" }, *const V[3] = { "top", "middle", "bottom" };
    switch (tolower((unsigned char)attr[0])) {
    case 'j':
    case 'o':
        for (int k = 0; k < 3; k++) {
            if (g_word(val, H[k])) { nd->justify = k; break; }
            if (g_word(val, V[k])) { nd->origin = k; break; }
        }
        break;
    case 's': nd->tscale = strtof(val, NULL); break;
    case 'c': nd->spacing = atoi(val); nd->has_spacing = 1; break;
    case 'l': nd->linesp = atoi(val); nd->has_linesp = 1; break;
    case 'w': nd->wrap = strtof(val, NULL); break;
    case 'i': nd->tslant = strtof(val, NULL); nd->has_tslant = 1; break;
    case 'm': {
        int c = tolower((unsigned char)val[0]);
        nd->tmode = c == 'l' ? 0 : c == 'c' ? 1 : c == 'r' ? 2 : 3;
        break;
    }
    default: break;
    }
}

/* GuiObject.yaw(a): the object's turn about the screen's normal, replacing the one it had [G: the scripts give the
 * whole angle each time; Loading.ifc's <x> holds the turn yaw(-4.15 pi) gives, 27 degrees] */
static void node_yaw(Node *nd, float a)
{
    if (!nd)
        return;
    nd->rot[0] = cosf(a); nd->rot[1] = sinf(a);
    nd->rot[2] = -sinf(a); nd->rot[3] = cosf(a);
    nd->has_rot = 1;
}

static void apply_prop(Gui *g, Node *nd, const char *tag, const char *text, const Attr *at, int na)
{
    const char *which = na > 0 ? at[0].k : "";
    if (strcmp(tag, "p") == 0) parse_floats(text, nd->p, 3);
    else if (strcmp(tag, "d") == 0) {
        if (strcmp(which, "highlight") == 0) parse_floats(text, nd->hl_d, 2);
        else { parse_floats(text, nd->d, 2); nd->has_d = 1; }
    } else if (strcmp(tag, "m") == 0) {
        nd->m = strtof(text, NULL);
        if (nd->r3_own) nd->m3 *= nd->m;                 /* [R 0x22DDF0: the 3x3 as it is, times m] */
    } else if (strcmp(tag, "z") == 0)                    /* [R 0x22C450: z is the fade (+0x5C), not a depth] */
        nd->fade = strtof(text, NULL);
    else if (nd->kind == N_FX && strcmp(tag, "k") == 0) snprintf(nd->attach, sizeof nd->attach, "%s", text);
    else if (nd->kind == N_FX && strcmp(tag, "la") == 0) snprintf(nd->lights, sizeof nd->lights, "%s", text);
    else if (nd->kind == N_FX && strcmp(tag, "E") == 0)  /* [R 0x22B1B0: replaces +0xB4] */
        nd->eflags = (unsigned)strtoul(text, NULL, 0);
    else if (nd->kind == N_FX && strcmp(tag, "A") == 0) col_rgba((uint32_t)strtoul(text, NULL, 0), nd->amb);
    else if (nd->kind == N_FX && strcmp(tag, "D") == 0) col_rgba((uint32_t)strtoul(text, NULL, 0), nd->dif);
    else if (nd->kind == N_FX && strcmp(tag, "S") == 0) col_rgba((uint32_t)strtoul(text, NULL, 0), nd->spec);
    else if (strcmp(tag, "F") == 0) nd->F = (unsigned)strtoul(text, NULL, 0);
    else if (strcmp(tag, "Q") == 0) nd->Q = (unsigned)strtoul(text, NULL, 0);
    else if (strcmp(tag, "M") == 0) nd->M = (unsigned)strtoul(text, NULL, 0);
    else if (strcmp(tag, "c") == 0) {
        float c[4];
        uint32_t v = (uint32_t)strtoul(text, NULL, 0);
        col_rgba(v, c);
        if (strcmp(which, "highlight") == 0) { memcpy(nd->hl_col, c, sizeof c); nd->has_hl = 1; }
        else if (strcmp(which, "normal") == 0) { memcpy(nd->norm_col, c, sizeof c); nd->has_norm = 1; }
        else if (strcmp(which, "disabled") == 0) { memcpy(nd->dis_col, c, sizeof c); nd->has_dis = 1; }
        else if (strcmp(which, "selector") == 0) memcpy(nd->sel_col, c, sizeof c);
        else if (nd->kind == N_TEXT) { memcpy(nd->tcol, c, sizeof c); nd->has_tcol = 1; }
        else
            memcpy(nd->col, c, sizeof c);
    } else if (strcmp(tag, "b") == 0) {
        if (strcmp(which, "highlight") == 0) nd->has_hl_tex = tex_find(g, text, &nd->hl_tex);
        else if (strcmp(which, "endcap") == 0) nd->has_endcap = tex_find(g, text, &nd->endcap);
        else nd->has_tex = tex_find(g, text, &nd->tex);
    } else if (strcmp(tag, "v") == 0) {
        char *o = nd->text;
        int n = 0;
        static const struct { const char *e; char c; } ENT[] = {
            { "&apos;", '\'' }, { "&quot;", '"' }, { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' } };
        for (const char *s = text; *s && n < MAX_TEXT - 1; s++) {
            if (*s == '\r')
                continue;
            size_t k = 0;
            if (*s == '&')
                for (; k < sizeof ENT / sizeof *ENT; k++)
                    if (strncmp(s, ENT[k].e, strlen(ENT[k].e)) == 0)
                        break;
            if (*s == '&' && k < sizeof ENT / sizeof *ENT) {
                o[n++] = ENT[k].c;
                s += strlen(ENT[k].e) - 1;
            } else
                o[n++] = *s;
        }
        o[n] = 0;
    } else if (strcmp(tag, "L") == 0)
        node_localize(g, nd, text);
    else if (strcmp(tag, "g") == 0) {
        for (int i = 0; i < na; i++)
            apply_g(nd, at[i].k, at[i].v);
        nd->font = find_font(g, text);
    } else if (strcmp(tag, "sh") == 0) {
        /* "3 0xFF000000 -4 4" on the cards' names: taken as flags, colour (the engine's AABBGGRR: opaque black), dx,
         * dy [G] */
        char *e1, *e2;
        strtoul(text, &e1, 0);
        uint32_t c = (uint32_t)strtoul(e1, &e2, 0);
        float dd[2] = { 0, 0 };
        parse_floats(e2, dd, 2);
        col_abgr(c, nd->shadow);
        nd->sh_dx = (int)dd[0];
        nd->sh_dy = (int)dd[1];
        nd->has_shadow = 1;
    } else if (strcmp(tag, "maxwidth") == 0) nd->maxwidth = strtof(text, NULL);
    else if (strcmp(tag, "val") == 0) nd->val = strtof(text, NULL);
    else if (strcmp(tag, "hasMaskBar") == 0) nd->mask_bar = atoi(text) != 0;
    else if (strcmp(tag, "barColor") == 0) col_rgba((uint32_t)strtoul(text, NULL, 0), nd->bar_col);
    else if (strcmp(tag, "maskColor") == 0) col_rgba((uint32_t)strtoul(text, NULL, 0), nd->mask_col);
    else if (strcmp(tag, "st") == 0) nd->has_side = tex_find(g, text, &nd->side);
    else if (strcmp(tag, "ct") == 0) nd->has_corner = tex_find(g, text, &nd->corner);
    else if (strcmp(tag, "bkgdColor") == 0) col_rgba((uint32_t)strtoul(text, NULL, 0), nd->bkgd);
    else if (strcmp(tag, "bc") == 0) col_rgba((uint32_t)strtoul(text, NULL, 0), nd->border);
    else if (strcmp(tag, "B") == 0) nd->B = strtof(text, NULL);
    else if (strcmp(tag, "bbias") == 0) nd->bbias = strtof(text, NULL);
    else if (strcmp(tag, "x") == 0) {
        /* the object's whole frame: the 3x3 by rows, then the position [R hud.c]; its 2D part over the length of its
         * first row is the turn (or mirror), that length its scale */
        float x[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
        parse_floats(text, x, 12);
        memcpy(nd->p, x + 9, sizeof nd->p);
        memcpy(nd->r3, x, sizeof nd->r3);                /* [R 0x22DEC0: rows, then the place] */
        nd->r3_own = 1;
        nd->m3 = 1;
        float m = sqrtf(x[0] * x[0] + x[1] * x[1]);
        if (m <= 0) m = 1;
        nd->m = m;
        nd->rot[0] = x[0] / m; nd->rot[1] = x[1] / m;
        nd->rot[2] = x[3] / m; nd->rot[3] = x[4] / m;
        nd->has_rot = 1;
    } else if (strcmp(tag, "ep") == 0) {
        float e[16] = { 0 };
        parse_floats(text, e, 16);
        int n = (int)e[0];
        if (n >= 2 && n <= 5) {
            nd->ep[0] = e[1]; nd->ep[1] = e[2];
            nd->ep[2] = e[1 + 3 * (n - 1)]; nd->ep[3] = e[2 + 3 * (n - 1)];
            nd->has_ep = 1;
        }
    } else if (strcmp(tag, "width") == 0) {              /* a GuiLine's width (+0x228) */
        float w = 3;
        parse_floats(text, &w, 1);
        nd->lw = w;
    }
}

/* the object tags; everything else is a property of the object around it */
static int node_kind(const char *tag)
{
    if (strcmp(tag, "q") == 0) return N_QUAD;
    if (strcmp(tag, "t") == 0) return N_TEXT;
    if (strcmp(tag, "sl") == 0) return N_SLIDER;
    if (strcmp(tag, "r") == 0) return N_RECT;
    if (strcmp(tag, "lb") == 0) return N_LIST;
    if (strcmp(tag, "l") == 0) return N_LINE;
    if (strcmp(tag, "DD") == 0) return N_2D;
    if (strcmp(tag, "o") == 0 || strcmp(tag, "i") == 0 || strcmp(tag, "sz") == 0) return N_GROUP;
    if (strcmp(tag, "e") == 0 || strcmp(tag, "s") == 0 || strcmp(tag, "ls") == 0 || strcmp(tag, "lt") == 0 ||
        strcmp(tag, "X") == 0)
        return N_FX;
    return -1;
}

static Node *node_new(Layout *L, int kind)
{
    if (L->n >= L->cap)
        return NULL;
    Node *nd = &L->nodes[L->n++];
    memset(nd, 0, sizeof *nd);
    nd->kind = kind;
    nd->m = 1;
    nd->sx = 1;
    nd->tscale = 1;
    nd->tmode = 3;
    for (int i = 0; i < 4; i++) nd->col[i] = nd->bar_col[i] = nd->mask_col[i] = nd->border[i] = 1;
    nd->val = 100;
    nd->vmax = 100;
    nd->B = 5;                                           /* [R GuiRect ctor 0x245650: 5.0 at +0x180, setBorderWidth's
                                                          * (0x244410, the `B` tag 0x22A9E0)] */
    nd->lw = 3;
    nd->sel = -1;
    nd->visible = 1;
    nd->fade = 1;
    nd->r3[0] = nd->r3[4] = nd->r3[8] = 1;
    nd->m3 = 1;
    nd->amb[3] = nd->spec[3] = 1;
    for (int i = 0; i < 3; i++) nd->dif[i] = 128.0f / 255.0f;   /* [R 0x239E90: a light's diffuse 128 grey] */
    nd->dif[3] = 1;
    return nd;
}

/* the attributes of <s> (0x22E4F0: m / M / mesh the model) and <e> (0x22E730: auto, then edf = setEffect, which sets
 * "to start" unless it is manual) [R] */
static void fx_attrs(Node *nd, const char *tag, const Attr *at, int na)
{
    nd->fx = !strcmp(tag, "e") ? FX_EFFECT : !strcmp(tag, "s") ? FX_SHAPE : !strcmp(tag, "ls") ? FX_LIGHTSET :
             !strcmp(tag, "lt") ? FX_LIGHT : FX_OTHER;
    for (int i = 0; i < na; i++) {
        if (nd->fx == FX_SHAPE && (!strcmp(at[i].k, "m") || !strcmp(at[i].k, "M") || !strcmp(at[i].k, "mesh")))
            snprintf(nd->res, sizeof nd->res, "%s", at[i].v);
        else if (nd->fx == FX_EFFECT && !strcmp(at[i].k, "auto"))
            nd->eflags = strcmp(at[i].v, "true") ? nd->eflags | FXE_MANUAL : nd->eflags & ~FXE_MANUAL;
        else if (nd->fx == FX_EFFECT && !strcmp(at[i].k, "edf"))
            snprintf(nd->res, sizeof nd->res, "%s", at[i].v);
    }
    if (nd->fx == FX_EFFECT && nd->res[0] && !(nd->eflags & FXE_MANUAL))
        nd->eflags |= FXE_START;
}

static void node_attach(Node *nd, Node *parent)
{
    nd->parent = parent;
    nd->next = NULL;
    if (!parent)
        return;
    Node **pp = &parent->child;
    while (*pp) pp = &(*pp)->next;
    *pp = nd;
}

static int xml_element(Gui *g, Layout *L, Xml *x, Node *parent)
{
    xml_skip_ws(x);
    if (x->p >= x->end || *x->p != '<')
        return 0;
    x->p++;
    if (x->p < x->end && *x->p == '?') {
        while (x->p + 1 < x->end && !(x->p[0] == '?' && x->p[1] == '>')) x->p++;
        x->p += 2;
        return 1;
    }
    char tag[24];
    xml_name(x, tag, sizeof tag);
    int kind = node_kind(tag);
    Node *nd = kind >= 0 ? node_new(L, kind) : NULL;
    Attr at[8];
    int na = 0;
    for (;;) {
        xml_skip_ws(x);
        if (x->p >= x->end) return 0;
        if (*x->p == '/' || *x->p == '>') break;
        char an[24], av[64];
        xml_name(x, an, sizeof an);
        xml_skip_ws(x);
        av[0] = 0;
        if (x->p < x->end && *x->p == '=') {
            x->p++;
            xml_skip_ws(x);
            if (x->p < x->end && *x->p == '"') {
                x->p++;
                int n = 0;
                while (x->p < x->end && *x->p != '"') { if (n < 63) av[n++] = *x->p; x->p++; }
                av[n] = 0;
                x->p++;
            }
        }
        if (nd && strcmp(an, "n") == 0)
            snprintf(nd->name, sizeof nd->name, "%s", av);
        else if (na < 8) {
            snprintf(at[na].k, sizeof at[na].k, "%s", an);
            snprintf(at[na].v, sizeof at[na].v, "%s", av);
            na++;
        }
    }
    if (nd && nd->kind == N_FX)
        fx_attrs(nd, tag, at, na);
    if (nd)
        node_attach(nd, parent);
    if (*x->p == '/') {
        x->p += 2;
        goto done;
    }
    x->p++;
    {
        const char *t0 = x->p;
        xml_skip_ws(x);
        if (x->p < x->end && *x->p == '<' && x->p + 1 < x->end && x->p[1] != '/') {
            /* element children: an unknown tag that has some is an object too (its properties are its own, never
             * its parent's: hud.c's reader gave the main menu's <e> effects' <x> to the interface) */
            Node *holder = nd;
            if (!holder) {
                holder = node_new(L, N_FX);
                if (holder) {
                    node_attach(holder, parent);
                    holder->visible = 0;
                }
            }
            while (x->p < x->end) {
                xml_skip_ws(x);
                if (x->p + 1 < x->end && x->p[0] == '<' && x->p[1] == '/')
                    break;
                if (!xml_element(g, L, x, holder ? holder : parent))
                    return 0;
            }
        } else {
            x->p = t0;
            const char *t1 = t0;
            while (t1 < x->end && *t1 != '<') t1++;
            char text[MAX_TEXT];
            int n = (int)(t1 - t0) < MAX_TEXT - 1 ? (int)(t1 - t0) : MAX_TEXT - 1;
            memcpy(text, t0, (size_t)n);
            text[n] = 0;
            x->p = t1;
            if (!nd && parent)
                apply_prop(g, parent, tag, text, at, na);
        }
        while (x->p < x->end && *x->p != '>') x->p++;
        x->p++;
    }
done:
    if (nd) {
        if (nd->F & 1)
            nd->visible = 0;                             /* [G hud.c: F 0x1 = starts hidden] */
        if (!parent)
            L->root = nd;
    }
    return 1;
}

/* a layout, with room for `extra` more objects (copies) */
static Layout *layout_load(Gui *g, const Bundle *b, const char *name, int extra)
{
    int n = b ? bdp_find_name(b, name) : -1;
    const BdpEntry *e = n >= 0 ? bdp_entry_of(b, (uint32_t)n, ET_IFC) : NULL;
    if (!e || !e->data)
        return NULL;
    int elements = 0;
    for (uint32_t i = 0; i < e->size; i++)
        elements += e->data[i] == '<';
    Layout *L = calloc(1, sizeof *L);
    if (!L)
        return NULL;
    L->cap = elements / 2 + 8 + extra;
    L->b = b;
    L->nodes = calloc((size_t)L->cap, sizeof *L->nodes);
    if (!L->nodes) {
        free(L);
        return NULL;
    }
    Xml x = { (const char *)e->data, (const char *)e->data + e->size };
    while (x.p < x.end && *x.p && xml_element(g, L, &x, NULL))
        xml_skip_ws(&x);
    if (!L->root) {
        free(L->nodes);
        free(L);
        return NULL;
    }
    return L;
}
static void layout_free(Layout *L)
{
    if (L) {
        free(L->nodes);
        free(L);
    }
}

/* GuiObject.findObject(name): depth first among the object's descendants [G: not the object itself; CitySelect's
 * cityStats.findObject("city_stats") must reach the rect inside the quad of that name] */
static Node *find_in(Node *nd, const char *name)
{
    for (; nd; nd = nd->next) {
        if (strcmp(nd->name, name) == 0)
            return nd;
        Node *c = find_in(nd->child, name);
        if (c)
            return c;
    }
    return NULL;
}
static Node *find(Node *in, const char *name) { return in && name ? find_in(in->child, name) : NULL; }
static Node *findf(Node *in, const char *fmt, int v)
{
    char name[48];
    snprintf(name, sizeof name, fmt, v);
    return find(in, name);
}
static Node *child_at(Node *nd, int i)
{
    Node *c = nd && i >= 0 ? nd->child : NULL;
    while (c && i-- > 0) c = c->next;
    return c;
}
static int child_count(const Node *nd)
{
    int n = 0;
    for (const Node *c = nd ? nd->child : NULL; c; c = c->next) n++;
    return n;
}
/* GuiObject.copyTree + setParent: a deep copy as the parent's last child [G: last] */
static Node *node_copy(Layout *L, const Node *src, Node *parent)
{
    if (!src || L->n >= L->cap)
        return NULL;
    Node *c = &L->nodes[L->n++];
    *c = *src;
    c->child = NULL;
    node_attach(c, parent);
    for (const Node *k = src->child; k; k = k->next)
        node_copy(L, k, c);
    return c;
}

static void show(Node *nd, int v) { if (nd) nd->visible = v != 0; }
static void set_fade(Node *nd, float f) { if (nd) nd->fade = f; }
static float get_fade(const Node *nd) { return nd ? nd->fade : 0; }
static void set_pos(Node *nd, float x, float y, float z) { if (nd) { nd->p[0] = x; nd->p[1] = y; nd->p[2] = z; } }
static void set_text(Node *nd, const char *s) { node_text(nd, s); }
static void set_tcol(Node *nd, const float c[4]) { if (nd) { memcpy(nd->tcol, c, sizeof nd->tcol); nd->has_tcol = 1; } }
/* GuiObject.setColor: a text's colour, any other object's */
static void set_col(Node *nd, const float c[4])
{
    if (!nd)
        return;
    if (nd->kind == N_TEXT)
        set_tcol(nd, c);
    else
        memcpy(nd->col, c, sizeof nd->col);
}
/* GuiQuad.setTexture / resetTexture */
static void set_tex(const Gui *g, Node *nd, const char *name)
{
    if (nd)
        nd->has_tex = tex_find(g, name, &nd->tex);
}

static void text_style(const Node *nd, TextStyle *ts)
{
    ts->f = nd->font;
    ts->justify = nd->justify;
    ts->origin = nd->origin;
    ts->spacing = nd->has_spacing ? nd->spacing : nd->font ? nd->font->spacing : 0;
    float tsc = nd->tscale > 0 ? nd->tscale : 1;
    /* [R] the line spacing is added unscaled (0x1B6F58), maxwidth is held against the width after s (0x1B6270) */
    ts->linesp = (float)(nd->has_linesp ? nd->linesp : nd->font ? nd->font->linespacing : 0) / tsc;
    ts->maxwidth = nd->maxwidth / tsc;
    ts->mode = nd->tmode;
    ts->slant = nd->has_tslant ? nd->tslant : nd->font ? (float)nd->font->slant : 0;
    ts->wrap = nd->wrap > 0 && nd->tscale > 0 ? nd->wrap / nd->tscale : 0;
}
/* GuiText.width() / height(): by the text's own scale s [hud.c] */
static float text_w(const Gui *g, const Node *nd)
{
    if (!nd || !nd->font)
        return 0;
    TextStyle ts;
    float w, h;
    text_style(nd, &ts);
    text_extent(g, &ts, nd->text, &w, &h);
    return w * nd->tscale;
}
static float text_h(const Gui *g, const Node *nd)
{
    if (!nd || !nd->font)
        return 0;
    TextStyle ts;
    float w, h;
    text_style(nd, &ts);
    text_extent(g, &ts, nd->text, &w, &h);
    return h * nd->tscale;
}

/* ---------------------------------------------------------------- list boxes (GuiListBox, hud.c) */
static int list_count(const Node *l) { return child_count(l); }
static Node *list_item(Node *l, int i) { return child_at(l, i); }
static void list_select(Node *l, int i)
{
    if (!l)
        return;
    l->sel = i;
    int j = 0;
    for (Node *c = l->child; c; c = c->next, j++) {
        if (c->kind != N_TEXT)
            continue;
        if (j == i && l->has_hl) { memcpy(c->tcol, l->hl_col, sizeof c->tcol); c->has_tcol = 1; }
        else if (j != i && l->has_norm) { memcpy(c->tcol, l->norm_col, sizeof c->tcol); c->has_tcol = 1; }
    }
}
/* incrementSelection(delta, 0): to the next item shown, no wrap [G hud.c] */
static void list_increment(Node *l, int delta)
{
    if (!l)
        return;
    int n = list_count(l);
    for (int j = l->sel + delta; j >= 0 && j < n; j += delta) {
        Node *c = list_item(l, j);
        if (c && c->visible) {
            list_select(l, j);
            return;
        }
    }
}
/* incrementSelection(delta, 1): the same, going round [G hud.c] */
static void list_increment_wrap(Node *l, int delta)
{
    int n = list_count(l);
    if (!l || n <= 0)
        return;
    for (int k = 1, j = l->sel; k <= n; k++) {
        j = ((j + delta) % n + n) % n;
        Node *c = list_item(l, j);
        if (c && c->visible) {
            list_select(l, j);
            return;
        }
    }
}
/* GuiListBox.isEnabled(i) [G: an item that exists and shows] */
static int list_enabled(Node *l, int i)
{
    Node *c = list_item(l, i);
    return c && c->visible;
}

/* ---------------------------------------------------------------- drawing a layout */
/* GuiRect [R hud.c]: the background inset by -bbias / 2 within d, then the frame inside d: the corner picture
 * mirrored to each corner, the side picture between (the left and right sides as one-pixel strips of it); a rect with
 * no pictures (MonsterSelect's highlights) gets plain bars B wide [G] */
static void rect_emit(Emit *e, const Node *nd, const Frame *f, float fade)
{
    float w = nd->has_d ? nd->d[0] : 0, h = nd->has_d ? nd->d[1] : 0;
    float bg[4] = { nd->bkgd[0], nd->bkgd[1], nd->bkgd[2], nd->bkgd[3] * fade };
    float bc[4] = { nd->border[0], nd->border[1], nd->border[2], nd->border[3] * fade };
    if (w <= 0 || h <= 0)
        return;
    float bb = -0.5f * nd->bbias;
    emit_quad(e, f, bb, bb, w - bb, h - bb, 0, 0, 1, 1, bg, NULL, 0);
    float B = nd->B;
    if (B <= 0)
        return;
    float x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (x1 - x0 < 2 * B) x1 = x0 + 2 * B;
    if (y1 - y0 < 2 * B) y1 = y0 + 2 * B;
    if (!nd->has_corner && !nd->has_side) {
        emit_quad(e, f, x0, y0, x1, y0 + B, 0, 0, 1, 1, bc, NULL, 0);
        emit_quad(e, f, x0, y1 - B, x1, y1, 0, 0, 1, 1, bc, NULL, 0);
        emit_quad(e, f, x0, y0 + B, x0 + B, y1 - B, 0, 0, 1, 1, bc, NULL, 0);
        emit_quad(e, f, x1 - B, y0 + B, x1, y1 - B, 0, 0, 1, 1, bc, NULL, 0);
        return;
    }
    if (nd->has_corner) {
        const TexRef *t = &nd->corner;
        emit_quad(e, f, x0, y0, x0 + B, y0 + B, 0, 0, t->us, t->vs, bc, t->b, t->name);
        emit_quad(e, f, x1 - B, y0, x1, y0 + B, t->us, 0, 0, t->vs, bc, t->b, t->name);
        emit_quad(e, f, x0, y1 - B, x0 + B, y1, 0, t->vs, t->us, 0, bc, t->b, t->name);
        emit_quad(e, f, x1 - B, y1 - B, x1, y1, t->us, t->vs, 0, 0, bc, t->b, t->name);
    }
    if (nd->has_side) {
        const TexRef *t = &nd->side;
        emit_quad(e, f, x0 + B, y0, x1 - B, y0 + B, 0, t->vs, t->us, 0, bc, t->b, t->name);
        emit_quad(e, f, x0 + B, y1 - B, x1 - B, y1, 0, 0, t->us, t->vs, bc, t->b, t->name);
        float sc = sqrtf(fabsf(f->a * f->d - f->b * f->c));
        int ns = (int)(B * sc + 0.5f);
        if (ns < 1) ns = 1;
        if (ns > 32) ns = 32;
        for (int i = 0; i < ns; i++) {
            float a = B * (float)i / (float)ns, b = B * (float)(i + 1) / (float)ns;
            float v = t->vs * (1.0f - ((float)i + 0.5f) / (float)ns);
            emit_quad(e, f, x0 + a, y0 + B, x0 + b, y1 - B, 0.5f * t->us, v, 0.5f * t->us, v, bc, t->b, t->name);
            emit_quad(e, f, x1 - b, y0 + B, x1 - a, y1 - B, 0.5f * t->us, v, 0.5f * t->us, v, bc, t->b, t->name);
        }
    }
}

/* the list's highlight behind its selected item [G hud.c] */
static void list_emit(Emit *e, const Gui *g, const Node *l, const Frame *f, float fade)
{
    if (l->col[3] > 0 && l->has_d)
        emit_quad(e, f, 0, 0, l->d[0], l->d[1], 0, 0, 1, 1,
                  (const float[4]){ l->col[0], l->col[1], l->col[2], l->col[3] * fade }, NULL, 0);
    const Node *it = list_item((Node *)l, l->sel);
    if (!l->has_hl_tex || !it || !it->visible || it->kind != N_TEXT)
        return;
    float w = text_w(g, it) * it->m, h = text_h(g, it) * it->m;
    float x0 = it->p[0] - (it->justify == 1 ? 0.5f * w : it->justify == 2 ? w : 0), x1 = x0 + w;
    float cy = it->p[1] + (it->origin == 0 ? 0.5f * h : it->origin == 2 ? -0.5f * h : 0);
    float bh = l->hl_d[1] > 0 ? l->hl_d[1] : h, cap = 0.5f * bh;
    float c[4] = { l->sel_col[0], l->sel_col[1], l->sel_col[2], l->sel_col[3] * fade };
    if (l->pulse)
        c[3] *= 0.7f + 0.3f * sinf(2.0f * SH_PI * 1.5f * g->time);
    float y0 = cy - 0.5f * bh, y1 = cy + 0.5f * bh;
    const TexRef *t = &l->hl_tex;
    emit_quad(e, f, x0, y0, x1, y1, 0, 0, t->us, t->vs, c, t->b, t->name);
    if (l->has_endcap) {
        const TexRef *k = &l->endcap;
        emit_quad(e, f, x0 - cap, y0, x0, y1, k->us, 0, 0, k->vs, c, k->b, k->name);
        emit_quad(e, f, x1, y0, x1 + cap, y1, 0, 0, k->us, k->vs, c, k->b, k->name);
    }
}

/* an object's own frame in its parent's: position, then scale m, its turn (<x>, yaw) and setScaleX's stretch of its
 * x axis */
static Frame node_local(const Node *nd)
{
    Frame l = { nd->m, 0, 0, nd->m, nd->p[0], nd->p[1] };
    if (nd->has_rot) {
        l.a = nd->m * nd->rot[0]; l.b = nd->m * nd->rot[1];
        l.c = nd->m * nd->rot[2]; l.d = nd->m * nd->rot[3];
    }
    l.a *= nd->sx;
    l.b *= nd->sx;
    return l;
}

/* The GUI's 3D objects: an object outside every 2D root (<DD>) has its position in screen widths about the screen's
 * centre, x right and y down (MonsterSelect's cards at x -0.325 -0.108 0.108 0.325 for four players stand 139 pixels
 * apart, as their "P1".."P4" texts at 54 193 332 470 do; two players' 0.025 down puts the card's top at 117, the masks'
 * edge) [G: the GUI camera is not traced], and a 2D root inside one draws its pixels about (320, 240) moved to that
 * point and scaled by it, so setScaleX on a card squashes it about its own centre [R MonsterSelect.scr's flip]. */
static Frame unit_to_pixels(const Frame *u)
{
    Frame p = { u->a, u->b, u->c, u->d, 0, 0 };
    p.x = 320.0f + 640.0f * u->x - (320.0f * u->a + 240.0f * u->c);
    p.y = 240.0f + 640.0f * u->y - (320.0f * u->b + 240.0f * u->d);
    return p;
}

/* ---- the 3D objects. Frames are row vectors (p' = p r + t) and a world is local x parent's [R 0x23FF70], up to the
 * interface (its own frame left out, as in layout_emit). The GUI camera [R 0x227AD0, 0x227F80]: eye (0, -1, 0) looking
 * along +y, z up, a vertical field of view with tan(fov / 2) = 0.375, near 1, far 10000; each interface's frame is
 * followed by rows (1, 0, 0), (0, 0, -1), (0, 1, 0) [R 0x239230 +0x78]: GUI (x, y, z) is the world's (x, z, -y), so a
 * GUI point is at px = 320 + 640 x / (1 + z), py = 240 + 640 y / (1 + z). A top-level <DD> maps its pixels to
 * (X / 640 - 0.5, Y / 640 - 0.375, 0) [R 0x2363F0], the screen at depth 1. */
typedef struct { float r[9], t[3]; } M34;

static M34 m34_mul(const M34 *a, const M34 *b)          /* a, then b */
{
    M34 o;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            o.r[3 * i + j] = a->r[3 * i] * b->r[j] + a->r[3 * i + 1] * b->r[3 + j] + a->r[3 * i + 2] * b->r[6 + j];
        o.t[i] = a->t[0] * b->r[i] + a->t[1] * b->r[3 + i] + a->t[2] * b->r[6 + i] + b->t[i];
    }
    return o;
}

static int in_2d(const Node *nd)                        /* below a <DD> */
{
    for (const Node *p = nd ? nd->parent : NULL; p; p = p->parent)
        if (p->kind == N_2D)
            return 1;
    return 0;
}

static M34 node_local3(const Node *nd)
{
    M34 l = { { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, { 0, 0, 0 } };
    int pix = in_2d(nd);
    if (nd->r3_own && !pix) {
        for (int i = 0; i < 9; i++)
            l.r[i] = nd->r3[i] * nd->m3;
        for (int i = 0; i < 3; i++)
            l.r[i] *= nd->sx;
    } else {
        Frame f = node_local(nd);
        l.r[0] = f.a; l.r[1] = f.b; l.r[3] = f.c; l.r[4] = f.d;
        l.r[8] = pix ? 1 : nd->m;
    }
    l.t[0] = nd->p[0]; l.t[1] = nd->p[1]; l.t[2] = pix ? 0 : nd->p[2];
    if (nd->kind == N_2D && !pix) {                      /* its pixels into the GUI's units */
        M34 dd = { { 1 / 640.0f, 0, 0, 0, 1 / 640.0f, 0, 0, 0, 1 / 640.0f }, { -0.5f, -0.375f, 0 } };
        l = m34_mul(&l, &dd);
    }
    return l;
}

static M34 node_world3(const Node *nd)
{
    M34 w = node_local3(nd);
    for (const Node *p = nd->parent; p && p->parent; p = p->parent) {
        M34 l = node_local3(p);
        w = m34_mul(&w, &l);
    }
    return w;
}

/* a shape's or an effect's draw matrix [R 0x240790: rows (r0, r2, -r1), the model's z up on screen] and the GUI to the
 * world: column-major, world = m (x, y, z, 1) */
static void gui_to_world(const M34 *g, float m[16])
{
    const float *r0 = g->r, *r1 = g->r + 3, *r2 = g->r + 6;
    float rows[3][3] = { { r0[0], r0[2], -r0[1] }, { r2[0], r2[2], -r2[1] }, { -r1[0], -r1[2], r1[1] } };
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            m[4 * i + j] = rows[i][j];
        m[4 * i + 3] = 0;
    }
    m[12] = g->t[0]; m[13] = g->t[2]; m[14] = -g->t[1]; m[15] = 1;
}

/* GuiObject.setIdentity (0x2573F0): the 3x3 is the identity, the place kept */
static void node_identity(Node *nd)
{
    if (!nd)
        return;
    memset(nd->r3, 0, sizeof nd->r3);
    nd->r3[0] = nd->r3[4] = nd->r3[8] = 1;
    nd->r3_own = 1;
    nd->m3 = 1;
}

/* GuiObject.setLookAt(p) (0x257868 -> 0x101330(a = p.z, b = p.x, c = p.y)): the 3x3 becomes Ry(p.y) Rx(p.x) Rz(p.z)
 * (a vector turned about y first), the place kept, no scale [R] */
static void node_look_at(Node *nd, const float p[3])
{
    if (!nd)
        return;
    float a = p[2], b = p[0], c = p[1];
    float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
    float r[9] = { ca * cc - sa * sb * sc, sa * cc + ca * sb * sc, -cb * sc,
                   -sa * cb, ca * cb, sb,
                   ca * sc + sa * sb * cc, sa * sc - ca * sb * cc, cb * cc };
    memcpy(nd->r3, r, sizeof r);
    nd->r3_own = 1;
    nd->m3 = 1;
}

/* GuiEffect.startEffect (0x237550): an idle one starts at its next update; stopEffect (0x237520): a pending or running
 * one stops, its particles play out; setEffect (0x2377E0): another edf, started unless it is manual [R] */
static void effect_start(Node *nd)
{
    if (nd && nd->fx == FX_EFFECT && !(nd->eflags & (FXE_START | FXE_RUN))) {
        nd->eflags |= FXE_START;
        nd->eflags &= ~FXE_STOP;
    }
}
static void effect_stop(Node *nd)
{
    if (nd && nd->fx == FX_EFFECT && (nd->eflags & (FXE_START | FXE_RUN))) {
        nd->eflags &= ~FXE_START;
        nd->eflags |= FXE_STOP;
    }
}
static void effect_set(Node *nd, const char *edf)
{
    if (!nd || nd->fx != FX_EFFECT)
        return;
    snprintf(nd->res, sizeof nd->res, "%s", edf ? edf : "");
    nd->eflags &= ~(unsigned)(FXE_RUN | FXE_START | FXE_STOP | FXE_SHUT);
    nd->drawn = 0;
    nd->gen++;
    if (nd->res[0] && !(nd->eflags & FXE_MANUAL))
        nd->eflags |= FXE_START;
}

/* the effects' update (0x237590): one to start that was drawn starts, one to stop stops emitting and runs until its
 * particles are gone (shell_effect_ended); it runs for hidden ones too (the update walk 0x2415E0 does not look at
 * visibility) [R] */
static void effects_update(Layout *L)
{
    for (int i = 0; L && i < L->n; i++) {
        Node *nd = &L->nodes[i];
        if (nd->fx != FX_EFFECT)
            continue;
        if ((nd->eflags & FXE_START) && nd->drawn > 0) {
            nd->eflags &= ~FXE_START;
            nd->eflags |= FXE_RUN;
        } else if (nd->eflags & FXE_STOP) {
            nd->eflags &= ~(unsigned)FXE_STOP;
            if (nd->eflags & FXE_RUN)
                nd->eflags |= FXE_SHUT;
        }
    }
}

/* object flag 0x400 (setFlags(1024) on "Press START", the caret, MonsterSheet's selection): the draw's alpha times the
 * GuiManager's factor, 1 - (0.2 + 0.2 sin phase), the phase 2 pi a second (0x242950): 0.6 .. 1 at 1 Hz [R] */
static float blink(const Gui *g) { return 0.8f - 0.2f * sinf(2.0f * SH_PI * g->time); }

/* a list box whose items all stand at one place (MainMenu's nb_selection: the human / computer counts) shows its
 * selected item alone [G: GuiListBox's layout is not traced] */
static int list_stacked(const Node *l)
{
    const Node *a = l ? l->child : NULL;
    if (!a || !a->next)
        return 0;
    for (const Node *c = a->next; c; c = c->next)
        if (c->p[0] != a->p[0] || c->p[1] != a->p[1])
            return 0;
    return 1;
}

static void layout_emit(Emit *e, const Gui *g, const Node *nd, const Frame *pf, int in2d, float fade, const Node *only)
{
    for (; nd; nd = nd->next) {
        if (!nd->visible || (nd->kind == N_FX && nd->fx != FX_SHAPE && nd->fx != FX_EFFECT) || (only && nd != only))
            continue;
        Frame local = node_local(nd), f;
        int c2d = in2d;
        if (!in2d && nd->kind == N_2D) {
            Frame pix = unit_to_pixels(pf);
            f = fr_mul(&local, &pix);
            c2d = 1;
        } else
            f = fr_mul(&local, pf);
        float nf = fade * nd->fade;
        if (nd->F & 0x400)
            nf *= blink(g);
        if (nd->kind == N_FX) {                          /* a shape or an effect: its 3D layer here, in tree order
                                                          * [R 0x241770: no sort; fade x z <= 0.01 is not drawn] */
            if (nf > 0.01f && nd->res[0]) {
                static const float white[4] = { 1, 1, 1, 1 };
                int at = e->n;
                if (!e->d3 || e->n3 < e->max3)
                    emit_quad(e, &f, 0, 0, 0, 0, 0, 0, 0, 0, white, NULL, HUD_TEX_3D);
                if (e->n > at && e->d3) {
                    e->d3[e->n3] = nd;
                    e->l3[e->n3++] = e->layout;
                }
            }
            layout_emit(e, g, nd->child, &f, c2d, nf, NULL);
            continue;
        }
        Frame df = in2d || nd->kind == N_2D ? f : unit_to_pixels(&f);   /* a 3D object's own picture [G] */
        if (nd->kind == N_QUAD) {
            float w = nd->has_d ? nd->d[0] : nd->has_tex ? nd->tex.w : 0;
            float h = nd->has_d ? nd->d[1] : nd->has_tex ? nd->tex.h : 0;
            float x0 = nd->Q & 2 ? -0.5f * w : 0, y0 = nd->Q & 4 ? -0.5f * h : 0;
            float c[4] = { nd->col[0], nd->col[1], nd->col[2], nd->col[3] * nf };
            /* M 0x200 (the 0x310 boxes): a mask, not drawn [G hud.c]; an untextured 0xFFFFFE quad without it (the
             * title boxes of MonsterSelect and MonsterSheet) is drawn, a faint white bar: neither the colour's handler
             * (0x22C650 -> 0x22B530) nor the draw (0x2439E0) tests that colour [R] */
            int drawn = !(nd->M & 0x200) && !((nd->F & 0x20) && !nd->has_tex);
            if (nd->video && g->video_pic && w > 0 && h > 0) {   /* Video.setVideo: the movie's picture */
                float wc[4] = { 1, 1, 1, c[3] };
                emit_quad(e, &df, x0, y0, x0 + w, y0 + h, 0, 0, 1, 1, wc, NULL, HUD_TEX_VIDEO);
            } else if (drawn && w > 0 && h > 0) {
                e->additive = (nd->M & 0x80) != 0;       /* CitySelect's `basic` glow over the globe */
                emit_quad(e, &df, x0, y0, x0 + w, y0 + h, 0, 0, nd->has_tex ? nd->tex.us : 1,
                          nd->has_tex ? nd->tex.vs : 1, c, nd->has_tex ? nd->tex.b : NULL,
                          nd->has_tex ? nd->tex.name : 0);
                e->additive = 0;
            }
        } else if (nd->kind == N_SLIDER) {
            float w = nd->has_d ? nd->d[0] : 100, h = nd->has_d ? nd->d[1] : 20;
            float fr = nd->vmax != nd->vmin ? (nd->val - nd->vmin) / (nd->vmax - nd->vmin) : 0;
            if (fr < 0) fr = 0;
            if (fr > 1) fr = 1;
            float xf = w * (fr - 0.5f);
            float bc[4] = { nd->bar_col[0], nd->bar_col[1], nd->bar_col[2], nd->bar_col[3] * nf };
            float mc[4] = { nd->mask_col[0], nd->mask_col[1], nd->mask_col[2], nd->mask_col[3] * nf };
            if (nd->mask_bar) {
                emit_quad(e, &df, -0.5f * w, -0.5f * h, 0.5f * w, 0.5f * h, 0, 0, 1, 1, bc, NULL, 0);
                if (fr < 1)
                    emit_quad(e, &df, xf, -0.5f * h, 0.5f * w, 0.5f * h, fr, 0, 1, 1, mc, NULL, 0);
            } else if (fr > 0)
                emit_quad(e, &df, -0.5f * w, -0.5f * h, xf, 0.5f * h, 0, 0, fr, 1, bc, NULL, 0);
        } else if (nd->kind == N_TEXT && nd->font) {
            const Font *fo = nd->font;
            float c[4], s[4];
            if (nd->has_tcol) memcpy(c, nd->tcol, sizeof c);
            else col_abgr(fo->col, c);
            if (nd->has_shadow) memcpy(s, nd->shadow, sizeof s);
            else col_abgr(fo->shadow_col, s);
            c[3] *= nf;
            s[3] *= nf;
            TextStyle ts;
            text_style(nd, &ts);
            Frame tf = { nd->tscale, 0, 0, nd->tscale, 0, 0 };   /* the text's own scale [R GuiText 0x234D88] */
            tf = fr_mul(&tf, &df);
            text_emit(e, g, &ts, nd->text, &tf, c, s, nd->has_shadow ? nd->sh_dx : fo->shadow_dx,
                      nd->has_shadow ? nd->sh_dy : fo->shadow_dy);
        } else if (nd->kind == N_LINE && nd->has_ep) {   /* [R 0x23A8E0] a band of +- width / 2, in its frame */
            float c[4] = { nd->col[0], nd->col[1], nd->col[2], nd->col[3] * nf }, hw = 0.5f * nd->lw;
            float x0 = fminf(nd->ep[0], nd->ep[2]), x1 = fmaxf(nd->ep[0], nd->ep[2]);
            float y0 = fminf(nd->ep[1], nd->ep[3]), y1 = fmaxf(nd->ep[1], nd->ep[3]);
            if (x1 - x0 >= y1 - y0) { y0 -= hw; y1 += hw; } else { x0 -= hw; x1 += hw; }
            emit_quad(e, &df, x0, y0, x1, y1, 0, 0, 1, 1, c, NULL, 0);
        } else if (nd->kind == N_RECT)
            rect_emit(e, nd, &df, nf);
        else if (nd->kind == N_LIST)
            list_emit(e, g, nd, &df, nf);
        const Node *one = nd->kind == N_LIST && list_stacked(nd) ? list_item((Node *)nd, nd->sel < 0 ? 0 : nd->sel) : NULL;
        layout_emit(e, g, nd->child, &f, c2d, nf, one);
    }
}

/* one interface's quads, in the layout's order [G: no depth sort here: the shell's <z> would put MonsterSelect's
 * frost border (0.58) under its background and MonsterSheet's "P2" stamps (0.769) under their cards if a larger z
 * were drawn first, as hud.c takes it] */
static void emit_layout(Emit *e, const Gui *g, const Layout *L)
{
    e->layout = L;
    if (L && L->root && L->root->visible)
        layout_emit(e, g, L->root->child, &FR_ID, 0, L->root->fade, NULL);
}

/* ---------------------------------------------------------------- ObjectFader.h [R] */
typedef struct { Node *obj[MAX_FADERS]; float dir[MAX_FADERS]; int n; } Faders;

static int fader_find(Faders *fd, Node *obj)
{
    for (int i = 0; i < fd->n; i++)
        if (fd->obj[i] == obj)
            return i;
    return -1;
}
static void fader_init(Faders *fd, Node *obj, int on)   /* InitObjectFader */
{
    if (!obj)
        return;
    int i = fader_find(fd, obj);
    if (i < 0) {
        if (fd->n >= MAX_FADERS)
            return;
        i = fd->n++;
        fd->obj[i] = obj;
    }
    fd->dir[i] = 0;
    obj->visible = on;
    obj->fade = on ? 1.0f : 0.0f;
}
static void fader_go(Faders *fd, Node *obj, float dir)  /* ObjectFaderShow (1) / ObjectFaderHide (-1) */
{
    int i = obj ? fader_find(fd, obj) : -1;
    if (i >= 0)
        fd->dir[i] = dir;
}
static void fader_update(Faders *fd, float dt)          /* UpdateObjectFader: 2 a second, dt at most 0.1 */
{
    if (dt > 0.1f)
        dt = 0.1f;
    for (int i = 0; i < fd->n; i++) {
        if (fd->dir[i] == 0)
            continue;
        Node *o = fd->obj[i];
        float f = o->fade + fd->dir[i] * 2.0f * dt;
        if (f < 0) { f = 0; fd->dir[i] = 0; }
        else if (f > 1) { f = 1; fd->dir[i] = 0; }
        o->fade = f;
        o->visible = f != 0;
    }
}

/* ---------------------------------------------------------------- the shell's state */
#define TICK (1.0f / 60.0f)                              /* a script frame [G: 60 a second] */
#define IDLE_SECONDS 120.0f                              /* [R 0x308710: 120.0 (0x42F0) into the shell's +0x18] */

/* the GuiEvent ids [R the scripts] */
enum { EV_RIGHT = 2, EV_LEFT = 4, EV_UP = 8, EV_DOWN = 16, EV_BACK = 32, EV_ACCEPT = 64, EV_START = 128,
       EV_BUTTON = 256, EV_GAIN = 512, EV_LOSE = 1024, EV_FRAME = 2048, EV_INIT = 8192 };

/* the ControlRoom's interfaces, in its layout's order [R ControlRoom.ifc's <X>] */
enum { IF_ATTRACT, IF_MAINMENU, IF_MONSTERSELECT, IF_MONSTERSHEET, IF_CITYSELECT, IF_OPTIONS, IF_COUNT };
static const char *const IFC_NAMES[IF_COUNT] = { "AttractMode", "MainMenu", "MonsterSelect", "MonsterSheet",
                                                  "CitySelect", "Options" };

/* AttractMode.scr's states [R attractModeStates] */
enum { AS_BOOT_CHECK, AS_SHORT_DELAY, AS_MIDWAY, AS_PIPEWORKS, AS_DIGITAL, AS_BINK, AS_INTRO, AS_RANDOM_GAME,
       AS_POST_RANDOM, AS_CHEAT_VIDEO, AS_COUNT };

typedef struct {
    Node *video, *still, *press;
    float still_time, elapsed;
    int cheat_video, state, delay;
    int movie_on;                                        /* a movie "plays" (Video.status 1 or 2) */
    float movie_left;
} Attract;

typedef struct {
    Node *icons, *numplayers, *numbots, *retro, *cheat, *playtype, *lb_players, *lb_bots;
    Node *qicons[4], *retro_rects[3], *cheat_nums[6];
    int state, play_mode, retro_game, start_retro, num_players, num_bots, ntsc;
    int code[6], code_index;
    float glow_time, retro_delay;
    Faders fd;
} MainMenu;

typedef struct {
    int selection;                                       /* an index into monsters[] */
    Node *base;                                          /* "Player<n>" */
    int spin;
    float spin_amount, scale_base;
    int monsters[SHELL_MONSTERS], nmon;
    int ready;
} MsPlayer;
typedef struct {
    Node *player_base, *p1_dialog;
    MsPlayer pl[4];
    int initialized, sheet_controller, num_players, num_bots, show_p1;
    float hl_time;
    Faders fd;
} MonsterSel;

typedef struct {
    Node *offset, *selrect, *arrow_l, *arrow_r, *cards, *blowup, *arrows;
    int initialized, controller, map_controller, state, sub, row, col, valid, in_use[3], sound_played;
    int selection, spin;
    float right_bounds, offset_x, target, prev_target, left_edge, right_edge, zoom, spin_amount, scale_base;
} MonsterSheet;

typedef struct {
    Node *city_bar, *city_rect, *arrow_l, *arrow_r, *time_text, *stats, *select_btn, *stats_btn, *news;
    Node *globe, *rings[SHELL_CITIES];                   /* guiGlobe, cityRings (fx_<city>) */
    int initialized, index, left, locked, night, jumped, stats_visible, flash_city, cheat_block, ring_on;
    float flash, look[3], target[3], dir[3];             /* lookAt, lookAtTarget, moveDir (radians) */
    char msg_key[48];
    Faders fd;
} CitySel;

typedef struct {
    Node *group, *sound, *music, *rumble, *titles[3];
    int initialized, mode, exiting, rumble_on, tweak_sound;
    float f_music, f_sound, pulse, tick;
} Options;

/* the SaveGame popup's panels [R 0x341880: +0x2D0 1 the profile list, 2 the name entry, 3 the messages] */
enum { MC_OFF, MC_LIST, MC_KEYBOARD, MC_MESSAGE };
enum { MSG_AUTOSAVE, MSG_WARN_CONTINUE, MSG_DUPLICATE, MSG_OVERWRITE };
enum { MCP_BOOT, MCP_LOAD, MCP_BUDDY };
typedef struct {
    Layout *L;
    int mode, purpose, msg, buddy;
    int had;                                             /* flag 0x20: flag 8 and a name held when it opened */
    Node *profiles, *keyboard, *messages;
    /* the messages (0x3401E0) */
    Node *message_rect, *autosave_rect, *nodata_rect, *title, *text, *btn_select, *btn_cancel, *btn_button0;
    /* the profile list (+0x2D8) */
    Node *list, *bar, *pl_name, *pl_id, *arrow_up, *arrow_down;
    int nentries;
    /* the name entry (+0x304, 0x340980) */
    Node *kb_name, *kb_id, *caret, *rows[4], *key;
    int row, col, len;
    float col_normal[4], col_selected[4];
    char name[16];
    int boot_done;                                       /* gShell.isBootCheckDone */
} MemCard;

typedef struct {
    Layout *L;
    Bundle b;
    int have_b, on;
    Node *image, *text, *fill, *dots, *progress;
    int intro, no_intro, dots_n, started;
    float t, dot_t;
    float start[3], end[3], s0, s1, rot;
} Loading;

typedef struct {
    Layout *L;
    Node *video;
    int on, video_n, movie_on;
    float movie_left;
} Victory;

struct Shell {
    const Vol *vol;
    Bundle b_shell, b_vshell, b_data, b_common, b_mc, b_loading;
    int have_shell, have_vshell, have_data, have_common, have_mc, have_loading;
    Gui gui;
    Book book;
    Group monster_data, city_data;
    ShellProfile prof;
    ShellGame gs;                                        /* gCommon.getGameStatus() and the players / bots */
    ShellStart st;
    /* gShell (0x4D51F0) */
    int state, attract_state, boot_completed, show_profile;
    float idle, movie_time;
    float movie_clock;                                   /* seconds since the playing movie started */
    /* gCommon */
    int choose_monster, city_destroyed, quitting, ctrl_mask, platform, region;
    uint32_t cheats;
    int cheat_param_block, cheat_param_video;
    /* pad 0 held */
    float held_x;
    int held_buttons;
    /* the ControlRoom */
    Layout *ifc[IF_COUNT];
    Faders cr_fd;
    int focus, cr_on;
    Attract am;
    MainMenu mm;
    MonsterSel ms;
    MonsterSheet sh;
    CitySel cs;
    Options op;
    MemCard mc;
    Loading ld;
    Victory vi;
    ShellEvent ev[MAX_EVENTS];
    int nev;
    ShellQuad *quads;
};

static void event(Shell *s, int kind, const char *name, int arg, float value)
{
    if (s->nev >= MAX_EVENTS)
        return;
    ShellEvent *e = &s->ev[s->nev++];
    e->kind = kind;
    e->arg = arg;
    e->value = value;
    snprintf(e->name, sizeof e->name, "%s", name ? name : "");
}
static void sound(Shell *s, const char *name) { event(s, SHELL_EVT_SOUND, name, 0, 0); }
static void music(Shell *s, const char *name) { event(s, SHELL_EVT_MUSIC, name, 0, 0.5f); }

static Node *ifc_root(Shell *s, int i) { return i >= 0 && i < IF_COUNT && s->ifc[i] ? s->ifc[i]->root : NULL; }

/* ---------------------------------------------------------------- ShellData: gShell.getStringProp & co [R 0x306530:
 * "Group.Sub.Key" through the book loaded by loadPropertyBook("ShellData")] */
static Group sub_group(Group g, const char *name)
{
    /* typed keys "Monster|Monster0": matched by the part after the bar */
    Group none = { g.book, 0 };
    for (uint32_t i = 0, n = group_count(g); i < n; i++) {
        Record r = group_record(g, i);
        const char *bar = strrchr(r.name, '|');
        if ((r.kind & 0xFF) == PK_GROUP && _stricmp(bar ? bar + 1 : r.name, name) == 0)
            return record_group(g, r);
    }
    return none;
}
static Group monster_group(const Shell *s, int i)
{
    char name[24];
    snprintf(name, sizeof name, "Monster%d", i);
    return sub_group(s->monster_data, name);
}
static Group city_group(const Shell *s, int i)
{
    char name[24];
    snprintf(name, sizeof name, "City%d", i);
    return sub_group(s->city_data, name);
}
/* getMonsterName(id): MonsterData.Monster<id>.name ("George"); "" when absent */
static const char *monster_name(const Shell *s, int i)
{
    const char *n = group_str(monster_group(s, i), "Name");
    return n ? n : "";
}
static float monster_prop(const Shell *s, int i, const char *key, float def)
{
    return group_num(monster_group(s, i), key, def);
}
/* CityData.City<i>.City ("LasVegas") */
static const char *city_name(const Shell *s, int i)
{
    const char *n = group_str(city_group(s, i), "City");
    return n ? n : "";
}

/* gCommon.getMonsterFlags(player, monster) [R 0x33D760: 0 outside 0 .. 30; a buddy's for players 1 .. 3] */
static int monster_flags(const Shell *s, int player, int monster)
{
    if (monster < 0 || monster >= SHELL_MONSTERS)
        return 0;
    if (player < 0 || player > 3)
        player = 0;
    return s->prof.monster_flags[player][monster];
}

/* bs_fixButton on the PS2 (gApp.getPlatform() 2) [R ButtonSetup.h] */
static void fix_button(Node *in, const char *name, int index)
{
    static const float ps2[4][3] = { { 0, 186, 255 }, { 190, 45, 39 }, { 220, 108, 194 }, { 0, 240, 156 } };
    Node *t = name && *name ? find(in, name) : NULL;
    if (t && index >= 0 && index < 4) {
        float c[4];
        col_255(c, ps2[index][0], ps2[index][1], ps2[index][2], 255);
        set_col(t, c);
    }
}
static void fix_buttons(Node *in, const char *sel, const char *cancel, const char *b0, const char *b1)
{
    fix_button(in, sel, 0);
    fix_button(in, cancel, 1);
    fix_button(in, b0, 2);
    fix_button(in, b1, 3);
}

/* gShell.setupControllerTable(mask) [R 0x306AD8: the pads below max(1, players - bots) that the mask names talk to
 * the GUI; G: the details of 0x16AC70 / 0x16AA20] */
static void controller_table(Shell *s, int mask) { s->ctrl_mask = mask; }
static int pad_allowed(const Shell *s, int pad)
{
    int n = s->gs.num_players - s->gs.num_bots;
    if (n < 1) n = 1;
    return pad >= 0 && pad < 4 && (pad < n || pad == 0) && (s->ctrl_mask & (1 << pad));
}

static void set_cheats(Shell *s, uint32_t flags)
{
    s->cheats = flags;
    event(s, SHELL_EVT_CHEATS, NULL, (int)flags, (float)s->cheat_param_block);
}

static void cr_show(Shell *s, int which);
static void cr_start_game(Shell *s);
static void ifc_event(Shell *s, int which, int ev, int button, int pad, float dt);
static void ms_setup_from_main_menu(Shell *s);
static void ms_monster_from_sheet(Shell *s, const char *name);
static int ms_player_selection(Shell *s, int player);
static void mm_update_faders(Shell *s, float dt);

/* ---------------------------------------------------------------- the SaveGame popup (MemoryCard.ifc) */
/* the message panel [R 0x3401E0 finds them; 0x33FD10 fills title, text and the buttons' texts] */
static void mc_message(Shell *s, int msg)
{
    MemCard *m = &s->mc;
    m->mode = MC_MESSAGE;
    m->msg = msg;
    show(m->profiles, 0);
    show(m->keyboard, 0);
    show(m->messages, 1);
    show(m->autosave_rect, msg == MSG_AUTOSAVE);
    show(m->nodata_rect, 0);
    show(m->message_rect, msg != MSG_AUTOSAVE);
    /* 0x33FD10(title, text, .., btn_select's, btn_cancel's, btn_button0's text) [R 0x3401E0: +0x2C / +0x30 / +0x34] */
    const char *title = NULL, *text = NULL, *sel = NULL, *can = NULL;
    if (msg == MSG_WARN_CONTINUE) {
        /* 0x3439E8: a name held, "Autosave Turned Off" with Don't Save; none, "WARNING: No Profile" with Continue.
         * Back on Cross, the other on Triangle [R] */
        int named = s->prof.loaded;
        title = named ? "mcWarnContinueTitle" : "mcWarnContinueTitle (NoProfile)";
        text = named ? "mcWarnContinue" : "mcWarnContinue (NoProfile)";
        sel = "MC_BUTTON_BACK";
        can = named ? "MC_BUTTON_DONTSAVE" : "MC_BUTTON_CONTINUE";
    } else if (msg == MSG_DUPLICATE) {                   /* error 24 for the list's request [R 0x3423B8] */
        title = "mcErrorTitle";
        text = "mcDuplicate";
        sel = "MC_BUTTON_CANCEL";
        can = "MC_BUTTON_OVERWRITE";
    } else if (msg == MSG_OVERWRITE) {                   /* state 11 [R 0x343868] */
        title = "mcConfirmOverwriteTitle";
        text = "mcConfirmOverwrite";
        sel = "MC_BUTTON_NO";
        can = "MC_BUTTON_YES";
    }
    if (title) {
        node_localize(&s->gui, m->title, title);
        node_localize(&s->gui, m->text, text);
        node_localize(&s->gui, m->btn_select, sel);
        node_localize(&s->gui, m->btn_cancel, can);
        show(m->btn_select, 1);
        show(m->btn_cancel, 1);
        show(m->btn_button0, 0);
    }
}

/* the profile list's selection shown (0x340B40): the bar under the selected item, its text as the name, the arrows */
static void mc_list_update(Shell *s)
{
    MemCard *m = &s->mc;
    Node *it = list_item(m->list, m->list ? m->list->sel : -1);
    if (m->bar && it)
        m->bar->p[1] = it->p[1] + 2.0f;                  /* [R 0x340C24: the item's y + 2] */
    if (m->pl_name)
        set_text(m->pl_name, it ? it->text : "");
    show(m->bar, 1);
    int top = m->list ? m->list->top : 0;
    show(m->arrow_up, top != 0);
    show(m->arrow_down, top < m->nentries - 6);
}

/* gCommon.loadProfile() -> SaveGame 0x345700 with "create allowed" (flag 0x10): the list, "Create New Profile" first,
 * then the profiles found (0x341250: item 0 kept, the others shown and named, copies of item 0 for more) */
static void mc_open_list(Shell *s, int purpose, int buddy)
{
    MemCard *m = &s->mc;
    if (!m->L)
        return;
    m->purpose = purpose;
    m->buddy = buddy;
    m->mode = MC_LIST;
    show(m->profiles, 1);
    show(m->keyboard, 0);
    show(m->messages, 0);
    m->nentries = s->prof.nprofiles;
    int n = list_count(m->list);
    for (int i = 1; i < n; i++)
        show(list_item(m->list, i), 0);
    for (int i = 0; i < m->nentries && i + 1 < n; i++) {
        Node *it = list_item(m->list, i + 1);
        show(it, 1);
        set_text(it, s->prof.profiles[i]);
        show(find(it, "loc"), 0);
    }
    if (m->pl_id)
        set_text(m->pl_id, buddy > 0 ? (buddy == 1 ? "P2:" : buddy == 2 ? "P3:" : "P4:") : "P1:");
    list_select(m->list, m->nentries > 0 ? 1 : 0);       /* [R 0x3415EC: 0x23E480(list, count > 0)] the first profile */
    if (m->list) m->list->top = 0;
    mc_list_update(s);
}

/* the name entry's start (0x340980): the keyboard shown, the colours from color_normal / color_selected, the caret
 * blinking (flag 0x400), the first key */
static void mc_open_keyboard(Shell *s)
{
    MemCard *m = &s->mc;
    m->mode = MC_KEYBOARD;
    show(m->profiles, 0);
    show(m->messages, 0);
    show(m->keyboard, 1);
    if (m->keyboard) m->keyboard->fade = 1;
    m->row = m->col = 0;
    m->len = 0;
    m->name[0] = 0;
    set_text(m->kb_name, "");
    if (m->kb_id)
        set_text(m->kb_id, m->pl_id ? m->pl_id->text : "P1:");
    if (m->caret) {
        m->caret->F |= 0x400;
        m->caret->p[0] = 0;
    }
    for (int r = 0; r < 4; r++)
        for (Node *k = m->rows[r] ? m->rows[r]->child : NULL; k; k = k->next)
            set_col(k, m->col_normal);
    m->key = child_at(m->rows[0], 0);
    set_col(m->key, m->col_selected);
}

/* close(result) [R 0x345540]: a result other than 3 (the list's Back with nothing to warn of, the attract mode's
 * interrupt) sets 0x300 (show-profile off, boot done), or only 0x100 for the boot's check (op 12): the autosave
 * notice. Nothing sets show-profile again */
static void mc_close(Shell *s, int result)
{
    if (result != 3 && !(s->mc.mode == MC_MESSAGE && s->mc.msg == MSG_AUTOSAVE))
        s->show_profile = 0;
    s->mc.mode = MC_OFF;
    if (s->mc.L)
        show(s->mc.L->root, 0);
}

static void mc_finish_buddy(Shell *s, int player)
{
    controller_table(s, -1);
    (void)player;
}

/* the name changed: the text and the caret after it [R 0x340780: setText, the caret at the text's width] */
static void mc_name_changed(Shell *s)
{
    MemCard *m = &s->mc;
    set_text(m->kb_name, m->name);
    if (m->caret)
        m->caret->p[0] = text_w(&s->gui, m->kb_name);
}

/* the name written as a new profile (state 5's write; close(1) in op 5: flags 4 and 8 stay as they are) */
static void mc_create(Shell *s)
{
    MemCard *m = &s->mc;
    event(s, SHELL_EVT_PROFILE_CREATE, m->name, m->purpose == MCP_BUDDY ? m->buddy : 0, 0);
    mc_close(s, 1);
}

/* DONE with a name [R 0x340700: trailing spaces cut, an empty name refused with "Cancel"] */
static void mc_done(Shell *s)
{
    MemCard *m = &s->mc;
    while (m->len > 0 && m->name[m->len - 1] == ' ')
        m->name[--m->len] = 0;
    mc_name_changed(s);
    if (!m->name[0]) {
        sound(s, "Cancel");
        return;
    }
    sound(s, "Click");
    /* [R] DONE does not look: state 5 opens the file by its name (the directory is crc32 of the name, so the case
     * counts) and finds it there: error 24, "The Profile name already exists." */
    for (int i = 0; i < s->prof.nprofiles; i++)
        if (strcmp(s->prof.profiles[i], m->name) == 0) {
            mc_message(s, MSG_DUPLICATE);
            return;
        }
    mc_create(s);
}

static void mc_key_add(Shell *s, char ch)
{
    MemCard *m = &s->mc;
    if (ch == ' ' && m->len == 0) {                      /* [R 0x3408A0: no leading space] */
        sound(s, "Cancel");
        return;
    }
    if (m->len < 15) {                                   /* [R 0x3408D0: 15 characters] */
        m->name[m->len++] = ch;
        m->name[m->len] = 0;
    }
    mc_name_changed(s);
    sound(s, "Click");
}

/* the name entry's pad (0x340320) [R] */
static void mc_keyboard_input(Shell *s, int ev)
{
    MemCard *m = &s->mc;
    if (ev == EV_LEFT || ev == EV_RIGHT) {
        int n = child_count(m->rows[m->row]);
        m->col += ev == EV_RIGHT ? 1 : -1;
        if (m->col == n) m->col = 0;
        else if (m->col < 0) m->col = n - 1;
    } else if (ev == EV_UP || ev == EV_DOWN) {
        /* the next row (round) that has a key in this column */
        for (int k = 1; k < 4; k++) {
            int r = ((m->row + (ev == EV_DOWN ? k : 4 - k)) % 4 + 4) % 4;
            if (m->col < child_count(m->rows[r])) {
                m->row = r;
                break;
            }
        }
    } else if (ev == EV_ACCEPT) {
        Node *key = m->key;
        if (!key)
            return;
        if (strcmp(key->name, "DONE") == 0)
            mc_done(s);
        else if (strcmp(key->name, "BACKSPACE") == 0) {
            if (m->len > 0)
                m->name[--m->len] = 0;
            mc_name_changed(s);
            sound(s, "Click");
        } else if (strcmp(key->name, "SPACE") == 0)
            mc_key_add(s, ' ');
        else
            mc_key_add(s, key->text[0]);                 /* the key's own text */
        return;
    } else if (ev == EV_BACK) {
        sound(s, "Cancel");                              /* [R: panel 1, result 0xE (0x340938); state 4 -> 3, the
                                                          * list built again (0x343FE8, 0x343BD0)] */
        mc_open_list(s, m->purpose, m->buddy);
        return;
    } else
        return;
    int n = child_count(m->rows[m->row]);
    if (n > 0)
        m->col %= n;
    set_col(m->key, m->col_normal);
    m->key = child_at(m->rows[m->row], m->col);
    set_col(m->key, m->col_selected);
    sound(s, "Rollover");
}

/* the profile list's pad (0x3410E0) [R] */
static void mc_list_input(Shell *s, int ev)
{
    MemCard *m = &s->mc;
    if (ev == EV_UP || ev == EV_DOWN) {
        int prev = m->list ? m->list->sel : 0;
        list_increment(m->list, ev == EV_UP ? -1 : 1);
        if (m->list && m->list->sel != prev)
            sound(s, "Rollover");
        mc_list_update(s);
    } else if (ev == EV_ACCEPT) {
        sound(s, "Click");
        int sel = m->list ? m->list->sel : 0;
        if (sel <= 0)
            mc_open_keyboard(s);                         /* state 7: create */
        else {                                           /* state 8: load */
            event(s, m->purpose == MCP_BUDDY ? SHELL_EVT_BUDDY_PROFILE : SHELL_EVT_PROFILE_LOAD,
                  s->prof.profiles[sel - 1], m->purpose == MCP_BUDDY ? m->buddy : sel - 1, (float)(sel - 1));
            mc_close(s, 1);                              /* (op 3: flags 4 and 8 set, the viewer's) */
        }
    } else if (ev == EV_BACK) {
        sound(s, "Cancel");                              /* state 9 */
        if (m->purpose == MCP_BUDDY) {
            mc_close(s, 2);                              /* [G: the result] */
            mc_finish_buddy(s, -1);
        } else if (m->had || !s->prof.allow_save)
            mc_close(s, 3);                              /* [R 0x3439E8: 0x20 set or 4 clear, nothing to warn of] */
        else
            mc_message(s, MSG_WARN_CONTINUE);            /* [R the list's Back, 0x343DF8: state 2] */
    }
}

/* the messages' pad (0x33FBB0) [R: Accept while the autosave notice shows or btn_select shows: 0x12; Back with
 * btn_cancel: 0x13; button 0 with btn_button0: 0x14] */
static void mc_message_input(Shell *s, int ev, int button)
{
    MemCard *m = &s->mc;
    int ok = ev == EV_ACCEPT && (m->msg == MSG_AUTOSAVE || (m->btn_select && m->btn_select->visible));
    int back = ev == EV_BACK && m->btn_cancel && m->btn_cancel->visible && m->msg != MSG_AUTOSAVE;
    (void)button;
    if (!ok && !back)
        return;
    sound(s, "Click");
    if (m->msg == MSG_AUTOSAVE) {
        m->boot_done = 1;
        mc_close(s, 1);
    } else if (m->msg == MSG_WARN_CONTINUE) {
        if (back) {                                      /* [R 0x343B00] Continue / Don't Save: flags 4 and 8 off */
            s->prof.allow_save = s->prof.was_saving = 0;
            event(s, SHELL_EVT_PROFILE_NONE, NULL, 0, 0);
            mc_close(s, 2);
        } else
            mc_open_list(s, m->purpose, m->buddy);       /* Back: the state it came from, the list */
    } else if (m->msg == MSG_DUPLICATE) {
        if (back)
            mc_message(s, MSG_OVERWRITE);                /* Overwrite: state 11 */
        else
            mc_open_list(s, m->purpose, m->buddy);       /* Cancel: state 3, the name lost */
    } else if (m->msg == MSG_OVERWRITE) {
        if (back)                                        /* YES: op 9, state 5 writes a new profile into the file
                                                          * (the list's hook 0x33F0F0, 0x344444) */
            mc_create(s);
        else
            mc_open_list(s, m->purpose, m->buddy);       /* NO: state 3 */
    }
}

static void mc_input(Shell *s, int ev, int button)
{
    if (s->mc.mode == MC_LIST) mc_list_input(s, ev);
    else if (s->mc.mode == MC_KEYBOARD) mc_keyboard_input(s, ev);
    else if (s->mc.mode == MC_MESSAGE) mc_message_input(s, ev, button);
}

/* SaveGame's interface (0x345D60 loads MemoryCard.ifc of MemoryCard.bdp) */
static void mc_load(Shell *s)
{
    MemCard *m = &s->mc;
    if (!s->have_mc)
        return;
    m->L = layout_load(&s->gui, &s->b_mc, "MemoryCard.ifc", 32);
    if (!m->L)
        return;
    Node *r = m->L->root;
    m->profiles = find(r, "profiles");
    m->keyboard = find(r, "keyboard");
    m->messages = find(r, "messages");
    m->message_rect = find(m->messages, "message_rect");
    m->autosave_rect = find(m->messages, "autosave_rect");
    m->nodata_rect = find(m->messages, "nodata_rect");
    m->title = find(m->message_rect, "message_title");
    m->text = find(m->message_rect, "message_text");
    m->btn_select = find(m->message_rect, "btn_select");
    m->btn_cancel = find(m->message_rect, "btn_cancel");
    m->btn_button0 = find(m->message_rect, "btn_button0");
    m->list = find(m->profiles, "profile_list");
    m->bar = find(m->profiles, "profile_bar");
    m->pl_name = find(m->profiles, "profile_name");
    m->pl_id = find(m->profiles, "profile_id");
    m->arrow_up = find(m->profiles, "profile_arrow_up");
    m->arrow_down = find(m->profiles, "profile_arrow_down");
    m->kb_name = find(m->keyboard, "profile_name");
    m->kb_id = find(m->keyboard, "profile_id");
    m->caret = find(m->keyboard, "caret");
    for (int i = 0; i < 4; i++)
        m->rows[i] = findf(m->keyboard, "row%d", i);
    Node *cn = find(r, "color_normal"), *cs = find(r, "color_selected");
    col_set(m->col_normal, 1, 1, 1, 1);
    col_set(m->col_selected, 1, 1, 0, 1);
    if (cn && cn->has_tcol) memcpy(m->col_normal, cn->tcol, sizeof m->col_normal);
    if (cs && cs->has_tcol) memcpy(m->col_selected, cs->tcol, sizeof m->col_selected);
    fix_buttons(m->profiles, "btn_select", "btn_cancel", "", "");
    fix_buttons(m->keyboard, "btn_select", "btn_cancel", "", "");
    fix_buttons(m->messages, "btn_select", "btn_cancel", "btn_button0", "");
    show(r, 0);
}
static void mc_show(Shell *s) { if (s->mc.L) show(s->mc.L->root, 1); }

/* gCommon.loadProfile() */
static void load_profile(Shell *s)
{
    if (!s->mc.L)
        return;
    mc_show(s);
    s->mc.had = s->prof.was_saving && s->prof.loaded;   /* [R 0x345760: flag 8 and a name held: 0x20] */
    mc_open_list(s, MCP_LOAD, 0);
}

/* gShell.doBootCheck() [R 0x306300: once, in shell state 1, SaveGame's boot check 0x345BC0]: here the autosave notice
 * (mcWarnAutosave) the PS2 build shows at the boot [G: the memory card's own checks are not there to make] */
static void boot_check(Shell *s)
{
    if (s->boot_completed || s->state != 1 || !s->mc.L) {
        s->mc.boot_done = 1;
        return;
    }
    mc_show(s);
    mc_message(s, MSG_AUTOSAVE);
}

/* ---------------------------------------------------------------- the loading screen (0x4D8250, Loading.scr) */
/* gShell.showLoading() (0x3485E0 with 0): Loading.ifc of loading_<city>.BDP, or loading_shell.ifc of loading.BDP
 * when that is not there; the music fades out over 0.5 s [R] */
static void show_loading(Shell *s, int shell)
{
    Loading *l = &s->ld;
    if (l->on)
        return;
    event(s, SHELL_EVT_MUSIC_VOLUME, NULL, 500, 0);
    char name[64];
    snprintf(name, sizeof name, "Loading_%s.bdp", s->gs.city);
    uint32_t size = 0;
    uint8_t *data = shell ? NULL : vol_read(s->vol, name, &size);
    l->have_b = data && bdp_load(&l->b, data, size) == 0;
    if (!l->have_b) free(data);
    if (l->have_b && s->gui.nb < MAX_BUNDLES)
        s->gui.bundles[s->gui.nb++] = &l->b;
    l->L = l->have_b ? layout_load(&s->gui, &l->b, "loading.ifc", 0) : NULL;
    if (!l->L && s->have_loading)
        l->L = layout_load(&s->gui, &s->b_loading, "loading_shell.ifc", 0);
    l->on = 1;
    l->t = 0;
    l->intro = 0;
    l->started = 0;
    l->dots_n = 0;
    l->dot_t = 0;
    if (!l->L)
        return;
    Node *r = l->L->root;
    /* Loading::doInterface 8192 [R] */
    l->fill = find(r, "loading_fill");
    l->image = find(r, "loading_image");
    l->text = find(r, "loading_text");
    l->dots = find(r, "DotsText");
    l->progress = find(r, "ProgressText");
    l->no_intro = find(r, "bNoIntro") != NULL;
    set_fade(l->fill, 0);
    set_fade(l->text, 1);
    set_fade(l->image, 1);
    set_pos(l->image, 320, 240, 0);
    if (l->image) l->image->m = 2;
    node_yaw(l->image, l->no_intro ? 0.0f : -SH_PI * 4.15f);
    /* 512 (the focus) -> setupUI */
    l->intro = 1;
    set_fade(l->fill, 0);
    set_fade(l->text, 0);
    l->start[0] = 610; l->start[1] = 17; l->start[2] = 0;
    l->end[0] = 320; l->end[1] = 240; l->end[2] = 0;
    l->s0 = 0.1f;
    l->s1 = 2.0f;
    l->rot = -SH_PI * 4.15f;
    set_pos(l->image, l->start[0], l->start[1], 0);
    if (l->image) l->image->m = l->s0;
    if (l->no_intro) {
        l->intro = 3;
        set_pos(l->image, l->end[0], l->end[1], 0);
        if (l->image) l->image->m = l->s1;
        node_yaw(l->image, 0);
        set_fade(l->image, 1);
        set_fade(l->text, 1);
    }
}

static void loading_update(Shell *s, float dt)
{
    Loading *l = &s->ld;
    if (!l->on)
        return;
    l->t += dt;
    if (l->intro == 1) {
        float ct = l->t;
        if (ct >= 0.5f) {
            ct = 0.5f;
            l->intro = 2;
            l->t = 0;
        }
        float k = ct / 0.5f;
        set_pos(l->image, l->start[0] + (l->end[0] - l->start[0]) * k, l->start[1] + (l->end[1] - l->start[1]) * k, 0);
        if (l->image) l->image->m = l->s0 + (l->s1 - l->s0) * k;
        node_yaw(l->image, l->rot * k);
        set_fade(l->fill, k * 2.0f);
    } else if (l->intro == 2) {
        float f = l->t / 0.1f;
        if (f >= 1.0f) {
            f = 1.0f;
            l->intro = 3;
        }
        set_fade(l->text, f);
    } else if (!l->started) {                            /* gShell.startGame(): the shell's state 4 */
        l->started = 1;
        s->state = 4;
        event(s, SHELL_EVT_START, s->gs.city, s->gs.city_index, 0);
    }
    /* the dots: one more every 0.25 s, 0 .. 6 [R 0x348B80] */
    l->dot_t += dt;
    while (l->dot_t > 0.25f) {
        l->dot_t -= 0.25f;
        l->dots_n = (l->dots_n + 1) % 7;
    }
    char d[8];
    int n = 0;
    for (; n < l->dots_n; n++) d[n] = '.';
    d[n] = 0;
    set_text(l->dots, d);
}

/* ---------------------------------------------------------------- Victory (VShell.bdp, Victory.scr) */
static void victory_next(Shell *s)
{
    Victory *v = &s->vi;
    v->video_n++;
    if (v->video_n == 1 || v->video_n == 2) {
        event(s, SHELL_EVT_MOVIE, v->video_n == 1 ? "Victory" : "Credits", 0, 0);   /* setVictoryShown on the first */
        v->movie_on = 1;
        s->movie_clock = 0;
        v->movie_left = s->movie_time;
    } else {                                             /* exitVictory: state 3, the shell package again */
        v->on = 0;
        v->movie_on = 0;
        s->state = 3;
    }
}

/* ---------------------------------------------------------------- AttractMode.scr [R] */
static int am_send(Shell *s, int ev);
static void am_next(Shell *s);

/* exitAttractMode */
static void am_exit(Shell *s)
{
    if (s->boot_completed) {
        s->state = 3;
        music(s, "Oingo");
        cr_show(s, IF_MAINMENU);
    } else {
        s->state = 1;
        s->attract_state = -1;
        am_next(s);
    }
}

/* gotoNextState */
static void am_next(Shell *s)
{
    Attract *a = &s->am;
    am_send(s, EV_LOSE);
    a->state = s->attract_state;
    if (a->state > 0 && (a->state & 128)) {
        a->cheat_video = a->state % 128;
        a->state = AS_COUNT - 1;
    } else
        a->state++;
    s->attract_state = a->state;
    if (a->state >= AS_COUNT) {
        s->boot_completed = 1;
        am_exit(s);
        return;
    }
    am_send(s, EV_GAIN);
}

/* how long a movie that is not played lasts: shell_set_movie_time's, but in the attract mode after the idle time
 * (state 2) at least 5 s of black under "Press START", or Start could only ever land in ShortDelay, which ignores it.
 * Not the game's: there a movie that cannot play has Video.status() 3 at once (0x243130: no player at +0xEC), so the
 * next frame goes on; a port convenience for a viewer that holds no movie */
static float am_movie_time(const Shell *s) { return s->state == 2 && s->movie_time < 5.0f ? 5.0f : s->movie_time; }

/* playVideo(event, name): Video.setVideo(name) and play; here the movie is named (SHELL_EVT_MOVIE) and "plays" for
 * shell_set_movie_time's seconds; the Video quad stays black (there is no picture to give it) */
static void am_video(Shell *s, int ev, const char *name)
{
    Attract *a = &s->am;
    if (ev == EV_GAIN) {
        if (!a->video)
            am_next(s);
        else {
            event(s, SHELL_EVT_MOVIE, name, 0, 0);
            a->movie_on = 1;
            s->movie_clock = 0;
            a->movie_left = am_movie_time(s);
        }
    } else if (ev == EV_LOSE)
        a->movie_on = 0;
    else if (ev == EV_FRAME) {
        if (!a->movie_on)                                /* Video.status() is neither 1 nor 2 */
            am_next(s);
    } else if (ev == EV_START) {
        if (s->state == 2)
            am_exit(s);
        else if (s->prof.nprofiles > 0)                  /* gCommon.getSaveFilesExist() */
            am_next(s);
    }
}

static void am_still(Shell *s, int ev, const char *name, float t)
{
    Attract *a = &s->am;
    Node *r = ifc_root(s, IF_ATTRACT);
    if (ev == EV_INIT) {
        a->still = find(r, name);
        show(a->still, 0);
    } else if (ev == EV_GAIN) {
        a->still = find(r, name);
        if (!a->still)
            am_next(s);
        else {
            show(a->still, 1);
            a->still_time = a->elapsed + t;
        }
    } else if (ev == EV_LOSE)
        show(a->still, 0);
    else if (ev == EV_FRAME) {
        if (a->still_time < a->elapsed)
            am_next(s);
    } else if (ev == EV_START) {
        if (s->prof.nprofiles > 0)
            am_next(s);
    }
}

static int am_state(Shell *s, int st, int ev)
{
    Attract *a = &s->am;
    switch (st) {
    case AS_BOOT_CHECK:
        if (ev == EV_GAIN) {
            show(a->press, 0);
            boot_check(s);
        } else if (ev == EV_LOSE) {
            if (s->state != 1) show(a->press, 1);
        } else if (ev == EV_FRAME) {
            if (s->mc.boot_done) {
                if (s->state != 1) show(a->press, 1);
                am_next(s);
            }
        }
        break;
    case AS_SHORT_DELAY:
        if (ev == EV_GAIN) {
            a->delay = 0;
            if (a->video) col_set(a->video->col, 0, 0, 0, 1);
        } else if (ev == EV_FRAME) {
            if (++a->delay > 115)                        /* 115 frames */
                am_next(s);
        }
        break;
    case AS_MIDWAY: am_video(s, ev, "midway"); break;
    case AS_PIPEWORKS: am_video(s, ev, "pipeworks"); break;
    case AS_DIGITAL: am_video(s, ev, "digital"); break;
    case AS_BINK:
        /* the Bink still only in the boot on platform 4 [R]: the PS2 (2) goes on */
        if (ev != EV_INIT) {
            if (s->state != 1 || s->platform != 4) {
                if (ev == EV_GAIN) am_next(s);
                return 0;
            }
        }
        am_still(s, ev, "Bink", 4.0f);
        break;
    case AS_INTRO:
        if (ev == EV_START) {
            if (s->state == 2) am_exit(s);
            else am_next(s);
            return 0;
        }
        am_video(s, ev, "intro");
        break;
    case AS_RANDOM_GAME:
        if (ev == EV_GAIN) {
            if (s->state == 1) {
                s->boot_completed = 1;
                am_exit(s);
            } else {
                /* gShell.startAttractGame() (0x308460: a random city and monsters, play type 0): not played
                 * here; the attract mode goes on as after one [G] */
                event(s, SHELL_EVT_ATTRACT_GAME, NULL, 0, 0);
                am_next(s);
            }
        }
        break;
    case AS_POST_RANDOM:
        if (ev == EV_GAIN) {
            if (s->state == 1)
                am_exit(s);
            else {
                s->attract_state = 0;
                am_next(s);
            }
        }
        break;
    case AS_CHEAT_VIDEO: {
        int quit = 0;
        if (ev == EV_GAIN) {
            if (!a->video)
                quit = 1;
            else {
                int c = a->cheat_video;
                const char *v = c == 0 ? "intro" : c == 1 ? "victory" : c == 2 ? "credits" :
                                (c == 3 && s->region == 0) || c == 10 ? "ballers" : "credits";
                event(s, SHELL_EVT_MOVIE, v, 0, 0);
                a->movie_on = 1;
                s->movie_clock = 0;
                a->movie_left = am_movie_time(s);
                show(a->press, c != 10);
            }
        } else if (ev == EV_LOSE)
            a->movie_on = 0;
        else if (ev == EV_FRAME) {
            if (!a->movie_on) quit = 1;
        } else if (ev == EV_START)
            quit = 1;
        if (quit) {
            s->attract_state = 0;
            s->idle = IDLE_SECONDS;                      /* gShell.resetAttractTimer */
            s->cheat_param_video = -1;                   /* setCheatFlagsI(16, -1) */
            set_cheats(s, s->cheats | 16);
            am_exit(s);
        }
        break;
    }
    }
    return 0;
}

/* sendStateEvent */
static int am_send(Shell *s, int ev)
{
    int st = s->am.state;
    return st >= 0 && st < AS_COUNT ? am_state(s, st, ev) : 0;
}

static void am_event(Shell *s, int ev, int pad, float dt)
{
    Attract *a = &s->am;
    Node *r = ifc_root(s, IF_ATTRACT);
    if (ev == EV_INIT) {
        a->video = find(r, "Video");
        if (a->video) col_set(a->video->col, 0, 0, 0, 1);
        if (a->video) a->video->video = 1;
        a->press = find(r, "PressStart");
        a->state = s->attract_state;
        if (a->state > 0 && (a->state & 128)) {
            a->cheat_video = a->state % 128;
            a->state = AS_COUNT - 1;
        }
        if (s->state == 1)
            show(a->press, 0);
        else if (a->press)
            a->press->F |= 0x400;                        /* setFlags(1024): blinking */
        for (int i = 0; i < AS_COUNT; i++)
            am_state(s, i, EV_INIT);
    } else if (ev == EV_GAIN && pad == 0) {
        music(s, "");
        am_next(s);
    } else if (ev == EV_LOSE && pad == 0)
        am_send(s, EV_LOSE);
    else if (ev == EV_ACCEPT || ev == EV_START)
        am_send(s, EV_START);
    else if (ev == EV_FRAME) {
        a->elapsed += dt;
        if (a->movie_on)
            s->movie_clock += dt;
        if (a->movie_on && (a->movie_left -= dt) <= 0)
            a->movie_on = 0;
        am_send(s, EV_FRAME);
    }
}

/* ---------------------------------------------------------------- MainMenu.scr [R] */
static const char *const PLAY_MODE_TITLES[4] = { "PlayMode_Campaign", "PlayMode_KingOfTheCity",
                                                 "PlayMode_KingOfTheWorld", "PlayMode_TimedRun" };

static void mm_retro_selection(Shell *s)               /* handleRetroGameSelection */
{
    MainMenu *m = &s->mm;
    if (m->retro_game < 0) m->retro_game = 0;
    if (m->retro_game > 1 + m->ntsc) m->retro_game = 1 + m->ntsc;
    for (int i = 0; i < 3; i++)
        show(m->retro_rects[i], m->retro_game == i);
}

static void mm_icons(Shell *s, float dt)               /* updateIconSelection */
{
    MainMenu *m = &s->mm;
    m->glow_time += dt * 6.0f;
    for (int i = 0; i < 4; i++) {
        Node *q = m->qicons[i];
        if (!q) continue;
        float sc = q->m;
        if (m->play_mode == i + 2) {
            float c[4];
            col_255(c, 2, 198, 27, 255);
            c[3] = 0.75f + (1.0f + sinf(m->glow_time)) / 8.0f;
            set_col(q, c);
            if (sc < 1.4f) {
                sc = fminf(sc + dt * 2.0f, 1.4f);
                if (dt == 0) sc = 1.4f;
                q->m = sc;
            }
        } else {
            float c[4];
            col_255(c, 150, 0, 0, 255);
            set_col(q, c);
            if (sc > 0.9f) {
                sc = fmaxf(sc - dt * 2.0f, 0.9f);
                if (dt == 0) sc = 0.9f;
                q->m = sc;
            }
        }
    }
}

static void mm_cheat_selector(Shell *s)
{
    MainMenu *m = &s->mm;
    Node *sel = find(m->cheat, "cheat_selector"), *row = child_at(m->cheat, m->code_index);
    if (sel && row)
        set_pos(sel, row->p[0] - 2.0f, row->p[1] - 2.0f, row->p[2]);
}

static void mm_reset_cheat(Shell *s)                   /* resetCheat */
{
    MainMenu *m = &s->mm;
    for (int i = 0; i < 6; i++) {
        Node *row = child_at(m->cheat, i);
        show(child_at(row, m->code[0]), 0);
        int j = 0;
        for (Node *c = row ? row->child : NULL; c; c = c->next, j++)
            show(c, j == 0);
        m->code[i] = 0;
        set_text(m->cheat_nums[i], "0");
    }
    m->code_index = 0;
    mm_cheat_selector(s);
}

/* activateCheat [R the codes] */
static void mm_activate_cheat(Shell *s)
{
    MainMenu *m = &s->mm;
    char code[8];
    for (int i = 0; i < 6; i++) code[i] = (char)('0' + m->code[i]);
    code[6] = 0;
    int valid = 1;
    uint32_t f = s->cheats;
    if (strcmp(code, "000000") == 0) { s->cheat_param_block = -1; f = 0; }   /* setCheatFlagsI(0x80000000, -1), clear */
    else if (strcmp(code, "141421") == 0) f |= 1 | 2;    /* every monster, every city */
    else if (strcmp(code, "271828") == 0) f |= 2;
    else if (strcmp(code, "082864") == 0) f |= 65536;
    else if (strcmp(code, "874098") == 0) f |= 65536 | 131072;
    else if (strcmp(code, "071767") == 0) f |= 1073741824;
    else if (strcmp(code, "986960") == 0) f |= 4;
    else if (strcmp(code, "011235") == 0) f |= 8;
    else if (strcmp(code, "000020") == 0) f |= 536870912;
    else if (strncmp(code, "66730", 5) == 0) {           /* a movie: setCheatFlagsI(16, digit) */
        s->cheat_param_video = code[5] - '0';
        f |= 16;
        m->state = 0;
        fader_init(&m->fd, m->icons, 1);
        fader_init(&m->fd, m->cheat, 0);
    } else if (strncmp(code, "24902", 5) == 0) {         /* the start block: setCheatFlagsI(0x80000000, digit) */
        s->cheat_param_block = code[5] - '0';
        f |= 0x80000000u;
    } else if (strcmp(code, "314159") == 0) {
        Node *t = find(ifc_root(s, IF_MAINMENU), "versionInfo");
        if (t) show(t, 1);                               /* its own text: "Version: 1.0.16 / Date: 10-15-04" */
    } else
        valid = 0;
    if (f != s->cheats || valid)
        set_cheats(s, f);
    sound(s, valid ? "Cheat_valid" : "Cheat_Invalid");
}

static void mm_cheating(Shell *s, int ev)              /* processCheating */
{
    MainMenu *m = &s->mm;
    if (ev == EV_UP || ev == EV_DOWN) {
        Node *row = child_at(m->cheat, m->code_index);
        int n = child_count(row);
        int code = m->code[m->code_index] + (ev == EV_UP) - (ev == EV_DOWN);
        if (code >= n) code = 0;
        else if (code < 0) code = n - 1;
        show(child_at(row, m->code[m->code_index]), 0);
        show(child_at(row, code), 1);
        m->code[m->code_index] = code;
        char t[8];
        snprintf(t, sizeof t, "%d", code);
        set_text(m->cheat_nums[m->code_index], t);
        sound(s, "Rollover");
    } else if (ev == EV_LEFT || ev == EV_RIGHT) {
        m->code_index += ev == EV_RIGHT ? 1 : -1;
        if (m->code_index > 5) m->code_index = 0;
        if (m->code_index < 0) m->code_index = 5;
        mm_cheat_selector(s);
        sound(s, "Rollover");
    } else if (ev == EV_ACCEPT) {
        mm_activate_cheat(s);
        m->state = 0;
        fader_go(&m->fd, m->cheat, -1);
        fader_go(&m->fd, m->icons, 1);
    } else if (ev == EV_BACK) {
        m->state = 0;
        fader_go(&m->fd, m->cheat, -1);
        fader_go(&m->fd, m->icons, 1);
        sound(s, "Cheat_back");
    }
}

static void mm_play_mode_select(Shell *s)              /* processPlayModeSelect */
{
    MainMenu *m = &s->mm;
    if (s->platform & (1 | 8 | 4)) {                     /* three and four players: not on the PS2 (2) */
        show(find(m->lb_players, "3 Player"), m->play_mode != 2);
        show(find(m->lb_players, "4 Player"), m->play_mode != 2);
    }
    show(find(m->lb_players, "1 Player"), m->play_mode == 2 || m->play_mode == 5);
    s->gs.play_type = m->play_mode;
    m->state = 1;
    /* the list's selection on a shown item [G: the list keeps its own] */
    if (!list_enabled(m->lb_players, m->lb_players ? m->lb_players->sel : -1)) {
        int n = list_count(m->lb_players);
        for (int i = 0; i < n; i++)
            if (list_enabled(m->lb_players, i)) { list_select(m->lb_players, i); break; }
    }
    fader_go(&m->fd, m->icons, -1);
    fader_go(&m->fd, m->numplayers, 1);
}

static void mm_to_monster_select(Shell *s)
{
    controller_table(s, -1);
    ms_setup_from_main_menu(s);
    s->cs.jumped = 0;                                    /* CitySelect::setJumpedFromMainMenu(0) */
    cr_show(s, IF_MONSTERSELECT);
}

static void mm_num_player_select(Shell *s)             /* processNumPlayerSelect */
{
    MainMenu *m = &s->mm;
    int sel = m->lb_players ? m->lb_players->sel : 0;
    if (sel < 0) sel = 0;
    m->num_players = sel + 1;
    m->num_bots = m->num_players - sel;
    if (m->num_players == 1 || m->play_mode == 2) {
        m->num_bots = 0;
        /* campaign: gCommon.restoreCheckpoint() (GameStatus 0x33D820: the profile's checkpoint) [G: the viewer's] */
        s->gs.num_players = m->num_players;
        s->gs.num_bots = m->num_bots;
        mm_to_monster_select(s);
        return;
    }
    m->state = 2;
    fader_go(&m->fd, m->numplayers, -1);
    fader_go(&m->fd, m->numbots, 1);
    Node *title = find(ifc_root(s, IF_MAINMENU), "nb_title");
    char key[32];
    snprintf(key, sizeof key, "MainMenu_%dPlayer", m->num_players);
    node_localize(&s->gui, title, key);
    for (int i = 0; i < 4; i++) {
        Node *item = list_item(m->lb_bots, i);
        if (!item) continue;
        if (i < m->num_players) {
            char t[8];
            show(item, 1);
            snprintf(t, sizeof t, "%d", m->num_players - i);
            set_text(item, t);
            snprintf(t, sizeof t, "%d", i);
            set_text(find(item, "computer"), t);
        } else
            show(item, 0);
    }
    list_select(m->lb_bots, m->num_players - 1);
}

static void mm_num_bot_select(Shell *s)                /* processNumBotSelect */
{
    MainMenu *m = &s->mm;
    m->num_bots = m->lb_bots ? m->lb_bots->sel : 0;
    if (m->num_bots < 0) m->num_bots = 0;
    s->gs.num_players = m->num_players;
    s->gs.num_bots = m->num_bots;
    mm_to_monster_select(s);
}

static void mm_retro_games(Shell *s, float dt)         /* processRetroGames */
{
    MainMenu *m = &s->mm;
    if (m->retro_game == 2) {                            /* the Ballers video (NTSC) */
        m->start_retro = 0;
        s->cheat_param_video = 10;
        set_cheats(s, s->cheats | 16);
        m->state = 0;
        fader_init(&m->fd, m->icons, 1);
        fader_init(&m->fd, m->retro, 0);
    } else {
        float prev = m->retro_delay;
        m->retro_delay -= dt;
        if (m->retro_delay <= 0 && prev > 0) {
            /* gShell.startEmulator(n) (0x3063A0: desi/m1 or desi/jarvos, LoadExecPS2): skipped; back to the menu */
            event(s, SHELL_EVT_EMULATOR, m->retro_game ? "rwt" : "rampage", m->retro_game, 0);
            m->start_retro = 0;
            m->state = 0;
            fader_go(&m->fd, m->retro, -1);
            fader_go(&m->fd, m->icons, 1);
        }
    }
}

static void mm_update_faders(Shell *s, float dt) { fader_update(&s->mm.fd, dt); }   /* MainMenu::updateObjectFader */

static void mm_event(Shell *s, int ev, int button, int pad, float dt)
{
    MainMenu *m = &s->mm;
    Node *r = ifc_root(s, IF_MAINMENU);
    if (ev == EV_INIT) {
        m->state = 0;
        m->play_mode = s->gs.play_type;
        m->retro_game = 0;
        fix_buttons(r, "btn_select", "btn_profile", "btn_retro", "btn_options");
        fix_buttons(r, "np_select", "np_back", "", "");
        fix_buttons(r, "nb_select", "nb_back", "", "");
        m->playtype = find(r, "playmode");
        m->numplayers = find(r, "NumPlayers");
        m->numbots = find(r, "NumBots");
        m->icons = find(r, "icons");
        m->retro = find(r, "RetroGames");
        m->cheat = find(r, "CheatMenu");
        fix_buttons(m->retro, "nb_select", "nb_back", "", "");
        fix_buttons(m->cheat, "cb_select", "cb_back", "", "");
        m->ntsc = s->platform == 2 && s->prof.language == 1 && s->region == 0;
        if (m->ntsc) m->retro = find(r, "RetroGamesNTSC");
        m->lb_players = find(r, "np_selection");
        m->lb_bots = find(r, "nb_selection");
        m->retro_rects[0] = child_at(find(m->retro, "rg_rampage1"), 0);
        m->retro_rects[1] = child_at(find(m->retro, "rg_rampage2"), 0);
        m->retro_rects[2] = child_at(find(m->retro, "Ballers_Phenom_logo"), 0);
        mm_retro_selection(s);
        /* InitGrowShrink(obj, "TopLevel", ..): there is no TopLevel in MainMenu.ifc: nothing grows */
        fader_init(&m->fd, m->icons, 1);
        fader_init(&m->fd, m->numplayers, 0);
        fader_init(&m->fd, m->numbots, 0);
        fader_init(&m->fd, m->retro, 0);
        fader_init(&m->fd, m->cheat, 0);
        if (m->lb_players) { m->lb_players->pulse = 1; list_select(m->lb_players, 0); }
        if (m->lb_bots) m->lb_bots->pulse = 1;
        for (int i = 0; i < 4; i++)
            m->qicons[i] = child_at(m->icons, i);
        Node *nums = find(r, "cmNumbers");
        m->code_index = 0;
        for (int i = 0; i < 6; i++) {
            m->code[i] = 0;
            m->cheat_nums[i] = child_at(nums, i);
        }
        Node *bs = find(r, "btn_select"), *bp = find(r, "btn_profile");
        float tw = text_w(&s->gui, bs);
        if (text_w(&s->gui, bp) > tw) tw = text_w(&s->gui, bp);
        float shifty = s->prof.language == 7 ? 5.0f : 0.0f;
        set_pos(bs, 590.0f - tw, 0.0f + shifty, 0);
        set_pos(bp, 590.0f - tw, -20.0f + shifty, 0);
    } else if (ev == EV_GAIN && pad == 0) {
        if (m->state == 0 && get_fade(m->icons) < 1.0f)
            fader_go(&m->fd, m->icons, 1);
        m->play_mode = s->gs.play_type > 2 ? s->gs.play_type : 2;
        node_localize(&s->gui, m->playtype, PLAY_MODE_TITLES[m->play_mode - 2]);
        mm_icons(s, 0);
        mm_reset_cheat(s);
        controller_table(s, 1);
        if (s->show_profile) {                           /* gShell.getShowProfile() */
            if (s->prof.allow_save)
                load_profile(s);
            /* else gCommon.restoreProfile(): the viewer's copy stays */
        }
        m->start_retro = 0;
        m->retro_delay = 0.25f;
    } else if (ev == EV_FRAME) {
        fader_update(&m->fd, dt);
        mm_icons(s, dt);
        /* updateCheatActivation: L2 and R2 held together, nothing else, no popup */
        if (m->state == 0 && s->mc.mode == MC_OFF && s->held_buttons == (SHELL_HELD_L2 | SHELL_HELD_R2)) {
            fader_go(&m->fd, m->icons, -1);
            fader_go(&m->fd, m->cheat, 1);
            mm_reset_cheat(s);
            m->state = 5;
        }
        if (m->start_retro)
            mm_retro_games(s, dt);
    } else if (m->state == 5)
        mm_cheating(s, ev);
    else if (ev == EV_ACCEPT || ev == EV_START) {
        if (m->state == 0) mm_play_mode_select(s);
        else if (m->state == 1) mm_num_player_select(s);
        else if (m->state == 2) mm_num_bot_select(s);
        else if (m->state == 4) m->start_retro = 1;
        sound(s, "Click");
    } else if (ev == EV_BACK) {
        if (m->state == 0)
            load_profile(s);                             /* "Profiles" */
        else if (m->state == 1) {
            sound(s, "Cancel");
            m->state = 0;
            fader_go(&m->fd, m->icons, 1);
            fader_go(&m->fd, m->numplayers, -1);
        } else if (m->state == 2) {
            sound(s, "Cancel");
            m->state = 1;
            fader_go(&m->fd, m->numbots, -1);
            fader_go(&m->fd, m->numplayers, 1);
        } else if (m->state == 4 && !m->start_retro) {
            sound(s, "Cancel");
            m->state = 0;
            fader_go(&m->fd, m->icons, 1);
            fader_go(&m->fd, m->retro, -1);
        }
    } else if (ev == EV_LEFT || ev == EV_RIGHT) {
        if (m->state == 0) {
            m->play_mode += ev == EV_RIGHT ? 1 : -1;
            if (m->play_mode < 2) m->play_mode = 5;
            if (m->play_mode > 5) m->play_mode = 2;
            node_localize(&s->gui, m->playtype, PLAY_MODE_TITLES[m->play_mode - 2]);
            sound(s, "Rollover");
        } else if (m->state == 4) {
            m->retro_game += ev == EV_RIGHT ? 1 : -1;
            mm_retro_selection(s);
            sound(s, "Rollover");
        }
    } else if (ev == EV_UP || ev == EV_DOWN) {
        Node *lb = m->state == 1 ? m->lb_players : m->state == 2 ? m->lb_bots : NULL;
        if (lb) {
            int d = ev == EV_UP ? -1 : 1;
            if (list_enabled(lb, lb->sel + d)) {
                list_increment(lb, d);
                sound(s, "Rollover");
            }
        }
    } else if (ev == EV_BUTTON && m->state == 0) {
        if (button == 1) {                               /* Options */
            fader_go(&m->fd, m->icons, -1);
            /* ControlRoom::fadeInInterface("Options") */
            fader_go(&s->cr_fd, ifc_root(s, IF_OPTIONS), 1);
            ifc_event(s, s->focus, EV_LOSE, 0, 0, 0);
            s->focus = IF_OPTIONS;
            ifc_event(s, s->focus, EV_GAIN, 0, 0, 0);
            sound(s, "Click");
        } else if (button == 0) {                        /* the retro games */
            m->state = 4;
            fader_go(&m->fd, m->icons, -1);
            fader_go(&m->fd, m->retro, 1);
            sound(s, "Click");
        }
    }
}

/* ---------------------------------------------------------------- Options.scr [R] */
static void op_setup(Shell *s)                         /* setupItems */
{
    Options *o = &s->op;
    show(o->group, 1);
    o->exiting = 0;
    o->tweak_sound = 0;
    o->f_sound = s->prof.sound;
    o->f_music = s->prof.music;
    o->rumble_on = s->prof.rumble;
    if (o->sound) o->sound->val = 100.0f * o->f_sound / 0.75f;
    if (o->music) o->music->val = 100.0f * o->f_music / 0.75f;
    list_select(o->rumble, !o->rumble_on);
    for (int i = 0; i < 3; i++)
        if (o->titles[i]) o->titles[i]->tscale = 1.0f;
}

static void op_save(Shell *s)                          /* saveSettings */
{
    Options *o = &s->op;
    s->prof.sound = (o->sound ? o->sound->val : 100) / 100.0f * 0.75f;
    s->prof.music = (o->music ? o->music->val : 100) / 100.0f * 0.75f;
    s->prof.rumble = !(o->rumble ? o->rumble->sel : 0);
    event(s, SHELL_EVT_SAVE_SETTINGS, NULL, s->prof.rumble, 0);
}

static void op_event(Shell *s, int ev, int pad, float dt)
{
    Options *o = &s->op;
    Node *r = ifc_root(s, IF_OPTIONS);
    (void)pad;
    if (ev == EV_FRAME) {
        o->pulse += dt;                                  /* pulseSelection */
        if (o->titles[o->mode]) o->titles[o->mode]->tscale = 1.0f + sinf(4.0f * o->pulse) * 0.1f;
        mm_update_faders(s, dt);
        if (o->exiting) {
            if (s->mc.mode == MC_OFF) {                  /* !gCommon.isSaveGameActive() */
                o->exiting = 0;
                /* ControlRoom::fadeOutInterface("MainMenu") */
                fader_go(&s->cr_fd, ifc_root(s, IF_OPTIONS), -1);
                ifc_event(s, s->focus, EV_LOSE, 0, 0, 0);
                s->focus = IF_MAINMENU;
                ifc_event(s, s->focus, EV_GAIN, 0, 0, 0);
            }
            return;
        }
        float sv = o->sound ? o->sound->val : 0, mv = o->music ? o->music->val : 0;
        /* updateSliders, one step a script frame while held */
        o->tick += dt;
        while (o->tick >= TICK) {
            o->tick -= TICK;
            Node *sl = o->mode == 1 ? o->music : o->sound;
            if (o->mode < 2 && sl) {
                if (s->held_x >= 0.05f || (s->held_buttons & SHELL_HELD_RIGHT)) sl->val = fminf(sl->val + 1, 100);
                else if (s->held_x <= -0.05f || (s->held_buttons & SHELL_HELD_LEFT)) sl->val = fmaxf(sl->val - 1, 0);
            }
        }
        float sn = o->sound ? o->sound->val : 0, mn = o->music ? o->music->val : 0;
        if (o->tweak_sound) {
            if (sv == sn) {
                event(s, SHELL_EVT_SFX_VOLUME, NULL, 0, sn / 100.0f * 0.75f);
                o->tweak_sound = 0;
                sound(s, "Click");
            }
        } else if (sv != sn)
            o->tweak_sound = 1;
        if (mv != mn)
            event(s, SHELL_EVT_MUSIC_VOLUME, NULL, 0, mn / 100.0f * 0.75f);
    } else if (o->exiting) {
    } else if (ev == EV_INIT && !o->initialized) {
        o->initialized = 1;
        o->mode = 0;
        o->pulse = 0;
        o->group = find(r, "options_group");
        o->sound = find(r, "sound");
        o->music = find(r, "music");
        o->rumble = find(r, "rumble");
        o->titles[0] = find(r, "sound_title");
        o->titles[1] = find(r, "music_title");
        o->titles[2] = find(r, "rumble_title");
        if (s->prof.language == 7 && o->rumble) {
            set_pos(o->rumble, 185, 180, 0);
            Node *on = find(o->rumble, "ON"), *off = find(o->rumble, "OFF");
            if (on) on->tscale = 0.6f;
            if (off) off->tscale = 0.6f;
        }
        op_setup(s);
    } else if (ev == EV_GAIN)
        op_setup(s);
    else if (ev == EV_ACCEPT) {
        sound(s, "Click");
        op_save(s);                                      /* and gCommon.saveProfile() */
        o->exiting = 1;
    } else if (ev == EV_BACK) {
        sound(s, "Cancel");
        event(s, SHELL_EVT_SFX_VOLUME, NULL, 0, o->f_sound);
        event(s, SHELL_EVT_MUSIC_VOLUME, NULL, 0, o->f_music);
        event(s, SHELL_EVT_RUMBLE, NULL, o->rumble_on, 0);
        o->exiting = 1;
    } else if (ev == EV_UP || ev == EV_DOWN) {
        int prev = o->mode;
        o->mode += ev == EV_UP ? -1 : 1;
        if (o->mode < 0) o->mode = 2;
        if (o->mode > 2) o->mode = 0;
        if (o->titles[prev]) o->titles[prev]->tscale = 1.0f;
        o->pulse = 0;
        sound(s, "Rollover");
    } else if ((ev == EV_LEFT || ev == EV_RIGHT) && o->mode >= 2) {
        int sel = o->rumble ? o->rumble->sel : 0;       /* updateLists */
        list_increment_wrap(o->rumble, ev == EV_RIGHT ? 1 : -1);
        if (sel && o->rumble && sel != o->rumble->sel) {
            event(s, SHELL_EVT_RUMBLE, NULL, 1, 0);
            event(s, SHELL_EVT_RUMBLE_PULSE, NULL, 250, 0);
        }
        sound(s, "Rollover");
    }
}

/* ---------------------------------------------------------------- MonsterSelect.scr [R] */
static const float CARD_SCALES[4] = { 1.15f, 1.0f, 0.85f, 0.65f };   /* fDefaultCardScales */

/* isMonsterAvailable(player, monster): flag 0x80, or the cheat 1 for any finished non-blank monster; four players
 * leave out DisableFor4Player */
static int ms_available(Shell *s, int player, int monster)
{
    int ok = (monster_flags(s, player, monster) & 128) != 0;
    if ((s->cheats & 1) && strcmp(monster_name(s, monster), "Blank") != 0 &&
        !(int)monster_prop(s, monster, "NotFinished", 0))
        ok = 1;
    if (ok && s->ms.num_players == 4 && (int)monster_prop(s, monster, "DisableFor4Player", 0))
        ok = 0;
    return ok;
}

static int ms_monster(const MonsterSel *m, int p)
{
    const MsPlayer *pl = &m->pl[p];
    return pl->selection >= 0 && pl->selection < pl->nmon ? pl->monsters[pl->selection] : 0;
}

static int ms_ready_for_button(const MonsterSel *m, int p)   /* readyForButtonPress */
{
    int st = m->pl[p].spin;
    return st == 0 || st == 3 || st == 6;
}

/* setupCardFace(name, player) */
static void ms_card_face(Shell *s, const char *name, int p)
{
    MonsterSel *m = &s->ms;
    int id = ms_monster(m, p);
    int up = monster_flags(s, p, id);
    Node *card = findf(m->player_base, "Player%d", p);
    Node *base = find(card, "base"), *fx = find(card, "card_effect"), *head = find(card, "head");
    if (!card || !base || !fx || !head)
        return;
    char t[64];
    snprintf(t, sizeof t, "Card_base_%d", p);
    set_tex(&s->gui, base, t);
    snprintf(t, sizeof t, "card_glow_%d.edf", p);       /* cardEffect.setEffect */
    effect_set(fx, t);
    snprintf(t, sizeof t, "%s_Body", name);
    set_tex(&s->gui, head, t);
    node_localize(&s->gui, find(card, "name"), name);
    Node *fu = find(card, "front_upgrades");
    for (int i = 0; i < 4; i++)
        show(child_at(fu, i), (up & (1 << i)) != 0);
    Node *cb = find(card, "card_back_name"), *face = find(card, "card_back_face");
    Node *climb = find(find(card, "ability0"), "slider"), *jump = find(find(card, "ability1"), "slider");
    Node *run = find(find(card, "ability2"), "slider"), *mut = find(card, "mutation");
    if (!cb || !climb || !jump || !run || !face || !mut)
        return;
    node_localize(&s->gui, cb, name);
    int four = s->gs.num_players == 4;
    snprintf(t, sizeof t, "MS_MUTATION_%s", name);
    if (four) {
        Node *c = child_at(mut, 0);
        if (c && c->kind == N_TEXT) node_localize(&s->gui, c, t);
        else four = 0;
    }
    if (!four) {                                         /* "MUTATION: Ape" */
        const char *a = loc_find(&s->gui, "MS_MUTATION:"), *b = loc_find(&s->gui, t);
        char both[MAX_TEXT];
        snprintf(both, sizeof both, "%s %s", a ? a : "MS_MUTATION:", b ? b : t);
        set_text(mut, both);
    }
    snprintf(t, sizeof t, "%s_Head", name);
    set_tex(&s->gui, face, t);
    climb->val = monster_prop(s, id, "Climb", 0);
    jump->val = monster_prop(s, id, "Jump", 0);
    run->val = monster_prop(s, id, "Run", 0);
    float yellow[4], grey[4];
    col_255(yellow, 223, 203, 31, 255);
    col_255(grey, 128, 128, 128, 255);
    Node *bu = find(card, "back_upgrades");
    for (int i = 0; i < 4; i++) {
        Node *ut = findf(bu, "upgrade%d", i), *star = find(ut, "star_flat");
        set_col(ut, (up & (1 << i)) ? yellow : grey);
        set_col(star, (up & (1 << i)) ? yellow : grey);
    }
}

static void ms_flip(Shell *s, int p)                   /* flipCard */
{
    MsPlayer *pl = &s->ms.pl[p];
    pl->spin_amount = 0;
    if (pl->spin == 0 || pl->spin >= 6) {
        effect_stop(find(pl->base, "card_effect"));
        pl->spin = 1;
        pl->spin_amount = 1;
    } else if (pl->spin == 3) {
        pl->spin = 4;
        pl->spin_amount = 1;
    }
}

static void ms_sides(Node *base, int front)
{
    Node *mo = find(base, "monster"), *bk = find(base, "backside");
    show(mo, front); set_fade(mo, front ? 1.0f : 0.0f);
    show(bk, !front); set_fade(bk, front ? 0.0f : 1.0f);
}

/* processFlipping: 0.125 s to edge-on, the other side, 0.125 s back; setScaleX on the card */
static void spin_step(int *spin, float *amount, Node *base, float scale_base, float dt)
{
    if (*spin <= 0 || *spin == 3 || *spin >= 6)
        return;
    if (*spin == 1 || *spin == 4) {
        *amount -= dt / 0.125f;
        if (*amount <= 0.05f) {
            ms_sides(base, *spin != 1);
            (*spin)++;
            *amount = 0.05f;
        }
    } else if (*spin == 2 || *spin == 5) {
        *amount += dt / 0.125f;
        if (*amount >= 1.0f) {
            ms_sides(base, *spin == 5);
            if (*spin == 5)
                effect_start(find(base, "card_effect"));
            (*spin)++;
            *amount = 1.0f;
        }
    }
    if (base) base->sx = *amount * scale_base;
}

static void ms_ready(Shell *s, int p, int ready)       /* setCardToReadyState */
{
    MonsterSel *m = &s->ms;
    m->pl[p].ready = ready;
    if (m->pl[p].spin == 3)
        ms_flip(s, p);
    const char *name = monster_name(s, ms_monster(m, p));
    Node *head = find(findf(m->player_base, "Player%d", p), "head");
    char t[64];
    snprintf(t, sizeof t, "%s_%s", name, ready ? "Head" : "Body");
    set_tex(&s->gui, head, t);
    show(findf(m->player_base, "P%d_Arrows", p + 1), !ready);
}

/* the available monsters of player p, the selection kept on prev (a monster) when it is among them */
static int ms_fill(Shell *s, int p, int prev)
{
    MsPlayer *pl = &s->ms.pl[p];
    int found = 0;
    pl->nmon = 0;
    for (int i = 0; i < SHELL_MONSTERS; i++)
        if (ms_available(s, p, i)) {
            if (i == prev) {
                pl->selection = pl->nmon;
                found = 1;
            }
            pl->monsters[pl->nmon++] = i;
        }
    if (pl->nmon == 0)
        pl->monsters[pl->nmon++] = 0;                    /* [G: George, so a card shows] */
    return found;
}

/* setupFromMainMenu */
static void ms_setup_from_main_menu(Shell *s)
{
    MonsterSel *m = &s->ms;
    Node *r = ifc_root(s, IF_MONSTERSELECT);
    m->num_players = s->gs.num_players;
    m->num_bots = s->gs.num_bots;
    char name[24];
    snprintf(name, sizeof name, "%d Player", m->num_players);
    m->player_base = find(r, name);
    int n = m->num_players > 4 ? 4 : m->num_players;
    for (int i = 0; i < n; i++)
        m->pl[i].selection = s->gs.monster[i];
    for (int i = 0; i < n; i++)
        if (!ms_available(s, i, m->pl[i].selection))
            for (int j = 0; j < 6; j++) {
                int ok = 1;
                for (int k = i + 1; ok && k < n; k++)
                    if (j == m->pl[k].selection) ok = 0;
                if (ok) m->pl[i].selection = j;
            }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            if (i == j || m->pl[i].selection != m->pl[j].selection)
                continue;
            for (int k = 0; k < 6; k++) {
                int ok = 1;
                for (int q = 0; ok && q < n; q++)
                    if (q != j && k == m->pl[q].selection) ok = 0;
                if (ok) { m->pl[j].selection = k; break; }
            }
        }
    for (int i = 0; i < n; i++) {
        MsPlayer *pl = &m->pl[i];
        pl->ready = 0;
        int mon = pl->selection;
        ms_fill(s, i, mon);                              /* (the selection stays as it was when not found) */
        if (pl->selection >= pl->nmon) pl->selection = 0;
        ms_card_face(s, monster_name(s, ms_monster(m, i)), i);
        show(findf(m->player_base, "P%d_Arrows", i + 1), 1);
    }
    for (int i = 1; i < 5; i++) {
        snprintf(name, sizeof name, "%d Player", i);
        show(find(r, name), i == m->num_players);
    }
    for (int i = 0; i < n; i++) {
        MsPlayer *pl = &m->pl[i];
        pl->base = findf(m->player_base, "Player%d", i);
        pl->spin = 0;
        pl->spin_amount = 0;
        pl->scale_base = CARD_SCALES[n > 0 ? n - 1 : 0];
        if (pl->base) pl->base->sx = pl->scale_base;
    }
    show(find(r, "nb_loadmonsters"), m->num_players > 1);
    int bot = 1;
    for (int i = 0; m->num_players > 1 && i < n; i++) {
        char key[16];
        if (i < m->num_players - m->num_bots) snprintf(key, sizeof key, "P%d", i + 1);
        else snprintf(key, sizeof key, "Bot%d", bot++);
        node_localize(&s->gui, findf(m->player_base, "playerId%d", i), key);
    }
}

static void ms_monster_from_sheet(Shell *s, const char *name)   /* setMonsterFromSheet */
{
    MonsterSel *m = &s->ms;
    MsPlayer *pl = &m->pl[m->sheet_controller];
    int i;
    for (i = 0; i < pl->nmon; i++)
        if (strcmp(monster_name(s, pl->monsters[i]), name) == 0)
            break;
    pl->selection = i < pl->nmon ? i : pl->selection;
    ms_card_face(s, name, m->sheet_controller);
}

static int ms_player_selection(Shell *s, int p) { return ms_monster(&s->ms, p); }   /* getPlayerSelection */

/* remapP0Controller: with bots, player 1's pad picks for each bot in turn once its own card is ready */
static int ms_remap(Shell *s, int ev, int pad)
{
    MonsterSel *m = &s->ms;
    int c = pad;
    if (ms_ready_for_button(m, c) && m->pl[c].ready) {
        for (int i = m->num_players - m->num_bots; i < m->num_players; i++)
            if (!m->pl[i].ready) { c = i; break; }
        if (c == pad)
            c = m->num_players - 1;
        if (ev == EV_BACK && !m->pl[c].ready) {
            c--;
            if (c < m->num_players - m->num_bots) c = 0;
        }
    }
    return c;
}

static void ms_selection_rects(Shell *s, float dt)    /* updateSelectionRects */
{
    static const float norm[4][3] = { { 2, 198, 27 }, { 213, 94, 43 }, { 0, 150, 255 }, { 255, 70, 163 } };
    static const float dark[4][3] = { { 2, 40, 6.5f }, { 50.5f, 22, 11.5f }, { 0, 36, 60 }, { 60.5f, 17, 42 } };
    MonsterSel *m = &s->ms;
    m->hl_time += dt * 8.0f;
    float k = (1.0f + sinf(m->hl_time)) / 2.0f;
    int cid = 0;
    for (int i = 0; i < m->num_players && i < 4; i++) {
        int idx = i, offbot = 0;
        if (i == 0 && m->pl[i].ready) cid = m->num_players - m->num_bots;
        if (i >= m->num_players - m->num_bots) {
            if (m->pl[i].ready) cid++;
            idx = 0;
            if (i != cid) offbot = 1;
        }
        float c[4];
        for (int j = 0; j < 3; j++)                      /* Color.interpolate */
            c[j] = (norm[idx][j] + (dark[idx][j] - norm[idx][j]) * k) / 255.0f;
        c[3] = 1;
        if (m->pl[i].ready || offbot) col_255(c, 128, 128, 128, 255);
        Node *f = find(m->pl[i].base, "frontHighlight"), *b = find(m->pl[i].base, "backHighlight");
        if (f) memcpy(f->border, c, sizeof c);
        if (b) memcpy(b->border, c, sizeof c);
    }
}

static void sheet_set_controller(Shell *s, int ctrl, int logic);
static void sheet_select_card(Shell *s, const char *name);

static void ms_event(Shell *s, int ev, int button, int pad, float dt)
{
    MonsterSel *m = &s->ms;
    Node *r = ifc_root(s, IF_MONSTERSELECT);
    if (ev == EV_INIT) {
        if (!m->initialized) {
            fix_buttons(r, "nb_select", "nb_back", "nb_flip", "nb_cardsheet");
            ms_setup_from_main_menu(s);
            if (s->choose_monster)
                show(find(r, "nb_back"), 0);
            m->initialized = 1;
            m->p1_dialog = find(r, "P1Load");
            fader_init(&m->fd, m->p1_dialog, 0);
        }
        return;
    }
    if (ev != EV_FRAME && pad >= m->num_players - m->num_bots && pad > 0)
        return;
    int logic = pad;
    int c = pad;
    if (c == 0 && m->num_bots != 0 && ev != EV_FRAME)
        c = ms_remap(s, ev, pad);
    if (c < 0 || c > 3) c = 0;
    if (m->show_p1) {
        if (ev == EV_FRAME) {
            fader_update(&m->fd, dt);
            if (m->show_p1 == 2 && get_fade(m->p1_dialog) <= 0)
                m->show_p1 = 0;
        } else if (ev == EV_ACCEPT && c == 0 && m->show_p1 == 1) {
            m->show_p1 = 2;
            fader_go(&m->fd, m->p1_dialog, -1);
            controller_table(s, -1);
        }
        return;
    }
    if (ev == EV_FRAME) {
        fader_update(&m->fd, dt);
        for (int i = 0; i < m->num_players && i < 4; i++)
            spin_step(&m->pl[i].spin, &m->pl[i].spin_amount, m->pl[i].base, m->pl[i].scale_base, dt);
        ms_selection_rects(s, dt);
    } else if (ev == EV_GAIN) {
        if (c == 0) {
            m->show_p1 = 0;
            fader_init(&m->fd, m->p1_dialog, 0);
            int mode = s->gs.play_type > 2 ? s->gs.play_type : 2;
            node_localize(&s->gui, find(r, "play_mode"), PLAY_MODE_TITLES[mode - 2]);
            for (int i = 0; i < m->num_players && i < 4; i++) {
                m->pl[i].spin = 0;
                ms_sides(m->pl[i].base, 1);
            }
            Node *t = find(m->p1_dialog, "np_title");
            node_localize(&s->gui, t, s->prof.loaded ? "MS_P1_Profile" : "MS_P1_Profile_NoProfile");
        }
    } else if (ev == EV_LEFT || ev == EV_RIGHT) {
        MsPlayer *pl = &m->pl[c];
        if (ms_ready_for_button(m, c) && !pl->ready && pl->nmon > 0) {
            int dir = ev == EV_LEFT ? -1 : 1, sel = pl->selection;
            if (m->num_players > 1) {
                for (int tries = 0, free_ = 0; !free_ && tries <= pl->nmon; tries++) {
                    free_ = 1;
                    sel += dir;
                    if (sel < 0) sel = pl->nmon - 1;
                    if (sel >= pl->nmon) sel = 0;
                    for (int i = 0; free_ && i < m->num_players; i++)
                        if (i != c && m->pl[i].selection == sel)   /* [R: list indexes compared] */
                            free_ = 0;
                }
            } else {
                sel += dir;
                if (sel < 0) sel = pl->nmon - 1;
                if (sel >= pl->nmon) sel = 0;
            }
            pl->selection = sel;
            ms_card_face(s, monster_name(s, ms_monster(m, c)), c);
            sound(s, "Rollover");
        }
    } else if (ev == EV_BACK) {
        if (ms_ready_for_button(m, c)) {
            sound(s, "Cancel");
            if (m->pl[c].ready)
                ms_ready(s, c, 0);
            else if (!s->choose_monster) {
                s->quitting = 1;                         /* gCommon.setPlayerQuiting(1) */
                cr_show(s, IF_MAINMENU);
            }
        }
    } else if (ev == EV_ACCEPT) {
        if (!ms_ready_for_button(m, c))
            return;
        sound(s, "Click");
        int nready = 0;
        for (int i = 0; i < m->num_players && i < 4; i++) {
            if (!m->pl[i].ready && c == i)
                ms_ready(s, i, 1);
            if (m->pl[i].ready) nready++;
        }
        if (nready < m->num_players)
            return;
        for (int i = 0; i < s->gs.num_players && i < 4; i++) {   /* GameStatus.setMonsterIdx */
            s->gs.monster[i] = ms_monster(m, i);
            snprintf(s->gs.monster_name[i], sizeof s->gs.monster_name[i], "%s", monster_name(s, s->gs.monster[i]));
        }
        if (s->choose_monster && !s->city_destroyed)
            cr_start_game(s);
        else if (s->gs.play_type == 4) {                 /* King of the World: city 0, block 0 */
            s->gs.city_index = 0;
            s->gs.start_block = 0;
            snprintf(s->gs.city, sizeof s->gs.city, "%s", city_name(s, 0));
            cr_start_game(s);
        } else
            cr_show(s, IF_CITYSELECT);
    } else if (ev == EV_BUTTON) {
        if (ms_ready_for_button(m, c) && !m->pl[c].ready) {
            if (button == 0) {
                sound(s, "Click");
                ms_flip(s, c);
            } else if (button == 1) {
                sound(s, "Click");
                m->sheet_controller = c;
                sheet_set_controller(s, c, pad);
                sheet_select_card(s, monster_name(s, ms_monster(m, c)));
                cr_show(s, IF_MONSTERSHEET);
            } else if (button == 4 && m->num_players > 1) {
                sound(s, "Click");
                if (c == 0) {
                    m->show_p1 = 1;
                    fader_go(&m->fd, m->p1_dialog, 1);
                    controller_table(s, 1);
                } else {
                    controller_table(s, 1 << logic);
                    /* gShell.loadBuddyProfile(c, "MonsterSelect::onGetBuddyProfileCallback") (0x3077B0: the
                     * popup for that player) */
                    if (s->mc.L) {
                        mc_show(s);
                        mc_open_list(s, MCP_BUDDY, c);
                    }
                }
            }
        }
    }
}

/* onGetBuddyProfileCallback(player) */
void shell_buddy_profile(Shell *s, int p)
{
    MonsterSel *m;
    if (!s)
        return;
    m = &s->ms;
    controller_table(s, -1);
    if (p < 0 || p > 3)
        return;
    MsPlayer *pl = &m->pl[p];
    int prev = ms_monster(m, p);
    if (!ms_fill(s, p, prev)) {
        for (int i = 0; i < 6; i++) {
            int match = 0;
            for (int j = 0; !match && j < m->num_players; j++)
                if (j != p && ms_monster(m, j) == i) match = 1;
            if (!match) { pl->selection = i < pl->nmon ? i : 0; break; }
        }
    }
    ms_card_face(s, monster_name(s, ms_monster(m, p)), p);
}

/* ---------------------------------------------------------------- MonsterSheet.scr [R] */
static int sheet_available(Shell *s, int monster)       /* isMonsterAvailable */
{
    MonsterSheet *h = &s->sh;
    int ok = (monster_flags(s, h->map_controller, monster) & 128) != 0;
    for (int i = 0; i < 3; i++)
        if (h->in_use[i] == monster) return 0;
    if ((s->cheats & 1) && strcmp(monster_name(s, monster), "Blank") != 0 &&
        !(int)monster_prop(s, monster, "NotFinished", 0))
        ok = 1;
    if (ok && s->gs.num_players == 4 && (int)monster_prop(s, monster, "DisableFor4Player", 0))
        ok = 0;
    return ok;
}

static void sheet_mini_upgrades(Shell *s, Node *card, int monster)
{
    int up = monster_flags(s, s->sh.map_controller, monster);
    Node *g = find(card, "small_upgrades");
    for (int i = 0; i < 4; i++)
        show(child_at(g, i), (up & (1 << i)) != 0);
}

static void sheet_set_controller(Shell *s, int ctrl, int logic)   /* setMonsterController */
{
    s->sh.map_controller = ctrl;
    s->sh.controller = logic;
}

static void sheet_update_selection(Shell *s, int playsound)
{
    MonsterSheet *h = &s->sh;
    Node *r = ifc_root(s, IF_MONSTERSHEET);
    h->selection = h->row + h->col * 2;
    int prev = h->valid;
    h->valid = sheet_available(s, h->selection);
    if (!h->valid && prev) {
        set_fade(find(r, "nb_select"), 0.25f);
        set_fade(find(r, "nb_grow"), 0.25f);
    } else if (h->valid && prev <= 0) {
        set_fade(find(r, "nb_select"), 1);
        set_fade(find(r, "nb_grow"), 1);
    }
    set_fade(find(r, "nb_flip"), 0.25f);
    if (playsound && !h->sound_played) {
        h->sound_played = 1;
        sound(s, "Rollover");
    }
}

static void sheet_positions(Shell *s)                  /* updatePositions */
{
    MonsterSheet *h = &s->sh;
    int prev = h->col;
    if (h->target < 0) h->target = 0;
    if (h->target > h->right_bounds) h->target = h->right_bounds;
    if (h->target < h->left_edge || h->target > h->right_edge) {
        float dif = h->target - h->left_edge;
        if (h->target > h->right_edge) dif = h->target - h->right_edge;
        h->left_edge += dif;
        h->right_edge += dif;
        h->offset_x += dif;
        set_pos(h->offset, -h->offset_x, 0, 0);
    }
    float cal = (float)((int)(h->target / 130.0f) * 130);
    if (cal < h->left_edge) cal += 130;
    if (cal > h->right_edge) cal -= 130;
    if (h->selrect && cal != h->selrect->p[0])
        h->selrect->p[0] = cal;
    show(h->arrow_r, h->target != h->right_bounds);
    show(h->arrow_l, h->target > 130.0f);
    h->col = (int)(cal / 130.0f);
    h->prev_target = h->target;
    if (h->col != prev)
        sheet_update_selection(s, 1);
}

static void sheet_select_card(Shell *s, const char *name)   /* selectMonsterCard */
{
    MonsterSheet *h = &s->sh;
    int i;
    for (i = 0; i < SHELL_MONSTERS; i++)
        if (strcmp(monster_name(s, i), name) == 0) break;
    if (i >= SHELL_MONSTERS) i = 0;
    h->state = 0;
    h->sub = 0;
    h->col = -1;
    h->row = i % 2;
    h->target = (float)(i / 2) * 130.0f;
    h->offset_x = -1;                                    /* [R: fOffset = -1, the sheet then at x 0] */
    h->prev_target = -1;
    h->valid = -1;
    h->left_edge = 0;
    h->right_edge = 3 * 130.0f;
    if (h->selrect) h->selrect->p[1] = (float)h->row * 170.0f;
    if (h->offset) h->offset->p[0] = 0;
    sheet_positions(s);
    sheet_update_selection(s, 1);
    set_fade(h->arrows, 1);
    set_fade(h->cards, 1);
    set_fade(h->blowup, 0);
}

static void sheet_create_card(Shell *s, Node *item, const char *name)   /* createCard */
{
    Node *head = find(item, "head"), *base = find(item, "base"), *text = find(item, "name");
    Node *stripe = find(item, "stripe"), *ups = find(item, "small_upgrades");
    char t[64];
    if (strcmp(name, "Blank") == 0) {
        float red[4] = { 1, 0, 0, 1 };
        set_tex(&s->gui, head, "card_empty");
        set_col(head, red);
        show(base, 0);
        show(stripe, 0);
        node_localize(&s->gui, text, name);
        show(text, 0);
        show(find(item, "black"), 0);
        show(ups, 0);
    } else {
        snprintf(t, sizeof t, "%s_head", name);
        set_tex(&s->gui, head, t);
        node_localize(&s->gui, text, name);
        show(stripe, 1);
        show(text, 1);
        show(ups, 1);
    }
}

static void sheet_card_face(Shell *s, const char *name)   /* setupCardFace */
{
    MonsterSheet *h = &s->sh;
    int up = monster_flags(s, h->controller, h->selection);
    char t[64];
    snprintf(t, sizeof t, "%s_head", name);
    set_tex(&s->gui, find(h->blowup, "head"), t);
    node_localize(&s->gui, find(h->blowup, "name"), name);
    Node *fu = find(h->blowup, "front_upgrades");
    for (int i = 0; i < 4; i++)
        show(child_at(fu, i), (up & (1 << i)) != 0);
    Node *card = find(h->blowup, "backside");
    Node *cb = find(card, "card_back_name"), *face = find(card, "card_back_face");
    Node *climb = find(find(card, "ability0"), "slider"), *jump = find(find(card, "ability1"), "slider");
    Node *run = find(find(card, "ability2"), "slider"), *mut = find(card, "mutation");
    if (!cb || !climb || !jump || !run || !face || !mut)
        return;
    node_localize(&s->gui, cb, name);
    snprintf(t, sizeof t, "MS_MUTATION_%s", name);
    const char *a = loc_find(&s->gui, "MS_MUTATION:"), *b = loc_find(&s->gui, t);
    char both[MAX_TEXT];
    snprintf(both, sizeof both, "%s %s", a ? a : "MS_MUTATION:", b ? b : t);
    set_text(mut, both);
    snprintf(t, sizeof t, "%s_Head", name);
    set_tex(&s->gui, face, t);
    climb->val = monster_prop(s, h->selection, "Climb", 0);
    jump->val = monster_prop(s, h->selection, "Jump", 0);
    run->val = monster_prop(s, h->selection, "Run", 0);
    float yellow[4], grey[4];
    col_255(yellow, 223, 203, 31, 255);
    col_255(grey, 128, 128, 128, 255);
    Node *bu = find(h->blowup, "back_upgrades");
    for (int i = 0; i < 4; i++) {
        Node *ut = findf(bu, "upgrade%d", i), *star = find(ut, "star_flat");
        set_col(ut, (up & (1 << i)) ? yellow : grey);
        set_col(star, (up & (1 << i)) ? yellow : grey);
    }
}

static void sheet_state_sheet(Shell *s, int ev, int button, float dt)   /* processStateSheet */
{
    MonsterSheet *h = &s->sh;
    if (ev == EV_FRAME) {                                /* processMovement: the stick moves the sheet */
        if (s->held_x >= 0.05f || s->held_x <= -0.05f) {
            h->target += s->held_x * 16.0f * (dt / TICK);
            sheet_positions(s);
        }
    } else if (ev == EV_BACK) {
        sound(s, "Cancel");
        cr_show(s, IF_MONSTERSELECT);
    } else if (ev == EV_ACCEPT) {
        if (h->valid) {
            sound(s, "Click");
            ms_monster_from_sheet(s, monster_name(s, h->selection));
            cr_show(s, IF_MONSTERSELECT);
        }
    } else if (ev == EV_BUTTON) {
        if (h->valid && button == 1) {
            sound(s, "Click");
            h->sub = 3;
            h->state = 2;
            h->zoom = 0;
            sheet_card_face(s, monster_name(s, h->selection));
            node_localize(&s->gui, find(ifc_root(s, IF_MONSTERSHEET), "nb_grow"), "Button_ZoomOut");
        }
    } else if (ev == EV_LEFT || ev == EV_RIGHT) {
        if (s->held_x >= -0.55f && s->held_x <= 0.55f) {
            if (ev == EV_LEFT)
                h->target = h->target >= 0 ? h->target - 130.0f + 129.0f : h->target - 130.0f;
            else
                h->target = h->target >= 0 ? h->target + 130.0f : h->target + 130.0f - 129.0f;
            h->target = (float)(((int)h->target / 130) * 130);
            sheet_positions(s);
        }
    } else if (ev == EV_UP || ev == EV_DOWN) {
        int prev = h->row;
        h->row = ev == EV_UP ? 0 : 1;
        if (prev != h->row) {
            if (h->selrect) h->selrect->p[1] = (float)h->row * 170.0f;
            sheet_update_selection(s, 1);
        }
    }
}

static void sheet_event(Shell *s, int ev, int button, int pad, float dt)
{
    MonsterSheet *h = &s->sh;
    Node *r = ifc_root(s, IF_MONSTERSHEET);
    h->sound_played = 0;
    if (ev == EV_INIT) {
        if (h->initialized)
            return;
        h->initialized = 1;
        fix_buttons(r, "nb_select", "nb_back", "nb_flip", "nb_grow");
        for (int i = 0; i < 3; i++)
            h->in_use[i] = 0;                            /* [R] an int[3] starts {0, 0, 0} (TypePInt's 0x1CDA40), so
                                                          * George is dark until the first 512's reset */
        h->state = h->sub = 0;
        h->cards = find(r, "cards");
        h->blowup = find(r, "blowup");
        h->arrows = find(r, "arrows");
        set_fade(h->cards, 1); show(h->cards, 1);
        set_fade(h->blowup, 0); show(h->blowup, 1);
        set_fade(h->arrows, 1);
        h->scale_base = h->blowup ? h->blowup->m : 1;
        h->offset = find(h->cards, "offset");
        h->selrect = find(r, "selectRect");             /* setPulse(1): its F 0x400 blinks [G] */
        h->arrow_r = find(h->arrows, "ArrowR");
        h->arrow_l = find(h->arrows, "ArrowL");
        Node *tpl = find(r, "card_template"), *row0 = find(r, "row0"), *row1 = find(r, "row1");
        for (int i = 0; i + 1 < SHELL_MONSTERS; i += 2)
            for (int k = 0; k < 2; k++) {
                Node *item = node_copy(s->ifc[IF_MONSTERSHEET], tpl, k ? row1 : row0);
                if (!item) continue;
                sheet_create_card(s, item, monster_name(s, i + k));
                set_pos(item, (float)(i / 2) * 130.0f, 0, 0);
                show(item, 1);
                if (!sheet_available(s, i + k))
                    show(find(item, "black"), 1);
            }
        h->right_bounds = 130.0f * (SHELL_MONSTERS / 2 - 1);
        h->left_edge = 0;
        h->right_edge = 3 * 130.0f;
        sheet_select_card(s, monster_name(s, s->gs.monster[0]));
        return;
    }
    if (ev == EV_GAIN) {
        if (!h->initialized)
            return;
        Node *row0 = find(r, "row0"), *row1 = find(r, "row1");
        for (int i = 0; i < SHELL_MONSTERS / 2; i++)
            for (int k = 0; k < 2; k++) {
                Node *item = child_at(k ? row1 : row0, i);
                show(find(item, "black"), !sheet_available(s, i * 2 + k));
                show(find(item, "facePlayerId"), 0);
                sheet_mini_upgrades(s, item, i * 2 + k);
            }
        for (int i = 0; i < 3; i++) h->in_use[i] = -1;
        static const float ids[4][3] = { { 2, 198, 27 }, { 213, 94, 43 }, { 0, 150, 255 }, { 255, 70, 163 } };
        int used = 0, np = s->gs.num_players, nb = s->gs.num_bots;
        for (int i = 0; i < np && i < 4; i++) {
            if (i == h->map_controller) continue;
            int mid = ms_player_selection(s, i);
            if (mid < 0) continue;
            if (used < 3) h->in_use[used++] = mid;
            Node *item = child_at(findf(r, "row%d", mid % 2), mid / 2);
            show(find(item, "black"), 1);
            Node *pid = find(item, "facePlayerId");
            float c[4];
            col_255(c, ids[i][0], ids[i][1], ids[i][2], 255);
            show(pid, 1);
            set_col(pid, c);
            set_fade(pid, 0.35f);
            char key[16];
            if (i < np - nb) snprintf(key, sizeof key, "P%d", i + 1);
            else snprintf(key, sizeof key, "Bot%d", 1 + i - (np - nb));
            node_localize(&s->gui, pid, key);
        }
        Node *gtp = find(r, "playerId");
        char key[16];
        int mc = h->map_controller;
        if (mc < np - nb || nb == 0) snprintf(key, sizeof key, "P%d", mc + 1);
        else snprintf(key, sizeof key, "Bot%d", mc - (np - nb) + 1);
        node_localize(&s->gui, gtp, key);
        float c[4];
        col_255(c, ids[mc & 3][0], ids[mc & 3][1], ids[mc & 3][2], 255);
        set_col(gtp, c);
        int mode = s->gs.play_type > 2 ? s->gs.play_type : 2;
        node_localize(&s->gui, find(r, "play_mode"), PLAY_MODE_TITLES[mode - 2]);
        node_localize(&s->gui, find(r, "nb_grow"), "Button_ZoomIn");
        sheet_update_selection(s, 0);
        return;
    }
    if (ev != EV_FRAME && pad != h->controller)
        return;
    if (h->state == 0)
        sheet_state_sheet(s, ev, button, dt);
    else if (h->state == 1) {                           /* processStateCard */
        if (ev == EV_FRAME)
            spin_step(&h->spin, &h->spin_amount, h->blowup, h->scale_base, dt);
        else if (ev == EV_BACK) {
            sound(s, "Cancel");
            cr_show(s, IF_MONSTERSELECT);
        } else if (ev == EV_ACCEPT) {
            sound(s, "Click");
            ms_monster_from_sheet(s, monster_name(s, h->selection));
            cr_show(s, IF_MONSTERSELECT);
        } else if (ev == EV_BUTTON && button == 0) {
            sound(s, "Click");
            h->spin_amount = 0;                          /* flipCard */
            if (h->spin == 0 || h->spin >= 6) {
                effect_stop(find(h->blowup, "card_effect"));
                h->spin = 1;
                h->spin_amount = 1;
            }
            else if (h->spin == 3) { h->spin = 4; h->spin_amount = 1; }
        } else if (ev == EV_BUTTON && button == 1) {
            sound(s, "Click");
            h->state = 2;
            h->sub = 4;
            h->zoom = 0;
            node_localize(&s->gui, find(r, "nb_grow"), "Button_ZoomIn");
        }
    } else if (h->state == 2 && ev == EV_FRAME) {       /* processStateZoom: 1 s */
        h->zoom += dt;
        if (h->zoom > 1) {
            h->zoom = 1;
            h->state = h->sub == 3 ? 1 : 0;
        }
        float z = h->zoom;
        if (h->sub == 3) {
            set_fade(h->arrows, 1 - z);
            set_fade(h->cards, fmaxf(1 - z, 0.2f));
            set_fade(h->blowup, z);
            set_fade(find(r, "nb_flip"), fmaxf(z, 0.25f));
        } else {
            set_fade(h->arrows, z);
            set_fade(h->cards, fmaxf(z, 0.2f));
            set_fade(h->blowup, 1 - z);
            set_fade(find(r, "nb_flip"), fmaxf(1 - z, 0.25f));
        }
    }
}

/* ---------------------------------------------------------------- CitySelect.scr [R] */
static void cs_day_night_globe(Shell *s)               /* updateDayNightGlobe: the globe's light, and the text */
{
    CitySel *c = &s->cs;
    Node *l = find(ifc_root(s, IF_CITYSELECT), "lights");
    if (l) {
        float v = c->night ? 100.0f / 255.0f : 1.0f, g = (c->night ? 5.0f : 55.0f) / 255.0f;
        for (int i = 0; i < 3; i++)
            l->amb[i] = l->dif[i] = v;
        l->spec[0] = l->spec[2] = 0;
        l->spec[1] = g;
    }
    node_localize(&s->gui, c->time_text, c->night ? "CitySelect_TOD_Night" : "CitySelect_TOD_Day");
}

/* updateGlobe(dt): lookAt goes 0.05 (radians) a frame along moveDir while that brings it nearer to the city's; within
 * 0.5 of it, the city's ring starts */
static void cs_globe(Shell *s, float dt)
{
    CitySel *c = &s->cs;
    float d[3], len = 0;
    for (int i = 0; i < 3; i++) {
        d[i] = c->look[i] - c->target[i];
        len += d[i] * d[i];
    }
    len = sqrtf(len);
    if (len <= 0.01f && dt != 0.0f)
        return;
    float la[3], l2 = 0;
    for (int i = 0; i < 3; i++) {
        la[i] = c->look[i] + c->dir[i] * 0.05f;
        l2 += (la[i] - c->target[i]) * (la[i] - c->target[i]);
    }
    if (sqrtf(l2) < len) {
        memcpy(c->look, la, sizeof la);
        node_look_at(c->globe, c->look);
    }
    if (!c->ring_on && len <= 0.5f) {
        c->ring_on = 1;
        effect_start(c->rings[c->index]);
    }
}

static void cs_day_night(Shell *s, int toggle)         /* updateDayNight */
{
    CitySel *c = &s->cs;
    if (toggle) c->night = !c->night;
    cs_day_night_globe(s);
    for (int i = 0; i < SHELL_CITIES; i++)
        show(find(child_at(c->city_bar, i), "night"), c->night);
}

static void cs_cheat_text(Shell *s)                    /* updateCheatText */
{
    CitySel *c = &s->cs;
    Node *t = find(ifc_root(s, IF_CITYSELECT), "_cheatJumpBlock");
    if (!t)
        return;
    c->cheat_block = (s->cheats & 0x80000000u) ? s->cheat_param_block + 1 : 0;   /* getCheatParam(0x80000000) + 1 */
    int n = (int)group_num(city_group(s, c->index), "NumBlocks", 10);
    if (c->cheat_block > n) c->cheat_block = n;
    if (c->cheat_block > 0) {
        char b[64];
        snprintf(b, sizeof b, "[Cheat] Jumping to block #%d", c->cheat_block);
        set_text(t, b);
        show(t, 1);
    } else
        show(t, 0);
}

static void cs_selection(Shell *s, int dir)            /* updateSelection */
{
    CitySel *c = &s->cs;
    int prev = c->index;
    int i = c->index + dir;
    if (i < 0) i = 0;
    if (i > SHELL_CITIES - 1) i = SHELL_CITIES - 1;
    if (i != c->index || dir == 0) {
        if (i < c->left) c->left = i;
        else if (i > c->left + 3) c->left = i - 3;
        if (c->city_rect) c->city_rect->p[0] = (float)i * 128.0f;
        if (c->city_bar) c->city_bar->p[0] = (float)c->left * -128.0f;
        c->index = i;
        show(c->arrow_l, c->left > 0);
        show(c->arrow_r, c->left < 3);
        for (int k = 0; k < SHELL_CITIES; k++)
            show(child_at(c->city_bar, k), k >= c->left && k < c->left + 4);
        /* the globe turns to CityData.City<i>.GlobeLocation (degrees: lat, lon, 0), from where it is now */
        Group gl = group_sub(city_group(s, i), "GlobeLocation");
        Record loc[3];
        int nl = gl.at ? (int)group_items(gl, loc, 3) : 0;
        float len = 0;
        for (int k = 0; k < 3; k++) {
            c->target[k] = k < nl ? record_num(loc[k]) * SH_PI / 180.0f : 0;
            c->dir[k] = c->target[k] - c->look[k];
            len += c->dir[k] * c->dir[k];
        }
        len = sqrtf(len);
        for (int k = 0; k < 3 && len > 0; k++)
            c->dir[k] /= len;
        if (dir != 0)
            effect_stop(c->rings[prev]);
        c->ring_on = 0;
        int flags = s->prof.city_flags[i];
        if (s->cheats & 2) flags |= 128;
        float yellow[4], grey[4];
        col_255(yellow, 255, 255, 0, 255);
        col_255(grey, 75, 75, 75, 255);
        Node *st = find(c->stats, "city_stats");
        for (int f = 0; f < 4; f++)
            set_col(find(child_at(st, f), "star"), (flags & (1 << f)) ? yellow : grey);
        if (s->gs.play_type == 2) {
            c->locked = (flags & 128) == 0;
            if (c->stats_visible)
                fader_go(&c->fd, c->stats, c->locked ? -1 : 1);
            set_fade(c->select_btn, 1.0f - (float)c->locked * 0.5f);
            set_fade(c->stats_btn, 1.0f - (float)c->locked * 0.5f);
            c->night = (int)group_num(city_group(s, i), "CampaignNight", 0);
            cs_day_night_globe(s);
        } else {
            c->locked = 0;
            set_fade(c->select_btn, 1);
            set_fade(c->stats_btn, 0);
        }
    }
    cs_cheat_text(s);
}

static void cs_gain(Shell *s, int pad)                 /* gainFocus */
{
    CitySel *c = &s->cs;
    if (c->initialized == 1) {
        c->initialized = 2;
        c->index = s->gs.city_index;
        c->left = c->index - 3 < 0 ? 0 : c->index - 3;
        c->stats_visible = 0;
        int campaign = s->gs.play_type == 2;
        fader_init(&c->fd, c->stats, 0);
        snprintf(c->msg_key, sizeof c->msg_key, "%s",
                 s->gs.num_players > 1 ? "CitySelect_Message_Plural" : "CitySelect_Message_Single");
        c->night = s->gs.night;
        float yellow[4], grey[4];
        col_255(yellow, 255, 255, 0, 255);
        col_255(grey, 75, 75, 75, 255);
        for (int i = 0; i < SHELL_CITIES; i++) {
            Node *city = find(c->city_bar, city_name(s, i));
            if (!city) continue;
            char ring[48];
            snprintf(ring, sizeof ring, "fx_%s", city_name(s, i));
            c->rings[i] = find(ifc_root(s, IF_CITYSELECT), ring);
            Node *stars = find(city, "stars");
            int flags = s->prof.city_flags[i];
            if (s->cheats & 2) {
                show(stars, 1);
                show(find(city, "locked"), 0);
            } else if (!campaign) {
                show(find(city, "locked"), 0);
                show(stars, 0);
                show(c->stats, 0);
            } else if (!(flags & 128)) {
                show(find(city, "locked"), 1);
                show(stars, 0);
            } else {
                show(find(city, "locked"), 0);
                show(stars, 1);
            }
            for (int f = 0; f < 4; f++)
                set_col(child_at(stars, f), (flags & (1 << f)) ? yellow : grey);
        }
        Node *tod = find(ifc_root(s, IF_CITYSELECT), "timeofday");
        show(child_at(tod, 0), !campaign);
        show(child_at(tod, 1), !campaign);
        cs_selection(s, 0);
        memcpy(c->look, c->target, sizeof c->look);      /* lookAt = lookAtTarget at once */
        node_look_at(c->globe, c->look);
        cs_globe(s, 0);
        if (campaign)
            for (int i = 0; i < SHELL_CITIES; i++)
                show(find(child_at(c->city_bar, i), "night"), (int)group_num(city_group(s, i), "CampaignNight", 0));
        else
            cs_day_night(s, 0);
    }
    if (pad == 0)
        cs_cheat_text(s);
}

static void cs_event(Shell *s, int ev, int button, int pad, float dt)
{
    CitySel *c = &s->cs;
    Node *r = ifc_root(s, IF_CITYSELECT);
    if (ev == EV_INIT && !c->initialized) {
        c->initialized = 1;
        fix_buttons(r, "nb_select", "nb_back", "nb_stats", "");
        show(find(r, "-.line.-"), 0);
        c->jumped = 0;
        c->globe = find(r, "globe");                     /* setIdentity, setVisible(1) */
        node_identity(c->globe);
        show(c->globe, 1);
        memset(c->look, 0, sizeof c->look);
        c->ring_on = 0;
        c->select_btn = find(r, "nb_select");
        c->stats_btn = find(r, "nb_stats");
        c->news = find(r, "movingtext");
        show(c->news, 0);
        c->flash = 0;
        c->flash_city = -1;
        c->stats = find(r, "city_stats");
        c->city_bar = find(r, "city_bar");
        c->city_rect = find(r, "city_rect");
        c->index = 0;
        set_pos(c->city_bar, 0, 0, 0);
        set_pos(c->city_rect, 0, 0, 0);
        c->arrow_l = find(r, "arrowleft");
        c->arrow_r = find(r, "arrowright");
        c->time_text = find(r, "timetext");
        c->night = s->gs.night;
        snprintf(c->msg_key, sizeof c->msg_key, "CitySelect_Message_Single");
        /* the time of day's text right-aligned at 180 by the wider of its two texts */
        char keep[MAX_TEXT];
        snprintf(keep, sizeof keep, "%s", c->time_text ? c->time_text->text : "");
        node_localize(&s->gui, c->time_text, "CitySelect_TOD_Day");
        float dw = text_w(&s->gui, c->time_text);
        node_localize(&s->gui, c->time_text, "CitySelect_TOD_Night");
        float nw = text_w(&s->gui, c->time_text);
        set_text(c->time_text, keep);
        set_pos(c->time_text, 180.0f - (nw > dw ? nw : dw), 115.0f, 0);
    } else if (ev == EV_LOSE) {
        if (c->initialized == 2) {                       /* loseFocus: the ring effect stops */
            effect_stop(c->rings[c->index]);
            c->initialized = 1;
        }
    } else if (ev == EV_GAIN)
        cs_gain(s, pad);
    else if (ev == EV_FRAME) {
        cs_globe(s, dt);
        fader_update(&c->fd, dt);
        c->flash -= dt;
        if (c->flash <= 0) {
            if (c->flash_city != c->index) {             /* "...A MONSTER IS HEADING TOWARDS %s..." in capitals */
                const char *fmt = loc_find(&s->gui, c->msg_key), *city = loc_find(&s->gui, city_name(s, c->index));
                char msg[MAX_TEXT];
                snprintf(msg, sizeof msg, fmt ? fmt : "%s", city ? city : city_name(s, c->index));
                for (char *p = msg; *p; p++)
                    if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
                set_text(c->news, msg);
            }
            if (c->news && c->news->visible) {
                show(c->news, 0);
                c->flash = 0.25f;
            } else {
                show(c->news, 1);
                c->flash = 1.0f;
            }
        }
    } else if (ev == EV_UP || ev == EV_DOWN) {
        if (s->gs.play_type != 2) {
            cs_day_night(s, 1);
            sound(s, "Rollover");
        }
    } else if (ev == EV_LEFT || ev == EV_RIGHT) {
        int prev = c->index;
        cs_selection(s, ev == EV_RIGHT ? 1 : -1);
        if (c->index != prev)
            sound(s, "Rollover");
    } else if (ev == EV_ACCEPT) {
        if (!c->locked) {
            s->gs.city_index = c->index;
            snprintf(s->gs.city, sizeof s->gs.city, "%s", city_name(s, c->index));
            s->gs.night = c->night;
            if (c->cheat_block > 0) s->gs.start_block = c->cheat_block;
            else if (s->gs.play_type != 2) s->gs.start_block = 0;
            else s->gs.start_block = s->prof.start_block[c->index];
            cr_start_game(s);
            sound(s, "Click");
        }
    } else if (ev == EV_BACK) {
        if (c->jumped)
            cr_show(s, IF_MAINMENU);
        else {
            ms_setup_from_main_menu(s);
            cr_show(s, IF_MONSTERSELECT);
        }
        sound(s, "Cancel");
    } else if (ev == EV_BUTTON && button == 0) {
        if (!c->locked && s->gs.play_type == 2) {
            sound(s, "Click");
            c->stats_visible = !c->stats_visible;
            fader_go(&c->fd, c->stats, c->stats_visible ? 1 : -1);
        }
    }
}

/* ---------------------------------------------------------------- the ControlRoom (ControlRoom.scr) [R] */
static void ifc_event(Shell *s, int which, int ev, int button, int pad, float dt)
{
    if (which < 0 || which >= IF_COUNT || !s->ifc[which])
        return;
    switch (which) {
    case IF_ATTRACT: am_event(s, ev, pad, dt); break;
    case IF_MAINMENU: mm_event(s, ev, button, pad, dt); break;
    case IF_MONSTERSELECT: ms_event(s, ev, button, pad, dt); break;
    case IF_MONSTERSHEET: sheet_event(s, ev, button, pad, dt); break;
    case IF_CITYSELECT: cs_event(s, ev, button, pad, dt); break;
    case IF_OPTIONS: op_event(s, ev, pad, dt); break;
    }
}

/* GuiScreen.setFocus (0x238970) [R]: the old one loses it (1024, not when it is the new one), the slot is stored,
 * then the new one gains it (512, even when it had it) */
static void cr_focus(Shell *s, int which)
{
    int old = s->focus;
    if (old != which)
        ifc_event(s, old, EV_LOSE, 0, 0, 0);
    s->focus = which;
    ifc_event(s, which, EV_GAIN, 0, 0, 0);
}

/* ControlRoom::showInterface: the focused one fades out, the other in, and takes the focus */
static void cr_show(Shell *s, int which)
{
    fader_go(&s->cr_fd, ifc_root(s, s->focus), -1);
    fader_go(&s->cr_fd, ifc_root(s, which), 1);
    cr_focus(s, which);
}

/* ControlRoom::startGame: the focused interface gone at once, the loading screen */
static void cr_start_game(Shell *s)
{
    set_fade(ifc_root(s, s->focus), 0);
    show_loading(s, 0);
}

/* Shell::onInit's ControlRoom: its six interfaces (each one's 8192), then its own 8192: every one hidden but the first
 * one, and that one's focus */
static void cr_open(Shell *s)
{
    if (s->cr_on || !s->have_shell)
        return;
    s->cr_on = 1;
    s->focus = -1;
    s->ctrl_mask = 1;                                    /* gShell.setupControllerTable(1) */
    for (int i = 0; i < IF_COUNT; i++) {
        char name[32];
        snprintf(name, sizeof name, "%s.ifc", IFC_NAMES[i]);
        s->ifc[i] = layout_load(&s->gui, &s->b_shell, name, i == IF_MONSTERSHEET ? 30 * 24 : 16);
    }
    for (int i = 0; i < IF_COUNT; i++)
        ifc_event(s, i, EV_INIT, 0, 0, 0);
    for (int i = 0; i < IF_COUNT; i++)
        fader_init(&s->cr_fd, ifc_root(s, i), 0);
    int active = IF_MAINMENU, play_music = 1;
    if (s->state == 1 || s->state == 2) {
        active = IF_ATTRACT;
        play_music = 0;
    } else if (s->quitting)
        active = IF_MAINMENU;
    else if (s->choose_monster)
        active = IF_MONSTERSELECT;
    else if (s->city_destroyed)
        active = IF_CITYSELECT;
    if (active == IF_MONSTERSELECT || active == IF_CITYSELECT)
        controller_table(s, -1);
    fader_init(&s->cr_fd, ifc_root(s, active), 1);
    if (play_music)
        music(s, "Oingo");
    cr_focus(s, active);
}

/* the idle timer (0x308710 / 0x308390): 120 s without the pad in the menus -> the attract mode [R]. It holds at 120
 * in states 6 and 7, in the campaign while the monster is to be chosen or the city is destroyed and not quit, and in
 * state 1 unless the memory card popup is up; a cheat movie set (setCheatFlagsI(16, n): 0x4D7E6C >= 0) runs it out at
 * once (0x3087D8). With the popup up it is not reset: at 0 an interruptible popup (the boot's list, before a profile
 * is chosen) is closed and the attract mode starts (0x341790, 0x345540(popup, 3)), any other waits at 0 */
static void idle_update(Shell *s, float dt)
{
    if (!s->cr_on || s->ld.on || s->vi.on || s->state == 2 || s->state == 4)
        return;
    int popup = s->mc.mode != MC_OFF;
    if (s->state == 6 || s->state == 7 || (s->state == 1 && !popup) || s->focus == IF_OPTIONS ||
        (s->gs.play_type == 2 && (s->choose_monster || (s->city_destroyed && !s->quitting)))) {
        s->idle = IDLE_SECONDS;
        return;
    }
    if (s->cheat_param_video >= 0)
        s->idle = 0;
    s->idle -= dt;
    if (s->idle > 0)
        return;
    if (popup) {
        s->idle = 0;
        /* [R 0x341790] the autosave notice, or the list while show-profile is on */
        if (!((s->mc.mode == MC_MESSAGE && s->mc.msg == MSG_AUTOSAVE) || (s->mc.mode == MC_LIST && s->show_profile)))
            return;
        s->mc.mode = MC_OFF;                             /* result 3: show-profile stays */
        if (s->mc.L)
            show(s->mc.L->root, 0);
    }
    s->idle = IDLE_SECONDS;
    s->city_destroyed = s->quitting = s->choose_monster = 0;
    s->state = 2;
    s->attract_state = -1;
    if (s->cheat_param_video >= 0) {
        s->attract_state = 128 + s->cheat_param_video;
        s->cheat_param_video = -1;
    }
    cr_show(s, IF_ATTRACT);
}

/* ---------------------------------------------------------------- the API */
static int load_bundle(const Vol *vol, const char *name, Bundle *b)
{
    uint32_t size = 0;
    uint8_t *data = vol_read(vol, name, &size);
    if (!data)
        return 0;
    if (bdp_load(b, data, size) != 0) {
        free(data);
        return 0;
    }
    return 1;
}

Shell *shell_open(const Vol *vol, const ShellStart *st, const ShellProfile *p)
{
    if (!vol)
        return NULL;
    Shell *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->vol = vol;
    s->quads = calloc(MAX_QUADS, sizeof *s->quads);
    s->have_shell = load_bundle(vol, "Shell.bdp", &s->b_shell);
    s->have_vshell = load_bundle(vol, "VShell.bdp", &s->b_vshell);
    s->have_data = load_bundle(vol, "ShellData.bdp", &s->b_data);
    s->have_common = load_bundle(vol, "CommonGUI.bdp", &s->b_common);
    s->have_mc = load_bundle(vol, "MemoryCard.bdp", &s->b_mc);
    s->have_loading = load_bundle(vol, "Loading.bdp", &s->b_loading);
    if (!s->quads || !s->have_shell || !s->have_data || !s->have_common) {
        shell_close(s);
        return NULL;
    }
    Bundle *order[6] = { &s->b_shell, s->have_vshell ? &s->b_vshell : NULL, &s->b_common,
                         s->have_mc ? &s->b_mc : NULL, s->have_loading ? &s->b_loading : NULL, &s->b_data };
    for (int i = 0; i < 6; i++)
        if (order[i]) s->gui.bundles[s->gui.nb++] = order[i];
    if (book_open(&s->book, &s->b_data) == 0) {          /* gShell.loadPropertyBook("ShellData") */
        s->monster_data = book_group(&s->book, "MonsterData");
        s->city_data = book_group(&s->book, "CityData");
    }
    if (p) s->prof = *p;
    else {
        s->prof.sound = 0.5f;                            /* a new profile's (0x33F630) [R] */
        s->prof.music = 0.25f;
        s->prof.rumble = 1;
        for (int k = 0; k < 4; k++) s->prof.monster_flags[k][0] = s->prof.monster_flags[k][1] = 0x80;
        s->prof.city_flags[0] = 0x80;
    }
    s->gui.lang = s->prof.language;
    s->cheats = s->prof.cheats;
    s->cheat_param_block = -1;
    s->cheat_param_video = -1;
    s->platform = 2;                                     /* gApp.getPlatform(): the PS2 [G: the value 2] */
    s->region = 0;
    s->idle = IDLE_SECONDS;
    if (st) s->st = *st;
    s->gs = s->st.game;
    if (s->gs.num_players < 1) s->gs.num_players = 1;
    if (s->st.from == SHELL_FROM_BOOT && !st) {
        for (int i = 0; i < 4; i++) s->gs.monster[i] = i;   /* [G: GameStatus' first monsters] */
    }
    if (!s->gs.city[0])
        snprintf(s->gs.city, sizeof s->gs.city, "%s", city_name(s, s->gs.city_index));
    s->attract_state = -1;
    mc_load(s);
    if (s->st.from == SHELL_FROM_GAME) {
        /* RShellPackage 0x308950: back from the world, the boot is done */
        s->state = 3;
        s->boot_completed = 1;
        s->quitting = s->st.quit;
        s->choose_monster = s->st.choose_monster;
        s->city_destroyed = s->st.city_destroyed;
        if (s->gs.play_type == 4 && s->city_destroyed && !s->quitting && s->st.king_of_world == -1) {
            show_loading(s, 0);                          /* Shell::onInit: the King of the World's next city */
            return s;
        }
        if (s->st.show_victory) {
            s->state = 6;
            s->vi.on = 1;
            if (s->have_vshell) {
                s->vi.L = layout_load(&s->gui, &s->b_vshell, "Victory.ifc", 0);
                s->vi.video = s->vi.L ? find(s->vi.L->root, "Video") : NULL;
                if (s->vi.video) col_set(s->vi.video->col, 0, 0, 0, 1);
                if (s->vi.video) s->vi.video->video = 1;
                Node *ps = s->vi.L ? find(s->vi.L->root, "PressStart") : NULL;
                if (ps) ps->F |= 0x400;
            }
            music(s, "");
            victory_next(s);
            return s;
        }
    } else {
        s->state = 1;                                    /* 0x308950: state 0 -> 1, the memory card check */
    }
    s->show_profile = !s->prof.profile_shown;            /* [R 0x30697C: !(0x4D7FF8 & 0x200), clear in .bss] */
    cr_open(s);
    return s;
}

void shell_close(Shell *s)
{
    if (!s)
        return;
    for (int i = 0; i < IF_COUNT; i++)
        layout_free(s->ifc[i]);
    layout_free(s->mc.L);
    layout_free(s->ld.L);
    layout_free(s->vi.L);
    if (s->ld.have_b) bdp_free(&s->ld.b);
    if (s->have_shell) bdp_free(&s->b_shell);
    if (s->have_vshell) bdp_free(&s->b_vshell);
    if (s->have_data) bdp_free(&s->b_data);
    if (s->have_common) bdp_free(&s->b_common);
    if (s->have_mc) bdp_free(&s->b_mc);
    if (s->have_loading) bdp_free(&s->b_loading);
    free(s->quads);
    free(s);
}

int shell_profile_shown(const Shell *s) { return s && !s->show_profile; }

void shell_set_profile(Shell *s, const ShellProfile *p)
{
    if (!s || !p)
        return;
    s->prof = *p;
    s->gui.lang = p->language;
    s->cheats = p->cheats;
}

void shell_set_movie_time(Shell *s, float seconds) { if (s) s->movie_time = seconds > 0 ? seconds : 0; }

/* the playing movie's time left (Victory's or the attract mode's), NULL none */
static float *movie_left(Shell *s)
{
    if (s->vi.on)
        return s->vi.movie_on ? &s->vi.movie_left : NULL;
    return s->am.movie_on ? &s->am.movie_left : NULL;
}

void shell_movie_hold(Shell *s)
{
    float *left = s ? movie_left(s) : NULL;
    if (left) *left = 1e30f;
}

void shell_movie_end(Shell *s)
{
    float *left = s ? movie_left(s) : NULL;
    if (left) *left = 0;
}

float shell_movie_clock(const Shell *s) { return s && movie_left((Shell *)s) ? s->movie_clock : -1; }

void shell_movie_picture(Shell *s, int on) { if (s) s->gui.video_pic = on; }

void shell_input_pad(Shell *s, int ev, int button, int pad)
{
    if (!s)
        return;
    /* [R] the timer is reset (0x308240: gShell+0x18 = 120) by input the GUI takes: the screen's (0x2C5AD0, for a pad
     * the controller table allows, 0x2C5E60) or the popup's (0x2328B0) */
    if (s->mc.mode != MC_OFF || pad_allowed(s, pad))
        s->idle = IDLE_SECONDS;
    if (s->ld.on)
        return;
    if (s->vi.on) {
        if ((ev == EV_ACCEPT || ev == EV_START) && s->vi.movie_on) {
            s->vi.movie_on = 0;
            victory_next(s);
            if (!s->vi.on) cr_open(s);
        }
        return;
    }
    if (s->mc.mode != MC_OFF) {                          /* gCommon.isSaveGameActive(): the popup has the pad */
        if (pad == 0 || (s->mc.purpose == MCP_BUDDY && pad == s->mc.buddy))
            mc_input(s, ev, button);
        return;
    }
    if (!pad_allowed(s, pad))
        return;
    ifc_event(s, s->focus, ev, button, pad, 0);
}
void shell_input(Shell *s, int ev, int button) { shell_input_pad(s, ev, button, 0); }

void shell_pad_held(Shell *s, float stick_x, int buttons)
{
    if (!s)
        return;
    s->held_x = stick_x;
    s->held_buttons = buttons;
}

int shell_type(Shell *s, int ch)
{
    if (!s || s->mc.mode != MC_KEYBOARD)
        return 0;
    if (ch == '\b') {
        if (s->mc.len > 0) s->mc.name[--s->mc.len] = 0;
        mc_name_changed(s);
        sound(s, "Click");
        return 1;
    }
    if (ch == '\r' || ch == '\n') {
        mc_done(s);
        return 1;
    }
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '[' || ch == ']' ||
        ch == ' ') {
        mc_key_add(s, (char)ch);
        return 1;
    }
    return 0;
}

void shell_update(Shell *s, float dt)
{
    if (!s)
        return;
    if (dt < 0) dt = 0;
    s->gui.time += dt;
    if (s->vi.on) {
        if (s->vi.movie_on)
            s->movie_clock += dt;
        if (s->vi.movie_on && (s->vi.movie_left -= dt) <= 0) {
            s->vi.movie_on = 0;
            victory_next(s);
            if (!s->vi.on) cr_open(s);
        }
        return;
    }
    if (s->cr_on) {
        fader_update(&s->cr_fd, dt);
        /* the frame event goes to the interface with the focus alone [G; R in effect: Options.scr runs
         * MainMenu::updateObjectFader itself while it has the focus, so MainMenu gets no frames of its own then] */
        ifc_event(s, s->focus, EV_FRAME, 0, 0, dt);
        idle_update(s, dt);
    }
    loading_update(s, dt);
    for (int i = 0; i < IF_COUNT; i++)
        effects_update(s->ifc[i]);
    effects_update(s->mc.L);
    effects_update(s->ld.L);
}

int shell_events(Shell *s, ShellEvent *out, int max)
{
    if (!s)
        return 0;
    int n = s->nev < max ? s->nev : max;
    if (out && n > 0)
        memcpy(out, s->ev, (size_t)n * sizeof *out);
    s->nev = 0;
    return n;
}

const ShellGame *shell_game(const Shell *s) { return s ? &s->gs : NULL; }

void shell_settings(const Shell *s, float *sound_vol, float *music_vol, int *rumble)
{
    if (!s) return;
    if (sound_vol) *sound_vol = s->prof.sound;
    if (music_vol) *music_vol = s->prof.music;
    if (rumble) *rumble = s->prof.rumble;
}

int shell_state(const Shell *s) { return s ? s->state : 0; }

const char *shell_focus(const Shell *s)
{
    if (!s) return "";
    if (s->ld.on) return "Loading";
    if (s->vi.on) return "Victory";
    if (s->mc.mode != MC_OFF) return "MemoryCard";
    return s->focus >= 0 && s->focus < IF_COUNT ? IFC_NAMES[s->focus] : "";
}

/* the interfaces back to front: the ControlRoom's six in its layout's order, the profile popup over them (its own
 * screen), the loading screen over everything, or the Victory screen alone */
static void emit_all(const Shell *s, Emit *e)
{
    if (s->vi.on) {
        emit_layout(e, &s->gui, s->vi.L);
        return;
    }
    if (!s->ld.on || !s->ld.L || s->ld.intro < 3 || get_fade(s->ld.fill) < 1.0f)   /* hidden under the loading */
        for (int i = 0; i < IF_COUNT; i++)
            emit_layout(e, &s->gui, s->ifc[i]);
    if (s->mc.mode != MC_OFF)
        emit_layout(e, &s->gui, s->mc.L);
    if (s->ld.on)
        emit_layout(e, &s->gui, s->ld.L);
}

/* a shape's light set: <la>'s <ls> of the interface (0x22BDC0 finds it by name) [R]; a light's direction is row 2 of
 * its world (0x239D60), in the world's axes */
static void shape_lights(const Layout *L, const Node *nd, Shell3D *o)
{
    const Node *ls = NULL;
    for (int i = 0; L && nd->lights[0] && i < L->n && !ls; i++)
        if (L->nodes[i].fx == FX_LIGHTSET && !strcmp(L->nodes[i].name, nd->lights))
            ls = &L->nodes[i];
    o->lit = ls != NULL;                                 /* none: the lights off (0x16E320) */
    if (!ls)
        return;
    memcpy(o->ambient, ls->amb, sizeof o->ambient);
    for (const Node *lt = ls->child; lt && o->nlights < 3; lt = lt->next) {   /* the renderer's 3 (0x25AEB0) */
        if (lt->fx != FX_LIGHT)
            continue;
        M34 w = node_world3(lt);
        float d[3] = { w.r[6], w.r[8], -w.r[7] }, l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; k++) {
            o->light_dir[o->nlights][k] = l > 0 ? d[k] / l : 0;
            o->light_diffuse[o->nlights][k] = lt->dif[k];
            o->light_ambient[o->nlights][k] = lt->amb[k];
            o->light_specular[o->nlights][k] = lt->spec[k];
        }
        o->nlights++;
    }
}

int shell_3d(Shell *s, Shell3D *out, int max)
{
    if (!s || !out || max <= 0)
        return 0;
    const Node *d3[64];
    const void *l3[64];
    Emit e = { s->quads, 0, MAX_QUADS, d3, l3, 0, max < 64 ? max : 64, NULL };
    emit_all(s, &e);
    int n = 0;
    for (int k = 0; k < e.n3; k++) {
        Node *nd = (Node *)d3[k];
        const Layout *L = l3[k];
        Shell3D *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->kind = nd->fx == FX_SHAPE ? SHELL_3D_SHAPE : SHELL_3D_EFFECT;
        o->key = nd;
        o->layer = k;
        o->name = nd->res;
        o->b = L ? L->b : NULL;
        o->parent = -1;
        M34 w = node_world3(nd);
        if (nd->attach[0] && nd->parent && nd->parent->fx == FX_SHAPE)
            for (int j = 0; j < n - 1; j++)
                if (out[j].key == nd->parent) {          /* on the shape's node: there (0x2400F0, 0x24D8C0) */
                    o->parent = j;
                    o->attach = nd->attach;
                    M34 l = node_local3(nd), pw = node_world3(nd->parent);
                    w = m34_mul(&l, &pw);                /* [G: its own frame taken in the node's] */
                }
        gui_to_world(&w, o->world);
        memcpy(nd->w3, o->world, sizeof nd->w3);
        if (o->kind == SHELL_3D_SHAPE)
            shape_lights(L, nd, o);
        else
            nd->drawn++;
        o->running = (nd->eflags & FXE_RUN) != 0;
        o->stopping = (nd->eflags & FXE_SHUT) != 0;
        o->gen = nd->gen;
    }
    Layout *all[IF_COUNT + 3];                           /* the effects not drawn now: they run on, unseen */
    int na = 0;
    for (int i = 0; i < IF_COUNT; i++) all[na++] = s->ifc[i];
    all[na++] = s->mc.L;
    all[na++] = s->ld.L;
    all[na++] = s->vi.L;
    for (int i = 0; i < na && n < max; i++)
        for (int j = 0; all[i] && j < all[i]->n && n < max; j++) {
            Node *nd = &all[i]->nodes[j];
            if (nd->fx != FX_EFFECT || !nd->res[0] || !(nd->eflags & (FXE_RUN | FXE_STOP)))
                continue;
            int seen = 0;
            for (int k = 0; k < e.n3 && !seen; k++)
                seen = d3[k] == nd;
            if (seen)
                continue;
            Shell3D *o = &out[n++];
            memset(o, 0, sizeof *o);
            o->kind = SHELL_3D_EFFECT;
            o->key = nd;
            o->layer = -1;
            o->name = nd->res;
            o->b = all[i]->b;
            o->parent = -1;
            memcpy(o->world, nd->w3, sizeof o->world);
            o->running = (nd->eflags & FXE_RUN) != 0;
            o->stopping = (nd->eflags & FXE_SHUT) != 0;
            o->gen = nd->gen;
        }
    return n;
}

void shell_effect_ended(Shell *s, const void *key)
{
    Node *nd = (Node *)key;
    if (s && nd && nd->fx == FX_EFFECT)
        nd->eflags &= ~(unsigned)(FXE_RUN | FXE_SHUT);
}

int shell_quads_turned(const Shell *s, ShellQuad *out, int max)
{
    if (!s || !out || max <= 0)
        return 0;
    Emit e = { out, 0, max, NULL, NULL, 0, 0, NULL };
    if (s->vi.on) {
        emit_layout(&e, &s->gui, s->vi.L);
        return e.n;
    }
    if (!s->ld.on || !s->ld.L || s->ld.intro < 3 || get_fade(s->ld.fill) < 1.0f)   /* hidden under the loading */
        for (int i = 0; i < IF_COUNT; i++)
            emit_layout(&e, &s->gui, s->ifc[i]);
    if (s->mc.mode != MC_OFF)
        emit_layout(&e, &s->gui, s->mc.L);
    if (s->ld.on)
        emit_layout(&e, &s->gui, s->ld.L);
    return e.n;
}

/* the same as HudQuads: each one's bounds, the texture coordinates of the corners nearest the bounds' top-left and
 * bottom-right, and its own four corners (drawn from them, so a turned picture keeps its turn) */
int shell_quads(const Shell *s, HudQuad *out, int max)
{
    if (!s || !out)
        return 0;
    int n = shell_quads_turned(s, s->quads, max < MAX_QUADS ? max : MAX_QUADS);
    for (int i = 0; i < n; i++) {
        const ShellQuad *q = &s->quads[i];
        HudQuad *h = &out[i];
        float x0 = q->x[0], x1 = q->x[0], y0 = q->y[0], y1 = q->y[0];
        for (int k = 1; k < 4; k++) {
            x0 = fminf(x0, q->x[k]); x1 = fmaxf(x1, q->x[k]);
            y0 = fminf(y0, q->y[k]); y1 = fmaxf(y1, q->y[k]);
        }
        int tl = 0, br = 0;
        float btl = 1e30f, bbr = 1e30f;
        for (int k = 0; k < 4; k++) {
            float dtl = (q->x[k] - x0) * (q->x[k] - x0) + (q->y[k] - y0) * (q->y[k] - y0);
            float dbr = (q->x[k] - x1) * (q->x[k] - x1) + (q->y[k] - y1) * (q->y[k] - y1);
            if (dtl < btl) { btl = dtl; tl = k; }
            if (dbr < bbr) { bbr = dbr; br = k; }
        }
        h->x0 = x0; h->y0 = y0; h->x1 = x1; h->y1 = y1;
        h->u0 = q->u[tl]; h->v0 = q->v[tl]; h->u1 = q->u[br]; h->v1 = q->v[br];
        h->skew = 0;
        if (q->y[0] == q->y[1] && q->y[2] == q->y[3] && q->y[0] < q->y[3] && q->x[0] != q->x[3] &&
            fabsf((q->x[1] - q->x[0]) - (q->x[2] - q->x[3])) < 1e-3f) {   /* a slanted glyph: its bottom, its lean */
            h->x0 = q->x[3]; h->x1 = q->x[2];
            h->skew = q->x[0] - q->x[3];
            h->u0 = q->u[0]; h->v0 = q->v[0]; h->u1 = q->u[2]; h->v1 = q->v[2];
        }
        h->corners = 1;                              /* its own corners: a turned picture keeps its turn */
        memcpy(h->cx, q->x, sizeof h->cx);
        memcpy(h->cy, q->y, sizeof h->cy);
        memcpy(h->cu, q->u, sizeof h->cu);
        memcpy(h->cv, q->v, sizeof h->cv);
        memcpy(h->col, q->col, sizeof h->col);
        h->additive = q->additive;
        h->b = q->b;
        h->tex = q->tex;
    }
    return n;
}
