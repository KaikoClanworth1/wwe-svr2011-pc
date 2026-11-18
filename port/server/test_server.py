"""Checks a running Community Creations server (server.py): accounts and
sign in, uploads come back as they went up, the port's logo store, the size
limit and many requests at once. It makes two throwaway accounts: run it
against a test server (tools/online_server.ps1), not a public one.

    python test_server.py [--server http://127.0.0.1:8411]
    python test_server.py --server ... --admin   (also the Settings page's API: a --local-admin test server only -
                                                  it moves the server's backups folder and back, sets a test token)
    python test_server.py --storage-only          (no server needed: the folder moves - storage.py - and a storage
                                                  node of its own on loopback, in a temporary folder)
"""
import argparse
import concurrent.futures
import json
import os
import sys
import time
import urllib.error
import urllib.request

import gamespy as gs
import storage


def check_storage(check):
    """storage.py on its own: a folder moved while a writer keeps adding files (each under the kind's lock, as the
    server's writes are) - every file ends up in the new folder, the old copy stays until a typed delete."""
    import shutil
    import tempfile
    import threading
    root = tempfile.mkdtemp(prefix="svr_storage_")
    try:
        data = os.path.join(root, "data")
        os.makedirs(os.path.join(data, "blobs", "ab"))
        for i in range(300):
            with open(os.path.join(data, "blobs", "ab", "f%03d.bin" % i), "wb") as f:
                f.write(os.urandom(2000 + i))
        os.makedirs(os.path.join(data, "blobs", "tmp"))
        with open(os.path.join(data, "blobs", "tmp", "half.zip"), "wb") as f:
            f.write(b"on its way")
        logs = []
        st = storage.Storage(data, ["blobs", "reports"], logs.append)
        lock = threading.Lock()
        where = {"blobs": st.path("blobs")}
        st.hook("blobs", lock, lambda p: where.update(blobs=p))
        new = os.path.join(root, "elsewhere", "blobs")
        c = st.check("blobs", os.path.join(data, "blobs", "inside"))
        check(not c["ok"] and any("inside" in p for p in c["problems"]), "storage: a folder inside the one in use refused")
        c = st.check("blobs", "relative\\path")
        check(not c["ok"], "storage: a relative path refused")
        c = st.check("blobs", st.path("reports"))
        check(not c["ok"] and any("overlaps" in p for p in c["problems"]), "storage: the reports folder refused")
        c = st.check("blobs", new)
        check(c["ok"] and c["writable"] and not c["exists"] and c["need"] > 600000,
              "storage: a new folder checks out (writable, %d bytes to move)" % c["need"])
        written, stop = [], threading.Event()

        def writer():
            i = 0
            while not stop.is_set():
                with lock:  # (as the server: the folder looked up under the lock, then written)
                    path = os.path.join(where["blobs"], "cd", "w%04d.bin" % i)
                    os.makedirs(os.path.dirname(path), exist_ok=True)
                    with open(path, "wb") as f:
                        f.write(b"x" * (100 + i))
                written.append("w%04d.bin" % i)
                i += 1
                time.sleep(0.002)
        t = threading.Thread(target=writer)
        t.start()
        ok_move, why = st.move("blobs", new)
        check(ok_move, "storage: move started %s" % why)
        for _ in range(500):
            if st.status()["state"] not in ("copying", "checking"):
                break
            time.sleep(0.02)
        time.sleep(0.05)
        stop.set()
        t.join()
        job = st.status()
        check(job["state"] == "done", "storage: move %s %s" % (job["state"], job["error"]))
        check(where["blobs"] == new and st.path("blobs") == new, "storage: the server switched to the new folder")
        missing = [n for n in written if not os.path.exists(os.path.join(new, "cd", n))]
        check(written and not missing, "storage: %d files written during the move, all in the new folder (%d missing)"
              % (len(written), len(missing)))
        check(len(os.listdir(os.path.join(new, "ab"))) == 300, "storage: the 300 files copied")
        check(not os.path.exists(os.path.join(new, "tmp")), "storage: tmp/ (files on their way) not copied")
        check(os.path.isdir(os.path.join(data, "blobs")) and st.old("blobs") == [os.path.join(data, "blobs")],
              "storage: the old copy kept and listed")
        again = storage.Storage(data, ["blobs", "reports"], logs.append)
        check(again.path("blobs") == new, "storage: the choice kept (storage.json) for the next start")
        ok_del, why, _ = st.delete_old("blobs", os.path.join(data, "blobs"), "delete")
        check(not ok_del and "DELETE" in why, "storage: delete refused without the typed DELETE")
        ok_del, why, _ = st.delete_old("blobs", new, "DELETE")
        check(not ok_del, "storage: the folder in use can't be deleted as an old copy")
        ok_del, why, freed = st.delete_old("blobs", os.path.join(data, "blobs"), "DELETE")
        check(ok_del and freed > 600000 and not os.path.exists(os.path.join(data, "blobs")),
              "storage: old copy deleted (%d bytes)" % freed)
        # a failed copy changes nothing (the switch raises: still the old folder, no old copy listed)
        st2 = storage.Storage(os.path.join(root, "data2"), ["reports"], logs.append)
        os.makedirs(st2.path("reports"))
        open(os.path.join(st2.path("reports"), "index.jsonl"), "w").close()

        def bad_switch(p):
            raise OSError("switch failed on purpose")
        st2.hook("reports", threading.Lock(), bad_switch)
        st2.move("reports", os.path.join(root, "r2"))
        for _ in range(200):
            if st2.status()["state"] not in ("copying", "checking"):
                break
            time.sleep(0.02)
        check(st2.status()["state"] == "failed" and os.path.isfile(os.path.join(st2.data, "reports", "index.jsonl")),
              "storage: a failed move keeps the old folder (%s)" % st2.status()["error"])
    finally:
        shutil.rmtree(root, ignore_errors=True)


def check_node_storage(check):
    """A storage node of its own (loopback, a temporary folder): /__storage needs the key; its blobs moved, a blob
    still served from the new folder after the move and after a restart."""
    import shutil
    import socket
    import subprocess
    import tempfile
    root = tempfile.mkdtemp(prefix="svr_node_")
    procs = []
    try:
        with socket.socket() as so:
            so.bind(("127.0.0.1", 0))
            port = so.getsockname()[1]
        data, key = os.path.join(root, "node"), os.urandom(16).hex()
        os.makedirs(data)
        with open(os.path.join(root, "key"), "w") as f:
            f.write(key)
        url = "http://127.0.0.1:%d" % port

        def start():
            p = subprocess.Popen([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "server.py"),
                                  "--role", "storage", "--port", str(port), "--data", data, "--storage-key",
                                  os.path.join(root, "key")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            procs.append(p)
            for _ in range(100):
                try:
                    urllib.request.urlopen(url + "/__health", timeout=1)
                    return p
                except Exception:
                    time.sleep(0.1)
            return p
        p = start()
        k = {"X-Storage-Key": key, "Content-Type": "application/json"}
        blob = os.urandom(5000)
        sha = __import__("hashlib").sha256(blob).hexdigest()
        status, _, _ = request(url, "PUT", "/blob/%s?packed=0" % sha, blob, {"X-Storage-Key": key})
        check(status == 200, "node: a blob stored")
        status, _, _ = request(url, "GET", "/__storage")
        check(status == 403, "node: /__storage needs the key")
        status, _, body = request(url, "GET", "/__storage", headers=k)
        info = json.loads(body or b"{}")
        check(status == 200 and [f["kind"] for f in info.get("folders", [])] == ["blobs", "reports"],
              "node: its folders listed (blobs, reports)")
        new = os.path.join(root, "moved", "blobs")
        status, _, body = request(url, "POST", "/__storage/move", json.dumps({"kind": "blobs", "path": new}).encode(), k)
        check(status == 200, "node: move started %s" % body[:120])
        for _ in range(100):
            job = json.loads(request(url, "GET", "/__storage", headers=k)[2])["job"]
            if job and job["state"] not in ("copying", "checking"):
                break
            time.sleep(0.1)
        check(job and job["state"] == "done", "node: move done")
        status, _, got = request(url, "GET", "/blob/" + sha, headers={"X-Storage-Key": key})
        check(status == 200 and got == blob and os.path.exists(os.path.join(new, sha[:2], sha + ".bin")),
              "node: the blob served from the new folder")
        p.terminate()
        p.wait()
        start()
        status, _, got = request(url, "GET", "/blob/" + sha, headers={"X-Storage-Key": key})
        check(status == 200 and got == blob, "node: after a restart, still the new folder")
        old = os.path.join(data, "blobs")
        status, _, body = request(url, "POST", "/__storage/delete-old", json.dumps(
            {"kind": "blobs", "path": old, "confirm": "DELETE"}).encode(), k)
        check(status == 200 and not os.path.exists(old), "node: old copy deleted with the typed DELETE")
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
                p.wait()
        shutil.rmtree(root, ignore_errors=True)


def check_admin(server, check):
    """The Settings page's API on a --local-admin test server: groups, defaults, secrets never echoed, a live
    setting applied, the backups folder moved out and back (both old copies deleted)."""
    import shutil
    import tempfile
    status, _, body = request(server, "GET", "/api/admin/settings")
    if status != 200:
        check(False, "admin: the settings page's API answers (a --local-admin server?) - %d" % status)
        return
    d = json.loads(body)
    keys = {x["key"]: x for x in d["settings"]}
    check(all(x["section"] in d["sections"] for x in d["settings"]), "admin: every setting in a section (%s)"
          % ", ".join(d["sections"]))
    check(all("default" in x and x.get("help") for x in d["settings"]), "admin: every setting has a default and help")
    check({"max_file_mb", "quota_mb", "report_limit_10min", "login_tries_10min", "discovery_max_mod_mb",
           "patreon_every_h"} <= set(keys), "admin: the new rows are there")
    check(not keys["max_file_mb"]["live"] and keys["quota_mb"]["live"], "admin: restart-only rows marked")
    check(d["server"] and any(f["key"] == "data folder" for f in d["server"]), "admin: the command line's facts listed")
    secret = "test-" + os.urandom(8).hex()
    status, r = post_json(server, "/api/admin/secret", {"name": "kofi_token", "value": secret})
    check(status == 200 and r["secrets"]["kofi_token"] == {**r["secrets"]["kofi_token"], "set": True, "where": "dashboard"},
          "admin: Ko-fi token replaced (shown as set, here)")
    status, _, body = request(server, "GET", "/api/admin/settings")
    check(secret.encode() not in body, "admin: the token never comes back")
    status, r = post_json(server, "/api/admin/secret", {"name": "kofi_token", "value": ""})
    check(status == 200 and r["secrets"]["kofi_token"]["where"] != "dashboard", "admin: Ko-fi token: the file used again")
    status, r = post_json(server, "/api/admin/secret", {"name": "admin_password", "value": "x"})
    check(status == 400, "admin: only the listed secrets")
    was = keys["report_limit_10min"]["saved"]
    status, _ = post_json(server, "/api/admin/settings", {"report_limit_10min": 7})
    status, _, body = request(server, "GET", "/api/admin/settings")
    check([x["value"] for x in json.loads(body)["settings"] if x["key"] == "report_limit_10min"] == [7],
          "admin: a live setting applied at once")
    post_json(server, "/api/admin/settings", {"report_limit_10min": was})
    status, _ = post_json(server, "/api/admin/settings", {"max_file_mb": 100000})
    check(status == 400, "admin: a value out of range refused")
    # the backups folder: out to a temporary folder, then back to where it was
    status, _, body = request(server, "GET", "/api/admin/folders")
    sides = json.loads(body)["sides"]
    here = [f for f in sides[0]["folders"] if f["kind"] == "backups"][0]
    back, tmp = here["path"], tempfile.mkdtemp(prefix="svr_backups_")
    try:
        new = os.path.join(tmp, "backups")
        status, c = post_json(server, "/api/admin/folders/check", {"side": "here", "kind": "backups", "path": new})
        check(status == 200 and c["ok"] and c["writable"], "admin: folder check (writable, %s free)" % c.get("free"))
        a_file = os.path.join(tmp, "a_file")
        open(a_file, "w").close()
        status, c = post_json(server, "/api/admin/folders/check", {"side": "here", "kind": "backups",
                                                                    "path": os.path.join(a_file, "x")})
        check(not c["ok"] and c["problems"], "admin: a folder that can't be made is refused: %s" % c["problems"][:1])
        for path, what in ((new, "out"), (back, "back")):
            status, r = post_json(server, "/api/admin/folders/move", {"side": "here", "kind": "backups", "path": path})
            check(status == 200, "admin: move %s started %s" % (what, r.get("error", "")))
            for _ in range(100):
                job = json.loads(request(server, "GET", "/api/admin/folders")[2])["sides"][0]["job"]
                if job and job["state"] not in ("copying", "checking"):
                    break
                time.sleep(0.1)
            check(job and job["state"] == "done" and job["to"] == path, "admin: backups moved %s" % what)
        folders = json.loads(request(server, "GET", "/api/admin/folders")[2])["sides"][0]["folders"]
        olds = [o["path"] for f in folders if f["kind"] == "backups" for o in f["old"]]
        check(new in olds, "admin: the temporary copy listed as old")
        status, r = post_json(server, "/api/admin/folders/delete-old", {"side": "here", "kind": "backups", "path": new,
                                                                        "confirm": "DELETE"})
        check(status == 200 and not os.path.exists(new), "admin: old copy deleted")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def request(server, method, path, body=None, headers=None, token=None, timeout=30):
    headers = dict(headers or {})
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(server.rstrip("/") + path, data=body, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def post_json(server, path, value, token=None):
    status, _, body = request(server, "POST", path, json.dumps(value).encode(),
                              {"Content-Type": "application/json"}, token)
    try:
        return status, json.loads(body or b"{}")
    except ValueError:
        return status, {}


def make_media(args):
    """A small media file made by ffmpeg (from its test sources)."""
    import shutil
    import subprocess
    import tempfile
    exe = shutil.which("ffmpeg") or r"C:fmpeginfmpeg.exe"
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "out")
        subprocess.run([exe, "-v", "error", "-y"] + args + [out], check=True)
        with open(out, "rb") as f:
            return f.read()


def multipart(data):
    b = b"svr2011test"
    return (b"--" + b + b'\r\nContent-Disposition: form-data; name="data"; filename="test.bin"\r\n'
            b"Content-Type: application/octet-stream\r\n\r\n" + data + b"\r\n--" + b + b"--\r\n",
            "multipart/form-data; boundary=" + b.decode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="http://127.0.0.1:8411", help="its address, with its base (e.g. .../svr)")
    ap.add_argument("--requests", type=int, default=150, help="sent at once (more than --max-connections)")
    ap.add_argument("--admin", action="store_true", help="also the Settings page's API (a --local-admin test server)")
    ap.add_argument("--storage-only", action="store_true", help="only the folder moves (no server needed)")
    args = ap.parse_args()
    server = args.server if "://" in args.server else "http://" + args.server
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok &= bool(cond)

    if args.storage_only:
        check_storage(check)
        check_node_storage(check)
        return 0 if ok else 1
    if args.admin:
        check_admin(server, check)

    status, _, body = request(server, "GET", "/api/status")
    check(status == 200, "status: %s" % body.decode(errors="replace").strip())

    # accounts
    suffix = os.urandom(3).hex()
    a_name, b_name, password = "test_a" + suffix, "test_b" + suffix, "pw-" + os.urandom(6).hex()
    status, a = post_json(server, "/api/register", {"name": a_name, "password": password})
    check(status == 200 and a.get("token"), "register %s -> xuid %s" % (a_name, a.get("xuid")))
    status, b = post_json(server, "/api/register", {"name": b_name, "password": password})
    check(status == 200 and b.get("token"), "register %s -> xuid %s" % (b_name, b.get("xuid")))
    status, again = post_json(server, "/api/register", {"name": a_name.upper(), "password": password})
    check(status != 200, "the same name again refused: %s" % again.get("error"))
    status, wrong = post_json(server, "/api/login", {"name": a_name, "password": "wrong"})
    check(status != 200, "a wrong password refused: %s" % wrong.get("error"))
    status, login = post_json(server, "/api/login", {"name": a_name, "password": password})
    check(status == 200 and login.get("xuid") == a.get("xuid"), "sign in")
    status, _, body = request(server, "GET", "/api/session", token=login.get("token"))
    session = json.loads(body or b"{}")
    check(status == 200 and session.get("name") == a_name and session.get("ticket"),
          "session: profile %s" % session.get("profileid"))
    status, _, _ = request(server, "GET", "/api/session", token="not-a-token")
    check(status == 401, "a made-up token refused")
    status, _, _ = request(server, "POST", "/game/SakeFileServer/upload.aspx", b"")
    check(status == 401, "the game's requests need a sign in")
    a_token, b_token = a.get("token"), b.get("token")

    # a file up and back (another player downloads it)
    data = os.urandom(300000)
    body, ctype = multipart(data)
    status, h, _ = request(server, "POST", "/game/SakeFileServer/upload.aspx?gameid=2886&pid=1", body,
                           {"Content-Type": ctype}, a_token)
    fileid = h.get("Sake-File-Id")
    check(h.get("Sake-File-Result") == "0" and fileid, "upload 300000 bytes -> file %s" % fileid)
    for n in (1, 2):
        t = time.time()
        _, h, back = request(server, "GET", "/game/SakeFileServer/download.aspx?fileid=%s&gameid=2886&pid=1" % fileid,
                             token=b_token)
        check(h.get("Sake-File-Result") == "0" and back == data,
              "download %d by the other player: %d bytes, %.0f ms" % (n, len(back), (time.time() - t) * 1000))
    # a big upload in 2 MB parts (a story, a highlight reel): one record for all of them
    parts = []
    for size in (2 << 20, 100000):
        body, ctype = multipart(os.urandom(size))
        _, h, _ = request(server, "POST", "/game/SakeFileServer/upload.aspx?gameid=2886&pid=1", body,
                          {"Content-Type": ctype}, a_token)
        parts.append((h.get("Sake-File-Id"), size))

    def create(total):
        values = "".join('<ns1:RecordField><ns1:name>F%02d</ns1:name><ns1:value><ns1:intValue><ns1:value>%s'
                         '</ns1:value></ns1:intValue></ns1:value></ns1:RecordField>' % (k, fid)
                         for k, (fid, _) in enumerate(parts))
        values += ('<ns1:RecordField><ns1:name>FileSize</ns1:name><ns1:value><ns1:intValue><ns1:value>%d'
                   '</ns1:value></ns1:intValue></ns1:value></ns1:RecordField>' % total)
        soap = ('<?xml version="1.0" encoding="UTF-8"?><SOAP-ENV:Envelope xmlns:SOAP-ENV='
                '"http://schemas.xmlsoap.org/soap/envelope/" xmlns:ns1="http://gamespy.net/sake"><SOAP-ENV:Body>'
                '<ns1:CreateRecord><ns1:gameid>2886</ns1:gameid><ns1:tableid>UserContent</ns1:tableid>'
                '<ns1:values>%s</ns1:values></ns1:CreateRecord></SOAP-ENV:Body></SOAP-ENV:Envelope>' % values)
        _, _, reply = request(server, "POST", "/game/SakeStorageServer/StorageServer.asmx", soap.encode(),
                              {"Content-Type": "text/xml", "SOAPAction": '"http://gamespy.net/sake/CreateRecord"'},
                              a_token)
        return reply.decode(errors="replace")
    total = sum(size for _, size in parts)
    check("Success" in create(total), "a record for an upload in 2 parts (%d bytes)" % total)
    check("NoPermission" in create(total + 1), "one with the wrong size refused")
    body, ctype = multipart(os.urandom(9 << 20))
    status, h, _ = request(server, "POST", "/game/SakeFileServer/upload.aspx?gameid=2886&pid=1", body,
                           {"Content-Type": ctype}, a_token)
    check(h.get("Sake-File-Result") == "5" or status == 413, "an upload over the size limit refused (%d)" % status)

    # an entrance's song and movie, kept with the Superstar's file (the server
    # makes songs MP3s at 128 kbps: a WAV comes back as one - media.py)
    song = make_media(["-f", "lavfi", "-i", "sine=frequency=440:duration=5", "-ac", "2", "-f", "wav"])
    status, _, body = request(server, "POST", "/api/media?kind=music", song, token=a_token)
    answer = json.loads(body or b"{}")
    song_sha = answer.get("sha")
    check(status == 200 and song_sha, "entrance song stored: %s" % answer.get("processed"))
    status, _, _ = request(server, "POST", "/api/media?kind=music", os.urandom(50000), token=a_token)
    check(status == 415, "a song that isn't one refused (%d)" % status)
    status, _, _ = request(server, "POST", "/api/media?kind=movie", os.urandom(200000), token=a_token)
    check(status == 415, "a movie that isn't one refused (%d)" % status)
    entrance = {"music": {"playlist": "Glass Shatters", "file": "Glass Shatters.mp3", "sha": song_sha}}
    status, _ = post_json(server, "/api/entrance/%s" % fileid, entrance, b_token)
    check(status == 403, "another player can't set the entrance")
    status, _, _ = request(server, "PUT", "/api/entrance/%s" % fileid, json.dumps(entrance).encode(),
                           {"Content-Type": "application/json"}, a_token)
    check(status == 200, "the owner sets the entrance")
    bad = {"music": {"playlist": "..\\Saves", "file": "x.mp3", "sha": song_sha}}
    status, _, _ = request(server, "PUT", "/api/entrance/%s" % fileid, json.dumps(bad).encode(),
                           {"Content-Type": "application/json"}, a_token)
    check(status == 400, "a song name with a folder in it refused")
    status, _, body = request(server, "GET", "/api/entrance/%s" % fileid, token=b_token)
    got = json.loads(body or b"{}")
    check(status == 200 and got.get("music", {}).get("playlist") == "Glass Shatters", "the other player reads it")
    status, _, back = request(server, "GET", "/api/media/%s" % song_sha, token=b_token)
    check(back[:3] == b"ID3" or back[:1] == bytes([0xFF]), "the song read back (as an MP3)")
    status, _, _ = request(server, "HEAD", "/api/media/%s" % ("0" * 64), token=b_token)
    check(status == 404, "an unknown song: 404")

    # logos: a wrong hash is refused, a right one stored and returned
    logo = os.urandom(gs.LOGO_SIZE)
    good = "%016X" % gs.fnv64(logo)
    _, h, _ = request(server, "POST", "/api/logo/%s" % ("0" * 16), logo, token=a_token)
    check(h.get("Svr2011-Result") == "6", "logo with a wrong hash refused")
    _, h, _ = request(server, "POST", "/api/logo/%s" % good, logo, token=a_token)
    check(h.get("Svr2011-Result") == "0", "logo stored")
    _, h, back = request(server, "GET", "/api/logo/%s" % good, token=b_token)
    check(back == logo, "logo read back by the other player")

    # many at once: the ones over the limit wait for a turn
    def fetch(_):
        try:
            return request(server, "GET", "/api/session", token=b_token, timeout=60)[0]
        except OSError:
            return None
    t = time.time()
    with concurrent.futures.ThreadPoolExecutor(args.requests) as pool:
        results = list(pool.map(fetch, range(args.requests)))
    check(all(r == 200 for r in results), "%d requests at once: %d answered (%.1f s)"
          % (args.requests, sum(r == 200 for r in results), time.time() - t))

    # signing out ends the session
    request(server, "POST", "/api/logout", b"{}", {"Content-Type": "application/json"}, a_token)
    status, _, _ = request(server, "GET", "/api/session", token=a_token)
    check(status == 401, "signed out: the token no longer works")
    status, _, _ = request(server, "GET", "/api/status")
    check(status == 200, "server still answering")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
