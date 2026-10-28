/* challenge.h - the block challenges, the game's side (docs/CHALLENGE_NOTES.md):
 *   1. the challenge object (the block manager's +0x1D0 = 0x4D5050): loaded from the block's ChallengeDef when the
 *      block becomes current (0x3468F0 from 0x302AF0), its goals counted by name (0x346780: every gCommon.addStats
 *      awards the object's template name and its points as "Score", the block's end awards the clock's "Clock"),
 *      met when every goal is (0x346860), and the gWorld getters the scripts read (getChallengeIdentifier ...);
 *   2. the monster upgrade: a block whose ChallengeDef names player 1's monster in an Upgrade| group offers that
 *      move (1 ChargePunch, 2 DoubleJump, 4 RoarAttack, 8 JumpSmash), unless the monster has it; once the challenge
 *      is met gAIMgr makes a MonsterUpgrade object (0x3333F0), and the monster's blow on it takes the move
 *      (StreetObject.bsf TakeDamage, gAIMgr.aquireUpgrade 0x2A62B8);
 *   3. the block's end (0x3255B0): a challenge not met clears the city's flag 4 ("All Challenges Met").
 * Engine-free: the caller hands in the block's BlockDef group, the play type, player 1's monster and its flags, and
 * reports the awards; it draws the HUD (hud.h), the upgrade's effect and the banners itself. The strings handed out
 * are the property book's (valid while the city is open). */
#ifndef RAMPAGE_CHALLENGE_H
#define RAMPAGE_CHALLENGE_H
#include <stdint.h>
#include "data.h"
#include "hud.h"

#define CHALLENGE_GOALS 4                            /* 0x3468F0: at most 4 Goal| groups (+0x130 < 4) */
#define CHALLENGE_NAMES 16                           /* at most 16 templates a goal (+0x40 < 0x10) */
#define CHALLENGE_POINTS 5000                        /* ChallengeStatus.scr: getCurrentBlockIntProp("ChallengeDef.Points",
                                                      * 5000); no book sets Points */

/* the moves an Upgrade| group can give (the table 0x3F67A0), as gCommon.getMonsterFlags has them */
enum { CH_CHARGE_PUNCH = 1, CH_DOUBLE_JUMP = 2, CH_ROAR_ATTACK = 4, CH_JUMP_SMASH = 8,
       CH_SUPER_JUMP = 16 };                         /* the Wii's fifth: its blocks' Upgrade| groups name it */

/* a goal (0x4C bytes from +0x0 of the object, one per Goal| group) */
typedef struct {
    const char *name[CHALLENGE_NAMES];               /* +0x0: the templates that count ("FireTruckVehicle"), or the
                                                      * awards "Score" (points) and "Clock" (seconds left) */
    int nnames;                                      /* +0x40 */
    int required;                                    /* +0x44: > 0 at least that many; < 0 fewer than -required */
    int collected;                                   /* +0x48 */
} ChallengeGoal;

/* the MonsterUpgrade template (Objects1_Props) as the AI manager and StreetObject.bsf read it */
typedef struct {
    float pos[3];                                    /* PosX, PosY, PosZ: (0, -5, 5), a place in the block */
    float delay;                                     /* UpgradeDelay (1.5; 1 when absent): before it can be taken */
    float radius;                                    /* CollisionRadius (1) */
    const char *effect;                              /* Effect|AttachEffect "Powerup_Upgrade", on it while it waits */
    const char *text;                                /* TextID "MonsterUpgrade" ("Monster\nUpgrade"), the banner */
    float colour[4];                                 /* Color|TextColor (200, 0, 0): bytes / 255, alpha 1 [R] */
} ChallengeUpgradeProps;

enum { CU_NONE = 0, CU_WAITING, CU_TAKEN };          /* the MonsterUpgrade object */

typedef struct {
    Group def;                                       /* the block's ChallengeDef (at = 0: none); the getters read it */
    ChallengeGoal goal[CHALLENGE_GOALS];
    int ngoals;                                      /* +0x130 */
    const char *identifier;                          /* +0x134: Name, a key of <City>.loc ("Challenge01") */
    int met;                                         /* +0x138: what the last challenge_check found: 1 met, 0 not,
                                                      * -1 no goals (wasChallengeMet) */
    uint32_t upgrade;                                /* +0x13C: getChallengeUpgrade (0, CH_*); 0 once taken */
    /* the MonsterUpgrade object: gAIMgr +0x50 bit 4 (made this block), +0x58 (the delay) */
    ChallengeUpgradeProps up;
    int up_state;                                    /* CU_* */
    int up_made;                                     /* gAIMgr +0x50 & 4 */
    float up_delay;                                  /* gAIMgr +0x58 */
    float up_pulse;                                  /* StreetObject's pulse: 0.95 s from the blow to its last banner */
    uint32_t up_taken;                               /* the move it gave (nFlags >> 3) */
} Challenge;

/* no challenge: required -1 (the HUD hides), the MonsterUpgrade defaults */
void challenge_init(Challenge *c);
/* the MonsterUpgrade template (city_template(city, "MonsterUpgrade")); at = 0 keeps the defaults. The keys found. */
int challenge_upgrade_props(Challenge *c, Group monster_upgrade);

/* ---- 1. the block's challenge --------------------------------------------------------------------------------- */

/* 0x3468F0, when the block becomes current (activateNextBlock 0x302AF0; the viewer: build() of the game's block):
 * everything counted so far is dropped and so is the upgrade object (the AI manager's load, 0x334C40). The goals are
 * read only in the campaign (play_type 2, game.h GP_CAMPAIGN), and in the Timed Run (5) on its last block
 * (BlockEnd: last_block != 0); otherwise there are none (required -1), though the getters still read the book.
 * monster: player 1's monster ("George"), flags: its getMonsterFlags (the moves it has: an upgrade it has is not
 * offered). block_def: the block's BlockDef| group (city.h's scene.block). upgrade_def: the BlockDef whose
 * ChallengeDef's Upgrade| groups are read instead of block_def's ({0}: block_def's): with a Wii folder the Wii's
 * copy of the block, whose lists name its own monsters and SuperJump (docs/WII_PLAN.md phase 5). Returns the goals
 * read. */
int challenge_load(Challenge *c, Group block_def, Group upgrade_def, int play_type, int last_block,
                   const char *monster, uint32_t flags);

/* the gWorld getters (World bindings 0x48 .. 0x4F; ChallengeStatus, ChallengeText and Pause read them) */
const char *challenge_identifier(const Challenge *c);   /* getChallengeIdentifier: Name, "" none (0x2B4970) */
const char *challenge_icon(const Challenge *c);         /* getChallengeIcon: Icon, "" none (0x2B47C0) */
float challenge_icon_scale(const Challenge *c, int pause);   /* getChallengeIconScale(pause): PauseScale (pause != 0
                                                              * and the key there), else Scale; 0 without [R], 1 in a book of
                                                              * the Wii's data (Dallas) [R Wii] */
float challenge_icon_shift(const Challenge *c);         /* getChallengeIconShift: ShiftX; 0 without [R] */
int challenge_required(const Challenge *c);             /* getChallengeItemRequired (0x3464F0): the goals' Required
                                                         * summed, -1 with no goal */
int challenge_collected(const Challenge *c);            /* getChallengeItemCollected (0x3465D0): their counts summed */
int challenge_points(const Challenge *c);               /* ChallengeDef.Points, CHALLENGE_POINTS without */
/* Levels::onActivateBlock -> ChallengeStatus::setupUI: what hud_challenge wants. last_block: the boss block
 * (getCurrentBlockNum() == getNumBlocks()) */
void challenge_hud(const Challenge *c, int last_block, HudChallenge *out);

/* 0x3466A0: a template one of the goals names (any case). The world object's init (0x30D280) then starts the
 * template's Effect|ChallengeEffect on it ("NPC_Target" for every actor, DefActorProps) */
int challenge_is_goal(const Challenge *c, const char *name);
/* 0x346780: amount more for each goal naming `name` (any case); how the object went (eaten, destroyed, picked up)
 * does not enter. Returns the goals it counted for (0: the count did not move). */
int challenge_award(Challenge *c, const char *name, int amount);
/* what gCommon.addStats (0x2C51A0) does, once per object: "Score" + its Points, then its template name + 1. Call it
 * with every addStats the viewer sees (tmpl NULL or "": only the Score). Returns challenge_award's for the name. */
int challenge_add_stats(Challenge *c, const char *tmpl, int points);

/* 0x346860: every goal met (collected >= required; for a negative Required, collected < -required)? Sets c->met
 * (1, 0, or -1 with no goals) and returns 1 met or no goals, 0 not. */
int challenge_check(Challenge *c);
int challenge_was_met(const Challenge *c);              /* wasChallengeMet (0x346690): c->met as last checked */

/* ---- 2. the monster upgrade ----------------------------------------------------------------------------------- */

/* the AI manager's frame (0x3333F0, the world update, from the block's second frame) and the object's pulse:
 * CUE_SPAWN: the challenge was just met with an upgrade on offer: the MonsterUpgrade object is at c->up.pos (start
 * c->up.effect there, until challenge_upgrade_hit takes it); CUE_BANNER: 0.95 s after it was taken: the banner
 * hud_banner_text2(challenge_upgrade_key(c->up_taken), challenge_upgrade_inst(c->up_taken), green, 1, 3 s, "") and
 * the object is gone. */
enum { CUE_NONE = 0, CUE_SPAWN, CUE_BANNER };
int challenge_upgrade_tick(Challenge *c, float dt);
/* a monster's blow (its attack sphere centre, radius: StreetObject.bsf TakeDamage from a Monster) on the waiting
 * object (the reach is the two radii, CHALLENGE_NOTES.md [R 0x2E22B0]). Once the delay is over (gAIMgr.aquireUpgrade) the move is player 1's
 * monster's: *flags |= it, c->upgrade becomes 0, and it returns the move: hide the CHALLENGE MET banners
 * (hideChallengeCompletionUI), show hud_banner_text(c->up.text, c->up.colour, 1, "v_upgrade") and stop the effect.
 * 0: missed, or too early (the blow is taken all the same) */
uint32_t challenge_upgrade_hit(Challenge *c, const float centre[3], float radius, uint32_t *flags);
const char *challenge_upgrade_key(uint32_t move);       /* "Upgrade1" .. "Upgrade8" (Game.loc: "CHARGE PUNCH" ...),
                                                         * the Wii's "Upgrade16" */
const char *challenge_upgrade_inst(uint32_t move);      /* "Inst_Upgrade1" .. (how to do it) */
const char *challenge_upgrade_move(uint32_t move);      /* "ChargePunch", "DoubleJump", "RoarAttack", "JumpSmash",
                                                         * "SuperJump" */

/* ---- 3. the block's end --------------------------------------------------------------------------------------- */

#define CITY_FLAG_CHALLENGES 4u                      /* the city's flag 4 (GameStatus +0x2AC): LevelResults'
                                                      * "challenges" star. A new campaign city starts at 0x84 */
/* the block won (0x3255B0, entering state 7), before the next block is activated: in the campaign the clock's
 * seconds left go to "Clock" (truncated); then challenge_check. Returns its answer: 0 = clear CITY_FLAG_CHALLENGES. */
int challenge_block_won(Challenge *c, int play_type, float seconds_left);

#endif
