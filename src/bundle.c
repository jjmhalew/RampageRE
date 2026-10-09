/* bundle.c - Pipeworks bundles (.bdp/.cmp), read by the game at 0x1A8740 (tools/bdp.py, docs/FORMAT_BDP.md).
 * Header: "Pipeworks Bundle File version 1.3", 0x30 version 1300, 0x34 offset of the name table, 0x38 u32[10]
 * segment headers. Segment 0 holds every entry: u16 number, u16 count, u32 entry table, u32 data offset / size,
 * u32 data2 offset / size, then 0x12 bytes per record (u16 entry, u32 data offset, size, data2 offset, size). */
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "data.h"

static const char MAGIC[] = "Pipeworks Bundle File version 1.3";

int bdp_load(Bundle *b, uint8_t *file, uint32_t size)
{
    memset(b, 0, sizeof *b);
    b->file = file;
    b->file_size = size;
    if (size < 0x60 || memcmp(file, MAGIC, sizeof MAGIC - 1) != 0 || rd32(file + 0x30) != 1300)
        return -1;
    uint32_t names_at = rd32(file + 0x34);
    b->nnames = rd32(file + names_at);
    b->names = calloc(b->nnames ? b->nnames : 1, sizeof *b->names);
    for (uint32_t i = 0; i < b->nnames; i++)
        b->names[i] = (const char *)file + names_at + rd32(file + names_at + 4 + 4 * i);
    const uint8_t *seg = file + rd32(file + 0x38);
    uint32_t count = rd16(seg + 2), table = rd32(seg + 4), d_off = rd32(seg + 8), d2_off = rd32(seg + 16);
    b->entries = calloc(count ? count : 1, sizeof *b->entries);
    b->by_name = malloc(sizeof *b->by_name * (b->nnames ? b->nnames : 1));
    for (uint32_t i = 0; i < b->nnames; i++)
        b->by_name[i] = -1;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *rec = seg + 0x18 + 0x12 * i, *ent = file + table + 16 * i;
        BdpEntry *e = &b->entries[i];
        e->index = (uint16_t)i;
        e->type = ent[2];
        e->extra = ent[3];
        e->name = rd32(ent + 4);
        e->size = rd32(rec + 6);
        e->size2 = rd32(rec + 14);
        e->data = e->size ? file + d_off + rd32(rec + 2) : NULL;
        e->data2 = e->size2 ? file + d2_off + rd32(rec + 10) : NULL;
        if (e->name < b->nnames)
            b->by_name[e->name] = (int32_t)i;
    }
    b->nentries = count;
    return 0;
}

void bdp_free(Bundle *b)
{
    free(b->file);
    free((void *)b->names);
    free(b->entries);
    free(b->by_name);
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

int bdp_find_name(const Bundle *b, const char *name)
{
    for (uint32_t i = 0; i < b->nnames; i++)
        if (_stricmp(bdp_name(b, i), name) == 0)
            return (int)i;
    return -1;
}
