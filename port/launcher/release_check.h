/* Checks an install against its release's file list (release_files.txt, written by tools/package.ps1:
 * "SvR2011 release <version>", then "<size>\t<sha256>\t<path>" per file). */
#pragma once
#include <windows.h>

typedef struct ReleaseCheck {
    WCHAR version[32];       /* the list's release */
    int files;               /* files listed */
    int bad;                 /* missing, a different size or (with hashes) different bytes */
    WCHAR first_bad[MAX_PATH];
} ReleaseCheck;

/* Checks `dir` against `list` (a release_files.txt; NULL = dir\release_files.txt). `hashes`: compare the
 * bytes (SHA-256) too, else only that each file is there with its size. `each_bad` (may be NULL) is called
 * with every bad file's path relative to the folder. 0 when there is no readable list. */
int release_check(const WCHAR *dir, const WCHAR *list, int hashes, ReleaseCheck *out,
                  void (*each_bad)(const WCHAR *rel, void *ctx), void *ctx);
