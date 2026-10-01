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

HERE = Path(__file__).resolve().parent
LOOPBACK = frozenset({"127.0.0.1", "::1", "::ffff:127.0.0.1"})
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 _.\-]{2,14}$")  # 3-15, what the game can show
XUID_BASE = 0x0009000100000000  # + account id: the player's XUID on every device
PBKDF2_ROUNDS = 240000


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
            self.db.execute("DELETE FROM accounts WHERE id = ?", (account_id,))
            self.db.commit()
        self.cache.clear()
        return removed

    def all(self):
        with self.lock:
            rows = self.db.execute("SELECT id, name, profileid, created, last_seen, banned, ip FROM accounts "
                                   "ORDER BY id").fetchall()
        return [self._row(r) for r in rows]


# -- Statistics ----------------------------------------------------------------

class Stats:
    """Per-minute counters of the game's traffic, kept in the database (the
    dashboard's history), plus the live gauges."""

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
            self.db.commit()
        self.active = 0
        self.peak = 0
        self.max_peak = 0
        self.minute = self._zero()
        self.recent = {}      # account id -> last request time
        self.total_in = 0
        self.total_out = 0
        self.started = now()

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
               m["uploads"], m["downloads"], m["logins"], m["registrations"], m["refused"])
        self.peak = self.active
        with self.lock:
            self.db.execute("INSERT OR REPLACE INTO stats VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", row)
            self.db.commit()

    def history(self, span, buckets):
        """[(t, requests, bytes_in, bytes_out, peak, players, uploads, downloads)] over the last
        `span` seconds, in `buckets` equal steps (sums; peak and players: max)."""
        start = int(now() - span)
        step = max(60, span // buckets)
        with self.lock:
            rows = self.db.execute(
                "SELECT (t - ?) / ? AS b, MIN(t), SUM(requests), SUM(bytes_in), SUM(bytes_out), MAX(peak), "
                "MAX(players), SUM(uploads), SUM(downloads) FROM stats WHERE t >= ? GROUP BY b ORDER BY b",
                (start, step, start)).fetchall()
        got = {r[0]: r[1:] for r in rows}
        out = []
        for b in range(buckets):
            r = got.get(b)
            out.append([start + b * step] + ([0] * 8 if r is None else [r[i] or 0 for i in range(1, 9)]))
        return step, out

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
        self.max_connections = args.max_connections
        self.attempts = collections.defaultdict(collections.deque)  # ip -> login/register times
        self.data_dir = Path(args.data)
        self.dashboard_html = (HERE / "dashboard.html").read_text(encoding="utf-8")

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
        if not request.path.startswith(self.base + "/"):
            return await handler(request)
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
        return web.json_response({
            "now": {
                "connections": self.stats.active, "max_connections": self.max_connections,
                "peak_since_start": self.stats.max_peak,
                "players_online": self.stats.players_online(),
                "bytes_in": self.stats.total_in, "bytes_out": self.stats.total_out,
                "since": self.stats.started,
            },
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
                        "columns": ["t", "requests", "bytes_in", "bytes_out", "peak", "players",
                                    "uploads", "downloads"]},
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
                "deleted": int((f.get("Deleted") or [None, 0])[1] or 0) != 0,
            })
        out.sort(key=lambda r: -r["created"])
        return web.json_response(out)

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
        app = web.Application(middlewares=[self.meter], client_max_size=self.store.max_file + (1 << 20))
        b = self.base
        app.router.add_post(b + "/api/register", self.api_register)
        app.router.add_post(b + "/api/login", self.api_login)
        app.router.add_post(b + "/api/logout", self.api_logout)
        app.router.add_route("*", b + "/api/session", self.api_session)
        app.router.add_get(b + "/api/status", self.api_status)
        app.router.add_route("*", b + "/api/logo/{hash}", self.api_logo)
        app.router.add_get(b + "/api/logos/wanted", self.api_logos_wanted)
        app.router.add_route("*", b + "/game/{tail:.*}", self.game)
        app.router.add_get("/__health", self.health)
        app.router.add_get("/", self.dashboard)
        app.router.add_get("/api/admin/stats", self.admin_stats)
        app.router.add_get("/api/admin/uploads", self.admin_uploads)
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
    args = ap.parse_args()
    if args.log:
        gs.LOG = open(args.log, "a", encoding="utf-8")
    service = Service(args)
    gs.log("Community Creations server on %s:%d (game: %s/, dashboard: /), data in %s, up to %d connections" % (
        args.host, args.port, service.base, os.path.abspath(args.data), args.max_connections))
    web.run_app(service.build(), host=args.host, port=args.port, print=None, access_log=None)


if __name__ == "__main__":
    main()
