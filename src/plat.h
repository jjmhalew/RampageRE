/* plat.h - what the viewer takes from the operating system outside the window (render_gl.c): Windows for now, as in
 * WoodyRE; the names are the Windows ones so a later SDL port keeps one spelling. */
#ifndef RAMPAGE_PLAT_H
#define RAMPAGE_PLAT_H
#ifdef _WIN32
#include <windows.h>
#define plat_gl_proc(name) ((void (*)(void))wglGetProcAddress(name))
static inline void plat_message(const char *text, int warn) { MessageBoxA(NULL, text, "RampageRE", warn ? MB_ICONWARNING : MB_ICONINFORMATION); }
#else
#include <stdio.h>
#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _fseeki64 fseeko
static inline void plat_message(const char *text, int warn) { (void)warn; fprintf(stderr, "%s\n", text); }
#endif
#endif
