/* gtao.h - ground-truth ambient occlusion (port extra, from WoodyRE; docs/NATIVE.md "Graphics settings"): a screen-space
 * pass over the opaque 3D image. The PS2 has nothing like it; off by default (rampage.cfg ao=, --ao, RAMPAGE_AO, F5). */
#ifndef RAMPAGE_GTAO_H
#define RAMPAGE_GTAO_H

void gtao_enable(int on, float radius);       /* the setting and its radius in world units; runs only when the GL can */
int  gtao_supported(void);                    /* 1 = GLSL 1.20 + framebuffer objects (needs the GL context), 0 = never runs */
int  gtao_active(void);                       /* on and supported: rnd_frame then holds the blended parts back */
/* darken the current viewport's colour by the occlusion of its depth buffer. zn / zf = the projection's near / far planes,
 * sx / sy = its x / y scale (f / aspect, f), fog_start / fog_end = the linear fog's eye distances (end <= start: no
 * fog), over which the occlusion fades out. Call after the opaque geometry, before anything blended or additive. */
void gtao_frame(float zn, float zf, float sx, float sy, float fog_start, float fog_end);

#endif
