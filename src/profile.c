/* profile.c - the player profile and its save (profile.h; docs/PROFILE_NOTES.md). [R] marks what is read from the
 * game's code or scripts, [G] what is a guess or a stand-in. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "profile.h"

#define CAMPAIGN 2                                   /* gCommon.getPlayType */

/* [R] ShellData.bdp MonsterData Monster0 .. Monster29 { Name }: the loader 0x33E850 puts Monster<n> at index n */
const char *const PROFILE_MONSTER_NAMES[PROFILE_PS2_MONSTERS] = {
    "George", "Lizzie", "Ralph", "Ramsey", "Gilman", "Rhett", "Cal", "Leon", "Crock", "Kyle",
    "Marco", "Rojo", "Icky", "Venus", "Wally", "Harry", "Snapper", "Amanda", "Kingston", "Natalie",
    "Rock", "Dick", "Furby", "Batty", "Jack", "Jill", "Nick", "Philbert", "Eyegore", "Boosy"
};
/* [R] CityData City0 .. City6 { City } */
const char *const PROFILE_CITY_NAMES[PROFILE_PS2_CITIES] = {
    "LasVegas", "SanFrancisco", "London", "LosAngeles", "Chicago", "HongKong", "NewYork"
};

/* [R Wii] n5StarUnlock / n5StarUnlockNames (0x802F1030, 0x802F1044) */
static const struct { int count; const char *name; } FIVE_STAR[] = {
    { 1, "Squirmy" }, { 3, "Flea" }, { 6, "Sarah" }, { 12, "Brian" }, { 20, "Vern" }
};
#define FIVE_STARS 0xFu                              /* the Wii's 0x1F, until the port has SuperJump [G] */
#define WII_START 8                                  /* [R Wii 0x8008FB58] Profile::reset opens 0 .. 7 */

/* [R] 0x3F6770: what updateProfile unlocks once every city is available and all their saved flags have the mask (the
 * roster's cities: the Wii's 8 with its folder; the Wii's table 0x802F1640 is the same [R], over its 8 [G]) */
static const struct { const char *name; uint32_t mask; } UNLOCKS[] = {
    { "Philbert", PC_PAR }, { "Eyegore", PC_CHALLENGES }, { "Boosy", 0xF }
};

/* [R] 0x32C9C8: the names compare in any case */
static int same(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    for (; *a && *b; a++, b++) {
        int x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a, y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

static int city_ok(const Profile *p, int c) { return c >= 0 && c < p->ncities && c < PROFILE_CITIES; }

int profile_ps2_monster(const char *name)
{
    for (int k = 0; name && k < PROFILE_PS2_MONSTERS; k++)
        if (same(PROFILE_MONSTER_NAMES[k], name))
            return k;
    return -1;
}

static int ps2_city(const char *name)
{
    for (int k = 0; name && k < PROFILE_PS2_CITIES; k++)
        if (same(PROFILE_CITY_NAMES[k], name))
            return k;
    return -1;
}

static int roster_beyond_ps2(const Profile *p)
{
    for (int m = 0; m < p->nmonsters; m++)
        if (profile_ps2_monster(p->monster_name[m]) < 0)
            return 1;
    for (int c = 0; c < p->ncities; c++)
        if (ps2_city(p->city_name[c]) < 0)
            return 1;
    return 0;
}

/* the Wii's roster: its own start monsters (of 0 .. 7: Boris and Ruby; the PS2's six open as they are) in a status
 * record */
static void wii_start(const Profile *p, ProfileStatus *s)
{
    for (int i = 0; i < WII_START && i < p->nmonsters; i++)
        if (profile_ps2_monster(p->monster_name[i]) < 0)
            s->monster_flags[i] |= PM_UNLOCKED;
}

/* ---------------------------------------------------------------- the rosters */

void profile_set_roster(Profile *p, const char *const *monsters, int nmonsters, const char *const *cities,
                        int ncities)
{
    if (monsters) {                                  /* [R] 0x33EDD8: 31 or more are cut to 30 (here 40) */
        p->nmonsters = nmonsters < 0 ? 0 : nmonsters > PROFILE_MONSTERS ? PROFILE_MONSTERS : nmonsters;
        for (int i = 0; i < PROFILE_MONSTERS; i++)
            p->monster_name[i] = i < p->nmonsters ? monsters[i] : NULL;
    }
    if (cities) {                                    /* [R] 0x33EED0: 8 or more are cut to 7 (here 9 to 8) */
        p->ncities = ncities < 0 ? 0 : ncities > PROFILE_CITIES ? PROFILE_CITIES : ncities;
        for (int i = 0; i < PROFILE_CITIES; i++)
            p->city_name[i] = i < p->ncities ? cities[i] : NULL;
    }
}

int profile_find_monster(const Profile *p, const char *name)
{
    for (int i = 0; name && i < p->nmonsters; i++)   /* [R] 0x33DAF0: the first, any case */
        if (same(p->monster_name[i], name))
            return i;
    return -1;
}

int profile_find_city(const Profile *p, const char *name)
{
    for (int i = 0; name && i < p->ncities; i++)
        if (same(p->city_name[i], name))
            return i;
    return -1;
}

/* ---------------------------------------------------------------- the records */

/* [R] 0x33F730(dst, src, keep_head): the merge of a status record into another, raising dst->changed when anything
 * differs. keep_head (updateProfile, the checkpoint at the city's end) leaves the seeds, the night, the city and the
 * start block alone, except that a later city, or a later block of the same city, still moves dst on */
static void status_merge(ProfileStatus *dst, const ProfileStatus *src, int keep_head)
{
    if (!keep_head) {
        dst->changed = dst->changed || dst->seed[0] != src->seed[0] || dst->seed[1] != src->seed[1] ||
                       dst->night != src->night || dst->city != src->city || dst->start_block != src->start_block;
        dst->seed[0] = src->seed[0];
        dst->seed[1] = src->seed[1];
        dst->night = src->night;
        dst->city = src->city;
        dst->start_block = src->start_block;
    }
    if (!dst->changed)
        dst->changed = memcmp(dst->monster, src->monster, sizeof dst->monster) != 0;
    memcpy(dst->monster, src->monster, sizeof dst->monster);
    if (!dst->changed)
        dst->changed = memcmp(dst->lives, src->lives, sizeof dst->lives) != 0;
    memcpy(dst->lives, src->lives, sizeof dst->lives);
    for (int i = 0; i < PROFILE_MONSTERS; i++) {     /* the monsters' flags only grow */
        uint32_t v = dst->monster_flags[i] | src->monster_flags[i];
        if (v != dst->monster_flags[i]) {
            dst->monster_flags[i] = v;
            dst->changed = 1;
        }
    }
    if (dst->city_flags != src->city_flags || dst->city_score != src->city_score) {
        dst->city_flags = src->city_flags;
        dst->city_score = src->city_score;
        dst->changed = 1;
    }
    if (dst->city < src->city) {
        dst->city = src->city;
        dst->start_block = src->start_block;
        dst->changed = 1;
    } else if (dst->city == src->city && dst->start_block < src->start_block) {
        dst->start_block = src->start_block;
        dst->changed = 1;
    }
}

/* [R] 0x33D820 / 0x33D910's tail: three lives for each player, the record changed when one was not 3 */
static void three_lives(ProfileStatus *s)
{
    for (int i = 0; i < PROFILE_PLAYERS; i++)
        if (s->lives[i] != 3) {
            s->lives[i] = 3;
            s->changed = 1;
        }
}

void profile_block_new(ProfileBlock *b)
{
    uint8_t changed = b->changed;                    /* [R] 0x33F630 leaves +0x118 alone */
    memset(b, 0, sizeof *b);
    b->changed = changed;
    b->version = PROFILE_VERSION;
    b->size = PROFILE_SIZE;
    b->status.seed[0] = 0xA0A0A0A0u;
    b->status.seed[1] = 0x1111;
    b->status.city = 0;
    for (int i = 0; i < 6; i++)                      /* George, Lizzie, Ralph, Ramsey, Gilman, Rhett: no upgrade */
        b->status.monster_flags[i] = PM_UNLOCKED;
    for (int i = 0; i < PROFILE_PLAYERS; i++)
        b->status.monster[i] = i;
    b->city_flags[b->status.city] = PC_AVAILABLE;    /* Las Vegas */
    b->victory_shown = 0;
    b->sound = 0.5f;
    b->music = 0.25f;
    b->rumble = 1;
}

void profile_init(Profile *p)
{
    memset(p, 0, sizeof *p);
    profile_set_roster(p, PROFILE_MONSTER_NAMES, PROFILE_PS2_MONSTERS, PROFILE_CITY_NAMES, PROFILE_PS2_CITIES);
    p->play_type = CAMPAIGN;
    /* allow_save 0 [R]: flag 4 is clear in .bss and the boot's card check does not set it */
    p->last_city = 0;
    profile_create(p, NULL);
}

/* ---------------------------------------------------------------- the shell */

int profile_create(Profile *p, const char *name)
{
    profile_block_new(&p->block);                    /* [R] 0x33E480 */
    wii_start(p, &p->block.status);
    if (name) {                                      /* [R] 16 bytes at +0x08 (strncpy 16, 0x344114; the keyboard */
        strncpy(p->block.name, name, 15);            /* takes 15, 0x3408D0); +0x18 .. +0x1F is padding */
        p->block.name[15] = 0;
    }
    p->checkpoint = p->block.status;
    p->current = p->checkpoint;
    return profile_needs_save(p);
}

void profile_restore(Profile *p)
{
    p->checkpoint = p->block.status;                 /* [R] 0x33D910 */
    p->current = p->checkpoint;
    three_lives(&p->current);
}

void profile_restore_checkpoint(Profile *p)
{
    p->current = p->checkpoint;                      /* [R] 0x33D820 */
    three_lives(&p->current);
}

/* ---------------------------------------------------------------- the game */

void profile_new_city(Profile *p)
{
    ProfileStatus *s = &p->current;                  /* [R] 0x33DB90, after the players' points are cleared */
    if (s->start_block != 0)
        return;
    switch (p->play_type) {
    case CAMPAIGN:
        if (s->city_flags != (PC_AVAILABLE | PC_CHALLENGES) || s->city_score != 0) {
            s->city_score = 0;
            s->city_flags = PC_AVAILABLE | PC_CHALLENGES;
            s->changed = 1;
        }
        three_lives(s);
        break;
    case 3: case 4: case 5:                          /* King of the City / World, Timed Run: lives do not count */
        memset(s->lives, 0, sizeof s->lives);
        break;
    default: break;
    }
}

int profile_city_start(Profile *p, int city, int night, int cheat_block)
{
    p->current.city = city;                          /* [R] CitySelect: setCityIdx, setNight, setStartBlock */
    p->current.night = (uint8_t)(night != 0);
    if (cheat_block > 0)
        p->current.start_block = cheat_block;
    else if (p->play_type != CAMPAIGN)
        p->current.start_block = 0;
    else
        p->current.start_block = profile_start_block(p, city);
    p->city_destroyed = 0;
    profile_save_checkpoint(p, 0);                   /* [R] 0x308710 -> 0x33E290(.., 0) */
    profile_new_city(p);                             /* [R] the world's start 0x2B7D40 -> 0x33DB90 */
    return p->current.start_block;
}

void profile_save_checkpoint(Profile *p, int keep_head)
{
    if (p->play_type != CAMPAIGN)                    /* [R] 0x33E290 */
        return;
    status_merge(&p->checkpoint, &p->current, keep_head != 0);
    p->current.changed = 0;
}

int profile_reveal_boosy(Profile *p)
{
    uint32_t all = 0xF;                              /* [R] 0x3259E4 .. 0x325A5C */
    for (int i = 0; i < p->ncities; i++)
        all &= p->block.city_flags[i] | (i == p->current.city ? (uint32_t)PC_REVEAL : 0);
    if (all != 0xF)
        return 0;
    int b = profile_find_monster(p, "Boosy");        /* [R] the name 0x434098 */
    if (b < 0 || (p->current.monster_flags[b] & PM_UNLOCKED))
        return 0;
    p->current.monster_flags[b] |= PM_UNLOCKED;
    return 1;
}

int profile_reveal_five_star(Profile *p)
{
    int n = 0;
    for (int m = 0; m < p->nmonsters; m++)
        if ((p->current.monster_flags[m] & FIVE_STARS) == FIVE_STARS)
            n++;
    for (size_t k = 0; k < sizeof FIVE_STAR / sizeof *FIVE_STAR && FIVE_STAR[k].count <= n; k++)
        if (FIVE_STAR[k].count == n) {
            int m = profile_find_monster(p, FIVE_STAR[k].name);
            if (m < 0 || (p->current.monster_flags[m] & PM_UNLOCKED))
                return -1;
            p->current.monster_flags[m] |= PM_UNLOCKED;
            return m;
        }
    return -1;
}

int profile_block_won(Profile *p, uint32_t city_flags, int city_score, int start_block, int city_done)
{
    p->current.city_flags = city_flags;              /* [R] 0x3255B0: the flags 2, 8, 1 at 0x4D7C3C */
    p->current.city_score = city_score;              /* 0x4D7C40 */
    p->current.start_block = start_block;            /* activateNextBlock's 0x4D7BA0 */
    int boosy = (city_flags & PC_REVEAL) ? profile_reveal_boosy(p) : 0;
    profile_reveal_five_star(p);                     /* [R Wii 0x80081CC4 on]: after Boosy's */
    p->city_destroyed = city_done != 0;              /* 0x4D7E4C */
    profile_save_checkpoint(p, !city_done);          /* [R] 0x325AC4: 0x33E290(.., (done != 0) ^ 1) */
    return boosy;
}

void profile_update(Profile *p)
{
    if (p->play_type != CAMPAIGN)                    /* [R] 0x33DE10 */
        return;
    ProfileBlock *b = &p->block;
    ProfileStatus *cp = &p->checkpoint;
    if (cp->changed || p->city_destroyed) {
        status_merge(&b->status, cp, 1);
        cp->changed = 0;
        if (b->status.changed)
            b->changed = 1;
        int c = cp->city;
        int sb = cp->start_block;                    /* where the city resumes: 0 once it is beaten */
        if (p->city_destroyed || (city_ok(p, c) && (b->city_flags[c] & PC_ALL_BLOCKS)))
            sb = 0;
        if (city_ok(p, c) && b->start_block[c] != sb) {
            b->city_flags[c] |= PC_AVAILABLE;
            b->start_block[c] = sb;
            b->changed = 1;
        }
        if (p->city_destroyed) {
            b->status.changed = 1;
            b->changed = 1;
            if (city_ok(p, c))
                b->city_flags[c] |= cp->city_flags;
            p->last_city = c;
            int n = 0;                               /* the first city not both available and destroyed */
            while (n < p->ncities && (b->city_flags[n] & (PC_AVAILABLE | PC_ALL_BLOCKS)) ==
                                     (PC_AVAILABLE | PC_ALL_BLOCKS))
                n++;
            if (n >= p->ncities) {                   /* all done: the one after the last played */
                n = c + 1;
                if (n >= p->ncities)
                    n = 0;
            }
            cp->city = n;
            if (city_ok(p, n)) {
                b->city_flags[n] |= PC_AVAILABLE;
                b->status.city = n;
                cp->start_block = b->status.start_block = b->start_block[n];
            }
        } else if (city_ok(p, c)) {                  /* mid-city: only the reveal's star is saved now */
            uint32_t f8 = cp->city_flags & PC_REVEAL;
            if ((b->city_flags[c] & PC_REVEAL) != f8) {
                b->city_flags[c] |= f8;
                b->changed = 1;
            }
        }
        if (p->city_destroyed) {                     /* [R] 0x33E034: the cities' flags ANDed */
            uint32_t all = ~0u;
            for (int i = 0; i < p->ncities; i++)
                all &= b->city_flags[i];
            if (all & PC_AVAILABLE)
                for (size_t u = 0; u < sizeof UNLOCKS / sizeof UNLOCKS[0]; u++) {
                    if ((all & UNLOCKS[u].mask) != UNLOCKS[u].mask)
                        continue;
                    int m = profile_find_monster(p, UNLOCKS[u].name);
                    if (m < 0 || (b->status.monster_flags[m] & PM_UNLOCKED))
                        continue;
                    b->status.monster_flags[m] |= PM_UNLOCKED;
                    b->changed = 1;
                    cp->monster_flags[m] |= PM_UNLOCKED;
                }
        }
    }
    p->current = *cp;                                /* [R] 0x33E1B0: the lives kept */
}

int profile_needs_save(const Profile *p)
{
    return p->block.changed || p->block.status.changed;   /* [R] 0x33E2E0 */
}

void profile_saved(Profile *p)
{
    p->block.status.changed = 0;                     /* [R] 0x33F230 with 1: 0x4D7A8C, 0x4D7AC8 */
    p->block.changed = 0;
}

/* ---------------------------------------------------------------- the file */

static void put32(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)v;
    o[1] = (uint8_t)(v >> 8);
    o[2] = (uint8_t)(v >> 16);
    o[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *i)
{
    return (uint32_t)i[0] | (uint32_t)i[1] << 8 | (uint32_t)i[2] << 16 | (uint32_t)i[3] << 24;
}

static void putf(uint8_t *o, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    put32(o, v);
}

static float getf(const uint8_t *i)
{
    uint32_t v = get32(i);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void pack_status(const ProfileStatus *s, uint8_t *o)   /* 0xB8 bytes */
{
    put32(o + 0x00, s->seed[0]);
    put32(o + 0x04, s->seed[1]);
    o[0x08] = s->night;
    put32(o + 0x0C, (uint32_t)s->city);
    put32(o + 0x10, (uint32_t)s->start_block);
    for (int i = 0; i < PROFILE_PLAYERS; i++) {
        put32(o + 0x14 + 4 * i, (uint32_t)s->monster[i]);
        put32(o + 0x24 + 4 * i, (uint32_t)s->lives[i]);
    }
    for (int i = 0; i < PROFILE_PS2_MONSTERS; i++)
        put32(o + 0x34 + 4 * i, s->monster_flags[i]);
    put32(o + 0xAC, s->city_flags);
    put32(o + 0xB0, (uint32_t)s->city_score);
    o[0xB4] = s->changed;
}

static void unpack_status(ProfileStatus *s, const uint8_t *i)
{
    s->seed[0] = get32(i + 0x00);
    s->seed[1] = get32(i + 0x04);
    s->night = i[0x08];
    s->city = (int32_t)get32(i + 0x0C);
    s->start_block = (int32_t)get32(i + 0x10);
    for (int k = 0; k < PROFILE_PLAYERS; k++) {
        s->monster[k] = (int32_t)get32(i + 0x14 + 4 * k);
        s->lives[k] = (int32_t)get32(i + 0x24 + 4 * k);
    }
    for (int k = 0; k < PROFILE_PS2_MONSTERS; k++)
        s->monster_flags[k] = get32(i + 0x34 + 4 * k);
    s->city_flags = get32(i + 0xAC);
    s->city_score = (int32_t)get32(i + 0xB0);
    s->changed = i[0xB4];
}

void profile_pack(const ProfileBlock *b, uint8_t out[PROFILE_SIZE])
{
    memset(out, 0, PROFILE_SIZE);
    put32(out + 0x00, b->version);
    put32(out + 0x04, b->size);
    memcpy(out + 0x08, b->name, sizeof b->name);
    put32(out + 0x20, (uint32_t)b->save_time);
    put32(out + 0x24, (uint32_t)(b->save_time >> 32));
    pack_status(&b->status, out + 0x28);
    for (int i = 0; i < PROFILE_PS2_CITIES; i++) {
        put32(out + 0xE0 + 4 * i, b->city_flags[i]);
        put32(out + 0xFC + 4 * i, (uint32_t)b->start_block[i]);
    }
    out[0x118] = b->changed;
    out[0x119] = b->victory_shown;
    putf(out + 0x11C, b->sound);
    putf(out + 0x120, b->music);
    out[0x124] = b->rumble;
}

int profile_unpack(ProfileBlock *b, const uint8_t in[PROFILE_SIZE])
{
    /* [R 0x3445E0] the load checks the CRC and the file's data size (profile_unpack_file), not the block's version */
    memset(b, 0, sizeof *b);
    b->version = get32(in + 0x00);
    b->size = get32(in + 0x04);
    memcpy(b->name, in + 0x08, sizeof b->name);
    b->name[sizeof b->name - 1] = 0;
    b->save_time = get32(in + 0x20) | (uint64_t)get32(in + 0x24) << 32;
    unpack_status(&b->status, in + 0x28);
    for (int i = 0; i < PROFILE_PS2_CITIES; i++) {
        b->city_flags[i] = get32(in + 0xE0 + 4 * i);
        b->start_block[i] = (int32_t)get32(in + 0xFC + 4 * i);
    }
    b->changed = in[0x118];
    b->victory_shown = in[0x119];
    b->sound = getf(in + 0x11C);
    b->music = getf(in + 0x120);
    b->rumble = in[0x124];
    return 1;
}

uint32_t profile_crc32(uint32_t crc, const uint8_t *data, unsigned long n)
{
    static uint32_t table[256];                      /* [R] 0x40A190: the 0xEDB88320 table */
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    if (!data)                                       /* [R] 0x106780: no buffer answers 0 */
        return 0;
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void profile_card_dir(const char *name, char out[24])
{
    uint32_t crc = profile_crc32(0, (const uint8_t *)name, name ? (unsigned long)strlen(name) : 0);
    snprintf(out, 24, "%s%08X", "BESLES-53800", crc);   /* [R] 0x1A4200's prefix, 0x1A2F90 */
}

void profile_pack_file(const ProfileBlock *b, uint8_t out[PROFILE_FILE_SIZE])
{
    uint8_t *d = out + PROFILE_FILE_HEADER;
    memset(out, 0, PROFILE_FILE_HEADER);             /* the game leaves heap bytes after the titles' NULs [no
                                                      * host meaning] */
    profile_pack(b, d);
    put32(out + 0x00, PROFILE_SIZE);
    put32(out + 0x04, PROFILE_FILE_HEADER);
    put32(out + 0x08, PROFILE_MAGIC);
    put32(out + 0x0C, profile_crc32(0, d, PROFILE_SIZE));
    memcpy(out + 0x10, "Rampage: TD", 12);
    memcpy(out + 0x30, b->name, strlen(b->name) < 31 ? strlen(b->name) : 31);
}

int profile_unpack_file(ProfileBlock *b, const uint8_t *in, long size)
{
    if (size < PROFILE_FILE_HEADER)
        return 0;
    uint32_t dsize = get32(in), hsize = get32(in + 4), crc = get32(in + 0x0C);
    if ((unsigned long)size < (unsigned long)dsize + hsize || get32(in + 8) != PROFILE_MAGIC)   /* [R] 0x1A2D08 */
        return 0;
    if (dsize != PROFILE_SIZE)                       /* [R] 0x34462C: the size the caller wants */
        return 0;
    if (crc && profile_crc32(0, in + hsize, dsize) != crc)   /* [R] 0x1A19B0: a CRC of 0 is not checked */
        return 0;
    return profile_unpack(b, in + hsize);
}

/* a status record into the file's roster (out) and back (in): the flags and the picks by name */
static void status_out(const Profile *p, const ProfileStatus *s, ProfileStatus *o)
{
    *o = *s;
    for (int k = 0; k < PROFILE_PS2_MONSTERS; k++) {
        int m = profile_find_monster(p, PROFILE_MONSTER_NAMES[k]);
        o->monster_flags[k] = m >= 0 ? s->monster_flags[m] & ~(uint32_t)PM_SUPER_JUMP : 0;
    }
    for (int k = PROFILE_PS2_MONSTERS; k < PROFILE_MONSTERS; k++)
        o->monster_flags[k] = 0;
    for (int i = 0; i < PROFILE_PLAYERS; i++)
        if (s->monster[i] >= 0 && s->monster[i] < p->nmonsters) {
            int k = PROFILE_PS2_MONSTERS;
            while (--k >= 0 && !same(PROFILE_MONSTER_NAMES[k], p->monster_name[s->monster[i]]))
                ;
            o->monster[i] = k >= 0 ? k : i;
        }
}

/* the block's cities into the file's (o: already a copy of b, its status mapped) and back */
static void cities_out(const Profile *p, const ProfileBlock *b, ProfileBlock *o)
{
    for (int k = 0; k < PROFILE_CITIES; k++) {
        int c = k < PROFILE_PS2_CITIES ? profile_find_city(p, PROFILE_CITY_NAMES[k]) : -1;
        o->city_flags[k] = c >= 0 ? b->city_flags[c] : 0;
        o->start_block[k] = c >= 0 ? b->start_block[c] : 0;
    }
    int c = b->status.city;
    if (c < 0 || c >= p->ncities)
        return;
    int k = ps2_city(p->city_name[c]);
    if (k < 0) {                                     /* not the PS2's: its first city to go on with */
        for (k = 0; k < PROFILE_PS2_CITIES && (o->city_flags[k] & (PC_AVAILABLE | PC_ALL_BLOCKS)) ==
                                              (PC_AVAILABLE | PC_ALL_BLOCKS); k++)
            ;
        if (k >= PROFILE_PS2_CITIES)
            k = 0;
        o->status.start_block = o->start_block[k];
        o->status.city_flags = o->city_flags[k];
        o->status.city_score = 0;
    }
    o->status.city = k;
}

static void cities_in(const Profile *p, const ProfileBlock *f, ProfileBlock *b)
{
    for (int c = 0; c < PROFILE_CITIES; c++) {
        int k = c < p->ncities ? ps2_city(p->city_name[c]) : -1;
        b->city_flags[c] = k >= 0 ? f->city_flags[k] : 0;
        b->start_block[c] = k >= 0 ? f->start_block[k] : 0;
    }
    int k = f->status.city;
    if (k >= 0 && k < PROFILE_PS2_CITIES) {
        int c = profile_find_city(p, PROFILE_CITY_NAMES[k]);
        b->status.city = c >= 0 ? c : k;
    }
}

static void status_in(const Profile *p, const ProfileStatus *s, ProfileStatus *o)
{
    *o = *s;
    memset(o->monster_flags, 0, sizeof o->monster_flags);
    for (int k = 0; k < PROFILE_PS2_MONSTERS; k++) {
        int m = profile_find_monster(p, PROFILE_MONSTER_NAMES[k]);
        if (m >= 0)
            o->monster_flags[m] = s->monster_flags[k];
    }
    for (int i = 0; i < PROFILE_PLAYERS; i++)
        if (s->monster[i] >= 0 && s->monster[i] < PROFILE_PS2_MONSTERS) {
            int m = profile_find_monster(p, PROFILE_MONSTER_NAMES[s->monster[i]]);
            o->monster[i] = m >= 0 ? m : i;
        }
}

/* ---- the side file (profile.h) */
#define SIDE_MAGIC 0x49575052u                       /* "RPWI" */
#define SIDE_VERSION 1
#define SIDE_MAX 128
enum { SIDE_FLAGS = 1, SIDE_PICK = 2, SIDE_CITY = 3, SIDE_STATUS_CITY = 4 };

void profile_side_path(const char *path, char *out, int size)
{
    size_t n = path ? strlen(path) : 0;
    if (n >= 4 && same(path + n - 4, ".bin"))
        snprintf(out, (size_t)size, "%.*s.wii.bin", (int)(n - 4), path);
    else
        snprintf(out, (size_t)size, "%s.wii", path ? path : "");
}

static int side_write(const Profile *p, const char *path, uint32_t crc)
{
    static uint8_t buf[16 + 32 * SIDE_MAX];
    int n = 0;
    const ProfileStatus *s = &p->block.status;
    for (int m = 0; m < p->nmonsters && n < SIDE_MAX; m++) {
        uint32_t f = profile_ps2_monster(p->monster_name[m]) < 0 ? s->monster_flags[m] :
                     s->monster_flags[m] & PM_SUPER_JUMP;
        if (!f)
            continue;
        uint8_t *r = buf + 16 + 32 * n++;
        memset(r, 0, 32);
        snprintf((char *)r, 16, "%s", p->monster_name[m]);
        put32(r + 16, SIDE_FLAGS);
        put32(r + 20, f);
    }
    for (int i = 0; i < PROFILE_PLAYERS && n < SIDE_MAX; i++) {
        int m = s->monster[i];
        if (m < 0 || m >= p->nmonsters || profile_ps2_monster(p->monster_name[m]) >= 0)
            continue;
        uint8_t *r = buf + 16 + 32 * n++;
        memset(r, 0, 32);
        snprintf((char *)r, 16, "%s", p->monster_name[m]);
        put32(r + 16, SIDE_PICK);
        put32(r + 20, (uint32_t)i);
    }
    for (int c = 0; c < p->ncities && n < SIDE_MAX; c++) {
        if (ps2_city(p->city_name[c]) >= 0)
            continue;
        uint8_t *r = buf + 16 + 32 * n++;
        memset(r, 0, 32);
        snprintf((char *)r, 16, "%s", p->city_name[c]);
        put32(r + 16, SIDE_CITY);
        put32(r + 20, p->block.city_flags[c]);
        put32(r + 24, (uint32_t)p->block.start_block[c]);
        if (c == s->city && n < SIDE_MAX) {
            r = buf + 16 + 32 * n++;
            memset(r, 0, 32);
            snprintf((char *)r, 16, "%s", p->city_name[c]);
            put32(r + 16, SIDE_STATUS_CITY);
            put32(r + 20, (uint32_t)s->start_block);
            put32(r + 24, s->city_flags);
            put32(r + 28, (uint32_t)s->city_score);
        }
    }
    put32(buf, SIDE_MAGIC);
    put32(buf + 4, SIDE_VERSION);
    put32(buf + 8, crc);
    put32(buf + 12, (uint32_t)n);
    char side[1024];
    profile_side_path(path, side, sizeof side);
    errno = 0;
    FILE *f = fopen(side, "wb");
    if (!f)
        return errno == ENOSPC ? -2 : -1;
    size_t len = 16 + 32 * (size_t)n;
    int ok = fwrite(buf, 1, len, f) == len;
    int room = errno != ENOSPC;
    ok = (fclose(f) == 0) && ok;
    return ok ? 1 : room && errno != ENOSPC ? -1 : -2;
}

static void side_read(Profile *p, const char *path, uint32_t crc)
{
    static uint8_t buf[16 + 32 * SIDE_MAX];
    char side[1024];
    profile_side_path(path, side, sizeof side);
    FILE *f = fopen(side, "rb");
    size_t len = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f)
        fclose(f);
    if (len < 16 || get32(buf) != SIDE_MAGIC || get32(buf + 4) != SIDE_VERSION)
        return;
    uint32_t n = get32(buf + 12);
    int fresh = get32(buf + 8) == crc;               /* the PS2's file is the one it was written with */
    ProfileStatus *s = &p->block.status;
    for (uint32_t k = 0; k < n && 16 + 32 * (size_t)(k + 1) <= len; k++) {
        const uint8_t *r = buf + 16 + 32 * k;
        char name[16];
        memcpy(name, r, 15);
        name[15] = 0;
        int m = profile_find_monster(p, name), c = profile_find_city(p, name);
        uint32_t a = get32(r + 20), kind = get32(r + 16);
        if (c >= 0 && ps2_city(name) < 0 && kind == SIDE_CITY) {
            p->block.city_flags[c] |= a;
            p->block.start_block[c] = (int32_t)get32(r + 24);
        } else if (c >= 0 && ps2_city(name) < 0 && kind == SIDE_STATUS_CITY && fresh) {
            s->city = c;
            s->start_block = (int32_t)a;
            s->city_flags = get32(r + 24);
            s->city_score = (int32_t)get32(r + 28);
        }
        if (m < 0)
            continue;
        if (kind == SIDE_FLAGS)
            s->monster_flags[m] |= profile_ps2_monster(name) < 0 ? a & 0xFF : a & PM_SUPER_JUMP;
        else if (kind == SIDE_PICK && fresh && a < PROFILE_PLAYERS)
            s->monster[a] = m;
    }
}

int profile_write(Profile *p, const char *path, int force, uint64_t now_ms)
{
    if (!force && !(profile_needs_save(p) && p->allow_save))
        return 0;
    if (now_ms)
        p->block.save_time = now_ms;                 /* [R] 0x344434 */
    uint8_t buf[PROFILE_FILE_SIZE];
    ProfileBlock fb = p->block;
    status_out(p, &p->block.status, &fb.status);
    cities_out(p, &p->block, &fb);
    profile_pack_file(&fb, buf);
    errno = 0;
    FILE *f = path ? fopen(path, "wb") : NULL;
    if (!f)
        return errno == ENOSPC ? -2 : -1;
    int ok = fwrite(buf, 1, sizeof buf, f) == sizeof buf;
    int room = errno != ENOSPC;
    ok = (fclose(f) == 0) && ok;
    if (!ok)
        return room && errno != ENOSPC ? -1 : -2;    /* the host's full drive as the card's -3, no room [no PS2
                                                      * meaning] */
    int side = roster_beyond_ps2(p) ? side_write(p, path, get32(buf + 0x0C)) : 1;
    if (side < 0)
        return side;
    profile_saved(p);                                /* [R] 0x33F230 with 1 */
    p->loaded = 1;
    return 1;
}

int profile_read(Profile *p, const char *path)
{
    uint8_t buf[PROFILE_FILE_SIZE + 16];
    FILE *f = path ? fopen(path, "rb") : NULL;
    long n = f ? (long)fread(buf, 1, sizeof buf, f) : 0;
    if (f)
        fclose(f);
    if (!profile_unpack_file(&p->block, buf, n)) {   /* [R] 0x33F0F0: a new profile instead */
        p->block.changed = 0;
        profile_block_new(&p->block);
        wii_start(p, &p->block.status);
        profile_restore(p);
        return f ? 0 : -1;
    }
    ProfileBlock fb = p->block;
    status_in(p, &fb.status, &p->block.status);
    cities_in(p, &fb, &p->block);
    if (roster_beyond_ps2(p))
        side_read(p, path, get32(buf + 0x0C));
    wii_start(p, &p->block.status);                  /* a PS2 profile on the Wii's roster: Boris and Ruby too */
    profile_restore(p);                              /* [R] 0x33F120 with 1 */
    p->block.status.changed = 0;
    p->block.changed = 0;
    p->loaded = 1;
    return 1;
}

/* ---------------------------------------------------------------- gCommon's getters and setters */

uint32_t profile_monster_flags(const Profile *p, int player, int monster)
{
    if (monster < 0 || monster > p->nmonsters)       /* [R] 0x33D760: 0 .. 30 */
        return 0;
    if (player > 0 && player < PROFILE_PLAYERS && p->player_flags[player][PROFILE_MONSTERS])
        return p->player_flags[player][monster];
    if (monster == p->nmonsters)                     /* [R] +0x234 + 4 x 30 is the city's flags (+0x2AC) */
        return p->current.city_flags;
    return p->current.monster_flags[monster];
}

void profile_set_monster_flags(Profile *p, int player, int monster, uint32_t flags)
{
    if (monster < 0 || monster > p->nmonsters)       /* [R] 0x33D7C0 */
        return;
    if (player > 0 && player < PROFILE_PLAYERS)
        p->player_flags[player][monster] = (uint8_t)flags;
    else if (monster == p->nmonsters)
        p->current.city_flags = flags & 0xFF;
    else
        p->current.monster_flags[monster] = flags & 0xFF;
}

int profile_monster_available(const Profile *p, int player, int monster)
{
    return (profile_monster_flags(p, player, monster) & PM_UNLOCKED) || (p->cheats & PCH_MONSTERS);
}

uint32_t profile_moves(const Profile *p, int player, int monster)
{
    return (p->cheats & PCH_MOVES) ? 0xFu : profile_monster_flags(p, player, monster) & 0xFu;
}

float profile_sound_volume(const Profile *p) { return p->block.sound; }
float profile_music_volume(const Profile *p) { return p->block.music; }

int profile_set_sound_volume(Profile *p, float v)
{
    if (p->block.sound == v)                         /* [R] 0x2C2F78: c.eq.s */
        return 0;
    p->block.changed = 1;
    p->block.sound = v;
    return 1;
}

int profile_set_music_volume(Profile *p, float v)
{
    if (p->block.music == v)                         /* [R] 0x2C3058 */
        return 0;
    p->block.changed = 1;
    p->block.music = v;
    return 1;
}

int profile_rumble(const Profile *p) { return p->block.rumble; }

int profile_set_rumble(Profile *p, int on, int save)
{
    int v = on != 0;                                 /* [R] 0x2C3440 */
    if (save) {
        if (p->block.rumble != v) {
            p->block.rumble = (uint8_t)v;
            p->block.changed = 1;
        }
        v = p->block.rumble;
    }
    return v && !(p->cheats & PCH_10000);            /* the pad's vt +0x34 */
}

uint32_t profile_cheat_flags(const Profile *p) { return p->cheats; }
void profile_set_cheat_flags(Profile *p, uint32_t f) { p->cheats |= f; }
void profile_clear_cheat_flags(Profile *p, uint32_t f) { p->cheats &= ~f; }

void profile_set_cheat_flags_i(Profile *p, uint32_t f, int param)
{
    if (f == PCH_BLOCK)                              /* [R] 0x2C3350: nothing at all */
        return;
    if (f == PCH_VIDEO)
        p->cheat_param = param;
    p->cheats |= f;
}

int profile_cheat_param(const Profile *p, uint32_t f)
{
    (void)p;
    return f == PCH_BLOCK ? -1 : 0;                  /* [R] 0x2C33B8: 0x4D7E6C is not read here */
}

int profile_cheat_code(Profile *p, const char *code)
{
    static const struct { const char *code; uint32_t flags; } FLAGS[] = {   /* [R] MainMenu.activateCheat */
        { "141421", PCH_MONSTERS | PCH_CITIES }, { "271828", PCH_CITIES }, { "082864", PCH_10000 },
        { "874098", PCH_10000 | PCH_20000 }, { "071767", PCH_40000000 }, { "986960", PCH_4 },
        { "011235", PCH_MOVES }, { "000020", PCH_20000000 }
    };
    if (!code || strlen(code) != 6)
        return 0;
    if (!strcmp(code, "000000")) {
        profile_set_cheat_flags_i(p, PCH_BLOCK, -1);
        profile_clear_cheat_flags(p, ~0u);
        return 1;
    }
    for (size_t i = 0; i < sizeof FLAGS / sizeof FLAGS[0]; i++)
        if (!strcmp(code, FLAGS[i].code)) {
            profile_set_cheat_flags(p, FLAGS[i].flags);
            return 1;
        }
    if (!strncmp(code, "66730", 5)) {                /* a movie: the sixth digit */
        profile_set_cheat_flags_i(p, PCH_VIDEO, code[5] - '0');
        return 1;
    }
    if (!strncmp(code, "24902", 5)) {                /* a start block: stored nowhere (above) */
        profile_set_cheat_flags_i(p, PCH_BLOCK, code[5] - '0');
        return 1;
    }
    return !strcmp(code, "314159");                  /* the version text */
}

int profile_victory_shown(const Profile *p) { return p->block.victory_shown; }

void profile_set_victory_shown(Profile *p)
{
    p->block.victory_shown = 1;                      /* [R] 0x2C3610 */
    p->block.changed = 1;
}

uint32_t profile_city_flags(const Profile *p, int city)
{
    return city >= 0 && city < PROFILE_CITIES ? p->block.city_flags[city] : 0;   /* [R] 0x2C46D0: +0xE0 + 4 i */
}

int profile_start_block(const Profile *p, int city)
{
    return city >= 0 && city < PROFILE_CITIES ? p->block.start_block[city] : 0;  /* [R] 0x2C4710: +0xFC + 4 i */
}

int profile_city_available(const Profile *p, int city)
{
    if (p->play_type != CAMPAIGN || (p->cheats & PCH_CITIES))   /* [R] CitySelect.scr */
        return 1;
    return (profile_city_flags(p, city) & PC_AVAILABLE) != 0;   /* [R] 0x2C4750 */
}
