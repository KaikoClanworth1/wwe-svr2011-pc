"""Problem reports from the launcher, the Mod Maker, the game and Android (POST /api/reports, docs/REPORTS_API.md).

Each one is kept where the Community Creations bytes live (here, or the storage node: the user's PC), readable
there as plain files:
    <data>/reports/<yyyy-mm-dd>/<id>/report.json, report.zip, image0.png ...
    <data>/reports/index.jsonl     (one line per report: the dashboard's list, and the duplicates' counts)
    <data>/reports/status.json     (id -> done / duplicate: the dashboard's marks)
The sender's address is never stored (the core keeps it in memory for its rate limit only).

Duplicates: a signature from app + version + problem + the first crash / error line in the zip's logs, made
general (times, addresses, thread ids and other numbers taken out); the client's own hint stands in when the
zip has none. seen_before = the reports before this one with the same signature.
"""
import hashlib
import io
import json
import os
import re
import secrets
import threading
import time
import zipfile

APPS = ("launcher", "modmaker", "game", "android")
MAX_ZIP = 25 << 20
MAX_IMAGE = 8 << 20
MAX_IMAGES = 5
TEXT_CAP = {"what": 4000, "before": 4000}  # (every other text field: 200)
FIELDS = ("format", "app", "version", "platform", "os", "problem", "happens", "what", "before", "contact")
STATUSES = ("", "done", "duplicate")


def image_ext(data):
    """png / jpg / webp by their magic bytes, else None."""
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if data[:3] == b"\xff\xd8\xff":
        return "jpg"
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "webp"
    return None


def clean_meta(raw):
    """The client's report.json -> (the fields kept, None) or (None, why it's refused)."""
    if not isinstance(raw, dict):
        return None, "report: not a JSON object"
    meta = {}
    for k in FIELDS:
        v = raw.get(k)
        if v is None:
            continue
        if k == "format":
            meta[k] = v if isinstance(v, int) else 1
            continue
        meta[k] = str(v).replace("\r\n", "\n").strip()[:TEXT_CAP.get(k, 200)]
    meta["app"] = meta.get("app", "").lower()
    if meta["app"] not in APPS:
        return None, "app must be one of: " + ", ".join(APPS)
    if not meta.get("version") or not meta.get("problem"):
        return None, "A version and a problem are needed."
    hint = str(raw.get("signature") or "").strip()[:300]
    if hint:
        meta["client_signature"] = hint
    return meta, None


# -- the signature ---------------------------------------------------------------------------------------------

_ERR = re.compile(r"\b(fatal|error|exception|crash(ed)?|assert(ion)?|unhandled|access violation|traceback)\b", re.I)
_NORMALISE = [
    (re.compile(r"^\s*\[?\d{4}-\d\d-\d\d[ T]\d\d:\d\d:\d\d(?:[.,]\d+)?\]?\s*"), ""),  # (a leading date + time)
    (re.compile(r"^\s*\[?\d\d:\d\d:\d\d(?:[.,]\d+)?\]?\s*"), ""),                       # (a leading time)
    (re.compile(r"\b\d{4}-\d\d-\d\d[ T]\d\d:\d\d:\d\d(?:[.,]\d+)?"), "<time>"),
    (re.compile(r"\b\d\d:\d\d:\d\d(?:[.,]\d+)?\b"), "<time>"),
    (re.compile(r"\b(thread|tid|pid)[\s:=#]*(0x)?[0-9a-f]+\b", re.I), r"\1 <n>"),
    (re.compile(r"\b0x[0-9a-f]+\b", re.I), "<addr>"),
    (re.compile(r"\b[0-9a-f]{8,16}\b", re.I), "<addr>"),
    (re.compile(r"[A-Za-z]:[\\/][^\s:\"']*[\\/]"), "<path>/"),  # (a player's own folders: only the file name counts)
    (re.compile(r"\d+"), "<n>"),
    (re.compile(r"\s+"), " "),
]


def normalise(line):
    s = line.strip()
    for rx, to in _NORMALISE:
        s = rx.sub(to, s)
    return s.strip()[:200]


_STAMP = re.compile(r"(20\d{6})[_-]?(\d{6})")


def _stamp(name):
    """The time in a file's name (crash_20261022_142646.txt, modmaker_20261024-044512.log) as an int, or 0."""
    m = _STAMP.search(name)
    return int(m.group(1) + m.group(2)) if m else 0


def _stale(stamp, newest):
    """A crash file from before this problem: a day older than the newest file in the zip, or two days older
    than now (the player's clock and time zone may be off a little)."""
    import datetime
    try:
        a = datetime.datetime.strptime(str(stamp), "%Y%m%d%H%M%S")
        b = datetime.datetime.strptime(str(newest), "%Y%m%d%H%M%S") if newest else a
    except ValueError:
        return False
    return b - a > datetime.timedelta(days=1) or datetime.datetime.now() - a > datetime.timedelta(days=2)


def first_error_line(zip_bytes, app=""):
    """The crash / error line a report is about, '' when there's none (or no zip). Report zips carry old crash
    files too (the crashes folder as it is), so the newest come first (by the time in their names), a crash file
    a day older than the newest file in the zip (or two days older than now) doesn't count, and the Mod Maker's own logs come before the
    game's crashes. Crash files: their first error line (else their first line); logs: their first error line."""
    if not zip_bytes:
        return ""
    try:
        z = zipfile.ZipFile(io.BytesIO(zip_bytes))
    except (zipfile.BadZipFile, ValueError):
        return ""
    names = [i for i in z.infolist() if not i.is_dir() and i.file_size <= (64 << 20)]
    newest = max([_stamp(i.filename) for i in names] or [0])
    by_time = lambda i: (-_stamp(i.filename), i.filename)
    crash = sorted((i for i in names if re.search(r"(^|/)crash", i.filename, re.I)
                    and not (_stamp(i.filename) and _stale(_stamp(i.filename), newest))),
                   key=by_time)
    logs = sorted((i for i in names if i not in crash and not re.search(r"(^|/)crash", i.filename, re.I)
                   and re.search(r"\.log$", i.filename, re.I)), key=by_time)  # (not report.txt: the client's summary)
    groups = [(crash, True), (logs, False)]
    if app == "modmaker":
        own = [i for i in logs if "modmaker" in i.filename.lower()]
        groups = [(own, False), (crash, True), ([i for i in logs if i not in own], False)]
    for group, crash_file in groups:
        for info in group:
            try:
                lines = [l.strip() for l in z.read(info).decode("utf-8", "replace").splitlines() if l.strip()]
            except Exception:
                continue
            hit = next((l for l in lines if _ERR.search(l)), None)
            if hit is None and crash_file and lines:
                hit = lines[0]  # (a crash file without the words: its first line is the crash)
            if hit:
                return hit[:500]
    return ""


def signature(meta, zip_bytes):
    """(signature, the line it was made from)"""
    line = first_error_line(zip_bytes, meta["app"]) or meta.get("client_signature", "")
    key = "|".join((meta["app"], meta["version"].lower(), meta["problem"].lower(), normalise(line)))
    return hashlib.sha1(key.encode("utf-8")).hexdigest()[:16], line


# -- where they are kept ---------------------------------------------------------------------------------------

def _day(rid):
    return "%s-%s-%s" % (rid[0:4], rid[4:6], rid[6:8])


def _valid_id(rid):
    return bool(re.fullmatch(r"\d{8}-\d{6}-[0-9a-f]{6}", rid or ""))


class LocalReports:
    """The reports in <data>/reports (this machine: --role all, or the storage node)."""

    def __init__(self, folder):
        self.dir = os.path.join(folder, "reports")
        os.makedirs(self.dir, exist_ok=True)
        self.index_path = os.path.join(self.dir, "index.jsonl")
        self.status_path = os.path.join(self.dir, "status.json")
        self.lock = threading.Lock()
        self.counts = {}
        for row in self._rows():
            self.counts[row.get("signature", "")] = self.counts.get(row.get("signature", ""), 0) + 1

    def _rows(self):
        try:
            with open(self.index_path, encoding="utf-8") as f:
                return [json.loads(l) for l in f if l.strip()]
        except FileNotFoundError:
            return []

    def _status(self):
        try:
            with open(self.status_path, encoding="utf-8") as f:
                return json.load(f)
        except (FileNotFoundError, ValueError):
            return {}

    def add(self, meta, zip_bytes, images):
        """meta: clean_meta's; images: [bytes] (checked) -> {id, signature, seen_before}"""
        sig, line = signature(meta, zip_bytes)
        received = time.time()
        with self.lock:
            rid = time.strftime("%Y%m%d-%H%M%S", time.localtime(received)) + "-" + secrets.token_hex(3)
            folder = os.path.join(self.dir, _day(rid), rid)
            os.makedirs(folder)
            seen = self.counts.get(sig, 0)
            names = []
            for n, img in enumerate(images):
                name = "image%d.%s" % (n, image_ext(img))
                with open(os.path.join(folder, name), "wb") as f:
                    f.write(img)
                names.append(name)
            if zip_bytes:
                with open(os.path.join(folder, "report.zip"), "wb") as f:
                    f.write(zip_bytes)
            rec = dict(meta, id=rid, received=time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(received)),
                       received_at=round(received), signature=sig, signature_line=line, seen_before=seen,
                       zip_size=len(zip_bytes or b""), images=names)
            with open(os.path.join(folder, "report.json"), "w", encoding="utf-8") as f:
                json.dump(rec, f, indent=2, ensure_ascii=False)
            row = {k: rec.get(k) for k in ("id", "received", "received_at", "app", "version", "platform", "problem",
                                           "happens", "signature", "signature_line", "seen_before", "zip_size",
                                           "images", "account")}
            row["what"] = meta.get("what", "")[:200]
            with open(self.index_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(row, ensure_ascii=False) + "\n")
            self.counts[sig] = seen + 1
        return {"id": rid, "signature": sig, "seen_before": seen}

    def list(self, limit=500):
        """The newest reports first (index lines + their dashboard marks + their signature's count)."""
        with self.lock:
            rows, marks, counts = self._rows(), self._status(), dict(self.counts)
        rows = rows[-limit:][::-1]
        for r in rows:
            r["status"] = marks.get(r["id"], "")
            r["count"] = counts.get(r.get("signature", ""), 1)
        return rows

    def file(self, rid, name):
        """One of a report's files (report.json, report.zip, imageN.ext) -> bytes or None."""
        if not _valid_id(rid) or not re.fullmatch(r"report\.(json|zip)|image[0-4]\.(png|jpg|webp)", name or ""):
            return None
        try:
            with open(os.path.join(self.dir, _day(rid), rid, name), "rb") as f:
                return f.read()
        except OSError:
            return None

    def mark(self, rid, status):
        if not _valid_id(rid) or status not in STATUSES:
            return False
        with self.lock:
            if not os.path.isdir(os.path.join(self.dir, _day(rid), rid)):
                return False
            marks = self._status()
            if status:
                marks[rid] = status
            else:
                marks.pop(rid, None)
            tmp = self.status_path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(marks, f, indent=1)
            os.replace(tmp, self.status_path)
        return True


def pack(meta, zip_bytes, images):
    """The core -> storage node body: a stored zip of report.json + report.zip + imageN.ext."""
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        z.writestr("meta.json", json.dumps(meta, ensure_ascii=False))
        if zip_bytes:
            z.writestr("report.zip", zip_bytes)
        for n, img in enumerate(images):
            z.writestr("image%d" % n, img)
    return out.getvalue()


def unpack(body):
    with zipfile.ZipFile(io.BytesIO(body)) as z:
        names = z.namelist()
        meta = json.loads(z.read("meta.json").decode("utf-8"))
        zip_bytes = z.read("report.zip") if "report.zip" in names else b""
        images = [z.read("image%d" % n) for n in range(MAX_IMAGES) if "image%d" % n in names]
    return meta, zip_bytes, images


class RemoteReports:
    """The reports on the storage node (--role core): POST /reports, GET /reports, GET /reports/<id>/<name>,
    POST /reports/<id>/mark with the shared key. Writes raise when the node is down."""

    def __init__(self, url, key, timeout=60):
        self.url = url.rstrip("/")
        self.key = key
        self.timeout = timeout

    def _req(self, method, path, data=None, ctype=None):
        import urllib.request
        headers = {"X-Storage-Key": self.key}
        if ctype:
            headers["Content-Type"] = ctype
        r = urllib.request.Request(self.url + path, data=data, method=method, headers=headers)
        return urllib.request.urlopen(r, timeout=self.timeout)

    def add(self, meta, zip_bytes, images):
        with self._req("POST", "/reports", pack(meta, zip_bytes, images), "application/zip") as f:
            return json.loads(f.read())

    def list(self, limit=500):
        with self._req("GET", "/reports?limit=%d" % limit) as f:
            return json.loads(f.read())

    def file(self, rid, name):
        try:
            with self._req("GET", "/reports/%s/%s" % (rid, name)) as f:
                return f.read()
        except Exception:
            return None

    def mark(self, rid, status):
        try:
            with self._req("POST", "/reports/%s/mark" % rid, json.dumps({"status": status}).encode(),
                           "application/json") as f:
                return f.status == 200
        except Exception:
            return False


def node_routes(app, folder, key_ok):
    """The storage node's side (server.py storage_app)."""
    import asyncio
    from aiohttp import web
    reports = LocalReports(folder)

    async def add(request):
        if not key_ok(request):
            return web.Response(status=403)
        body = await request.read()
        loop = asyncio.get_running_loop()
        try:
            meta, zip_bytes, images = unpack(body)
        except Exception:
            return web.json_response({"ok": False, "error": "bad body"}, status=400)
        return web.json_response(dict({"ok": True}, **await loop.run_in_executor(None, reports.add, meta, zip_bytes,
                                                                                  images)))

    async def listing(request):
        if not key_ok(request):
            return web.Response(status=403)
        limit = max(1, min(5000, int(request.query.get("limit", "500") or 500)))
        return web.json_response(await asyncio.get_running_loop().run_in_executor(None, reports.list, limit))

    async def one(request):
        if not key_ok(request):
            return web.Response(status=403)
        data = await asyncio.get_running_loop().run_in_executor(
            None, reports.file, request.match_info["id"], request.match_info["name"])
        return web.Response(status=404) if data is None else web.Response(body=data,
                                                                           content_type="application/octet-stream")

    async def mark(request):
        if not key_ok(request):
            return web.Response(status=403)
        try:
            status = str((await request.json()).get("status", ""))
        except ValueError:
            return web.Response(status=400)
        return web.Response(status=200 if reports.mark(request.match_info["id"], status) else 404)

    app.router.add_post("/reports", add)
    app.router.add_get("/reports", listing)
    app.router.add_post("/reports/{id}/mark", mark)
    app.router.add_get("/reports/{id}/{name}", one)
