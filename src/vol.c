/* vol.c - RAMPAGE.VOL, the game's data archive (tools/pvol.py): 352 named files stored one after the other. Read from
 * the file itself or in place from the disc image (iso.c): every offset is then from Vol.base. */
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "data.h"
#include "iso.h"

int vol_open(Vol *v, const char *path)
{
    uint8_t head[16];
    memset(v, 0, sizeof *v);
    v->f = fopen(path, "rb");
    if (!v->f || fread(head, 1, 16, v->f) != 16)
        return VOL_UNREADABLE;
    if (memcmp(head, "PVOL", 4) != 0) {                 /* a disc image: its RAMPAGE.VOL */
        uint32_t size;
        if (!iso_is(v->f))
            return VOL_UNREADABLE;
        if (!iso_find(v->f, "RAMPAGE.VOL", &v->base, &size) || size < 16 || _fseeki64(v->f, v->base, SEEK_SET) != 0 ||
            fread(head, 1, 16, v->f) != 16 || memcmp(head, "PVOL", 4) != 0)
            return VOL_NOT_IN_IMAGE;
    }
    uint32_t count = rd32(head + 8), names_size = rd32(head + 12);
    uint8_t *index = malloc(12u * count);
    v->names = malloc(names_size + 1);
    v->files = calloc(count, sizeof *v->files);
    if (!index || !v->names || !v->files || fread(index, 12, count, v->f) != count ||
        fread(v->names, 1, names_size, v->f) != names_size)
        return VOL_UNREADABLE;
    v->names[names_size] = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = rd32((uint8_t *)v->names + 4 + 4 * i);
        v->files[i].offset = rd32(index + 12 * i);
        v->files[i].size = rd32(index + 12 * i + 4);
        v->files[i].name = at < names_size ? v->names + at : "";
    }
    v->count = count;
    free(index);
    return 0;
}

const VolFile *vol_find(const Vol *v, const char *name)
{
    for (uint32_t i = 0; i < v->count; i++)
        if (_stricmp(v->files[i].name, name) == 0)
            return &v->files[i];
    return NULL;
}

uint8_t *vol_read(const Vol *v, const char *name, uint32_t *size)
{
    const VolFile *f = vol_find(v, name);
    if (!f)
        return NULL;
    uint8_t *buf = malloc(f->size ? f->size : 1);
    if (!buf || _fseeki64(v->f, v->base + f->offset, SEEK_SET) != 0 || fread(buf, 1, f->size, v->f) != f->size) {
        free(buf);
        return NULL;
    }
    if (size)
        *size = f->size;
    return buf;
}
