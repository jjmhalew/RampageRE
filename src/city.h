/* city.h - a city block put together from the property books (city.c, the port of tools/city.py; docs/FORMAT_BDP.md
 * "City layout", "Spawns", "Lighting"): what to draw, where, and with which lights. Game space: Z up, metres-ish units,
 * the street towards -Y. Matrices are column-major 4 x 4 (OpenGL order). */
#ifndef RAMPAGE_CITY_H
#define RAMPAGE_CITY_H
#include "data.h"

typedef struct { float m[16]; } Mat4;

typedef struct { Mesh mesh; } SceneMesh;            /* a bundle mesh entry, or a generated facade */
typedef struct {
    uint32_t mesh;                                   /* index into Scene.meshes */
    Mat4 world;
    int vehicle;                                     /* -1, or the vehicle it rides on (Scene.vehicles) */
    Mat4 local;                                      /* vehicles: the transform below the vehicle's root */
} DrawItem;
typedef struct { float y, direction, phase; float turn_deg; } Vehicle;   /* a car on a lane, phase 0..1 along it */

typedef struct {
    const Bundle *city;                              /* textures and meshes come from the city bundle */
    SceneMesh *meshes; uint32_t nmeshes, meshes_cap;
    int32_t *mesh_of_entry;                          /* per city bundle entry: its SceneMesh, -1 = not loaded */
    DrawItem *items; uint32_t nitems, items_cap;
    Vehicle *vehicles; uint32_t nvehicles;
    float ambient[3]; float light_colour[4][3], light_dir[4][3]; int nlights;
    float fog_colour[3], fog_start, fog_end;
    uint32_t cloud_material; float cloud_rate[2];   /* the material whose texture drifts (AnimatingSkyData) */
    int people, missing;
} Scene;

typedef struct City City;
City *city_open(const Vol *vol, const char *name);  /* NULL if the city's bundles are not in the archive */
void city_close(City *c);
uint32_t city_blocks(City *c, const char **names, uint32_t max);   /* the block list, without repeats */
int city_build(City *c, const char *block, int night, Scene *s);    /* 0 = ok */
void scene_free(Scene *s);
float vehicle_x(const Vehicle *v, double t);       /* where a vehicle is at time t (seconds) */
const Bundle *city_bundle(City *c);

/* Mat4 helpers */
Mat4 mat_identity(void);
Mat4 mat_mul(Mat4 a, Mat4 b);
Mat4 mat_trs_z(float x, float y, float z, float deg);
Mat4 mat_tq(const float t[3], const float q[4]);   /* q in the game's row-vector convention */

#endif
