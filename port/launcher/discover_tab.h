/* WWE SmackDown vs. Raw 2011 PC launcher - Mods -> Discovery (discover_tab.c): players' .svrmod mods on the
   online server (server/discovery.py) - search them, see their pictures, download and install them, upload your
   own (signed in online: its name is the author). */
#pragma once

#include <windows.h>

#include "mods_tab.h" /* (mods_add_fn: the launcher's control helper) */

/* The online server's base address (e.g. https://sho-ti.me/svr) and the signed-in account: its token, name
   ("" signed out) and id (*id: -1 signed out) - a mod is the player's own when its owner is that id. */
typedef void (*discover_server_fn)(WCHAR *out, int n);
typedef void (*discover_account_fn)(char *token, size_t token_n, WCHAR *name, size_t name_n, long long *id);

/* Builds the page's controls (ids id_base .. id_base + 63). */
void discover_build(HWND wnd, mods_add_fn add, int tab, int id_base, discover_server_fn server,
                    discover_account_fn account);
/* The page shown (the first time: the newest mods). */
void discover_show(const WCHAR *game_dir);
int discover_command(int id, int code);
int discover_notify(const NMHDR *nm);
/* Its owner-drawn picture (WM_DRAWITEM); 1 when it was its. */
int discover_draw(const DRAWITEMSTRUCT *d);
/* Its threads' answers (WM_APP + 20); 1 when it was its. */
#define WM_APP_DISCOVER (WM_APP + 20)
int discover_message(UINT m, WPARAM wp, LPARAM lp);
