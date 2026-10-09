/* city.h - a city block put together from the property books (city.c, the port of tools/city.py; docs/FORMAT_BDP.md
 * "City layout", "Spawns", "Lighting"), with what moves in it: the props' own loops, the people's clips, traffic and a
 * monster. Game space: Z up, the street towards -Y. Matrices are column-major 4 x 4 (OpenGL order). */
#ifndef RAMPAGE_CITY_H
#define RAMPAGE_CITY_H
#include "data.h"

typedef struct { float m[16]; } Mat4;

typedef struct { Mesh mesh; const Bundle *b; } SceneMesh;   /* a mesh entry of bundle b, or a generated facade */

typedef struct {                                     /* a skinned mesh's vertices, posed every frame */
    uint8_t (*bones)[4]; float (*weights)[4];         /* per vertex of all parts, in part order */
    float *pos, *nrm;                                 /* the posed vertices */
    uint32_t nverts;
} Skin;

typedef struct {
    Tree tree;                                        /* n = 0: a model without a node tree */
    Mat4 base;                                        /* where it stands */
    Clip clip; int has_clip; float offset;            /* a looping clip, started offset seconds in */
    int vehicle;                                      /* -1, or the vehicle it rides on */
    Mat4 *node;                                       /* per node, relative to base */
    int still_root;                                   /* keep the root node's x / y at rest (the walk moves base) */
} Instance;

typedef struct {
    uint32_t mesh;                                    /* index into Scene.meshes */
    int instance, node;                               /* node -1: the instance's root */
    Mat4 world;                                       /* updated by scene_update for moving instances */
    Skin *skin;                                       /* skinned meshes: draw skin->pos / nrm instead of the mesh's */
} DrawItem;

typedef struct { float y, direction, phase, turn_deg; } Vehicle;   /* a car on a lane, phase 0..1 along it */

typedef struct {
    const Bundle *city;
    Bundle *own[4]; int nown;                         /* bundles loaded for this scene (a monster and its clips) */
    SceneMesh *meshes; uint32_t nmeshes, meshes_cap;
    int32_t *mesh_of_entry;                           /* per city bundle entry: its SceneMesh, -1 = not loaded */
    Instance *instances; uint32_t ninstances, instances_cap;
    DrawItem *items; uint32_t nitems, items_cap;
    Vehicle *vehicles; uint32_t nvehicles;
    float ambient[3]; float light_colour[4][3], light_dir[4][3]; int nlights;
    float fog_colour[3], fog_start, fog_end;
    uint32_t cloud_material; float cloud_rate[2];    /* the material whose texture drifts (AnimatingSkyData) */
    int people, missing;
    int monster;                                      /* instance of the monster, -1 = none */
    const char *monster_clip;                         /* the name of the clip it plays */
    const Bundle *monster_anims;
    const Bundle *monster_bundle;                     /* <Monster>.cmp: its meshes, skeleton and MONSTER_DATA */
} Scene;

typedef struct City City;
City *city_open(const Vol *vol, const char *name);   /* NULL if the city's bundles are not in the archive */
void city_close(City *c);
uint32_t city_blocks(City *c, const char **names, uint32_t max);   /* the block list, without repeats */
int city_build(City *c, const char *block, int night, Scene *s);    /* 0 = ok */
/* a monster in the street at (x, y), turned deg about Z, playing its clip number `clip` (-1: its idle) */
int city_monster(City *c, Scene *s, const char *monster, int clip, float x, float y, float deg);
void scene_update(Scene *s, double t);               /* pose everything that moves at time t (seconds) */
/* the monster plays the clip whose name ends in suffix ("_WALK2", "_IDLE"); 0 = ok. *speed: how fast its root
 * travels in that clip (units per second), for moving it by hand */
int scene_monster_clip(Scene *s, const char *suffix, float *speed);
void scene_monster_place(Scene *s, float x, float y, float deg, int still_root);
int scene_monster_play(Scene *s, const char *clip);  /* the clip of that name (any case, as MONSTER_DATA names it) */
/* the root node's translation and rotation in the monster's clip at `seconds`; 0 = ok */
int scene_monster_root(const Scene *s, double seconds, float t[3], float q[4]);
int city_camera(City *c, Group *two_tilts);           /* camera.bdp's TwoTilts group; 0 = ok */
void scene_free(Scene *s);
uint32_t monster_names(const Vol *vol, const char **names, uint32_t max);   /* the 30 monsters (NAME_anims.cmp) */

/* Mat4 helpers */
Mat4 mat_identity(void);
Mat4 mat_mul(Mat4 a, Mat4 b);
Mat4 mat_trs_z(float x, float y, float z, float deg);
Mat4 mat_tq(const float t[3], const float q[4]);   /* q in the game's row-vector convention */

#endif
