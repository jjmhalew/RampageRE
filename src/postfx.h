/* postfx.h - edge smoothing of the 3D picture (port extra, from WoodyRE; docs/NATIVE.md "Graphics settings"):
 * multisampling (MSAA) and SMAA. The PS2 draws without either; both are off by default (rampage.cfg msaa= / smaa=,
 * --msaa / --smaa, RAMPAGE_MSAA / RAMPAGE_SMAA, F7 / F6). */
#ifndef RAMPAGE_POSTFX_H
#define RAMPAGE_POSTFX_H

/* msaa = samples (0, 2, 4, 8), smaa = 0 off, 1..4 low / medium / high / ultra */
void postfx_set(int msaa, int smaa);
int  postfx_msaa_max(void);                   /* the most samples this GL can draw with, 0 = no MSAA (needs the GL context) */
int  postfx_smaa_supported(void);             /* 1 = framebuffer objects + GLSL 1.30 (needs the GL context) */
/* postfx_begin before the 3D picture (its clear): with an effect on, the frame is drawn into an own target of w x h
 * instead of the framebuffer bound now (the window's, or gl_offscreen's for --shot). postfx_end after the 3D picture and
 * its particles, before the HUD (it stays sharp): the target is resolved, smoothed by SMAA when on, and put into that
 * framebuffer, which is bound again. Both do nothing while every effect is off. */
void postfx_begin(int w, int h);
void postfx_end(void);
/* the samples of the target the 3D picture is drawn into right now (0 = single sampled): gtao.c reads its depth */
int  postfx_samples(void);

#endif
