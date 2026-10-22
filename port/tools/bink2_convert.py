"""Bink 2 movies (.bk2: WWE '13 / WWE 2K14-2K17 titantrons) to .mp4, with a
decoder the PLAYER installed - opt in, nothing of RAD's ships with the port.

The port's movie tools read Bink 1 only; Bink 2 needs one of:
  - RAD Video Tools (free from RAD Game Tools, Windows x64:
    https://www.radgametools.com/bnkdown.htm): its command-line converter
    (radvideo64.exe BinkConv <in> <out>), run on a desktop of its own
    (CreateDesktop) - its progress and "Done!" windows never reach the
    player's screen (they showed even started hidden: 2026-10-13); the "Done!"
    window is closed by message, a stuck run is ended by its process handle.
    It writes raw-video AVIs (under 2 GB each: long movies in frame ranges,
    /S /E - 1-based, inclusive), which the port's ffmpeg joins and encodes.
    No desktop of its own: never on the player's - they convert it in RAD's
    window themselves (the manual message);
  - Librempeg (an FFmpeg fork with a Bink 2 decoder, GPLv3:
    https://github.com/librempeg/librempeg): its ffmpeg.exe. Mainline FFmpeg
    reads .bk2 files but has no Bink 2 video decoder, so an ffmpeg counts only
    when "-decoders" lists one.
Never a game's own bink2w64.dll (RAD's SDK, licensed to that game).

The .mp4 then goes through the port's HD movie path like any other (the Mod
Maker makes the 1024 .bik beside it; hd_movies.h).

Header: "KB2" + a revision letter. a-e are the early (2012) format with a
float IDCT - WWE '13 is likely one - that Librempeg may not read: those go
through RAD Video Tools when it's there (a warning otherwise). WWE 2K15-2K17's
are KB2i.

Commands (JSON on stdout; "progress <pct> <text>" lines on stderr):
    py bink2_convert.py find [--tool <path>]
        -> {ok, kind: "rad" | "librempeg", path}  or  {ok: false, error, download}
    py bink2_convert.py info <in.bk2>
        -> {ok, revision, frames, width, height, fps, warnings[]}
    py bink2_convert.py <in.bk2> <out.mp4> [--tool <path>]
        -> {ok, kind, revision, warnings[]}  or  {ok: false, error, warnings[], download?}
The tool: --tool, else SVR2011_BINK2_TOOL, else Librempeg's ffmpeg / RAD Video
Tools in their usual folders, the uninstall list, the Start menu and PATH. The
temporary AVIs go beside <out.mp4> (the Mod Maker's Mods\\.convert; SVR2011_BINK2_TEMP;
never a Downloads folder) and are always removed.
Exit codes: 0 done, 2 bad input (not a Bink 2 file, missing), 3 no tool or
the conversion failed (the import goes on without the titantron).
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import time

RAD_PAGE = "https://www.radgametools.com/bnkdown.htm"
NO_TOOL = ("Bink 2 movies need a decoder you install yourself: RAD Video Tools (free from RAD Game Tools, "
           + RAD_PAGE + ") or Librempeg's ffmpeg. Install one and set its path in the Mod Maker's settings "
           "(Enable Bink 2 conversion). The import goes on without the titantron.")
NO_FFMPEG = ("RAD Video Tools decoded the movie, but ffmpeg (the port's setup installs it) wasn't found to make "
             "the .mp4. The import goes on without the titantron.")
EARLY = "abcde"
# (no desktop of its own for RAD's windows: never the player's screen - they convert it themselves)
RAD_MANUAL = ("RAD Video Tools couldn't run in the background here. Convert this movie in RAD Video Tools "
              "yourself (select it, Convert a file, to an .avi), then pick that file in the Mod Maker. "
              "The import goes on without the titantron.")
RAD_CHUNK_BYTES = 1800 * 1000 * 1000  # (a raw 24-bit AVI part: RAD writes AVIs under 2 GB)
DEBUG = bool(os.environ.get("SVR2011_BINK2_DEBUG"))  # (the RAD windows seen on its desktop, on stderr)


def _hidden():
    """subprocess options so a console tool never shows a window (Windows: no
    console, SW_HIDE)."""
    if os.name != "nt":
        return {}
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 0  # SW_HIDE
    return {"startupinfo": si, "creationflags": subprocess.CREATE_NO_WINDOW}


def progress(pct, text):
    print("progress %d %s" % (pct, text), file=sys.stderr, flush=True)


def header(path):
    """The .bk2 header: {revision, frames, width, height, fps} or None."""
    try:
        with open(path, "rb") as f:
            h = f.read(44)
    except OSError:
        return None
    if len(h) < 36 or h[:3] != b"KB2":
        return None
    frames, = struct.unpack_from("<I", h, 8)
    width, height, num, den = struct.unpack_from("<IIII", h, 20)
    return {"revision": chr(h[3]), "frames": frames, "width": width, "height": height,
            "fps": round(num / den, 3) if den else 0}


def warnings_for(info):
    if info and info["revision"] in EARLY:
        return ["an early Bink 2 file (KB2%s, 2012): it may not convert" % info["revision"]]
    return []


def _ffmpeg_has_bink2(path):
    try:
        r = subprocess.run([path, "-hide_banner", "-decoders"], capture_output=True, text=True, timeout=30, **_hidden())
    except (OSError, subprocess.SubprocessError):
        return False
    return any("bink2" in line.lower() or "binkvideo2" in line.lower() for line in r.stdout.splitlines())


def _kind(path):
    """"rad" / "librempeg" for a usable tool at path, else None."""
    if not path or not os.path.isfile(path):
        return None
    name = os.path.basename(path).lower()
    if name.startswith("radvideo"):
        return "rad"
    if (name.startswith("ffmpeg") or name.startswith("librempeg")) and _ffmpeg_has_bink2(path):
        return "librempeg"
    return None


def _rad_installed():
    """radvideo64.exe beside RAD Video Tools' uninstall entry (its folder), if it's listed."""
    out = []
    if os.name != "nt":
        return out
    import winreg
    key = r"Software\Microsoft\Windows\CurrentVersion\Uninstall"
    for hive, view in ((winreg.HKEY_CURRENT_USER, 0), (winreg.HKEY_LOCAL_MACHINE, winreg.KEY_WOW64_64KEY),
                       (winreg.HKEY_LOCAL_MACHINE, winreg.KEY_WOW64_32KEY)):
        try:
            root = winreg.OpenKey(hive, key, 0, winreg.KEY_READ | view)
        except OSError:
            continue
        with root:
            for i in range(4096):
                try:
                    sub = winreg.EnumKey(root, i)
                except OSError:
                    break
                try:
                    with winreg.OpenKey(root, sub) as k:
                        name = str(winreg.QueryValueEx(k, "DisplayName")[0])
                        if "rad video" not in name.lower():
                            continue
                        for v in ("InstallLocation", "DisplayIcon", "UninstallString"):
                            try:
                                s = str(winreg.QueryValueEx(k, v)[0]).strip().strip('"').split('",')[0].split(",")[0]
                            except OSError:
                                continue
                            d = s if os.path.isdir(s) else os.path.dirname(s)
                            if d:
                                out.append(os.path.join(d, "radvideo64.exe"))
                except OSError:
                    continue
    return out


def _lnk_target(raw):
    """A shell link's local target (its LinkInfo: LocalBasePath + CommonPathSuffix, MS-SHLLINK 2.3), or ""."""
    try:
        if raw[:4] != b"\x4c\x00\x00\x00":
            return ""
        flags, = struct.unpack_from("<I", raw, 0x14)
        at = 0x4C
        if flags & 1:  # (HasLinkTargetIDList)
            at += 2 + struct.unpack_from("<H", raw, at)[0]
        if not flags & 2:  # (HasLinkInfo)
            return ""
        info_flags, = struct.unpack_from("<I", raw, at + 8)
        base_at, = struct.unpack_from("<I", raw, at + 16)
        suffix_at, = struct.unpack_from("<I", raw, at + 24)
        if not info_flags & 1:  # (VolumeIDAndLocalBasePath)
            return ""

        def cstr(o):
            return raw[o:raw.index(b"\x00", o)].decode("mbcs" if os.name == "nt" else "latin-1", "replace")
        return cstr(at + base_at) + cstr(at + suffix_at)
    except (struct.error, ValueError, IndexError):
        return ""


def _rad_shortcuts():
    """radvideo64.exe from RAD Video Tools' Start menu shortcuts (a .lnk's target, read from its bytes: the
    local path is kept both as ANSI and as UTF-16)."""
    import re
    out = []
    for base in (os.environ.get("APPDATA"), os.environ.get("ProgramData")):
        if not base:
            continue
        menu = os.path.join(base, "Microsoft", "Windows", "Start Menu", "Programs")
        for d, _, files in os.walk(menu):
            for f in files:
                if not (f.lower().endswith(".lnk") and "rad" in (d + f).lower()):
                    continue
                try:
                    with open(os.path.join(d, f), "rb") as fh:
                        raw = fh.read(65536)
                except OSError:
                    continue
                texts = [raw.decode("latin-1"), raw.decode("utf-16-le", "replace"), raw[1:].decode("utf-16-le", "replace")]
                texts.append(_lnk_target(raw))
                for t in texts:
                    for m in re.finditer(r"[A-Za-z]:\\[^\x00-\x1f\"*?<>|]*?\\radvideo(?:64)?\.exe", t, re.I):
                        out.append(os.path.join(os.path.dirname(m.group(0)), "radvideo64.exe"))
    return out


def _candidates(tool=None):
    c = []
    if tool:
        c.append(tool)
    if os.environ.get("SVR2011_BINK2_TOOL"):
        c.append(os.environ["SVR2011_BINK2_TOOL"])
    for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)"),
                 os.environ.get("ProgramW6432"), os.environ.get("LOCALAPPDATA")):
        if base:
            for sub in ("RADVideo", "RAD Video Tools", os.path.join("RAD Game Tools", "RADVideo"),
                        os.path.join("RAD Game Tools", "RAD Video Tools")):
                c.append(os.path.join(base, sub, "radvideo64.exe"))
    c += _rad_installed()
    c += _rad_shortcuts()
    for exe in ("librempeg.exe", "ffmpeg.exe", "radvideo64.exe"):
        found = shutil.which(exe)
        if found:
            c.append(found)
    out, seen = [], set()
    for p in c:
        key = os.path.normcase(os.path.abspath(p))
        if key not in seen:
            seen.add(key)
            out.append(p)
    return out


def find_tool(tool=None):
    """(kind, path): Librempeg's ffmpeg when there is one, else RAD Video Tools,
    else (None, None)."""
    found = [(k, c) for c in _candidates(tool) for k in [_kind(c)] if k]
    for k, c in found:
        if k == "librempeg":
            return k, c
    return found[0] if found else (None, None)


def rad_tool(tool=None):
    """RAD Video Tools' radvideo64.exe, if it's there (early files when the found tool is Librempeg)."""
    for c in _candidates(tool):
        if _kind(c) == "rad":
            return c
    return None


def ffmpeg_exe():
    """The port's ffmpeg (the Mod Maker puts the setup's first on PATH), for the AVI -> mp4 step."""
    found = shutil.which("ffmpeg.exe") or shutil.which("ffmpeg")
    if not found and os.path.isfile(r"C:\ffmpeg\bin\ffmpeg.exe"):
        found = r"C:\ffmpeg\bin\ffmpeg.exe"
    return found


# ---------------------------------------------------------------- RAD on a desktop of its own

def _desktop_run(cmd, timeout):
    """Runs cmd on a new desktop that is never shown; closes its "Done!" window; ends it (its own process
    handle) on a timeout or a message box. -> (ok, error, no_desktop)"""
    import ctypes
    from ctypes import wintypes as W
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    u32.CreateDesktopW.restype = W.HANDLE
    u32.CreateDesktopW.argtypes = [W.LPCWSTR, W.LPCWSTR, ctypes.c_void_p, W.DWORD, W.DWORD, ctypes.c_void_p]
    u32.CloseDesktop.argtypes = [W.HANDLE]
    u32.EnumDesktopWindows.argtypes = [W.HANDLE, ctypes.c_void_p, W.LPARAM]
    u32.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
    k32.WaitForSingleObject.argtypes = [W.HANDLE, W.DWORD]
    k32.TerminateProcess.argtypes = [W.HANDLE, W.UINT]
    k32.GetExitCodeProcess.argtypes = [W.HANDLE, ctypes.POINTER(W.DWORD)]
    k32.CloseHandle.argtypes = [W.HANDLE]
    name = "svr2011_bink2_%d_%d" % (os.getpid(), int(time.time() * 1000) % 100000000)
    desk = u32.CreateDesktopW(name, None, None, 0, 0x10000000, None)  # (GENERIC_ALL)
    if not desk:
        return False, "no desktop of its own (Windows error %d)" % ctypes.get_last_error(), True

    class SI(ctypes.Structure):
        _fields_ = [("cb", W.DWORD), ("lpReserved", W.LPWSTR), ("lpDesktop", W.LPWSTR), ("lpTitle", W.LPWSTR),
                    ("dwX", W.DWORD), ("dwY", W.DWORD), ("dwXSize", W.DWORD), ("dwYSize", W.DWORD),
                    ("dwXCountChars", W.DWORD), ("dwYCountChars", W.DWORD), ("dwFillAttribute", W.DWORD),
                    ("dwFlags", W.DWORD), ("wShowWindow", W.WORD), ("cbReserved2", W.WORD),
                    ("lpReserved2", ctypes.c_void_p), ("hStdInput", W.HANDLE), ("hStdOutput", W.HANDLE),
                    ("hStdError", W.HANDLE)]

    class PI(ctypes.Structure):
        _fields_ = [("hProcess", W.HANDLE), ("hThread", W.HANDLE), ("dwProcessId", W.DWORD), ("dwThreadId", W.DWORD)]

    si, pi = SI(), PI()
    si.cb = ctypes.sizeof(SI)
    si.lpDesktop = name
    line = ctypes.create_unicode_buffer(subprocess.list2cmdline(cmd))
    if not k32.CreateProcessW(None, line, None, None, False, 0x08000000, None, None, ctypes.byref(si),
                              ctypes.byref(pi)):
        err = ctypes.get_last_error()
        u32.CloseDesktop(desk)
        return False, "RAD Video Tools didn't start (Windows error %d)" % err, False
    ENUM = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)

    def text(h):
        b = ctypes.create_unicode_buffer(1024)
        u32.GetWindowTextW(h, b, 1024)
        return b.value

    def windows():
        found = []

        def top(h, _):
            pid = W.DWORD()
            u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if pid.value == pi.dwProcessId and u32.IsWindowVisible(h):
                kids = []

                def kid(c, _):
                    kids.append(text(c))
                    return True
                u32.EnumChildWindows(h, ENUM(kid), 0)
                found.append((h, text(h), kids))
            return True
        u32.EnumDesktopWindows(desk, ENUM(top), 0)
        return found

    ok, error, t0 = False, "", time.time()
    try:
        while True:
            if k32.WaitForSingleObject(pi.hProcess, 250) == 0:
                code = W.DWORD()
                k32.GetExitCodeProcess(pi.hProcess, ctypes.byref(code))
                ok = code.value == 0
                error = "" if ok else "RAD Video Tools stopped (exit %d)" % code.value
                break
            if time.time() - t0 > timeout:
                error = "RAD Video Tools took longer than %d s" % timeout
                break
            for h, title, kids in windows():
                if DEBUG:
                    print("%.1f s window %r %r" % (time.time() - t0, title, kids[:12]), file=sys.stderr, flush=True)
                if "done" in title.lower():
                    u32.PostMessageW(h, 0x0010, 0, 0)  # WM_CLOSE: the converter's finished window
                elif "OK" in [k.replace("&", "") for k in kids] and " Conversion Info: " not in kids:
                    msg = " ".join(k for k in kids if k and k.replace("&", "") not in ("OK", "Cancel", "Yes", "No"))
                    error = "RAD Video Tools said: " + (msg.split("\n")[0] if msg.startswith("Syntax") else msg)[:300]
                    break
            if error:
                break
    finally:
        if not ok and k32.WaitForSingleObject(pi.hProcess, 0) != 0:
            k32.TerminateProcess(pi.hProcess, 1)  # (this process only: its own handle)
            k32.WaitForSingleObject(pi.hProcess, 5000)
        k32.CloseHandle(pi.hProcess)
        k32.CloseHandle(pi.hThread)
        u32.CloseDesktop(desk)
    return ok, error, False


def _in_downloads(path):
    p = os.path.normcase(os.path.abspath(path))
    if os.path.normcase(os.path.join(os.path.expanduser("~"), "Downloads")) in p:
        return True
    return any(part.lower() == "downloads" for part in p.split(os.sep))


def work_dir(dst):
    """Where RAD's temporary AVIs go: SVR2011_BINK2_TEMP, else beside <out.mp4> (the Mod Maker's and
    entrance_port's Mods\\.convert) - never a Downloads folder (then the system temp folder). Parts a killed
    run left behind (older than a day) are removed."""
    d = os.environ.get("SVR2011_BINK2_TEMP") or os.path.dirname(os.path.abspath(dst)) or "."
    if _in_downloads(d):
        import tempfile
        d = os.path.join(tempfile.gettempdir(), "svr2011_bink2")
    os.makedirs(d, exist_ok=True)
    try:
        for f in os.listdir(d):
            p = os.path.join(d, f)
            if f.startswith(".bk2_") and time.time() - os.path.getmtime(p) > 86400:
                os.remove(p)
    except OSError:
        pass
    return d


def convert_rad(rad, src, dst, info, warnings):
    """src -> raw AVI parts (RAD, a desktop of its own) -> dst (ffmpeg H.264). The parts are removed whatever
    happens."""
    ff = ffmpeg_exe()
    if not ff:
        return 3, {"ok": False, "error": NO_FFMPEG, "warnings": warnings, "revision": info["revision"]}
    os.makedirs(os.path.dirname(os.path.abspath(dst)) or ".", exist_ok=True)
    work = work_dir(dst)
    frames = max(1, info["frames"])
    per = max(1, RAD_CHUNK_BYTES // max(1, info["width"] * info["height"] * 3))
    ranges = [(s, min(frames, s + per - 1)) for s in range(1, frames + 1, per)]  # (1-based, inclusive)
    stem = os.path.join(work, ".bk2_%d_%d" % (os.getpid(), int(time.time())))
    parts, listing, part_mp4 = [], stem + ".txt", dst + ".part.mp4"
    try:
        for n, (s, e) in enumerate(ranges):
            avi = os.path.normpath("%s_%02d.avi" % (stem, n))  # (backslashes: RAD reads "/..." as a switch)
            parts.append(avi)
            progress(5 + 70 * n // len(ranges), "decoding with RAD Video Tools (%d of %d)" % (n + 1, len(ranges)))
            cmd = [rad, "BinkConv", os.path.normpath(os.path.abspath(src)), avi]  # (no /O: the part is always new)
            if len(ranges) > 1:
                cmd += ["/S%d" % s, "/E%d" % e]
            ok, err, no_desktop = _desktop_run(cmd, 120 + (e - s + 1) // 2)
            if no_desktop:
                return 3, {"ok": False, "error": RAD_MANUAL, "manual": True, "download": RAD_PAGE, "detail": err,
                           "warnings": warnings, "revision": info["revision"]}
            if not ok or not os.path.isfile(avi) or os.path.getsize(avi) == 0:
                return 3, {"ok": False, "error": "RAD Video Tools couldn't decode it%s. The import goes on without "
                           "the titantron." % ((": " + err) if err else ""), "warnings": warnings,
                           "revision": info["revision"]}
        progress(80, "making the .mp4")
        with open(listing, "w", encoding="utf-8") as f:
            for p in parts:
                f.write("file '%s'\n" % p.replace("\\", "/").replace("'", "'\\''"))
        if os.path.exists(part_mp4):
            os.remove(part_mp4)
        r = subprocess.run([ff, "-hide_banner", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0", "-i", listing,
                            "-an", "-c:v", "libx264", "-preset", "fast", "-pix_fmt", "yuv420p", "-crf", "18", "-f", "mp4", part_mp4],
                           capture_output=True, text=True, timeout=3600, **_hidden())
        if not (os.path.isfile(part_mp4) and os.path.getsize(part_mp4) > 0):
            return 3, {"ok": False, "error": "ffmpeg couldn't make the .mp4: %s. The import goes on without the "
                       "titantron." % (r.stderr or r.stdout or "").strip()[-300:], "warnings": warnings,
                       "revision": info["revision"]}
        os.replace(part_mp4, dst)
    except (OSError, subprocess.SubprocessError) as e:
        return 3, {"ok": False, "error": "the conversion failed: %s. The import goes on without the titantron." % e,
                   "warnings": warnings, "revision": info["revision"]}
    finally:
        for p in parts + [listing, part_mp4]:
            try:
                if os.path.exists(p):
                    os.remove(p)
            except OSError:
                pass
    progress(100, "done")
    return 0, {"ok": True, "kind": "rad", "tool": rad, "revision": info["revision"], "warnings": warnings,
               "frames": info["frames"], "fps": info["fps"], "parts": len(ranges)}


def convert(src, dst, tool=None):
    info = header(src)
    if not info:
        return 2, {"ok": False, "error": "not a Bink 2 movie (no KB2 header): " + src, "warnings": []}
    warnings = warnings_for(info)
    kind, path = find_tool(tool)
    # (an early file - KB2a-e, WWE '13's - Librempeg may not read: RAD Video Tools when it's there)
    if info["revision"] in EARLY and kind == "librempeg":
        rad = rad_tool(tool)
        if rad:
            kind, path = "rad", rad
    if kind == "rad":
        return convert_rad(path, src, dst, info, warnings)
    if not kind:
        return 3, {"ok": False, "error": NO_TOOL, "download": RAD_PAGE, "warnings": warnings,
                   "revision": info["revision"]}
    os.makedirs(os.path.dirname(os.path.abspath(dst)) or ".", exist_ok=True)
    part = dst + ".part.mp4"
    if os.path.exists(part):
        os.remove(part)
    progress(5, "converting with Librempeg")
    attempts = ([[path, "-hide_banner", "-loglevel", "error", "-y", "-i", src, "-an", "-c:v", "libx264",
                  "-pix_fmt", "yuv420p", "-crf", "18", part],
                 [path, "-hide_banner", "-loglevel", "error", "-y", "-i", src, "-an", "-c:v", "mpeg4",
                  "-q:v", "2", part]])
    err = ""
    for cmd in attempts:
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=3600, **_hidden())
            err = (r.stderr or r.stdout or "").strip()[-400:]
        except (OSError, subprocess.SubprocessError) as e:
            err = str(e)
        if os.path.isfile(part) and os.path.getsize(part) > 0:
            break
    if not (os.path.isfile(part) and os.path.getsize(part) > 0):
        msg = "the %s conversion failed%s" % (kind, (": " + err) if err else "")
        if info["revision"] in EARLY:
            msg += " (an early Bink 2 file - the decoder may not read it)"
        return 3, {"ok": False, "error": msg + ". The import goes on without the titantron.",
                   "warnings": warnings, "revision": info["revision"]}
    os.replace(part, dst)
    progress(100, "done")
    return 0, {"ok": True, "kind": kind, "tool": path, "revision": info["revision"], "warnings": warnings,
               "frames": info["frames"], "fps": info["fps"]}


def main(argv):
    args = [a for a in argv[1:]]
    tool = None
    if "--tool" in args:
        i = args.index("--tool")
        tool = args[i + 1] if i + 1 < len(args) else None
        del args[i:i + 2]
    if args[:1] == ["find"]:
        kind, path = find_tool(tool)
        out = {"ok": True, "kind": kind, "path": path} if kind else \
            {"ok": False, "error": NO_TOOL, "download": RAD_PAGE}
        print(json.dumps(out))
        return 0 if kind else 3
    if args[:1] == ["info"] and len(args) == 2:
        info = header(args[1])
        if not info:
            print(json.dumps({"ok": False, "error": "not a Bink 2 movie: " + args[1]}))
            return 2
        print(json.dumps(dict(ok=True, warnings=warnings_for(info), **info)))
        return 0
    if len(args) == 2:
        code, out = convert(args[0], args[1], tool)
        print(json.dumps(out))
        return code
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
