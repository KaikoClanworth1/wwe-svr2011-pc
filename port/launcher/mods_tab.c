/* WWE SmackDown vs. Raw 2011 PC launcher - the Mods tab (arenas branch).
 *
 * Mods live in <game>/Mods/<Type>/<id>/ with a manifest.txt (key=value:
 * type, id, name, author, version). Arena mods hold arena.pac and banner.dds
 * (256 x 128 DXT5); the game lists them on the arena select pages after its
 * own arenas (src/arena_mods.cpp). Superstar mods hold ch.pac (and maybe a
 * theme song and an entrance movie); the game lists them under the M tile of
 * the character select (src/superstar_mods.cpp). Sign packs hold crowd signs
 * (*.dds, 128 x 64) the crowd holds up (src/crowd_signs.cpp); media packs replace
 * the game's videos, renders, arena screens and sounds (src/media_mods.cpp). Match type mods
 * (Mods/MatchTypes/<id>: a manifest only) switch on one of the match types the port adds -
 * Slobber Knocker, Three Stages of Hell, Elimination... (src/match_types.cpp); they come
 * switched on. Background mods (Mods/Backgrounds/<id>: picture.dds + manifest) are main menu
 * pictures (or a pack of them, NN_<name>.dds, from the Mod Maker); "Add background picture"
 * makes one from a PNG / JPG (backgrounds.c, src/backgrounds_page.cpp). A background's name
 * (what the game's BACKGROUNDS page shows) can be changed in the list (click it, or F2). A mod is turned off by a "disabled" file in its folder. A .svrmod file is a
 * zip of such a folder.
 *   +  installs a .svrmod/.zip   -  sends a mod's folder to the Recycle Bin
 */
#include "mods_tab.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <restartmanager.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "backgrounds.h"
#include "movie_maker.h"
#include "unzip.h"

enum { MODS_MAX = 512 };

typedef struct {
    WCHAR folder[MAX_PATH];  /* full path */
    WCHAR type[32], id[64], name[128], author[64], version[32];
    WCHAR made_with[64];  /* manifest made_with= (the Mod Maker stamps its mods); "" = not made with it */
    unsigned long long bytes;
    int enabled;
    int left_out;  /* a superstar mod over the game's ids (s_star_ids): not in the game (superstar_limit) */
} Mod;

static Mod s_mods[MODS_MAX];
static int s_nmods;
static int s_base;                 /* first control id */
static HWND s_list, s_status, s_wnd;
static WCHAR s_game[MAX_PATH];
static int s_filling;              /* (checkbox notifications while filling the list) */

enum { M_LIST, M_ADD, M_REMOVE, M_FOLDER, M_MAKER, M_STATUS, M_BACKGROUND, M_ON, M_OFF };

static void status(const WCHAR *t) { SetWindowTextW(s_status, t); }

/* The game has 137 ids for superstar mods (src/superstar_mods.cpp kPool; it
   writes the number to Mods\Superstars\limit.txt at each start), kept
   per mod folder in Mods\Superstars\slots.txt. As the game does: a mod that's
   on and holds an id keeps it; the ones without wait, in folder order, for
   the ids left (those of mods switched off or gone included). Marks the
   waiting ones that don't fit; returns how many superstar mods are on. */
enum { SUPERSTAR_IDS = 137 };  /* (until the game has written limit.txt: a fresh install) */
static int s_star_ids = SUPERSTAR_IDS;
static WCHAR s_over_label[64];
static int s_stars_on, s_stars_out;
static int folder_cmp(const void *a, const void *b)
{
    const Mod *x = *(const Mod *const *)a, *y = *(const Mod *const *)b;
    return wcscmp(wcsrchr(x->folder, L'\\') + 1, wcsrchr(y->folder, L'\\') + 1);
}
static void superstar_limit(void)
{
    WCHAR path[MAX_PATH];
    char line[1024];
    static char held[MODS_MAX * 2][MAX_PATH];
    int nheld = 0, holders = 0, i, room;
    Mod *waiting[MODS_MAX];
    int nwait = 0;
    FILE *f = NULL;
    s_stars_on = s_stars_out = 0;
    s_star_ids = SUPERSTAR_IDS;
    swprintf_s(path, MAX_PATH, L"%s\\Mods\\Superstars\\limit.txt", s_game);
    if (!_wfopen_s(&f, path, L"rb") && f) {
        int n = 0;
        if (fscanf_s(f, "%d", &n) == 1 && n > 0 && n < 1000) s_star_ids = n;
        fclose(f);
        f = NULL;
    }
    swprintf_s(s_over_label, 64, L"superstar - NOT IN GAME (over the %d limit)", s_star_ids);
    swprintf_s(path, MAX_PATH, L"%s\\Mods\\Superstars\\slots.txt", s_game);
    if (!_wfopen_s(&f, path, L"rb") && f) {
        while (nheld < MODS_MAX * 2 && fgets(line, sizeof line, f)) {
            char *tab = strrchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            strncpy_s(held[nheld++], MAX_PATH, line, _TRUNCATE);
        }
        fclose(f);
    }
    for (i = 0; i < s_nmods; i++) {
        Mod *m = &s_mods[i];
        char acp[MAX_PATH * 2], utf8[MAX_PATH * 3];
        int k, has = 0;
        m->left_out = 0;
        if (_wcsicmp(m->type, L"superstar") || !m->enabled) continue;
        s_stars_on++;
        WideCharToMultiByte(CP_ACP, 0, wcsrchr(m->folder, L'\\') + 1, -1, acp, sizeof acp, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, wcsrchr(m->folder, L'\\') + 1, -1, utf8, sizeof utf8, NULL, NULL);
        for (k = 0; k < nheld && !has; k++) has = !strcmp(held[k], acp) || !strcmp(held[k], utf8);
        if (has) holders++;
        else waiting[nwait++] = m;
    }
    qsort(waiting, nwait, sizeof *waiting, folder_cmp);
    room = s_star_ids - holders;
    for (i = 0; i < nwait; i++)
        if (i >= room) waiting[i]->left_out = 1, s_stars_out++;
}

static void trim(WCHAR *s)
{
    size_t n = wcslen(s);
    while (n && (s[n - 1] == L'\r' || s[n - 1] == L'\n' || s[n - 1] == L' ')) s[--n] = 0;
}

/* manifest.txt (UTF-8) -> fields. 1 if it names a type and an id. */
static int read_manifest(const WCHAR *path, Mod *m)
{
    FILE *f = NULL;
    char line[512];
    if (_wfopen_s(&f, path, L"rb") || !f) return 0;
    while (fgets(line, sizeof line, f)) {
        WCHAR w[512], *eq;
        if (!MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 512)) continue;
        trim(w);
        eq = wcschr(w, L'=');
        if (!eq) continue;
        *eq++ = 0;
        if (!wcscmp(w, L"type")) wcsncpy_s(m->type, 32, eq, _TRUNCATE);
        else if (!wcscmp(w, L"id")) wcsncpy_s(m->id, 64, eq, _TRUNCATE);
        else if (!wcscmp(w, L"name")) wcsncpy_s(m->name, 128, eq, _TRUNCATE);
        else if (!wcscmp(w, L"author")) wcsncpy_s(m->author, 64, eq, _TRUNCATE);
        else if (!wcscmp(w, L"version")) wcsncpy_s(m->version, 32, eq, _TRUNCATE);
        else if (!wcscmp(w, L"made_with")) wcsncpy_s(m->made_with, 64, eq, _TRUNCATE);
    }
    fclose(f);
    return m->type[0] && m->id[0];
}

static unsigned long long folder_bytes(const WCHAR *dir)
{
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    unsigned long long n = 0;
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            n += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static int exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static int maker_present(void);

static void scan(void)
{
    static const WCHAR *const types[] = { L"Arenas", L"Superstars", L"Signs", L"Media", L"Backstage", L"Moves", L"MatchTypes",
                                         L"Backgrounds", L"Attires" };
    int t;
    s_nmods = 0;
    if (!s_game[0]) return;
    for (t = 0; t < (int)(sizeof types / sizeof types[0]); t++) {
        WCHAR pat[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        swprintf_s(pat, MAX_PATH, L"%s\\Mods\\%s\\*", s_game, types[t]);
        h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            Mod *m;
            WCHAR man[MAX_PATH], off[MAX_PATH];
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || s_nmods >= MODS_MAX)
                continue;
            m = &s_mods[s_nmods];
            ZeroMemory(m, sizeof *m);
            swprintf_s(m->folder, MAX_PATH, L"%s\\Mods\\%s\\%s", s_game, types[t], fd.cFileName);
            swprintf_s(man, MAX_PATH, L"%s\\manifest.txt", m->folder);
            if (!read_manifest(man, m)) {  /* a folder without a manifest (older test mods: info.txt) */
                swprintf_s(man, MAX_PATH, L"%s\\info.txt", m->folder);
                read_manifest(man, m);
                if (!m->type[0]) wcsncpy_s(m->type, 32, L"arena", _TRUNCATE);
                if (!m->id[0]) wcsncpy_s(m->id, 64, fd.cFileName, _TRUNCATE);
            }
            if (!m->name[0]) wcsncpy_s(m->name, 128, m->id, _TRUNCATE);
            swprintf_s(off, MAX_PATH, L"%s\\disabled", m->folder);
            m->enabled = !exists(off);
            m->bytes = folder_bytes(m->folder);
            s_nmods++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static void fill(void)
{
    int i;
    s_filling = 1;
    superstar_limit();
    ListView_DeleteAllItems(s_list);
    for (i = 0; i < s_nmods; i++) {
        LVITEMW it;
        WCHAR size[32];
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = s_mods[i].name;
        it.lParam = i;
        ListView_InsertItem(s_list, &it);
        ListView_SetItemText(s_list, i, 1, s_mods[i].left_out ? s_over_label : s_mods[i].type);
        ListView_SetItemText(s_list, i, 2, s_mods[i].version);
        ListView_SetItemText(s_list, i, 3, s_mods[i].author);
        swprintf_s(size, 32, L"%.1f MB", s_mods[i].bytes / 1048576.0);
        ListView_SetItemText(s_list, i, 4, size);
        /* made with the Mod Maker (its stamp), or not - a flag, not a warning */
        ListView_SetItemText(s_list, i, 5, s_mods[i].made_with[0] ? s_mods[i].made_with : L"non-Mod Maker");
        ListView_SetCheckState(s_list, i, s_mods[i].enabled);
    }
    s_filling = 0;
    {
        WCHAR t[160];
        if (!s_game[0]) status(L"Install the game first (Install tab).");
        else if (!s_nmods) status(maker_present() ? L"No mods yet. + adds a .svrmod file; Open Mod Maker makes one."
                                                  : L"No mods yet. + adds a .svrmod file.");
        else if (s_stars_out) {
            swprintf_s(t, 160, L"%d of %d superstar mods fit - %d aren't in the game (%d is the limit). Switch some "
                               L"off to choose which play.", s_stars_on - s_stars_out, s_stars_on, s_stars_out, s_star_ids);
            status(t);
        } else {
            swprintf_s(t, 160, L"%d mod%s. They load the next time the game starts; your Game Files are never "
                               L"changed.", s_nmods, s_nmods == 1 ? L"" : L"s");
            status(t);
        }
    }
}

void mods_show(const WCHAR *game_dir)
{
    wcsncpy_s(s_game, MAX_PATH, game_dir ? game_dir : L"", _TRUNCATE);
    scan();
    fill();
}

/* Removes a folder tree (Recycle Bin when `recycle`). */
static int remove_tree(const WCHAR *dir, int recycle)
{
    WCHAR from[MAX_PATH + 2] = { 0 };
    SHFILEOPSTRUCTW op;
    wcsncpy_s(from, MAX_PATH, dir, _TRUNCATE);  /* (double NUL terminated) */
    ZeroMemory(&op, sizeof op);
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | (recycle ? FOF_ALLOWUNDO : 0);
    return SHFileOperationW(&op) == 0;
}

/* A removed superstar mod gives its id back: its line ("<folder>\t<id>") leaves
   Mods\Superstars\slots.txt. (The game keeps a mod's id there so saves stay
   with it; a line left behind held one of its 72 ids for good, and new mods
   no longer showed once they were all taken.) The game writes the folder in
   the ANSI code page; both that and UTF-8 match. */
static void free_superstar_slot(const WCHAR *game, const WCHAR *folder)
{
    WCHAR path[MAX_PATH];
    const WCHAR *name = wcsrchr(folder, L'\\');
    char acp[MAX_PATH * 2], utf8[MAX_PATH * 3], line[1024];
    char *out = NULL;
    size_t used = 0, cap = 0;
    int dropped = 0;
    FILE *f = NULL;
    name = name ? name + 1 : folder;
    swprintf_s(path, MAX_PATH, L"%s\\Mods\\Superstars\\slots.txt", game);
    if (_wfopen_s(&f, path, L"rb") || !f) return;
    WideCharToMultiByte(CP_ACP, 0, name, -1, acp, sizeof acp, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof utf8, NULL, NULL);
    while (fgets(line, sizeof line, f)) {
        const char *tab = strchr(line, '\t');
        size_t n = strlen(line), k = tab ? (size_t)(tab - line) : 0;
        if (tab && ((strlen(acp) == k && !strncmp(line, acp, k)) || (strlen(utf8) == k && !strncmp(line, utf8, k)))) {
            dropped = 1;
            continue;
        }
        if (used + n + 1 > cap) {
            char *grown = (char *)realloc(out, cap = (used + n + 1) * 2);
            if (!grown) { free(out); fclose(f); return; }
            out = grown;
        }
        memcpy(out + used, line, n);
        used += n;
    }
    fclose(f);
    if (dropped && !_wfopen_s(&f, path, L"wb") && f) {
        if (used) fwrite(out, 1, used, f);
        fclose(f);
    }
    free(out);
}

/* ---- Two attire mods on one attire (Mods\Attires\<folder>: type=attire, superstar=<id>, attire=<1-4>|new).
   The rule the game, this tab, the phone's Mods page and the Mod Maker share: a superstar's attire is
   free when his own pac (pac\ch\ch<id>.pac, EMD entries "%06d%d2") doesn't have it and no switched-on
   attire mod asks for it. Of two switched-on mods asking for the same attire the one installed first
   keeps it (a mod just installed is the newer one); the other moves to the superstar's next free attire
   (1-4) - its manifest's attire= is rewritten, with a "# moved" line - or, with none free, it stays and
   the player is told to switch one off (the game then leaves it out: src/superstar_mods.cpp
   LoadAttireMods). Run when an attire mod is installed and when Play starts the game. */
static const struct { int id; const WCHAR *name; const WCHAR *note; } k_roster[] = {
#include "roster_names.inc"
};

static const WCHAR *roster_name(int id, WCHAR *buf, size_t n)
{
    int i;
    for (i = 0; i < (int)(sizeof k_roster / sizeof k_roster[0]); i++)
        if (k_roster[i].id == id) return k_roster[i].name;
    swprintf_s(buf, n, L"superstar %d", id);
    return buf;
}

/* Attires 1-4 (bits 0-3) the superstar's own pac has; *gear: it also has other model kinds for attire 1 (the
   entrance gear superstars - Undertaker, CM Punk, Cena, Shawn Michaels, Morrison: kind 4). A new attire for
   them doesn't load in the game yet (src/superstar_mods.cpp ExtraKinds): only their own can be replaced. */
static unsigned own_attires(const WCHAR *game, int id, int *gear)
{
    WCHAR path[MAX_PATH];
    unsigned char d[0x4000];
    unsigned mask = 0;
    char want[8];
    size_t p;
    FILE *f = NULL;
    swprintf_s(path, MAX_PATH, L"%s\\pac\\ch\\ch%03d.pac", game, id);
    *gear = 0;
    if (_wfopen_s(&f, path, L"rb") || !f) return 0;
    p = fread(d, 1, sizeof d, f);
    fclose(f);
    if (p != sizeof d || memcmp(d, "EPK8", 4)) return 0;
    sprintf_s(want, sizeof want, "%06d", id);
    for (p = 0x800; p + 12 <= sizeof d;) {
        unsigned k, entries;
        int emd;
        if (!memcmp(d + p, "\0\0\0\0", 4)) break;
        emd = !memcmp(d + p, "EMD ", 4);
        entries = (d[p + 4] | d[p + 5] << 8) / 4;
        p += 12;
        for (k = 0; k < entries && p + 16 <= sizeof d; k++, p += 16)
            if (emd && !memcmp(d + p, want, 6) && d[p + 6] >= '0' && d[p + 6] <= '3') {
                if (d[p + 7] == '2') mask |= 1u << (d[p + 6] - '0');
                else if (d[p + 6] == '0' && d[p + 7] >= '0' && d[p + 7] <= '9') *gear = 1;
            }
    }
    return mask;
}

typedef struct {
    WCHAR folder[MAX_PATH], name[128];
    int id, attire;            /* attire 1-4, 0 = new */
    unsigned long long born;   /* folder creation time: the order they were installed in */
    int fresh;                 /* the mod just installed */
} AttireMod;

static int attire_older(const AttireMod *a, const AttireMod *b)
{
    if (a->fresh != b->fresh) return b->fresh;
    if (a->born != b->born) return a->born < b->born;
    return wcscmp(a->folder, b->folder) < 0;
}

/* attire=<n> into the mod's manifest, the other lines kept, and a note why. */
static int set_attire(const WCHAR *folder, int from, int to, const WCHAR *keeper)
{
    WCHAR man[MAX_PATH], tmp[MAX_PATH];
    FILE *in = NULL, *out = NULL;
    char line[1024], k8[256];
    int done = 0;
    swprintf_s(man, MAX_PATH, L"%s\\manifest.txt", folder);
    swprintf_s(tmp, MAX_PATH, L"%s\\manifest.new", folder);
    if (_wfopen_s(&in, man, L"rb") || !in) return 0;
    if (_wfopen_s(&out, tmp, L"wb") || !out) { fclose(in); return 0; }
    WideCharToMultiByte(CP_UTF8, 0, keeper, -1, k8, sizeof k8, NULL, NULL);
    while (fgets(line, sizeof line, in)) {
        if (!strncmp(line, "attire=", 7) && !done) {
            fprintf(out, "attire=%d\n# moved from attire %d by the launcher (%s has attire %d)\n", to, from, k8, from);
            done = 1;
            continue;
        }
        fputs(line, out);
        if (line[0] && line[strlen(line) - 1] != '\n') fputs("\n", out);
    }
    fclose(in);
    fclose(out);
    return done && MoveFileExW(tmp, man, MOVEFILE_REPLACE_EXISTING);
}

/* -> moves + warnings made (report: one line each, may be NULL). */
int mods_attire_guard(const WCHAR *game, const WCHAR *fresh, WCHAR *report, size_t report_n)
{
    static AttireMod mods[MODS_MAX];
    int n = 0, i, j, said = 0;
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (report && report_n) report[0] = 0;
    swprintf_s(pat, MAX_PATH, L"%s\\Mods\\Attires\\*", game);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        WCHAR man[MAX_PATH], off[MAX_PATH];
        FILE *f = NULL;
        char line[512], model[MAX_PATH] = "attire.pac";
        WCHAR wmodel[MAX_PATH], mpath[MAX_PATH];
        int attire_mod = 0;
        AttireMod *a = &mods[n];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || n >= MODS_MAX) continue;
        ZeroMemory(a, sizeof *a);
        swprintf_s(a->folder, MAX_PATH, L"%s\\Mods\\Attires\\%s", game, fd.cFileName);
        swprintf_s(off, MAX_PATH, L"%s\\disabled", a->folder);
        if (exists(off)) continue;
        swprintf_s(man, MAX_PATH, L"%s\\manifest.txt", a->folder);
        if (_wfopen_s(&f, man, L"rb") || !f) continue;
        a->attire = -1;
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '='), *e;
            if (!eq || line[0] == '#') continue;
            *eq++ = 0;
            for (e = eq + strlen(eq); e > eq && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ');) *--e = 0;
            if (!strcmp(line, "type")) attire_mod = !strcmp(eq, "attire");
            else if (!strcmp(line, "superstar")) a->id = atoi(eq);
            else if (!strcmp(line, "attire")) a->attire = !strcmp(eq, "new") ? 0 : atoi(eq);
            else if (!strcmp(line, "name")) MultiByteToWideChar(CP_UTF8, 0, eq, -1, a->name, 128);
            else if (!strcmp(line, "model") && *eq) strncpy_s(model, MAX_PATH, eq, _TRUNCATE);
        }
        fclose(f);
        if (!attire_mod || a->id < 100 || a->id > 321 || a->attire < 0 || a->attire > 4) continue;
        /* no model: only an entrance for one of the superstar's own attires (Simple entrance porting) - it holds no
           attire, so it isn't moved (the game: the first folder A-Z keeps an attire's entrance) */
        MultiByteToWideChar(CP_UTF8, 0, model, -1, wmodel, MAX_PATH);
        swprintf_s(mpath, MAX_PATH, L"%s\\%s", a->folder, wmodel);
        if (!exists(mpath)) continue;
        if (!a->name[0]) wcsncpy_s(a->name, 128, fd.cFileName, _TRUNCATE);
        a->born = ((unsigned long long)fd.ftCreationTime.dwHighDateTime << 32) | fd.ftCreationTime.dwLowDateTime;
        a->fresh = fresh && !_wcsicmp(a->folder, fresh);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (i = 0; i < n; i++) {
        unsigned used, own;
        int k, news = 0, gear = 0;
        if (mods[i].id < 0) continue;  /* (a superstar already done) */
        own = used = own_attires(game, mods[i].id, &gear);
        /* the attires asked for, the oldest mod on each keeping it */
        for (j = 0; j < n; j++) {
            AttireMod *a = &mods[j];
            int best = 1;
            if (a->id != mods[i].id || !a->attire) continue;
            for (k = 0; k < n; k++)
                if (k != j && mods[k].id == a->id && mods[k].attire == a->attire && attire_older(&mods[k], a)) best = 0;
            if (best) used |= 1u << (a->attire - 1);
        }
        if (gear) used = 0xF;  /* (no new attires for them: nothing free to move to) */
        for (j = 0; j < n; j++) {
            AttireMod *a = &mods[j], *keeper = NULL;
            WCHAR who[48], t[512];
            int to = 0;
            if (a->id != mods[i].id) continue;
            if (!a->attire) { news++; continue; }
            if (gear && !(own >> (a->attire - 1) & 1)) {  /* (a new attire for a gear superstar: left out by the game) */
                swprintf_s(t, 512, L"\"%s\" gives %s an attire %d, but %s wears entrance gear and new attires for them "
                                   L"don't load in the game yet: the game leaves it out (replacing attire 1-%d works).\r\n",
                           a->name, roster_name(a->id, who, 48), a->attire, roster_name(a->id, who, 48),
                           own & 4 ? 3 : own & 2 ? 2 : 1);
                if (report) wcsncat_s(report, report_n, t, _TRUNCATE);
                said++;
                continue;
            }
            for (k = 0; k < n; k++)
                if (k != j && mods[k].id == a->id && mods[k].attire == a->attire && attire_older(&mods[k], a))
                    if (!keeper || attire_older(&mods[k], keeper)) keeper = &mods[k];
            if (!keeper) continue;
            for (k = 0; k < 4 && !to; k++)
                if (!(used >> k & 1)) to = k + 1;
            if (to && set_attire(a->folder, a->attire, to, keeper->name)) {
                used |= 1u << (to - 1);
                swprintf_s(t, 512, L"\"%s\" and \"%s\" both use %s's attire %d: \"%s\" moved to attire %d.\r\n",
                           keeper->name, a->name, roster_name(a->id, who, 48), a->attire, a->name, to);
                a->attire = to;
            } else if (gear)
                swprintf_s(t, 512, L"\"%s\" and \"%s\" both use %s's attire %d. %s wears entrance gear, and new attires "
                                   L"for them don't load in the game yet, so neither can move: switch one off, or the "
                                   L"game leaves \"%s\" out.\r\n",
                           keeper->name, a->name, roster_name(a->id, who, 48), a->attire, roster_name(a->id, who, 48),
                           a->name);
            else
                swprintf_s(t, 512, L"\"%s\" and \"%s\" both use %s's attire %d and there is no free attire to "
                                   L"move one to (4 at most): switch one off, or the game leaves \"%s\" out.\r\n",
                           keeper->name, a->name, roster_name(a->id, who, 48), a->attire, a->name);
            if (report) wcsncat_s(report, report_n, t, _TRUNCATE);
            said++;
        }
        /* attire=new ones take what is left (the game hands them out in folder order) */
        for (k = 0; k < 4; k++)
            if (!(used >> k & 1) && news) news--;
        if (news) {
            WCHAR who[48], t[256];
            swprintf_s(t, 256, gear ? L"%s wears entrance gear: new attires for them don't load in the game yet, so "
                                      L"%d attire=new mod%s stay%s out: switch %s off.\r\n"
                                    : L"%s has no free attire for %d attire=new mod%s (4 at most): switch some off.\r\n",
                       roster_name(mods[i].id, who, 48), news, news == 1 ? L"" : L"s", news == 1 ? L"s" : L"",
                       news == 1 ? L"it" : L"them");
            if (report) wcsncat_s(report, report_n, t, _TRUNCATE);
            said++;
        }
        for (j = n - 1; j >= i; j--)
            if (mods[j].id == mods[i].id && j != i) mods[j].id = -1;
        mods[i].id = -1;
    }
    return said;
}

/* Tests: --mods-attires <game folder> runs the guard as Play does and prints what it did. */
int mods_print_attires(const WCHAR *game)
{
    static WCHAR report[8192];
    const int n = mods_attire_guard(game, NULL, report, 8192);
    wprintf(L"%s%d\n", report, n);
    return 1;
}

static int pick_file(WCHAR *out)
{
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    int ok = 0;
    COMDLG_FILTERSPEC spec[] = { { L"SvR2011 mods (*.svrmod, *.zip)", L"*.svrmod;*.zip" } };
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg)))
        return 0;
    dlg->lpVtbl->SetFileTypes(dlg, 1, spec);
    dlg->lpVtbl->SetTitle(dlg, L"Add a mod");
    if (SUCCEEDED(dlg->lpVtbl->Show(dlg, s_wnd)) && SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item)) &&
        SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
        wcsncpy_s(out, MAX_PATH, path, _TRUNCATE);
        CoTaskMemFree(path);
        ok = 1;
    }
    if (item) item->lpVtbl->Release(item);
    dlg->lpVtbl->Release(dlg);
    return ok;
}

/* id -> a safe folder name */
static void safe_id(WCHAR *id)
{
    for (; *id; id++)
        if (!(iswalnum(*id) || *id == L'_' || *id == L'-' || *id == L'.')) *id = L'_';
}

static int install_file(const WCHAR *zip);
static void install_log(const WCHAR *game, const WCHAR *text);

/* + : one or several .svrmod / .zip files, installed one after the other. */
static void add_mod(void)
{
    IFileOpenDialog *dlg = NULL;
    IShellItemArray *items = NULL;
    DWORD n = 0, i;
    int done = 0;
    COMDLG_FILTERSPEC spec[] = { { L"SvR2011 mods (*.svrmod, *.zip)", L"*.svrmod;*.zip" } };
    if (!s_game[0]) { status(L"Install the game first (Setup \x2192 The game)."); return; }
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg)))
        return;
    dlg->lpVtbl->SetFileTypes(dlg, 1, spec);
    dlg->lpVtbl->SetOptions(dlg, FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dlg->lpVtbl->SetTitle(dlg, L"Add mods (several at once: Ctrl+click)");
    if (SUCCEEDED(dlg->lpVtbl->Show(dlg, s_wnd)) && SUCCEEDED(dlg->lpVtbl->GetResults(dlg, &items))) {
        items->lpVtbl->GetCount(items, &n);
        for (i = 0; i < n; i++) {
            IShellItem *it = NULL;
            PWSTR path = NULL;
            if (SUCCEEDED(items->lpVtbl->GetItemAt(items, i, &it)) &&
                SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &path))) {
                WCHAR one[MAX_PATH];
                wcsncpy_s(one, MAX_PATH, path, _TRUNCATE);
                CoTaskMemFree(path);
                install_file(one);   /* (its own status line; the last one's stays) */
                done++;
            }
            if (it) it->lpVtbl->Release(it);
        }
        items->lpVtbl->Release(items);
    }
    dlg->lpVtbl->Release(dlg);
    if (done > 1) {
        WCHAR t[96];
        swprintf_s(t, 96, L"Installed %d mods (they load the next time the game starts).", done);
        status(t);
    }
}

/* A picture for the main menu (backgrounds.h). */
static int pick_image(WCHAR *out)
{
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    int ok = 0;
    COMDLG_FILTERSPEC spec[] = { { L"Pictures (*.png, *.jpg, *.jpeg, *.bmp, *.webp)", L"*.png;*.jpg;*.jpeg;*.bmp;*.webp" } };
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg)))
        return 0;
    dlg->lpVtbl->SetFileTypes(dlg, 1, spec);
    dlg->lpVtbl->SetTitle(dlg, L"Add a main menu background picture");
    if (SUCCEEDED(dlg->lpVtbl->Show(dlg, s_wnd)) && SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item)) &&
        SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
        wcsncpy_s(out, MAX_PATH, path, _TRUNCATE);
        CoTaskMemFree(path);
        ok = 1;
    }
    if (item) item->lpVtbl->Release(item);
    dlg->lpVtbl->Release(dlg);
    return ok;
}

static void add_background(void)
{
    WCHAR img[MAX_PATH], err[600] = L"", t[700];
    HCURSOR old;
    int ok;
    if (!s_game[0]) { status(L"Install the game first (Install tab)."); return; }
    if (!pick_image(img)) return;
    status(L"Converting the picture\x2026");
    old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    ok = bg_make(s_game, img, NULL, 0, NULL, err, 600);
    SetCursor(old);
    scan();
    fill();
    if (!ok) { status(err); return; }
    swprintf_s(t, 700, L"Added. The main menu shows it after its own 12 pictures (MY WWE \x2192 OPTIONS \x2192 "
                       L"BACKGROUNDS turns pictures on or off, X there shows the game's logo on it).");
    status(t);
}

int mods_add_background(const WCHAR *game_dir, const WCHAR *image, int logo)
{
    WCHAR err[600] = L"", dir[MAX_PATH] = L"";
    const int ok = bg_make(game_dir, image, NULL, logo, dir, err, 600);
    if (ok) wprintf(L"added: %s\n", dir);
    else wprintf(L"failed: %s\n", err);
    return ok;
}

static WCHAR s_installed[MAX_PATH];  /* the folder install_file put the last mod in */
static int s_installed_existed;      /* ... and whether it was installed already */
static int s_bundle_install;         /* (mods_install_bundled: never an older version over a newer one) */

/* a > b as dotted versions ("1.10" > "1.9"). */
static int version_newer(const WCHAR *a, const WCHAR *b)
{
    while (*a || *b) {
        long x = wcstol(a, (WCHAR **)&a, 10), y = wcstol(b, (WCHAR **)&b, 10);
        if (x != y) return x > y;
        if (*a == L'.') a++;
        if (*b == L'.') b++;
        if ((*a && !iswdigit(*a)) || (*b && !iswdigit(*b))) break;
    }
    return 0;
}

/* A .svrmod's manifest.txt read from the zip (its central directory and that
   one entry, not an unpack of the whole mod) -> type and id. */
static int zip_manifest(const WCHAR *zip, Mod *m)
{
    FILE *f;
    unsigned char tail[65536 + 22], *cd = NULL, hdr[30], *data = NULL, *raw = NULL;
    long long size;
    size_t got, i, at;
    unsigned cd_size, cd_off, n;
    int ok = 0;
    if (_wfopen_s(&f, zip, L"rb") || !f) return 0;
    _fseeki64(f, 0, SEEK_END);
    size = _ftelli64(f);
    got = (size_t)(size < (long long)sizeof tail ? size : (long long)sizeof tail);
    _fseeki64(f, size - (long long)got, SEEK_SET);
    if (fread(tail, 1, got, f) != got) goto done;
    for (i = got >= 22 ? got - 22 : 0;; --i) {   /* the end-of-central-directory record */
        if (tail[i] == 0x50 && tail[i + 1] == 0x4B && tail[i + 2] == 5 && tail[i + 3] == 6) break;
        if (!i) goto done;
    }
    n = tail[i + 10] | tail[i + 11] << 8;
    cd_size = tail[i + 12] | tail[i + 13] << 8 | tail[i + 14] << 16 | (unsigned)tail[i + 15] << 24;
    cd_off = tail[i + 16] | tail[i + 17] << 8 | tail[i + 18] << 16 | (unsigned)tail[i + 19] << 24;
    if (!(cd = (unsigned char *)malloc(cd_size)) || _fseeki64(f, cd_off, SEEK_SET) || fread(cd, 1, cd_size, f) != cd_size)
        goto done;
    for (at = 0; n-- && at + 46 <= cd_size;) {
        const unsigned method = cd[at + 10] | cd[at + 11] << 8;
        const unsigned csize = cd[at + 20] | cd[at + 21] << 8 | cd[at + 22] << 16 | (unsigned)cd[at + 23] << 24;
        const unsigned usize = cd[at + 24] | cd[at + 25] << 8 | cd[at + 26] << 16 | (unsigned)cd[at + 27] << 24;
        const unsigned nlen = cd[at + 28] | cd[at + 29] << 8, xlen = cd[at + 30] | cd[at + 31] << 8;
        const unsigned clen = cd[at + 32] | cd[at + 33] << 8;
        const unsigned loc = cd[at + 42] | cd[at + 43] << 8 | cd[at + 44] << 16 | (unsigned)cd[at + 45] << 24;
        if (at + 46 + nlen > cd_size) break;
        if (nlen == 12 && !memcmp(cd + at + 46, "manifest.txt", 12) && usize < (1u << 20) && csize < (1u << 20)) {
            if (_fseeki64(f, loc, SEEK_SET) || fread(hdr, 1, 30, f) != 30 ||
                _fseeki64(f, loc + 30 + (hdr[26] | hdr[27] << 8) + (hdr[28] | hdr[29] << 8), SEEK_SET))
                goto done;
            if (!(raw = (unsigned char *)malloc(csize + 1)) || fread(raw, 1, csize, f) != csize) goto done;
            if (method == 0 && csize == usize) data = raw, raw = NULL;
            else if (method == 8 && (data = (unsigned char *)malloc(usize + 1)) && unzip_inflate(raw, csize, data, usize)) {}
            else goto done;
            data[usize] = 0;
            {   /* "key=value" lines, UTF-8 */
                char *line = (char *)data, *next;
                for (; line && *line; line = next) {
                    WCHAR w[512], *eq;
                    next = strchr(line, '\n');
                    if (next) *next++ = 0;
                    if (!MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 512)) continue;
                    trim(w);
                    if (!(eq = wcschr(w, L'='))) continue;
                    *eq++ = 0;
                    if (!wcscmp(w, L"type")) wcsncpy_s(m->type, 32, eq, _TRUNCATE);
                    else if (!wcscmp(w, L"id")) wcsncpy_s(m->id, 64, eq, _TRUNCATE);
                    else if (!wcscmp(w, L"version")) wcsncpy_s(m->version, 32, eq, _TRUNCATE);
                }
            }
            ok = m->type[0] && m->id[0];
            break;
        }
        at += 46 + nlen + xlen + clen;
    }
done:
    free(cd);
    free(raw);
    free(data);
    fclose(f);
    return ok;
}

/* A mod type's folder under Mods (as install_file places it); NULL for one it doesn't install. */
static const WCHAR *kind_folder(const WCHAR *type)
{
    static const struct { const WCHAR *type, *folder; } k[] = {
        {L"superstar", L"Superstars"}, {L"arena", L"Arenas"}, {L"backstage", L"Backstage"}, {L"signs", L"Signs"},
        {L"media", L"Media"}, {L"matchtype", L"MatchTypes"}, {L"background", L"Backgrounds"}, {L"attire", L"Attires"},
        {L"moves", L"Moves"},
    };
    int i;
    for (i = 0; i < (int)(sizeof k / sizeof *k); ++i)
        if (!_wcsicmp(type, k[i].type)) return k[i].folder;
    return NULL;
}

/* An installed mod folder missing what install_file requires of its type (a superstar without ch.pac, an arena
   without arena.pac ...): what a half-done replace left. */
static int folder_incomplete(const WCHAR *dir, const WCHAR *type)
{
    WCHAR p[MAX_PATH];
    const WCHAR *need[3] = { L"manifest.txt", NULL, NULL };
    int i;
    if (!_wcsicmp(type, L"superstar")) need[1] = L"ch.pac";
    else if (!_wcsicmp(type, L"arena")) need[1] = L"arena.pac", need[2] = L"banner.dds";
    else if (!_wcsicmp(type, L"backstage")) need[1] = L"arena.pac";
    else if (!_wcsicmp(type, L"attire")) need[1] = L"attire.pac";
    else if (!_wcsicmp(type, L"moves")) need[1] = L"pack.txt";
    else if (!_wcsicmp(type, L"background")) need[1] = L"*.dds";
    for (i = 0; i < 3 && need[i]; ++i) {
        swprintf_s(p, MAX_PATH, L"%s\\%s", dir, need[i]);
        if (wcschr(need[i], L'*')) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(p, &fd);
            if (h == INVALID_HANDLE_VALUE) return 1;
            FindClose(h);
        } else if (!exists(p)) {
            return 1;
        }
    }
    return 0;
}

int mods_install(const WCHAR *game_dir, const WCHAR *zip)
{
    wcsncpy_s(s_game, MAX_PATH, game_dir ? game_dir : L"", _TRUNCATE);
    return s_game[0] && install_file(zip);
}

/* Mods shipped with the port (<bundle_dir>\*.svrmod, the release's
 * "Bundled Mods"): each is installed into the game once, and again only when
 * the shipped file changes - <game>\Mods\.bundled keeps "name|size|time" of
 * those installed - so one the player removed stays removed. A bundle comes
 * switched off the first time ("disabled" in its folder) - except a match
 * type, which the game had on before it became a mod. One the player
 * already has (installed by hand, or an older bundle) is updated to it and
 * keeps its on / off - unless the player's copy is a newer version. */
void mods_install_bundled(const WCHAR *game_dir, const WCHAR *bundle_dir)
{
    WCHAR pat[MAX_PATH], zip[MAX_PATH], list[MAX_PATH], line[MAX_PATH + 64], mods[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    static WCHAR known[8192];
    FILE *f;
    int changed = 0, fresh = 0;
    if (!game_dir || !game_dir[0] || !bundle_dir || !bundle_dir[0])
        return;
    swprintf_s(pat, MAX_PATH, L"%s\\*.svrmod", bundle_dir);
    swprintf_s(mods, MAX_PATH, L"%s\\Mods", game_dir);
    swprintf_s(list, MAX_PATH, L"%s\\.bundled", mods);
    known[0] = 0;
    if (_wfopen_s(&f, list, L"r, ccs=UTF-8") == 0 && f) {
        size_t n = fread(known, sizeof(WCHAR), sizeof known / sizeof *known - 1, f);
        known[n] = 0;
        fclose(f);
    }
    /* Bundles no longer shipped: the copy the launcher installed is switched
       off once ("disabled" in its folder; the player may switch it back on)
       and its line leaves .bundled. hurricane.svrmod (2.0.5): The Hurricane
       is the playable manager on the M tile already - two tiles, neither with
       an entrance video, theme or announcer clip (the game has none). */
    {
        static const struct { const WCHAR *file, *folder; } retired[] = {
            {L"hurricane.svrmod", L"Superstars\\the_hurricane"},
        };
        int i;
        for (i = 0; i < (int)(sizeof retired / sizeof *retired); ++i) {
            WCHAR key[MAX_PATH], *at, *end, off[MAX_PATH];
            FILE *g;
            swprintf_s(key, MAX_PATH, L"%s|", retired[i].file);
            at = wcsstr(known, key);
            if (!at || (at != known && at[-1] != L'\n'))
                continue;
            swprintf_s(off, MAX_PATH, L"%s\\%s", mods, retired[i].folder);
            if (GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
                wcscat_s(off, MAX_PATH, L"\\disabled");
                if (_wfopen_s(&g, off, L"wb") == 0 && g) fclose(g);
            }
            end = wcschr(at, L'\n');
            end = end ? end + 1 : at + wcslen(at);
            memmove(at, end, (wcslen(end) + 1) * sizeof(WCHAR));
            changed = 1;
        }
    }
    /* Old copies a replace couldn't recycle (swap_in): gone now. */
    {
        WCHAR olds[MAX_PATH];
        swprintf_s(olds, MAX_PATH, L"%s\\.old", mods);
        if (exists(olds)) remove_tree(olds, 1);
    }
    /* Repair: a bundle installed before whose folder is now incomplete (a 2.1.2 update's replace that recycled
       the old folder half-way - a held file - and then couldn't move the new one in: the manifest but no model,
       listed but left out by the game) is installed again, keeping its on / off. A bundle whose folder is gone
       altogether stays gone (the player removed it). */
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            WCHAR key[MAX_PATH + 2], *at, dir[MAX_PATH], msg[512];
            const WCHAR *kf;
            Mod bm;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            swprintf_s(key, MAX_PATH + 2, L"%s|", fd.cFileName);
            for (at = wcsstr(known, key); at && at != known && at[-1] != L'\n'; at = wcsstr(at + 1, key)) {}
            if (!at) continue;   /* (not installed yet: the loop below does it) */
            ZeroMemory(&bm, sizeof bm);
            swprintf_s(zip, MAX_PATH, L"%s\\%s", bundle_dir, fd.cFileName);
            if (!zip_manifest(zip, &bm) || !(kf = kind_folder(bm.type))) continue;
            safe_id(bm.id);
            swprintf_s(dir, MAX_PATH, L"%s\\%s\\%s", mods, kf, bm.id);
            if (!exists(dir) || !folder_incomplete(dir, bm.type)) continue;
            swprintf_s(msg, 512, L"bundled %s: %s\\%s is incomplete (a replace stopped half-way) - installing it again",
                       fd.cFileName, kf, bm.id);
            install_log(game_dir, msg);
            /* its lines out (every one: an update leaves the older version's line too) - the loop below installs it
               again, keeping its on / off */
            while (at) {
                WCHAR *end = wcschr(at, L'\n');
                end = end ? end + 1 : at + wcslen(at);
                memmove(at, end, (wcslen(end) + 1) * sizeof(WCHAR));
                changed = 1;
                for (at = wcsstr(known, key); at && at != known && at[-1] != L'\n'; at = wcsstr(at + 1, key)) {}
            }
            {
                WCHAR mark[MAX_PATH + 4];
                swprintf_s(mark, MAX_PATH + 4, L"\n%s|repair\n", fd.cFileName);   /* (keeps "fresh" off below) */
                if (wcslen(known) + wcslen(mark) < sizeof known / sizeof *known - 1) wcscat_s(known, sizeof known / sizeof *known, mark + 1);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (changed && _wfopen_s(&f, list, L"w, ccs=UTF-8") == 0 && f) {
            fputws(known, f);
            fclose(f);
        }
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        swprintf_s(line, MAX_PATH + 64, L"%s|%lu|%lu%lu\n", fd.cFileName, fd.nFileSizeLow,
                   fd.ftLastWriteTime.dwHighDateTime, fd.ftLastWriteTime.dwLowDateTime);
        if (wcsstr(known, line))
            continue;
        swprintf_s(zip, MAX_PATH, L"%s\\%s", bundle_dir, fd.cFileName);
        SHCreateDirectoryExW(NULL, mods, NULL);
        {
            /* (a bundle installed for the first time comes switched off - the
               player ticks the ones they want on the Mods tab; a new version
               of one already installed keeps its on / off) */
            WCHAR first[MAX_PATH + 4];
            swprintf_s(first, MAX_PATH + 4, L"\n%s|", fd.cFileName);
            fresh = wcsncmp(known, first + 1, wcslen(first + 1)) != 0 && !wcsstr(known, first);
        }
        s_installed[0] = 0;
        s_installed_existed = 0;
        s_bundle_install = 1;
        if (mods_install(game_dir, zip) && fresh && !s_installed_existed && s_installed[0] &&
            !wcsstr(s_installed, L"\\Mods\\MatchTypes\\")) {
            WCHAR off[MAX_PATH];
            FILE *f;
            swprintf_s(off, MAX_PATH, L"%s\\disabled", s_installed);
            if (_wfopen_s(&f, off, L"wb") == 0 && f) fclose(f);
            scan();
            fill();
        }
        s_bundle_install = 0;
        if (s_installed[0] && wcslen(known) + wcslen(line) < sizeof known / sizeof *known - 1) {
            wcscat_s(known, sizeof known / sizeof *known, line);
            changed = 1;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    {   /* (the repair's "<file>|repair" marks: not written) */
        WCHAR *r;
        while ((r = wcsstr(known, L"|repair\n")) != NULL) {
            WCHAR *start = r, *end = r + 8;
            while (start > known && start[-1] != L'\n') --start;
            memmove(start, end, (wcslen(end) + 1) * sizeof(WCHAR));
        }
    }
    if (changed && _wfopen_s(&f, list, L"w, ccs=UTF-8") == 0 && f) {
        fputws(known, f);
        fclose(f);
    }
}

/* A superstar / media mod's .mp4 entrance movies (movie= / video.<id>=) that
 * have no .bik of the same name beside them get one (1024 x 1024, the movie
 * on the big screen): the game plays the .bik where the platform can't decode
 * the .mp4 (Proton on the Steam Deck, Windows "N" editions). The Mod Maker
 * writes both; this covers mods made by hand. */
static void make_movie_fallbacks(const WCHAR *dir)
{
    WCHAR man[MAX_PATH];
    FILE *f;
    char line[1024];
    swprintf_s(man, MAX_PATH, L"%s\\manifest.txt", dir);
    if (_wfopen_s(&f, man, L"rb") || !f) return;
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, '='), *e;
        WCHAR file[MAX_PATH];
        size_t n;
        static MovieJob job;
        if (!v || (strncmp(line, "movie=", 6) && strncmp(line, "video.", 6))) continue;
        v++;
        for (e = v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
        n = strlen(v);
        if (n < 5 || _stricmp(v + n - 4, ".mp4")) continue;
        if (!MultiByteToWideChar(CP_UTF8, 0, v, -1, file, MAX_PATH)) continue;
        ZeroMemory(&job, sizeof job);
        swprintf_s(job.video, MAX_PATH, L"%s\\%s", dir, file);
        swprintf_s(job.out, MAX_PATH, L"%s\\%s", dir, file);
        wcscpy_s(job.out + wcslen(job.out) - 4, 5, L".bik");
        if (exists(job.out) || !exists(job.video)) continue;
        job.fit = MOVIE_FIT;
        job.size = 1024;
        status(L"Making the mod's movie for PCs that can't play its MP4 (Steam Deck)...");
        if (!movie_make(&job)) status(job.err);
    }
    fclose(f);
}

/* A line in <game>\Mods\.install.log (UTF-8): what an install or the bundle
   repair did when it didn't simply work - the launcher may have no window to
   say it in (installs at start-up, after an update, --bundled). */
static void install_log(const WCHAR *game, const WCHAR *text)
{
    WCHAR path[MAX_PATH];
    SYSTEMTIME st;
    FILE *f;
    swprintf_s(path, MAX_PATH, L"%s\\Mods\\.install.log", game);
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") || !f) return;
    GetLocalTime(&st);
    fwprintf(f, L"%04d-%02d-%02d %02d:%02d:%02d %s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, text);
    fclose(f);
}

static void collect_files(const WCHAR *dir, WCHAR (*files)[MAX_PATH], int *n, int cap)
{
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || *n >= cap) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            WCHAR sub[MAX_PATH];
            swprintf_s(sub, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            collect_files(sub, files, n, cap);
        } else {
            swprintf_s(files[(*n)++], MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* The programs holding files of `dir` open (Restart Manager), "" if none is found. */
static void who_holds(const WCHAR *dir, WCHAR *out, size_t cap)
{
    static WCHAR files[256][MAX_PATH];
    static LPCWSTR names[256];
    int n = 0, i;
    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = { 0 };
    out[0] = 0;
    collect_files(dir, files, &n, 256);
    if (!n || RmStartSession(&session, 0, key) != ERROR_SUCCESS) return;
    for (i = 0; i < n; ++i) names[i] = files[i];
    if (RmRegisterResources(session, (UINT)n, names, 0, NULL, 0, NULL) == ERROR_SUCCESS) {
        RM_PROCESS_INFO info[16];
        UINT need = 0, have = 16;
        DWORD reason = 0;
        if (RmGetList(session, &need, &have, info, &reason) == ERROR_SUCCESS)
            for (i = 0; i < (int)have; ++i)
                if (!wcsstr(out, info[i].strAppName)) {
                    if (out[0]) wcsncat_s(out, cap, L", ", _TRUNCATE);
                    wcsncat_s(out, cap, info[i].strAppName, _TRUNCATE);
                }
    }
    RmEndSession(session);
}

/* MoveFileW, tried a few times over ~2 s: an antivirus scanning freshly
   unpacked files, or OneDrive, holds them for a moment. */
static int move_retry(const WCHAR *from, const WCHAR *to)
{
    int i;
    for (i = 0; i < 8; ++i) {
        if (MoveFileW(from, to)) return 1;
        Sleep(250);
    }
    return 0;
}

/* The new copy of a mod (tmp) in place of the installed one (dst), never
   leaving a half: the old folder is renamed into Mods\.old first (a rename
   fails whole when a file in it is held, and then nothing changed), the new
   one moved in (on failure the old one goes back), and only then the old one
   goes to the Recycle Bin (if that fails, Mods\.old is emptied at the next
   start). It used to be recycled first: a held file left a folder with its
   manifest but no model - listed, but left out by the game (2.1.2 bundled-mod
   reports). 1 on success; else `why` says what to do. */
static int swap_in(const WCHAR *tmp, const WCHAR *dst, const WCHAR *kind, const WCHAR *id, WCHAR *why, size_t cap)
{
    WCHAR old[MAX_PATH], olds[MAX_PATH], who[512];
    int n = 0;
    if (!exists(dst)) {
        if (move_retry(tmp, dst)) return 1;
        swprintf_s(why, cap, L"The mod could not be moved into %s (Windows error %lu): close the game and the Mod Maker "
                             L"and install again.", dst, GetLastError());
        return 0;
    }
    swprintf_s(olds, MAX_PATH, L"%s\\Mods\\.old", s_game);
    SHCreateDirectoryExW(NULL, olds, NULL);
    do swprintf_s(old, MAX_PATH, L"%s\\%s-%s-%d", olds, kind, id, n++);
    while (exists(old) && n < 1000);
    if (!move_retry(dst, old)) {
        const DWORD err = GetLastError();
        who_holds(dst, who, 512);
        if (who[0]) swprintf_s(why, cap, L"The installed copy of \"%s\" is in use by %s: close that program and install "
                                         L"again (the installed copy is unchanged).", id, who);
        else swprintf_s(why, cap, L"The installed copy of \"%s\" couldn't be replaced (Windows error %lu - the game, the "
                                  L"Mod Maker, an antivirus or OneDrive may be using it): it is unchanged; close them and "
                                  L"install again.", id, err);
        return 0;
    }
    if (!move_retry(tmp, dst)) {
        const DWORD err = GetLastError();
        if (!MoveFileW(old, dst)) {
            swprintf_s(why, cap, L"\"%s\" couldn't be installed (Windows error %lu) and its old copy couldn't be put back: "
                                 L"it is in %s.", id, err, old);
            return 0;
        }
        swprintf_s(why, cap, L"\"%s\" couldn't be installed (Windows error %lu - an antivirus or OneDrive may be holding "
                             L"the new files): the installed copy is unchanged; install again.", id, err);
        return 0;
    }
    remove_tree(old, 1);  /* (an older version: to the Recycle Bin; a held file leaves it for the next start) */
    return 1;
}

static int install_file(const WCHAR *zip)
{
    WCHAR tmp[MAX_PATH], man[MAX_PATH], dst[MAX_PATH], parent[MAX_PATH], t[512], kind[32];
    Mod m;
    swprintf_s(tmp, MAX_PATH, L"%s\\Mods\\.incoming", s_game);
    remove_tree(tmp, 0);
    SHCreateDirectoryExW(NULL, tmp, NULL);
    if (!unzip_file(zip, tmp)) {
        status(L"That file is not a mod (it could not be unpacked).");
        remove_tree(tmp, 0);
        return 0;
    }
    ZeroMemory(&m, sizeof m);
    swprintf_s(man, MAX_PATH, L"%s\\manifest.txt", tmp);
    if (!read_manifest(man, &m)) {
        status(L"That file has no manifest.txt (type and id): it is not an SvR2011 mod.");
        remove_tree(tmp, 0);
        return 0;
    }
    {
        const int backstage = !_wcsicmp(m.type, L"backstage");
        const int arena = !_wcsicmp(m.type, L"arena") || backstage, star = !_wcsicmp(m.type, L"superstar");
        const int signs = !_wcsicmp(m.type, L"signs"), media = !_wcsicmp(m.type, L"media");
        const int moves = !_wcsicmp(m.type, L"moves");
        const int match_type = !_wcsicmp(m.type, L"matchtype");
        const int background = !_wcsicmp(m.type, L"background");
        const int attire = !_wcsicmp(m.type, L"attire");
        WCHAR need1[MAX_PATH], need2[MAX_PATH];
        if (signs || media || match_type) {
            wcsncpy_s(kind, 32, signs ? L"Signs" : media ? L"Media" : L"MatchTypes", _TRUNCATE);
            goto place;
        }
        if (background) {  /* main menu pictures: picture.dds or a pack's *.dds (1024 x 512 DXT5) */
            WIN32_FIND_DATAW pf;
            HANDLE ph;
            swprintf_s(need1, MAX_PATH, L"%s\\*.dds", tmp);
            ph = FindFirstFileW(need1, &pf);
            if (ph != INVALID_HANDLE_VALUE) FindClose(ph);
            if (ph == INVALID_HANDLE_VALUE) {
                status(L"That background mod is incomplete (it has no pictures: picture.dds).");
                remove_tree(tmp, 0);
                return 0;
            }
            wcsncpy_s(kind, 32, L"Backgrounds", _TRUNCATE);
            goto place;
        }
        if (attire) {  /* an attire for a superstar (Mods/Attires/<id>: manifest + attire.pac) */
            swprintf_s(need1, MAX_PATH, L"%s\\attire.pac", tmp);
            if (!exists(need1)) {
                status(L"That attire mod is incomplete (it needs attire.pac).");
                remove_tree(tmp, 0);
                return 0;
            }
            wcsncpy_s(kind, 32, L"Attires", _TRUNCATE);
            goto place;
        }
        if (moves) {  /* a move pack: pack.txt + motions (docs/MOVE_PACKS.md) */
            swprintf_s(need1, MAX_PATH, L"%s\\pack.txt", tmp);
            if (!exists(need1)) {
                status(L"That move pack is incomplete (it needs pack.txt).");
                remove_tree(tmp, 0);
                return 0;
            }
            wcsncpy_s(kind, 32, L"Moves", _TRUNCATE);
            goto place;
        }
        if (!arena && !star) {
            swprintf_s(t, 512, L"\"%s\" is a %s mod; this version installs arena, backstage, superstar, sign, media, "
                               L"move, match type, background and attire mods.", m.name, m.type);
            status(t);
            remove_tree(tmp, 0);
            return 0;
        }
        swprintf_s(need1, MAX_PATH, L"%s\\%s", tmp, arena ? L"arena.pac" : L"ch.pac");
        swprintf_s(need2, MAX_PATH, L"%s\\%s", tmp, arena && !backstage ? L"banner.dds" : L"manifest.txt");
        if (!exists(need1) || !exists(need2)) {
            status(arena ? L"That arena mod is incomplete (it needs arena.pac and banner.dds)."
                         : L"That superstar mod is incomplete (it needs ch.pac).");
            remove_tree(tmp, 0);
            return 0;
        }
        wcsncpy_s(kind, 32, backstage ? L"Backstage" : arena ? L"Arenas" : L"Superstars", _TRUNCATE);
    }
place:
    safe_id(m.id);
    swprintf_s(parent, MAX_PATH, L"%s\\Mods\\%s", s_game, kind);
    SHCreateDirectoryExW(NULL, parent, NULL);
    swprintf_s(dst, MAX_PATH, L"%s\\%s", parent, m.id);
    s_installed_existed = exists(dst);
    if (s_installed_existed && s_bundle_install) {
        /* (the player's copy is newer than the port's: kept) */
        WCHAR old_man[MAX_PATH];
        Mod old;
        ZeroMemory(&old, sizeof old);
        swprintf_s(old_man, MAX_PATH, L"%s\\manifest.txt", dst);
        if (read_manifest(old_man, &old) && version_newer(old.version, m.version)) {
            remove_tree(tmp, 0);
            wcscpy_s(s_installed, MAX_PATH, dst);
            return 1;
        }
    }
    {
        WCHAR off[MAX_PATH];
        int was_off;
        swprintf_s(off, MAX_PATH, L"%s\\disabled", dst);
        was_off = exists(off);  /* (a new version keeps the old one's on / off) */
        if (!swap_in(tmp, dst, kind, m.id, t, 512)) {
            status(t);
            install_log(s_game, t);
            remove_tree(tmp, 0);
            return 0;
        }
        if (was_off) {
            FILE *f;
            if (_wfopen_s(&f, off, L"wb") == 0 && f) fclose(f);
        }
    }
    wcscpy_s(s_installed, MAX_PATH, dst);
    if (!wcscmp(kind, L"Superstars") || !wcscmp(kind, L"Media")) make_movie_fallbacks(dst);
    scan();
    fill();
    swprintf_s(t, 512, !wcscmp(kind, L"Backstage") ? L"Installed \"%s\". It is played in that room's backstage brawls."
                       : !wcscmp(kind, L"Media") ? L"Installed \"%s\". It takes effect the next time the game starts."
                       : !wcscmp(kind, L"Moves") ? L"Installed \"%s\". The game merges its moves into its files when it next starts (a few seconds)."
                       : !wcscmp(kind, L"MatchTypes") ? L"Installed \"%s\". The match type is in the game's menus the "
                                                        L"next time the game starts."
                       : !wcscmp(kind, L"Backgrounds") ? L"Installed \"%s\". The main menu shows it after its own "
                                                         L"12 pictures."
                       : !wcscmp(kind, L"Signs") ? L"Installed \"%s\". The crowd holds these signs up in every match."
                       : !wcscmp(kind, L"Attires") ? L"Installed \"%s\". The superstar has it on CHANGE ATTIRE the next "
                                                     L"time the game starts."
                       : wcscmp(kind, L"Arenas") ? L"Installed \"%s\". It is under the M tile of the character "
                                                   L"select (up to 137 superstar mods)."
                                                 : L"Installed \"%s\". It is on the arena select pages after the "
                                                   L"game's own arenas.",
               m.name[0] ? m.name : m.id);
    if (!wcscmp(kind, L"Attires")) {  /* (two attire mods on one attire: the new one moves) */
        static WCHAR clash[2048];
        if (mods_attire_guard(s_game, dst, clash, 2048)) {
            scan();
            fill();
            wcsncat_s(t, 512, L" ", _TRUNCATE);
            wcsncat_s(t, 512, clash, _TRUNCATE);
            if (s_wnd) MessageBoxW(s_wnd, clash, L"Attire mods", MB_OK | MB_ICONINFORMATION);
        }
    }
    status(t);
    return 1;
}

static int selected(void) { return ListView_GetNextItem(s_list, -1, LVNI_SELECTED); }
static void set_enabled(int i, int on);

/* The selected rows' mods (indices into s_mods), at most `max`. */
static int selected_all(int *out, int max)
{
    int row = -1, n = 0;
    while (n < max && (row = ListView_GetNextItem(s_list, row, LVNI_SELECTED)) >= 0) {
        LVITEMW it;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_PARAM;
        it.iItem = row;
        if (ListView_GetItem(s_list, &it) && (int)it.lParam >= 0 && (int)it.lParam < s_nmods)
            out[n++] = (int)it.lParam;
    }
    return n;
}

/* On / Off: the selected ones (through their tick, as a click on it does: the superstar limit's count too). */
static void switch_selected(int on)
{
    int row = -1, n = 0;
    while ((row = ListView_GetNextItem(s_list, row, LVNI_SELECTED)) >= 0) {
        ListView_SetCheckState(s_list, row, on);
        n++;
    }
    if (!n) status(L"Select mods in the list first (Ctrl / Shift + click for several, Ctrl+A for all).");
}

static void remove_one(int i);
static void remove_mod(void)
{
    static int sel[MODS_MAX];
    WCHAR t[512];
    int n = selected_all(sel, MODS_MAX), k, gone = 0, kept = 0;
    if (n > 1) {
        swprintf_s(t, 512, L"Remove these %d mods? Their folders go to the Recycle Bin (match types are switched off "
                           L"instead).", n);
        if (MessageBoxW(s_wnd, t, L"Remove mods", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
        for (k = 0; k < n; k++) {
            const int i = sel[k];
            if (!_wcsicmp(s_mods[i].type, L"matchtype")) { set_enabled(i, 0); kept++; continue; }
            if (remove_tree(s_mods[i].folder, 1)) {
                if (!_wcsicmp(s_mods[i].type, L"superstar")) free_superstar_slot(s_game, s_mods[i].folder);
                gone++;
            }
        }
        scan();
        fill();
        swprintf_s(t, 512, L"Removed %d (in the Recycle Bin)%s%s.", gone, kept ? L"; match types switched off" : L"",
                   gone + kept < n ? L"; some could not be removed (is the game running?)" : L"");
        status(t);
        return;
    }
    remove_one(n ? sel[0] : -1);
}

static void remove_one(int i)
{
    WCHAR t[512];
    if (i < 0 || i >= s_nmods) { status(L"Select a mod in the list first."); return; }
    if (!_wcsicmp(s_mods[i].type, L"matchtype")) {
        /* (the game has a match type on unless its folder says "disabled": removing it would turn it back on) */
        set_enabled(i, 0);
        fill();
        status(L"Match types can't be removed - switched off instead (the game leaves it out of its menus).");
        return;
    }
    swprintf_s(t, 512, L"Remove \"%s\"? Its folder goes to the Recycle Bin.", s_mods[i].name);
    if (MessageBoxW(s_wnd, t, L"Remove mod", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    if (!remove_tree(s_mods[i].folder, 1)) { status(L"It could not be removed (is the game running?)."); return; }
    if (!_wcsicmp(s_mods[i].type, L"superstar")) free_superstar_slot(s_game, s_mods[i].folder);
    scan();
    fill();
    status(L"Removed (it is in the Recycle Bin).");
}

static void set_enabled(int i, int on)
{
    WCHAR off[MAX_PATH];
    if (i < 0 || i >= s_nmods || s_mods[i].enabled == on) return;
    swprintf_s(off, MAX_PATH, L"%s\\disabled", s_mods[i].folder);
    if (on) DeleteFileW(off);
    else {
        HANDLE h = CreateFileW(off, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    s_mods[i].enabled = on;
    status(on ? L"On: it shows in the game next time it starts." : L"Off: the game leaves it out.");
}

static void open_folder(void)
{
    WCHAR dir[MAX_PATH];
    if (!s_game[0]) return;
    swprintf_s(dir, MAX_PATH, L"%s\\Mods", s_game);
    SHCreateDirectoryExW(NULL, dir, NULL);
    ShellExecuteW(s_wnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

/* The Mod Maker beside the launcher (left out of a release while it isn't
   ready: then the tab has no Open Mod Maker button). */
static int maker_present(void)
{
    WCHAR exe[MAX_PATH], *slash;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;
    wcscat_s(exe, MAX_PATH, L"\\SvR2011 Mod Maker.exe");
    return exists(exe);
}

static void open_maker(void)
{
    WCHAR exe[MAX_PATH], dir[MAX_PATH], args[MAX_PATH + 16], *slash;
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    swprintf_s(exe, MAX_PATH, L"%s\\SvR2011 Mod Maker.exe", dir);
    if (!exists(exe)) { status(L"SvR2011 Mod Maker.exe is not next to the launcher."); return; }
    swprintf_s(args, MAX_PATH + 16, L"--game \"%s\"", s_game);  /* (where the arenas are read from) */
    if ((INT_PTR)ShellExecuteW(s_wnd, L"open", exe, s_game[0] ? args : NULL, dir, SW_SHOWNORMAL) <= 32)
        status(L"The Mod Maker could not be started.");
}

void mods_build(HWND wnd, mods_add_fn add, int tab, int id_base, HFONT title_font)
{
    static const WCHAR *const names[] = { L"Name", L"Type", L"Version", L"Author", L"Size", L"Made with" };
    static const int widths[] = { 150, 60, 52, 84, 56, 104 };  /* (the list is 512 wide) */
    HWND h;
    HDC dc;
    int i, dpi;
    s_wnd = wnd;
    dc = GetDC(wnd);
    dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(wnd, dc);
    s_base = id_base;
    h = add(tab, L"Static", L"Mods", SS_LEFT, 28, 54, 300, 28, 0);
    if (title_font) SendMessageW(h, WM_SETFONT, (WPARAM)title_font, TRUE);
    add(tab, L"Static", L"Arenas, superstars and more. + adds .svrmod files, \x2212 removes the selected mods, "
                        L"On / Off or the tick turns them on or off (Ctrl / Shift + click: several, Ctrl+A: all).",
        SS_LEFT, 28, 86, 560, 36, 0);
    s_list = add(tab, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_EDITLABELS | WS_BORDER | WS_TABSTOP,
                 28, 126, 512, 250, id_base + M_LIST);
    ListView_SetExtendedListViewStyle(s_list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
    for (i = 0; i < 6; i++) {
        LVCOLUMNW c;
        ZeroMemory(&c, sizeof c);
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = i == 4 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.cx = MulDiv(widths[i], dpi, 96);
        c.pszText = (WCHAR *)names[i];
        ListView_InsertColumn(s_list, i, &c);
    }
    add(tab, L"Button", L"+", BS_PUSHBUTTON | WS_TABSTOP, 548, 126, 40, 34, id_base + M_ADD);
    add(tab, L"Button", L"\x2212", BS_PUSHBUTTON | WS_TABSTOP, 548, 166, 40, 34, id_base + M_REMOVE);
    /* (the selected ones - several with Ctrl / Shift, all with Ctrl+A - on or off) */
    add(tab, L"Button", L"On", BS_PUSHBUTTON | WS_TABSTOP, 548, 214, 40, 34, id_base + M_ON);
    add(tab, L"Button", L"Off", BS_PUSHBUTTON | WS_TABSTOP, 548, 254, 40, 34, id_base + M_OFF);
    if (maker_present()) {
        add(tab, L"Button", L"Open Mod Maker", BS_PUSHBUTTON | WS_TABSTOP, 28, 386, 170, 32, id_base + M_MAKER);
        add(tab, L"Button", L"Open Mods folder", BS_PUSHBUTTON | WS_TABSTOP, 206, 386, 150, 32, id_base + M_FOLDER);
        add(tab, L"Button", L"Add background picture\x2026", BS_PUSHBUTTON | WS_TABSTOP, 364, 386, 224, 32,
            id_base + M_BACKGROUND);
    } else {
        add(tab, L"Button", L"Open Mods folder", BS_PUSHBUTTON | WS_TABSTOP, 28, 386, 150, 32, id_base + M_FOLDER);
        add(tab, L"Button", L"Add background picture\x2026", BS_PUSHBUTTON | WS_TABSTOP, 186, 386, 224, 32,
            id_base + M_BACKGROUND);
    }
    s_status = add(tab, L"Static", L"", SS_LEFT, 28, 428, 560, 40, id_base + M_STATUS);
}

int mods_command(int id, int code)
{
    (void)code;
    if (id == s_base + M_ADD) { add_mod(); return 1; }
    if (id == s_base + M_ON || id == s_base + M_OFF) { switch_selected(id == s_base + M_ON); return 1; }
    if (id == s_base + M_REMOVE) { remove_mod(); return 1; }
    if (id == s_base + M_FOLDER) { open_folder(); return 1; }
    if (id == s_base + M_MAKER) { open_maker(); return 1; }
    if (id == s_base + M_BACKGROUND) { add_background(); return 1; }
    return 0;
}

/* A background's new name into its manifest.txt (name=), the other lines kept. */
/* Tests: prints the superstar mods as the Mods list shows them (left out =
   over the game's ids), and the status line's counts. */
int mods_print_superstars(const WCHAR *game)
{
    int i;
    wcsncpy_s(s_game, MAX_PATH, game, _TRUNCATE);
    scan();
    superstar_limit();
    for (i = 0; i < s_nmods; i++)
        if (!_wcsicmp(s_mods[i].type, L"superstar"))
            wprintf(L"%s\t%s\t%s\n", wcsrchr(s_mods[i].folder, L'\\') + 1, s_mods[i].enabled ? L"on" : L"off",
                    s_mods[i].left_out ? L"NOT IN GAME" : L"");
    wprintf(L"on %d, left out %d\n", s_stars_on, s_stars_out);
    return 1;
}

/* Tests: removes an installed mod as the list's Remove does (its folder to the
   Recycle Bin, a superstar mod's id freed). */
int mods_remove(const WCHAR *game, const WCHAR *folder)
{
    const int star = wcsstr(folder, L"\\Superstars\\") != NULL;
    if (!remove_tree(folder, 1)) return 0;
    if (star) free_superstar_slot(game, folder);
    return 1;
}

int mods_rename(const WCHAR *folder, const WCHAR *name)
{
    WCHAR man[MAX_PATH], tmp[MAX_PATH];
    FILE *in = NULL, *out = NULL;
    char line[1024], u[512];
    int done = 0;
    if (!name[0]) return 0;
    swprintf_s(man, MAX_PATH, L"%s\\manifest.txt", folder);
    swprintf_s(tmp, MAX_PATH, L"%s\\manifest.new", folder);
    WideCharToMultiByte(CP_UTF8, 0, name, -1, u, sizeof u, NULL, NULL);
    if (_wfopen_s(&out, tmp, L"wb") || !out) return 0;
    if (_wfopen_s(&in, man, L"rb") == 0 && in) {
        while (fgets(line, sizeof line, in)) {
            if (!strncmp(line, "name=", 5)) {
                if (!done) fprintf(out, "name=%s\n", u);
                done = 1;
            } else {
                fputs(line, out);
                if (line[0] && line[strlen(line) - 1] != '\n') fputs("\n", out);
            }
        }
        fclose(in);
    }
    if (!done) fprintf(out, "name=%s\n", u);
    fclose(out);
    return MoveFileExW(tmp, man, MOVEFILE_REPLACE_EXISTING) != 0;
}

int mods_notify(const NMHDR *nm)
{
    if (!s_list || nm->hwndFrom != s_list || s_filling) return 0;
    /* Renaming in place (backgrounds only: the others' names come with the
       mod). 2: the window answers TRUE, 3: FALSE (svr2011_launcher.c). */
    if (nm->code == LVN_BEGINLABELEDITW || nm->code == LVN_ENDLABELEDITW) {
        const NMLVDISPINFOW *d = (const NMLVDISPINFOW *)nm;
        LVITEMW it;
        int i;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_PARAM;
        it.iItem = d->item.iItem;
        i = ListView_GetItem(s_list, &it) ? (int)it.lParam : -1;
        if (i < 0 || i >= s_nmods || _wcsicmp(s_mods[i].type, L"background")) {
            if (nm->code == LVN_BEGINLABELEDITW) status(L"Only backgrounds can be renamed here.");
            return nm->code == LVN_BEGINLABELEDITW ? 2 : 3;  /* (cancel / keep the old name) */
        }
        if (nm->code == LVN_BEGINLABELEDITW) return 3;
        if (!d->item.pszText || !d->item.pszText[0]) return 3;  /* (Esc, or an empty name) */
        if (!mods_rename(s_mods[i].folder, d->item.pszText)) {
            status(L"It could not be renamed (is the game running?).");
            return 3;
        }
        wcsncpy_s(s_mods[i].name, 128, d->item.pszText, _TRUNCATE);
        status(L"Renamed: the game's BACKGROUNDS page shows the new name from the next start.");
        return 2;
    }
    if (nm->code == LVN_KEYDOWN && ((const NMLVKEYDOWN *)nm)->wVKey == 'A' && GetKeyState(VK_CONTROL) < 0) {
        ListView_SetItemState(s_list, -1, LVIS_SELECTED, LVIS_SELECTED);   /* (Ctrl+A: all) */
        return 1;
    }
    if (nm->code == LVN_ITEMCHANGED) {
        const NMLISTVIEW *lv = (const NMLISTVIEW *)nm;
        if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK))
        {
            const int i = (int)lv->lParam, was = s_mods[i].enabled;
            set_enabled(i, ListView_GetCheckState(s_list, lv->iItem));
            if (was != s_mods[i].enabled && !_wcsicmp(s_mods[i].type, L"superstar")) {
                const int out = s_stars_out;
                superstar_limit();
                if (out || s_stars_out) {  /* (the rows' text in place: no refill inside the notification) */
                    WCHAR t[160];
                    int k;
                    for (k = 0; k < s_nmods; k++)
                        if (!_wcsicmp(s_mods[k].type, L"superstar"))
                            ListView_SetItemText(s_list, k, 1, s_mods[k].left_out ? s_over_label : s_mods[k].type);
                    if (s_stars_out)
                        swprintf_s(t, 160, L"%d of %d superstar mods fit - %d aren't in the game (%d is the limit). "
                                           L"Switch some off to choose which play.", s_stars_on - s_stars_out, s_stars_on, s_stars_out, s_star_ids);
                    else
                        swprintf_s(t, 160, L"All %d superstar mods fit (%d is the limit). They load the next time the game starts.", s_stars_on, s_star_ids);
                    status(t);
                }
            }
        }
        return 1;
    }
    return 0;
}
