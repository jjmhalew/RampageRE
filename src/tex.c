/* tex.c - textures (bundle entry type 9) to RGBA8 (tools/tex.py). Header: u32 kind, u8 GS pixel format (0x00 PSMCT32,
 * 0x02 PSMCT16, 0x13 PSMT8), u8 flags at 0x07 (0x40 = stored in GS memory order), u16 width, u16 height, ..., u32 name
 * index of the palette at 0x20. Pixels (mip 0 first) are the entry's data2. Palettes (type 13): 256 RGBA in GS CSM1 order,
 * alpha 0x80 = opaque; "_Alpha8" and "_Intensity8" are the engine's own. The Wii's (Bundle.wii) are GX: gx_decode. */
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "data.h"

/* PSMT8 pixels written to GS memory as PSMCT32 back to rows */
static void unswizzle8(const uint8_t *src, uint8_t *out, int w, int h)
{
    for (int y = 0; y < h; y++) {
        int block_row = (y & ~0xF) * w, swap = (((y + 2) >> 2) & 1) * 4;
        int column = ((((y & ~3) >> 1) + (y & 1)) & 7) * w * 2, byte_y = (y >> 1) & 1;
        for (int x = 0; x < w; x++)
            out[y * w + x] = src[block_row + (x & ~0xF) * 2 + column + ((x + swap) & 7) * 4 + byte_y + ((x >> 2) & 2)];
    }
}

static uint8_t alpha2(uint8_t a) { return a >= 128 ? 255 : (uint8_t)(a * 2); }

/* ---- the Wii's GX textures (tools/tex.py, docs/FORMAT_BDP.md "The Wii's bundles"): the pixels in tiles, a tile's
 * rows one after the other, the tiles left to right, top to bottom; mip 0 first */
enum { GX_I4 = 0, GX_I8 = 1, GX_IA8 = 3, GX_RGB5A3 = 5, GX_RGBA8 = 6, GX_CMPR = 14 };

static void rgb565(uint16_t c, uint8_t *o)
{
    o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    o[2] = (uint8_t)((c & 31) * 255 / 31);
    o[3] = 255;
}

/* one tile's pixels, tw x th, row by row, into tile (RGBA) */
static void gx_tile(int fmt, const uint8_t *p, uint8_t *tile)
{
    switch (fmt) {
    case GX_I4:                                          /* 8 x 8, 4 bits; intensity into alpha too, as GX does */
        for (int k = 0; k < 64; k++) {
            uint8_t v = (uint8_t)((k & 1 ? p[k / 2] & 15 : p[k / 2] >> 4) * 17);
            memset(tile + 4 * k, v, 4);
        }
        break;
    case GX_I8:                                          /* 8 x 4 */
        for (int k = 0; k < 32; k++)
            memset(tile + 4 * k, p[k], 4);
        break;
    case GX_IA8:                                         /* 4 x 4: alpha, intensity */
        for (int k = 0; k < 16; k++) {
            memset(tile + 4 * k, p[2 * k + 1], 3);
            tile[4 * k + 3] = p[2 * k];
        }
        break;
    case GX_RGB5A3:                                      /* 4 x 4, u16: RGB555 opaque, or A3 RGB444 */
        for (int k = 0; k < 16; k++) {
            uint16_t c = rd16be(p + 2 * k);
            uint8_t *o = tile + 4 * k;
            if (c & 0x8000) {
                o[0] = (uint8_t)(((c >> 10) & 31) * 255 / 31); o[1] = (uint8_t)(((c >> 5) & 31) * 255 / 31);
                o[2] = (uint8_t)((c & 31) * 255 / 31); o[3] = 255;
            } else {
                o[0] = (uint8_t)(((c >> 8) & 15) * 17); o[1] = (uint8_t)(((c >> 4) & 15) * 17);
                o[2] = (uint8_t)((c & 15) * 17); o[3] = (uint8_t)(((c >> 12) & 7) * 255 / 7);
            }
        }
        break;
    case GX_RGBA8:                                       /* 4 x 4: the AR pairs, then the GB pairs */
        for (int k = 0; k < 16; k++) {
            uint8_t *o = tile + 4 * k;
            o[0] = p[2 * k + 1]; o[1] = p[32 + 2 * k]; o[2] = p[33 + 2 * k]; o[3] = p[2 * k];
        }
        break;
    case GX_CMPR:                                        /* 8 x 8: four 4 x 4 DXT1 blocks in Z order */
        for (int s = 0; s < 4; s++) {
            const uint8_t *q = p + 8 * s;
            uint16_t c0 = rd16be(q), c1 = rd16be(q + 2);
            uint8_t col[4][4];
            rgb565(c0, col[0]);
            rgb565(c1, col[1]);
            for (int j = 0; j < 3; j++) {
                col[2][j] = (uint8_t)(c0 > c1 ? (2 * col[0][j] + col[1][j]) / 3 : (col[0][j] + col[1][j]) / 2);
                col[3][j] = (uint8_t)(c0 > c1 ? (col[0][j] + 2 * col[1][j]) / 3 : 0);
            }
            col[2][3] = 255;
            col[3][3] = c0 > c1 ? 255 : 0;
            int base = (s >> 1) * 32 + (s & 1) * 4;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    memcpy(tile + 4 * (base + y * 8 + x), col[(q[4 + y] >> (6 - 2 * x)) & 3], 4);
        }
        break;
    }
}

static int gx_decode(const BdpEntry *e, uint8_t **rgba, int *w_out, int *h_out)
{
    if (e->size < 0x10)
        return -1;
    int fmt = (int)rd32(e->data + 4), w = rd16(e->data + 8), h = rd16(e->data + 10);   /* swapped by bdp_load */
    int tw = fmt == GX_I4 || fmt == GX_I8 || fmt == GX_CMPR ? 8 : 4, th = fmt == GX_I4 || fmt == GX_CMPR ? 8 : 4;
    int bits = fmt == GX_I4 || fmt == GX_CMPR ? 4 : fmt == GX_I8 ? 8 : fmt == GX_RGBA8 ? 32 : 16;
    if ((fmt != GX_I4 && fmt != GX_I8 && fmt != GX_IA8 && fmt != GX_RGB5A3 && fmt != GX_RGBA8 && fmt != GX_CMPR) ||
        w <= 0 || h <= 0)
        return -1;
    int tiles_x = (w + tw - 1) / tw, tiles_y = (h + th - 1) / th, tile_bytes = tw * th * bits / 8;
    if (e->size2 < (uint32_t)(tiles_x * tiles_y * tile_bytes))
        return -1;
    uint8_t *out = malloc((size_t)w * h * 4), tile[8 * 8 * 4];
    if (!out)
        return -1;
    const uint8_t *p = e->data2;
    for (int ty = 0; ty < tiles_y; ty++)
        for (int tx = 0; tx < tiles_x; tx++, p += tile_bytes) {
            gx_tile(fmt, p, tile);
            for (int y = 0; y < th && ty * th + y < h; y++) {
                int x0 = tx * tw, n = x0 + tw <= w ? tw : w - x0;
                memcpy(out + 4 * ((size_t)(ty * th + y) * w + x0), tile + 4 * y * tw, (size_t)4 * n);
            }
        }
    *rgba = out;
    *w_out = w;
    *h_out = h;
    return 0;
}

int tex_decode(const Bundle *b, const BdpEntry *e, uint8_t **rgba, int *w_out, int *h_out)
{
    if (e && b->wii && e->type == ET_TEXTURE && e->data2)
        return gx_decode(e, rgba, w_out, h_out);
    if (!e || e->type != ET_TEXTURE || e->size < 0x24 || !e->data2)
        return -1;
    const uint8_t *d = e->data, *px = e->data2;
    int psm = d[4], flags = d[7], w = rd16(d + 8), h = rd16(d + 10);
    uint8_t *out = malloc((size_t)w * h * 4);
    if (!out)
        return -1;
    if (psm == 0x13) {
        if (e->size2 < (uint32_t)(w * h)) { free(out); return -1; }
        uint8_t pal[256][4];
        uint32_t pal_name = rd32(d + 0x20);
        const char *pn = bdp_name(b, pal_name);
        const BdpEntry *pe = bdp_entry(b, pal_name);
        if (strcmp(pn, "_Alpha8") == 0 || strcmp(pn, "_Intensity8") == 0) {
            int alpha = pn[1] == 'A';
            for (int i = 0; i < 256; i++) {
                pal[i][0] = pal[i][1] = pal[i][2] = alpha ? 255 : (uint8_t)i;
                pal[i][3] = alpha ? (uint8_t)i : 255;
            }
        } else if (pe && pe->type == ET_PALETTE && pe->size2 >= 1024) {
            for (int i = 0; i < 256; i++) {      /* CSM1: entries 8-15 and 16-23 of each 32 swapped */
                const uint8_t *c = pe->data2 + 4 * ((i & ~0x18) | ((i & 8) << 1) | ((i & 16) >> 1));
                pal[i][0] = c[0]; pal[i][1] = c[1]; pal[i][2] = c[2]; pal[i][3] = alpha2(c[3]);
            }
        } else {
            free(out);
            return -1;
        }
        uint8_t *index = malloc((size_t)w * h);
        if (flags & 0x40)
            unswizzle8(px, index, w, h);
        else
            memcpy(index, px, (size_t)w * h);
        for (int i = 0; i < w * h; i++)
            memcpy(out + 4 * i, pal[index[i]], 4);
        free(index);
    } else if (psm == 0x00) {
        if (e->size2 < (uint32_t)(w * h * 4)) { free(out); return -1; }
        for (int i = 0; i < w * h; i++) {
            memcpy(out + 4 * i, px + 4 * i, 3);
            out[4 * i + 3] = alpha2(px[4 * i + 3]);
        }
    } else if (psm == 0x02) {
        if (e->size2 < (uint32_t)(w * h * 2)) { free(out); return -1; }
        for (int i = 0; i < w * h; i++) {
            uint16_t p = rd16(px + 2 * i);
            out[4 * i] = (uint8_t)((p & 31) << 3);
            out[4 * i + 1] = (uint8_t)(((p >> 5) & 31) << 3);
            out[4 * i + 2] = (uint8_t)(((p >> 10) & 31) << 3);
            out[4 * i + 3] = (p & 0x8000) ? 255 : 0;
        }
    } else {
        free(out);
        return -1;
    }
    *rgba = out;
    *w_out = w;
    *h_out = h;
    return 0;
}
