/* iso.h - the game's data straight from the disc image (port extra, after WoodyRE's datasetup): an ISO 9660 image
 * with 2048-byte sectors (the PS2 DVD's UDF bridge keeps an ISO 9660 tree too, tools/iso9660.py) is read in place,
 * nothing is copied out: vol_open finds RAMPAGE.VOL in it (Vol.base), fx.c the game's ELF. data_find() is where the
 * data is when the command line names none (docs/NATIVE.md "Game data"). */
#ifndef RAMPAGE_ISO_H
#define RAMPAGE_ISO_H
#include <stdint.h>
#include <stdio.h>
#include "data.h"

int iso_is(FILE *f);                                     /* 1 = an ISO 9660 image (a primary volume descriptor) */
/* a file by its path in the image ("RAMPAGE.VOL", "MODULES/PADMAN.IRX"; case and ";1" ignored): 1 = found, its
 * bytes at *offset (from the start of the image), *size long */
int iso_find(FILE *f, const char *path, int64_t *offset, uint32_t *size);

/* the game's ELF for fx.c's words: in a disc image (path) the one SYSTEM.CNF boots (SLES_538.00); beside a
 * RAMPAGE.VOL RAMPAGE.ELF (build.ps1's split copy), else SLES_538.00. malloc'd, NULL if none */
uint8_t *data_elf(const char *path, uint32_t *size);
/* the first place that holds the game's data, in this order: $RAMPAGE_VOL (as it is), build/disc/RAMPAGE.VOL,
 * ./RAMPAGE.VOL, data/RAMPAGE.VOL, RAMPAGE.VOL beside the program, a disc image (*.iso with RAMPAGE.VOL in it)
 * beside the program, in the current folder, in data/, then the user's data folder (%LOCALAPPDATA%\RampageRE;
 * $XDG_DATA_HOME/RampageRE or ~/.local/share/RampageRE) with RAMPAGE.VOL or an image. 1 = found (out); never asks */
int data_find(char *out, size_t cap);
const char *data_where(void);                            /* that list in words, for the message when none is there */
/* the supported release, SLES-53800 version 1.00: 0 (with why in msg) when SYSTEM.CNF (in the image, or beside the
 * VOL) boots another one or RAMPAGE.VOL is not that release's; 1 when it is or nothing says otherwise */
int data_check(const char *path, const Vol *v, char *msg, size_t cap);
#endif
