/* WWE SmackDown vs. Raw 2011 launcher - "Report a problem": one zip to send
   (Discord, GitHub) with what a problem report needs - the newest game logs,
   crash reports and svr2011.toml (the account's token and password removed),
   and a short report.txt (launcher version, Windows version). */
#pragma once

#include <windows.h>

/* Writes <game>\Reports\SvR2011-report-<date>.zip (its path in out).
   logs / crashes: how many were put in. Returns 1, or 0 if it couldn't be written. */
int report_make(const WCHAR *game_dir, const WCHAR *version, WCHAR *out, int outn, int *logs, int *crashes);
