"""The dashboard's own sign-in (server.py), for a server behind a public proxy (the VPS)
where the SSH tunnel's --local-admin isn't wanted every time.

The dashboard and its API are served again under PREFIX (/svr-admin/: nginx passes it);
there, a valid admin session is the only way in - X-Forwarded-For or any other header
never is. The routes outside it keep the old rules (the portal's headers, --local-admin).

- The password: a PBKDF2-SHA256 hash in <data>/admin_password (mode 600), set with
  `server.py --set-admin-password --data <dir>` (asked twice; from stdin when it isn't a
  terminal). Never stored in plain text. Read at each sign-in: no restart after a change.
  With --admin-account NAME instead: that online account's name and its own password.
- Sessions live in memory (a restart signs out): the cookie is for PREFIX only, HttpOnly,
  Secure, SameSite=Strict, 12 h (2 h idle); every POST also carries the session's CSRF
  token (X-CSRF-Token; the dashboard reads it from its page).
- Failed sign-ins: 5 from one address in 15 min lock that address out for 15 min; 20 from
  anywhere lock every sign-in for 15 min. Every failure and lockout is logged."""
import collections
import hashlib
import hmac
import html
import os
import secrets
import time

PREFIX = "/svr-admin"
COOKIE = "svr_admin"
ROUNDS = 240000
LIFE = 12 * 3600         # a session's longest life
IDLE = 2 * 3600          # ... and unused
WINDOW = 15 * 60         # failures counted over this, lockouts last this
PER_IP = 5
OVERALL = 20


def hash_password(password, salt=None, rounds=ROUNDS):
    salt = salt or secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, rounds)
    return "pbkdf2_sha256$%d$%s$%s" % (rounds, salt.hex(), digest.hex())


def verify(password, stored):
    try:
        kind, rounds, salt, digest = stored.strip().split("$")
        if kind != "pbkdf2_sha256":
            return False
        mine = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), bytes.fromhex(salt), int(rounds))
    except (ValueError, TypeError):
        return False
    return hmac.compare_digest(mine.hex(), digest)


def write_password(path, password):
    """The hash into path (mode 600, replaced atomically)."""
    tmp = "%s.%d.tmp" % (path, os.getpid())
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(hash_password(password) + "\n")
    os.replace(tmp, path)


def set_password_cli(path):
    """--set-admin-password: asks for the password (twice on a terminal), stores its hash."""
    import getpass
    import sys
    if sys.stdin.isatty():
        first = getpass.getpass("New dashboard password: ")
        if getpass.getpass("Again: ") != first:
            print("They differ - nothing changed.")
            return 1
    else:
        first = sys.stdin.readline().lstrip("﻿").rstrip("\r\n")  # (a PowerShell pipe starts with a BOM)
    if len(first) < 10:
        print("Use at least 10 characters - nothing changed.")
        return 1
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    write_password(path, first)
    print("Dashboard password set (%s). Sign in at https://<host>%s/" % (path, PREFIX))
    return 0


class AdminAuth:
    def __init__(self, password_file, log, account=None, accounts=None):
        self.password_file = password_file
        self.log = log
        self.account = (account or "").strip()   # (--admin-account: that online account signs in)
        self.accounts = accounts
        self.sessions = {}                       # token -> {"csrf", "at", "seen", "ip", "who"}
        self.fails = collections.defaultdict(collections.deque)   # ip -> failure times
        self.all_fails = collections.deque()
        self.locked_until = {}                   # ip (or "*") -> time

    def configured(self):
        return bool(self.account) or os.path.exists(self.password_file)

    def _stored(self):
        try:
            with open(self.password_file, encoding="utf-8") as f:
                return f.read().strip()
        except OSError:
            return ""

    def lockout(self, ip):
        """Seconds left of a lockout for this address (or every address), else 0."""
        t = time.time()
        left = max(self.locked_until.get(ip, 0), self.locked_until.get("*", 0)) - t
        return int(left) + 1 if left > 0 else 0

    def _failed(self, ip, why):
        t = time.time()
        q = self.fails[ip]
        q.append(t)
        self.all_fails.append(t)
        for d in (q, self.all_fails):
            while d and d[0] < t - WINDOW:
                d.popleft()
        self.log("admin: sign-in refused from %s (%s; %d in 15 min)" % (ip, why, len(q)))
        if len(q) >= PER_IP:
            self.locked_until[ip] = t + WINDOW
            q.clear()
            self.log("admin: %s locked out of the dashboard sign-in for 15 min (%d failures)" % (ip, PER_IP))
        if len(self.all_fails) >= OVERALL:
            self.locked_until["*"] = t + WINDOW
            self.all_fails.clear()
            self.log("admin: EVERY dashboard sign-in locked for 15 min (%d failures from anywhere)" % OVERALL)

    def check(self, ip, name, password):
        """(blocking: PBKDF2) -> (who, None) or (None, why). Counts failures."""
        if self.lockout(ip):
            self.log("admin: sign-in from %s while locked out" % ip)
            return None, "locked"
        if not password or len(password) > 200:
            self._failed(ip, "no password")
            return None, "wrong"
        if self.account:
            if (name or "").strip().lower() != self.account.lower() or not self.accounts:
                self._failed(ip, "not the admin account")
                return None, "wrong"
            acc, _ = self.accounts.check(self.account, password)
            if not acc or acc["name"].lower() != self.account.lower():
                self._failed(ip, "wrong password")
                return None, "wrong"
            return acc["name"], None
        stored = self._stored()
        if not stored:
            return None, "unset"
        if not verify(password, stored):
            self._failed(ip, "wrong password")
            return None, "wrong"
        return "admin", None

    def start(self, ip, who):
        token = secrets.token_urlsafe(32)
        t = time.time()
        self.sessions = {k: s for k, s in self.sessions.items() if self._alive(s, t)}  # (old ones go)
        self.sessions[token] = {"csrf": secrets.token_urlsafe(24), "at": t, "seen": t, "ip": ip, "who": who}
        self.fails.pop(ip, None)
        self.log("admin: %s signed in to the dashboard from %s" % (who, ip))
        return token

    @staticmethod
    def _alive(s, t):
        return t - s["at"] < LIFE and t - s["seen"] < IDLE

    def session(self, request):
        token = request.cookies.get(COOKIE, "")
        s = self.sessions.get(token) if token else None
        if not s:
            return None
        t = time.time()
        if not self._alive(s, t):
            self.sessions.pop(token, None)
            return None
        s["seen"] = t
        return s

    def csrf_ok(self, request, s):
        return hmac.compare_digest(request.headers.get("X-CSRF-Token", ""), s["csrf"])

    def end(self, request):
        s = self.sessions.pop(request.cookies.get(COOKIE, ""), None)
        if s:
            self.log("admin: %s signed out of the dashboard" % s["who"])

    @staticmethod
    def cookie(token, life=LIFE):
        return "%s=%s; Path=%s/; Max-Age=%d; HttpOnly; Secure; SameSite=Strict" % (COOKIE, token, PREFIX, life)

    def login_page(self, error=""):
        why = {"wrong": "Wrong password." if not self.account else "Wrong name or password.",
               "locked": "Too many wrong tries - this address is locked out for a while.",
               "unset": "No dashboard password yet: on the server run server.py --set-admin-password.",
               "expired": "Signed out - sign in again."}.get(error, "")
        unset = not self.configured()
        name_row = ('<label>Account<input name="name" autocomplete="username" required></label>'
                    if self.account else '<input type="hidden" name="name" value="admin">')
        return LOGIN_HTML % {
            "error": '<p class="err">%s</p>' % html.escape(why) if why else "",
            "form": ('<p class="err">No dashboard password yet: on the server run '
                     '<code>server.py --set-admin-password --data &lt;data folder&gt;</code>.</p>' if unset else
                     '<form method="post" action="login">%s<label>Password<input type="password" name="password" '
                     'autocomplete="current-password" required autofocus></label><button>Sign in</button></form>'
                     % name_row)}


LOGIN_HTML = """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>SvR 2011 Online - sign in</title>
<style>
:root { color-scheme: dark; --bg: #0f1115; --panel: #181b22; --line: #2a2f3a; --text: #e8eaf0; --faint: #9aa3b2; }
body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: var(--bg); color: var(--text);
       font: 15px/1.4 system-ui, sans-serif; }
main { width: min(360px, calc(100vw - 32px)); background: var(--panel); border: 1px solid var(--line);
       border-radius: 12px; padding: 24px; }
h1 { font-size: 18px; margin: 0 0 16px; }
label { display: block; margin: 0 0 12px; color: var(--faint); font-size: 13px; }
input { display: block; width: 100%%; box-sizing: border-box; margin-top: 4px; padding: 8px 10px; border-radius: 7px;
        border: 1px solid var(--line); background: var(--bg); color: var(--text); font: inherit; }
button { width: 100%%; padding: 9px; border: 0; border-radius: 7px; background: #c8102e; color: #fff; font: inherit;
         font-weight: 600; cursor: pointer; }
.err { color: #ffb4bb; font-size: 13px; }
code { font-size: 12px; }
</style></head>
<body><main><h1>SvR 2011 Online dashboard</h1>%(error)s%(form)s</main></body></html>
"""
