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

/* ---- the legs (docs/FOOTSTEP_NOTES.md, "The foot IK") ---------------------------------------------------------- */

/* the 3-link leg (vtable 0x44AF60, MiscData|Use3LinkFootIK: the solve 0x2FCA70 and its blend 0x2F66D0). chain:
 * thigh, calf, horselink, foot. The thigh and the horselink stay parallel, all three turn about the thigh's own z
 * (each bone runs along its x); the foot goes to pos with the rotation rot by the share w, as ik_arm's hand; rot
 * NULL: the foot keeps its rotation as posed. The 2-link leg is ik_arm itself (StdIKPairAnimator, 0x44AFA0). */
void ik_leg3(Mat4 *node, const Tree *tree, const int chain[4], const float pos[3], const float rot[9], float w);

/* the monster's foot IK (monster +0x258C, made by 0x2E5640 from the animation player's setup 0x301AE0 when the
 * skeleton has both "Bip01 L Foot" and "Bip01 R Foot"; run each frame after the arms by the monster update at
 * 0x2E1338: vtable +0x8, 0x2F9560). Feet are as drawn: 0 the left, 1 the right (mirrored: the other side's nodes,
 * reflected), the records of footstep.c. */
typedef struct IkFeet {
    int links;                                       /* in: 3 the 2-link chain (thigh, calf, foot: 0x2F8D20), 4 the
                                                      * 3-link (thigh, calf, horselink, foot: 0x2F7BC0); 0 none */
    float w[2], pos[2][3];                           /* in: each foot's share (chain +0x48) and where it goes (model
                                                      * space, as drawn: footstep.c's foot_update) */
    /* out, each posing: as the clips posed them, before the IK (model space, as drawn) */
    int ok[2];                                       /* the foot has its chain */
    float foot[2][3];                                /* the foot (0x2E5C40's node) */
    float hip[2][3];                                 /* the chain's first node, the thigh (0x2F8F50, link 0) */
    float reach[2];                                  /* the chain's length as posed (0x2F9060) */
    /* called once the feet are noted, before the legs bend: the game's foot object update (0x33D450) runs right
     * there, between the posing and the foot IK, so it may set w and pos from this very pose; NULL: they stay */
    void (*update)(void *ctx, struct IkFeet *f);
    void *ctx;
    int chain[2][4], ready;                          /* the unmirrored chains, found on the first posing */
} IkFeet;
/* after the clips and the arm IK have posed node (model space, relative to the monster's base): note the feet as
 * posed, call f->update, then bend the legs whose share is above 0 (rotation: the foot's own, as 0x33CDE0 copies the
 * posed node's into the target) */
void ik_feet(Mat4 *node, const Tree *tree, int mirrored, IkFeet *f);
/* a node's place in the monster's model space as its clip alone poses it at `seconds` (no blend, no face, no head
 * turn, and the root as the clip has it: no motion taken out), as drawn (mirrored: the other side's node,
 * reflected): the animation player's own clip query (+0x2A24 vtable +0x34 = NodeTreeAnim 0x196D20, 0x33C290
 * mirrored), which the foot object asks at the clip's time 0. 0 = ok */
int ik_clip_node(const Instance *in, double seconds, const char *name, float out[3]);

#endif
