/* pack.c - makes the single-file build for your own use (make_standalone.bat, "build.bat standalone"): the launcher stub
 * (bundle.c) + RampageRE.exe + the game data of YOUR disc that the port reads (RAMPAGE.VOL, the ELF, SYSTEM.CNF; about
 * 1.35 GB), in the payload layout bundle.c documents. After WoodyRE's tools/native/pack.c.
 * The result holds the game's data: it is for your own machine only, never share or upload it.
 * Usage: pack <stub.exe> <RampageRE.exe> <data> <out.exe>
 *   data: your disc image (.iso), or a RAMPAGE.VOL with SLES_538.00 (or RAMPAGE.ELF) and SYSTEM.CNF beside it, or "auto":
 *   the data the viewer finds by itself (src/iso.c data_find: build\disc, the current folder, %LOCALAPPDATA%\RampageRE...)
 * Every file is copied in 4 MB pieces with 64-bit offsets (the VOL alone is 1.34 GB); the result must stay below 4 GB,
 * which Windows' loader takes as the most an exe may be.
 * Build (build.bat standalone): zig cc -std=c99 -O2 -DRAMPAGE_TOOL -Isrc -o pack.exe tools/native/pack.c src/iso.c */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "plat.h"
#include "iso.h"

#define MAX_EXE 0xff000000ull                       /* below 4 GB, with room for the index */

static uint8_t g_buf[4 << 20];
static const char *g_out;

static void fail(const char *what, const char *path)
{
    fprintf(stderr, "pack: %s%s%s\n", what, path ? " " : "", path ? path : "");
    if (g_out)
        remove(g_out);
    exit(1);
}

/* size bytes of path from at on to o; FNV-1a 64 of them into *hash (bundle.c skips a file it already holds) */
static void copy(FILE *o, const char *path, int64_t at, uint64_t size, uint64_t *hash, uint64_t *done, uint64_t total)
{
    FILE *f = fopen(path, "rb");
    if (!f || _fseeki64(f, at, SEEK_SET) != 0)
        fail("cannot read", path);
    uint64_t h = 0xcbf29ce484222325ull;
    while (size) {
        size_t n = size < sizeof g_buf ? (size_t)size : sizeof g_buf;
        if (fread(g_buf, 1, n, f) != n)
            fail("cannot read all of", path);
        for (size_t i = 0; i < n; i++)
            h = (h ^ g_buf[i]) * 0x100000001b3ull;
        if (fwrite(g_buf, 1, n, o) != n)
            fail("cannot write (disk full?)", g_out);
        size -= n;
        if (total && (*done >> 27) != ((*done + n) >> 27))   /* every 128 MB */
            printf("pack: %llu of %llu MB\n", (unsigned long long)((*done + n) >> 20), (unsigned long long)(total >> 20));
        *done += n;
    }
    fclose(f);
    *hash = h;
}

static uint64_t size_of(const char *path)
{
    FILE *f = fopen(path, "rb");
    int64_t n = f && _fseeki64(f, 0, SEEK_END) == 0 ? _ftelli64(f) : -1;
    if (f)
        fclose(f);
    if (n <= 0)
        fail("cannot read", path);
    return (uint64_t)n;
}

static void put(FILE *o, const void *p, size_t n)
{
    if (fwrite(p, 1, n, o) != n)
        fail("cannot write (disk full?)", g_out);
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: pack <stub.exe> <RampageRE.exe> <disc image | RAMPAGE.VOL | auto> <out.exe>\n");
        return 2;
    }
    char found[1024];
    const char *data = argv[3];
    if (strcmp(data, "auto") == 0) {
        if (!data_find(found, sizeof found)) {
            fprintf(stderr, "pack: found no game data in %s.\nPass your disc image or RAMPAGE.VOL: make_standalone.bat "
                    "\"D:\\path\\to\\Rampage.iso\" (or drag the image onto it).\n", data_where());
            return 1;
        }
        data = found;
    }
    DataPart part[3];
    if (!data_parts(data, part))
        fail("holds no RAMPAGE.VOL (not the game's disc image, nor its RAMPAGE.VOL):", data);
    printf("pack: the game data from %s\n", data);
    if (!part[1].size)
        printf("pack: no %s there: the effects' words will be missing\n", part[1].name);

    /* the entries: the program as RampageRE.exe, then the data as the viewer reads it beside a RAMPAGE.VOL */
    struct { const char *name, *file; int64_t at; uint64_t size, off, hash; } e[4] = {
        { "RampageRE.exe", argv[2], 0, size_of(argv[2]), 0, 0 },
        { "SYSTEM.CNF", part[0].file, part[0].at, part[0].size, 0, 0 },
        { "RAMPAGE.ELF", part[1].file, part[1].at, part[1].size, 0, 0 },
        { "RAMPAGE.VOL", part[2].file, part[2].at, part[2].size, 0, 0 },   /* the largest last */
    };
    uint64_t stub = size_of(argv[1]), total = 0, done = 0;
    for (int i = 0; i < 4; i++)
        total += e[i].size;
    if (stub + total > MAX_EXE)
        fail("the result would be 4 GB or more, more than Windows runs as an exe", NULL);

    g_out = argv[4];
    FILE *o = fopen(g_out, "wb");
    if (!o)
        fail("cannot write", g_out);
    uint64_t h;
    copy(o, argv[1], 0, stub, &h, &done, 0);
    uint32_t count = 0;
    for (int i = 0; i < 4; i++) {
        if (!e[i].size)
            continue;
        e[i].off = (uint64_t)_ftelli64(o);
        copy(o, e[i].file, e[i].at, e[i].size, &e[i].hash, &done, total);
        count++;
    }
    uint64_t ioff = (uint64_t)_ftelli64(o);
    for (int i = 0; i < 4; i++) {
        if (!e[i].size)
            continue;
        uint32_t nl = (uint32_t)strlen(e[i].name);
        put(o, &nl, 4);
        put(o, e[i].name, nl);
        put(o, &e[i].off, 8);
        put(o, &e[i].size, 8);
        put(o, &e[i].hash, 8);
    }
    uint64_t id = ((uint64_t)time(NULL) * 1000003u) ^ ioff;
    for (int i = 0; i < 4; i++)
        id = (id ^ e[i].hash) * 0x100000001b3ull;
    put(o, &ioff, 8);
    put(o, &count, 4);
    put(o, &id, 8);
    put(o, "RAMPPK01", 8);
    if (fclose(o) != 0)
        fail("cannot write (disk full?)", g_out);
    printf("pack: %s - %u files, %.0f MB\n", g_out, count, (double)ioff / (1 << 20));
    return 0;
}
