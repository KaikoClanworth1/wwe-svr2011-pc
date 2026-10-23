r"""The storage's cold pass: blob files stored as sent (<sha>.bin: Discovery mods, songs, movies) that haven't
changed for a while become <sha>.xz (lzma, as the server's own packed blobs), where that saves enough.

The server already reads both (gamespy.LocalBlobs: .bin first, else .xz; the core un-packs what the storage
node says is packed), so nothing else changes: players download the same bytes. Run it beside a running
server - each file is swapped safely:
  1. <sha>.xz.tmp written, read back, decompressed, its sha256 checked against the name;
  2. renamed to <sha>.xz;
  3. <sha>.bin removed - unless it vanished meanwhile (deleted while this ran): then the .xz goes too.
It can stop at any time and run again (a file with both .bin and .xz is finished first). Files that don't
shrink (already-compressed songs / movies: a sample tried first) are remembered and skipped next time.

    python blob_pack.py <data folder> [--days 7] [--min-gain 5] [--dry-run] [--limit-mb N]

<data folder> is the server's --data (its blobs\ folder is used). A log goes to <data>\blob_pack.log.
"""
import argparse
import hashlib
import json
import lzma
import os
import sys
import time

SAMPLE = 4 << 20


def log(f, text):
    line = time.strftime("%Y-%m-%d %H:%M:%S ") + text
    print(line, flush=True)
    f.write(line + "\n")
    f.flush()


def sample_ratio(path, size):
    """The packed / raw ratio of up to 3 slices (start, middle, end) of a file."""
    raw = packed = 0
    with open(path, "rb") as fh:
        for at in sorted({0, max(0, size // 2 - SAMPLE // 2), max(0, size - SAMPLE)}):
            fh.seek(at)
            data = fh.read(SAMPLE)
            raw += len(data)
            packed += len(lzma.compress(data, preset=6))
    return packed / raw if raw else 1.0


def pack_one(bin_path, xz_path, sha):
    """bin -> verified xz; returns the packed size, or raises."""
    with open(bin_path, "rb") as fh:
        data = fh.read()
    if hashlib.sha256(data).hexdigest() != sha:
        raise ValueError("the .bin's content isn't its name's sha: left alone")
    packed = lzma.compress(data, preset=6)
    tmp = xz_path + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(packed)
        fh.flush()
        os.fsync(fh.fileno())
    with open(tmp, "rb") as fh:
        back = fh.read()
    if hashlib.sha256(lzma.decompress(back)).hexdigest() != sha:
        os.remove(tmp)
        raise ValueError("the packed copy didn't read back right: left alone")
    os.replace(tmp, xz_path)
    return len(packed)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("data")
    ap.add_argument("--days", type=float, default=7, help="only files unchanged this many days (default 7)")
    ap.add_argument("--min-gain", type=float, default=5, help="pack only when it saves at least this %% (default 5)")
    ap.add_argument("--dry-run", action="store_true", help="only say what it would save")
    ap.add_argument("--limit-mb", type=float, default=0, help="stop after packing this many MB (0: all)")
    a = ap.parse_args()
    blobs = os.path.join(a.data, "blobs")
    if not os.path.isdir(blobs):
        sys.exit("no blobs folder in %s" % a.data)
    state_path = os.path.join(a.data, "blob_pack.json")
    try:
        state = json.load(open(state_path))
    except (OSError, ValueError):
        state = {"skip": {}}
    lf = open(os.path.join(a.data, "blob_pack.log"), "a", encoding="utf-8")
    log(lf, "start%s: %s, unchanged %g days, at least %g%% saved" % (" (dry run)" if a.dry_run else "", blobs,
                                                                    a.days, a.min_gain))
    cutoff = time.time() - a.days * 86400
    n = before = after = skipped = young = errors = 0
    done_mb = 0.0
    for d in sorted(os.listdir(blobs)):
        sub = os.path.join(blobs, d)
        if len(d) != 2 or not os.path.isdir(sub):
            continue
        for f in sorted(os.listdir(sub)):
            if not f.endswith(".bin") or len(f) != 68:
                continue
            sha = f[:64]
            bin_path, xz_path = os.path.join(sub, f), os.path.join(sub, sha + ".xz")
            try:
                st = os.stat(bin_path)
            except OSError:
                continue
            if os.path.exists(xz_path) and not a.dry_run:   # (an earlier run stopped after the rename)
                try:
                    with open(xz_path, "rb") as fh:
                        ok = hashlib.sha256(lzma.decompress(fh.read())).hexdigest() == sha
                except (OSError, lzma.LZMAError):
                    ok = False
                if ok:
                    os.remove(bin_path)
                    log(lf, "%s finished (an earlier run's .xz checked, .bin removed)" % sha[:16])
                else:
                    os.remove(xz_path)
                    log(lf, "%s a broken .xz from an earlier run removed; the .bin stays" % sha[:16])
                continue
            if st.st_mtime > cutoff:
                young += 1
                continue
            if sha in state["skip"]:
                skipped += 1
                continue
            ratio = sample_ratio(bin_path, st.st_size)
            if (1 - ratio) * 100 < a.min_gain:
                skipped += 1
                state["skip"][sha] = round(ratio, 3)
                log(lf, "%s %.1f MB skipped: about %.0f%% saved (already compressed)" % (
                    sha[:16], st.st_size / 1e6, (1 - ratio) * 100))
                continue
            if a.dry_run:
                n += 1
                before += st.st_size
                after += int(st.st_size * ratio)
                log(lf, "%s %.1f MB -> about %.1f MB" % (sha[:16], st.st_size / 1e6, st.st_size * ratio / 1e6))
                continue
            try:
                size = pack_one(bin_path, xz_path, sha)
            except (OSError, ValueError, lzma.LZMAError) as e:
                errors += 1
                log(lf, "%s %.1f MB not packed: %s" % (sha[:16], st.st_size / 1e6, e))
                continue
            if os.path.exists(bin_path):
                os.remove(bin_path)
            else:   # (deleted while it was packed: don't bring it back)
                os.remove(xz_path)
                log(lf, "%s was deleted meanwhile: its .xz removed" % sha[:16])
                continue
            n += 1
            before += st.st_size
            after += size
            done_mb += st.st_size / 1e6
            log(lf, "%s %.1f MB -> %.1f MB (%.0f%%)" % (sha[:16], st.st_size / 1e6, size / 1e6, 100 * size / st.st_size))
            if a.limit_mb and done_mb >= a.limit_mb:
                break
        else:
            continue
        break
    if not a.dry_run:
        tmp = state_path + ".tmp"
        json.dump(state, open(tmp, "w"))
        os.replace(tmp, state_path)
    log(lf, "%s: %d files %.1f MB -> %.1f MB (%.1f MB saved)%s; %d skipped (don't shrink), %d too new, %d errors" % (
        "would pack" if a.dry_run else "packed", n, before / 1e6, after / 1e6, (before - after) / 1e6,
        " - estimated from samples" if a.dry_run else "", skipped, young, errors))


if __name__ == "__main__":
    main()
