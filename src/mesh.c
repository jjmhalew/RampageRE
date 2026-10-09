/* mesh.c - meshes (bundle entry types 0 and 17, tools/mesh.py, docs/FORMAT_BDP.md "Meshes").
 * Type 0: u32 part count, then 0x50 bytes per part from 0x20 (material u16 at +0x0A, section descriptor at +0x40).
 * Type 17: u16 part count, u16 bone count, u32[bones], 3 x u32, then 0x58 bytes per part (material +0x38, descriptor +0x44).
 * Descriptor: u32 vertex count, u32 ?, u32 format, u32 0, u32 offset in data2, u32 size. The streams start on 16-byte
 * boundaries: position (4 x f32; rigid w != 0 closes a triangle, skinned w = u16 weight + 2 bone bytes), 0x010 normal
 * (3 x s8 + u8; skinned 0x7F closes), 0x040 colour (4 x u8, 0x80 = 1), 0x100 uv (2 x s16, 4.12). Every vertex flagged
 * "closes" makes a triangle with the two before it, winding alternating along the strip. */
#include <stdlib.h>
#include <string.h>
#include "data.h"

enum { F_NORMAL = 0x010, F_COLOUR = 0x040, F_UV = 0x100 };

static uint32_t align16(uint32_t n) { return (n + 15) & ~15u; }

static int read_section(const BdpEntry *e, const uint8_t *desc, int skinned, MeshPart *p)
{
    uint32_t count = rd32(desc), fmt = rd32(desc + 8), off = rd32(desc + 16), size = rd32(desc + 20);
    if (rd32(desc + 12) != 0 || (uint64_t)off + size > e->size2 || count == 0)
        return -1;
    const uint8_t *s = e->data2 + off, *end = s + size;
    uint8_t *closes = calloc(count, 1);
    p->nverts = count;
    p->pos = malloc(12u * count);
    if (skinned) {
        p->bones = malloc(2u * count);
        p->weight = malloc(4u * count);
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *v = s + 16 * i;
        p->pos[3 * i] = rdf(v); p->pos[3 * i + 1] = rdf(v + 4); p->pos[3 * i + 2] = rdf(v + 8);
        if (skinned) {
            p->weight[i] = rd16(v + 12) / 4096.0f;
            p->bones[i][0] = v[14] / 4;
            p->bones[i][1] = v[15] / 4;
        } else {
            closes[i] = rdf(v + 12) != 0.0f;
        }
    }
    s += align16(16 * count);
    if (fmt & F_NORMAL) {
        p->nrm = malloc(12u * count);
        for (uint32_t i = 0; i < count; i++) {
            p->nrm[3 * i] = (int8_t)s[4 * i] / 127.0f;
            p->nrm[3 * i + 1] = (int8_t)s[4 * i + 1] / 127.0f;
            p->nrm[3 * i + 2] = (int8_t)s[4 * i + 2] / 127.0f;
            if (skinned)
                closes[i] = s[4 * i + 3] != 0;
        }
        s += align16(4 * count);
    }
    if (fmt & F_COLOUR) {
        p->col = malloc(16u * count);
        for (uint32_t i = 0; i < 4 * count; i++)
            p->col[i] = s[i] >= 128 ? 1.0f : s[i] / 128.0f;
        s += align16(4 * count);
    }
    if (fmt & F_UV) {
        p->uv = malloc(8u * count);
        for (uint32_t i = 0; i < count; i++) {
            p->uv[2 * i] = (int16_t)rd16(s + 4 * i) / 4096.0f;
            p->uv[2 * i + 1] = (int16_t)rd16(s + 4 * i + 2) / 4096.0f;
        }
        s += align16(4 * count);
    }
    if (s > end || (skinned && !(fmt & F_NORMAL))) {
        free(closes);
        return -1;
    }
    p->tris = malloc(12u * count);
    int run = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!closes[i]) { run = 0; continue; }
        run++;
        if (i >= 2) {
            uint32_t *t = p->tris + 3 * p->ntris++;
            if (run & 1) { t[0] = i - 2; t[1] = i - 1; }
            else { t[0] = i - 1; t[1] = i - 2; }
            t[2] = i;
        }
    }
    free(closes);
    return 0;
}

int mesh_read(const BdpEntry *e, Mesh *m)
{
    memset(m, 0, sizeof *m);
    const uint8_t *d = e->data;
    uint32_t count, first, stride, mat_at, desc_at;
    if (e->type == ET_MESH) {
        count = rd32(d); first = 0x20; stride = 0x50; mat_at = 0x0A; desc_at = 0x40;
    } else if (e->type == ET_SKINNED) {
        count = rd16(d); first = 4 + 4u * rd16(d + 2) + 12; stride = 0x58; mat_at = 0x38; desc_at = 0x44;
    } else {
        return -1;
    }
    m->parts = calloc(count ? count : 1, sizeof *m->parts);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *rec = d + first + stride * i;
        MeshPart *p = &m->parts[m->nparts];
        memset(p, 0, sizeof *p);              /* a part that failed before leaves nothing behind for the next */
        p->material = rd16(rec + mat_at);
        if (read_section(e, rec + rd32(rec + desc_at), e->type == ET_SKINNED, p) == 0)
            m->nparts++;
    }
    return 0;
}

void mesh_free(Mesh *m)
{
    for (uint32_t i = 0; i < m->nparts; i++) {
        MeshPart *p = &m->parts[i];
        free(p->pos); free(p->nrm); free(p->uv); free(p->col); free(p->bones); free(p->weight); free(p->tris);
    }
    free(m->parts);
    memset(m, 0, sizeof *m);
}

uint32_t material_texture(const Bundle *b, uint32_t material)
{
    const BdpEntry *e = bdp_entry(b, material);
    if (!e)
        return 0;
    if (e->type == ET_TEXTURE)            /* 2,524 of the 6,292 parts name a texture directly */
        return material;
    if (e->type == ET_MATERIAL && e->size >= 0x3A)
        return rd16(e->data + 0x38);
    return 0;
}
