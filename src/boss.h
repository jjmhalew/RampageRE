/* boss.h - the bosses: each city's last block and the ambient bosses seen in the blocks before it
 * (docs/BOSS_NOTES.md).
 *
 * [R] What the game has, and what this is the port of (decompiled in build/scripts/Boss_*.scr):
 *   - Boss_Tank.bsf (BossTank, "Carbonator": Las Vegas LV09_Neros, Los Angeles LA08_Hollywood_Blv) and
 *     Boss_Tank_Bomb.bsf (BossTankBomb, the shell it lobs onto a roof);
 *   - Boss_Helicopter.bsf (BossHelicopter, "Scumhawk": San Francisco SF10_Presidio);
 *   - Boss_Jumper.bsf (BossJumper, "Leeper", an Actor: Hong Kong HK10_Fortress) and Boss_Jumper_Bomb.bsf
 *     (BossJumperBomb, the burning goo it leaves where it lands);
 *   - Boss_Chicago_Saucer.bsf (BossSaucer: Chicago CG10_Block10, London GB08_BigBen) with its rotor
 *     (BossSaucerRotor) and Boss_Chicago_Whirlwind.bsf (BossSaucerWhirlwind, the tornado it drops);
 *   - Boss_NY_Blimp.bsf (NYBlimp: New York NY10_Empire_State), its two Boss_NY_Generator.bsf (NYGenerator) and the
 *     barriers it raises over the Empire buildings (NewYork.scr BlimpBarrierFire);
 *   - Boss_Blimp_Ambient.bsf (BossBlimpAmbient in New York, BossHelicopterAmbient in San Francisco) and
 *     Boss_Saucer_Ambient.bsf (BossSaucerAmbient in Chicago and London): the bosses crossing the blocks waiting
 *     behind the one being played;
 *   - the natives they lean on: the SplineMover (update 0x33ACA0, its Kochanek-Bartels curve 0x13B780 / 0x13B2F0),
 *     the World's building finders (findBuildingNearMonster 0x318240 over the monsters' span 0x318390,
 *     findBuildingNearPosition 0x2B4D50, getRandomBuilding 0x2AEC40), gCommon.isCityDestroyed (0x4D7E4F),
 *     Monster.isClimbing (+0xB9C0) and playReaction (0x2D9890 + 0x2D9710).
 *
 * Engine-agnostic, like army.h: nothing here draws, plays a sound or fires a shot itself. The caller gives each
 * frame what the bosses see of the monsters and the buildings, and gets back what to draw (the parts) and what
 * happened (events). The weapons are the army's: a boss holds ArmyWeapons and the host runs their countdown and
 * fire (army.c's weapon_make / weapon_tick), so the shots, beams and hits on the monster are the army's own.
 * Positions are the game's: z up, the street at z = 0, the front row of buildings on y 0 .. 10 facing the street
 * and the camera at -y; matrices column-major 4 x 4; facings in radians about z (0 faces +y, pi / 2 faces -x,
 * -pi / 2 faces +x), the monsters' too (BossMonster.heading, BE_FACING). */
#ifndef RAMPAGE_BOSS_H
#define RAMPAGE_BOSS_H
#include <stdint.h>
#include "data.h"
#include "army.h"
#include "collide.h"

typedef struct Boss Boss;

/* ---- what a boss object is (by its template's Script|ScriptName) */
enum {
    BK_TANK,            /* Boss_Tank.bsf */
    BK_TANK_BOMB,       /* Boss_Tank_Bomb.bsf */
    BK_HELI,            /* Boss_Helicopter.bsf */
    BK_JUMPER,          /* Boss_Jumper.bsf (an Actor: plays clips) */
    BK_JUMPER_BOMB,     /* Boss_Jumper_Bomb.bsf */
    BK_SAUCER,          /* Boss_Chicago_Saucer.bsf */
    BK_WHIRLWIND,       /* Boss_Chicago_Whirlwind.bsf */
    BK_BLIMP,           /* Boss_NY_Blimp.bsf */
    BK_GENERATOR,       /* Boss_NY_Generator.bsf */
    BK_AMBIENT_BLIMP,   /* Boss_Blimp_Ambient.bsf */
    BK_AMBIENT_SAUCER,  /* Boss_Saucer_Ambient.bsf */
    BK_ROTOR,           /* an Attachment (BossSaucerRotor): no script, placed by its owner each frame */
    BK_NONE
};

/* ---- one part to draw (boss_part) */
typedef struct {
    const char *name;   /* its template (Objects1_Props) */
    Group tmpl, inst;   /* the template; the instance book (its AIInstance / ObjectInstance group, or none) */
    int kind, alive;    /* BK_*; 0 once deleted (skip it) */
    int owner;          /* BK_ROTOR: the part it belongs to; -1 otherwise */
    int is_boss;        /* the block's boss itself (its health is the challenge HUD's) */
    float pos[3];       /* its place */
    float facing[3];    /* the game's getFacing: x, y tilts and the z turn (radians) */
    float world[16];    /* column-major; always up to date. Draw `shape` with it, scaled by `scale` */
    const char *shape;  /* the model (Shape|Shape, actors their ActorAnimShape's); "InvisShape": draw nothing */
    float scale;        /* RenderScale (1 by default) */
    int animate;        /* object flag 0x8000000: the shape's own RawAnim plays (0x309CE0), else it holds its start */
    /* actors (the Jumper): the ActorAnimShape| record it plays */
    char anim[24];
    const char *clip;   /* its Anim|AnimName, NULL: the shape's own */
    float anim_time, anim_len;
    int anim_loop;
    float hp_frac;      /* what updateChallengeHud was last told for it (1 at the start) */
} BossPart;

/* ---- what the module asks of its host */
typedef struct {
    /* a property group by book and name: "Objects1_Props" for a template or a path (Path_NYEmpire1), "Weapons" */
    Group (*find)(void *ctx, const char *book, const char *name);
    /* the length in seconds of the clip anim (NULL: the shape's own RawAnim) of shape; 0 = unknown */
    float (*clip_seconds)(void *ctx, const char *anim, const char *shape);
    /* optional: node (a WeaponNode, "Attach_Rotor_Right", ...) of the part as drawn now, in the world; 0 = unknown
     * (the part's place stands in) */
    int (*node_world)(void *ctx, const BossPart *p, const char *node, float out[16]);
    /* optional: a model's bounds in its own frame (AutoCollisionBox); 0 = unknown (the shape is then a
     * sphere of radius 1, collide_shape_pick) */
    int (*shape_box)(void *ctx, const char *shape, float min[3], float max[3]);
    int (*shape_sphere)(void *ctx, const char *shape, float centre[3], float *radius);   /* the sphere the model keeps (0x184CB0); nonzero = found */
    /* gWorld.isSphereVisible (id 0x40 -> 0x16C0A0): the sphere inside the camera's frustum; unset, the view's x range */
    int (*visible)(void *ctx, const float p[3], float r);
    /* the army's weapons (army.c weapon_make over a template's Weapon| group, and one frame of weapon_tick with no
     * owning unit). Without them the bosses do not fire */
    int (*weapon_make)(void *ctx, ArmyWeapon *w, Group weapon_group);
    void (*weapon_tick)(void *ctx, ArmyWeapon *w, int may, const float from[3], const float dir[3], int target,
                        float dt);
    void *ctx;
} BossHost;

/* ---- what the bosses see of a monster, this frame (index = its slot, 0 .. 3) */
typedef struct {
    float pos[3];
    float heading;                                   /* the game's facing z */
    const char *move;                                /* its state: "Ground...", "Jump...", "...Side...", ... */
    int climbing;                                    /* Monster.isClimbing: +0xB9C0 != 0, on a building's wall */
    const float (*spheres)[4]; int nspheres;         /* its collision spheres in the world */
    float box_min[3], box_max[3];                    /* its collision box in the world (used without spheres) */
} BossMonster;

/* ---- a building as the bosses see it (index = the caller's; events name it) */
typedef struct {
    float pos[3];       /* getPosition: the middle of its footprint on the street (x = LeftEdgeXPos + 2.5 x width,
                         * y = 5, + BackRowOffset on the back row) */
    float min[3], max[3];   /* its box in the world */
    int width, storeys;     /* getSizeX (cells along the front), getSizeZ (the template's storeys) */
    float height;       /* getCurrentHeight: the top of what stands now */
    int standing;       /* still there (+0x14C): neither destroyed nor gone */
    int falling;        /* isFalling: its collapse has started */
    int destroyed;      /* isDestroyed */
    float hp_frac;      /* getDestructionFrac: the share of its BuildingHP left, 1 .. 0 */
    Group inst;         /* its BuildingInstance| group (the NY barriers' VolumeTrigger| groups) */
} BossBuilding;

/* ---- what happened */
enum {
    BE_EFFECT,          /* start effect `name` at pos along dir; id its handle. obj >= 0: it follows that part
                         * (offset pos - the part's place), on its node `node` when not NULL, or its node number
                         * `level` when >= 0 (Leeper_Steam on nodes 2, 5, 8); monster >= 0: it follows that monster
                         * (Hazard_Electricity_Explode); neither: it stays at pos */
    BE_EFFECT_STOP,     /* the effect with handle id stops */
    BE_SOUND,           /* add3DSfx `name` at pos from part obj; level 0: stop every sound of part obj (it is gone) */
    BE_REACTION,        /* monster.playReaction(name): start its move state `name` (React_Airborne, React_Tornado,
                         * Jump_Air_Borne_Fall, React_Front_Zap, React_Side_L_Zap) */
    BE_VEL,             /* monster.setVel(v, 0): its velocity becomes v (it is thrown) */
    BE_HP,              /* monster.addHitPoints(amount): + heals, - hurts (0 .. its most; nothing at 0 HP) */
    BE_FACING,          /* monster.setFacingZ(amount) */
    BE_PUSH,            /* the monster is pushed by v (not sent any more: the PushOutMonsters parts' and the DoPushout
                         * triggers' pushes are the collision world's, collide_monster) */
    BE_STATS,           /* gCommon.addStats(part obj, monster, level): army_add_stats(army, monster, tmpl, level) */
    BE_BOSS_HEALTH,     /* gCommon.updateChallengeHud(amount): the boss's health bar, 0 .. 1 */
    BE_DEFEATED,        /* the boss's CompletionCallback `name` (gWorld.iCallScriptV): see BOSS_NOTES "The block" */
    BE_CAMERA_RMH,      /* Levels::changeTwoTiltCameraRMH(amount): the TwoTilts camera's RepMonsterHeight */
    BE_COLLAPSE,        /* building.collapseBuilding() */
    BE_NO_AUTOCOLLAPSE, /* building.disableAutoCollapse(1): it does not fall by itself when its HP runs out */
    BE_CELL_DAMAGE,     /* building.setCellDamage(level = side, a = floor, b = cell, c = state) */
    BE_FORCE_DAMAGE,    /* the world object `name` at pos: forceDamage (RoofItem.bsf: it is knocked off) */
    BE_SPAWNED,         /* part obj was made */
    BE_REMOVED          /* part obj is gone (requestDelete) */
};
typedef struct {
    int type, obj, monster, building, id, level, a, b, c;
    const char *name, *node;
    Group tmpl;
    float pos[3], dir[3], v[3], amount;
} BossEvent;

Boss *boss_new(const BossHost *host);
void boss_free(Boss *b);
/* [G] gWorld.isSphereVisible's stand-in: the street's x range the camera sees (default -45 .. 45) */
void boss_set_view(Boss *b, float xmin, float xmax);
/* gCommon.isCityDestroyed (0x4D7E4F): set once the city's last block is finished; the bosses then leave */
void boss_set_city_destroyed(Boss *b, int on);

/* the template's script is a boss's (Boss_*.bsf): the army must not spawn these (army_load_block takes every
 * AIInstance; the viewer skips these there, or removes the AK_OTHER unit the army makes of them) */
int boss_is_boss_template(Group tmpl);

/* the block being played (its BlockDef| group): the boss's AIInstance (spawned by the same rules as gAIMgr once
 * boss_activate is called), the generators among its ObjectInstance|s, the world objects the bosses look for
 * (the Hong Kong palace's roof adornments). Returns the number of boss records */
int boss_load_block(Boss *b, Group block_def);
/* a block waiting behind it, `y` further back (city.c's 41 x slot): its AttackObjectInstance|s with an ambient
 * boss's script (LoadWhenWaiting on slot 1, LoadWhenDistant on slot 2, as city.c places them) start there */
int boss_load_waiting(Boss *b, Group block_def, float y, int slot);
void boss_activate(Boss *b, int on);                 /* gAIMgr.activate: the boss's clock runs from here */
void boss_clear(Boss *b);                            /* everything gone (a new block) */
/* a boss object made by name at pos (createAttackObject + setPoint + callOnInit), inst its instance book (or
 * none); -1 if the template is unknown */
int boss_spawn(Boss *b, const char *tmpl_name, Group inst, const float pos[3]);

void boss_update(Boss *b, float dt, const BossMonster *m, int nm, const BossBuilding *bl, int nb);

int boss_count(const Boss *b);                       /* parts (skip !alive) */
const BossPart *boss_part(const Boss *b, int i);
int boss_events(Boss *b, BossEvent *out, int max);   /* takes the events since the last call */
/* the block's boss: its part (-1 none yet / gone), and whether it has been beaten */
int boss_main(const Boss *b);
int boss_defeated(const Boss *b);

/* ---- the monster's side */
/* an armed attack sphere reaching boss parts: their TakeDamage(monster, amount, 0 [G: Concussive], the way from
 * the monster). How many it hit */
int boss_hit(Boss *b, int monster, const float centre[3], float radius, float amount, unsigned record);

/* ---- the collision world (collide.c) */
/* the parts and the VolumeTriggers put into the frame's world: each alive part with its shape (tag
 * COLLIDE_TAG(COLLIDE_OWN_BOSS, part), kind 0; flags 4 and its own: PushOutMonsters, IsObstacle, 8), each alive
 * trigger as its box (Center +- Extents; tag COLLIDE_TAG(COLLIDE_OWN_BOSS, trigger), kind 1; flags 4 |
 * COLLIDE_F_TRIGGER, | COLLIDE_F_PUSHOUT with DoPushout [R 0x3360F4, 0x284CD0]: a shot meets it). collide_monster pushes the monster out of
 * both and reports the triggers it touched */
void boss_collide_add(Boss *b, CollideWorld *w);
/* collide_monster's touch of trigger `trigger` (a kind 1 object's tag index) by monster `monster` with the normal:
 * 0x335BF0 fires its FireFunc at the next boss_update, its MinRefireTime permitting */
void boss_trigger_touch(Boss *b, int trigger, int monster, const float normal[3]);

#endif
