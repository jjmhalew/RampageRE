/* render_gl.h - the window (Win32 + WGL) and an OpenGL 1.1 renderer for a city block (render_gl.c), the WoodyRE way.
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
    void *hwnd, *hdc, *hglrc;
} Window;

int  win_open(Window *w, const char *title, int width, int height);   /* 0 = ok */
void win_poll(Window *w);
void win_swap(Window *w);
void win_close(Window *w);
void win_title(Window *w, const char *title);
double win_time(void);

typedef struct { float pos[3], yaw, pitch, fov_deg; } Camera;   /* Z up; yaw 0 looks along +x, pitch up is positive */

typedef struct Renderer Renderer;
Renderer *rnd_create(void);
void rnd_scene(Renderer *r, const Scene *s);       /* drops the textures of the scene before */
void rnd_frame(Renderer *r, const Window *w, const Camera *cam, double t, int fog);
void rnd_free(Renderer *r);
uint32_t rnd_triangles(const Renderer *r);          /* drawn in the last frame */
int shot_png(const char *path, int width, int height);   /* the back buffer to a PNG (shot.c) */

#endif
