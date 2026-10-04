#!/usr/bin/env python3
"""WWE SmackDown vs. Raw 2011 (PC port) - the Community Creations server.

One aiohttp service with two halves:

  <base>/...        what the game and the launchers use (the port relays the
                    game's GameSpy traffic here over HTTP(S), see
                    port/src/online.cpp); behind Project Index this is its
                    public mount, e.g. https://sho-ti.me/svr
      api/register, api/login, api/logout     accounts (name + password)
      api/session                             the game's login (profile, ticket)
      api/logo/<hash>, api/logos/wanted       the port's extra Superstar logos
      api/status                              is the server up
      game/<GameSpy path>                     the game's own requests (gamespy.py)
      api/relay/<id>                          the online match relay (relay.py)
  /                 the admin dashboard: connections, bandwidth, storage,
                    players, uploads and accounts (Project Index admins, or
                    anyone on this PC when run on its own)

Everything a player does is tied to their account (a bearer token from
api/login): the game's own identities - its XUID, login ticket - are not
trusted.

    python server.py [--host 127.0.0.1] [--port 8101] [--data ./data] [--base /svr]

Needs Python 3.9+ and aiohttp.
"""

import argparse
import asyncio
import base64
import collections
import hashlib
import hmac
import json
import os
import re
import secrets
import sqlite3
import threading
import time
import traceback
from pathlib import Path

os.environ.setdefault("AIOHTTP_NOSENDFILE", "1")

from aiohttp import web

import gamespy as gs
import leaderboards
import media
import relay
import thumbs

HERE = Path(__file__).resolve().parent
LOOPBACK = frozenset({"127.0.0.1", "::1", "::ffff:127.0.0.1"})
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 _.\-]{2,14}$")  # 3-15, what the game can show
XUID_BASE = 0x0009000100000000  # + account id: the player's XUID on every device
PBKDF2_ROUNDS = 240000
MAX_FRIENDS = 200  # (a player's list)
INVITE_LIFE = 600  # seconds an invite to a match stays


def now():
    return time.time()


# -- Accounts ------------------------------------------------------------------

class Accounts:
    """Players' accounts and their sessions (bearer tokens), in the store's
    database. Passwords are kept as PBKDF2 hashes; tokens as SHA-256."""

    def __init__(self, store):
        self.store = store
        self.db = store.db
        self.lock = store.lock
        with self.lock:
            self.db.executescript("""
                CREATE TABLE IF NOT EXISTS accounts (
                    id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE COLLATE NOCASE,
                    salt TEXT, pwhash TEXT, profileid INTEGER, created REAL, last_seen REAL,
                    banned INTEGER DEFAULT 0, ip TEXT);
                CREATE TABLE IF NOT EXISTS sessions (
                    token TEXT PRIMARY KEY, account INTEGER, created REAL, last_used REAL, ip TEXT);
                CREATE INDEX IF NOT EXISTS sessions_account ON sessions (account);
                CREATE TABLE IF NOT EXISTS friends (
                    account INTEGER, friend INTEGER, created REAL, PRIMARY KEY (account, friend));
                CREATE INDEX IF NOT EXISTS friends_friend ON friends (friend);
            """)
            self.db.commit()
        self.cache = {}  # token hash -> (account row, cached at)

    @staticmethod
    def hash_password(password, salt):
        return hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), bytes.fromhex(salt), PBKDF2_ROUNDS).hex()

    @staticmethod
    def xuid(account):
        return XUID_BASE + account["id"]

    def _row(self, row):
        if not row:
            return None
        keys = ("id", "name", "profileid", "created", "last_seen", "banned", "ip")
        return dict(zip(keys, row))

    def get(self, account_id):
        with self.lock:
            return self._row(self.db.execute(
                "SELECT id, name, profileid, created, last_seen, banned, ip FROM accounts WHERE id = ?",
                (account_id,)).fetchone())

    def by_profile(self, profileid):
        with self.lock:
            return self._row(self.db.execute(
                "SELECT id, name, profileid, created, last_seen, banned, ip FROM accounts WHERE profileid = ?",
                (profileid,)).fetchone())

    def register(self, name, password, ip):
        name = name.strip()
        if not NAME_RE.match(name):
            return None, "Names are 3-15 letters, digits, spaces, dots, dashes or underscores."
        if len(password) < 6:
            return None, "Passwords need at least 6 characters."
        salt = secrets.token_hex(16)
        pwhash = self.hash_password(password, salt)
        with self.lock:
            if self.db.execute("SELECT 1 FROM accounts WHERE name = ?", (name,)).fetchone():
                return None, "That name is taken."
            cur = self.db.execute(
                "INSERT INTO accounts (name, salt, pwhash, created, last_seen, ip) VALUES (?, ?, ?, ?, ?, ?)",
                (name, salt, pwhash, now(), now(), ip))
            account_id = cur.lastrowid
            profileid = self.store.profile("account:%d" % account_id, name)
            self.db.execute("UPDATE accounts SET profileid = ? WHERE id = ?", (profileid, account_id))
            self.db.commit()
        gs.log("accounts: %s registered (account %d) from %s" % (name, account_id, ip))
        return self.get(account_id), None

    def check(self, name, password):
        with self.lock:
            row = self.db.execute("SELECT id, salt, pwhash, banned FROM accounts WHERE name = ?",
                                  (name.strip(),)).fetchone()
        if not row or not hmac.compare_digest(self.hash_password(password, row[1]), row[2]):
            return None, "Wrong name or password."
        if row[3]:
            return None, "This account is banned."
        return self.get(row[0]), None

    def new_token(self, account, ip):
        token = secrets.token_urlsafe(32)
        with self.lock:
            self.db.execute("INSERT INTO sessions VALUES (?, ?, ?, ?, ?)",
                            (hashlib.sha256(token.encode()).hexdigest(), account["id"], now(), now(), ip))
            self.db.commit()
        return token

    def from_token(self, token):
        """The account behind a bearer token (not banned), or None."""
        if not token:
            return None
        key = hashlib.sha256(token.encode()).hexdigest()
        hit = self.cache.get(key)
        if hit and now() - hit[1] < 60:
            return hit[0]
        with self.lock:
            row = self.db.execute("SELECT account FROM sessions WHERE token = ?", (key,)).fetchone()
            if not row:
                return None
            account = self.get(row[0])
            if not account or account["banned"]:
                return None
            self.db.execute("UPDATE sessions SET last_used = ? WHERE token = ?", (now(), key))
            self.db.execute("UPDATE accounts SET last_seen = ? WHERE id = ?", (now(), account["id"]))
            self.db.commit()
        self.cache[key] = (account, now())
        return account

    def logout(self, token):
        with self.lock:
            self.db.execute("DELETE FROM sessions WHERE token = ?", (hashlib.sha256(token.encode()).hexdigest(),))
            self.db.commit()
        self.cache.clear()

    def set_banned(self, account_id, banned):
        with self.lock:
            self.db.execute("UPDATE accounts SET banned = ? WHERE id = ?", (1 if banned else 0, account_id))
            if banned:
                self.db.execute("DELETE FROM sessions WHERE account = ?", (account_id,))
            self.db.commit()
        self.cache.clear()

    def reset_password(self, account_id):
        password = secrets.token_urlsafe(9)
        salt = secrets.token_hex(16)
        with self.lock:
            self.db.execute("UPDATE accounts SET salt = ?, pwhash = ? WHERE id = ?",
                            (salt, self.hash_password(password, salt), account_id))
            self.db.execute("DELETE FROM sessions WHERE account = ?", (account_id,))
            self.db.commit()
        self.cache.clear()
        return password

    def delete(self, account_id):
        account = self.get(account_id)
        if not account:
            return 0
        removed = 0
        for table in list(self.store.records):
            for rec in [r for r in self.store.all(table) if r["ownerid"] == account["profileid"]]:
                self.store.delete(table, rec["recordid"])
                removed += 1
        with self.lock:
            self.db.execute("DELETE FROM sessions WHERE account = ?", (account_id,))
            self.db.execute("DELETE FROM friends WHERE account = ? OR friend = ?", (account_id, account_id))
            self.db.execute("DELETE FROM accounts WHERE id = ?", (account_id,))
            self.db.commit()
        self.cache.clear()
        return removed

    # friends: each player's own list (adding someone needs no answer from them;
    # they see who added them, to add them back)

    def by_name(self, name):
        with self.lock:
            return self._row(self.db.execute(
                "SELECT id, name, profileid, created, last_seen, banned, ip FROM accounts WHERE name = ?",
                (name,)).fetchone())

    def friends(self, account_id):
        """(the accounts this one added, the accounts that added this one)"""
        cols = "a.id, a.name, a.profileid, a.created, a.last_seen, a.banned, a.ip"
        with self.lock:
            mine = self.db.execute("SELECT %s FROM friends f JOIN accounts a ON a.id = f.friend "
                                   "WHERE f.account = ? ORDER BY a.name COLLATE NOCASE" % cols, (account_id,)).fetchall()
            theirs = self.db.execute("SELECT %s FROM friends f JOIN accounts a ON a.id = f.account "
                                     "WHERE f.friend = ? ORDER BY a.name COLLATE NOCASE" % cols,
                                     (account_id,)).fetchall()
        return [self._row(r) for r in mine], [self._row(r) for r in theirs]

    def add_friend(self, account_id, friend_id):
        with self.lock:
            if self.db.execute("SELECT COUNT(*) FROM friends WHERE account = ?", (account_id,)).fetchone()[0] >= MAX_FRIENDS:
                return False
            self.db.execute("INSERT OR IGNORE INTO friends VALUES (?, ?, ?)", (account_id, friend_id, now()))
            self.db.commit()
        return True

    def remove_friend(self, account_id, friend_id):
        with self.lock:
            self.db.execute("DELETE FROM friends WHERE account = ? AND friend = ?", (account_id, friend_id))
            self.db.commit()

    def all(self):
        with self.lock:
            rows = self.db.execute("SELECT id, name, profileid, created, last_seen, banned, ip FROM accounts "
                                   "ORDER BY id").fetchall()
        return [self._row(r) for r in rows]


# -- Statistics ----------------------------------------------------------------

class Stats:
    """Per-minute counters of the game's traffic, kept in the database (the
    dashboard's history), plus the live gauges."""

    RELAY_COLUMNS = ("games", "sessions", "relayed")
    COLUMNS = ("requests", "bytes_in", "bytes_out", "peak", "players", "uploads", "downloads") + RELAY_COLUMNS

    def __init__(self, store):
        self.db = store.db
        self.lock = store.lock
        with self.lock:
            self.db.executescript("""
                CREATE TABLE IF NOT EXISTS stats (
                    t INTEGER PRIMARY KEY, requests INTEGER, bytes_in INTEGER, bytes_out INTEGER,
                    peak INTEGER, players INTEGER, uploads INTEGER, downloads INTEGER,
                    logins INTEGER, registrations INTEGER, refused INTEGER);
                CREATE TABLE IF NOT EXISTS player_days (day TEXT, account INTEGER, PRIMARY KEY (day, account));
            """)
            # (online play through the relay, added later: games open at once, games
            # that came on, packets passed on)
            have = {r[1] for r in self.db.execute("PRAGMA table_info(stats)")}
            for column in self.RELAY_COLUMNS:
                if column not in have:
                    self.db.execute("ALTER TABLE stats ADD COLUMN %s INTEGER DEFAULT 0" % column)
            self.db.commit()
        self.active = 0
        self.peak = 0
        self.max_peak = 0
        self.minute = self._zero()
        self.recent = {}      # account id -> last request time
        self.total_in = 0
        self.total_out = 0
        self.started = now()
        self.games = 0        # relay games open now

    def relay_games(self, n, came_on=False):
        """The relay's games open now (and one more came on)."""
        self.games = n
        self.minute["games"] = max(self.minute["games"], n)
        if came_on:
            self.minute["sessions"] += 1

    @staticmethod
    def _zero():
        return collections.Counter()

    def begin(self):
        self.active += 1
        self.peak = max(self.peak, self.active)
        self.max_peak = max(self.max_peak, self.active)

    def end(self, bytes_in, bytes_out):
        self.active -= 1
        self.minute["requests"] += 1
        self.minute["bytes_in"] += bytes_in
        self.minute["bytes_out"] += bytes_out
        self.total_in += bytes_in
        self.total_out += bytes_out

    def count(self, what, n=1):
        self.minute[what] += n

    def seen(self, account_id):
        first = account_id not in self.recent
        self.recent[account_id] = now()
        if first:
            with self.lock:
                self.db.execute("INSERT OR IGNORE INTO player_days VALUES (?, ?)",
                                (time.strftime("%Y-%m-%d"), account_id))
                self.db.commit()

    def players_online(self, window=300):
        cutoff = now() - window
        for k in [k for k, t in self.recent.items() if t < cutoff]:
            del self.recent[k]
        return len(self.recent)

    def flush(self):
        """The minute just ended, into the history."""
        m, self.minute = self.minute, self._zero()
        t = int(now() // 60 * 60) - 60
        row = (t, m["requests"], m["bytes_in"], m["bytes_out"], self.peak, self.players_online(),
               m["uploads"], m["downloads"], m["logins"], m["registrations"], m["refused"],
               max(m["games"], self.games), m["sessions"], m["relayed"])
        self.peak = self.active
        with self.lock:
            self.db.execute("INSERT OR REPLACE INTO stats (t, requests, bytes_in, bytes_out, peak, players, uploads, "
                            "downloads, logins, registrations, refused, games, sessions, relayed) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", row)
            self.db.commit()

    def history(self, span, buckets):
        """[(t, requests, bytes_in, bytes_out, peak, players, uploads, downloads, games, sessions,
        relayed)] over the last `span` seconds, in `buckets` equal steps (sums; peak, players and
        games: max)."""
        start = int(now() - span)
        step = max(60, span // buckets)
        with self.lock:
            rows = self.db.execute(
                "SELECT (t - ?) / ? AS b, SUM(requests), SUM(bytes_in), SUM(bytes_out), MAX(peak), "
                "MAX(players), SUM(uploads), SUM(downloads), MAX(games), SUM(sessions), SUM(relayed) "
                "FROM stats WHERE t >= ? GROUP BY b ORDER BY b",
                (start, step, start)).fetchall()
        got = {r[0]: r[1:] for r in rows}
        n = len(self.COLUMNS)
        out = []
        for b in range(buckets):
            r = got.get(b)
            out.append([start + b * step] + ([0] * n if r is None else [v or 0 for v in r]))
        return step, out

    def totals(self, since=0):
        """Sums over the history since `since` (all of it by default)."""
        with self.lock:
            r = self.db.execute("SELECT SUM(uploads), SUM(downloads), SUM(sessions), SUM(relayed), "
                                "MAX(games), MAX(players) FROM stats WHERE t >= ?", (since,)).fetchone()
        return {k: v or 0 for k, v in zip(("uploads", "downloads", "sessions", "relayed", "max_games",
                                             "max_players"), r)}

    def unique_players(self, days):
        with self.lock:
            return self.db.execute("SELECT COUNT(DISTINCT account) FROM player_days WHERE day >= ?",
                                   (time.strftime("%Y-%m-%d", time.localtime(now() - days * 86400)),)
                                   ).fetchone()[0]


# -- The service ---------------------------------------------------------------

class Service:
    def __init__(self, args):
        self.base = "/" + args.base.strip("/") if args.base.strip("/") else ""
        self.store = gs.Store(args.data, max_file=args.max_file_mb << 20, quota=args.quota_mb << 20)
        self.sake = gs.Sake(self.store)
        self.accounts = Accounts(self.store)
        self.stats = Stats(self.store)
        self.slots = asyncio.Semaphore(args.max_connections)
        self.relay = relay.Relay(self, gs.log)
        self.leaderboards = leaderboards.Leaderboards(self.store)
        self.invites = collections.defaultdict(dict)  # account id -> {inviter id: invite} (in memory)
        self.max_connections = args.max_connections
        self.attempts = collections.defaultdict(collections.deque)  # ip -> login/register times
        self.data_dir = Path(args.data)
        self.dashboard_html = (HERE / "dashboard.html").read_text(encoding="utf-8")
        self.max_media = {"music": args.max_music_mb << 20, "movie": args.max_movie_mb << 20}
        self.media_quota = args.media_quota_mb << 20

    # who is asking

    @staticmethod
    def client_ip(request):
        peer = request.remote or ""
        if peer in LOOPBACK and request.headers.get("X-Forwarded-For"):
            return request.headers["X-Forwarded-For"].split(",")[0].strip()
        return peer

    def account(self, request):
        auth = request.headers.get("Authorization", "")
        if not auth.startswith("Bearer "):
            return None
        account = self.accounts.from_token(auth[7:].strip())
        if account:
            self.stats.seen(account["id"])
        return account

    def is_admin(self, request):
        """The dashboard: Project Index admins (through the portal), or anyone on
        this PC talking to the service directly."""
        if request.headers.get("X-Portal-Public"):
            return False
        if request.remote not in LOOPBACK:
            return False
        if request.headers.get("X-Forwarded-For"):  # through the portal
            return request.headers.get("X-Portal-Role") == "admin"
        return True

    def throttled(self, ip, limit=10, window=600):
        q = self.attempts[ip]
        while q and q[0] < now() - window:
            q.popleft()
        if len(q) >= limit:
            return True
        q.append(now())
        return False

    # metering and the connection limit (the game's half only)

    @web.middleware
    async def meter(self, request, handler):
        if not request.path.startswith(self.base + "/") or request.path.startswith(self.base + "/api/relay/"):
            return await handler(request)  # (the relay's streams stay open all session: not requests' slots)
        try:
            await asyncio.wait_for(self.slots.acquire(), 10)
        except asyncio.TimeoutError:
            self.stats.count("refused")
            return web.Response(status=503, text="busy")
        self.stats.begin()
        bytes_in = request.content_length or 0
        bytes_out = 0
        try:
            response = await handler(request)
            if isinstance(response, web.Response) and response.body is not None:
                try:
                    bytes_out = len(response.body)
                except TypeError:
                    bytes_out = response.content_length or 0
            return response
        finally:
            self.stats.end(bytes_in, bytes_out)
            self.slots.release()

    # accounts

    async def api_register(self, request):
        return await self._sign_in(request, register=True)

    async def api_login(self, request):
        return await self._sign_in(request, register=False)

    async def _sign_in(self, request, register):
        ip = self.client_ip(request)
        if self.throttled(ip):
            return web.json_response({"ok": False, "error": "Too many tries - wait a few minutes."}, status=429)
        try:
            body = await request.json()
            name, password = str(body.get("name", "")), str(body.get("password", ""))
        except (ValueError, AttributeError):
            return web.json_response({"ok": False, "error": "Bad request."}, status=400)
        loop = asyncio.get_running_loop()
        if register:
            account, error = await loop.run_in_executor(None, self.accounts.register, name, password, ip)
        else:
            account, error = await loop.run_in_executor(None, self.accounts.check, name, password)
        if not account:
            return web.json_response({"ok": False, "error": error}, status=403)
        token = self.accounts.new_token(account, ip)
        self.stats.count("registrations" if register else "logins")
        gs.log("accounts: %s %s from %s" % (account["name"], "registered" if register else "signed in", ip))
        return web.json_response({"ok": True, "token": token, "name": account["name"],
                                  "xuid": "%016X" % Accounts.xuid(account)})

    async def api_logout(self, request):
        auth = request.headers.get("Authorization", "")
        if auth.startswith("Bearer "):
            self.accounts.logout(auth[7:].strip())
        return web.json_response({"ok": True})

    async def api_session(self, request):
        """The game's login: its profile, a login ticket, its name and XUID."""
        account = self.account(request)
        if not account:
            return web.json_response({"ok": False, "error": "Not signed in."}, status=401)
        ticket = self.store.new_ticket(account["profileid"])
        self.stats.count("logins")
        return web.json_response({"ok": True, "name": account["name"], "profileid": account["profileid"],
                                  "ticket": ticket, "xuid": "%016X" % Accounts.xuid(account)})

    # friends and who's online (the launchers' Friends list)

    def presence(self, account_id):
        """playing: in an online match or its lobby (its game has the relay open);
        online: in the game's ONLINE menus (game requests in the last 5 minutes);
        else offline. (The launchers' own requests don't count.)"""
        if any(g.account["id"] == account_id for g in self.relay.games.values()):
            return "playing"
        if now() - self.stats.recent.get(account_id, 0) < 300:
            return "online"
        return "offline"

    def friends_json(self, account):
        mine, theirs = self.accounts.friends(account["id"])
        waiting = self.invites.get(account["id"], {})
        for k in [k for k, v in waiting.items() if now() - v["t"] > INVITE_LIFE]:
            del waiting[k]
        added = {a["id"] for a in mine}
        friends = [{"name": a["name"], "status": self.presence(a["id"]), "last_seen": a["last_seen"],
                    "mutual": any(t["id"] == a["id"] for t in theirs)} for a in mine if not a["banned"]]
        friends.sort(key=lambda f: ("playing", "online", "offline").index(f["status"]))  # (online first)
        return {
            "ok": True,
            "friends": friends,
            "added_you": [a["name"] for a in theirs if a["id"] not in added and not a["banned"]],
            "invites": [{"from": v["from"], "xuid": v["xuid"], "session": v["session"], "kind": v["kind"],
                         "age": int(now() - v["t"])} for v in sorted(waiting.values(), key=lambda v: -v["t"])],
        }

    def token_account(self, request):
        """The signed-in account, without counting it as playing (the launchers)."""
        auth = request.headers.get("Authorization", "")
        return self.accounts.from_token(auth[7:].strip()) if auth.startswith("Bearer ") else None

    async def api_friends(self, request):
        account = self.token_account(request)
        if not account:
            return web.json_response({"ok": False, "error": "Not signed in."}, status=401)
        if request.query.get("here"):  # (the game asking: it's running, signed in)
            self.stats.seen(account["id"])
        if request.method == "POST":
            try:
                body = await request.json()
            except ValueError:
                body = {}
            name = str(body.get("add") or body.get("remove") or "").strip()
            other = self.accounts.by_name(name) if name else None
            if not other or other["banned"]:
                return web.json_response({"ok": False, "error": "There's no player called %s." % name[:20]},
                                         status=404)
            if other["id"] == account["id"]:
                return web.json_response({"ok": False, "error": "That's you."}, status=400)
            if body.get("add"):
                if not self.accounts.add_friend(account["id"], other["id"]):
                    return web.json_response({"ok": False, "error": "Your list is full (%d)." % MAX_FRIENDS},
                                             status=400)
                gs.log("friends: %s added %s" % (account["name"], other["name"]))
            else:
                self.accounts.remove_friend(account["id"], other["id"])
        return web.json_response(self.friends_json(account))

    async def api_invite(self, request):
        """An invite to the match session the player is in, for a friend's game
        (it accepts through the game's own Xbox LIVE invite path): {"to": name,
        "session": base64 XSESSION_INFO, "kind": text}; {"decline": name} drops
        one received."""
        account = self.token_account(request)
        if not account:
            return web.json_response({"ok": False, "error": "Not signed in."}, status=401)
        try:
            body = await request.json()
        except ValueError:
            body = {}
        if body.get("decline"):
            other = self.accounts.by_name(str(body["decline"]))
            if other:
                self.invites.get(account["id"], {}).pop(other["id"], None)
            return web.json_response({"ok": True})
        other = self.accounts.by_name(str(body.get("to") or "").strip())
        session = str(body.get("session") or "")
        try:
            info = base64.b64decode(session, validate=True)
        except ValueError:
            info = b""
        if not other or other["banned"]:
            return web.json_response({"ok": False, "error": "There's no such player."}, status=404)
        if len(info) != 60:
            return web.json_response({"ok": False, "error": "Not a session."}, status=400)
        mine, _ = self.accounts.friends(account["id"])
        if other["id"] not in {a["id"] for a in mine}:
            return web.json_response({"ok": False, "error": "Only friends can be invited."}, status=403)
        if self.throttled("invite:%d" % account["id"], limit=30, window=60):
            return web.json_response({"ok": False, "error": "Too many invites - wait a little."}, status=429)
        self.invites[other["id"]][account["id"]] = {"from": account["name"], "xuid": "%016X" % Accounts.xuid(account),
                                                    "session": session,
                                                    "kind": str(body.get("kind") or "")[:40], "t": now()}
        gs.log("friends: %s invited %s" % (account["name"], other["name"]))
        return web.json_response({"ok": True})

    # the game's leaderboards (leaderboards.py; the port's leaderboards.cpp)

    def lb_row(self, r, names):
        xuid, account, rating, cols, rank = r
        return {"xuid": "%016X" % xuid, "rank": rank, "rating": rating, "name": names.get(account, ""),
                "columns": json.loads(cols)}

    def lb_names(self):
        return {a["id"]: a["name"] for a in self.accounts.all()}

    async def api_stats(self, request):
        account = self.account(request)
        if not account:
            return web.json_response({"ok": False, "error": "Not signed in."}, status=401)
        try:
            body = await request.json()
        except ValueError:
            return web.json_response({"ok": False, "error": "Not JSON."}, status=400)
        what = request.match_info["what"]
        lb = self.leaderboards
        loop = asyncio.get_running_loop()
        if what == "write":
            # (a game writes its own player's stats only)
            xuid = Accounts.xuid(account)
            if int(str(body.get("xuid") or "0"), 16) != xuid:
                return web.json_response({"ok": False, "error": "Only your own stats."}, status=403)
            done = await loop.run_in_executor(None, lb.write, xuid, account["id"], body.get("views") or [])
            gs.log("stats: %s wrote %d view(s)" % (account["name"], done))
            return web.json_response({"ok": True, "views": done})
        names = await loop.run_in_executor(None, self.lb_names)
        if what == "read":
            xuids = [int(str(x), 16) for x in (body.get("xuids") or [])][:100]
            out = []
            for vid in [int(v) for v in (body.get("views") or [])][:64]:
                rows = await loop.run_in_executor(None, lb.rows_for, vid, xuids)
                out.append({"view": vid, "total": await loop.run_in_executor(None, lb.total, vid),
                            "rows": [self.lb_row(r, names) for r in rows]})
            return web.json_response({"ok": True, "views": out})
        if what == "page":
            vid, count = int(body.get("view") or 0), max(1, min(100, int(body.get("count") or 10)))
            mode, pivot = body.get("mode"), body.get("pivot") or 0
            if mode == "xuid":
                rows = await loop.run_in_executor(None, lb.page_around, vid, int(str(pivot), 16), count)
            elif mode == "rating":
                rows = await loop.run_in_executor(None, lb.page_by_rating, vid, int(pivot), count)
            else:
                rows = await loop.run_in_executor(None, lb.page_by_rank, vid, int(pivot) or 1, count)
            return web.json_response({"ok": True, "total": await loop.run_in_executor(None, lb.total, vid),
                                      "rows": [self.lb_row(r, names) for r in rows]})
        return web.json_response({"ok": False}, status=404)

    async def api_status(self, request):
        return web.json_response({"ok": True, "server": "svr2011-community", "version": 1})

    # the port's logo store

    async def api_logo(self, request):
        if not self.account(request):
            return web.Response(status=401)
        hash16 = request.match_info["hash"].upper()
        if not re.fullmatch(r"[0-9A-F]{16}", hash16):
            return web.Response(status=400)
        loop = asyncio.get_running_loop()
        if request.method == "POST":
            data = await request.read()
            ok = await loop.run_in_executor(None, self.store.put_logo, hash16, data)
            return web.Response(text="ok" if ok else "", headers={"Svr2011-Result": "0" if ok else "6"})
        data = await loop.run_in_executor(None, self.store.get_logo, hash16)
        return web.Response(body=data or b"", content_type="application/octet-stream",
                            headers={"Svr2011-Result": "0" if data else "4"})

    # Created Superstars' entrance songs and movies (gamespy.Store.set_entrance)

    async def api_media_post(self, request):
        account = self.account(request)
        if not account:
            return web.Response(status=401)
        kind = request.query.get("kind", "")
        if kind not in self.max_media:
            return web.json_response({"ok": False, "error": "kind is music or movie"}, status=400)
        if (request.content_length or 0) > self.max_media[kind]:
            return web.json_response({"ok": False, "error": "too large"}, status=413)
        data = await request.read()
        if not data or len(data) > self.max_media[kind]:
            return web.json_response({"ok": False, "error": "too large"}, status=413)
        pid = account["profileid"]
        loop = asyncio.get_running_loop()
        sent = hashlib.sha256(data).hexdigest()
        if self.store.stored_media(sent):  # (sent before: as it was stored)
            return web.json_response({"ok": True, "sha": self.store.stored_media(sent)})
        # (any size: songs made MP3 at the game's rate, movies cut at 4 minutes - media.py)
        out, ext, what = await loop.run_in_executor(None, media.process, kind, data)
        if out is None:
            gs.log("media: %s from %s refused: %s" % (kind, account["name"], what))
            return web.json_response({"ok": False, "error": what}, status=415)
        if self.store.media_bytes(pid) + len(out) > self.media_quota:
            gs.log("media: %s from %s refused: over the quota" % (kind, account["name"]))
            return web.json_response({"ok": False, "error": "over the quota"}, status=507)
        sha = await loop.run_in_executor(None, self.store.put_media, pid, kind, out)
        if sha != sent:
            self.store.set_media_alias(sent, sha, ext)
        gs.log("media: %s %d bytes from %s -> %s, %d bytes: %s" % (kind, len(data), account["name"], sha[:12],
                                                                   len(out), what))
        return web.json_response({"ok": True, "sha": sha, "processed": what})

    async def api_media_get(self, request):
        if not self.account(request):
            return web.Response(status=401)
        sha = request.match_info["sha"].lower()
        if not re.fullmatch(r"[0-9a-f]{64}", sha):
            return web.Response(status=400)
        if request.method == "HEAD":
            return web.Response(status=200 if self.store.stored_media(sha) else 404)
        sha = self.store.stored_media(sha) or sha
        data = await asyncio.get_running_loop().run_in_executor(None, self.store.get_media, sha)
        if data is None:
            return web.Response(status=404)
        self.stats.count("downloads")
        return web.Response(body=data, content_type="application/octet-stream")

    async def api_entrance(self, request):
        account = self.account(request)
        if not account:
            return web.Response(status=401)
        try:
            fileid = int(request.match_info["fileid"])
        except ValueError:
            return web.Response(status=400)
        if request.method == "GET":
            info = self.store.get_entrance(fileid)
            return web.json_response(info) if info is not None else web.Response(status=404)
        # PUT: the Superstar's owner says which song and movie its entrance uses
        if self.store.file_owner(fileid) != account["profileid"]:
            return web.json_response({"ok": False, "error": "not your upload"}, status=403)
        try:
            body = await request.json()
        except ValueError:
            return web.json_response({"ok": False, "error": "not JSON"}, status=400)
        info = {}
        for kind, names in (("music", ("playlist", "file")), ("movie", ("name",))):
            m = body.get(kind)
            if not isinstance(m, dict):
                continue
            sent = str(m.get("sha", "")).lower()
            sha = self.store.stored_media(sent)
            if not sha:
                return web.json_response({"ok": False, "error": "%s not uploaded" % kind}, status=400)
            _, new_ext = self.store.media_alias(sent)
            entry = {"sha": sha}
            for n in names:
                v = str(m.get(n, ""))
                # (a file name on the downloader's PC: no folders, nothing odd)
                if not v or len(v) > 80 or re.search(r'[\\/:*?"<>|\x00-\x1f]', v) or v.strip(". ") != v:
                    return web.json_response({"ok": False, "error": "bad %s %s" % (kind, n)}, status=400)
                entry[n] = v
            for n in ("frames",):
                if isinstance(m.get(n), int):
                    entry[n] = m[n]
            if kind == "music" and new_ext and "file" in entry:  # (the song became an MP3)
                entry["file"] = os.path.splitext(entry["file"])[0] + new_ext
            info[kind] = entry
        self.store.set_entrance(fileid, account["profileid"], info)
        gs.log("entrance: file %d by %s: %s" % (fileid, account["name"], ", ".join(
            "%s %s" % (k, v.get("playlist") or v.get("name")) for k, v in info.items()) or "nothing"))
        return web.json_response({"ok": True})

    async def api_logos_wanted(self, request):
        if not self.account(request):
            return web.Response(status=401)
        return web.Response(text="\n".join(self.store.wanted_logos()))

    # the game

    async def game(self, request):
        account = self.account(request)
        if not account:
            return web.Response(status=401, text="sign in first")
        path = request.match_info["tail"].lower()
        body = await request.read()
        loop = asyncio.get_running_loop()
        try:
            if path.endswith("sakefileserver/upload.aspx"):
                return await loop.run_in_executor(None, self.upload, account, request.headers, body)
            if path.endswith("sakefileserver/download.aspx"):
                fileid = int(request.query.get("fileid", "0") or 0)
                data = await loop.run_in_executor(None, self.store.read_file, fileid)
                if data is None:
                    return web.Response(body=b"", content_type="application/octet-stream",
                                        headers={"Sake-File-Result": "4"})
                self.stats.count("downloads")
                return web.Response(body=data, content_type="application/octet-stream",
                                    headers={"Sake-File-Result": "0"})
            if "dime" in request.headers.get("Content-Type", ""):
                body = gs.dime_soap(body)
            func, call = gs.soap_call(body)
            xuid = Accounts.xuid(account)
            if path.endswith("authservice/authservice.asmx"):
                xml = gs.login(self.store, func, call, account["profileid"], account["name"])
            elif path.endswith("sakestorageserver/storageserver.asmx"):
                xml = await loop.run_in_executor(None, self.sake.handle, func, call, account["profileid"], xuid)
                result = re.search(rb"<%sResult>([^<]*)<" % func.encode(), xml)
                gs.log("sake: %s %s %s by %s -> %s" % (func, gs.text_of(call, "tableid"), gs.text_of(call, "recordid", ""),
                                                     account["name"], result.group(1).decode() if result else "?"))
            elif path.endswith("competitionservice/competitionservice.asmx"):
                xml = gs.competition(func, call)
            else:
                gs.log("game: %s %s: not handled" % (request.method, path))
                return web.Response(body=b"")
            return web.Response(body=xml, content_type="text/xml", charset="utf-8")
        except Exception:
            gs.log("error:", traceback.format_exc())
            return web.Response(body=b"")

    def upload(self, account, headers, body):
        files = [f for f in gs.multipart_files(body, headers.get("Content-Type", "")) if f["filename"] is not None]
        if len(files) != 1:
            return web.Response(body=b"", headers={"Sake-File-Result": "2"})
        data = files[0]["data"]
        if len(data) > self.store.max_file:
            return web.Response(body=b"", headers={"Sake-File-Result": "5"})
        pid = account["profileid"]
        if self.store.owner_bytes(pid) + len(data) > self.store.quota:
            gs.log("files: upload from %s refused: over the quota" % account["name"])
            return web.Response(body=b"", headers={"Sake-File-Result": "5"})
        fileid = self.store.add_file(pid, data)
        self.stats.count("uploads")
        gs.log("files: upload %d bytes from %s -> file %d" % (len(data), account["name"], fileid))
        return web.Response(body=b"", headers={"Sake-File-Result": "0", "Sake-File-Id": str(fileid)})

    # the dashboard

    def admin_only(handler):
        async def wrapped(self, request):
            if not self.is_admin(request):
                return web.Response(status=403, text="Admins only.")
            return await handler(self, request)
        return wrapped

    @admin_only
    async def dashboard(self, request):
        return web.Response(text=self.dashboard_html, content_type="text/html", charset="utf-8")

    @admin_only
    async def admin_stats(self, request):
        ranges = {"hour": (3600, 60), "day": (86400, 96), "week": (7 * 86400, 84), "month": (30 * 86400, 90)}
        span, buckets = ranges.get(request.query.get("range", "day"), ranges["day"])
        step, history = self.stats.history(span, buckets)
        files, raw, stored = self.store.stats()
        db_bytes = sum(p.stat().st_size for p in self.data_dir.glob("community.db*"))
        with self.store.lock:
            logos = self.store.db.execute("SELECT COUNT(*), COALESCE(SUM(b.stored), 0) FROM logos l "
                                          "JOIN blobs b ON b.sha = l.sha").fetchone()
        accounts = self.accounts.all()
        records = sum(len(t) for name, t in self.store.records.items() if name == "UserContent")
        with self.store.lock:
            downloads = sum(f[1] or 0 for f in self.store.files.values())
        today = time.mktime(time.strptime(time.strftime("%Y-%m-%d"), "%Y-%m-%d"))
        return web.json_response({
            "now": {
                "connections": self.stats.active, "max_connections": self.max_connections,
                "peak_since_start": self.stats.max_peak,
                "players_online": self.stats.players_online(),
                "bytes_in": self.stats.total_in, "bytes_out": self.stats.total_out,
                "since": self.stats.started,
                "relay_games": len(self.relay.games), "relayed_packets": self.relay.relayed,
                "relay_sessions": self.relay.sessions,
            },
            "totals": {"all": self.stats.totals(), "today": self.stats.totals(today),
                       "file_downloads": downloads},
            "players": {
                "accounts": len(accounts),
                "banned": sum(1 for a in accounts if a["banned"]),
                "today": self.stats.unique_players(0), "week": self.stats.unique_players(7),
                "month": self.stats.unique_players(30),
            },
            "storage": {
                "files": files, "raw": raw, "stored": stored, "database": db_bytes,
                "logos": logos[0], "logo_bytes": logos[1], "uploads": records,
                "free": self._free_space(),
            },
            "history": {"step": step, "rows": history,
                        "columns": ["t"] + list(Stats.COLUMNS)},
        })

    def _free_space(self):
        try:
            import shutil
            return shutil.disk_usage(self.data_dir).free
        except OSError:
            return 0

    @admin_only
    async def admin_uploads(self, request):
        out = []
        names = {a["profileid"]: a["name"] for a in self.accounts.all()}
        for rec in self.store.all("UserContent"):
            f = rec["fields"]
            fid = int((f.get("F00") or [None, 0])[1] or 0)
            size, downloads = self.store.file_info(fid)
            out.append({
                "id": rec["recordid"], "title": (f.get("Title") or [None, ""])[1],
                "type": (f.get("ContentType") or [None, -1])[1],
                "author": names.get(rec["ownerid"], "profile %d" % rec["ownerid"]),
                "size": size, "downloads": downloads, "created": rec["created"],
                "hidden": int((f.get("Moderated") or [None, 0])[1] or 0) != 0,
                "picture": int((f.get("ThumbSize") or [None, 0])[1] or 0) > 16,
                "deleted": int((f.get("Deleted") or [None, 0])[1] or 0) != 0,
            })
        out.sort(key=lambda r: -r["created"])
        return web.json_response(out)

    @staticmethod
    def thumb_blob(rec):
        """The picture the game keeps with an upload (ThumbData0, 1 ...)."""
        out = b""
        for k in range(16):
            v = rec["fields"].get("ThumbData%d" % k)
            if not v or v[0] != "binaryDataValue":
                break
            out += base64.b64decode(v[1] or "")
        return out

    @admin_only
    async def admin_upload_thumb(self, request):
        rec = self.store.get("UserContent", int(request.match_info["id"]))
        if not rec:
            return web.Response(status=404)
        data = await asyncio.get_running_loop().run_in_executor(None, thumbs.thumb_png, self.thumb_blob(rec))
        if not data:
            return web.Response(status=404, text="No picture.")
        return web.Response(body=data, content_type="image/png", headers={"Cache-Control": "private, max-age=3600"})

    @admin_only
    async def admin_upload_action(self, request):
        rid, action = int(request.match_info["id"]), request.match_info["action"]
        if not self.store.get("UserContent", rid):
            return web.json_response({"ok": False, "error": "No such upload."}, status=404)
        if action == "delete":
            self.store.delete("UserContent", rid)
        elif action in ("hide", "show"):
            self.store.update("UserContent", rid, {"Moderated": ["intValue", 1 if action == "hide" else 0]})
        else:
            return web.json_response({"ok": False}, status=400)
        gs.log("admin: upload %d %s" % (rid, action))
        return web.json_response({"ok": True})

    @admin_only
    async def admin_users(self, request):
        out = []
        uploads = collections.Counter(r["ownerid"] for r in self.store.all("UserContent"))
        for a in self.accounts.all():
            out.append(dict(a, uploads=uploads.get(a["profileid"], 0),
                            storage=self.store.owner_bytes(a["profileid"]),
                            online=a["id"] in self.stats.recent))
        return web.json_response(out)

    @admin_only
    async def admin_user_action(self, request):
        aid, action = int(request.match_info["id"]), request.match_info["action"]
        if not self.accounts.get(aid):
            return web.json_response({"ok": False, "error": "No such account."}, status=404)
        result = {"ok": True}
        if action == "ban":
            self.accounts.set_banned(aid, True)
        elif action == "unban":
            self.accounts.set_banned(aid, False)
        elif action == "reset":
            result["password"] = self.accounts.reset_password(aid)
        elif action == "delete":
            result["removed"] = self.accounts.delete(aid)
        else:
            return web.json_response({"ok": False}, status=400)
        gs.log("admin: account %d %s" % (aid, action))
        return web.json_response(result)

    async def health(self, request):
        return web.json_response({"ok": True})

    # background

    async def background(self, app):
        async def minutes():
            while True:
                await asyncio.sleep(60 - now() % 60 + 0.5)
                try:
                    self.stats.flush()
                except Exception:
                    gs.log("error:", traceback.format_exc())

        async def janitor():
            while True:
                await asyncio.sleep(3600)
                try:
                    await asyncio.get_running_loop().run_in_executor(None, self.store.collect_garbage)
                except Exception:
                    gs.log("error:", traceback.format_exc())

        tasks = [asyncio.create_task(minutes()), asyncio.create_task(janitor())]
        yield
        for t in tasks:
            t.cancel()

    def build(self):
        app = web.Application(middlewares=[self.meter],
                              client_max_size=max([self.store.max_file] + list(self.max_media.values())) + (1 << 20))
        b = self.base
        app.router.add_post(b + "/api/register", self.api_register)
        app.router.add_post(b + "/api/login", self.api_login)
        app.router.add_post(b + "/api/logout", self.api_logout)
        app.router.add_route("*", b + "/api/session", self.api_session)
        app.router.add_get(b + "/api/status", self.api_status)
        app.router.add_get(b + "/api/friends", self.api_friends)
        app.router.add_post(b + "/api/friends", self.api_friends)
        app.router.add_post(b + "/api/invite", self.api_invite)
        app.router.add_post(b + "/api/stats/{what}", self.api_stats)
        app.router.add_route("*", b + "/api/logo/{hash}", self.api_logo)
        app.router.add_get(b + "/api/logos/wanted", self.api_logos_wanted)
        app.router.add_post(b + "/api/media", self.api_media_post)
        app.router.add_get(b + "/api/media/{sha}", self.api_media_get)  # (and HEAD)
        app.router.add_route("*", b + "/api/entrance/{fileid}", self.api_entrance)
        app.router.add_route("*", b + "/game/{tail:.*}", self.game)
        self.relay.routes(app, b)
        app.router.add_get("/__health", self.health)
        app.router.add_get("/", self.dashboard)
        app.router.add_get("/api/admin/stats", self.admin_stats)
        app.router.add_get("/api/admin/uploads", self.admin_uploads)
        app.router.add_get("/api/admin/upload/{id}/thumb.png", self.admin_upload_thumb)
        app.router.add_post("/api/admin/upload/{id}/{action}", self.admin_upload_action)
        app.router.add_get("/api/admin/users", self.admin_users)
        app.router.add_post("/api/admin/user/{id}/{action}", self.admin_user_action)
        app.cleanup_ctx.append(self.background)
        return app


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    # The defaults are the launcher's Custom server on this PC (127.0.0.1:8411);
    # Project Index passes its own (8101, its public mount /svr).
    ap.add_argument("--port", type=int, default=8411)
    ap.add_argument("--data", default=str(HERE / "data"))
    ap.add_argument("--base", default="", help="where the game's half is (Project Index: its public mount)")
    ap.add_argument("--log", default=None, help="also write every event to this file")
    ap.add_argument("--max-connections", type=int, default=100, help="game requests at once")
    ap.add_argument("--max-file-mb", type=int, default=8, help="the largest upload")
    ap.add_argument("--quota-mb", type=int, default=256, help="what one player may store (uncompressed)")
    ap.add_argument("--max-music-mb", type=int, default=512, help="the largest entrance song sent (stored as MP3)")
    ap.add_argument("--max-movie-mb", type=int, default=1024, help="the largest entrance movie sent (MP4; kept 4 min)")
    ap.add_argument("--media-quota-mb", type=int, default=8192, help="entrance songs and movies one player may store")
    args = ap.parse_args()
    if args.log:
        gs.LOG = open(args.log, "a", encoding="utf-8")
    service = Service(args)
    gs.log("Community Creations server on %s:%d (game: %s/, dashboard: /), data in %s, up to %d connections" % (
        args.host, args.port, service.base, os.path.abspath(args.data), args.max_connections))
    web.run_app(service.build(), host=args.host, port=args.port, print=None, access_log=None)


if __name__ == "__main__":
    main()
