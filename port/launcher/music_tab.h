/* WWE SmackDown vs. Raw 2011 PC launcher - the Music tab (music_tab.c). */
#pragma once

#include <windows.h>

#include "mods_tab.h" /* (mods_add_fn: the launcher's control helper) */

/* Builds the tab's controls (ids id_base .. id_base + 7). */
void music_build(HWND wnd, mods_add_fn add, int tab, int id_base);
/* The tab was shown: rescans <game_dir>/Music. */
void music_show(const WCHAR *game_dir);
/* WM_COMMAND: 1 when handled. */
int music_command(int id, int code);
/* Copies songs into <game_dir>/Music (tests: --music-add <file>...). The
   number copied; the tab's status says what happened. */
int music_add_files(const WCHAR *game_dir, const WCHAR *const *files, int n);
