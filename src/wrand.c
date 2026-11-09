/* wrand.c - the world's two random states (gWorld +0x828, +0x82C) and its step, see wrand.h */
#include "wrand.h"

/* RAMPAGE_RNGLOG=file: who draws (tools/movelog.py, against the recompiled game's PS2X_RNGLOG): one line per draw,
 * the move log's frame, P (Park-Miller) or L (lagged Fibonacci), and the first callers outside this file, the first
 * with its line (Windows, through dbghelp and the build's .pdb) */
#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
static FILE *rng_log;
static int rng_frame;
typedef struct {
    ULONG SizeOfStruct, TypeIndex;
    ULONG64 Reserved[2];
    ULONG Index, Size;
    ULONG64 ModBase;
    ULONG Flags;
    ULONG64 Value, Address;
    ULONG Register, Scope, Tag, NameLen, MaxNameLen;
    CHAR Name[128];
} RngSym;                                            /* dbghelp's SYMBOL_INFO with room for the name */
static BOOL (WINAPI *rng_sym_from_addr)(HANDLE, DWORD64, DWORD64 *, void *);
typedef struct { DWORD SizeOfStruct; PVOID Key; DWORD LineNumber; PCHAR FileName; DWORD64 Address; } RngLine;
static BOOL (WINAPI *rng_line_from_addr)(HANDLE, DWORD64, PDWORD, RngLine *);   /* SymGetLineFromAddr64 */

void wrand_log_frame(int frame) { rng_frame = frame; }

static void rng_note(char kind, uint32_t a, uint32_t b)   /* a, b: the state drawn from (P's; L's i and j) */
{
    static int init;
    if (!init) {
        init = 1;
        const char *path = getenv("RAMPAGE_RNGLOG");
        HMODULE dh = path ? LoadLibraryA("dbghelp.dll") : NULL;
        BOOL (WINAPI *sym_init)(HANDLE, PCSTR, BOOL) = dh ? (void *)GetProcAddress(dh, "SymInitialize") : NULL;
        rng_sym_from_addr = dh ? (void *)GetProcAddress(dh, "SymFromAddr") : NULL;
        rng_line_from_addr = dh ? (void *)GetProcAddress(dh, "SymGetLineFromAddr64") : NULL;
        if (sym_init && rng_sym_from_addr && sym_init(GetCurrentProcess(), NULL, TRUE))
            rng_log = fopen(path, "w");
    }
    if (!rng_log)
        return;
    void *st[10];
    USHORT n = RtlCaptureStackBackTrace(1, 10, st, NULL);
    fprintf(rng_log, "%d %c", rng_frame, kind);
    int shown = 0;
    for (USHORT k = 0; k < n && shown < 3; k++) {
        RngSym sym;
        memset(&sym, 0, sizeof sym);
        sym.SizeOfStruct = 88;                       /* sizeof(SYMBOL_INFO) */
        sym.MaxNameLen = sizeof sym.Name;
        DWORD64 off = 0;
        if (!rng_sym_from_addr(GetCurrentProcess(), (DWORD64)(uintptr_t)st[k], &off, &sym))
            continue;
        if (!strncmp(sym.Name, "wrand_", 6) || !strcmp(sym.Name, "rng_note"))
            continue;
        fprintf(rng_log, " %s", sym.Name);
        RngLine line = { sizeof line };
        DWORD disp = 0;                              /* the first caller's line: an inlined draw's place */
        if (!shown && rng_line_from_addr &&
            rng_line_from_addr(GetCurrentProcess(), (DWORD64)(uintptr_t)st[k], &disp, &line))
            fprintf(rng_log, ":%lu", (unsigned long)line.LineNumber);
        shown++;
    }
    if (kind == 'P')                                 /* and the state it draws from, as the game's log has it */
        fprintf(rng_log, " s=%08x", a);
    else
        fprintf(rng_log, " s=%u,%u", a, b);
    fputc('\n', rng_log);
    fflush(rng_log);
}
#else
void wrand_log_frame(int frame) { (void)frame; }
#define rng_note(kind, a, b) ((void)0)
#endif

static float step = 1.0f / 30.0f;

float world_step(void) { return step; }
void world_set_step(float s) { step = s; }

static uint32_t pm = 0x2545F491u;

uint32_t *wrand_pm_state(void) { return &pm; }
void wrand_seed_pm(uint32_t seed) { pm = seed; }

uint32_t wrand_pm_next(uint32_t *s)
{
    if (s == &pm)
        rng_note('P', *s, 0);
    uint64_t x = (uint64_t)*s * 16807u;
    x = (x & 0x7FFFFFFFu) + (x >> 31);
    x = (x & 0x7FFFFFFFu) + (x >> 31);
    *s = x == 0x7FFFFFFFu ? 0 : (uint32_t)x;
    return *s;
}

uint32_t wrand_pm(void) { return wrand_pm_next(&pm); }

float wrand_frand(void) { return (float)wrand_pm() * (1.0f / 2147483648.0f); }
int wrand_rand(void) { return (int)(wrand_pm() >> 8 & 0xFFFF); }

/* 0x106950: v = t[i] + t[j], i on (16 wraps to 0), t[j] = v, j on. Seeded (0x1069C0) i = 12, j = 0, t[0] = 0, t[k]
 * the Park-Miller run from the seed | 1, t[6] |= 1 */
static struct { uint32_t i, j, t[17]; } lf = { 12, 0, { 0 } };

void wrand_seed_lf(uint32_t seed)
{
    uint32_t x = seed;
    lf.i = 12;
    lf.j = 0;
    lf.t[0] = 0;
    for (int k = 1; k < 17; k++) {
        uint64_t y = (uint64_t)x * 16807u;
        y = (y & 0x7FFFFFFFu) + (y >> 31);
        y = (y & 0x7FFFFFFFu) + (y >> 31);
        x = y == 0x7FFFFFFFu ? 0 : (uint32_t)y;
        lf.t[k] = x | 1;
    }
    lf.t[6] |= 1;
}

void wrand_lf_peek(uint32_t *i, uint32_t *j, uint32_t *sum)
{
    *i = lf.i;
    *j = lf.j;
    *sum = 0;
    for (int k = 0; k < 17; k++)
        *sum += lf.t[k];
}

uint32_t wrand_lf(void)
{
    rng_note('L', lf.i, lf.j);
    uint32_t v = lf.t[lf.i] + lf.t[lf.j];
    lf.i = lf.i == 16 ? 0 : lf.i + 1;
    lf.t[lf.j] = v;
    lf.j = lf.j == 16 ? 0 : lf.j + 1;
    return v;
}
