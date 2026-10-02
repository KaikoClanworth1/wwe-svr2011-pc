"""
The online match relay, part of the Community Creations server (server.py),
for players who can't reach each other directly (VPNs, mobile networks,
strict NATs).

Games play peer to peer (port/src/p2p.cpp). A signed-in game keeps two
streams open with the server, so it works anywhere the server's https does
(the game's UDP never has to reach it):
    GET  <base>/api/relay/<id>   the server's packets for this game, as they come
    POST <base>/api/relay/<id>   the game's packets (one chunked request)
<id> is the game's 6-byte hardware id (12 hex digits). Both carry frames:
a 16-bit big-endian length, then a packet ("SVRR" + version 1 + type):
    1 register  game -> relay   id[6]                 (keeps the upload alive)
      2 welcome relay -> game   ip[4] port[2]         (its address as seen here)
    3 forward   game -> relay   to[6] packet          (to ff..ff: every game here)
      4 from    relay -> game   ip[4] port[2] packet  (the sender's address)
A zero-length frame is a keepalive. The relay never reads the game's
packets and keeps nothing on disk.
"""
import asyncio
import re
import socket
import struct
import time

from aiohttp import web

MAGIC = b"SVRR"
VERSION = 1
REGISTER, WELCOME, FORWARD, FROM = 1, 2, 3, 4
BROADCAST = b"\xff" * 6
MAX_GAMES = 2048
MAX_BROADCAST = 256    # the most recently seen games a search reaches
RATE = 400             # packets per second one game may send (a match: ~60)
QUEUE = 1024           # packets waiting for a slow game (then dropped)
KEEPALIVE = 5.0        # seconds between keepalives on a quiet download


def packet(kind, body):
    return MAGIC + bytes([VERSION, kind]) + body


def frame(data):
    return struct.pack(">H", len(data)) + data


class Game:
    def __init__(self, gid, account, ip):
        self.id, self.account, self.ip = gid, account, ip
        self.queue = asyncio.Queue(QUEUE)
        self.seen = time.monotonic()
        self.window, self.count = 0.0, 0

    def put(self, data):
        try:
            self.queue.put_nowait(data)
        except asyncio.QueueFull:
            pass  # (a game that stopped reading: it loses packets, not the server's memory)

    def allowed(self):
        now = time.monotonic()
        if now - self.window >= 1.0:
            self.window, self.count = now, 0
        self.count += 1
        return self.count <= RATE


class Relay:
    def __init__(self, service, log):
        self.service, self.log = service, log
        self.games = {}  # id -> Game (with a download open)
        self.relayed = 0

    @staticmethod
    def game_id(request):
        gid = request.match_info["id"].lower()
        return bytes.fromhex(gid) if re.fullmatch(r"[0-9a-f]{12}", gid) else None

    def address(self, ip):
        try:
            return socket.inet_aton(ip) + b"\0\0"
        except OSError:
            return b"\0" * 6

    async def download(self, request):
        account = self.service.account(request)
        gid = self.game_id(request)
        if not account:
            return web.Response(status=401)
        if not gid:
            return web.Response(status=400)
        if gid not in self.games and len(self.games) >= MAX_GAMES:
            return web.Response(status=503)
        ip = self.service.client_ip(request)
        old = self.games.get(gid)
        if old and old.account["id"] != account["id"]:
            return web.Response(status=409)  # (someone else's game id)
        game = Game(gid, account, ip)
        self.games[gid] = game
        if old:
            old.put(None)  # (a reconnect: the old download ends)
        resp = web.StreamResponse(headers={"Content-Type": "application/octet-stream",
                                           "Cache-Control": "no-store", "X-Accel-Buffering": "no"})
        await resp.prepare(request)
        self.log("relay: %s (%s) on from %s, %d game(s)" % (gid.hex(), account["name"], ip, len(self.games)))
        try:
            await resp.write(frame(packet(WELCOME, self.address(ip))))
            while True:
                try:
                    data = await asyncio.wait_for(game.queue.get(), KEEPALIVE)
                except asyncio.TimeoutError:
                    data = b""
                if data is None:
                    break
                out = frame(data)
                while not game.queue.empty() and len(out) < 16384:  # (what's waiting goes together)
                    more = game.queue.get_nowait()
                    if more is None:
                        break
                    out += frame(more)
                await resp.write(out)
        except (ConnectionResetError, asyncio.CancelledError, RuntimeError):
            pass
        finally:
            if self.games.get(gid) is game:
                del self.games[gid]
                self.log("relay: %s off, %d game(s)" % (gid.hex(), len(self.games)))
        return resp

    async def upload(self, request):
        account = self.service.account(request)
        gid = self.game_id(request)
        if not account:
            return web.Response(status=401)
        if not gid:
            return web.Response(status=400)
        # (the answer comes when the upload ends: an answer started while the body
        # still streams made the Project Index portal end the request with a 502)
        content = request.content
        try:
            while True:
                size = struct.unpack(">H", await content.readexactly(2))[0]
                data = await content.readexactly(size) if size else b""
                game = self.games.get(gid)
                if not game or game.account["id"] != account["id"]:
                    continue  # (its download isn't open yet, or it's someone else's)
                game.seen = time.monotonic()
                if len(data) < 6 or data[:4] != MAGIC or data[4] != VERSION or not game.allowed():
                    continue
                if data[5] == FORWARD and len(data) > 12:
                    to, inner = data[6:12], data[12:]
                    out = packet(FROM, self.address(game.ip) + inner)
                    if to == BROADCAST:
                        others = sorted((g for g in self.games.values() if g is not game), key=lambda g: -g.seen)
                        for other in others[:MAX_BROADCAST]:
                            other.put(out)
                    elif to in self.games:
                        self.games[to].put(out)
                        self.relayed += 1
        except (asyncio.IncompleteReadError, ConnectionResetError, asyncio.CancelledError):
            pass
        return web.Response(status=200)

    def routes(self, app, base):
        app.router.add_get(base + "/api/relay/{id}", self.download)
        app.router.add_post(base + "/api/relay/{id}", self.upload)
