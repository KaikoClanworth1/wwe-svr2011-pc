/* WWE SmackDown vs. Raw 2011 PC launcher - the Texture packs tab.
 *
 * Texture packs (src/native/texture_packs.h) replace the game's textures:
 * folders in <game>\Texture Packs holding PNG / DDS files named after the
 * game's textures (..._<16 hex digits>.png). Ticked packs are used the next
 * time the game starts; a pack higher in the list wins over the ones below
 * (svr2011.toml native_texture_packs = "first;second"). For pack makers,
 * "Dump textures" (native_dump_textures) has the game write every texture
 * it shows to <game>\Texture Dumps, named after the game's own textures.
 *   Add a pack folder... / Add a .zip...   copy or extract into Texture Packs
 *   Remove                                 sends the pack to the Recycle Bin
 *   Up / Down                              the pack's priority
 */
#include "texpacks_tab.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "unzip.h"

enum { TP_LIST, TP_ADD_FOLDER, TP_ADD_ZIP, TP_REMOVE, TP_UP, TP_DOWN, TP_OPEN, TP_DUMP, TP_OPEN_DUMPS, TP_STATUS,
       TP_COUNT };
enum { PACKS_MAX = 256 };

typedef struct {
    WCHAR name[MAX_PATH];
    int files;
    unsigned long long bytes;
    int on;
} Pack;

static Pack s_packs[PACKS_MAX];
static int s_n;
static int s_base;
static int s_filling;
static HWND s_wnd, s_list, s_status, s_dump;
static WCHAR s_game[MAX_PATH];
static texpacks_get_fn s_get;
static texpacks_set_fn s_set;

static void status(const WCHAR *t) { SetWindowTextW(s_status, t); }

static void packs_dir(WCHAR *out) { swprintf_s(out, MAX_PATH, L"%s\\Texture Packs", s_game); }

static int is_image(const WCHAR *name)
{
    const WCHAR *dot = wcsrchr(name, L'.');
    return dot && (!_wcsicmp(dot, L".png") || !_wcsicmp(dot, L".dds"));
}

static void count_files(const WCHAR *dir, int *files, unsigned long long *bytes, int depth)
{
    WCHAR pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            WCHAR sub[MAX_PATH];
            if (depth < 8) {
                swprintf_s(sub, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
                count_files(sub, files, bytes, depth + 1);
            }
        } else if (is_image(fd.cFileName)) {
            (*files)++;
            *bytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void fmt_bytes(WCHAR *out, size_t n, unsigned long long b)
{
    if (b >= 1024ull * 1024 * 1024) swprintf_s(out, n, L"%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024ull * 1024) swprintf_s(out, n, L"%.1f MB", b / (1024.0 * 1024));
    else swprintf_s(out, n, L"%llu KB", (b + 1023) / 1024);
}

static int cmp_name(const void *a, const void *b)
{
    return _wcsicmp(((const Pack *)a)->name, ((const Pack *)b)->name);
}

/* The packs on disk: the enabled ones first, in svr2011.toml's order, then
   the rest by name. */
static void scan(void)
{
    WCHAR dir[MAX_PATH], pat[MAX_PATH], wanted[4096];
    char v[4096];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int i, first_off;
    s_n = 0;
    if (!s_game[0]) return;
    packs_dir(dir);
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            Pack *p;
            if (fd.cFileName[0] == L'.' || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || s_n >= PACKS_MAX)
                continue;
            p = &s_packs[s_n++];
            ZeroMemory(p, sizeof *p);
            wcscpy_s(p->name, MAX_PATH, fd.cFileName);
            swprintf_s(pat, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            count_files(pat, &p->files, &p->bytes, 0);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    qsort(s_packs, (size_t)s_n, sizeof s_packs[0], cmp_name);
    v[0] = 0;
    if (s_get) s_get("native_texture_packs", v, sizeof v);
    MultiByteToWideChar(CP_UTF8, 0, v, -1, wanted, 4096);
    /* Move the enabled ones to the front, in the saved order. */
    first_off = 0;
    {
        WCHAR *ctx = NULL, *tok = wcstok_s(wanted, L";", &ctx);
        while (tok) {
            while (*tok == L' ') tok++;
            for (i = first_off; i < s_n; i++) {
                if (!_wcsicmp(s_packs[i].name, tok)) {
                    Pack t = s_packs[i];
                    memmove(&s_packs[first_off + 1], &s_packs[first_off], (size_t)(i - first_off) * sizeof(Pack));
                    s_packs[first_off] = t;
                    s_packs[first_off].on = 1;
                    first_off++;
                    break;
                }
            }
            tok = wcstok_s(NULL, L";", &ctx);
        }
    }
}

static void save(void)
{
    char v[4096] = "\"", name[MAX_PATH * 3];
    int i, any = 0, on = 0;
    for (i = 0; i < s_n; i++) {
        char *q;
        if (!s_packs[i].on) continue;
        WideCharToMultiByte(CP_UTF8, 0, s_packs[i].name, -1, name, sizeof name, NULL, NULL);
        for (q = name; *q; q++)
            if (*q == '"' || *q == '\\' || *q == ';') *q = '_';
        if (strlen(v) + strlen(name) + 3 >= sizeof v) break;
        if (any) strcat_s(v, sizeof v, ";");
        strcat_s(v, sizeof v, name);
        any = 1;
        on++;
    }
    strcat_s(v, sizeof v, "\"");
    if (!s_set || !s_set("native_texture_packs", v)) {
        status(L"Could not save the setting (is the game folder writable?).");
        return;
    }
    {
        WCHAR m[256];
        if (on)
            swprintf_s(m, 256, L"%d pack%s on - used the next time the game starts. A pack higher in the list wins.", on,
                       on == 1 ? L"" : L"s");
        else
            wcscpy_s(m, 256, L"No packs on: the game's own textures.");
        status(m);
    }
}

static void fill(int select)
{
    int i;
    WCHAR t[64];
    s_filling = 1;
    ListView_DeleteAllItems(s_list);
    for (i = 0; i < s_n; i++) {
        LVITEMW it;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = s_packs[i].name;
        it.lParam = i;
        ListView_InsertItem(s_list, &it);
        ListView_SetCheckState(s_list, i, s_packs[i].on);
        swprintf_s(t, 64, L"%d", s_packs[i].files);
        ListView_SetItemText(s_list, i, 1, t);
        fmt_bytes(t, 64, s_packs[i].bytes);
        ListView_SetItemText(s_list, i, 2, t);
    }
    if (select >= 0 && select < s_n) {
        ListView_SetItemState(s_list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(s_list, select, FALSE);
    }
    s_filling = 0;
    if (!s_game[0] || GetFileAttributesW(s_game) == INVALID_FILE_ATTRIBUTES)
        status(L"Install the game first (Install tab).");
    else if (!s_n)
        status(L"No texture packs yet. Add a pack folder or a .zip; tick it to use it.");
}

void texpacks_show(const WCHAR *game_dir)
{
    char v[32] = "";
    wcsncpy_s(s_game, MAX_PATH, game_dir ? game_dir : L"", _TRUNCATE);
    scan();
    fill(-1);
    if (s_get) s_get("native_dump_textures", v, sizeof v);
    SendMessageW(s_dump, BM_SETCHECK, !strcmp(v, "true") ? BST_CHECKED : BST_UNCHECKED, 0);
    if (s_n) status(L"Ticked packs are used the next time the game starts. A pack higher in the list wins.");
}

static int selected(void) { return ListView_GetNextItem(s_list, -1, LVNI_SELECTED); }

/* A new pack's folder name in Texture Packs (name, name (2)...). */
static void new_pack_dir(const WCHAR *name, WCHAR *out)
{
    WCHAR dir[MAX_PATH];
    int k;
    packs_dir(dir);
    CreateDirectoryW(dir, NULL);
    swprintf_s(out, MAX_PATH, L"%s\\%s", dir, name);
    for (k = 2; GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES && k < 100; k++)
        swprintf_s(out, MAX_PATH, L"%s\\%s (%d)", dir, name, k);
}

static void turn_on(const WCHAR *dir)
{
    const WCHAR *name = wcsrchr(dir, L'\\');
    int i;
    scan();
    for (i = 0; i < s_n; i++)
        if (!_wcsicmp(s_packs[i].name, name ? name + 1 : dir)) {
            /* (a new pack goes on top: it's the one the player wants to see) */
            Pack t = s_packs[i];
            memmove(&s_packs[1], &s_packs[0], (size_t)i * sizeof(Pack));
            s_packs[0] = t;
            s_packs[0].on = 1;
        }
    save();
    fill(0);
}

int texpacks_add(const WCHAR *game_dir, const WCHAR *path)
{
    WCHAR to[MAX_PATH], name[MAX_PATH], m[MAX_PATH + 128];
    const WCHAR *base;
    const WCHAR *dot;
    int ok;
    if (game_dir) wcsncpy_s(s_game, MAX_PATH, game_dir, _TRUNCATE);
    if (!s_game[0]) return 0;
    base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    wcscpy_s(name, MAX_PATH, base);
    dot = wcsrchr(name, L'.');
    if (GetFileAttributesW(path) & FILE_ATTRIBUTE_DIRECTORY) {
        SHFILEOPSTRUCTW op;
        WCHAR from[MAX_PATH + 2] = { 0 }, dest[MAX_PATH + 2] = { 0 };
        new_pack_dir(name, to);
        swprintf_s(from, MAX_PATH, L"%s\\*", path);
        wcscpy_s(dest, MAX_PATH, to);
        CreateDirectoryW(to, NULL);
        ZeroMemory(&op, sizeof op);
        op.hwnd = s_wnd;
        op.wFunc = FO_COPY;
        op.pFrom = from;  /* (double NUL terminated) */
        op.pTo = dest;
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_SILENT | FOF_NOERRORUI;
        ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    } else if (dot && !_wcsicmp(dot, L".zip")) {
        name[dot - name] = 0;
        new_pack_dir(name, to);
        CreateDirectoryW(to, NULL);
        ok = unzip_file(path, to);
    } else {
        status(L"A texture pack is a folder or a .zip of PNG / DDS files.");
        return 0;
    }
    if (!ok) {
        RemoveDirectoryW(to);  /* (only if it's empty) */
        swprintf_s(m, MAX_PATH + 128, L"Could not add \"%s\".", base);
        status(m);
        return 0;
    }
    turn_on(to);
    {
        const Pack *p = &s_packs[0];
        swprintf_s(m, MAX_PATH + 128, L"Added \"%s\" (%d texture files) and turned it on - used the next time the game starts.",
                   p->name, p->files);
        status(m);
    }
    return 1;
}

static void add_folder(void)
{
    IFileOpenDialog *d = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return;
    d->lpVtbl->SetOptions(d, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->lpVtbl->SetTitle(d, L"Choose a texture pack folder (PNG / DDS files)");
    if (SUCCEEDED(d->lpVtbl->Show(d, s_wnd)) && SUCCEEDED(d->lpVtbl->GetResult(d, &item)) &&
        SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
        texpacks_add(NULL, path);
        CoTaskMemFree(path);
    }
    if (item) item->lpVtbl->Release(item);
    d->lpVtbl->Release(d);
}

static void add_zip(void)
{
    OPENFILENAMEW of;
    WCHAR buf[MAX_PATH] = L"";
    ZeroMemory(&of, sizeof of);
    of.lStructSize = sizeof of;
    of.hwndOwner = s_wnd;
    of.lpstrFilter = L"Texture pack (*.zip)\0*.zip\0";
    of.lpstrFile = buf;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = L"Add a texture pack (.zip)";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&of)) texpacks_add(NULL, buf);
}

static void remove_selected(void)
{
    WCHAR dir[MAX_PATH], from[MAX_PATH + 2] = { 0 }, m[MAX_PATH + 64];
    SHFILEOPSTRUCTW op;
    const int i = selected();
    if (i < 0) {
        status(L"Select the pack to remove.");
        return;
    }
    packs_dir(dir);
    swprintf_s(from, MAX_PATH, L"%s\\%s", dir, s_packs[i].name);
    ZeroMemory(&op, sizeof op);
    op.hwnd = s_wnd;
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) {
        swprintf_s(m, MAX_PATH + 64, L"Removed \"%s\" (in the Recycle Bin).", s_packs[i].name);
        s_packs[i].on = 0;
        scan();
        save();
        fill(-1);
        status(m);
    } else {
        status(L"Could not remove it (is the game running?).");
    }
}

static void move(int dir)
{
    const int i = selected(), j = i + dir;
    Pack t;
    if (i < 0 || j < 0 || j >= s_n) return;
    t = s_packs[i];
    s_packs[i] = s_packs[j];
    s_packs[j] = t;
    fill(j);
    save();
}

static void open_dir(const WCHAR *sub)
{
    WCHAR dir[MAX_PATH];
    if (!s_game[0]) return;
    swprintf_s(dir, MAX_PATH, L"%s\\%s", s_game, sub);
    CreateDirectoryW(dir, NULL);
    ShellExecuteW(s_wnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

void texpacks_build(HWND wnd, mods_add_fn add, int tab, int id_base, texpacks_get_fn get, texpacks_set_fn set)
{
    static const WCHAR *const names[] = { L"Pack", L"Textures", L"Size" };
    static const int widths[] = { 330, 100, 78 };  /* (the list is 532 wide) */
    HDC dc;
    int i, dpi;
    s_wnd = wnd;
    s_base = id_base;
    s_get = get;
    s_set = set;
    dc = GetDC(wnd);
    dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(wnd, dc);
    add(tab, L"Static",
        L"Texture packs replace the game's textures with new ones, at any resolution. Tick a pack to use it (from the "
        L"next start); a pack higher in the list wins.",
        SS_LEFT, 28, 10, 560, 40, 0);
    add(tab, L"Button", L"Packs", BS_GROUPBOX, 28, 52, 560, 300, 0);
    s_list = add(tab, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | WS_BORDER | WS_TABSTOP, 42,
                 76, 532, 214, id_base + TP_LIST);
    ListView_SetExtendedListViewStyle(s_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
    for (i = 0; i < 3; i++) {
        LVCOLUMNW c;
        ZeroMemory(&c, sizeof c);
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = i ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.cx = MulDiv(widths[i], dpi, 96);
        c.pszText = (WCHAR *)names[i];
        ListView_InsertColumn(s_list, i, &c);
    }
    add(tab, L"Button", L"Add a pack folder\x2026", BS_PUSHBUTTON | WS_TABSTOP, 42, 300, 140, 34,
        id_base + TP_ADD_FOLDER);
    add(tab, L"Button", L"Add a .zip\x2026", BS_PUSHBUTTON | WS_TABSTOP, 188, 300, 100, 34, id_base + TP_ADD_ZIP);
    add(tab, L"Button", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, 294, 300, 70, 34, id_base + TP_REMOVE);
    add(tab, L"Button", L"Up", BS_PUSHBUTTON | WS_TABSTOP, 370, 300, 44, 34, id_base + TP_UP);
    add(tab, L"Button", L"Down", BS_PUSHBUTTON | WS_TABSTOP, 418, 300, 52, 34, id_base + TP_DOWN);
    add(tab, L"Button", L"Open folder", BS_PUSHBUTTON | WS_TABSTOP, 476, 300, 98, 34, id_base + TP_OPEN);
    add(tab, L"Button", L"Making a pack", BS_GROUPBOX, 28, 360, 560, 92, 0);
    s_dump = add(tab, L"Button",
                 L"Dump textures: the game writes every texture it shows to Texture Dumps, as PNG named after the "
                 L"game's own textures",
                 BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, 42, 380, 400, 40, id_base + TP_DUMP);
    add(tab, L"Button", L"Open Texture Dumps", BS_PUSHBUTTON | WS_TABSTOP, 450, 382, 124, 34, id_base + TP_OPEN_DUMPS);
    add(tab, L"Static",
        L"Edit a dumped PNG, keep its name, and put it in a pack folder (subfolders are fine).",
        SS_LEFT, 42, 424, 530, 20, 0);
    s_status = add(tab, L"Static", L"", SS_LEFT | SS_NOPREFIX, 28, 460, 560, 40, id_base + TP_STATUS);
}

int texpacks_command(int id, int code)
{
    (void)code;
    if (id == s_base + TP_ADD_FOLDER) { add_folder(); return 1; }
    if (id == s_base + TP_ADD_ZIP) { add_zip(); return 1; }
    if (id == s_base + TP_REMOVE) { remove_selected(); return 1; }
    if (id == s_base + TP_UP) { move(-1); return 1; }
    if (id == s_base + TP_DOWN) { move(1); return 1; }
    if (id == s_base + TP_OPEN) { open_dir(L"Texture Packs"); return 1; }
    if (id == s_base + TP_OPEN_DUMPS) { open_dir(L"Texture Dumps"); return 1; }
    if (id == s_base + TP_DUMP) {
        const int on = SendMessageW(s_dump, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (s_set && s_set("native_dump_textures", on ? "true" : "false"))
            status(on ? L"Dumping on: the game writes the textures it shows to Texture Dumps (from the next start)."
                      : L"Dumping off.");
        return 1;
    }
    return 0;
}

int texpacks_notify(const NMHDR *nm)
{
    if (nm->hwndFrom != s_list || nm->code != LVN_ITEMCHANGED || s_filling) return 0;
    {
        const NMLISTVIEW *v = (const NMLISTVIEW *)nm;
        /* (the checkbox is the state image: 1 off, 2 on) */
        if ((v->uChanged & LVIF_STATE) && ((v->uNewState ^ v->uOldState) & LVIS_STATEIMAGEMASK) && v->iItem >= 0 &&
            v->iItem < s_n && (v->uOldState & LVIS_STATEIMAGEMASK)) {
            s_packs[v->iItem].on = ListView_GetCheckState(s_list, v->iItem) ? 1 : 0;
            save();
        }
    }
    return 1;
}
