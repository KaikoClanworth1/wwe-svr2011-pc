/* WWE SmackDown vs. Raw 2011 PC launcher - the Texture packs tab
   (texpacks_tab.c). */
#pragma once

#include <windows.h>

#include "mods_tab.h" /* (mods_add_fn: the launcher's control helper) */

/* svr2011.toml access (the launcher's): get a top-level key's value as
   written (quotes removed) - 0 when absent; set it (value as written). */
typedef int (*texpacks_get_fn)(const char *key, char *out, size_t n);
typedef int (*texpacks_set_fn)(const char *key, const char *value);

/* Builds the tab's controls (ids id_base .. id_base + 11). */
void texpacks_build(HWND wnd, mods_add_fn add, int tab, int id_base, texpacks_get_fn get, texpacks_set_fn set);
/* The tab was shown: rescans <game_dir>\Texture Packs. */
void texpacks_show(const WCHAR *game_dir);
/* WM_COMMAND / WM_NOTIFY: 1 when handled. */
int texpacks_command(int id, int code);
int texpacks_notify(const NMHDR *nm);
/* Adds a pack: a folder (copied) or a .zip (extracted) into Texture Packs
   and turns it on (tests: --texpacks-add <path>). 1 on success. */
int texpacks_add(const WCHAR *game_dir, const WCHAR *path);
