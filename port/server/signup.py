"""Account security for the Community Creations server (server.py, 2.1.6 on): invite-code sign-up,
re-activation codes for the accounts from before, email confirmation and password resets by mail.

  - Sign-up needs a one-use INVITE code (the dashboard makes them, one per person) and an email.
  - Accounts from before 2.1.6 are blocked online until the player enters the RE-ACTIVATION code the server
    owner made for that one account (the dashboard's account row) - and, like everyone, confirms an email.
  - Online play (the game's /api/session, presence, matches, Community Creations) needs an active account
    with a confirmed email; the launcher's own account requests work before that (to add the email).
  - Mails carry an 8-character code the player types in the launcher (confirm: 24 h; password reset: 30 min;
    each works once; a newer one replaces the older). The confirm mail has a one-click link too.
  - Codes and tokens are kept only as SHA-256 hashes. A password reset or change ends the account's sessions.
  - Rate limits per IP and per account / email (memory only). "Forgot password" answers the same whether
    or not the email has an account.
"""

import collections
import hashlib
import os
import re
import secrets
import sqlite3
import time

ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"  # (no 0/O, 1/I: typed from a mail or a message)
EMAIL_RE = re.compile(r"^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?"
                      r"(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+$")
CONFIRM_LIFE = 24 * 3600
RESET_LIFE = 30 * 60
MIN_PASSWORD = 8
KINDS = ("signup", "reactivate")

# The texts an old (2.1.5) launcher or game shows - its sign-in's error, as it is.
OLD_NEEDS_CODE = ("Online needs the 2.1.6 launcher now: update, then sign in there with the re-activation code "
                  "the server owner gives you, and add an email.")
OLD_NEEDS_EMAIL = "Online needs an email on your account now: update to 2.1.6 and add it in the launcher's Online tab."
NEEDS_CODE = "Your account needs its re-activation code (from the server owner) to play online again."
NEEDS_EMAIL = "Add an email to your account to play online."
NEEDS_CONFIRM = "Confirm your email to play online: type the code from the mail we sent."


def now():
    return time.time()


def normalize(code):
    return re.sub(r"[^0-9A-Za-z]", "", code or "").upper()


def new_code(n):
    c = "".join(secrets.choice(ALPHABET) for _ in range(n))
    return "-".join(c[i:i + 4] for i in range(0, n, 4))


def code_hash(kind, code):
    return hashlib.sha256(("%s:%s" % (kind, normalize(code))).encode()).hexdigest()


def valid_email(e):
    return isinstance(e, str) and len(e) <= 254 and bool(EMAIL_RE.match(e)) and len(e.split("@")[0]) <= 64


class Limiter:
    """At most `limit` tries per `window` seconds for each key (memory only)."""

    def __init__(self):
        self.q = collections.defaultdict(collections.deque)

    def hit(self, key, limit, window):
        q = self.q[key]
        while q and q[0] < now() - window:
            q.popleft()
        if len(q) >= limit:
            return True
        q.append(now())
        return False


class Signup:
    def __init__(self, store, accounts, mailer, log, public_url="", open_accounts=False, exempt=""):
        self.store, self.db, self.lock = store, store.db, store.lock
        self.accounts, self.mailer, self.log = accounts, mailer, log
        self.public_url = public_url.rstrip("/")
        self.open = open_accounts          # (--test-open-accounts: a private test server - no codes, no email)
        self.limits = Limiter()
        self.migrate(exempt)

    # -- the database ----------------------------------------------------------------------------------

    def migrate(self, exempt):
        with self.lock:
            cols = {r[1] for r in self.db.execute("PRAGMA table_info(accounts)")}
            if "email" not in cols:
                self.backup("before-email")
                n = self.db.execute("SELECT COUNT(*) FROM accounts").fetchone()[0]
                # (the accounts from before: email none, not active until their re-activation code)
                self.db.executescript("""
                    ALTER TABLE accounts ADD COLUMN email TEXT;
                    ALTER TABLE accounts ADD COLUMN email_ok INTEGER DEFAULT 0;
                    ALTER TABLE accounts ADD COLUMN activated INTEGER DEFAULT 0;
                """)
                self.log("accounts: added email / activation (%d account(s) from before: blocked online until "
                         "their re-activation code and a confirmed email)" % n)
            self.db.executescript("""
                CREATE UNIQUE INDEX IF NOT EXISTS accounts_email ON accounts (email COLLATE NOCASE)
                    WHERE email IS NOT NULL;
                CREATE TABLE IF NOT EXISTS invites (
                    id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT, hash TEXT UNIQUE, note TEXT,
                    account INTEGER, created REAL, used_by INTEGER, used_at REAL, revoked REAL);
                CREATE TABLE IF NOT EXISTS mail_codes (
                    hash TEXT PRIMARY KEY, link TEXT, account INTEGER, kind TEXT, email TEXT,
                    created REAL, expires REAL);
                CREATE INDEX IF NOT EXISTS mail_codes_account ON mail_codes (account, kind);
            """)
            if exempt:  # (--admin-account: the server owner's own account never needs a re-activation code)
                if self.db.execute("UPDATE accounts SET activated = 1 WHERE name = ? AND activated = 0",
                                   (exempt,)).rowcount:
                    self.log("accounts: %s active without a re-activation code (--admin-account)" % exempt)
            self.db.execute("DELETE FROM mail_codes WHERE expires < ?", (now(),))
            self.db.commit()

    def backup(self, why):
        folder = os.path.join(self.store.folder, "backups")
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, "community-%s-%s.db" % (time.strftime("%Y%m%d-%H%M%S"), why))
        dst = sqlite3.connect(path)
        try:
            self.db.backup(dst)
        finally:
            dst.close()
        self.log("accounts: database backed up to %s" % path)
        return path

    # -- the account's state ---------------------------------------------------------------------------

    def state(self, account_id):
        with self.lock:
            row = self.db.execute("SELECT email, email_ok, activated FROM accounts WHERE id = ?",
                                  (account_id,)).fetchone()
        if not row:
            return None
        return {"email": row[0] or "", "email_ok": bool(row[1]), "activated": bool(row[2])}

    def online_ok(self, account):
        if self.open:
            return True
        s = self.state(account["id"])
        return bool(s and s["activated"] and s["email_ok"])

    def blocked_why(self, account, old_client):
        """'' when it may play online, else what the player has to do (an old client's text: update)."""
        if self.open:
            return ""
        s = self.state(account["id"])
        if not s:
            return "No such account."
        if not s["activated"]:
            return OLD_NEEDS_CODE if old_client else NEEDS_CODE
        if not s["email"]:
            return OLD_NEEDS_EMAIL if old_client else NEEDS_EMAIL
        if not s["email_ok"]:
            return OLD_NEEDS_EMAIL if old_client else NEEDS_CONFIRM
        return ""

    def account_json(self, account):
        s = self.state(account["id"]) or {}
        return {"name": account["name"], "email": s.get("email", ""), "email_ok": s.get("email_ok", False),
                "needs_code": not self.open and not s.get("activated"),
                "needs_email": not self.open and not s.get("email"),
                "needs_confirm": not self.open and bool(s.get("email")) and not s.get("email_ok"),
                "online_ok": self.online_ok(account)}

    # -- invite and re-activation codes (the dashboard) -------------------------------------------------

    def make_code(self, kind, note="", account=None):
        """A new one-use code (shown once: only its hash is kept)."""
        assert kind in KINDS
        code = new_code(12)
        with self.lock:
            if kind == "reactivate":  # (one live code per account: a new one replaces the old)
                self.db.execute("UPDATE invites SET revoked = ? WHERE kind = 'reactivate' AND account = ? AND "
                                "used_by IS NULL AND revoked IS NULL", (now(), account))
            cur = self.db.execute("INSERT INTO invites (kind, hash, note, account, created) VALUES (?, ?, ?, ?, ?)",
                                  (kind, code_hash("invite", code), note[:80], account, now()))
            self.db.commit()
        return cur.lastrowid, code

    def revoke(self, invite_id):
        with self.lock:
            n = self.db.execute("UPDATE invites SET revoked = ? WHERE id = ? AND used_by IS NULL AND revoked IS NULL",
                                (now(), invite_id)).rowcount
            self.db.commit()
        return n > 0

    def invites(self):
        with self.lock:
            rows = self.db.execute(
                "SELECT i.id, i.kind, i.note, i.account, a.name, i.created, i.used_by, u.name, i.used_at, i.revoked "
                "FROM invites i LEFT JOIN accounts a ON a.id = i.account LEFT JOIN accounts u ON u.id = i.used_by "
                "ORDER BY i.id DESC").fetchall()
        keys = ("id", "kind", "note", "account", "account_name", "created", "used_by", "used_by_name", "used_at",
                "revoked")
        return [dict(zip(keys, r)) for r in rows]

    def _take(self, kind, code, account_id=None):
        """The live code's row id (under the lock), or None."""
        row = self.db.execute("SELECT id, account FROM invites WHERE hash = ? AND kind = ? AND used_by IS NULL AND "
                              "revoked IS NULL", (code_hash("invite", code), kind)).fetchone()
        if not row or (kind == "reactivate" and row[1] != account_id):
            return None
        return row[0]

    def reactivate(self, account, code, ip):
        if self.limits.hit(("code", account["id"]), 8, 1800) or self.limits.hit(("code-ip", ip), 20, 1800):
            return "Too many tries - wait half an hour."
        with self.lock:
            iid = self._take("reactivate", code, account["id"])
            if not iid:
                return "That code isn't right for this account (or it was used or replaced)."
            self.db.execute("UPDATE invites SET used_by = ?, used_at = ? WHERE id = ?", (account["id"], now(), iid))
            self.db.execute("UPDATE accounts SET activated = 1 WHERE id = ?", (account["id"],))
            self.db.commit()
        self.accounts.cache.clear()
        self.log("accounts: %s re-activated (code %d) from %s" % (account["name"], iid, ip))
        return ""

    def set_active(self, account_id):
        with self.lock:
            self.db.execute("UPDATE accounts SET activated = 1 WHERE id = ?", (account_id,))
            self.db.commit()
        self.accounts.cache.clear()

    # -- sign-up -----------------------------------------------------------------------------------------

    def register(self, name, password, email, invite, ip):
        """(account, error) - the account active, its email waiting for the confirm code."""
        if not self.open and self.limits.hit(("signup-ip", ip), 5, 3600):  # (test servers make many)
            return None, "Too many sign-ups from here - wait an hour."
        email = (email or "").strip()
        if not self.open:
            if not valid_email(email):
                return None, "That email doesn't look right."
            if len(password) < MIN_PASSWORD:
                return None, "Passwords need at least %d characters." % MIN_PASSWORD
        with self.lock:
            iid = None
            if not self.open:
                iid = self._take("signup", invite)
                if not iid:
                    return None, "That invite code isn't right (or it was used)."
                if self.db.execute("SELECT 1 FROM accounts WHERE email = ? COLLATE NOCASE", (email,)).fetchone():
                    return None, "That email has an account already (Forgot password? signs you in again)."
            account, error = self.accounts.register(name, password, ip)
            if not account:
                return None, error
            self.db.execute("UPDATE accounts SET email = ?, email_ok = 0, activated = 1 WHERE id = ?",
                            (email or None, account["id"]))
            if iid:
                self.db.execute("UPDATE invites SET used_by = ?, used_at = ? WHERE id = ?", (account["id"], now(), iid))
            self.db.commit()
        if email and not self.open:
            self.send_confirm(account, email)
        return account, None

    # -- email ---------------------------------------------------------------------------------------------

    def set_email(self, account, email, by_admin=False):
        email = (email or "").strip()
        if not valid_email(email):
            return "That email doesn't look right."
        if not by_admin and self.limits.hit(("email", account["id"]), 5, 3600):
            return "Too many changes - wait an hour."
        with self.lock:
            other = self.db.execute("SELECT id FROM accounts WHERE email = ? COLLATE NOCASE", (email,)).fetchone()
            if other and other[0] != account["id"]:
                return "That email has another account."
            self.db.execute("UPDATE accounts SET email = ?, email_ok = 0 WHERE id = ?", (email, account["id"]))
            self.db.commit()
        self.accounts.cache.clear()
        self.send_confirm(account, email)
        return ""

    def _issue(self, account_id, kind, email, life):
        code, link = new_code(8), secrets.token_urlsafe(24)
        with self.lock:
            self.db.execute("DELETE FROM mail_codes WHERE account = ? AND kind = ?", (account_id, kind))
            self.db.execute("INSERT INTO mail_codes VALUES (?, ?, ?, ?, ?, ?, ?)",
                            (code_hash(kind + str(account_id), code), hashlib.sha256(link.encode()).hexdigest(),
                             account_id, kind, email, now(), now() + life))
            self.db.commit()
        return code, link

    def send_confirm(self, account, email):
        code, link = self._issue(account["id"], "confirm", email, CONFIRM_LIFE)
        text = ("Hi %s,\n\nYour code to confirm this email for SvR2011 Online:\n\n    %s\n\n"
                "Type it in the launcher's Online tab (Confirm email). It works once, for 24 hours.\n"
                % (account["name"], code))
        if self.public_url:
            text += "\nOr open this link to confirm it:\n%s/api/email/confirm?t=%s\n" % (self.public_url, link)
        text += "\nIf you didn't sign up, ignore this mail.\n"
        self.mailer.send(email, "Confirm your SvR2011 Online email", text)

    def resend_confirm(self, account):
        s = self.state(account["id"])
        if not s or not s["email"]:
            return NEEDS_EMAIL
        if s["email_ok"]:
            return "Your email is confirmed already."
        if self.limits.hit(("resend", account["id"]), 3, 3600):
            return "Mail sent a few times already - wait an hour (check the spam folder)."
        self.send_confirm(account, s["email"])
        return ""

    def _confirmed(self, row):
        account_id, email = row
        with self.lock:
            self.db.execute("DELETE FROM mail_codes WHERE account = ? AND kind = 'confirm'", (account_id,))
            n = self.db.execute("UPDATE accounts SET email_ok = 1 WHERE id = ? AND email = ? COLLATE NOCASE",
                                (account_id, email)).rowcount
            self.db.commit()
        self.accounts.cache.clear()
        return n > 0

    def confirm(self, account, code, ip):
        if self.limits.hit(("confirm", account["id"]), 10, 1800) or self.limits.hit(("confirm-ip", ip), 30, 1800):
            return "Too many tries - wait half an hour."
        with self.lock:
            row = self.db.execute("SELECT account, email FROM mail_codes WHERE hash = ? AND kind = 'confirm' AND "
                                  "expires >= ?", (code_hash("confirm" + str(account["id"]), code), now())).fetchone()
        if not row or not self._confirmed(row):
            return "That code isn't right (or it's old: Resend makes a new one)."
        self.log("accounts: %s confirmed its email" % account["name"])
        return ""

    def confirm_link(self, link, ip):
        if self.limits.hit(("confirm-ip", ip), 30, 1800):
            return False
        with self.lock:
            row = self.db.execute("SELECT account, email FROM mail_codes WHERE link = ? AND kind = 'confirm' AND "
                                  "expires >= ?", (hashlib.sha256((link or "").encode()).hexdigest(), now())).fetchone()
        return bool(row) and self._confirmed(row)

    # -- passwords ----------------------------------------------------------------------------------------------

    def forgot(self, email, ip):
        """Mails a reset code when the email has a (confirmed or not) account; the answer is the same either way."""
        email = (email or "").strip()
        if not valid_email(email) or self.limits.hit(("forgot-ip", ip), 5, 3600) or \
                self.limits.hit(("forgot", email.lower()), 3, 3600):
            return
        with self.lock:
            row = self.db.execute("SELECT id, name, banned FROM accounts WHERE email = ? COLLATE NOCASE",
                                  (email,)).fetchone()
        if not row or row[2]:
            self.log("accounts: password reset asked for an unknown email from %s" % ip)
            return
        code, _ = self._issue(row[0], "reset", email, RESET_LIFE)
        self.mailer.send(email, "Your SvR2011 Online password reset code",
                         "Hi %s,\n\nYour code to set a new password for SvR2011 Online:\n\n    %s\n\n"
                         "Type it in the launcher's Online tab (Forgot password). It works once, for 30 minutes.\n\n"
                         "If you didn't ask for this, ignore this mail: your password stays as it is.\n" % (row[1], code))
        self.log("accounts: password reset code mailed for %s (asked from %s)" % (row[1], ip))

    def reset(self, email, code, password, ip):
        email = (email or "").strip()
        if self.limits.hit(("reset-ip", ip), 20, 1800) or self.limits.hit(("reset", email.lower()), 10, 1800):
            return "Too many tries - wait half an hour."
        if len(password) < MIN_PASSWORD:
            return "Passwords need at least %d characters." % MIN_PASSWORD
        with self.lock:
            acc = self.db.execute("SELECT id, name FROM accounts WHERE email = ? COLLATE NOCASE", (email,)).fetchone()
            row = acc and self.db.execute(
                "SELECT account FROM mail_codes WHERE hash = ? AND kind = 'reset' AND expires >= ?",
                (code_hash("reset" + str(acc[0]), code), now())).fetchone()
            if not row:
                return "That code isn't right (or it's old: ask for a new one)."
            self.db.execute("DELETE FROM mail_codes WHERE account = ? AND kind = 'reset'", (acc[0],))
            # (the mail reached its owner: the email counts as confirmed)
            self.db.execute("UPDATE accounts SET email_ok = 1 WHERE id = ?", (acc[0],))
            self.db.commit()
        self.set_password(acc[0], password)
        self.log("accounts: %s set a new password with a mailed code (from %s)" % (acc[1], ip))
        return ""

    def set_password(self, account_id, password, keep_token=None):
        """A new password; every session of the account ends (but keep_token's: the one changing it)."""
        salt = secrets.token_hex(16)
        with self.lock:
            self.db.execute("UPDATE accounts SET salt = ?, pwhash = ? WHERE id = ?",
                            (salt, self.accounts.hash_password(password, salt), account_id))
            keep = hashlib.sha256(keep_token.encode()).hexdigest() if keep_token else ""
            self.db.execute("DELETE FROM sessions WHERE account = ? AND token != ?", (account_id, keep))
            self.db.commit()
        self.accounts.cache.clear()
