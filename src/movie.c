/* movie.c - the game's movies (*.pss, movie.h): the program stream, an MPEG-2 video decoder (ISO 13818-2, the parts
 * Main Profile frame pictures use) and the SPU ADPCM sound (docs/MOVIE_NOTES.md). */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "plat.h"
#include "movie.h"

/* ---------------------------------------------------------------- the variable length codes (Annex B) */
typedef struct { const char *code; int val; } Code;
typedef struct { int maxlen; int32_t *t; } Vlc;      /* t[peek(maxlen)] = val << 8 | length, 0 none */

enum { MB_QUANT = 1, MB_FWD = 2, MB_BWD = 4, MB_PATTERN = 8, MB_INTRA = 16 };
enum { MBA_ESCAPE = 34, MBA_STUFFING = 35 };
#define RL(run, level) ((run) | (level) << 6)
#define DCT_EOB (1 << 20)
#define DCT_ESC (2 << 20)

static const Code MBA[] = {           /* B-1 macroblock_address_increment */
    { "1", 1 }, { "011", 2 }, { "010", 3 }, { "0011", 4 }, { "0010", 5 }, { "00011", 6 }, { "00010", 7 },
    { "0000111", 8 }, { "0000110", 9 }, { "00001011", 10 }, { "00001010", 11 }, { "00001001", 12 },
    { "00001000", 13 }, { "00000111", 14 }, { "00000110", 15 }, { "0000010111", 16 }, { "0000010110", 17 },
    { "0000010101", 18 }, { "0000010100", 19 }, { "0000010011", 20 }, { "0000010010", 21 },
    { "00000100011", 22 }, { "00000100010", 23 }, { "00000100001", 24 }, { "00000100000", 25 },
    { "00000011111", 26 }, { "00000011110", 27 }, { "00000011101", 28 }, { "00000011100", 29 },
    { "00000011011", 30 }, { "00000011010", 31 }, { "00000011001", 32 }, { "00000011000", 33 },
    { "00000001000", MBA_ESCAPE }, { "00000001111", MBA_STUFFING } };
static const Code MBT_I[] = { { "1", MB_INTRA }, { "01", MB_INTRA | MB_QUANT } };   /* B-2 */
static const Code MBT_P[] = {         /* B-3 */
    { "1", MB_FWD | MB_PATTERN }, { "01", MB_PATTERN }, { "001", MB_FWD }, { "00011", MB_INTRA },
    { "00010", MB_QUANT | MB_FWD | MB_PATTERN }, { "00001", MB_QUANT | MB_PATTERN }, { "000001", MB_INTRA | MB_QUANT } };
static const Code MBT_B[] = {         /* B-4 */
    { "10", MB_FWD | MB_BWD }, { "11", MB_FWD | MB_BWD | MB_PATTERN }, { "010", MB_BWD },
    { "011", MB_BWD | MB_PATTERN }, { "0010", MB_FWD }, { "0011", MB_FWD | MB_PATTERN }, { "00011", MB_INTRA },
    { "00010", MB_QUANT | MB_FWD | MB_BWD | MB_PATTERN }, { "000011", MB_QUANT | MB_FWD | MB_PATTERN },
    { "000010", MB_QUANT | MB_BWD | MB_PATTERN }, { "000001", MB_INTRA | MB_QUANT } };
static const Code CBP[] = {           /* B-9 coded_block_pattern (bit 5: block 0 .. bit 0: block 5) */
    { "111", 60 }, { "1101", 4 }, { "1100", 8 }, { "1011", 16 }, { "1010", 32 }, { "10011", 12 }, { "10010", 48 },
    { "10001", 20 }, { "10000", 40 }, { "01111", 28 }, { "01110", 44 }, { "01101", 52 }, { "01100", 56 },
    { "01011", 1 }, { "01010", 61 }, { "01001", 2 }, { "01000", 62 }, { "001111", 24 }, { "001110", 36 },
    { "001101", 3 }, { "001100", 63 }, { "0010111", 5 }, { "0010110", 9 }, { "0010101", 17 }, { "0010100", 33 },
    { "0010011", 6 }, { "0010010", 10 }, { "0010001", 18 }, { "0010000", 34 }, { "00011111", 7 },
    { "00011110", 11 }, { "00011101", 19 }, { "00011100", 35 }, { "00011011", 13 }, { "00011010", 49 },
    { "00011001", 21 }, { "00011000", 41 }, { "00010111", 14 }, { "00010110", 50 }, { "00010101", 22 },
    { "00010100", 42 }, { "00010011", 15 }, { "00010010", 51 }, { "00010001", 23 }, { "00010000", 43 },
    { "00001111", 25 }, { "00001110", 37 }, { "00001101", 26 }, { "00001100", 38 }, { "00001011", 29 },
    { "00001010", 45 }, { "00001001", 53 }, { "00001000", 57 }, { "00000111", 30 }, { "00000110", 46 },
    { "00000101", 54 }, { "00000100", 58 }, { "000000111", 31 }, { "000000110", 47 }, { "000000101", 55 },
    { "000000100", 59 }, { "000000011", 27 }, { "000000010", 39 }, { "000000001", 0 } };
static const Code MOTION[] = {        /* B-10 motion_code, without its sign */
    { "1", 0 }, { "01", 1 }, { "001", 2 }, { "0001", 3 }, { "000011", 4 }, { "0000101", 5 }, { "0000100", 6 },
    { "0000011", 7 }, { "000001011", 8 }, { "000001010", 9 }, { "000001001", 10 }, { "0000010001", 11 },
    { "0000010000", 12 }, { "0000001111", 13 }, { "0000001110", 14 }, { "0000001101", 15 }, { "0000001100", 16 } };
static const Code DC_Y[] = {          /* B-12 dct_dc_size_luminance */
    { "100", 0 }, { "00", 1 }, { "01", 2 }, { "101", 3 }, { "110", 4 }, { "1110", 5 }, { "11110", 6 },
    { "111110", 7 }, { "1111110", 8 }, { "11111110", 9 }, { "111111110", 10 }, { "111111111", 11 } };
static const Code DC_C[] = {          /* B-13 dct_dc_size_chrominance */
    { "00", 0 }, { "01", 1 }, { "10", 2 }, { "110", 3 }, { "1110", 4 }, { "11110", 5 }, { "111110", 6 },
    { "1111110", 7 }, { "11111110", 8 }, { "111111110", 9 }, { "1111111110", 10 }, { "1111111111", 11 } };

/* B-14 (dct_coefficients, table zero), without the sign bits; the first coefficient of a non-intra block reads "1s"
 * as run 0 level 1 (dct_first) */
static const Code DCT0[] = {
    { "10", DCT_EOB }, { "11", RL(0, 1) }, { "011", RL(1, 1) }, { "0100", RL(0, 2) }, { "0101", RL(2, 1) },
    { "00101", RL(0, 3) }, { "00111", RL(3, 1) }, { "00110", RL(4, 1) }, { "000110", RL(1, 2) },
    { "000111", RL(5, 1) }, { "000101", RL(6, 1) }, { "000100", RL(7, 1) }, { "0000110", RL(0, 4) },
    { "0000100", RL(2, 2) }, { "0000111", RL(8, 1) }, { "0000101", RL(9, 1) }, { "000001", DCT_ESC },
    { "00100110", RL(0, 5) }, { "00100001", RL(0, 6) }, { "00100101", RL(1, 3) }, { "00100100", RL(3, 2) },
    { "00100111", RL(10, 1) }, { "00100011", RL(11, 1) }, { "00100010", RL(12, 1) }, { "00100000", RL(13, 1) },
    { "0000001010", RL(0, 7) }, { "0000001100", RL(1, 4) }, { "0000001011", RL(2, 3) }, { "0000001111", RL(4, 2) },
    { "0000001001", RL(5, 2) }, { "0000001110", RL(14, 1) }, { "0000001101", RL(15, 1) },
    { "0000001000", RL(16, 1) },
    { "000000011101", RL(0, 8) }, { "000000011000", RL(0, 9) }, { "000000010011", RL(0, 10) },
    { "000000010000", RL(0, 11) }, { "000000011011", RL(1, 5) }, { "000000010100", RL(2, 4) },
    { "0000000011010", RL(0, 12) }, { "0000000011001", RL(0, 13) }, { "0000000011000", RL(0, 14) },
    { "0000000010111", RL(0, 15) } };
/* B-15 (table one, intra_vlc_format 1): its own short codes */
static const Code DCT1[] = {
    { "0110", DCT_EOB }, { "10", RL(0, 1) }, { "010", RL(1, 1) }, { "110", RL(0, 2) }, { "00101", RL(2, 1) },
    { "0111", RL(0, 3) }, { "00111", RL(3, 1) }, { "000110", RL(4, 1) }, { "00110", RL(1, 2) },
    { "000111", RL(5, 1) }, { "0000110", RL(6, 1) }, { "0000100", RL(7, 1) }, { "11100", RL(0, 4) },
    { "0000111", RL(2, 2) }, { "0000101", RL(8, 1) }, { "1111000", RL(9, 1) }, { "000001", DCT_ESC },
    { "11101", RL(0, 5) }, { "000101", RL(0, 6) }, { "1111001", RL(1, 3) }, { "00100110", RL(3, 2) },
    { "1111010", RL(10, 1) }, { "00100001", RL(11, 1) }, { "00100101", RL(12, 1) }, { "00100100", RL(13, 1) },
    { "000100", RL(0, 7) }, { "00100111", RL(1, 4) }, { "11111100", RL(2, 3) }, { "11111101", RL(4, 2) },
    { "000000100", RL(5, 2) }, { "000000101", RL(14, 1) }, { "000000111", RL(15, 1) },
    { "0000001101", RL(16, 1) }, { "1111011", RL(0, 8) }, { "1111100", RL(0, 9) }, { "00100011", RL(0, 10) },
    { "00100010", RL(0, 11) }, { "00100000", RL(1, 5) }, { "0000001100", RL(2, 4) }, { "11111010", RL(0, 12) },
    { "11111011", RL(0, 13) }, { "11111110", RL(0, 14) }, { "11111111", RL(0, 15) } };
/* the codes both tables share */
static const Code DCT_SHARED[] = {
    { "000000011100", RL(3, 3) }, { "000000010010", RL(4, 3) }, { "000000011110", RL(6, 2) },
    { "000000010101", RL(7, 2) }, { "000000010001", RL(8, 2) }, { "000000011111", RL(17, 1) },
    { "000000011010", RL(18, 1) }, { "000000011001", RL(19, 1) }, { "000000010111", RL(20, 1) },
    { "000000010110", RL(21, 1) },
    { "0000000010110", RL(1, 6) }, { "0000000010101", RL(1, 7) }, { "0000000010100", RL(2, 5) },
    { "0000000010011", RL(3, 4) }, { "0000000010010", RL(5, 3) }, { "0000000010001", RL(9, 2) },
    { "0000000010000", RL(10, 2) }, { "0000000011111", RL(22, 1) }, { "0000000011110", RL(23, 1) },
    { "0000000011101", RL(24, 1) }, { "0000000011100", RL(25, 1) }, { "0000000011011", RL(26, 1) },
    { "00000000011111", RL(0, 16) }, { "00000000011110", RL(0, 17) }, { "00000000011101", RL(0, 18) },
    { "00000000011100", RL(0, 19) }, { "00000000011011", RL(0, 20) }, { "00000000011010", RL(0, 21) },
    { "00000000011001", RL(0, 22) }, { "00000000011000", RL(0, 23) }, { "00000000010111", RL(0, 24) },
    { "00000000010110", RL(0, 25) }, { "00000000010101", RL(0, 26) }, { "00000000010100", RL(0, 27) },
    { "00000000010011", RL(0, 28) }, { "00000000010010", RL(0, 29) }, { "00000000010001", RL(0, 30) },
    { "00000000010000", RL(0, 31) },
    { "000000000011000", RL(0, 32) }, { "000000000010111", RL(0, 33) }, { "000000000010110", RL(0, 34) },
    { "000000000010101", RL(0, 35) }, { "000000000010100", RL(0, 36) }, { "000000000010011", RL(0, 37) },
    { "000000000010010", RL(0, 38) }, { "000000000010001", RL(0, 39) }, { "000000000010000", RL(0, 40) },
    { "000000000011111", RL(1, 8) }, { "000000000011110", RL(1, 9) }, { "000000000011101", RL(1, 10) },
    { "000000000011100", RL(1, 11) }, { "000000000011011", RL(1, 12) }, { "000000000011010", RL(1, 13) },
    { "000000000011001", RL(1, 14) },
    { "0000000000010011", RL(1, 15) }, { "0000000000010010", RL(1, 16) }, { "0000000000010001", RL(1, 17) },
    { "0000000000010000", RL(1, 18) }, { "0000000000010100", RL(6, 3) }, { "0000000000011010", RL(11, 2) },
    { "0000000000011001", RL(12, 2) }, { "0000000000011000", RL(13, 2) }, { "0000000000010111", RL(14, 2) },
    { "0000000000010110", RL(15, 2) }, { "0000000000010101", RL(16, 2) }, { "0000000000011111", RL(27, 1) },
    { "0000000000011110", RL(28, 1) }, { "0000000000011101", RL(29, 1) }, { "0000000000011100", RL(30, 1) },
    { "0000000000011011", RL(31, 1) } };

#define N(a) (int)(sizeof a / sizeof *a)
static Vlc V_MBA, V_MBT[3], V_CBP, V_MOTION, V_DCY, V_DCC, V_DCT[2];
static int vlc_ready;

static void vlc_add(Vlc *v, const Code *c, int n)
{
    for (int i = 0; i < n; i++) {
        int len = (int)strlen(c[i].code), bits = 0;
        for (int k = 0; k < len; k++)
            bits = bits << 1 | (c[i].code[k] == '1');
        int sh = v->maxlen - len;
        for (int k = 0; k < 1 << sh; k++)
            v->t[bits << sh | k] = c[i].val << 8 | len;
    }
}

static void vlc_new(Vlc *v, int maxlen)
{
    v->maxlen = maxlen;
    v->t = calloc((size_t)1 << maxlen, sizeof *v->t);
}

static void vlc_init(void)
{
    if (vlc_ready)
        return;
    vlc_new(&V_MBA, 11); vlc_add(&V_MBA, MBA, N(MBA));
    vlc_new(&V_MBT[0], 2); vlc_add(&V_MBT[0], MBT_I, N(MBT_I));
    vlc_new(&V_MBT[1], 6); vlc_add(&V_MBT[1], MBT_P, N(MBT_P));
    vlc_new(&V_MBT[2], 6); vlc_add(&V_MBT[2], MBT_B, N(MBT_B));
    vlc_new(&V_CBP, 9); vlc_add(&V_CBP, CBP, N(CBP));
    vlc_new(&V_MOTION, 10); vlc_add(&V_MOTION, MOTION, N(MOTION));
    vlc_new(&V_DCY, 9); vlc_add(&V_DCY, DC_Y, N(DC_Y));
    vlc_new(&V_DCC, 10); vlc_add(&V_DCC, DC_C, N(DC_C));
    for (int t = 0; t < 2; t++) {
        vlc_new(&V_DCT[t], 16);
        vlc_add(&V_DCT[t], t ? DCT1 : DCT0, t ? N(DCT1) : N(DCT0));
        vlc_add(&V_DCT[t], DCT_SHARED, N(DCT_SHARED));
    }
    vlc_ready = 1;
}

/* ---------------------------------------------------------------- the bits */
typedef struct { const uint8_t *p, *end; uint64_t cache; int bits; } Bits;

static void br_init(Bits *b, const uint8_t *p, const uint8_t *end)
{
    b->p = p; b->end = end; b->cache = 0; b->bits = 0;
}
static inline void br_fill(Bits *b)
{
    while (b->bits <= 56) {
        uint64_t c = b->p < b->end ? *b->p : 0;
        b->p++;
        b->cache |= c << (56 - b->bits);
        b->bits += 8;
    }
}
static inline uint32_t br_peek(Bits *b, int n)
{
    if (b->bits < n) br_fill(b);
    return (uint32_t)(b->cache >> (64 - n));
}
static inline void br_skip(Bits *b, int n) { b->cache <<= n; b->bits -= n; }
static inline uint32_t br_get(Bits *b, int n)
{
    if (n <= 0) return 0;
    uint32_t v = br_peek(b, n);
    br_skip(b, n);
    return v;
}
static inline int br_over(const Bits *b) { return b->p - b->bits / 8 > b->end + 2; }   /* read past the end */
static inline int vlc_get(Bits *b, const Vlc *v)       /* the value, -1 no such code */
{
    int32_t e = v->t[br_peek(b, v->maxlen)];
    if (!e) return -1;
    br_skip(b, e & 0xFF);
    return e >> 8;
}

/* ---------------------------------------------------------------- the pictures */
static const uint8_t SCAN_ZIGZAG[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63 };
static const uint8_t SCAN_ALT[64] = {
    0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58,
    35, 43, 51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45, 53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54,
    62, 39, 47, 55, 63 };
static const uint8_t INTRA_Q[64] = {   /* the default intra matrix, in raster order */
    8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27,
    29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83 };
static const uint8_t QSCALE_NONLINEAR[32] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96,
    104, 112 };
static const double FPS[16] = { 0, 24000.0 / 1001, 24, 25, 30000.0 / 1001, 30, 50, 60000.0 / 1001, 60 };

typedef struct { uint8_t *p[3]; } Frame;               /* Y, Cb, Cr */

#define AU_CAP_MAX (4 << 20)                           /* the sound kept when nobody pulls it */

struct Movie {
    FILE *f;
    int64_t base, size, at;                            /* the file's bytes in the Vol, read up to `at` */
    uint8_t io[1 << 16]; int io_n, io_pos;
    int eof;
    uint8_t pes[65536 + 16];
    /* the video's elementary stream */
    uint8_t *es; size_t es_n, es_cap, es_pos;
    /* the sound: ADPCM, its blocks of ch x inter bytes */
    uint8_t *au; size_t au_n, au_cap, au_pos;
    int au_head, au_ch, au_rate, au_inter;
    int16_t au_pcm[2][28 * 64]; int au_have, au_at, au_hist[2][2];
    double au_phase;
    /* the sequence */
    int w, h, mbw, mbh, fps_code, have_seq;
    uint8_t intra_q[64], inter_q[64];
    Frame fr[3];
    int ref_old, ref_new, cur;
    /* the picture */
    int in_pic, pic_ok, ptype, fcode[2][2], dc_prec, pstruct, fpfd, cmv, qst, ivf, alt;
    /* the slice */
    int qscale, dc_pred[3], pmv[2][2][2];
    int last_flags, last_mtype, last_mv[2][2][2], last_fsel[2][2];
    /* shown */
    int pictures, shown, disp, flushed, conv;
    uint8_t *rgba;
};

/* ---------------------------------------------------------------- the program stream */
static int getb(Movie *m)
{
    if (m->io_pos >= m->io_n) {
        int64_t left = m->size - m->at;
        if (left <= 0)
            return -1;
        int n = left < (int64_t)sizeof m->io ? (int)left : (int)sizeof m->io;
        if (_fseeki64(m->f, m->base + m->at, SEEK_SET) != 0 || (int)fread(m->io, 1, (size_t)n, m->f) != n)
            return -1;
        m->at += n;
        m->io_n = n;
        m->io_pos = 0;
    }
    return m->io[m->io_pos++];
}

static int getn(Movie *m, uint8_t *dst, int n)
{
    for (int i = 0; i < n; i++) {
        int c = getb(m);
        if (c < 0) return i;
        if (dst) dst[i] = (uint8_t)c;
    }
    return n;
}

static void grow(uint8_t **buf, size_t *cap, size_t need)
{
    if (need <= *cap)
        return;
    size_t c = *cap ? *cap : 1 << 16;
    while (c < need) c *= 2;
    *buf = realloc(*buf, c);
    *cap = c;
}

/* a packet's sound: the four bytes before the data (FF A1 00 00), the stream's header in the first */
static void audio_data(Movie *m, const uint8_t *p, int n)
{
    if (n < 4)
        return;
    p += 4;
    n -= 4;
    if (!m->au_head) {
        if (n >= 8 && !memcmp(p, "SShd", 4)) {
            int sz = (int)rd32(p + 4);
            if (sz >= 16 && n >= 8 + sz) {
                m->au_rate = (int)rd32(p + 12);
                m->au_ch = (int)rd32(p + 16);
                m->au_inter = (int)rd32(p + 20);
            }
            p += 8 + sz;
            n -= 8 + sz;
        }
        if (n >= 8 && !memcmp(p, "SSbd", 4)) {
            p += 8;
            n -= 8;
        }
        if (m->au_ch < 1 || m->au_ch > 2) m->au_ch = 2;
        if (m->au_inter < 16 || m->au_inter % 16 || m->au_inter > 16 * 64) m->au_inter = 0x20;
        if (m->au_rate <= 0) m->au_rate = 48000;
        m->au_head = 1;
    }
    if (n <= 0)
        return;
    size_t blk = (size_t)m->au_ch * m->au_inter;
    if (m->au_n - m->au_pos > AU_CAP_MAX)                /* nobody listens: the oldest goes */
        m->au_pos += (m->au_n - m->au_pos - AU_CAP_MAX / 2) / blk * blk;
    if (m->au_pos > (1 << 20)) {
        memmove(m->au, m->au + m->au_pos, m->au_n - m->au_pos);
        m->au_n -= m->au_pos;
        m->au_pos = 0;
    }
    grow(&m->au, &m->au_cap, m->au_n + (size_t)n);
    memcpy(m->au + m->au_n, p, (size_t)n);
    m->au_n += (size_t)n;
}

/* the next pack or packet; 0 at the end */
static int demux(Movie *m)
{
    if (m->eof)
        return 0;
    uint32_t sc = 0xFFFFFFFF;
    int c;
    while ((c = getb(m)) >= 0) {
        sc = sc << 8 | (uint32_t)c;
        if ((sc & 0xFFFFFF00) == 0x100 && (sc & 0xFF) >= 0xB9)
            break;
    }
    int id = (int)(sc & 0xFF);
    if (c < 0 || id == 0xB9) {                           /* the program's end */
        m->eof = 1;
        return 0;
    }
    if (id == 0xBA) {                                    /* the pack header */
        int b = getb(m);
        if ((b & 0xC0) == 0x40) {
            getn(m, NULL, 8);
            int st = getb(m);
            getn(m, NULL, st & 7);
        } else
            getn(m, NULL, 7);                            /* MPEG-1 */
        return 1;
    }
    int hi = getb(m), lo = getb(m);
    if (hi < 0 || lo < 0) {
        m->eof = 1;
        return 0;
    }
    int len = hi << 8 | lo;
    if (id != 0xE0 && id != 0xBD) {
        getn(m, NULL, len);
        return 1;
    }
    if (getn(m, m->pes, len) != len) {
        m->eof = 1;
        return 0;
    }
    const uint8_t *p = m->pes;
    int k = 0;
    if (len >= 3 && (p[0] & 0xC0) == 0x80)               /* the MPEG-2 PES header */
        k = 3 + p[2];
    else {                                               /* MPEG-1's */
        while (k < len && p[k] == 0xFF) k++;
        if (k < len && (p[k] & 0xC0) == 0x40) k += 2;
        if (k < len && (p[k] & 0xF0) == 0x20) k += 5;
        else if (k < len && (p[k] & 0xF0) == 0x30) k += 10;
        else k++;
    }
    if (k >= len)
        return 1;
    if (id == 0xE0) {
        if (m->es_pos > (1 << 20)) {
            memmove(m->es, m->es + m->es_pos, m->es_n - m->es_pos);
            m->es_n -= m->es_pos;
            m->es_pos = 0;
        }
        grow(&m->es, &m->es_cap, m->es_n + (size_t)(len - k));
        memcpy(m->es + m->es_n, p + k, (size_t)(len - k));
        m->es_n += (size_t)(len - k);
    } else
        audio_data(m, p + k, len - k);
    return 1;
}

/* the first 00 00 01 at or after i (that has its code byte), n none */
static size_t find_code(const uint8_t *b, size_t i, size_t n)
{
    for (; i + 3 < n; i++)
        if (b[i + 2] <= 1 && b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
            return i;
    return n;
}

/* the next unit of the video: its start code at *at, its bytes from *s (after the code) up to *e (the next code or
 * the end); the code, -1 at the end */
static int next_unit(Movie *m, size_t *at, size_t *s, size_t *e)
{
    size_t from = 0, k;                                  /* offsets from es_pos: demux may move the buffer down */
    while ((k = find_code(m->es, m->es_pos + from, m->es_n)) >= m->es_n) {
        size_t n = m->es_n - m->es_pos;
        from = n > 3 ? n - 3 : 0;
        if (!demux(m))
            return -1;
    }
    size_t a = k - m->es_pos;
    from = a + 4;
    while ((k = find_code(m->es, m->es_pos + from, m->es_n)) >= m->es_n) {
        size_t n = m->es_n - m->es_pos;
        from = n > a + 7 ? n - 3 : a + 4;
        if (!demux(m)) {
            k = m->es_n;
            break;
        }
    }
    *at = m->es_pos + a;
    *s = *at + 4;
    *e = k;
    return m->es[*at + 3];
}

/* ---------------------------------------------------------------- the IDCT (separable, float) */
static float IDCT_C[8][8];                             /* [x][u] = C(u) / 2 cos((2x + 1) u pi / 16) */

static void idct_init(void)
{
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++)
            IDCT_C[x][u] = (u ? 0.5f : 0.35355339f) * (float)cos((2 * x + 1) * u * 3.14159265358979 / 16);
}

static void idct(const int16_t in[64], int out[64])
{
    float tmp[64];
    for (int v = 0; v < 8; v++) {                        /* the rows (along u) */
        const int16_t *r = in + 8 * v;
        int any = 0;
        for (int u = 1; u < 8; u++) any |= r[u];
        for (int x = 0; x < 8; x++) {
            float s = r[0] * IDCT_C[x][0];
            if (any)
                for (int u = 1; u < 8; u++) s += r[u] * IDCT_C[x][u];
            tmp[8 * v + x] = s;
        }
    }
    for (int x = 0; x < 8; x++)                          /* then the columns */
        for (int y = 0; y < 8; y++) {
            float s = 0;
            for (int v = 0; v < 8; v++) s += tmp[8 * v + x] * IDCT_C[y][v];
            out[8 * y + x] = (int)floorf(s + 0.5f);
        }
}

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* ---------------------------------------------------------------- the blocks */
static int block(Movie *m, Bits *b, int comp, int intra, int16_t blk[64])
{
    memset(blk, 0, 64 * sizeof *blk);
    const uint8_t *scan = m->alt ? SCAN_ALT : SCAN_ZIGZAG;
    const uint8_t *W = intra ? m->intra_q : m->inter_q;
    int qs = m->qscale, sum = 0, i = 0;
    if (intra) {
        int size = vlc_get(b, comp ? &V_DCC : &V_DCY), diff = 0;
        if (size < 0) return -1;
        if (size) {
            diff = (int)br_get(b, size);
            if (!(diff & 1 << (size - 1))) diff -= (1 << size) - 1;
        }
        m->dc_pred[comp] += diff;
        blk[0] = (int16_t)(m->dc_pred[comp] << (3 - m->dc_prec));
        sum = blk[0];
        i = 1;
    }
    const Vlc *t = &V_DCT[intra && m->ivf];
    for (int first = !intra;; first = 0) {
        int run, level;
        if (first && br_peek(b, 1)) {                    /* dct_first: "1s" is run 0 level 1 */
            br_skip(b, 1);
            run = 0;
            level = br_get(b, 1) ? -1 : 1;
        } else {
            int e = vlc_get(b, t);
            if (e < 0) return -1;
            if (e == DCT_EOB) break;
            if (e == DCT_ESC) {
                run = (int)br_get(b, 6);
                level = (int)br_get(b, 12);
                if (level & 0x800) level -= 0x1000;
                if (!level || level == -2048) return -1;
            } else {
                run = e & 63;
                level = e >> 6;
                if (br_get(b, 1)) level = -level;
            }
        }
        i += run;
        if (i > 63) return -1;
        int pos = scan[i++], v;
        if (intra)
            v = level * W[pos] * qs * 2 / 32;
        else
            v = (2 * level + (level > 0 ? 1 : -1)) * W[pos] * qs / 32;
        if (v > 2047) v = 2047;
        if (v < -2048) v = -2048;
        blk[pos] = (int16_t)v;
        sum += v;
    }
    if (!(sum & 1))                                      /* mismatch control */
        blk[63] ^= 1;
    return 0;
}

/* a block into the picture: intra written, otherwise added to the prediction */
static void put_block(const int16_t blk[64], uint8_t *dst, int stride, int add)
{
    int r[64];
    idct(blk, r);
    for (int y = 0; y < 8; y++, dst += stride)
        for (int x = 0; x < 8; x++)
            dst[x] = clamp8(add ? dst[x] + r[8 * y + x] : r[8 * y + x]);
}

/* ---------------------------------------------------------------- motion */
/* a w x h block at (x, y) of dst from ref at the half-pel vector (dx, dy); avg: averaged with what is there */
static void mc(uint8_t *dst, const uint8_t *ref, int stride, int pw, int ph, int x, int y, int w, int h, int dx,
               int dy, int avg)
{
    int sx = 2 * x + dx, sy = 2 * y + dy, ix = sx >> 1, iy = sy >> 1, hx = sx & 1, hy = sy & 1;
    if (ix < 0) ix = 0, hx = 0;
    if (iy < 0) iy = 0, hy = 0;
    if (ix + w + hx > pw) ix = pw - w - hx;
    if (iy + h + hy > ph) iy = ph - h - hy;
    if (ix < 0 || iy < 0)
        return;
    const uint8_t *s = ref + iy * stride + ix;
    uint8_t *d = dst + y * stride + x;
    for (int j = 0; j < h; j++, s += stride, d += stride)
        for (int i = 0; i < w; i++) {
            int v;
            if (hx && hy) v = (s[i] + s[i + 1] + s[i + stride] + s[i + stride + 1] + 2) >> 2;
            else if (hx) v = (s[i] + s[i + 1] + 1) >> 1;
            else if (hy) v = (s[i] + s[i + stride] + 1) >> 1;
            else v = s[i];
            d[i] = (uint8_t)(avg ? (d[i] + v + 1) >> 1 : v);
        }
}

/* the prediction of macroblock (mx, my): frame motion (mtype 2) or field motion (1) from the references the flags name */
static void predict(Movie *m, int mx, int my, int flags, int mtype, int mv[2][2][2], int fsel[2][2])
{
    Frame *cur = &m->fr[m->cur];
    int avg = 0;
    for (int r = 0; r < 2; r++) {
        if (!(flags & (r ? MB_BWD : MB_FWD)))
            continue;
        int ri = m->ptype == 3 && r == 0 ? m->ref_old : m->ref_new;
        if (ri < 0)
            continue;
        Frame *ref = &m->fr[ri];
        if (mtype == 2) {
            int dx = mv[0][r][0], dy = mv[0][r][1];
            mc(cur->p[0], ref->p[0], m->w, m->w, m->h, 16 * mx, 16 * my, 16, 16, dx, dy, avg);
            for (int c = 1; c < 3; c++)
                mc(cur->p[c], ref->p[c], m->w / 2, m->w / 2, m->h / 2, 8 * mx, 8 * my, 8, 8, dx / 2, dy / 2, avg);
        } else {                                         /* the two fields, each from the field it selects */
            for (int s = 0; s < 2; s++) {
                int dx = mv[s][r][0], dy = mv[s][r][1], fs = fsel[s][r];
                mc(cur->p[0] + s * m->w, ref->p[0] + fs * m->w, 2 * m->w, m->w, m->h / 2, 16 * mx, 8 * my, 16, 8,
                   dx, dy, avg);
                for (int c = 1; c < 3; c++)
                    mc(cur->p[c] + s * (m->w / 2), ref->p[c] + fs * (m->w / 2), m->w, m->w / 2, m->h / 4, 8 * mx,
                       4 * my, 8, 4, dx / 2, dy / 2, avg);
            }
        }
        avg = 1;
    }
}

static int motion_delta(Movie *m, Bits *b, int fcode)
{
    int code = vlc_get(b, &V_MOTION);
    if (code < 0) return 0;
    if (!code) return 0;
    int sign = (int)br_get(b, 1), rs = fcode - 1;
    int d = ((code - 1) << rs) + (int)br_get(b, rs) + 1;
    (void)m;
    return sign ? -d : d;
}

static int motion_wrap(int v, int fcode)
{
    int f = 1 << (fcode - 1), lo = -16 * f, hi = 16 * f - 1;
    if (v < lo) v += 32 * f;
    if (v > hi) v -= 32 * f;
    return v;
}

/* motion_vectors(r) of a frame picture: one frame vector or two field ones */
static void motion_vectors(Movie *m, Bits *b, int r, int mtype, int mv[2][2][2], int fsel[2][2])
{
    int fx = m->fcode[r][0], fy = m->fcode[r][1];
    if (mtype == 2) {
        int x = motion_wrap(m->pmv[0][r][0] + motion_delta(m, b, fx), fx);
        int y = motion_wrap(m->pmv[0][r][1] + motion_delta(m, b, fy), fy);
        m->pmv[0][r][0] = m->pmv[1][r][0] = mv[0][r][0] = x;
        m->pmv[0][r][1] = m->pmv[1][r][1] = mv[0][r][1] = y;
    } else {
        for (int s = 0; s < 2; s++) {
            fsel[s][r] = (int)br_get(b, 1);
            int x = motion_wrap(m->pmv[s][r][0] + motion_delta(m, b, fx), fx);
            int y = motion_wrap((m->pmv[s][r][1] >> 1) + motion_delta(m, b, fy), fy);
            m->pmv[s][r][0] = mv[s][r][0] = x;
            m->pmv[s][r][1] = y * 2;
            mv[s][r][1] = y;
        }
    }
}

/* ---------------------------------------------------------------- the macroblocks and slices */
static void dc_reset(Movie *m) { m->dc_pred[0] = m->dc_pred[1] = m->dc_pred[2] = 1 << (7 + m->dc_prec); }

static void skipped(Movie *m, int addr)
{
    int mx = addr % m->mbw, my = addr / m->mbw;
    dc_reset(m);
    if (m->ptype == 2) {                                 /* P: no motion from the reference */
        int mv[2][2][2] = { { { 0 } } }, fs[2][2] = { { 0, 0 }, { 1, 1 } };
        memset(m->pmv, 0, sizeof m->pmv);
        predict(m, mx, my, MB_FWD, 2, mv, fs);
    } else if (m->ptype == 3)                            /* B: the macroblock before's motion */
        predict(m, mx, my, m->last_flags, m->last_mtype, m->last_mv, m->last_fsel);
}

static int macroblock(Movie *m, Bits *b, int addr)
{
    int mx = addr % m->mbw, my = addr / m->mbw;
    int flags = vlc_get(b, &V_MBT[m->ptype - 1]);
    if (flags < 0) return -1;
    int mtype = 2, dct_field = 0;
    if (flags & (MB_FWD | MB_BWD)) {
        if (!m->fpfd) mtype = (int)br_get(b, 2);
        if (mtype == 3 || mtype == 0) return -1;         /* dual prime: not in these movies */
    }
    if (!m->fpfd && (flags & (MB_INTRA | MB_PATTERN)))
        dct_field = (int)br_get(b, 1);
    if (flags & MB_QUANT) {
        int q = (int)br_get(b, 5);
        m->qscale = m->qst ? QSCALE_NONLINEAR[q] : 2 * q;
    }
    int mv[2][2][2] = { { { 0 } } }, fsel[2][2] = { { 0, 0 }, { 1, 1 } };
    if (flags & MB_INTRA) {
        if (m->cmv) {                                    /* concealment vectors: read, not used */
            int t[2][2][2], f[2][2];
            motion_vectors(m, b, 0, 2, t, f);           /* (frame-based, frame_motion_type absent) */
            br_get(b, 1);
        } else
            memset(m->pmv, 0, sizeof m->pmv);
    } else {
        if (flags & MB_FWD) motion_vectors(m, b, 0, mtype, mv, fsel);
        if (flags & MB_BWD) motion_vectors(m, b, 1, mtype, mv, fsel);
        if (m->ptype == 2 && !(flags & MB_FWD)) {        /* P, no motion: from the reference as it is */
            memset(m->pmv, 0, sizeof m->pmv);
            flags |= MB_FWD;
            mtype = 2;
        }
        dc_reset(m);
        predict(m, mx, my, flags, mtype, mv, fsel);
        m->last_flags = flags & (MB_FWD | MB_BWD);
        m->last_mtype = mtype;
        memcpy(m->last_mv, mv, sizeof mv);
        memcpy(m->last_fsel, fsel, sizeof fsel);
    }
    int cbp = flags & MB_INTRA ? 63 : 0;
    if (flags & MB_PATTERN) {
        cbp = vlc_get(b, &V_CBP);
        if (cbp < 0) return -1;
    }
    Frame *cur = &m->fr[m->cur];
    int16_t blk[64];
    for (int i = 0; i < 6; i++) {
        if (!(cbp & 1 << (5 - i)))
            continue;
        if (block(m, b, i < 4 ? 0 : i - 3, flags & MB_INTRA, blk) < 0)
            return -1;
        uint8_t *dst;
        int stride;
        if (i < 4) {
            int bx = 16 * mx + 8 * (i & 1);
            if (dct_field) {                             /* blocks 0 1 the top field's lines, 2 3 the bottom's */
                dst = cur->p[0] + (16 * my + (i >> 1)) * m->w + bx;
                stride = 2 * m->w;
            } else {
                dst = cur->p[0] + (16 * my + 8 * (i >> 1)) * m->w + bx;
                stride = m->w;
            }
        } else {
            stride = m->w / 2;
            dst = cur->p[i - 3] + 8 * my * stride + 8 * mx;
        }
        put_block(blk, dst, stride, !(flags & MB_INTRA));
    }
    return 0;
}

static void slice(Movie *m, const uint8_t *p, const uint8_t *end, int row)
{
    Bits b;
    br_init(&b, p, end);
    int q = (int)br_get(&b, 5);
    m->qscale = m->qst ? QSCALE_NONLINEAR[q] : 2 * q;
    if (br_get(&b, 1)) {                                 /* intra_slice_flag, intra_slice, reserved, the extras */
        br_get(&b, 8);
        while (br_get(&b, 1)) br_get(&b, 8);
    }
    dc_reset(m);
    memset(m->pmv, 0, sizeof m->pmv);
    m->last_flags = MB_FWD;
    m->last_mtype = 2;
    memset(m->last_mv, 0, sizeof m->last_mv);
    int addr = row * m->mbw - 1, first = 1, total = m->mbw * m->mbh;
    for (;;) {
        int inc = 0;
        for (;;) {
            int e = vlc_get(&b, &V_MBA);
            if (e < 0) return;
            if (e == MBA_STUFFING) continue;
            if (e == MBA_ESCAPE) { inc += 33; continue; }
            inc += e;
            break;
        }
        if (!first)
            for (int k = 1; k < inc && addr + k < total; k++)
                skipped(m, addr + k);
        addr += inc;
        first = 0;
        if (addr >= total || macroblock(m, &b, addr) < 0 || br_over(&b) || br_peek(&b, 23) == 0)
            return;
    }
}

/* ---------------------------------------------------------------- the headers */
static void frames_alloc(Movie *m)
{
    for (int i = 0; i < 3; i++) {
        free(m->fr[i].p[0]);
        size_t y = (size_t)m->w * m->h;
        m->fr[i].p[0] = malloc(y + y / 2);
        memset(m->fr[i].p[0], 16, y);
        memset(m->fr[i].p[0] + y, 128, y / 2);
        m->fr[i].p[1] = m->fr[i].p[0] + y;
        m->fr[i].p[2] = m->fr[i].p[1] + y / 4;
    }
    free(m->rgba);
    m->rgba = calloc((size_t)m->w * m->h, 4);
    m->ref_old = m->ref_new = m->disp = -1;
}

static void load_matrix(Bits *b, uint8_t q[64])
{
    for (int i = 0; i < 64; i++)
        q[SCAN_ZIGZAG[i]] = (uint8_t)br_get(b, 8);
}

static void sequence_header(Movie *m, Bits *b)
{
    int w = (int)br_get(b, 12), h = (int)br_get(b, 12);
    br_get(b, 4);
    m->fps_code = (int)br_get(b, 4);
    br_get(b, 18); br_get(b, 1); br_get(b, 10); br_get(b, 1);
    if (br_get(b, 1)) load_matrix(b, m->intra_q);
    else memcpy(m->intra_q, INTRA_Q, 64);
    if (br_get(b, 1)) load_matrix(b, m->inter_q);
    else memset(m->inter_q, 16, 64);
    w = (w + 15) & ~15;
    h = (h + 15) & ~15;
    if (w <= 0 || h <= 0 || w > 1920 || h > 1152)
        return;
    if (w != m->w || h != m->h || !m->have_seq) {
        m->w = w;
        m->h = h;
        m->mbw = w / 16;
        m->mbh = h / 16;
        frames_alloc(m);
    }
    m->have_seq = 1;
}

static void extension(Movie *m, Bits *b)
{
    int id = (int)br_get(b, 4);
    if (id == 3) {                                       /* quant_matrix_extension */
        if (br_get(b, 1)) load_matrix(b, m->intra_q);
        if (br_get(b, 1)) load_matrix(b, m->inter_q);
    } else if (id == 8) {                                /* picture_coding_extension */
        m->fcode[0][0] = (int)br_get(b, 4); m->fcode[0][1] = (int)br_get(b, 4);
        m->fcode[1][0] = (int)br_get(b, 4); m->fcode[1][1] = (int)br_get(b, 4);
        m->dc_prec = (int)br_get(b, 2);
        m->pstruct = (int)br_get(b, 2);
        br_get(b, 1);                                    /* top_field_first */
        m->fpfd = (int)br_get(b, 1);
        m->cmv = (int)br_get(b, 1);
        m->qst = (int)br_get(b, 1);
        m->ivf = (int)br_get(b, 1);
        m->alt = (int)br_get(b, 1);
    }
}

/* the picture decoded: the reference before it (I, P) or itself (B) is the one shown now */
static void picture_end(Movie *m)
{
    m->in_pic = 0;
    if (m->pic_ok != 1)                                  /* not decodable, or no slices */
        return;
    m->pictures++;
    if (m->ptype == 3) {
        m->disp = m->cur;
    } else {
        int before = m->ref_new;
        m->ref_old = m->ref_new;
        m->ref_new = m->cur;
        if (before < 0)
            return;
        m->disp = before;
    }
    m->shown++;
    m->conv = 0;
}

static void picture_start(Movie *m, Bits *b)
{
    br_get(b, 10);
    m->ptype = (int)br_get(b, 3);
    m->pstruct = 3;                                      /* until the coding extension says */
    m->fpfd = 1;
    m->in_pic = 1;
    m->pic_ok = -1;                                      /* decided at the first slice */
}

static void picture_begin(Movie *m)
{
    m->pic_ok = m->have_seq && m->pstruct == 3 && m->ptype >= 1 && m->ptype <= 3 &&
                (m->ptype < 2 || m->ref_new >= 0) && (m->ptype < 3 || m->ref_old >= 0);
    if (!m->pic_ok)
        return;
    for (int i = 0; i < 3; i++)
        if (i != m->ref_old && i != m->ref_new) {
            m->cur = i;
            break;
        }
}

/* the next picture decoded (shown or not); 0 at the end of the stream */
static int decode_picture(Movie *m)
{
    for (;;) {
        size_t at, s, e;
        int code = next_unit(m, &at, &s, &e);
        if (code < 0) {
            if (m->in_pic) {
                picture_end(m);
                m->es_pos = m->es_n;
                return 1;
            }
            m->es_pos = m->es_n;
            return 0;
        }
        int is_slice = code >= 1 && code <= 0xAF;
        if (m->in_pic && (code == 0x00 || code == 0xB3 || code == 0xB7 || code == 0xB8)) {   /* the picture is over:
                                                          * this unit is the next one's */
            m->es_pos = at;
            picture_end(m);
            return 1;
        }
        m->es_pos = e;
        Bits b;
        br_init(&b, m->es + s, m->es + e);
        if (code == 0xB3)
            sequence_header(m, &b);
        else if (code == 0xB5)
            extension(m, &b);
        else if (code == 0x00)
            picture_start(m, &b);
        else if (is_slice && m->in_pic) {
            if (m->pic_ok < 0)
                picture_begin(m);
            if (m->pic_ok && code - 1 < m->mbh)
                slice(m, m->es + s, m->es + e, code - 1);
        }
    }
}

/* ---------------------------------------------------------------- the interface */
Movie *movie_open(const Vol *v, const char *name)
{
    char path[64];
    snprintf(path, sizeof path, "%s%s", name, strchr(name, '.') ? "" : ".pss");
    const VolFile *vf = v ? vol_find(v, path) : NULL;
    if (!vf || vf->size < 2048)
        return NULL;
    vlc_init();
    if (IDCT_C[0][0] == 0) idct_init();
    Movie *m = calloc(1, sizeof *m);
    m->f = v->f;
    m->base = v->base + vf->offset;                      /* (a disc image: from the VOL's start in it) */
    m->size = vf->size;
    m->ref_old = m->ref_new = m->disp = -1;
    for (int i = 0; i < 64 && !m->w; i++) {              /* the first sequence header: the size */
        size_t k = 0;
        while ((k = find_code(m->es, k, m->es_n)) < m->es_n && m->es[k + 3] != 0xB3) k++;
        if (k + 150 <= m->es_n) {
            Bits b;
            br_init(&b, m->es + k + 4, m->es + m->es_n);
            sequence_header(m, &b);                      /* (read again in its turn) */
            break;
        }
        if (!demux(m))
            break;
    }
    if (!m->w) {
        movie_close(m);
        return NULL;
    }
    return m;
}

/* [R] 0x169360, FullMotionVideoManager's file pick: the name without its extension (cut at its '.', 0x32D7B0), the
 * language's suffix by gApp +0x70 (0x1693D8, the table 0x40F0A0: the .loc column, 1 English .. 9 Portuguese; 0 and
 * past 9 none), then up to four tries: <name><lang>_pal, <name>_pal, <name><lang>, <name>, the first two only when the
 * display's rate (0x4CB550) is under 59 (PAL, 50 Hz); each with the name's own extension if it had one, then the
 * manager's (slot +0x28, PS2FullMotionVideoManager 0x1A6E70: ".PSS" alone), until a file opens (0x10C0E0). The rate
 * it hands the player is 25 for a _pal one, else 30 (0x169600). */
static const char *const LANG_SUFFIX[10] = { "", "_a", "_e", "_j", "_f", "_g", "_i", "_s", "_d", "_p" };

Movie *movie_pick(const Vol *v, const char *name, int lang, int pal, char *picked, size_t n)
{
    char base[64], ext[16] = "", path[96];
    if (!v || !name)
        return NULL;
    snprintf(base, sizeof base, "%s", name);
    char *dot = strchr(base, '.');
    if (dot) {
        snprintf(ext, sizeof ext, "%s", dot);
        *dot = 0;
    }
    const char *suffix = lang >= 0 && lang < 10 ? LANG_SUFFIX[lang] : "";
    for (int try = 0; try < 4; try++) {
        if (!pal && try < 2)                             /* (60 Hz: no _pal) */
            continue;
        for (int e = ext[0] ? -1 : 0; ; e++) {
            const char *x = e < 0 ? ext : e == 0 ? ".PSS" : NULL;
            if (!x)
                break;
            snprintf(path, sizeof path, "%s%s%s%s", base, try & 1 ? "" : suffix, try < 2 ? "_pal" : "", x);
            if (!vol_find(v, path))
                continue;
            Movie *m = movie_open(v, path);
            if (m) {
                if (picked && n)
                    snprintf(picked, n, "%s", path);
                return m;
            }
        }
    }
    return NULL;
}

void movie_close(Movie *m)
{
    if (!m)
        return;
    free(m->fr[0].p[0]); free(m->fr[1].p[0]); free(m->fr[2].p[0]);
    free(m->rgba);
    free(m->es);
    free(m->au);
    free(m);
}

void movie_size(const Movie *m, int *w, int *h)
{
    if (w) *w = m ? m->w : 0;
    if (h) *h = m ? m->h : 0;
}

double movie_fps(const Movie *m) { return m && FPS[m->fps_code & 15] > 0 ? FPS[m->fps_code & 15] : 30000.0 / 1001; }

int movie_pictures(const Movie *m) { return m ? m->pictures : 0; }

static void convert(Movie *m)
{
    const Frame *f = &m->fr[m->disp];
    for (int y = 0; y < m->h; y++) {
        const uint8_t *py = f->p[0] + y * m->w, *pu = f->p[1] + (y / 2) * (m->w / 2), *pv = f->p[2] + (y / 2) * (m->w / 2);
        uint8_t *o = m->rgba + (size_t)y * m->w * 4;
        for (int x = 0; x < m->w; x++, o += 4) {          /* ITU-R 601, studio range (the IPU's CSC) */
            int c = 298 * (py[x] - 16), d = pu[x / 2] - 128, e = pv[x / 2] - 128;
            o[0] = clamp8((c + 409 * e + 128) >> 8);
            o[1] = clamp8((c - 100 * d - 208 * e + 128) >> 8);
            o[2] = clamp8((c + 516 * d + 128) >> 8);
            o[3] = 255;
        }
    }
    m->conv = 1;
}

const uint8_t *movie_video(Movie *m, double t, int *changed)
{
    if (changed) *changed = 0;
    if (!m)
        return NULL;
    int want = (int)floor(t * movie_fps(m)) + 1;         /* pictures shown by t */
    int before = m->shown;
    while (m->shown < want && !m->flushed) {
        if (!decode_picture(m)) {                        /* the end: the last reference is shown */
            if (m->ref_new >= 0 && m->disp != m->ref_new) {
                m->disp = m->ref_new;
                m->shown++;
                m->conv = 0;
            }
            m->flushed = 1;
        }
    }
    if (m->disp < 0)
        return NULL;
    if (!m->conv)
        convert(m);
    if (changed) *changed = m->shown != before;
    return m->rgba;
}

int movie_planes(const Movie *m, const uint8_t *plane[3], int stride[3])
{
    if (!m || m->disp < 0)
        return 0;
    for (int c = 0; c < 3; c++) {
        plane[c] = m->fr[m->disp].p[c];
        stride[c] = c ? m->w / 2 : m->w;
    }
    return 1;
}

int movie_done(const Movie *m, double t)
{
    return !m || (m->flushed && t * movie_fps(m) >= m->shown);
}

/* ---------------------------------------------------------------- the sound */
static const int FILT[5][2] = { { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 } };   /* the SPU's */

static void adpcm_frame(const uint8_t *f, int16_t *out, int *h1, int *h2)   /* as sound.c's */
{
    int shift = f[0] & 15, filt = f[0] >> 4;
    if (filt > 4) filt = 4;
    for (int i = 0; i < 28; i++) {
        int n = f[2 + i / 2] >> (4 * (i & 1)) & 15;
        int s = ((int16_t)(n << 12) >> shift) + ((*h1 * FILT[filt][0] + *h2 * FILT[filt][1] + 32) >> 6);
        s = s < -32768 ? -32768 : s > 32767 ? 32767 : s;
        *h2 = *h1;
        *h1 = s;
        out[i] = (int16_t)s;
    }
}

/* the next block of every channel; 0 none left */
static int audio_block(Movie *m)
{
    size_t blk;
    while (!m->au_head || m->au_n - m->au_pos < (blk = (size_t)m->au_ch * m->au_inter))
        if (!demux(m))
            return 0;
    for (int c = 0; c < m->au_ch; c++)
        for (int k = 0; k < m->au_inter / 16; k++)
            adpcm_frame(m->au + m->au_pos + (size_t)c * m->au_inter + 16 * k, m->au_pcm[c] + 28 * k,
                        &m->au_hist[c][0], &m->au_hist[c][1]);
    m->au_pos += blk;
    m->au_have = m->au_inter / 16 * 28;
    m->au_at = 0;
    return 1;
}

int movie_audio(Movie *m, int16_t *out, int frames)
{
    int f = 0;
    if (m) {
        double step = m->au_rate > 0 ? m->au_rate / 48000.0 : 1;
        for (; f < frames; f++) {
            if (m->au_at >= m->au_have && !audio_block(m))
                break;
            out[2 * f] = m->au_pcm[0][m->au_at];
            out[2 * f + 1] = m->au_pcm[m->au_ch - 1][m->au_at];
            for (m->au_phase += step; m->au_phase >= 1; m->au_phase -= 1)
                m->au_at++;
        }
    }
    memset(out + 2 * f, 0, sizeof *out * 2 * (size_t)(frames - f));
    return f;
}
