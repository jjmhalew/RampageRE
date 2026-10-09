/* render_gl.c - Win32 window, WGL context and an OpenGL 1.1 renderer for a Scene (city.c). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include <GL/gl.h>
#include "render_gl.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

/* ---------------------------------------------------------------- window */

static Window *g_win;

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Window *w = g_win;
    static int last_x, last_y;
    switch (msg) {
    case WM_CLOSE: if (w) w->quit = 1; return 0;
    case WM_SIZE: if (w) { w->width = LOWORD(lp); w->height = HIWORD(lp); } return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (w && wp < 256) { if (!w->keys[wp]) w->pressed[wp] = 1; w->keys[wp] = 1; }
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP: if (w && wp < 256) w->keys[wp] = 0; return 0;
    case WM_RBUTTONDOWN: if (w) { w->mouse_right = 1; last_x = (short)LOWORD(lp); last_y = (short)HIWORD(lp); SetCapture(hwnd); } return 0;
    case WM_RBUTTONUP: if (w) w->mouse_right = 0; ReleaseCapture(); return 0;
    case WM_MOUSEMOVE:
        if (w && w->mouse_right) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            w->mouse_dx += x - last_x; w->mouse_dy += y - last_y;
            last_x = x; last_y = y;
        }
        return 0;
    case WM_MOUSEWHEEL: if (w) w->wheel += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA; return 0;
    case WM_KILLFOCUS: if (w) memset(w->keys, 0, sizeof w->keys); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* hidden: never shown nor focused (--shot); the frame goes to an offscreen framebuffer (EXT_framebuffer_object)
 * of width x height, or, without one, to the window shown without taking the focus, off the screen */
static int offscreen(int width, int height)
{
    typedef void (APIENTRY *Gen)(GLsizei, GLuint *);
    typedef void (APIENTRY *Bind)(GLenum, GLuint);
    typedef void (APIENTRY *Storage)(GLenum, GLenum, GLsizei, GLsizei);
    typedef void (APIENTRY *Attach)(GLenum, GLenum, GLenum, GLuint);
    typedef GLenum (APIENTRY *Status)(GLenum);
    Gen gen_fb = (Gen)(void *)wglGetProcAddress("glGenFramebuffersEXT");
    Gen gen_rb = (Gen)(void *)wglGetProcAddress("glGenRenderbuffersEXT");
    Bind bind_fb = (Bind)(void *)wglGetProcAddress("glBindFramebufferEXT");
    Bind bind_rb = (Bind)(void *)wglGetProcAddress("glBindRenderbufferEXT");
    Storage storage = (Storage)(void *)wglGetProcAddress("glRenderbufferStorageEXT");
    Attach attach = (Attach)(void *)wglGetProcAddress("glFramebufferRenderbufferEXT");
    Status status = (Status)(void *)wglGetProcAddress("glCheckFramebufferStatusEXT");
    if (!gen_fb || !gen_rb || !bind_fb || !bind_rb || !storage || !attach || !status)
        return -1;
    GLuint fb, rb[2];
    gen_fb(1, &fb);
    gen_rb(2, rb);
    bind_fb(0x8D40, fb);                                   /* GL_FRAMEBUFFER_EXT */
    bind_rb(0x8D41, rb[0]);                                /* GL_RENDERBUFFER_EXT */
    storage(0x8D41, 0x8058, width, height);                /* GL_RGBA8 */
    attach(0x8D40, 0x8CE0, 0x8D41, rb[0]);                 /* GL_COLOR_ATTACHMENT0_EXT */
    bind_rb(0x8D41, rb[1]);
    storage(0x8D41, 0x88F0, width, height);                /* GL_DEPTH24_STENCIL8_EXT */
    attach(0x8D40, 0x8D00, 0x8D41, rb[1]);                 /* GL_DEPTH_ATTACHMENT_EXT */
    attach(0x8D40, 0x8D20, 0x8D41, rb[1]);                 /* GL_STENCIL_ATTACHMENT_EXT */
    return status(0x8D40) == 0x8CD5 ? 0 : -1;              /* GL_FRAMEBUFFER_COMPLETE_EXT */
}

int win_open(Window *w, const char *title, int width, int height, int hidden)
{
    memset(w, 0, sizeof *w);
    g_win = w;
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc = { 0 };
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "RampageRE";
    RegisterClassA(&wc);
    RECT rc = { 0, 0, width, height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(hidden ? WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW : 0, "RampageRE", title,
                                WS_OVERLAPPEDWINDOW | (hidden ? 0 : WS_VISIBLE), hidden ? -32000 : CW_USEDEFAULT,
                                hidden ? -32000 : CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL,
                                inst, NULL);
    if (!hwnd)
        return -1;
    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA,
                                  32, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0, 0, 0 };
    int pf = ChoosePixelFormat(hdc, &pfd);
    if (!pf || !SetPixelFormat(hdc, pf, &pfd))
        return -1;
    HGLRC rc_gl = wglCreateContext(hdc);
    if (!rc_gl || !wglMakeCurrent(hdc, rc_gl))
        return -1;
    typedef BOOL (WINAPI *SwapInterval)(int);
    SwapInterval swap = (SwapInterval)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap)
        swap(1);
    w->hwnd = hwnd; w->hdc = hdc; w->hglrc = rc_gl;
    w->width = width; w->height = height;
    if (hidden && offscreen(width, height) != 0) {
        SetWindowPos(hwnd, HWND_BOTTOM, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    return 0;
}

void win_poll(Window *w)
{
    MSG msg;
    memset(w->pressed, 0, sizeof w->pressed);
    w->mouse_dx = w->mouse_dy = w->wheel = 0;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void win_swap(Window *w) { SwapBuffers((HDC)w->hdc); }
void win_title(Window *w, const char *title) { SetWindowTextA((HWND)w->hwnd, title); }

void win_close(Window *w)
{
    wglMakeCurrent(NULL, NULL);
    if (w->hglrc) wglDeleteContext((HGLRC)w->hglrc);
    if (w->hwnd) DestroyWindow((HWND)w->hwnd);
    g_win = NULL;
}

double win_time(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

/* ---------------------------------------------------------------- renderer */

typedef struct { const Bundle *b; GLuint *tex; uint32_t n; } BundleTex;   /* per name index: GL texture, ~0 = none */

/* an animated texture (.ifl, a text entry: per line a frame's image and how many frames it shows, 3ds Max's 30 per
 * second): its frames' texture names */
#define IFL_FRAMES 64
typedef struct { const Bundle *b; uint32_t name; int n; uint32_t frame[IFL_FRAMES]; float until[IFL_FRAMES]; } Ifl;

struct Renderer {
    const Scene *scene;
    BundleTex cache[16]; int ncache;
    Ifl ifl[32]; int nifl;
    uint32_t drawn;
};

Renderer *rnd_create(void) { return calloc(1, sizeof(Renderer)); }

static void drop_textures(Renderer *r)
{
    for (int k = 0; k < r->ncache; k++) {
        for (uint32_t i = 0; i < r->cache[k].n; i++)
            if (r->cache[k].tex[i] && r->cache[k].tex[i] != ~0u)
                glDeleteTextures(1, &r->cache[k].tex[i]);
        free(r->cache[k].tex);
    }
    r->ncache = 0;
    r->nifl = 0;
}

void rnd_scene(Renderer *r, const Scene *s)
{
    drop_textures(r);
    r->scene = s;
}

void rnd_free(Renderer *r)
{
    if (!r)
        return;
    drop_textures(r);
    free(r);
}

uint32_t rnd_triangles(const Renderer *r) { return r->drawn; }

static GLuint texture(Renderer *r, const Bundle *b, uint32_t name)
{
    BundleTex *bt = NULL;
    for (int k = 0; k < r->ncache && !bt; k++)
        if (r->cache[k].b == b)
            bt = &r->cache[k];
    if (!bt) {
        if (r->ncache == 16)
            return 0;
        bt = &r->cache[r->ncache++];
        bt->b = b;
        bt->n = b->nnames;
        bt->tex = calloc(bt->n ? bt->n : 1, sizeof *bt->tex);
    }
    if (!name || name >= bt->n)
        return 0;
    if (!bt->tex[name]) {
        uint8_t *rgba;
        int w, h;
        bt->tex[name] = ~0u;
        if (tex_decode(b, bdp_entry_of(b, name, ET_TEXTURE), &rgba, &w, &h) == 0) {
            GLuint id;
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            free(rgba);
            bt->tex[name] = id;
        }
    }
    return bt->tex[name] == ~0u ? 0 : bt->tex[name];
}

/* the texture a material shows at time t: itself, or an .ifl's frame then */
static uint32_t shown_texture(Renderer *r, const Bundle *b, uint32_t name, double t)
{
    const BdpEntry *e = bdp_entry(b, name);
    if (!name || !e || e->type != ET_RAW || bdp_entry_of(b, name, ET_TEXTURE))
        return name;
    const char *nm = bdp_name(b, name);
    size_t len = strlen(nm);
    if (len < 4 || _stricmp(nm + len - 4, ".ifl") != 0)
        return name;
    Ifl *f = NULL;
    for (int k = 0; k < r->nifl && !f; k++)
        if (r->ifl[k].b == b && r->ifl[k].name == name)
            f = &r->ifl[k];
    if (!f) {
        if (r->nifl == 32)
            return name;
        f = &r->ifl[r->nifl++];
        memset(f, 0, sizeof *f);
        f->b = b;
        f->name = name;
        const char *p = (const char *)e->data, *end = p + e->size;
        float total = 0;
        while (p < end && f->n < IFL_FRAMES) {
            char img[64], line[128];
            size_t k = 0;
            while (p < end && *p != '\n' && k + 1 < sizeof line) line[k++] = *p++;
            while (p < end && (*p == '\n' || *p == '\r')) p++;
            line[k] = 0;
            int frames = 1;
            if (sscanf(line, "%63s %d", img, &frames) < 1)
                continue;
            char *dot = strrchr(img, '.');
            if (dot) *dot = 0;
            int idx = bdp_find_name(b, img);
            if (idx <= 0)
                continue;
            total += (frames > 0 ? frames : 1) / 30.0f;
            f->frame[f->n] = (uint32_t)idx;
            f->until[f->n++] = total;
        }
    }
    if (!f->n)
        return name;
    double at = fmod(t, f->until[f->n - 1]);
    int k = 0;
    while (k + 1 < f->n && at >= f->until[k]) k++;
    return f->frame[k];
}

static void perspective(float fov_deg, float aspect, float zn, float zf)
{
    float f = 1.0f / tanf(fov_deg * 3.14159265f / 360.0f);
    float m[16] = { f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (zf + zn) / (zn - zf), -1, 0, 0, 2 * zf * zn / (zn - zf), 0 };
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(m);
}

static void look(const Camera *c)
{
    float cp = cosf(c->pitch), f[3] = { cp * cosf(c->yaw), cp * sinf(c->yaw), sinf(c->pitch) };
    float s[3] = { f[1], -f[0], 0 };                       /* f x up(0, 0, 1) */
    float sl = sqrtf(s[0] * s[0] + s[1] * s[1]);
    s[0] /= sl; s[1] /= sl;
    float u[3] = { s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0] };
    if (c->use_rows)
        for (int k = 0; k < 3; k++) { s[k] = c->rows[0][k]; f[k] = c->rows[1][k]; u[k] = c->rows[2][k]; }
    float m[16] = { s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0, 0, 0, 0, 1 };
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(m);
    glTranslatef(-c->pos[0], -c->pos[1], -c->pos[2]);
}

static void draw_mesh(Renderer *r, const SceneMesh *sm, const Skin *skin, double t)
{
    const Scene *s = r->scene;
    const Mesh *m = &sm->mesh;
    uint32_t first = 0;                                    /* the part's first vertex in the skin buffers */
    for (uint32_t i = 0; i < m->nparts; first += m->parts[i].nverts, i++) {
        const MeshPart *p = &m->parts[i];
        if (!p->ntris)
            continue;
        GLuint id = texture(r, sm->b, shown_texture(r, sm->b, material_texture(sm->b, p->material), t));
        if (material_flags(sm->b, p->material) & MAT_TWO_SIDED) glDisable(GL_CULL_FACE);
        else glEnable(GL_CULL_FACE);
        if (id) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, id);
        } else {
            glDisable(GL_TEXTURE_2D);
        }
        int scroll = s->cloud_material && sm->b == s->city && p->material == s->cloud_material;
        if (scroll) {                                        /* the drifting cloud band (AnimatingSkyData) */
            glMatrixMode(GL_TEXTURE);
            glLoadIdentity();
            glTranslatef((float)fmod(s->cloud_rate[0] * t, 1.0), (float)fmod(s->cloud_rate[1] * t, 1.0), 0);
            glMatrixMode(GL_MODELVIEW);
        }
        glVertexPointer(3, GL_FLOAT, 0, skin ? skin->pos + 3 * first : p->pos);
        if (skin) { glEnableClientState(GL_NORMAL_ARRAY); glNormalPointer(GL_FLOAT, 0, skin->nrm + 3 * first); }
        else if (p->nrm) { glEnableClientState(GL_NORMAL_ARRAY); glNormalPointer(GL_FLOAT, 0, p->nrm); }
        else { glDisableClientState(GL_NORMAL_ARRAY); glNormal3f(0, 0, 1); }
        if (p->uv) { glEnableClientState(GL_TEXTURE_COORD_ARRAY); glTexCoordPointer(2, GL_FLOAT, 0, p->uv); }
        else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        if (p->col) { glEnableClientState(GL_COLOR_ARRAY); glColorPointer(4, GL_FLOAT, 0, p->col); }
        else { glDisableClientState(GL_COLOR_ARRAY); glColor4f(1, 1, 1, 1); }
        if (p->blend) {                                      /* over what is behind, by its alpha */
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glAlphaFunc(GL_GREATER, 0.01f);
            glDepthMask(GL_FALSE);
        }
        glDrawElements(GL_TRIANGLES, (GLsizei)(3 * p->ntris), GL_UNSIGNED_INT, p->tris);
        if (p->blend) {
            glDisable(GL_BLEND);
            glAlphaFunc(GL_GREATER, 0.5f);
            glDepthMask(GL_TRUE);
        }
        r->drawn += p->ntris;
        if (scroll) {
            glMatrixMode(GL_TEXTURE);
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);
        }
    }
}

/* the particles (fx.c): after the world, not writing depth, tested against it unless the effect says not to
 * (-TestZBuffer); additive or alpha blended; the colour times the texture (the game's 128 = 1) */
/* the water surfaces (water.c; ROWater 0x339D00): unlit white x the EnvMap texture at the view's reflection, opaque,
 * fogged as the world */
static void draw_water(Renderer *r, const Scene *s, const Camera *cam)
{
    static float *uv;
    static int nuv;
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor4f(1, 1, 1, 1);
    for (int k = 0; k < s->nwater; k++) {
        const Water *w = s->water[k];
        int nv = water_vertex_count(w), ntris = 0;
        if (nv > nuv) {
            uv = realloc(uv, sizeof(float) * 2 * (size_t)nv);
            nuv = nv;
        }
        water_env_uv(w, cam->pos, uv);
        int name = water_texture(w) ? bdp_find_name(s->city, water_texture(w)) : -1;
        GLuint id = name > 0 ? texture(r, s->city, (uint32_t)name) : 0;
        if (id) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        } else
            glDisable(GL_TEXTURE_2D);
        const uint32_t *tris = water_triangles(w, &ntris);
        glVertexPointer(3, GL_FLOAT, 0, water_positions(w));
        glTexCoordPointer(2, GL_FLOAT, 0, uv);
        glDrawElements(GL_TRIANGLES, 3 * ntris, GL_UNSIGNED_INT, tris);
        if (id)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    glEnable(GL_LIGHTING);
}

/* the HUD's quads (hud.c): the GUI's 640 x 480 screen at the window's height, centred (its 4:3 kept), in order, no
 * depth, alpha blended */
static void draw_hud(Renderer *r, const Scene *s, float aspect)
{
    float half = HUD_SCREEN_H / 2 * aspect;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(HUD_SCREEN_W / 2 - half, HUD_SCREEN_W / 2 + half, HUD_SCREEN_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (int i = 0; i < s->nhud; i++) {
        const HudQuad *q = &s->hud[i];
        GLuint id = q->b ? texture(r, q->b, q->tex) : 0;
        if (q->b && !id)
            continue;
        if (id) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, id);
        } else
            glDisable(GL_TEXTURE_2D);
        glColor4fv(q->col);
        glBegin(GL_QUADS);
        glTexCoord2f(q->u0, q->v0); glVertex2f(q->x0, q->y0);
        glTexCoord2f(q->u1, q->v0); glVertex2f(q->x1, q->y0);
        glTexCoord2f(q->u1, q->v1); glVertex2f(q->x1, q->y1);
        glTexCoord2f(q->u0, q->v1); glVertex2f(q->x0, q->y1);
        glEnd();
    }
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
    glEnable(GL_DEPTH_TEST);
    glColor4f(1, 1, 1, 1);
}

static void draw_fx(Renderer *r, const Camera *cam)
{
    const Scene *s = r->scene;
    float M[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, M);
    float right[3] = { M[0], M[4], M[8] }, up[3] = { M[1], M[5], M[9] };
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glAlphaFunc(GL_GREATER, 0.004f);
    float fog_col[4], black[4] = { 0, 0, 0, 0 };
    glGetFloatv(GL_FOG_COLOR, fog_col);
    for (int i = 0; i < s->nfx; i++) {
        const FxQuad *q = &s->fx[i];
        GLuint id = q->b ? texture(r, q->b, q->tex) : 0;
        if (id) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, id);
        } else {
            glDisable(GL_TEXTURE_2D);
        }
        if (q->ztest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glBlendFunc(GL_SRC_ALPHA, q->additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
        /* fog goes on before the blend: an additive quad fogs towards black, so its black adds nothing (fogged
         * towards the fog colour, every texel would add that colour) */
        glFogfv(GL_FOG_COLOR, q->additive ? black : fog_col);
        glColor4fv(q->col);
        float c[4][3];                                     /* top-left, top-right, bottom-right, bottom-left */
        if (q->kind == FXQ_BILLBOARD) {
            float cs = cosf(q->rot), sn = sinf(q->rot), rr[3], uu[3];
            for (int k = 0; k < 3; k++) {
                rr[k] = (cs * right[k] + sn * up[k]) * q->size[0] / 2;
                uu[k] = (-sn * right[k] + cs * up[k]) * q->size[1] / 2;
            }
            for (int k = 0; k < 3; k++) {
                c[0][k] = q->pos[k] - rr[k] + uu[k];
                c[1][k] = q->pos[k] + rr[k] + uu[k];
                c[2][k] = q->pos[k] + rr[k] - uu[k];
                c[3][k] = q->pos[k] - rr[k] - uu[k];
            }
        } else if (q->kind == FXQ_FLAT) {
            for (int k = 0; k < 3; k++) {
                c[0][k] = q->pos[k] - q->axis[0][k] + q->axis[1][k];
                c[1][k] = q->pos[k] + q->axis[0][k] + q->axis[1][k];
                c[2][k] = q->pos[k] + q->axis[0][k] - q->axis[1][k];
                c[3][k] = q->pos[k] - q->axis[0][k] - q->axis[1][k];
            }
        } else {                                           /* a streak, head (v 0) to tail (v 1), facing the camera */
            float view[3] = { q->pos[0] - cam->pos[0], q->pos[1] - cam->pos[1], q->pos[2] - cam->pos[2] }, wd[3];
            const float *a = q->axis[0];
            wd[0] = a[1] * view[2] - a[2] * view[1];
            wd[1] = a[2] * view[0] - a[0] * view[2];
            wd[2] = a[0] * view[1] - a[1] * view[0];
            float l = sqrtf(wd[0] * wd[0] + wd[1] * wd[1] + wd[2] * wd[2]);
            for (int k = 0; k < 3; k++) {
                wd[k] = l > 1e-6f ? wd[k] / l * q->size[1] / 2 : 0;
                c[0][k] = q->pos[k] + a[k] - wd[k];
                c[1][k] = q->pos[k] + a[k] + wd[k];
                c[2][k] = q->pos[k] - a[k] + wd[k];
                c[3][k] = q->pos[k] - a[k] - wd[k];
            }
        }
        if (q->kind == FXQ_FLAT) {                         /* lying on the street: pulled in front of it [G] */
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-1, -4);
        } else
            glDisable(GL_POLYGON_OFFSET_FILL);
        glBegin(GL_QUADS);
        glTexCoord2f(q->uv[0], q->uv[1]); glVertex3fv(c[0]);
        glTexCoord2f(q->uv[2], q->uv[1]); glVertex3fv(c[1]);
        glTexCoord2f(q->uv[2], q->uv[3]); glVertex3fv(c[2]);
        glTexCoord2f(q->uv[0], q->uv[3]); glVertex3fv(c[3]);
        glEnd();
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glFogfv(GL_FOG_COLOR, fog_col);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
}

void rnd_frame(Renderer *r, const Window *w, const Camera *cam, double t, int fog)
{
    const Scene *s = r->scene;
    r->drawn = 0;
    glViewport(0, 0, w->width, w->height);
    float sky[3] = { 0.47f, 0.6f, 0.89f };
    if (s && s->fog_end > 0)
        memcpy(sky, s->fog_colour, sizeof sky);
    if (getenv("RAMPAGE_MAGENTA")) { sky[0] = 1; sky[1] = 0; sky[2] = 1; }
    glClearColor(sky[0], sky[1], sky[2], 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!s)
        return;
    perspective(cam->fov_deg, w->height ? (float)w->width / w->height : 1, 0.5f, 3000);
    look(cam);
    glEnable(GL_DEPTH_TEST);
    glCullFace(GL_BACK);                                   /* one side, but for the two-sided materials (draw_mesh) */
    glFrontFace(GL_CCW);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glEnable(GL_NORMALIZE);
    glEnable(GL_LIGHTING);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_FALSE);      /* a two-sided one's back lit as its front (no flip) */
    float amb[4] = { s->ambient[0], s->ambient[1], s->ambient[2], 1 };
    if (!s->nlights)
        amb[0] = amb[1] = amb[2] = 1;
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    for (int i = 0; i < 4; i++) {
        if (i >= s->nlights) {
            glDisable(GL_LIGHT0 + i);
            continue;
        }
        const float *d = s->light_dir[i];
        float to_light[4] = { -d[0], -d[1], -d[2], 0 }, col[4] = { s->light_colour[i][0], s->light_colour[i][1], s->light_colour[i][2], 1 };
        float zero[4] = { 0, 0, 0, 1 };
        glLightfv(GL_LIGHT0 + i, GL_POSITION, to_light);  /* with the view matrix loaded: world-space directions */
        glLightfv(GL_LIGHT0 + i, GL_DIFFUSE, col);
        glLightfv(GL_LIGHT0 + i, GL_AMBIENT, zero);
        glLightfv(GL_LIGHT0 + i, GL_SPECULAR, zero);
        glEnable(GL_LIGHT0 + i);
    }
    if (fog && s->fog_end > 0) {
        float fc[4] = { s->fog_colour[0], s->fog_colour[1], s->fog_colour[2], 1 };
        glEnable(GL_FOG);
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogfv(GL_FOG_COLOR, fc);
        glFogf(GL_FOG_START, s->fog_start);
        glFogf(GL_FOG_END, s->fog_end);
    } else {
        glDisable(GL_FOG);
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    /* the sky first, around the camera (0x2A53E0): x 0.01 at the camera's (x, y, 0.99 z), the depth test off
     * (GS TEST 0x31001) and no fog (PRIM.FGE cleared for its state 0xB), so it is a background that nothing shows
     * through and that hides nothing */
    if (s->sky >= 0) {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_FOG);
        for (uint32_t i = 0; i < s->nitems; i++) {
            const DrawItem *it = &s->items[i];
            if (it->instance != s->sky)
                continue;
            glPushMatrix();
            glTranslatef(cam->pos[0], cam->pos[1], 0.99f * cam->pos[2]);
            glScalef(0.01f, 0.01f, 0.01f);
            glMultMatrixf(it->world.m);
            draw_mesh(r, &s->meshes[it->mesh], it->skin, t);
            glPopMatrix();
        }
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        if (fog && s->fog_end > 0)
            glEnable(GL_FOG);
    }
    for (uint32_t i = 0; i < s->nitems; i++) {
        const DrawItem *it = &s->items[i];
        if (s->instances[it->instance].hidden || it->instance == s->sky)
            continue;
        glPushMatrix();
        glMultMatrixf(it->world.m);
        draw_mesh(r, &s->meshes[it->mesh], it->skin, t);
        glPopMatrix();
    }
    for (int i = 0; i < s->nfxm; i++) {                    /* the effects' mesh debris, drawn as props (0x21DC20) */
        const FxMesh *m = &s->fxm[i];
        glPushMatrix();
        glMultMatrixf(m->m);
        for (int k = 0; k < s->fx_nshapes[m->shape]; k++)
            draw_mesh(r, &s->fx_shapes[m->shape][k], NULL, t);
        glPopMatrix();
    }
    if (s->nwater)
        draw_water(r, s, cam);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisable(GL_CULL_FACE);
    if (s->nfx)                                            /* the effects' particles, over the world */
        draw_fx(r, cam);
    if (s->nspheres) {                                     /* the overlay: three great circles per sphere */
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_FOG);
        glColor3f(1, 0.2f, 0.1f);
        for (int i = 0; i < s->nspheres; i++) {
            const float *c = s->spheres[i];
            for (int axis = 0; axis < 3; axis++) {
                glBegin(GL_LINE_LOOP);
                for (int k = 0; k < 32; k++) {
                    float a = k * 6.2831853f / 32, u = c[3] * cosf(a), v = c[3] * sinf(a);
                    float p[3] = { c[0], c[1], c[2] };
                    p[(axis + 1) % 3] += u;
                    p[(axis + 2) % 3] += v;
                    glVertex3fv(p);
                }
                glEnd();
            }
        }
        glEnable(GL_DEPTH_TEST);
        glColor3f(1, 1, 1);
    }
    if (s->nhud)
        draw_hud(r, s, w->height ? (float)w->width / w->height : 4.0f / 3);
}
