/* Updates from the port's GitHub releases (updater.c). */
#pragma once
#include <windows.h>
#include <stdint.h>

#define UPDATE_REPO L"KaikoClanworth1/wwe-svr2011-pc"

typedef struct UpdateInfo {
    WCHAR version[32];         /* the newest release, "0.2.0" (tag without the v) */
    WCHAR url[1024];           /* its SvR2011-PC-*.zip */
    WCHAR page[512];           /* its release page */
    uint64_t size;             /* the zip's size in bytes */
    int prerelease;
} UpdateInfo;

/* Asks GitHub for the newest release (pre-releases included). 1 on success. */
int update_check(UpdateInfo *out, WCHAR *err, size_t errn);

/* The release of `version` ("2.0.4": tag v2.0.4), as update_check. */
int update_check_version(const WCHAR *version, UpdateInfo *out, WCHAR *err, size_t errn);

/* a > b as dotted versions ("0.10.0" > "0.9.1"). */
int version_newer(const WCHAR *a, const WCHAR *b);

/* Downloads `url` to `path`, posting `msg` (wParam = percent) to `notify`. */
int update_download(const WCHAR *url, const WCHAR *path, HWND notify, UINT msg, volatile LONG *cancel,
                    WCHAR *err, size_t errn);
