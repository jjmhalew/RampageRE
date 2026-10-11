/* texpack.h - texture packs (port extra, docs/TEXTURES.md; from WoodyRE): every texture the game uploads (render_gl.c
 * texture()) can be replaced by a PNG of any size in mods/textures/ in the current directory (next to
 * rampage_profile.bin), named after a hash of the original's pixels; --dumptex writes every texture the game uploads to
 * mods/dump/<scope>/ under exactly that name, so a pack starts from the dumped files. */
#ifndef RAMPAGE_TEXPACK_H
#define RAMPAGE_TEXPACK_H
#include <stdint.h>

enum { TP_ASIS, TP_OPAQUE, TP_KEY };   /* what a replacement's alpha becomes: as in the PNG, 255, colour key (on/off at 128, colour black where off) */

uint64_t tp_hash(char kind, const void *data, uint32_t bytes, int w, int h);   /* kind 'T' = a bundle texture (its decoded RGBA) */
int tp_active(void);                 /* a pack or --dumptex is there: only then is a texture hashed (mods/ is looked at once) */
void tp_scope(const char *name);     /* the dump subfolder for what is uploaded next (the bundle's Bundle.scope, NULL = "Common") */
const char *tp_scope_get(void);
void tp_set_dump(int on);            /* --dumptex */
/* with a texture bound: when mods/textures has a PNG for this hash, uploads it (all mip levels, trilinear filtering) and
 * returns 1; otherwise returns 0 and the caller uploads the original. The caller sets the wrap mode either way. */
int tp_replace(uint64_t hash, int alpha_mode);
/* tp_replace in two steps, for a texture cut before it is uploaded (a material's texture region: render_gl.c
 * region_texture): the PNG for this hash as RGBA (its alpha as alpha_mode says; free() it), NULL when there is none;
 * then, with a texture bound, px uploaded with all its mip levels and trilinear filtering (px is overwritten) */
uint8_t *tp_pixels(uint64_t hash, int alpha_mode, int *w, int *h);
void tp_upload(uint8_t *px, int w, int h, int alpha_mode);
void tp_dump(uint64_t hash, const uint8_t *rgba, int w, int h);   /* --dumptex: writes the original as the game shows it (RGBA, top row = GL row 0) */
#endif
