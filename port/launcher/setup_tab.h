/* WWE SmackDown vs. Raw 2011 PC launcher - the Setup tab (setup_tab.c):
   what a player needs, found or missing, and the Mod Maker's tools
   installed into <game>\Tools. */
#pragma once

#include <windows.h>

#include "mods_tab.h" /* (mods_add_fn: the launcher's control helper) */

enum { SETUP_GAME, SETUP_VCRT, SETUP_MF, SETUP_PYTHON, SETUP_FFMPEG, SETUP_BINK2, SETUP_ADB, SETUP_N };
enum { SETUP_FOUND, SETUP_MISSING, SETUP_OPTIONAL };

typedef struct {
    int state;           /* SETUP_FOUND / SETUP_MISSING / SETUP_OPTIONAL */
    const WCHAR *name;
    WCHAR detail[400];
} SetupRow;

/* The Visual C++ runtime the game and the Mod Maker use - Windows' own (System32): SETUP_VCRT_OK, _OLD (older
   than 14.<SETUP_VCRT_MIN_MINOR>) or _MISSING; `ver` its version ("14.51.36247"). */
enum { SETUP_VCRT_OK, SETUP_VCRT_OLD, SETUP_VCRT_MISSING };
#define SETUP_VCRT_MIN_MINOR 40
#define SETUP_VCRT_URL L"https://aka.ms/vs/17/release/vc_redist.x64.exe"
int setup_vcrt_system(WCHAR *ver, size_t vern);
/* Deletes the VC++ runtime DLLs 2.1.0 shipped (version 14.29.x) from `dir` - copies a player put there keep;
   one in use goes after a restart. The count; `log` (may be NULL) lists them. */
int setup_vcrt_cleanup(const WCHAR *dir, WCHAR *log, size_t logn);

/* Each item's state (rows[SETUP_N]). */
void setup_check(const WCHAR *game_dir, const WCHAR *launcher_dir, SetupRow *rows);

/* Downloads (pinned URLs, sizes and SHA-256s) and installs into
   <game>\Tools: 1 << SETUP_PYTHON (Python 3.14.8 + numpy + Pillow) and / or
   1 << SETUP_FFMPEG. `progress` (may be NULL): permille of the current
   item and a line of text. 1 on success. */
typedef void (*setup_progress_fn)(int permille, const WCHAR *text);
int setup_install(const WCHAR *game_dir, unsigned items, setup_progress_fn progress, volatile LONG *cancel,
                  WCHAR *err, size_t errn);
/* What setup_install(items) downloads, for the confirmation: lines of
   "name - size - host (licence)" and the total in bytes. */
void setup_describe(unsigned items, WCHAR *out, size_t n, unsigned long long *total);

/* The launcher's "go to the Install tab". */
typedef void (*setup_goto_fn)(void);

/* Builds the tab's controls (ids id_base .. id_base + 19). */
void setup_build(HWND wnd, mods_add_fn add, int tab, int id_base, setup_goto_fn go_install);
/* The tab was shown: checks again. */
void setup_show(const WCHAR *game_dir, const WCHAR *launcher_dir);
/* WM_COMMAND: 1 when handled. */
int setup_command(int id, int code);
/* 1 while an install runs (the launcher keeps its window open). */
int setup_busy(void);
