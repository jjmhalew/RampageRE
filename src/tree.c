/* tree.c - node trees (bundle entry type 3) and their rest pose (tools/anim.py, docs/FORMAT_BDP.md "Skeletons").
 * Tree: 0x20 u32 node count, 0x2C u32 offset of the node table; node: i32 index, i32 parent, u32 child count,
 * u32 name index, child offsets. Skeletons (first node at 0x40) have a bind pose at 0x3C: 0x50 bytes per node, local
 * translation at +0x34, rotation (x, y, z, w) at +0x40. A model's rest pose is its RawAnim (type 4, kind 1, same
 * name) at time 0: 0x2C record count, 0x30 offset of the record table; record: u32 bone, ..., u32 offset of the
 * translation segments at +0x1C (first: f32[3] p) and of the rotation segments at +0x20 (first: f32[4] q0). */
#include <stdlib.h>
#include <string.h>
#include "data.h"

int tree_read(const Bundle *b, const BdpEntry *te, const BdpEntry *raw, Tree *t)
{
    memset(t, 0, sizeof *t);
    const uint8_t *d = te->data;
    uint32_t n = rd32(d + 0x20), table = rd32(d + 0x2C), first = rd32(d + 0x1C);
    t->nodes = calloc(n ? n : 1, sizeof *t->nodes);
    t->n = n;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *node = d + rd32(d + table + 4 * i);
        t->nodes[i].parent = (int32_t)rd32(node + 4);
        t->nodes[i].name = bdp_name(b, rd32(node + 12));
        t->nodes[i].q[3] = 1.0f;
        if (first == 0x40) {                     /* a skeleton: its bind pose */
            const uint8_t *pose = d + rd32(d + 0x3C) + 0x50 * i;
            for (int k = 0; k < 3; k++) t->nodes[i].t[k] = rdf(pose + 0x34 + 4 * k);
            for (int k = 0; k < 4; k++) t->nodes[i].q[k] = rdf(pose + 0x40 + 4 * k);
            float *ib = t->nodes[i].inverse_bind;            /* x, y, z axis rows and translation: GL's column order */
            for (int col = 0; col < 4; col++) {
                for (int row = 0; row < 3; row++)
                    ib[4 * col + row] = rdf(pose + 4 + 4 * (3 * col + row));
                ib[4 * col + 3] = col == 3 ? 1.0f : 0.0f;
            }
            t->nodes[i].has_bind = 1;
        }
    }
    for (uint32_t i = 0; i < n; i++)
        t->nodes[i].mirror = (int)i;
    uint32_t pairs = rd32(d + 0x34), mt = rd32(d + 0x38);   /* the mirror table: u16 pairs, then the middle nodes */
    if (mt && mt + 4 * pairs <= te->size)
        for (uint32_t k = 0; k < pairs; k++) {
            uint32_t a = d[mt + 4 * k] | d[mt + 4 * k + 1] << 8, b2 = d[mt + 4 * k + 2] | d[mt + 4 * k + 3] << 8;
            if (a < n && b2 < n) {
                t->nodes[a].mirror = (int)b2;
                t->nodes[b2].mirror = (int)a;
            }
        }
    if (raw && raw->size >= 0x38 && rd32(raw->data + 0x20) == 1) {
        const uint8_t *r = raw->data;
        uint32_t count = rd32(r + 0x2C), rtable = rd32(r + 0x30);
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *rec = r + rd32(r + rtable + 4 * i);
            uint32_t bone = rd32(rec);
            if (bone >= n)
                continue;
            const uint8_t *tp = rec + rd32(rec + 0x1C), *qp = rec + rd32(rec + 0x20);
            for (int k = 0; k < 3; k++) t->nodes[bone].t[k] = rdf(tp + 4 * k);
            for (int k = 0; k < 4; k++) t->nodes[bone].q[k] = rdf(qp + 4 * k);
        }
    }
    return 0;
}

void tree_free(Tree *t)
{
    free(t->nodes);
    memset(t, 0, sizeof *t);
}

uint32_t cloth_particles(const BdpEntry *e, ClothParticle **out)
{
    const uint8_t *d = e->data;
    uint32_t parts = rd32(d + 0x34), at = rd32(d + 0x38), total = 0;
    for (uint32_t k = 0; k < parts; k++)
        total += rd32(d + at + 0x8C * k + 0x80);
    ClothParticle *p = calloc(total ? total : 1, sizeof *p);
    uint32_t n = 0;
    for (uint32_t k = 0; k < parts; k++) {
        const uint8_t *rec = d + at + 0x8C * k, *q = d + rd32(rec + 0x88);
        for (uint32_t i = 0, c = rd32(rec + 0x80); i < c; i++, q += 64) {
            ClothParticle *cp = &p[n++];
            for (int j = 0; j < 3; j++) cp->pos[j] = rdf(q + 4 * j);
            cp->nbones = q[0x34] > 4 ? 4 : q[0x34];
            for (int j = 0; j < 4; j++) {
                cp->weights[j] = rdf(q + 0x1C + 4 * j);
                cp->bones[j] = rd16(q + 0x2C + 2 * j);
            }
        }
    }
    *out = p;
    return n;
}
