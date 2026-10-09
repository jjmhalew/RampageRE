/* data.h - the game's file formats, as the Python tools read them (docs/FORMAT_BDP.md, tools/), in C:
 * RAMPAGE.VOL (vol.c), Pipeworks bundles (bundle.c), textures (tex.c), meshes (mesh.c), property books (prb.c) and
 * node trees with their bind pose (tree.c). Everything little endian; the readers never assume alignment. */
#ifndef RAMPAGE_DATA_H
#define RAMPAGE_DATA_H
#include <stdint.h>
#include <stdio.h>

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline float rdf(const uint8_t *p) { union { uint32_t u; float f; } v; v.u = rd32(p); return v.f; }

/* ---- RAMPAGE.VOL ("PVOL" 0x1001: u32 count, u32 name block size, count x {u32 offset, u32 size, u32 index}, names) */
typedef struct { const char *name; uint32_t offset, size; } VolFile;
typedef struct { FILE *f; VolFile *files; uint32_t count; char *names; } Vol;
int vol_open(Vol *v, const char *path);                 /* 0 = ok */
const VolFile *vol_find(const Vol *v, const char *name); /* case-insensitive */
uint8_t *vol_read(const Vol *v, const char *name, uint32_t *size);   /* malloc'd copy of a file, NULL if absent */

/* ---- bundles (.bdp/.cmp, "Pipeworks Bundle File version 1.3", segment 0) */
enum { ET_MESH = 0, ET_RAW = 2, ET_TREE = 3, ET_ANIM = 4, ET_MATERIAL = 6, ET_TEXTURE = 9, ET_PALETTE = 13,
       ET_PWK = 16, ET_SKINNED = 17, ET_EFFECT = 20, ET_BOOK = 22, ET_LOC = 23, ET_IFC = 24, ET_CLOTH = 25 };
typedef struct { uint16_t index; uint8_t type, extra; uint32_t name; const uint8_t *data, *data2; uint32_t size, size2; } BdpEntry;
typedef struct {
    uint8_t *file; uint32_t file_size;
    const char **names; uint32_t nnames;
    BdpEntry *entries; uint32_t nentries;
    int32_t *by_name;                                    /* per name index: entry index or -1 */
} Bundle;
int bdp_load(Bundle *b, uint8_t *file, uint32_t size);   /* takes the buffer; 0 = ok */
void bdp_free(Bundle *b);
const char *bdp_name(const Bundle *b, uint32_t i);        /* name i without its leading 0x13 */
const BdpEntry *bdp_entry(const Bundle *b, uint32_t name_index);   /* the entry with that name, NULL if none */
int bdp_find_name(const Bundle *b, const char *name);    /* name index of a name (case-insensitive), -1 if absent */

/* ---- textures (type 9) to RGBA8, mip level 0 */
int tex_decode(const Bundle *b, const BdpEntry *e, uint8_t **rgba, int *w, int *h);   /* 0 = ok; *rgba malloc'd */

/* ---- meshes (types 0 and 17): one strip per part, triangulated */
typedef struct {
    uint32_t material;                                   /* name index: a material (type 6) or a texture (type 9) */
    uint32_t nverts, ntris;
    float *pos, *nrm, *uv, *col;                         /* 3, 3, 2, 4 floats per vertex (nrm / uv / col may be NULL) */
    uint8_t (*bones)[2]; float *weight;                  /* skinned: skeleton node indices and the first one's weight */
    uint32_t *tris;                                      /* 3 indices per triangle */
} MeshPart;
typedef struct { MeshPart *parts; uint32_t nparts; } Mesh;
int mesh_read(const BdpEntry *e, Mesh *m);               /* 0 = ok */
void mesh_free(Mesh *m);
uint32_t material_texture(const Bundle *b, uint32_t material);   /* texture name index of a part's material, 0 = none */

/* ---- property books (type 22), read in place; a value of a group-valued record is an offset into the book */
enum { PK_INT = 0, PK_FLOAT = 1, PK_NAME = 2, PK_GROUP = 3 };
typedef struct { const uint8_t *d; const Bundle *b; } Book;
typedef struct { const Book *book; uint32_t at; } Group;   /* at = 0: none */
typedef struct { uint32_t kind; const char *name; uint32_t value; } Record;
int book_open(Book *bk, const Bundle *b);                /* the bundle's first property book; 0 = ok */
Group book_group(const Book *bk, const char *name);       /* a top-level group by name */
uint32_t group_count(Group g);
Record group_record(Group g, uint32_t i);
const char *group_name(Group g);
/* lookups follow ParentPropertySet = "Group:Book" through the books the resolver can load */
typedef Group (*BookResolver)(void *ctx, const char *book, const char *group);
void prb_set_resolver(BookResolver r, void *ctx);
int  group_get(Group g, const char *key, Record *out);   /* 1 = found (in g or a parent) */
Group group_sub(Group g, const char *key);                /* a group-valued key */
const char *group_str(Group g, const char *key);          /* a name-valued key, NULL if absent */
float group_num(Group g, const char *key, float def);     /* an int or float key */
uint32_t group_items(Group g, Record *out, uint32_t max); /* the unnamed list elements (kind >> 8 == 2) */
uint32_t group_values(Group g, const char *key, Group *where, Record *out, uint32_t max);   /* every record named key, from
                                                         * the nearest group (g or a parent) that has one; *where = that group */
uint32_t group_all(Group g, const char *prefix, Record *out, uint32_t max);   /* own records whose name starts with prefix */
Group record_group(Group g, Record r);
float record_num(Record r);
const char *record_str(Group g, Record r);

/* ---- node trees (type 3) and their rest pose: the RawAnim of the same name at time 0, or the skeleton's bind pose */
typedef struct {
    int parent; const char *name;
    float t[3], q[4];                                    /* rest pose; q: x, y, z, w, row-vector convention */
    float inverse_bind[16]; int has_bind;                /* skeletons: the stored inverse bind matrix (column-major) */
} TreeNode;
typedef struct { TreeNode *nodes; uint32_t n; } Tree;
int tree_read(const Bundle *b, const BdpEntry *tree, const BdpEntry *rawanim, Tree *t);   /* rawanim may be NULL */
void tree_free(Tree *t);

/* ---- animation clips (type 4): KeyInterpAnim (kind 2, every monster and person clip) and RawAnim (kind 1) */
typedef struct {
    const uint8_t *data;
    uint32_t kind, bones, skeleton;                      /* skeleton: name index of the node tree it drives */
    float duration;                                      /* seconds */
    const uint8_t **trans, **rot;                        /* kind 2: per bone its tracks; kind 1: trans = per bone its record */
} Clip;
int clip_read(const BdpEntry *e, Clip *c);               /* 0 = ok */
void clip_free(Clip *c);
int clip_moves(const Clip *c);                           /* RawAnims mostly hold one pose; 1 = this one moves */
int clip_pose(const Clip *c, double seconds, uint32_t bone, float t[3], float q[4]);   /* looping; 0 = bone not in it */
/* the shortest-way slerp from a to b at f (0: a, 1: b), normalised; nearly equal ones lerp */
void quat_slerp(const float a[4], const float b[4], float f, float out[4]);

/* ---- cloth (type 25): the particles of a monster's hair, each held by up to four bones (docs/FORMAT_BDP.md "Cloth") */
typedef struct { float pos[3]; uint16_t bones[4]; float weights[4]; int nbones; } ClothParticle;
uint32_t cloth_particles(const BdpEntry *e, ClothParticle **out);   /* malloc'd; returns the count */

#endif
