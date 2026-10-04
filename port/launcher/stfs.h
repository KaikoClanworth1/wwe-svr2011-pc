/* WWE SmackDown vs. Raw 2011 - Xbox 360 save import: reads the console's
 * saved-game packages (STFS "CON " / "LIVE" / "PIRS") and turns them into the
 * port's save files. */
#pragma once

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

/* Each of the game's 360 saves under `src` (a 5451085D folder, its 00000001,
 * a USB drive's Content folder, ...; searched a few folders deep) becomes
 * <dest>\<package file name> plus its header in <dest>\.info (the display
 * name the game finds it by). Returns how many; *skipped counts the packages
 * that weren't the game's saves or couldn't be read. */
int stfs_import_saves(const WCHAR *src, const WCHAR *dest, int *skipped);

/* A 360 saved-game package of the game's: its save file, malloc'd (the
 * caller frees it). 0 if `path` isn't one. */
int stfs_read_save(const WCHAR *path, uint8_t **data, size_t *size);
