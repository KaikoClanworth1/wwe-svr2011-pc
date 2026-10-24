"""Checks POST /api/reports and the dashboard's report routes against a running private server
(never the live one): py test_reports.py --url http://127.0.0.1:8495 --node-data <the node's --data>

The server needs --local-admin (the dashboard routes from this PC). Each request gives its own X-Forwarded-For
(from loopback the server takes it as the sender), so the rate limit is tested on one address only."""
import argparse
import io
import json
import os
import secrets
import struct
import sys
import urllib.error
import urllib.request
import zipfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reports  # noqa: E402

FAILS = []


def check(what, ok):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        FAILS.append(what)


def png(w=4, h=3):
    raw = b"".join(b"\x00" + b"\x80\x20\x20" * w for _ in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


JPG = b"\xff\xd8\xff\xe0" + b"\x00" * 64 + b"\xff\xd9"


def report_zip(crash=None, log=None):
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        z.writestr("system.txt", "Windows 10.0.19045\n")
        if crash:
            z.writestr("crashes/crash_1.txt", crash)
        if log:
            z.writestr("logs/svr2011.log", log)
    return out.getvalue()


def multipart(parts):
    boundary = "----svr" + secrets.token_hex(8)
    body = io.BytesIO()
    for name, data, ctype in parts:
        body.write(("--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\nContent-Type: %s\r\n\r\n" %
                    (boundary, name, name, ctype)).encode())
        body.write(data)
        body.write(b"\r\n")
    body.write(("--%s--\r\n" % boundary).encode())
    return body.getvalue(), "multipart/form-data; boundary=" + boundary


def post(url, meta, zip_bytes=None, images=(), ip=None, raw_parts=None):
    parts = raw_parts if raw_parts is not None else (
        [("report", json.dumps(meta).encode(), "application/json")] +
        ([("zip", zip_bytes, "application/zip")] if zip_bytes is not None else []) +
        [("image%d" % n, img, "image/png") for n, img in enumerate(images)])
    body, ctype = multipart(parts)
    r = urllib.request.Request(url + "/api/reports", data=body, method="POST",
                               headers={"Content-Type": ctype, "X-Forwarded-For": ip or "10.9.%d.%d" % (
                                   secrets.randbelow(250), secrets.randbelow(250))})
    try:
        with urllib.request.urlopen(r, timeout=120) as f:
            return f.status, json.loads(f.read())
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read())
        except ValueError:
            return e.code, {}


def get(url, path):
    try:
        with urllib.request.urlopen(url + path, timeout=60) as f:
            return f.status, f.read()
    except urllib.error.HTTPError as e:
        return e.code, b""


def meta(**kw):
    m = {"format": 1, "app": "launcher", "version": "2.1.5", "platform": "windows", "os": "10.0.19045",
         "problem": "Crash", "happens": "every time", "what": "It closed when I pressed Play.",
         "before": "Installed a mod.", "contact": ""}
    m.update(kw)
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--node-data", required=True, help="the --data folder of the server that keeps the reports")
    a = ap.parse_args()
    url = a.url.rstrip("/")
    tag = secrets.token_hex(3)  # (this run's own version: earlier runs don't count as duplicates)
    ver = "9.9." + tag

    # the signature's normalising (offline)
    n1 = reports.normalise("[2026-10-24 15:30:12.123] exception 0xC0000005 at 0x7FF6A1B2C3D4 thread 1234 in D:\\Games\\SvR\\svr2011.exe")
    n2 = reports.normalise("[2026-10-25 01:02:03.999] exception 0xC0000005 at 0x7FF6000000AA thread 77 in C:\\x\\svr2011.exe")
    check("normalise: times, addresses, thread ids and folders out (%s)" % n1, n1 == n2)

    # report zips carry old crash files too: the newest counts, a stale one doesn't, report.txt is no log
    import datetime
    stamp = lambda days: (datetime.datetime.now() - datetime.timedelta(days=days)).strftime("%Y%m%d_%H%M%S")
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        z.writestr("report.txt", "contents: the crashes folder, an error summary\n")
        z.writestr("crashes/crash_%s.txt" % stamp(5), "exception OLD\n")
        z.writestr("crashes/crash_%s.txt" % stamp(0.01), "exception NEW\n")
    check("the newest crash file, not an old one", reports.first_error_line(out.getvalue(), "launcher") == "exception NEW")
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        z.writestr("report.txt", "contents: the crashes folder, an error summary\n")
        z.writestr("crashes/crash_%s.txt" % stamp(5), "exception OLD\n")
    check("only a stale crash file and report.txt: no line (the client's hint stands in)",
          reports.first_error_line(out.getvalue(), "launcher") == "")
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        z.writestr("game/crashes/crash_%s.txt" % stamp(0.01), "exception GAME\n")
        z.writestr("logs/modmaker_%s.log" % stamp(0.01).replace("_", "-"), "info\nERROR: convert failed\n")
    check("the Mod Maker: its own log before the game's crash",
          reports.first_error_line(out.getvalue(), "modmaker") == "ERROR: convert failed")

    crash1 = "exception 0xC0000005 at 0x7FF6A1B2C3D4 (svr2011.exe+0x1234)\nstack...\n"
    crash2 = "exception 0xC0000005 at 0x7FF600001111 (svr2011.exe+0x1234)\nother stack\n"
    img = png()
    st, r1 = post(url, meta(version=ver), report_zip(crash1), [img, JPG])
    check("a report with a zip and two pictures: 200 (%s)" % r1, st == 200 and r1.get("ok") and r1.get("seen_before") == 0)
    st, r2 = post(url, meta(version=ver, what="again"), report_zip(crash2))
    check("the same crash at another address: the same signature, seen_before 1",
          st == 200 and r2.get("signature") == r1.get("signature") and r2.get("seen_before") == 1)
    st, r3 = post(url, meta(version=ver, problem="Freeze"), report_zip(crash1))
    check("another problem: another signature", st == 200 and r3.get("signature") != r1.get("signature"))
    log = "12:00:01 info: started\n12:00:05 ERROR: pac load failed: arena.pac (0x80070002)\n12:00:06 ERROR: second\n"
    st, r4 = post(url, meta(version=ver, app="modmaker", problem="Mods"), report_zip(log=log))
    st5, r5 = post(url, meta(version=ver, app="modmaker", problem="Mods"),
                   report_zip(log=log.replace("12:00:05", "23:59:59").replace("0x80070002", "0x80070003")))
    check("a log's first ERROR line, times out: the same signature",
          st == st5 == 200 and r4.get("signature") == r5.get("signature") and r5.get("seen_before") == 1)
    st, r6 = post(url, meta(version=ver, app="android", signature="Game froze on the title screen"))
    st7, r7 = post(url, meta(version=ver, app="android", signature="Game froze on the title screen"))
    check("no zip: the client's hint stands in", st == st7 == 200 and r6.get("signature") == r7.get("signature")
          and r7.get("seen_before") == 1)

    # refused
    st, r = post(url, meta(app="xbox"))
    check("an unknown app: 400 (%s)" % r.get("error"), st == 400 and not r.get("ok"))
    st, r = post(url, meta(problem=""))
    check("no problem: 400", st == 400)
    st, r = post(url, None, raw_parts=[("zip", report_zip(crash1), "application/zip")])
    check("no report part: 400", st == 400)
    st, r = post(url, None, raw_parts=[("report", b"{not json", "application/json")])
    check("bad JSON: 400", st == 400)
    st, r = post(url, meta(version=ver), report_zip(crash1), [b"GIF89a....."])
    check("a GIF picture: 400 (%s)" % r.get("error"), st == 400)
    st, r = post(url, meta(version=ver), None, [img] * 6)
    check("six pictures: 400", st == 400)
    st, r = post(url, meta(version=ver), b"not a zip")
    check("a zip that isn't one: 400", st == 400)
    st, r = post(url, meta(version=ver), b"PK" + os.urandom((25 << 20) + 10))
    check("a 25 MB+ zip: 413 (%s)" % r.get("error"), st == 413)
    st, r = post(url, meta(version=ver), None, [b"\x89PNG\r\n\x1a\n" + b"\x00" * (8 << 20)])
    check("an 8 MB+ picture: 413", st == 413)

    # the rate limit: 5 in 10 minutes from one address
    ip = "10.250.%d.%d" % (secrets.randbelow(250), secrets.randbelow(250))
    codes = [post(url, meta(version=ver, problem="Rate"), ip=ip)[0] for _ in range(6)]
    check("rate limit: 5 accepted, the 6th 429 (%s)" % codes, codes == [200] * 5 + [429])

    # the files on the PC
    day = "%s-%s-%s" % (r1["id"][0:4], r1["id"][4:6], r1["id"][6:8])
    folder = os.path.join(a.node_data, "reports", day, r1["id"])
    files = sorted(os.listdir(folder)) if os.path.isdir(folder) else []
    check("on the node: report.json, report.zip, image0.png, image1.jpg (%s)" % files,
          files == ["image0.png", "image1.jpg", "report.json", "report.zip"])
    if files:
        rec = json.load(open(os.path.join(folder, "report.json"), encoding="utf-8"))
        text = open(os.path.join(folder, "report.json"), encoding="utf-8").read()
        check("report.json: the fields, the signature and its line", rec["what"] == "It closed when I pressed Play."
              and rec["signature"] == r1["signature"] and "0xC0000005" in rec["signature_line"])
        check("report.json: no sender address", "10.9." not in text and "127.0.0.1" not in text and "ip" not in rec)
        check("report.zip: the bytes sent", open(os.path.join(folder, "report.zip"), "rb").read() == report_zip(crash1)
              or zipfile.ZipFile(os.path.join(folder, "report.zip")).read("crashes/crash_1.txt").decode() == crash1)
    index = [json.loads(l) for l in open(os.path.join(a.node_data, "reports", "index.jsonl"), encoding="utf-8")]
    mine = [x for x in index if x.get("version") == ver]
    check("index.jsonl: a line for each accepted report (%d)" % len(mine), len(mine) == 12)

    # the dashboard's routes
    st, body = get(url, "/api/admin/reports")
    rows = json.loads(body).get("reports", []) if st == 200 else []
    one = next((x for x in rows if x["id"] == r1["id"]), None)
    check("admin list: the report, its signature's count 2", one is not None and one["count"] == 2)
    st, body = get(url, "/api/admin/report/%s/image0.png" % r1["id"])
    check("admin picture: the PNG", st == 200 and body == img)
    st, body = get(url, "/api/admin/report/%s/report.zip" % r1["id"])
    check("admin zip: 200", st == 200 and body[:2] == b"PK")
    st, _ = get(url, "/api/admin/report/%s/..%%2Fsettings.json" % r1["id"])
    check("admin file outside a report: 404", st == 404)
    req = urllib.request.Request(url + "/api/admin/report/%s/mark" % r1["id"], method="POST",
                                 data=json.dumps({"status": "done"}).encode(), headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as f:
        check("mark done: 200", f.status == 200)
    st, body = get(url, "/api/admin/reports")
    one = next((x for x in json.loads(body)["reports"] if x["id"] == r1["id"]), {})
    check("the mark shows in the list", one.get("status") == "done")

    print("\n%d failed" % len(FAILS) if FAILS else "\nall passed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
