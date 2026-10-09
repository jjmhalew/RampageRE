/* city.h - a city block put together from the property books (city.c, the port of tools/city.py; docs/FORMAT_BDP.md
 * "City layout", "Spawns", "Lighting"), with what moves in it: the props' own loops, the people's clips, traffic and a
 * monster. Game space: Z up, the street towards -Y. Matrices are column-major 4 x 4 (OpenGL order). */
#ifndef RAMPAGE_CITY_H
#define RAMPAGE_CITY_H
#include "data.h"
#include "building.h"

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
    /* root motion the monster takes over (0x301080): the root's axes in still_root (bit 0 x, 1 y, 2 z) are held
     * at root_hold, and its turn about Z is taken out by root_turn degrees */
    int still_root; float root_hold[3], root_turn;
    float (*pose)[7];                                 /* the monster: each node's last local pose (t, q) */
    /* the monster's face layer (0x2E9350, 0x2FFF10): a clip on its own 0 .. 1 clock whose bones replace the body
     * clip's in face_mask (the nodes under Bip01 Head, the head itself too for a head layer) */
    Clip face; int has_face; double face_t; uint8_t *face_mask;
    float (*held)[7]; float blend_left, blend_time;   /* the pose blended from while blend_left > 0 (0x2FFF10) */
    int hidden;                                       /* not drawn (a building's interior until it is damaged) */
    int dynamic;                                      /* its base moves (a building falling): refreshed every frame */
    int mirrored;                                     /* skinned mirror-wise: left and right swapped, x reflected */
} Instance;

typedef struct {
    uint32_t mesh;                                    /* index into Scene.meshes */
    int instance, node;                               /* node -1: the instance's root */
    Mat4 world;                                       /* updated by scene_update for moving instances */
    Skin *skin;                                       /* skinned meshes: draw skin->pos / nrm instead of the mesh's */
} DrawItem;

typedef struct { float y, direction, phase, turn_deg; } Vehicle;

typedef struct {                                      /* a building's cells and its facade quads */
    Building b;
    int mesh;                                         /* the facade's scene mesh, -1 none */
    int32_t quad[BUILDING_STOREYS][BUILDING_CELLS];   /* per cell (building.h order) its quad's first vertex, -1 */
    /* per storey its interior models (instances, -1 none): Skeleton once a cell is damaged, DamagedSkeleton once
     * FloorHP is down to DamagedSkeletonHP (half of FloorHP unless set; drawn around 0x2CD898) */
    int skeleton[BUILDING_STOREYS], damaged_skeleton[BUILDING_STOREYS], skeleton_hp[BUILDING_STOREYS];
    int ground, facade, top, rubble;                  /* its instances (-1 none) */
    int first, last; Mat4 *rest;                      /* all its instances, first .. last - 1, where they were built */
    Mat4 node;                                        /* its place */
    int moving;                                       /* it moved last frame */
    /* the facade's other passes (FORMAT_BDP.md "Facade geometry"), parts 1 .. 3 of its mesh: the window glass, the
     * cracks, the inner corner walls. Per sheet column its window outline in cell units (x0, z0, x1, z1; x0 < 0
     * none), the sheet's InnerWallColumn, 1 / CrackScale and CrackTint */
    float outline[4][4]; int inner_wall; float crack_k, crack_tint[3];
} SceneBuilding;   /* a car on a lane, phase 0..1 along it */

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
    float cube_tint[3];                               /* LightingData's CubeTint (the window glass), 0 .. 255 */
    /* the objects whose template Flags have IsObstacle (object flag 0x40000: the electric side signs): their boxes
     * in the world (min, max: AutoCollisionBox, the model's bounds), for the climbing's -2 (0x2801A0, mask 0x40040) */
    int *obstacle_of; float (*obstacles)[6]; uint32_t nobstacles;
    uint32_t cloud_material; float cloud_rate[2];    /* the material whose texture drifts (AnimatingSkyData) */
    int people, missing;
    int monster;                                      /* instance of the monster, -1 = none */
    const char *monster_clip;                         /* the name of the clip it plays */
    const Bundle *monster_anims;
    const Bundle *monster_bundle;                     /* <Monster>.cmp: its meshes, skeleton and MONSTER_DATA */
    float spheres[64][4]; int nspheres;               /* drawn over the scene as wire spheres: centre, radius */
    SceneBuilding *buildings; uint32_t nbuildings;
    /* the monster's arm IK (ik.c), per hand (0 left, 1 right, as drawn): its share, where the hand goes (model space,
     * as drawn: rotation 3 x 3 column-major), the chain (upper arm, forearm, hand; -1 none), and the hand as the
     * clips posed it before the IK (model space, as drawn) */
    float ik_w[2], ik_pos[2][3], ik_rot[2][9];
    int ik_chain[2][3], ik_ready;
    Mat4 ik_anim[2];
    float ik_shoulder[2][3], ik_reach[2];             /* the upper arm's place (model space, as drawn), the arm's length */
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
void scene_monster_place(Scene *s, float x, float y, float z, float deg);
/* the root's axes held (bit 0 x, 1 y, 2 z) at hold, and turn degrees about Z taken out of it: the motion the
 * monster itself makes */
void scene_monster_root_motion(Scene *s, int axes, const float hold[3], float turn);
int scene_monster_play(Scene *s, const char *clip);
/* the face layer plays clip (head: Bip01 Head itself too), at face_t 0 .. 1 (Instance.face_t); 0 = ok */
int scene_monster_face(Scene *s, const char *clip, int head);
/* the monster's hand (0 left, 1 right, as drawn) in the world, as the clips posed it (before the IK); 0 = ok */
int scene_monster_hand(const Scene *s, int hand, Mat4 *world);
/* where that arm's shoulder (upper arm) is in the world, as posed, and how far the arm reaches; 0 = ok */
int scene_monster_shoulder(const Scene *s, int hand, float at[3], float *reach);
/* the buildings' next dt seconds (collapses, destruction, shaking: building_update), and their facades, interiors,
 * storeys and rubble drawn as they now are */
void scene_buildings_update(Scene *s, float dt);
/* the next clip blends in over `seconds` from the pose the monster has now, as the game's animation player does
 * (0x2FFA00): call before scene_monster_play; scene_monster_blend_step counts it down */
void scene_monster_blend(Scene *s, float seconds);
void scene_monster_blend_step(Scene *s, float dt);  /* the clip of that name (any case, as MONSTER_DATA names it) */
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
