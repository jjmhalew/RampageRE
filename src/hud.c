/* hud.c - the game's 2D interface (hud.h; docs/FORMAT_GUI.md, docs/SCREENS_NOTES.md), transcribed from the PS2 game:
 * the GUI layout files (.ifc, bundle type 24: Hud0..3.ifc, Clock.ifc, Rage.ifc, ChallengeStatus.ifc,
 * ChallengeText.ifc, Results.ifc, LevelResults.ifc, WorldResults.ifc, KingBlockResults.ifc, KingCityResults.ifc,
 * KingWorldResults.ifc, TimedRunResults.ifc, GameOver.ifc, Pause.ifc of WorldGui.bdp), the logic of their scripts
 * (build/scripts/Hud.scr, Clock.scr, Rage.scr, ChallengeStatus.scr, ChallengeText.scr, Results.scr, LevelResults.scr,
 * WorldResults.scr, KingBlockResults.scr, KingCityResults.scr, KingWorldResults.scr, TimedRunResults.scr,
 * GameOver.scr, Pause.scr, WorldGuiDriver.scr and the
 * include files ObjectFader.h, ConfirmQuit.h, ButtonSetup.h), the GUI objects that draw them (GuiQuad 0x2439E0,
 * GuiSlider layout 0x24A310, GuiText 0x234D30), the bitmap fonts (OFontData loader 0x1B7CD0, OFont defaults 0x1B7200,
 * the line measure 0x1B5AA0 / 0x1B6270, the string draw 0x1B6A80 and its glyph quads 0x1B64B0), the string tables
 * (.loc, type 23: tools/loc.py), the floating score numbers (PointsBanner, vtable 0x44B6A0: start 0x331D50, advance
 * 0x331B00, draw 0x331960, font setup 0x2B8510) and the text banners (PointsBanner's script bindings 0x2A6A40:
 * createAndStartText 0x2B1640, createAndStartText2 0x2B14E0). [R] = read in that code or in the files, [G] = a guess
 * where it matters.
 *
 * The GUI is a tree of objects, each with a position p (x y z) and a scale m in its parent's frame [R: GuiObject's
 * 3x3 + position at +0x8 / +0x2C, the children's bounds go through the parent's matrix 0x240200]; a quad's sprite
 * spans its size d (or its texture's size) from the object's origin, or is centred on it (Q 0x2 across, 0x4 down,
 * 0x243C00), a slider is always centred (0x24A310), a text sits on its origin by its justification. A script holds
 * the objects it found by name and changes their position, scale, fade, colour, text and visibility; here each
 * interface is its own copy of its layout tree, changed the same way. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "hud.h"

#define HUD_PI 3.14159265f
#define MAX_NODES 128
#define MAX_FLOATS 64
#define MAX_TEXT 256
#define MAX_SORT 2048
#define MAX_FONTS 8
#define MAX_BUNDLES 8
#define MAX_LINES 16
#define MAX_RAGE 8
#define MAX_SOUNDS 32
#define MAX_FADERS 6
#define MAX_CALLS 48
#define RAGE_COPIES 15
#define MAX_INTRO 24                                     /* a screen's intro items (KingBlockResults: 20) */
#define MAX_WALK 12                                      /* its walkout, rank and alternative walkout items */

/* ---------------------------------------------------------------- colours */
/* the files write RRGGBBAA [R: Hud.scr's rageColor (1, 0.5, 0.25) is the layout's maskColor 0xFF8040FF] */
static void col_rgba(uint32_t c, float out[4])
{
    out[0] = (float)(c >> 24 & 255) / 255.0f;
    out[1] = (float)(c >> 16 & 255) / 255.0f;
    out[2] = (float)(c >> 8 & 255) / 255.0f;
    out[3] = (float)(c & 255) / 255.0f;
}
/* the engine's own colours are GS order, AABBGGRR (fonts, PointsBanner); alpha 255 = opaque [R: GuiText's fade scales
 * the top byte as n / 255, 0x234F6C] */
static void col_abgr(uint32_t c, float out[4])
{
    out[0] = (float)(c & 255) / 255.0f;
    out[1] = (float)(c >> 8 & 255) / 255.0f;
    out[2] = (float)(c >> 16 & 255) / 255.0f;
    out[3] = (float)(c >> 24 & 255) / 255.0f;
}
static void col_set(float out[4], float r, float g, float b, float a) { out[0] = r; out[1] = g; out[2] = b; out[3] = a; }

/* ---------------------------------------------------------------- the resources: bundles, fonts, texts */
typedef struct { const Bundle *b; uint32_t name; float w, h, us, vs; } TexRef;   /* w h: the used size */
typedef struct { int ok; float u0, u1, v0, v1, a, w, c, h; } Glyph;
typedef struct {
    int ok;
    TexRef tex;
    float cell_h, avg_h;        /* OFontData +0x24 (the texture's height / rows), +0x28 */
    int spacing, linespacing;   /* OFont +0x10, +0x11 */
    int shadow_dx, shadow_dy;   /* OFont +0x12, +0x13 */
    int slant;                  /* OFont +0x44 */
    uint32_t shadow_col, col;   /* OFont +0x18, +0x14 (AABBGGRR) */
    Glyph g[256];
} Font;
typedef struct {
    const Bundle *bundles[MAX_BUNDLES];
    int nb;
    Font fonts[MAX_FONTS];
    char font_names[MAX_FONTS][32];
    int nfonts;
    int lang;
    float time;                 /* seconds since hud_open: the list highlight's pulse */
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
        /* texture header: 0x08 u16 width, 0x0A height, 0x14 used width, 0x16 used height [R tools/tex.py]. The floats
         * at 0x18 / 0x1C are used / the GS's power-of-two size (156 / 256 for PointsBanner), the game's UV scale for
         * its GS texture; over the decoded image (tex_decode: width x height) the used part ends at used / size. */
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

/* OFontData::load 0x1B7CD0 [R]. File: 0x000 char[0x100] font name, 0x100 char[0x100] bitmap ("POINTSBANNER.BMP"),
 * 0x200 a Windows LOGFONT as text, 0x304 u32 rows of glyphs in the bitmap, 0x308 u32 size of the character list at
 * 0x358, 0x30C u32 size of the alias list after it, 0x310 f32 x4 character spacing, line spacing, shadow dx, dy,
 * 0x320 u32 shadow colour, 0x324 u32 colour, 0x32C f32 slant, 0x330 / 0x334 f32 x / y scale; then 0x24-byte glyphs
 * to the end: f32 u0 u1 v0 v1 (over the bitmap's used size), f32 a (before), w (drawn width), c (after), h (height,
 * < 0: the cell's), char[4] the character. */
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
    /* the bitmap: its file name without path or extension, looked up as a texture [R 0x1B7DA0..0x1B7EBC] */
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
    f->cell_h = f->tex.h / (float)(rows ? rows : 1);   /* [R 0x1B8050; G: the texture's height is its used height] */
    f->spacing = (int)rdf(d + 0x310);                   /* [R 0x1B7200: cvt.w.s, truncated] */
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
            continue;                                   /* a Shift-JIS pair: not drawn here */
        Glyph *g = &f->g[ch];
        g->ok = 1;
        g->u0 = rdf(d + r) * f->tex.us;                 /* [R 0x1B80E0: u by the texture's u scale, v by its v] */
        g->u1 = rdf(d + r + 4) * f->tex.us;
        g->v0 = rdf(d + r + 8) * f->tex.vs;
        g->v1 = rdf(d + r + 12) * f->tex.vs;
        g->a = rdf(d + r + 16);
        g->w = rdf(d + r + 20);
        g->c = rdf(d + r + 24);
        g->h = rdf(d + r + 28);
        if (g->h < 0)
            g->h = f->cell_h;
        /* the average height of the digits and lower case (else the capitals) [R 0x1B8144..0x1B8220] */
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z')) { lower_n += 1; lower_h += g->h; }
        else if (ch >= 'A' && ch <= 'Z') { upper_n += 1; upper_h += g->h; }
    }
    f->avg_h = lower_n > 0 ? lower_h / lower_n : upper_n > 0 ? upper_h / upper_n : f->cell_h;
    /* aliases: pairs (character, the character whose glyph it shares) [R 0x1B824C..0x1B8520]: Atrox60's lower case
     * is its capitals' */
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

/* a font by the name the layouts give (<g>ARIALBLACK24</g>): loaded once, from <name>.fdf in the bundles */
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

/* gApp.localizeString(key): the .loc tables (type 23) of the bundles [R tools/loc.py: 0x24 u32 count, 0x28 u32[11]
 * column offsets: the keys, then one column per language; each column count x u32 string offsets]. Keys are looked
 * up ignoring case ("Rampage" is the table's "RAMPAGE") [R 0x1D4C60: the tables from the last loaded back, each a
 * strcasecmp binary search (0x1D5320); an empty text tries the next]. */
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

/* ---------------------------------------------------------------- the draw list */
typedef struct { HudQuad *q; float *z; int n, max; int clip; float cx0, cx1; float skew; int additive; } Emit;

static void emit_quad(Emit *e, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                      const float col[4], const Bundle *b, uint32_t tex, float z)
{
    if (e->n >= e->max || col[3] <= 0.0f)
        return;
    if (e->clip) {                                       /* a mask's cut across (Results' scrolling title) */
        if (x1 <= e->cx0 || x0 >= e->cx1 || x1 <= x0)
            return;
        if (x0 < e->cx0) { u0 += (u1 - u0) * (e->cx0 - x0) / (x1 - x0); x0 = e->cx0; }
        if (x1 > e->cx1) { u1 -= (u1 - u0) * (x1 - e->cx1) / (x1 - x0); x1 = e->cx1; }
    }
    HudQuad *q = &e->q[e->n];
    q->x0 = x0; q->y0 = y0; q->x1 = x1; q->y1 = y1;
    q->u0 = u0; q->v0 = v0; q->u1 = u1; q->v1 = v1;
    q->skew = e->skew;
    q->additive = e->additive;
    q->corners = 0;
    memcpy(q->col, col, sizeof q->col);
    q->b = b;
    q->tex = tex;
    if (e->z)
        e->z[e->n] = z;
    e->n++;
}

/* ---------------------------------------------------------------- texts */
typedef struct { const char *s; int n; float w, h; } TLine;

/* "\v1.25Controller_Back\v": a picture in a .loc text (the upgrade hints' buttons) [G: OFont's special glyphs at
 * +0x25C are not traced; drawn square, the cell's height times the number]. s[0] is the first \v; returns the
 * characters it takes */
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

/* the line measure (0x1B5AA0): width = the glyphs' a + w + c + spacing, less one spacing, plus the slant; a character
 * without a glyph adds nothing; height = the tallest glyph's h, a line without glyphs the font's average */
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

/* the size of a text in font pixels, after the maxwidth squeeze (GuiText's width() and height() [G: by the text's own
 * scale, which the caller adds]) */
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

/* a string in font space mapped to the screen by (ox + sc * x, oy + sc * y). The game measures each line, shrinks the
 * whole text to maxwidth (0x1B6350: scale maxwidth / width when wider), places it by justify (0 left, 1 centre, 2
 * right: of each line, OFont +0x7 = 3 [R 0x1B6CB4..0x1B6E18]) and origin (0 top, 1 middle, 2 bottom of the text's
 * height [R 0x1B6B50]), then draws the shadow and the text (0x1B6EB8..0x1B6F38). col / shadow: rgba; sdx sdy: the
 * shadow's offset in font pixels. */
static void text_emit(Emit *e, const Gui *g, const TextStyle *ts, const char *s, float ox, float oy, float sc,
                      const float col[4], const float shadow[4], int sdx, int sdy, float z)
{
    const Font *f = ts->f;
    if (!f || !f->ok || !s || !*s)
        return;
    TLine L[MAX_LINES];
    int nl = text_lines(g, f, s, ts->spacing, ts->slant, ts->wrap, L, MAX_LINES);
    float wmax = 0, hsum = 0;
    for (int i = 0; i < nl; i++) {
        if (L[i].w > wmax) wmax = L[i].w;
        hsum += L[i].h;
    }
    float k = 1.0f;                                     /* OFont +0xC x +0x48 / +0x4C are 1 in every font file */
    if (ts->maxwidth > 0 && wmax > ts->maxwidth)
        k = ts->maxwidth / wmax;
    float total_h = hsum * k + ts->linesp * (float)(nl - 1);
    float y = ts->origin == 1 ? -0.5f * total_h : ts->origin == 2 ? -total_h : 0.0f;
    float cell = f->cell_h * k, slant = ts->slant * k, spacing = (float)ts->spacing;
    for (int l = 0; l < nl; l++) {
        float h = L[l].h * k;
        y += h - cell;                                  /* [R 0x1B6CB0] */
        float yoff = h <= cell ? 0.0f : 0.5f * (f->avg_h * k - h);
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
            /* the glyph quads [R 0x1B64B0]: x += a; the glyph from x to x + w, the cell's height, the top edge moved
             * by the slant; x += w + c + spacing. A space only advances (a + w + c + spacing). */
            for (int i = 0; i < L[l].n;) {
                unsigned char ch = (unsigned char)L[l].s[i];
                if (ch == '\v') {
                    TexRef t;
                    int ok;
                    float size;
                    i += text_icon(g, f, L[l].s + i, L[l].n - i, &t, &ok, &size);
                    if (ok && pass == 1) {
                        float ic[4] = { 1, 1, 1, c[3] }, it = top + 0.5f * (cell - size * k);
                        emit_quad(e, ox + sc * cx, oy + sc * it, ox + sc * (cx + size * k), oy + sc * (it + size * k),
                                  0, 0, t.us, t.vs, ic, t.b, t.name, z);
                    }
                    cx += k * (size + spacing);
                    continue;
                }
                i++;
                const Glyph *gl = &f->g[ch];
                if (!gl->ok)
                    continue;
                if (ch == ' ') {
                    cx += k * (gl->a + gl->w + gl->c + spacing);
                    continue;
                }
                cx += gl->a * k;
                float x1 = cx + gl->w * k;
                e->skew = sc * slant;                    /* the slant moves the top edge (an italic) */
                emit_quad(e, ox + sc * cx, oy + sc * top, ox + sc * x1, oy + sc * bot, gl->u0, gl->v0, gl->u1, gl->v1,
                          c, f->tex.b, f->tex.name, z);
                e->skew = 0;
                cx += k * (spacing + gl->w + gl->c);
            }
        }
        y += cell + ts->linesp;                  /* [R 0x1B6F58] */
    }
}

/* ---------------------------------------------------------------- the layout tree (.ifc) */
enum { N_GROUP, N_QUAD, N_TEXT, N_SLIDER, N_RECT, N_LIST, N_LINE };
typedef struct Node {
    int kind;
    char name[32];
    float p[3], m, z;           /* <p> x y z, <m> scale, <z> */
    int flip_y;                 /* <x> with a mirrored frame (Pause's arrows under MIN / MAX) */
    float d[2]; int has_d;      /* <d> size */
    float col[4];               /* <c> (white when absent) */
    unsigned F, Q, M;           /* <F> flags (0x1: hidden [G]), <Q> sprite flags, <M> (0x310: a mask [G]) */
    TexRef tex; int has_tex;    /* <b> */
    /* text: <v> / <L>, <g j o s charspacing linespacing w>font</g>, <maxwidth> */
    char text[MAX_TEXT];
    const Font *font;
    int justify, origin, spacing, has_spacing, linesp, has_linesp;
    float tscale, maxwidth, wrap;
    int tmode, has_tslant; float tslant;   /* <g> m (3: each line by j) and i */
    float tcol[4]; int has_tcol;
    /* slider: <val>, <hasMaskBar>, <barColor>, <maskColor>; min 0 max 100 [R ctor 0x24ACD0] */
    float val, vmin, vmax;
    int mask_bar;
    float bar_col[4], mask_col[4];
    /* rect (GuiRect): <st> side and <ct> corner pictures, <bkgdColor>, <bc> the frame's colour, <B> its width,
     * <bbias> its offset */
    TexRef side, corner; int has_side, has_corner;
    float bkgd[4], border[4], B, bbias;
    /* list box (GuiListBox): <c highlight / normal / disabled / selector>, <b highlight / endcap>, <d highlight> */
    float hl_col[4], norm_col[4], dis_col[4], sel_col[4];
    int has_hl, has_norm, has_dis;
    TexRef hl_tex, endcap; int has_hl_tex, has_endcap;
    float hl_d[2];
    int sel, pulse;
    unsigned la; float lsp;     /* list: <a> its flags (8 across), <sp> its spacing (setExtraSpacing replaces it) */
    int sizer; unsigned sflags; /* <sz> (GuiSizer): <sflags>, <stretch>; an item's <si> room before and after it, */
    float si[2], stretch;       /* and its stretch byte (s=) */
    int si_s;
    int clip; float clip_x0, clip_x1;
    float ep[4]; int has_ep;    /* line (<l>): <ep> its first and last point (x y) */
    float lw;                   /* line: <width>, 3 without (GuiLine ctor 0x23BA10: +0x228) */
    int visible;
    float fade;
    struct Node *parent, *child, *next;
} Node;

typedef struct {
    Node nodes[MAX_NODES];
    int n;
    Node *root;
} Layout;

/* a tiny reader for the layout XML: <tag a="v" ...>text or elements</tag> (no comments, no entities) */
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

/* GuiText.setText / localizeString (a key without a text keeps the key [G]) */
static void node_text(Node *nd, const char *s)
{
    if (nd)
        snprintf(nd->text, sizeof nd->text, "%s", s ? s : "");
}
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

/* one property of node nd: tag with its text and attributes */
static void apply_prop(Gui *g, Node *nd, const char *tag, const char *text, const Attr *at, int na)
{
    const char *which = na > 0 ? at[0].k : "";
    if (strcmp(tag, "p") == 0) parse_floats(text, nd->p, 3);
    else if (strcmp(tag, "d") == 0) {
        if (strcmp(which, "highlight") == 0) parse_floats(text, nd->hl_d, 2);
        else { parse_floats(text, nd->d, 2); nd->has_d = 1; }
    } else if (strcmp(tag, "m") == 0) nd->m = strtof(text, NULL);
    else if (strcmp(tag, "z") == 0) nd->fade = strtof(text, NULL);   /* [R 0x22C450: z is the fade (+0x5C)] */
    else if (strcmp(tag, "F") == 0) nd->F = (unsigned)strtoul(text, NULL, 0);
    else if (strcmp(tag, "Q") == 0) nd->Q = (unsigned)strtoul(text, NULL, 0);
    else if (strcmp(tag, "M") == 0) nd->M = (unsigned)strtoul(text, NULL, 0);
    else if (strcmp(tag, "c") == 0) {
        float c[4];
        col_rgba((uint32_t)strtoul(text, NULL, 0), c);
        if (strcmp(which, "highlight") == 0) { memcpy(nd->hl_col, c, sizeof c); nd->has_hl = 1; }
        else if (strcmp(which, "normal") == 0) { memcpy(nd->norm_col, c, sizeof c); nd->has_norm = 1; }
        else if (strcmp(which, "disabled") == 0) { memcpy(nd->dis_col, c, sizeof c); nd->has_dis = 1; }
        else if (strcmp(which, "selector") == 0) memcpy(nd->sel_col, c, sizeof c);
        else if (nd->kind == N_TEXT) { memcpy(nd->tcol, c, sizeof c); nd->has_tcol = 1; }
        else memcpy(nd->col, c, sizeof c);
    } else if (strcmp(tag, "b") == 0) {
        if (strcmp(which, "highlight") == 0) nd->has_hl_tex = tex_find(g, text, &nd->hl_tex);
        else if (strcmp(which, "endcap") == 0) nd->has_endcap = tex_find(g, text, &nd->endcap);
        else nd->has_tex = tex_find(g, text, &nd->tex);
    } else if (strcmp(tag, "v") == 0) {
        char *o = nd->text;                              /* the layouts' texts keep their line breaks, not \r */
        int n = 0;
        static const struct { const char *e; char c; } ENT[] = {
            { "&apos;", '\'' }, { "&quot;", '"' }, { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' } };
        for (const char *s = text; *s && n < MAX_TEXT - 1; s++) {
            if (*s == '\r')
                continue;
            size_t k = 0;
            if (*s == '&')                               /* "Crazy Man&apos;s Village" (Pause.ifc) */
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
        /* the object's whole frame: the 3x3 by rows, then the position (Pause's "completed" stamps, turned 28 degrees
         * at scale 0.8, and its arrows, mirrored up-down). A quad here cannot turn: the position and the scale are
         * kept, a mirror flips the picture, a turn is dropped [G] */
        float x[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
        parse_floats(text, x, 12);
        memcpy(nd->p, x + 9, sizeof nd->p);
        nd->m = sqrtf(x[0] * x[0] + x[1] * x[1]);
        nd->flip_y = x[0] * x[4] - x[1] * x[3] < 0;
    } else if (strcmp(tag, "ep") == 0) {
        float e[16] = { 0 };                             /* the count, then x y z each (0x22C0E0 copies two points) */
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
    } else if (strcmp(tag, "a") == 0) nd->la = (unsigned)strtoul(text, NULL, 0);
    else if (strcmp(tag, "sflags") == 0) nd->sflags = (unsigned)strtoul(text, NULL, 0);
    else if (strcmp(tag, "stretch") == 0) nd->stretch = strtof(text, NULL);
    else if (strcmp(tag, "si") == 0) {
        parse_floats(text, nd->si, 2);
        for (int i = 0; i < na; i++)
            if (strcmp(at[i].k, "s") == 0) nd->si_s = atoi(at[i].v);
    }
    else if (strcmp(tag, "sp") == 0) nd->lsp = strtof(text, NULL);
    /* sc (script), f (entry function), R, growth, si / sflags / stretch (sizers), X / xf (child interfaces):
     * not needed for drawing */
}

static int node_kind(const char *tag)
{
    if (strcmp(tag, "q") == 0) return N_QUAD;
    if (strcmp(tag, "t") == 0) return N_TEXT;
    if (strcmp(tag, "sl") == 0) return N_SLIDER;
    if (strcmp(tag, "r") == 0) return N_RECT;
    if (strcmp(tag, "lb") == 0) return N_LIST;
    if (strcmp(tag, "l") == 0) return N_LINE;
    /* sz: a sizer (GuiSizer: Pause's Cancel / Accept buttons): a group that lays its items out (layouts_lay) */
    if (strcmp(tag, "o") == 0 || strcmp(tag, "DD") == 0 || strcmp(tag, "i") == 0 || strcmp(tag, "sz") == 0)
        return N_GROUP;
    return -1;
}

static Node *node_new(Layout *L, int kind)
{
    if (L->n >= MAX_NODES)
        return NULL;
    Node *nd = &L->nodes[L->n++];
    memset(nd, 0, sizeof *nd);
    nd->kind = kind;
    nd->m = 1;
    nd->tscale = 1;
    nd->tmode = 3;
    for (int i = 0; i < 4; i++) nd->col[i] = nd->bar_col[i] = nd->mask_col[i] = nd->border[i] = 1;
    nd->val = 100;                                       /* [R 0x24AD94: value 100, min 0, max 100, 100 x 20] */
    nd->vmax = 100;
    nd->sel = -1;
    nd->visible = 1;
    nd->fade = 1;
    nd->lw = 3;
    return nd;
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

/* reads one element at x->p ('<' expected); nodes become children of parent, properties go to parent */
static int xml_element(Gui *g, Layout *L, Xml *x, Node *parent)
{
    xml_skip_ws(x);
    if (x->p >= x->end || *x->p != '<')
        return 0;
    x->p++;
    if (x->p < x->end && *x->p == '?') {                 /* <?xml ...?> */
        while (x->p + 1 < x->end && !(x->p[0] == '?' && x->p[1] == '>')) x->p++;
        x->p += 2;
        return 1;
    }
    char tag[24];
    xml_name(x, tag, sizeof tag);
    int kind = node_kind(tag);
    Node *nd = kind >= 0 ? node_new(L, kind) : NULL;
    if (nd && strcmp(tag, "sz") == 0)
        nd->sizer = 1;
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
    if (nd)                                              /* attach first: its children's frames need the parent */
        node_attach(nd, parent);
    if (*x->p == '/') {                                  /* <tag/> */
        x->p += 2;
        goto done;
    }
    x->p++;
    {
        const char *t0 = x->p;
        xml_skip_ws(x);
        if (x->p < x->end && *x->p == '<' && x->p + 1 < x->end && x->p[1] != '/') {
            while (x->p < x->end) {
                xml_skip_ws(x);
                if (x->p + 1 < x->end && x->p[0] == '<' && x->p[1] == '/')
                    break;
                if (!xml_element(g, L, x, nd ? nd : parent))
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
        while (x->p < x->end && *x->p != '>') x->p++;   /* </tag> */
        x->p++;
    }
done:
    if (nd) {
        if (nd->F & 1)
            nd->visible = 0;                             /* [G: F 0x1 = starts hidden; the scripts show them] */
        if (!parent)
            L->root = nd;
    }
    return 1;
}

static Layout *layout_load(Gui *g, const Bundle *b, const char *name)
{
    int n = b ? bdp_find_name(b, name) : -1;
    const BdpEntry *e = n >= 0 ? bdp_entry_of(b, (uint32_t)n, ET_IFC) : NULL;
    if (!e || !e->data)
        return NULL;
    Layout *L = calloc(1, sizeof *L);
    if (!L)
        return NULL;
    Xml x = { (const char *)e->data, (const char *)e->data + e->size };
    while (x.p < x.end && *x.p && xml_element(g, L, &x, NULL))
        xml_skip_ws(&x);
    if (!L->root) {
        free(L);
        return NULL;
    }
    return L;
}

static Node *layout_find(Node *nd, const char *name)
{
    for (; nd; nd = nd->next) {
        if (strcmp(nd->name, name) == 0)
            return nd;
        Node *c = layout_find(nd->child, name);
        if (c)
            return c;
    }
    return NULL;
}
/* GuiObject.copyObject + setParent: a deep copy, the parent's last child [G: last] */
static Node *node_copy(Layout *L, const Node *src, Node *parent)
{
    if (L->n >= MAX_NODES)
        return NULL;
    Node *c = &L->nodes[L->n++];
    *c = *src;
    c->child = NULL;
    node_attach(c, parent);
    for (const Node *k = src->child; k; k = k->next)
        node_copy(L, k, c);
    return c;
}

static void show(Node *nd, int v) { if (nd) nd->visible = v; }
static void set_fade(Node *nd, float f) { if (nd) nd->fade = f; }

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
/* GuiText.width() / height(): the text's size by its own scale s, not the object's m [G: Results::setupButtons
 * multiplies width() by getScale()] */
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

/* ---------------------------------------------------------------- list boxes (GuiListBox) */
/* the lists lay their items out every frame [R 0x23CF60]: a list across (<a> & 8) puts each shown item at a running
 * place from 0, by its justify (0, 1/2, 1 of its width), and steps its width + <sp>. Down the list the steps are the
 * items' heights, which no language changes: those keep the layout's places */
/* a node's extent across x in its own frame (before its <m>): itself (a quad's picture or <d>, a text's width by its
 * justify), its origin and its children's, each through its place and scale [R 0x248B80's boxes] */
static void node_span(const Gui *g, const Node *nd, float *x0, float *x1)
{
    float a = 0, b = 0;                                  /* joined with the origin */
    if (nd->kind == N_QUAD) {
        float w = nd->has_d ? nd->d[0] : nd->has_tex ? nd->tex.w : 0, q = nd->Q & 2 ? -0.5f * w : 0;
        a = fminf(a, q);
        b = fmaxf(b, q + w);
    } else if (nd->kind == N_TEXT) {
        float w = text_w(g, nd), q = nd->justify == 1 ? -0.5f * w : nd->justify == 2 ? -w : 0;
        a = fminf(a, q);
        b = fmaxf(b, q + w);
    }
    for (const Node *c = nd->child; c; c = c->next) {
        float c0, c1;
        node_span(g, c, &c0, &c1);
        a = fminf(a, c->p[0] + c->m * c0);
        b = fmaxf(b, c->p[0] + c->m * c1);
    }
    *x0 = a;
    *x1 = b;
}

/* GuiSizer's row across (sflags 1) [R 0x248B80]: n items with boxes [mn, mx] about their origins, room before /
 * after (<si>, s16) and stretch bytes s; out: the origins. The first sits at 0 and each next one at the last's
 * origin + max + after + its before - its min. With 0x100 and <stretch> S above the boxes' total, (S - total) /
 * sum(s + 1) a unit goes between them: the first's share f (s + 1) after it, the last's before it, a middle one's
 * split. Then (any of 0x7E) 4 ends the last box at 0 (its after not counted), 8 centres the row (8 over 4) */
static void sizer_row(int n, const float *mn, const float *mx, const float *before, const float *after,
                      const int *st, unsigned fl, float S, float *pos)
{
    float tot = 0, ssum = 0, f = 0;
    for (int i = 0; i < n; i++) {
        tot += mx[i] - mn[i];
        ssum += (float)(st[i] + 1);
    }
    if ((fl & 0x100) && ssum > 0 && S > 0 && tot < S)
        f = (S - tot) / ssum;
    float carry = (fl & 0x100) ? f * (float)(st[0] + 1) : 0, span = 0, prevsz = mx[0] - mn[0];
    pos[0] = 0;
    for (int i = 1; i < n; i++) {
        float p = pos[i - 1] + mx[i - 1] + after[i - 1] + before[i];
        if (fl & 0x100) {
            float sh = f * (float)(st[i] + 1);
            if (i != n - 1)
                sh *= 0.5f;
            carry += sh;
            p += carry;
            prevsz += carry;
            carry = sh;
        }
        pos[i] = p - mn[i];
        span += prevsz + after[i - 1] + before[i];
        prevsz = mx[i] - mn[i];
    }
    span += prevsz;
    if (!(fl & 0x7E))
        return;
    float off = 0;
    if (fl & 4)
        off = -mn[0] - span;
    if (fl & 8)
        off = -mn[0] - 0.5f * span;
    for (int i = 0; i < n; i++)
        pos[i] += off;
}

/* a sizer across (Pause's buttons): its items' boxes (hidden ones count) laid as sizer_row. Down (no flag 1: no
 * layout here needs it) and the across alignments 0x10 / 0x20 / 0x40 (none uses them) are not ported */
static void sizer_lay(const Gui *g, Node *sz)
{
    enum { N = 16 };
    float mn[N], mx[N], be[N], af[N], pos[N];
    int st[N], n = 0;
    Node *it[N];
    if (!(sz->sflags & 1))
        return;
    for (Node *c = sz->child; c && n < N; c = c->next, n++) {
        node_span(g, c, &mn[n], &mx[n]);
        mn[n] *= c->m;
        mx[n] *= c->m;
        be[n] = c->si[0];
        af[n] = c->si[1];
        st[n] = c->si_s;
        it[n] = c;
    }
    if (!n)
        return;
    sizer_row(n, mn, mx, be, af, st, sz->sflags, sz->stretch, pos);
    for (int i = 0; i < n; i++)
        it[i]->p[0] = pos[i];
}

static void layouts_lay(const Gui *g, Node *nd)
{
    for (; nd; nd = nd->next) {
        if (nd->sizer)
            sizer_lay(g, nd);
        if (nd->kind == N_LIST && (nd->la & 8)) {
            float place = 0;
            for (Node *c = nd->child; c; c = c->next) {
                if (!c->visible || c->kind != N_TEXT)
                    continue;
                float w = text_w(g, c) * c->m;
                c->p[0] = place + (c->justify == 1 ? 0.5f * w : c->justify == 2 ? w : 0.0f);
                place += w + nd->lsp;
            }
        }
        layouts_lay(g, nd->child);
    }
}

static int list_count(const Node *l)
{
    int n = 0;
    for (const Node *c = l ? l->child : NULL; c; c = c->next) n++;
    return n;
}
static Node *list_item(Node *l, int i)
{
    Node *c = l ? l->child : NULL;
    while (c && i-- > 0) c = c->next;
    return i < 0 ? c : NULL;
}
/* selectItem(i): the item in the highlight colour, the rest in the normal one [G: the list's colours replace the
 * items'; the layouts give the items the same ones] */
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
/* incrementSelection(delta, 0): to the next item shown, no wrap [G] */
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
/* incrementSelection(delta, 1): the same, going round at the ends [G: the second argument] (Pause's vibration
 * ON / OFF) */
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

/* ---------------------------------------------------------------- drawing a layout */
/* GuiRect (vtable 0x447A00, draw 0x244500, rebuild 0x2449F0) [R]: the outer box is d from the origin; the background
 * (bkgdColor, sprite +0x80) from -bbias / 2 with the size d + bbias (bbias -8: 4 in on each side, 0x243840); then the
 * frame in bc inside the box: B wide sides (GuiLines, FrameLine) and B x B corners (FrameCorner, turned 0, 270, 180
 * and 90 degrees: here mirrored [G: the same for a symmetric corner]). The left and right sides need the side picture
 * turned, which a quad cannot do: they are drawn as one-pixel strips, each the colour of one row of it [G: which rows
 * of FrameLine are the outside, the GuiLine draw not read]. */
static void rect_emit(Emit *e, const Node *nd, float ox, float oy, float sc, float z, float fade)
{
    float w = nd->has_d ? nd->d[0] : 0, h = nd->has_d ? nd->d[1] : 0;
    float bg[4] = { nd->bkgd[0], nd->bkgd[1], nd->bkgd[2], nd->bkgd[3] * fade };
    float bc[4] = { nd->border[0], nd->border[1], nd->border[2], nd->border[3] * fade };
    if (w <= 0 || h <= 0)
        return;
    float bb = -0.5f * nd->bbias;
    emit_quad(e, ox + sc * bb, oy + sc * bb, ox + sc * (w - bb), oy + sc * (h - bb), 0, 0, 1, 1, bg, NULL, 0, z);
    float B = nd->B;
    if (B <= 0)
        return;
    float x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (x1 - x0 < 2 * B) x1 = x0 + 2 * B;
    if (y1 - y0 < 2 * B) y1 = y0 + 2 * B;
#define RX(v) (ox + sc * (v))
#define RY(v) (oy + sc * (v))
    if (nd->has_corner) {
        const TexRef *t = &nd->corner;
        emit_quad(e, RX(x0), RY(y0), RX(x0 + B), RY(y0 + B), 0, 0, t->us, t->vs, bc, t->b, t->name, z);
        emit_quad(e, RX(x1 - B), RY(y0), RX(x1), RY(y0 + B), t->us, 0, 0, t->vs, bc, t->b, t->name, z);
        emit_quad(e, RX(x0), RY(y1 - B), RX(x0 + B), RY(y1), 0, t->vs, t->us, 0, bc, t->b, t->name, z);
        emit_quad(e, RX(x1 - B), RY(y1 - B), RX(x1), RY(y1), t->us, t->vs, 0, 0, bc, t->b, t->name, z);
    }
    if (nd->has_side) {
        const TexRef *t = &nd->side;
        emit_quad(e, RX(x0 + B), RY(y0), RX(x1 - B), RY(y0 + B), 0, t->vs, t->us, 0, bc, t->b, t->name, z);
        emit_quad(e, RX(x0 + B), RY(y1 - B), RX(x1 - B), RY(y1), 0, 0, t->us, t->vs, bc, t->b, t->name, z);
        int ns = (int)(B * sc + 0.5f);
        if (ns < 1) ns = 1;
        for (int i = 0; i < ns; i++) {
            float a = B * (float)i / (float)ns, b = B * (float)(i + 1) / (float)ns;
            float v = t->vs * (1.0f - ((float)i + 0.5f) / (float)ns);   /* the outside (v = 1) at the edge */
            emit_quad(e, RX(x0 + a), RY(y0 + B), RX(x0 + b), RY(y1 - B), 0.5f * t->us, v, 0.5f * t->us, v, bc, t->b,
                      t->name, z);
            emit_quad(e, RX(x1 - b), RY(y0 + B), RX(x1 - a), RY(y1 - B), 0.5f * t->us, v, 0.5f * t->us, v, bc, t->b,
                      t->name, z);
        }
    }
#undef RX
#undef RY
}

/* the list's highlight behind its selected item [G: GuiListBox's highlight (its growth and pulse) is not traced]:
 * the segment picture across the item's text, an end cap at each end, hl_d's height, in the selector colour,
 * pulsing when setHighlightPulse(1) */
static void list_emit(Emit *e, const Gui *g, const Node *l, float ox, float oy, float sc, float z, float fade)
{
    if (l->col[3] > 0 && l->has_d)
        emit_quad(e, ox, oy, ox + sc * l->d[0], oy + sc * l->d[1], 0, 0, 1, 1,
                  (const float[4]){ l->col[0], l->col[1], l->col[2], l->col[3] * fade }, NULL, 0, z);
    const Node *it = list_item((Node *)l, l->sel);
    if (!l->has_hl_tex || !it || !it->visible || it->kind != N_TEXT)
        return;
    float w = text_w(g, it) * it->m, h = text_h(g, it) * it->m;
    float x0 = it->p[0] - (it->justify == 1 ? 0.5f * w : it->justify == 2 ? w : 0), x1 = x0 + w;
    float cy = it->p[1] + (it->origin == 0 ? 0.5f * h : it->origin == 2 ? -0.5f * h : 0);
    float bh = l->hl_d[1] > 0 ? l->hl_d[1] : h, cap = 0.5f * bh;
    float c[4] = { l->sel_col[0], l->sel_col[1], l->sel_col[2], l->sel_col[3] * fade };
    if (l->pulse)
        c[3] *= 0.7f + 0.3f * sinf(2.0f * HUD_PI * 1.5f * g->time);
    float y0 = oy + sc * (cy - 0.5f * bh), y1 = oy + sc * (cy + 0.5f * bh);
    const TexRef *t = &l->hl_tex;
    emit_quad(e, ox + sc * x0, y0, ox + sc * x1, y1, 0, 0, t->us, t->vs, c, t->b, t->name, z);
    if (l->has_endcap) {
        const TexRef *k = &l->endcap;
        emit_quad(e, ox + sc * (x0 - cap), y0, ox + sc * x0, y1, k->us, 0, 0, k->vs, c, k->b, k->name, z);
        emit_quad(e, ox + sc * x1, y0, ox + sc * (x1 + cap), y1, 0, 0, k->us, k->vs, c, k->b, k->name, z);
    }
}

/* the layout drawn: the object's frame maps (x, y) to (ox + sc x, oy + sc y); children in it after p and m. The
 * sprite of a quad spans d (or its texture's used size, UVs over the used part) from the origin, or centred (Q); a
 * slider is centred (0x24A310): the bar (barColor) its whole size and the mask (maskColor) from the value's point to
 * its right end (hasMaskBar, flag 0x10), else the bar to the value's point; texts by text_emit. */
static void layout_emit(Emit *e, const Gui *g, const Node *nd, float ox, float oy, float sc, float z, float fade)
{
    for (; nd; nd = nd->next) {
        if (!nd->visible)
            continue;
        float nox = ox + sc * nd->p[0], noy = oy + sc * nd->p[1], nsc = sc * nd->m;
        float nz = z + nd->p[2] + nd->z, nf = fade * nd->fade;
        if (nd->F & 0x400)                               /* [R 0x242950] 1 - (0.2 + 0.2 sin 2 pi t): 0.6 .. 1, 1 Hz */
            nf *= 0.8f - 0.2f * sinf(2.0f * HUD_PI * g->time);
        int clip = e->clip;
        float cx0 = e->cx0, cx1 = e->cx1;
        if (nd->clip) {
            e->clip = 1;
            e->cx0 = nd->clip_x0;
            e->cx1 = nd->clip_x1;
        }
        if (nd->kind == N_QUAD) {
            float w = nd->has_d ? nd->d[0] : nd->has_tex ? nd->tex.w : 0;
            float h = nd->has_d ? nd->d[1] : nd->has_tex ? nd->tex.h : 0;
            float x0 = nd->Q & 2 ? -0.5f * w : 0, y0 = nd->Q & 4 ? -0.5f * h : 0;
            float c[4] = { nd->col[0], nd->col[1], nd->col[2], nd->col[3] * nf };
            /* M 0x200 (Results' 0x310 boxes at the title's ends): the depth-only shader, no colour [R]; F 0x20 (the challenge
             * icons): drawn only with a picture [G] */
            int drawn = !(nd->M & 0x200) && !((nd->F & 0x20) && !nd->has_tex);
            float vs = nd->has_tex ? nd->tex.vs : 1, v0 = nd->flip_y ? vs : 0, v1 = nd->flip_y ? 0 : vs;
            if (nd->flip_y)                              /* mirrored about the origin */
                y0 = -(y0 + h);
            e->additive = (nd->M & 0x80) != 0;           /* [R 0x17D100] Pause's icons, ChallengeText's icon1 */
            if (drawn && w > 0 && h > 0)
                emit_quad(e, nox + nsc * x0, noy + nsc * y0, nox + nsc * (x0 + w), noy + nsc * (y0 + h), 0, v0,
                          nd->has_tex ? nd->tex.us : 1, v1, c,
                          nd->has_tex ? nd->tex.b : NULL, nd->has_tex ? nd->tex.name : 0, nz);
            e->additive = 0;
        } else if (nd->kind == N_SLIDER) {
            float w = nd->has_d ? nd->d[0] : 100, h = nd->has_d ? nd->d[1] : 20;
            float f = nd->vmax != nd->vmin ? (nd->val - nd->vmin) / (nd->vmax - nd->vmin) : 0;
            if (f < 0) f = 0;                            /* [G: the game does not clamp; the HUD stays in range] */
            if (f > 1) f = 1;
            float xf = w * (f - 0.5f);
            float bc[4] = { nd->bar_col[0], nd->bar_col[1], nd->bar_col[2], nd->bar_col[3] * nf };
            float mc[4] = { nd->mask_col[0], nd->mask_col[1], nd->mask_col[2], nd->mask_col[3] * nf };
            if (nd->mask_bar) {
                emit_quad(e, nox - nsc * 0.5f * w, noy - nsc * 0.5f * h, nox + nsc * 0.5f * w, noy + nsc * 0.5f * h,
                          0, 0, 1, 1, bc, NULL, 0, nz);
                if (f < 1)
                    emit_quad(e, nox + nsc * xf, noy - nsc * 0.5f * h, nox + nsc * 0.5f * w, noy + nsc * 0.5f * h,
                              f, 0, 1, 1, mc, NULL, 0, nz);
            } else if (f > 0)
                emit_quad(e, nox - nsc * 0.5f * w, noy - nsc * 0.5f * h, nox + nsc * xf, noy + nsc * 0.5f * h,
                          0, 0, f, 1, bc, NULL, 0, nz);
        } else if (nd->kind == N_TEXT && nd->font) {
            const Font *f = nd->font;
            float c[4], s[4];
            if (nd->has_tcol) memcpy(c, nd->tcol, sizeof c);
            else col_abgr(f->col, c);
            col_abgr(f->shadow_col, s);
            c[3] *= nf;
            s[3] *= nf;
            TextStyle ts;
            text_style(nd, &ts);
            /* the text's own scale s goes into its matrix (GuiText 0x234D88 scales the matrix by it) */
            text_emit(e, g, &ts, nd->text, nox, noy, nsc * nd->tscale, c, s, f->shadow_dx, f->shadow_dy, nz);
        } else if (nd->kind == N_LINE && nd->has_ep) {
            /* a GuiLine (Pause's options rule) [R 0x23A8E0]: a band of +- width / 2 across its points, in its frame */
            float c[4] = { nd->col[0], nd->col[1], nd->col[2], nd->col[3] * nf }, hw = 0.5f * nd->lw;
            float x0 = fminf(nd->ep[0], nd->ep[2]), x1 = fmaxf(nd->ep[0], nd->ep[2]);
            float y0 = fminf(nd->ep[1], nd->ep[3]), y1 = fmaxf(nd->ep[1], nd->ep[3]);
            if (x1 - x0 >= y1 - y0) { y0 -= hw; y1 += hw; } else { x0 -= hw; x1 += hw; }
            emit_quad(e, nox + nsc * x0, noy + nsc * y0, nox + nsc * x1, noy + nsc * y1, 0, 0, 1, 1, c, NULL, 0, nz);
        } else if (nd->kind == N_RECT)
            rect_emit(e, nd, nox, noy, nsc, nz, nf);
        else if (nd->kind == N_LIST)
            list_emit(e, g, nd, nox, noy, nsc, nz, nf);
        layout_emit(e, g, nd->child, nox, noy, nsc, nz, nf);
        e->clip = clip;
        e->cx0 = cx0;
        e->cx1 = cx1;
    }
}

/* one interface's quads, in the layout's order [R 0x241770: self, then the children, no sort; the bars' black backing
 * (z 0.5: half faded) lies over the bar's frame and under its slider] */
static int emit_layout(HudQuad *out, int max, const Gui *g, const Layout *L)
{
    if (!L || max <= 0)
        return 0;
    float z[MAX_SORT];
    Emit e = { out, z, 0, max < MAX_SORT ? max : MAX_SORT, 0, 0, 0 };
    layout_emit(&e, g, L->root, 0, 0, 1, 0, 1);
    return e.n;
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
static void fader_init(Faders *fd, Node *obj, int on)
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
static void fader_go(Faders *fd, Node *obj, float dir)
{
    int i = fader_find(fd, obj);
    if (i >= 0)
        fd->dir[i] = dir;
}
/* UpdateObjectFader: 2 a second, at most a tenth of a second a frame */
static void fader_update(Faders *fd, float dt)
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

/* ---------------------------------------------------------------- the interfaces */
typedef struct { Node *box, *list, *rect, *title; int state; } ConfirmQuit;

/* a text banner: Rage.ifc run by Rage.scr [R] */
typedef struct {
    Layout *L;
    Node *rage, *head, *copies[RAGE_COPIES];
    int init;
    int removeable;                                      /* this.setName("Removeable") (setPointsBannerData) */
    float timer, hold, scale, col[4];
    char message[MAX_TEXT], message2[MAX_TEXT], sound[32];
} Rage;

typedef struct { Node *obj; float t0, t1, p0[3], p1[3], s0, s1, f0, f1; int done; } Intro;

enum { SCR_STANDIN, SCR_RESULTS, SCR_GAME_OVER, SCR_LEVEL, SCR_PAUSE, SCR_WORLD, SCR_KING_BLOCK, SCR_KING_CITY,
       SCR_KING_WORLD, SCR_TIMED };
typedef struct {
    Layout *L;
    int kind, screen;
    Faders fd;
    ConfirmQuit cq;
    int ended;                                           /* WorldGuiDriver::endResults: unloaded next frame */
    int exit_fn, exit_val;                               /* the call Results::setExitState holds back a frame */
    float age, ready;                                    /* seconds shown; seconds it has taken buttons (stand-in) */
    /* the data */
    HudResults r;
    char played[64], current[64], results_text[64], city[64];
    /* Results.scr (and LevelResults.scr: rating) */
    int ni_state, do_intro, do_walkout, scroll, count_up, live_vis, cu_index, cu_value, cu_total, sound_on, last_block;
    int rating, track_save, allow_save;                  /* bTrackSave; getAllowSave as the screen has it */
    float intro_t, live_delay, cu_progress, pulse_t, left_x, right_x, title_w, save_delay;
    Node *list, *total, *live, *title1, *title2, *base, *save, *cu_item;
    Intro intro[MAX_INTRO], walk[MAX_WALK];
    int nintro, nwalk;
    /* WorldResults, KingBlockResults, KingCityResults, KingWorldResults, TimedRunResults */
    char monsters[4][32], next_city[64];
    int ids[4];                                          /* KingBlockResults' aPlayerIds: the players by points */
    int do_rank, alt_walkout;                            /* bDoRank, bUseAltWalkout */
    Intro rank[MAX_WALK], alt[MAX_WALK];                 /* rankItems, altWalkoutItems */
    int nrank, nalt;
    Node *times[3];                                      /* TimedRunResults' gtTimes */
    /* GameOver.scr (and Pause.scr: listbox) */
    Node *choices;
    /* Pause.scr */
    int pause_state;                                     /* nPauseState: 0 the menu, 1 the Options page */
    int pad;                                             /* the controller that opened it */
    int prev_rumble, b_rumble, mode, tweak_sound;        /* nMode: the Options row (0 sound, 1 music, 2 rumble) */
    float prev_music, f_music, f_sound, opt_pulse, tick;
    Node *pause_group, *options_group, *s_sound, *s_music, *l_rumble, *titles[3];
} Screen;

typedef struct {
    Layout *L;
    Node *icon_shadow, *icon, *status, *boss_health, *boss_group, *boss_icon;
    int cur, goal, show_points, boss, completed, upgrade;
    float upgrade_delay;
} ChStatus;

typedef struct {
    Layout *L;
    Faders fd;
    Node *title;
    float display, cycle, scalar;
    int upgrade;
} ChText;

typedef struct {
    float t, scale;
    float start[3], pos[3];
    uint32_t col;
    char text[16];
} Banner;

struct Hud {
    HudConfig cfg;
    char monster[32];
    int save_active;                                     /* gCommon.isSaveGameActive() (hud_set_save_active) */
    Gui gui;
    Layout *hud, *clock, *attract;
    /* Hud.scr's objects and state */
    Node *health, *rage, *score, *heads, *blocks, *num_text, *block_text, *block;
    float hp, hp_max, energy, energy_max;
    int points, lives, blocks_won;
    int prev_points, prev_lives;
    float prev_health, prev_energy, rage_flash;
    /* Clock.scr */
    Node *clock_group, *digits[4];
    int clock_on, last_seconds;
    float clock_time, clock_fade;
    /* PointsBanner */
    const Font *banner_font;
    Banner banners[MAX_FLOATS];
    int nbanners;
    HudProject project;
    void *project_ctx;
    /* the screen's other interfaces */
    ChStatus cs;
    ChText *ct;
    HudChallenge ch;
    char ch_identifier[64], ch_icon[64];
    int ch_collected, ch_required;
    Rage *rages[MAX_RAGE];
    int nrages;
    Screen *scr;
    int result, paused;
    float auto_press;
    HudSound sounds[MAX_SOUNDS];
    int nsounds;
    /* the Pause menu and what it changes */
    Screen *pause;
    int pause_result;
    float set_sound, set_music;                          /* gCommon.getSoundVolume / getMusicVolume, 0 .. 0.75 */
    int set_rumble;                                      /* gCommon.getEnableRumble */
    float held_x;                                        /* pad 0 held (hud_pad_held) */
    int held_l, held_r;
    HudCall calls[MAX_CALLS];
    int ncalls;
};

static void sound(Hud *h, const char *name, int kill)
{
    if (!name || !*name || h->nsounds >= MAX_SOUNDS)
        return;
    snprintf(h->sounds[h->nsounds].name, sizeof h->sounds[0].name, "%s", name);
    h->sounds[h->nsounds].kill = kill;
    h->nsounds++;
}

/* ---------------------------------------------------------------- the HUD */
Hud *hud_open(const Bundle *worldgui, const Bundle *common, const HudConfig *cfg)
{
    if (!worldgui || !cfg)
        return NULL;
    Hud *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->cfg = *cfg;
    snprintf(h->monster, sizeof h->monster, "%s", cfg->monster ? cfg->monster : "George");
    h->cfg.monster = h->monster;
    h->gui.bundles[h->gui.nb++] = worldgui;
    if (common)
        h->gui.bundles[h->gui.nb++] = common;
    char name[32];
    snprintf(name, sizeof name, "Hud%d.ifc", cfg->player >= 0 && cfg->player < 4 ? cfg->player : 0);
    h->hud = layout_load(&h->gui, worldgui, name);
    h->clock = layout_load(&h->gui, worldgui, "Clock.ifc");
    /* gWorld.setBannerFont("PointsBanner") [R build/scripts/<City>.scr] */
    h->banner_font = find_font(&h->gui, "PointsBanner");
    if (!h->hud) {
        hud_close(h);
        return NULL;
    }
    /* Hud::doInterface, event 8192 (the interface starts) [R Hud.scr] */
    Node *r = h->hud->root;
    h->health = layout_find(r, "health");
    h->rage = layout_find(r, "rage");
    if (h->health) h->health = layout_find(h->health->child, "slider");
    if (h->rage) h->rage = layout_find(h->rage->child, "slider");
    h->score = layout_find(r, "score");
    h->heads = layout_find(r, "heads");
    h->blocks = layout_find(r, "blocks");
    h->num_text = layout_find(r, "numText");
    h->block = layout_find(r, "block");
    h->block_text = layout_find(r, "blockText");
    char tname[64];
    snprintf(tname, sizeof tname, "Hud_%s_Head", h->monster);
    int gt = cfg->play_type, king = gt >= 3 && gt <= 4;
    if (h->heads) {
        h->heads->has_tex = tex_find(&h->gui, tname, &h->heads->tex);
        h->heads->visible = !king;
    }
    if (h->blocks) {
        h->blocks->has_tex = tex_find(&h->gui, tname, &h->blocks->tex);
        h->blocks->visible = king;
        int i = 0;
        for (Node *c = h->blocks->child; c; c = c->next, i++) {
            c->col[0] = c->col[1] = c->col[2] = 0.4f;    /* dkGreyColor */
            c->col[3] = 1;
            c->visible = king && i < cfg->blocks_to_win;
        }
    }
    if (h->num_text)
        h->num_text->visible = gt == HUD_PLAY_CAMPAIGN;
    if (cfg->player == 1 && cfg->num_players == 2) {
        Node *g = layout_find(r, "group");
        if (g) { g->p[0] = 502; g->p[1] = 60; g->p[2] = 0; }
    }
    h->hp_max = 200;
    h->energy_max = 100;
    h->prev_lives = -1;
    h->prev_points = -1;
    h->prev_health = h->prev_energy = -1;                /* [G: the script starts at 0; its first update has the real
                                                          * numbers, here hud_set comes after hud_open] */
    h->lives = -1;
    h->blocks_won = -1;
    h->set_sound = 0.5f;                                 /* a new profile's (0x33F630: 0.5, 0.25, on) [R] */
    h->set_music = 0.25f;
    h->set_rumble = 1;
    /* Clock::doInterface */
    if (h->clock) {
        h->clock_group = layout_find(h->clock->root, "clock");
        h->digits[0] = layout_find(h->clock->root, "ten_minute");
        h->digits[1] = layout_find(h->clock->root, "one_minute");
        h->digits[2] = layout_find(h->clock->root, "ten_second");
        h->digits[3] = layout_find(h->clock->root, "one_second");
    }
    h->last_seconds = -1;
    if (h->clock_group)
        h->clock_group->visible = 0;
    /* WorldGuiDriver::process 8192: screen.loadInterface("AttractMode") in the demo (getPlayType() 0), after the Clock
     * [R]; GAttractMode.scr's 8192 hides "Video" (it has none) and sets PressStart's flag 1024 (the GUI's pulse) */
    if (cfg->player == 0 && gt == HUD_PLAY_ATTRACT &&
        (h->attract = layout_load(&h->gui, worldgui, "AttractMode.ifc")) != NULL) {
        Node *video = layout_find(h->attract->root, "Video"), *press = layout_find(h->attract->root, "PressStart");
        if (video) video->visible = 0;
        if (press) press->F |= 0x400;
    }
    /* <City>::onInit: gCommon.setChallengeHud(screen.loadInterface("ChallengeStatus")) [R LasVegas.scr]; one for the
     * screen: player 0's */
    h->ch.required = -1;
    h->ch_required = -1;
    if (cfg->player == 0 && (h->cs.L = layout_load(&h->gui, worldgui, "ChallengeStatus.ifc")) != NULL) {
        /* ChallengeStatus::doInterface 8192 [R] */
        Node *cr = h->cs.L->root;
        h->cs.icon_shadow = layout_find(cr, "icon");
        h->cs.icon = h->cs.icon_shadow ? layout_find(h->cs.icon_shadow->child, "icon") : NULL;
        h->cs.status = layout_find(cr, "status");
        h->cs.boss_health = layout_find(cr, "bossHealth");
        h->cs.boss_group = layout_find(cr, "boss_group");
        h->cs.boss_icon = h->cs.boss_group ? layout_find(h->cs.boss_group->child, "icon") : NULL;
        h->cs.upgrade_delay = -1;
        hud_challenge(h, NULL);
    }
    hud_update(h, 0);
    return h;
}

static void rage_free(Rage *r) { if (r) { free(r->L); free(r); } }
static void screen_free(Screen *s) { if (s) { free(s->L); free(s); } }
static void ctext_free(ChText *c) { if (c) { free(c->L); free(c); } }

void hud_close(Hud *h)
{
    if (!h)
        return;
    free(h->hud);
    free(h->clock);
    free(h->attract);
    free(h->cs.L);
    ctext_free(h->ct);
    for (int i = 0; i < h->nrages; i++)
        rage_free(h->rages[i]);
    screen_free(h->scr);
    screen_free(h->pause);
    free(h);
}

void hud_add_bundle(Hud *h, const Bundle *b)
{
    if (!h || !b || h->gui.nb >= MAX_BUNDLES)
        return;
    for (int i = 0; i < h->gui.nb; i++)
        if (h->gui.bundles[i] == b)
            return;
    h->gui.bundles[h->gui.nb++] = b;
}

const char *hud_string(const Hud *h, const char *key) { return h ? loc_find(&h->gui, key) : NULL; }
void hud_set_language(Hud *h, int lang) { if (h) h->gui.lang = lang; }

void hud_set(Hud *h, float hp, float hp_max, float energy, float energy_max, int score, int lives, int blocks)
{
    if (!h)
        return;
    if (hp >= 0) h->hp = hp;
    if (hp_max > 0) h->hp_max = hp_max;
    if (energy >= 0) h->energy = energy;
    if (energy_max > 0) h->energy_max = energy_max;
    if (score >= 0) h->points = score;
    if (lives >= 0) h->lives = lives;
    if (blocks >= 0) h->blocks_won = blocks;
}

void hud_set_clock(Hud *h, int visible, float seconds)
{
    if (!h)
        return;
    h->clock_on = visible;
    h->clock_time = seconds;
}

/* gui.setFade(a) on "Hud<i>" and "ChallengeStatus" [R Levels.scr BeginBlockNI / ResultsNI / SkipResultsNI] */
void hud_show(Hud *h, float fade)
{
    if (!h)
        return;
    if (h->hud)
        h->hud->root->fade = fade;
    if (h->cs.L)
        h->cs.L->root->fade = fade;
}

void hud_set_project(Hud *h, HudProject fn, void *ctx)
{
    if (!h)
        return;
    h->project = fn;
    h->project_ctx = ctx;
}

/* ---------------------------------------------------------------- PointsBanner: the floating numbers */
/* PointsBanner::advance 0x331B00 [R]: age t (+0x104) grows by the frame time; with a = t / 1.4 the number rises from
 * 2 below its point by 12 (2a - a^3) along Z and shakes across and in depth by 1.5 (1 - a)^2 sin(1.4 + 16 pi t) and
 * sin(2.7 + 20 pi t) (the third sine, for Z, is added times 0); it is gone after 1.4 s. */
static void banner_place(Banner *b)
{
    float a = b->t * (1.0f / 1.4f), amp = 1.5f * (1 - a) * (1 - a), w = 2.0f * HUD_PI * b->t;
    b->pos[0] = b->start[0] + amp * sinf(1.4f + 8.0f * w);
    b->pos[1] = b->start[1] + amp * sinf(2.7f + 10.0f * w);
    b->pos[2] = b->start[2] + (-2.0f + 12.0f * (2.0f * a - a * a * a));
}

void hud_float(Hud *h, int amount, const float pos[3], int building, float scale)
{
    if (!h || !pos)
        return;
    if (h->nbanners >= MAX_FLOATS) {                     /* [G: the game's pool size is not checked; drop the oldest] */
        memmove(h->banners, h->banners + 1, sizeof h->banners[0] * (MAX_FLOATS - 1));
        h->nbanners--;
    }
    Banner *b = &h->banners[h->nbanners++];
    memset(b, 0, sizeof *b);
    /* PointsBanner::start 0x331D50 [R]: the text is sprintf("%d", min(amount, 99999999)), the colour 0xFF00F0F0
     * (yellow) for a building and 0xFFF0F0F0 for the rest (the caller, 0x2BAAE0), the scale 0.8 there */
    snprintf(b->text, sizeof b->text, "%d", amount < 99999999 ? amount : 99999999);
    b->col = building ? 0xFF00F0F0u : 0xFFF0F0F0u;
    b->scale = scale > 0 ? scale : 0.8f;
    memcpy(b->start, pos, sizeof b->start);
    banner_place(b);
}

/* PointsBanner::draw 0x331960 [R]: a camera-facing frame (the camera's, Y turned down for the font) at the number's
 * point, scaled by s = max(0.01, 0.75 scale a - a^2) world units per font pixel (0.75 from 0x44CA48), the string
 * centred across and down (0x2B8480 with t0 = 1), with a shadow 3 3 font pixels off in 0xC4000000 (0x2B855C). */
static int banners_emit(const Hud *h, HudQuad *out, int max)
{
    const Font *f = h->banner_font;
    Emit e = { out, NULL, 0, max, 0, 0, 0 };
    if (!f || !f->ok)
        return 0;
    for (int i = 0; i < h->nbanners; i++) {
        const Banner *b = &h->banners[i];
        float a = b->t * (1.0f / 1.4f);
        float s = 0.75f * b->scale * a - a * a;
        if (s < 0.01f) s = 0.01f;
        float scr[2], ppu = 8.0f;
        if (h->project) {
            if (!h->project(h->project_ctx, b->pos, scr, &ppu))
                continue;
        } else {                                         /* pos[0], pos[1] a screen point; the motion at ppu */
            scr[0] = b->start[0] + (b->pos[0] - b->start[0]) * ppu;
            scr[1] = b->start[1] - (b->pos[2] - b->start[2]) * ppu;
        }
        float col[4], sh[4];
        col_abgr(b->col, col);
        col_abgr(0xC4000000u, sh);
        /* the banner's own shadow (3 3), not the file's 5 5 */
        TextStyle ts = { f, 1, 1, f->spacing, f->linespacing, 0, 0 };
        text_emit(&e, &h->gui, &ts, b->text, scr[0], scr[1], s * ppu, col, sh, 3, 3, 0);
    }
    return e.n;
}

/* Hud::updateStats [R Hud.scr] and Clock::updateTime [R Clock.scr] */
static void hud_stats(Hud *h, float dt)
{
    int gt = h->cfg.play_type;
    if (gt < 3 || gt > 4) {
        if (h->lives != h->prev_lives && h->lives >= 0 && h->num_text) {
            snprintf(h->num_text->text, sizeof h->num_text->text, "x%d", h->lives);
            h->prev_lives = h->lives;
        }
    } else {
        if (h->blocks_won != h->prev_lives && h->blocks_won >= 0 && h->block_text)
            snprintf(h->block_text->text, sizeof h->block_text->text, "x%d", h->blocks_won);
        h->prev_lives = h->blocks_won;
    }
    float hp = 100.0f * (h->hp / h->hp_max);
    float eg = h->energy / h->energy_max;
    float en = 100.0f - 100.0f * eg;                     /* the rage meter's mask covers what is not yet rage */
    if (h->health && h->prev_health != hp) { h->health->val = hp; h->prev_health = hp; }
    if (h->rage && h->prev_energy != en) { h->rage->val = en; h->prev_energy = en; }
    if (h->score && h->prev_points != h->points) {
        snprintf(h->score->text, sizeof h->score->text, "%d", h->points);
        h->prev_points = h->points;
    }
    h->rage_flash += dt;
    if (h->rage) {
        static const float rage_col[4] = { 1.0f, 0.5f, 0.25f, 1.0f };
        if (eg >= 1.0f) {                                /* full rage: the mask flashes to white, fRageFlashSpeed 30 */
            float flash = (1.0f + cosf(30.0f * h->rage_flash)) / 2.0f;
            for (int i = 0; i < 4; i++)
                h->rage->mask_col[i] = rage_col[i] + (1.0f - rage_col[i]) * flash;
        } else
            memcpy(h->rage->mask_col, rage_col, sizeof rage_col);
    }
    /* the clock */
    if (h->clock_group) {
        Node *c = h->clock_group;
        int was = c->visible;                            /* the script tests the visibility it read first */
        if (was != h->clock_on) {
            c->fade = 0;
            h->clock_fade = 0;
            if (!was) {
                h->last_seconds = -1;
                c->visible = 1;
            } else
                c->visible = 0;
        }
        if (was && c->visible) {
            int s = (int)h->clock_time;
            if (s > 59 * 60 + 59) s = 59 * 60 + 59;
            if (s < 0) s = 0;
            if (s != h->last_seconds) {
                char *d[4];
                for (int i = 0; i < 4; i++) d[i] = h->digits[i] ? h->digits[i]->text : NULL;
                if (d[0]) d[0][0] = 0;
                if (d[1]) d[1][0] = 0;
                if (s >= 60) {
                    if (d[0] && s >= 600) snprintf(d[0], MAX_TEXT, "%d", (s / 60) / 10);
                    if (d[1]) snprintf(d[1], MAX_TEXT, "%d", (s / 60) % 10);
                }
                if (d[2]) snprintf(d[2], MAX_TEXT, "%d", (s % 60) / 10);
                if (d[3]) snprintf(d[3], MAX_TEXT, "%d", (s % 60) % 10);
                h->last_seconds = s;
            }
            if (h->clock_fade < 1.0f) {
                h->clock_fade += dt * 1.5f;
                c->fade = h->clock_fade < 1.0f ? h->clock_fade : 1.0f;
            }
        }
    }
    /* the floating numbers */
    int n = 0;
    for (int i = 0; i < h->nbanners; i++) {
        Banner *b = &h->banners[i];
        b->t += dt;
        banner_place(b);
        if (b->t <= 1.4f)
            h->banners[n++] = *b;
    }
    h->nbanners = n;
}

/* ---------------------------------------------------------------- the text banners (Rage.ifc, Rage.scr) */
/* screen.loadInterface("Rage"), Rage::doInterface 8192 [R]: the defaults are the RAMPAGE banner's */
static Rage *rage_load(Hud *h)
{
    if (h->nrages >= MAX_RAGE) {                         /* [G: the game has no limit; the oldest goes] */
        rage_free(h->rages[0]);
        memmove(h->rages, h->rages + 1, sizeof h->rages[0] * (MAX_RAGE - 1));
        h->nrages--;
    }
    Rage *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->L = layout_load(&h->gui, h->gui.bundles[0], "Rage.ifc");
    r->rage = r->L ? layout_find(r->L->root, "Rage") : NULL;
    if (!r->rage) {
        rage_free(r);
        return NULL;
    }
    r->head = layout_find(r->L->root, "Head");
    if (r->head) {
        r->head->m = 4.0f;                               /* goHead.setScale(4.0) */
        /* nothing in Rage.scr shows the head: its <z> 0 is its fade, so it is never seen [R 0x22C450] */
        r->head->visible = 0;
    }
    r->rage->fade = 0;
    snprintf(r->message, sizeof r->message, "Rampage");
    r->scale = 1.0f;
    snprintf(r->sound, sizeof r->sound, "v_rampage");
    col_set(r->col, 1, 0, 0, 1);
    r->hold = 0.35f;                                     /* [G: fHoldTime is only set by setPointsBannerData; 0.35] */
    h->rages[h->nrages++] = r;
    return r;
}

/* Rage::setPointsBannerData / setPointsBannerData2, and this.setName("Removeable"): gWorld.hideChallengeCompletionUI
 * hides those (hud_hide_challenge_completion) */
static void rage_data(Rage *r, const char *message, const char *message2, const float col[4], float size, float hold,
                      const char *sfx)
{
    snprintf(r->message, sizeof r->message, "%s", message ? message : "");
    snprintf(r->message2, sizeof r->message2, "%s", message2 ? message2 : "");
    memcpy(r->col, col, sizeof r->col);
    r->scale = size;
    r->hold = hold;
    r->removeable = 1;
    if (!sfx || strcmp(sfx, "Default") != 0)
        snprintf(r->sound, sizeof r->sound, "%s", sfx ? sfx : "");
}

/* Rage::setupData [R]: the text (a key, or '#' and the text), its colour and scale, the second line, 15 copies of
 * it with fades 0, 1/15 .. 14/15, the text itself hidden, the sound */
static void rage_setup(Hud *h, Rage *r)
{
    Node *t = r->rage;
    node_localize(&h->gui, t, r->message);
    memcpy(t->tcol, r->col, sizeof t->tcol);
    t->has_tcol = 1;
    t->m = r->scale;
    if (r->message[0] == '#')
        node_text(t, r->message + 1);
    if (r->message2[0]) {
        Node *l2 = layout_find(t->child, "line2");
        if (l2) {
            node_localize(&h->gui, l2, r->message2);
            memcpy(l2->tcol, r->col, sizeof l2->tcol);
            l2->has_tcol = 1;
            if (r->message2[0] == '#')
                node_text(l2, r->message2 + 1);
            l2->visible = 1;
        }
    }
    float fi = 0;
    for (int i = 0; i < RAGE_COPIES; i++) {
        Node *c = node_copy(r->L, t, t->parent);
        if (c) {
            c->fade = fi;
            c->visible = 1;
        }
        r->copies[i] = c;
        fi += 1.0f / RAGE_COPIES;
    }
    t->visible = 0;
    if (r->sound[0])
        sound(h, r->sound, 0);
}

/* Rage::doInterface 2048 [R]; returns 0 when it unloads itself */
static int rage_update(Hud *h, Rage *r, float dt)
{
    if (!r->init) {
        r->init = 1;
        rage_setup(h, r);
        return 1;
    }
    r->timer += dt;
    float t = r->timer;
    Node *g = r->rage;
    if (t < 0.75f) {                                     /* the copies swirl in */
        float p = t / 0.75f;
        if (p > 1) p = 1;
        float sp = sqrtf(p);
        for (int i = 0; i < RAGE_COPIES; i++) {
            Node *c = r->copies[i];
            if (!c)
                continue;
            c->fade = sp * (float)(RAGE_COPIES - i) / (float)RAGE_COPIES;
            float off = (1.0f - sp) * 100.0f * cosf(1.3f * (float)i);
            float ang = (float)(i - 4) / 6.0f + p;
            c->p[0] = g->p[0] + off * cosf(ang);
            c->p[1] = g->p[1] + off * sinf(ang);
            c->p[2] = g->p[2];
        }
    } else if (t < 0.75f + r->hold) {                    /* the text holds, shaking */
        for (int i = 0; i < RAGE_COPIES; i++) show(r->copies[i], 0);
        g->visible = 1;
        g->fade = 1;
        float a = (t - 0.75f) / r->hold * 2.0f * 3.1415927f;
        g->p[0] = 320.0f + 3.0f * cosf(a * 2.0f);
        g->p[1] = 240.0f + 2.0f * sinf(a * 3.0f);
        g->p[2] = 0;
    } else if (t < 0.75f + r->hold + 0.5f) {             /* and fades from half */
        float f = 1.0f - (t - (0.75f + r->hold)) / 0.5f;
        for (int i = 0; i < RAGE_COPIES; i++) show(r->copies[i], 0);
        g->visible = 1;
        g->fade = f / 2.0f;
    } else
        return 0;
    return 1;
}

void hud_banner_text2(Hud *h, const char *name, const char *name2, const float rgba[4], float scale, float hold,
                      const char *snd)
{
    if (!h || !name)
        return;
    Rage *r = rage_load(h);
    if (!r)
        return;
    /* Edible.scr: the first "\n" (backslash, n) of the message becomes a line break */
    char msg[MAX_TEXT];
    snprintf(msg, sizeof msg, "%s", name);
    char *nl = strstr(msg, "\\n");
    if (nl && nl > msg) {
        nl[0] = '\n';
        memmove(nl + 1, nl + 2, strlen(nl + 2) + 1);
    }
    static const float red[4] = { 1, 0, 0, 1 };
    rage_data(r, msg, name2, rgba ? rgba : red, scale, hold, snd);
}

void hud_banner_text(Hud *h, const char *name, const float rgba[4], float scale, const char *snd)
{
    hud_banner_text2(h, name, NULL, rgba, scale, 0.35f, snd);   /* setPointsBannerData: fHoldTime 0.35 */
}

void hud_banner_rage(Hud *h)
{
    if (h)
        rage_load(h);
}

void hud_banner_reveal(Hud *h, const char *monster)
{
    if (!h)
        return;
    /* Cryotube.scr: message.format(localize("MonsterRevealed"), localize(name)); "#" + message */
    const char *fmt = loc_find(&h->gui, "MonsterRevealed"), *name = loc_find(&h->gui, monster);
    char msg[MAX_TEXT], text[MAX_TEXT];
    const char *s = fmt ? strstr(fmt, "%s") : NULL;
    if (!name)
        name = monster ? monster : "";
    if (s)
        snprintf(text, sizeof text, "%.*s%s%s", (int)(s - fmt), fmt, name, s + 2);
    else
        snprintf(text, sizeof text, "%s", fmt ? fmt : name);
    snprintf(msg, sizeof msg, "#%s", text);
    float col[4] = { 220.0f / 255.0f, 0, 0, 1 };
    Rage *r = rage_load(h);
    if (r)
        rage_data(r, msg, NULL, col, 0.5f, 0.35f, "V_Monster_Reveal_Seq");
}

/* gWorld.hideChallengeCompletionUI, 0x2B01C0 [R]: for each interface of the screen (screen +0x238: the list at +0x0,
 * its count at +0x8) whose name (+0x38) is "Removeable", vtable +0x44 (0x22A3B0: flags +0x5A |= 1) with 1. Flag 0x1
 * is the one GuiObject.isVisible tests (0x256C64: !(flags & 1)) [R]. The banner's script goes on [G: the GUI manager's
 * frame events to a hidden interface not traced] and unloads it at its end as usual; here it is not drawn. */
void hud_hide_challenge_completion(Hud *h)
{
    if (!h)
        return;
    for (int i = 0; i < h->nrages; i++)
        if (h->rages[i]->removeable && h->rages[i]->L)
            h->rages[i]->L->root->visible = 0;
}

/* ---------------------------------------------------------------- ChallengeStatus [R ChallengeStatus.scr] */
static void cs_status_text(Hud *h, int count)
{
    char s[32];
    snprintf(s, sizeof s, "%d/%d", count, h->cs.goal);
    node_text(h->cs.status, s);
}

/* setupUI (updateBossStatus) */
void hud_challenge(Hud *h, const HudChallenge *c)
{
    if (!h)
        return;
    if (c) {
        h->ch = *c;
        snprintf(h->ch_identifier, sizeof h->ch_identifier, "%s", c->identifier ? c->identifier : "");
        snprintf(h->ch_icon, sizeof h->ch_icon, "%s", c->icon ? c->icon : "");
        h->ch.identifier = h->ch_identifier;
        h->ch.icon = h->ch_icon;
        h->ch_required = c->required;
    }
    ChStatus *cs = &h->cs;
    if (!cs->L)
        return;
    int pt = h->cfg.play_type;
    cs->boss = h->ch.last_block && (pt == HUD_PLAY_CAMPAIGN || pt == HUD_PLAY_TIMED);
    show(layout_find(cs->L->root, "challenge_group"), !cs->boss);
    show(cs->boss_group, cs->boss);
    cs->completed = 0;
    const char *icon = h->ch.icon ? h->ch.icon : "";
    if (cs->boss) {
        if (cs->boss_health) cs->boss_health->val = 100;
        set_fade(cs->boss_group, 1);
        cs->L->root->visible = 1;
        if (icon[0] && cs->boss_icon)
            cs->boss_icon->has_tex = tex_find(&h->gui, icon, &cs->boss_icon->tex);
        cs->upgrade = 0;
    } else {
        char s[32];
        snprintf(s, sizeof s, "0/%d", cs->goal);
        node_text(cs->status, s);
        if (icon[0]) {
            if (cs->icon_shadow) cs->icon_shadow->has_tex = tex_find(&h->gui, icon, &cs->icon_shadow->tex);
            if (cs->icon) cs->icon->has_tex = tex_find(&h->gui, icon, &cs->icon->tex);
        }
        cs->upgrade = h->ch.upgrade != 0;
    }
}

/* doInterface 2048, the item count's part (every frame, and when the numbers come) */
static int cs_items(Hud *h, int count, int goal)
{
    ChStatus *cs = &h->cs;
    int met = 0;
    if (!cs->L || cs->boss)
        return 0;
    if (cs->goal != goal) {
        cs->goal = goal;
        if (count > cs->goal) count = cs->goal;
        cs_status_text(h, count);
        cs->L->root->visible = cs->goal != -1;
    }
    if (count != cs->cur) {
        cs->cur = count;
        if (count > 0 && count >= cs->goal && !cs->completed) {
            cs->completed = 1;
            cs->upgrade_delay = 2.0f;
            cs->show_points = 1;
            static const float green[4] = { 0, 1, 0, 1 };
            hud_banner_text(h, "Challenge_Met", green, 1.0f, "v_challenge_complete");
            met = 1;                                     /* gCommon.addPoints(i, 5, Points / players) */
        }
        if (count > cs->goal)
            count = cs->goal;
        cs_status_text(h, count);
    }
    return met;
}

int hud_challenge_items(Hud *h, int count, int goal)
{
    if (!h)
        return 0;
    h->ch_collected = count;
    h->ch_required = goal;
    return cs_items(h, count, goal);
}

int hud_boss_health(Hud *h, float percent)
{
    if (!h || !h->cs.L)
        return 0;
    ChStatus *cs = &h->cs;
    if (cs->boss_health)
        cs->boss_health->val = 100.0f * percent;
    if (percent > 0)
        return 0;
    if (h->cfg.play_type == HUD_PLAY_CAMPAIGN) {
        static const float green[4] = { 0, 1, 0, 1 };
        hud_banner_text(h, "Challenge_Met", green, 1.0f, "v_challenge_complete");
    }
    cs->boss = 2;
    return 1;
}

static void cs_update(Hud *h, float dt)
{
    ChStatus *cs = &h->cs;
    if (!cs->L)
        return;
    if (!cs->boss) {
        cs_items(h, h->ch_collected, h->ch_required);
        if (cs->show_points) {
            cs->upgrade_delay -= dt;
            if (cs->upgrade_delay <= 0) {
                cs->show_points = 0;
                static const float green[4] = { 0, 1, 0, 1 };
                hud_banner_text2(h, "ChallengeReward", "", green, 1.0f, 1.5f, "v_challenge_complete");
            }
        }
    } else if (cs->boss == 2 && cs->boss_group) {        /* the bar fades out, 0.5 a second */
        float f = cs->boss_group->fade - 0.5f * dt;
        if (f <= 0) {
            f = 0;
            cs->boss = 3;
        }
        cs->boss_group->fade = f;
    }
}

/* ---------------------------------------------------------------- ChallengeText [R ChallengeText.scr] */
static void ct_unload(Hud *h)
{
    ctext_free(h->ct);
    h->ct = NULL;
}

static int ct_load(Hud *h)
{
    ct_unload(h);
    ChText *c = calloc(1, sizeof *c);
    if (!c)
        return 0;
    c->L = layout_load(&h->gui, h->gui.bundles[0], "ChallengeText.ifc");
    if (!c->L) {
        free(c);
        return 0;
    }
    h->ct = c;
    Node *root = c->L->root;
    const HudChallenge *ch = &h->ch;
    root->visible = 1;
    root->fade = 1;
    int boss = ch->last_block;
    c->display = boss ? 8.0f : 3.0f;
    char title[48] = "CT_Challenge";
    if (ch->upgrade) {
        c->upgrade = 1;
        strcat(title, "_Upgrade");
    }
    const char *text = ch->identifier ? ch->identifier : "";
    c->scalar = 1.0f;
    Node *rect = layout_find(root, "ctRect"), *icon = layout_find(root, "icon1");
    show(layout_find(root, "challenge"), !boss);
    show(layout_find(root, "boss"), boss);
    if (boss) {
        c->title = layout_find(root, "boss_title");
        Node *msg = layout_find(root, "boss_message"), *obj = layout_find(root, "boss_objectives");
        Node *hint = layout_find(root, "boss_hint");
        node_localize(&h->gui, c->title, "ChallengeBossTitle");
        node_localize(&h->gui, msg, "ChallengeBoss");
        node_localize(&h->gui, obj, "ChallengeBossObjectives");
        node_localize(&h->gui, hint, "ChallengeBossHint");
        if (msg && obj && hint) {
            float mh = text_h(&h->gui, msg), oh = text_h(&h->gui, obj), hh = text_h(&h->gui, hint);
            float y = msg->p[1] + mh + 8.0f;
            obj->p[0] = msg->p[0];
            obj->p[1] = y;
            y += oh + 8.0f;
            hint->p[0] = msg->p[0];
            hint->p[1] = y;
            if (rect) rect->d[1] = 75.0f + 12.0f + mh + oh + hh;
        }
        if (icon) { icon->p[0] = 45; icon->p[1] = 35; icon->p[2] = 0; }
    } else {
        c->title = layout_find(root, "title");
        node_localize(&h->gui, c->title, title);
        node_localize(&h->gui, layout_find(root, "message"), text);
        if (rect) rect->d[1] = 115.0f;
        if (icon) { icon->p[0] = 45; icon->p[1] = 60; icon->p[2] = 0; }
        const char *rw = loc_find(&h->gui, "Reward");
        char key[24], s[MAX_TEXT];
        if (ch->upgrade) snprintf(key, sizeof key, "Upgrade%d", ch->upgrade);
        else snprintf(key, sizeof key, "ChallengeReward");
        const char *rv = loc_find(&h->gui, key);
        snprintf(s, sizeof s, "%s %s", rw ? rw : "Reward", rv ? rv : key);
        node_text(layout_find(root, "reward"), s);
    }
    if (c->title)
        c->title->m = 1.0f;
    fader_init(&c->fd, root, 1);                         /* InitObjectFader(this, 1); ObjectFaderSetRate(this, 3):
                                                          * no effect while it is not moving */
    if (strcmp(text, "Xo oX") == 0 || !text[0] || ch->required <= 0 || h->cfg.play_type != HUD_PLAY_CAMPAIGN) {
        ct_unload(h);
        return 0;
    }
    if (icon) {
        icon->has_tex = tex_find(&h->gui, ch->icon, &icon->tex);   /* resetTexture */
        icon->m = ch->icon_scale;
        icon->p[0] += ch->icon_shift;
    }
    return 1;
}

static void ct_update(Hud *h, float dt)
{
    ChText *c = h->ct;
    if (!c)
        return;
    Node *root = c->L->root;
    c->display -= dt;
    if (c->display <= 0 && c->display + dt > 0)
        fader_go(&c->fd, root, -1);
    fader_update(&c->fd, dt);
    if (c->upgrade && c->title) {                        /* the title pulses, less each frame */
        c->cycle += dt * 6.0f;
        c->title->m = 1.0f + c->scalar * sinf(c->cycle) / 4.0f;
        c->scalar *= 0.975f;
    }
    if (!root->visible)
        ct_unload(h);
}

int hud_load_interface(Hud *h, const char *name)
{
    if (!h || !name)
        return 0;
    if (_stricmp(name, "ChallengeText") == 0)
        return ct_load(h);
    if (_stricmp(name, "Rage") == 0)
        return rage_load(h) != NULL;
    return 0;
}

/* ---------------------------------------------------------------- the results screens */
/* WorldGuiDriver::setResultState / endResults as the screens call them */
static void set_result(Hud *h, int v) { h->result = v; }

/* Results::getFormattedValue [R]: "%i,%03i" from 1000 on (one comma only) */
static void fmt_value(char *out, size_t n, int v)
{
    if (v >= 1000)
        snprintf(out, n, "%i,%03i", v / 1000, v - v / 1000 * 1000);
    else
        snprintf(out, n, "%d", v);
}

/* ConfirmQuit.h [R] */
static void cq_init(Screen *s)
{
    ConfirmQuit *q = &s->cq;
    q->box = layout_find(s->L->root, "confirmQuit");
    q->list = q->box ? layout_find(q->box->child, "cq_list") : NULL;
    q->rect = q->box ? layout_find(q->box->child, "cq_rect") : NULL;
    q->title = q->box ? layout_find(q->box->child, "cq_title") : NULL;
    list_select(q->list, 0);
    if (q->list) q->list->pulse = 1;
    fader_init(&s->fd, q->box, 0);
    q->state = 0;
}

static void cq_show(Hud *h, Screen *s, int shorter)
{
    ConfirmQuit *q = &s->cq;
    if (!q->box)
        return;
    node_localize(&h->gui, q->title, shorter ? "ConfirmQuitShort" : "ConfirmQuit");
    if (q->rect) q->rect->d[0] = 480.0f;
    float ht = text_h(&h->gui, q->title);
    if (q->list && q->title)
        q->list->p[1] = q->title->p[1] + ht + 15.0f;
    if (q->rect) {
        q->rect->d[1] = ht + 70.0f;
        q->rect->p[1] = (480.0f - (ht + 70.0f)) / 2.0f;
    }
    fader_go(&s->fd, q->box, 1);
    q->state = 1;
}

static void results_quit(Hud *h, Screen *s);
static void gameover_quit(Hud *h, Screen *s);
static void pause_quit(Hud *h, Screen *s);

static int cq_process(Hud *h, Screen *s, int ev)
{
    ConfirmQuit *q = &s->cq;
    if (!q->state)
        return 0;
    if (ev == HUD_EV_ACCEPT) {
        if (q->list && q->list->sel == 0)
            fader_go(&s->fd, q->box, -1);
        else if (s->kind == SCR_RESULTS)
            results_quit(h, s);
        else if (s->kind == SCR_LEVEL || s->kind == SCR_KING_BLOCK || s->kind == SCR_KING_CITY || s->kind == SCR_TIMED)
            set_result(h, HUD_RESULT_QUIT);              /* LevelResults / KingBlockResults / KingCityResults /
                                                          * TimedRunResults::onQuit */
        else if (s->kind == SCR_PAUSE)
            pause_quit(h, s);
        else
            gameover_quit(h, s);
        q->state = 0;
    } else if (ev == HUD_EV_BACK) {
        fader_go(&s->fd, q->box, -1);
        q->state = 0;
    } else if (ev == HUD_EV_UP || ev == HUD_EV_DOWN || ev == HUD_EV_LEFT || ev == HUD_EV_RIGHT) {
        if (ev == HUD_EV_UP) ev = HUD_EV_LEFT;           /* No / Yes side by side: up and down as left and right */
        if (ev == HUD_EV_DOWN) ev = HUD_EV_RIGHT;
        int sel = q->list ? q->list->sel : 0;
        list_increment(q->list, -1 + 2 * (ev == HUD_EV_RIGHT));
        if (q->list && q->list->sel != sel)
            sound(h, "Rollover", 0);
    }
    return 1;
}

/* Results::setupIntroItem: from and to where the object is, scale and fade unchanged */
static Intro *intro_item(Screen *s, Intro *list, int *n, int max, const char *name)
{
    if (*n >= max)
        return NULL;
    Node *o = layout_find(s->L->root, name);
    if (!o)
        return NULL;                                     /* gCommon.dbgAssert */
    Intro *it = &list[(*n)++];
    memset(it, 0, sizeof *it);
    it->obj = o;
    memcpy(it->p0, o->p, sizeof it->p0);
    memcpy(it->p1, o->p, sizeof it->p1);
    it->s0 = it->s1 = o->m;
    it->f0 = it->f1 = 1;
    return it;
}
static Intro *intro_x(Screen *s, const char *name, float t0, float t1, float x0, float x1)
{
    Intro *it = intro_item(s, s->intro, &s->nintro, MAX_INTRO, name);
    if (it) { it->t0 = t0; it->t1 = t1; it->p0[0] = x0; it->p1[0] = x1; }
    return it;
}
static Intro *intro_y(Screen *s, const char *name, float t0, float t1, float y0, float y1)
{
    Intro *it = intro_item(s, s->intro, &s->nintro, MAX_INTRO, name);
    if (it) { it->t0 = t0; it->t1 = t1; it->p0[1] = y0; it->p1[1] = y1; }
    return it;
}
static Intro *walk_item(Screen *s, const char *name, float t1, int axis, float a, float b)
{
    Intro *it = intro_item(s, s->walk, &s->nwalk, MAX_WALK, name);
    if (it) { it->t0 = 0; it->t1 = t1; it->p0[axis] = a; it->p1[axis] = b; }
    return it;
}

/* Results::setMarqueText */
static void res_marquee(Hud *h, Screen *s, const char *text)
{
    node_text(s->base, text);
    s->title_w = text_w(&h->gui, s->base);
    if (s->title1) {
        s->title1->p[0] = s->right_x;
        node_text(s->title1, s->base ? s->base->text : text);
    }
    if (s->title2) {
        s->title2->p[0] = s->right_x + s->title_w + 150.0f;
        node_text(s->title2, s->base ? s->base->text : text);
    }
}

/* bs_fixButton(this, name, index) on the PS2 (gApp.getPlatform() 2): the button's text in ps2Buttons[index]
 * [R ButtonSetup.h] */
static void button_colour(Screen *s, const char *name, int index)
{
    static const float ps2[4][3] = { { 0, 186, 255 }, { 190, 45, 39 }, { 220, 108, 194 }, { 0, 240, 156 } };
    Node *t = name && *name ? layout_find(s->L->root, name) : NULL;
    if (t && index >= 0 && index < 4) {
        col_set(t->tcol, ps2[index][0] / 255.0f, ps2[index][1] / 255.0f, ps2[index][2] / 255.0f, 1);
        t->has_tcol = 1;
    }
}
/* fixButtonColors(this, "continue", "quit", "select", "save") */
static void res_button_colours(Screen *s)
{
    static const char *const names[4] = { "continue", "quit", "select", "save" };
    for (int i = 0; i < 4; i++)
        button_colour(s, names[i], i);
}

/* Results::doInterface 8192 [R] */
static void results_start(Hud *h, Screen *s)
{
    Node *root = s->L->root;
    const HudResults *r = &s->r;
    s->do_intro = 1;
    s->cu_index = -1;
    s->cu_progress = -1;
    s->live_vis = 1;
    s->save = layout_find(root, "save");
    show(s->save, 0);
    s->track_save = 0;
    s->save_delay = 0;
    s->allow_save = r->allow_save;
    root->fade = 1;
    set_result(h, HUD_RESULT_NONE);
    s->list = layout_find(root, "pointsList");
    Node *d = layout_find(root, "destruction");
    node_text(d ? layout_find(d->child, "points") : NULL, "0");
    s->last_block = strcmp(s->played, s->current) == 0;
    if (s->last_block)
        node_localize(&h->gui, layout_find(root, "challenge"), "Results_Points_ChallengeBoss");
    for (int i = 1; i < list_count(s->list); i++)
        show(list_item(s->list, i), 0);
    s->total = layout_find(root, "totalScore");
    node_text(s->total, "0");
    s->live = layout_find(root, "live");
    show(s->live, 1);
    fader_init(&s->fd, layout_find(root, "checkpoint"), 0);
    fader_init(&s->fd, layout_find(root, "revealed"), 0);
    s->title1 = layout_find(root, "goal1");
    s->title2 = layout_find(root, "goal2");
    s->base = layout_find(root, "goalbase");
    s->left_x = 62.0f;
    s->right_x = 503.0f;
    s->title_w = text_w(&h->gui, s->title1);
    /* the title's M 0x310 boxes at x -3 .. 62 and 503 .. do not cut it [R]: they write depth only, 0.001 behind the
     * glyphs (0x25B4D4's bias), which test GEQUAL and pass over them (SCREENS_NOTES.md) */
    {                                                    /* the PS2's screen ends at 0 and 640: the viewer's wider
                                                          * frame would show the title running on past them */
        Node *gg = layout_find(root, "gGoal");
        if (gg) {
            gg->clip = 1;
            gg->clip_x0 = 0;
            gg->clip_x1 = 640;
        }
    }
    char msg[MAX_TEXT];
    if (s->last_block && r->buildings != 0 && s->results_text[0]) {
        const char *t = loc_find(&h->gui, s->results_text);
        res_marquee(h, s, t ? t : s->results_text);
    } else {
        const char *fmt = loc_find(&h->gui, "Results_CompletlyDestroyed"), *bn = loc_find(&h->gui, s->played);
        const char *at = fmt ? strstr(fmt, "%s") : NULL;
        if (!bn) bn = s->played;
        if (at) snprintf(msg, sizeof msg, "%.*s%s%s", (int)(at - fmt), fmt, bn, at + 2);
        else snprintf(msg, sizeof msg, "%s", bn);
        res_marquee(h, s, msg);
    }
    if (s->title1 && s->title2) {
        s->title2->p[0] = s->left_x + s->title_w + 150.0f;
        node_text(s->title2, s->title1->text);
    }
    if (r->city_destroyed) {
        show(layout_find(root, "select"), 0);
        show(layout_find(root, "quit"), 0);
    }
    /* setupButtons: the buttons' left edges at 590 less the widest shown */
    Node *bt = layout_find(root, "buttons");
    float w = 0;
    for (Node *c = bt ? bt->child : NULL; c; c = c->next)
        if (strcmp(c->name, "save") != 0 && c->visible) {
            float wt = text_w(&h->gui, c) * c->m;
            if (wt > w) w = wt;
        }
    for (Node *c = bt ? bt->child : NULL; c; c = c->next)
        if (strcmp(c->name, "save") != 0)
            c->p[0] = 590.0f - w;
    /* setupIntro */
    intro_x(s, "titlebarshadow", 0, 0.5f, -640, 0);
    intro_x(s, "titlebar", 0, 0.5f, -640, 0);
    intro_y(s, "alert", 0.33f, 0.75f, -44, 44);
    intro_y(s, "channel", 0.33f, 0.75f, -51, 51);
    Intro *it = intro_x(s, "channelNumber", 0.33f, 0.75f, 640, 503);
    if (it) { it->s0 = 3.0f; it->s1 = 0.85f; }
    intro_y(s, "grayDataField", 0.75f, 1.0f, -280, 80);
    intro_x(s, "orangeBand", 0.75f, 1.0f, 640, 60);
    it = intro_x(s, "Live", 0.75f, 1.0f, 640, 487);
    if (it) { it->p0[1] = 340; it->p1[1] = 340; }
    intro_x(s, "buttons", 0, 0.1f, 0, 0);
    intro_x(s, "goal1", 1.25f, 1.5f, 640, 62);
    intro_y(s, "pointsList", 1.25f, 1.5f, -40, 216);
    intro_y(s, "totalScore", 1.25f, 1.5f, -93, 163);
    intro_y(s, "TotalDamage", 1.25f, 1.5f, -121, 135);
    float save_t = 3.0f * (float)(s->allow_save != 0);   /* 3 x gCommon.getAllowSave(): the autosave's time */
    static const char *const fades[3] = { "continue", "select", "quit" };
    static const float ft[3][2] = { { 2.05f, 2.15f }, { 2.15f, 2.26f }, { 2.25f, 2.36f } };
    for (int i = 0; i < 3; i++) {
        it = intro_item(s, s->intro, &s->nintro, MAX_INTRO, fades[i]);
        if (it) { it->t0 = ft[i][0] + save_t; it->t1 = ft[i][1] + save_t; it->f0 = 0; it->f1 = 1; }
    }
    for (int i = 0; i < s->nintro; i++) {
        memcpy(s->intro[i].obj->p, s->intro[i].p0, sizeof s->intro[i].p0);
        s->intro[i].obj->m = s->intro[i].s0;
        s->intro[i].obj->fade = s->intro[i].f0;
    }
    /* setupWalkout */
    walk_item(s, "grayDataField", 0.5f, 1, 80, 480);
    walk_item(s, "pointsList", 0.5f, 1, 216, 616);
    walk_item(s, "totalScore", 0.5f, 1, 163, 563);
    walk_item(s, "TotalDamage", 0.5f, 1, 135, 535);
    walk_item(s, "orangeBand", 0.5f, 0, 60, 640);
    walk_item(s, "buttons", 0.5f, 0, 0, 600);
    it = walk_item(s, "Live", 1.0f, 1, 340, 408);
    if (it) { it->p0[0] = 487; it->p1[0] = 487; }
    res_button_colours(s);
    cq_init(s);
}

/* Results::processIntro: returns its flag (the script's niFlag, passed by reference: the PP interpreter passes a
 * writable variable uncopied, 0x1B9870 / 0x1C3270 [R]); cleared before its early return */
static void hud_call(Hud *h, int what, float value, int arg);

static int results_intro(Hud *h, Screen *s, float dt)
{
    float prev = s->intro_t;
    s->intro_t += dt;
    int flag = 0, count;
    Intro *items;
    if (s->ni_state == 0) { items = s->intro; count = s->nintro; }
    else if (s->ni_state == 1) { items = s->walk; count = s->nwalk; }
    else return 0;
    for (int i = 0; i < count; i++) {
        Intro *it = &items[i];
        if (it->done)
            continue;
        flag = 1;
        if (s->intro_t < it->t0)
            continue;
        float p = it->t1 > it->t0 ? (s->intro_t - it->t0) / (it->t1 - it->t0) : 1;
        if (s->intro_t >= it->t1) {
            p = 1;
            it->done = 1;
        }
        for (int k = 0; k < 3; k++)
            it->obj->p[k] = it->p0[k] + (it->p1[k] - it->p0[k]) * p;
        it->obj->m = it->s0 + p * (it->s1 - it->s0);
        it->obj->fade = it->f0 + p * (it->f1 - it->f0);
    }
    if (s->ni_state == 0 && s->kind == SCR_LEVEL) {
        /* LevelResults: once the items are in, a star every 0.75 s for each of the city's flags 1 2 4 8 (the first at
         * once, the items taking longer) */
        static const char *const ratings[4] = { "allBlocks", "parScore", "challenges", "reveal" };
        if (!flag) {
            if (s->intro_t >= 0.75f) {
                s->intro_t = 0;
                while (s->rating != 4) {
                    s->rating++;
                    if (s->r.city_flags & (1u << s->rating))
                        break;
                }
                if (s->rating != 4) {
                    Node *o = layout_find(s->L->root, ratings[s->rating]);
                    show(o ? layout_find(o->child, "star") : NULL, 1);
                    sound(h, "Tank_Fire", 0);
                }
            }
            flag = s->rating != 4;
        }
    } else if (s->ni_state == 0) {
        if (prev < 1.5f && s->intro_t >= 1.5f) {         /* bTrackSave; gCommon.updateProfile, saveProfile */
            s->track_save = 1;
            hud_call(h, HUD_CALL_SAVE_PROFILE, 0.0f, 1);
        }
        s->scroll = s->intro_t >= 1.5f;
        if (!s->count_up)
            s->count_up = s->cu_index == -1 && s->intro_t >= 1.5f;
        if (!s->sound_on) {
            s->sound_on = 1;
            sound(h, "Slot_Jackpot", 0);
        }
    }
    if (!flag) {
        s->ni_state++;
        s->intro_t = 0;
    }
    return flag;
}

/* Results::scrollTitle: the two copies of the title run left at 150 a second, 150 apart; the "Live" box blinks */
static void results_scroll(Hud *h, Screen *s, float dt)
{
    if (s->title1 && s->title2) {
        Node *a = s->title1, *b = s->title2;
        a->p[0] -= 150.0f * dt;
        if (a->p[0] <= s->left_x - s->title_w - 150.0f) {
            node_text(a, s->base ? s->base->text : a->text);
            a->p[0] = b->p[0] + (text_w(&h->gui, b) + 150.0f);
            if (a->p[0] < s->right_x) a->p[0] = s->right_x;
        }
        b->p[0] -= 150.0f * dt;
        if (b->p[0] <= s->left_x - s->title_w - 150.0f) {
            node_text(b, s->base ? s->base->text : b->text);
            b->p[0] = a->p[0] + (text_w(&h->gui, a) + 150.0f);
            if (b->p[0] < s->right_x) b->p[0] = s->right_x;
        }
    }
    s->live_delay += dt;
    if (s->live_delay >= 0.5f) {
        s->live_delay = 0;
        s->live_vis = !s->live_vis;
        show(s->live, s->live_vis);
    }
}

/* Results::countUpScores: each line (destruction, people, vehicles, bonus, time bonus, challenge: the point
 * categories 0 .. 5, all players' together) counts up from 0 in half a second, the total with it */
static void results_count(Hud *h, Screen *s, float dt)
{
    char buf[32];
    const HudResults *r = &s->r;
    if (s->cu_progress < 0) {
        s->cu_progress = 0;
        s->cu_index++;
        s->cu_total += s->cu_value;
        int count = list_count(s->list);
        Node *it = list_item(s->list, s->cu_index);
        if (s->cu_index >= count || !it) {
            s->count_up = 0;
            s->sound_on = 0;
            sound(h, "Slot_Jackpot", 1);
            return;
        }
        show(it, 1);
        Node *cnt = layout_find(it->child, "count");
        s->cu_item = layout_find(it->child, "points");
        s->cu_value = 0;
        int kills = 0, np = r->num_players > 0 && r->num_players <= 4 ? r->num_players : 1;
        for (int i = 0; i < np; i++) {
            s->cu_value += r->points[i][s->cu_index < 7 ? s->cu_index : 6];
            kills += r->kills[i][s->cu_index < 7 ? s->cu_index : 6];
        }
        node_text(s->cu_item, "0");
        snprintf(buf, sizeof buf, "%d", kills);
        node_text(cnt, buf);
        if (s->cu_index == count - 1 && s->last_block && s->cu_value == 0) {   /* no boss defeated: "No" */
            s->count_up = 0;
            s->sound_on = 0;
            show(s->cu_item, 0);
            show(layout_find(it->child, "no"), 1);
            sound(h, "Slot_Jackpot", 1);
            s->cu_index++;
            return;
        }
    }
    s->cu_progress += dt / 0.5f;
    float p = s->cu_progress;
    if (s->cu_progress >= 1.0f) {
        p = 1.0f;
        s->cu_progress = -1.0f;
    }
    fmt_value(buf, sizeof buf, (int)((float)s->cu_value * p));
    node_text(s->cu_item, buf);
    fmt_value(buf, sizeof buf, s->cu_total + (int)((float)s->cu_value * p));
    node_text(s->total, buf);
}

static void results_quit(Hud *h, Screen *s)             /* Results::onQuit */
{
    (void)h;
    if (!s->r.city_destroyed) {
        s->exit_fn = 1;
        s->exit_val = HUD_RESULT_QUIT;
    }
}

/* Results::doInterface 2048 [R] */
static void results_frame(Hud *h, Screen *s, float dt)
{
    if (s->exit_fn && !h->save_active) {                 /* the held call, once the SaveGame popup is gone */
        if (s->exit_fn == 1) set_result(h, s->exit_val);
        else s->ended = 1;
        s->exit_fn = 0;
    }
    if (dt > 0.5f)
        return;
    s->save_delay -= dt;
    if (s->track_save && s->save) {                      /* Save shows with a profile and autosave off; hidden
                                                          * again (pressed): saveProfile */
        int was = s->save->visible;
        show(s->save, s->r.loaded && !s->allow_save);
        if (was && !s->save->visible)
            hud_call(h, HUD_CALL_SAVE_PROFILE, 0.0f, 0);
    }
    if (s->do_intro)
        s->do_intro = results_intro(h, s, dt);
    if (s->scroll)
        results_scroll(h, s, dt);
    if (s->count_up)
        results_count(h, s, dt);
    if (s->do_walkout)
        s->do_walkout = results_intro(h, s, dt);
    if (s->ni_state == 1) {
        if (!s->last_block && s->r.monster_revealed) {   /* "Monster Revealed" for 1 .. 5 s */
            Node *o = layout_find(s->L->root, "revealed");
            if (s->pulse_t < 1.0f) fader_go(&s->fd, o, 1);
            else if (s->pulse_t > 5.0f) fader_go(&s->fd, o, -1);
        }
        s->pulse_t += dt;
    } else if (s->ni_state == 2) {                       /* walked out: Continue */
        s->exit_fn = 1;
        s->exit_val = HUD_RESULT_DONE;
        s->intro_t = 5.0f;
        s->ni_state = 3;
    } else if (s->ni_state == 3) {                       /* 5 s more on screen */
        s->intro_t -= dt;
        if (s->intro_t <= 0) {
            s->intro_t = 1.0f;
            s->ni_state = 4;
        }
    } else if (s->ni_state == 4) {                       /* then fades out in half a second */
        s->intro_t -= 2.0f * dt;
        if (s->intro_t <= 0) {
            s->ni_state = 5;
            s->exit_fn = 2;
        } else
            s->L->root->fade = s->intro_t;
    }
    fader_update(&s->fd, dt);
}

static int results_ready(const Screen *s)
{
    return !s->cq.state && !s->do_intro && !s->do_walkout && s->ni_state == 1;
}

/* Results::doInterface, the buttons [R] */
static void results_input(Hud *h, Screen *s, int ev, int button)
{
    if (cq_process(h, s, ev))
        return;
    if (s->do_intro || s->do_walkout || s->ni_state > 1)
        return;
    Node *root = s->L->root;
    if (ev == HUD_EV_START || ev == HUD_EV_ACCEPT) {
        if (s->save_delay > 0)
            return;
        if (s->ni_state == 1 && !s->do_walkout) {
            sound(h, "Click", 0);
            s->track_save = 0;
            if (!s->last_block && strcmp(s->current, s->played) != 0) {   /* "MOVING ON TO <the next block>" */
                const char *mo = loc_find(&h->gui, "Results_MovingOnTo"), *bn = loc_find(&h->gui, s->current);
                char msg[MAX_TEXT];
                snprintf(msg, sizeof msg, "%s%s", mo ? mo : "MOVING ON TO ", bn ? bn : s->current);
                res_marquee(h, s, msg);
            }
            s->do_walkout = 1;
        }
    } else if (ev == HUD_EV_BACK) {
        Node *q = layout_find(root, "quit");
        if (!s->do_walkout && q && q->visible) {
            sound(h, "Cancel", 0);
            cq_show(h, s, !(s->save && s->save->visible));
        }
    } else if (ev == HUD_EV_BUTTON && button == 0) {
        Node *sel = layout_find(root, "select");
        if (!s->do_walkout && sel && sel->visible) {
            sound(h, "Click", 0);
            s->exit_fn = 1;
            s->exit_val = HUD_RESULT_SELECT;
        }
    } else if (ev == HUD_EV_BUTTON && button == 1) {
        if (!s->do_walkout && s->save && s->save->visible) {   /* Save: autosave on, the button hides next frame */
            sound(h, "Click", 0);
            s->allow_save = 1;
            hud_call(h, HUD_CALL_ALLOW_SAVE, 1.0f, 0);
            s->save_delay = 0.25f;
        }
    }
}

/* LevelResults.scr [R]: the city's name, DESTROYED, its score against the par, a star for each goal met */
static void level_start(Hud *h, Screen *s)
{
    static const char *const ratings[4] = { "allBlocks", "parScore", "challenges", "reveal" };
    Node *root = s->L->root;
    char buf[32];
    node_localize(&h->gui, layout_find(root, "cityName"), s->city);
    s->do_intro = 1;
    s->rating = -1;
    root->fade = 1;
    for (int i = 0; i < 4; i++) {
        Node *o = layout_find(root, ratings[i]);
        show(o ? layout_find(o->child, "star") : NULL, 0);
    }
    snprintf(buf, sizeof buf, "%d", s->r.city_score);
    node_text(layout_find(root, "totalScore"), buf);
    snprintf(buf, sizeof buf, "%d", s->r.par_score);
    node_text(layout_find(root, "parScoreValue"), buf);
    if (s->r.show_victory) {
        show(layout_find(root, "select"), 0);
        show(layout_find(root, "quit"), 0);
    }
    /* setupButtons: left edges at 590 less the widest; setupRatings: the lines centred as a block, 32 for the star */
    static const char *const groups[2] = { "buttons", "ratings" };
    for (int gi = 0; gi < 2; gi++) {
        Node *gob = layout_find(root, groups[gi]);
        float w = 0;
        for (Node *c = gob ? gob->child : NULL; c; c = c->next)
            if (c->visible) {
                float wt = text_w(&h->gui, c) * c->m;
                if (wt > w) w = wt;
            }
        for (Node *c = gob ? gob->child : NULL; c; c = c->next)
            c->p[0] = gi == 0 ? 590.0f - w : 320.0f - (w - 32.0f) / 2.0f;
    }
    /* setupIntro */
    intro_x(s, "cityName", 0, 0.5f, -640, 320);
    intro_x(s, "destroyed", 0, 0.5f, 640, 320);
    intro_x(s, "TotalDamage", 0, 0.5f, 1024, 350);
    intro_x(s, "ParScoreTitle", 0, 0.5f, 0, 350);
    static const char *const fades[3] = { "continue", "select", "quit" };
    static const float ft[3][2] = { { 1.65f, 1.75f }, { 1.75f, 1.86f }, { 1.85f, 1.96f } };
    for (int i = 0; i < 3; i++) {
        Intro *it = intro_item(s, s->intro, &s->nintro, MAX_INTRO, fades[i]);
        if (it) { it->t0 = ft[i][0]; it->t1 = ft[i][1]; it->f0 = 0; it->f1 = 1; }
    }
    float y = 245.0f, t = 0.5f;
    for (int i = 0; i < 4; i++, y += 30.0f, t += 0.2f)
        intro_y(s, ratings[i], 0, t, y + 480.0f - 245.0f, y);
    for (int i = 0; i < s->nintro; i++) {
        memcpy(s->intro[i].obj->p, s->intro[i].p0, sizeof s->intro[i].p0);
        s->intro[i].obj->m = s->intro[i].s0;
        s->intro[i].obj->fade = s->intro[i].f0;
    }
    /* setupWalkout: nothing walks out */
    res_button_colours(s);
    cq_init(s);
}

static void level_frame(Hud *h, Screen *s, float dt)
{
    if (s->do_intro)
        s->do_intro = results_intro(h, s, dt);
    if (s->do_walkout)
        s->do_walkout = results_intro(h, s, dt);
    if (s->ni_state == 2) {
        set_result(h, HUD_RESULT_DONE);
        s->intro_t = 5.0f;
        s->ni_state = 3;
    } else if (s->ni_state == 3) {
        s->intro_t -= dt;
        if (s->intro_t <= 0) {
            s->intro_t = 1.0f;
            s->ni_state = 4;
        }
    } else if (s->ni_state == 4) {
        s->intro_t -= 2.0f * dt;
        if (s->intro_t <= 0) {
            s->ni_state = 5;
            s->ended = 1;
        } else
            s->L->root->fade = s->intro_t;
    }
    fader_update(&s->fd, dt);
}

static void level_input(Hud *h, Screen *s, int ev, int button)
{
    if (cq_process(h, s, ev))
        return;
    if (s->do_intro || s->do_walkout || s->ni_state > 1)
        return;
    Node *root = s->L->root, *o;
    if (ev == HUD_EV_START || ev == HUD_EV_ACCEPT) {
        if (s->ni_state == 1 && !s->do_walkout) {
            sound(h, "Click", 0);
            s->do_walkout = 1;
        }
    } else if (ev == HUD_EV_BACK) {
        if ((o = layout_find(root, "quit")) != NULL && o->visible) {
            sound(h, "Cancel", 0);
            cq_show(h, s, 1);
        }
    } else if (ev == HUD_EV_BUTTON && button == 0) {
        if ((o = layout_find(root, "select")) != NULL && o->visible) {
            sound(h, "Click", 0);
            set_result(h, HUD_RESULT_SELECT);
        }
    }
}

/* GameOver.scr [R] */
static void gameover_start(Hud *h, Screen *s)
{
    s->choices = layout_find(s->L->root, "choices");
    if (s->choices) s->choices->pulse = 1;
    list_select(s->choices, 0);
    if (s->r.play_type == HUD_PLAY_TIMED)
        show(list_item(s->choices, 1), 0);
    h->paused = 1;                                       /* gApp.pauseGame(1) */
    cq_init(s);
}

static void gameover_quit(Hud *h, Screen *s)            /* GameOver::onQuit */
{
    h->paused = 0;
    set_result(h, HUD_RESULT_QUIT);
    s->ended = 1;
}

static void gameover_input(Hud *h, Screen *s, int ev)
{
    if (cq_process(h, s, ev)) {
        if (!s->cq.state && s->choices)
            s->choices->pulse = 1;
        return;
    }
    Node *l = s->choices;
    int sel = l ? l->sel : 0;
    if (ev == HUD_EV_LEFT || ev == HUD_EV_UP || ev == HUD_EV_RIGHT || ev == HUD_EV_DOWN) {
        list_increment(l, ev == HUD_EV_LEFT || ev == HUD_EV_UP ? -1 : 1);
        if (l && l->sel != sel)
            sound(h, "Rollover", 0);
    } else if (ev == HUD_EV_START || ev == HUD_EV_ACCEPT) {
        sound(h, "Click", 0);
        if (sel == 2) {
            if (l) l->pulse = 0;
            cq_show(h, s, 0);
            return;
        }
        h->paused = 0;
        if (sel == 0) set_result(h, HUD_RESULT_CONTINUE);
        else if (sel == 1) set_result(h, HUD_RESULT_SELECT);
        s->ended = 1;
    }
}

/* ---------------------------------------------------------------- WorldResults, the King screens, TimedRunResults */
/* Their scripts follow Results.scr [R WorldResults.scr, KingBlockResults.scr, KingCityResults.scr,
 * KingWorldResults.scr, TimedRunResults.scr]: an intro of timed items (from / to, scale, fade), the scrolling title
 * and the "Live" box, a count-up (KingBlockResults: the players' points; TimedRunResults: the times), Continue for
 * the walkout, then setResultState(1), 5 s, a 0.5 s fade, endResults. Their processIntro takes niFlag by value like
 * Results.scr's; it is taken by reference here, as there [G]. */

/* setupIntroItem into list 0 introItems, 1 walkoutItems, 2 rankItems or 3 altWalkoutItems, t0 .. t1, moving along
 * axis (0 x, 1 y, -1 none) from a to b */
static Intro *kr_item(Screen *s, int list, const char *name, float t0, float t1, int axis, float a, float b)
{
    Intro *it = list == 0 ? intro_item(s, s->intro, &s->nintro, MAX_INTRO, name) :
                list == 1 ? intro_item(s, s->walk, &s->nwalk, MAX_WALK, name) :
                list == 2 ? intro_item(s, s->rank, &s->nrank, MAX_WALK, name) :
                intro_item(s, s->alt, &s->nalt, MAX_WALK, name);
    if (!it)
        return NULL;
    it->t0 = t0;
    it->t1 = t1;
    if (axis >= 0) {
        it->p0[axis] = a;
        it->p1[axis] = b;
    }
    return it;
}
static Intro *kr_fade(Screen *s, int list, const char *name, float t0, float t1, float f0, float f1)
{
    Intro *it = kr_item(s, list, name, t0, t1, -1, 0, 0);
    if (it) { it->f0 = f0; it->f1 = f1; }
    return it;
}
/* setupIntro's end: each intro item where it starts */
static void intro_apply(Screen *s)
{
    for (int i = 0; i < s->nintro; i++) {
        memcpy(s->intro[i].obj->p, s->intro[i].p0, sizeof s->intro[i].p0);
        s->intro[i].obj->m = s->intro[i].s0;
        s->intro[i].obj->fade = s->intro[i].f0;
    }
}
/* the news flash's intro, the same in the King screens and TimedRunResults (Results.scr's first ten) */
static void kr_news_intro(Screen *s)
{
    kr_item(s, 0, "titlebarshadow", 0, 0.5f, 0, -640, 0);
    kr_item(s, 0, "titlebar", 0, 0.5f, 0, -640, 0);
    kr_item(s, 0, "alert", 0.33f, 0.75f, 1, -44, 44);
    kr_item(s, 0, "channel", 0.33f, 0.75f, 1, -51, 51);
    Intro *it = kr_item(s, 0, "channelNumber", 0.33f, 0.75f, 0, 640, 503);
    if (it) { it->s0 = 3.0f; it->s1 = 0.85f; }
    kr_item(s, 0, "grayDataField", 0.75f, 1.0f, 1, -280, 80);
    kr_item(s, 0, "orangeBand", 0.75f, 1.0f, 0, 640, 60);
    it = kr_item(s, 0, "Live", 0.75f, 1.0f, 0, 640, 487);
    if (it) it->p0[1] = it->p1[1] = 340;
    kr_item(s, 0, "buttons", 0, 0.1f, 0, 0, 0);
    kr_item(s, 0, "goal1", 1.25f, 1.5f, 0, 640, 62);
}
/* and its walkout: the grey field drops, the band and the buttons slide right, the "Live" box sinks */
static void kr_news_walkout(Screen *s, int list)
{
    kr_item(s, list, "grayDataField", 0, 0.5f, 1, 80, 480);
    kr_item(s, list, "orangeBand", 0, 0.5f, 0, 60, 640);
    kr_item(s, list, "buttons", 0, 0.5f, 0, 0, 600);
    Intro *it = kr_item(s, list, "Live", 0, 1.0f, 1, 340, 408);
    if (it) it->p0[0] = it->p1[0] = 487;
}

/* setupButtons: every button's left edge at 590 less the widest shown */
static void kr_buttons(Hud *h, Screen *s)
{
    Node *bt = layout_find(s->L->root, "buttons");
    float w = 0;
    for (Node *c = bt ? bt->child : NULL; c; c = c->next)
        if (c->visible) {
            float wt = text_w(&h->gui, c) * c->m;
            if (wt > w) w = wt;
        }
    for (Node *c = bt ? bt->child : NULL; c; c = c->next)
        c->p[0] = 590.0f - w;
}

/* string.format(localizeString(fmt_key), localizeString(arg_key)) */
static void loc_format(const Gui *g, char *out, size_t n, const char *fmt_key, const char *arg_key)
{
    const char *fmt = loc_find(g, fmt_key), *arg = loc_find(g, arg_key);
    const char *at = fmt ? strstr(fmt, "%s") : NULL;
    if (!arg)
        arg = arg_key ? arg_key : "";
    if (at)
        snprintf(out, n, "%.*s%s%s", (int)(at - fmt), fmt, arg, at + 2);
    else
        snprintf(out, n, "%s", fmt ? fmt : arg);
}

/* the scrolling title (goal1 and goal2, from goalbase, cut to 62 .. 503 as in Results [G]) set to text; "Live" */
static void kr_title(Hud *h, Screen *s, const char *text)
{
    Node *root = s->L->root;
    s->live = layout_find(root, "live");
    show(s->live, 1);
    s->live_vis = 1;
    s->title1 = layout_find(root, "goal1");
    s->title2 = layout_find(root, "goal2");
    s->base = layout_find(root, "goalbase");
    s->left_x = 62.0f;
    s->right_x = 503.0f;
    s->title_w = text_w(&h->gui, s->title1);
    Node *gg = layout_find(root, "gGoal");              /* the title runs on to the screen's edges (results_start) */
    if (gg) {
        gg->clip = 1;
        gg->clip_x0 = 0;
        gg->clip_x1 = 640;
    }
    res_marquee(h, s, text);
    if (s->title1 && s->title2) {
        s->title2->p[0] = s->left_x + s->title_w + 150.0f;
        node_text(s->title2, s->title1->text);
    }
}
/* Continue: setMarqueText(localizeString("Results_MovingOnTo") + localizeString(key)) */
static void kr_moving_on(Hud *h, Screen *s, const char *key)
{
    const char *mo = loc_find(&h->gui, "Results_MovingOnTo"), *bn = loc_find(&h->gui, key);
    char msg[MAX_TEXT];
    snprintf(msg, sizeof msg, "%s%s", mo ? mo : "MOVING ON TO ", bn ? bn : key ? key : "");
    res_marquee(h, s, msg);
}

static int kr_players(const Screen *s)
{
    return s->r.num_players > 0 && s->r.num_players <= 4 ? s->r.num_players : 1;
}
/* localizeString(getGameStatus().getMonsterName(slot)) [G: slot -1, no King, gives ""] */
static const char *kr_monster(const Hud *h, const Screen *s, int slot)
{
    if (slot < 0 || slot > 3)
        return "";
    const char *t = loc_find(&h->gui, s->monsters[slot]);
    return t ? t : s->monsters[slot];
}
/* the winner's line: localizeString("P" + (slot + 1)) + ": " + the monster's name ("P1: Lizzie") */
static void kr_winner(Hud *h, Screen *s, Node *t, int slot)
{
    char key[16], buf[MAX_TEXT];
    snprintf(key, sizeof key, "P%d", slot + 1);
    const char *p = loc_find(&h->gui, key);
    snprintf(buf, sizeof buf, "%s: %s", p ? p : key, kr_monster(h, s, slot));
    node_text(t, buf);
}
/* one row of a standings list: the item localized "P<slot + 1>", its "name" ": <monster>", its child `value_of`
 * the value */
static void kr_row(Hud *h, Screen *s, Node *item, int slot, const char *value_of, const char *value)
{
    if (!item)
        return;
    char key[16], buf[MAX_TEXT];
    snprintf(key, sizeof key, "P%d", slot + 1);
    node_localize(&h->gui, item, key);
    snprintf(buf, sizeof buf, ": %s", kr_monster(h, s, slot));
    node_text(layout_find(item->child, "name"), buf);
    if (value_of)
        node_text(layout_find(item->child, value_of), value);
}
/* orderPlayers' exchange sort over the first n, most first, as written (not stable) */
static void kr_sort(int *ids, int *scores, int n)
{
    for (int i = 0; i < n; i++)
        for (int j = i; j < n; j++)
            if (scores[j] > scores[i]) {
                int k = ids[i]; ids[i] = ids[j]; ids[j] = k;
                k = scores[i]; scores[i] = scores[j]; scores[j] = k;
            }
}
/* the players by blocks won (KingBlockResults' br_list, KingCityResults, KingWorldResults) */
static void kr_rank_by_blocks(const Screen *s, int rank[4])
{
    int scores[4] = { 0, 0, 0, 0 }, np = kr_players(s);
    for (int i = 0; i < 4; i++) {
        rank[i] = i;
        scores[i] = i < np ? s->r.blocks_won[i] : 0;
    }
    kr_sort(rank, scores, np);
}

/* TimedRunResults::getFormattedValue: ":ss" under a minute, else "m:ss" */
static void tr_time(char *out, size_t n, int v)
{
    if (v < 60)
        snprintf(out, n, ":%02d", v);
    else
        snprintf(out, n, "%d:%02d", v / 60, v % 60);
}

/* WorldResults::doInterface 8192: "TOTAL DESTRUCTION!" / "ALL CITIES DESTROYED!" between two red bands; Continue
 * fades the screen to black */
static void world_start(Hud *h, Screen *s)
{
    Node *root = s->L->root;
    s->do_intro = 1;
    root->fade = 1;
    kr_buttons(h, s);
    kr_item(s, 0, "titlebar1", 0, 0.5f, 0, 320.0f - 640.0f, 320);
    kr_item(s, 0, "totalDestruction", 0, 0.5f, 0, 320.0f + 640.0f, 320);
    kr_item(s, 0, "destroyed", 0, 0.5f, 0, 320.0f - 640.0f, 320);
    kr_item(s, 0, "titlebar2", 0, 0.5f, 0, 640, 320);       /* its startPos.x set twice: 960, then 640 */
    kr_fade(s, 0, "continue", 1.55f, 1.65f, 0, 1);
    kr_fade(s, 0, "fade", 0, 0, 0, 0);
    intro_apply(s);
    show(layout_find(root, "fade"), 0);
    kr_fade(s, 1, "fade", 0, 0.5f, 0, 1);                  /* setupWalkout: the black veil over 0.5 s */
    button_colour(s, "continue", 0);
}

/* KingBlockResults::doInterface 8192: the block's winner ("P1: George" / "Wins this block", or "It Is A Tie!"), the
 * players' points counting up in their order, the winner's under it; Continue swaps in the city standings (blocks
 * won of the blocks to win), Continue again walks out. With a King of the City the standings are skipped (the
 * alternative walkout). */
static void kb_start(Hud *h, Screen *s)
{
    Node *root = s->L->root;
    const HudResults *r = &s->r;
    int np = kr_players(s), scores[4], rank[4];
    char msg[MAX_TEXT];
    s->do_intro = 1;
    s->cu_index = -1;
    s->cu_progress = -1;
    root->fade = 1;
    set_result(h, HUD_RESULT_NONE);
    s->list = layout_find(root, "pointsList");
    for (int i = 1; i < list_count(s->list); i++)
        show(list_item(s->list, i), 0);
    s->total = layout_find(root, "winnerScore");
    node_text(s->total, "0");
    int tie = r->king_block == -1;
    show(layout_find(root, "itsatie"), tie);
    show(layout_find(root, "replay"), tie);
    show(layout_find(root, "playerwins"), !tie);
    show(layout_find(root, "winsblock"), !tie);
    show(layout_find(root, "continue"), !tie);
    if (!tie)
        kr_winner(h, s, layout_find(root, "playerwins"), r->king_block);
    loc_format(&h->gui, msg, sizeof msg, "Results_CompletlyDestroyed", s->played);
    kr_title(h, s, msg);
    /* orderPlayers: the points list by points (category 6), the block standings by blocks won */
    for (int i = 0; i < 4; i++) {
        s->ids[i] = i;
        scores[i] = r->points[i][6];
    }
    kr_sort(s->ids, scores, np);
    for (int i = 0; i < np; i++)
        kr_row(h, s, list_item(s->list, i), s->ids[i], NULL, NULL);
    kr_rank_by_blocks(s, rank);
    Node *bl = layout_find(root, "br_list");
    for (int i = 0; i < np; i++) {
        snprintf(msg, sizeof msg, "%d/%d", r->blocks_won[rank[i]], r->blocks_to_win);
        kr_row(h, s, list_item(bl, i), rank[i], "points", msg);
    }
    for (int i = 0; i < 4; i++)
        show(list_item(bl, i), i < np);
    if (r->city_destroyed || r->king_city != -1)
        show(layout_find(root, "quit"), 0);
    kr_buttons(h, s);
    /* setupIntro */
    kr_item(s, 0, "br_winner", 0, 0, 0, -320, -320);         /* the standings wait off the screen */
    kr_item(s, 0, "br_list", 0, 0, 0, -265, -265);
    kr_news_intro(s);
    kr_item(s, 0, "pointsList", 1.25f, 1.5f, 1, -40, 281);
    kr_item(s, 0, "playerwins", 2.25f, 2.5f, 0, -640, 320);
    kr_item(s, 0, "winsblock", 2.25f, 2.5f, 0, 1280, 320);
    kr_item(s, 0, "itsatie", 2.25f, 2.5f, 0, -640, 320);
    kr_item(s, 0, "winnerScore", 2.5f, 2.75f, 1, -93, 205);
    kr_fade(s, 0, "continue", 2.45f, 2.55f, 0, 1);
    kr_fade(s, 0, "replay", 2.55f, 2.65f, 0, 1);
    kr_fade(s, 0, "quit", 2.65f, 2.76f, 0, 1);
    intro_apply(s);
    /* the later lists' items start where the objects are now, at the intro's start: br_winner's walkout begins at
     * x -320 (it leaves the screen at once) and the points list's rank move at y 216, not the 281 it came to [R as
     * written] */
    s->alt_walkout = r->king_city != -1;
    if (s->alt_walkout) {                                    /* setupAltWalkout */
        kr_news_walkout(s, 3);
        kr_item(s, 3, "pointsList", 0, 0.5f, 1, 216, 616);
        kr_item(s, 3, "winnerScore", 0, 0.5f, 1, 163, 563);
        kr_fade(s, 3, "playerwins", 0, 1.0f, 1, 0);
        kr_fade(s, 3, "winsblock", 0, 1.0f, 1, 0);
        kr_fade(s, 3, "itsatie", 1.0f, 2.0f, 1, 0);
    } else {
        kr_item(s, 2, "br_winner", 0.25f, 0.75f, 0, -320, 320);   /* setupRank */
        kr_item(s, 2, "br_list", 0.25f, 0.75f, 0, -265, 65);
        kr_item(s, 2, "pointsList", 0, 0.5f, 1, 216, 616);
        kr_item(s, 2, "winnerScore", 0, 0.5f, 1, 163, 563);
        kr_fade(s, 2, "playerwins", 0, 0.5f, 1, 0);
        kr_fade(s, 2, "winsblock", 0, 0.5f, 1, 0);
        kr_fade(s, 2, "itsatie", 0, 0.5f, 1, 0);
        kr_item(s, 1, "br_winner", 0.25f, 0.75f, 1, 110, 510);   /* setupWalkout */
        kr_item(s, 1, "br_list", 0.25f, 0.75f, 1, 270, 670);
        kr_news_walkout(s, 1);
    }
    button_colour(s, "continue", 0);
    button_colour(s, "quit", 1);
    button_colour(s, "replay", 0);
    cq_init(s);
}

/* KingBlockResults::countUpScores: each player's points in their order, half a second each; the first's also under
 * the winner's line */
static void kb_count(Hud *h, Screen *s, float dt)
{
    char buf[32];
    if (s->cu_progress < 0) {
        s->cu_progress = 0;
        s->cu_index++;
        s->cu_total += s->cu_value;
        if (s->cu_index >= kr_players(s)) {
            sound(h, "Slot_Jackpot", 1);
            s->sound_on = 0;
            s->count_up = 2;
            return;
        }
        Node *it = list_item(s->list, s->cu_index);
        show(it, 1);
        s->cu_value = s->r.points[s->ids[s->cu_index]][6];
        s->cu_item = it ? layout_find(it->child, "points") : NULL;
        node_text(s->cu_item, "0");
    }
    s->cu_progress += dt / 0.5f;
    float p = s->cu_progress;
    if (s->cu_progress >= 1.0f) {
        p = 1.0f;
        s->cu_progress = -1.0f;
    }
    fmt_value(buf, sizeof buf, (int)((float)s->cu_value * p));
    node_text(s->cu_item, buf);
    if (s->cu_index == 0)
        node_text(s->total, buf);
}

/* KingCityResults::doInterface 8192: "P1: George" / "is King of <city>", the city (King of the World: the world)
 * standings, blocks (cities) won of those to win. The King of the City's Continue answers 4 (back to the menus);
 * the King of the World's walks out, "MOVING ON TO <the next city>" */
static void kc_start(Hud *h, Screen *s)
{
    Node *root = s->L->root;
    const HudResults *r = &s->r;
    int np = kr_players(s), rank[4], world = r->play_type == HUD_PLAY_KING_WORLD;
    char msg[MAX_TEXT];
    s->do_intro = 1;
    root->fade = 1;
    set_result(h, HUD_RESULT_NONE);
    s->list = layout_find(root, "pointsList");
    kr_winner(h, s, layout_find(root, "playerwins"), r->king_city);
    const char *ik = loc_find(&h->gui, "Results_IsKingOf"), *cn = loc_find(&h->gui, s->city);
    snprintf(msg, sizeof msg, "%s%s", ik ? ik : "is King of ", cn ? cn : s->city);
    node_text(layout_find(root, "winscity"), msg);
    loc_format(&h->gui, msg, sizeof msg, "Results_CompletlyDestroyed", s->city);
    kr_title(h, s, msg);
    if (r->play_type == HUD_PLAY_KING_CITY || r->king_world != -1)
        show(layout_find(root, "quit"), 0);
    /* orderPlayers: by blocks won (in both modes) */
    kr_rank_by_blocks(s, rank);
    node_localize(&h->gui, layout_find(root, "standings_type"), world ? "Results_WorldStandings" :
                                                                         "Results_CityStandings");
    for (int i = 0; i < np; i++) {
        snprintf(msg, sizeof msg, "%d/%d", world ? r->cities_won[rank[i]] : r->blocks_won[rank[i]],
                 world ? r->cities_to_win : r->blocks_to_win);
        kr_row(h, s, list_item(s->list, i), rank[i], "wins", msg);
    }
    for (int i = 0; i < 4; i++)
        show(list_item(s->list, i), i < np);
    kr_buttons(h, s);
    kr_news_intro(s);
    kr_item(s, 0, "pointsList", 1.25f, 1.5f, 1, -40, 275);
    kr_item(s, 0, "standings_type", 1.25f, 1.5f, 1, -60, 255);
    kr_item(s, 0, "playerwins", 2.25f, 2.5f, 0, -640, 320);
    kr_item(s, 0, "winscity", 2.25f, 2.5f, 0, 1280, 320);
    kr_fade(s, 0, "continue", 2.45f, 2.55f, 0, 1);
    kr_fade(s, 0, "quit", 2.55f, 2.66f, 0, 1);
    intro_apply(s);
    kr_news_walkout(s, 1);
    kr_item(s, 1, "pointsList", 0, 0.5f, 1, 265, 616);
    kr_item(s, 1, "standings_type", 0, 0.5f, 1, 245, 606);
    kr_fade(s, 1, "playerwins", 0, 1.0f, 1, 0);
    kr_fade(s, 1, "winscity", 0, 1.0f, 1, 0);
    button_colour(s, "continue", 0);
    button_colour(s, "quit", 1);
    cq_init(s);
}

/* KingWorldResults::doInterface 8192: "THE WORLD COMPLETELY DESTROYED", "P1: George" / "is King of the World" (the
 * winner asked for is getKingOfCity(): the last city's King, the world's), the cities won of those to win; OK
 * answers 4 (the script has no walkout trigger and no ConfirmQuit) */
static void kw_start(Hud *h, Screen *s)
{
    Node *root = s->L->root;
    const HudResults *r = &s->r;
    int np = kr_players(s), rank[4];
    char msg[MAX_TEXT];
    s->do_intro = 1;
    root->fade = 1;
    set_result(h, HUD_RESULT_NONE);
    kr_winner(h, s, layout_find(root, "playerwins"), r->king_city);
    const char *wd = loc_find(&h->gui, "Results_WorldDestroyed");
    kr_title(h, s, wd ? wd : "Results_WorldDestroyed");
    kr_rank_by_blocks(s, rank);                              /* by blocks won, showing the cities won */
    Node *pl = layout_find(root, "pointsList");
    for (int i = 0; i < np; i++) {
        snprintf(msg, sizeof msg, "%d/%d", r->cities_won[rank[i]], r->cities_to_win);
        kr_row(h, s, list_item(pl, i), rank[i], "wins", msg);
    }
    for (int i = 0; i < 4; i++)
        show(list_item(pl, i), i < np);
    kr_buttons(h, s);                                        /* the quit button still counted */
    kr_news_intro(s);
    kr_item(s, 0, "playerwins", 2.25f, 2.5f, 0, -640, 320);
    kr_item(s, 0, "winsworld", 2.25f, 2.5f, 0, 1280, 320);
    kr_fade(s, 0, "continue", 2.45f, 2.55f, 0, 1);
    kr_fade(s, 0, "continue", 2.45f, 2.55f, 0, 1);           /* added twice [R] */
    intro_apply(s);
    kr_news_walkout(s, 1);
    kr_fade(s, 1, "playerwins", 0, 1.0f, 1, 0);
    kr_fade(s, 1, "winsworld", 0, 1.0f, 1, 0);
    show(layout_find(root, "quit"), 0);
    button_colour(s, "continue", 0);
    button_colour(s, "quit", 1);
}

/* TimedRunResults::doInterface 8192: "Block Destroyed" and the time left, the time added (the next block's
 * TimeLimit) and the total counting up (m:ss), or, the city done, "YOU WIN! City Destroyed" and the time left */
static void tr_start(Hud *h, Screen *s)
{
    Node *root = s->L->root;
    const HudResults *r = &s->r;
    char msg[MAX_TEXT];
    s->do_intro = 1;
    s->cu_index = -1;
    s->cu_progress = -1;
    root->fade = 1;
    set_result(h, HUD_RESULT_NONE);
    s->times[0] = layout_find(root, "timeRemaining");
    s->times[1] = layout_find(root, "bonusTime");
    s->times[2] = layout_find(root, "totalTime");
    for (int i = 0; i < 3; i++)
        show(s->times[i], 0);
    show(layout_find(root, "block_destroyed"), !r->city_destroyed);
    show(layout_find(root, "city_destroyed"), r->city_destroyed);
    if (r->city_destroyed) {
        tr_time(msg, sizeof msg, (int)r->clock_time);
        node_text(layout_find(root, "finalTimeRemaining"), msg);
    }
    loc_format(&h->gui, msg, sizeof msg, "Results_CompletlyDestroyed", s->played);
    kr_title(h, s, msg);
    kr_buttons(h, s);                                        /* quit still hidden (F 0x1): continue's width */
    kr_news_intro(s);
    kr_fade(s, 0, "block_destroyed", 1.0f, 1.5f, 0, 1);
    kr_fade(s, 0, "city_destroyed", 1.0f, 1.5f, 0, 1);
    kr_fade(s, 0, "continue", 1.45f, 1.55f, 0, 1);
    kr_fade(s, 0, "quit", 1.55f, 1.66f, 0, 1);
    intro_apply(s);
    kr_news_walkout(s, 1);
    kr_fade(s, 1, "block_destroyed", 0, 1.5f, 1, 0);
    kr_fade(s, 1, "city_destroyed", 0, 1.5f, 1, 0);
    show(layout_find(root, "quit"), !r->city_destroyed);
    button_colour(s, "continue", 0);
    button_colour(s, "quit", 1);
    cq_init(s);
}

/* TimedRunResults::countUpTime: the time left before (total - added), the time added, the total, half a second
 * each; nothing when the city is done */
static void tr_count(Screen *s, float dt)
{
    char buf[32];
    if (s->cu_progress < 0) {
        s->cu_progress = 0;
        s->cu_index++;
        if (s->cu_index >= 3 || s->r.city_destroyed) {
            s->count_up = 2;
            return;
        }
        int total = (int)s->r.clock_time, bonus = (int)s->r.clock_limit;
        s->cu_value = s->cu_index == 0 ? total - bonus : s->cu_index == 1 ? bonus : total;
        s->cu_item = s->times[s->cu_index];
        show(s->cu_item, 1);
    }
    s->cu_progress += dt / 0.5f;
    float p = s->cu_progress;
    if (s->cu_progress >= 1.0f) {
        p = 1.0f;
        s->cu_progress = -1.0f;
    }
    tr_time(buf, sizeof buf, (int)((float)s->cu_value * p));
    node_text(s->cu_item, buf);
}

/* processIntro: the items of the state's list (KingBlockResults: 0 intro, 1 rank, 2 walkout, or after the intro the
 * alternative walkout; the others: 0 intro, 1 walkout); at its end the state goes on (KingBlockResults' alternative
 * walkout skips the walkout's state) */
static int kr_process(Hud *h, Screen *s, float dt)
{
    s->intro_t += dt;
    Intro *items = NULL;
    int count = 0, flag = 0, kb = s->kind == SCR_KING_BLOCK;
    if (s->ni_state == 0) { items = s->intro; count = s->nintro; }
    else if (kb && s->alt_walkout) { items = s->alt; count = s->nalt; }
    else if (kb && s->ni_state == 1) { items = s->rank; count = s->nrank; }
    else if (kb && s->ni_state == 2) { items = s->walk; count = s->nwalk; }
    else if (!kb && s->ni_state == 1) { items = s->walk; count = s->nwalk; }
    else
        return 0;
    for (int i = 0; i < count; i++) {
        Intro *it = &items[i];
        if (it->done)
            continue;
        flag = 1;
        if (s->intro_t < it->t0)
            continue;
        float p = it->t1 > it->t0 ? (s->intro_t - it->t0) / (it->t1 - it->t0) : 1;
        if (s->intro_t >= it->t1) {
            p = 1;
            it->done = 1;
        }
        for (int k = 0; k < 3; k++)
            it->obj->p[k] = it->p0[k] + (it->p1[k] - it->p0[k]) * p;
        it->obj->m = it->s0 + p * (it->s1 - it->s0);
        it->obj->fade = it->f0 + p * (it->f1 - it->f0);
    }
    if (s->ni_state == 0 && s->kind != SCR_WORLD) {
        s->scroll = s->intro_t >= 1.5f;                      /* the title scrolls from 1.5 s */
        if (kb && !s->count_up && (s->count_up = s->intro_t >= 1.5f) != 0) {
            sound(h, "Slot_Jackpot", 0);
            s->sound_on = 1;
        } else if (s->kind == SCR_TIMED && !s->count_up)
            s->count_up = s->intro_t >= 1.5f;
    }
    if (!flag) {
        if (kb && s->ni_state == 1 && s->alt_walkout)
            s->ni_state++;
        s->ni_state++;
        s->intro_t = 0;
    }
    return flag;
}

/* doInterface 2048 */
static void kr_frame(Hud *h, Screen *s, float dt)
{
    int k = s->kind;
    if ((k == SCR_KING_BLOCK || k == SCR_KING_CITY) && dt > 0.5f)
        return;
    if (s->do_intro)
        s->do_intro = kr_process(h, s, dt);
    if (s->scroll)
        results_scroll(h, s, dt);
    if (s->count_up == 1) {
        if (k == SCR_KING_BLOCK) kb_count(h, s, dt);
        else if (k == SCR_TIMED) tr_count(s, dt);
    }
    if (s->do_rank)
        s->do_rank = kr_process(h, s, dt);
    if (s->do_walkout)
        s->do_walkout = kr_process(h, s, dt);
    int done = k == SCR_KING_BLOCK ? 3 : 2;                  /* the state after the walkout */
    if (k == SCR_WORLD) {
        if (s->ni_state == 2) {                              /* setResultState(1) and endResults at once */
            set_result(h, HUD_RESULT_DONE);
            s->ended = 1;
            s->ni_state = 3;
        }
    } else if (s->ni_state == done) {
        set_result(h, HUD_RESULT_DONE);
        s->intro_t = 5.0f;
        s->ni_state = done + 1;
    } else if (s->ni_state == done + 1) {                    /* 5 s more on screen */
        s->intro_t -= dt;
        if (s->intro_t <= 0) {
            s->intro_t = 1.0f;
            s->ni_state = done + 2;
        }
    } else if (s->ni_state == done + 2) {                    /* then fades out in half a second */
        s->intro_t -= 2.0f * dt;
        if (s->intro_t <= 0) {
            s->ni_state = done + 3;
            s->ended = 1;
        } else
            s->L->root->fade = s->intro_t;
    }
    if (k != SCR_WORLD && k != SCR_KING_WORLD)               /* UpdateObjectFader: the ConfirmQuit box */
        fader_update(&s->fd, dt);
}

/* the screen takes Continue (hud_results_auto) */
static int kr_ready(const Screen *s)
{
    if (s->cq.state || s->do_intro || s->do_walkout || s->do_rank)
        return 0;
    return s->ni_state == 1 || (s->kind == SCR_KING_BLOCK && s->ni_state == 2);
}

/* doInterface, the pad */
static void kr_input(Hud *h, Screen *s, int ev)
{
    int k = s->kind;
    if (k != SCR_WORLD && k != SCR_KING_WORLD && cq_process(h, s, ev))
        return;
    if (s->do_intro || s->do_walkout || s->ni_state > (k == SCR_KING_BLOCK ? 2 : 1))
        return;
    Node *root = s->L->root, *quit = layout_find(root, "quit");
    if (ev == HUD_EV_START || ev == HUD_EV_ACCEPT) {
        switch (k) {
        case SCR_WORLD:
            if (s->ni_state == 1) {
                sound(h, "Click", 0);
                show(layout_find(root, "fade"), 1);
                s->do_walkout = 1;
            }
            break;
        case SCR_KING_BLOCK:
            if (s->ni_state == 1 && !s->do_rank) {           /* the standings */
                sound(h, "Click", 0);
                s->do_rank = 1;
            } else if (s->ni_state == 2) {
                sound(h, "Click", 0);
                kr_moving_on(h, s, s->current);
                s->do_walkout = 1;
            }
            break;
        case SCR_KING_CITY:
            if (s->ni_state == 1) {
                sound(h, "Click", 0);
                if (s->r.play_type == HUD_PLAY_KING_CITY)
                    set_result(h, HUD_RESULT_QUIT);
                else {
                    kr_moving_on(h, s, s->next_city);
                    s->do_walkout = 1;
                }
            }
            break;
        case SCR_KING_WORLD:
            sound(h, "Click", 0);
            set_result(h, HUD_RESULT_QUIT);
            break;
        case SCR_TIMED:
            if (s->ni_state == 1) {
                sound(h, "Click", 0);
                if (strcmp(s->current, s->played) != 0)
                    kr_moving_on(h, s, s->current);
                s->do_walkout = 1;
            }
            break;
        default: break;
        }
    } else if (ev == HUD_EV_BACK) {
        int ask = k == SCR_KING_BLOCK ? !s->r.city_destroyed && s->r.king_city == -1 :
                  k == SCR_KING_CITY ? s->r.king_world == -1 && quit && quit->visible :
                  k == SCR_TIMED ? quit && quit->visible : 0;
        if (ask) {
            sound(h, "Cancel", 0);
            cq_show(h, s, 1);
        }
    }
    /* buttons 0 and 1 (Change Monster, Autosave) do nothing in these screens */
}

/* ---------------------------------------------------------------- the Pause menu [R Pause.scr] */
/* what the menu asks of gSndDev / gCommon (hud_calls); the same call twice in a row keeps the last, PLAY_MUSIC once */
static void hud_call(Hud *h, int what, float value, int arg)
{
    HudCall *c = NULL;
    if (h->ncalls > 0 && h->calls[h->ncalls - 1].call == what)
        c = &h->calls[h->ncalls - 1];
    else if (what == HUD_CALL_PLAY_MUSIC)
        for (int i = 0; i < h->ncalls; i++)
            if (h->calls[i].call == what)
                return;
    if (!c) {
        if (h->ncalls >= MAX_CALLS)
            return;
        c = &h->calls[h->ncalls++];
    }
    c->call = what;
    c->value = value;
    c->arg = arg;
}

static void pause_unload(Hud *h)
{
    screen_free(h->pause);
    h->pause = NULL;
}

/* setupNonCampaign: no challenge box; the list higher, its top rule hidden, the lower one moved; the pad's picture */
static void pause_noncampaign(Screen *s)
{
    Node *r = s->L->root;
    show(layout_find(r, "challenge_group"), 0);
    show(layout_find(r, "boss_group"), 0);
    Node *go = layout_find(r, "selection_group");
    if (go) {
        go->p[0] = 0; go->p[1] = -20; go->p[2] = 0;
        show(list_item(go, 0), 0);
        Node *bar = list_item(go, 2);
        if (bar) { bar->p[0] = 4; bar->p[1] = 255; bar->p[2] = 0; }
    }
    show(layout_find(r, "controller_group"), 1);
}

/* the icon of the box: resetTexture(getChallengeIcon()), setScale(getChallengeIconScale(1)), showObject */
static void pause_icon(Hud *h, Node *icon)
{
    if (!icon)
        return;
    const HudChallenge *ch = &h->ch;
    icon->has_tex = tex_find(&h->gui, ch->icon, &icon->tex);
    icon->m = ch->pause_scale;                       /* Pause.scr: as it comes (PauseScale, else Scale, else 0) */
    icon->visible = 1;
}

/* setupChallenge: the block's challenge ("Bonus Challenge" / "Upgrade Challenge", its text, its icon, COMPLETED once
 * the items are in) */
static void pause_challenge(Hud *h, Screen *s)
{
    Node *r = s->L->root;
    const HudChallenge *ch = &h->ch;
    /* ChallengeStatus::isChallengeUpgrade: getChallengeUpgrade() != 0 on a block that is not the boss's */
    node_localize(&h->gui, layout_find(r, "challenge"), ch->upgrade ? "CT_Challenge_Upgrade" : "CT_Challenge");
    node_localize(&h->gui, layout_find(r, "goal"), ch->identifier ? ch->identifier : "");
    Node *icon = layout_find(r, "icon");
    pause_icon(h, icon);
    if (icon)
        icon->p[0] += ch->icon_shift;
    int goal = h->ch_required, count = h->ch_collected < goal ? h->ch_collected : goal;
    Node *done = layout_find(r, "completed");
    show(done, goal == count);
    if (done)
        done->F |= 0x400;                                /* setFlags(1024): the GUI's pulse (0x242950) */
    Node *go = layout_find(r, "selection_group");
    if (go) { go->p[0] = 0; go->p[1] = 20; go->p[2] = 0; }
    show(layout_find(r, "controller_group"), 1);
}

/* setupBoss: the boss's card (its title, text, objectives and hint stacked by their heights), no pad picture */
static void pause_boss(Hud *h, Screen *s)
{
    Node *r = s->L->root;
    /* gApp.getLanguage() > 2 (French and on) and gApp.getPlatform() == 2 (taken as the PS2's [G]): the wider card */
    int foreign = h->gui.lang > 2;
    float padding = foreign ? 0.0f : 8.0f;
    Node *head = layout_find(r, foreign ? "boss_group_foreign" : "boss_group");
    if (!head)
        head = layout_find(r, "boss_group");
    if (!head)
        return;
    Node *title = layout_find(head->child, "boss_title"), *msg = layout_find(head->child, "boss_challenge");
    Node *obj = layout_find(head->child, "boss_objectives"), *hint = layout_find(head->child, "boss_hint");
    node_localize(&h->gui, title, "ChallengeBossTitle");
    node_localize(&h->gui, msg, "ChallengeBoss");
    node_localize(&h->gui, obj, "ChallengeBossObjectives");
    node_localize(&h->gui, hint, "ChallengeBossHint");
    if (msg && obj && hint) {
        float x = msg->p[0], y = msg->p[1] + text_h(&h->gui, msg) + padding;
        obj->p[0] = x; obj->p[1] = y;
        y += text_h(&h->gui, obj) + padding;
        hint->p[0] = x; hint->p[1] = y;
    }
    pause_icon(h, layout_find(head->child, "boss_icon"));
    int goal = h->ch_required, count = h->ch_collected < goal ? h->ch_collected : goal;
    show(layout_find(head->child, "boss_completed"), goal == count);
    show(head, 1);
    if (foreign) {
        show(layout_find(r, "boss_group"), 0);
        Node *rect = layout_find(r, "boss_rect");
        if (rect) {
            rect->p[0] = 80; rect->p[1] = 40; rect->p[2] = 0;
            rect->d[0] = 480;
            Node *c = layout_find(rect->child, "center");
            if (c) { c->p[0] = 50; c->p[1] = 0; c->p[2] = 0; }
        }
    } else
        show(layout_find(r, "boss_group_foreign"), 0);
    Node *go = layout_find(r, "selection_group");
    if (go) { go->p[0] = 0; go->p[1] = 160; go->p[2] = 0; }
    show(layout_find(r, "controller_group"), 0);
}

/* Pause::process 8192: the interface starts */
static void pause_start(Hud *h, Screen *s, int pad, const HudPause *p)
{
    Node *r = s->L->root;
    s->pad = pad;
    s->pause_state = 0;
    s->prev_music = h->set_music;                        /* gCommon.getMusicVolume() */
    hud_call(h, HUD_CALL_MUSIC_VOLUME, 0.0f, 1000);      /* gSndDev.setMusicVolume(0.0, 1000) */
    s->prev_rumble = h->set_rumble;
    hud_call(h, HUD_CALL_RUMBLE, 0.0f, 0);               /* gCommon.setEnableRumble(0, 0) */
    int boss = h->ch.last_block;                         /* getCurrentBlockNum() == getNumBlocks() */
    s->choices = layout_find(r, "choices");
    if (s->choices) s->choices->pulse = 1;
    list_select(s->choices, 0);
    show(layout_find(r, "challenge_group"), !boss);
    show(layout_find(r, "boss_group"), boss);
    if (h->cfg.play_type != HUD_PLAY_CAMPAIGN)
        pause_noncampaign(s);
    else if (boss)
        pause_boss(h, s);
    else
        pause_challenge(h, s);
    /* cityTitle: localizeString(city) + " : " + localizeString(getBlockName(1)) (a missing key shows itself [G]) */
    const char *ck = p && p->city ? p->city : "", *bk = p && p->block ? p->block : "";
    const char *cs = loc_find(&h->gui, ck), *bs = loc_find(&h->gui, bk);
    char title[MAX_TEXT];
    snprintf(title, sizeof title, "%s : %s", cs ? cs : ck, bs ? bs : bk);
    node_text(layout_find(r, "city_location"), title);
    cq_init(s);
    s->pause_group = layout_find(r, "pause_group");
    s->options_group = layout_find(r, "options_group");  /* the outer one (there is a second inside it) */
    fader_init(&s->fd, s->pause_group, 1);
    fader_init(&s->fd, s->options_group, 0);
}

/* Continue: unloadInterface(this), the focus back, pauseGame(0), the music back in 1 s, the vibration back */
static void pause_resume(Hud *h)
{
    Screen *s = h->pause;
    hud_call(h, HUD_CALL_MUSIC_VOLUME, s->prev_music, 1000);
    hud_call(h, HUD_CALL_RUMBLE, (float)s->prev_rumble, 0);
    h->pause_result = HUD_PAUSE_RESUME;
    pause_unload(h);
}

/* Pause::onQuit (ConfirmQuit's Yes): pauseGame(0), clearCheatFlags(65536), the music and vibration back,
 * gWorld.restartShell. The script leaves the interface up for the world's end; here it goes on the next frame [G] */
static void pause_quit(Hud *h, Screen *s)
{
    hud_call(h, HUD_CALL_CLEAR_CHEATS, 0.0f, 65536);
    hud_call(h, HUD_CALL_MUSIC_VOLUME, s->prev_music, 1000);
    hud_call(h, HUD_CALL_RUMBLE, (float)s->prev_rumble, 0);
    h->pause_result = HUD_PAUSE_QUIT;
    s->ended = 1;
}

/* setupOptions: the sliders from the settings (100 x volume / 0.75), the vibration's ON / OFF, the row titles at 1 */
static void pause_options_setup(Hud *h, Screen *s)
{
    Node *go = s->options_group;
    s->opt_pulse = 0;
    s->mode = 0;
    s->s_sound = go ? layout_find(go->child, "sound") : NULL;
    s->s_music = go ? layout_find(go->child, "music") : NULL;
    s->l_rumble = go ? layout_find(go->child, "rumble") : NULL;
    s->titles[0] = go ? layout_find(go->child, "sound_title") : NULL;
    s->titles[1] = go ? layout_find(go->child, "music_title") : NULL;
    s->titles[2] = go ? layout_find(go->child, "rumble_title") : NULL;
    s->f_music = s->prev_music;
    s->b_rumble = s->prev_rumble;
    s->f_sound = h->set_sound;                           /* gCommon.getSoundVolume() */
    /* GuiSlider.setValue clamps to its range [R 0x24ABA0] */
    if (s->s_sound) s->s_sound->val = fminf(100.0f, fmaxf(0.0f, 100.0f * s->f_sound / 0.75f));
    if (s->s_music) s->s_music->val = fminf(100.0f, fmaxf(0.0f, 100.0f * s->f_music / 0.75f));
    list_select(s->l_rumble, !s->b_rumble);              /* item 0 ON, 1 OFF */
    for (int i = 0; i < 3; i++)
        if (s->titles[i]) s->titles[i]->tscale = 1.0f;
    if (h->gui.lang == 7 && s->l_rumble) {               /* Dutch: the list moved, its words smaller, 30 apart */
        s->l_rumble->p[0] = 185; s->l_rumble->p[1] = 180; s->l_rumble->p[2] = 0;
        s->l_rumble->lsp = 30;
        Node *on = layout_find(s->l_rumble->child, "ON"), *off = layout_find(s->l_rumble->child, "OFF");
        if (on) on->tscale = 0.6f;
        if (off) off->tscale = 0.6f;
    }
}

static void pause_options_open(Hud *h, Screen *s)       /* activateOptions */
{
    s->pause_state = 1;
    fader_go(&s->fd, s->pause_group, -1);
    fader_go(&s->fd, s->options_group, 1);
    pause_options_setup(h, s);
    hud_call(h, HUD_CALL_MUSIC_VOLUME, s->f_music, 1000);   /* the music comes back to be heard */
}

static void pause_options_close(Hud *h, Screen *s)      /* deactivateOptions */
{
    s->pause_state = 0;
    fader_go(&s->fd, s->options_group, -1);
    fader_go(&s->fd, s->pause_group, 1);
    hud_call(h, HUD_CALL_MUSIC_VOLUME, 0.0f, 1000);
}

/* saveSettings: the sliders' values back to volumes; gCommon.setSoundVolume / setMusicVolume (they set the sound
 * device's volumes at once [R 0x2C2F68, 0x2C3048]) and setEnableRumble(b, 1) (into the profile) */
static void pause_options_save(Hud *h, Screen *s)
{
    float fs = (s->s_sound ? s->s_sound->val : 100.0f) / 100.0f * 0.75f;
    float fm = (s->s_music ? s->s_music->val : 100.0f) / 100.0f * 0.75f;
    int br = !(s->l_rumble ? s->l_rumble->sel : 0);
    s->prev_music = fm;
    s->prev_rumble = br;
    h->set_sound = fs;
    h->set_music = fm;
    h->set_rumble = br;
    hud_call(h, HUD_CALL_SAVE_SETTINGS, 0.0f, 0);
    h->pause_result = HUD_PAUSE_SETTINGS;
}

static void pause_options_row(Screen *s, int dir)       /* updateSelection */
{
    int pm = s->mode;
    s->mode += dir;
    if (s->mode < 0) s->mode = 2;
    if (s->mode > 2) s->mode = 0;
    if (s->titles[pm]) s->titles[pm]->tscale = 1.0f;
    s->opt_pulse = 0;
}

static void pause_options_list(Hud *h, Screen *s, int dir)   /* updateLists: OFF -> ON buzzes the pad */
{
    if (!s->l_rumble)
        return;
    int sel = s->l_rumble->sel;
    list_increment_wrap(s->l_rumble, dir);
    if (sel && sel != s->l_rumble->sel) {
        hud_call(h, HUD_CALL_RUMBLE, 1.0f, 0);
        hud_call(h, HUD_CALL_RUMBLE_PULSE, 0.0f, 250);
    }
}

/* one game frame of the Options page's sliders (processOptions 2048): pad 0 held right / left (or its stick past
 * 0.05) moves the row's slider one step (GuiSlider.increment / decrement: value +- 1 [R 0x250EE0, 0x250F00]); the
 * sound's new volume is set (with a Click) once it stops moving, the music's at every step (the block's music
 * started if none plays) */
static void pause_options_tick(Hud *h, Screen *s)
{
    float sv = s->s_sound ? s->s_sound->val : 0, mv = s->s_music ? s->s_music->val : 0;
    if (s->mode < 2) {
        Node *sl = s->mode == 1 ? s->s_music : s->s_sound;
        if (sl) {
            if (h->held_x >= 0.05f || h->held_r)
                sl->val = fminf(sl->vmax, sl->val + 1.0f);
            else if (h->held_x <= -0.05f || h->held_l)
                sl->val = fmaxf(sl->vmin, sl->val - 1.0f);
        }
    }
    float sn = s->s_sound ? s->s_sound->val : 0, mn = s->s_music ? s->s_music->val : 0;
    if (s->tweak_sound) {
        if (sv == sn) {
            hud_call(h, HUD_CALL_SFX_VOLUME, sn / 100.0f * 0.75f, 0);
            s->tweak_sound = 0;
            sound(h, "Click", 0);
        }
    } else if (sv != sn)
        s->tweak_sound = 1;
    if (mv != mn) {
        if (mn > 0.0f)
            hud_call(h, HUD_CALL_PLAY_MUSIC, 0.0f, 0);   /* if !gSndDev.isMusicPlaying(): playMusic(block's Music) */
        hud_call(h, HUD_CALL_MUSIC_VOLUME, mn / 100.0f * 0.75f, 0);
    }
}

/* the script's frame: the GuiManager's update runs in the world's tick (0x288340, paused or not) with the world's step
 * as its dt, 1/30 (RWorld 0x2B7A70, 0x2B7800) [R] */
#define PAUSE_TICK (1.0f / 30.0f)

static void pause_frame(Hud *h, Screen *s, float dt)
{
    fader_update(&s->fd, dt);                            /* UpdateObjectFader */
    if (s->pause_state != 1)
        return;
    s->opt_pulse += dt;                                  /* pulseSelection: the row's title 1 + 0.1 sin(4t) */
    if (s->titles[s->mode])
        s->titles[s->mode]->tscale = 1.0f + sinf(4.0f * s->opt_pulse) * 0.1f;
    s->tick += dt;
    int n = 0;
    while (s->tick >= PAUSE_TICK && n < 8) {
        s->tick -= PAUSE_TICK;
        pause_options_tick(h, s);
        n++;
    }
    if (n == 8)
        s->tick = 0;
}

static void pause_options_input(Hud *h, Screen *s, int ev, int pad)   /* processOptions, the pad's events */
{
    if (pad != 0)                                        /* event.getController() != 0: not taken */
        return;
    if (ev == HUD_EV_ACCEPT) {
        sound(h, "Click", 0);
        pause_options_save(h, s);
        pause_options_close(h, s);
    } else if (ev == HUD_EV_BACK) {
        sound(h, "Cancel", 0);
        hud_call(h, HUD_CALL_SFX_VOLUME, s->f_sound, 0);         /* the sound as it was */
        hud_call(h, HUD_CALL_RUMBLE, (float)s->b_rumble, 0);
        pause_options_close(h, s);
    } else if (ev == HUD_EV_UP || ev == HUD_EV_DOWN) {
        pause_options_row(s, ev == HUD_EV_UP ? -1 : 1);
        sound(h, "Rollover", 0);
    } else if ((ev == HUD_EV_LEFT || ev == HUD_EV_RIGHT) && s->mode >= 2) {
        pause_options_list(h, s, ev == HUD_EV_LEFT ? -1 : 1);
        sound(h, "Rollover", 0);
    }
}

static void pause_input(Hud *h, Screen *s, int ev, int pad)   /* Pause::process, the pad's events */
{
    if (s->pause_state == 1) {
        pause_options_input(h, s, ev, pad);
        return;
    }
    if (cq_process(h, s, ev)) {
        if (!s->cq.state && s->choices && !s->ended)
            s->choices->pulse = 1;
        return;
    }
    Node *l = s->choices;
    int sel = l ? l->sel : 0;
    if (ev == HUD_EV_UP || ev == HUD_EV_LEFT || ev == HUD_EV_DOWN || ev == HUD_EV_RIGHT) {
        list_increment(l, ev == HUD_EV_UP || ev == HUD_EV_LEFT ? -1 : 1);
        if (l && l->sel != sel)
            sound(h, "Rollover", 0);
    } else if (ev == HUD_EV_START || ev == HUD_EV_ACCEPT) {
        sound(h, "Click", 0);
        if (sel == 0)
            pause_resume(h);
        else if (sel == 1) {
            if (pad != 0)
                sound(h, "Cheat_back", 0);               /* the Options page is player 1's */
            else
                pause_options_open(h, s);
        } else if (sel == 2) {
            if (l) l->pulse = 0;
            cq_show(h, s, 0);
        }
    }
    /* Back: nothing (the script has no case for it) */
}

int hud_pause_open(Hud *h, int pad, const HudPause *p)
{
    /* WorldGuiDriver::process 128: only while it has the focus (no results screen), not in the attract mode */
    if (!h || h->pause || h->scr || h->cfg.play_type == HUD_PLAY_ATTRACT)
        return 0;
    Screen *s = calloc(1, sizeof *s);
    if (!s)
        return 0;
    s->kind = SCR_PAUSE;
    s->screen = -1;
    s->L = layout_load(&h->gui, h->gui.bundles[0], "Pause.ifc");
    if (!s->L) {
        free(s);
        return 0;
    }
    h->pause = s;
    h->pause_result = HUD_PAUSE_NONE;
    pause_start(h, s, pad, p);
    return 1;
}

int hud_pause_showing(const Hud *h) { return h && h->pause && !h->pause->ended; }

int hud_pause_result(Hud *h)
{
    if (!h)
        return HUD_PAUSE_NONE;
    int r = h->pause_result;
    h->pause_result = HUD_PAUSE_NONE;
    return r;
}

void hud_set_settings(Hud *h, float sound_vol, float music_vol, int rumble)
{
    if (!h)
        return;
    h->set_sound = sound_vol;
    h->set_music = music_vol;
    h->set_rumble = rumble != 0;
}

void hud_settings(const Hud *h, float *sound_vol, float *music_vol, int *rumble)
{
    if (!h)
        return;
    if (sound_vol) *sound_vol = h->set_sound;
    if (music_vol) *music_vol = h->set_music;
    if (rumble) *rumble = h->set_rumble;
}

void hud_pad_held(Hud *h, float stick_x, int left, int right)
{
    if (!h)
        return;
    h->held_x = stick_x;
    h->held_l = left != 0;
    h->held_r = right != 0;
}

int hud_calls(Hud *h, HudCall *out, int max)
{
    if (!h || !out)
        return 0;
    int n = h->ncalls < max ? h->ncalls : max;
    memcpy(out, h->calls, sizeof *out * (size_t)n);
    memmove(h->calls, h->calls + n, sizeof *out * (size_t)(h->ncalls - n));
    h->ncalls -= n;
    return n;
}

static void results_unload(Hud *h)
{
    if (h->scr && h->scr->sound_on)
        sound(h, "Slot_Jackpot", 1);
    screen_free(h->scr);
    h->scr = NULL;
    h->paused = 0;
}

/* WorldGuiDriver::startResults [R]: the screen's layout by number (7, 8 and 9 are all TimedRunResults) */
void hud_results(Hud *h, int screen, const HudResults *r)
{
    static const char *const names[8] = { "GameOver", "Results", "LevelResults", "WorldResults", "KingBlockResults",
                                          "KingCityResults", "KingWorldResults", "TimedRunResults" };
    if (!h)
        return;
    results_unload(h);
    Screen *s = calloc(1, sizeof *s);
    if (!s)
        return;
    h->scr = s;
    s->screen = screen;
    if (r) {
        s->r = *r;
        snprintf(s->played, sizeof s->played, "%s", r->block_played ? r->block_played : "");
        snprintf(s->current, sizeof s->current, "%s", r->block_current ? r->block_current : "");
        snprintf(s->results_text, sizeof s->results_text, "%s", r->results_text ? r->results_text : "");
        snprintf(s->city, sizeof s->city, "%s", r->city ? r->city : "");
        snprintf(s->next_city, sizeof s->next_city, "%s", r->next_city ? r->next_city : "");
        for (int i = 0; i < 4; i++)
            snprintf(s->monsters[i], sizeof s->monsters[i], "%s", r->monsters[i] ? r->monsters[i] : "");
    } else {
        s->r.play_type = h->cfg.play_type;
        s->r.num_players = 1;
    }
    s->r.block_played = s->played;
    s->r.block_current = s->current;
    s->r.results_text = s->results_text;
    s->r.city = s->city;
    s->r.next_city = s->next_city;
    for (int i = 0; i < 4; i++)
        s->r.monsters[i] = s->monsters[i];
    h->result = HUD_RESULT_NONE;                         /* nResultState = 0 */
    char file[40];
    snprintf(file, sizeof file, "%s.ifc", names[screen >= 0 && screen < 8 ? screen : 7]);
    static const int kinds[8] = { SCR_GAME_OVER, SCR_RESULTS, SCR_LEVEL, SCR_WORLD, SCR_KING_BLOCK, SCR_KING_CITY,
                                  SCR_KING_WORLD, SCR_TIMED };
    s->kind = kinds[screen >= 0 && screen < 8 ? screen : 7];
    if (s->kind != SCR_STANDIN && (s->L = layout_load(&h->gui, h->gui.bundles[0], file)) == NULL)
        s->kind = SCR_STANDIN;
    if (s->kind == SCR_RESULTS)
        results_start(h, s);
    else if (s->kind == SCR_LEVEL)
        level_start(h, s);
    else if (s->kind == SCR_GAME_OVER)
        gameover_start(h, s);
    else if (s->kind == SCR_WORLD)
        world_start(h, s);
    else if (s->kind == SCR_KING_BLOCK)
        kb_start(h, s);
    else if (s->kind == SCR_KING_CITY)
        kc_start(h, s);
    else if (s->kind == SCR_KING_WORLD)
        kw_start(h, s);
    else if (s->kind == SCR_TIMED)
        tr_start(h, s);
}

int hud_result(Hud *h)
{
    if (!h)
        return HUD_RESULT_NONE;
    int r = h->result;
    h->result = HUD_RESULT_NONE;
    return r;
}

int hud_results_showing(const Hud *h) { return h && h->scr; }
int hud_paused(const Hud *h) { return h && (h->paused || hud_pause_showing(h)); }
void hud_set_save_active(Hud *h, int on) { if (h) h->save_active = on != 0; }
void hud_set_allow_save(Hud *h, int on) { if (h && h->scr) h->scr->allow_save = on != 0; }
void hud_results_auto(Hud *h, float seconds) { if (h) h->auto_press = seconds; }

static void screen_input(Hud *h, Screen *s, int ev, int button)
{
    if (s->ended)
        return;
    if (s->kind == SCR_RESULTS)
        results_input(h, s, ev, button);
    else if (s->kind == SCR_LEVEL)
        level_input(h, s, ev, button);
    else if (s->kind == SCR_GAME_OVER)
        gameover_input(h, s, ev);
    else if (s->kind >= SCR_WORLD)
        kr_input(h, s, ev);
    else if (ev == HUD_EV_ACCEPT || ev == HUD_EV_START) {
        set_result(h, HUD_RESULT_DONE);
        s->ended = 1;
    }
}

/* the event goes to the interface with the focus: the Pause menu over a results screen [R: both take it] */
void hud_input_pad(Hud *h, int ev, int button, int pad)
{
    if (!h)
        return;
    if (h->pause) {
        if (!h->pause->ended)
            pause_input(h, h->pause, ev, pad);
        return;
    }
    if (h->scr)
        screen_input(h, h->scr, ev, button);
}

void hud_input(Hud *h, int ev, int button) { hud_input_pad(h, ev, button, 0); }

static void screen_update(Hud *h, float dt)
{
    Screen *s = h->scr;
    if (!s)
        return;
    if (s->ended) {                                      /* WorldGuiDriver::process 2048: bEndResults -> unload */
        results_unload(h);
        return;
    }
    s->age += dt;
    if (s->kind == SCR_RESULTS)
        results_frame(h, s, dt);
    else if (s->kind == SCR_LEVEL)
        level_frame(h, s, dt);
    else if (s->kind == SCR_GAME_OVER)
        fader_update(&s->fd, dt);
    else if (s->kind >= SCR_WORLD)
        kr_frame(h, s, dt);
    else if (s->age >= 3.0f) {                           /* the stand-in (a layout missing) */
        set_result(h, s->screen == HUD_SCREEN_GAME_OVER ? HUD_RESULT_CONTINUE : HUD_RESULT_DONE);
        s->ended = 1;
        return;
    }
    if (s->ended)
        return;
    int ready = s->kind == SCR_RESULTS || s->kind == SCR_LEVEL ? results_ready(s) :
                s->kind == SCR_GAME_OVER ? !s->cq.state : s->kind >= SCR_WORLD ? kr_ready(s) : 0;
    s->ready = ready ? s->ready + dt : 0;
    if (h->auto_press > 0 && ready && s->ready >= h->auto_press) {
        s->ready = 0;
        screen_input(h, s, HUD_EV_ACCEPT, 0);
    }
}

/* ---------------------------------------------------------------- every frame */
void hud_update(Hud *h, float dt)
{
    if (!h)
        return;
    h->gui.time += dt;
    hud_stats(h, dt);
    cs_update(h, dt);
    ct_update(h, dt);
    int n = 0;
    for (int i = 0; i < h->nrages; i++) {
        if (rage_update(h, h->rages[i], dt))
            h->rages[n++] = h->rages[i];
        else
            rage_free(h->rages[i]);
    }
    h->nrages = n;
    screen_update(h, dt);
    if (h->pause) {                                      /* Pause::process 2048 */
        if (h->pause->ended)
            pause_unload(h);
        else
            pause_frame(h, h->pause, dt);
    }
    if (h->scr && h->scr->L)
        layouts_lay(&h->gui, h->scr->L->root);
    if (h->pause && h->pause->L)
        layouts_lay(&h->gui, h->pause->L->root);
}

int hud_sounds(Hud *h, HudSound *out, int max)
{
    if (!h || !out)
        return 0;
    int n = h->nsounds < max ? h->nsounds : max;
    memcpy(out, h->sounds, sizeof *out * (size_t)n);
    memmove(h->sounds, h->sounds + n, sizeof *out * (size_t)(h->nsounds - n));
    h->nsounds -= n;
    return n;
}

int hud_quads(const Hud *h, HudQuad *out, int max)
{
    if (!h || !out || max <= 0)
        return 0;
    int n = banners_emit(h, out, max);
    n += emit_layout(out + n, max - n, &h->gui, h->hud);
    n += emit_layout(out + n, max - n, &h->gui, h->clock);
    n += emit_layout(out + n, max - n, &h->gui, h->attract);
    n += emit_layout(out + n, max - n, &h->gui, h->cs.L);
    if (h->ct)
        n += emit_layout(out + n, max - n, &h->gui, h->ct->L);
    for (int i = 0; i < h->nrages; i++)
        if (h->rages[i]->init)                           /* drawn once set up (its first frame) */
            n += emit_layout(out + n, max - n, &h->gui, h->rages[i]->L);
    if (h->scr && h->scr->L)
        n += emit_layout(out + n, max - n, &h->gui, h->scr->L);
    if (h->pause && h->pause->L)                         /* loaded last: over everything */
        n += emit_layout(out + n, max - n, &h->gui, h->pause->L);
    return n;
}
