/* mesh.c - meshes (bundle entry types 0 and 17, tools/mesh.py, docs/FORMAT_BDP.md "Meshes").
 * Type 0: u32 part count, then 0x50 bytes per part from 0x20 (material u16 at +0x0A, section descriptor at +0x40).
 * Type 17: u16 part count, u16 bone count, u32[bones], 3 x u32, then 0x58 bytes per part (material +0x38, descriptor +0x44).
 * Descriptor: u32 vertex count, u32 ?, u32 format, u32 0, u32 offset in data2, u32 size. The streams start on 16-byte
 * boundaries: position (4 x f32; rigid w != 0 closes a triangle, skinned w = u16 weight + 2 bone bytes), 0x010 normal
 * (3 x s8 + u8; skinned 0x7F closes), 0x040 colour (4 x u8, 0x80 = 1), 0x100 uv (2 x s16, 4.12). Every vertex flagged
 * "closes" makes a triangle with the two before it, its winding by the vertex's place in the section (odd ones turn
 * it), as the GS's strips do: so wound, the triangles face their vertices' normals (40,468 of Chicago's 40,630, 5,514
 * of George's and Lizzie's 5,540) and the back faces can be culled. */
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
    for (uint32_t i = 2; i < count; i++) {
        if (!closes[i])
            continue;
        uint32_t *t = p->tris + 3 * p->ntris++;
        if (i & 1) { t[0] = i - 1; t[1] = i - 2; }
        else { t[0] = i - 2; t[1] = i - 1; }
        t[2] = i;
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
    const BdpEntry *e = bdp_entry_of(b, material, ET_MATERIAL);
    if (e && e->size >= 0x3A)
        return rd16(e->data + 0x38);
    return bdp_entry_of(b, material, ET_TEXTURE) ? material : 0;   /* 2,524 of the 6,292 parts name a texture */
}

int material_color(const Bundle *b, uint32_t material, float rgba[4])
{
    const BdpEntry *e = bdp_entry_of(b, material, ET_MATERIAL);
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 1;
    if (!e || e->size < 0x18)
        return 0;
    memcpy(rgba, e->data + 0x08, 4 * sizeof(float));
    for (int k = 0; k < 4; k++)
        rgba[k] = rgba[k] < 0 ? 0 : rgba[k] > 1 ? 1 : rgba[k];
    return 1;
}

/* the second layer's texture when the material's layer kind (the byte at +0x2, read so by 0x16F030 and 0x25F960) is
 * `kind`: 0x25F960 binds +0x38 to texture slot 0 and +0x3A to slot 1 for kinds 2 .. 8 */
static uint32_t material_layer(const Bundle *b, uint32_t material, int kind)
{
    const BdpEntry *e = bdp_entry_of(b, material, ET_MATERIAL);
    if (!e || e->size < 0x3C || e->data[2] != kind)
        return 0;
    return rd16(e->data + 0x3A);
}

uint32_t material_env(const Bundle *b, uint32_t material)
{
    return material_layer(b, material, MAT_LAYER_REFLECTION);
}

uint32_t material_damage_layer(const Bundle *b, uint32_t material)
{
    return material_layer(b, material, MAT_LAYER_DAMAGE);
}

uint32_t material_flags(const Bundle *b, uint32_t material)
{
    const BdpEntry *e = bdp_entry_of(b, material, ET_MATERIAL);
    return e && e->size >= 6 ? rd16(e->data + 4) : 0;
}

/* 0x16F030's kind 2 table (0x3B1680) gives a material with flag 0x100000 (and not 0x200000) shader 2, 4, 8 or 11
 * (with 0x8000000 117, 119, 76, 83), every one a pass in GS state 2 of 0x3B4240: ALPHA 0x44, TEST 0x5000D (alpha
 * GREATER 0, Z test GEQUAL, Z written), PRMODE 0x78 (blended, fogged); without it they are state 1, opaque [R] */
int material_blended(const Bundle *b, uint32_t material)
{
    const BdpEntry *e = bdp_entry_of(b, material, ET_MATERIAL);
    return e && e->size >= 8 && e->data[2] == 2 && (rd32(e->data + 4) & 0x300000) == 0x100000;
}

/* 0x25F960 (its jump table 0x426940 by the kind at +0x2): kinds 0, 1 no texture, 2 one (the region at +0x3A), 3, 4,
 * 5, 7 two (+0x3C), 6 three and 8 two (+0x3E); with flag 0x100 it copies the 16 bytes there and, when the material has
 * a first texture, makes them its CLAMP_1 (material +0x80) */
int material_region(const Bundle *b, uint32_t material, float region[4])
{
    static const uint8_t at[9] = { 0, 0, 0x3A, 0x3C, 0x3C, 0x3C, 0x3E, 0x3C, 0x3E };
    const BdpEntry *e = bdp_entry_of(b, material, ET_MATERIAL);
    if (!e || e->size < 6 || !(rd32(e->data + 4) & MAT_REGION) || e->data[2] > 8 || !at[e->data[2]] ||
        e->size < at[e->data[2]] + 16u)
        return 0;
    memcpy(region, e->data + at[e->data[2]], 4 * sizeof(float));
    return 1;
}
