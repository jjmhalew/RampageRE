/* bundle.c - the single-file launcher of RampageRE (not part of the port itself; after WoodyRE's tools/native/bundle.c).
 * tools/native/pack.c appends RampageRE.exe and the game data of your disc (RAMPAGE.VOL, the ELF as RAMPAGE.ELF,
 * SYSTEM.CNF) to this exe; at start it unpacks them once to %LOCALAPPDATA%\RampageRE (again only what a new bundle
 * changes), then runs RampageRE.exe from there with RAMPAGE_VOL set to that RAMPAGE.VOL and passes the command line on
 * (e.g. "RampageRE-standalone.exe --shell" or "... Chicago 1 --monster George"). rampage.cfg, rampage_profile.bin,
 * rampage.log and mods\ are written there too and survive a new bundle. The Wii's files, when pack.c was given them
 * (wii/Game/NAME ..., docs/WII_PLAN.md), are unpacked into its wii folder, and RAMPAGE_WII_DATA names that folder unless
 * RAMPAGE_WII_DATA or that rampage.cfg's wii_data= names another (or none).
 * The data inside makes the result for your own use only: never share or upload it.
 *
 * Payload layout (after the launcher itself): the file bodies, then the index (per entry: u32 name length, the name,
 * u64 offset from the start of the exe, u64 size, u64 FNV-1a 64 of the bytes), then the trailer: u64 index offset,
 * u32 count, u64 bundle id, "RAMPPK01". All offsets are 64-bit (the VOL alone is 1.34 GB; the exe stays below 4 GB).
 * What was unpacked is listed in <folder>\.bundle (hash, size, name a line): a file whose size and hash are listed and
 * whose size on disk matches is not written again, so a newer RampageRE.exe does not cost another 1.35 GB.
 *
 * A command line with --shot (the tests) shows no window and no message box: messages go to stderr. Testing:
 * RAMPAGE_STANDALONE_DIR=<folder> unpacks there instead of %LOCALAPPDATA%\RampageRE.
 * build.bat standalone builds it: zig cc -std=c99 -O2 -Wl,--subsystem,windows tools/native/bundle.c res/rampagere.rc
 * -luser32 -lgdi32, and appends the payload with pack.c. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>

#pragma pack(push, 1)
typedef struct { uint64_t index_off; uint32_t count; uint64_t id; char magic[8]; } Trailer;
#pragma pack(pop)

typedef struct { wchar_t name[MAX_PATH]; uint64_t off, size, hash; int skip; } Entry;

static HWND g_win, g_text;
static int g_quiet, g_err;                    /* --shot: no window, no box; stderr can be written to */

static void say(const wchar_t *msg, int error)
{
    if (g_err)
        fwprintf(stderr, L"RampageRE: %ls\n", msg);
    if (!g_quiet && (error || !g_err))
        MessageBoxW(NULL, msg, L"RampageRE", error ? MB_ICONERROR : MB_ICONINFORMATION);
}

static void fail(const wchar_t *what, const wchar_t *path)
{
    wchar_t msg[MAX_PATH * 3];
    DWORD e = GetLastError();
    int n = swprintf(msg, MAX_PATH * 3, L"%ls%ls%ls", what, path ? L"\n" : L"", path ? path : L"");
    if (e && n > 0)
        swprintf(msg + n, MAX_PATH * 3 - n, L" (error %lu)", e);
    if (g_win)
        DestroyWindow(g_win);
    say(msg, 1);
    ExitProcess(1);
}

static void pump(void)
{
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

static void progress(uint64_t done, uint64_t total)
{
    static int last = -1;
    int pct = total ? (int)(done * 100 / total) : 100;
    if (pct == last)
        return;
    last = pct;
    wchar_t s[128];
    swprintf(s, 128, L"Unpacking the game data (first start only)... %d%%", pct);
    if (g_text) {
        SetWindowTextW(g_text, s);
        pump();
    }
    if (g_err && pct % 10 == 0)
        fwprintf(stderr, L"RampageRE: %ls\n", s);
}

static void show_window(void)
{
    if (g_quiet)
        return;
    g_win = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"RampageRE", WS_POPUP | WS_BORDER | WS_VISIBLE,
                            (GetSystemMetrics(SM_CXSCREEN) - 440) / 2, (GetSystemMetrics(SM_CYSCREEN) - 70) / 2, 440, 70,
                            NULL, NULL, NULL, NULL);
    g_text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 0, 0, 440, 70, g_win,
                             NULL, NULL, NULL);
    SendMessageW(g_text, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), 0);
    pump();
}

static int read_at(HANDLE f, uint64_t off, void *buf, DWORD n)
{
    LARGE_INTEGER li;
    DWORD got;
    li.QuadPart = (LONGLONG)off;
    return SetFilePointerEx(f, li, NULL, FILE_BEGIN) && ReadFile(f, buf, n, &got, NULL) && got == n;
}

static uint64_t disk_size(const wchar_t *path)  /* the size of a file, ~0 if it is not there */
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return ~0ull;
    return (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
}

static const wchar_t *skip_argv0(const wchar_t *c)
{
    if (*c == L'"') {
        c++;
        while (*c && *c != L'"')
            c++;
        if (*c)
            c++;
    } else
        while (*c && *c != L' ' && *c != L'\t')
            c++;
    while (*c == L' ' || *c == L'\t')
        c++;
    return c;
}

/* a plain name, or folder/name with plain parts (no \\ or :, no empty, . or .. part): inside the folder */
static int safe_name(const wchar_t *n)
{
    if (!n[0] || wcspbrk(n, L"\\:"))
        return 0;
    for (const wchar_t *p = n;;) {
        const wchar_t *sl = wcschr(p, L'/');
        size_t l = sl ? (size_t)(sl - p) : wcslen(p);
        if (l == 0 || (l == 1 && p[0] == L'.') || (l == 2 && p[0] == L'.' && p[1] == L'.'))
            return 0;
        if (!sl)
            return 1;
        p = sl + 1;
    }
}

/* base\name with name's / as \; make: its folders made */
static void out_path(wchar_t *out, size_t cap, const wchar_t *base, const wchar_t *name, int make)
{
    size_t bl = wcslen(base);
    swprintf(out, cap, L"%ls\\%ls", base, name);
    for (wchar_t *c = out + bl + 1; *c; c++)
        if (*c == L'/') {
            *c = 0;
            if (make)
                CreateDirectoryW(out, NULL);
            *c = L'\\';
        }
}

/* 1 = base\rampage.cfg has a wii_data= with a value (the player's own choice, none included) */
static int cfg_names_wii(const wchar_t *base)
{
    wchar_t path[MAX_PATH + 16];
    char line[1200];
    int yes = 0;
    swprintf(path, MAX_PATH + 16, L"%ls\\rampage.cfg", base);
    FILE *f = _wfopen(path, L"rb");
    while (f && !yes && fgets(line, sizeof line, f)) {
        const char *k = line;
        while (*k == ' ' || *k == '\t') k++;
        if (_strnicmp(k, "wii_data", 8) != 0)
            continue;
        for (k += 8; *k == ' ' || *k == '\t'; k++) { }
        if (*k++ != '=')
            continue;
        while (*k == ' ' || *k == '\t') k++;
        yes = *k && *k != '#' && *k != '\r' && *k != '\n';
    }
    if (f)
        fclose(f);
    return yes;
}

/* the .bundle list: "hash size name" lines of the files unpacked whole */
static int listed(const wchar_t *state, const Entry *e)
{
    FILE *f = _wfopen(state, L"rb");
    char line[MAX_PATH * 3], want[MAX_PATH * 3], name[MAX_PATH * 3];
    int yes = 0;
    if (!f)
        return 0;
    WideCharToMultiByte(CP_UTF8, 0, e->name, -1, name, sizeof name, NULL, NULL);
    snprintf(want, sizeof want, "%016llx %llu %s", (unsigned long long)e->hash, (unsigned long long)e->size, name);
    while (!yes && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        yes = strcmp(line, want) == 0;
    }
    fclose(f);
    return yes;
}

static void write_list(const wchar_t *state, const Entry *e, uint32_t n)
{
    wchar_t part[MAX_PATH + 8];
    swprintf(part, MAX_PATH + 8, L"%ls.part", state);
    FILE *f = _wfopen(part, L"wb");
    if (!f)
        return;
    for (uint32_t i = 0; i < n; i++)
        if (e[i].skip) {
            char name[MAX_PATH * 3];
            WideCharToMultiByte(CP_UTF8, 0, e[i].name, -1, name, sizeof name, NULL, NULL);
            fprintf(f, "%016llx %llu %s\n", (unsigned long long)e[i].hash, (unsigned long long)e[i].size, name);
        }
    if (fclose(f) == 0)
        MoveFileExW(part, state, MOVEFILE_REPLACE_EXISTING);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hi; (void)hp; (void)cmd; (void)show;
    const wchar_t *args = skip_argv0(GetCommandLineW());
    g_quiet = wcsstr(args, L"--shot") != NULL;
    HANDLE eh = GetStdHandle(STD_ERROR_HANDLE);
    if (eh && eh != INVALID_HANDLE_VALUE)                  /* redirected by whoever started it: keep that */
        g_err = 1;
    else if (AttachConsole(ATTACH_PARENT_PROCESS) && freopen("CONOUT$", "w", stderr))   /* started from a console */
        g_err = 1;

    wchar_t self[MAX_PATH];
    if (!GetModuleFileNameW(NULL, self, MAX_PATH))
        fail(L"Cannot find this exe's own path.", NULL);
    HANDLE f = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        fail(L"Cannot open the exe itself:", self);
    LARGE_INTEGER fsz;
    Trailer t;
    if (!GetFileSizeEx(f, &fsz) || (uint64_t)fsz.QuadPart < sizeof t ||
        !read_at(f, (uint64_t)fsz.QuadPart - sizeof t, &t, sizeof t) || memcmp(t.magic, "RAMPPK01", 8) != 0 ||
        t.index_off > (uint64_t)fsz.QuadPart - sizeof t || t.count == 0 || t.count > 4096) {
        SetLastError(0);
        fail(L"No game data is attached to this exe (make it with make_standalone.bat).", NULL);
    }

    /* the index */
    uint64_t isz = (uint64_t)fsz.QuadPart - sizeof t - t.index_off;
    unsigned char *idx = isz < (1u << 20) ? malloc((size_t)isz) : NULL, *q = idx;
    Entry *e = calloc(t.count, sizeof *e);
    if (!idx || !e || !read_at(f, t.index_off, idx, (DWORD)isz))
        fail(L"Cannot read the bundle's index.", NULL);
    for (uint32_t i = 0; i < t.count; i++) {
        uint32_t nl;
        if ((size_t)(q - idx) + 4 > isz || (memcpy(&nl, q, 4), nl == 0 || nl >= MAX_PATH) ||
            (size_t)(q - idx) + 4 + nl + 24 > isz)
            fail(L"The bundle's index is damaged.", NULL);
        int wl = MultiByteToWideChar(CP_UTF8, 0, (const char *)q + 4, (int)nl, e[i].name, MAX_PATH - 1);
        e[i].name[wl > 0 ? wl : 0] = 0;
        q += 4 + nl;
        memcpy(&e[i].off, q, 8);
        memcpy(&e[i].size, q + 8, 8);
        memcpy(&e[i].hash, q + 16, 8);
        q += 24;
        /* plain names (the Wii's in wii/Game/ ...) inside the folder, inside the bundle's bytes */
        if (!safe_name(e[i].name) || e[i].off > t.index_off || e[i].size > t.index_off - e[i].off)
            fail(L"The bundle's index is damaged.", NULL);
    }
    free(idx);

    /* the folder: %LOCALAPPDATA%\RampageRE, or RAMPAGE_STANDALONE_DIR (testing) */
    wchar_t base[MAX_PATH], state[MAX_PATH + 16], vol[MAX_PATH + 16], exe[MAX_PATH + 16];
    DWORD n = GetEnvironmentVariableW(L"RAMPAGE_STANDALONE_DIR", base, MAX_PATH);
    if (!n || n >= MAX_PATH - 32) {
        n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (!n || n >= MAX_PATH - 32)
            fail(L"LOCALAPPDATA is not set.", NULL);
        wcscat(base, L"\\RampageRE");
    }
    CreateDirectoryW(base, NULL);
    swprintf(state, MAX_PATH + 16, L"%ls\\.bundle", base);

    /* what is there already; the free room for the rest */
    uint64_t total = 0, done = 0;
    int wii = 0;
    for (uint32_t i = 0; i < t.count; i++) {
        wchar_t out[MAX_PATH * 2];
        out_path(out, MAX_PATH * 2, base, e[i].name, 0);
        wii |= wcsncmp(e[i].name, L"wii/", 4) == 0;
        e[i].skip = disk_size(out) == e[i].size && listed(state, &e[i]);
        if (!e[i].skip)
            total += e[i].size;
    }
    ULARGE_INTEGER avail;
    if (total && GetDiskFreeSpaceExW(base, &avail, NULL, NULL) && avail.QuadPart < total + (64u << 20)) {
        wchar_t m[MAX_PATH + 200];
        swprintf(m, MAX_PATH + 200, L"Not enough free disk space to unpack the game data (%llu MB) into", (unsigned long long)(total >> 20));
        SetLastError(ERROR_DISK_FULL);
        fail(m, base);
    }
    if (total) {
        show_window();
        progress(0, total);
        unsigned char *buf = malloc(4u << 20);
        if (!buf)
            fail(L"Out of memory.", NULL);
        for (uint32_t i = 0; i < t.count; i++) {
            if (e[i].skip)
                continue;
            wchar_t out[MAX_PATH * 2], part[MAX_PATH * 2 + 8];
            out_path(out, MAX_PATH * 2, base, e[i].name, 1);
            swprintf(part, MAX_PATH * 2 + 8, L"%ls.part", out);
            HANDLE o = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (o == INVALID_HANDLE_VALUE)
                fail(L"Cannot write", part);
            LARGE_INTEGER li;
            li.QuadPart = (LONGLONG)e[i].off;
            if (!SetFilePointerEx(f, li, NULL, FILE_BEGIN))
                fail(L"Cannot read the bundle.", NULL);
            for (uint64_t left = e[i].size; left; ) {
                DWORD chunk = left > (4u << 20) ? (4u << 20) : (DWORD)left, got, put;
                if (!ReadFile(f, buf, chunk, &got, NULL) || got != chunk)
                    fail(L"Cannot read the bundle.", NULL);
                if (!WriteFile(o, buf, chunk, &put, NULL) || put != chunk) {
                    CloseHandle(o);
                    DeleteFileW(part);
                    fail(L"Cannot write (is the disk full?)", out);
                }
                left -= chunk;
                done += chunk;
                progress(done, total);
            }
            if (!CloseHandle(o) || !MoveFileExW(part, out, MOVEFILE_REPLACE_EXISTING)) {
                DeleteFileW(part);
                fail(L"Cannot write (is RampageRE still running?)", out);
            }
            e[i].skip = 1;                                 /* whole: listed */
            write_list(state, e, t.count);
        }
        free(buf);
        if (g_win)
            DestroyWindow(g_win);
        g_win = NULL;
    }
    CloseHandle(f);

    /* RampageRE.exe [the arguments this exe got], run in the folder with RAMPAGE_VOL its RAMPAGE.VOL */
    swprintf(exe, MAX_PATH + 16, L"%ls\\RampageRE.exe", base);
    swprintf(vol, MAX_PATH + 16, L"%ls\\RAMPAGE.VOL", base);
    SetEnvironmentVariableW(L"RAMPAGE_VOL", vol);
    if (wii && !GetEnvironmentVariableW(L"RAMPAGE_WII_DATA", NULL, 0) && !cfg_names_wii(base)) {
        wchar_t wd[MAX_PATH + 16];                         /* the Wii's, unpacked (docs/WII_PLAN.md) */
        swprintf(wd, MAX_PATH + 16, L"%ls\\wii", base);
        SetEnvironmentVariableW(L"RAMPAGE_WII_DATA", wd);
    }
    size_t cl = wcslen(args) + MAX_PATH + 8;
    wchar_t *line = malloc(cl * sizeof *line);
    if (!line)
        fail(L"Out of memory.", NULL);
    swprintf(line, cl, L"\"%ls\" %ls", exe, args);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;                     /* the tests read what it prints */
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessW(exe, line, NULL, NULL, TRUE, 0, NULL, base, &si, &pi))
        fail(L"Cannot start", exe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}
