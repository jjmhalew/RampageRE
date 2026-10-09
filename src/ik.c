/* ik.c - the game's arm and leg IK (ik.h). */
#include <math.h>
#include <stdlib.h>
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

/* ---- the legs ------------------------------------------------------------------------------------------------- */

/* the rotation about z by the angle whose cosine and sine are c, s (column-major: x goes to (c, s, 0)); 0x101570
 * builds the same for row vectors, rows (c, s, 0), (-s, c, 0), (0, 0, 1) */
static void rot_z(float c, float s, float r[9])
{
    float o[9] = { c, s, 0, -s, c, 0, 0, 0, 1 };
    memcpy(r, o, sizeof o);
}

/* the child's new matrix under its parent's new one: its frame in the parent's, between the posed and the solved, by
 * w (the rotation slerped, the offset moved straight), as ik_arm blends its forearm */
static Mat4 blend_link(const Mat4 *old_parent, const Mat4 *old_child, const Mat4 *sol_parent, const Mat4 *sol_child,
                       const Mat4 *new_parent, float w)
{
    Mat4 la = mat_mul(rigid_inverse(old_parent), *old_child), ls = mat_mul(rigid_inverse(sol_parent), *sol_child);
    Mat4 lb = la;
    float ra[9], rs[9], rb[9];
    rot_get(&la, ra);
    rot_get(&ls, rs);
    rot_slerp(ra, rs, w, rb);
    rot_set(&lb, rb);
    for (int k = 0; k < 3; k++)
        lb.m[12 + k] = la.m[12 + k] + (ls.m[12 + k] - la.m[12 + k]) * w;
    return mat_mul(*new_parent, lb);
}

/* [R] 0x2FCA70 (thigh M0, calf M1, horselink M2, foot M3, the target): the thigh is first aimed at the target, its x
 * turned the shortest way onto the target's direction in its own frame (0x101190, then 0x104E90 from (1, 0, 0) at
 * 0x40A008 and 0x104BB0); then, the thigh and the horselink kept parallel, the chain is the triangle (D, L2, L1 + L3)
 * scaled by k = L3 / (L1 + L3) (0x2FCC98: 1 / (1 + L1 / L3)): the thigh turns about its z by a, cos a = (D'^2 + L3^2
 * - L2'^2) / (2 D' L3), the calf by g + pi from the thigh, cos g = (L2'^2 + L3^2 - D'^2) / (2 L2' L3) (0x101570 with
 * -sin g, -cos g), and the horselink back by the transpose of that turn. Each bone sits at its parent's place plus
 * the parent's length along the parent's x. Reached: D at most L1 + L2 + L3 and neither cosine clamped to -1 .. 1.
 * -1: no solution (a bone, or the scaled triangle, of no length, where the game divides by zero) */
static int solve3(const Mat4 *M[4], const float target[3], Mat4 sol[3])
{
    float P[4][3], d[3], L[3];
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++)
            P[i][k] = M[i]->m[12 + k];
    for (int k = 0; k < 3; k++)
        d[k] = target[k] - P[0][k];
    for (int i = 0; i < 3; i++) {
        float e[3] = { P[i + 1][0] - P[i][0], P[i + 1][1] - P[i][1], P[i + 1][2] - P[i][2] };
        L[i] = sqrtf(dot3(e, e));
    }
    float D = sqrtf(dot3(d, d)), r0[9], aim[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    rot_get(M[0], r0);
    if (D > 0.001f) {                                /* the target's direction in the thigh's frame */
        float l[3] = { dot3(d, r0) / D, dot3(d, r0 + 3) / D, dot3(d, r0 + 6) / D }, x[3] = { 1, 0, 0 };
        arc(x, l, aim);
    }
    if (L[0] < 1e-6f || L[1] < 1e-6f || L[2] < 1e-6f)
        return -1;
    int reached = D <= L[0] + L[1] + L[2];
    float k = 1 / (1 + L[0] / L[2]), Dk = D * k, L2k = L[1] * k;
    if (Dk < 1e-6f)
        return -1;
    float ca = (Dk * Dk + L[2] * L[2] - L2k * L2k) / (2 * Dk * L[2]);
    float cg = (L2k * L2k + L[2] * L[2] - Dk * Dk) / (2 * L2k * L[2]);
    if (ca < -1) { ca = -1; reached = 0; } else if (!(ca <= 1)) { ca = 1; reached = 0; }
    if (cg < -1) { cg = -1; reached = 0; } else if (!(cg <= 1)) { cg = 1; reached = 0; }
    float aimed[9], ra[9], rg[9], rgt[9], s[3][9];
    rot_mul(r0, aim, aimed);
    rot_z(ca, sqrtf(1 - ca * ca), ra);
    rot_mul(aimed, ra, s[0]);                        /* the thigh: Rz(a) x aim x M0, as rows */
    rot_z(-cg, -sqrtf(1 - cg * cg), rg);
    rot_mul(s[0], rg, s[1]);                         /* the calf: Rz(g + pi) under the thigh */
    rot_transpose(rg, rgt);
    rot_mul(s[1], rgt, s[2]);                        /* the horselink: turned back, parallel to the thigh */
    float at[3] = { P[0][0], P[0][1], P[0][2] };
    for (int i = 0; i < 3; i++) {
        sol[i] = *M[i];
        rot_set(&sol[i], s[i]);
        for (int j = 0; j < 3; j++)
            sol[i].m[12 + j] = at[j];
        for (int j = 0; j < 3; j++)                  /* the next one: this one's length along its x */
            at[j] += L[i] * s[i][j];
    }
    return reached;
}

/* [R] 0x2F66D0 (the 3-link's vtable +0x18) around the solve: at the full share the three bones take the solution,
 * the foot the target when reached (else it rides on the horselink, 0x2F79A0) and the target's rotation; then
 * 0x1017F0 on each [G: an orthonormalisation; these matrices are rigid]. Below the full share [G: read as 0x2F7E10,
 * which has the same blocks with one bone less, as ik_arm ports it]: the thigh slerped in the model's frame, the calf
 * and the horselink in their parents', the foot riding with its rotation slerped. Then 0x2F9320: the nodes below
 * follow. */
void ik_leg3(Mat4 *node, const Tree *tree, const int chain[4], const float target[3], const float rot[9], float w)
{
    if (w <= 0 || !tree || tree->n > 256)
        return;
    if (w > 0.999f)
        w = 1;
    static Mat4 old[256];
    memcpy(old, node, sizeof *node * tree->n);
    const Mat4 *M[4] = { &old[chain[0]], &old[chain[1]], &old[chain[2]], &old[chain[3]] };
    Mat4 sol[3], n[4];
    int reached = solve3(M, target, sol);
    if (reached < 0)
        return;
    if (w >= 1) {
        n[0] = sol[0];
        n[1] = sol[1];
        n[2] = sol[2];
    } else {
        float r0[9], s0[9], b0[9];
        rot_get(M[0], r0);
        rot_get(&sol[0], s0);
        rot_slerp(r0, s0, w, b0);
        n[0] = *M[0];
        rot_set(&n[0], b0);
        n[1] = blend_link(M[0], M[1], &sol[0], &sol[1], &n[0], w);
        n[2] = blend_link(M[1], M[2], &sol[1], &sol[2], &n[1], w);
    }
    n[3] = mat_mul(n[2], mat_mul(rigid_inverse(M[2]), *M[3]));   /* the foot rides on the horselink */
    if (w >= 1 && reached)
        for (int k = 0; k < 3; k++)
            n[3].m[12 + k] = target[k];
    float r3[9], fr[9];
    rot_get(M[3], r3);
    if (!rot)
        memcpy(fr, r3, sizeof fr);
    else if (w >= 1)
        memcpy(fr, rot, sizeof fr);
    else
        rot_slerp(r3, rot, w, fr);
    rot_set(&n[3], fr);
    for (int i = 0; i < 4; i++)
        node[chain[i]] = n[i];
    static uint8_t moved[256];                       /* 0x2F9320: the nodes below (the toes) follow their parents */
    memset(moved, 0, tree->n);
    for (int i = 0; i < 4; i++)
        moved[chain[i]] = 1;
    for (uint32_t i = 0; i < tree->n; i++) {
        int p = tree->nodes[i].parent;
        if (moved[i] || p < 0 || (uint32_t)p >= i || !moved[p])
            continue;
        node[i] = mat_mul(node[p], mat_mul(rigid_inverse(&old[p]), old[i]));
        moved[i] = 1;
    }
}

static int name_ends(const char *s, const char *tail)
{
    size_t a = s ? strlen(s) : 0, b = strlen(tail);
    return a >= b && !strcmp(s + a - b, tail);
}

/* [R] the chains: 0x2F9720 (vtable +0x1C, from 0x3024D0 with the animation player's +0x72C0 / +0x72C4, the nodes
 * "Bip01 L Foot" and "Bip01 R Foot") sets chains 0 and 1 up with vtable +0x14: the 2-link's 0x2F8D20 takes the
 * foot, its parent and grandparent, the 3-link's 0x2F7BC0 one parent more; both start with the identity target and
 * the share 0. Mirrored, a foot as drawn is the other side's node reflected (S k S), as the arms' monster_ik works. */
void ik_feet(Mat4 *node, const Tree *tree, int mirrored, IkFeet *f)
{
    if (!f || !tree || !node)
        return;
    int n = f->links == 4 ? 4 : f->links == 3 ? 3 : 0;
    f->ok[0] = f->ok[1] = 0;
    if (!n)
        return;
    if (!f->ready) {
        static const char *const FOOT[2] = { "Bip01 L Foot", "Bip01 R Foot" };
        f->ready = 1;
        for (int s = 0; s < 2; s++) {
            for (int k = 0; k < 4; k++)
                f->chain[s][k] = -1;
            for (uint32_t i = 0; i < tree->n; i++)
                if (name_ends(tree->nodes[i].name, FOOT[s])) {
                    int j = (int)i, k = n - 1;
                    for (; k >= 0 && j >= 0 && (uint32_t)j < tree->n; k--, j = tree->nodes[j].parent)
                        f->chain[s][k] = j;
                    if (k >= 0)                      /* too few parents: no chain */
                        f->chain[s][n - 1] = -1;
                    break;
                }
        }
    }
    int c[2][4];
    float sx = mirrored ? -1.0f : 1.0f;
    for (int s = 0; s < 2; s++) {
        if (f->chain[s][n - 1] < 0)
            continue;
        for (int k = 0; k < n; k++) {
            int b = f->chain[s][k], m = mirrored ? tree->nodes[b].mirror : b;
            c[s][k] = m >= 0 && (uint32_t)m < tree->n ? m : b;
        }
        float len = 0;
        for (int k = 0; k < 3; k++) {
            f->foot[s][k] = node[c[s][n - 1]].m[12 + k] * (k == 0 ? sx : 1);
            f->hip[s][k] = node[c[s][0]].m[12 + k] * (k == 0 ? sx : 1);
        }
        for (int k = 0; k + 1 < n; k++) {            /* 0x2F9060: the links' lengths as posed */
            const float *a = node[c[s][k]].m + 12, *b = node[c[s][k + 1]].m + 12;
            float e[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            len += sqrtf(dot3(e, e));
        }
        f->reach[s] = len;
        f->ok[s] = 1;
    }
    if (f->update)                                   /* 0x33D450, then the IK (0x2E1324, 0x2E1338) */
        f->update(f->ctx, f);
    for (int s = 0; s < 2; s++) {
        if (!f->ok[s] || f->w[s] <= 0 || getenv("RAMPAGE_NOIK"))   /* 0x2F9560: a chain of share 0 is left */
            continue;
        float p[3] = { f->pos[s][0] * sx, f->pos[s][1], f->pos[s][2] }, r[9];
        rot_get(&node[c[s][n - 1]], r);              /* 0x33CDE0: the target's rotation is the posed foot's */
        if (n == 3)
            ik_arm(node, tree, c[s], p, r, f->w[s]);
        else
            ik_leg3(node, tree, c[s], p, NULL, f->w[s]);
    }
}

int ik_clip_node(const Instance *in, double seconds, const char *name, float out[3])
{
    if (!in || !in->tree.n)
        return -1;
    int i = -1;
    for (uint32_t k = 0; k < in->tree.n && i < 0; k++)
        if (name_ends(in->tree.nodes[k].name, name))
            i = (int)k;
    if (i < 0)
        return -1;
    if (in->mirrored) {
        int m = in->tree.nodes[i].mirror;
        if (m >= 0 && (uint32_t)m < in->tree.n)
            i = m;
    }
    int path[64], np = 0;
    for (int j = i; j >= 0 && (uint32_t)j < in->tree.n && np < 64; j = in->tree.nodes[j].parent)
        path[np++] = j;
    Mat4 acc = mat_identity();
    for (int k = np - 1; k >= 0; k--) {
        const TreeNode *nd = &in->tree.nodes[path[k]];
        float tr[3] = { nd->t[0], nd->t[1], nd->t[2] }, q[4] = { nd->q[0], nd->q[1], nd->q[2], nd->q[3] };
        if (in->has_clip)
            clip_pose(&in->clip, seconds, (uint32_t)path[k], tr, q);
        int root = k == np - 1;
        if (root)
            for (int a = 0; a < 3; a++)
                if (in->still_root >> a & 1)
                    tr[a] = in->root_hold[a];
        Mat4 local = mat_tq(tr, q);
        if (root && in->root_turn != 0)
            local = mat_mul(mat_trs_z(0, 0, 0, -in->root_turn), local);
        acc = root ? local : mat_mul(acc, local);
    }
    out[0] = acc.m[12] * (in->mirrored ? -1.0f : 1.0f);
    out[1] = acc.m[13];
    out[2] = acc.m[14];
    return 0;
}
