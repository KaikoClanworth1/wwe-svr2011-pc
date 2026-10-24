/* WWE SmackDown vs. Raw 2011 - "Report a problem" sent to the online server (the launcher and the Mod Maker):
   POST <online_server>/api/reports, multipart/form-data -
     "report"  application/json: {"format":1, "app", "version", "platform", "os", "problem", "happens", "what",
               "before", "contact", "signature"} (report_json)
     "zip"     the report zip (report.c / the Mod Maker's), already without tokens, passwords and addresses
     "image0".."image4"  png / jpeg / webp pictures the player picked
   The answer: {"ok":true, "id", "signature", "seen_before":N} or {"ok":false, "error"}. No account token is sent.
   Plain C and WinHTTP only: the launcher and the Mod Maker build the same file. */
#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { REPORT_MAX_IMAGES = 5 };

typedef struct {
    const char *json;                              /* the "report" part (report_json), UTF-8 */
    const WCHAR *zip;                              /* the report zip, or NULL */
    const WCHAR *images[REPORT_MAX_IMAGES];        /* pictures, or NULL */
    int n_images;
} ReportUpload;

typedef struct {
    int status;              /* the HTTP status (0: no answer - offline, no server) */
    char id[64];             /* the report's id on the server */
    int seen_before;         /* earlier reports of the same problem (the server's signature) */
    WCHAR why[300];          /* when it failed: what to show */
} ReportResult;

/* The "report" part (malloc'd, the caller frees). Every text is JSON-escaped; NULL is "". */
char *report_json(const char *app, const char *version, const char *problem, const char *happens, const char *what,
                  const char *before, const char *contact, const char *signature);

/* A hint for the server's duplicate check: the first line of the newest crash report in <game>\crashes, else the
   newest log's last error line; "" if none (UTF-8). */
void report_signature(const WCHAR *game_dir, char *out, int n);

/* A picture the server takes: png / jpeg / webp by its first bytes, 5 MB at most. 0 with why if not. */
int report_image_ok(const WCHAR *path, WCHAR *why, int why_n);

/* One line saying what will be sent (shown before Send). */
void report_send_summary(const ReportUpload *u, WCHAR *out, int n);

/* Sends it to <server>/api/reports (server: online_server, e.g. https://sho-ti.me/svr). 1 if the server took it
   (r->id, r->seen_before), else 0 with r->why. Blocks: call it from a worker thread. */
int report_send(const WCHAR *server, const ReportUpload *u, ReportResult *r);

#ifdef __cplusplus
}
#endif
