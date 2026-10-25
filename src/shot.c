/* shot.c - the window's picture to a PNG (stored zlib blocks: no compressor needed), for --shot and the F12 key; and
 * as raw frames for RAMPAGE_CAPTURE (tools/clip.py). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include <GL/gl.h>
#include "render_gl.h"

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t head[8];
    be32(head, n);
    memcpy(head + 4, type, 4);
    fwrite(head, 1, 8, f);
    fwrite(data, 1, n, f);
    uint32_t c = crc(crc(0, head + 4, 4), data, n);
    be32(head, c);
    fwrite(head, 1, 4, f);
}

int shot_png(const char *path, int w, int h)
{
    uint8_t *px = malloc((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    size_t row = 1 + (size_t)w * 3, raw_n = row * h;
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < h; y++) {                       /* GL rows go bottom up */
        raw[y * row] = 0;
        memcpy(raw + y * row + 1, px + (size_t)(h - 1 - y) * w * 3, (size_t)w * 3);
    }
    size_t blocks = (raw_n + 65534) / 65535, z_n = 2 + raw_n + 5 * blocks + 4;
    uint8_t *z = malloc(z_n), *q = z;
    *q++ = 0x78; *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_n; off += 65535) {
        uint32_t n = raw_n - off < 65535 ? (uint32_t)(raw_n - off) : 65535;
        *q++ = off + n == raw_n;
        *q++ = (uint8_t)n; *q++ = (uint8_t)(n >> 8); *q++ = (uint8_t)~n; *q++ = (uint8_t)(~n >> 8);
        memcpy(q, raw + off, n);
        q += n;
    }
    be32(q, b << 16 | a);
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(px); free(raw); free(z);
        return -1;
    }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    fwrite(sig, 1, 8, f);
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)z_n);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(px); free(raw); free(z);
    return 0;
}

/* the picture's rows as they are (RGB, bottom row first) appended to f: one frame of RAMPAGE_CAPTURE's video.rgb */
int shot_raw(FILE *f, int w, int h)
{
    uint8_t *px = malloc((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    size_t n = fwrite(px, 1, (size_t)w * h * 3, f);
    free(px);
    return n == (size_t)w * h * 3 ? 0 : -1;
}
