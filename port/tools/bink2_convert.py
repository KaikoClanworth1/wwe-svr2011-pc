"""Bink 2 movies (.bk2: WWE '13 / WWE 2K14 titantrons) to .mp4, with a
decoder the PLAYER installed - opt in, nothing of RAD's ships with the port.

The port's movie tools read Bink 1 only; Bink 2 needs one of:
  - RAD Video Tools (free from RAD Game Tools, Windows x64:
    https://www.radgametools.com/bnkdown.htm): radvideo64.exe's converter,
    "radvideo64.exe binkconv <in> <out.mp4> /#" (/#: no Done button); its
    MP4 export goes through Windows Media Foundation (H.264);
  - Librempeg (an FFmpeg fork with a Bink 2 decoder, GPLv3:
    https://github.com/librempeg/librempeg): its ffmpeg.exe. Mainline FFmpeg
    reads .bk2 files but has no Bink 2 video decoder, so an ffmpeg counts only
    when "-decoders" lists one.
Never a game's own bink2w64.dll (RAD's SDK, licensed to that game).

The .mp4 then goes through the port's HD movie path like any other (the Mod
Maker makes the 1024 .bik beside it; hd_movies.h).

Header: "KB2" + a revision letter. a-e are the early (2012) format with a
float IDCT - WWE '13 is likely one - that today's decoders may not read:
those get a warning, not a refusal.

Commands (JSON on stdout; "progress <pct> <text>" lines on stderr):
    py bink2_convert.py find [--tool <path>]
        -> {ok, kind: "rad" | "librempeg", path}  or  {ok: false, error, download}
    py bink2_convert.py info <in.bk2>
        -> {ok, revision, frames, width, height, fps, warnings[]}
    py bink2_convert.py <in.bk2> <out.mp4> [--tool <path>]
        -> {ok, kind, revision, warnings[]}  or  {ok: false, error, warnings[], download?}
The tool: --tool, else SVR2011_BINK2_TOOL, else RAD Video Tools' usual
folders, else radvideo64.exe / ffmpeg.exe on PATH.
Exit codes: 0 done, 2 bad input (not a Bink 2 file, missing), 3 no tool or
the conversion failed (the import goes on without the titantron).
"""
import json
import os
import shutil
import struct
import subprocess
import sys

RAD_PAGE = "https://www.radgametools.com/bnkdown.htm"
NO_TOOL = ("Bink 2 movies need a decoder you install yourself: RAD Video Tools (free from RAD Game Tools, "
           + RAD_PAGE + ") or Librempeg's ffmpeg. Install one and set its path in the Mod Maker's settings "
           "(Enable Bink 2 conversion). The import goes on without the titantron.")
EARLY = "abcde"
# radvideo64 ignores "hidden" (a window opened and the conversion waited in a
# test): not run by itself unless SVR2011_BINK2_ALLOW_RAD=1; the player
# converts in RAD's own window instead and gives the Mod Maker the .mp4.
RAD_MANUAL = ("RAD Video Tools can't convert in the background (it opens its own window). Convert this movie in "
              "RAD Video Tools yourself (Convert a file -> MP4), then pick the .mp4 in the Mod Maker. "
              "The import goes on without the titantron.")


def _hidden():
    """subprocess options so a decoder never shows a window (Windows: no
    console, SW_HIDE). RAD Video Tools' radvideo64 showed a window even started
    hidden in a test (2026-10-13), then waited: see RAD_MANUAL."""
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
    if name.startswith("ffmpeg") and _ffmpeg_has_bink2(path):
        return "librempeg"
    return None


def find_tool(tool=None):
    """(kind, path) of the decoder to use, or (None, None)."""
    candidates = []
    if tool:
        candidates.append(tool)
    if os.environ.get("SVR2011_BINK2_TOOL"):
        candidates.append(os.environ["SVR2011_BINK2_TOOL"])
    for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)"),
                 os.environ.get("ProgramW6432"), os.environ.get("LOCALAPPDATA")):
        if base:
            for sub in ("RADVideo", "RAD Video Tools", os.path.join("RAD Game Tools", "RADVideo")):
                candidates.append(os.path.join(base, sub, "radvideo64.exe"))
    for exe in ("radvideo64.exe", "ffmpeg.exe"):
        found = shutil.which(exe)
        if found:
            candidates.append(found)
    for c in candidates:
        k = _kind(c)
        if k:
            return k, c
    return None, None


def convert(src, dst, tool=None):
    info = header(src)
    if not info:
        return 2, {"ok": False, "error": "not a Bink 2 movie (no KB2 header): " + src, "warnings": []}
    warnings = warnings_for(info)
    kind, path = find_tool(tool)
    if not kind:
        return 3, {"ok": False, "error": NO_TOOL, "download": RAD_PAGE, "warnings": warnings,
                   "revision": info["revision"]}
    if kind == "rad" and os.environ.get("SVR2011_BINK2_ALLOW_RAD") != "1":
        return 3, {"ok": False, "error": RAD_MANUAL, "kind": kind, "tool": path, "manual": True,
                   "warnings": warnings, "revision": info["revision"]}
    os.makedirs(os.path.dirname(os.path.abspath(dst)) or ".", exist_ok=True)
    part = dst + ".part.mp4"
    if os.path.exists(part):
        os.remove(part)
    progress(5, "converting with " + ("RAD Video Tools" if kind == "rad" else "Librempeg"))
    attempts = ([[path, "binkconv", src, part, "/#"]] if kind == "rad" else
                [[path, "-hide_banner", "-loglevel", "error", "-y", "-i", src, "-an", "-c:v", "libx264",
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
