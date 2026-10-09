/* tex.c - textures (bundle entry type 9) to RGBA8 (tools/tex.py). Header: u32 kind, u8 GS pixel format (0x00 PSMCT32,
 * 0x02 PSMCT16, 0x13 PSMT8), u8 flags at 0x07 (0x40 = stored in GS memory order), u16 width, u16 height, ..., u32 name
 * index of the palette at 0x20. Pixels (mip 0 first) are the entry's data2. Palettes (type 13): 256 RGBA in GS CSM1 order,
 * alpha 0x80 = opaque; "_Alpha8" and "_Intensity8" are the engine's own. */
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

int tex_decode(const Bundle *b, const BdpEntry *e, uint8_t **rgba, int *w_out, int *h_out)
{
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
