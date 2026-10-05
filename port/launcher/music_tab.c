/* WWE SmackDown vs. Raw 2011 PC launcher - the Music tab.
 *
 * The game's Create An Entrance -> MUSIC -> USER PLAYLIST plays songs from
 * <game>\Music (src/music.cpp): each song there is a playlist of its own (by
 * its name), and each folder is a playlist (its first song). Media
 * Foundation plays them: .mp3, .wma, .m4a, .aac, .wav.
 *   Add songs...      copies songs into Music
 *   Add a playlist... copies a folder's songs into Music\<folder name>
 *   Remove            sends the selected songs / playlists to the Recycle Bin
 */
#include "music_tab.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

enum { MU_LIST, MU_ADD, MU_ADD_FOLDER, MU_REMOVE, MU_OPEN, MU_STATUS, MU_COUNT };
enum { MUSIC_MAX = 1024 };

typedef struct {
    WCHAR name[MAX_PATH];  /* file or folder name in Music */
    int folder;            /* a playlist folder */
    int songs;             /* (a folder: how many songs) */
    unsigned long long bytes;
} Entry;

static Entry s_entries[MUSIC_MAX];
static int s_n;
static int s_base;
static HWND s_wnd, s_list, s_status;
static WCHAR s_game[MAX_PATH];

static const WCHAR *const k_ext[] = { L".mp3", L".wma", L".m4a", L".aac", L".wav" };

static void status(const WCHAR *t) { SetWindowTextW(s_status, t); }

static int is_song(const WCHAR *name)
{
    const WCHAR *dot = wcsrchr(name, L'.');
    size_t i;
    if (!dot) return 0;
    for (i = 0; i < sizeof k_ext / sizeof k_ext[0]; i++)
        if (!_wcsicmp(dot, k_ext[i])) return 1;
    return 0;
}

static void music_dir(WCHAR *out)
{
    swprintf_s(out, MAX_PATH, L"%s\\Music", s_game);
}

static void fmt_bytes(WCHAR *out, size_t n, unsigned long long b)
{
    if (b >= 1024ull * 1024 * 1024) swprintf_s(out, n, L"%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024ull * 1024) swprintf_s(out, n, L"%.1f MB", b / (1024.0 * 1024));
    else swprintf_s(out, n, L"%llu KB", (b + 1023) / 1024);
}

static void scan(void)
{
    WCHAR dir[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    s_n = 0;
    if (!s_game[0]) return;
    music_dir(dir);
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        Entry *e;
        if (fd.cFileName[0] == L'.') continue;
        if (s_n >= MUSIC_MAX) break;
        e = &s_entries[s_n];
        ZeroMemory(e, sizeof *e);
        wcscpy_s(e->name, MAX_PATH, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            WCHAR sub[MAX_PATH];
            WIN32_FIND_DATAW f2;
            HANDLE h2;
            e->folder = 1;
            swprintf_s(sub, MAX_PATH, L"%s\\%s\\*", dir, fd.cFileName);
            h2 = FindFirstFileW(sub, &f2);
            if (h2 != INVALID_HANDLE_VALUE) {
                do {
                    if (!(f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && is_song(f2.cFileName)) {
                        e->songs++;
                        e->bytes += ((unsigned long long)f2.nFileSizeHigh << 32) | f2.nFileSizeLow;
                    }
                } while (FindNextFileW(h2, &f2));
                FindClose(h2);
            }
            s_n++;
        } else if (is_song(fd.cFileName)) {
            e->bytes = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            e->songs = 1;
            s_n++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void fill(void)
{
    int i;
    WCHAR t[64];
    ListView_DeleteAllItems(s_list);
    for (i = 0; i < s_n; i++) {
        const Entry *e = &s_entries[i];
        LVITEMW it;
        WCHAR name[MAX_PATH];
        wcscpy_s(name, MAX_PATH, e->name);
        if (!e->folder) {  /* (the playlist is the song's name, without the extension) */
            WCHAR *dot = wcsrchr(name, L'.');
            if (dot) *dot = 0;
        }
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = name;
        it.lParam = i;
        ListView_InsertItem(s_list, &it);
        if (e->folder) swprintf_s(t, 64, e->songs == 1 ? L"Folder (1 song)" : L"Folder (%d songs)", e->songs);
        else wcscpy_s(t, 64, L"Song");
        ListView_SetItemText(s_list, i, 1, t);
        fmt_bytes(t, 64, e->bytes);
        ListView_SetItemText(s_list, i, 2, t);
    }
    if (!s_game[0] || GetFileAttributesW(s_game) == INVALID_FILE_ATTRIBUTES) {
        status(L"Install the game first (Install tab).");
    } else if (!s_n) {
        status(L"No music yet. Add songs, then in the game: Create An Entrance \x2192 MUSIC \x2192 USER PLAYLIST.");
    } else {
        WCHAR m[256];
        swprintf_s(m, 256, L"%d playlist%s. In the game: Create An Entrance \x2192 MUSIC \x2192 USER PLAYLIST.", s_n,
                   s_n == 1 ? L"" : L"s");
        status(m);
    }
}

void music_show(const WCHAR *game_dir)
{
    wcsncpy_s(s_game, MAX_PATH, game_dir ? game_dir : L"", _TRUNCATE);
    scan();
    fill();
}

/* Copies one song into dir (a song of the same name is replaced). */
static int copy_song(const WCHAR *from, const WCHAR *dir)
{
    const WCHAR *base = wcsrchr(from, L'\\');
    WCHAR to[MAX_PATH];
    swprintf_s(to, MAX_PATH, L"%s\\%s", dir, base ? base + 1 : from);
    return CopyFileW(from, to, FALSE) != 0;
}

int music_add_files(const WCHAR *game_dir, const WCHAR *const *files, int n)
{
    WCHAR dir[MAX_PATH], m[256];
    int i, done = 0, skipped = 0;
    if (game_dir) wcsncpy_s(s_game, MAX_PATH, game_dir, _TRUNCATE);
    music_dir(dir);
    CreateDirectoryW(dir, NULL);
    for (i = 0; i < n; i++) {
        if (!is_song(files[i])) { skipped++; continue; }
        if (copy_song(files[i], dir)) done++;
        else skipped++;
    }
    scan();
    fill();
    if (skipped)
        swprintf_s(m, 256, L"Added %d song%s; %d not added (the game plays .mp3, .wma, .m4a, .aac and .wav).", done,
                   done == 1 ? L"" : L"s", skipped);
    else
        swprintf_s(m, 256, L"Added %d song%s. In the game: Create An Entrance \x2192 MUSIC \x2192 USER PLAYLIST.", done,
                   done == 1 ? L"" : L"s");
    status(m);
    return done;
}

static void add_songs(void)
{
    OPENFILENAMEW of;
    static WCHAR buf[32768];
    const WCHAR *files[512];
    WCHAR paths[512][MAX_PATH];
    int n = 0;
    buf[0] = 0;
    ZeroMemory(&of, sizeof of);
    of.lStructSize = sizeof of;
    of.hwndOwner = s_wnd;
    of.lpstrFilter = L"Music (*.mp3, *.wma, *.m4a, *.aac, *.wav)\0*.mp3;*.wma;*.m4a;*.aac;*.wav\0All files\0*.*\0";
    of.lpstrFile = buf;
    of.nMaxFile = sizeof buf / sizeof buf[0];
    of.lpstrTitle = L"Add songs to the game's Music folder";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&of)) return;
    /* One file: its path. Several: the folder, then the names (NUL separated). */
    if (buf[wcslen(buf) + 1] == 0) {
        wcscpy_s(paths[0], MAX_PATH, buf);
        files[n++] = paths[0];
    } else {
        const WCHAR *folder = buf, *p = buf + wcslen(buf) + 1;
        while (*p && n < 512) {
            swprintf_s(paths[n], MAX_PATH, L"%s\\%s", folder, p);
            files[n] = paths[n];
            n++;
            p += wcslen(p) + 1;
        }
    }
    music_add_files(NULL, files, n);
}

static void add_folder(void)
{
    IFileOpenDialog *d = NULL;
    IShellItem *item = NULL;
    PWSTR path = NULL;
    WCHAR dir[MAX_PATH], to[MAX_PATH], pat[MAX_PATH], m[256];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int done = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return;
    d->lpVtbl->SetOptions(d, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->lpVtbl->SetTitle(d, L"Choose a folder of songs: it becomes one playlist");
    if (SUCCEEDED(d->lpVtbl->Show(d, s_wnd)) && SUCCEEDED(d->lpVtbl->GetResult(d, &item)) &&
        SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
        const WCHAR *name = wcsrchr(path, L'\\');
        music_dir(dir);
        CreateDirectoryW(dir, NULL);
        swprintf_s(to, MAX_PATH, L"%s\\%s", dir, name ? name + 1 : path);
        CreateDirectoryW(to, NULL);
        swprintf_s(pat, MAX_PATH, L"%s\\*", path);
        h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                WCHAR from[MAX_PATH];
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !is_song(fd.cFileName)) continue;
                swprintf_s(from, MAX_PATH, L"%s\\%s", path, fd.cFileName);
                if (copy_song(from, to)) done++;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!done) RemoveDirectoryW(to);  /* (only if it's empty) */
        scan();
        fill();
        swprintf_s(m, 256, done ? L"Playlist \"%s\": %d song%s added." : L"No songs in \"%s\" (.mp3, .wma, .m4a, .aac, .wav).",
                   name ? name + 1 : path, done, done == 1 ? L"" : L"s");
        status(m);
        CoTaskMemFree(path);
    }
    if (item) item->lpVtbl->Release(item);
    d->lpVtbl->Release(d);
}

static void remove_selected(void)
{
    WCHAR dir[MAX_PATH], m[256];
    WCHAR *from;
    size_t used = 0, cap = 64 * 1024;
    int i = -1, count = 0;
    SHFILEOPSTRUCTW op;
    if (!ListView_GetSelectedCount(s_list)) {
        status(L"Select the songs or playlists to remove.");
        return;
    }
    music_dir(dir);
    from = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!from) return;
    while ((i = ListView_GetNextItem(s_list, i, LVNI_SELECTED)) >= 0) {
        LVITEMW it;
        WCHAR p[MAX_PATH];
        size_t n;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_PARAM;
        it.iItem = i;
        ListView_GetItem(s_list, &it);
        swprintf_s(p, MAX_PATH, L"%s\\%s", dir, s_entries[it.lParam].name);
        n = wcslen(p) + 1;
        if (used + n + 1 >= cap) break;
        memcpy(from + used, p, n * sizeof(WCHAR));
        used += n;
        count++;
    }
    ZeroMemory(&op, sizeof op);
    op.hwnd = s_wnd;
    op.wFunc = FO_DELETE;
    op.pFrom = from;  /* (double NUL terminated: calloc) */
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted)
        swprintf_s(m, 256, L"Removed %d (in the Recycle Bin).", count);
    else
        swprintf_s(m, 256, L"Could not remove them (is the game playing one?).");
    free(from);
    scan();
    fill();
    status(m);
}

static void open_folder(void)
{
    WCHAR dir[MAX_PATH];
    if (!s_game[0]) return;
    music_dir(dir);
    CreateDirectoryW(dir, NULL);
    ShellExecuteW(s_wnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

void music_build(HWND wnd, mods_add_fn add, int tab, int id_base)
{
    static const WCHAR *const names[] = { L"Playlist", L"Kind", L"Size" };
    static const int widths[] = { 300, 130, 78 };  /* (the list is 532 wide) */
    HDC dc;
    int i, dpi;
    s_wnd = wnd;
    s_base = id_base;
    dc = GetDC(wnd);
    dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(wnd, dc);
    add(tab, L"Static",
        L"Your own entrance music. Each song is a playlist of its own; a folder of songs is one playlist. In the game: "
        L"Create An Entrance \x2192 MUSIC \x2192 USER PLAYLIST.",
        SS_LEFT, 28, 10, 560, 40, 0);
    add(tab, L"Button", L"Songs", BS_GROUPBOX, 28, 60, 560, 380, 0);
    s_list = add(tab, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 42, 96, 532, 280,
                 id_base + MU_LIST);
    ListView_SetExtendedListViewStyle(s_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    for (i = 0; i < 3; i++) {
        LVCOLUMNW c;
        ZeroMemory(&c, sizeof c);
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = i == 2 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.cx = MulDiv(widths[i], dpi, 96);
        c.pszText = (WCHAR *)names[i];
        ListView_InsertColumn(s_list, i, &c);
    }
    add(tab, L"Button", L"Add songs\x2026", BS_DEFPUSHBUTTON | WS_TABSTOP, 42, 390, 130, 34, id_base + MU_ADD);
    add(tab, L"Button", L"Add a playlist folder\x2026", BS_PUSHBUTTON | WS_TABSTOP, 180, 390, 170, 34,
        id_base + MU_ADD_FOLDER);
    add(tab, L"Button", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, 358, 390, 90, 34, id_base + MU_REMOVE);
    add(tab, L"Button", L"Open folder", BS_PUSHBUTTON | WS_TABSTOP, 456, 390, 118, 34, id_base + MU_OPEN);
    s_status = add(tab, L"Static", L"", SS_LEFT | SS_NOPREFIX, 28, 452, 560, 40, id_base + MU_STATUS);
}

int music_command(int id, int code)
{
    (void)code;
    if (id == s_base + MU_ADD) { add_songs(); return 1; }
    if (id == s_base + MU_ADD_FOLDER) { add_folder(); return 1; }
    if (id == s_base + MU_REMOVE) { remove_selected(); return 1; }
    if (id == s_base + MU_OPEN) { open_folder(); return 1; }
    return 0;
}
