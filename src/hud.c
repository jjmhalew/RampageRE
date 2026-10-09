/* hud.c - the in-play HUD (docs/FORMAT_GUI.md), transcribed from the PS2 game: the GUI layout files (.ifc, bundle
 * type 24: Hud0..3.ifc and Clock.ifc of WorldGui.bdp), the logic of their scripts (build/scripts/Hud.scr, Clock.scr:
 * Hud::doInterface / updateStats, Clock::updateTime), the GUI objects that draw them (GuiQuad 0x2439E0, GuiSlider
 * layout 0x24A310, GuiText 0x234D30), the bitmap fonts (OFontData loader 0x1B7CD0, OFont defaults 0x1B7200, the line
 * measure 0x1B5AA0 / 0x1B6270, the string draw 0x1B6A80 and its glyph quads 0x1B64B0) and the floating score numbers
 * (PointsBanner, vtable 0x44B6A0: start 0x331D50, advance 0x331B00, draw 0x331960, font setup 0x2B8510).
 * [R] = read in that code or in the files, [G] = a guess where it matters.
 *
 * The GUI is a tree of objects, each with a position p (x y z) and a scale m in its parent's frame [R: GuiObject's
 * 3x3 + position at +0x8 / +0x2C, the children's bounds go through the parent's matrix 0x240200]; a quad's sprite
 * spans its size d (or its texture's size) from the object's origin, or is centred on it (Q 0x2 across, 0x4 down,
 * 0x243C00), a slider is always centred (0x24A310), a text sits on its origin by its justification. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "hud.h"

#define HUD_PI 3.14159265f
#define MAX_NODES 96
#define MAX_FLOATS 64
#define MAX_TEXT 64
#define MAX_SORT 1024

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

/* ---------------------------------------------------------------- textures */
typedef struct { const Bundle *b; uint32_t name; float w, h, us, vs; } TexRef;   /* w h: the used size */

static int tex_find(const Bundle *const *bundles, int nb, const char *name, TexRef *out)
{
    for (int i = 0; i < nb; i++) {
        if (!bundles[i])
            continue;
        int n = bdp_find_name(bundles[i], name);
        const BdpEntry *e = n >= 0 ? bdp_entry_of(bundles[i], (uint32_t)n, ET_TEXTURE) : NULL;
        if (!e || e->size < 0x20)
            continue;
        /* texture header: 0x08 u16 width, 0x0A height, 0x14 used width, 0x16 used height [R tools/tex.py]. The floats
         * at 0x18 / 0x1C are used / the GS's power-of-two size (156 / 256 for PointsBanner), the game's UV scale for
         * its GS texture; over the decoded image (tex_decode: width x height) the used part ends at used / size. */
        int w = rd16(e->data + 0x8), h = rd16(e->data + 0xA);
        out->b = bundles[i];
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

/* ---------------------------------------------------------------- fonts (.fdf, type 2) */
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

/* OFontData::load 0x1B7CD0 [R]. File: 0x000 char[0x100] font name, 0x100 char[0x100] bitmap ("POINTSBANNER.BMP"),
 * 0x200 a Windows LOGFONT as text, 0x304 u32 rows of glyphs in the bitmap, 0x308 u32 size of the character list at
 * 0x358, 0x30C u32 size of the alias list after it, 0x310 f32 x4 character spacing, line spacing, shadow dx, dy,
 * 0x320 u32 shadow colour, 0x324 u32 colour, 0x32C f32 slant, 0x330 / 0x334 f32 x / y scale; then 0x24-byte glyphs
 * to the end: f32 u0 u1 v0 v1 (over the bitmap's used size), f32 a (before), w (drawn width), c (after), h (height,
 * < 0: the cell's), char[4] the character. */
static int font_load(Font *f, const Bundle *const *bundles, int nb, const char *name)
{
    memset(f, 0, sizeof *f);
    char want[80];
    snprintf(want, sizeof want, "%s.fdf", name);
    const BdpEntry *e = NULL;
    for (int i = 0; i < nb && !e; i++) {
        if (!bundles[i])
            continue;
        int n = bdp_find_name(bundles[i], want);
        if (n >= 0)
            e = bdp_entry_of(bundles[i], (uint32_t)n, ET_RAW);
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
    if (!tex_find(bundles, nb, base, &f->tex))
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
    /* aliases: pairs (character, the character whose glyph it shares) [R 0x1B824C..0x1B8520]; the fonts here only
     * alias Shift-JIS characters, which are skipped like their glyphs */
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

/* ---------------------------------------------------------------- the draw list */
typedef struct { HudQuad *q; float *z; int n, max; } Emit;

static void emit_quad(Emit *e, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                      const float col[4], const Bundle *b, uint32_t tex, float z)
{
    if (e->n >= e->max || col[3] <= 0.0f)
        return;
    HudQuad *q = &e->q[e->n];
    q->x0 = x0; q->y0 = y0; q->x1 = x1; q->y1 = y1;
    q->u0 = u0; q->v0 = v0; q->u1 = u1; q->v1 = v1;
    memcpy(q->col, col, sizeof q->col);
    q->b = b;
    q->tex = tex;
    if (e->z)
        e->z[e->n] = z;
    e->n++;
}

/* a string in font space mapped to the screen by (ox + sc * x, oy + sc * y). The game measures each line (0x1B5AA0:
 * width = the glyphs' a + w + c + spacing, less one spacing, plus the slant; height = the tallest glyph's h, an empty
 * line the font's average), shrinks the whole text to maxwidth (0x1B6350: scale maxwidth / width when wider), places
 * it by justify (0 left, 1 centre, 2 right: of each line, OFont +0x7 = 3 [R 0x1B6CB4..0x1B6E18]) and origin (0 top,
 * 1 middle, 2 bottom of the text's height [R 0x1B6B50]), then draws the shadow and the text (0x1B6EB8..0x1B6F38).
 * col / shadow: rgba; spacing: the character spacing in use; sdx sdy: the shadow's offset in font pixels. */
static void text_emit(Emit *e, const Font *f, const char *s, float ox, float oy, float sc, int justify, int origin,
                      float maxwidth, int spacing, const float col[4], const float shadow[4], int sdx, int sdy, float z)
{
    if (!f || !f->ok || !s || !*s)
        return;
    enum { MAXL = 8 };
    const char *line[MAXL];
    int len[MAXL], nl = 0;
    float lw[MAXL], lh[MAXL], wmax = 0, hsum = 0;
    for (const char *p = s; nl < MAXL;) {
        const char *q = strchr(p, '\n');
        int n = q ? (int)(q - p) : (int)strlen(p);
        float w = (float)(f->slant - spacing), h = 0;
        int any = 0;
        for (int i = 0; i < n; i++) {
            const Glyph *g = &f->g[(unsigned char)p[i]];
            if (!g->ok)
                continue;                               /* no glyph: no width [R 0x1B5CC0] */
            w += g->a + g->w + g->c + (float)spacing;
            if (g->h > h) h = g->h;
            any = 1;
        }
        if (!any) { w = 0; h = f->avg_h; }
        line[nl] = p; len[nl] = n; lw[nl] = w; lh[nl] = h; nl++;
        if (w > wmax) wmax = w;
        hsum += h;
        if (!q) break;
        p = q + 1;
    }
    float k = 1.0f;                                     /* OFont +0xC x +0x48 / +0x4C are 1 in every font file */
    if (maxwidth > 0 && wmax > maxwidth)
        k = maxwidth / wmax;
    float total_h = hsum * k + (float)(f->linespacing * (nl - 1));
    float y = origin == 1 ? -0.5f * total_h : origin == 2 ? -total_h : 0.0f;
    float cell = f->cell_h * k, slant = (float)f->slant * k;
    for (int l = 0; l < nl; l++) {
        float h = lh[l] * k;
        y += h - cell;                                  /* [R 0x1B6CB0] */
        float yoff = h <= cell ? 0.0f : 0.5f * (f->avg_h * k - h);
        float x = justify == 1 ? -0.5f * lw[l] * k : justify == 2 ? -lw[l] * k : 0.0f;
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
            for (int i = 0; i < len[l]; i++) {
                unsigned char ch = (unsigned char)line[l][i];
                const Glyph *g = &f->g[ch];
                if (!g->ok)
                    continue;
                if (ch == ' ') {
                    cx += k * (g->a + g->w + g->c + (float)spacing);
                    continue;
                }
                cx += g->a * k;
                float x1 = cx + g->w * k;
                /* the slant leans the top edge; the quads here stay upright (every font file has slant 0) */
                emit_quad(e, ox + sc * cx, oy + sc * top, ox + sc * x1, oy + sc * bot, g->u0, g->v0, g->u1, g->v1,
                          c, f->tex.b, f->tex.name, z);
                cx += k * ((float)spacing + g->w + g->c);
            }
        }
        y += cell + (float)f->linespacing;              /* [R 0x1B6F58] */
    }
}

/* ---------------------------------------------------------------- the layout tree (.ifc) */
enum { N_GROUP, N_QUAD, N_TEXT, N_SLIDER };
typedef struct Node {
    int kind;
    char name[32];
    float p[3], m, z;           /* <p> x y z, <m> scale, <z> */
    float d[2]; int has_d;      /* <d> size */
    float col[4];               /* <c> (white when absent) */
    unsigned F, Q;              /* <F> flags (0x1: hidden [G]), <Q> sprite flags (0x2 / 0x4: centred across / down) */
    TexRef tex; int has_tex;    /* <b> */
    /* text: <v> / <L>, <g j o s charspacing>font</g>, <maxwidth> */
    char text[MAX_TEXT];
    const Font *font;
    int justify, origin, spacing, has_spacing;
    float tscale, maxwidth;
    float tcol[4]; int has_tcol;
    /* slider: <val>, <hasMaskBar>, <barColor>, <maskColor>; min 0 max 100 [R ctor 0x24ACD0] */
    float val, vmin, vmax;
    int mask_bar;
    float bar_col[4], mask_col[4];
    int visible;
    float fade;
    struct Node *child, *next;
} Node;

typedef struct {
    Node nodes[MAX_NODES];
    int n;
    Node *root;
} Layout;

/* a tiny reader for the layout XML: <tag a="v" ...>text or elements</tag> (no comments, no entities) */
typedef struct { const char *p, *end; } Xml;

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

static int find_font(void *ctx, const char *name, const Font **out);

#define MAX_FONTS 4
typedef struct { const Bundle *const *bundles; int nb; Font *fonts; char (*names)[32]; int nfonts, maxfonts; } LoadCtx;

static void apply_g(Node *nd, const char *attr, const char *val)
{
    if (strcmp(attr, "j") == 0) nd->justify = val[0] == 'c' ? 1 : val[0] == 'r' ? 2 : 0;   /* [G: the letters] */
    else if (strcmp(attr, "o") == 0) nd->origin = val[0] == 'm' ? 1 : val[0] == 'b' ? 2 : 0;
    else if (strcmp(attr, "s") == 0) nd->tscale = strtof(val, NULL);
    else if (strcmp(attr, "charspacing") == 0) { nd->spacing = atoi(val); nd->has_spacing = 1; }
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

/* one property of node nd: tag with its text (and the <g> attributes already applied) */
static void apply_prop(LoadCtx *lc, Node *nd, const char *tag, const char *text)
{
    if (strcmp(tag, "p") == 0) parse_floats(text, nd->p, 3);
    else if (strcmp(tag, "d") == 0) { parse_floats(text, nd->d, 2); nd->has_d = 1; }
    else if (strcmp(tag, "m") == 0) nd->m = strtof(text, NULL);
    else if (strcmp(tag, "z") == 0) nd->z = strtof(text, NULL);
    else if (strcmp(tag, "F") == 0) nd->F = (unsigned)strtoul(text, NULL, 0);
    else if (strcmp(tag, "Q") == 0) nd->Q = (unsigned)strtoul(text, NULL, 0);
    else if (strcmp(tag, "c") == 0) {
        float c[4];
        col_rgba((uint32_t)strtoul(text, NULL, 0), c);
        if (nd->kind == N_TEXT) { memcpy(nd->tcol, c, sizeof c); nd->has_tcol = 1; }
        else memcpy(nd->col, c, sizeof c);
    } else if (strcmp(tag, "b") == 0)
        nd->has_tex = tex_find(lc->bundles, lc->nb, text, &nd->tex);
    else if (strcmp(tag, "v") == 0 || strcmp(tag, "L") == 0)
        snprintf(nd->text, sizeof nd->text, "%s", text);   /* <L> is a .loc key; the HUD's texts are all <v> */
    else if (strcmp(tag, "g") == 0)
        find_font(lc, text, &nd->font);
    else if (strcmp(tag, "maxwidth") == 0) nd->maxwidth = strtof(text, NULL);
    else if (strcmp(tag, "val") == 0) nd->val = strtof(text, NULL);
    else if (strcmp(tag, "hasMaskBar") == 0) nd->mask_bar = atoi(text) != 0;
    else if (strcmp(tag, "barColor") == 0) col_rgba((uint32_t)strtoul(text, NULL, 0), nd->bar_col);
    else if (strcmp(tag, "maskColor") == 0) col_rgba((uint32_t)strtoul(text, NULL, 0), nd->mask_col);
    /* sc (script), f (entry function), M, X / xf (child interfaces) and the rest: not needed for drawing */
}

static int node_kind(const char *tag)
{
    if (strcmp(tag, "q") == 0) return N_QUAD;
    if (strcmp(tag, "t") == 0) return N_TEXT;
    if (strcmp(tag, "sl") == 0) return N_SLIDER;
    if (strcmp(tag, "o") == 0 || strcmp(tag, "DD") == 0 || strcmp(tag, "i") == 0) return N_GROUP;
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
    for (int i = 0; i < 4; i++) nd->col[i] = nd->bar_col[i] = nd->mask_col[i] = 1;
    nd->val = 100;                                       /* [R 0x24AD94: value 100, min 0, max 100, 100 x 20] */
    nd->vmax = 100;
    nd->visible = 1;
    nd->fade = 1;
    return nd;
}

/* reads one element at x->p ('<' expected); nodes become children of parent, properties go to parent */
static int xml_element(LoadCtx *lc, Layout *L, Xml *x, Node *parent)
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
    char gattr[4][2][24];
    int ngattr = 0;
    /* attributes */
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
        else if (ngattr < 4) {
            snprintf(gattr[ngattr][0], 24, "%s", an);
            snprintf(gattr[ngattr][1], 24, "%s", av);
            ngattr++;
        }
    }
    if (*x->p == '/') {                                  /* <tag/> */
        x->p += 2;
        goto attach;
    }
    x->p++;
    /* content: elements, or text up to the closing tag */
    {
        const char *t0 = x->p;
        xml_skip_ws(x);
        if (x->p < x->end && *x->p == '<' && x->p + 1 < x->end && x->p[1] != '/') {
            while (x->p < x->end) {
                xml_skip_ws(x);
                if (x->p + 1 < x->end && x->p[0] == '<' && x->p[1] == '/')
                    break;
                if (!xml_element(lc, L, x, nd ? nd : parent))
                    return 0;
            }
        } else {
            x->p = t0;
            const char *t1 = t0;
            while (t1 < x->end && *t1 != '<') t1++;
            char text[128];
            int n = (int)(t1 - t0) < 127 ? (int)(t1 - t0) : 127;
            memcpy(text, t0, (size_t)n);
            text[n] = 0;
            x->p = t1;
            if (!nd && parent) {
                if (strcmp(tag, "g") == 0)
                    for (int i = 0; i < ngattr; i++)
                        apply_g(parent, gattr[i][0], gattr[i][1]);
                apply_prop(lc, parent, tag, text);
            }
        }
        /* </tag> */
        while (x->p < x->end && *x->p != '>') x->p++;
        x->p++;
    }
attach:
    if (nd) {
        if (nd->F & 1)
            nd->visible = 0;                             /* [G: F 0x1 = starts hidden; the HUD script sets them] */
        if (!parent)
            L->root = nd;
        else {
            Node **pp = &parent->child;
            while (*pp) pp = &(*pp)->next;
            *pp = nd;
        }
    }
    return 1;
}

static int layout_load(LoadCtx *lc, Layout *L, const Bundle *b, const char *name)
{
    memset(L, 0, sizeof *L);
    int n = bdp_find_name(b, name);
    const BdpEntry *e = n >= 0 ? bdp_entry_of(b, (uint32_t)n, ET_IFC) : NULL;
    if (!e || !e->data)
        return 0;
    Xml x = { (const char *)e->data, (const char *)e->data + e->size };
    while (x.p < x.end && *x.p && xml_element(lc, L, &x, NULL))
        xml_skip_ws(&x);
    return L->root != NULL;
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

/* the layout drawn: the object's frame maps (x, y) to (ox + sc x, oy + sc y); children in it after p and m. The
 * sprite of a quad spans d (or its texture's used size, UVs over the used part) from the origin, or centred (Q); a
 * slider is centred (0x24A310): the bar (barColor) its whole size and the mask (maskColor) from the value's point to
 * its right end (hasMaskBar, flag 0x10), else the bar to the value's point; texts by text_emit. */
static void layout_emit(Emit *e, const Node *nd, float ox, float oy, float sc, float z, float fade)
{
    for (; nd; nd = nd->next) {
        if (!nd->visible)
            continue;
        float nox = ox + sc * nd->p[0], noy = oy + sc * nd->p[1], nsc = sc * nd->m;
        float nz = z + nd->p[2] + nd->z, nf = fade * nd->fade;
        if (nd->kind == N_QUAD) {
            float w = nd->has_d ? nd->d[0] : nd->has_tex ? nd->tex.w : 0;
            float h = nd->has_d ? nd->d[1] : nd->has_tex ? nd->tex.h : 0;
            float x0 = nd->Q & 2 ? -0.5f * w : 0, y0 = nd->Q & 4 ? -0.5f * h : 0;
            float c[4] = { nd->col[0], nd->col[1], nd->col[2], nd->col[3] * nf };
            if (w > 0 && h > 0)
                emit_quad(e, nox + nsc * x0, noy + nsc * y0, nox + nsc * (x0 + w), noy + nsc * (y0 + h), 0, 0,
                          nd->has_tex ? nd->tex.us : 1, nd->has_tex ? nd->tex.vs : 1, c,
                          nd->has_tex ? nd->tex.b : NULL, nd->has_tex ? nd->tex.name : 0, nz);
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
            /* the text's own scale s goes into its matrix (GuiText 0x234D88 scales the matrix by it) */
            text_emit(e, f, nd->text, nox, noy, nsc * nd->tscale, nd->justify, nd->origin, nd->maxwidth,
                      nd->has_spacing ? nd->spacing : f->spacing, c, s, f->shadow_dx, f->shadow_dy, nz);
        }
        layout_emit(e, nd->child, nox, noy, nsc, nz, nf);
    }
}

/* ---------------------------------------------------------------- the HUD */
typedef struct {
    float t, scale;
    float start[3], pos[3];
    uint32_t col;
    char text[16];
} Banner;

struct Hud {
    HudConfig cfg;
    char monster[32];
    Font fonts[MAX_FONTS];
    char font_names[MAX_FONTS][32];
    int nfonts;
    const Bundle *bundles[2];
    Layout hud, clock;
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
};

/* a font by the name the layouts give (<g>ARIALBLACK24</g>): loaded once, from <name>.fdf in the bundles */
static int find_font(void *ctx, const char *name, const Font **out)
{
    LoadCtx *lc = ctx;
    for (int i = 0; i < lc->nfonts; i++)
        if (_stricmp(lc->names[i], name) == 0) {
            *out = &lc->fonts[i];
            return 1;
        }
    if (lc->nfonts >= lc->maxfonts || !font_load(&lc->fonts[lc->nfonts], lc->bundles, lc->nb, name))
        return 0;
    snprintf(lc->names[lc->nfonts], sizeof lc->names[0], "%s", name);
    *out = &lc->fonts[lc->nfonts++];
    return 1;
}

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
    h->bundles[0] = worldgui;
    h->bundles[1] = common;
    LoadCtx lc = { h->bundles, 2, h->fonts, h->font_names, 0, MAX_FONTS };
    char name[32];
    snprintf(name, sizeof name, "Hud%d.ifc", cfg->player >= 0 && cfg->player < 4 ? cfg->player : 0);
    int ok = layout_load(&lc, &h->hud, worldgui, name);
    layout_load(&lc, &h->clock, worldgui, "Clock.ifc");
    /* gWorld.setBannerFont("PointsBanner") [R build/scripts/<City>.scr] */
    find_font(&lc, "PointsBanner", &h->banner_font);
    h->nfonts = lc.nfonts;
    if (!ok) {
        free(h);
        return NULL;
    }
    /* Hud::doInterface, event 8192 (the interface starts) [R Hud.scr] */
    Node *r = h->hud.root;
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
        h->heads->has_tex = tex_find(h->bundles, 2, tname, &h->heads->tex);
        h->heads->visible = !king;
    }
    if (h->blocks) {
        h->blocks->has_tex = tex_find(h->bundles, 2, tname, &h->blocks->tex);
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
    /* Clock::doInterface */
    h->clock_group = layout_find(h->clock.root, "clock");
    h->digits[0] = layout_find(h->clock.root, "ten_minute");
    h->digits[1] = layout_find(h->clock.root, "one_minute");
    h->digits[2] = layout_find(h->clock.root, "ten_second");
    h->digits[3] = layout_find(h->clock.root, "one_second");
    h->last_seconds = -1;
    if (h->clock_group)
        h->clock_group->visible = 0;
    hud_update(h, 0);
    return h;
}

void hud_close(Hud *h) { free(h); }

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

void hud_set_project(Hud *h, HudProject fn, void *ctx)
{
    if (!h)
        return;
    h->project = fn;
    h->project_ctx = ctx;
}

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

/* Hud::updateStats [R Hud.scr] and Clock::updateTime [R Clock.scr] */
void hud_update(Hud *h, float dt)
{
    if (!h)
        return;
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
                if (d[0]) snprintf(d[0], MAX_TEXT, "%s", "");
                if (d[1]) snprintf(d[1], MAX_TEXT, "%s", "");
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
    /* the banners */
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

/* PointsBanner::draw 0x331960 [R]: a camera-facing frame (the camera's, Y turned down for the font) at the number's
 * point, scaled by s = max(0.01, 0.75 scale a - a^2) world units per font pixel (0.75 from 0x44CA48), the string
 * centred across and down (0x2B8480 with t0 = 1), with a shadow 3 3 font pixels off in 0xC4000000 (0x2B855C). */
static void banners_emit(const Hud *h, Emit *e)
{
    const Font *f = h->banner_font;
    if (!f || !f->ok)
        return;
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
        text_emit(e, f, b->text, scr[0], scr[1], s * ppu, 1, 1, 0, f->spacing, col, sh, 3, 3, 0);
    }
}

int hud_quads(const Hud *h, HudQuad *out, int max)
{
    if (!h || !out || max <= 0)
        return 0;
    Emit e = { out, NULL, 0, max };
    banners_emit(h, &e);
    int nb = e.n;
    /* the GUI: back to front by depth, the larger z behind [G: the bars' black backing has z 0.5 and must lie under
     * the bar], in the layout's order otherwise */
    float z[MAX_SORT];
    Emit g = { out + nb, z, 0, max - nb < MAX_SORT ? max - nb : MAX_SORT };
    layout_emit(&g, h->hud.root, 0, 0, 1, 0, 1);
    layout_emit(&g, h->clock.root, 0, 0, 1, 0, 1);
    for (int i = 1; i < g.n; i++) {                      /* stable insertion sort, z descending */
        HudQuad q = g.q[i];
        float k = z[i];
        int j = i - 1;
        while (j >= 0 && z[j] < k) {
            g.q[j + 1] = g.q[j];
            z[j + 1] = z[j];
            j--;
        }
        g.q[j + 1] = q;
        z[j + 1] = k;
    }
    return nb + g.n;
}
