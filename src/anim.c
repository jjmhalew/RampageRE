/* anim.c - animation clips (bundle entry type 4, tools/anim.py, docs/FORMAT_BDP.md "Animations"), evaluated as the game
 * does it.
 * KeyInterpAnim (kind 2): 0x1C f32 duration, 0x2C u32 bones, 0x38 offset of the translation tracks, 0x3C of the rotation
 *   tracks. Translation track (0x191860): f32[3] scale, u8 bone, u8 keys, u8 keys - 1, u8 0, then per key u16 time,
 *   s16 x, y, z; value = s16 x scale x f32(0x38000100). Rotation track (0x191C50): u8 bone, u8 keys, then per key u16 time,
 *   s16 a, b, c, decoded by 0x192010. Key times are (time & 0xFFFE) / 65534 of the duration.
 * RawAnim (kind 1): per bone n translation segments (f32[3] p, f32[3] d: p + d f) and rotation segments (f32[4] q0,
 *   f32[4] q1, f32 a: normalise(q0 cos(a f) + q1 sin(a f)), or normalise(q0 + q1 f) when a is 0), spread evenly over the
 *   clip (0x19CEC0, 0x19D0A0, 0x167DE0).
 * Quaternions stay in the game's row-vector convention (mat_tq takes them so). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "data.h"

static float f32bits(uint32_t u) { union { uint32_t u; float f; } v; v.u = u; return v.f; }

/* 0x192010: three 13-bit components, w from the unit length plus a small correction, w's sign in the time's bit 0 */
static void decode_rotation(const uint8_t *key, float q[4])
{
    uint16_t time = rd16(key);
    int a = (int16_t)rd16(key + 2), b = (int16_t)rd16(key + 4), c = (int16_t)rd16(key + 6);
    float unit = f32bits(0x39800801), corr = f32bits(0x3B808081) * f32bits(0x39DD7E7C);
    float x = (float)(a >> 3) * unit, y = (float)(b >> 3) * unit, z = (float)(c >> 3) * unit;
    float s = x * x + y * y + z * z;
    if (s > 1) s = 1;
    int r = (a & 3) << 6 | (b & 7) << 3 | (c & 7);
    s += (float)((a & 4) ? -r : r) * corr;
    float w;
    if (s <= 1) {
        w = sqrtf(1 - s);
    } else {
        float n = sqrtf(x * x + y * y + z * z);
        x /= n; y /= n; z /= n; w = 0;
    }
    if (time & 1)
        w = -w;
    q[0] = x; q[1] = y; q[2] = z; q[3] = w;
}

void quat_slerp(const float a[4], const float b[4], float f, float out[4])
{
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], bb[4] = { b[0], b[1], b[2], b[3] };
    if (d < 0) { d = -d; for (int k = 0; k < 4; k++) bb[k] = -bb[k]; }
    float wa = 1 - f, wb = f;
    if (d < 0.9995f) {
        float th = acosf(d), s = sinf(th);
        wa = sinf((1 - f) * th) / s;
        wb = sinf(f * th) / s;
    }
    float n = 0;
    for (int k = 0; k < 4; k++) { out[k] = a[k] * wa + bb[k] * wb; n += out[k] * out[k]; }
    n = sqrtf(n);
    for (int k = 0; k < 4; k++) out[k] /= n;
}

int clip_read(const BdpEntry *e, Clip *c)
{
    memset(c, 0, sizeof *c);
    if (!e || e->type != ET_ANIM || e->size < 0x44)
        return -1;
    const uint8_t *d = e->data;
    c->data = d;
    c->kind = rd32(d + 0x20);
    c->duration = rdf(d + 0x1C);
    c->skeleton = rd16(d + 0x28);
    if (c->kind == 2) {
        c->bones = rd32(d + 0x2C);
        c->trans = calloc(c->bones ? c->bones : 1, sizeof *c->trans);
        c->rot = calloc(c->bones ? c->bones : 1, sizeof *c->rot);
        const uint8_t *p = d + rd32(d + 0x38);
        for (uint32_t i = 0; i < c->bones; i++) {
            c->trans[i] = p;
            p += 0x10 + 8 * p[0xD];
        }
        p = d + rd32(d + 0x3C);
        for (uint32_t i = 0; i < c->bones; i++) {
            c->rot[i] = p;
            p += 2 + 8 * p[1];
        }
    } else if (c->kind == 1) {
        uint32_t count = rd32(d + 0x2C), table = rd32(d + 0x30);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t bone = rd32(d + rd32(d + table + 4 * i)) + 1;
            if (bone > c->bones) c->bones = bone;
        }
        c->trans = calloc(c->bones ? c->bones : 1, sizeof *c->trans);   /* per bone: its record */
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *rec = d + rd32(d + table + 4 * i);
            c->trans[rd32(rec)] = rec;
        }
    } else {
        return -1;
    }
    return 0;
}

void clip_free(Clip *c)
{
    free((void *)c->trans);
    free((void *)c->rot);
    memset(c, 0, sizeof *c);
}

int clip_moves(const Clip *c)
{
    if (c->kind == 2)
        return 1;
    for (uint32_t i = 0; i < c->bones; i++) {
        const uint8_t *rec = c->trans[i];
        if (!rec) continue;
        uint32_t nt = rd32(rec + 8), nr = rd32(rec + 12);
        const uint8_t *tp = rec + rd32(rec + 0x1C), *rp = rec + rd32(rec + 0x20);
        float q0[4];
        for (int k = 0; k < 4; k++) q0[k] = rdf(rp + 4 * k);
        for (uint32_t s = 0; s < nt; s++)
            for (int k = 0; k < 3; k++)
                if (fabsf(rdf(tp + 24 * s + 12 + 4 * k)) > 1e-5f || fabsf(rdf(tp + 24 * s + 4 * k) - rdf(tp + 4 * k)) > 1e-5f)
                    return 1;
        for (uint32_t s = 0; s < nr; s++) {
            if (rdf(rp + 36 * s + 32) != 0) return 1;
            for (int k = 0; k < 4; k++)
                if (fabsf(rdf(rp + 36 * s + 16 + 4 * k)) > 1e-5f || fabsf(rdf(rp + 36 * s + 4 * k) - q0[k]) > 1e-5f)
                    return 1;
        }
    }
    return 0;
}

int clip_pose(const Clip *c, double seconds, uint32_t bone, float t[3], float q[4])
{
    if (bone >= c->bones || c->duration <= 0)
        return 0;
    double f = fmod(seconds / c->duration, 1.0);
    if (f < 0) f += 1;
    if (c->kind == 2) {
        float tn = (float)f;
        const uint8_t *tr = c->trans[bone], *keys = tr + 0x10;
        uint32_t n = tr[0xD];
        float scale = f32bits(0x38000100);
        if (n) {
            uint32_t k = 0;
            while (k + 1 < n && (rd16(keys + 8 * (k + 1)) & 0xFFFE) / 65534.0f <= tn) k++;
            float a[3], b[3], fr = 0;
            for (int i = 0; i < 3; i++) a[i] = (int16_t)rd16(keys + 8 * k + 2 + 2 * i) * rdf(tr + 4 * i) * scale;
            if (k + 1 < n) {
                float t0 = (rd16(keys + 8 * k) & 0xFFFE) / 65534.0f, t1 = (rd16(keys + 8 * (k + 1)) & 0xFFFE) / 65534.0f;
                fr = t1 > t0 ? (tn - t0) / (t1 - t0) : 0;
                if (fr < 0) fr = 0;
                for (int i = 0; i < 3; i++) b[i] = (int16_t)rd16(keys + 8 * (k + 1) + 2 + 2 * i) * rdf(tr + 4 * i) * scale;
            } else {
                memcpy(b, a, sizeof b);
            }
            for (int i = 0; i < 3; i++) t[i] = a[i] + (b[i] - a[i]) * fr;
        }
        const uint8_t *rt = c->rot[bone], *rk = rt + 2;
        n = rt[1];
        if (n) {
            uint32_t k = 0;
            while (k + 1 < n && (rd16(rk + 8 * (k + 1)) & 0xFFFE) / 65534.0f <= tn) k++;
            float a[4];
            decode_rotation(rk + 8 * k, a);
            if (k + 1 < n) {
                float b[4], t0 = (rd16(rk + 8 * k) & 0xFFFE) / 65534.0f, t1 = (rd16(rk + 8 * (k + 1)) & 0xFFFE) / 65534.0f;
                float fr = t1 > t0 ? (tn - t0) / (t1 - t0) : 0;
                decode_rotation(rk + 8 * (k + 1), b);
                quat_slerp(a, b, fr < 0 ? 0 : fr, q);
            } else {
                memcpy(q, a, sizeof a);
            }
        }
        return 1;
    }
    const uint8_t *rec = c->trans[bone];
    if (!rec)
        return 0;
    uint32_t nt = rd32(rec + 8), nr = rd32(rec + 12);
    const uint8_t *tp = rec + rd32(rec + 0x1C), *rp = rec + rd32(rec + 0x20);
    double x = f * nt;
    uint32_t k = (uint32_t)x < nt ? (uint32_t)x : nt - 1;
    float fr = (float)(x - k);
    for (int i = 0; i < 3; i++) t[i] = rdf(tp + 24 * k + 4 * i) + rdf(tp + 24 * k + 12 + 4 * i) * fr;
    x = f * nr;
    k = (uint32_t)x < nr ? (uint32_t)x : nr - 1;
    fr = (float)(x - k);
    const uint8_t *s = rp + 36 * k;
    float angle = rdf(s + 32), n = 0;
    for (int i = 0; i < 4; i++) {
        float q0 = rdf(s + 4 * i), q1 = rdf(s + 16 + 4 * i);
        q[i] = angle == 0 ? q0 + q1 * fr : q0 * cosf(angle * fr) + q1 * sinf(angle * fr);
        n += q[i] * q[i];
    }
    n = sqrtf(n);
    for (int i = 0; i < 4; i++) q[i] /= n;
    return 1;
}
