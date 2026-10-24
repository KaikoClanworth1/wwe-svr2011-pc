"""Mods' movies (.bik Bink 1, .bk2 Bink 2, or an .mp4) -> a small H.264 / AAC .mp4 the game plays as an HD
movie (hd_movies.h: the game makes its own stub .bik for it, so the mod needs no .bik). An entrance .bik is
320 x 320, 30 s at 15-20 Mbit/s (6-8 MB); the .mp4 at CRF 20 is about a tenth.

    py movie_shrink.py <in.bik|.bk2|.mp4> <out.mp4> [--ffmpeg <exe>] [--max-size N] [--crf N]
        the movie: same size (or its longest side at most N), frame rate, audio, length.
    py movie_shrink.py mod <mod folder | .svrmod> [--ffmpeg <exe>] [--crf N] [--keep-bik] [--bump]
        every movie a mod's manifest names as a .bik (movie=, video.<id>=, titantron=, and the keys' values
        in any section): its .mp4 beside it, the key rewritten (movie=movie.mp4), the .bik deleted once the
        .mp4 is verified (unless --keep-bik); --bump: version= +0.0.1. An .svrmod is rewritten in place
        (a zip: written to a temp file first, then replaced).

stdout: its last line is one JSON object - {"ok": true, "in_bytes", "out_bytes", "width", "height", "seconds"}
(mod: {"ok": true, "movies": [...], "in_bytes", "out_bytes"}) or {"ok": false, "error": "<plain words>"}
(exit code 2). Progress: "progress <pct> <text>" lines on stderr.

Verified = the .mp4 decodes to its end with no errors and its length is the source's (within 0.3 s or 2 %).
ffmpeg: --ffmpeg, else the one bink2_convert finds (PATH, C:\\ffmpeg\\bin). Bink 2 goes through bink2_convert.py
(a Bink 2 decoder the player installed) first.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

MOVIE_KEYS = re.compile(r"^(\s*(?:movie|titantron|video\.[^=\s]+)\s*=\s*)(.+?\.(?:bik|bk2))\s*$", re.I)


class Fail(Exception):
    pass


def progress(pct, text):
    print("progress %d %s" % (pct, text), file=sys.stderr, flush=True)


def find_ffmpeg(given=None):
    if given:
        if not os.path.isfile(given):
            raise Fail("ffmpeg wasn't found at %s." % given)
        return given
    import bink2_convert
    found = bink2_convert.ffmpeg_exe()
    if not found:
        raise Fail("ffmpeg wasn't found (the port's setup installs it).")
    return found


def run(args, **kw):
    flags = 0x08000000 if os.name == "nt" else 0  # (CREATE_NO_WINDOW)
    return subprocess.run(args, capture_output=True, text=True, errors="replace", creationflags=flags, **kw)


def probe(ffmpeg, path):
    """-> (seconds, width, height, fps, has_audio) from ffmpeg's own report."""
    r = run([ffmpeg, "-hide_banner", "-i", path])
    text = r.stderr
    m = re.search(r"Duration:\s*(\d+):(\d+):([\d.]+)", text)
    seconds = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + float(m.group(3)) if m else 0.0
    v = re.search(r"Video:.*?(\d{2,5})x(\d{2,5})", text)
    f = re.search(r"([\d.]+)\s*(?:fps|tbr)", text)
    if not v:
        raise Fail("%s isn't a movie ffmpeg can read." % os.path.basename(path))
    return seconds, int(v.group(1)), int(v.group(2)), float(f.group(1)) if f else 0.0, "Audio:" in text


def kind_of(path):
    with open(path, "rb") as fh:
        head = fh.read(12)
    if head[:3] == b"BIK":
        return "bink1"
    if head[:3] == b"KB2":
        return "bink2"
    if head[4:8] == b"ftyp":
        return "mp4"
    raise Fail("%s isn't a Bink or .mp4 movie." % os.path.basename(path))


def encode(ffmpeg, src, out, crf=20, max_size=0, seconds=0.0, pct0=0, pct1=100):
    args = [ffmpeg, "-hide_banner", "-y", "-i", src, "-map", "0:v:0", "-map", "0:a?",
            "-c:v", "libx264", "-preset", "medium", "-crf", str(crf), "-pix_fmt", "yuv420p"]
    if max_size:
        args += ["-vf", "scale='if(gt(iw,ih),min(%d,iw),-2)':'if(gt(iw,ih),-2,min(%d,ih))'" % (max_size, max_size)]
    else:
        args += ["-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2"]  # (H.264 4:2:0 needs even sizes)
    args += ["-c:a", "aac", "-b:a", "128k", "-movflags", "+faststart", out]
    flags = 0x08000000 if os.name == "nt" else 0
    p = subprocess.Popen(args, stderr=subprocess.PIPE, stdout=subprocess.DEVNULL, text=True, errors="replace",
                         creationflags=flags)
    tail = []
    for line in iter(p.stderr.readline, ""):
        tail = (tail + [line])[-20:]
        m = re.search(r"time=(\d+):(\d+):([\d.]+)", line)
        if m and seconds:
            t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + float(m.group(3))
            progress(pct0 + (pct1 - pct0) * min(1.0, t / seconds), "encoding")
    if p.wait() != 0 or not os.path.isfile(out):
        raise Fail("ffmpeg couldn't make the .mp4: " + "".join(tail[-3:]).strip()[-300:])


def verify(ffmpeg, out, seconds):
    r = run([ffmpeg, "-hide_banner", "-v", "error", "-i", out, "-f", "null", "-"])
    if r.returncode != 0 or r.stderr.strip():
        raise Fail("the .mp4 doesn't decode cleanly: " + r.stderr.strip()[-300:])
    got = probe(ffmpeg, out)[0]
    if seconds and abs(got - seconds) > max(0.3, seconds * 0.02):
        raise Fail("the .mp4 is %.2f s long, the movie %.2f s." % (got, seconds))
    return got


def shrink(src, out, ffmpeg=None, crf=20, max_size=0, pct0=0, pct1=100):
    ff = find_ffmpeg(ffmpeg)
    kind = kind_of(src)
    work = src
    tmp = None
    if kind == "bink2":
        tmp = out + ".bk2.mp4"
        progress(pct0, "decoding Bink 2")
        r = run([sys.executable, os.path.join(HERE, "bink2_convert.py"), src, tmp])
        if r.returncode != 0 or not os.path.isfile(tmp):
            last = (r.stdout.strip().splitlines() or ["{}"])[-1]
            try:
                why = json.loads(last).get("error")
            except ValueError:
                why = None
            raise Fail(why or "the Bink 2 movie couldn't be decoded (bink2_convert).")
        work = tmp
    try:
        seconds, w, h, fps, audio = probe(ff, work)
        part = os.path.join(os.path.dirname(os.path.abspath(out)), "." + os.path.basename(out) + ".part.mp4")
        encode(ff, work, part, crf, max_size, seconds, pct0, pct0 + (pct1 - pct0) * 0.9)
        progress(pct0 + (pct1 - pct0) * 0.95, "checking")
        got = verify(ff, part, seconds)
        os.replace(part, out)
        _, ow, oh, _, _ = probe(ff, out)
        return {"ok": True, "in_bytes": os.path.getsize(src), "out_bytes": os.path.getsize(out), "width": ow,
                "height": oh, "seconds": round(got, 3)}
    finally:
        for f in (tmp,):
            if f and os.path.isfile(f):
                os.remove(f)


def shrink_folder(folder, ffmpeg=None, crf=20, keep_bik=False, bump=False):
    """A mod folder: its manifests' .bik movie keys -> .mp4 (see the module text)."""
    manifests = [os.path.join(d, f) for d, _, fs in os.walk(folder) for f in fs if f.lower() == "manifest.txt"]
    jobs = []
    for man in manifests:
        with open(man, "r", encoding="utf-8", errors="replace", newline="") as fh:
            lines = fh.read().splitlines(True)
        for k, line in enumerate(lines):
            m = MOVIE_KEYS.match(line.rstrip("\r\n"))
            if m:
                jobs.append((man, k, m.group(1), m.group(2)))
    movies, done = [], {}
    for i, (man, k, key, name) in enumerate(jobs):
        src = os.path.join(os.path.dirname(man), name)
        if not os.path.isfile(src):
            continue
        out = os.path.splitext(src)[0] + ".mp4"
        if src not in done:
            r = shrink(src, out, ffmpeg, crf, 0, 100 * i // len(jobs), 100 * (i + 1) // len(jobs))
            done[src] = r
            movies.append(dict(r, movie=os.path.relpath(src, folder)))
        with open(man, "r", encoding="utf-8", errors="replace", newline="") as fh:
            lines = fh.read().splitlines(True)
        end = "\r\n" if lines[k].endswith("\r\n") else "\n"
        lines[k] = key + os.path.splitext(name)[0] + ".mp4" + end
        with open(man, "w", encoding="utf-8", newline="") as fh:
            fh.write("".join(lines))
    if not keep_bik:
        for src in done:
            os.remove(src)
    if bump and movies:
        for man in manifests:
            with open(man, "r", encoding="utf-8", errors="replace", newline="") as fh:
                text = fh.read()
            # (1.1 -> 1.1.1, 1.1.3 -> 1.1.4)
            text = re.sub(r"(?m)^(\s*version\s*=\s*)(\d+)\.(\d+)(?:\.(\d+))?\b",
                          lambda m: "%s%s.%s.%d" % (m.group(1), m.group(2), m.group(3), int(m.group(4) or 0) + 1),
                          text, 1)
            with open(man, "w", encoding="utf-8", newline="") as fh:
                fh.write(text)
    return {"ok": True, "movies": movies, "in_bytes": sum(m["in_bytes"] for m in movies),
            "out_bytes": sum(m["out_bytes"] for m in movies)}


def shrink_mod(path, ffmpeg=None, crf=20, keep_bik=False, bump=False):
    if os.path.isdir(path):
        return shrink_folder(path, ffmpeg, crf, keep_bik, bump)
    if not zipfile.is_zipfile(path):
        raise Fail("%s is neither a mod folder nor an .svrmod." % path)
    work = tempfile.mkdtemp(prefix="movie_shrink_", dir=os.path.dirname(os.path.abspath(path)))
    try:
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            z.extractall(work)
        r = shrink_folder(work, ffmpeg, crf, keep_bik, bump)
        if not r["movies"]:
            return dict(r, svrmod_before=os.path.getsize(path), svrmod_after=os.path.getsize(path))
        before = os.path.getsize(path)
        tmp = path + ".new"
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
            for d, _, fs in os.walk(work):
                for f in sorted(fs):
                    full = os.path.join(d, f)
                    arc = os.path.relpath(full, work).replace(os.sep, "/")
                    # (movies are compressed already: stored)
                    z.write(full, arc, zipfile.ZIP_STORED if f.lower().endswith((".mp4", ".bik")) else
                            zipfile.ZIP_DEFLATED)
        os.replace(tmp, path)
        return dict(r, svrmod_before=before, svrmod_after=os.path.getsize(path), entries_before=len(names))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main(argv):
    def opt(name, default=None):
        if name in argv:
            i = argv.index(name)
            v = argv[i + 1]
            del argv[i:i + 2]
            return v
        return default

    def flag(name):
        if name in argv:
            argv.remove(name)
            return True
        return False

    ffmpeg, crf, max_size = opt("--ffmpeg"), int(opt("--crf", "20")), int(opt("--max-size", "0"))
    keep_bik, bump = flag("--keep-bik"), flag("--bump")
    try:
        if argv and argv[0] == "mod" and len(argv) == 2:
            r = shrink_mod(argv[1], ffmpeg, crf, keep_bik, bump)
        elif len(argv) == 2:
            if not os.path.isfile(argv[0]):
                raise Fail("%s wasn't found." % argv[0])
            r = shrink(argv[0], argv[1], ffmpeg, crf, max_size)
        else:
            raise Fail("usage: movie_shrink.py <in> <out.mp4> | mod <folder|.svrmod> (see the file's text)")
    except Fail as e:
        print(json.dumps({"ok": False, "error": str(e)}))
        return 2
    print(json.dumps(r))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
