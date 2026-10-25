/* touch.c - the on-screen pad (touch.h, port extra; WoodyRE's with the PS2 pad's buttons and glyphs). Sizes follow the
 * short side of the screen (u), so the controls are the same physical size on a phone in landscape whatever its aspect.
 * RAMPAGE_TOUCHLOG=1 prints the finger events. */
#ifdef __ANDROID__
#include "touch.h"
#include <SDL.h>
#include <GL/gl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { G_CROSS, G_CIRCLE, G_SQUARE, G_TRIANGLE, G_R2, G_START };
typedef struct { int pad, glyph, face; float x, y, r, rgb[3]; } Btn;
#define NBTN 6
#define NFING 10
static struct {
    int shown;
    float W, H;
    struct { SDL_FingerID id; int ctl, seen, up; } f[NFING]; int nf;   /* ctl: -1 nothing, 0 the stick, 1 + i button i; seen:
                                                                      * the game has had a frame of it; up: lifted before
                                                                      * that, goes after it */
    int stick; float sx, sy, kx, ky;                          /* the stick's finger: base and knob, pixels */
    int taps;                                                 /* fingers put down on no button since touch_tapped */
} T = { 1, 0, 0, { { 0, 0, 0, 0 } }, 0, -1, 0, 0, 0, 0, 0 };

static float unit(void) { return T.W < T.H ? T.W : T.H; }
static void layout(Btn *b)
{
    float u = unit(), cx = T.W - 0.27f * u, cy = T.H - 0.28f * u, d = 0.14f * u, r = 0.07f * u;
    const Btn k[NBTN] = {
        { PAD_A, G_CROSS,    1, cx,             cy + d,              r,         { 0.49f, 0.70f, 0.96f } },   /* Cross: jump, accept */
        { PAD_B, G_CIRCLE,   1, cx + d,         cy,                  r,         { 1.00f, 0.40f, 0.42f } },   /* Circle: grab, back */
        { PAD_X, G_SQUARE,   1, cx - d,         cy,                  r,         { 0.98f, 0.55f, 0.85f } },   /* Square: punch */
        { PAD_Y, G_TRIANGLE, 1, cx,             cy - d,              r,         { 0.25f, 0.88f, 0.70f } },   /* Triangle: kick */
        { PAD_RT, G_R2,      0, cx + 1.25f * d, cy - d - 0.17f * u,  0.065f * u, { 0.85f, 0.85f, 0.85f } },  /* R2: roar */
        { PAD_START, G_START, 0, T.W - 0.08f * u, 0.08f * u,         0.05f * u, { 0.85f, 0.85f, 0.85f } } }; /* Start: pause */
    memcpy(b, k, sizeof k);
}
static int hit(float x, float y, int faces_only)
{
    Btn b[NBTN];
    layout(b);
    int best = -1;
    float bd = 0;
    for (int i = 0; i < NBTN; i++) {
        if (faces_only && !b[i].face) continue;
        float dx = x - b[i].x, dy = y - b[i].y, d2 = dx * dx + dy * dy, R = b[i].r * (b[i].face ? 1.3f : 1.5f);
        if (d2 < R * R && (best < 0 || d2 < bd)) { best = i; bd = d2; }
    }
    return best;
}

static void drop(int k)
{
    if (T.stick == k) T.stick = -1;
    T.f[k] = T.f[--T.nf];
    if (T.stick == T.nf) T.stick = k;                                       /* the stick's finger moved into the freed slot */
}
void touch_event(const void *ev, int width, int height)
{
    const SDL_Event *e = (const SDL_Event *)ev;
    T.W = (float)width; T.H = (float)height;
    if (e->type == SDL_KEYDOWN && e->key.keysym.scancode != SDL_SCANCODE_AC_BACK) { T.shown = 0; return; }   /* a keyboard */
    if (e->type != SDL_FINGERDOWN && e->type != SDL_FINGERUP && e->type != SDL_FINGERMOTION) return;
    float x = e->tfinger.x * T.W, y = e->tfinger.y * T.H;
    int k = -1;
    static int log = -1;
    if (log < 0) log = getenv("RAMPAGE_TOUCHLOG") != NULL;
    if (log)
        printf("touch %s id %lld at %.0f %.0f of %.0fx%.0f\n", e->type == SDL_FINGERDOWN ? "down" : e->type == SDL_FINGERUP ? "up" : "move",
               (long long)e->tfinger.fingerId, x, y, T.W, T.H);
    for (int i = 0; i < T.nf; i++) if (T.f[i].id == e->tfinger.fingerId) k = i;
    if (e->type == SDL_FINGERDOWN) {
        T.shown = 1;
        if (k < 0) { if (T.nf == NFING) return; k = T.nf++; T.f[k].id = e->tfinger.fingerId; }
        T.f[k].seen = T.f[k].up = 0;
        int b = hit(x, y, 0);
        if (b < 0) T.taps++;
        if (b >= 0) T.f[k].ctl = 1 + b;
        else if (x < T.W * 0.5f && T.stick < 0) { T.f[k].ctl = 0; T.stick = k; T.sx = T.kx = x; T.sy = T.ky = y; }
        else T.f[k].ctl = -1;
    } else if (k >= 0 && e->type == SDL_FINGERMOTION) {
        if (T.f[k].ctl == 0) {
            float R = 0.14f * unit(), dx = x - T.sx, dy = y - T.sy, d = sqrtf(dx * dx + dy * dy);
            if (d > R) { T.sx = x - dx / d * R; T.sy = y - dy / d * R; }   /* the base follows a finger that goes further */
            T.kx = x; T.ky = y;
        } else if (T.f[k].ctl > 0) {                                        /* slide between the face buttons */
            Btn bs[NBTN];
            layout(bs);
            if (bs[T.f[k].ctl - 1].face) { int b = hit(x, y, 1); if (b >= 0) T.f[k].ctl = 1 + b; }
        }
    } else if (k >= 0 && e->type == SDL_FINGERUP) {
        if (!T.f[k].seen) { T.f[k].up = 1; return; }                        /* a tap within one frame: the game still gets that frame */
        drop(k);
    }
}

int touch_tapped(void) { int n = T.taps; T.taps = 0; return n; }

unsigned touch_pad(PadState st[PAD_SLOTS], unsigned on)
{
    for (int s = 0; s < PAD_SLOTS; s++)                                     /* a real pad in use hides the controls */
        if (on >> s & 1 && (st[s].buttons || fabsf(st[s].lx) > 0.5f || fabsf(st[s].ly) > 0.5f ||
                            fabsf(st[s].rx) > 0.5f || fabsf(st[s].ry) > 0.5f)) {
            T.shown = 0; T.nf = 0; T.stick = -1;
        }
    if (!T.shown) return 0;
    Btn b[NBTN];
    layout(b);
    uint32_t m = 0;
    for (int i = 0; i < T.nf; i++) if (T.f[i].ctl > 0) m |= 1u << b[T.f[i].ctl - 1].pad;
    float lx = 0, ly = 0;
    if (T.stick >= 0) {
        float R = 0.14f * unit();
        lx = (T.kx - T.sx) / R; ly = (T.ky - T.sy) / R;
        float d = sqrtf(lx * lx + ly * ly);
        if (d > 1) { lx /= d; ly /= d; }
    }
    for (int i = T.nf; i-- > 0; ) { if (T.f[i].up) drop(i); else T.f[i].seen = 1; }
    PadState *p = &st[0];
    p->buttons |= m;
    if (m >> PAD_RT & 1) p->rt = 1;
    if (lx * lx + ly * ly > p->lx * p->lx + p->ly * p->ly) { p->lx = lx; p->ly = ly; }
    if (p->kind == PADK_NONE) p->kind = PADK_DS4;
    return 1;
}

/* ---- drawing ------------------------------------------------------------------------------------------------------- */
static void col(const float *rgb, float a) { glColor4f(rgb[0], rgb[1], rgb[2], a); }
static void disc(float x, float y, float r)
{
    glBegin(GL_TRIANGLE_FAN); glVertex2f(x, y);
    for (int i = 0; i <= 32; i++) { float a = (float)i * 6.2831853f / 32; glVertex2f(x + cosf(a) * r, y + sinf(a) * r); }
    glEnd();
}
static void ring(float x, float y, float r, float w)
{
    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i <= 32; i++) { float a = (float)i * 6.2831853f / 32, c = cosf(a), s = sinf(a); glVertex2f(x + c * r, y + s * r); glVertex2f(x + c * (r - w), y + s * (r - w)); }
    glEnd();
}
static void line(float x0, float y0, float x1, float y1, float w)          /* a stroke w wide, its ends squared off */
{
    float dx = x1 - x0, dy = y1 - y0, d = sqrtf(dx * dx + dy * dy);
    if (d <= 0) return;
    float nx = -dy / d * w * 0.5f, ny = dx / d * w * 0.5f, ex = dx / d * w * 0.5f, ey = dy / d * w * 0.5f;
    glBegin(GL_TRIANGLE_STRIP);
    glVertex2f(x0 - ex + nx, y0 - ey + ny); glVertex2f(x0 - ex - nx, y0 - ey - ny);
    glVertex2f(x1 + ex + nx, y1 + ey + ny); glVertex2f(x1 + ex - nx, y1 + ey - ny);
    glEnd();
}
static void strokes(const float (*s)[4], int n, float x, float y, float sx, float sy, float w)   /* lines in a -1 .. 1 box */
{
    for (int i = 0; i < n; i++) line(x + s[i][0] * sx, y + s[i][1] * sy, x + s[i][2] * sx, y + s[i][3] * sy, w);
}
static void glyph(const Btn *b)
{
    static const float CROSS[2][4] = { { -1, -1, 1, 1 }, { -1, 1, 1, -1 } };
    static const float SQUARE[4][4] = { { -1, -1, 1, -1 }, { 1, -1, 1, 1 }, { 1, 1, -1, 1 }, { -1, 1, -1, -1 } };
    static const float TRI[3][4] = { { 0, -1.1f, 1, 0.65f }, { 1, 0.65f, -1, 0.65f }, { -1, 0.65f, 0, -1.1f } };
    static const float R[5][4] = { { -1, 1, -1, -1 }, { -1, -1, 0.7f, -1 }, { 0.7f, -1, 0.7f, 0 }, { 0.7f, 0, -1, 0 }, { -0.1f, 0, 0.8f, 1 } };
    static const float TWO[5][4] = { { -0.8f, -1, 0.8f, -1 }, { 0.8f, -1, 0.8f, 0 }, { 0.8f, 0, -0.8f, 0 }, { -0.8f, 0, -0.8f, 1 }, { -0.8f, 1, 0.8f, 1 } };
    float x = b->x, y = b->y, s = b->r * 0.42f, w = b->r * 0.12f;
    col(b->rgb, 0.9f);
    switch (b->glyph) {
    case G_CROSS:    strokes(CROSS, 2, x, y, s, s, w); break;
    case G_CIRCLE:   ring(x, y, s * 1.05f, w); break;
    case G_SQUARE:   strokes(SQUARE, 4, x, y, s * 0.85f, s * 0.85f, w); break;
    case G_TRIANGLE: strokes(TRI, 3, x, y, s, s, w); break;
    case G_R2:       strokes(R, 5, x - s * 0.6f, y, s * 0.4f, s * 0.6f, w); strokes(TWO, 5, x + s * 0.6f, y, s * 0.4f, s * 0.6f, w); break;
    case G_START:    line(x - s * 0.45f, y - s * 0.8f, x - s * 0.45f, y + s * 0.8f, s * 0.5f);   /* pause bars */
                     line(x + s * 0.45f, y - s * 0.8f, x + s * 0.45f, y + s * 0.8f, s * 0.5f); break;
    }
}
void touch_draw(int width, int height)
{
    if (!T.shown || width <= 0 || height <= 0) return;
    T.W = (float)width; T.H = (float)height;
    static const GLenum caps[] = { GL_DEPTH_TEST, GL_TEXTURE_2D, GL_CULL_FACE, GL_ALPHA_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_BLEND, GL_FOG, GL_LIGHTING };
    GLboolean was[sizeof caps / sizeof caps[0]];
    GLint vp[4], bs = GL_ONE, bd = GL_ZERO;
    for (unsigned i = 0; i < sizeof caps / sizeof caps[0]; i++) { was[i] = glIsEnabled(caps[i]); glDisable(caps[i]); }
    glGetIntegerv(GL_VIEWPORT, vp); glGetIntegerv(GL_BLEND_SRC, &bs); glGetIntegerv(GL_BLEND_DST, &bd);
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, width, height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Btn b[NBTN];
    layout(b);
    uint32_t held = 0;
    for (int i = 0; i < T.nf; i++) if (T.f[i].ctl > 0) held |= 1u << (T.f[i].ctl - 1);
    const float dark[3] = { 0.08f, 0.08f, 0.10f }, grey[3] = { 0.8f, 0.8f, 0.8f };
    for (int i = 0; i < NBTN; i++) {
        col(dark, held >> i & 1 ? 0.75f : 0.4f); disc(b[i].x, b[i].y, b[i].r);
        col(grey, held >> i & 1 ? 0.8f : 0.35f); ring(b[i].x, b[i].y, b[i].r, 0.006f * unit());
        glyph(&b[i]);
    }
    float u = unit(), R = 0.14f * u;
    float sx = T.stick >= 0 ? T.sx : 0.27f * u, sy = T.stick >= 0 ? T.sy : T.H - 0.27f * u, kx = T.stick >= 0 ? T.kx : sx, ky = T.stick >= 0 ? T.ky : sy;
    { float dx = kx - sx, dy = ky - sy, d = sqrtf(dx * dx + dy * dy); if (d > R) { kx = sx + dx / d * R; ky = sy + dy / d * R; } }
    col(grey, 0.18f); disc(sx, sy, R); col(grey, 0.45f); ring(sx, sy, R, 0.012f * u);
    col(grey, T.stick >= 0 ? 0.7f : 0.4f); disc(kx, ky, 0.06f * u);

    glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glBlendFunc((GLenum)bs, (GLenum)bd); glColor4f(1, 1, 1, 1);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    for (unsigned i = 0; i < sizeof caps / sizeof caps[0]; i++) if (was[i]) glEnable(caps[i]); else glDisable(caps[i]);
}
#endif
