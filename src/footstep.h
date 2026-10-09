/* footstep.h - the monster's footfalls and landings (docs/FOOTSTEP_NOTES.md):
 *   1. the effect names: the property book's MonsterFx group (read by 0x2F5D80 into the monster's effect record at
 *      +0x7DFC: GroundFootStepFx +0x140, WaterFootStepFx +0x144, GroundImpactFx +0x150, WaterImpactFx +0x154);
 *   2. the clip events Monsters::footStep / footPlant / cancelFootStep (script methods 0x88 / 0x87 / 0x89, the
 *      monster's foot object at +0xC81C: 0x33CDA0, 0x33CCC0, 0x33CA10): footPlant puts a footfall under the foot,
 *      footStep lifts the foot, cancelFootStep plants the feet where they stand;
 *   3. the landing (0x2DC450): the big footfall under the monster's middle.
 * Engine-free: the caller looks the bones up, knows the ground and the water, and starts the effects itself
 * (fx_spawn(w, fx.name, fx.pos, NULL, 1)). Positions are the game's: z up. Foot 0 is the clip's left foot. */
#ifndef RAMPAGE_FOOTSTEP_H
#define RAMPAGE_FOOTSTEP_H
#include <stdint.h>
#include "data.h"

/* ---- 1. the names --------------------------------------------------------------------------------------------- */

typedef struct {                                     /* "" = none: that effect is never started (0x2F4A20 ...) */
    char ground_step[64];                            /* GroundFootStepFx: "footfall.edf" in all seven books */
    char water_step[64];                             /* WaterFootStepFx: "footfall_water.edf" */
    char ground_impact[64];                          /* GroundImpactFx: "footfall_big.edf" */
    char water_impact[64];                           /* WaterImpactFx: "footfall_big_water.edf" */
} FootProps;

/* the names every book gives (Amanda, Cal, George, Jill, Lizzie, Ralph, Rhett all agree) */
void foot_props_default(FootProps *p);
/* the book's MonsterFx group (city_book_group(city, "George_prop", "MonsterFx")): each key it has replaces the name,
 * each it lacks is none, as the game reads it; no group (at = 0): the defaults stay. How many keys were found. */
int foot_props_read(FootProps *p, Group monster_fx);

/* the bones (the animation player's setup 0x301AE0 finds them by these names, not through HandFootList): a clip's
 * foot 0 is "Bip01 L Foot" and 1 "Bip01 R Foot"; mirrored they swap (0x33CCC0: foot = foot + 1 & 1), which with
 * scene_monster_node (the drawn side's node, reflected) is the clip's foot as drawn. */
const char *foot_bone(int foot, int mirrored);
#define FOOT_IMPACT_BONE "Bip01 Spine1"              /* the landing's place (0x2E6000: anim +0x72C8, gp-0x71E4) */

/* ---- 2. the steps --------------------------------------------------------------------------------------------- */

/* the monster as the foot object reads it, this frame */
typedef struct {
    float foot[2][3];                                /* the feet's places in the world: foot_bone(f, mirrored) */
    float place[3];                                  /* the monster's place (+0x2C) */
    float fwd[3];                                    /* its forward axis (its model y: "forward 0 1"), 1 long */
    float vel[3];                                    /* getVelocity (0x2E7460): +0xBD30 plus the stick's +0xA68 */
    float ground;                                    /* 0x2E8530: the height it stands on (its roof's top on a
                                                      * building, the world's ground, the floor under water) */
    int in_water;                                    /* 0x2D5F00: its box is in a water's box */
    float water_z;                                   /* that water's surface (the water's +0x34: Origin z) */
    uint32_t node_flags;                             /* its state node's flags (+0x2C) */
    int mirrored;                                    /* +0x7DA1 */
} FootPose;

/* the foot record (0x28 bytes from foot object +0, one per foot, indexed after the mirror swap) */
typedef struct {
    float weight;                                    /* +0x0: 1 held (planted or dragged), 0 free with the clip */
    float at[3];                                     /* +0xC: where it was planted, z the ground + the ankle */
    uint32_t flags;                                  /* +0x24: FOOT_PLANTED, FOOT_DRAGGED */
} FootRec;
#define FOOT_PLANTED 1u
#define FOOT_DRAGGED 2u
typedef struct {
    FootRec rec[2];
    int anchor;                                      /* +0x6C: the planted foot the root follows, -1 none */
    int no_drag;                                     /* +0x69: MiscData|DisableFootDrag (Lizzie, Ralph) */
    float ankle;                                     /* +0x50: added to the ground for a planted foot [G: 0 will
                                                      * do; set by 0x33D4E0 from the right foot's bind pose] */
} FootState;

/* an effect to start: fx_spawn(w, name, pos, NULL, 1) (the game: no direction, the identity frame, no scale) */
typedef struct {
    const char *name;                                /* a FootProps name, never "" */
    float pos[3];
    int water;                                       /* the water's one (the step does not press the water: the
                                                      * wading does that, every frame, on its own) */
    int slot;                                        /* the game's emitter: 0 / 1 a foot's (+0x148 / +0x14C, after
                                                      * the mirror swap), 2 the landing's (+0x158) */
} FootFx;

void foot_init(FootState *s, int disable_foot_drag);
/* Monsters::footStep(foot) (0x33CDA0): the foot lifts: free, neither planted nor dragged. No effect. */
void foot_step(FootState *s, int foot, int mirrored);
/* Monsters::footPlant(foot) (0x33CCC0): only while the monster does not move (|vel|^2 < 1e-6), the footfall at the
 * foot's (x, y) on the ground it stands on, or on the water's surface (WaterFootStepFx). The foot's record is not
 * touched. 1 = *fx is to be started. */
int foot_plant(FootState *s, const FootProps *p, int foot, const FootPose *m, FootFx *fx);
/* Monsters::cancelFootStep(foot) (0x33CA10): the clip's start in a standing state (every such clip sends it for 0
 * and 1; only foot 0 does anything). In a foot-locking state (node flags 0x50 or 0x1080) and not moving: the foot
 * further forward is planted where it stands and becomes the anchor; the other is dragged, or free with
 * DisableFootDrag. No effect. Returns the anchor, -1 when nothing happened. */
int foot_cancel(FootState *s, int foot, const FootPose *m);

/* ---- 3. the landing ------------------------------------------------------------------------------------------- */

/* 0x2DC450, every landing from the air: the big footfall (GroundImpactFx / WaterImpactFx) at FOOT_IMPACT_BONE's
 * (x, y), on the ground or the water's surface. 1 = *fx is to be started. */
int foot_impact(const FootProps *p, const float spine[3], float ground, int in_water, float water_z, FootFx *fx);

#endif
