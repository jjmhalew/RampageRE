/* prb.c - property books (bundle entry type 22, tools/prb.py, docs/FORMAT_BDP.md "Property books"), read in place as the
 * game does (0x1B00E0). Header: 0x1C u32 group count, 0x24 u32 offset of the group table. Group: u32 0, u32 name index,
 * u32 record count, u32 ?, u32 0x14, then 12-byte records {u32 kind, u32 name index, u32 value}; kind & 0xFF: 0 int,
 * 1 float, 2 name, 3 group (value = offset from the containing group); (kind >> 8) & 0xFF == 2: an unnamed list element.
 * A group inherits what it lacks from ParentPropertySet = "Group:Book", which may live in another bundle. */
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "data.h"

static BookResolver resolver;
static void *resolver_ctx;

void prb_set_resolver(BookResolver r, void *ctx) { resolver = r; resolver_ctx = ctx; }

int book_open(Book *bk, const Bundle *b)
{
    for (uint32_t i = 0; i < b->nentries; i++)
        if (b->entries[i].type == ET_BOOK) {
            bk->d = b->entries[i].data;
            bk->b = b;
            return 0;
        }
    bk->d = NULL;
    return -1;
}

Group book_group(const Book *bk, const char *name)
{
    Group g = { bk, 0 };
    if (!bk || !bk->d)
        return g;
    uint32_t count = rd32(bk->d + 0x1C), table = rd32(bk->d + 0x24);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = rd32(bk->d + table + 4 * i);
        if (_stricmp(bdp_name(bk->b, rd32(bk->d + at + 4)), name) == 0) {
            g.at = at;
            return g;
        }
    }
    return g;
}

uint32_t group_count(Group g) { return g.at ? rd32(g.book->d + g.at + 8) : 0; }
const char *group_name(Group g) { return g.at ? bdp_name(g.book->b, rd32(g.book->d + g.at + 4)) : ""; }

Record group_record(Group g, uint32_t i)
{
    const uint8_t *r = g.book->d + g.at + 0x14 + 12 * i;
    Record rec = { rd32(r), bdp_name(g.book->b, rd32(r + 4)), rd32(r + 8) };
    return rec;
}

Group record_group(Group g, Record r)
{
    Group sub = { g.book, (r.kind & 0xFF) == PK_GROUP ? g.at + r.value : 0 };
    return sub;
}

float record_num(Record r)
{
    if ((r.kind & 0xFF) == PK_FLOAT) {
        union { uint32_t u; float f; } v;
        v.u = r.value;
        return v.f;
    }
    return (float)(int32_t)r.value;
}

const char *record_str(Group g, Record r) { return (r.kind & 0xFF) == PK_NAME ? bdp_name(g.book->b, r.value) : NULL; }

static Group parent_of(Group g)
{
    Group none = { NULL, 0 };
    for (uint32_t i = 0, n = group_count(g); i < n; i++) {
        Record r = group_record(g, i);
        if (strcmp(r.name, "ParentPropertySet") == 0 && (r.kind & 0xFF) == PK_NAME) {
            const char *ref = record_str(g, r), *colon = strchr(ref, ':');
            if (!colon || !resolver)
                return none;
            char group[128];
            size_t len = (size_t)(colon - ref) < sizeof group - 1 ? (size_t)(colon - ref) : sizeof group - 1;
            memcpy(group, ref, len);
            group[len] = 0;
            return resolver(resolver_ctx, colon + 1, group);
        }
    }
    return none;
}

/* the group (g or a parent) whose own records hold `key`, and the first such record */
static int find(Group g, const char *key, Group *where, Record *out)
{
    for (int depth = 0; g.at && depth < 16; depth++) {
        for (uint32_t i = 0, n = group_count(g); i < n; i++) {
            Record r = group_record(g, i);
            if (strcmp(r.name, key) == 0) {
                *where = g;
                *out = r;
                return 1;
            }
        }
        g = parent_of(g);
    }
    return 0;
}

int group_get(Group g, const char *key, Record *out) { Group w; return find(g, key, &w, out); }

Group group_sub(Group g, const char *key)
{
    Group w, none = { g.book, 0 };
    Record r;
    return find(g, key, &w, &r) ? record_group(w, r) : none;
}

const char *group_str(Group g, const char *key)
{
    Group w;
    Record r;
    return find(g, key, &w, &r) ? record_str(w, r) : NULL;
}

float group_num(Group g, const char *key, float def)
{
    Group w;
    Record r;
    if (!find(g, key, &w, &r) || ((r.kind & 0xFF) != PK_INT && (r.kind & 0xFF) != PK_FLOAT))
        return def;
    return record_num(r);
}

uint32_t group_items(Group g, Record *out, uint32_t max)
{
    uint32_t k = 0;
    for (uint32_t i = 0, n = group_count(g); i < n && k < max; i++) {
        Record r = group_record(g, i);
        if (((r.kind >> 8) & 0xFF) == 2)
            out[k++] = r;
    }
    return k;
}

uint32_t group_all(Group g, const char *prefix, Record *out, uint32_t max)
{
    uint32_t k = 0;
    size_t len = strlen(prefix);
    for (uint32_t i = 0, n = group_count(g); i < n && k < max; i++) {
        Record r = group_record(g, i);
        if (strncmp(r.name, prefix, len) == 0)
            out[k++] = r;
    }
    return k;
}

uint32_t group_values(Group g, const char *key, Group *where, Record *out, uint32_t max)
{
    for (int depth = 0; g.at && depth < 16; depth++) {
        uint32_t k = 0;
        for (uint32_t i = 0, n = group_count(g); i < n && k < max; i++) {
            Record r = group_record(g, i);
            if (strcmp(r.name, key) == 0)
                out[k++] = r;
        }
        if (k) {
            *where = g;
            return k;
        }
        g = parent_of(g);
    }
    return 0;
}
