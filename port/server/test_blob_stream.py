"""Checks that big downloads stream (core --role core + storage node, or --role all) against a running private
server (never the live one): py test_blob_stream.py --url http://127.0.0.1:8495 --node-data <the blobs' --data>

Uploads a big mod (random bytes: stored as sent), a mid-sized one that is then packed (xz) on the node as
blob_pack.py would, and a small one; downloads each and checks its bytes, and how soon the first byte came."""
import argparse
import hashlib
import io
import json
import lzma
import os
import secrets
import sys
import time
import urllib.error
import urllib.request
import zipfile

FAILS = []


def check(what, ok):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        FAILS.append(what)


def jpeg():
    from PIL import Image
    b = io.BytesIO()
    Image.new("RGB", (1024, 768), (90, 40, 40)).save(b, "JPEG", quality=70)
    return b.getvalue()


def svrmod(size, compressible=False):
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        z.writestr("manifest.txt", "type=superstar\nname=Stream test\n")
        block = (b"SvR2011 stream test " * 52429)[:1 << 20] if compressible else None
        with z.open("movie.bik", "w") as f:
            left = size
            while left > 0:
                n = min(left, 1 << 20)
                f.write(block[:n] if compressible else os.urandom(n))
                left -= n
    return out.getvalue()


def call(url, method="GET", data=None, headers=None):
    r = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    with urllib.request.urlopen(r, timeout=600) as f:
        return f.status, f.read()


def upload(url, token, data, name):
    boundary = "----st" + secrets.token_hex(8)
    body = io.BytesIO()

    def part(n, value, filename=None, ctype="text/plain"):
        body.write(("--%s\r\nContent-Disposition: form-data; name=\"%s\"%s\r\nContent-Type: %s\r\n\r\n" % (
            boundary, n, '; filename="%s"' % filename if filename else "", ctype)).encode())
        body.write(value if isinstance(value, bytes) else value.encode())
        body.write(b"\r\n")
    part("name", name)
    part("version", "1.0")
    part("types", "superstar")
    part("svrmod", data, name.replace(" ", "_") + ".svrmod", "application/zip")
    img = jpeg()
    for n in range(4):
        part("img%d" % n, img, "img%d.jpg" % n, "image/jpeg")
    body.write(("--%s--\r\n" % boundary).encode())
    st, out = call(url + "/api/mods", "POST", body.getvalue(), {
        "Authorization": "Bearer " + token, "Content-Type": "multipart/form-data; boundary=" + boundary})
    return json.loads(out)


def download(url, mid):
    """-> (sha256 of the body, seconds to the first byte, seconds in all, bytes) or raises."""
    t0 = time.time()
    with urllib.request.urlopen(url + "/api/mods/%d/file" % mid, timeout=600) as f:
        h = hashlib.sha256()
        first = f.read(65536)
        ttfb = time.time() - t0
        h.update(first)
        n = len(first)
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
            n += len(b)
    return h.hexdigest(), ttfb, time.time() - t0, n


def ranged(url, mid, rng):
    """GET the mod's file with "Range: <rng>" -> (status, headers, body)."""
    r = urllib.request.Request(url + "/api/mods/%d/file" % mid, headers={"Range": rng})
    try:
        with urllib.request.urlopen(r, timeout=600) as f:
            return f.status, dict(f.headers), f.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def range_checks(url, mid, data, what):
    """A resumed download (the launcher's Discovery): from N, N-M, and past the end."""
    n = len(data)
    for a, b in ((n // 3, None), (n // 2 + 7, n // 2 + 7 + 99999), (n - 10, None)):
        st, h, body = ranged(url, mid, "bytes=%d-%s" % (a, "" if b is None else b))
        want = data[a:] if b is None else data[a:b + 1]
        check("%s: Range %d-%s -> %d, Content-Range %s, %d bytes, the right ones" % (
            what, a, "" if b is None else b, st, h.get("Content-Range"), len(body)),
            st == 206 and body == want and h.get("Content-Range") == "bytes %d-%d/%d" % (a, a + len(want) - 1, n)
            and h.get("Accept-Ranges") == "bytes")
    st, h, body = ranged(url, mid, "bytes=%d-" % n)
    check("%s: Range past the end -> 416 (%d, %s)" % (what, st, h.get("Content-Range")),
          st == 416 and h.get("Content-Range") == "bytes */%d" % n)


def blob_path(node_data, sha, packed):
    return os.path.join(node_data, "blobs", sha[:2], sha + (".xz" if packed else ".bin"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--node-data", required=True)
    ap.add_argument("--big-mb", type=int, default=200)
    ap.add_argument("--baseline", action="store_true", help="only time the big download (an older server)")
    a = ap.parse_args()
    url = a.url.rstrip("/")
    name = "st" + secrets.token_hex(4)
    st, out = call(url + "/api/register", "POST", json.dumps({"name": name, "password": "pw-" + name}).encode(),
                   {"Content-Type": "application/json", "X-Forwarded-For": "10.77.%d.1" % secrets.randbelow(250)})
    token = json.loads(out)["token"]

    big = svrmod(a.big_mb << 20)
    t = time.time()
    up = upload(url, token, big, "Stream big")
    check("upload a %d MB mod (%.1f s): %s" % (len(big) >> 20, time.time() - t, up.get("id")), up.get("ok"))
    sha, ttfb, total, n = download(url, up["id"])
    check("big mod: the same bytes back (%d MB)" % (n >> 20), sha == hashlib.sha256(big).hexdigest())
    check("big mod: the first byte after %.2f s (all in %.1f s)" % (ttfb, total), a.baseline or ttfb < 2.0)
    if a.baseline:
        return 0

    # a packed one: xz on the node (as blob_pack.py leaves it), streamed and unpacked on the way
    mid_mod = svrmod(48 << 20, compressible=True)
    up2 = upload(url, token, mid_mod, "Stream packed")
    msha = hashlib.sha256(mid_mod).hexdigest()
    raw = blob_path(a.node_data, msha, False)
    check("packed mod stored as sent first (%s)" % os.path.basename(raw), os.path.exists(raw))
    with open(raw, "rb") as f:
        packed = lzma.compress(f.read(), preset=1)
    with open(blob_path(a.node_data, msha, True), "wb") as f:
        f.write(packed)
    os.remove(raw)
    sha, ttfb, total, n = download(url, up2["id"])
    check("packed mod (%d MB, %.1f MB as xz): the same bytes back, first byte after %.2f s" % (
        len(mid_mod) >> 20, len(packed) / 1048576, ttfb), sha == msha and ttfb < 2.0)
    sha, ttfb, total, n = download(url, up2["id"])
    check("packed mod again: the same", sha == msha)
    range_checks(url, up["id"], big, "big mod (stored)")
    range_checks(url, up2["id"], mid_mod, "packed mod (xz)")

    # a broken packed blob: the client sees a short download, not a wrong one
    with open(blob_path(a.node_data, msha, True), "wb") as f:
        f.write(packed[:len(packed) // 2])
    try:
        sha, ttfb, total, n = download(url, up2["id"])
        check("broken packed blob: the download is cut short (%d of %d bytes)" % (n, len(mid_mod)),
              n < len(mid_mod) and sha != msha)
    except Exception as e:
        check("broken packed blob: the download fails (%s)" % type(e).__name__, True)
    with open(blob_path(a.node_data, msha, True), "wb") as f:
        f.write(packed)

    # small (under the streaming size): read whole, as before
    small = svrmod(1 << 20)
    up3 = upload(url, token, small, "Stream small")
    sha, ttfb, total, n = download(url, up3["id"])
    check("small mod: the same bytes back", sha == hashlib.sha256(small).hexdigest())
    range_checks(url, up3["id"], small, "small mod")
    st, h, body = ranged(url, up3["id"], "bytes=0-")
    check("a plain download says Accept-Ranges: bytes", h.get("Accept-Ranges") == "bytes")

    # the download counts still go up - once per download: a resume (Range not from 0) isn't another
    st, out = call(url + "/api/mods/%d" % up["id"])
    check("big mod's downloads counted (%s), its Range resumes not" % json.loads(out).get("downloads"),
          json.loads(out).get("downloads") == 1)
    ranged(url, up["id"], "bytes=0-1023")
    st, out = call(url + "/api/mods/%d" % up["id"])
    check("a Range from 0 counts (%s)" % json.loads(out).get("downloads"), json.loads(out).get("downloads") == 2)
    try:
        call(url + "/api/mods/999999/file")
        check("a missing mod: 404", False)
    except urllib.error.HTTPError as e:
        check("a missing mod: 404", e.code == 404)

    # tidy: the test's mods go (their owner deletes them)
    for m in (up, up2, up3):
        try:
            call(url + "/api/mods/%d" % m["id"], "DELETE", headers={"Authorization": "Bearer " + token})
        except urllib.error.HTTPError:
            pass
    print("\n%d failed" % len(FAILS) if FAILS else "\nall passed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
