/* fx.h - the game's particle effects: the "Text EDF file" entries (bundle type 20) of fx.bdp, parsed and simulated the
 * way ParticleSystemFx (vtable 0x446AE0, definition ParticleSystemDef 0x446BB0) and CompositeFx (0x446830) do it.
 * docs/FX_NOTES.md has the format and what each key does; fx.c cites the game code ([R] read in the code, [G] guessed).
 *
 * Use: fx_open() once with the bundles that hold the effects and their textures (fx.bdp, read from RAMPAGE.VOL with
 * vol_read + bdp_load; others may follow), fx_world() for the live effects, fx_spawn() where something breaks,
 * fx_update() every frame, fx_quads() for what to draw. Nothing here touches OpenGL. */
#ifndef RAMPAGE_FX_H
#define RAMPAGE_FX_H
#include <stdint.h>
#include "data.h"

typedef struct FxLib FxLib;   /* the parsed effect definitions */
/* reads the 342-word dictionary at 0x3B1C10 of RAMPAGE.ELF (elf_path), then every type-20 entry of the bundles (the
 * first of a name wins); texture names resolve to the first bundle that has a texture of that name. NULL when the ELF
 * cannot be read. The bundles must outlive the FxLib. */
FxLib *fx_open(const char *elf_path, const Bundle *const *bundles, int nbundles);
void fx_close(FxLib *lib);
int fx_count(const FxLib *lib);
const char *fx_name(const FxLib *lib, int effect);
int fx_find(const FxLib *lib, const char *name);   /* case-insensitive, ".edf" may follow; -1 if absent */

typedef struct FxWorld FxWorld;  /* live effects and particles */
FxWorld *fx_world(const FxLib *lib);
void fx_world_free(FxWorld *w);
/* starts an effect at pos (world, Z up). dir is the effect's local +Y axis in the world (NULL: the world's +Y); its +Z
 * stays as close to the world's up as it can. scale multiplies the effect's size and reach (1 = as authored). Returns
 * an id, or -1 (unknown name, a camera shake only, or the world is full). Camera shakes inside a composite go to the
 * fx_on_shake callback. */
int fx_spawn(FxWorld *w, const char *effect_name, const float pos[3], const float dir[3], float scale);
void fx_stop(FxWorld *w, int id);       /* shut down: emitters stop, live particles finish (a looping fire, ...) */
int fx_alive(const FxWorld *w, int id);
void fx_clear(FxWorld *w);
/* advances everything by dt seconds (run as fixed 1/60 s game frames); ground_z is the height of the ground plane for
 * particles that collide with the ground (+CollidesWithGround) */
void fx_update(FxWorld *w, float dt, float ground_z);
typedef void (*FxShakeFn)(void *ctx, const char *effect_name, const float pos[3]);
void fx_on_shake(FxWorld *w, FxShakeFn fn, void *ctx);   /* a composite's camerashake part (camshake, camshake_collapse) */
int fx_particles(const FxWorld *w);                       /* live particles, all kinds */

/* a quad to draw. Texture: the bundle entry of type 9 named tex in b (bdp_entry_of(b, tex, ET_TEXTURE), tex_decode);
 * b == NULL: no texture (draw the colour). uv: u0 v0 at the quad's top-left corner, u1 v1 at its bottom-right, v = 0 at
 * the texture's first row. col multiplies the texture (1 = as is), its alpha too. Blending: additive
 * (dst += src.rgb * src.a) or normal (src.a, 1 - src.a); depth test when ztest, never depth writes. */
enum {
    FXQ_BILLBOARD,   /* faces the camera: corners pos +- right * size[0]/2 +- up * size[1]/2, turned by rot (radians,
                      * counter-clockwise on screen) */
    FXQ_FLAT,        /* lies in the effect's frame: corners pos +- axis[0] +- axis[1] (half extents), rot already in */
    FXQ_STRETCH      /* a streak: from pos - axis[0] to pos + axis[0] (tail to head), size[1] wide, turned about the
                      * streak to face the camera; u runs along the width, v from the head (0) to the tail (1) */
};
typedef struct {
    int kind;
    float pos[3], size[2], rot, axis[2][3];
    float col[4], uv[4];
    const Bundle *b;
    uint32_t tex;
    int additive, ztest;
} FxQuad;
/* the quads of every live particle, emitter by emitter, an emitter's particles by texture and the newest first (the
 * game's list order, 0x211970; it does not sort by depth); returns the count (at most max) */
int fx_quads(const FxWorld *w, FxQuad *out, int max);
#endif
