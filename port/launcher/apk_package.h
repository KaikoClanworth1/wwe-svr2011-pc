/* WWE SmackDown vs. Raw 2011 launcher - "Create APK Package": the Android app
   and a zip of the installed game, for copying to a phone. */
#pragma once

#include <windows.h>

/* Called from the packaging thread: progress 0-1000 and a status line (or NULL). */
typedef void (*apk_progress_fn)(int permille, const WCHAR *msg);

/* The APK the release ships (<dir>\Android\SvR2011.apk), or 0. */
int apk_find(const WCHAR *game_dir, const WCHAR *launcher_dir, WCHAR *out, int outn);

/* Writes out_dir\SvR2011.apk, out_dir\SvR2011-Game.zip (the game folder
   without its Windows programs, logs and caches) and a how-to text file.
   Returns 1, or 0 with err filled (cancelled: err is "Cancelled."). */
int apk_package(const WCHAR *game_dir, const WCHAR *apk, const WCHAR *out_dir,
                volatile LONG *cancel, apk_progress_fn progress, WCHAR *err, int errn);
