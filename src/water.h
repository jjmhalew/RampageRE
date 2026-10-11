/* water.h - the game's water surfaces: a city block's WaterObject| groups (Hong Kong's harbour, Las Vegas' pools and
 * fountains, Chicago's river; docs/WATER_NOTES.md). A port of the WaterObject code (vtable 0x44B778, its draw object
 * ROWater 0x44B7D8, 0x3368C0 .. 0x339DF0): read by 0x338CC0, set up by 0x338510, stepped each frame by 0x336DC0.
 *
 * The surface is a height field of (NumCols + 1) x (NumRows + 1) vertices TileSize apart, centred on Origin, run by
 * the damped 2D wave equation (two height buffers, the newer replacing the older each frame). Nothing pushes it but
 * its WaveList: each CylWave is an invisible disc that wanders about the pool on sine waves and presses the water
 * under it down to a sine-driven depth (a BoxWave does the same with a rectangle), and the monster wading or landing
 * in it (water_disturb). The ripples spread from there. The edge vertices never move.
 *
 * Drawing (ROWater 0x339D00, VU1 program at 0x37C120): no lighting and no vertex colours; white x the EnvMap
 * texture, whose texture coordinates the VU computes per vertex from the camera: the view ray reflected off the
 * vertex normal, u = its azimuth (a pseudo-angle in 8 segments, 0 .. 1 all round), v = 1 - its z. The texture
 * (water_reflection_hk / _map, 128 x 64) is a skyline panorama, sky at the top. water_env_uv() does that on the CPU.
 *
 * Engine-agnostic: plain arrays out, nothing touches OpenGL. Game space, Z up. Using it:
 *   WaterDef d; water_read(group, &d);          the BlockDef's WaterObject| group (prb.c)
 *   Water *w = water_new(&d, offset, 0);        offset: where the block stands (the block at the origin: 0, 0, 0)
 *   each 1/60 s game frame: water_step(w, 1/60.f);  water_disturb() when something hits the water
 *   to draw: water_positions(), water_env_uv(w, camera_position, uv), water_triangles(), water_texture() */
#ifndef RAMPAGE_WATER_H
#define RAMPAGE_WATER_H
#include <stdint.h>
#include "data.h"

/* a SineWave| { - freq - phase - amp - offset }: amp * sin(freq * t + phase) + offset, t the game clock in seconds
 * (0x337C40, the sine is the VU0 routine at micro address 0xE00, the one sincos 0x106730 uses) */
typedef struct { float freq, phase, amp, offset; } WaterSine;

enum { WATER_CYLWAVE, WATER_BOXWAVE };
typedef struct {
    int kind;
    /* CylWave (4 sines, 0x337C40): [0] depth (pressed to -value), [1] radius, [2] centre x, [3] centre y
     * BoxWave (5 sines, 0x339BB0): [0] depth, [1] half width, [2] half length, [3] centre x, [4] centre y
     * The centre waves' amp is scaled by the half size of the pool at load time (0x338CC0): amp 1 = edge to edge.
     * The centre is relative to the pool's middle. */
    WaterSine s[5];
} WaterWave;

#define WATER_MAX_WAVES 16
typedef struct {
    int num_cols, num_rows;                          /* NumCols, NumRows: tiles; the grid has one more vertex each way */
    float tile;                                      /* TileSize */
    float origin[3];                                 /* Origin: the pool's middle, relative to the block */
    float damp;                                      /* Damp: heights x this every frame (0.98) */
    float wave_speed;                                /* WaveSpeed: of the ripples, units per second (5, 2) */
    int has_min_ground;                              /* MinGroundHeight given (all 12 have it) */
    float min_ground, min_ground_xscale, min_ground_yscale;   /* the floor under the water, see water_floor() */
    char env_map[64];                                /* Texture|EnvMap: the reflection texture */
    int nwaves;
    WaterWave waves[WATER_MAX_WAVES];                /* CylWaves first, then BoxWaves: the order the game runs them */
} WaterDef;

/* from a WaterObject| property group: 0 = ok, -1 if it lacks NumCols / NumRows or they are out of range */
int water_read(Group g, WaterDef *def);

typedef struct Water Water;
/* offset: added to Origin (the game adds the block's place along Y, 0 for the block being played, 0x304450 /
 * 0x3050F0). flat: the game's stand-in for a far-off block (0x3381B0, when that offset is > 0): one still quad,
 * no waves. NULL on a bad def. */
Water *water_new(const WaterDef *def, const float offset[3], int flat);
void water_free(Water *w);

/* one game frame (0x336DC0): the waves press the water, the wave equation moves it on by dt, the normals follow.
 * The game runs it once a frame at the frame's dt; run it at a fixed 1/60 s. dt <= 0 does nothing. */
void water_step(Water *w, float dt);
void water_set_time(Water *w, float t);              /* the clock the waves read (starts at 0; water_step adds dt) */
float water_time(const Water *w);
/* press the water down to `depth` (<= 0, units below the surface) inside a disc of `radius` about pos (world; z is
 * ignored) - 0x3369E0, what the monster does through 0x2E2BD0 at its body centre: wading, every frame of a state
 * that travels (0x2E28A0: depth -0.9 x how deep its ground is under the surface / MONSTER_DATA +0x19C, at most 1;
 * radius half its radius +0x198) and landing (0x2DCA94: depth -(0.5 + 1.5 s), s 0 .. 1 with its fall speed / 50,
 * radius 1.25 x its radius). A height only goes down to depth, never up. */
void water_disturb(Water *w, const float pos[3], float radius, float depth);

/* the floor under the water at (x, y) world (0x337E00, the monster's ground height while it wades): MinGroundHeight
 * inside the inner rectangle (half size x MinGroundHeightX/YScale), rising linearly to 0 at the edges; the water's
 * z where (x, y) is outside it. World z (Origin z + offset z added). */
float water_floor(const Water *w, float x, float y);
int water_contains(const Water *w, float x, float y);   /* (x, y) over the water (its rectangle) */
/* does an axis-aligned box (centre +- half, world) touch the water's box, its rectangle from z 0 up to the surface
 * (+0x104: (-hx, -hy, -Origin z) .. (hx, hy, 0) about its place)? An overlap of 0 counts. The monster's test
 * (0x2D5F00 -> 0x2801A0); the monster's box is its body centre (0x2E4D30) up to its height, its radius across */
int water_box_touches(const Water *w, const float centre[3], const float half[3]);
float water_surface(const Water *w);                 /* its z (+0x34: Origin z + the offset's z) */

/* the mesh, rebuilt by every water_step. Vertices: water_cols() x water_rows(), row by row along +X, rows along +Y */
int water_cols(const Water *w);
int water_rows(const Water *w);
int water_vertex_count(const Water *w);
const float *water_positions(const Water *w);        /* xyz per vertex, world */
const float *water_normals(const Water *w);          /* xyz per vertex, unit; (0, 0, 1) on the edges */
/* the grid's own texture coordinates (col / NumCols, row / NumRows), which the game puts in the vertices but its VU
 * program does not read: not what to draw with */
const float *water_grid_uv(const Water *w);
/* the reflection's texture coordinates for a camera at cam (world): 2 floats per vertex into uv_out (0x37C120) */
void water_env_uv(const Water *w, const float cam[3], float *uv_out);
const uint32_t *water_triangles(const Water *w, int *ntris);   /* 3 vertex indices each, counter-clockwise from above */
const char *water_texture(const Water *w);           /* Texture|EnvMap, a texture of the city's bundle */
/* how it is drawn: white x the texture (alpha 1), unlit, opaque, depth tested and written, fogged as the rest of the
 * world. The game culls the back faces in its VU program; seen from above that is none, so either way. */

#endif
