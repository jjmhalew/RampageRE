/* autoplay.c - the automated playthrough's watchdog (autoplay.h): a line per block, the problems seen, the run's
 * summary, and a crash's last word. Not the game's: nothing here is [R] or [G], it tests the port. */
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "autoplay.h"
#include "game.h"
#ifdef _WIN32
#define PSAPI_VERSION 2                              /* K32GetProcessMemoryInfo, in kernel32 */
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>                                 /* the crash's stack, by the program's .pdb */
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

enum { PK_NAN, PK_STUCK, PK_OUTSIDE, PK_UNDER, PK_INSIDE, PK_DOWN, PK_KINDS };   /* a monster's, once a block each */
enum { OB_PEOPLE, OB_VEHICLES, OB_ARMY, OB_PARTICLES, OB_STREET, OB_PARADE, OB_RAIL, OB_EDIBLES, OB_KINDS };
static const char *const OB_NAMES[OB_KINDS] = { "people", "vehicles", "army", "particles", "street", "parade", "rail",
                                                "edibles" };
static const int OB_LIMIT[OB_KINDS] = { 300, 150, 300, 30000, 800, 120, 120, 300 };   /* a runaway past these */

struct Autoplay {
    FILE *log;
    char shots[512], shot[640];
    ApLimits lim;
    double t;                                        /* the last frame's */
    /* the game's state and the shell's screen, and since when */
    int state, in_game;
    double state_since;
    char focus[24];
    double focus_since;
    int stuck_told, shell_told;
    const char *why;                                 /* why the run stops */
    /* the block played */
    int playing;                                     /* in GS_PLAY */
    double play_since;
    char city[32], block[48];
    int block_index, play_type, forced, told_block, guard0;
    char lost_key[96]; int lost_n, force_next;       /* the block lost again and again: how often; forced next time */
    int blocks, won, lost, problems, games, demos, shots_n;
    int pk[AP_MONSTERS];                             /* the PK_ bits told this block */
    float anchor[AP_MONSTERS][3]; double anchor_t[AP_MONSTERS];
    double bad_since[AP_MONSTERS][PK_KINDS];
    int ob_max[OB_KINDS], ob_told;
    ApFrame last;                                    /* the last frame of the block's play */
    const char *end;                                 /* the block played ended so (its line the next frame: the game
                                                      * enters a state the frame after it is set) */
    double mem_first, mem_last, mem_peak, mem_at;
    int mem_told;
    int shot_reason;                                 /* 0 none, 1 a block's end (a sample), 2 a problem */
    char summary[512];                               /* the last frame, for a crash */
};

static Autoplay *g_ap;                               /* for the crash handlers */

double ap_peak_mb(void)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        return (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
    return -1;
#else
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0)
#ifdef __APPLE__
        return (double)ru.ru_maxrss / (1024.0 * 1024.0);
#else
        return (double)ru.ru_maxrss / 1024.0;
#endif
    return -1;
#endif
}

static double mem_now(void)                          /* the working set now (MB), -1 unknown */
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
    return -1;
#else
    long pages = 0, rss = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        if (fscanf(f, "%ld %ld", &pages, &rss) != 2)
            rss = 0;
        fclose(f);
    }
    return rss > 0 ? (double)rss * (double)sysconf(_SC_PAGESIZE) / (1024.0 * 1024.0) : ap_peak_mb();
#endif
}

static void line(Autoplay *a, const char *kind, const char *fmt, va_list ap)
{
    fprintf(a->log, "%s t=%.1f ", kind, a->t);
    vfprintf(a->log, fmt, ap);
    fputc('\n', a->log);
    fflush(a->log);
}

static void logf_(Autoplay *a, const char *kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    line(a, kind, fmt, ap);
    va_end(ap);
}

void ap_event(Autoplay *a, const char *fmt, ...)
{
    if (!a)
        return;
    va_list ap;
    va_start(ap, fmt);
    line(a, "EVENT", fmt, ap);
    va_end(ap);
}

static const char *type_name(int pt)
{
    return pt == GP_ATTRACT ? "demo" : pt == GP_CAMPAIGN ? "campaign" : pt == GP_KING_CITY ? "kingcity" :
           pt == GP_KING_WORLD ? "kingworld" : pt == GP_TIMED_RUN ? "timed" : "?";
}

static void want_shot(Autoplay *a, int reason, const char *tag)
{
    if (!a->shots[0] || a->shot_reason >= reason)
        return;
    if (reason == 2 && a->shots_n >= 80)              /* enough of them */
        return;
    char c[33], b[49];
    snprintf(c, sizeof c, "%s", a->in_game ? a->city : "shell");
    snprintf(b, sizeof b, "%s", a->in_game ? a->block : a->focus);
    for (char *p = b; *p; p++)
        if (*p == '|' || *p == ' ' || *p == ':' || *p == '/' || *p == '\\')
            *p = '_';
    snprintf(a->shot, sizeof a->shot, "%s/%03d_%s_%s_%s.png", a->shots, a->shots_n + 1, c, b, tag);
    a->shot_reason = reason;
}

/* a PROBLEM line (and its screenshot) */
static void problem(Autoplay *a, const char *kind, const char *fmt, ...)
{
    a->problems++;
    want_shot(a, 2, kind);
    fprintf(a->log, "PROBLEM t=%.1f kind=%s city=%s block=%s state=%d focus=%s ", a->t, kind,
            a->in_game ? a->city : "-", a->in_game ? a->block : "-", a->state, a->in_game ? "-" : a->focus);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(a->log, fmt, ap);
    va_end(ap);
    if (a->shot_reason == 2)
        fprintf(a->log, " shot=%s", a->shot);
    fputc('\n', a->log);
    fflush(a->log);
}

/* ---- a crash's last word */
static void crash_write(const char *what)
{
    if (!g_ap || !g_ap->log)
        return;
    fprintf(g_ap->log, "CRASH t=%.1f what=%s last=\"%s\"\n", g_ap->t, what, g_ap->summary);
    fflush(g_ap->log);
}
#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e)
{
    char what[128];
    DWORD64 base = (DWORD64)GetModuleHandleA(NULL);
    snprintf(what, sizeof what, "exception_0x%08lX_at_%p_(exe+0x%llx)",
             e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionCode : 0,
             e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionAddress : NULL,
             e && e->ExceptionRecord ? (unsigned long long)((DWORD64)e->ExceptionRecord->ExceptionAddress - base) : 0ull);
    crash_write(what);
    if (g_ap && g_ap->log && e && e->ContextRecord) {   /* the stack, each frame by its symbol and line */
        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        char dir[MAX_PATH] = "";                     /* the .pdb beside the exe, whatever the current folder */
        DWORD dn = GetModuleFileNameA(NULL, dir, sizeof dir);
        while (dn > 0 && dir[dn - 1] != '\\' && dir[dn - 1] != '/')
            dir[--dn] = 0;
        SymInitialize(proc, dir[0] ? dir : NULL, TRUE);
        CONTEXT c = *e->ContextRecord;
        STACKFRAME64 f;
        memset(&f, 0, sizeof f);
        f.AddrPC.Offset = c.Rip;
        f.AddrFrame.Offset = c.Rbp;
        f.AddrStack.Offset = c.Rsp;
        f.AddrPC.Mode = f.AddrFrame.Mode = f.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 24 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &f, &c, NULL,
                                              SymFunctionTableAccess64, SymGetModuleBase64, NULL) && f.AddrPC.Offset; i++) {
            char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
            memset(buf, 0, sizeof buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 off = 0;
            DWORD loff = 0;
            IMAGEHLP_LINE64 ln;
            memset(&ln, 0, sizeof ln);
            ln.SizeOfStruct = sizeof ln;
            int named = SymFromAddr(proc, f.AddrPC.Offset, &off, sym);
            int lined = SymGetLineFromAddr64(proc, f.AddrPC.Offset, &loff, &ln);
            fprintf(g_ap->log, "CRASH_FRAME %d %s+0x%llx %s:%lu\n", i, named ? sym->Name : "?", (unsigned long long)off,
                    lined ? ln.FileName : "?", lined ? (unsigned long)ln.LineNumber : 0ul);
        }
        fflush(g_ap->log);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
static void crash_signal(int sig)
{
    char what[32];
    snprintf(what, sizeof what, "signal_%d", sig);
    crash_write(what);
    signal(sig, SIG_DFL);
    raise(sig);
}

Autoplay *ap_open(const char *log, const char *shots, const ApLimits *lim, const char *what)
{
    Autoplay *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->log = fopen(log, "w");
    if (!a->log) {
        free(a);
        return NULL;
    }
    if (shots)
        snprintf(a->shots, sizeof a->shots, "%s", shots);
    if (lim)
        a->lim = *lim;
    if (a->lim.minutes <= 0) a->lim.minutes = 600;
    if (a->lim.stuck <= 0) a->lim.stuck = 90;
    if (a->lim.block <= 0) a->lim.block = 1200;
    if (a->lim.screen <= 0) a->lim.screen = 240;
    if (a->lim.shell <= 0) a->lim.shell = 300;
    if (a->lim.keep_every <= 0) a->lim.keep_every = 5;
    a->state = -1;
    a->mem_first = -1;
    fprintf(a->log, "RUN %s\n", what ? what : "");
    fflush(a->log);
    g_ap = a;
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
#else
    signal(SIGSEGV, crash_signal);
    signal(SIGBUS, crash_signal);
    signal(SIGFPE, crash_signal);
#endif
    signal(SIGABRT, crash_signal);
    return a;
}

const char *ap_shot_path(const Autoplay *a) { return a && a->shot_reason ? a->shot : NULL; }

void ap_shot_done(Autoplay *a, int ok)
{
    if (!a)
        return;
    if (ok)
        a->shots_n++;
    a->shot_reason = 0;
}

void ap_game_left(Autoplay *a, int demo)
{
    if (!a)
        return;
    a->games++;
    a->demos += demo != 0;
    logf_(a, "EVENT", "what=game_left demo=%d games=%d", demo, a->games);
}

/* the block played is over: its line */
static void block_end(Autoplay *a, const ApFrame *f, const char *result)
{
    const ApFrame *p = &a->last;                     /* the monsters as they were in play */
    a->end = NULL;
    a->blocks++;
    int won = !strcmp(result, "won");
    a->won += won;
    a->lost += !won;
    char key[96];
    snprintf(key, sizeof key, "%s %s", a->city, a->block);
    if (won || strcmp(key, a->lost_key)) {
        a->lost_n = 0;
        snprintf(a->lost_key, sizeof a->lost_key, "%s", won ? "" : key);
    }
    a->lost_n += !won;
    char score[64] = "", lives[64] = "", hp[64] = "", st[160] = "";
    for (int k = 0; k < f->nmon && k < AP_MONSTERS; k++) {
        snprintf(score + strlen(score), sizeof score - strlen(score), "%s%d", k ? "," : "", f->points[k]);
        snprintf(lives + strlen(lives), sizeof lives - strlen(lives), "%s%d", k ? "," : "", f->mon[k].lives);
        snprintf(hp + strlen(hp), sizeof hp - strlen(hp), "%s%.0f", k ? "," : "", p->mon[k].hp);
        snprintf(st + strlen(st), sizeof st - strlen(st), "%s%s", k ? "," : "", p->mon[k].state ? p->mon[k].state : "-");
    }
    double mem = mem_now();
    if (a->mem_first < 0)
        a->mem_first = mem;
    a->mem_last = mem;
    logf_(a, "BLOCK", "n=%d city=%s block=%s index=%d type=%s dur=%.1f result=%s%s score=%s city_score=%d lives=%s hp=%s "
          "states=%s boss=%s boss_hp=%.2f clock=%.0f standing=%d/%d people=%d vehicles=%d army=%d particles=%d "
          "street=%d guard_falls=%d mem_mb=%.0f", a->blocks, a->city, a->block, a->block_index, type_name(a->play_type),
          f->t - a->play_since, result, a->forced ? "(forced)" : "", score, f->city_score, lives, hp, st,
          p->boss == 1 ? "alive" : p->boss == 2 ? "beaten" : "none", p->boss_hp, p->clock, p->standing, p->nbuildings,
          a->ob_max[OB_PEOPLE], a->ob_max[OB_VEHICLES], a->ob_max[OB_ARMY], a->ob_max[OB_PARTICLES],
          a->ob_max[OB_STREET], f->guard_falls - a->guard0, mem);
    if (a->blocks == 1 || a->blocks % a->lim.keep_every == 0)
        want_shot(a, 1, result);
    a->playing = 0;
    if (a->lost_n == 3) {                            /* lost three times in a row: the next try is felled, 30 s in */
        problem(a, "block_lost_repeatedly", "times=3 last=%s: the next try is forced", result);
        a->force_next = 1;
    }
}

static void monsters_check(Autoplay *a, const ApFrame *f)
{
    for (int k = 0; k < f->nmon && k < AP_MONSTERS; k++) {
        const ApMonster *m = &f->mon[k];
        const float *p = m->pos;
        int bad[PK_KINDS] = { 0 };
        bad[PK_NAN] = !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) || !isfinite(m->hp);
        bad[PK_DOWN] = m->hp <= 0;                   /* down and the block goes on: it should come back */
        if (!bad[PK_NAN] && !m->out && m->hp > 0) {
            bad[PK_OUTSIDE] = p[0] < -80 || p[0] > 80 || p[1] < -32 || p[1] > 80 || p[2] > 150;
            bad[PK_UNDER] = p[2] < m->ground - 4;   /* (a landing puts it 3 under, then eases it up) */
            for (int i = 0; !m->mounted && i < f->nbuildings && i < AP_BUILDINGS; i++) {
                const ApBuilding *b = &f->b[i];
                if (b->standing && p[0] > b->min[0] + 1 && p[0] < b->max[0] - 1 && p[1] > b->min[1] + 1 &&
                    p[1] < b->max[1] - 1 && p[2] < b->max[2] - 1.5f)
                    bad[PK_INSIDE] = 1;
            }
            float dx = p[0] - a->anchor[k][0], dy = p[1] - a->anchor[k][1], dz = p[2] - a->anchor[k][2];
            if (dx * dx + dy * dy + dz * dz > 0.25f || a->anchor_t[k] <= 0) {
                memcpy(a->anchor[k], p, sizeof a->anchor[k]);
                a->anchor_t[k] = f->t;
            }
            bad[PK_STUCK] = f->t - a->anchor_t[k] > a->lim.stuck;
        } else
            a->anchor_t[k] = f->t;
        static const char *const NAMES[PK_KINDS] = { "nan_position", "monster_stuck", "outside_street", "under_ground",
                                                     "inside_building", "never_respawns" };
        static const double HOLD[PK_KINDS] = { 0, 0, 3, 2, 2, 90 };   /* seconds it must last */
        for (int q = 0; q < PK_KINDS; q++) {
            if (!bad[q]) {
                a->bad_since[k][q] = -1;
                continue;
            }
            if (a->bad_since[k][q] < 0)
                a->bad_since[k][q] = f->t;
            if (!(a->pk[k] & 1 << q) && f->t - a->bad_since[k][q] >= HOLD[q]) {
                a->pk[k] |= 1 << q;
                problem(a, NAMES[q], "monster=%d at=%.1f,%.1f,%.1f ground=%.1f hp=%.0f state=%s mounted=%d", k + 1, p[0],
                        p[1], p[2], m->ground, m->hp, m->state ? m->state : "-", m->mounted);
            }
        }
    }
}

int ap_frame(Autoplay *a, const ApFrame *f)
{
    if (!a)
        return 0;
    int r = 0;
    a->t = f->t;
    if (f->t > 5 && getenv("RAMPAGE_AP_CRASH")) {    /* the CRASH lines' test: a write through NULL 5 s in */
        volatile int *null = NULL;
        *null = 1;
    }
    a->in_game = f->in_game;
    if (a->end)                                      /* the block's line, its points and score in */
        block_end(a, f->in_game ? f : &a->last, a->end);
    if (f->in_game) {
        a->focus[0] = 0;                             /* (the shell's screen is new when it comes back) */
        if (f->state != a->state) {                  /* the state moved on */
            if (a->playing && f->state != GS_PLAY) {
                a->end = f->state == GS_BLOCK_WON ? (a->last.clock <= 0.5f && a->last.standing > 0 ? "timeup" : "won") :
                         f->state == GS_GAME_OVER ? "gameover" : "left";
                a->playing = 0;
            }
            if (f->state == GS_PLAY) {
                a->playing = 1;
                a->play_since = f->t;
                snprintf(a->city, sizeof a->city, "%s", f->city ? f->city : "?");
                snprintf(a->block, sizeof a->block, "%s", f->block ? f->block : "?");
                a->block_index = f->block_index;
                a->guard0 = f->guard_falls;
                a->play_type = f->play_type;
                a->forced = a->told_block = 0;
                memset(a->pk, 0, sizeof a->pk);
                memset(a->ob_max, 0, sizeof a->ob_max);
                for (int k = 0; k < AP_MONSTERS; k++) {
                    a->anchor_t[k] = 0;
                    for (int q = 0; q < PK_KINDS; q++)
                        a->bad_since[k][q] = -1;
                }
            }
            if (f->state == GS_END_LEVEL || f->state == GS_END_WORLD)
                logf_(a, "EVENT", "what=%s city=%s score=%d", f->state == GS_END_LEVEL ? "city_done" : "world_done",
                      f->city ? f->city : "?", f->city_score);
            a->state = f->state;
            a->state_since = f->t;
            a->stuck_told = 0;
        }
        if (a->playing) {
            snprintf(a->city, sizeof a->city, "%s", f->city ? f->city : "?");
            int ob[OB_KINDS] = { f->people, f->vehicles, f->army, f->particles, f->street, f->parade, f->rail, f->edibles };
            for (int i = 0; i < OB_KINDS; i++) {
                if (ob[i] > a->ob_max[i])
                    a->ob_max[i] = ob[i];
                if (ob[i] > OB_LIMIT[i] && !(a->ob_told & 1 << i)) {
                    a->ob_told |= 1 << i;
                    problem(a, "objects_runaway", "objects=%s count=%d limit=%d", OB_NAMES[i], ob[i], OB_LIMIT[i]);
                }
            }
            monsters_check(a, f);
            if (a->force_next && f->t - a->play_since > 30) {
                a->force_next = 0;
                a->forced = 1;
                r |= AP_FORCE_BLOCK;
            }
            if (f->t - a->play_since > a->lim.block && !a->told_block) {
                a->told_block = 1;
                problem(a, "block_never_ends", "dur=%.0f standing=%d/%d boss=%d boss_hp=%.2f clock=%.0f", f->t - a->play_since,
                        f->standing, f->nbuildings, f->boss, f->boss_hp, f->clock);
                a->forced = 1;
                r |= AP_FORCE_BLOCK;
            }
            if (a->told_block && f->t - a->play_since > a->lim.block + 300) {
                problem(a, "block_never_ends", "forcing did not end it");
                a->why = "block_hang";
                r |= AP_STOP;
            }
        } else if (f->t - a->state_since > a->lim.screen) {   /* a screen or a script that never ends */
            if (a->stuck_told == 0) {
                problem(a, f->results ? "results_never_answer" : "state_never_ends", "game_state=%d for=%.0f",
                        f->state, f->t - a->state_since);
                r |= AP_FORCE_RESULT;
                a->stuck_told = 1;
            } else if (f->t - a->state_since > 2 * a->lim.screen) {
                problem(a, "state_never_ends", "game_state=%d for=%.0f: stopped", f->state, f->t - a->state_since);
                a->why = "state_hang";
                r |= AP_STOP;
            }
        }
    } else {
        if (a->playing) {                            /* left during the block (a quit) */
            block_end(a, &a->last, "left");
            a->playing = 0;
        }
        a->state = -1;
        const char *fo = f->focus ? f->focus : "";
        if (strcmp(fo, a->focus)) {
            snprintf(a->focus, sizeof a->focus, "%s", fo);
            a->focus_since = f->t;
            a->shell_told = 0;
            logf_(a, "EVENT", "what=shell focus=%s state=%d", fo, f->shell_state);
        } else if (f->t - a->focus_since > (f->movie ? 3 * a->lim.shell : a->lim.shell)) {
            if (!a->shell_told) {
                problem(a, "shell_never_advances", "for=%.0f shell_state=%d movie=%d", f->t - a->focus_since,
                        f->shell_state, f->movie);
                a->shell_told = 1;
                r |= AP_FORCE_SHELL;
            } else if (f->t - a->focus_since > 2 * (f->movie ? 3 * a->lim.shell : a->lim.shell)) {
                problem(a, "shell_never_advances", "for=%.0f: stopped", f->t - a->focus_since);
                a->why = "shell_hang";
                r |= AP_STOP;
            }
        }
    }
    if (f->t - a->mem_at >= 1) {                     /* the memory, once a second */
        a->mem_at = f->t;
        double m = mem_now(), pk = ap_peak_mb();
        if (pk > a->mem_peak)
            a->mem_peak = pk;
        if (m > 3000 && !a->mem_told) {
            a->mem_told = 1;
            problem(a, "memory", "mb=%.0f", m);
        }
        snprintf(a->summary, sizeof a->summary, "%s state=%d city=%s block=%s focus=%s mon1=%.1f,%.1f,%.1f %s",
                 f->in_game ? "game" : "shell", f->state, f->city ? f->city : "-", f->block ? f->block : "-",
                 f->focus ? f->focus : "-", f->mon[0].pos[0], f->mon[0].pos[1], f->mon[0].pos[2],
                 f->mon[0].state ? f->mon[0].state : "-");
    }
    if (f->in_game && a->playing)
        a->last = *f;
    if (f->t >= a->lim.minutes * 60) {
        a->why = "budget";
        r |= AP_STOP;
    }
    if (a->lim.blocks && a->blocks >= a->lim.blocks) {
        a->why = "blocks";
        r |= AP_STOP;
    }
    if ((a->lim.cities && a->games >= a->lim.cities) || (a->lim.demos && a->demos >= a->lim.demos)) {
        a->why = a->lim.demos ? "demos" : "cities";
        r |= AP_STOP;
    }
    if (a->lim.stop_at_menu && a->games > 0 && !f->in_game && f->focus && !strcmp(f->focus, "MainMenu")) {
        a->why = "main_menu";
        r |= AP_STOP;
    }
    if (a->shot_reason)
        r |= AP_SHOT;
    return r;
}

int ap_close(Autoplay *a, const char *why)
{
    if (!a)
        return 0;
    double pk = ap_peak_mb();
    if (pk > a->mem_peak)
        a->mem_peak = pk;
    if (a->blocks >= 10 && a->mem_first > 0 && a->mem_last - a->mem_first > 500)
        problem(a, "memory_growth", "first_block_mb=%.0f last_block_mb=%.0f", a->mem_first, a->mem_last);
    fprintf(a->log, "SUMMARY t=%.1f end=%s blocks=%d won=%d lost=%d games=%d demos=%d problems=%d peak_mb=%.0f "
            "first_block_mb=%.0f last_block_mb=%.0f\n", a->t, why ? why : a->why ? a->why : "-", a->blocks, a->won, a->lost, a->games,
            a->demos, a->problems, a->mem_peak, a->mem_first, a->mem_last);
    fclose(a->log);
    int code = a->problems ? 3 : 0;
    g_ap = NULL;
    free(a);
    return code;
}
