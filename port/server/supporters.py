"""Supporters: the people who support the project, for the launcher's SUPPORTERS tab (docs/SUPPORTERS_SETUP.md).

The port is free; supporting is optional and unlocks nothing. This only lists names the supporters chose to show:

- Ko-fi: its webhook (POST /api/supporters/kofi) - a form with one field, `data`, a JSON object. Its
  verification_token is checked against the secret file (--kofi-token-file). Kept per supporter: the name
  (from_name), the type (Donation / Subscription / Shop Order), when, and whether the supporter made it public
  (is_public). Never the email, the amount or the message. Each message_id is taken once (Ko-fi retries).
  A supporter is listed only once one of their payments was public.
- Patreon: every 6 hours, API v2 /campaigns/{id}/members (full_name, patron_status) with the creator's access
  token (--patreon-token-file). Active patrons only ("active_patron"); names only - no amounts, no tiers.
  No token file: Patreon is off, quietly.
- By hand (the dashboard): PayPal donors (or others) who ask to be listed.

The dashboard's Supporters tab hides a name (a hidden one is never listed again, whatever comes in later) and adds
or removes the hand-added ones. GET /api/supporters (public) lists [{name, platform, since}], newest first, at most
LIST_MAX, cached LIST_CACHE seconds: no amounts, no emails, nothing hidden, nothing private.

Tokens are never logged, stored in the database or sent anywhere but where they belong.
"""
import hmac
import json
import os
import re
import threading
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request

LIST_MAX = 500
LIST_CACHE = 60
NAME_MAX = 60
PLATFORMS = ("kofi", "patreon", "paypal", "other")
HAND_PLATFORMS = ("paypal", "other")
KOFI_TYPES = ("Donation", "Subscription", "Shop Order", "Commission")
PATREON_API = "https://www.patreon.com/api/oauth2/v2"
PATREON_EVERY = 6 * 3600
EVENTS_KEEP = 90 * 86400  # (Ko-fi message ids kept this long for the duplicate check)

_SPACES = re.compile(r"\s+")


def init(store):
    with store.lock:
        store.db.executescript("""
            CREATE TABLE IF NOT EXISTS supporters (
                id INTEGER PRIMARY KEY, platform TEXT NOT NULL, ext_id TEXT NOT NULL, name TEXT NOT NULL,
                kind TEXT DEFAULT '', since REAL NOT NULL, last REAL NOT NULL, public INTEGER DEFAULT 0,
                active INTEGER DEFAULT 1, hidden INTEGER DEFAULT 0, by_hand INTEGER DEFAULT 0,
                UNIQUE (platform, ext_id));
            CREATE TABLE IF NOT EXISTS supporter_events (message_id TEXT PRIMARY KEY, at REAL NOT NULL);
        """)
        store.db.commit()


def clean_name(name):
    """A name as listed: printable, single spaces, NAME_MAX characters at most ('' = nothing to list)."""
    if not isinstance(name, str):
        return ""
    s = "".join(c for c in unicodedata.normalize("NFC", name) if unicodedata.category(c)[0] != "C")
    return _SPACES.sub(" ", s).strip()[:NAME_MAX].strip()


def _key(name):
    return clean_name(name).casefold()


def read_secret(path):
    """The secret file's first line, or '' (no file = that source is off). A list: the first file that has one
    (server.py: the dashboard's copy in <data>/secrets, then the command line's file)."""
    if isinstance(path, (list, tuple)):
        for p in path:
            got = read_secret(p)
            if got:
                return got
        return ""
    if not path:
        return ""
    try:
        with open(path, encoding="utf-8") as f:
            return f.readline().strip()
    except OSError:
        return ""


class Supporters:
    """The list, the Ko-fi webhook's checks and the Patreon poll. log: one line per event (never a token);
    flagged(name): True for a name the name filter catches (it comes in hidden)."""

    def __init__(self, store, log, kofi_token_file=None, patreon_token_file=None, patreon_api=PATREON_API,
                 patreon_campaign="", flagged=None):
        self.store = store
        self.log = log
        self.kofi_token_file = kofi_token_file
        self.patreon_token_file = patreon_token_file
        self.patreon_api = (patreon_api or PATREON_API).rstrip("/")
        self.patreon_campaign = str(patreon_campaign or "")
        self.flagged = flagged or (lambda name: False)
        self._cache = (0.0, None)
        self._cache_lock = threading.Lock()
        self._kofi_off_said = False
        self.patreon_state = {"last": 0.0, "ok": None, "result": "not polled yet"}
        init(store)

    # -- the public list ----------------------------------------------------------------------------------------

    def changed(self):
        with self._cache_lock:
            self._cache = (0.0, None)

    def public_list(self):
        """[{name, platform, since}] newest first: public, active, not hidden. Cached LIST_CACHE s."""
        with self._cache_lock:
            at, body = self._cache
            if body is not None and time.time() - at < LIST_CACHE:
                return body
        with self.store.lock:
            rows = self.store.db.execute(
                "SELECT name, platform, since FROM supporters WHERE public = 1 AND active = 1 AND hidden = 0 "
                "ORDER BY since DESC, id DESC LIMIT ?", (LIST_MAX,)).fetchall()
        body = [{"name": n, "platform": p, "since": time.strftime("%Y-%m-%d", time.gmtime(s))} for n, p, s in rows]
        with self._cache_lock:
            self._cache = (time.time(), body)
        return body

    # -- Ko-fi --------------------------------------------------------------------------------------------------

    def kofi_on(self):
        return bool(read_secret(self.kofi_token_file))

    def kofi(self, form_data):
        """One webhook call: form_data is the form's `data` field (a JSON string). Returns (http status, answer)."""
        token = read_secret(self.kofi_token_file)
        if not token:
            if not self._kofi_off_said:
                self._kofi_off_said = True
                self.log("supporters: a Ko-fi call came in, but Ko-fi is off (no token file)")
            return 503, {"ok": False, "error": "Ko-fi is off on this server."}
        try:
            d = json.loads(form_data or "")
        except ValueError:
            return 400, {"ok": False, "error": "No data."}
        if not isinstance(d, dict):
            return 400, {"ok": False, "error": "No data."}
        if not hmac.compare_digest(str(d.get("verification_token", "")).encode(), token.encode()):
            self.log("supporters: a Ko-fi call refused - wrong verification token")
            return 403, {"ok": False, "error": "Wrong token."}
        mid = str(d.get("message_id") or "").strip()[:100]
        if not mid:
            return 400, {"ok": False, "error": "No message_id."}
        name = clean_name(d.get("from_name"))
        kind = str(d.get("type") or "")[:20]
        public = d.get("is_public") is True or str(d.get("is_public")).lower() == "true"
        when = time.time()
        with self.store.lock:
            db = self.store.db
            if db.execute("SELECT 1 FROM supporter_events WHERE message_id = ?", (mid,)).fetchone():
                return 200, {"ok": True, "duplicate": True}
            db.execute("INSERT INTO supporter_events (message_id, at) VALUES (?, ?)", (mid, when))
            new = False
            if name:
                key = _key(name)
                row = db.execute("SELECT id, public FROM supporters WHERE platform = 'kofi' AND ext_id = ?",
                                 (key,)).fetchone()
                if row:
                    db.execute("UPDATE supporters SET name = CASE WHEN ? THEN ? ELSE name END, kind = ?, last = ?, "
                               "public = MAX(public, ?), active = 1 WHERE id = ?",
                               (int(public), name, kind, when, int(public), row[0]))
                else:
                    new = True
                    db.execute("INSERT INTO supporters (platform, ext_id, name, kind, since, last, public, active, "
                               "hidden) VALUES ('kofi', ?, ?, ?, ?, ?, ?, 1, ?)",
                               (key, name, kind, when, when, int(public), int(bool(public and self.flagged(name)))))
            db.execute("DELETE FROM supporter_events WHERE at < ?", (when - EVENTS_KEEP,))
            db.commit()
        self.changed()
        self.log("supporters: Ko-fi %s%s (%s)" % (
            kind or "payment", " from a new supporter" if new else "",
            "public" if public else "private - not listed" if name else "no name"))
        return 200, {"ok": True}

    # -- Patreon ------------------------------------------------------------------------------------------------

    def patreon_on(self):
        return bool(read_secret(self.patreon_token_file))

    def _get(self, url, token):
        req = urllib.request.Request(url, headers={"Authorization": "Bearer " + token,
                                                   "User-Agent": "svr2011-server supporters"})
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read(8 << 20).decode("utf-8"))

    def patreon_poll(self):
        """The active patrons now (blocking; run in a thread). Returns a line for the log / dashboard."""
        token = read_secret(self.patreon_token_file)
        if not token:
            self.patreon_state = {"last": time.time(), "ok": None, "result": "off (no token file)"}
            return self.patreon_state["result"]
        try:
            campaign = self.patreon_campaign
            if not campaign:
                d = self._get(self.patreon_api + "/campaigns", token)
                camps = d.get("data") or []
                if not camps:
                    raise ValueError("the token's account has no campaign")
                campaign = str(camps[0].get("id"))
            q = urllib.parse.urlencode({"fields[member]": "full_name,patron_status", "page[count]": "500"})
            url = "%s/campaigns/%s/members?%s" % (self.patreon_api, urllib.parse.quote(campaign), q)
            active, pages = {}, 0
            while url and pages < 100:
                d = self._get(url, token)
                pages += 1
                for m in d.get("data") or []:
                    a = m.get("attributes") or {}
                    if a.get("patron_status") == "active_patron" and m.get("id"):
                        name = clean_name(a.get("full_name"))
                        if name:
                            active[str(m["id"])] = name
                url = ((d.get("links") or {}).get("next")) or None
                if url and not url.startswith(self.patreon_api):   # (only ever the API's own next page)
                    url = None
        except urllib.error.HTTPError as e:
            self.patreon_state = {"last": time.time(), "ok": False, "result": "Patreon said HTTP %d" % e.code}
            self.log("supporters: Patreon poll failed - HTTP %d" % e.code)
            return self.patreon_state["result"]
        except (OSError, ValueError, AttributeError) as e:
            self.patreon_state = {"last": time.time(), "ok": False, "result": "failed: %s" % type(e).__name__}
            self.log("supporters: Patreon poll failed - %s" % type(e).__name__)
            return self.patreon_state["result"]
        now = time.time()
        new = 0
        with self.store.lock:
            db = self.store.db
            have = {r[0]: r[1] for r in db.execute("SELECT ext_id, id FROM supporters WHERE platform = 'patreon'")}
            for mid, name in active.items():
                if mid in have:
                    db.execute("UPDATE supporters SET name = ?, last = ?, active = 1, public = 1 WHERE id = ?",
                               (name, now, have[mid]))
                else:
                    new += 1
                    db.execute("INSERT INTO supporters (platform, ext_id, name, kind, since, last, public, active, "
                               "hidden) VALUES ('patreon', ?, ?, 'Member', ?, ?, 1, 1, ?)",
                               (mid, name, now, now, int(bool(self.flagged(name)))))
            gone = [i for m, i in have.items() if m not in active]
            for i in gone:
                db.execute("UPDATE supporters SET active = 0 WHERE id = ?", (i,))
            db.commit()
        self.changed()
        line = "%d active patron(s), %d new, %d no longer active" % (len(active), new, len(gone))
        self.patreon_state = {"last": now, "ok": True, "result": line}
        self.log("supporters: Patreon - " + line)
        return line

    # -- the dashboard ------------------------------------------------------------------------------------------

    def admin_rows(self):
        with self.store.lock:
            rows = self.store.db.execute(
                "SELECT id, name, platform, kind, since, last, public, active, hidden, by_hand FROM supporters "
                "ORDER BY since DESC, id DESC").fetchall()
        keys = ("id", "name", "platform", "kind", "since", "last", "public", "active", "hidden", "by_hand")
        return [dict(zip(keys, r)) for r in rows]

    def set_hidden(self, sid, hidden):
        with self.store.lock:
            n = self.store.db.execute("UPDATE supporters SET hidden = ? WHERE id = ?", (int(bool(hidden)), sid)).rowcount
            self.store.db.commit()
        self.changed()
        return n > 0

    def add_by_hand(self, name, platform):
        """(row id, '') or (None, why)."""
        name = clean_name(name)
        if not name:
            return None, "Write a name."
        if platform not in HAND_PLATFORMS:
            return None, "The platform is PayPal or other."
        now = time.time()
        with self.store.lock:
            db = self.store.db
            if db.execute("SELECT 1 FROM supporters WHERE by_hand = 1 AND platform = ? AND ext_id = ?",
                          (platform, "hand:" + _key(name))).fetchone():
                return None, "That name is on the list already."
            cur = db.execute("INSERT INTO supporters (platform, ext_id, name, kind, since, last, public, active, "
                             "hidden, by_hand) VALUES (?, ?, ?, 'By hand', ?, ?, 1, 1, 0, 1)",
                             (platform, "hand:" + _key(name), name, now, now))
            db.commit()
        self.changed()
        return cur.lastrowid, ""

    def remove_by_hand(self, sid):
        with self.store.lock:
            n = self.store.db.execute("DELETE FROM supporters WHERE id = ? AND by_hand = 1", (sid,)).rowcount
            self.store.db.commit()
        self.changed()
        return n > 0
