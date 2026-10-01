"""WWE SmackDown vs. Raw 2011 (PC port) - the GameSpy services Community
Creations used (gamename svsr11x360, shut down in 2014): the protocol and the
storage, without a web server (server.py serves them).

  AuthService          AuthService/AuthService.asmx          LoginProfile: a signed login certificate
  Sake storage         SakeStorageServer/StorageServer.asmx  Community Creations records (SOAP)
  Sake files           SakeFileServer/upload.aspx, download.aspx
  CompetitionService   CompetitionService/CompetitionService.asmx  (ranked reports: accepted, ignored)

Who is asking is decided by server.py (the player's account); the game's own
login ticket and XUIDs are not trusted.
"""

import base64
import collections
import datetime
import hashlib
import json
import lzma
import os
import random
import re
import secrets
import sqlite3
import struct
import threading
import time
import xml.etree.ElementTree as ET

# The key login certificates are signed with. The port writes this modulus
# over GameSpy's in the game (online.cpp), so the game accepts our
# certificates. (It protects nothing: a login certificate is only checked by
# the game itself.)
SIGNING_N = int(
    "EFD6AB4EF900EFFDD0634EE8B5C5E7355B06831E51DF9380E5D73B1516E63D7BE3F6A3323AC4D402CAEAE7250885D7DC"
    "C755F16B77DC7C1B2D14ED9D8CC926DE48F3675C63499910398B12D27B04FF858CB1F928F5CE0A6F2C4841AD3EA0DA13"
    "AEAA68893865509FEDB7F559F52A94DC0B26D145918CF8FEA1F84DF47506EA63", 16)
SIGNING_D = int(
    "01D91C9ED22A60AF0B0108E5A028F33C25046AD43BFB460EB336B25CDA3D51F1F772BA153400BA457F3CCA252DE8A21E"
    "3067082234DD3D13948859C0620A56C351E8D4C6999149EFD2A5602FE0CDD7DA799A4AA68EB69CF9899B90F85CE8CC56"
    "22A07739E296690DB691182BAB2641BFFC337E37EAB14F04667CC7987B532871", 16)

GAME_ID = 2886           # (the game's GameSpy game id)
RECORD_LIMIT = 200       # records one player may own per table
TICKET_LEN = 24          # GP login ticket length

LOG = None
LOG_LOCK = threading.Lock()


def log(*parts):
    line = time.strftime("%H:%M:%S ") + " ".join(str(p) for p in parts)
    print(line, flush=True)
    if LOG:
        with LOG_LOCK:
            LOG.write(line + "\n")
            LOG.flush()


def log_raw(title, data):
    if LOG:
        with LOG_LOCK:
            LOG.write("---- %s\n%s\n" % (title, data.decode("utf-8", "replace") if isinstance(data, bytes) else data))
            LOG.flush()


# -- RSA / certificates ---------------------------------------------------------

MD5_DIGEST_INFO = bytes.fromhex("3020300c06082a864886f70d020505000410")


def rsa_sign_md5(digest, n=SIGNING_N, d=SIGNING_D):
    """RSASSA-PKCS1-v1_5 with MD5 (what the game checks certificates with)."""
    k = (n.bit_length() + 7) // 8
    t = MD5_DIGEST_INFO + digest
    em = b"\x00\x01" + b"\xff" * (k - len(t) - 3) + b"\x00" + t
    return pow(int.from_bytes(em, "big"), d, n).to_bytes(k, "big")


def minimal_bytes(value):
    return value.to_bytes(max(1, (value.bit_length() + 7) // 8), "big")


def make_prime(bits):
    def probable_prime(n):
        for p in (3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
            if n % p == 0:
                return False
        d, s = n - 1, 0
        while d % 2 == 0:
            d //= 2
            s += 1
        for _ in range(32):
            x = pow(random.randrange(2, n - 2), d, n)
            if x in (1, n - 1):
                continue
            for _ in range(s - 1):
                x = pow(x, 2, n)
                if x == n - 1:
                    break
            else:
                return False
        return True
    while True:
        c = secrets.randbits(bits) | (3 << (bits - 2)) | 1
        if probable_prime(c) and (c - 1) % 65537:
            return c


def make_rsa_key():
    while True:
        p, q = make_prime(512), make_prime(512)
        n = p * q
        if n.bit_length() == 1024:
            return n, pow(65537, -1, (p - 1) * (q - 1))


# -- Storage --------------------------------------------------------------------
#
# Uploaded files are stored once per content (named by their SHA-256), lzma
# compressed: the game's files are mostly zero padding (a Created Superstar
# keeps ~1% of its 1.3 MB, a Paint Tool logo ~5%, a highlight reel ~26%).
# Records, ratings and file details are kept in memory too (searches never
# touch the database); the database is written through.

LOGO_SIZE = 1024 + 65536        # a High Resolution logo of the port's (palette + pixels)
CAW_SIZE = 1347532              # a Created Superstar (.cas)
CAW_EXT_OFFSET = 20 + 160000    # its 'XLG1' record: the logos the port keeps beside it
FILE_FIELDS = ["F%02d" % k for k in range(10)]


def fnv64(data, h=14695981039346656037):
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def caw_logo_hashes(data):
    """The port's extra High Resolution logos a Created Superstar refers to."""
    if len(data) != CAW_SIZE:
        return []
    e = data[CAW_EXT_OFFSET:CAW_EXT_OFFSET + 8 + 10 + 6 + 80]
    if len(e) < 104 or struct.unpack_from("<I", e, 0)[0] != 0x584C4731:
        return []
    used = e[8:18]
    hashes = struct.unpack_from("<10Q", e, 24)
    return sorted({"%016X" % h for h, u in zip(hashes, used) if u and h})


class Store:
    def __init__(self, folder, max_file=8 << 20, quota=256 << 20, cache_bytes=64 << 20):
        self.folder = folder
        self.blob_dir = os.path.join(folder, "blobs")
        os.makedirs(self.blob_dir, exist_ok=True)
        self.max_file = max_file
        self.quota = quota
        self.db = sqlite3.connect(os.path.join(folder, "community.db"), check_same_thread=False)
        self.lock = threading.RLock()
        self.cache = collections.OrderedDict()   # sha -> decompressed bytes (recently downloaded)
        self.cache_bytes = 0
        self.cache_max = cache_bytes
        with self.lock:
            self.db.executescript("""
                PRAGMA journal_mode = WAL;
                PRAGMA synchronous = NORMAL;
                CREATE TABLE IF NOT EXISTS profiles (
                    profileid INTEGER PRIMARY KEY AUTOINCREMENT, email TEXT UNIQUE, nick TEXT, created REAL);
                CREATE TABLE IF NOT EXISTS records (
                    recordid INTEGER PRIMARY KEY AUTOINCREMENT, tableid TEXT, ownerid INTEGER,
                    fields TEXT, created REAL, updated REAL);
                CREATE TABLE IF NOT EXISTS ratings (
                    tableid TEXT, recordid INTEGER, profileid INTEGER, rating INTEGER,
                    PRIMARY KEY (tableid, recordid, profileid));
                CREATE TABLE IF NOT EXISTS files (
                    fileid INTEGER PRIMARY KEY AUTOINCREMENT, ownerid INTEGER, size INTEGER,
                    downloads INTEGER DEFAULT 0, created REAL, sha TEXT);
                CREATE TABLE IF NOT EXISTS blobs (sha TEXT PRIMARY KEY, size INTEGER, stored INTEGER, refs INTEGER);
                CREATE TABLE IF NOT EXISTS logos (hash TEXT PRIMARY KEY, sha TEXT, created REAL);
                CREATE TABLE IF NOT EXISTS file_logos (fileid INTEGER, hash TEXT, PRIMARY KEY (fileid, hash));
                CREATE TABLE IF NOT EXISTS field_types (
                    tableid TEXT, name TEXT, type TEXT, PRIMARY KEY (tableid, name));
                CREATE TABLE IF NOT EXISTS settings (name TEXT PRIMARY KEY, value TEXT);
                CREATE TABLE IF NOT EXISTS tickets (ticket TEXT PRIMARY KEY, profileid INTEGER, created REAL);
                CREATE INDEX IF NOT EXISTS records_table ON records (tableid);
                CREATE INDEX IF NOT EXISTS files_owner ON files (ownerid);
            """)
            if "sha" not in [r[1] for r in self.db.execute("PRAGMA table_info(files)")]:
                self.db.execute("ALTER TABLE files ADD COLUMN sha TEXT")
            self.db.execute("INSERT OR IGNORE INTO sqlite_sequence (name, seq) SELECT 'profiles', 100000 "
                            "WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name = 'profiles')")
            self.db.execute("DELETE FROM tickets WHERE created < ?", (time.time() - 7 * 86400,))
            self.db.commit()
            self._migrate_old_files()
            # in memory
            self.records = collections.defaultdict(dict)    # tableid -> {recordid: record}
            for row in self.db.execute("SELECT recordid, tableid, ownerid, fields, created, updated FROM records"):
                self.records[row[1]][row[0]] = {"recordid": row[0], "ownerid": row[2], "fields": json.loads(row[3]),
                                                "created": row[4], "updated": row[5]}
            self.ratings = collections.defaultdict(dict)    # (tableid, recordid) -> {profileid: rating}
            for t, r, p, v in self.db.execute("SELECT tableid, recordid, profileid, rating FROM ratings"):
                self.ratings[(t, r)][p] = v
            self.files = {}                                  # fileid -> [size, downloads, sha, ownerid, created]
            for fid, own, size, dl, created, sha in self.db.execute(
                    "SELECT fileid, ownerid, size, downloads, created, sha FROM files"):
                self.files[fid] = [size, dl, sha, own, created]
            self.types = {(t, n): ty for t, n, ty in self.db.execute("SELECT tableid, name, type FROM field_types")}
        self.peer_key = self._peer_key()
        # the ONLINE menu's NEWS panel (Newsfeed: one record per message, the
        # game shows the newest for its language; Text1-5 are its lines)
        if not self.all("Newsfeed"):
            # (the game joins the lines: each ends with its space)
            lines = ["Welcome to Community Creations on the PC port! ",
                     "Share your Superstars, Threads, logos, highlight reels ",
                     "and stories with other players.", "", ""]
            fields = {"Language": ["asciiStringValue", "en,fr,de,es,it"]}
            for k, line in enumerate(lines):
                fields["Text%d" % (k + 1)] = ["unicodeStringValue", line]
            self.create("Newsfeed", 0, fields)

    def _peer_key(self):
        # The key pair a login certificate hands the player (the game can use it
        # to sign its own messages); one for the whole server.
        with self.lock:
            row = self.db.execute("SELECT value FROM settings WHERE name = 'peer_key'").fetchone()
            if row:
                n, d = (int(x, 16) for x in row[0].split(":"))
                return n, d
            n, d = make_rsa_key()
            self.db.execute("INSERT INTO settings VALUES ('peer_key', ?)", ("%x:%x" % (n, d),))
            self.db.commit()
            return n, d

    def profile(self, email, nick):
        email = email.lower()  # (GP lowercases it, AuthService doesn't)
        with self.lock:
            row = self.db.execute("SELECT profileid FROM profiles WHERE email = ?", (email,)).fetchone()
            if row:
                self.db.execute("UPDATE profiles SET nick = ? WHERE profileid = ?", (nick, row[0]))
                self.db.commit()
                return row[0]
            cur = self.db.execute("INSERT INTO profiles (email, nick, created) VALUES (?, ?, ?)",
                                  (email, nick, time.time()))
            self.db.commit()
            return cur.lastrowid

    def nick(self, profileid):
        with self.lock:
            r = self.db.execute("SELECT nick FROM profiles WHERE profileid = ?", (profileid,)).fetchone()
        return r[0] if r else None

    # login tickets (GP login -> Sake requests)
    def new_ticket(self, profileid):
        alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
        ticket = "".join(secrets.choice(alphabet) for _ in range(TICKET_LEN))
        with self.lock:
            self.db.execute("INSERT INTO tickets VALUES (?, ?, ?)", (ticket, profileid, time.time()))
            self.db.commit()
        return ticket

    def ticket_profile(self, ticket):
        with self.lock:
            r = self.db.execute("SELECT profileid FROM tickets WHERE ticket = ?", (ticket,)).fetchone()
        return r[0] if r else 0

    # records: fields = {name: [type, value]}
    def create(self, tableid, ownerid, fields):
        with self.lock:
            self._learn_types(tableid, fields)
            now = time.time()
            cur = self.db.execute(
                "INSERT INTO records (tableid, ownerid, fields, created, updated) VALUES (?, ?, ?, ?, ?)",
                (tableid, ownerid, json.dumps(fields), now, now))
            self.db.commit()
            rid = cur.lastrowid
            self.records[tableid][rid] = {"recordid": rid, "ownerid": ownerid, "fields": fields,
                                          "created": now, "updated": now}
            return rid

    def update(self, tableid, recordid, fields):
        with self.lock:
            rec = self.records[tableid].get(recordid)
            if not rec:
                return None
            self._learn_types(tableid, fields)
            old_files = self.record_files(rec)
            rec["fields"].update(fields)
            rec["updated"] = time.time()
            self.db.execute("UPDATE records SET fields = ?, updated = ? WHERE recordid = ?",
                            (json.dumps(rec["fields"]), rec["updated"], recordid))
            self.db.commit()
            for fid in old_files - self.record_files(rec):   # (a file replaced by a new one)
                self.release_file(fid)
            return rec

    def delete(self, tableid, recordid):
        with self.lock:
            rec = self.records[tableid].pop(recordid, None)
            self.db.execute("DELETE FROM records WHERE tableid = ? AND recordid = ?", (tableid, recordid))
            self.db.execute("DELETE FROM ratings WHERE tableid = ? AND recordid = ?", (tableid, recordid))
            self.db.commit()
            self.ratings.pop((tableid, recordid), None)
            for fid in self.record_files(rec) if rec else ():
                self.release_file(fid)

    def get(self, tableid, recordid):
        with self.lock:
            return self.records[tableid].get(recordid)

    def all(self, tableid):
        with self.lock:
            return sorted(self.records[tableid].values(), key=lambda r: r["recordid"])

    def count_owned(self, tableid, ownerid):
        with self.lock:
            return sum(1 for r in self.records[tableid].values() if r["ownerid"] == ownerid)

    def rate(self, tableid, recordid, profileid, rating):
        with self.lock:
            self.db.execute("INSERT OR REPLACE INTO ratings VALUES (?, ?, ?, ?)", (tableid, recordid, profileid, rating))
            self.db.commit()
            self.ratings[(tableid, recordid)][profileid] = rating
        return self.rating(tableid, recordid, profileid)

    def rating(self, tableid, recordid, profileid=None):
        with self.lock:
            r = self.ratings.get((tableid, recordid)) or {}
            n = len(r)
            avg = sum(r.values()) / n if n else 0.0
            return n, float(avg), r.get(profileid) if profileid is not None else None

    def _learn_types(self, tableid, fields):
        for name, (type_, _) in fields.items():
            if self.types.get((tableid, name)) != type_:
                self.types[(tableid, name)] = type_
                self.db.execute("INSERT OR REPLACE INTO field_types VALUES (?, ?, ?)", (tableid, name, type_))

    def field_type(self, tableid, name):
        return self.types.get((tableid, name))

    # -- files: content-addressed, compressed --------------------------------

    def _blob_path(self, sha):
        return os.path.join(self.blob_dir, sha[:2], sha + ".xz")

    def _put_blob(self, data):
        """Stores data (once per content); returns its sha. Caller holds the lock."""
        sha = hashlib.sha256(data).hexdigest()
        row = self.db.execute("SELECT refs FROM blobs WHERE sha = ?", (sha,)).fetchone()
        if row and os.path.exists(self._blob_path(sha)):
            self.db.execute("UPDATE blobs SET refs = refs + 1 WHERE sha = ?", (sha,))
            return sha
        packed = lzma.compress(data, preset=6)
        path = self._blob_path(sha)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(packed)
        os.replace(tmp, path)
        self.db.execute("INSERT OR REPLACE INTO blobs VALUES (?, ?, ?, ?)",
                        (sha, len(data), len(packed), (row[0] if row else 0) + 1))
        return sha

    def _drop_blob(self, sha):
        """One reference fewer; the content goes with the last. Caller holds the lock."""
        if not sha:
            return
        self.db.execute("UPDATE blobs SET refs = refs - 1 WHERE sha = ?", (sha,))
        row = self.db.execute("SELECT refs FROM blobs WHERE sha = ?", (sha,)).fetchone()
        if row and row[0] <= 0:
            self.db.execute("DELETE FROM blobs WHERE sha = ?", (sha,))
            try:
                os.remove(self._blob_path(sha))
            except OSError:
                pass
            if sha in self.cache:
                self.cache_bytes -= len(self.cache.pop(sha))

    def _get_blob(self, sha):
        with self.lock:
            data = self.cache.get(sha)
            if data is not None:
                self.cache.move_to_end(sha)
                return data
        try:
            with open(self._blob_path(sha), "rb") as f:
                data = lzma.decompress(f.read())
        except (OSError, lzma.LZMAError):
            return None
        with self.lock:
            if len(data) <= self.cache_max // 4:
                self.cache[sha] = data
                self.cache_bytes += len(data)
                while self.cache_bytes > self.cache_max and self.cache:
                    self.cache_bytes -= len(self.cache.popitem(last=False)[1])
        return data

    def owner_bytes(self, ownerid):
        with self.lock:
            return sum(f[0] for f in self.files.values() if f[3] == ownerid)

    def add_file(self, ownerid, data):
        with self.lock:
            sha = self._put_blob(data)
            now = time.time()
            cur = self.db.execute("INSERT INTO files (ownerid, size, created, sha) VALUES (?, ?, ?, ?)",
                                  (ownerid, len(data), now, sha))
            fileid = cur.lastrowid
            self.files[fileid] = [len(data), 0, sha, ownerid, now]
            for h in caw_logo_hashes(data):
                self.db.execute("INSERT OR IGNORE INTO file_logos VALUES (?, ?)", (fileid, h))
            self.db.commit()
            return fileid

    def read_file(self, fileid, count=True):
        with self.lock:
            f = self.files.get(fileid)
        if not f:
            return None
        data = self._get_blob(f[2])
        if data is not None and count:
            with self.lock:
                f[1] += 1
                self.db.execute("UPDATE files SET downloads = downloads + 1 WHERE fileid = ?", (fileid,))
                self.db.commit()
        return data

    def file_info(self, fileid):
        with self.lock:
            f = self.files.get(fileid)
        return (f[0], f[1]) if f else (0, 0)

    def file_owner(self, fileid):
        with self.lock:
            f = self.files.get(fileid)
        return f[3] if f else None

    def release_file(self, fileid):
        with self.lock:
            f = self.files.pop(fileid, None)
            if not f:
                return
            self.db.execute("DELETE FROM files WHERE fileid = ?", (fileid,))
            self.db.execute("DELETE FROM file_logos WHERE fileid = ?", (fileid,))
            self._drop_blob(f[2])
            self.db.commit()

    @staticmethod
    def record_files(rec):
        ids = set()
        for name in FILE_FIELDS:
            v = rec["fields"].get(name)
            if v and v[0] == "intValue" and int(v[1] or 0) > 0:
                ids.add(int(v[1]))
        return ids

    def collect_garbage(self, min_age=3600):
        """Files no record points at (an upload whose record never came, or
        a record deleted elsewhere), once they are min_age seconds old."""
        with self.lock:
            used = set()
            for table in self.records.values():
                for rec in table.values():
                    used |= self.record_files(rec)
            old = [fid for fid, f in self.files.items() if fid not in used and time.time() - f[4] > min_age]
        for fid in old:
            self.release_file(fid)
        if old:
            log("storage: %d unused file(s) removed" % len(old))

    def stats(self):
        with self.lock:
            n, raw, stored = self.db.execute("SELECT COUNT(*), COALESCE(SUM(size), 0), COALESCE(SUM(stored), 0) "
                                             "FROM blobs").fetchone()
        return n, raw, stored

    # -- the port's extra Superstar logos (Saves\.logos) ----------------------

    def put_logo(self, hash16, data):
        if len(data) != LOGO_SIZE or "%016X" % fnv64(data) != hash16:
            return False
        with self.lock:
            if self.db.execute("SELECT 1 FROM logos WHERE hash = ?", (hash16,)).fetchone():
                return True
            sha = self._put_blob(data)
            self.db.execute("INSERT INTO logos VALUES (?, ?, ?)", (hash16, sha, time.time()))
            self.db.commit()
        return True

    def get_logo(self, hash16):
        with self.lock:
            row = self.db.execute("SELECT sha FROM logos WHERE hash = ?", (hash16,)).fetchone()
        return self._get_blob(row[0]) if row else None

    def wanted_logos(self, limit=200):
        """Logos an uploaded Superstar refers to that the server doesn't have yet."""
        with self.lock:
            rows = self.db.execute("SELECT DISTINCT fl.hash FROM file_logos fl LEFT JOIN logos l ON l.hash = fl.hash "
                                   "WHERE l.hash IS NULL LIMIT ?", (limit,)).fetchall()
        return [r[0] for r in rows]

    def _migrate_old_files(self):
        """files/<id>.bin of the first test servers -> the blob store."""
        old_dir = os.path.join(self.folder, "files")
        if not os.path.isdir(old_dir):
            return
        for fid, sha in self.db.execute("SELECT fileid, sha FROM files").fetchall():
            path = os.path.join(old_dir, "%d.bin" % fid)
            if sha or not os.path.exists(path):
                continue
            with open(path, "rb") as f:
                data = f.read()
            new = self._put_blob(data)
            self.db.execute("UPDATE files SET sha = ?, size = ? WHERE fileid = ?", (new, len(data), fid))
            for h in caw_logo_hashes(data):
                self.db.execute("INSERT OR IGNORE INTO file_logos VALUES (?, ?)", (fid, h))
            self.db.commit()
            os.remove(path)
            log("storage: file %d moved to the blob store" % fid)
        try:
            os.rmdir(old_dir)
        except OSError:
            pass


# -- Sake filters ("Deleted = 0 AND ( AuthorName = '123' OR Moderated = 1 )") ----

TOKEN = re.compile(r"\s*(?:(\d+\.\d+|\d+)|('(?:[^']|'')*')|(<>|!=|<=|>=|=|<|>|\(|\)|,)|([A-Za-z_][A-Za-z0-9_.]*))")


class FilterError(Exception):
    pass


def tokenize(text):
    pos, out = 0, []
    text = text.strip()
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m or m.end() == pos:
            raise FilterError("bad filter near %r" % text[pos:pos + 20])
        num, string, op, word = m.groups()
        if num is not None:
            out.append(("num", float(num) if "." in num else int(num)))
        elif string is not None:
            out.append(("str", string[1:-1].replace("''", "'")))
        elif op is not None:
            out.append(("op", op))
        else:
            up = word.upper()
            out.append(("kw", up) if up in ("AND", "OR", "NOT", "IN", "LIKE", "ESCAPE", "IS", "NULL") else ("id", word))
        pos = m.end()
    return out


def parse_filter(text):
    """The filter as a function record-values -> bool."""
    if not text or not text.strip():
        return lambda get: True
    toks = tokenize(text)
    i = [0]

    def peek(k=0):
        return toks[i[0] + k] if i[0] + k < len(toks) else (None, None)

    def take(kind=None, value=None):
        t = peek()
        if kind and t[0] != kind or value is not None and t[1] != value:
            raise FilterError("expected %s %s, got %s" % (kind, value, t))
        i[0] += 1
        return t

    def expr():
        left = term()
        while peek() == ("kw", "OR"):
            take()
            right = term()
            left = (lambda a, b: lambda g: a(g) or b(g))(left, right)
        return left

    def term():
        left = factor()
        while peek() == ("kw", "AND"):
            take()
            right = factor()
            left = (lambda a, b: lambda g: a(g) and b(g))(left, right)
        return left

    def factor():
        if peek() == ("kw", "NOT"):
            take()
            f = factor()
            return lambda g: not f(g)
        if peek() == ("op", "("):
            take()
            f = expr()
            take("op", ")")
            return f
        return comparison()

    def value():
        t = take()
        if t[0] in ("num", "str"):
            return lambda g, v=t[1]: v
        if t[0] == "id":
            return lambda g, n=t[1]: g(n)
        if t == ("kw", "NULL"):
            return lambda g: None
        raise FilterError("unexpected %s" % (t,))

    def comparison():
        left = value()
        t = peek()
        if t == ("kw", "IS"):
            take()
            negate = peek() == ("kw", "NOT")
            if negate:
                take()
            take("kw", "NULL")
            return lambda g: (left(g) is None) != negate
        negate = False
        if t == ("kw", "NOT"):
            take()
            negate = True
            t = peek()
        if t == ("kw", "IN"):
            take()
            take("op", "(")
            items = [value()]
            while peek() == ("op", ","):
                take()
                items.append(value())
            take("op", ")")
            return lambda g: (any(compare(left(g), "=", it(g)) for it in items)) != negate
        if t == ("kw", "LIKE"):
            take()
            pattern = value()
            escape = None
            if peek() == ("kw", "ESCAPE"):
                take()
                escape = take("str")[1]
            return lambda g: like(left(g), pattern(g), escape) != negate
        op = take("op")[1]
        right = value()
        return lambda g: compare(left(g), op, right(g))

    f = expr()
    if i[0] != len(toks):
        raise FilterError("trailing tokens in filter")
    return f


def coerce(a, b):
    if isinstance(a, bool):
        a = int(a)
    if isinstance(b, bool):
        b = int(b)
    if isinstance(a, (int, float)) and isinstance(b, str):
        try:
            b = float(b) if "." in b else int(b)
        except ValueError:
            a = str(a)
    elif isinstance(b, (int, float)) and isinstance(a, str):
        try:
            a = float(a) if "." in a else int(a)
        except ValueError:
            b = str(b)
    return a, b


def compare(a, op, b):
    if a is None or b is None:
        return op in ("<>", "!=") and (a is None) != (b is None)
    a, b = coerce(a, b)
    try:
        return {"=": a == b, "<>": a != b, "!=": a != b, "<": a < b, ">": a > b, "<=": a <= b, ">=": a >= b}[op]
    except TypeError:
        return False


def like(value, pattern, escape):
    if value is None or pattern is None:
        return False
    out, k = "", 0
    while k < len(pattern):
        c = pattern[k]
        if escape and c == escape and k + 1 < len(pattern):
            out += re.escape(pattern[k + 1])
            k += 2
            continue
        out += ".*" if c == "%" else "." if c == "_" else re.escape(c)
        k += 1
    return re.fullmatch(out, str(value), re.IGNORECASE | re.DOTALL) is not None


def parse_sort(text):
    keys = []
    for part in (text or "").split(","):
        words = part.split()
        if words:
            keys.append((words[0], len(words) > 1 and words[1].lower() == "desc"))
    return keys


# -- SOAP -------------------------------------------------------------------------

def local(tag):
    return tag.rsplit("}", 1)[-1]


def child(el, name):
    if el is None:
        return None
    for c in el:
        if local(c.tag) == name:
            return c
    return None


def text_of(el, name, default=""):
    c = child(el, name)
    return (c.text or "") if c is not None else default


def soap_call(body):
    root = ET.fromstring(body)
    env_body = next(el for el in root if local(el.tag) == "Body")
    call = next(iter(env_body))
    return local(call.tag), call


def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace('"', "&quot;").replace("'", "&apos;"))


def envelope(inner):
    return ('<?xml version="1.0" encoding="utf-8"?><soap:Envelope xmlns:soap="http://schemas.xmlsoap.org/soap/envelope/" '
            'xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:xsd="http://www.w3.org/2001/XMLSchema">'
            "<soap:Body>%s</soap:Body></soap:Envelope>" % inner).encode("utf-8")


# Sake value types (element names) -> python
VALUE_TYPES = ("byteValue", "shortValue", "intValue", "floatValue", "asciiStringValue", "unicodeStringValue",
               "booleanValue", "dateAndTimeValue", "binaryDataValue", "int64Value")


def read_values(values_el):
    fields = {}
    for rf in values_el if values_el is not None else []:
        name = text_of(rf, "name")
        v = child(rf, "value")
        typed = next(iter(v)) if v is not None and len(v) else None
        if not name or typed is None:
            continue
        type_ = local(typed.tag)
        raw = text_of(typed, "value")
        if type_ in ("byteValue", "shortValue", "intValue", "int64Value"):
            val = int(raw or 0)
        elif type_ == "floatValue":
            val = float(raw or 0)
        elif type_ == "booleanValue":
            val = 1 if raw.strip().lower() in ("1", "true") else 0
        else:
            val = raw  # strings as text, binary as base64, dates as text
        fields[name] = [type_, val]
    return fields


def value_xml(type_, val):
    if type_ == "booleanValue":
        v = "true" if val else "false"
    elif type_ == "floatValue":
        v = "%f" % float(val or 0)
    elif val is None:
        v = ""
    else:
        v = esc(val)
    return "<RecordValue><%s><value>%s</value></%s></RecordValue>" % (type_, v, type_)


class Sake:
    def __init__(self, store):
        self.store = store

    def field(self, tableid, rec, name, viewer):
        """A record's value for a requested field: (type, value)."""
        f = rec["fields"]
        if name == "recordid":
            return "intValue", rec["recordid"]
        if name == "ownerid":
            return "intValue", rec["ownerid"]
        if name in ("num_ratings", "average_rating", "my_rating"):
            n, avg, mine = self.store.rating(tableid, rec["recordid"], viewer)
            if name == "num_ratings":
                return "intValue", n
            if name == "average_rating":
                return "floatValue", avg
            return "intValue", mine if mine is not None else 0
        if "." in name:
            base, attr = name.split(".", 1)
            fileid = f.get(base, [None, 0])[1]
            size, downloads = self.store.file_info(int(fileid or 0))
            if attr == "size":
                return "intValue", size
            if attr == "downloads":
                return "intValue", downloads
            if attr in ("create_time", "created"):
                return "dateAndTimeValue", iso(rec["created"])
            return "intValue", 0
        if name in f:
            return f[name][0], f[name][1]
        if name in ("create_time", "CreateTime") and self.store.field_type(tableid, name) is None:
            return "dateAndTimeValue", iso(rec["created"])
        type_ = self.store.field_type(tableid, name) or "intValue"
        return type_, default_for(type_)

    def records_xml(self, tableid, records, fields, viewer):
        rows = []
        for rec in records:
            rows.append("<ArrayOfRecordValue>%s</ArrayOfRecordValue>" %
                        "".join(value_xml(*self.field(tableid, rec, name, viewer)) for name in fields))
        return "<values>%s</values>" % "".join(rows)

    def select(self, tableid, filter_text, viewer, targetfilter=None):
        pred = parse_filter(filter_text)
        out = []
        for rec in self.store.all(tableid):
            def get(name, rec=rec):
                return self.field(tableid, rec, name, viewer)[1]
            if pred(get):
                out.append(rec)
        return out

    def handle(self, func, call, owner, xuid):
        """A Sake call from the player with profile `owner` and XUID `xuid`."""
        tableid = text_of(call, "tableid")
        fields = [s.text or "" for s in (child(call, "fields") or [])]

        def reply(result, extra=""):
            return envelope('<%sResponse xmlns="http://gamespy.net/sake"><%sResult>%s</%sResult>%s</%sResponse>'
                            % (func, func, result, func, extra, func))

        if func == "CreateRecord":
            values = read_values(child(call, "values"))
            # the author is the caller, whatever the game wrote
            if "AuthorName" in values:
                values["AuthorName"] = [values["AuthorName"][0], str(signed64(xuid))]
            if "UserID" in values and values["UserID"][0] == "binaryDataValue":
                values["UserID"] = ["binaryDataValue", base64.b64encode(xuid.to_bytes(8, "big")).decode()]
            if self.store.count_owned(tableid, owner) >= RECORD_LIMIT:
                return reply("RecordLimitReached")
            # its file: uploaded by the same player, the size the record says
            for name in FILE_FIELDS:
                v = values.get(name)
                fid = int(v[1] or 0) if v and v[0] == "intValue" else 0
                if fid <= 0:
                    continue
                size, _ = self.store.file_info(fid)
                expected = values.get("FileSize", [None, None])[1]
                if self.store.file_owner(fid) != owner or (name == "F00" and expected and int(expected) != size):
                    log("sake: %s record refused: file %d is not this player's upload or not its size" % (tableid, fid))
                    return reply("NoPermission")
            recordid = self.store.create(tableid, owner, values)
            log("sake: %s record %d created by %d: %s" % (tableid, recordid, owner, short(values)))
            return reply("Success", "<recordid>%d</recordid>" % recordid)
        if func == "UpdateRecord":
            recordid = int(text_of(call, "recordid", "0") or 0)
            rec = self.store.get(tableid, recordid)
            if not rec:
                return reply("RecordNotFound")
            if rec["ownerid"] != owner:
                log("sake: %s record %d: update by %d refused (owner %d)" % (tableid, recordid, owner, rec["ownerid"]))
                return reply("NotOwned")
            values = read_values(child(call, "values"))
            values.pop("AuthorName", None)
            values.pop("UserID", None)
            self.store.update(tableid, recordid, values)
            return reply("Success")
        if func == "DeleteRecord":
            recordid = int(text_of(call, "recordid", "0") or 0)
            rec = self.store.get(tableid, recordid)
            if not rec:
                return reply("RecordNotFound")
            if rec["ownerid"] != owner:
                return reply("NotOwned")
            self.store.delete(tableid, recordid)
            log("sake: %s record %d deleted" % (tableid, recordid))
            return reply("Success")
        if func in ("SearchForRecords", "GetRandomRecords", "GetRecordCount"):
            filter_text = text_of(call, "filter", None) if child(call, "filter") is not None else None
            try:
                records = self.select(tableid, filter_text, owner)
            except FilterError as e:
                log("sake: filter error %s: %r" % (e, filter_text))
                return reply("FilterInvalid")
            ownerids = [int(x.text or 0) for x in (child(call, "ownerids") or [])]
            if ownerids:
                records = [r for r in records if r["ownerid"] in ownerids]
            if func == "GetRecordCount":
                return reply("Success", "<count>%d</count>" % len(records))
            if func == "GetRandomRecords":
                random.shuffle(records)
                return reply("Success", self.records_xml(tableid, records[:1], fields, owner))
            for key, desc in reversed(parse_sort(text_of(call, "sort", ""))):
                records.sort(key=lambda r, k=key: sort_key(self.field(tableid, r, k, owner)[1]), reverse=desc)
            offset = int(text_of(call, "offset", "0") or 0)
            maximum = int(text_of(call, "max", "0") or 0) or len(records)
            page = records[offset:offset + maximum]
            log("sake: %s search %r sort %r -> %d of %d" % (tableid, filter_text, text_of(call, "sort", ""),
                                                           len(page), len(records)))
            return reply("Success", self.records_xml(tableid, page, fields, owner))
        if func == "GetMyRecords":
            records = [r for r in self.store.all(tableid) if r["ownerid"] == owner]
            return reply("Success", self.records_xml(tableid, records, fields, owner))
        if func == "GetSpecificRecords":
            ids = [int(x.text or 0) for x in (child(call, "recordids") or [])]
            records = [r for r in (self.store.get(tableid, i) for i in ids) if r]
            return reply("Success", self.records_xml(tableid, records, fields, owner))
        if func == "GetRecordLimit":
            return reply("Success", "<limitPerOwner>%d</limitPerOwner><numOwned>%d</numOwned>"
                         % (RECORD_LIMIT, self.store.count_owned(tableid, owner)))
        if func == "RateRecord":
            recordid = int(text_of(call, "recordid", "0") or 0)
            if not self.store.get(tableid, recordid):
                return reply("RecordNotFound")
            n, avg, _ = self.store.rate(tableid, recordid, owner, int(text_of(call, "rating", "0") or 0))
            return reply("Success", "<numRatings>%d</numRatings><averageRating>%f</averageRating>" % (n, avg))
        log("sake: unknown call %s" % func)
        return reply("ServiceDisabled")


def signed64(v):
    return v - (1 << 64) if v >= 1 << 63 else v


def default_for(type_):
    if type_ in ("asciiStringValue", "unicodeStringValue", "binaryDataValue"):
        return ""
    if type_ == "dateAndTimeValue":
        return iso(0)
    if type_ == "floatValue":
        return 0.0
    return 0


def sort_key(v):
    return (0, v) if isinstance(v, (int, float)) else (1, str(v))


def iso(t):
    return datetime.datetime.fromtimestamp(t or 0, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def short(values):
    s = {k: (v[1] if len(str(v[1])) < 40 else "<%d chars>" % len(str(v[1]))) for k, v in values.items()}
    return json.dumps(s)[:400]


# -- AuthService -------------------------------------------------------------------

def login(store, func, call, profileid, nick):
    """AuthService: a login certificate for the caller's profile."""
    partner = int(text_of(call, "partnercode", "0") or 0)
    namespace = int(text_of(call, "namespaceid", "0") or 0)
    peer_n, peer_d = store.peer_key
    cert = {
        "length": 0, "version": 1, "partnercode": partner, "namespaceid": namespace,
        "userid": profileid, "profileid": profileid, "expiretime": int(time.time()) + 7 * 24 * 3600,
        "profilenick": nick[:30], "uniquenick": nick[:20], "cdkeyhash": "",
    }
    serverdata = secrets.token_bytes(128)
    md5 = hashlib.md5()
    for k in ("length", "version", "partnercode", "namespaceid", "userid", "profileid", "expiretime"):
        md5.update(struct.pack("<I", cert[k] & 0xFFFFFFFF))
    for k in ("profilenick", "uniquenick", "cdkeyhash"):
        md5.update(cert[k].encode("ascii", "replace"))
    md5.update(minimal_bytes(peer_n))
    md5.update(minimal_bytes(65537))
    md5.update(serverdata)
    signature = rsa_sign_md5(md5.digest())
    body = "".join("<%s>%s</%s>" % (k, esc(v), k) for k, v in cert.items())
    body += "<peerkeymodulus>%X</peerkeymodulus><peerkeyexponent>010001</peerkeyexponent>" % peer_n
    body += "<serverdata>%s</serverdata><signature>%s</signature>" % (serverdata.hex(), signature.hex())
    inner = ('<%sResponse xmlns="http://gamespy.net/AuthService/"><%sResult><responseCode>0</responseCode>'
             "<certificate>%s</certificate><peerkeyprivate>%s</peerkeyprivate></%sResult></%sResponse>"
             % (func, func, body, peer_d.to_bytes(128, "big").hex(), func, func))
    log("auth: %s -> profile %d (%s)" % (func, profileid, nick))
    return envelope(inner)


# -- CompetitionService --------------------------------------------------------------

def competition(func, call):
    guid = lambda: "%08x-%04x-%04x-%04x-%012x" % tuple(secrets.randbits(b) for b in (32, 16, 16, 16, 48))
    if func in ("CreateSession", "CreateMatchlessSession"):
        extra = "<result>0</result><csid>%s</csid><ccid>%s</ccid>" % (guid(), guid())
    elif func == "SetReportIntention":
        extra = "<result>0</result><ccid>%s</ccid>" % (text_of(call, "ccid") or guid())
    else:
        extra = "<result>0</result>"
    log("competition: %s" % func)
    return envelope('<%sResponse xmlns="http://gamespy.net/competition/"><%sResult>%s</%sResult></%sResponse>'
                    % (func, func, extra, func, func))


def dime_soap(body):
    """The SOAP envelope inside a DIME message (SubmitReport)."""
    start = body.find(b"<?xml")
    end = body.find(b"Envelope>", start)
    return body[start:end + len("Envelope>")] if start >= 0 and end >= 0 else body


# -- HTTP ----------------------------------------------------------------------------

def multipart_files(body, content_type):
    m = re.search(r'boundary=("?)(.+)\1\s*$', content_type)
    if not m:
        return []
    boundary = b"--" + m.group(2).encode("latin-1")
    files = []
    for part in body.split(boundary)[1:]:
        if part.startswith(b"--"):
            break
        part = part[2:] if part.startswith(b"\r\n") else part
        head, _, data = part.partition(b"\r\n\r\n")
        if data.endswith(b"\r\n"):
            data = data[:-2]
        disp = head.decode("latin-1", "replace")
        name = re.search(r'name="([^"]*)"', disp)
        filename = re.search(r'filename="([^"]*)"', disp)
        files.append({"name": name.group(1) if name else "", "filename": filename.group(1) if filename else None,
                      "data": data})
    return files
