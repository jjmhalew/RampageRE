/* ik.h - the game's arm IK (StdIKPairAnimator, vtable 0x44AFA0: the two-bone solve 0x2FD0B0 and its blend 0x2F7E10)
 * on posed model-space node matrices (column vectors). Rotations are 3 x 3, column-major (R[3 * c + r]); quaternions
 * x, y, z, w in the usual column-vector sense (v' = q v q*). */
#ifndef RAMPAGE_IK_H
#define RAMPAGE_IK_H
#include "city.h"

/* chain: upper arm, forearm, hand. The hand goes to pos with the rotation rot (model space), by the share w (0 .. 1):
 * the elbow keeps the clip's bend plane (no pole), the upper arm and the forearm turn by the shortest arcs, the
 * nodes below the hand (fingers) follow it. */
void ik_arm(Mat4 *node, const Tree *tree, const int chain[3], const float pos[3], const float rot[9], float w);

void ik_quat_of(const float r[9], float q[4]);
void ik_rot_of(const float q[4], float r[9]);
void ik_slerp(const float a[4], const float b[4], float f, float out[4]);   /* sign-aligned */

#endif
