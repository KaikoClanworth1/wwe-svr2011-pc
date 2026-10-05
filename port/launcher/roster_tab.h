/* WWE SmackDown vs. Raw 2011 PC launcher - the Roster tab (roster_tab.c):
 * the list menus' sort categories of each character (sort_tags.txt). */
#pragma once

#include <windows.h>

#include "mods_tab.h" /* (mods_add_fn: the launcher's control helper) */

/* Builds the tab's controls (ids id_base .. id_base + 15). */
void roster_build(HWND wnd, mods_add_fn add, int tab, int id_base);
/* The tab was shown: reads <game_dir>\UserData\sort_tags.txt and the mods. */
void roster_show(const WCHAR *game_dir);
/* WM_COMMAND: 1 when handled. */
int roster_command(int id, int code);
/* Tags a character (tests: --roster-tag <id> <SMACKDOWN|RAW|NPC|MODS|DEFAULT>):
   1 when sort_tags.txt was written. */
int roster_tag(const WCHAR *game_dir, int id, const WCHAR *category);
