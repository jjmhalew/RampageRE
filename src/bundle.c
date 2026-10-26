/* bundle.c - Pipeworks bundles (.bdp/.cmp), read by the game at 0x1A8740 (tools/bdp.py, docs/FORMAT_BDP.md).
 * Header: "Pipeworks Bundle File version 1.3", 0x30 version 1300, 0x34 offset of the name table, 0x38 u32[10]
 * segment headers. Segment 0 holds every entry: u16 number, u16 count, u32 entry table, u32 data offset / size,
 * u32 data2 offset / size, then 0x12 bytes per record (u16 entry, u32 data offset, size, data2 offset, size).
 * The Wii's (.bdg / .cmg, "version 1.4", 1400) are the same, big endian but for the name table. */
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "data.h"

static const char MAGIC[] = "Pipeworks Bundle File version 1.";   /* 3 the PS2's, 4 the Wii's */

/* ---- the Wii's entries into the PS2's layout (docs/FORMAT_BDP.md "The Wii's bundles"). A map marks where each
 * field starts and how wide it is (4, 2; 0 = a byte or inside a field), then every marked field is swapped. The
 * fields are the ones the port reads (tree.c, anim.c, mesh.c's materials, moves.c, director.c); the rest of an entry
 * is left as it is */
typedef struct { uint8_t *w; uint32_t n; } Map;

static void mark(Map *m, uint32_t from, uint32_t to, uint8_t width)   /* [from, to) as fields of width (0: bytes) */
{
    if (to > m->n)
        to = m->n;
    for (uint32_t i = from; i < to; i++)
        m->w[i] = width && (i - from) % width == 0 && i + width <= to ? width : 0;
}

static void swap_marked(uint8_t *d, const Map *m)
{
    for (uint32_t i = 0; i < m->n; i++)
        if (m->w[i] == 4) {
            uint8_t t = d[i]; d[i] = d[i + 3]; d[i + 3] = t;
            t = d[i + 1]; d[i + 1] = d[i + 2]; d[i + 2] = t;
        } else if (m->w[i] == 2) {
            uint8_t t = d[i]; d[i] = d[i + 1]; d[i + 1] = t;
        }
}

static uint32_t be32_at(const Map *m, const uint8_t *d, uint32_t o) { return o + 4 <= m->n ? rd32be(d + o) : 0; }
static uint32_t be16_at(const Map *m, const uint8_t *d, uint32_t o) { return o + 2 <= m->n ? rd16be(d + o) : 0; }

/* node tree: the header words, the nodes, the node and by-name tables and the bind pose 32-bit; the mirror table
 * u16 (tools/anim.py Skeleton) */
static void map_tree(Map *m, const uint8_t *d)
{
    uint32_t first = be32_at(m, d, 0x1C), n = be32_at(m, d, 0x20), mt = be32_at(m, d, 0x38);
    mark(m, 0x1C, m->n, 4);
    if (mt && first >= 0x3C && n < 0x10000)
        mark(m, mt, mt + ((2 * n + 3) & ~3u), 2);
}

/* clip: KeyInterpAnim (kind 2) its tracks walked, keys u16 / s16 and the bytes of the track heads; RawAnim (kind 1)
 * all 32-bit; the skeleton's name index u16 (tools/anim.py Animation, RawAnim) */
static void map_clip(Map *m, const uint8_t *d)
{
    uint32_t kind = be32_at(m, d, 0x20);
    mark(m, 0x1C, 0x28, 4);
    mark(m, 0x28, 0x2C, 2);
    if (kind == 1) {
        mark(m, 0x2C, m->n, 4);
        return;
    }
    if (kind != 2)
        return;
    mark(m, 0x2C, 0x44, 4);
    uint32_t bones = be32_at(m, d, 0x2C), mask = be32_at(m, d, 0x34);
    uint32_t at = be32_at(m, d, 0x38), rot = be32_at(m, d, 0x3C);
    mark(m, mask, at, 4);                                /* the channel mask: u32 words */
    for (uint32_t i = 0; i < bones && at + 0x10 <= m->n; i++) {
        uint32_t keys = d[at + 0xD];
        mark(m, at, at + 12, 4);                         /* f32[3] scale, then u8 bone, count, count - 1, 0 */
        mark(m, at + 12, at + 16, 0);
        mark(m, at + 16, at + 16 + 8 * keys, 2);         /* u16 time, s16 x, y, z */
        at += 16 + 8 * keys;
    }
    for (uint32_t i = 0; i < bones && rot + 2 <= m->n; i++) {
        uint32_t keys = d[rot + 1];
        mark(m, rot + 2, rot + 2 + 8 * keys, 2);         /* u8 bone, count, then u16 time, s16 a, b, c */
        rot += 2 + 8 * keys;
    }
}

/* material (0x70 bytes on the Wii): u16 own name, u8 kind, u8, u32 flags, f32 colours, u16 texture name indices by
 * kind, then with flag 0x100 the region's four floats (mesh.c material_texture .. material_region) */
static void map_material(Map *m, const uint8_t *d)
{
    static const uint8_t textures[9] = { 0, 0, 1, 2, 2, 2, 3, 2, 2 };
    static const uint8_t region[9] = { 0, 0, 0x3A, 0x3C, 0x3C, 0x3C, 0x3E, 0x3C, 0x3E };
    if (m->n < 0x38)
        return;
    uint8_t kind = d[2] <= 8 ? d[2] : 0;
    mark(m, 0, 2, 2);
    mark(m, 4, 0x38, 4);
    mark(m, 0x38, 0x38 + 2u * textures[kind], 2);
    if (be32_at(m, d, 4) & 0x100 && region[kind])
        mark(m, region[kind], region[kind] + 16u, 4);
}

/* MONSTER_DATA (tools/monster.py; the fields moves.c and director.c read): 32-bit but for the u16 coded item
 * offsets, the attack items' parts and count (+0x8 .. +0x13) and the effect items' slot, effect, on, flags (+0x8 ..
 * +0xF), each state's +0xC / +0xE and its 16 bytes at +0x18, each transition's priority and count (+0x1C, +0x1E),
 * each state node's clip events (u16 count, first at +0x28), u8 +0x60, +0x61 and u16 +0x62 .. +0x66 (found by
 * matching George's PS2 and Wii records field by field), the attachment nodes' bone and index (+0x34), the shapes'
 * bone and part (+0x14, +0x16), the body parts' count
 * and bones (+0xC ..), the effect nodes' bone and mirror (+0x30, +0x32); the bytes after the states (the next-hop
 * table) as they are */
static void map_monster(Map *m, const uint8_t *d)
{
    if (m->n < 0xAA8 || rd32be(d) != 11)
        return;
    uint32_t graph = be32_at(m, d, 0xA54), nstates = be32_at(m, d, graph + 0x2C), at = be32_at(m, d, graph + 0x30);
    mark(m, 0, at < m->n ? at : m->n, 4);
    uint32_t nitems = be32_at(m, d, 0xA60), block = be32_at(m, d, 0xA64), offsets = be32_at(m, d, 0xA68);
    for (uint32_t i = 0; i < nitems && offsets + 2 * i + 2 <= m->n; i++) {
        uint32_t o = block + be16_at(m, d, offsets + 2 * i), type = be32_at(m, d, o);
        if (type == 0)
            mark(m, o + 8, o + 0x14, 2);
        else if (type == 12)
            mark(m, o + 8, o + 0x10, 2);
    }
    mark(m, offsets, offsets + 2 * nitems, 2);
    uint32_t nnodes = be32_at(m, d, 0xA4C), nodes = be32_at(m, d, 0xA50);
    for (uint32_t k = 0; k < nnodes && k < 4096; k++) {
        uint32_t nd = nodes + 0x84 * k;
        mark(m, nd + 0x28, nd + 0x2C, 2);                /* the clip events: u16 count, u16 first */
        mark(m, nd + 0x60, nd + 0x62, 0);
        mark(m, nd + 0x62, nd + 0x68, 2);
    }
    uint32_t natt = be32_at(m, d, 0xA84), att = be32_at(m, d, 0xA88);
    for (uint32_t k = 0; k < natt && k < 4096; k++)      /* attachment nodes: u16 bone, u16 index at +0x34 */
        mark(m, att + 0x38 * k + 0x34, att + 0x38 * k + 0x38, 2);
    uint32_t nsh = be32_at(m, d, 0xA44), sh = be32_at(m, d, 0xA48);
    for (uint32_t k = 0; k < nsh && k < 4096; k++)
        mark(m, sh + 24 * k + 0x14, sh + 24 * k + 0x18, 2);
    uint32_t nparts = be32_at(m, d, 0xA6C), parts = be32_at(m, d, 0xA74);
    for (uint32_t k = 0; k < nparts && k < 4096; k++) {
        uint32_t o = be32_at(m, d, parts + 4 * k);
        mark(m, o + 0xC, o + 0xE + 2 * be16_at(m, d, o + 0xC), 2);
    }
    uint32_t nsets = be32_at(m, d, 0xA78), sets = be32_at(m, d, 0xA80);
    for (uint32_t k = 0; k < nsets && k < 4096; k++) {
        uint32_t rec = be32_at(m, d, sets + 4 * k), cnt = be32_at(m, d, rec), arr = be32_at(m, d, rec + 4);
        for (uint32_t j = 0; j < cnt && j < 4096; j++)
            mark(m, arr + 0x34 * j + 0x30, arr + 0x34 * j + 0x34, 2);
    }
    for (uint32_t k = 0; k < nstates && k < 4096 && at + 0x2C <= m->n; k++) {
        uint32_t ntrans = be16_at(m, d, at + 0xE);
        mark(m, at, at + 0x2C, 4);
        mark(m, at + 0xC, at + 0x10, 2);
        mark(m, at + 0x18, at + 0x28, 0);
        for (uint32_t t = 0; t < ntrans; t++) {
            uint32_t q = at + 0x2C + 0x74 * t;
            mark(m, q, q + 0x74, 4);
            mark(m, q + 0x1C, q + 0x20, 2);
        }
        at += 0x2C + 0x74 * ntrans;
    }
}

static void swap_entry(const Bundle *b, BdpEntry *e)
{
    const char *name = bdp_name(b, e->name);
    size_t len = strlen(name);
    int monster = e->type == ET_RAW && len >= 12 && strcmp(name + len - 12, "MONSTER_DATA") == 0;
    if (!e->data || (e->type != ET_TREE && e->type != ET_ANIM && e->type != ET_MATERIAL && !monster))
        return;                                          /* raw files (scripts, fonts, .ifl) stay little endian */
    uint8_t *d = (uint8_t *)e->data;
    Map m = { calloc(e->size ? e->size : 1, 1), e->size };
    if (!m.w)
        return;
    if (e->type == ET_TREE) map_tree(&m, d);
    else if (e->type == ET_ANIM) map_clip(&m, d);
    else if (e->type == ET_MATERIAL) map_material(&m, d);
    else map_monster(&m, d);
    swap_marked(d, &m);
    free(m.w);
}

int bdp_load(Bundle *b, uint8_t *file, uint32_t size)
{
    memset(b, 0, sizeof *b);
    b->file = file;
    b->file_size = size;
    if (size < 0x60 || memcmp(file, MAGIC, sizeof MAGIC - 1) != 0)
        return -1;
    b->wii = file[sizeof MAGIC - 1] == '4';
    uint32_t (*r32)(const uint8_t *) = b->wii ? rd32be : rd32;
    uint16_t (*r16)(const uint8_t *) = b->wii ? rd16be : rd16;
    if ((file[sizeof MAGIC - 1] != '3' && !b->wii) || r32(file + 0x30) != (b->wii ? 1400u : 1300u))
        return -1;
    uint32_t names_at = r32(file + 0x34);                /* the name table: little endian on the Wii too */
    b->nnames = rd32(file + names_at);
    b->names = calloc(b->nnames ? b->nnames : 1, sizeof *b->names);
    for (uint32_t i = 0; i < b->nnames; i++)
        b->names[i] = (const char *)file + names_at + rd32(file + names_at + 4 + 4 * i);
    const uint8_t *seg = file + r32(file + 0x38);
    uint32_t count = r16(seg + 2), table = r32(seg + 4), d_off = r32(seg + 8), d2_off = r32(seg + 16);
    b->entries = calloc(count ? count : 1, sizeof *b->entries);
    b->by_name = malloc(sizeof *b->by_name * (b->nnames ? b->nnames : 1));
    b->mat_by_name = malloc(sizeof *b->mat_by_name * (b->nnames ? b->nnames : 1));
    b->tex_by_name = malloc(sizeof *b->tex_by_name * (b->nnames ? b->nnames : 1));
    for (uint32_t i = 0; i < b->nnames; i++)
        b->by_name[i] = b->mat_by_name[i] = b->tex_by_name[i] = -1;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *rec = seg + 0x18 + 0x12 * i, *ent = file + table + 16 * i;
        BdpEntry *e = &b->entries[i];
        e->index = (uint16_t)i;
        e->type = ent[2];
        e->extra = ent[3];
        e->name = r32(ent + 4);
        e->size = r32(rec + 6);
        e->size2 = r32(rec + 14);
        e->data = e->size ? file + d_off + r32(rec + 2) : NULL;
        e->data2 = e->size2 ? file + d2_off + r32(rec + 10) : NULL;
        if (e->name < b->nnames) {
            b->by_name[e->name] = (int32_t)i;
            if (e->type == ET_MATERIAL) b->mat_by_name[e->name] = (int32_t)i;
            if (e->type == ET_TEXTURE) b->tex_by_name[e->name] = (int32_t)i;
        }
    }
    b->nentries = count;
    if (b->wii)
        for (uint32_t i = 0; i < count; i++)
            swap_entry(b, &b->entries[i]);
    return 0;
}

void bdp_free(Bundle *b)
{
    free(b->file);
    free((void *)b->names);
    free(b->entries);
    free(b->by_name);
    free(b->mat_by_name);
    free(b->tex_by_name);
    memset(b, 0, sizeof *b);
}

const char *bdp_name(const Bundle *b, uint32_t i)
{
    if (i >= b->nnames)
        return "";
    const char *s = b->names[i];
    return *s == 0x13 ? s + 1 : s;
}

const BdpEntry *bdp_entry(const Bundle *b, uint32_t name_index)
{
    if (name_index >= b->nnames || b->by_name[name_index] < 0)
        return NULL;
    return &b->entries[b->by_name[name_index]];
}

const BdpEntry *bdp_entry_of(const Bundle *b, uint32_t name_index, int type)
{
    if (name_index >= b->nnames)
        return NULL;
    if (type == ET_MATERIAL || type == ET_TEXTURE) {
        int32_t i = (type == ET_MATERIAL ? b->mat_by_name : b->tex_by_name)[name_index];
        return i >= 0 ? &b->entries[i] : NULL;
    }
    for (uint32_t i = 0; i < b->nentries; i++)
        if (b->entries[i].name == name_index && b->entries[i].type == type)
            return &b->entries[i];
    return NULL;
}

int bdp_find_name(const Bundle *b, const char *name)
{
    for (uint32_t i = 0; i < b->nnames; i++)
        if (_stricmp(bdp_name(b, i), name) == 0)
            return (int)i;
    return -1;
}
