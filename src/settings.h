/* settings.h - the graphics settings (port extra, after WoodyRE's woodyre.cfg; docs/NATIVE.md "Graphics settings"):
 * the window, vsync, ambient occlusion (gtao.c), edge smoothing (postfx.c) and texture filtering (render_gl.c), from
 * rampage.cfg in the current directory, the environment (RAMPAGE_AO ...) and the command line, in that order; a --shot
 * run reads no rampage.cfg (its pictures stay the same whatever the file says). Every effect is off by default, which
 * draws exactly what the viewer drew without them. The viewer's F5 .. F9 change them while it runs, F11 fullscreen;
 * the Options' Graphics page changes them too, and its Accept writes them to the file (settings_save). */
#ifndef RAMPAGE_SETTINGS_H
#define RAMPAGE_SETTINGS_H
#include <stddef.h>
#include "render_gl.h"

typedef struct {
    int width, height, fullscreen, vsync;            /* window=WxH fullscreen= vsync= */
    int ao; float ao_radius;                         /* ao= ao_radius= (world units) */
    int aniso, mips;                                 /* aniso=1|2|4|8|16 mips= */
    int smaa, msaa;                                  /* smaa=0..4 (off, low, medium, high, ultra) msaa=0|2|4|8 */
    /* profile_autoload= (port extra, docs/NATIVE.md "The profile at launch"): 1 (the default) loads the profile file at
     * launch as if picked from the list, and with none opens the name entry once; 0 the game's way */
    int profile_autoload;
} Settings;

void settings_unset(Settings *flags);                /* nothing given yet (for settings_arg) */
/* the flag at argv[*i] (with its value, *i moves past it) into flags: 1 = it was one of ours */
int  settings_arg(Settings *flags, int argc, char **argv, int *i);
/* the defaults, then rampage.cfg (use_cfg), then RAMPAGE_*, then the flags given */
void settings_resolve(Settings *out, const Settings *flags, int use_cfg);
/* to the renderer, and (a shown window) its size, fullscreen and vsync; needs the GL context */
void settings_apply(const Settings *s, Window *w);
/* the keys: F5 ambient occlusion, F6 SMAA, F7 MSAA, F8 anisotropy, F9 mips, F11 fullscreen; 1 = something changed
 * (applied and printed) */
int  settings_keys(Settings *s, Window *w);
/* the graphics of `to` (ao, smaa, msaa, aniso, mips, fullscreen, vsync) into s, applied (the window's mode and vsync
 * only when they change) and printed: the Options' Graphics page (shell.h ShellGraphics) */
void settings_graphics(Settings *s, Window *w, const Settings *to);
/* those seven into the file (rampage.cfg): each key's line changed in place (its comment kept), the missing ones added
 * at the end, every other line as it was; 0 written */
int  settings_save(const Settings *s, const char *path);
/* the effects that are on, for the title (", AO, SMAA high, ..."; "" none) */
void settings_describe(const Settings *s, char *buf, size_t cap);

#endif
