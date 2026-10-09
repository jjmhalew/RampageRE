/* city.c - a city block from the property books, as tools/city.py builds it (docs/FORMAT_BDP.md "Assembling a block",
 * "Spawns", "Lighting"):
 *   <City>_Props: WorldData (BlockData.BlockList, BlockDef|<block>, LightingData / LightingDataNight, the template books)
 *   <City>_buildings: building templates (ShapeSet|GroundFloorShapes, TopFloorShapes, TextureSheet, Floor|)
 *   Objects1_Props (ObjectTemplateBook): object and actor templates
 *   <City>.bdp: the models (node trees with their meshes) and textures
 * A building's ground floor is 6 high and each storey above it 3; the storeys have no mesh, their facade is made from the
 * template's 4 x 4 texture sheet (column = window style, row = damage). Traffic runs on the two lanes of 0x332D90. */
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

const Bundle *city_bundle(City *c) { return &c->bundle; }

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

static uint32_t scene_mesh_of(Scene *s, uint32_t entry)
{
    if (s->mesh_of_entry[entry] < 0) {
        if (s->nmeshes == s->meshes_cap) {
            s->meshes_cap = s->meshes_cap ? 2 * s->meshes_cap : 256;
            s->meshes = realloc(s->meshes, s->meshes_cap * sizeof *s->meshes);
        }
        mesh_read(&s->city->entries[entry], &s->meshes[s->nmeshes].mesh);
        s->mesh_of_entry[entry] = (int32_t)s->nmeshes++;
    }
    return (uint32_t)s->mesh_of_entry[entry];
}

static uint32_t scene_add_mesh(Scene *s, Mesh m)
{
    if (s->nmeshes == s->meshes_cap) {
        s->meshes_cap = s->meshes_cap ? 2 * s->meshes_cap : 256;
        s->meshes = realloc(s->meshes, s->meshes_cap * sizeof *s->meshes);
    }
    s->meshes[s->nmeshes].mesh = m;
    return s->nmeshes++;
}

static void push_item(Scene *s, uint32_t mesh, Mat4 world, int vehicle)
{
    if (s->nitems == s->items_cap) {
        s->items_cap = s->items_cap ? 2 * s->items_cap : 1024;
        s->items = realloc(s->items, s->items_cap * sizeof *s->items);
    }
    DrawItem *it = &s->items[s->nitems++];
    it->mesh = mesh;
    it->world = world;
    it->local = world;
    it->vehicle = vehicle;
}

static const BdpEntry *find_entry(const Bundle *b, int type, const char *name)
{
    for (uint32_t i = 0; i < b->nentries; i++)
        if (b->entries[i].type == type && _stricmp(bdp_name(b, b->entries[i].name), name) == 0)
            return &b->entries[i];
    return NULL;
}

static const BdpEntry *find_rawanim(const Bundle *b, const char *name)
{
    for (uint32_t i = 0; i < b->nentries; i++) {
        const BdpEntry *e = &b->entries[i];
        if (e->type == ET_ANIM && e->size >= 0x2C && rd32(e->data + 0x20) == 1 &&
            _stricmp(bdp_name(b, rd16(e->data + 0x28)), name) == 0)
            return e;
    }
    return NULL;
}

/* place model `name` (with or without .pwk): each mesh object on its node of the model's tree, at the tree's rest pose */
static int place_model(City *c, Scene *s, const char *name, Mat4 parent, int vehicle)
{
    char key[64];
    size_t n = 0;
    for (; name && name[n] && n < 63; n++)
        key[n] = (char)tolower((unsigned char)name[n]);
    key[n] = 0;
    if (n > 4 && strcmp(key + n - 4, ".pwk") == 0)
        key[n - 4] = 0;
    Tree tree = { 0 };
    Mat4 *node_world = NULL;
    const BdpEntry *te = find_entry(&c->bundle, ET_TREE, key);
    if (te) {
        tree_read(&c->bundle, te, find_rawanim(&c->bundle, key), &tree);
        node_world = malloc(sizeof *node_world * (tree.n ? tree.n : 1));
        for (uint32_t i = 0; i < tree.n; i++) {
            Mat4 local = mat_tq(tree.nodes[i].t, tree.nodes[i].q);
            int p = tree.nodes[i].parent;
            node_world[i] = mat_mul(p >= 0 && (uint32_t)p < i ? node_world[p] : parent, local);
        }
    }
    int placed = 0;
    for (uint32_t i = 0; i < c->bundle.nentries; i++) {
        if (!c->model_key[i][0] || strcmp(c->model_key[i], key) != 0)
            continue;
        const char *obj = strchr(bdp_name(&c->bundle, c->bundle.entries[i].name), '|');
        Mat4 world = parent;
        for (uint32_t k = 0; obj && k < tree.n; k++)
            if (_stricmp(tree.nodes[k].name, obj + 1) == 0) {
                world = node_world[k];
                break;
            }
        push_item(s, scene_mesh_of(s, i), world, vehicle);
        placed++;
    }
    free(node_world);
    tree_free(&tree);
    if (!placed)
        s->missing++;
    return placed;
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

static const BdpEntry *find_clip(const Bundle *b, const char *name)
{
    for (uint32_t i = 0; i < b->nentries; i++) {
        const BdpEntry *e = &b->entries[i];
        if (e->type == ET_ANIM && e->size >= 0x2C && rd32(e->data + 0x20) == 2 &&
            _stricmp(bdp_name(b, e->name), name) == 0)
            return e;
    }
    return NULL;
}

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
    const char *shape = look.at ? group_str(look, "Shape|ShapeName") : NULL;
    if (!shape) shape = group_str(t, "Shape|ShapeName");
    if (!shape) shape = group_str(t, "Shape|Shape");
    if (!shape && look.at) {                      /* the clip's own node tree */
        const char *clip = group_str(look, "Anim|AnimName");
        const BdpEntry *e = clip ? find_clip(&c->bundle, clip) : NULL;
        shape = e ? bdp_name(&c->bundle, rd16(e->data + 0x28)) : NULL;
    }
    float xyz[3];
    location(inst, xyz);
    if (shape && place_model(c, s, shape, mat_trs_z(xyz[0], xyz[1], xyz[2], group_num(inst, "ZRotation", 0)), -1))
        s->people++;
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
            if (shape && place_model(c, s, shape, mat_identity(), (int)s->nvehicles))
                s->nvehicles++;
        }
    }
}

float vehicle_x(const Vehicle *v, double t)
{
    double f = fmod(t * TRAFFIC_SPEED / (2 * LANE_END) + v->phase, 1.0);
    return v->direction * (float)(-LANE_END + 2 * LANE_END * f);
}

/* the generated upper storeys: one quad per facade cell, its tile out of the 4 x 4 sheet */
static Mesh facade(uint32_t sheet, int width, int depth, const float *z, Record (*tiles)[64], const uint32_t *ntiles,
                   Group *floors, int nstoreys)
{
    Mesh m = { 0 };
    int cells = width + 2 * depth;
    uint32_t nq = (uint32_t)(cells * nstoreys);
    m.parts = calloc(1, sizeof *m.parts);
    m.nparts = 1;
    MeshPart *p = m.parts;
    p->material = sheet;
    p->pos = malloc(sizeof(float) * 12 * nq);
    p->nrm = malloc(sizeof(float) * 12 * nq);
    p->uv = malloc(sizeof(float) * 8 * nq);
    p->tris = malloc(sizeof(uint32_t) * 6 * nq);
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
    (void)floors;
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
    const char *ground = str_item(group_sub(t, "ShapeSet|GroundFloorShapes"), 0);
    if (ground)
        place_model(c, s, ground, node, -1);
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
    int nst = 0;
    for (uint32_t i = 1; i < nfloors; i++) {
        z[nst] = GROUND_FLOOR + STOREY * (float)(i - 1);
        ntiles[nst] = group_items(floors[i], tiles[nst], 64);
        nst++;
    }
    const char *sheet = group_str(t, "TextureSheet");
    int sheet_name = sheet ? bdp_find_name(&c->bundle, sheet) : -1;
    if (nst && sheet_name >= 0)
        push_item(s, scene_add_mesh(s, facade((uint32_t)sheet_name, width, depth, z, tiles, ntiles, floors, nst)), node, -1);
    const char *top = str_item(group_sub(t, "ShapeSet|TopFloorShapes"), 0);
    if (top)
        place_model(c, s, top, mat_mul(node, mat_trs_z(0, 0, GROUND_FLOOR + STOREY * (float)(nfloors ? nfloors - 1 : 0), 0)), -1);
    Record obj[MAX_REC];
    uint32_t no = group_values(inst, "ObjectInstance|", &where, obj, MAX_REC);
    for (uint32_t i = 0; i < no; i++)
        place_object(c, s, record_group(where, obj[i]), node);
}

static void read_lights(Scene *s, const char *text, uint32_t len)
{
    char line[256], key[64];
    float v[8] = { 0 };
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

int city_build(City *c, const char *block_name, int night, Scene *s)
{
    memset(s, 0, sizeof *s);
    s->city = &c->bundle;
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
    return 0;
}

void scene_free(Scene *s)
{
    for (uint32_t i = 0; i < s->nmeshes; i++)
        mesh_free(&s->meshes[i].mesh);
    free(s->meshes);
    free(s->mesh_of_entry);
    free(s->items);
    free(s->vehicles);
    memset(s, 0, sizeof *s);
}
