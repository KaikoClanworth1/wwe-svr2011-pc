"""Entrance songs and movies sent with a Created Superstar, made ready on the
server (ffmpeg): any size can be sent, then
  - a song is an MP3 of at most 160 kbps (as the game's own music, 44.1 kHz
    stereo): one already like that is kept, any other is re-encoded at 128 kbps;
  - a movie (an MP4 from the port) is at most 4 minutes: a longer one is cut
    there (the streams copied, not re-encoded).
The stored file is the processed one; media_alias maps what was sent (its
sha256) to it, so the sender's sha still finds it and a Superstar's entrance
info is rewritten to the stored file.
"""
import json
import os
import shutil
import subprocess
import tempfile

MAX_MOVIE_SECONDS = 240
SONG_KBPS = 128
KEEP_SONG_KBPS = 160


def ffmpeg_path(name="ffmpeg"):
    found = shutil.which(name)
    if found:
        return found
    for folder in (r"C:\ffmpeg\bin", r"C:\Program Files\ffmpeg\bin"):
        p = os.path.join(folder, name + (".exe" if os.name == "nt" else ""))
        if os.path.exists(p):
            return p
    return None


def _run(args, timeout=600):
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    return subprocess.run(args, capture_output=True, timeout=timeout, creationflags=flags)


def probe(path):
    exe = ffmpeg_path("ffprobe")
    if not exe:
        return {}
    r = _run([exe, "-v", "error", "-show_entries", "format=duration,bit_rate,format_name:stream=codec_type,codec_name",
              "-of", "json", path], timeout=60)
    try:
        return json.loads(r.stdout or b"{}")
    except ValueError:
        return {}


def process(kind, data):
    """(the stored bytes, a new file extension or None, what was done), or
    (None, None, why) when it can't be used."""
    exe = ffmpeg_path()
    if not exe:
        return data, None, "kept (no ffmpeg on the server)"
    with tempfile.TemporaryDirectory(prefix="svr2011-media-") as tmp:
        src = os.path.join(tmp, "in")
        with open(src, "wb") as f:
            f.write(data)
        info = probe(src)
        fmt = info.get("format", {})
        streams = info.get("streams", [])
        try:
            seconds = float(fmt.get("duration") or 0)
        except ValueError:
            seconds = 0
        if kind == "music":
            audio = [s for s in streams if s.get("codec_type") == "audio"]
            if not audio:
                return None, None, "not a song"
            kbps = int(fmt.get("bit_rate") or 0) // 1000
            if audio[0].get("codec_name") == "mp3" and 0 < kbps <= KEEP_SONG_KBPS and not any(
                    s.get("codec_type") == "video" for s in streams):
                return data, None, "kept (MP3, %d kbps)" % kbps
            out = os.path.join(tmp, "out.mp3")
            r = _run([exe, "-v", "error", "-y", "-i", src, "-vn", "-map_metadata", "-1", "-ac", "2", "-ar", "44100",
                      "-c:a", "libmp3lame", "-b:a", "%dk" % SONG_KBPS, out])
            if r.returncode != 0 or not os.path.exists(out):
                return None, None, "couldn't convert the song: " + r.stderr.decode(errors="replace")[-200:]
            with open(out, "rb") as f:
                return f.read(), ".mp3", "re-encoded (%s, %d kbps -> MP3 %d kbps)" % (
                    audio[0].get("codec_name"), kbps, SONG_KBPS)
        if kind == "movie":
            if not any(st.get("codec_type") == "video" for st in streams):
                return None, None, "not a movie"
            if seconds <= MAX_MOVIE_SECONDS + 0.5:
                return data, None, "kept (%.0f s)" % seconds
            out = os.path.join(tmp, "out.mp4")
            r = _run([exe, "-v", "error", "-y", "-i", src, "-t", str(MAX_MOVIE_SECONDS), "-c", "copy",
                      "-movflags", "+faststart", out])
            if r.returncode != 0 or not os.path.exists(out):
                return None, None, "couldn't cut the movie: " + r.stderr.decode(errors="replace")[-200:]
            with open(out, "rb") as f:
                return f.read(), None, "cut at %d s (was %.0f s)" % (MAX_MOVIE_SECONDS, seconds)
    return data, None, "kept"
