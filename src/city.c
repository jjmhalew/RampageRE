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
#include "ik.h"
#include "rail.h"

#define CELL 5.0f
#define GROUND_FLOOR 6.0f
#define STOREY 3.0f
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
    if (data && bdp_load(&lb->b, data, size) == 0 && book_open(&lb->book, &lb->b) == 0) {
        lb->ok = 1;
        lb->b.scope = c->name;                           /* texture packs: dumped under the city (texpack.c) */
    }
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
    c->bundle.scope = c->name;
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

int city_block_index(City *c, const char *name)
{
    Group list = group_sub(group_sub(c->world, "BlockData"), "BlockList");
    Record r[MAX_REC];
    uint32_t n = group_items(list, r, MAX_REC);
    int start = (int)group_num(c->world, "BlockStart", 0), first = -1;
    for (uint32_t i = 0; i < n && name; i++)
        if (_stricmp(record_str(list, r[i]), name) == 0) {
            if ((int)i >= start)
                return (int)i;
            if (first < 0)
                first = (int)i;
        }
    return first;
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

/* an object's flags (ClothBit| groups, the ClothObject 0x3473B0): ClothRef|Name a template whose Shape is the flag's
 * model, its cloth (<model>|cloth) simulated and drawn through the model's own mesh (one part per cloth part, each
 * vertex on its particle: cloth_mesh_part), at the object x ClothOffset (rows, then the place), in its wind */
static Skin *new_skin(const Mesh *m);

static void place_flags(City *c, Scene *s, Group t, Mat4 base)
{
    Group where;
    Record r[8];
    uint32_t n = group_values(t, "ClothBit|", &where, r, 8);
    for (uint32_t i = 0; i < n; i++) {
        Group cb = record_group(where, r[i]), ref = template_of(c, group_str(cb, "ClothRef|Name"));
        const char *model = ref.at ? group_str(ref, "Shape|Shape") : NULL;
        if (!model)
            continue;
        char key[96];
        snprintf(key, sizeof key, "%s|cloth", model);
        const BdpEntry *ce = find_entry(&c->bundle, ET_CLOTH, key), *me = NULL;
        size_t ml = strlen(model);
        for (uint32_t k = 0; k < c->bundle.nentries && !me; k++) {   /* the model's mesh: <model>|<object> */
            const char *en = bdp_name(&c->bundle, c->bundle.entries[k].name);
            if (c->bundle.entries[k].type == ET_MESH && !_strnicmp(en, model, ml) && en[ml] == '|')
                me = &c->bundle.entries[k];
        }
        Cloth *cl = ce ? cloth_new(ce->data, ce->size) : NULL;
        if (!cl || !me) {
            if (cl) cloth_free(cl);
            continue;
        }
        Mesh m = { 0 };
        int np = cloth_mesh_parts(me->data, me->size), nv = 0;
        m.parts = calloc((size_t)(np > 0 ? np : 1), sizeof *m.parts);
        ClothMeshPart *cp = calloc((size_t)(np > 0 ? np : 1), sizeof *cp);
        for (int k = 0; k < np; k++) {
            if (cloth_mesh_part(cl, k, me->data, me->size, me->data2, me->size2, &cp[k]) != 0)
                continue;
            MeshPart *p = &m.parts[m.nparts++];
            p->material = cp[k].material;
            p->nverts = (uint32_t)cp[k].nverts;
            p->pos = calloc((size_t)cp[k].nverts * 3 + 1, sizeof(float));
            p->nrm = calloc((size_t)cp[k].nverts * 3 + 1, sizeof(float));
            if (cp[k].uv) {
                p->uv = malloc(sizeof(float) * 2 * (size_t)cp[k].nverts + 1);
                memcpy(p->uv, cp[k].uv, sizeof(float) * 2 * (size_t)cp[k].nverts);
            }
            p->ntris = (uint32_t)cp[k].ntris;
            p->tris = malloc(sizeof(uint32_t) * 3 * (size_t)cp[k].ntris + 4);
            memcpy(p->tris, cp[k].tris, sizeof(uint32_t) * 3 * (size_t)cp[k].ntris);
            nv += cp[k].nverts;
        }
        if (!m.nparts) {
            free(m.parts);
            free(cp);
            cloth_free(cl);
            continue;
        }
        Mat4 off = mat_identity();
        float rows[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
        Group og = group_sub(cb, "ClothOffset");
        Record orr[12];
        if (og.at && group_items(og, orr, 12) == 12)
            for (int k = 0; k < 12; k++)
                rows[k] = record_num(orr[k]);
        for (int a = 0; a < 3; a++)
            for (int b = 0; b < 3; b++)
                off.m[4 * a + b] = rows[3 * a + b];
        for (int b = 0; b < 3; b++)
            off.m[12 + b] = rows[9 + b];
        int id = new_instance(s, mat_mul(base, off), -1);
        s->instances[id].dynamic = 1;
        Skin *sk = new_skin(&m);
        sk->cloth = cl;
        sk->particle = malloc(sizeof *sk->particle * (size_t)(nv ? nv : 1));
        int at = 0;
        for (int k = 0; k < np; k++)
            for (int v = 0; v < cp[k].nverts; v++)
                sk->particle[at++] = cp[k].particle ? cp[k].particle[v] : -1;
        for (int k = 0; k < np; k++)
            cloth_mesh_part_free(&cp[k]);
        free(cp);
        sk->has_wind = 1;
        sk->wind.dir_freq = group_num(cb, "HorizontalWindDirFreq", 1);
        sk->wind.force = group_num(cb, "HorizontalWindForce", 10);
        sk->wind.force_freq = group_num(cb, "HorizontalWindForceFreq", 3);
        sk->wind.min_angle = group_num(cb, "HorizontalWindMinAngle", 0);
        sk->wind.max_angle = group_num(cb, "HorizontalWindMaxAngle", 6.28f);
        sk->wind.vert_freq = group_num(cb, "VerticalWindFreq", 2);
        sk->wind.vert_force = group_num(cb, "VerticalWindForce", 10);
        sk->wind.time_offset = group_num(cb, "TimeOffset", 0);
        push_item(s, scene_add_mesh(s, m, s->city), id, -1, sk);
        if (getenv("RAMPAGE_TRACE"))
            printf("flag %s at %.1f %.1f %.1f\n", model, s->instances[id].base.m[12], s->instances[id].base.m[13],
                   s->instances[id].base.m[14]);
    }
}

static void place_object(City *c, Scene *s, Group inst, Mat4 parent)
{
    Group t = template_of(c, group_str(inst, "Name"));
    const char *shape = t.at ? group_str(t, "Shape|Shape") : NULL, *script = t.at ? group_str(t, "Script|ScriptName") : NULL;
    float xyz[3];
    location(inst, xyz);
    Mat4 at = mat_mul(parent, mat_trs_z(xyz[0], xyz[1], xyz[2], group_num(inst, "ZRotation", 0)));
    if (s->placing_building >= 0 && script && !_strnicmp(script, "Rocket.", 7)) {
        if (s->nrockets < 16) {                      /* a roof rocket: vehicle.c makes it, its Base and BasePipe */
            s->rockets[s->nrockets].name = group_str(inst, "Name");
            for (int k = 0; k < 3; k++)
                s->rockets[s->nrockets].pos[k] = at.m[12 + k];
            s->rockets[s->nrockets].facing = group_num(inst, "ZRotation", 0) / 57.2957795f;
            s->rockets[s->nrockets++].building = s->placing_building;
        }
        return;
    }
    if (t.at && boss_is_boss_template(t))           /* a boss's part (the NY generators): boss.c's */
        return;
    if (s->placing_block && parade_script_ours(script))   /* a balloon or a food cart: parade.c's (main) */
        return;
    int scripted = -1;                               /* a band or a guard's hut: for people.c, once it exists */
    if (s->placing_block && script && s->nscripted < 8 &&
        (!_strnicmp(script, "Band.", 5) || !_strnicmp(script, "RoyalGuardHut.", 14))) {
        scripted = s->nscripted++;
        memset(&s->scripted[scripted], 0, sizeof s->scripted[scripted]);
        s->scripted[scripted].t = t;
        s->scripted[scripted].hut = !_strnicmp(script, "RoyalGuardHut.", 14);
        s->scripted[scripted].hut_id = s->scripted[scripted].instance = -1;
        for (int k = 0; k < 3; k++)
            s->scripted[scripted].pos[k] = at.m[12 + k];
    }
    if (!shape) {
        if (scripted < 0)
            s->missing++;
        return;
    }
    int id = place_model(c, s, shape, at, -1);
    if (id >= 0 && t.at)
        s->instances[id].shadow = group_sub(t, "ShadowData");
    if (scripted >= 0)
        s->scripted[scripted].instance = id;
    if (id >= 0)
        place_flags(c, s, t, at);
    if (id >= 0 && script && (s->placing_block || s->placing_building >= 0) && s->nstreet < SCENE_STREET_MAX &&
        (!_stricmp(script, "StreetObject.bsf") || !_stricmp(script, "RoofItem.bsf"))) {
        struct SceneStreetObj *so = &s->street[s->nstreet++];   /* streetobj.c's, with its flags' cloths */
        so->t = t;
        so->inst = inst;
        so->first = so->model = id;
        so->last = (int)s->ninstances;
        so->building = s->placing_building;
        so->nrel = -1;                               /* the cloths' places: taken at its first move */
        memcpy(so->world, at.m, sizeof so->world);
    }
    const char *flags = group_str(t, "Flags");
    if (id >= 0 && flags && strstr(flags, "IsObstacle")) {   /* 0x335ED0: IsObstacle sets object flag 0x40000 */
        s->obstacle_of = realloc(s->obstacle_of, sizeof *s->obstacle_of * (s->nobstacles + 1));
        s->obstacle_of[s->nobstacles++] = id;
    }
}

/* the obstacles' boxes: their models' bounds, placed (once the scene is posed) */
static void obstacle_boxes(Scene *s)
{
    free(s->obstacles);
    s->obstacles = calloc(s->nobstacles ? s->nobstacles : 1, sizeof *s->obstacles);
    for (uint32_t o = 0; o < s->nobstacles; o++) {
        float *box = s->obstacles[o];
        box[0] = box[1] = box[2] = 1e30f;
        box[3] = box[4] = box[5] = -1e30f;
        for (uint32_t i = 0; i < s->nitems; i++) {
            const DrawItem *it = &s->items[i];
            if (it->instance != s->obstacle_of[o])
                continue;
            const Mesh *m = &s->meshes[it->mesh].mesh;
            for (uint32_t p = 0; p < m->nparts; p++)
                for (uint32_t v = 0; v < m->parts[p].nverts; v++) {
                    const float *x = m->parts[p].pos + 3 * v, *w = it->world.m;
                    for (int k = 0; k < 3; k++) {
                        float y = w[k] * x[0] + w[4 + k] * x[1] + w[8 + k] * x[2] + w[12 + k];
                        if (y < box[k]) box[k] = y;
                        if (y > box[3 + k]) box[3 + k] = y;
                    }
                }
        }
    }
}

/* the inverse of a rigid placement (a rotation and a place) */
static Mat4 rigid_inverse(Mat4 m)
{
    Mat4 r = mat_identity();
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 3; k++)
            r.m[c * 4 + k] = m.m[k * 4 + c];
    for (int k = 0; k < 3; k++)
        r.m[12 + k] = -(r.m[k] * m.m[12] + r.m[4 + k] * m.m[13] + r.m[8 + k] * m.m[14]);
    return r;
}

void scene_street_place(Scene *s, int k, const float world[16], int hidden)
{
    if (k < 0 || k >= s->nstreet)
        return;
    struct SceneStreetObj *so = &s->street[k];
    if (so->model < 0 || (uint32_t)so->model >= s->ninstances)
        return;
    if (so->nrel < 0) {                              /* its cloths relative to the model, as they stand now */
        Mat4 inv = rigid_inverse(s->instances[so->first].base);
        so->nrel = 0;
        for (int i = so->first + 1; i < so->last && so->nrel < 4; i++)
            so->rel[so->nrel++] = mat_mul(inv, s->instances[i].base);
    }
    Mat4 w;
    memcpy(w.m, world, sizeof w.m);
    Instance *in = &s->instances[so->model];
    in->dynamic = 1;
    in->base = w;
    in->hidden = hidden;
    if (so->model != so->first)                      /* the model it had before its DamageShape */
        s->instances[so->first].hidden = 1;
    for (int i = 0; i < so->nrel; i++) {
        Instance *c = &s->instances[so->first + 1 + i];
        c->dynamic = 1;
        c->base = mat_mul(w, so->rel[i]);
        c->hidden = hidden;
    }
}

int scene_street_shape(Scene *s, int k, const char *shape)
{
    if (k < 0 || k >= s->nstreet || !s->owner || !shape)
        return -1;
    struct SceneStreetObj *so = &s->street[k];
    if (so->nrel < 0)                                /* the cloths' places before anything moves */
        scene_street_place(s, k, s->instances[so->first].base.m, s->instances[so->first].hidden);
    Mat4 at = s->instances[so->model].base;
    int missing = s->missing;
    int id = place_model(s->owner, s, shape, at, -1);
    s->missing = missing;
    if (id < 0)
        return -1;
    s->instances[id].dynamic = 1;
    s->instances[id].shadow = s->instances[so->model].shadow;
    s->instances[so->model].hidden = 1;
    so->model = id;
    return 0;
}

int scene_instance_node(const Scene *s, int inst, const char *name, int id, float out[16])
{
    if (inst < 0 || (uint32_t)inst >= s->ninstances)
        return -1;
    const Instance *in = &s->instances[inst];
    int k = -1;
    for (uint32_t i = 0; in->node && i < in->tree.n && k < 0; i++)
        if (name ? _stricmp(in->tree.nodes[i].name, name) == 0 : (int)i == id)
            k = (int)i;
    if (k < 0)
        return -1;
    memcpy(out, mat_mul(in->base, in->node[k]).m, sizeof(float) * 16);
    return 0;
}

/* the people: one look (instance) per person and ActorAnimShape|<anim> it has shown */
struct PersonLook { int who; char anim[32]; int inst; };

Group city_book_group(City *c, const char *book, const char *name)
{
    Group none = { NULL, 0 };
    LoadedBook *lb = book && name ? load_book(c, book) : NULL;
    return lb ? book_group(&lb->book, name) : none;
}

int city_shape_box(City *c, const char *shape, float min[3], float max[3])
{
    char key[64];
    size_t n = 0;
    for (; shape && shape[n] && n < 63; n++)
        key[n] = (char)tolower((unsigned char)shape[n]);
    key[n] = 0;
    if (n > 4 && strcmp(key + n - 4, ".pwk") == 0)
        key[n - 4] = 0;
    int found = 0;
    for (int k = 0; k < 3; k++) {
        min[k] = 1e30f;
        max[k] = -1e30f;
    }
    for (uint32_t i = 0; i < c->bundle.nentries; i++) {
        const BdpEntry *e = &c->bundle.entries[i];
        Mesh m;
        if (e->type != ET_MESH || !c->model_key[i][0] || strcmp(c->model_key[i], key) != 0 || mesh_read(e, &m) != 0)
            continue;
        for (uint32_t p = 0; p < m.nparts; p++)
            for (uint32_t v = 0; v < m.parts[p].nverts; v++)
                for (int k = 0; k < 3; k++) {
                    float x = m.parts[p].pos[3 * v + k];
                    if (x < min[k]) min[k] = x;
                    if (x > max[k]) max[k] = x;
                    found = 1;
                }
        mesh_free(&m);
    }
    return found ? 0 : -1;
}

/* the sphere a model keeps (SimpleShape 0x184CB0: centre +0x38, radius +0x44): at load the union (0x103770) of its
 * parts' spheres (0x185AA0), each a type-0 mesh's header sphere (+0x4 the centre, +0x10 r^2) [R] */
int city_shape_sphere(City *c, const char *shape, float centre[3], float *radius)
{
    char key[64];
    size_t n = 0;
    for (; shape && shape[n] && n < 63; n++)
        key[n] = (char)tolower((unsigned char)shape[n]);
    key[n] = 0;
    if (n > 4 && strcmp(key + n - 4, ".pwk") == 0)
        key[n - 4] = 0;
    int found = 0;
    float sc[3] = { 0, 0, 0 }, sr = 0;
    for (uint32_t i = 0; i < c->bundle.nentries; i++) {
        const BdpEntry *e = &c->bundle.entries[i];
        if (e->type != ET_MESH || !c->model_key[i][0] || strcmp(c->model_key[i], key) != 0 || e->size < 0x14)
            continue;
        float b[4];
        memcpy(b, e->data + 4, sizeof b);
        float r = b[3] > 0 ? sqrtf(b[3]) : 0;
        if (!found) {
            memcpy(sc, b, sizeof sc);
            sr = r;
            found = 1;
            continue;
        }
        float d[3] = { b[0] - sc[0], b[1] - sc[1], b[2] - sc[2] }, l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l + r <= sr)                             /* inside the one so far */
            continue;
        if (l + sr <= r) {                           /* holds the one so far */
            memcpy(sc, b, sizeof sc);
            sr = r;
            continue;
        }
        float nr = (l + sr + r) / 2;                 /* the sphere round both */
        for (int k = 0; k < 3; k++)
            sc[k] += d[k] * (nr - sr) / l;
        sr = nr;
    }
    if (!found)
        return -1;
    memcpy(centre, sc, sizeof sc);
    *radius = sr;
    return 0;
}

static Group people_template(void *ctx, const char *name) { return template_of(ctx, name); }
Group city_template(City *c, const char *name) { return template_of(c, name); }
static float people_clip_seconds(void *ctx, const char *anim, const char *shape);
float city_clip_seconds(City *c, const char *anim, const char *shape) { return people_clip_seconds(c, anim, shape); }

PeopleEaten scene_huts_hit(Scene *s, const float centre[3], float radius, const float from[3])
{
    PeopleEaten sum = { 0, 0, 0, 0 };
    for (int k = 0; k < s->nscripted && s->folk; k++) {
        if (!s->scripted[k].hut || s->scripted[k].hut_id < 0 || people_hut_state(s->folk, s->scripted[k].hut_id) != 0)
            continue;
        const float *p = s->scripted[k].pos;
        float d[3] = { p[0] - centre[0], p[1] - centre[1], p[2] + 2 - centre[2] };   /* the hut's middle, 2 up [G] */
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > (radius + 2) * (radius + 2))   /* 2 its reach [G] */
            continue;
        float dir[3] = { from[0] - p[0], from[1] - p[1], 0 }, l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
        if (l > 1e-4f) { dir[0] /= l; dir[1] /= l; }    /* towards the monster: it flies away from it */
        PeopleEaten e = people_hut_hit(s->folk, s->scripted[k].hut_id, dir, s->scripted[k].vel);
        if (getenv("RAMPAGE_TRACE"))
            printf("  a guard's hut hit: %.0f points, flying at %.1f %.1f\n", e.points, s->scripted[k].vel[0],
                   s->scripted[k].vel[1]);
        sum.health += e.health; sum.energy += e.energy; sum.points += e.points;
    }
    return sum;
}

void scene_huts_update(Scene *s, float dt)
{
    for (int k = 0; k < s->nscripted && s->folk; k++) {
        int id = s->scripted[k].instance, st;
        if (!s->scripted[k].hut || id < 0 || s->scripted[k].hut_id < 0)
            continue;
        st = people_hut_state(s->folk, s->scripted[k].hut_id);
        if (st == 1)                                 /* its SphereMover */
            for (int j = 0; j < 3; j++) {
                s->scripted[k].pos[j] += s->scripted[k].vel[j] * dt;
                s->instances[id].base.m[12 + j] += s->scripted[k].vel[j] * dt;
            }
        else if (st == 2)
            s->instances[id].hidden = 1;
    }
}

static float people_clip_seconds(void *ctx, const char *anim, const char *shape)
{
    City *c = ctx;
    const BdpEntry *ce = anim ? find_anim(&c->bundle, 2, anim) : NULL;
    if (!ce && shape)
        ce = find_anim(&c->bundle, 1, shape);        /* the shape's own RawAnim */
    Clip clip;
    if (!ce || clip_read(ce, &clip) != 0)
        return 0;
    float d = clip.duration;
    clip_free(&clip);
    return d;
}

/* a person's look for its anim now: the model (Shape|ShapeName, else the template's, else the clip's own tree) and
 * its clip, as place_actor did; -1 none */
static int person_look(City *c, Scene *s, int who, const Person *p)
{
    for (int i = 0; i < s->nlooks; i++)
        if (s->looks[i].who == who && strcmp(s->looks[i].anim, p->anim) == 0)
            return s->looks[i].inst;
    char key[96];
    snprintf(key, sizeof key, "ActorAnimShape|%s", p->anim);
    Group look = group_sub(p->tmpl, key);
    const char *clip_name = look.at ? group_str(look, "Anim|AnimName") : NULL;
    const BdpEntry *ce = clip_name ? find_anim(&c->bundle, 2, clip_name) : NULL;
    const char *shape = look.at ? group_str(look, "Shape|ShapeName") : NULL;
    if (!shape) shape = group_str(p->tmpl, "Shape|ShapeName");
    if (!shape) shape = group_str(p->tmpl, "Shape|Shape");
    if (!shape && ce)
        shape = bdp_name(&c->bundle, rd16(ce->data + 0x28));
    int missing = s->missing;
    int id = shape ? place_model(c, s, shape, mat_identity(), -1) : -1;
    s->missing = missing;
    if (id >= 0) {
        Instance *in = &s->instances[id];
        in->dynamic = 1;
        in->shadow = group_sub(p->tmpl, "ShadowData");   /* DefActorProps': shadow_blob */
        Clip clip;
        if (ce && clip_read(ce, &clip) == 0) {
            if (clip.bones == in->tree.n) {
                if (in->has_clip)
                    clip_free(&in->clip);
                in->clip = clip;
                in->has_clip = 1;
            } else {
                clip_free(&clip);
            }
        }
    }
    s->looks = realloc(s->looks, sizeof *s->looks * (size_t)(s->nlooks + 1));
    struct PersonLook *l = &s->looks[s->nlooks++];
    l->who = who;
    snprintf(l->anim, sizeof l->anim, "%s", p->anim);
    l->inst = id;
    return id;
}

/* an army unit's look: its shape, its clip (NULL: the shape's own RawAnim, which place_model takes), keyed by both */
static int unit_look(City *c, Scene *s, int who, const ArmyUnit *u)
{
    char key[32];
    snprintf(key, sizeof key, "%.15s|%.15s", u->shape ? u->shape : "-", u->clip ? u->clip : "");
    for (int i = 0; i < s->nlooks; i++)
        if (s->looks[i].who == who && strcmp(s->looks[i].anim, key) == 0)
            return s->looks[i].inst;
    int missing = s->missing;
    int id = u->shape ? place_model(c, s, u->shape, mat_identity(), -1) : -1;
    s->missing = missing;
    if (id >= 0) {
        Instance *in = &s->instances[id];
        in->dynamic = 1;
        if (u->tmpl.at)
            in->shadow = group_sub(u->tmpl, "ShadowData");
        const BdpEntry *ce = u->clip ? find_anim(&c->bundle, 2, u->clip) : NULL;
        Clip clip;
        if (ce && clip_read(ce, &clip) == 0) {
            if (clip.bones == in->tree.n) {
                if (in->has_clip)
                    clip_free(&in->clip);
                in->clip = clip;
                in->has_clip = 1;
            } else {
                clip_free(&clip);
            }
        }
    }
    s->looks = realloc(s->looks, sizeof *s->looks * (size_t)(s->nlooks + 1));
    struct PersonLook *l = &s->looks[s->nlooks++];
    l->who = who;
    snprintf(l->anim, sizeof l->anim, "%s", key);
    l->inst = id;
    return id;
}

static int ends_with(const char *s, const char *tail);

int scene_unit_node(const Scene *s, int unit, const char *node, float out[16])
{
    for (int i = 0; i < s->nlooks; i++) {
        const struct PersonLook *l = &s->looks[i];
        if (l->who != (1 << 20) + unit || l->inst < 0 || s->instances[l->inst].hidden)
            continue;
        const Instance *in = &s->instances[l->inst];
        for (uint32_t k = 0; k < in->tree.n && in->node; k++)
            if (ends_with(in->tree.nodes[k].name, node)) {
                Mat4 w = mat_mul(in->base, in->node[k]);
                memcpy(out, w.m, sizeof w.m);
                return 0;
            }
    }
    return -1;
}

/* a vehicle's look (vehicle.c): its shape now (the DamageShape once hit), keyed by the object and the shape */
static int vehicle_look(City *c, Scene *s, int who, const char *shape)
{
    for (int i = 0; i < s->nlooks; i++)
        if (s->looks[i].who == who && strcmp(s->looks[i].anim, shape) == 0)
            return s->looks[i].inst;
    int missing = s->missing;
    int id = place_model(c, s, shape, mat_identity(), -1);
    s->missing = missing;
    if (id >= 0)
        s->instances[id].dynamic = 1;
    s->looks = realloc(s->looks, sizeof *s->looks * (size_t)(s->nlooks + 1));
    struct PersonLook *l = &s->looks[s->nlooks++];
    l->who = who;
    snprintf(l->anim, sizeof l->anim, "%s", shape);
    l->inst = id;
    return id;
}

/* each frame: every revealed edible at its matrix (looks keyed from 3 << 20); an InvisShape one is its effect alone */
static void edible_sync(Scene *s)
{
    if (!s->edibles || !s->owner)
        return;
    for (int i = 0, n = edible_count(s->edibles); i < n; i++) {
        const Edible *e = edible_get(s->edibles, i);
        int who = (3 << 20) + i;
        int show = e->phase != EP_GONE && e->shape ? vehicle_look(s->owner, s, who, e->shape) : -1;
        for (int k = 0; k < s->nlooks; k++)
            if (s->looks[k].who == who && s->looks[k].inst >= 0 && s->looks[k].inst != show)
                s->instances[s->looks[k].inst].hidden = 1;
        if (show >= 0) {
            s->instances[show].hidden = 0;
            memcpy(s->instances[show].base.m, e->world, sizeof e->world);
        }
    }
}

/* each frame: every boss part at its matrix x RenderScale, an actor's clip at its time (looks keyed from 4 << 20) */
static void boss_sync(Scene *s, double t)
{
    if (!s->boss || !s->owner)
        return;
    for (int i = 0, n = boss_count(s->boss); i < n; i++) {
        const BossPart *p = boss_part(s->boss, i);
        int who = (4 << 20) + i;
        int show = -1;
        if (p->alive && p->shape && _stricmp(p->shape, "InvisShape") != 0) {
            ArmyUnit look;                           /* the army's look: a shape and its clip */
            memset(&look, 0, sizeof look);
            look.shape = p->shape;
            look.clip = p->clip;
            show = unit_look(s->owner, s, who, &look);
        }
        for (int k = 0; k < s->nlooks; k++)
            if (s->looks[k].who == who && s->looks[k].inst >= 0 && s->looks[k].inst != show)
                s->instances[s->looks[k].inst].hidden = 1;
        if (show < 0)
            continue;
        Instance *in = &s->instances[show];
        in->hidden = 0;
        memcpy(in->base.m, p->world, sizeof in->base.m);
        float k = p->scale > 0 ? p->scale : 1;
        for (int c = 0; c < 12; c++)
            in->base.m[c] *= k;
        if (in->has_clip && p->anim_len > 0 && p->clip)
            in->offset = (float)(p->anim_time / p->anim_len * in->clip.duration - t);
        else if (in->has_clip && !p->clip && !p->animate)   /* [R 0x309CE0] the model's own animation runs only with */
            in->offset = (float)-t;                  /* flag 0x8000000; else it stays at its start */
    }
}

/* each frame: every balloon, biker, bike and food cart at its matrix x RenderScale, an actor's clip at its time (looks
 * keyed from 5 << 20) */
static void parade_sync(Scene *s, double t)
{
    if (!s->parade || !s->owner)
        return;
    for (int i = 0, n = parade_count(s->parade); i < n; i++) {
        const ParadeObj *o = parade_obj(s->parade, i);
        int who = (5 << 20) + i, show = -1;
        if (o->phase != PAP_GONE && o->shape) {
            ArmyUnit look;                           /* the army's look: a shape and its clip */
            memset(&look, 0, sizeof look);
            look.shape = o->shape;
            look.clip = o->clip;
            look.tmpl = o->tmpl;
            show = unit_look(s->owner, s, who, &look);
        }
        for (int k = 0; k < s->nlooks; k++)
            if (s->looks[k].who == who && s->looks[k].inst >= 0 && s->looks[k].inst != show)
                s->instances[s->looks[k].inst].hidden = 1;
        if (show < 0)
            continue;
        Instance *in = &s->instances[show];
        in->hidden = 0;
        memcpy(in->base.m, o->world, sizeof in->base.m);
        for (int c = 0; c < 12; c++)
            if ((c & 3) != 3)
                in->base.m[c] *= o->scale > 0 ? o->scale : 1;
        if (in->has_clip && o->anim_len > 0 && o->clip)
            in->offset = (float)(o->anim_time / o->anim_len * in->clip.duration - t);
    }
}

/* each frame: every vehicle at its matrix (looks keyed from 2 << 20) */
static void vehicle_sync(Scene *s)
{
    if (!s->traffic || !s->owner)
        return;
    for (int i = 0, n = vehicle_count(s->traffic); i < n; i++) {
        const VehObj *o = vehicle_obj(s->traffic, i);
        int who = (2 << 20) + i;
        int show = o->phase != VP_GONE && !o->hidden && o->shape ? vehicle_look(s->owner, s, who, o->shape) : -1;
        for (int k = 0; k < s->nlooks; k++)
            if (s->looks[k].who == who && s->looks[k].inst >= 0 && s->looks[k].inst != show)
                s->instances[s->looks[k].inst].hidden = 1;
        if (show >= 0) {
            s->instances[show].hidden = 0;
            memcpy(s->instances[show].base.m, o->world, sizeof o->world);
            if (o->tmpl.at && !s->instances[show].shadow.at)
                s->instances[show].shadow = group_sub(o->tmpl, "ShadowData");
        }
    }
}

/* each frame: every rail object at its matrix (looks keyed from 6 << 20) */
static void rail_sync(Scene *s)
{
    if (!s->rail || !s->owner)
        return;
    for (int i = 0, n = rail_count(s->rail); i < n; i++) {
        const RailObj *o = rail_obj(s->rail, i);
        int who = (6 << 20) + i;
        int show = o->phase != RP_GONE && !o->hidden && o->shape ? vehicle_look(s->owner, s, who, o->shape) : -1;
        for (int k = 0; k < s->nlooks; k++)
            if (s->looks[k].who == who && s->looks[k].inst >= 0 && s->looks[k].inst != show)
                s->instances[s->looks[k].inst].hidden = 1;
        if (show >= 0) {
            s->instances[show].hidden = 0;
            memcpy(s->instances[show].base.m, o->world, sizeof o->world);
            if (o->tmpl.at && !s->instances[show].shadow.at)
                s->instances[show].shadow = group_sub(o->tmpl, "ShadowData");
        }
    }
}

/* each frame: every army unit at its place, its clip at anim_time / anim_len of the clip (looks keyed from 1 << 20) */
static void army_sync(Scene *s, double t)
{
    if (!s->army || !s->owner)
        return;
    for (int i = 0, n = army_count(s->army); i < n; i++) {
        const ArmyUnit *u = army_unit(s->army, i);
        int who = (1 << 20) + i;
        int show = u->phase != AU_GONE ? unit_look(s->owner, s, who, u) : -1;
        for (int k = 0; k < s->nlooks; k++)
            if (s->looks[k].who == who && s->looks[k].inst >= 0 && s->looks[k].inst != show)
                s->instances[s->looks[k].inst].hidden = 1;
        if (show < 0)
            continue;
        Instance *in = &s->instances[show];
        in->hidden = 0;
        memcpy(in->base.m, u->world, sizeof in->base.m);
        if (in->has_clip && u->anim_len > 0 && u->clip)
            in->offset = (float)(u->anim_time / u->anim_len * in->clip.duration - t);
    }
}

/* each frame: every person's look where people.c has it (its world matrix, the template's RenderScale), its clip at
 * anim_time / anim_len of the clip; the other looks hidden */
static void people_sync(Scene *s, double t)
{
    if (!s->folk || !s->owner)
        return;
    int n = people_count(s->folk);
    for (int who = 0; who < n; who++) {
        const Person *p = people_get(s->folk, who);
        int show = p->phase != PP_GONE ? person_look(s->owner, s, who, p) : -1;
        for (int i = 0; i < s->nlooks; i++)
            if (s->looks[i].who == who && s->looks[i].inst >= 0 && s->looks[i].inst != show)
                s->instances[s->looks[i].inst].hidden = 1;
        if (show < 0)
            continue;
        Instance *in = &s->instances[show];
        in->hidden = 0;
        float rs = group_num(p->tmpl, "RenderScale", 1);
        Mat4 m;
        memcpy(m.m, p->world, sizeof m.m);
        for (int k = 0; k < 12; k++)
            if ((k & 3) != 3)
                m.m[k] *= rs;
        in->base = m;
        if (in->has_clip && p->anim_len > 0)
            in->offset = (float)(p->anim_time / p->anim_len * in->clip.duration - t);
    }
}

static uint32_t spawn_seed = 12345;
static float spawn_rand(void)                        /* the LCG of the stand-ins, 0 .. 1 */
{
    spawn_seed = spawn_seed * 1664525u + 1013904223u;
    return (float)(spawn_seed >> 8) / 16777216.0f;
}

void scene_people_spawn(Scene *s, float dt)
{
    if (!s->folk || (!s->noccupants && !s->nmilitary))
        return;
    if (!s->slots_made) {
        /* the windows' slots (0x31A540 -> 0x3196F0 for every building): of a building's n windows (here: its front
         * cells with an outline [G]), round(n x FillRate) in a shuffled order, the first total - int(total x
         * NonMilitaryRate) of them soldiers, the rest civilians; the building's NumNonMilitaryOccupants (then all n,
         * that many civilians) and NumMilitaryOccupants override the counts, each at most n. A slot's occupant: one of
         * the kinds of its sort (the game: a random one of a list 0x318B40 makes from the instances; here by their
         * Frequency [G]) */
        s->slots_made = 1;
        float civ_total = 0, mil_total = 0;
        for (int k = 0; k < s->noccupants; k++)
            civ_total += s->occupant_freq[k];
        for (int k = 0; k < s->nmilitary; k++)
            mil_total += s->military_freq[k];
        for (uint32_t bi = 0; bi < s->nbuildings; bi++) {
            const SceneBuilding *sb = &s->buildings[bi];
            int win[512][3], n = 0;
            /* 0x2D01F0: live storeys 1 .. standing - 1 (not the ground), the front cells with an Openable window:
             * one of the column's two (LRPos), the first there is from a random one */
            for (int o = 0; o < sb->b.storeys; o++)
                for (int col = 0; col < sb->b.width && n < 512; col++) {
                    int ci = building_cell_index(&sb->b, FACE_FRONT, col), ls = building_live_of(&sb->b, o);
                    if (ci < 0 || ls < 1 || ls > sb->b.n - 1)
                        continue;
                    int tile = sb->b.cell[o][ci].tile & 3, lr = spawn_rand() < 0.5f ? 0 : 1;
                    if (sb->openable[tile][lr][0] < 0)
                        lr ^= 1;
                    if (sb->openable[tile][lr][0] < 0)
                        continue;
                    win[n][0] = o;
                    win[n][1] = col;
                    win[n++][2] = lr;
                }
            for (int k = 0; k < n; k++) {             /* each one swapped with a random one */
                int j = (int)(spawn_rand() * (float)n) % n, t[3];
                memcpy(t, win[k], sizeof t);
                memcpy(win[k], win[j], sizeof t);
                memcpy(win[j], t, sizeof t);
            }
            int total = (int)(0.5f + (float)n * s->fill_rate);
            int military = total - (int)((float)total * s->nonmilitary_rate);
            float civ = group_num(sb->props, "NumNonMilitaryOccupants", -1);
            float mil = group_num(sb->props, "NumMilitaryOccupants", -1);
            if (civ >= 0) {
                int c = (int)civ < n ? (int)civ : n;
                military = n - c;
                total = n;
            }
            if (mil >= 0)
                military = (int)mil < n ? (int)mil : n;
            if (military > total)
                military = total;
            if (!s->nmilitary)
                military = 0;
            for (int k = 0; k < total; k++) {
                int is_mil = k < military, kinds = is_mil ? s->nmilitary : s->noccupants;
                if (!kinds)
                    continue;
                const float *freq = is_mil ? s->military_freq : s->occupant_freq;
                float pick = spawn_rand() * (is_mil ? mil_total : civ_total);
                int q = 0;
                while (q + 1 < kinds && pick >= freq[q]) { pick -= freq[q]; q++; }
                s->slots = realloc(s->slots, sizeof *s->slots * (size_t)(s->nslots + 1));
                struct WindowSlot ws = { (int)bi, win[k][0], win[k][1], q, is_mil, -1, win[k][2] };
                s->slots[s->nslots++] = ws;
            }
        }
        if (getenv("RAMPAGE_TRACE")) {
            int nm = 0;
            for (int k = 0; k < s->nslots; k++)
                nm += s->slots[k].military;
            printf("window slots: %d civilian, %d military\n", s->nslots - nm, nm);
        }
    }
    s->spawn_clock += dt;
    s->spawn_timer += dt;                            /* +0x30 against SpawnRate (+0x44) */
    if (s->spawn_timer < s->spawn_rate || !s->nslots)
        return;
    s->spawn_timer = 0;
    /* a soldier with the chance MilitaryStartRate + min(1, t / MilitaryTimeRange) x (End - Start), else a civilian
     * (0x318650; t the game clock 0x347DC0, here the block's time) */
    float f = s->military_range > 0 ? s->spawn_clock / s->military_range : 1;
    int want_mil = s->army && s->nmilitary &&
                   spawn_rand() < s->military_start + (f < 1 ? f : 1) * (s->military_end - s->military_start);
    int start = (int)(spawn_rand() * (float)s->nslots);
    for (int i = 0; i < s->nslots; i++) {            /* the first free slot of that sort from a random one */
        struct WindowSlot *ws = &s->slots[(start + i) % s->nslots];
        if (ws->military != want_mil)
            continue;
        SceneBuilding *sb = &s->buildings[ws->building];
        const Building *b = &sb->b;
        int ls = building_live_of(b, ws->storey), ci = building_cell_index(b, FACE_FRONT, ws->column);
        if (b->state != BUILDING_STANDING || ls <= 0 || ci < 0 || b->cell[ws->storey][ci].state != 0 ||
            people_window_at(s->folk, ws->building, ls, ws->column) >= 0)
            continue;                                /* [R 0x31AD00] the ground storey (its record +0x10 is 0) none */
        {                                            /* [R 0x31AD00] 10 out in front of the cell, the last building
                                                      * whose box holds it (0x2A3D20) must stand lower than it */
            float px = b->min[0] + CELL * ((float)ws->column + 0.5f), py = b->min[1] - 10;
            int front = -1;
            for (uint32_t k = 0; k < s->nbuildings; k++) {
                const Building *o = &s->buildings[k].b;
                if (px >= o->min[0] && px <= o->max[0] && py >= o->min[1] && py <= o->max[1])
                    front = (int)k;
            }
            if (front >= 0 && s->buildings[front].b.n >= ls)
                continue;
        }
        /* 0x2BE660: the bottom middle of the slot's Openable (0x2CFF30), on the front */
        int tile = b->cell[ws->storey][ci].tile & 3;
        const float *ol = sb->openable[tile][ws->lr];
        if (ol[0] < 0)
            continue;
        float at[3] = { b->min[0] + CELL * (float)ws->column + (ol[0] + ol[2]) / 2, b->min[1],
                        b->min[2] + GROUND_FLOOR + STOREY * (float)ls + ol[1] };
        if (ws->unit >= 0 && ws->unit < army_count(s->army)) {   /* its soldier still there (the army reuses ids) */
            const ArmyUnit *u = army_unit(s->army, ws->unit);
            if (u->phase != AU_GONE && fabsf(u->pos[0] - at[0]) < 1 && fabsf(u->pos[2] - at[2]) < 2)
                continue;
        }
        if (ws->military) {
            ws->unit = army_spawn_window(s->army, s->military[ws->occupant], at);
            if (getenv("RAMPAGE_TRACE"))
                printf("  a window soldier %s at %.1f %.1f %.1f (unit %d)\n", s->military[ws->occupant], at[0], at[1], at[2], ws->unit);
        }
        else
            people_add_window(s->folk, s->occupant[ws->occupant], at, ws->building, ls, ws->column);
        /* and its window opens over 0.5 s (0x318B04: openWindow(storey, column, LRPos, 1, 0.5)) */
        building_window_open(&sb->b, ws->storey, ws->column, ws->lr, 1, 0.5f, ol, sb->opens_h[tile][ws->lr]);
        return;
    }
}

int scene_window_close(Scene *s, int building, int storey, int column)
{
    if (building < 0 || (uint32_t)building >= s->nbuildings)
        return 0;
    Building *b = &s->buildings[building].b;
    if (storey < 0 || storey >= b->n)
        return 0;
    int o = b->live[storey], shut = 0;
    for (int i = 0; i < s->nslots; i++)              /* 0x30C3F0: the slot's (+0x8 storey, +0x4 column, +0xC LRPos) */
        if (s->slots[i].building == building && s->slots[i].storey == o && s->slots[i].column == column)
            return building_window_open(b, o, column, s->slots[i].lr, 0, 0.5f, NULL, 0);
    for (int lr = 0; lr < 2; lr++)                   /* no slot (not one of the spawner's): whichever is open */
        shut |= building_window_open(b, o, column, lr, 0, 0.5f, NULL, 0);
    return shut;
}

int scene_window_close_at(Scene *s, const float pos[3])
{
    for (uint32_t i = 0; i < s->nbuildings; i++) {   /* the building whose front the point is on */
        const Building *b = &s->buildings[i].b;
        if (pos[0] < b->min[0] || pos[0] >= b->max[0] || fabsf(pos[1] - b->min[1]) > 2.5f)
            continue;
        float zr = pos[2] - b->min[2] - GROUND_FLOOR;
        if (zr < 0)
            return 0;
        return scene_window_close(s, (int)i, (int)(zr / STOREY), (int)((pos[0] - b->min[0]) / CELL));
    }
    return 0;
}

int scene_people_boxes(const Scene *s, PeopleBox *out, int max)
{
    int n = 0;
    for (uint32_t i = 0; i < s->nbuildings && n < max; i++, n++) {
        const Building *b = &s->buildings[i].b;
        memcpy(out[n].min, b->min, sizeof out[n].min);
        memcpy(out[n].max, b->max, sizeof out[n].max);
        out[n].width = b->width;
        out[n].gone = b->state == BUILDING_GONE || b->state >= BUILDING_CRACKING;
    }
    return n;
}



/* a block slot's traffic (gAIMgr's TrafficDef, 0x334240): the viewer's vehicle.c drives it from the BlockDef */
static void traffic(Scene *s, Group block, float y0)
{
    int slot = (int)(y0 / 41 + 0.5f);
    if (slot >= 0 && slot < 3)
        s->traffic_def[slot] = block;
}

/* the generated upper storeys: one quad per facade cell, its tile out of the 4 x 4 sheet. No normals: the game lights
 * the facade itself, a colour per grid vertex (building_light_grid), and draws it with that (light_facade) */
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
    p->col = malloc(sizeof(float) * 16 * (nq ? nq : 1));
    p->uv = malloc(sizeof(float) * 8 * (nq ? nq : 1));
    p->tris = malloc(sizeof(uint32_t) * 6 * (nq ? nq : 1));
    float hw = width * CELL / 2, hd = depth * CELL / 2;
    for (int st = 0; st < nstoreys; st++) {
        for (int cell = 0; cell < cells && (uint32_t)cell < ntiles[st]; cell++) {
            float ax, ay, bx, by;
            if (cell < depth) {                       /* left side, back to front, facing -X */
                ax = -hw; ay = hd - CELL * cell; bx = -hw; by = ay - CELL;
            } else if (cell < depth + width) {        /* front, left to right, facing -Y */
                int k = cell - depth;
                ax = -hw + CELL * k; ay = -hd; bx = ax + CELL; by = -hd;
            } else {                                  /* right side, front to back, facing +X */
                int j = cell - depth - width;
                ax = hw; ay = -hd + CELL * j; bx = hw; by = ay + CELL;
            }
            float u0 = (float)((int)record_num(tiles[st][cell]) % 4) * 0.25f;
            float corner[4][5] = { { ax, ay, z[st], u0, 0.25f }, { ax, ay, z[st] + STOREY, u0, 0 },
                                   { bx, by, z[st], u0 + 0.25f, 0.25f }, { bx, by, z[st] + STOREY, u0 + 0.25f, 0 } };
            uint32_t v0 = p->nverts;
            for (int k = 0; k < 4; k++) {
                memcpy(p->pos + 3 * p->nverts, corner[k], 12);
                for (int c = 0; c < 4; c++)
                    p->col[4 * p->nverts + c] = 1;
                p->uv[2 * p->nverts] = corner[k][3]; p->uv[2 * p->nverts + 1] = corner[k][4];
                p->nverts++;
            }
            uint32_t q[6] = { v0, v0 + 2, v0 + 1, v0 + 2, v0 + 3, v0 + 1 };   /* counter-clockwise from outside */
            memcpy(p->tris + 3 * p->ntris, q, sizeof q);
            p->ntris += 2;
        }
    }
    return m;
}

/* the sheet's ColumnDef| outlines (0x2D0FB0): pixels of a tile RefTexWidth / 4 x RefTexHeight / 4 to cell units,
 * x = (o & (Wt - 1)) x 5 / Wt, z = (Ht - o) x 3 / Ht, the first x and the last z a pixel in */
static void sheet_columns(City *c, SceneBuilding *sb, const char *sheet)
{
    for (int k = 0; k < 4; k++) {
        sb->outline[k][0] = -1;
        sb->openable[k][0][0] = sb->openable[k][1][0] = -1;
        sb->opens_h[k][0] = sb->opens_h[k][1] = 0;
    }
    sb->inner_wall = 0;
    sb->crack_k = 1.0f / 8;
    sb->crack_tint[0] = sb->crack_tint[1] = sb->crack_tint[2] = 1;
    Group g = template_of(c, sheet), where;
    if (!g.at)
        return;
    sb->inner_wall = (int)group_num(g, "InnerWallColumn", 0) & 3;
    float scale = group_num(g, "CrackScale", 8);
    sb->crack_k = scale > 0 ? 1 / scale : 1.0f / 8;
    sb->crack_tint[0] = group_num(g, "CrackTintR", 1);
    sb->crack_tint[1] = group_num(g, "CrackTintG", 1);
    sb->crack_tint[2] = group_num(g, "CrackTintB", 1);
    int wt = (int)group_num(g, "RefTexWidth", 512) / 4, ht = (int)group_num(g, "RefTexHeight", 512) / 4;
    if (wt < 1 || ht < 1 || (wt & (wt - 1)) || (ht & (ht - 1)))
        return;
    Record r[8], o[8];
    uint32_t n = group_values(g, "ColumnDef|", &where, r, 8);
    for (uint32_t i = 0; i < n; i++) {
        Group cd = record_group(where, r[i]);
        int col = (int)group_num(cd, "Column", -1);
        if (col < 0 || col > 3)
            continue;
        Group ol = group_sub(cd, "Outline|");
        int p[4];
        if (ol.at && group_items(ol, o, 8) >= 4) {
            for (int k = 0; k < 4; k++)
                p[k] = (int)record_num(o[k]);
            sb->outline[col][0] = (float)((p[0] & (wt - 1)) + 1) * CELL / wt;
            sb->outline[col][1] = (float)(ht - (p[1] & (ht - 1))) * STOREY / ht;
            sb->outline[col][2] = (float)(p[2] & (wt - 1)) * CELL / wt;
            sb->outline[col][3] = (float)(ht - 1 - (p[3] & (ht - 1))) * STOREY / ht;
        }
        /* its Openable| windows (0x2D1280 ..): LRPos says which (0, 1), OpensH, and the rect, in the same pixels;
         * the four numbers are left, bottom, right, top (the window's lower sash, or its casement) */
        Group ow;
        Record orr[4];
        uint32_t no = group_values(cd, "Openable|", &ow, orr, 4);
        for (uint32_t j = 0; j < no; j++) {
            Group og = record_group(ow, orr[j]);
            int lr = (int)group_num(og, "LRPos", 0);
            if (lr < 0 || lr > 1 || group_items(og, o, 8) < 4)
                continue;
            for (int k = 0; k < 4; k++)
                p[k] = (int)record_num(o[k]);
            float *q = sb->openable[col][lr];
            q[0] = (float)((p[0] & (wt - 1)) + 1) * CELL / wt;
            q[1] = (float)(ht - (p[1] & (ht - 1))) * STOREY / ht;
            q[2] = (float)(p[2] & (wt - 1)) * CELL / wt;
            q[3] = (float)(ht - 1 - (p[3] & (ht - 1))) * STOREY / ht;
            sb->opens_h[col][lr] = group_num(og, "OpensH", 0) != 0;
        }
    }
}

static void part_reset(MeshPart *p, uint32_t quads)
{
    free(p->pos); free(p->nrm); free(p->uv); free(p->col); free(p->tris);
    p->pos = malloc(sizeof(float) * 12 * (quads ? quads : 1));
    p->nrm = malloc(sizeof(float) * 12 * (quads ? quads : 1));
    p->uv = malloc(sizeof(float) * 8 * (quads ? quads : 1));
    p->col = malloc(sizeof(float) * 16 * (quads ? quads : 1));
    p->tris = malloc(sizeof(uint32_t) * 6 * (quads ? quads : 1));
    p->nverts = p->ntris = 0;
}

/* one quad, corners in order around it, wound to face n */
static void part_quad(MeshPart *p, const float pos[4][3], const float uv[4][2], const float col[4][4], const float n[3])
{
    uint32_t v0 = p->nverts;
    for (int k = 0; k < 4; k++) {
        memcpy(p->pos + 3 * p->nverts, pos[k], 12);
        memcpy(p->nrm + 3 * p->nverts, n, 12);
        memcpy(p->uv + 2 * p->nverts, uv[k], 8);
        memcpy(p->col + 4 * p->nverts, col[k], 16);
        p->nverts++;
    }
    float a[3], b[3];
    for (int k = 0; k < 3; k++) {
        a[k] = pos[1][k] - pos[0][k];
        b[k] = pos[2][k] - pos[0][k];
    }
    float d = (a[1] * b[2] - a[2] * b[1]) * n[0] + (a[2] * b[0] - a[0] * b[2]) * n[1] + (a[0] * b[1] - a[1] * b[0]) * n[2];
    uint32_t q[6] = { v0, v0 + 1, v0 + 2, v0, v0 + 2, v0 + 3 };
    if (d < 0) { q[1] = v0 + 2; q[2] = v0 + 1; q[4] = v0 + 3; q[5] = v0 + 2; }
    memcpy(p->tris + 3 * p->ntris, q, sizeof q);
    p->ntris += 2;
}

/* a point of a cell of face (FACE_*) column col: a along the face (0 .. 5, left to right seen from outside), z up
 * the storey (0 .. 3, squashed to its height now), d into the building */
static void face_point(const SceneBuilding *sb, int face, int col, float a, float z, float d, float bottom, float height,
                       float out[3])
{
    float hw = sb->b.width * CELL / 2, hd = sb->b.depth * CELL / 2;
    if (face == FACE_FRONT) { out[0] = -hw + CELL * (float)col + a; out[1] = -hd + d; }
    else if (face == FACE_LEFT) { out[0] = -hw + d; out[1] = hd - CELL * (float)col - a; }
    else { out[0] = hw - d; out[1] = -hd + CELL * (float)col + a; }
    out[2] = bottom + z * height / STOREY;
}

static void face_normal(int face, float n[3])
{
    n[0] = face == FACE_LEFT ? -1.0f : face == FACE_RIGHT ? 1.0f : 0;
    n[1] = face == FACE_FRONT ? -1.0f : 0;
    n[2] = 0;
}

/* the facade's other passes (0x2C92F0 .., 0x2C6A30 .., 0x2C63A0), made again whenever the building changes:
 * - the window glass, for every cell in state 0 whose column has an outline: the outline grown by 0.3, 0.03 behind
 *   the face (seen through the sheet's clear windows), one of 16 InnerWindow tiles, tinted CubeTint (/ 256: the
 *   game halves its vertex colours, 0x80 = 1);
 * - the cracks on the front, faded in by the vertex rows' levels (level x 85, halved: level / 3), around the window
 *   below state 2, CrackTexture running on across the faces at 1 / CrackScale (here just in front of the face: the
 *   game puts it 0.015 behind and must draw it over the facade some other way), coloured by the building light at
 *   its grid vertex times CrackTint (the vertex's +0x0, 0x2CE7B8), drawn unlit as the facade;
 * - while a corner front cell is damaged, a wall 5 deep just inside the side wall (0.3 in at its back), the sheet's
 *   InnerWallColumn at the side cell's state, grey (0x80 halved). */
static void facade_passes(Scene *s, SceneBuilding *sb)
{
    if (sb->mesh < 0 || s->meshes[sb->mesh].mesh.nparts < 4)
        return;
    MeshPart *glass = &s->meshes[sb->mesh].mesh.parts[1], *crack = &s->meshes[sb->mesh].mesh.parts[2];
    MeshPart *inner = &s->meshes[sb->mesh].mesh.parts[3];
    const Building *b = &sb->b;
    int cells = 2 * b->depth + b->width;
    part_reset(glass, (uint32_t)(b->storeys * cells));
    part_reset(crack, (uint32_t)(b->storeys * cells * 4));
    part_reset(inner, (uint32_t)(b->storeys * 2));
    float tint[4] = { s->cube_tint[0] / 256, s->cube_tint[1] / 256, s->cube_tint[2] / 256, 1 };
    for (int o = 0; o < b->storeys; o++) {
        int ls = building_live_of(b, o);
        if (ls < 0 || b->state == BUILDING_GONE)
            continue;
        float bottom = 0, height = 0;
        building_storey(b, ls, &bottom, &height);
        if (height <= 0.01f)
            continue;
        for (int face = 0; face < 3; face++) {
            int ncol = face == FACE_FRONT ? b->width : b->depth;
            float n[3];
            face_normal(face, n);
            for (int col = 0; col < ncol; col++) {
                int ci = building_cell_index(b, face, col);
                if (ci < 0 || sb->quad[o][ci] < 0)
                    continue;
                const BuildingCell *cell = &b->cell[o][ci];
                const float *ol = sb->outline[cell->tile & 3];
                if (cell->state == 0 && ol[0] >= 0) {
                    unsigned t = ((unsigned)o * 7u + (unsigned)ci * 13u + b->seed * 5u) & 15u;   /* cell +6 */
                    float a0 = ol[0] - 0.3f, a1 = ol[2] + 0.3f, z0 = ol[1] - 0.3f, z1 = ol[3] + 0.3f;
                    float az[4][2] = { { a0, z0 }, { a1, z0 }, { a1, z1 }, { a0, z1 } }, pos[4][3], uv[4][2], col4[4][4];
                    for (int k = 0; k < 4; k++) {
                        face_point(sb, face, col, az[k][0], az[k][1], 0.03f, bottom, height, pos[k]);
                        uv[k][0] = 0.25f * (float)(t & 3) + 0.05f * az[k][0];
                        uv[k][1] = 1 - (0.25f * (float)(t >> 2) + az[k][1] / 12);
                        memcpy(col4[k], tint, sizeof tint);
                    }
                    part_quad(glass, pos, uv, col4, n);
                }
                /* the cracks: this cell's corners in the vertex rows ls (bottom) and ls + 1 (top) */
                const BuildingVert *cv[4] = { &b->vert[ls][face][col], &b->vert[ls][face][col + 1],
                                              &b->vert[ls + 1][face][col + 1], &b->vert[ls + 1][face][col] };
                float lv[4] = { cv[0]->level, cv[1]->level, cv[2]->level, cv[3]->level };
                if (lv[0] + lv[1] + lv[2] + lv[3] <= 0)
                    continue;
                float outer[4][2] = { { 0, 0 }, { CELL, 0 }, { CELL, STOREY }, { 0, STOREY } };
                float in[4][2] = { { ol[0], ol[1] }, { ol[2], ol[1] }, { ol[2], ol[3] }, { ol[0], ol[3] } };
                int bands = cell->state < 2 && ol[0] >= 0;
                float start = (face == FACE_FRONT ? CELL * (float)b->depth : face == FACE_RIGHT ?
                               CELL * (float)(b->depth + b->width) : 0) + CELL * (float)col, up = STOREY * (float)ls;
                int corner_of[4] = { 0, 1, 2, 3 };           /* an inner corner moves with the nearest outer one */
                for (int band = 0; band < (bands ? 4 : 1); band++) {
                    float az[4][2], al[4], pos[4][3], uv[4][2], col4[4][4];
                    int who[4];
                    if (bands) {                         /* bottom, right, top, left of the window */
                        int i = band, j = (band + 1) & 3;
                        memcpy(az[0], outer[i], 8); memcpy(az[1], outer[j], 8);
                        memcpy(az[2], in[j], 8); memcpy(az[3], in[i], 8);
                        al[0] = lv[i]; al[1] = lv[j]; al[2] = lv[j]; al[3] = lv[i];
                        who[0] = who[3] = corner_of[i];
                        who[1] = who[2] = corner_of[j];
                    } else {
                        memcpy(az, outer, sizeof az);
                        memcpy(al, lv, sizeof al);
                        for (int k = 0; k < 4; k++) who[k] = k;
                    }
                    for (int k = 0; k < 4; k++) {
                        face_point(sb, face, col, az[k][0] + cv[who[k]]->along, az[k][1], -0.02f, bottom, height, pos[k]);
                        pos[k][2] += cv[who[k]]->up;
                        uv[k][0] = (start + az[k][0]) * sb->crack_k;
                        uv[k][1] = -(up + az[k][1]) * sb->crack_k;
                        /* its grid vertex's light (corners 0 .. 3: bottom left, bottom right, top right, top left) */
                        const float *lit = sb->light ? sb->light[ls + (who[k] >= 2)][face][col + (who[k] == 1 || who[k] == 2)]
                                                     : NULL;
                        for (int c = 0; c < 3; c++) {
                            float v = (lit ? lit[c] : 1) * sb->crack_tint[c];
                            col4[k][c] = v < 0 ? 0 : v > 1 ? 1 : v;
                        }
                        col4[k][3] = al[k] / 3;
                    }
                    part_quad(crack, pos, uv, col4, n);
                }
            }
        }
        /* the inner corner walls, behind the first and the last front cell */
        for (int side = 0; side < 2; side++) {
            int fi = building_cell_index(b, FACE_FRONT, side ? b->width - 1 : 0);
            int si = building_cell_index(b, side ? FACE_RIGHT : FACE_LEFT, side ? 0 : b->depth - 1);
            if (fi < 0 || si < 0 || b->cell[o][fi].state == 0)
                continue;
            float hw = b->width * CELL / 2, hd = b->depth * CELL / 2, x = side ? hw : -hw, in = side ? -0.3f : 0.3f;
            float pos[4][3] = { { x, -hd, bottom }, { x + in, -hd + CELL, bottom }, { x + in, -hd + CELL, bottom + height },
                                { x, -hd, bottom + height } };
            float u0 = (float)(sb->inner_wall + 1) * 0.25f, u1 = (float)sb->inner_wall * 0.25f;
            float v0 = b->cell[o][si].state * 0.25f;
            float uv[4][2] = { { u0, v0 + 0.25f }, { u1, v0 + 0.25f }, { u1, v0 }, { u0, v0 } };
            float grey[4][4] = { { 0.5f, 0.5f, 0.5f, 1 }, { 0.5f, 0.5f, 0.5f, 1 }, { 0.5f, 0.5f, 0.5f, 1 }, { 0.5f, 0.5f, 0.5f, 1 } };
            float n[3] = { side ? -1.0f : 1.0f, 0, 0 };
            part_quad(inner, pos, uv, grey, n);
        }
    }
    free(crack->nrm);                                /* the cracks carry their light: drawn unlit (render_gl.c) */
    crack->nrm = NULL;
    free(inner->nrm);                                /* the inner corner walls: 0xFF808080 halved, no normal (0x2C63A0) */
    inner->nrm = NULL;
    if (glass->material) {                           /* the glass behind an InnerWindowTexture: one colour (G +0x6F0) */
        free(glass->nrm);                            /* at every corner, format 0x142, unlit (0x2C92F0, 0x2D2700) */
        glass->nrm = NULL;
    }
}

/* the work queue (0x2BF270(Q, item, 4)) a relight goes through: the first request of a frame is granted, a later one
 * once it has waited 4 frames [R]; a grant takes it off the queue */
static int light_grant(Scene *s, SceneBuilding *sb)
{
    if (!sb->light_queued)
        sb->light_queued = s->light_frame + 1;
    if (s->light_granted && s->light_frame - (sb->light_queued - 1) < 4)
        return 0;
    s->light_granted = 1;
    sb->light_queued = 0;
    return 1;
}

/* the open windows (0x2BECC0, every frame): per window of the building's list (the first 40), a flat quad of colour
 * 0xFF101010 (material 0x25C4F0 shader 0x11, untextured: 16 / 255, unlit, opaque) 0.015 in front of the face (the
 * front's y - 0.03 / 2), from its rect's bottom left: across `open` of its width (OpensH) or up `open` of its height,
 * the full other way; the corners bottom left, bottom right, top right, top left. Not for a storey that is gone. */
static void open_windows(Scene *s, SceneBuilding *sb)
{
    if (!sb->open_part || sb->mesh < 0 || s->meshes[sb->mesh].mesh.nparts <= (uint32_t)sb->open_part)
        return;
    MeshPart *p = &s->meshes[sb->mesh].mesh.parts[sb->open_part];
    const Building *b = &sb->b;
    if (!b->nwindows && !p->ntris)
        return;
    int n = b->nwindows < 40 ? b->nwindows : 40;
    part_reset(p, (uint32_t)n);
    static const float DARK[4] = { 16.0f / 255, 16.0f / 255, 16.0f / 255, 1 }, N[3] = { 0, -1, 0 };
    for (int k = 0; k < n && b->state != BUILDING_GONE; k++) {
        const BuildingWindow *w = &b->window[k];
        int ls = building_live_of(b, w->o);
        if (ls < 0)
            continue;
        float bottom, height;
        building_storey(b, ls, &bottom, &height);
        float x1 = w->x0 + (w->opens_h ? w->open * w->w : w->w), z1 = w->z0 + (w->opens_h ? w->h : w->open * w->h);
        float az[4][2] = { { w->x0, w->z0 }, { x1, w->z0 }, { x1, z1 }, { w->x0, z1 } };
        float pos[4][3], uv[4][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } }, col[4][4];
        for (int c = 0; c < 4; c++) {
            face_point(sb, FACE_FRONT, w->column, az[c][0], az[c][1], -0.015f, bottom, STOREY, pos[c]);
            memcpy(col[c], DARK, sizeof DARK);
        }
        part_quad(p, pos, uv, col, N);
    }
    free(p->nrm);                                    /* its colour is all it has: drawn unlit, untextured */
    p->nrm = NULL;
    free(p->uv);
    p->uv = NULL;
}

/* a group's Adornment| entries (0x2D1A30: the template's, then the instance's), placed by 0x30FAA0 / 0x30F710:
 * on a side (0 left, 1 right, 2 front) at storey `floor`, `cell` 5 wide, u along the face (left to right seen from
 * outside), v up from the storey's floor, the model facing out (its +y into the building); on the ground floor
 * (side 4: v above the base) or the roof (side 5: 1.5 + v above the roof band's foot), by LocationOrigin: 0 the
 * left face from its back, 1 the right face from its front, 2 the front from its left, 4 / 5 (u, v) from the
 * building's middle. AdornmentFlags and CellDetachDamage come from the entry, else its template. */
static void adornments(City *c, Scene *s, SceneBuilding *sb, Group from, int nst)
{
    Group where;
    static Record r[MAX_REC];
    uint32_t n = from.at ? group_all(from, "Adornment|", r, MAX_REC) : 0;
    (void)where;
    float hw = sb->b.width * CELL / 2, hd = sb->b.depth * CELL / 2;
    for (uint32_t i = 0; i < n; i++) {
        Group e = record_group(from, r[i]), at = template_of(c, group_str(e, "TemplateName"));
        const char *shape = at.at ? group_str(at, "Shape|Shape") : NULL;
        if (!shape)
            continue;
        Adornment a = { 0 };
        a.side = (int)group_num(e, "side", 2);
        float u = group_num(e, "u", 0), v = group_num(e, "v", 0);
        int origin = 3;
        if (a.side < 4) {
            a.record = (int)group_num(e, "floor", 0);
            a.cell = (int)group_num(e, "cell", 0);
        } else {
            a.record = a.side == 5 ? nst : 0;
            a.cell = -1;
            origin = (int)group_num(e, "LocationOrigin", 0);
        }
        a.flags = (int)group_num(e, "AdornmentFlags", group_num(at, "AdornmentFlags", 0));
        a.detach = (int)group_num(e, "CellDetachDamage", group_num(at, "CellDetachDamage", 0));
        a.fx = group_str(e, "DislodgedFx");
        if (!a.fx)
            a.fx = group_str(at, "DislodgedFx");
        int face = a.side < 4 ? a.side : origin;
        float along = a.side < 4 ? CELL * (float)a.cell + u : u;
        if (face == FACE_LEFT) { a.x = -hw; a.y = hd - along; a.deg = -90; }
        else if (face == FACE_RIGHT) { a.x = hw; a.y = -hd + along; a.deg = 90; }
        else if (face == FACE_FRONT) { a.x = -hw + along; a.y = -hd; a.deg = 0; }
        else if (origin == 4 || origin == 5) { a.x = u; a.y = v; a.deg = 0; }
        a.zoff = (a.side == 4 ? -GROUND_FLOOR : a.side == 5 ? 1.5f : 0) + (origin == 4 || origin == 5 ? 0 : v);
        if (a.record < 0 || a.record > nst)
            continue;
        int missing = s->missing;
        a.inst = place_model(c, s, shape, sb->node, -1);
        s->missing = missing;
        if (a.inst < 0)
            continue;
        for (int k = 0; k < 3; k++) {                /* its model's box (0x2D4390 tests the model's own) */
            a.lo[k] = 1e30f;
            a.hi[k] = -1e30f;
        }
        for (uint32_t it = 0; it < s->nitems; it++) {
            if (s->items[it].instance != a.inst)
                continue;
            const Mesh *m = &s->meshes[s->items[it].mesh].mesh;
            for (uint32_t p = 0; p < m->nparts; p++)
                for (uint32_t q = 0; q < m->parts[p].nverts; q++)
                    for (int k = 0; k < 3; k++) {
                        float x = m->parts[p].pos[3 * q + k];
                        if (x < a.lo[k]) a.lo[k] = x;
                        if (x > a.hi[k]) a.hi[k] = x;
                    }
        }
        sb->adorn = realloc(sb->adorn, sizeof *sb->adorn * (size_t)(sb->nadorn + 1));
        sb->adorn[sb->nadorn++] = a;
    }
}

/* 0x30F690 / 0x2CD010: an adornment's height now: on its storey's slot, or the first one above standing (the roof
 * band's at the top), 6 + 3 x slot + its offset, lower by the falling storey's progress above it */
static float adornment_z(const Building *b, const Adornment *a)
{
    int slot = b->n;
    for (int o = a->record; o < b->storeys; o++) {
        int ls = building_live_of(b, o);
        if (ls >= 0) {
            slot = ls;
            break;
        }
    }
    float z = GROUND_FLOOR + STOREY * (float)slot + a->zoff;
    if (b->collapse > 0 && slot >= b->collapse)
        z -= STOREY * b->progress;
    return z;
}

static void adornments_place(Scene *s, SceneBuilding *sb, Mat4 sway)
{
    const Building *b = &sb->b;
    for (int i = 0; i < sb->nadorn; i++) {
        Adornment *a = &sb->adorn[i];
        if (!a->gone && a->side < 3) {               /* 0x2CFC20: its cell damaged enough, it falls off (flag 4 once
                                                        the building's HP is gone) */
            int ci = building_cell_index(b, a->side, a->cell);
            int st = ci >= 0 && a->record < b->storeys ? b->cell[a->record][ci].state : 0;
            if (st > 0 && st >= a->detach && (!(a->flags & 4) || b->hp <= 0))
                a->gone = 1;
        }
        Instance *in = &s->instances[a->inst];
        in->dynamic = 1;
        in->hidden = a->gone || b->state == BUILDING_GONE;
        in->base = mat_mul(sway, mat_mul(sb->node, mat_trs_z(a->x, a->y, adornment_z(b, a), a->deg)));
    }
}

void scene_fx(Scene *s, const char *name, const float pos[3], const float dir[3])
{
    if (!name || s->nfxreq >= 64)
        return;
    struct SceneFx *f = &s->fxreq[s->nfxreq++];
    f->name = name;
    memcpy(f->pos, pos, sizeof f->pos);
    memcpy(f->dir, dir, sizeof f->dir);
}

int scene_adornments_hit(Scene *s, const float centre[3], float radius)
{
    int n = 0;
    for (uint32_t bi = 0; bi < s->nbuildings; bi++) {
        SceneBuilding *sb = &s->buildings[bi];
        for (int i = 0; i < sb->nadorn; i++) {
            Adornment *a = &sb->adorn[i];
            if (a->gone || (a->flags & 6))
                continue;
            const float *m = s->instances[a->inst].base.m;   /* the centre in its own frame (a rotation about z) */
            float d[3] = { centre[0] - m[12], centre[1] - m[13], centre[2] - m[14] }, l[3], r = 0.75f * radius, dd = 0;
            for (int k = 0; k < 3; k++)
                l[k] = m[4 * k] * d[0] + m[4 * k + 1] * d[1] + m[4 * k + 2] * d[2];
            for (int k = 0; k < 3; k++) {
                float c = l[k] < a->lo[k] ? a->lo[k] - l[k] : l[k] > a->hi[k] ? l[k] - a->hi[k] : 0;
                dd += c * c;
            }
            if (dd <= r * r) {
                a->gone = 1;                         /* flag 0x10, and its DislodgedFx where it was */
                const float *w = s->instances[a->inst].base.m;
                float at[3] = { w[12], w[13], w[14] }, dir[3] = { -w[4], -w[5], -w[6] };   /* out of the wall */
                scene_fx(s, a->fx, at, dir);
                if (getenv("RAMPAGE_TRACE"))
                    printf("  knocked off building %u's adornment %d (side %d)\n", bi, i, a->side);
                s->instances[a->inst].hidden = 1;
                n++;
            }
        }
    }
    return n;
}

static void building(City *c, Scene *s, Group inst, float row_offset, float y0, int scenery)
{
    SceneBuilding **list = scenery ? &s->scenery : &s->buildings;
    uint32_t *count = scenery ? &s->nscenery : &s->nbuildings;
    Group t = template_of(c, group_str(inst, "TemplateName")), where;
    if (!t.at) {
        s->missing++;
        return;
    }
    int width = (int)group_num(t, "width", 1), depth = (int)group_num(t, "depth", 2);
    /* 0x305E78 .. 0x305F08: x = LeftEdgeXPos + 2.5 x width, y = the slot's + 5 + (Row ? BackRowOffset : 0): the front
     * row's fronts at y 0, behind the street's far sidewalk (y -5 .. 0) */
    Mat4 node = mat_trs_z(group_num(inst, "LeftEdgeXPos", 0) + width * CELL / 2,
                          y0 + 5 + (group_num(inst, "Row", 0) != 0 ? row_offset : 0), 0, 0);
    int first = (int)s->ninstances, ground_inst = -1, facade_inst = -1, top_inst = -1;
    uint32_t had = *count;
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
        Mesh fm = facade((uint32_t)sheet_name, width, depth, z, tiles, ntiles, nst);
        /* glass, cracks, inner corner walls (facade_passes), the open windows (open_windows) */
        fm.parts = realloc(fm.parts, sizeof *fm.parts * 5);
        memset(fm.parts + 1, 0, sizeof *fm.parts * 4);
        fm.nparts = 5;
        Group sg = template_of(c, sheet);
        const char *glass = group_str(t, "Texture|InnerWindowTexture"), *crack = sg.at ? group_str(sg, "CrackTexture") : NULL;
        int gi = glass ? bdp_find_name(&c->bundle, glass) : -1, ki = crack ? bdp_find_name(&c->bundle, crack) : -1;
        fm.parts[1].material = gi > 0 ? (uint32_t)gi : 0;
        fm.parts[2].material = ki > 0 ? (uint32_t)ki : 0;
        fm.parts[2].blend = 1;
        fm.parts[3].material = (uint32_t)sheet_name;
        fm.parts[4].material = 0xFFFFFFFFu;              /* no material: untextured, its vertex colours */
        mesh = (int)scene_add_mesh(s, fm, s->city);
        facade_inst = new_instance(s, node, -1);
        push_item(s, (uint32_t)mesh, facade_inst, -1, NULL);
    }
    /* its cells (the game's grid, 0x2D1A30): the box is 6 + 3 x storeys + 1.5 (the roof's band) high */
    if (nst && width > 0 && depth > 0 && 2 * depth + width <= BUILDING_CELLS) {
        *list = realloc(*list, sizeof **list * (*count + 1));
        SceneBuilding *sb = &(*list)[(*count)++];
        float cx = node.m[12], cy = node.m[13];
        float lo[3] = { cx - width * CELL / 2, cy - depth * CELL / 2, 0 };
        float hi[3] = { cx + width * CELL / 2, cy + depth * CELL / 2, GROUND_FLOOR + STOREY * (float)nst + 1.5f };
        building_init(&sb->b, lo, hi, width, depth, nst);
        sb->hitter = 0;
        sb->b.row = (int)group_num(inst, "Row", 0);
        sb->b.hp = sb->b.hp_max = (int)group_num(t, "BuildingHP", (float)sb->b.hp_max);
        sb->b.seed = *count;
        sb->mesh = mesh;
        sb->node = node;
        sb->ground = ground_inst;
        sb->facade = facade_inst;
        sb->top = sb->rubble = -1;
        sb->roof_inst[0] = sb->roof_inst[1] = sb->roof_inst[2] = sb->roof_inst[3] = -1;
        sb->first = sb->last = 0;
        sb->rest = NULL;
        sb->moving = 0;
        sb->adorn = NULL;
        sb->nadorn = 0;
        memset(sb->seen, 0, sizeof sb->seen);
        sb->seen_collapse = -1;
        sb->props = t;
        sb->inst = inst;
        sb->seen_state = BUILDING_STANDING;
        sb->fx_glass = group_str(t, "Glass_Small");
        sb->fx_wall_small = group_str(t, "Wall_Small");
        sb->fx_wall_large = group_str(t, "Wall_Large");
        sb->fx_floor = group_str(t, "Floor_Large");
        memset(sb->broke, 0, sizeof sb->broke);
        sb->b.collapse_time = group_num(t, "CollapseTime", sb->b.collapse_time);
        sb->b.crack_time = group_num(t, "CrackSpreadTime", sb->b.crack_time);
        sb->b.hesitation = group_num(t, "HesitationTime", sb->b.hesitation);
        sb->b.min_crack = (int)group_num(t, "MinCollapseCrackLevel", (float)sb->b.min_crack);
        sheet_columns(c, sb, sheet);
        sb->open_part = mesh >= 0 && s->meshes[mesh].mesh.nparts >= 5 ? 4 : 0;
        memset(sb->quad, 0xFF, sizeof sb->quad);
        memset(sb->skeleton, 0xFF, sizeof sb->skeleton);
        memset(sb->damaged_skeleton, 0xFF, sizeof sb->damaged_skeleton);
        sb->light = calloc(BUILDING_STOREYS + 1, sizeof *sb->light);   /* lit at the block's end (light_buildings) */
        sb->shade_nb[0] = sb->shade_nb[1] = -1;
        sb->shade_top[0] = sb->shade_top[1] = -1e20f;   /* +0xCC, +0xD4: so the first look builds the shape */
        memset(&sb->shade, 0, sizeof sb->shade);
        memset(sb->shade_faces, 1, sizeof sb->shade_faces);   /* G+0x1EC: 0xFF at the grid's making (0x2D2824) */
        sb->nlit_items = 0;
        int32_t v = 0;
        for (int st = 0; st < nst && st < BUILDING_STOREYS; st++) {
            sb->floor_tint[st][0] = group_num(floors[st], "FloorTintR", 1);   /* the cells' tint (0x2CBC78) */
            sb->floor_tint[st][1] = group_num(floors[st], "FloorTintG", 1);
            sb->floor_tint[st][2] = group_num(floors[st], "FloorTintB", 1);
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
    int roof_inst[4] = { -1, -1, -1, -1 };          /* the roof's models by its damage state (0x28D890) */
    for (uint32_t k = 0; k < 4; k++) {
        const char *top = str_item(group_sub(t, "ShapeSet|TopFloorShapes"), k);
        if (!top)
            break;
        roof_inst[k] = place_model(c, s, top, mat_mul(node, mat_trs_z(0, 0, GROUND_FLOOR + STOREY * (float)nfloors, 0)), -1);
        if (k > 0 && roof_inst[k] >= 0)
            s->instances[roof_inst[k]].hidden = 1;
    }
    top_inst = roof_inst[0];
    Record obj[MAX_REC];
    uint32_t no = group_values(inst, "ObjectInstance|", &where, obj, MAX_REC);
    s->placing_building = !scenery && *count > had ? (int)(*count - 1) : -1;
    for (uint32_t i = 0; i < no; i++)
        place_object(c, s, record_group(where, obj[i]), node);
    s->placing_building = -1;
    if (*count > had) {                              /* its pieces, to move */
        SceneBuilding *sb = &(*list)[*count - 1];
        adornments(c, s, sb, t, nst);
        adornments(c, s, sb, inst, nst);
        const char *rubble = group_str(t, "Shape|RubbleShape");
        int missing = s->missing;
        sb->rubble = rubble ? place_model(c, s, rubble, node, -1) : -1;
        s->missing = missing;
        if (sb->rubble >= 0)
            s->instances[sb->rubble].hidden = 1;
        sb->top = top_inst;
        memcpy(sb->roof_inst, roof_inst, sizeof roof_inst);
        sb->first = first;
        sb->last = (int)s->ninstances;
        sb->rest = malloc(sizeof *sb->rest * (size_t)(sb->last - sb->first + 1));
        for (int i = sb->first; i < sb->last; i++)
            sb->rest[i - sb->first] = s->instances[i].base;
        facade_passes(s, sb);
        adornments_place(s, sb, mat_identity());
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
    for (int k = 0; k < 3; k++)
        s->cube_tint[k] = group_num(ld, k == 0 ? "CubeTintR" : k == 1 ? "CubeTintG" : "CubeTintB", 192);
    /* the buildings' light (0x2B92F0: the light file's ambient and first three lights, the Ramp keys; a missing
     * float key reads as 0) and the facade darkening's colour (MShadowR/G/B, +0x314 ..) */
    static const float WHITE[3] = { 1, 1, 1 };       /* no light file: everything at full light, as rnd_frame does */
    building_light_init(&s->building_light, s->nlights ? s->ambient : WHITE, s->nlights, (const float (*)[3])s->light_dir,
                        (const float (*)[3])s->light_colour, group_num(ld, "RampStart", 0), group_num(ld, "RampEnd", 0),
                        group_num(ld, "RampIntensity", 0));
    shadow_lighting_default(&s->shade_light);
    s->shade_light.mshadow[0] = group_num(ld, "MShadowR", s->shade_light.mshadow[0]);
    s->shade_light.mshadow[1] = group_num(ld, "MShadowG", s->shade_light.mshadow[1]);
    s->shade_light.mshadow[2] = group_num(ld, "MShadowB", s->shade_light.mshadow[2]);
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

/* ---------------------------------------------------------------- the building light (BUILDING_LIGHT_NOTES.md) */

static void box_of(const Building *b, float box[6])
{
    memcpy(box, b->min, 3 * sizeof(float));
    memcpy(box + 3, b->max, 3 * sizeof(float));
}

/* 0x31C8C0, as a playable building is made: only one in the back row (Row 1) has neighbours whose shadows can reach
 * it. A: the first of the front row's buildings (the row list 0x4D0918, in the order 0x2A5110 added them: the block's
 * BuildingInstance| order) whose min x is past its own; B: the first of its own row's whose min x is at or past its
 * own max x (its neighbour on the right) */
static void shade_neighbours(Scene *s, uint32_t bi)
{
    SceneBuilding *sb = &s->buildings[bi];
    sb->shade_nb[0] = sb->shade_nb[1] = -1;
    if (sb->b.row != 1)
        return;
    for (uint32_t i = 0; i < s->nbuildings; i++)
        if (s->buildings[i].b.row == 0 && !(s->buildings[i].b.min[0] <= sb->b.min[0])) {
            sb->shade_nb[0] = (int)i;
            break;
        }
    for (uint32_t i = 0; i < s->nbuildings; i++)
        if (s->buildings[i].b.row == sb->b.row && !(s->buildings[i].b.min[0] < sb->b.max[0])) {
            sb->shade_nb[1] = (int)i;
            break;
        }
}

/* 0x31C860: has a neighbour's top (+0x1F0) changed since the shape was made */
static int shade_changed(const Scene *s, const SceneBuilding *sb)
{
    for (int k = 0; k < 2; k++)
        if (sb->shade_nb[k] >= 0 && s->buildings[sb->shade_nb[k]].b.max[2] != sb->shade_top[k])
            return 1;
    return 0;
}

/* 0x31C7B0: the shape again from the neighbours as they now are; when it leaves no shadow on either face, the
 * neighbours are dropped for good */
static void shade_update(Scene *s, SceneBuilding *sb)
{
    if (!shade_changed(s, sb))
        return;
    float own[6], nb[2][6];
    const float *at[2] = { NULL, NULL };
    box_of(&sb->b, own);
    for (int k = 0; k < 2; k++)
        if (sb->shade_nb[k] >= 0) {
            box_of(&s->buildings[sb->shade_nb[k]].b, nb[k]);
            sb->shade_top[k] = nb[k][5];             /* +0xCC, +0xD4 */
            at[k] = nb[k];
        }
    shadow_facade_build(&sb->shade, own, at[0], at[1]);
    if (!sb->shade.front && sb->shade.nright <= 0)
        sb->shade_nb[0] = sb->shade_nb[1] = -1;
}

/* the facade quads' colours (the facade passes 0x2C81A0 .., 0x2C6790): each corner its grid vertex's light times its
 * storey's FloorTint (bytes x the tint's u16 >> 8, at most 255, then halved for the GS: 1.0 at most) */
static void light_facade(SceneBuilding *sb, MeshPart *p)
{
    const Building *b = &sb->b;
    if (!p->col)
        return;
    for (int o = 0; o < b->storeys; o++) {
        int ls = building_live_of(b, o);
        for (int c = 0; c < BUILDING_CELLS; c++) {
            int32_t v = sb->quad[o][c];
            if (v < 0 || (uint32_t)v + 4 > p->nverts)
                continue;
            int face = c < b->depth ? FACE_LEFT : c < 2 * b->depth ? FACE_RIGHT : FACE_FRONT;
            int col = face == FACE_LEFT ? c : face == FACE_RIGHT ? c - b->depth : c - 2 * b->depth;
            for (int k = 0; k < 4; k++) {            /* corners: bottom, top, bottom, top; left, left, right, right */
                const float *lit = ls >= 0 ? sb->light[ls + (k & 1)][face][col + (k >> 1)] : NULL;
                float *out = p->col + 4 * (v + k);
                for (int ch = 0; ch < 3; ch++) {
                    float x = (lit ? lit[ch] : 1) * sb->floor_tint[o][ch];
                    out[ch] = x < 0 ? 0 : x > 1 ? 1 : x;
                }
                out[3] = 1;
            }
        }
    }
}

/* a copy of a mesh whose parts with normals and no colours get colours instead of the normals (lit here, drawn
 * unlit); the others as they are */
static Mesh mesh_copy_lit(const Mesh *m)
{
    Mesh c = { 0 };
    c.parts = calloc(m->nparts ? m->nparts : 1, sizeof *c.parts);
    c.nparts = m->nparts;
    for (uint32_t i = 0; i < m->nparts; i++) {
        const MeshPart *p = &m->parts[i];
        MeshPart *q = &c.parts[i];
        size_t nv = p->nverts ? p->nverts : 1, nt = p->ntris ? p->ntris : 1;
        q->material = p->material;
        q->nverts = p->nverts;
        q->ntris = p->ntris;
        q->blend = p->blend;
        q->pos = malloc(12 * nv);
        memcpy(q->pos, p->pos, 12 * (size_t)p->nverts);
        q->tris = malloc(12 * nt);
        memcpy(q->tris, p->tris, 12 * (size_t)p->ntris);
        if (p->uv) {
            q->uv = malloc(8 * nv);
            memcpy(q->uv, p->uv, 8 * (size_t)p->nverts);
        }
        if (p->nrm && !p->col) {
            q->col = malloc(16 * nv);
            for (uint32_t k = 0; k < 4 * p->nverts; k++)
                q->col[k] = 1;
            continue;
        }
        if (p->nrm) {
            q->nrm = malloc(12 * nv);
            memcpy(q->nrm, p->nrm, 12 * (size_t)p->nverts);
        }
        if (p->col) {
            q->col = malloc(16 * nv);
            memcpy(q->col, p->col, 16 * (size_t)p->nverts);
        }
    }
    return c;
}

/* the ground-floor and top models' items get meshes of their own (the building light is per building) */
static void light_items_make(Scene *s, SceneBuilding *sb)
{
    sb->nlit_items = 0;
    for (uint32_t i = 0; i < s->nitems && sb->nlit_items < 16; i++) {
        DrawItem *it = &s->items[i];
        int roof = 0;
        for (int k = 0; k < 4; k++)
            roof |= sb->roof_inst[k] >= 0 && it->instance == sb->roof_inst[k];
        if (it->skin || (it->instance != sb->ground && it->instance != sb->top && !roof))
            continue;
        uint32_t src = it->mesh;
        Mesh copy = mesh_copy_lit(&s->meshes[src].mesh);
        uint32_t mi = scene_add_mesh(s, copy, s->meshes[src].b);
        s->items[i].mesh = mi;
        sb->lit_item[sb->nlit_items] = (int)i;
        sb->lit_src[sb->nlit_items++] = (int)src;
    }
}

/* 0x31CCA0: the ground-floor and top models' vertices lit where they are (the light at the vertex, its normal turned
 * into the world), less the neighbours' shadows (0x31D560: the front's planes where p.y <= front y + 0.1, the right
 * face's boxes where p.x > max x - 1) */
static void light_models(Scene *s, SceneBuilding *sb, const ShadowFacade *f)
{
    for (int k = 0; k < sb->nlit_items; k++) {
        const DrawItem *it = &s->items[sb->lit_item[k]];
        const Instance *in = &s->instances[it->instance];
        Mat4 w = it->node >= 0 && in->node ? mat_mul(in->base, in->node[it->node]) : in->base;
        const float *m = w.m;
        const Mesh *src = &s->meshes[sb->lit_src[k]].mesh;
        Mesh *dst = &s->meshes[it->mesh].mesh;
        for (uint32_t pi = 0; pi < src->nparts && pi < dst->nparts; pi++) {
            const MeshPart *sp = &src->parts[pi];
            MeshPart *dp = &dst->parts[pi];
            if (!sp->nrm || sp->col || !dp->col || dp->nrm || sp->nverts != dp->nverts)
                continue;
            for (uint32_t v = 0; v < sp->nverts; v++) {
                const float *q = sp->pos + 3 * v, *nn = sp->nrm + 3 * v;
                float p[3], n[3], rgb[3];
                for (int r = 0; r < 3; r++) {
                    p[r] = m[r] * q[0] + m[4 + r] * q[1] + m[8 + r] * q[2] + m[12 + r];
                    n[r] = m[r] * nn[0] + m[4 + r] * nn[1] + m[8 + r] * nn[2];
                }
                float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (l > 1e-6f)
                    for (int r = 0; r < 3; r++)
                        n[r] /= l;
                building_light_at(&s->building_light, p, n, rgb);
                if (f)
                    shadow_facade_darken(f, &s->shade_light, p, rgb);
                memcpy(dp->col + 4 * v, rgb, sizeof rgb);
                dp->col[4 * v + 3] = 1;
            }
        }
    }
}

/* a building's light as it now is (0x2BF180 for a playable one, play 1: its neighbours' shadow shape again if one's
 * top changed, then 0x2CE300 with it; 0x3059D0's 0x2CE300 without one for the blocks behind): the grid vertices'
 * colours, the facade quads with them, the ground-floor and top models */
static void light_building(Scene *s, SceneBuilding *sb, int play)
{
    if (!sb->light)
        return;
    const ShadowFacade *f = play ? &sb->shade : NULL;
    if (play)
        shade_update(s, sb);
    building_light_grid(&sb->b, &s->building_light, f, &s->shade_light, sb->shade_faces, sb->light);
    if (sb->mesh >= 0 && s->meshes[sb->mesh].mesh.nparts)
        light_facade(sb, &s->meshes[sb->mesh].mesh.parts[0]);
    light_models(s, sb, f);
}

/* at the block's end, everything placed and posed: the neighbours, the models' own meshes, the first light */
static void light_buildings(Scene *s)
{
    for (uint32_t i = 0; i < s->nbuildings; i++) {
        shade_neighbours(s, i);
        light_items_make(s, &s->buildings[i]);
    }
    for (uint32_t i = 0; i < s->nscenery; i++)
        light_items_make(s, &s->scenery[i]);
    for (uint32_t i = 0; i < s->nbuildings; i++) {
        light_building(s, &s->buildings[i], 1);
        const SceneBuilding *sb = &s->buildings[i];
        if (getenv("RAMPAGE_TRACE") && (sb->shade_faces[FACE_FRONT] || sb->shade_faces[FACE_RIGHT]))
            printf("building %u (x %.0f .. %.0f): its %s%s%s in the shadow of building %d / %d\n", i, sb->b.min[0],
                   sb->b.max[0], sb->shade_faces[FACE_FRONT] ? "front" : "", sb->shade_faces[FACE_FRONT] &&
                   sb->shade_faces[FACE_RIGHT] ? " and " : "", sb->shade_faces[FACE_RIGHT] ? "right face" : "",
                   sb->shade_nb[0], sb->shade_nb[1]);
    }
    for (uint32_t i = 0; i < s->nscenery; i++)
        light_building(s, &s->scenery[i], 0);
}

/* ---------------------------------------------------------------- posing */

static void pose_instance(Scene *s, Instance *in, double t)
{
    int mk = scene_monster_slot(s, (int)(in - s->instances));
    const SceneMonster *sm = mk >= 0 ? &s->mon[mk] : NULL;
    float w = in->held && in->blend_left > 0 && in->blend_time > 0 ? in->blend_left / in->blend_time : 0;
    for (uint32_t i = 0; i < in->tree.n; i++) {
        TreeNode *nd = &in->tree.nodes[i];
        float tr[3] = { nd->t[0], nd->t[1], nd->t[2] }, q[4] = { nd->q[0], nd->q[1], nd->q[2], nd->q[3] };
        if (in->has_clip)
            clip_pose(&in->clip, t + in->offset, i, tr, q);
        if (in->has_face && in->face_mask && in->face_mask[i])   /* the face clip's, fully (weight 1.0) */
            clip_pose(&in->face, in->face_t * in->face.duration, i, tr, q);
        float wi = w;                                /* the face bones blend on their own timer (0x2FFF10) */
        if (in->has_face && in->face_mask && in->face_mask[i])
            wi = in->face_held && in->face_blend_left > 0 && in->face_blend_time > 0 ?
                 in->face_blend_left / in->face_blend_time : 0;
        if (wi > 0) {                                /* towards the held pose by the share of the blend left */
            const float *h = in->face_mask && in->face_mask[i] && in->has_face ? in->face_held[i] : in->held[i];
            float w = wi;
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
        if (sm && (sm->look_yaw != 0 || sm->look_pitch != 0)) {
            if ((int)i == sm->look_neck) {           /* model x Rz(yaw) (row vectors): about up, at the neck */
                Mat4 r = mat_trs_z(0, 0, 0, sm->look_yaw * 57.2957795f), m = in->node[i];
                float t[3] = { m.m[12], m.m[13], m.m[14] };
                m.m[12] = m.m[13] = m.m[14] = 0;
                m = mat_mul(r, m);
                m.m[12] = t[0]; m.m[13] = t[1]; m.m[14] = t[2];
                in->node[i] = m;
            } else if ((int)i == sm->look_head)      /* Rz(-pitch) x model (row vectors): about its own z */
                in->node[i] = mat_mul(in->node[i], mat_trs_z(0, 0, 0, -sm->look_pitch * 57.2957795f));
        }
    }
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
    if (in->mirrored) {                              /* the game mirrors the pose (0x194BA0); for a mesh that is
                                                        symmetric this is S x the other side's matrix x S, S the
                                                        reflection of x */
        static Mat4 other[256];
        for (uint32_t j = 0; j < nb; j++) {
            int m = in->tree.nodes[j].mirror;
            other[j] = palette[m >= 0 && (uint32_t)m < nb ? (uint32_t)m : j];
        }
        for (uint32_t j = 0; j < nb; j++) {
            Mat4 k = other[j];
            for (int c = 0; c < 4; c++) {            /* S k S: negate row 0 and column 0, the corner twice */
                k.m[4 * c] = -k.m[4 * c];
                k.m[c] = -k.m[c];
            }
            palette[j] = k;
        }
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
    if (sk->cloth && sk->particle) {                 /* hair: simulated in the world (the monster's moves drag it) */
        static float world[256][16];
        int nodes = cloth_nodes(sk->cloth);
        for (int j = 0; j < nodes && j < (int)nb; j++) {
            Mat4 w = mat_mul(in->base, palette[j]);
            memcpy(world[j], w.m, sizeof world[j]);
        }
        /* the world matrix moves only with a step: a posing between steps draws the particles where the last step
         * left them, under that step's matrix (else the hair would trail by the monster's move since) */
        if (!sk->cloth_started) {
            cloth_set_world(sk->cloth, in->base.m);
            cloth_reset(sk->cloth, (const float (*)[16])world);
            sk->cloth_started = 1;
        } else if (s->cloth_dt > 0) {
            cloth_set_world(sk->cloth, in->base.m);
            float wind[3] = { 0, 0, 0 };
            if (sk->has_wind)                        /* 0x3473B0: the flag's wind at the game clock */
                cloth_flag_wind(&sk->wind, s->cloth_time, wind);
            cloth_step(sk->cloth, s->cloth_dt, nodes ? (const float (*)[16])world : NULL, sk->has_wind ? wind : NULL);
        }
        int np = cloth_count(sk->cloth);
        float *lp = malloc(sizeof(float) * 6 * (size_t)(np ? np : 1)), *ln = lp + 3 * np;
        cloth_to_local(sk->cloth, lp, ln);
        float dev = 0;
        for (uint32_t k = 0; k < sk->nverts; k++) {
            int32_t q = sk->particle[k];
            if (q < 0 || q >= np)
                continue;
            float d2 = 0;
            for (int j = 0; j < 3; j++)
                d2 += (sk->pos[3 * k + j] - lp[3 * q + j]) * (sk->pos[3 * k + j] - lp[3 * q + j]);
            if (d2 > dev) dev = d2;
            memcpy(sk->pos + 3 * k, lp + 3 * q, 12);
            memcpy(sk->nrm + 3 * k, ln + 3 * q, 12);
        }
        free(lp);
        if (getenv("RAMPAGE_CLOTH"))
            printf("cloth: %d particles, the farthest %.3f from its rigid place\n", np, sqrtf(dev));
    }
}

static int ends_with(const char *s, const char *tail);

/* a node of the other side as the mirrored pose has it [R 0x194BA0]: the game reflects the bone-local transforms
 * (a pair swapped, each L' = Sz L Sz; Bip01 L' = Sx L Sy, Pelvis Sy L Sz, Lift_Node Sy L Sy), which composes in the
 * model to M'_n = Sx M[mirror(n)] S_n: model x on the left, the bone's own y on the right for Bip01 and Lift_Node,
 * its z for every other node. The place is Sx's alone (the same as S M S); the skin (S M IB S) agrees too */
static int reflect_axis(const Instance *in, int n)
{
    const char *name = n >= 0 && (uint32_t)n < in->tree.n ? in->tree.nodes[n].name : "";
    return ends_with(name, "Bip01") || ends_with(name, "Lift_Node") ? 1 : 2;
}
static Mat4 reflect_node(Mat4 k, int axis)
{
    for (int c = 0; c < 4; c++) {
        k.m[4 * c] = -k.m[4 * c];                    /* model x: row 0 */
        k.m[4 * axis + c] = -k.m[4 * axis + c];      /* the bone's axis: its column */
    }
    return k;
}

/* the monster's hands, posed: kept as the clips have them, then the arm IK (StdIKPairAnimator at monster +0x2588:
 * chains for Bip01 L Hand and Bip01 R Hand, 0x2F9720). Mirrored, a hand as drawn is the other side's node reflected,
 * so the IK works on that one with the target reflected. */
static void monster_ik(SceneMonster *s, Instance *in)
{
    if (!s->ik_ready) {
        s->ik_ready = 1;
        static const char *const HAND[2] = { "Bip01 L Hand", "Bip01 R Hand" };
        for (int h = 0; h < 2; h++) {
            s->ik_chain[h][0] = s->ik_chain[h][1] = s->ik_chain[h][2] = -1;
            for (uint32_t i = 0; i < in->tree.n; i++)
                if (ends_with(in->tree.nodes[i].name, HAND[h])) {
                    int fa = in->tree.nodes[i].parent, ua = fa >= 0 ? in->tree.nodes[fa].parent : -1;
                    if (ua >= 0) {
                        s->ik_chain[h][0] = ua;
                        s->ik_chain[h][1] = fa;
                        s->ik_chain[h][2] = (int)i;
                    }
                }
        }
    }
    for (int h = 0; h < 2; h++) {
        int c[3], ok = s->ik_chain[h][2] >= 0;
        for (int k = 0; k < 3 && ok; k++) {
            int b = s->ik_chain[h][k], m = in->mirrored ? in->tree.nodes[b].mirror : b;
            c[k] = m >= 0 && (uint32_t)m < in->tree.n ? m : b;
        }
        if (!ok)
            continue;
        s->ik_anim[h] = in->mirrored ? reflect_node(in->node[c[2]], reflect_axis(in, c[2])) : in->node[c[2]];
        float len = 0;
        for (int k = 0; k < 2; k++) {                /* upper arm + forearm */
            float d2 = 0;
            for (int j = 0; j < 3; j++) {
                float d = in->node[c[k + 1]].m[12 + j] - in->node[c[k]].m[12 + j];
                d2 += d * d;
            }
            len += sqrtf(d2);
        }
        s->ik_reach[h] = len;
        for (int j = 0; j < 3; j++)
            s->ik_shoulder[h][j] = in->node[c[0]].m[12 + j] * (in->mirrored && j == 0 ? -1.0f : 1.0f);
        if (s->ik_w[h] <= 0 || getenv("RAMPAGE_NOIK"))
            continue;
        float p[3] = { s->ik_pos[h][0], s->ik_pos[h][1], s->ik_pos[h][2] }, r[9];
        memcpy(r, s->ik_rot[h], sizeof r);
        if (in->mirrored) {                          /* the drawn hand Sx N Sz back to the node N: Sx r Sz */
            p[0] = -p[0];
            for (int col = 0; col < 3; col++)
                for (int k = 0; k < 3; k++)
                    if ((k == 0) != (col == 2))
                        r[3 * col + k] = -r[3 * col + k];
        }
        ik_arm(in->node, &in->tree, c, p, r, s->ik_w[h]);
    }
    if (s->feet)                                     /* the feet after the arms (0x2E1338) */
        ik_feet(in->node, &in->tree, in->mirrored, s->feet);
}

int scene_monster_slot(const Scene *s, int inst)
{
    for (int k = 0; inst >= 0 && k < s->nmon; k++)
        if (s->mon[k].inst == inst)
            return k;
    return -1;
}

/* monster slot k's instance, NULL none */
static Instance *mon_inst(const Scene *s, int k)
{
    return k >= 0 && k < s->nmon && s->mon[k].inst >= 0 ? &s->instances[s->mon[k].inst] : NULL;
}

int scene_monster_node(const Scene *s, int k, const char *name, Mat4 *world)
{
    const Instance *in = mon_inst(s, k);
    if (!in)
        return -1;
    for (uint32_t i = 0; i < in->tree.n; i++)
        if (ends_with(in->tree.nodes[i].name, name)) {
            int m = in->mirrored ? in->tree.nodes[i].mirror : (int)i;
            if (m < 0 || (uint32_t)m >= in->tree.n)
                m = (int)i;
            *world = mat_mul(in->base, in->mirrored ? reflect_node(in->node[m], reflect_axis(in, m)) : in->node[m]);
            return 0;
        }
    return -1;
}

int scene_monster_hand(const Scene *s, int k, int hand, Mat4 *world)
{
    const Instance *in = mon_inst(s, k);
    if (!in || hand < 0 || hand > 1 || !s->mon[k].ik_ready || s->mon[k].ik_chain[hand][2] < 0)
        return -1;
    *world = mat_mul(in->base, s->mon[k].ik_anim[hand]);
    return 0;
}

int scene_monster_shoulder(const Scene *s, int k, int hand, float at[3], float *reach)
{
    const Instance *in = mon_inst(s, k);
    if (!in || hand < 0 || hand > 1 || !s->mon[k].ik_ready || s->mon[k].ik_chain[hand][2] < 0)
        return -1;
    const float *m = in->base.m, *p = s->mon[k].ik_shoulder[hand];
    for (int j = 0; j < 3; j++)
        at[j] = m[j] * p[0] + m[4 + j] * p[1] + m[8 + j] * p[2] + m[12 + j];
    *reach = s->mon[k].ik_reach[hand];
    return 0;
}

static void update(Scene *s, double t, int all)
{
    people_sync(s, t);
    army_sync(s, t);
    vehicle_sync(s);
    rail_sync(s);
    edible_sync(s);
    boss_sync(s, t);
    parade_sync(s, t);
    for (uint32_t i = 0; i < s->ninstances; i++) {
        Instance *in = &s->instances[i];
        if (in->tree.n && (all || in->has_clip || in->dynamic)) {   /* a model made later is posed too */
            pose_instance(s, in, t);
            int mk = scene_monster_slot(s, (int)i);
            if (mk >= 0)
                monster_ik(&s->mon[mk], in);
        }
    }
    for (uint32_t i = 0; i < s->nitems; i++) {
        DrawItem *it = &s->items[i];
        Instance *in = &s->instances[it->instance];
        if (!all && !in->has_clip && in->vehicle < 0 && !in->dynamic && scene_monster_slot(s, it->instance) < 0)
            continue;
        if (it->skin) {
            skin_item(s, in, it);
            it->world = in->base;
        } else {
            it->world = it->node >= 0 ? mat_mul(in->base, in->node[it->node]) : in->base;
        }
    }
    s->cloth_time += s->cloth_dt;
    s->cloth_dt = 0;
}

void scene_update(Scene *s, double t) { update(s, t, 0); }

int city_build(City *c, const char *block_name, int night, Scene *s)
{
    memset(s, 0, sizeof *s);
    s->city = &c->bundle;
    for (int k = 0; k < SCENE_MONSTERS; k++)
        s->mon[k].inst = -1;
    s->sky = -1;
    s->ground_inst = -1;
    s->placing_building = -1;
    s->mesh_of_entry = malloc(sizeof *s->mesh_of_entry * (c->bundle.nentries ? c->bundle.nentries : 1));
    for (uint32_t i = 0; i < c->bundle.nentries; i++)
        s->mesh_of_entry[i] = -1;
    char key[128];
    snprintf(key, sizeof key, "BlockDef|%s", block_name);
    Group block = group_sub(group_sub(c->world, "BlockData"), key), ld = group_sub(c->world, night ? "LightingDataNight" : "LightingData"), where;
    if (!block.at)
        return -1;
    Mat4 root = mat_identity();
    static const float WHITE[3] = { 1, 1, 1 };       /* no LightingData: the buildings at full light */
    building_light_init(&s->building_light, WHITE, 0, NULL, NULL, 0, 0, 0);
    shadow_lighting_default(&s->shade_light);
    if (ld.at) {
        const char *backdrop = group_str(ld, "Shape|Backdrop"), *sky = group_str(ld, "Shape|SkyBox");
        if (backdrop) place_model(c, s, backdrop, root, -1);
        if (sky) s->sky = place_model(c, s, sky, root, -1);
        lighting(c, s, ld);
    }
    const char *ground = group_str(block, "Shape|GroundShape");
    if (ground) {
        int gi = place_model(c, s, ground, root, -1);
        if (s->ground_inst < 0)                      /* the play block's, the first built */
            s->ground_inst = gi;
    }
    float row_offset = group_num(block, "BackRowOffset", 10);
    static Record r[MAX_REC];
    uint32_t n = group_values(block, "BuildingInstance|", &where, r, MAX_REC);
    for (uint32_t i = 0; i < n; i++)
        building(c, s, record_group(where, r[i]), row_offset, 0, 0);
    static const char *const objects[] = { "ObjectInstance|", "AttackObjectInstance|", "XObjectInstance|" };
    s->placing_block = 1;
    for (int k = 0; k < 3; k++) {
        n = group_values(block, objects[k], &where, r, MAX_REC);
        for (uint32_t i = 0; i < n; i++)
            place_object(c, s, record_group(where, r[i]), root);
    }
    s->placing_block = 0;
    /* the people (people.c): the block's ActorInstance| entries (SpawnedItem = 1: comes after an earlier one is
     * gone), Traffic's two crowds of the block's occupants' runners, the window occupants later */
    s->owner = c;
    s->folk = people_new(people_clip_seconds, c);
    people_set_templates(s->folk, people_template, c);
    for (int k = 0; k < s->nscripted; k++) {        /* Band.bsf, RoyalGuardHut.bsf */
        if (getenv("RAMPAGE_TRACE"))
            printf("%s at %.1f %.1f\n", s->scripted[k].hut ? "a guard's hut" : "a band", s->scripted[k].pos[0],
                   s->scripted[k].pos[1]);
        if (s->scripted[k].hut)
            s->scripted[k].hut_id = people_add_hut(s->folk, s->scripted[k].t, s->scripted[k].pos);
        else
            people_add_band(s->folk, s->scripted[k].t, s->scripted[k].pos);
    }
    n = group_all(block, "WaterObject|", r, MAX_REC);   /* the harbour, the pools (0x338CC0) */
    for (uint32_t i = 0; i < n && s->nwater < 16; i++) {
        WaterDef wd;
        static const float at[3] = { 0, 0, 0 };
        if (water_read(record_group(block, r[i]), &wd) == 0 && (s->water[s->nwater] = water_new(&wd, at, 0)) != NULL)
            s->nwater++;
    }
    n = group_all(block, "ActorInstance|", r, MAX_REC);
    for (uint32_t i = 0; i < n; i++) {
        Group a = record_group(block, r[i]), t = template_of(c, group_str(a, "Name"));
        if (!a.at || group_num(a, "SpawnedItem", 0) != 0)
            continue;
        if (!t.at) {
            s->missing++;
            continue;
        }
        if (parade_script_ours(group_str(t, "Script|ScriptName")))   /* a biker: parade.c's (main) */
            continue;
        float xyz[3];
        location(a, xyz);
        people_add(s->folk, t, xyz, group_num(a, "ZRotation", 0));
        s->people++;
    }
    Group occ = group_sub(block, "Occupants"), runners[16];
    s->music = group_str(block, "Music");
    s->block = block;
    s->fill_rate = group_num(occ, "FillRate", 0);
    s->spawn_rate = group_num(occ, "SpawnRate", 0.75f);   /* 0x31A540: the Occupants group's, 0.75 unless set */
    s->nonmilitary_rate = group_num(occ, "NonMilitaryRate", 0);
    s->military_start = group_num(occ, "MilitaryStartRate", 0.1f);
    s->military_end = group_num(occ, "MilitaryEndRate", 0.9f);
    s->military_range = group_num(occ, "MilitaryTimeRange", 240);
    n = occ.at ? group_values(occ, "MilitaryInstance|", &where, r, MAX_REC) : 0;
    for (uint32_t i = 0; i < n && s->nmilitary < 16; i++) {
        Group mi = record_group(where, r[i]);
        const char *name = group_str(mi, "OTRef|Name");
        if (!name || !template_of(c, name).at)
            continue;
        s->military[s->nmilitary] = name;
        s->military_freq[s->nmilitary++] = group_num(mi, "Frequency", 1);
    }
    int nrun = 0;
    n = occ.at ? group_values(occ, "OccupantInstance|", &where, r, MAX_REC) : 0;
    for (uint32_t i = 0; i < n && s->noccupants < 16; i++) {
        Group oi = record_group(where, r[i]);
        const char *name = group_str(oi, "OTRef|Name");
        Group t = template_of(c, name);
        if (!t.at)
            continue;
        s->occupant[s->noccupants] = t;
        s->occupant_freq[s->noccupants++] = group_num(oi, "Frequency", 1);
        char rn[96];
        snprintf(rn, sizeof rn, "%sRunner", name);
        Group rt = template_of(c, rn);
        if (rt.at && nrun < 16)
            runners[nrun++] = rt;
    }
    if (nrun) {
        static const float CROWD[2][3] = { { -14, 0.5f, 0 }, { 8, -3, 0 } };
        for (int k = 0; k < 2; k++)
            s->people += people_crowd(s->folk, runners, nrun, CROWD[k]);
    }
    traffic(s, block, 0);
    /* the next two blocks behind it (0x302AF0): slot 1 at y + 41, slot 2 at y + 82 (hard-coded), their ground, every
     * building (0x3059D0) and the objects and waters LoadWhenWaiting (slot 1) or LoadWhenDistant (slot 2) has
     * (0x3050F0), and their traffic; no people. Every BlockList ends in two buffer blocks, so the last block has them too. */
    Group bl = group_sub(group_sub(c->world, "BlockData"), "BlockList");
    Record lr[MAX_REC];
    uint32_t nl = group_items(bl, lr, MAX_REC);
    int at = -1;
    for (uint32_t i = 0; i < nl; i++)                /* its last place: entry 0 repeats entry 1 */
        if (_stricmp(record_str(bl, lr[i]), block_name) == 0)
            at = (int)i;
    for (int slot = 1; slot <= 2 && at >= 0 && at + slot < (int)nl; slot++) {
        const char *next = record_str(bl, lr[at + slot]);
        snprintf(key, sizeof key, "BlockDef|%s", next);
        Group nb = group_sub(group_sub(c->world, "BlockData"), key);
        if (!nb.at)
            continue;
        float y0 = 41.0f * (float)slot;
        Mat4 there = mat_trs_z(0, y0, 0, 0);
        const char *g = group_str(nb, "Shape|GroundShape");
        if (g)
            place_model(c, s, g, there, -1);
        float bro = group_num(nb, "BackRowOffset", 10);
        n = group_values(nb, "BuildingInstance|", &where, r, MAX_REC);
        for (uint32_t i = 0; i < n; i++)
            building(c, s, record_group(where, r[i]), bro, y0, 1);
        for (int k = 0; k < 3; k++) {
            n = group_values(nb, objects[k], &where, r, MAX_REC);
            for (uint32_t i = 0; i < n; i++) {
                Group o = record_group(where, r[i]);
                if (group_num(o, slot == 1 ? "LoadWhenWaiting" : "LoadWhenDistant", 0) != 0)
                    place_object(c, s, o, there);
            }
        }
        /* its waters with the same key (0x3050F0 at 0x305888 .. 0x305960: the reader with f12 = the slot's y > 0):
         * the far-off stand-in, one still flat quad at the surface (0x3381B0), no waves [R] */
        n = group_all(nb, "WaterObject|", r, MAX_REC);
        for (uint32_t i = 0; i < n && s->nwater < 16; i++) {
            Group o = record_group(nb, r[i]);
            WaterDef wd;
            float off[3] = { 0, y0, 0 };
            if (group_num(o, slot == 1 ? "LoadWhenWaiting" : "LoadWhenDistant", 0) != 0 && water_read(o, &wd) == 0 &&
                (s->water[s->nwater] = water_new(&wd, off, 1)) != NULL)
                s->nwater++;
        }
        traffic(s, nb, y0);
    }
    update(s, 0, 1);
    obstacle_boxes(s);
    light_buildings(s);
    return 0;
}

void city_build_left(City *c, const char *block_name, Scene *s)
{
    char key[96];
    snprintf(key, sizeof key, "BlockDef|%s", block_name ? block_name : "");
    Group b = group_sub(group_sub(c->world, "BlockData"), key), where;
    if (!b.at)
        return;
    const char *g = group_str(b, "Shape|GroundShape");
    if (g)
        place_model(c, s, g, mat_trs_z(0, -41, 0, 0), -1);
    float back = group_num(b, "BackRowOffset", 10);
    static Record r[MAX_REC];
    uint32_t n = group_values(b, "BuildingInstance|", &where, r, MAX_REC);
    for (uint32_t i = 0; i < n; i++) {
        Group inst = record_group(where, r[i]), t = template_of(c, group_str(inst, "TemplateName"));
        float left = group_num(inst, "LeftEdgeXPos", 0);
        if (!t.at || left < -50.0f || !(left < 50.0f))   /* [R 0x303EB4 .. 0x303EF0] */
            continue;
        const char *shape = group_str(t, "Shape|RubbleShape");
        if (!shape)
            shape = group_str(t, "RubbleShape");
        if (!shape)
            continue;
        float w = group_num(t, "Width", 0);          /* [R 0x303C90] where the building stood, 41 behind */
        place_model(c, s, shape, mat_trs_z(left + 0.5f * CELL * w, -36 + (group_num(inst, "Row", 0) != 0 ? back : 0),
                                           0, 0), -1);
    }
    update(s, 0, 1);
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
    if (!data || s->nown == (int)(sizeof s->own / sizeof *s->own))
        return free(data), NULL;
    Bundle *b = calloc(1, sizeof *b);
    if (bdp_load(b, data, size) != 0) {
        free(data);
        free(b);
        return NULL;
    }
    b->scope = "Monsters";
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
    if (s->nmon >= SCENE_MONSTERS)
        return -1;
    SceneMonster *sm = &s->mon[s->nmon];
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
        sm->clip = bdp_name(ab, pick->name);
        if (!strncmp(sm->clip, "000", 3))
            sm->clip += 3;
    }
    ClothParticle *cloth = NULL;
    uint32_t ncloth = 0;
    const BdpEntry *cloth_entry = NULL;
    for (uint32_t i = 0; i < mb->nentries; i++)
        if (mb->entries[i].type == ET_CLOTH && !cloth) {
            ncloth = cloth_particles(&mb->entries[i], &cloth);
            cloth_entry = &mb->entries[i];
        }
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
        if (!m.parts[0].bones && cloth_entry && m.nparts) {   /* the cloth mesh: on the simulation */
            sk->cloth = cloth_new(cloth_entry->data, cloth_entry->size);
            if (sk->cloth) {
                sk->particle = malloc(sizeof *sk->particle * (sk->nverts ? sk->nverts : 1));
                for (uint32_t k = 0; k < sk->nverts; k++)
                    sk->particle[k] = -1;
                uint32_t base = 0;                   /* the mesh's own vertex -> particle table (0x1AA960), else a
                                                        vertex is placed by one of its triangles (cloth_triangle) */
                for (uint32_t pi = 0; pi < m.nparts; base += m.parts[pi].nverts, pi++) {
                    const MeshPart *p = &m.parts[pi];
                    ClothMeshPart cp;
                    if (cloth_mesh_part(sk->cloth, (int)pi, e->data, e->size, e->data2, e->size2, &cp) == 0) {
                        if ((uint32_t)cp.nverts == p->nverts)
                            for (uint32_t j = 0; j < p->nverts; j++)
                                sk->particle[base + j] = cp.particle[j];
                        cloth_mesh_part_free(&cp);
                    }
                    for (uint32_t t = 0; t < p->ntris; t++) {
                        const uint32_t *tri = p->tris + 3 * t;
                        int out[3];
                        if (cloth_triangle(sk->cloth, p->pos + 3 * tri[0], p->pos + 3 * tri[1], p->pos + 3 * tri[2], out))
                            for (int k = 0; k < 3; k++)
                                if (sk->particle[base + tri[k]] < 0)
                                    sk->particle[base + tri[k]] = out[k];
                    }
                    for (uint32_t j = 0; j < p->nverts; j++)
                        if (sk->particle[base + j] < 0)
                            sk->particle[base + j] = cloth_nearest(sk->cloth, p->pos + 3 * j);
                }
            }
        }
        push_item(s, scene_add_mesh(s, m, mb), id, -1, sk);
    }
    free(cloth);
    sm->inst = id;
    sm->look_neck = sm->look_head = -1;              /* MD +0xA14: two bones, the neck yaws and the head pitches */
    for (uint32_t i = 0; i < in->tree.n; i++) {
        if (ends_with(in->tree.nodes[i].name, "Bip01 Neck")) sm->look_neck = (int)i;
        if (ends_with(in->tree.nodes[i].name, "Bip01 Head")) sm->look_head = (int)i;
    }
    sm->anims = ab;
    sm->bundle = mb;
    s->nmon++;
    update(s, 0, 1);
    return s->nmon - 1;
}

int scene_monster_clip(Scene *s, int k, const char *suffix, float *speed)
{
    Instance *in = mon_inst(s, k);
    if (!in || !s->mon[k].anims)
        return -1;
    const Bundle *ab = s->mon[k].anims;
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
        s->mon[k].clip = n;
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

int scene_monster_play(Scene *s, int k, const char *clip)
{
    Instance *in = mon_inst(s, k);
    if (!in || !s->mon[k].anims || !clip)
        return -1;
    const Bundle *ab = s->mon[k].anims;
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
        s->mon[k].clip = n;
        return 0;
    }
    return -1;
}

static int ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && _stricmp(s + a - b, tail) == 0;
}

static void mark_subtree(const Tree *t, int root, uint8_t *mask)
{
    mask[root] = 1;
    for (uint32_t i = (uint32_t)root + 1; i < t->n; i++)  /* parents come first */
        if (t->nodes[i].parent >= 0 && mask[t->nodes[i].parent])
            mask[i] = 1;
}

int scene_monster_face(Scene *s, int k, const char *clip, int head)
{
    Instance *in = mon_inst(s, k);
    if (!in || !s->mon[k].anims || !clip)
        return -1;
    const Bundle *ab = s->mon[k].anims;
    const BdpEntry *e = NULL;
    for (uint32_t i = 0; i < ab->nentries && !e; i++) {
        const char *n = bdp_name(ab, ab->entries[i].name);
        if (!strncmp(n, "000", 3))
            n += 3;
        if (ab->entries[i].type == ET_ANIM && _stricmp(n, clip) == 0)
            e = &ab->entries[i];
    }
    Clip c;
    if (!e || clip_read(e, &c) != 0)
        return -1;
    if (in->has_face)
        clip_free(&in->face);
    in->face = c;
    in->has_face = 1;
    in->face_t = 0;
    /* the mask (0x2EA458): the subtree of Bip01 Head; for five skeletons the subtree three nodes above it and their
     * brows and eyes by name; setFacialAnim's head layer adds or takes out the head itself */
    if (!in->face_mask)
        in->face_mask = calloc(in->tree.n ? in->tree.n : 1, 1);
    memset(in->face_mask, 0, in->tree.n);
    int hd = -1;
    for (uint32_t i = 0; i < in->tree.n && hd < 0; i++)
        if (ends_with(in->tree.nodes[i].name, "Bip01 Head"))
            hd = (int)i;
    if (hd < 0)
        return 0;
    static const char *const WIDE[] = { "Eyegore_Skeleton", "Cal_skeleton", "Venus_skeleton", "Natilie_skeleton",
                                        "Jill_skeleton" };
    int wide = 0;
    const Bundle *mb = s->mon[k].bundle;
    for (uint32_t i = 0; mb && i < mb->nentries; i++)
        if (mb->entries[i].type == ET_TREE)
            for (size_t k = 0; k < sizeof WIDE / sizeof *WIDE; k++)
                wide |= ends_with(bdp_name(mb, mb->entries[i].name), WIDE[k]);
    if (wide) {
        int top = hd;
        for (int k = 0; k < 3 && in->tree.nodes[top].parent >= 0; k++)
            top = in->tree.nodes[top].parent;
        mark_subtree(&in->tree, top, in->face_mask);
        static const char *const NAMED[] = { "Bip01 L Brow", "Bip01 R Brow", "Bip01 L Eye", "Bip01 R Eye" };
        for (uint32_t i = 0; i < in->tree.n; i++)
            for (size_t k = 0; k < sizeof NAMED / sizeof *NAMED; k++)
                if (ends_with(in->tree.nodes[i].name, NAMED[k]))
                    in->face_mask[i] = 1;
    } else {
        mark_subtree(&in->tree, hd, in->face_mask);
    }
    in->face_mask[hd] = (uint8_t)(head != 0);
    return 0;
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
        else {
            int k = 0;
            while (k < 4 && (sb->roof_inst[k] < 0 || i != sb->roof_inst[k]))
                k++;
            if (k < 4 || i == sb->top) {
                float bottom, height;
                building_storey(b, b->n, &bottom, &height);    /* where a storey above the last would stand */
                float was = GROUND_FLOOR + STOREY * (float)b->storeys;
                in->base = mat_mul(mat_trs_z(x, 0, bottom - was, 0), sb->rest[i - sb->first]);
            }
            if (k < 4) {                             /* 0x2CD5E0: the roof state's model, or the nearest lower one */
                int st = b->roof < 0 ? 0 : b->roof > 3 ? 3 : b->roof;
                while (st > 0 && sb->roof_inst[st] < 0)
                    st--;
                in->hidden = k != st;
            }
        }
        if (gone && i != sb->rubble)
            in->hidden = 1;
    }
    adornments_place(s, sb, sway);
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
                int face = c < b->depth ? FACE_LEFT : c < 2 * b->depth ? FACE_RIGHT : FACE_FRONT;
                int col = face == FACE_LEFT ? c : face == FACE_RIGHT ? c - b->depth : c - 2 * b->depth;
                for (int k = 0; k < 4; k++) {        /* corners: bottom, top, bottom, top; a fallen storey: no height */
                    const BuildingVert *dv = ls >= 0 ? &b->vert[ls + (k & 1)][face][col + (k >> 1)] : NULL;
                    float at[3];
                    face_point(sb, face, col, CELL * (float)(k >> 1) + (dv ? dv->along : 0), 0, 0,
                               bottom + (k & 1 ? height : 0) + (dv ? dv->up : 0), STOREY, at);
                    memcpy(p->pos + 3 * (v + k), at, sizeof at);
                }
            }
        }
    }
}

/* the move state of the monster that hit the building last (MultiFloor's key in its Scoring) */
static const char *hitter_move(const Scene *s, const SceneBuilding *sb)
{
    return sb->hitter >= 0 && sb->hitter < s->nmon ? s->mon[sb->hitter].move : NULL;
}

void scene_block_left(Scene *s)
{
    s->left = 1;
    for (uint32_t i = 0; i < s->nbuildings; i++) {
        SceneBuilding *sb = &s->buildings[i];
        for (int k = sb->first; k < sb->last; k++)
            s->instances[k].hidden = k != sb->rubble;
    }
    for (int k = 0; k < s->nstreet; k++)             /* the slots behind (41, 82) keep theirs */
        if (s->street[k].world[13] < 20.5f)
            for (int i = s->street[k].first; i < s->street[k].last; i++)
                s->instances[i].hidden = 1;
}

void scene_buildings_update(Scene *s, float dt)
{
    s->water_acc += dt;                              /* the water, a world step at a time: 0x338180 -> 0x336DC0 in */
    if (s->water_acc > 0.25f)                        /* the world's object update (0x284E40), dt gWorld +4 = 1/30 [R] */
        s->water_acc = 0.25f;
    for (; s->water_acc >= 1.0f / 30; s->water_acc -= 1.0f / 30)
        for (int k = 0; k < s->nwater; k++)
            water_step(s->water[k], 1.0f / 30);
    s->light_frame++;                                /* 0x2B8F80: the queue's frame; no grant yet */
    s->light_granted = 0;
    for (uint32_t i = 0; i < s->nbuildings && !s->left; i++) {
        SceneBuilding *sb = &s->buildings[i];
        building_update(&sb->b, dt);
        int moving = sb->b.collapse >= 0 || sb->b.shake_time > 0 || building_sink(&sb->b) > 0 ||
                     (sb->b.state != BUILDING_STANDING && sb->b.state != BUILDING_FLOOR_FALLING);
        if (moving || sb->moving || sb->b.dirty)
            building_place(s, sb);
        /* its light again (0x2BF180: when it changed, +0x154, kept until it is served, or a neighbour's top did,
         * 0x31C860, used up even when refused), through the work queue, before the passes that use it */
        if (moving || sb->moving || sb->b.dirty)
            sb->light_pending = 1;
        int asked = shade_changed(s, sb) || sb->light_pending;
        int relit = asked && light_grant(s, sb);
        if (relit) {
            light_building(s, sb, 1);
            sb->light_pending = 0;
        }
        sb->moving = moving;
        open_windows(s, sb);                         /* the open windows, as they now are (0x2BECC0) */
        if (!sb->b.dirty || sb->mesh < 0) {
            if (moving || relit)
                facade_passes(s, sb);
            continue;
        }
        sb->b.dirty = 0;
        facade_passes(s, sb);
        /* the cells' effects as their state rises (0x2BB580: glass at the first crack, wall debris as it breaks, the
         * heavy debris when destroyed; which of the props' Small / Large goes with which state is a stand-in) */
        for (int ls = 0; ls < sb->b.n; ls++)
            for (int col = 0; col < sb->b.width; col++) {
                int ci = building_cell_index(&sb->b, FACE_FRONT, col), o = sb->b.live[ls];
                if (ci < 0)
                    continue;
                int st = sb->b.cell[o][ci].state, was = sb->seen[o][ci];
                if (st <= was)
                    continue;
                sb->seen[o][ci] = (uint8_t)st;
                if (s->army && sb->b.state == BUILDING_STANDING) {   /* a cell hit: Scoring.cell[state] */
                    float at[3] = { sb->b.min[0] + CELL * (float)col + CELL / 2, sb->b.min[1],
                                    sb->b.min[2] + GROUND_FLOOR + STOREY * (float)ls };
                    army_score_building(s->army, sb->hitter, (int)i, sb->props, ARMY_SCORE_CELL, st, hitter_move(s, sb),
                                        sb->b.width, sb->b.n, NULL, 0, at);
                }
                const char *name = st == 1 ? sb->fx_glass : st == 2 ? sb->fx_wall_small : sb->fx_wall_large;
                float at[3] = { sb->b.min[0] + CELL * (float)col + CELL / 2, sb->b.min[1],
                                sb->b.min[2] + GROUND_FLOOR + STOREY * (float)ls + STOREY / 2 }, in[3] = { 0, 1, 0 };
                scene_fx(s, name, at, in);
            }
        if (sb->b.collapse > 0 && sb->b.collapse != sb->seen_collapse) {   /* a floor comes down */
            int k = sb->b.collapse;
            if (s->army && sb->b.state == BUILDING_FLOOR_FALLING) {
                float at[3] = { (sb->b.min[0] + sb->b.max[0]) / 2, sb->b.min[1], sb->b.min[2] };
                army_score_building(s->army, sb->hitter, (int)i, sb->props, ARMY_SCORE_FLOOR, 0, hitter_move(s, sb),
                                    sb->b.width, sb->b.n, NULL, 0, at);
            }
            float at[3] = { (sb->b.min[0] + sb->b.max[0]) / 2, sb->b.min[1],
                            sb->b.min[2] + GROUND_FLOOR + STOREY * (float)(k - 1) }, in[3] = { 0, 1, 0 };
            scene_fx(s, sb->fx_floor, at, in);
            if (s->edibles && k - 1 < BUILDING_STOREYS)   /* 0x317A70: its edibles gone, those next to it out */
                edible_floor_fell(s->edibles, (int)i, sb->b.live[k - 1], k < sb->b.storeys ? sb->b.live[k] : -1,
                                  k >= 2 ? sb->b.live[k - 2] : -1);
        }
        sb->seen_collapse = sb->b.collapse;
        if (s->army && sb->b.state == BUILDING_CRACKING && sb->seen_state != BUILDING_CRACKING) {   /* destroyed */
            int fronts[BUILDING_CELLS], nf = 0;
            for (int col = 0; col < sb->b.width && nf < BUILDING_CELLS; col++) {
                int ci = building_cell_index(&sb->b, FACE_FRONT, col), top = sb->b.n > 0 ? sb->b.live[sb->b.n - 1] : 0;
                fronts[nf++] = ci >= 0 ? sb->b.cell[top][ci].state : 0;
            }
            float at[3] = { (sb->b.min[0] + sb->b.max[0]) / 2, sb->b.min[1], sb->b.min[2] };
            army_score_building(s->army, sb->hitter, (int)i, sb->props, ARMY_SCORE_BUILDING, 0, hitter_move(s, sb),
                                sb->b.width, sb->b.n, fronts, nf, at);
        }
        if (s->traffic && sb->b.state == BUILDING_CRACKING && sb->seen_state != BUILDING_CRACKING)
            vehicle_building_gone(s->traffic, (int)i);   /* onBuildingDestruction: its roof rockets fly */
        sb->seen_state = sb->b.state;
        for (int ls = 0; (s->folk || s->edibles) && ls < sb->b.n; ls++)   /* 0x317DC0: a front cell at 2 or more */
            for (int col = 0; col < sb->b.width; col++) {
                int ci = building_cell_index(&sb->b, FACE_FRONT, col), o = sb->b.live[ls];
                if (ci < 0 || sb->b.cell[o][ci].state < 2 || sb->broke[o][ci])
                    continue;
                sb->broke[o][ci] = 1;
                if (s->folk)
                    people_cell_broken(s->folk, (int)i, ls);
                if (s->edibles)                      /* its edible comes out (0x317BC0) */
                    edible_cell_broken(s->edibles, (int)i, o, col, sb->b.cell[o][ci].state, sb->b.state);
            }
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

void scene_monster_blend(Scene *s, int k, float seconds)
{
    Instance *in = mon_inst(s, k);
    if (!in)
        return;
    if (!in->pose || !in->held || !in->has_clip || !(seconds > 0)) {
        in->blend_left = 0;
        return;
    }
    memcpy(in->held, in->pose, sizeof *in->held * in->tree.n);
    in->blend_left = in->blend_time = seconds;
}

void scene_cloth_step(Scene *s, float dt)
{
    s->cloth_dt += dt;                               /* for the cloth, at the next posing */
}

void scene_monster_blend_step(Scene *s, int k, float dt)
{
    Instance *in = mon_inst(s, k);
    if (in && in->blend_left > 0)
        in->blend_left -= dt;
    if (in && in->face_blend_left > 0)
        in->face_blend_left -= dt;
}

void scene_monster_face_blend(Scene *s, int k, float seconds)
{
    Instance *in = mon_inst(s, k);
    if (!in)
        return;
    if (!in->pose || !(seconds > 0)) {
        in->face_blend_left = 0;
        return;
    }
    if (!in->face_held)
        in->face_held = calloc(in->tree.n ? in->tree.n : 1, sizeof *in->face_held);
    memcpy(in->face_held, in->pose, sizeof *in->face_held * in->tree.n);
    in->face_blend_left = in->face_blend_time = seconds;
}

int scene_monster_root(const Scene *s, int k, double seconds, float t[3], float q[4])
{
    const Instance *in = mon_inst(s, k);
    if (!in)
        return -1;
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

void scene_monster_place(Scene *s, int k, float x, float y, float z, float deg)
{
    Instance *in = mon_inst(s, k);
    if (in)
        in->base = mat_trs_z(x, y, z, deg);
}

void scene_monster_root_motion(Scene *s, int k, int axes, const float hold[3], float turn)
{
    Instance *in = mon_inst(s, k);
    if (!in)
        return;
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
    for (int k = 0; k < s->nwater; k++)
        water_free(s->water[k]);
    for (uint32_t i = 0; i < s->nmeshes; i++)
        mesh_free(&s->meshes[i].mesh);
    for (uint32_t i = 0; i < s->nitems; i++)
        if (s->items[i].skin) {
            Skin *sk = s->items[i].skin;
            if (sk->cloth) cloth_free(sk->cloth);
            free(sk->particle);
            free(sk->bones); free(sk->weights); free(sk->pos); free(sk->nrm); free(sk);
        }
    for (uint32_t i = 0; i < s->ninstances; i++) {
        tree_free(&s->instances[i].tree);
        if (s->instances[i].has_clip)
            clip_free(&s->instances[i].clip);
        if (s->instances[i].has_face)
            clip_free(&s->instances[i].face);
        free(s->instances[i].face_mask);
        free(s->instances[i].face_held);
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
    free(s->obstacle_of);
    free(s->obstacles);
    if (s->folk)
        people_free(s->folk);
    free(s->looks);
    free(s->slots);
    for (uint32_t i = 0; i < s->nbuildings; i++) {
        free(s->buildings[i].rest);
        free(s->buildings[i].adorn);
        free(s->buildings[i].light);
    }
    free(s->buildings);
    for (uint32_t i = 0; i < s->nscenery; i++) {
        free(s->scenery[i].rest);
        free(s->scenery[i].adorn);
        free(s->scenery[i].light);
    }
    free(s->scenery);
    memset(s, 0, sizeof *s);
}
