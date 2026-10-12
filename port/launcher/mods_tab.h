/* WWE SmackDown vs. Raw 2011 PC launcher - the Mods tab (mods_tab.c). */
#pragma once

#include <windows.h>

/* The launcher's control helper: add(tab, class, text, style, x, y, w, h, id)
 * in 96-DPI units, registered with the tab's show/hide list. */
typedef HWND (*mods_add_fn)(int tab, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h,
                            int id);

/* Builds the tab's controls (ids id_base .. id_base + 5). */
void mods_build(HWND wnd, mods_add_fn add, int tab, int id_base, HFONT title_font);
/* The tab was shown: rescans <game_dir>/Mods. */
void mods_show(const WCHAR *game_dir);
/* WM_COMMAND / WM_NOTIFY: 1 when handled. */
int mods_command(int id, int code);
/* 1: handled; 2 / 3: the window answers TRUE / FALSE (renaming a background). */
int mods_notify(const NMHDR *nm);
/* A background's name (manifest.txt name=). 1 on success. */
int mods_rename(const WCHAR *folder, const WCHAR *name);
int mods_remove(const WCHAR *game, const WCHAR *folder);
/* Installs a .svrmod into <game_dir>/Mods (the + button without its dialog;
   tests: --mods-add <file>). 1 on success; the tab's status says why not. */
int mods_install(const WCHAR *game_dir, const WCHAR *zip);

/* A main menu background made from a picture (backgrounds.h; tests:
   --bg-add <game folder> <picture> [logo]). 1 on success; prints the folder. */
int mods_add_background(const WCHAR *game_dir, const WCHAR *image, int logo);
/* Installs the mods shipped with the port (<bundle_dir>\*.svrmod) that the
   game hasn't had yet, or whose shipped file changed. */
void mods_install_bundled(const WCHAR *game_dir, const WCHAR *bundle_dir);
