/* WWE SmackDown vs. Raw 2011 PC launcher - the Roster tab.
 *
 * The game's list menus (Superstar Threads and the like: X opens Sort
 * Category) sort characters into SMACKDOWN, RAW, NPC and MODS
 * (src/sort_filters.cpp). <game>\UserData\sort_tags.txt moves a character
 * into one of them: lines "<id> = SMACKDOWN | RAW | NPC | MODS", # comments.
 * This tab lists the characters (the game's, from roster_names.inc, and the
 * superstar mods: Mods\Superstars\slots.txt + their manifest names) with
 * their category, and rewrites the file as they change - its comments and
 * the lines of characters it doesn't know are kept.
 */
#include "roster_tab.h"

#include <commctrl.h>
#include <ctype.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

enum { R_SEARCH, R_LIST, R_SMACKDOWN, R_RAW, R_NPC, R_MODS, R_DEFAULT, R_STATUS, R_OPEN, R_COUNT };
enum { CAT_NONE, CAT_SMACKDOWN, CAT_RAW, CAT_NPC, CAT_MODS };
static const WCHAR *const k_cat_names[] = { L"", L"SMACKDOWN", L"RAW", L"NPC", L"MODS" };

/* The game's characters: { id, name, a note ("" if none) }. */
static const struct { int id; const WCHAR *name; const WCHAR *brand; } k_roster[] = {
#include "roster_names.inc"
};
#define ROSTER_N ((int)(sizeof k_roster / sizeof k_roster[0]))

enum { CHARS_MAX = 512 };
typedef struct {
    int id;
    WCHAR name[96];
    WCHAR brand[48];  /* a note: DLC, Diva, Not selectable, Superstar mod */
    int mod;          /* a superstar mod */
    int tag;          /* CAT_ */
} Char;
static Char s_chars[CHARS_MAX];
static int s_n;

static int s_base;
static HWND s_wnd, s_list, s_search, s_status;
static WCHAR s_game[MAX_PATH];

static void status(const WCHAR *t) { SetWindowTextW(s_status, t); }

static void tags_path(WCHAR *out) { swprintf_s(out, MAX_PATH, L"%s\\UserData\\sort_tags.txt", s_game); }

static Char *find(int id)
{
    int i;
    for (i = 0; i < s_n; i++)
        if (s_chars[i].id == id) return &s_chars[i];
    return NULL;
}

static Char *add_char(int id, const WCHAR *name, const WCHAR *brand, int mod)
{
    Char *c = find(id);
    if (!c) {
        if (s_n >= CHARS_MAX) return NULL;
        c = &s_chars[s_n++];
        ZeroMemory(c, sizeof *c);
        c->id = id;
    }
    if (name && name[0]) wcsncpy_s(c->name, 96, name, _TRUNCATE);
    if (brand) wcsncpy_s(c->brand, 48, brand, _TRUNCATE);
    c->mod |= mod;
    return c;
}

static int cat_of(const char *v)
{
    char t[32];
    size_t n = 0;
    for (; *v && n < sizeof t - 1; v++)
        if (*v != ' ' && *v != '\t' && *v != '\r' && *v != '\n') t[n++] = (char)toupper((unsigned char)*v);
    t[n] = 0;
    return !strcmp(t, "SMACKDOWN") ? CAT_SMACKDOWN : !strcmp(t, "RAW") ? CAT_RAW : !strcmp(t, "NPC") ? CAT_NPC
         : !strcmp(t, "MODS")      ? CAT_MODS      : CAT_NONE;
}

/* sort_tags.txt as the game reads it: the tag of each id. */
static void read_tags(void)
{
    WCHAR p[MAX_PATH];
    FILE *f = NULL;
    char line[512];
    tags_path(p);
    if (_wfopen_s(&f, p, L"rb") || !f) return;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#'), *eq;
        int id, cat;
        if (hash) *hash = 0;
        eq = strchr(line, '=');
        if (!eq) continue;
        id = atoi(line);
        cat = cat_of(eq + 1);
        if (id > 0 && cat != CAT_NONE) {
            Char *c = add_char(id, NULL, NULL, 0);
            if (c) c->tag = cat;
        }
    }
    fclose(f);
}

/* The superstar mods: Mods\Superstars\slots.txt ("<folder>\t<id>") and
   their manifest.txt names. */
static void read_mods(void)
{
    WCHAR p[MAX_PATH];
    FILE *f = NULL;
    char line[512];
    swprintf_s(p, MAX_PATH, L"%s\\Mods\\Superstars\\slots.txt", s_game);
    if (_wfopen_s(&f, p, L"rb") || !f) return;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        WCHAR folder[MAX_PATH], name[96], mp[MAX_PATH];
        FILE *m = NULL;
        int id;
        if (!tab) continue;
        *tab = 0;
        id = atoi(tab + 1);
        if (id <= 0) continue;
        MultiByteToWideChar(CP_UTF8, 0, line, -1, folder, MAX_PATH);
        wcscpy_s(name, 96, folder);
        swprintf_s(mp, MAX_PATH, L"%s\\Mods\\Superstars\\%s\\manifest.txt", s_game, folder);
        if (!_wfopen_s(&m, mp, L"rb") && m) {
            char ml[512];
            while (fgets(ml, sizeof ml, m)) {
                if (!strncmp(ml, "name=", 5)) {
                    size_t n = strlen(ml);
                    while (n && (ml[n - 1] == '\r' || ml[n - 1] == '\n')) ml[--n] = 0;
                    MultiByteToWideChar(CP_UTF8, 0, ml + 5, -1, name, 96);
                    break;
                }
            }
            fclose(m);
        }
        add_char(id, name, L"Superstar mod", 1);
    }
    fclose(f);
}

static void load(void)
{
    int i;
    s_n = 0;
    for (i = 0; i < ROSTER_N; i++) add_char(k_roster[i].id, k_roster[i].name, k_roster[i].brand, 0);
    if (!s_game[0]) return;
    read_mods();
    read_tags();
    for (i = 0; i < s_n; i++)  /* (tagged ids we have no name for) */
        if (!s_chars[i].name[0]) swprintf_s(s_chars[i].name, 96, L"Character %d", s_chars[i].id);
}

static int cmp_name(const void *a, const void *b)
{
    return _wcsicmp(((const Char *)a)->name, ((const Char *)b)->name);
}

static void fill(void)
{
    WCHAR filter[64], low_name[96], t[48];
    int i, row = 0, tagged = 0;
    GetWindowTextW(s_search, filter, 64);
    _wcslwr_s(filter, 64);
    SendMessageW(s_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(s_list);
    for (i = 0; i < s_n; i++) {
        const Char *c = &s_chars[i];
        LVITEMW it;
        if (c->tag) tagged++;
        wcscpy_s(low_name, 96, c->name);
        _wcslwr_s(low_name, 96);
        if (filter[0] && !wcsstr(low_name, filter)) continue;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = row;
        it.pszText = (WCHAR *)c->name;
        it.lParam = c->id;
        ListView_InsertItem(s_list, &it);
        swprintf_s(t, 48, L"%d", c->id);
        ListView_SetItemText(s_list, row, 1, t);
        if (c->tag) swprintf_s(t, 48, L"%s", k_cat_names[c->tag]);
        else swprintf_s(t, 48, L"\x2014");  /* (its own category) */
        ListView_SetItemText(s_list, row, 2, t);
        ListView_SetItemText(s_list, row, 3, (WCHAR *)c->brand);
        row++;
    }
    SendMessageW(s_list, WM_SETREDRAW, TRUE, 0);
    if (!s_game[0] || GetFileAttributesW(s_game) == INVALID_FILE_ATTRIBUTES) {
        status(L"Install the game first (Install tab).");
    } else {
        WCHAR m[200];
        swprintf_s(m, 200, L"%d character%s re-sorted. Select characters (Ctrl / Shift for several), then a category; "
                           L"the game uses it from its next start.", tagged, tagged == 1 ? L"" : L"s");
        status(m);
    }
}

void roster_show(const WCHAR *game_dir)
{
    wcsncpy_s(s_game, MAX_PATH, game_dir ? game_dir : L"", _TRUNCATE);
    load();
    qsort(s_chars, (size_t)s_n, sizeof s_chars[0], cmp_name);
    fill();
}

/* sort_tags.txt rewritten: its comments and blank lines as they were, a tag
   line kept (with its comment) or changed or dropped, new tags at the end. */
static int write_tags(void)
{
    WCHAR p[MAX_PATH], tmp[MAX_PATH], dir[MAX_PATH];
    char *old = NULL, *line, *next;
    size_t old_n = 0;
    FILE *f = NULL;
    static unsigned char written[CHARS_MAX];
    int i;
    tags_path(p);
    swprintf_s(dir, MAX_PATH, L"%s\\UserData", s_game);
    CreateDirectoryW(dir, NULL);
    if (!_wfopen_s(&f, p, L"rb") && f) {
        fseek(f, 0, SEEK_END);
        old_n = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        old = (char *)calloc(old_n + 1, 1);
        if (old) old_n = fread(old, 1, old_n, f);
        fclose(f);
        f = NULL;
    }
    swprintf_s(tmp, MAX_PATH, L"%s.tmp", p);
    if (_wfopen_s(&f, tmp, L"wb") || !f) {
        free(old);
        return 0;
    }
    memset(written, 0, sizeof written);
    if (!old || !old_n)
        fputs("# Sort categories of the list menus (Superstar Threads and the like: X opens Sort Category).\r\n"
              "# <character id> = SMACKDOWN | RAW | NPC | MODS - the launcher's Roster tab edits this.\r\n", f);
    for (line = old; line && *line; line = next) {
        char body[512], *hash, *eq;
        size_t n;
        int id, cat_old;
        Char *c;
        next = strchr(line, '\n');
        n = next ? (size_t)(next - line) : strlen(line);
        if (next) next++;
        else next = line + n;
        if (n >= sizeof body) n = sizeof body - 1;
        memcpy(body, line, n);
        body[n] = 0;
        if (n && body[n - 1] == '\r') body[--n] = 0;
        hash = strchr(body, '#');
        eq = strchr(body, '=');
        id = eq && (!hash || eq < hash) ? atoi(body) : 0;
        c = id > 0 ? find(id) : NULL;
        if (!c) {  /* (a comment, a blank line, or a character we don't list) */
            fprintf(f, "%s\r\n", body);
            continue;
        }
        cat_old = cat_of(eq + 1);
        (void)cat_old;
        if (written[c - s_chars] || !c->tag) continue;  /* (dropped: back to the default) */
        written[c - s_chars] = 1;
        {
            char name[96];
            WideCharToMultiByte(CP_UTF8, 0, k_cat_names[c->tag], -1, name, 96, NULL, NULL);
            fprintf(f, "%d = %s%s%s\r\n", id, name, hash ? "  " : "", hash ? hash : "");
        }
    }
    for (i = 0; i < s_n; i++) {
        char name[96], cn[96];
        if (!s_chars[i].tag || written[i]) continue;
        WideCharToMultiByte(CP_UTF8, 0, k_cat_names[s_chars[i].tag], -1, cn, 96, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, s_chars[i].name, -1, name, 96, NULL, NULL);
        fprintf(f, "%d = %s  # %s\r\n", s_chars[i].id, cn, name);
    }
    fclose(f);
    free(old);
    return MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING) != 0;
}

static int set_category(int cat)
{
    int i = -1, count = 0;
    WCHAR m[200];
    while ((i = ListView_GetNextItem(s_list, i, LVNI_SELECTED)) >= 0) {
        LVITEMW it;
        Char *c;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_PARAM;
        it.iItem = i;
        ListView_GetItem(s_list, &it);
        c = find((int)it.lParam);
        if (c) {
            c->tag = cat;
            count++;
        }
    }
    if (!count) {
        status(L"Select the characters first.");
        return 0;
    }
    if (!write_tags()) {
        status(L"Could not write UserData\\sort_tags.txt (is the game folder read-only?).");
        return 0;
    }
    fill();
    swprintf_s(m, 200, cat ? L"%d character%s moved to %s (in the game from its next start)."
                           : L"%d character%s back in %s their default category (from the game's next start).",
               count, count == 1 ? L"" : L"s", cat ? k_cat_names[cat] : L"");
    status(m);
    return 1;
}

int roster_tag(const WCHAR *game_dir, int id, const WCHAR *category)
{
    Char *c;
    char cat[32];
    roster_show(game_dir);
    c = find(id);
    if (!c) c = add_char(id, NULL, NULL, 0);
    if (!c) return 0;
    if (!c->name[0]) swprintf_s(c->name, 96, L"Character %d", id);
    WideCharToMultiByte(CP_UTF8, 0, category, -1, cat, 32, NULL, NULL);
    c->tag = cat_of(cat);
    if (!write_tags()) return 0;
    fill();
    return 1;
}

void roster_build(HWND wnd, mods_add_fn add, int tab, int id_base)
{
    static const WCHAR *const names[] = { L"Character", L"ID", L"Category", L"Note" };
    static const int widths[] = { 190, 48, 100, 152 };
    HDC dc;
    int i, dpi;
    s_wnd = wnd;
    s_base = id_base;
    dc = GetDC(wnd);
    dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(wnd, dc);
    add(tab, L"Static",
        L"The list menus' sort categories (Superstar Threads and the like: X opens Sort Category). Move a character "
        L"into SMACKDOWN, RAW, NPC or MODS.",
        SS_LEFT, 28, 10, 560, 40, 0);
    add(tab, L"Button", L"Characters", BS_GROUPBOX, 28, 60, 560, 404, 0);
    add(tab, L"Static", L"Search", SS_LEFT, 44, 96, 60, 22, 0);
    s_search = add(tab, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 104, 92, 200, 26, id_base + R_SEARCH);
    s_list = add(tab, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 44, 128, 400, 320,
                 id_base + R_LIST);
    ListView_SetExtendedListViewStyle(s_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    for (i = 0; i < 4; i++) {
        LVCOLUMNW c;
        ZeroMemory(&c, sizeof c);
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = i == 1 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.cx = MulDiv(widths[i] * 400 / 490, dpi, 96);
        c.pszText = (WCHAR *)names[i];
        ListView_InsertColumn(s_list, i, &c);
    }
    add(tab, L"Static", L"Move to", SS_LEFT, 456, 128, 118, 20, 0);
    add(tab, L"Button", L"SMACKDOWN", BS_PUSHBUTTON | WS_TABSTOP, 456, 150, 118, 32, id_base + R_SMACKDOWN);
    add(tab, L"Button", L"RAW", BS_PUSHBUTTON | WS_TABSTOP, 456, 188, 118, 32, id_base + R_RAW);
    add(tab, L"Button", L"NPC", BS_PUSHBUTTON | WS_TABSTOP, 456, 226, 118, 32, id_base + R_NPC);
    add(tab, L"Button", L"MODS", BS_PUSHBUTTON | WS_TABSTOP, 456, 264, 118, 32, id_base + R_MODS);
    add(tab, L"Button", L"Default", BS_PUSHBUTTON | WS_TABSTOP, 456, 316, 118, 32, id_base + R_DEFAULT);
    add(tab, L"Button", L"Open the file", BS_PUSHBUTTON | WS_TABSTOP, 456, 416, 118, 32, id_base + R_OPEN);
    s_status = add(tab, L"Static", L"", SS_LEFT | SS_NOPREFIX, 28, 476, 560, 40, id_base + R_STATUS);
}

int roster_command(int id, int code)
{
    if (id == s_base + R_SEARCH && code == EN_CHANGE) { fill(); return 1; }
    if (id == s_base + R_SMACKDOWN) { set_category(CAT_SMACKDOWN); return 1; }
    if (id == s_base + R_RAW) { set_category(CAT_RAW); return 1; }
    if (id == s_base + R_NPC) { set_category(CAT_NPC); return 1; }
    if (id == s_base + R_MODS) { set_category(CAT_MODS); return 1; }
    if (id == s_base + R_DEFAULT) { set_category(CAT_NONE); return 1; }
    if (id == s_base + R_OPEN) {
        WCHAR p[MAX_PATH];
        if (!s_game[0]) return 1;
        tags_path(p);
        if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) write_tags();
        ShellExecuteW(s_wnd, L"open", L"notepad.exe", p, NULL, SW_SHOWNORMAL);
        return 1;
    }
    return 0;
}
