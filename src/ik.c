/* ik.c - the game's arm IK (ik.h). */
#include <math.h>
#include <string.h>
#include "ik.h"

static void rot_get(const Mat4 *m, float r[9])
{
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 3; k++)
            r[3 * c + k] = m->m[4 * c + k];
}

static void rot_set(Mat4 *m, const float r[9])
{
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 3; k++)
            m->m[4 * c + k] = r[3 * c + k];
}

static void rot_mul(const float a[9], const float b[9], float out[9])
{
    float o[9];
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 3; k++)
            o[3 * c + k] = a[k] * b[3 * c] + a[3 + k] * b[3 * c + 1] + a[6 + k] * b[3 * c + 2];
    memcpy(out, o, sizeof o);
}

static void rot_apply(const float r[9], const float v[3], float out[3])
{
    float o[3];
    for (int k = 0; k < 3; k++)
        o[k] = r[k] * v[0] + r[3 + k] * v[1] + r[6 + k] * v[2];
    memcpy(out, o, sizeof o);
}

static void rot_transpose(const float r[9], float out[9])
{
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 3; k++)
            out[3 * k + c] = r[3 * c + k];
}

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void cross3(const float a[3], const float b[3], float o[3])
{
    float r[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    memcpy(o, r, sizeof r);
}

static float norm3(float a[3])
{
    float l = sqrtf(dot3(a, a));
    if (l > 1e-12f)
        for (int k = 0; k < 3; k++)
            a[k] /= l;
    return l;
}

void ik_quat_of(const float r[9], float q[4])
{
    float m00 = r[0], m11 = r[4], m22 = r[8], tr = m00 + m11 + m22;
    if (tr > 0) {
        float s = sqrtf(tr + 1) * 2;
        q[3] = s / 4; q[0] = (r[5] - r[7]) / s; q[1] = (r[6] - r[2]) / s; q[2] = (r[1] - r[3]) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1 + m00 - m11 - m22) * 2;
        q[3] = (r[5] - r[7]) / s; q[0] = s / 4; q[1] = (r[3] + r[1]) / s; q[2] = (r[6] + r[2]) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1 + m11 - m00 - m22) * 2;
        q[3] = (r[6] - r[2]) / s; q[0] = (r[3] + r[1]) / s; q[1] = s / 4; q[2] = (r[7] + r[5]) / s;
    } else {
        float s = sqrtf(1 + m22 - m00 - m11) * 2;
        q[3] = (r[1] - r[3]) / s; q[0] = (r[6] + r[2]) / s; q[1] = (r[7] + r[5]) / s; q[2] = s / 4;
    }
}

void ik_rot_of(const float q[4], float r[9])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z); r[3] = 2 * (x * y - z * w);     r[6] = 2 * (x * z + y * w);
    r[1] = 2 * (x * y + z * w);     r[4] = 1 - 2 * (x * x + z * z); r[7] = 2 * (y * z - x * w);
    r[2] = 2 * (x * z - y * w);     r[5] = 2 * (y * z + x * w);     r[8] = 1 - 2 * (x * x + y * y);
}

void ik_slerp(const float a[4], const float b[4], float f, float out[4])
{
    float bb[4] = { b[0], b[1], b[2], b[3] }, d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) {
        d = -d;
        for (int k = 0; k < 4; k++) bb[k] = -bb[k];
    }
    float ka = 1 - f, kb = f;
    if (d < 0.9995f) {
        float th = acosf(d), s = sinf(th);
        ka = sinf((1 - f) * th) / s;
        kb = sinf(f * th) / s;
    }
    float o[4], l = 0;
    for (int k = 0; k < 4; k++) {
        o[k] = ka * a[k] + kb * bb[k];
        l += o[k] * o[k];
    }
    l = sqrtf(l);
    for (int k = 0; k < 4; k++)
        out[k] = l > 0 ? o[k] / l : a[k];
}

static void rot_slerp(const float a[9], const float b[9], float f, float out[9])
{
    float qa[4], qb[4], q[4];
    ik_quat_of(a, qa);
    ik_quat_of(b, qb);
    ik_slerp(qa, qb, f, q);
    ik_rot_of(q, out);
}

/* 0x104E90: the shortest rotation taking the unit vector a onto b */
static void arc(const float a[3], const float b[3], float r[9])
{
    float ax[3], c = dot3(a, b);
    cross3(a, b, ax);
    float s = norm3(ax);
    if (s < 1e-6f) {
        float id[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        memcpy(r, id, sizeof id);
        return;
    }
    float q[4] = { ax[0] * sqrtf((1 - c) / 2), ax[1] * sqrtf((1 - c) / 2), ax[2] * sqrtf((1 - c) / 2), sqrtf((1 + c) / 2) };
    ik_rot_of(q, r);
}

static Mat4 rigid_inverse(const Mat4 *m)
{
    Mat4 o = *m;
    float r[9], rt[9];
    rot_get(m, r);
    rot_transpose(r, rt);
    rot_set(&o, rt);
    float t[3] = { m->m[12], m->m[13], m->m[14] }, it[3];
    rot_apply(rt, t, it);
    o.m[12] = -it[0]; o.m[13] = -it[1]; o.m[14] = -it[2];
    o.m[3] = o.m[7] = o.m[11] = 0; o.m[15] = 1;
    return o;
}

void ik_arm(Mat4 *node, const Tree *tree, const int chain[3], const float target[3], const float rot[9], float w)
{
    if (w <= 0 || !tree || tree->n > 256)
        return;
    if (w > 0.999f)
        w = 1;
    static Mat4 old[256];
    memcpy(old, node, sizeof *node * tree->n);
    Mat4 *M0 = &node[chain[0]], *M1 = &node[chain[1]], *M2 = &node[chain[2]];
    float P0[3] = { M0->m[12], M0->m[13], M0->m[14] }, P1[3] = { M1->m[12], M1->m[13], M1->m[14] };
    float P2[3] = { M2->m[12], M2->m[13], M2->m[14] }, u[3], v[3], d[3];
    for (int k = 0; k < 3; k++) {
        u[k] = P1[k] - P0[k];
        v[k] = P2[k] - P1[k];
        d[k] = target[k] - P0[k];
    }
    float uh[3] = { u[0], u[1], u[2] }, vh[3] = { v[0], v[1], v[2] }, dh[3] = { d[0], d[1], d[2] };
    float L1 = norm3(uh), L2 = norm3(vh), D = norm3(dh);
    if (L1 < 1e-6f || L2 < 1e-6f)
        return;
    int reached = D <= L1 + L2;
    float c = (L1 * L1 + L2 * L2 - D * D) / (2 * L1 * L2);
    if (c < -1) { c = -1; reached = 0; }
    if (c > 1) { c = 1; reached = 0; }
    float th = acosf(c), wv[3], n[3];
    cross3(uh, vh, n);                               /* the clip's bend plane; (1, 0, 0) when the arm is straight */
    if (dot3(n, n) < 1e-6f) { n[0] = 1; n[1] = n[2] = 0; }
    norm3(n);
    cross3(n, uh, wv);                               /* in the plane, at right angles to u, towards v */
    norm3(wv);
    float f[3], R1[9], e[3], Rs[9];
    for (int k = 0; k < 3; k++)
        f[k] = -c * uh[k] + sinf(th) * wv[k];
    arc(vh, f, R1);
    for (int k = 0; k < 3; k++)
        e[k] = u[k] + L2 * f[k];
    norm3(e);
    arc(e, dh, Rs);
    /* the solved upper arm and forearm (model space) */
    float r0[9], r1[9], r2[9], s0[9], s1[9], RsR1[9];
    rot_get(M0, r0);
    rot_get(M1, r1);
    rot_get(M2, r2);
    rot_mul(Rs, r0, s0);
    rot_mul(Rs, R1, RsR1);
    rot_mul(RsR1, r1, s1);
    float su[3];
    rot_apply(Rs, u, su);
    Mat4 n0 = *M0, n1 = *M1, n2 = *M2;
    if (w >= 1) {
        rot_set(&n0, s0);
        rot_set(&n1, s1);
        for (int k = 0; k < 3; k++)
            n1.m[12 + k] = P0[k] + su[k];
    } else {                                         /* the upper arm, then the forearm in its frame, by w */
        float b0[9];
        rot_slerp(r0, s0, w, b0);
        rot_set(&n0, b0);
        Mat4 sol0 = *M0, sol1 = *M1;
        rot_set(&sol0, s0);
        rot_set(&sol1, s1);
        for (int k = 0; k < 3; k++)
            sol1.m[12 + k] = P0[k] + su[k];
        Mat4 la = mat_mul(rigid_inverse(M0), *M1), ls = mat_mul(rigid_inverse(&sol0), sol1), lb = la;
        float ra[9], rs[9], rb[9];
        rot_get(&la, ra);
        rot_get(&ls, rs);
        rot_slerp(ra, rs, w, rb);
        rot_set(&lb, rb);
        for (int k = 0; k < 3; k++)
            lb.m[12 + k] = la.m[12 + k] + (ls.m[12 + k] - la.m[12 + k]) * w;
        n1 = mat_mul(n0, lb);
    }
    /* the hand: its place rides with the forearm (at the target when reached and fully on), its rotation the target's */
    Mat4 lh = mat_mul(rigid_inverse(M1), *M2);
    n2 = mat_mul(n1, lh);
    if (w >= 1 && reached)
        for (int k = 0; k < 3; k++)
            n2.m[12 + k] = target[k];
    float hr[9];
    if (w >= 1)
        memcpy(hr, rot, sizeof hr);
    else
        rot_slerp(r2, rot, w, hr);
    rot_set(&n2, hr);
    *M0 = n0;
    *M1 = n1;
    *M2 = n2;
    /* 0x2F9320: the nodes below (fingers) follow their parents */
    static uint8_t moved[256];
    memset(moved, 0, tree->n);
    moved[chain[0]] = moved[chain[1]] = moved[chain[2]] = 1;
    for (uint32_t i = 0; i < tree->n; i++) {
        int p = tree->nodes[i].parent;
        if (moved[i] || p < 0 || (uint32_t)p >= i || !moved[p])
            continue;
        node[i] = mat_mul(node[p], mat_mul(rigid_inverse(&old[p]), old[i]));
        moved[i] = 1;
    }
}
