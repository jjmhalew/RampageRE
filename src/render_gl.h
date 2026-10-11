/* render_gl.h - the window (Win32 + WGL in render_gl.c; SDL2 elsewhere, plat_sdl.c) and an OpenGL 1.1 renderer for a city block (render_gl.c), the WoodyRE way.
 * The game lights its city at run time with an ambient colour and directional lights from a light file
 * (docs/FORMAT_BDP.md "Lighting"): texture x (ambient + sum of colour x N.L), which is what GL's fixed-function
 * lighting computes, so the lights go in as GL lights. */
#ifndef RAMPAGE_RENDER_GL_H
#define RAMPAGE_RENDER_GL_H
#include "city.h"

typedef struct {
    int width, height;
    int keys[256];                     /* current key state (VK codes) */
    int pressed[256];                  /* went down since the last win_poll */
    int mouse_dx, mouse_dy, mouse_right, wheel;
    int quit;
    int focused;                       /* has the keyboard focus */
    int dev_changes;                   /* counts devices plugged in or out (pads) */
    int hidden;                        /* never shown (--shot): the frame is gl_offscreen's */
    void *hwnd, *hdc, *hglrc;          /* Win32: HWND, HDC, HGLRC; SDL: SDL_Window *, -, SDL_GLContext */
} Window;

int  win_open(Window *w, const char *title, int width, int height, int hidden);   /* 0 = ok; hidden: offscreen */
void win_poll(Window *w);
void win_swap(Window *w);
void win_close(Window *w);
void win_title(Window *w, const char *title);
double win_time(void);
/* port extras (WoodyRE's, docs/NATIVE.md "Graphics settings"): a window whose inside is width x height, centred on
 * the screen and shrunk to fit, or borderless fullscreen on the desktop's resolution (no mode switch); nothing for a
 * hidden window. win_vsync: the swap interval (1 = vsync, 0 = off), 0 = ok */
void win_mode(Window *w, int width, int height, int full);
int  win_vsync(Window *w, int interval);
/* an offscreen framebuffer of width x height (GL 3.0's or EXT_framebuffer_object's), bound from now on: a hidden
 * window draws there; 0 = ok */
int gl_offscreen(int width, int height);
/* 1 = GL 3.0's framebuffer objects or ARB_framebuffer_object's (the same entry points, with the blit and multisampled
 * renderbuffers): what postfx.c and gtao.c need, and the names gl_offscreen then uses too */
int gl_core_fbo(void);

typedef struct {
    float pos[3], yaw, pitch, fov_deg;               /* Z up; yaw 0 looks along +x, pitch up is positive */
    int use_rows; float rows[3][3];                  /* or the game camera's axes: right, forward, up */
} Camera;

typedef struct Renderer Renderer;
Renderer *rnd_create(void);
void rnd_scene(Renderer *r, const Scene *s);       /* drops the textures of the scene before */
void rnd_frame(Renderer *r, const Window *w, const Camera *cam, double t, int fog);
void rnd_free(Renderer *r);
/* port extra (docs/NATIVE.md "Graphics settings"): the textures' filtering: aniso = GL_TEXTURE_MAX_ANISOTROPY (1 =
 * off, as the PS2), mips = 1: a 2x2 box mip chain with trilinear filtering (0: the one level, GL_LINEAR, as before);
 * a change reloads the textures at the next frame. rnd_aniso_max: the most this GL can (1 = none; needs the context) */
void rnd_set_textures(int aniso, int mips);
int  rnd_aniso_max(void);
/* the movie's picture (RGBA, w x h, the first row on top) for the HudQuads with tex HUD_TEX_VIDEO; it is read at
 * the next rnd_frame, so it stays valid until then; NULL: none (the quads stay black) */
void rnd_video(Renderer *r, const uint8_t *rgba, int w, int h);
uint32_t rnd_triangles(const Renderer *r);          /* drawn in the last frame */
int shot_png(const char *path, int width, int height);   /* the back buffer to a PNG (shot.c) */

#endif
