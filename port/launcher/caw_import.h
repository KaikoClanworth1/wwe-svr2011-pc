/* WWE SmackDown vs. Raw 2011 - bringing someone else's Created Superstar into
 * your saves (a .cas file, or an Xbox 360 package of one). */
#pragma once

#include <windows.h>
#include <stdint.h>

/* One of a Created Superstar's Paint Tool logos, as a Paint Tool slot keeps
 * it: 256 palette entries (A R G B bytes) and 256 x 256 8-bit pixels (a
 * 128 x 128 logo is doubled). `id` is the checksum of the Paint Tool slot it
 * was made from (the game's logo id). */
typedef struct {
    uint8_t palette[1024];
    uint8_t pixels[65536];
    uint32_t id;
} CawLogo;

/* Puts the Created Superstar `src` into the first free slot of the saves
 * folder `saves` (whose SaveData.dat must exist): the .cas as the slot's
 * file, its record in SaveData.dat (from the source save's SaveData.dat when
 * it sits beside `src`), the header, and the port's extra logo files it uses
 * (from the source's .logos). Its logos come back in *logos (malloc'd; the
 * caller frees it). Returns the slot (0-based), or -1 with a message in err. */
int caw_import(const WCHAR *src, const WCHAR *saves, WCHAR *name, size_t name_n, CawLogo **logos, int *n_logos,
               WCHAR *err, size_t err_n);
