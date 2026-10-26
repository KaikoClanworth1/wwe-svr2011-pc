/* WWE SmackDown vs. Raw 2011 - Report a problem: a recorded match and the mods it used (report_match.c).
   The game writes a folder per match, <game>\logs\matches\<date>_<people>\ (match.json, inputs.bin, box.txt,
   loaded.txt, mods.txt); the report sends that folder as its "match" part and, when the player ticks it, the mods
   the match used as "mod" parts - only those the server doesn't have yet (asked by content hash first).
   Plain C: the launcher builds it. */
#pragma once

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { REPORT_MAX_MATCHES = 20, REPORT_MAX_MODS = 64 };

typedef struct {
    WCHAR dir[MAX_PATH];     /* the match's folder */
    WCHAR label[200];        /* what the picker shows: people, arena, when */
    FILETIME time;           /* (newest first) */
} ReportMatch;

typedef struct {
    WCHAR type[32];          /* the Mods sub-folder: Superstars, Arenas, Media, ... */
    WCHAR id[128];           /* its folder name */
    char name[128];          /* manifest name= (UTF-8) */
    char version[32];        /* manifest version= */
    WCHAR folder[MAX_PATH];  /* <game>\Mods\<type>\<id>; "" if it isn't installed any more */
    uint64_t size;           /* its files' bytes */
    char hash[65];           /* report_mod_hash; "" if not installed */
    int on_server;           /* the hash check said the server has it */
    int send;                /* to upload */
} ReportMod;

/* The newest recorded matches (up to max), newest first. */
int report_match_list(const WCHAR *game_dir, ReportMatch *out, int max);

/* A match folder's picker line (people, arena, rule, when, length; "(crashed)" / "(unfinished)"). */
void report_match_label(const WCHAR *match_dir, WCHAR *out, int n);

/* The mods a match used (its mods.txt), each resolved to its installed folder and hashed. */
int report_match_mods(const WCHAR *game_dir, const WCHAR *match_dir, ReportMod *out, int max);

/* A mod folder's content hash (lower-case hex SHA-256): every file but the game's and launcher's dot files and
   "disabled", sorted by relative path (lower-case, '/'), each as path (UTF-8), 0, size (8 bytes little-endian),
   bytes. 1 on success (size: the files' bytes). */
int report_mod_hash(const WCHAR *folder, char hex[65], uint64_t *size);

/* Every file under dir (the same files as the hash) into a stored zip at zip_path, paths relative to dir under
   `prefix` ("" or "match/<name>/"). 1 on success. */
int report_zip_dir(const WCHAR *dir, const WCHAR *zip_path, const char *prefix);

/* Asks the server which of the mods it lacks (POST <server>/api/reports/mods/check): sets on_server, and
   *max_upload (bytes per report, 0 if not said). 0 if the server can't be asked (mods then all count as missing). */
int report_mods_check(const WCHAR *server, ReportMod *mods, int n, uint64_t *max_upload, WCHAR *why, int why_n);

#ifdef __cplusplus
}
#endif
