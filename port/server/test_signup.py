"""Checks the account security (signup.py, 2.1.6 on) on a server of its own: a temporary data folder with an
account from before (the old table), the file mail sender (mails as .eml files), the dashboard on loopback.

  invite sign-up (one use, revoked codes refused), email confirmation by code and by link, the game's online
  sign-in blocked until then, re-activation codes bound to one account, old launchers' texts, forgot / reset
  (same answer for unknown emails, sessions ended), password change, rate limits, --activate, the migration's
  backup.

    python test_signup.py [--port 8461] [--keep]
"""
import argparse
import email
import glob
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))


def call(server, method, path, value=None, token=None, ip=""):
    """(the player's calls: each from its own address - X-Forwarded-For, as nginx sets it - unless ip; the
    dashboard's: none, loopback)"""
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    if "/api/admin/" not in path:
        headers["X-Forwarded-For"] = ip or "10.9.%d.%d" % tuple(os.urandom(2))
    data = json.dumps(value).encode() if value is not None else (b"{}" if method == "POST" else None)
    req = urllib.request.Request(server + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            body, status = r.read(), r.status
    except urllib.error.HTTPError as e:
        body, status = e.read(), e.code
    try:
        return status, json.loads(body or b"{}")
    except ValueError:
        return status, {"text": body.decode(errors="replace")}


def old_database(folder):
    """community.db as 2.1.5 left it: one account (old_one, password 'oldpass1'), no email columns."""
    import hashlib
    db = sqlite3.connect(os.path.join(folder, "community.db"))
    db.executescript("""
        CREATE TABLE accounts (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE COLLATE NOCASE,
            salt TEXT, pwhash TEXT, profileid INTEGER, created REAL, last_seen REAL, banned INTEGER DEFAULT 0, ip TEXT);
        CREATE TABLE sessions (token TEXT PRIMARY KEY, account INTEGER, created REAL, last_used REAL, ip TEXT);
    """)
    for name in ("old_one", "Old_Two"):
        salt = os.urandom(16).hex()
        h = hashlib.pbkdf2_hmac("sha256", b"oldpass1", bytes.fromhex(salt), 240000).hex()
        db.execute("INSERT INTO accounts (name, salt, pwhash, created, last_seen) VALUES (?, ?, ?, ?, ?)",
                   (name, salt, h, time.time(), time.time()))
    db.commit()
    db.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8461)
    ap.add_argument("--keep", action="store_true", help="leave the data folder")
    args = ap.parse_args()
    data = tempfile.mkdtemp(prefix="svr_signup_")
    old_database(data)
    server = "http://127.0.0.1:%d" % args.port
    proc = subprocess.Popen([sys.executable, os.path.join(HERE, "server.py"), "--port", str(args.port), "--data", data,
                             "--local-admin", "--public-url", server, "--admin-account", "Old_Two"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok &= bool(cond)

    def mails(to):
        out = []
        for f in sorted(glob.glob(os.path.join(data, "mail_out", "*.eml"))):
            m = email.message_from_string(open(f, encoding="utf-8").read())
            if m["To"].lower() == to.lower():
                out.append(m.get_payload(decode=True).decode("utf-8"))  # (long lines: quoted-printable)
        return out

    def last_code(to, wait=5.0):
        end = time.time() + wait
        while time.time() < end:
            m = mails(to)
            if m:
                found = re.findall(r"^\s{4}([2-9A-Z]{4}-[2-9A-Z]{4})\s*$", m[-1], re.M)
                if found:
                    return found[-1], m[-1]
            time.sleep(0.2)
        return "", ""

    try:
        for _ in range(100):
            try:
                if call(server, "GET", "/api/status")[0] == 200:
                    break
            except OSError:
                pass
            time.sleep(0.2)
        check(glob.glob(os.path.join(data, "backups", "community-*-before-email.db")), "migration: a backup first")
        cols = {r[1] for r in sqlite3.connect(os.path.join(data, "community.db")).execute("PRAGMA table_info(accounts)")}
        check({"email", "email_ok", "activated"} <= cols, "migration: the new columns")

        # -- invite sign-up --
        st, inv = call(server, "POST", "/api/admin/invites", {"note": "for Bob", "count": 2})
        check(st == 200 and len(inv.get("codes", [])) == 2, "dashboard: 2 invite codes made")
        code, code2 = inv["codes"][0]["code"], inv["codes"][1]["code"]
        st, r = call(server, "POST", "/api/register", {"name": "newbie", "password": "longpass1", "email": "bob@example.com"})
        check(st == 403 and "invite" in r.get("error", ""), "sign-up: an old launcher (no client) is told to update")
        st, r = call(server, "POST", "/api/register", {"name": "newbie", "password": "longpass1", "email": "bob@example.com",
                                                       "invite": "AAAA-BBBB-CCCC", "client": 2})
        check(st == 403 and "invite code" in r.get("error", ""), "sign-up: a wrong invite code refused")
        st, r = call(server, "POST", "/api/register", {"name": "newbie", "password": "longpass1", "email": "not-an-email",
                                                       "invite": code, "client": 2})
        check(st == 403 and "email" in r.get("error", ""), "sign-up: a bad email refused")
        st, r = call(server, "POST", "/api/register", {"name": "OLD_ONE", "password": "longpass1",
                                                       "email": "x@example.com", "invite": code, "client": 2})
        check(st == 403 and "taken" in r.get("error", ""), "sign-up: a name taken in other case refused")
        st, r = call(server, "POST", "/api/register", {"name": "newbie", "password": "short", "email": "bob@example.com",
                                                       "invite": code, "client": 2})
        check(st == 403 and "8 characters" in r.get("error", ""), "sign-up: a short password refused")
        st, r = call(server, "POST", "/api/register", {"name": "newbie", "password": "longpass1",
                                                       "email": "Bob@Example.com", "invite": code.lower(), "client": 2})
        check(st == 200 and r.get("token") and r.get("needs_confirm") and not r.get("online_ok"),
              "sign-up: with the code - signed in, the email to confirm")
        bob = r.get("token", "")
        st, r = call(server, "POST", "/api/register", {"name": "newbie2", "password": "longpass1",
                                                       "email": "other@example.com", "invite": code, "client": 2})
        check(st == 403, "sign-up: the same invite code twice refused")
        st, r = call(server, "POST", "/api/register", {"name": "newbie2", "password": "longpass1",
                                                       "email": "BOB@example.com", "invite": code2, "client": 2})
        check(st == 403 and "email" in r.get("error", ""), "sign-up: an email with an account already refused")
        st, r = call(server, "GET", "/api/session?client=2", token=bob)
        check(st == 403 and "Confirm" in r.get("error", ""), "game: online refused until the email is confirmed")
        st, r = call(server, "GET", "/api/session", token=bob)
        check(st == 403 and "2.1.6" in r.get("error", ""), "game: a 2.1.5 game's text says to update")
        st, r = call(server, "GET", "/api/friends?client=2", token=bob)
        check(st == 403, "friends / presence refused too")

        # -- confirm by code --
        mcode, text = last_code("Bob@Example.com")
        check(mcode and "/api/email/confirm?t=" in text, "mail: the confirm code and link came")
        st, r = call(server, "POST", "/api/email/confirm", {"code": "2222-2222"}, token=bob)
        check(st == 403, "confirm: a wrong code refused")
        st, r = call(server, "POST", "/api/email/confirm", {"code": mcode.lower().replace("-", " ")}, token=bob)
        check(st == 200 and r.get("online_ok"), "confirm: the code (any case / spacing) - online ok")
        st, r = call(server, "POST", "/api/email/confirm", {"code": mcode}, token=bob)
        check(st == 403 or r.get("email_ok"), "confirm: the code works once")
        st, r = call(server, "GET", "/api/session", token=bob)
        check(st == 200 and r.get("ticket"), "game: online sign-in works now")

        # -- an account from before: re-activation code, then an email (link) --
        st, r = call(server, "POST", "/api/login", {"name": "old_one", "password": "oldpass1"})
        check(st == 403 and "re-activation code" in r.get("error", "") and "2.1.6" in r.get("error", ""),
              "old account, old launcher: told to update and get the code")
        st, r = call(server, "POST", "/api/login", {"name": "old_one", "password": "oldpass1", "client": 2})
        check(st == 200 and r.get("needs_code") and r.get("needs_email"), "old account, 2.1.6 launcher: signed in, steps")
        old = r.get("token", "")
        st, r = call(server, "GET", "/api/session", token=old)
        check(st == 403 and "re-activation" in r.get("error", ""), "old account: online refused (old game text)")
        users = call(server, "GET", "/api/admin/users")[1]
        ids = {u["name"]: u["id"] for u in users}
        st, rc_bob = call(server, "POST", "/api/admin/user/%d/reactivation-code" % ids["newbie"])
        st, rc = call(server, "POST", "/api/admin/user/%d/reactivation-code" % ids["old_one"])
        check(st == 200 and rc.get("code"), "dashboard: a re-activation code for old_one")
        st, r = call(server, "POST", "/api/reactivate", {"code": rc_bob.get("code", "")}, token=old)
        check(st == 403, "re-activation: another account's code refused")
        st, r = call(server, "POST", "/api/register", {"name": "sneaky", "password": "longpass1",
                                                       "email": "s@example.com", "invite": rc["code"], "client": 2})
        check(st == 403, "re-activation code refused for a sign-up")
        st, r = call(server, "POST", "/api/reactivate", {"code": rc["code"]}, token=old)
        check(st == 200 and not r.get("needs_code") and r.get("needs_email"), "re-activation: its own code - active")
        st, r = call(server, "POST", "/api/reactivate", {"code": rc["code"]}, token=old)
        check(st == 403, "re-activation: the code works once")
        st, r = call(server, "POST", "/api/email", {"email": "old@example.com"}, token=old)
        check(st == 200 and r.get("needs_confirm"), "add email: waiting for its confirmation")
        _, text = last_code("old@example.com")
        link = re.search(r"(/api/email/confirm\?t=\S+)", text)
        st, r = call(server, "GET", link.group(1) if link else "/api/email/confirm?t=x")
        check(st == 200 and "confirmed" in r.get("text", ""), "confirm: the mail's link")
        st, r = call(server, "GET", "/api/session", token=old)
        check(st == 200, "old account: online again")

        # -- the admin account (--admin-account): no re-activation code --
        st, r = call(server, "POST", "/api/login", {"name": "old_two", "password": "oldpass1", "client": 2})
        check(st == 200 and not r.get("needs_code") and r.get("needs_email"), "--admin-account: no code, an email")

        # -- forgot / reset --
        st, a = call(server, "POST", "/api/password/forgot", {"email": "nobody@example.com"})
        st, b = call(server, "POST", "/api/password/forgot", {"email": "bob@example.com"})
        check(a == b and a.get("ok"), "forgot: the same answer for an unknown email")
        check(not mails("nobody@example.com"), "forgot: no mail for an unknown email")
        rcode, _ = last_code("bob@example.com")
        while rcode and "password" not in mails("bob@example.com")[-1]:
            time.sleep(0.2)
            rcode, _ = last_code("bob@example.com")
        st, r = call(server, "POST", "/api/password/reset", {"email": "bob@example.com", "code": "2222-3333",
                                                             "password": "newpass99"})
        check(st == 403, "reset: a wrong code refused")
        st, r = call(server, "POST", "/api/password/reset", {"email": "bob@example.com", "code": rcode,
                                                             "password": "newpass99"})
        check(st == 200, "reset: the mailed code sets the password")
        check(call(server, "GET", "/api/account", token=bob)[0] == 401, "reset: the account's sessions ended")
        st, r = call(server, "POST", "/api/password/reset", {"email": "bob@example.com", "code": rcode,
                                                             "password": "again999"})
        check(st == 403, "reset: the code works once")
        st, r = call(server, "POST", "/api/login", {"name": "bob@example.com", "password": "newpass99", "client": 2})
        check(st == 200 and r.get("online_ok"), "sign in with the email and the new password")
        bob = r.get("token", "")
        st, r2 = call(server, "POST", "/api/login", {"name": "newbie", "password": "newpass99", "client": 2})
        other = r2.get("token", "")

        # -- password change: the other sessions end --
        st, r = call(server, "POST", "/api/password/change", {"password": "wrong", "new_password": "x" * 9}, token=bob)
        check(st == 403, "change: the old password is asked")
        st, r = call(server, "POST", "/api/password/change", {"password": "newpass99", "new_password": "third999"},
                     token=bob)
        check(st == 200, "change: done")
        check(call(server, "GET", "/api/account", token=bob)[0] == 200, "change: this session stays")
        check(call(server, "GET", "/api/account", token=other)[0] == 401, "change: the other session ended")

        # -- revoke, limits, the dashboard's email --
        st, inv = call(server, "POST", "/api/admin/invites", {"note": "revoked"})
        iid, rcode = inv["codes"][0]["id"], inv["codes"][0]["code"]
        call(server, "POST", "/api/admin/invite/%d/revoke" % iid)
        st, r = call(server, "POST", "/api/register", {"name": "late", "password": "longpass1", "email": "l@example.com",
                                                       "invite": rcode, "client": 2})
        check(st == 403, "a revoked invite refused")
        listed = call(server, "GET", "/api/admin/invites")[1].get("invites", [])
        used = [i for i in listed if i["used_by_name"] == "newbie"]
        check(used and used[0]["note"] == "for Bob", "Invites tab: who used each code")
        check(any(i["kind"] == "reactivate" and i["used_by_name"] == "old_one" for i in listed),
              "Invites tab: the re-activation codes too")
        db_codes = [r[0] for r in sqlite3.connect(os.path.join(data, "community.db")).execute("SELECT hash FROM invites")]
        check(all(len(h) == 64 and "-" not in h for h in db_codes), "codes kept as hashes only")
        tries = [call(server, "POST", "/api/email/confirm", {"code": "3333-4444"}, token=old)[1] for _ in range(12)]
        check("Too many" in tries[-1].get("error", ""), "confirm: rate limited per account")
        st, r = call(server, "POST", "/api/admin/user/%d/email" % ids["Old_Two"], {"email": "kaiko@example.com"})
        check(st == 200 and last_code("kaiko@example.com")[0], "dashboard: set an email - a confirm mail goes")
        st, r = call(server, "POST", "/api/admin/user/%d/email" % ids["Old_Two"], {"email": "OLD@example.com"})
        check(st == 400, "dashboard: an email of another account refused")
        user = [u for u in call(server, "GET", "/api/admin/users")[1] if u["name"] == "Old_Two"][0]
        check(user["email"] == "kaiko@example.com" and not user["email_ok"] and user["activated"],
              "dashboard: the account's state")
    finally:
        proc.terminate()
        proc.wait(10)
    r = subprocess.run([sys.executable, os.path.join(HERE, "server.py"), "--data", data, "--activate", "newbie2"],
                       capture_output=True, text=True)
    check(r.returncode == 1, "--activate: an unknown name says so")
    if args.keep:
        print("data:", data)
    else:
        shutil.rmtree(data, ignore_errors=True)
    print("ALL OK" if ok else "SOME CHECKS FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
