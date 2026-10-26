/* iso.h - the game's data straight from the disc image (port extra, after WoodyRE's datasetup): an ISO 9660 image
 * with 2048-byte sectors (the PS2 DVD's UDF bridge keeps an ISO 9660 tree too, tools/iso9660.py) is read in place,
 * nothing is copied out: vol_open finds RAMPAGE.VOL in it (Vol.base), fx.c the game's ELF. An APK of
 * make_android_bundle.bat counts as an image too: its stored entries assets/RAMPAGE.VOL, assets/SLES_538.00 and
 * assets/SYSTEM.CNF are the disc's files (tools/native/apkbundle.c). data_find() is where the data is when the command
 * line names none (docs/NATIVE.md "Game data"). */
#ifndef RAMPAGE_ISO_H
#define RAMPAGE_ISO_H
#include <stdint.h>
#include <stdio.h>
#include "data.h"

int iso_is(FILE *f);                                     /* 1 = an ISO 9660 image (a primary volume descriptor), or a
                                                          * ZIP (APK) with a stored assets/RAMPAGE.VOL */
/* a file by its path in the image ("RAMPAGE.VOL", "MODULES/PADMAN.IRX"; case and ";1" ignored): 1 = found, its
 * bytes at *offset (from the start of the image), *size long */
int iso_find(FILE *f, const char *path, int64_t *offset, uint32_t *size);
/* a ZIP's (an APK of make_android_bundle.bat) stored files in assets/<prefix> ("wii/Game/"), not in its subfolders:
 * fn(ctx, the name past the prefix, its bytes at offset, size) for each. How many */
int zip_each(FILE *f, const char *prefix, void (*fn)(void *ctx, const char *name, int64_t at, uint32_t size), void *ctx);

/* the game's ELF for fx.c's words: in a disc image (path) the one SYSTEM.CNF boots (SLES_538.00, SLUS_213.23); beside
 * a RAMPAGE.VOL RAMPAGE.ELF (build.ps1's split copy), else SLES_538.00, else SLUS_213.23. malloc'd, NULL if none */
uint8_t *data_elf(const char *path, uint32_t *size);
/* the disc's files the port reads, and where they are for path (a disc image or APK; or a RAMPAGE.VOL with SYSTEM.CNF
 * and RAMPAGE.ELF, SLES_538.00 or SLUS_213.23 beside it): [0] SYSTEM.CNF, [1] the ELF, [2] RAMPAGE.VOL; each in file from at on,
 * size bytes (0 = not there); name: what an image calls it (the ELF: what SYSTEM.CNF boots). 1 = RAMPAGE.VOL is there.
 * For the packers of tools/native (make_standalone.bat, make_android_bundle.bat) */
typedef struct { char file[1024]; int64_t at; uint32_t size; char name[64]; } DataPart;
int data_parts(const char *path, DataPart part[3]);
/* the first place that holds the game's data, in this order: $RAMPAGE_VOL (as it is), build/disc/RAMPAGE.VOL,
 * ./RAMPAGE.VOL, data/RAMPAGE.VOL, RAMPAGE.VOL beside the program, a disc image (*.iso with RAMPAGE.VOL in it)
 * beside the program, in the current folder, in data/, then the user's data folder (%LOCALAPPDATA%\RampageRE;
 * $XDG_DATA_HOME/RampageRE or ~/.local/share/RampageRE) with RAMPAGE.VOL or an image, then the path that folder's
 * disc.txt holds (data_ask's choice); on the Switch first sdmc:/switch/rampagere (SWITCH_HOME, either), on Android
 * the app folder (android_home()), then the installed APK (android_apk(): a bundle of make_android_bundle.bat; a first
 * start without either asks, android_data_setup). 1 = found (out); never asks */
int data_find(char *out, size_t cap);
int data_usable(const char *path);                       /* 1 = path is a RAMPAGE.VOL or an image (APK) with one */
int data_remember(const char *path);                     /* path into the user's data folder's disc.txt; 1 = written */
#if defined _WIN32 && !defined RAMPAGE_TOOL
/* the first start on Windows (not under --shot): a message, then a file dialog for the disc image or RAMPAGE.VOL,
 * checked (data_usable) and remembered (data_remember). 1 = chosen (out), 0 = cancelled (plat_told set) */
int data_ask(char *out, size_t cap);
#endif
const char *data_where(void);                            /* that list in words, for the message when none is there */
/* the supported releases, SLES-53800 version 1.00 (Europe) and SLUS-21323 version 1.01 (USA; the same ELF and
 * RAMPAGE.VOL): 0 (with why in msg) when SYSTEM.CNF (in the image, or beside the VOL) boots another one or RAMPAGE.VOL
 * is not theirs; 1 when it is or nothing says otherwise */
int data_check(const char *path, const Vol *v, char *msg, size_t cap);
/* gApp.getRegion() [R 0x10D290]: SYSTEM.CNF's BOOT2 with "SLUS" 0, "SLES" 1, else 2; 1 (the European disc) without
 * a SYSTEM.CNF */
int data_region(const char *path, const Vol *v);
#endif
