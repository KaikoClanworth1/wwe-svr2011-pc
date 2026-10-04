/* WWE SmackDown vs. Raw 2011 PC launcher - the Mods tab (arenas branch).
 *
 * Mods live in <game>/Mods/<Type>/<id>/ with a manifest.txt (key=value:
 * type, id, name, author, version). Arena mods hold arena.pac and banner.dds
 * (256 x 128 DXT5); the game lists them on the arena select pages after its
 * own arenas (src/arena_mods.cpp). Superstar mods hold ch.pac (and maybe a
 * theme song and an entrance movie); the game lists them under the M tile of
 * the character select (src/superstar_mods.cpp). Sign packs hold crowd signs
 * (*.dds, 128 x 64) the crowd holds up (src/crowd_signs.cpp); media packs replace
 * the game's videos, renders, arena screens and sounds (src/media_mods.cpp). A mod is turned off by a
 * "disabled" file in its folder. A .svrmod file is a zip of such a folder.
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

static void scan(void)
{
    static const WCHAR *const types[] = { L"Arenas", L"Superstars", L"Signs", L"Media", L"Backstage" };
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
        ListView_SetCheckState(s_list, i, s_mods[i].enabled);
    }
    s_filling = 0;
    {
        WCHAR t[160];
        if (!s_game[0]) status(L"Install the game first (Install tab).");
        else if (!s_nmods) status(L"No mods yet. + adds a .svrmod file; Open Mod Maker makes one.");
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

int mods_install(const WCHAR *game_dir, const WCHAR *zip)
{
    wcsncpy_s(s_game, MAX_PATH, game_dir ? game_dir : L"", _TRUNCATE);
    return s_game[0] && install_file(zip);
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
        WCHAR need1[MAX_PATH], need2[MAX_PATH];
        if (signs || media) {
            wcsncpy_s(kind, 32, signs ? L"Signs" : L"Media", _TRUNCATE);
            goto place;
        }
        if (!arena && !star) {
            swprintf_s(t, 512, L"\"%s\" is a %s mod; this version installs arena, superstar and sign mods.", m.name, m.type);
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
    if (exists(dst)) remove_tree(dst, 1);  /* an older version: to the Recycle Bin */
    if (!MoveFileW(tmp, dst)) {
        status(L"The mod could not be installed (is the game running?).");
        remove_tree(tmp, 0);
        return 0;
    }
    scan();
    fill();
    swprintf_s(t, 512, !wcscmp(kind, L"Backstage") ? L"Installed \"%s\". It is played in that room's backstage brawls."
                       : !wcscmp(kind, L"Media") ? L"Installed \"%s\". It takes effect the next time the game starts."
                       : !wcscmp(kind, L"Signs") ? L"Installed \"%s\". The crowd holds these signs up in every match."
                       : wcscmp(kind, L"Arenas") ? L"Installed \"%s\". It is under the M tile of the character "
                                                   L"select (up to 50 superstar mods)."
                                                 : L"Installed \"%s\". It is on the arena select pages after the "
                                                   L"game's own arenas.",
               m.name[0] ? m.name : m.id);
    status(t);
    return 1;
}

static int selected(void) { return ListView_GetNextItem(s_list, -1, LVNI_SELECTED); }

static void remove_mod(void)
{
    WCHAR t[512];
    int i = selected();
    if (i < 0 || i >= s_nmods) { status(L"Select a mod in the list first."); return; }
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
    static const WCHAR *const names[] = { L"Name", L"Type", L"Version", L"Author", L"Size" };
    static const int widths[] = { 196, 60, 62, 110, 62 };  /* (the list is 512 wide) */
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
    add(tab, L"Static", L"Arenas and more made with the Mod Maker. + adds a .svrmod file, \x2212 removes the selected "
                        L"mod, the tick turns it on or off.", SS_LEFT, 28, 86, 560, 36, 0);
    s_list = add(tab, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                 28, 126, 512, 250, id_base + M_LIST);
    ListView_SetExtendedListViewStyle(s_list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
    for (i = 0; i < 5; i++) {
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
    add(tab, L"Button", L"Open Mod Maker", BS_PUSHBUTTON | WS_TABSTOP, 28, 386, 170, 32, id_base + M_MAKER);
    add(tab, L"Button", L"Open Mods folder", BS_PUSHBUTTON | WS_TABSTOP, 206, 386, 150, 32, id_base + M_FOLDER);
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
