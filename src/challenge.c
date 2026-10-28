/* challenge.c - the block challenges, the game's side (challenge.h, docs/CHALLENGE_NOTES.md).
 * [R] read in the ELF, [G] guessed. */
#include <string.h>
#include "challenge.h"

/* [R] 0x32C9C8, the game's compare for template and monster names: any case */
static int same(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
}

/* [R] 0x3468F0 compares a typed record's type (kind >> 16 characters) with "Goal" / "Upgrade" (toupper, 0x32EC30)
 * and wants the whole of it: "Goal|" is a Goal, "Goals|" is not */
static int typed(Record r, const char *type)
{
    if (((r.kind >> 8) & 0xFF) == 0)                 /* [R] +0x1 of the record: a plain key is never one */
        return 0;
    size_t len = r.kind >> 16, n = strlen(type);
    if (len != n || !r.name || strlen(r.name) < n)
        return 0;
    for (size_t i = 0; i < n; i++) {
        int x = (unsigned char)r.name[i], y = (unsigned char)type[i];
        if (x >= 'a' && x <= 'z')
            x -= 32;
        if (y >= 'a' && y <= 'z')
            y -= 32;
        if (x != y)
            return 0;
    }
    return 1;
}

/* a list element: [R] the game walks a group's records from its +0xC on, the count of its named keys, which the book
 * keeps first (every ChallengeDef, Goal| and Upgrade| group has them so); here the same records by their kind */
static int listed(Record r)
{
    return ((r.kind >> 8) & 0xFF) >= 2;
}

/* [R] the table 0x3F67A0: {name, flag} x 4, compared any case (0x32C9C8); the Wii's fifth, SuperJump (16), which
 * the PS2's data never names [R the Wii's blocks' Upgrade| groups and Monsters.bsf hasSuperJump; its place last, G] */
#define NMOVES 5
static const struct { const char *name; uint32_t flag; } MOVES[NMOVES] = {
    { "ChargePunch", CH_CHARGE_PUNCH }, { "DoubleJump", CH_DOUBLE_JUMP },
    { "RoarAttack", CH_ROAR_ATTACK }, { "JumpSmash", CH_JUMP_SMASH }, { "SuperJump", CH_SUPER_JUMP },
};

/* ---- the MonsterUpgrade template ------------------------------------------------------------------------------ */

static void props_default(ChallengeUpgradeProps *p)
{
    p->pos[0] = 0;                                   /* [R] Objects1_Props MonsterUpgrade */
    p->pos[1] = -5;
    p->pos[2] = 5;
    p->delay = 1.5f;
    p->radius = 1;
    p->effect = "Powerup_Upgrade";
    p->text = "MonsterUpgrade";
    p->colour[0] = 200 / 255.0f;                     /* [R 0x27B4F0] bytes, x 1/255 (0x1C97F0) */
    p->colour[1] = 0;
    p->colour[2] = 0;
    p->colour[3] = 1;
}

void challenge_init(Challenge *c)
{
    memset(c, 0, sizeof *c);
    props_default(&c->up);
}

/* [R] 0x3333F0 reads PosX / PosY / PosZ (floats, 0 when absent) and UpgradeDelay (1 when absent) of the object it
 * made; StreetObject.bsf the TextID (""), the TextColor (white when absent) and the AttachEffect; the collision is
 * the engine's (CollisionRadius) */
int challenge_upgrade_props(Challenge *c, Group t)
{
    if (!t.at)
        return 0;
    static const char *const POS[3] = { "PosX", "PosY", "PosZ" };
    Record r;
    int n = 0;
    for (int i = 0; i < 3; i++) {
        n += group_get(t, POS[i], &r);
        c->up.pos[i] = group_num(t, POS[i], 0);
    }
    n += group_get(t, "UpgradeDelay", &r);
    c->up.delay = group_num(t, "UpgradeDelay", 1);
    if (group_get(t, "CollisionRadius", &r)) {
        n++;
        c->up.radius = record_num(r);
    }
    const char *s = group_str(t, "Effect|AttachEffect");
    if (!s)
        s = group_str(t, "AttachEffect");
    if (s) {
        n++;
        c->up.effect = s;
    }
    s = group_str(t, "TextID");
    c->up.text = s ? s : "";
    n += s != NULL;
    Group col = group_sub(t, "Color|TextColor");
    Record v[5];
    uint32_t k = col.at ? group_items(col, v, 5) : 0;
    if (k == 3 || k == 4) {                          /* [R 0x278C38 -> 0x27B4F0] three or four numbers, each
                                                      * truncated to a byte, alpha 255 with three, x 1/255
                                                      * (0x1C97F0) */
        for (uint32_t i = 0; i < 4; i++)
            c->up.colour[i] = (float)(i < k ? (int)record_num(v[i]) & 0xFF : 255) / 255.0f;
        n++;
    } else {
        for (int i = 0; i < 4; i++)
            c->up.colour[i] = 1;                     /* [R] the script's default: white */
    }
    return n;
}

/* ---- 1. the block's challenge --------------------------------------------------------------------------------- */

/* [R] 0x3468F0 (a0 = block manager +0x1D0, a1 = the block's ChallengeDef, 0x302CA8):
 *   +0x130 (goals) = 0, +0x13C (upgrade) = 0; +0x138 is left as it was;
 *   play type (0x4D7E44) 5 and the current block (+0x8) the last (+0x10, BlockEnd): read; else a group and play
 *   type 2: read; else nothing more;
 *   +0x134 = Name (a name record, else 0);
 *   the records from the group's +0xC on (its list elements):
 *     "Goal|" (while < 4): Required (an int record, else 0); 0: the goal is left out; otherwise a new goal with that
 *       Required, nothing collected, and every name record of the Goal group's list (up to 16);
 *     "Upgrade|": Monster and Upgrade (name records); only with an Upgrade: the candidates are Monster, then every
 *       name record of the group's list; one whose monster index (0x33DAF0, any case) is player 1's (GameStatus
 *       +0x214) and an Upgrade in the table: the group stops there, and +0x13C = its flag unless the monster's flags
 *       (GameStatus +0x234 + 4 x monster) have it. A later group can therefore overwrite an earlier one. */
int challenge_load(Challenge *c, Group block_def, Group upgrade_def, int play_type, int last_block,
                   const char *monster, uint32_t flags)
{
    c->ngoals = 0;
    c->upgrade = 0;
    c->identifier = NULL;
    c->def = block_def.at ? group_sub(block_def, "ChallengeDef") : block_def;
    c->up_state = CU_NONE;                           /* [R] 0x334C40 (the block's load): gAIMgr +0x50 = 1, +0x58 = 0 */
    c->up_made = 0;
    c->up_delay = 0;
    c->up_pulse = 0;
    c->up_taken = 0;
    Group d = c->def;
    if (!(play_type == 5 && last_block) && !(d.at && play_type == 2))
        return 0;
    if (!d.at)
        return 0;
    c->identifier = group_str(d, "Name");
    for (uint32_t i = 0, n = group_count(d); i < n; i++) {
        Record r = group_record(d, i);
        if (!listed(r))
            continue;
        if (typed(r, "Goal")) {
            if (c->ngoals >= CHALLENGE_GOALS)
                continue;                            /* [R] a fifth is skipped, the loop goes on */
            Group g = record_group(d, r);
            Record q;
            int required = group_get(g, "Required", &q) && (q.kind & 0xFF) == PK_INT ? (int)(int32_t)q.value : 0;
            if (!required)
                continue;
            ChallengeGoal *goal = &c->goal[c->ngoals++];
            memset(goal, 0, sizeof *goal);
            goal->required = required;
            for (uint32_t k = 0, m = group_count(g); k < m; k++) {
                Record e = group_record(g, k);
                if (!listed(e) || (e.kind & 0xFF) != PK_NAME)
                    continue;
                if (goal->nnames >= CHALLENGE_NAMES)
                    break;                           /* [R] 0x346B68: the 17th ends the goal's list */
                goal->name[goal->nnames++] = record_str(g, e);
            }
        }
    }
    Group ud = upgrade_def.at ? group_sub(upgrade_def, "ChallengeDef") : d;   /* (the goals and the upgrade are */
    for (uint32_t i = 0, n = ud.at ? group_count(ud) : 0; i < n; i++) {       /* apart: one walk each) */
        Record r = group_record(ud, i);
        if (!listed(r))
            continue;
        if (typed(r, "Upgrade")) {
            Group g = record_group(ud, r);
            const char *move = group_str(g, "Upgrade");
            if (!move)
                continue;
            uint32_t m = group_count(g);
            int found = 0;
            for (int k = -1; k < (int)m && !found; k++) {   /* -1: the Monster key */
                const char *who;
                if (k < 0)
                    who = group_str(g, "Monster");
                else {
                    Record e = group_record(g, (uint32_t)k);
                    if (!listed(e))
                        continue;
                    who = record_str(g, e);
                }
                if (!who || !same(who, monster))
                    continue;
                for (int t = 0; t < NMOVES && !found; t++)
                    if (same(move, MOVES[t].name)) {
                        found = 1;
                        if (!(flags & MOVES[t].flag))
                            c->upgrade = MOVES[t].flag;
                    }
            }
        }
    }
    return c->ngoals;
}

/* [R] 0x2B4970 / 0x2B47C0 / the cases 0x2AF488 and 0x2AF5C8: the current block's ChallengeDef (0x303160 on the block
 * manager, then "ChallengeDef" a group record), its key read every call; a missing key leaves the answer as made:
 * the floats 0.0 (PFloat's constructor 0x1CE830) [R]; the strings get no string and are cleared to "" (0x2B4A48 ..
 * 0x2B4A70) [R] */
const char *challenge_identifier(const Challenge *c)
{
    const char *s = c->def.at ? group_str(c->def, "Name") : NULL;
    return s ? s : "";
}

const char *challenge_icon(const Challenge *c)
{
    const char *s = c->def.at ? group_str(c->def, "Icon") : NULL;
    return s ? s : "";
}

static int float_key(Group g, const char *key, float *out)   /* [R] the cases want a float record (type 1) */
{
    Record r;
    if (!g.at || !group_get(g, key, &r) || (r.kind & 0xFF) != PK_FLOAT)
        return 0;
    *out = record_num(r);
    return 1;
}

/* the Wii's starts its answer at 1.0 (0x80008304: sda2 0x804DDFA0) where the PS2's PFloat is 0.0; seven of Dallas's
 * nine ChallengeDefs have no Scale (every PS2 one with an Icon has it) [R Wii] */
float challenge_icon_scale(const Challenge *c, int pause)
{
    float v = c->def.at && c->def.book && c->def.book->b && c->def.book->b->wii ? 1.0f : 0.0f;
    if (pause && float_key(c->def, "PauseScale", &v))
        return v;
    float_key(c->def, "Scale", &v);
    return v;
}

float challenge_icon_shift(const Challenge *c)
{
    float v = 0;
    float_key(c->def, "ShiftX", &v);
    return v;
}

/* [R] 0x3464F0: the sum of +0x44 over the goals (unrolled by 8), -1 when there are none */
int challenge_required(const Challenge *c)
{
    if (c->ngoals <= 0)
        return -1;
    int n = 0;
    for (int i = 0; i < c->ngoals; i++)
        n += c->goal[i].required;
    return n;
}

/* [R] 0x3465D0: the sum of +0x48 (0 with no goals) */
int challenge_collected(const Challenge *c)
{
    int n = 0;
    for (int i = 0; i < c->ngoals; i++)
        n += c->goal[i].collected;
    return n;
}

int challenge_points(const Challenge *c)
{
    return c->def.at ? (int)group_num(c->def, "Points", CHALLENGE_POINTS) : CHALLENGE_POINTS;
}

/* [R] ChallengeStatus::setupUI and ChallengeText's doInterface: the getters as they read them */
void challenge_hud(const Challenge *c, int last_block, HudChallenge *out)
{
    memset(out, 0, sizeof *out);
    out->last_block = last_block != 0;
    const char *id = challenge_identifier(c);
    out->identifier = *id ? id : NULL;
    out->icon = challenge_icon(c);
    out->icon_scale = challenge_icon_scale(c, 0);    /* ChallengeText: getChallengeIconScale(0) */
    out->icon_shift = challenge_icon_shift(c);
    out->upgrade = (int)c->upgrade;
    out->required = challenge_required(c);
}

/* [R] 0x3466A0: for each goal, each name: 0x32C9C8 equal -> 1 */
int challenge_is_goal(const Challenge *c, const char *name)
{
    if (!name)
        return 0;
    for (int i = 0; i < c->ngoals; i++)
        for (int k = 0; k < c->goal[i].nnames; k++)
            if (same(name, c->goal[i].name[k]))
                return 1;
    return 0;
}

/* [R] 0x346780 (name, amount): name NULL does nothing; for each goal the first name that matches adds amount to its
 * +0x48 and the next goal is tried */
int challenge_award(Challenge *c, const char *name, int amount)
{
    if (!name)
        return 0;
    int n = 0;
    for (int i = 0; i < c->ngoals; i++)
        for (int k = 0; k < c->goal[i].nnames; k++)
            if (same(name, c->goal[i].name[k])) {
                c->goal[i].collected += amount;
                n++;
                break;
            }
    return n;
}

/* [R] 0x2C51A0 after Player.addPoints (0x2C55B4): 0x346780("Score", points), then, when the object's template
 * (obj +0x80, its name at +0x4) has a name that is not empty, 0x346780(name, 1). The whole of addStats is skipped for
 * an object already counted (flag 0x40000000): call this once per object. The building's Scoring (0x2BA4B0) awards
 * "Score" too (challenge_award(c, "Score", pts)). */
int challenge_add_stats(Challenge *c, const char *tmpl, int points)
{
    challenge_award(c, "Score", points);
    return tmpl && *tmpl ? challenge_award(c, tmpl, 1) : 0;
}

/* [R] 0x346860: +0x138 = 0; any goal with Required > 0 and collected < Required, or Required <= 0 and collected >=
 * -Required, returns 0 (+0x138 stays 0); otherwise +0x138 = 1 (-1 with no goals) and it returns 1. A Required of 0
 * would never be met, but the load leaves such a goal out. */
int challenge_check(Challenge *c)
{
    c->met = 0;
    for (int i = 0; i < c->ngoals; i++) {
        const ChallengeGoal *g = &c->goal[i];
        if (g->required > 0 ? g->collected < g->required : g->collected >= -g->required)
            return 0;
    }
    c->met = c->ngoals ? 1 : -1;
    return 1;
}

int challenge_was_met(const Challenge *c)
{
    return c->met;
}

/* ---- 2. the monster upgrade ----------------------------------------------------------------------------------- */

/* [R] 0x3333F0 (gAIMgr 0x4D7800, every world update; this part from the block's second frame, +0x50 & 2):
 *   +0x50 & 4 clear, +0x13C (the upgrade) != 0 and 0x346860 met: +0x50 |= 4; the template "MonsterUpgrade" made
 *   (0x285710 on 0x4D17A0, 0x283DA0), added to the world, its matrix's place set to (PosX, PosY, PosZ) and its
 *   onInit run; +0x58 = UpgradeDelay (1 without);
 *   then +0x58 > 0: -= dt, kept at 0 or more.
 * StreetObject.bsf: taken, it shows its last banner at its next pulse (setPulseDelay(0.95)) and deletes itself. */
int challenge_upgrade_tick(Challenge *c, float dt)
{
    int ev = CUE_NONE;
    if (!c->up_made && c->upgrade && challenge_check(c)) {
        c->up_made = 1;
        c->up_state = CU_WAITING;
        c->up_delay = c->up.delay;
        ev = CUE_SPAWN;
    }
    if (c->up_delay > 0) {
        c->up_delay -= dt;
        if (c->up_delay <= 0)
            c->up_delay = 0;
    }
    if (c->up_state == CU_TAKEN) {
        c->up_pulse -= dt;
        if (c->up_pulse <= 0) {
            c->up_state = CU_NONE;                   /* requestDelete */
            ev = CUE_BANNER;
        }
    }
    return ev;
}

/* [R] StreetObject.bsf TakeDamage (nFlags & 2: IsMonsterUpgrade) from a Monster: not yet taken (nFlags & 4):
 * nug = getChallengeUpgrade(); gAIMgr.aquireUpgrade() (0x2A62B8: +0x58 <= 0: GameStatus +0x234 + 4 x (player 1's
 * monster) |= +0x13C, +0x13C = 0, 1; else 0); on 1: nFlags |= nug << 3 | 4, hideChallengeCompletionUI, the TextID
 * banner (scale 1, "v_upgrade"), setPulseDelay(0.95), the object moved to (0, -50, 0), its effect stopped. It answers
 * 1 (the blow is taken) either way. */
uint32_t challenge_upgrade_hit(Challenge *c, const float centre[3], float radius, uint32_t *flags)
{
    if (c->up_state != CU_WAITING)
        return 0;
    float d2 = 0;
    for (int i = 0; i < 3; i++) {
        float e = centre[i] - c->up.pos[i];
        d2 += e * e;
    }
    float reach = radius + c->up.radius;             /* [R 0x2E22B0] the monster's object test: each collision
                                                      * sphere against the shape, CollisionRadius about its middle */
    if (d2 > reach * reach)
        return 0;
    if (c->up_delay > 0)
        return 0;
    uint32_t move = c->upgrade;
    if (flags)
        *flags |= move;
    c->upgrade = 0;
    c->up_taken = move;
    c->up_state = CU_TAKEN;
    c->up_pulse = 0.95f;
    return move;                                     /* never 0: the object is made only with an upgrade on offer */
}

static int move_index(uint32_t move)
{
    for (int t = 0; t < NMOVES; t++)
        if (move == MOVES[t].flag)
            return t;
    return -1;
}

/* [R] StreetObject.bsf onPulse: "Upgrade" + nug and "Inst_Upgrade" + nug; ChallengeText "Upgrade" + cupgrade */
const char *challenge_upgrade_key(uint32_t move)
{
    static const char *const K[NMOVES] = { "Upgrade1", "Upgrade2", "Upgrade4", "Upgrade8", "Upgrade16" };
    int t = move_index(move);
    return t < 0 ? "Upgrade0" : K[t];
}

const char *challenge_upgrade_inst(uint32_t move)
{
    static const char *const K[NMOVES] = { "Inst_Upgrade1", "Inst_Upgrade2", "Inst_Upgrade4", "Inst_Upgrade8",
                                           "Inst_Upgrade16" };
    int t = move_index(move);
    return t < 0 ? "Inst_Upgrade0" : K[t];
}

const char *challenge_upgrade_move(uint32_t move)
{
    int t = move_index(move);
    return t < 0 ? "" : MOVES[t].name;
}

/* ---- 3. the block's end --------------------------------------------------------------------------------------- */

/* [R] 0x3255B0: the clock stopped (0x347E10); play type 2: the time bonus shared out, then 0x346780("Clock",
 * (int)time left (0x347DC0, cvt.w.s)); ... the Kings; 0x346860 and, when it answers 0, GameStatus +0x2AC &= ~4 */
int challenge_block_won(Challenge *c, int play_type, float seconds_left)
{
    if (play_type == 2)
        challenge_award(c, "Clock", (int)seconds_left);
    return challenge_check(c);
}
