"""Checks a running Community Creations server (server.py): accounts and
sign in, uploads come back as they went up, the port's logo store, the size
limit and many requests at once. It makes two throwaway accounts: run it
against a test server (tools/online_server.ps1), not a public one.

    python test_server.py [--server http://127.0.0.1:8411]
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
    args = ap.parse_args()
    server = args.server if "://" in args.server else "http://" + args.server
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok &= bool(cond)

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
