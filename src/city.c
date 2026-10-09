/* city.c - a city block from the property books, as tools/city.py builds it (docs/FORMAT_BDP.md "Assembling a block",
 * "Spawns", "Lighting"):
 *   <City>_Props: WorldData (BlockData.BlockList, BlockDef|<block>, LightingData / LightingDataNight, the template books)
 *   <City>_buildings: building templates (ShapeSet|GroundFloorShapes, TopFloorShapes, TextureSheet, Floor|)
 *   Objects1_Props (ObjectTemplateBook): object and actor templates
 *   <City>.bdp: the models (node trees with their meshes) and textures
 * A building's ground floor is 6 high and each storey above it 3; the storeys have no mesh, their facade is made from the
 * template's 4 x 4 texture sheet (column = window style, row = damage). Traffic runs on the two lanes of 0x332D90.
 * What moves: models whose own RawAnim moves (rotors, the fountain), the people's state clips, traffic and a monster
 * (its skeleton's clips; its skinned mesh and cloth posed on the CPU every frame). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "plat.h"
#include "city.h"

#define CELL 5.0f
#define GROUND_FLOOR 6.0f
#define STOREY 3.0f
#define LANE_END 75.0f
#define TRAFFIC_SPEED 15.0f
#define MAX_BOOKS 32
#define MAX_REC 512

typedef struct { char name[64]; Bundle b; Book book; int ok; } LoadedBook;
struct City {
    const Vol *vol;
    char name[64];
    Bundle bundle;
    LoadedBook books[MAX_BOOKS]; int nbooks;
    Group world;
    char template_books[4][64]; int ntemplate_books;
    char (*model_key)[64];                           /* per city entry: model name of a mesh entry ("model|object") */
};

/* ---------------------------------------------------------------- matrices */

Mat4 mat_identity(void)
{
    Mat4 m = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
    return m;
}

Mat4 mat_mul(Mat4 a, Mat4 b)
{
    Mat4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

Mat4 mat_trs_z(float x, float y, float z, float deg)
{
    Mat4 m = mat_identity();
    float c = cosf(deg * 3.14159265f / 180.0f), s = sinf(deg * 3.14159265f / 180.0f);
    m.m[0] = c; m.m[1] = s; m.m[4] = -s; m.m[5] = c;
    m.m[12] = x; m.m[13] = y; m.m[14] = z;
    return m;
}

Mat4 mat_tq(const float t[3], const float q[4])
{
    float x = -q[0], y = -q[1], z = -q[2], w = q[3];   /* stored for row vectors: the conjugate for column vectors */
    Mat4 m = mat_identity();
    m.m[0] = 1 - 2 * (y * y + z * z); m.m[4] = 2 * (x * y - z * w);     m.m[8] = 2 * (x * z + y * w);
    m.m[1] = 2 * (x * y + z * w);     m.m[5] = 1 - 2 * (x * x + z * z); m.m[9] = 2 * (y * z - x * w);
    m.m[2] = 2 * (x * z - y * w);     m.m[6] = 2 * (y * z + x * w);     m.m[10] = 1 - 2 * (x * x + y * y);
    m.m[12] = t[0]; m.m[13] = t[1]; m.m[14] = t[2];
    return m;
}

/* ---------------------------------------------------------------- books */

static LoadedBook *load_book(City *c, const char *name)
{
    for (int i = 0; i < c->nbooks; i++)
        if (_stricmp(c->books[i].name, name) == 0)
            return c->books[i].ok ? &c->books[i] : NULL;
    if (c->nbooks == MAX_BOOKS)
        return NULL;
    LoadedBook *lb = &c->books[c->nbooks++];
    memset(lb, 0, sizeof *lb);
    snprintf(lb->name, sizeof lb->name, "%s", name);
    char file[96];
    snprintf(file, sizeof file, "%s.bdp", name);
    uint32_t size;
    uint8_t *data = vol_read(c->vol, file, &size);
    if (data && bdp_load(&lb->b, data, size) == 0 && book_open(&lb->book, &lb->b) == 0)
        lb->ok = 1;
    else if (data && !lb->b.file)
        free(data);
    return lb->ok ? lb : NULL;
}

static Group resolve(void *ctx, const char *book, const char *group)
{
    LoadedBook *lb = load_book((City *)ctx, book);
    Group none = { NULL, 0 };
    return lb ? book_group(&lb->book, group) : none;
}

static Group template_of(City *c, const char *name)
{
    Group none = { NULL, 0 };
    if (!name)
        return none;
    for (int i = 0; i < c->ntemplate_books; i++) {
        LoadedBook *lb = load_book(c, c->template_books[i]);
        if (lb) {
            Group g = book_group(&lb->book, name);
            if (g.at)
                return g;
        }
    }
    return none;
}

static const char *str_item(Group g, uint32_t i)
{
    Record r[64];
    uint32_t n = group_items(g, r, 64);
    return i < n ? record_str(g, r[i]) : NULL;
}

static void location(Group inst, float xyz[3])
{
    Group loc = group_sub(inst, "Location");
    Record r[3];
    uint32_t n = loc.at ? group_items(loc, r, 3) : 0;
    for (uint32_t k = 0; k < 3; k++)
        xyz[k] = k < n ? record_num(r[k]) : 0.0f;
}

/* ---------------------------------------------------------------- the city */

City *city_open(const Vol *vol, const char *name)
{
    City *c = calloc(1, sizeof *c);
    c->vol = vol;
    snprintf(c->name, sizeof c->name, "%s", name);
    char file[96], props[96];
    snprintf(file, sizeof file, "%s.bdp", name);
    uint32_t size;
    uint8_t *data = vol_read(vol, file, &size);
    if (!data || bdp_load(&c->bundle, data, size) != 0) {
        free(data);
        free(c);
        return NULL;
    }
    prb_set_resolver(resolve, c);
    snprintf(props, sizeof props, "%s_Props", name);
    LoadedBook *lb = load_book(c, props);
    if (!lb) {
        city_close(c);
        return NULL;
    }
    c->world = book_group(&lb->book, "WorldData");
    const char *otb = group_str(c->world, "ObjectTemplateBook"), *wtb = group_str(c->world, "WeaponsTemplateBook");
    if (otb) snprintf(c->template_books[c->ntemplate_books++], 64, "%s", otb);
    if (wtb) snprintf(c->template_books[c->ntemplate_books++], 64, "%s", wtb);
    snprintf(c->template_books[c->ntemplate_books++], 64, "%s_Props", name);
    snprintf(c->template_books[c->ntemplate_books++], 64, "%s_buildings", name);
    c->model_key = calloc(c->bundle.nentries ? c->bundle.nentries : 1, sizeof *c->model_key);
    for (uint32_t i = 0; i < c->bundle.nentries; i++) {
        const BdpEntry *e = &c->bundle.entries[i];
        if (e->type != ET_MESH && e->type != ET_SKINNED)
            continue;
        const char *n = bdp_name(&c->bundle, e->name);
        size_t k = 0;
        for (; n[k] && n[k] != '|' && k < 63; k++)
            c->model_key[i][k] = (char)tolower((unsigned char)n[k]);
        c->model_key[i][k] = 0;
    }
    return c;
}

void city_close(City *c)
{
    if (!c)
        return;
    for (int i = 0; i < c->nbooks; i++)
        if (c->books[i].b.file)
            bdp_free(&c->books[i].b);
    bdp_free(&c->bundle);
    free(c->model_key);
    free(c);
}

uint32_t city_blocks(City *c, const char **names, uint32_t max)
{
    Group list = group_sub(group_sub(c->world, "BlockData"), "BlockList");
    Record r[MAX_REC];
    uint32_t n = group_items(list, r, MAX_REC), k = 0;
    for (uint32_t i = 0; i < n && k < max; i++) {
        const char *s = record_str(list, r[i]);
        int seen = 0;
        for (uint32_t j = 0; j < k; j++)
            seen |= strcmp(names[j], s) == 0;
        if (!seen)
            names[k++] = s;
    }
    return k;
}

/* ---------------------------------------------------------------- the scene */

static uint32_t scene_add_mesh(Scene *s, Mesh m, const Bundle *b)
{
    if (s->nmeshes == s->meshes_cap) {
        s->meshes_cap = s->meshes_cap ? 2 * s->meshes_cap : 256;
        s->meshes = realloc(s->meshes, s->meshes_cap * sizeof *s->meshes);
    }
    s->meshes[s->nmeshes].mesh = m;
    s->meshes[s->nmeshes].b = b;
    return s->nmeshes++;
}

static uint32_t scene_mesh_of(Scene *s, uint32_t entry)
{
    if (s->mesh_of_entry[entry] < 0) {
        Mesh m;
        mesh_read(&s->city->entries[entry], &m);
        s->mesh_of_entry[entry] = (int32_t)scene_add_mesh(s, m, s->city);
    }
    return (uint32_t)s->mesh_of_entry[entry];
}

static int new_instance(Scene *s, Mat4 base, int vehicle)
{
    if (s->ninstances == s->instances_cap) {
        s->instances_cap = s->instances_cap ? 2 * s->instances_cap : 256;
        s->instances = realloc(s->instances, s->instances_cap * sizeof *s->instances);
    }
    Instance *in = &s->instances[s->ninstances];
    memset(in, 0, sizeof *in);
    in->base = base;
    in->vehicle = vehicle;
    return (int)s->ninstances++;
}

static void push_item(Scene *s, uint32_t mesh, int instance, int node, Skin *skin)
{
    if (s->nitems == s->items_cap) {
        s->items_cap = s->items_cap ? 2 * s->items_cap : 1024;
        s->items = realloc(s->items, s->items_cap * sizeof *s->items);
    }
    DrawItem *it = &s->items[s->nitems++];
    it->mesh = mesh;
    it->instance = instance;
    it->node = node;
    it->world = s->instances[instance].base;
    it->skin = skin;
}

static const BdpEntry *find_entry(const Bundle *b, int type, const char *name)
{
    for (uint32_t i = 0; i < b->nentries; i++)
        if (b->entries[i].type == type && _stricmp(bdp_name(b, b->entries[i].name), name) == 0)
            return &b->entries[i];
    return NULL;
}

/* an animation (type 4) of the given kind: by its own name (kind 2) or by the tree it drives (kind 1) */
static const BdpEntry *find_anim(const Bundle *b, uint32_t kind, const char *name)
{
    for (uint32_t i = 0; i < b->nentries; i++) {
        const BdpEntry *e = &b->entries[i];
        if (e->type != ET_ANIM || e->size < 0x44 || rd32(e->data + 0x20) != kind)
            continue;
        if (_stricmp(bdp_name(b, kind == 1 ? rd16(e->data + 0x28) : e->name), name) == 0)
            return e;
    }
    return NULL;
}

/* place model `name` (with or without .pwk): each mesh object on its node of the model's tree, at its rest pose; a model
 * whose own RawAnim moves (rotors, the fountain) plays it on a loop. Returns the instance, -1 if it has no meshes. */
static int place_model(City *c, Scene *s, const char *name, Mat4 base, int vehicle)
{
    char key[64];
    size_t n = 0;
    for (; name && name[n] && n < 63; n++)
        key[n] = (char)tolower((unsigned char)name[n]);
    key[n] = 0;
    if (n > 4 && strcmp(key + n - 4, ".pwk") == 0)
        key[n - 4] = 0;
    int found = 0;
    for (uint32_t i = 0; i < c->bundle.nentries && !found; i++)
        found = c->model_key[i][0] && strcmp(c->model_key[i], key) == 0;
    if (!found) {
        s->missing++;
        return -1;
    }
    int id = new_instance(s, base, vehicle);
    Instance *in = &s->instances[id];
    const BdpEntry *te = find_entry(&c->bundle, ET_TREE, key);
    if (te) {
        const BdpEntry *raw = find_anim(&c->bundle, 1, key);
        tree_read(&c->bundle, te, raw, &in->tree);
        if (raw && clip_read(raw, &in->clip) == 0) {
            if (clip_moves(&in->clip))
                in->has_clip = 1;
            else
                clip_free(&in->clip);
        }
        in->node = calloc(in->tree.n ? in->tree.n : 1, sizeof *in->node);
    }
    for (uint32_t i = 0; i < c->bundle.nentries; i++) {
        if (!c->model_key[i][0] || strcmp(c->model_key[i], key) != 0)
            continue;
        const char *obj = strchr(bdp_name(&c->bundle, c->bundle.entries[i].name), '|');
        int node = -1;
        for (uint32_t k = 0; obj && k < in->tree.n; k++)
            if (_stricmp(in->tree.nodes[k].name, obj + 1) == 0) {
                node = (int)k;
                break;
            }
        push_item(s, scene_mesh_of(s, i), id, node, NULL);
    }
    return id;
}

static void place_object(City *c, Scene *s, Group inst, Mat4 parent)
{
    Group t = template_of(c, group_str(inst, "Name"));
    const char *shape = t.at ? group_str(t, "Shape|Shape") : NULL;
    if (!shape) {
        s->missing++;
        return;
    }
    float xyz[3];
    location(inst, xyz);
    place_model(c, s, shape, mat_mul(parent, mat_trs_z(xyz[0], xyz[1], xyz[2], group_num(inst, "ZRotation", 0))), -1);
}

/* a person in its template's default state (ActorAnimShape|<DefaultAnim>: model and clip), the clip on a loop */
static void place_actor(City *c, Scene *s, Group inst)
{
    Group t = template_of(c, group_str(inst, "Name"));
    if (!t.at) {
        s->missing++;
        return;
    }
    const char *state = group_str(t, "DefaultAnim");
    char key[96];
    snprintf(key, sizeof key, "ActorAnimShape|%s", state ? state : "Idle");
    Group look = group_sub(t, key);
    if (!look.at)
        look = group_sub(t, "ActorAnimShape|Idle");
    const char *clip_name = look.at ? group_str(look, "Anim|AnimName") : NULL;
    const BdpEntry *ce = clip_name ? find_anim(&c->bundle, 2, clip_name) : NULL;
    const char *shape = look.at ? group_str(look, "Shape|ShapeName") : NULL;
    if (!shape) shape = group_str(t, "Shape|ShapeName");
    if (!shape) shape = group_str(t, "Shape|Shape");
    if (!shape && ce)                             /* the clip's own node tree */
        shape = bdp_name(&c->bundle, rd16(ce->data + 0x28));
    float xyz[3];
    location(inst, xyz);
    int id = shape ? place_model(c, s, shape, mat_trs_z(xyz[0], xyz[1], xyz[2], group_num(inst, "ZRotation", 0)), -1) : -1;
    if (id < 0)
        return;
    s->people++;
    Instance *in = &s->instances[id];
    Clip clip;
    if (ce && clip_read(ce, &clip) == 0) {
        if (clip.bones == in->tree.n) {
            if (in->has_clip)
                clip_free(&in->clip);
            in->clip = clip;
            in->has_clip = 1;
            in->offset = (float)(s->people % 7) * 0.37f;   /* not everyone in step */
        } else {
            clip_free(&clip);
        }
    }
}

static uint32_t rng_next(uint32_t *state) { *state = *state * 1664525u + 1013904223u; return *state >> 8; }

static void traffic(City *c, Scene *s, Group block, const char *block_name)
{
    Group td = group_sub(block, "TrafficDef"), where;
    if (!td.at)
        return;
    Record r[64];
    uint32_t n = group_values(td, "VehicleDef|", &where, r, 64);
    const char *names[64];
    float freq[64], total = 0;
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        Group v = record_group(where, r[i]);
        const char *name = group_str(v, "OTRef|Name");
        float f = group_num(v, "Frequency", 0);
        if (name && f > 0) {
            names[m] = name;
            freq[m] = f;
            total += f;
            m++;
        }
    }
    if (!m)
        return;
    uint32_t seed = 2166136261u;                   /* the game picks at random; here a fixed draw per block */
    for (const char *p = block_name; *p; p++)
        seed = (seed ^ (uint8_t)*p) * 16777619u;
    int per_lane = (int)lroundf(2 * LANE_END / TRAFFIC_SPEED / 3.5f), cap = (int)group_num(td, "MaxVehicles", 7) / 2;
    if (per_lane > cap) per_lane = cap;
    if (per_lane < 1) per_lane = 1;
    int off = (group_num(td, "NoFrontTraffic", 0) != 0 ? 1 : 0) | (group_num(td, "NoRearTraffic", 0) != 0 ? 2 : 0);
    static const struct { float y, dir, turn; int bit; } lanes[2] = { { -12.5f, 1, -90, 1 }, { -7.5f, -1, 90, 2 } };
    s->vehicles = realloc(s->vehicles, sizeof *s->vehicles * (s->nvehicles + 2 * (uint32_t)per_lane));
    for (int l = 0; l < 2; l++) {
        if (off & lanes[l].bit)
            continue;
        for (int k = 0; k < per_lane; k++) {
            float pick = (rng_next(&seed) & 0xFFFF) / 65536.0f * total;
            uint32_t j = 0;
            while (j + 1 < m && pick >= freq[j]) { pick -= freq[j]; j++; }
            Group t = template_of(c, names[j]);
            const char *shape = t.at ? group_str(t, "Shape|Shape") : NULL;
            Vehicle v = { lanes[l].y, lanes[l].dir, (float)k / per_lane, lanes[l].turn };
            s->vehicles[s->nvehicles] = v;
            if (shape && place_model(c, s, shape, mat_identity(), (int)s->nvehicles) >= 0)
                s->nvehicles++;
        }
    }
}

static float vehicle_x(const Vehicle *v, double t)
{
    double f = fmod(t * TRAFFIC_SPEED / (2 * LANE_END) + v->phase, 1.0);
    return v->direction * (float)(-LANE_END + 2 * LANE_END * f);
}

/* the generated upper storeys: one quad per facade cell, its tile out of the 4 x 4 sheet */
static Mesh facade(uint32_t sheet, int width, int depth, const float *z, Record (*tiles)[64], const uint32_t *ntiles,
                   int nstoreys)
{
    Mesh m = { 0 };
    int cells = width + 2 * depth;
    uint32_t nq = (uint32_t)(cells * nstoreys);
    m.parts = calloc(1, sizeof *m.parts);
    m.nparts = 1;
    MeshPart *p = m.parts;
    p->material = sheet;
    p->pos = malloc(sizeof(float) * 12 * (nq ? nq : 1));
    p->nrm = malloc(sizeof(float) * 12 * (nq ? nq : 1));
    p->uv = malloc(sizeof(float) * 8 * (nq ? nq : 1));
    p->tris = malloc(sizeof(uint32_t) * 6 * (nq ? nq : 1));
    float hw = width * CELL / 2, hd = depth * CELL / 2;
    for (int st = 0; st < nstoreys; st++) {
        for (int cell = 0; cell < cells && (uint32_t)cell < ntiles[st]; cell++) {
            float ax, ay, bx, by, nx, ny;
            if (cell < depth) {                       /* left side, back to front, facing -X */
                ax = -hw; ay = hd - CELL * cell; bx = -hw; by = ay - CELL; nx = -1; ny = 0;
            } else if (cell < depth + width) {        /* front, left to right, facing -Y */
                int k = cell - depth;
                ax = -hw + CELL * k; ay = -hd; bx = ax + CELL; by = -hd; nx = 0; ny = -1;
            } else {                                  /* right side, front to back, facing +X */
                int j = cell - depth - width;
                ax = hw; ay = -hd + CELL * j; bx = hw; by = ay + CELL; nx = 1; ny = 0;
            }
            float u0 = (float)((int)record_num(tiles[st][cell]) % 4) * 0.25f;
            float corner[4][5] = { { ax, ay, z[st], u0, 0.25f }, { ax, ay, z[st] + STOREY, u0, 0 },
                                   { bx, by, z[st], u0 + 0.25f, 0.25f }, { bx, by, z[st] + STOREY, u0 + 0.25f, 0 } };
            uint32_t v0 = p->nverts;
            for (int k = 0; k < 4; k++) {
                memcpy(p->pos + 3 * p->nverts, corner[k], 12);
                p->nrm[3 * p->nverts] = nx; p->nrm[3 * p->nverts + 1] = ny; p->nrm[3 * p->nverts + 2] = 0;
                p->uv[2 * p->nverts] = corner[k][3]; p->uv[2 * p->nverts + 1] = corner[k][4];
                p->nverts++;
            }
            uint32_t q[6] = { v0, v0 + 1, v0 + 2, v0 + 2, v0 + 1, v0 + 3 };
            memcpy(p->tris + 3 * p->ntris, q, sizeof q);
            p->ntris += 2;
        }
    }
    return m;
}

static void building(City *c, Scene *s, Group inst, float row_offset)
{
    Group t = template_of(c, group_str(inst, "TemplateName")), where;
    if (!t.at) {
        s->missing++;
        return;
    }
    int width = (int)group_num(t, "width", 1), depth = (int)group_num(t, "depth", 2);
    Mat4 node = mat_trs_z(group_num(inst, "LeftEdgeXPos", 0) + width * CELL / 2, row_offset * group_num(inst, "Row", 0), 0, 0);
    int first = (int)s->ninstances, ground_inst = -1, facade_inst = -1, top_inst = -1;
    uint32_t had = s->nbuildings;
    const char *ground = str_item(group_sub(t, "ShapeSet|GroundFloorShapes"), 0);
    if (ground)
        ground_inst = place_model(c, s, ground, node, -1);
    static Record fr[64], tiles[64][64];
    static Group floors[64];
    static uint32_t ntiles[64];
    static float z[64];
    uint32_t nf = group_values(t, "Floor|", &where, fr, 64), nfloors = 0;
    for (uint32_t i = 0; i < nf; i++) {
        Group f = record_group(where, fr[i]);
        if (group_count(f))
            floors[nfloors++] = f;
    }
    int nst = 0;                                     /* every Floor| with tiles is a storey above the ground floor */
    for (uint32_t i = 0; i < nfloors; i++) {         /* (the game's grid: storeys = Floor| entries - 1, the last empty) */
        z[nst] = GROUND_FLOOR + STOREY * (float)i;
        ntiles[nst] = group_items(floors[i], tiles[nst], 64);
        nst++;
    }
    const char *sheet = group_str(t, "TextureSheet");
    int sheet_name = sheet ? bdp_find_name(&c->bundle, sheet) : -1;
    int mesh = -1;
    if (nst && sheet_name >= 0) {
        mesh = (int)scene_add_mesh(s, facade((uint32_t)sheet_name, width, depth, z, tiles, ntiles, nst), s->city);
        facade_inst = new_instance(s, node, -1);
        push_item(s, (uint32_t)mesh, facade_inst, -1, NULL);
    }
    /* its cells (the game's grid, 0x2D1A30): the box is 6 + 3 x storeys + 1.5 (the roof's band) high */
    if (nst && width > 0 && depth > 0 && 2 * depth + width <= BUILDING_CELLS) {
        s->buildings = realloc(s->buildings, sizeof *s->buildings * (s->nbuildings + 1));
        SceneBuilding *sb = &s->buildings[s->nbuildings++];
        float cx = node.m[12], cy = node.m[13];
        float lo[3] = { cx - width * CELL / 2, cy - depth * CELL / 2, 0 };
        float hi[3] = { cx + width * CELL / 2, cy + depth * CELL / 2, GROUND_FLOOR + STOREY * (float)nst + 1.5f };
        building_init(&sb->b, lo, hi, width, depth, nst);
        sb->b.hp = sb->b.hp_max = (int)group_num(t, "BuildingHP", (float)sb->b.hp_max);
        sb->b.seed = s->nbuildings;
        sb->mesh = mesh;
        sb->node = node;
        sb->ground = ground_inst;
        sb->facade = facade_inst;
        sb->top = sb->rubble = -1;
        sb->first = sb->last = 0;
        sb->rest = NULL;
        sb->moving = 0;
        sb->b.collapse_time = group_num(t, "CollapseTime", sb->b.collapse_time);
        sb->b.crack_time = group_num(t, "CrackSpreadTime", sb->b.crack_time);
        sb->b.hesitation = group_num(t, "HesitationTime", sb->b.hesitation);
        sb->b.min_crack = (int)group_num(t, "MinCollapseCrackLevel", (float)sb->b.min_crack);
        memset(sb->quad, 0xFF, sizeof sb->quad);
        memset(sb->skeleton, 0xFF, sizeof sb->skeleton);
        memset(sb->damaged_skeleton, 0xFF, sizeof sb->damaged_skeleton);
        int32_t v = 0;
        for (int st = 0; st < nst && st < BUILDING_STOREYS; st++) {
            sb->b.floor_hp[st] = (int)group_num(floors[st], "FloorHP", (float)sb->b.floor_hp[st]);
            sb->skeleton_hp[st] = (int)group_num(floors[st], "DamagedSkeletonHP", (float)(sb->b.floor_hp[st] / 2));
            Mat4 at = mat_mul(node, mat_trs_z(0, 0, z[st], 0));
            const char *sk = group_str(floors[st], "Skeleton"), *dsk = group_str(floors[st], "DamagedSkeleton");
            int missing = s->missing;
            sb->skeleton[st] = sk ? place_model(c, s, sk, at, -1) : -1;
            sb->damaged_skeleton[st] = dsk ? place_model(c, s, dsk, at, -1) : -1;
            s->missing = missing;                    /* an interior not found is not counted as missing */
            if (sb->skeleton[st] >= 0) s->instances[sb->skeleton[st]].hidden = 1;
            if (sb->damaged_skeleton[st] >= 0) s->instances[sb->damaged_skeleton[st]].hidden = 1;
            for (int k = 0; k < 2 * depth + width && (uint32_t)k < ntiles[st]; k++) {
                int face = k < depth ? FACE_LEFT : k < depth + width ? FACE_FRONT : FACE_RIGHT;
                int col = k < depth ? k : k < depth + width ? k - depth : k - depth - width;
                int i = building_cell_index(&sb->b, face, col);
                if (i >= 0) {
                    sb->quad[st][i] = v;
                    sb->b.cell[st][i].tile = (uint8_t)((int)record_num(tiles[st][k]) % 4);
                }
                v += 4;
            }
        }
    }
    const char *top = str_item(group_sub(t, "ShapeSet|TopFloorShapes"), 0);
    if (top)
        top_inst = place_model(c, s, top, mat_mul(node, mat_trs_z(0, 0, GROUND_FLOOR + STOREY * (float)nfloors, 0)), -1);
    Record obj[MAX_REC];
    uint32_t no = group_values(inst, "ObjectInstance|", &where, obj, MAX_REC);
    for (uint32_t i = 0; i < no; i++)
        place_object(c, s, record_group(where, obj[i]), node);
    if (s->nbuildings > had) {                       /* its pieces, to move */
        SceneBuilding *sb = &s->buildings[s->nbuildings - 1];
        const char *rubble = group_str(t, "Shape|RubbleShape");
        int missing = s->missing;
        sb->rubble = rubble ? place_model(c, s, rubble, node, -1) : -1;
        s->missing = missing;
        if (sb->rubble >= 0)
            s->instances[sb->rubble].hidden = 1;
        sb->top = top_inst;
        sb->first = first;
        sb->last = (int)s->ninstances;
        sb->rest = malloc(sizeof *sb->rest * (size_t)(sb->last - sb->first + 1));
        for (int i = sb->first; i < sb->last; i++)
            sb->rest[i - sb->first] = s->instances[i].base;
    }
}

static void read_lights(Scene *s, const char *text, uint32_t len)
{
    char line[256], key[64];
    float v[8];
    int in_light = 0;
    memset(v, 0, sizeof v);
    for (uint32_t i = 0; i < len;) {
        uint32_t j = 0;
        while (i < len && text[i] != '\n' && j < sizeof line - 1)
            line[j++] = text[i++];
        line[j] = 0;
        i++;
        char *comment = strstr(line, "//");
        if (comment)
            *comment = 0;
        float val = 0;
        if (sscanf(line, "%63s %f", key, &val) < 1)
            continue;
        if (!strcmp(key, "BEGIN_LIGHT")) { in_light = 1; memset(v, 0, sizeof v); }
        else if (!strcmp(key, "BEGIN_AMBIENT")) { in_light = 0; memset(v, 0, sizeof v); }
        else if (!strcmp(key, "END_AMBIENT")) { s->ambient[0] = v[0]; s->ambient[1] = v[1]; s->ambient[2] = v[2]; }
        else if (!strcmp(key, "END_LIGHT") && in_light && s->nlights < 4) {
            memcpy(s->light_colour[s->nlights], v, 12);
            memcpy(s->light_dir[s->nlights], v + 3, 12);
            s->nlights++;
        }
        else if (!strcmp(key, "RED") || !strcmp(key, "DIFFUSE_R")) v[0] = val;
        else if (!strcmp(key, "GREEN") || !strcmp(key, "DIFFUSE_G")) v[1] = val;
        else if (!strcmp(key, "BLUE") || !strcmp(key, "DIFFUSE_B")) v[2] = val;
        else if (!strcmp(key, "DIRECTION_X")) v[3] = val;
        else if (!strcmp(key, "DIRECTION_Y")) v[4] = val;
        else if (!strcmp(key, "DIRECTION_Z")) v[5] = val;
    }
}

static void lighting(City *c, Scene *s, Group ld)
{
    const char *file = group_str(ld, "text|LightFile");
    uint32_t size;
    uint8_t *data = vol_read(c->vol, "common.bdp", &size);
    Bundle common;
    if (data && bdp_load(&common, data, size) == 0) {
        for (uint32_t i = 0; i < common.nentries; i++) {
            const BdpEntry *e = &common.entries[i];
            if (e->type == ET_RAW && _stricmp(bdp_name(&common, e->name), file ? file : "default_lights.txt") == 0)
                read_lights(s, (const char *)e->data, e->size);
        }
        bdp_free(&common);
    } else {
        free(data);
    }
    s->fog_colour[0] = group_num(ld, "FogR", 0) / 255;
    s->fog_colour[1] = group_num(ld, "FogG", 0) / 255;
    s->fog_colour[2] = group_num(ld, "FogB", 0) / 255;
    s->fog_start = group_num(ld, "FogStart", 0);
    s->fog_end = group_num(ld, "FogEnd", 0);
    Group sky = group_sub(ld, "AnimatingSkyData");
    const char *mat = sky.at ? group_str(sky, "MaterialName") : NULL;
    int idx = mat ? bdp_find_name(&c->bundle, mat) : -1;
    if (idx > 0) {
        s->cloud_material = (uint32_t)idx;
        s->cloud_rate[0] = group_num(sky, "UAnimRate", 0);
        s->cloud_rate[1] = group_num(sky, "VAnimRate", 0);
    }
}

/* ---------------------------------------------------------------- posing */

static void pose_instance(Scene *s, Instance *in, double t)
{
    float w = in->held && in->blend_left > 0 && in->blend_time > 0 ? in->blend_left / in->blend_time : 0;
    for (uint32_t i = 0; i < in->tree.n; i++) {
        TreeNode *nd = &in->tree.nodes[i];
        float tr[3] = { nd->t[0], nd->t[1], nd->t[2] }, q[4] = { nd->q[0], nd->q[1], nd->q[2], nd->q[3] };
        if (in->has_clip)
            clip_pose(&in->clip, t + in->offset, i, tr, q);
        if (w > 0) {                                 /* towards the held pose by the share of the blend left */
            const float *h = in->held[i];
            for (int k = 0; k < 3; k++)
                tr[k] += (h[k] - tr[k]) * w;
            quat_slerp(q, h + 3, w > 1 ? 1 : w, q);
        }
        if (in->pose) {
            memcpy(in->pose[i], tr, sizeof tr);
            memcpy(in->pose[i] + 3, q, sizeof q);
        }
        if (nd->parent < 0)
            for (int k = 0; k < 3; k++)
                if (in->still_root >> k & 1)
                    tr[k] = in->root_hold[k];
        Mat4 local = mat_tq(tr, q);
        if (nd->parent < 0 && in->root_turn != 0)
            local = mat_mul(mat_trs_z(0, 0, 0, -in->root_turn), local);
        int p = nd->parent;
        in->node[i] = p >= 0 && (uint32_t)p < i ? mat_mul(in->node[p], local) : local;
    }
    (void)s;
}

static void skin_item(const Scene *s, const Instance *in, DrawItem *it)
{
    Skin *sk = it->skin;
    static Mat4 palette[256];
    uint32_t nb = in->tree.n < 256 ? in->tree.n : 256;
    for (uint32_t j = 0; j < nb; j++) {
        Mat4 ib;
        memcpy(ib.m, in->tree.nodes[j].inverse_bind, sizeof ib.m);
        palette[j] = mat_mul(in->node[j], ib);
    }
    const Mesh *m = &s->meshes[it->mesh].mesh;
    uint32_t v = 0;
    for (uint32_t pi = 0; pi < m->nparts; pi++) {
        const MeshPart *p = &m->parts[pi];
        for (uint32_t i = 0; i < p->nverts; i++, v++) {
            float o[3] = { 0, 0, 0 }, on[3] = { 0, 0, 0 };
            const float *x = p->pos + 3 * i, *n = p->nrm ? p->nrm + 3 * i : NULL;
            for (int k = 0; k < 4; k++) {
                float w = sk->weights[v][k];
                if (w <= 0 || sk->bones[v][k] >= nb)
                    continue;
                const float *M = palette[sk->bones[v][k]].m;
                for (int r = 0; r < 3; r++) {
                    o[r] += w * (M[r] * x[0] + M[4 + r] * x[1] + M[8 + r] * x[2] + M[12 + r]);
                    if (n) on[r] += w * (M[r] * n[0] + M[4 + r] * n[1] + M[8 + r] * n[2]);
                }
            }
            memcpy(sk->pos + 3 * v, o, sizeof o);
            memcpy(sk->nrm + 3 * v, on, sizeof on);
        }
    }
}

static void update(Scene *s, double t, int all)
{
    for (uint32_t i = 0; i < s->ninstances; i++) {
        Instance *in = &s->instances[i];
        if (in->vehicle >= 0) {
            const Vehicle *v = &s->vehicles[in->vehicle];
            in->base = mat_trs_z(vehicle_x(v, t), v->y, 0, v->turn_deg);
        }
        if (in->tree.n && (all || in->has_clip))
            pose_instance(s, in, t);
    }
    for (uint32_t i = 0; i < s->nitems; i++) {
        DrawItem *it = &s->items[i];
        Instance *in = &s->instances[it->instance];
        if (!all && !in->has_clip && in->vehicle < 0 && !in->dynamic && (int)(in - s->instances) != s->monster)
            continue;
        if (it->skin) {
            skin_item(s, in, it);
            it->world = in->base;
        } else {
            it->world = it->node >= 0 ? mat_mul(in->base, in->node[it->node]) : in->base;
        }
    }
}

void scene_update(Scene *s, double t) { update(s, t, 0); }

int city_build(City *c, const char *block_name, int night, Scene *s)
{
    memset(s, 0, sizeof *s);
    s->city = &c->bundle;
    s->monster = -1;
    s->mesh_of_entry = malloc(sizeof *s->mesh_of_entry * (c->bundle.nentries ? c->bundle.nentries : 1));
    for (uint32_t i = 0; i < c->bundle.nentries; i++)
        s->mesh_of_entry[i] = -1;
    char key[128];
    snprintf(key, sizeof key, "BlockDef|%s", block_name);
    Group block = group_sub(group_sub(c->world, "BlockData"), key), ld = group_sub(c->world, night ? "LightingDataNight" : "LightingData"), where;
    if (!block.at)
        return -1;
    Mat4 root = mat_identity();
    if (ld.at) {
        const char *backdrop = group_str(ld, "Shape|Backdrop"), *sky = group_str(ld, "Shape|SkyBox");
        if (backdrop) place_model(c, s, backdrop, root, -1);
        if (sky) place_model(c, s, sky, root, -1);
        lighting(c, s, ld);
    }
    const char *ground = group_str(block, "Shape|GroundShape");
    if (ground)
        place_model(c, s, ground, root, -1);
    float row_offset = group_num(block, "BackRowOffset", 10);
    static Record r[MAX_REC];
    uint32_t n = group_values(block, "BuildingInstance|", &where, r, MAX_REC);
    for (uint32_t i = 0; i < n; i++)
        building(c, s, record_group(where, r[i]), row_offset);
    static const char *const objects[] = { "ObjectInstance|", "AttackObjectInstance|", "XObjectInstance|" };
    for (int k = 0; k < 3; k++) {
        n = group_values(block, objects[k], &where, r, MAX_REC);
        for (uint32_t i = 0; i < n; i++)
            place_object(c, s, record_group(where, r[i]), root);
    }
    n = group_all(block, "ActorInstance|", r, MAX_REC);   /* SpawnedItem = 1: comes after an earlier one is gone */
    for (uint32_t i = 0; i < n; i++) {
        Group a = record_group(block, r[i]);
        if (a.at && group_num(a, "SpawnedItem", 0) == 0)
            place_actor(c, s, a);
    }
    traffic(c, s, block, block_name);
    update(s, 0, 1);
    return 0;
}

/* ---------------------------------------------------------------- monsters */

uint32_t monster_names(const Vol *vol, const char **names, uint32_t max)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < vol->count && k < max; i++) {
        size_t len = strlen(vol->files[i].name);
        if (len > 10 && _stricmp(vol->files[i].name + len - 10, "_anims.cmp") == 0) {
            char *n = malloc(len - 9);
            memcpy(n, vol->files[i].name, len - 10);
            n[len - 10] = 0;
            names[k++] = n;
        }
    }
    for (uint32_t i = 1; i < k; i++)                     /* sorted */
        for (uint32_t j = i; j > 0 && _stricmp(names[j - 1], names[j]) > 0; j--) {
            const char *t = names[j]; names[j] = names[j - 1]; names[j - 1] = t;
        }
    return k;
}

static Bundle *own_bundle(const Vol *vol, Scene *s, const char *file)
{
    uint32_t size;
    uint8_t *data = vol_read(vol, file, &size);
    if (!data || s->nown == 4)
        return free(data), NULL;
    Bundle *b = calloc(1, sizeof *b);
    if (bdp_load(b, data, size) != 0) {
        free(data);
        free(b);
        return NULL;
    }
    s->own[s->nown++] = b;
    return b;
}

static Skin *new_skin(const Mesh *m)
{
    Skin *sk = calloc(1, sizeof *sk);
    for (uint32_t i = 0; i < m->nparts; i++)
        sk->nverts += m->parts[i].nverts;
    sk->bones = calloc(sk->nverts ? sk->nverts : 1, sizeof *sk->bones);
    sk->weights = calloc(sk->nverts ? sk->nverts : 1, sizeof *sk->weights);
    sk->pos = calloc(3 * (sk->nverts ? sk->nverts : 1), sizeof(float));
    sk->nrm = calloc(3 * (sk->nverts ? sk->nverts : 1), sizeof(float));
    return sk;
}

int city_monster(City *c, Scene *s, const char *monster, int clip_index, float x, float y, float deg)
{
    char file[96];
    snprintf(file, sizeof file, "%s.cmp", monster);
    Bundle *mb = own_bundle(c->vol, s, file);
    snprintf(file, sizeof file, "%s_anims.cmp", monster);
    Bundle *ab = own_bundle(c->vol, s, file);
    if (!mb || !ab)
        return -1;
    const BdpEntry *te = NULL;
    for (uint32_t i = 0; i < mb->nentries && !te; i++)
        if (mb->entries[i].type == ET_TREE && mb->entries[i].extra == 1)
            te = &mb->entries[i];
    if (!te)
        return -1;
    int id = new_instance(s, mat_trs_z(x, y, 0, deg), -1);
    Instance *in = &s->instances[id];
    tree_read(mb, te, NULL, &in->tree);
    in->node = calloc(in->tree.n ? in->tree.n : 1, sizeof *in->node);
    in->pose = calloc(in->tree.n ? in->tree.n : 1, sizeof *in->pose);
    in->held = calloc(in->tree.n ? in->tree.n : 1, sizeof *in->held);
    /* its clips: the keyframed ones for this skeleton; -1 picks the body idle (SKELETON_n_IDLE, else ..._SHORT) */
    const BdpEntry *pick = NULL;
    int k = 0, pick_rank = 0;
    for (uint32_t i = 0; i < ab->nentries; i++) {
        const BdpEntry *e = &ab->entries[i];
        if (e->type != ET_ANIM || e->size < 0x44 || rd32(e->data + 0x20) != 2 || rd32(e->data + 0x2C) != in->tree.n)
            continue;
        const char *n = bdp_name(ab, e->name), *rest;
        if (!strncmp(n, "000", 3))
            n += 3;
        int rank = 0;                                /* 2: SKELETON_<n>_IDLE, 1: SKELETON_<n>_IDLE_SHORT */
        if (!_strnicmp(n, "SKELETON_", 9)) {
            for (rest = n + 9; *rest >= '0' && *rest <= '9'; rest++)
                ;
            rank = !_stricmp(rest, "_IDLE") ? 2 : !_stricmp(rest, "_IDLE_SHORT") ? 1 : 0;
        }
        if (clip_index < 0 ? rank > pick_rank : k == clip_index) {
            pick = e;
            pick_rank = rank;
        }
        k++;
    }
    if (pick && clip_read(pick, &in->clip) == 0) {
        in->has_clip = 1;
        s->monster_clip = bdp_name(ab, pick->name);
        if (!strncmp(s->monster_clip, "000", 3))
            s->monster_clip += 3;
    }
    ClothParticle *cloth = NULL;
    uint32_t ncloth = 0;
    for (uint32_t i = 0; i < mb->nentries; i++)
        if (mb->entries[i].type == ET_CLOTH && !cloth)
            ncloth = cloth_particles(&mb->entries[i], &cloth);
    for (uint32_t i = 0; i < mb->nentries; i++) {
        const BdpEntry *e = &mb->entries[i];
        if (e->type != ET_SKINNED && e->type != ET_MESH)
            continue;
        Mesh m;
        if (mesh_read(e, &m) != 0)
            continue;
        Skin *sk = new_skin(&m);
        uint32_t v = 0;
        for (uint32_t pi = 0; pi < m.nparts; pi++) {
            const MeshPart *p = &m.parts[pi];
            for (uint32_t j = 0; j < p->nverts; j++, v++) {
                if (p->bones) {                  /* skinned: two bones, the first one's weight */
                    sk->bones[v][0] = p->bones[j][0];
                    sk->bones[v][1] = p->bones[j][1];
                    sk->weights[v][0] = p->weight[j] > 1 ? 1 : p->weight[j];
                    sk->weights[v][1] = 1 - sk->weights[v][0];
                    continue;
                }
                for (uint32_t q = 0; q < ncloth; q++)   /* cloth: the bones of the particle it sits on */
                    if (!memcmp(cloth[q].pos, p->pos + 3 * j, 12)) {
                        for (int b = 0; b < cloth[q].nbones; b++) {
                            sk->bones[v][b] = (uint8_t)cloth[q].bones[b];
                            sk->weights[v][b] = cloth[q].weights[b];
                        }
                        break;
                    }
            }
        }
        push_item(s, scene_add_mesh(s, m, mb), id, -1, sk);
    }
    free(cloth);
    s->monster = id;
    s->monster_anims = ab;
    s->monster_bundle = mb;
    update(s, 0, 1);
    return 0;
}

int scene_monster_clip(Scene *s, const char *suffix, float *speed)
{
    if (s->monster < 0 || !s->monster_anims)
        return -1;
    Instance *in = &s->instances[s->monster];
    const Bundle *ab = s->monster_anims;
    for (uint32_t i = 0; i < ab->nentries; i++) {
        const BdpEntry *e = &ab->entries[i];
        if (e->type != ET_ANIM || e->size < 0x44 || rd32(e->data + 0x20) != 2 || rd32(e->data + 0x2C) != in->tree.n)
            continue;
        const char *n = bdp_name(ab, e->name), *rest;
        if (!strncmp(n, "000", 3))
            n += 3;
        if (_strnicmp(n, "SKELETON_", 9) != 0)      /* exactly SKELETON_<n><suffix>: not ..._REACT_FACEUP_IDLE */
            continue;
        for (rest = n + 9; *rest >= '0' && *rest <= '9'; rest++)
            ;
        if (_stricmp(rest, suffix) != 0)
            continue;
        Clip clip;
        if (clip_read(e, &clip) != 0)
            return -1;
        if (in->has_clip)
            clip_free(&in->clip);
        in->clip = clip;
        in->has_clip = 1;
        s->monster_clip = n;
        if (speed) {                                 /* the root's travel over one loop */
            float a[3] = { 0, 0, 0 }, b[3] = { 0, 0, 0 }, q[4];
            clip_pose(&clip, 0, 0, a, q);
            clip_pose(&clip, clip.duration * 0.9999, 0, b, q);
            *speed = clip.duration > 0 ? sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1])) / clip.duration : 0;
        }
        return 0;
    }
    return -1;
}

int scene_monster_play(Scene *s, const char *clip)
{
    if (s->monster < 0 || !s->monster_anims || !clip)
        return -1;
    Instance *in = &s->instances[s->monster];
    const Bundle *ab = s->monster_anims;
    for (uint32_t i = 0; i < ab->nentries; i++) {
        const BdpEntry *e = &ab->entries[i];
        if (e->type != ET_ANIM)
            continue;
        const char *n = bdp_name(ab, e->name);
        if (!strncmp(n, "000", 3))
            n += 3;
        if (_stricmp(n, clip) != 0)
            continue;
        Clip c;
        if (clip_read(e, &c) != 0)
            return -1;
        if (in->has_clip)
            clip_free(&in->clip);
        in->clip = c;
        in->has_clip = 1;
        s->monster_clip = n;
        return 0;
    }
    return -1;
}

/* a building's pieces where it now is: its sway, its storeys coming down, its body sinking */
static void building_place(Scene *s, SceneBuilding *sb)
{
    const Building *b = &sb->b;
    float x = building_shake(b), sink = building_sink(b);
    int gone = b->state == BUILDING_GONE;
    Mat4 sway = mat_trs_z(x, 0, -sink, 0);
    for (int i = sb->first; i < sb->last && sb->rest; i++) {
        Instance *in = &s->instances[i];
        in->dynamic = 1;
        if (i == sb->rubble) {
            in->hidden = sink <= 0;
            continue;
        }
        in->base = mat_mul(sway, sb->rest[i - sb->first]);
        if (i == sb->facade)
            in->base = mat_mul(mat_trs_z(x, 0, 0, 0), sb->rest[i - sb->first]);   /* its storeys move by vertex */
        else if (i == sb->top) {
            float bottom, height;
            building_storey(b, b->n, &bottom, &height);    /* where a storey above the last would stand */
            float was = GROUND_FLOOR + STOREY * (float)b->storeys;
            in->base = mat_mul(mat_trs_z(x, 0, bottom - was, 0), sb->rest[i - sb->first]);
        }
        if (gone && i != sb->rubble)
            in->hidden = 1;
    }
    for (int o = 0; o < b->storeys; o++) {           /* the interiors: at their storey, a falling one squashed */
        int ls = building_live_of(b, o);
        float bottom = 0, height = STOREY;
        if (ls >= 0)
            building_storey(b, ls, &bottom, &height);
        int ids[2] = { sb->skeleton[o], sb->damaged_skeleton[o] };
        for (int k = 0; k < 2; k++) {
            if (ids[k] < 0)
                continue;
            Mat4 m = mat_mul(sb->node, mat_trs_z(x, 0, bottom, 0));
            for (int r = 0; r < 3; r++)
                m.m[8 + r] *= height / STOREY;
            s->instances[ids[k]].base = m;
            if (ls < 0)
                s->instances[ids[k]].hidden = 1;
        }
    }
    if (sb->mesh >= 0) {                             /* the facade's storeys: each quad's corners, bottom and top */
        MeshPart *p = &s->meshes[sb->mesh].mesh.parts[0];
        for (int o = 0; o < b->storeys; o++) {
            int ls = building_live_of(b, o);
            float bottom = 0, height = 0;
            if (ls >= 0)
                building_storey(b, ls, &bottom, &height);
            for (int c = 0; c < BUILDING_CELLS; c++) {
                int32_t v = sb->quad[o][c];
                if (v < 0 || (uint32_t)v + 4 > p->nverts)
                    continue;
                for (int k = 0; k < 4; k++)          /* a storey that has gone: no height, nothing drawn */
                    p->pos[3 * (v + k) + 2] = bottom + (k & 1 ? height : 0);
            }
        }
    }
}

void scene_buildings_update(Scene *s, float dt)
{
    for (uint32_t i = 0; i < s->nbuildings; i++) {
        SceneBuilding *sb = &s->buildings[i];
        building_update(&sb->b, dt);
        int moving = sb->b.collapse >= 0 || sb->b.shake_time > 0 || building_sink(&sb->b) > 0 ||
                     (sb->b.state != BUILDING_STANDING && sb->b.state != BUILDING_FLOOR_FALLING);
        if (moving || sb->moving || sb->b.dirty)
            building_place(s, sb);
        sb->moving = moving;
        if (!sb->b.dirty || sb->mesh < 0)
            continue;
        sb->b.dirty = 0;
        MeshPart *p = &s->meshes[sb->mesh].mesh.parts[0];
        for (int st = 0; st < sb->b.storeys; st++)
            for (int c = 0; c < BUILDING_CELLS; c++) {
                int32_t v = sb->quad[st][c];
                if (v < 0 || (uint32_t)v + 4 > p->nverts)
                    continue;
                /* the sheet's row is the damage state (0 intact .. 3): a quad's corners are bottom, top, bottom, top */
                float row = sb->b.cell[st][c].state * 0.25f;
                for (int k = 0; k < 4; k++)
                    p->uv[2 * (v + k) + 1] = row + (k & 1 ? 0 : 0.25f);
            }
        for (int st = 0; st < sb->b.storeys; st++) {     /* the interiors */
            int damaged = 0;
            for (int c = 0; c < BUILDING_CELLS && !damaged; c++)
                damaged = sb->b.cell[st][c].state > 0;
            int worn = sb->b.floor_hp[st] <= sb->skeleton_hp[st] && sb->damaged_skeleton[st] >= 0;
            int gone = building_live_of(&sb->b, st) < 0 || sb->b.state == BUILDING_GONE;
            if (sb->skeleton[st] >= 0) s->instances[sb->skeleton[st]].hidden = !damaged || worn || gone;
            if (sb->damaged_skeleton[st] >= 0) s->instances[sb->damaged_skeleton[st]].hidden = !damaged || !worn || gone;
        }
    }
}

void scene_monster_blend(Scene *s, float seconds)
{
    if (s->monster < 0)
        return;
    Instance *in = &s->instances[s->monster];
    if (!in->pose || !in->held || !in->has_clip || !(seconds > 0)) {
        in->blend_left = 0;
        return;
    }
    memcpy(in->held, in->pose, sizeof *in->held * in->tree.n);
    in->blend_left = in->blend_time = seconds;
}

void scene_monster_blend_step(Scene *s, float dt)
{
    if (s->monster >= 0 && s->instances[s->monster].blend_left > 0)
        s->instances[s->monster].blend_left -= dt;
}

int scene_monster_root(const Scene *s, double seconds, float t[3], float q[4])
{
    if (s->monster < 0)
        return -1;
    const Instance *in = &s->instances[s->monster];
    int found = -1;
    for (uint32_t i = 0; i < in->tree.n; i++) {      /* the first root the clip moves, else the first root */
        const TreeNode *nd = &in->tree.nodes[i];
        if (nd->parent >= 0)
            continue;
        memcpy(t, nd->t, sizeof nd->t);
        memcpy(q, nd->q, sizeof nd->q);
        if (in->has_clip && clip_pose(&in->clip, seconds, i, t, q))
            return 0;
        if (found < 0)
            found = (int)i;
    }
    if (found < 0)
        return -1;
    memcpy(t, in->tree.nodes[found].t, sizeof in->tree.nodes[found].t);
    memcpy(q, in->tree.nodes[found].q, sizeof in->tree.nodes[found].q);
    return 0;
}

void scene_monster_place(Scene *s, float x, float y, float z, float deg)
{
    if (s->monster >= 0)
        s->instances[s->monster].base = mat_trs_z(x, y, z, deg);
}

void scene_monster_root_motion(Scene *s, int axes, const float hold[3], float turn)
{
    if (s->monster < 0)
        return;
    Instance *in = &s->instances[s->monster];
    in->still_root = axes;
    memcpy(in->root_hold, hold, sizeof in->root_hold);
    in->root_turn = turn;
}

int city_camera(City *c, Group *two_tilts)
{
    LoadedBook *lb = load_book(c, "camera");
    if (!lb)
        return -1;
    *two_tilts = book_group(&lb->book, "TwoTilts");
    return two_tilts->at ? 0 : -1;
}

void scene_free(Scene *s)
{
    for (uint32_t i = 0; i < s->nmeshes; i++)
        mesh_free(&s->meshes[i].mesh);
    for (uint32_t i = 0; i < s->nitems; i++)
        if (s->items[i].skin) {
            Skin *sk = s->items[i].skin;
            free(sk->bones); free(sk->weights); free(sk->pos); free(sk->nrm); free(sk);
        }
    for (uint32_t i = 0; i < s->ninstances; i++) {
        tree_free(&s->instances[i].tree);
        if (s->instances[i].has_clip)
            clip_free(&s->instances[i].clip);
        free(s->instances[i].node);
        free(s->instances[i].pose);
        free(s->instances[i].held);
    }
    for (int i = 0; i < s->nown; i++) {
        bdp_free(s->own[i]);
        free(s->own[i]);
    }
    free(s->meshes);
    free(s->mesh_of_entry);
    free(s->instances);
    free(s->items);
    free(s->vehicles);
    for (uint32_t i = 0; i < s->nbuildings; i++)
        free(s->buildings[i].rest);
    free(s->buildings);
    memset(s, 0, sizeof *s);
}
