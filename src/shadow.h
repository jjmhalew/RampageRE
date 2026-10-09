/* shadow.h - the game's shadows (docs/SHADOW_NOTES.md), traced from the PS2 code; shadow.c cites it ([R] read in the
 * code, [G] guessed). There are no projected silhouettes, no stencil and no shadow maps: there are four kinds, all
 * flat dark shapes cast along one fixed light direction, (-1, 1, -1) / sqrt 3 [R 0x2B9928], not the block's lights.
 *
 *   1. object shadows (0x325C90): every object or actor whose template (or a parent) has a ShadowData group - the
 *      vehicles and street props (shadow_square) and every DefActorProps actor, i.e. the people and soldiers
 *      (shadow_blob) - a textured rectangle on the street at z = 0.01, slid along the light by the object's height;
 *   2. the monster's ground blob (ROMonster 0x2FEA20): shadow_blob under its body on the street or on the roof it
 *      stands on, fading with its height above that ground;
 *   3. the monster's wall blob (0x2FDEB0): shadow_blob on the front face (and the right side face) of the building it
 *      climbs, drawn with no depth test;
 *   4. the buildings' shadows on the block's ground (0x31F8B0): a 23 x 6 cell vertex-coloured grid over the lot,
 *      shadowed where a ray towards the light hits a building that still stands tall enough (damage lights it).
 * And a lighting tweak, not geometry: facade darkening (0x31D560), the building's vertex light colours minus MShadowRGB
 * where a neighbour's shadow falls on its front or right face (building.h's building light, city.c drives it).
 *
 * Everything here is in game space (Z up, the street at z = 0, the play block's ground grid at fixed world x/y).
 * Output is triangles (3 vertices each) in batches that share a texture and GL state; nothing touches OpenGL.
 * The textures are "shadow_blob" and "shadow_square" in fx.bdp (32 x 32, PSMT8 with the engine's _Alpha8 palette:
 * white, the shape in the alpha). The colours are GL scale (1.0 = the GS's 128). */
#ifndef RAMPAGE_SHADOW_H
#define RAMPAGE_SHADOW_H

#define SHADOW_BUNDLE "fx.bdp"          /* where shadow_blob / shadow_square are */

typedef struct { float pos[3], uv[2], rgba[4]; } ShadowVert;

/* when to draw a batch (see shadow_list_* and docs/SHADOW_NOTES.md "Drawing it") */
enum {
    SHADOW_PASS_GROUND,   /* right after the street, before buildings and objects: the building shadow grid */
    SHADOW_PASS_DECAL,    /* after the opaque world: object shadows, the monster's ground blob */
    SHADOW_PASS_WALL      /* after the buildings, before the monster's own mesh: the wall blob (no depth test) */
};

typedef struct {
    const char *texture;  /* "shadow_blob", "shadow_square" (SHADOW_BUNDLE), or NULL: vertex colour only */
    int pass;             /* SHADOW_PASS_* */
    int depth_test;       /* 0: the game draws it with ZTST ALWAYS (no test, no write) */
    int alpha_test;       /* 1: the game's alpha test GREATER, ref 1 (drop fully transparent pixels) */
    int first, count;     /* vertices first .. first + count - 1, count a multiple of 3 */
} ShadowBatch;

typedef struct {
    ShadowVert *v; int nv, vcap;
    ShadowBatch *b; int nb, bcap;
} ShadowList;
void shadow_list_init(ShadowList *l);
void shadow_list_clear(ShadowList *l);   /* every frame, before filling it */
void shadow_list_free(ShadowList *l);

/* the light the shadows are cast along (where the light travels), unit: (-1, 1, -1) / sqrt 3 */
void shadow_dir(float out[3]);

/* WorldData.LightingData's shadow keys (read by 0x2B92F0); all seven cities, day and night, have the defaults */
typedef struct {
    float tint[4];            /* TShadowTintR, G, B, A: 0, 0, 0, 0.85 */
    float ground_intensity;   /* GroundShadowIntensity 0.8 (the building grid) */
    float monster_intensity;  /* MonsterShadowIntensity 0.8 (both monster blobs) */
    float mshadow[3];         /* MShadowR, G, B: 0.4, 0.25, 0 (facade darkening) */
    const char *monster_blob; /* Texture|MonsterBlob: "shadow_blob" */
} ShadowLighting;
void shadow_lighting_default(ShadowLighting *l);

/* ---- 1. object shadows ------------------------------------------------------------------------------------------- */
/* a template's ShadowData group (read by 0x3265D0; Objects1_props.bdp has 62, inherited through ParentPropertySet) */
typedef struct {
    const char *texture;      /* Texture|TextureName */
    float intensity;          /* Intensity */
    float max_height;         /* MaxHeight: no shadow when the object's z is at or above it */
    float full_height;        /* FullHeight */
    float ex_proj_height;     /* ExProjHeight (0 if absent) */
    float rect[4];            /* ShadowRect x0 y0 x1 y1 in the object's frame ((-1, -1, 1, 1) if absent) */
} ShadowData;
void shadow_data_default(ShadowData *d);
/* m: the object's world matrix, rows m[0..2] its x axis, m[3..5] y, m[6..8] z, m[9..11] position (scale included,
 * as the game's node matrix has it). Appends one quad (2 triangles); returns 0 when it casts nothing. */
int shadow_object(ShadowList *out, const ShadowLighting *l, const ShadowData *d, const float m[12]);

/* ---- 2 and 3. the monster ---------------------------------------------------------------------------------------- */
typedef struct {              /* the ROMonster fields (+0x14 .. +0x58), kept between frames */
    int ground_on;            /* +0x18 */
    float ground_fade;        /* +0x1C */
    float ground[3];          /* +0x20: the point the blob is centred on, z = the ground under it */
    int wall_on;              /* +0x2C */
    float wall_fade;          /* +0x30 */
    float wall_pos[3];        /* +0x34 */
    float wall_box[6];        /* +0x40: the climbed building's box, min xyz, max xyz (adjusted) */
} ShadowMonster;
void shadow_monster_init(ShadowMonster *s);
/* once a frame (0x2FDA50, 0x2FDD00). dt in seconds.
 *   centre: the monster's body centre (0x2E4D30: its position + its matrix x axis x MONSTER_DATA +0x1A0 + y axis x
 *           +0x1A4, both x scale; the game adds a per-move offset there [G]);
 *   body:   the point the wall blob is centred on (0x2E61E0: a point of the body [G: use the pelvis / root]);
 *   radius: MONSTER_DATA +0x198 x scale (George 3);
 *   mounted / mount_box: climbing a building, and that building's world box (min xyz, max xyz);
 *   roof_box: the building the monster is over (0x2D6150), or NULL;
 *   blocked: an object stands in the box over the ground point (0x2FD870: a query with mask 0x800000) - kills it. */
void shadow_monster_update(ShadowMonster *s, float dt, const float centre[3], const float body[3], float radius,
                           int mounted, const float mount_box[6], const float *roof_box, int blocked);
/* rot: the monster's 3x3 (rows x, y, z axes; the game copies its matrix). Appends the ground and wall blobs. */
void shadow_monster_draw(ShadowList *out, const ShadowLighting *l, const ShadowMonster *s, const float rot[9],
                         float radius);

/* ---- 4. the buildings' shadows on the ground --------------------------------------------------------------------- */
#define SHADOW_GRID_COLS 22   /* sample columns: x = -59.5 + 5 c */
#define SHADOW_GRID_ROWS 5    /* sample rows:    y =   2.5 + 5 r */
#define SHADOW_MAX_BUILDINGS 64
/* a ray against building b's model: return 1 and the distance in *t if it hits within maxdist. NULL: the box. */
typedef int (*ShadowRayFn)(void *ctx, int b, const float org[3], const float dir[3], float maxdist, float *t);
typedef struct {              /* 0x18 bytes in the game [R 0x31FE3C, 0x31FEC8] */
    unsigned char n;          /* +0: buildings met (at most 4) */
    unsigned char lit;        /* +1: 0 = in shadow */
    unsigned char who[4];     /* +2: their indices */
    float hit[4];             /* +8: the height each was met at */
} ShadowCell;
typedef struct {
    int enabled;                                   /* +0x0: the block's EnableGroundShadow (1 if absent) */
    float dirty[4];                                /* +0x4: x0 y0 x1 y1 to redo */
    ShadowCell cell[SHADOW_GRID_ROWS][SHADOW_GRID_COLS];   /* +0x14, rows 0x210 apart */
    int nbuildings;                                /* +0xA64 */
    float top[SHADOW_MAX_BUILDINGS];               /* +0xA68: each building's top z when last seen */
} ShadowGround;
/* at block start (0x31FC40): boxes are the buildings' world boxes (min xyz, max xyz; box[5] = the top, +0x1F0) */
void shadow_ground_init(ShadowGround *g, int enabled, int nbuildings, const float (*boxes)[6], ShadowRayFn ray,
                        void *ctx);
/* a building's top changed (0x31F790, from its damage): box is its box now (box[5] the new top) */
void shadow_ground_top(ShadowGround *g, int b, const float box[6]);
/* every frame (0x31F8B0): redoes the dirty cells, appends the grid (SHADOW_PASS_GROUND) */
void shadow_ground_draw(ShadowList *out, ShadowGround *g, const ShadowLighting *l);

/* ---- facade darkening (a lighting tweak) ------------------------------------------------------------------------- */
typedef struct {
    int front;                /* S+0x00: a neighbour's shadow reaches the front face */
    float plane[3][4];        /* S+0x04: its three edges, n.p >= d inside (left, right, the sloped top) */
    float front_y, right_x;   /* S+0xBC: front face y + 0.1; S+0xC0: right face x - 1 */
    int nright;               /* S+0x88 */
    float right[2][6];        /* shadow boxes on the right face (min xyz, max xyz) */
} ShadowFacade;
/* own, nb_a, nb_b: world boxes (0x31D6B0). Only a back-row building (Row 1) has neighbours (0x31C8C0, at its
 * creation): nb_a (+0xC8) the first building of the front row's list whose min x is past its own min x (casts on its
 * front, and on its right face when it lies to the right), nb_b (+0xD0) the first of its own row's list whose min x
 * is at or past its own max x (casts on its right face); the lists in the order the buildings were added. Built again
 * whenever one of their tops changed (0x31C860 -> 0x31C7B0); when that leaves no shadow, the neighbours are dropped. */
void shadow_facade_build(ShadowFacade *f, const float own[6], const float *nb_a, const float *nb_b);
/* darkens a lit vertex colour rgb (0..1) at world point p (0x31D560: the front when p.y <= front_y, else the right
 * face when p.x > right_x); returns 1 if it did */
int shadow_facade_darken(const ShadowFacade *f, const ShadowLighting *l, const float p[3], float rgb[3]);
/* the two halves alone, as the facade's grid light (0x2CE300) calls them for its front and right face vertices:
 * the front's three planes (0x31DA60), the right face's boxes (0x31D7F0) */
int shadow_facade_front(const ShadowFacade *f, const ShadowLighting *l, const float p[3], float rgb[3]);
int shadow_facade_right(const ShadowFacade *f, const ShadowLighting *l, const float p[3], float rgb[3]);

#endif
