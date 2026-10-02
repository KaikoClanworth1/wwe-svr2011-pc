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
int mods_notify(const NMHDR *nm);
/* Installs a .svrmod into <game_dir>/Mods (the + button without its dialog;
   tests: --mods-add <file>). 1 on success; the tab's status says why not. */
int mods_install(const WCHAR *game_dir, const WCHAR *zip);
