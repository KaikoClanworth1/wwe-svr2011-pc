"""Checks the supporters list (supporters.py) on a server of its own: a temporary data folder, a loopback port,
fake Ko-fi webhook calls and a mock Patreon API (a local HTTP server) - nothing reaches Ko-fi or Patreon.

    python test_supporters.py [--port 8563] [--keep]
"""
import argparse
import http.server
import json
import os
import secrets
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BAD = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        BAD.append(what)


def call(server, path, method="GET", value=None, form=None):
    headers, data = {}, None
    if form is not None and method == "GET":
        method = "POST"
    if form is not None:
        data = urllib.parse.urlencode(form).encode()
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    elif value is not None or method == "POST":
        data = json.dumps(value if value is not None else {}).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(server + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}


def kofi_data(token, mid, name, public=True, kind="Donation"):
    """A Ko-fi webhook body as Ko-fi sends it (with the email and amount the server must never keep)."""
    return {"data": json.dumps({
        "verification_token": token, "message_id": mid, "timestamp": "2026-11-15T10:00:00Z", "type": kind,
        "is_public": public, "from_name": name, "message": "Keep it up!", "amount": "3.00", "url": "https://ko-fi.com/x",
        "email": "someone@example.invalid", "currency": "USD", "is_subscription_payment": kind == "Subscription",
        "is_first_subscription_payment": False, "kofi_transaction_id": "tx-" + mid})}


class MockPatreon(http.server.BaseHTTPRequestHandler):
    """API v2's two calls: /campaigns and /campaigns/{id}/members (two pages). The bearer token is checked."""
    token = ""
    members = []   # [(id, full_name, patron_status)]
    seen = []

    def log_message(self, *a):
        pass

    def do_GET(self):
        MockPatreon.seen.append(self.path)
        if self.headers.get("Authorization") != "Bearer " + MockPatreon.token:
            self.send_response(401)
            self.end_headers()
            return
        base = "http://127.0.0.1:%d" % self.server.server_port
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/campaigns":
            body = {"data": [{"id": "4242", "type": "campaign"}]}
        elif u.path == "/campaigns/4242/members":
            page = int(q.get("page[cursor]", ["0"])[0])
            ms = MockPatreon.members[page * 2:page * 2 + 2]
            body = {"data": [{"id": i, "type": "member", "attributes": {"full_name": n, "patron_status": s}}
                             for i, n, s in ms]}
            if (page + 1) * 2 < len(MockPatreon.members):
                body["links"] = {"next": base + "/campaigns/4242/members?page%%5Bcursor%%5D=%d" % (page + 1)}
            assert q.get("fields[member]") in (["full_name,patron_status"], None)
        else:
            self.send_response(404)
            self.end_headers()
            return
        out = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)


def names(server):
    st, d = call(server, "/api/supporters")
    return st, [(s["name"], s["platform"]) for s in d.get("supporters", [])], d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8563)  # (outside 8460-8499: the test tools' free-port range)
    ap.add_argument("--keep", action="store_true", help="leave the data folder")
    args = ap.parse_args()
    data = tempfile.mkdtemp(prefix="svr_supporters_")
    print("data folder:", data)
    kofi_token, patreon_token = secrets.token_hex(16), secrets.token_hex(20)
    kofi_file, patreon_file = os.path.join(data, "kofi_token"), os.path.join(data, "patreon_token")
    open(kofi_file, "w").write(kofi_token + "\n")
    open(patreon_file, "w").write(patreon_token + "\n")
    MockPatreon.token = patreon_token
    MockPatreon.members = [("m1", "Carol Patron", "active_patron"), ("m2", "Eve Former", "former_patron"),
                           ("m3", "Dave Patron", "active_patron"), ("m4", "Gus Declined", "declined_patron")]
    mock = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MockPatreon)
    threading.Thread(target=mock.serve_forever, daemon=True).start()
    log = os.path.join(data, "server.log")
    env = dict(os.environ, SVR2011_PATREON_EVERY="3600")
    proc = subprocess.Popen([sys.executable, os.path.join(HERE, "server.py"), "--port", str(args.port), "--data", data,
                             "--local-admin", "--log", log, "--kofi-token-file", kofi_file,
                             "--patreon-token-file", patreon_file,
                             "--patreon-api", "http://127.0.0.1:%d" % mock.server_port],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, env=env)
    server = "http://127.0.0.1:%d" % args.port
    try:
        for _ in range(100):
            try:
                urllib.request.urlopen(server + "/__health", timeout=1)
                break
            except OSError:
                time.sleep(0.2)
        st, lst, d = names(server)
        check(st == 200 and lst == [] and "kofi" in d.get("links", {}), "empty list at first (with the 3 links)")

        # Ko-fi
        st, d = call(server, "/api/supporters/kofi", form=kofi_data(kofi_token, "k1", "Alice Kofi"))
        check(st == 200 and d.get("ok"), "Ko-fi: good token -> 200 (%s %s)" % (st, d))
        st, lst, _ = names(server)
        check(lst == [("Alice Kofi", "kofi")], "Ko-fi: the public supporter is listed")
        st, d = call(server, "/api/supporters/kofi", form=kofi_data("wrong" + kofi_token, "k2", "Mallory"))
        check(st == 403, "Ko-fi: bad token -> 403")
        st, d = call(server, "/api/supporters/kofi", form=kofi_data(kofi_token, "k1", "Alice Kofi"))
        check(st == 200 and d.get("duplicate"), "Ko-fi: the same message_id again -> 200, duplicate")
        st, d = call(server, "/api/supporters/kofi", form=kofi_data(kofi_token, "k3", "Alice Kofi", kind="Subscription"))
        st, lst, _ = names(server)
        check(lst == [("Alice Kofi", "kofi")], "Ko-fi: one row per supporter (a second payment, the duplicate)")
        st, d = call(server, "/api/supporters/kofi", form=kofi_data(kofi_token, "k4", "Bob Private", public=False))
        st, lst, _ = names(server)
        check(st == 200 and ("Bob Private", "kofi") not in lst, "Ko-fi: is_public false -> stored, never listed")
        st, d = call(server, "/api/supporters/kofi", form={"nodata": "1"})
        check(st == 400, "Ko-fi: no data field -> 400")
        st, d = call(server, "/api/supporters/kofi", form={"data": "{not json"})
        check(st == 400, "Ko-fi: broken JSON -> 400")
        flagged = "hit" + "ler"
        call(server, "/api/supporters/kofi", form=kofi_data(kofi_token, "k5", flagged + " fan"))
        st, lst, _ = names(server)
        check(all(flagged not in n for n, _ in lst), "Ko-fi: a name the name filter flags comes in hidden")

        # Patreon (the mock)
        st, d = call(server, "/api/admin/supporters/patreon", "POST")
        check(st == 200 and d.get("result", "").startswith("2 active"), "Patreon: poll -> %s" % d.get("result"))
        st, lst, _ = names(server)
        check(("Carol Patron", "patreon") in lst and ("Dave Patron", "patreon") in lst and
              not any(n in ("Eve Former", "Gus Declined") for n, _ in lst), "Patreon: active patrons only, two pages")
        MockPatreon.members = [("m1", "Carol Patron", "active_patron"), ("m3", "Dave Patron", "former_patron")]
        st, d = call(server, "/api/admin/supporters/patreon", "POST")
        st, lst, _ = names(server)
        check(("Dave Patron", "patreon") not in lst and ("Carol Patron", "patreon") in lst,
              "Patreon: a patron no longer active leaves the list (%s)" % d.get("result"))
        bad_tok = MockPatreon.token
        MockPatreon.token = "another"
        st, d = call(server, "/api/admin/supporters/patreon", "POST")
        check("401" in d.get("result", ""), "Patreon: a refused token -> %s" % d.get("result"))
        MockPatreon.token = bad_tok

        # the dashboard
        st, d = call(server, "/api/admin/supporters")
        rows = {r["name"]: r for r in d.get("rows", [])}
        check(st == 200 and d.get("kofi") is True and d["patreon"]["on"] is True, "dashboard: the sources' state")
        check(rows.get("Bob Private", {}).get("public") == 0, "dashboard: the private one is there, public 0")
        check(any(n.endswith(" fan") and r["hidden"] == 1 for n, r in rows.items()),
              "dashboard: the flagged name is stored hidden")
        alice = rows["Alice Kofi"]["id"]
        call(server, "/api/admin/supporter/%d/hide" % alice, "POST")
        st, lst, _ = names(server)
        check(("Alice Kofi", "kofi") not in lst, "dashboard: Hide -> not listed")
        call(server, "/api/supporters/kofi", form=kofi_data(kofi_token, "k6", "Alice Kofi"))
        st, lst, _ = names(server)
        check(("Alice Kofi", "kofi") not in lst, "dashboard: a hidden name stays hidden after another payment")
        call(server, "/api/admin/supporter/%d/show" % alice, "POST")
        st, lst, _ = names(server)
        check(("Alice Kofi", "kofi") in lst, "dashboard: Show -> listed again")
        st, d = call(server, "/api/admin/supporters/add", "POST", {"name": "  Frank   PayPal ", "platform": "paypal"})
        st2, lst, _ = names(server)
        check(st == 200 and lst[0] == ("Frank PayPal", "paypal"), "dashboard: added by hand, newest first")
        st, d2 = call(server, "/api/admin/supporters/add", "POST", {"name": "Frank PayPal", "platform": "paypal"})
        check(st == 400, "dashboard: the same name by hand twice -> refused")
        st, d2 = call(server, "/api/admin/supporters/add", "POST", {"name": "Zed", "platform": "kofi"})
        check(st == 400, "dashboard: by hand only PayPal / other")
        st, d2 = call(server, "/api/admin/supporter/%d/remove" % alice, "POST")
        check(st == 404, "dashboard: Remove refuses a Ko-fi supporter")
        call(server, "/api/admin/supporter/%d/remove" % d["id"], "POST")
        st, lst, _ = names(server)
        check(("Frank PayPal", "paypal") not in lst, "dashboard: Remove takes a hand-added one off")

        # Ko-fi off (no token file): 503
        os.remove(kofi_file)
        st, d = call(server, "/api/supporters/kofi", form=kofi_data(kofi_token, "k7", "Late"))
        check(st == 503, "Ko-fi off (no token file) -> 503")

        # the public list's fields; nothing kept that shouldn't be; no token in the log
        st, lst, d = names(server)
        check(all(set(s) == {"name", "platform", "since"} for s in d["supporters"]), "public list: name, platform, since only")
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        mock.shutdown()
    db = sqlite3.connect(os.path.join(data, "community.db"))
    dump = "\n".join(db.iterdump())
    db.close()
    check("example.invalid" not in dump and "3.00" not in dump and "Keep it up" not in dump and "tx-k" not in dump,
          "database: no email, amount, message or transaction id")
    check(kofi_token not in dump and patreon_token not in dump, "database: no token")
    text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
    check(text and kofi_token not in text and patreon_token not in text and "example.invalid" not in text,
          "server log: no token, no email (%d lines)" % text.count("\n"))
    if args.keep:
        print("data kept:", data)
    else:
        shutil.rmtree(data, ignore_errors=True)
    if BAD:
        print("FAILED (%d)" % len(BAD))
        return 1
    print("ALL OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
