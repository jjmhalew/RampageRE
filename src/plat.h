/* plat.h - what the viewer takes from the operating system outside the window (render_gl.c): Windows for now, as in
 * WoodyRE; the names are the Windows ones so a later SDL port keeps one spelling. */
#ifndef RAMPAGE_PLAT_H
#define RAMPAGE_PLAT_H
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define plat_gl_proc(name) ((void (*)(void))wglGetProcAddress(name))
static inline void plat_message(const char *text, int warn) { MessageBoxA(NULL, text, "RampageRE", warn ? MB_ICONWARNING : MB_ICONINFORMATION); }
/* the free room in KB on the drive that holds path's folder (the current drive for a bare name), -1 unknown */
static inline long long plat_free_kb(const char *path)
{
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s", path ? path : ".");
    char *cut = strrchr(dir, '\\'), *cut2 = strrchr(dir, '/');
    if (cut2 > cut) cut = cut2;
    if (cut) cut[1] = 0;
    else snprintf(dir, sizeof dir, ".\\");
    ULARGE_INTEGER avail;
    return GetDiskFreeSpaceExA(dir, &avail, NULL, NULL) ? (long long)(avail.QuadPart / 1024) : -1;
}
#else
#include <stdio.h>
#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _fseeki64 fseeko
static inline void plat_message(const char *text, int warn) { (void)warn; fprintf(stderr, "%s\n", text); }
static inline long long plat_free_kb(const char *path) { (void)path; return -1; }
#endif
#endif
