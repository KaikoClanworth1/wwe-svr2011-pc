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
 * switched on. A mod is turned off by a "disabled" file in its folder. A .svrmod file is a
 * zip of such a folder.
 *   +  installs a .svrmod/.zip   -  sends a mod's folder to the Recycle Bin
 */
#include "mods_tab.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <wchar.h>

#include "unzip.h"

enum { MODS_MAX = 512 };

typedef struct {
    WCHAR folder[MAX_PATH];  /* full path */
    WCHAR type[32], id[64], name[128], author[64], version[32];
    WCHAR made_with[64];  /* manifest made_with= (the Mod Maker stamps its mods); "" = not made with it */
    unsigned long long bytes;
    int enabled;
} Mod;

static Mod s_mods[MODS_MAX];
static int s_nmods;
static int s_base;                 /* first control id */
static HWND s_list, s_status, s_wnd;
static WCHAR s_game[MAX_PATH];
static int s_filling;              /* (checkbox notifications while filling the list) */

enum { M_LIST, M_ADD, M_REMOVE, M_FOLDER, M_MAKER, M_STATUS };

static void status(const WCHAR *t) { SetWindowTextW(s_status, t); }

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
    static const WCHAR *const types[] = { L"Arenas", L"Superstars", L"Signs", L"Media", L"Backstage", L"Moves", L"MatchTypes" };
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
        ListView_SetItemText(s_list, i, 1, s_mods[i].type);
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
        else {
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

static void add_mod(void)
{
    WCHAR zip[MAX_PATH];
    if (!s_game[0]) { status(L"Install the game first (Install tab)."); return; }
    if (pick_file(zip)) install_file(zip);
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
    if (changed && _wfopen_s(&f, list, L"w, ccs=UTF-8") == 0 && f) {
        fputws(known, f);
        fclose(f);
    }
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
        WCHAR need1[MAX_PATH], need2[MAX_PATH];
        if (signs || media || match_type) {
            wcsncpy_s(kind, 32, signs ? L"Signs" : media ? L"Media" : L"MatchTypes", _TRUNCATE);
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
                               L"move and match type mods.", m.name, m.type);
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
        if (exists(dst)) remove_tree(dst, 1);  /* an older version: to the Recycle Bin */
        if (!MoveFileW(tmp, dst)) {
            status(L"The mod could not be installed (is the game running?).");
            remove_tree(tmp, 0);
            return 0;
        }
        if (was_off) {
            FILE *f;
            if (_wfopen_s(&f, off, L"wb") == 0 && f) fclose(f);
        }
    }
    wcscpy_s(s_installed, MAX_PATH, dst);
    scan();
    fill();
    swprintf_s(t, 512, !wcscmp(kind, L"Backstage") ? L"Installed \"%s\". It is played in that room's backstage brawls."
                       : !wcscmp(kind, L"Media") ? L"Installed \"%s\". It takes effect the next time the game starts."
                       : !wcscmp(kind, L"Moves") ? L"Installed \"%s\". The game merges its moves into its files when it next starts (a few seconds)."
                       : !wcscmp(kind, L"MatchTypes") ? L"Installed \"%s\". The match type is in the game's menus the "
                                                        L"next time the game starts."
                       : !wcscmp(kind, L"Signs") ? L"Installed \"%s\". The crowd holds these signs up in every match."
                       : wcscmp(kind, L"Arenas") ? L"Installed \"%s\". It is under the M tile of the character "
                                                   L"select (up to 72 superstar mods)."
                                                 : L"Installed \"%s\". It is on the arena select pages after the "
                                                   L"game's own arenas.",
               m.name[0] ? m.name : m.id);
    status(t);
    return 1;
}

static int selected(void) { return ListView_GetNextItem(s_list, -1, LVNI_SELECTED); }
static void set_enabled(int i, int on);

static void remove_mod(void)
{
    WCHAR t[512];
    int i = selected();
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
    add(tab, L"Static", L"Arenas, superstars and more. + adds a .svrmod file, \x2212 removes the selected "
                        L"mod, the tick turns it on or off.", SS_LEFT, 28, 86, 560, 36, 0);
    s_list = add(tab, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
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
    if (maker_present()) {
        add(tab, L"Button", L"Open Mod Maker", BS_PUSHBUTTON | WS_TABSTOP, 28, 386, 170, 32, id_base + M_MAKER);
        add(tab, L"Button", L"Open Mods folder", BS_PUSHBUTTON | WS_TABSTOP, 206, 386, 150, 32, id_base + M_FOLDER);
    } else {
        add(tab, L"Button", L"Open Mods folder", BS_PUSHBUTTON | WS_TABSTOP, 28, 386, 150, 32, id_base + M_FOLDER);
    }
    s_status = add(tab, L"Static", L"", SS_LEFT, 28, 428, 560, 40, id_base + M_STATUS);
}

int mods_command(int id, int code)
{
    (void)code;
    if (id == s_base + M_ADD) { add_mod(); return 1; }
    if (id == s_base + M_REMOVE) { remove_mod(); return 1; }
    if (id == s_base + M_FOLDER) { open_folder(); return 1; }
    if (id == s_base + M_MAKER) { open_maker(); return 1; }
    return 0;
}

int mods_notify(const NMHDR *nm)
{
    if (!s_list || nm->hwndFrom != s_list || s_filling) return 0;
    if (nm->code == LVN_ITEMCHANGED) {
        const NMLISTVIEW *lv = (const NMLISTVIEW *)nm;
        if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK))
            set_enabled((int)lv->lParam, ListView_GetCheckState(s_list, lv->iItem));
        return 1;
    }
    return 0;
}
