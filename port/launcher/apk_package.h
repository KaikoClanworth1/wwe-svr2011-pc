/* WWE SmackDown vs. Raw 2011 launcher - the Android app: "Create APK Package"
   (the app and a zip of the installed game, for copying to a phone) and
   "Install to phone" (over USB with Google's adb). */
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

/* Google's platform tools (adb), fetched on request into <launcher>\platform-tools. */
#define ADB_TOOLS_URL L"https://dl.google.com/android/repository/platform-tools-latest-windows.zip"

/* adb.exe: beside the launcher (platform-tools), on PATH or in the Android
   SDK's usual place. 1 if found. */
int adb_find(const WCHAR *launcher_dir, WCHAR *out, int outn);

/* Installs the app and the game folder on the phone connected with USB
   debugging: games/WWE SmackDown vs. Raw 2011, only what changed, and the
   phone's saves are kept once it has the game. Returns 1, or 0 with err. */
int adb_install(const WCHAR *adb, const WCHAR *game_dir, const WCHAR *apk,
                volatile LONG *cancel, apk_progress_fn progress, WCHAR *err, int errn);
