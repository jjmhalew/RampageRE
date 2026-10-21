/* cloth.h - the game's cloth: the monsters' hair, fur and whiskers and the cities' flags (MODEL|cloth entries, type 25;
 * docs/FORMAT_BDP.md "Cloth", docs/CLOTH_NOTES.md). A port of the ClothInstance code (0x1D5920 .. 0x1DAAA0; the
 * entry is a ClothEntry, vtable 0x446500, its offsets fixed up by 0x1DB840): set up by 0x1DA4B0, stepped each frame
 * by 0x1D90E0. Each part is a set of particles, some held by their bones (pins), the rest integrated with
 * semi-implicit Euler in substeps of MAX_TIME_DELTA under gravity, air drag relative to the pins' mean velocity,
 * springs and bends (flags) or springs to the skinned rest shape (monsters, IS_STIFF), then the springs' stretch is
 * cut back NUM_INV_DYN_ITERS times (inverse dynamics) and the velocities re-read from the positions.
 *
 * Engine-agnostic: plain arrays in, positions out. Matrices are column-major 4x4 (OpenGL order; the game's 4x3 row
 * matrices have the same memory layout), points transform as m * (x, y, z, 1). The simulation runs in the space of
 * the matrices it is given: give it world-space matrices so that the monster's motion through the world drags the
 * hair (the game does), and convert back with cloth_to_local() to draw under the instance's own model matrix.
 *
 * Driving it each frame:
 *   cloth_set_world(c, world);            the object's model -> world matrix (a flag: its placement x ClothOffset)
 *   cloth_step(c, dt, skin, wind);        skin[node] = world x node_model[node] x inverse_bind[node] (the skinning
 *                                         palette of the skeleton, premultiplied by world); NULL for a flag
 *   cloth_positions(c), cloth_normals(c)  xyz per particle, world space
 * The first cloth_step after cloth_new (or after cloth_reset) only places the cloth at rest, as the game does. */
#ifndef RAMPAGE_CLOTH_H
#define RAMPAGE_CLOTH_H
#include <stdint.h>

typedef struct Cloth Cloth;

/* from the type 25 entry's data (the bundle keeps it; the cloth copies what it needs). NULL if it does not parse */
Cloth *cloth_new(const uint8_t *entry_data, uint32_t size);
void cloth_free(Cloth *c);

/* all the parts' particles, part after part (named cloth_count: data.h already has a cloth_particles()) */
int cloth_count(const Cloth *c);
int cloth_parts(const Cloth *c);
void cloth_part_range(const Cloth *c, int part, int *first, int *count);
/* the skeleton nodes the cloth reads (the entry's bone list); skin[] must have at least cloth_nodes() matrices */
const uint32_t *cloth_bones(const Cloth *c, int *count);
int cloth_nodes(const Cloth *c);                     /* 1 + the highest node index any particle uses, 0 for a flag */

void cloth_set_world(Cloth *c, const float world[16]);   /* identity until set */
/* at rest on the posed skeleton now (0x1D9B80): every particle where its bones put it, still. skin may be NULL */
void cloth_reset(Cloth *c, const float (*skin)[16]);
/* one frame of dt seconds (0x1D90E0). wind: a force added to every free particle this frame (0x1D6740; the flags'
 * comes from cloth_flag_wind, the monsters get none), may be NULL. dt <= 0 does nothing. */
void cloth_step(Cloth *c, float dt, const float (*skin)[16], const float wind[3]);

/* after a step, a flag's collisions (the entry's own spheres, 0x1D87F0, are none): every free particle on the far
 * side of the plane n.x = d (n unit) is put on it (0x1D6630, a building's flag against its wall), or every one within
 * sqrt(r) of the centre out to r (0x1D6520, the game's test [R]); its velocity bounced off the collider moving at vel.
 * Remakes the normals as for a cloth without skin (a flag). The particles moved */
int cloth_collide_plane(Cloth *c, const float n[3], float d, const float vel[3]);
int cloth_collide_sphere(Cloth *c, const float centre[3], float r, const float vel[3]);
void cloth_step_again(Cloth *c, float dt);           /* a frame more under the last frame's matrix, no wind */

const float *cloth_positions(const Cloth *c);        /* xyz per particle */
const float *cloth_normals(const Cloth *c);          /* xyz per particle, unit (0x1D6E30) */
const float *cloth_bind_positions(const Cloth *c);   /* xyz per particle: the entry's, model space of the bind pose */
int cloth_held(const Cloth *c, int i);               /* 1 = pinned to its bones (or to the object) */
/* positions (and normals if nrm_out) brought back into the model space of the current world matrix */
void cloth_to_local(const Cloth *c, float *pos_out, float *nrm_out);

/* mapping a cloth mesh (the type 0 meshes beside the entry) to the particles. Every mesh vertex sits on a particle's
 * bind position, but some positions carry two or three particles (hair strands from one root, seams: Amanda has 16
 * such places), so map a vertex through a triangle it is in: cloth_triangle finds the cloth triangle on those three
 * bind positions and gives its particles in the order asked (1), else each corner's nearest particle (0). */
int cloth_nearest(const Cloth *c, const float pos[3]);   /* the particle whose bind position is nearest */
int cloth_triangle(const Cloth *c, const float p0[3], const float p1[3], const float p2[3], int out[3]);
const uint32_t *cloth_triangles(const Cloth *c, int *count);   /* 3 particle indices each, all parts */

/* the cloth's own render mesh, as the game draws it (0x1DAB40, 0x1DA4B0, ClothRenderer 0x1AB120 / 0x1AA960). The
 * entry <Model>|cloth belongs to a model whose type 0 mesh (<Model>|<Model> for the flags, 000GEORGE_CLOTH|George_Hair
 * ...) has one mesh part per cloth part, in order. Each mesh part carries, besides its vertex stream (the record's
 * +0x40), a u16 vertex -> particle table (the record's +0x3C: {u32 count, u32, u32 offset in data2}); every frame the
 * renderer writes particle (map[v]) position and normal into vertex v and draws the part's strips with its
 * material. Model names are matched without case (0x19AE80 hashes tolower). A city object reaches its flag
 * through ClothBit| { ClothRef|Name = "Flagpole_US" }: that names a template in Objects1_props (Flagpole_US {
 * ClothProps = "Cloth_Test_Props.txt"  Shape|Shape = "Flag_US" }), whose Shape is the model. */
typedef struct {
    uint16_t material;                               /* name index (in the mesh's bundle) of the part's material */
    int nverts;
    int *particle;                                   /* per vertex: the particle (cloth_positions numbering) */
    float *uv;                                       /* 2 per vertex, NULL when the part has none (the poles) */
    int ntris;
    uint32_t *tris;                                  /* 3 vertex indices each, from the strips, wound as mesh.c */
} ClothMeshPart;
int cloth_mesh_parts(const uint8_t *mesh_data, uint32_t size);   /* parts of a type 0 mesh entry, -1 if bad */
/* mesh part `part` of the model's type 0 mesh entry (its data and data2) for cloth part `part`: 0 = ok, -1 if it
 * does not parse or does not fit the cloth. Free with cloth_mesh_part_free */
int cloth_mesh_part(const Cloth *c, int part, const uint8_t *mesh_data, uint32_t size, const uint8_t *data2,
                    uint32_t size2, ClothMeshPart *out);
void cloth_mesh_part_free(ClothMeshPart *p);

/* the wind on a city flag (the ClothObject update 0x3473B0): the ClothBit property group of the object */
typedef struct {
    float dir_freq;                                  /* HorizontalWindDirFreq   (+0x150) */
    float force;                                     /* HorizontalWindForce     (+0x154) */
    float force_freq;                                /* HorizontalWindForceFreq (+0x158) */
    float min_angle, max_angle;                      /* HorizontalWindMinAngle, MaxAngle (+0x15C, +0x160), radians */
    float vert_freq, vert_force;                     /* VerticalWindFreq, VerticalWindForce (+0x164, +0x168) */
    float time_offset;                               /* TimeOffset (+0x148) */
} ClothWind;
/* the force for cloth_step at game time t seconds: world space, z up */
void cloth_flag_wind(const ClothWind *w, float t, float out[3]);
float cloth_noise(float x);                          /* 0x13C160: 5 octaves of smoothed 1D value noise, about -2 .. 2 */

#endif
