/* WWE SmackDown vs. Raw 2011 PC launcher - the Supporters tab (supporters_tab.c).
   The people who support the project, read from the online server (GET /api/supporters: name, platform, since -
   server/supporters.py, from Ko-fi, Patreon and names added by hand), with the Patreon / Ko-fi / PayPal links.
   Read-only, no sign-in. Supporting is optional and unlocks nothing. */
#pragma once

#include <windows.h>
#include <stdio.h>

#include "mods_tab.h" /* (mods_add_fn: the launcher's control helper) */
#include "reports_tab.h" /* (reports_server_fn) */

/* refuses: a test run's guard (test_run_refuses_url); the three links' addresses. */
void supporters_build(HWND wnd, mods_add_fn add, int tab, int id_base, reports_server_fn server,
                      int (*refuses)(const WCHAR *server, const char *what), const WCHAR *patreon,
                      const WCHAR *kofi, const WCHAR *paypal);
void supporters_show(void);
int supporters_command(int id, int code);
/* Its thread's answer (WM_APP + 23); 1 when it was its. */
#define WM_APP_SUPPORTERS (WM_APP + 23)
int supporters_message(UINT m, WPARAM wp, LPARAM lp);

/* Tests (--capture supporters): 1 while it asks; how many it lists and the status line; the list into f. */
int supporters_test_state(int *n, WCHAR *line, size_t line_n);
void supporters_test_dump(FILE *f);
