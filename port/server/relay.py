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

UDP (--relay-udp-port): the same packets, one per datagram, no length -
over TCP one lost segment held back every later packet, and online matches
step only once every player's input is in. A game asks for a ticket over
https,
    GET  <base>/api/relay/<id>/udp   -> {"port": p, "ticket": hex}
registers from its P2P socket with it every few seconds,
    1 register  game -> relay   id[6] ticket[16]
and is answered with a welcome; from then on its forwards come over UDP and
packets for it go there (the https download keeps working: a game whose UDP
goes quiet for 20 s gets its packets there again).
"""
import asyncio
import os
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
TICKET_LIFE = 3600.0   # seconds a UDP ticket may register
UDP_QUIET = 20.0       # a UDP game not heard from for this long: back to its https download


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


class UdpGame:
    def __init__(self, gid, account, addr):
        self.id, self.account, self.addr = gid, account, addr
        self.seen = time.monotonic()
        self.window, self.count = 0.0, 0

    def fresh(self):
        return time.monotonic() - self.seen < UDP_QUIET

    allowed = Game.allowed


class UdpEndpoint(asyncio.DatagramProtocol):
    def __init__(self, relay):
        self.relay = relay

    def connection_made(self, transport):
        self.relay.transport = transport

    def datagram_received(self, data, addr):
        try:
            self.relay.on_datagram(data, addr)
        except Exception as e:  # (one bad datagram never stops the relay)
            self.relay.log("relay: udp packet from %s:%d dropped (%s)" % (addr[0], addr[1], e))


class Relay:
    def __init__(self, service, log, udp_port=0):
        self.service, self.log = service, log
        self.games = {}  # id -> Game (with a download open)
        self.relayed = 0
        self.sessions = 0  # games that came on since the start
        self.udp_port = udp_port
        self.transport = None   # (the UDP socket, once open)
        self.tickets = {}       # ticket -> (game id, account, expiry)
        self.udp_games = {}     # id -> UdpGame
        self.udp_by_addr = {}   # (ip, port) -> id

    async def start_udp(self, app):
        if self.udp_port:
            loop = asyncio.get_running_loop()
            await loop.create_datagram_endpoint(lambda: UdpEndpoint(self), local_addr=("0.0.0.0", self.udp_port))
            self.log("relay: UDP on port %d" % self.udp_port)
        yield
        if self.transport:
            self.transport.close()

    def deliver(self, to, out):
        """A packet for game `to`: over UDP while it registers there, else its download."""
        g = self.udp_games.get(to)
        if g and g.fresh() and self.transport:
            self.transport.sendto(out, g.addr)
            return True
        if to in self.games:
            self.games[to].put(out)
            return True
        return False

    def broadcast(self, sender, out):
        recent = {}
        for g in list(self.games.values()) + [g for g in self.udp_games.values() if g.fresh()]:
            if g.id != sender and (g.id not in recent or g.seen > recent[g.id]):
                recent[g.id] = g.seen
        for gid in sorted(recent, key=lambda i: -recent[i])[:MAX_BROADCAST]:
            self.deliver(gid, out)

    async def udp_ticket(self, request):
        account = self.service.account(request)
        gid = self.game_id(request)
        if not account:
            return web.Response(status=401)
        if not gid or not self.udp_port:
            return web.Response(status=404)
        now = time.monotonic()
        for t in [t for t, (_, _, exp) in self.tickets.items() if exp < now]:
            del self.tickets[t]
        ticket = os.urandom(16)
        self.tickets[ticket] = (gid, account, now + TICKET_LIFE)
        return web.json_response({"port": self.udp_port, "ticket": ticket.hex()})

    def on_datagram(self, data, addr):
        if len(data) < 6 or data[:4] != MAGIC or data[4] != VERSION:
            return
        kind = data[5]
        if kind == REGISTER and len(data) >= 6 + 6 + 16:
            gid, ticket = data[6:12], data[12:28]
            t = self.tickets.get(ticket)
            if not t or t[0] != gid or t[2] < time.monotonic():
                return
            g = self.udp_games.get(gid)
            if not g or g.addr != addr:
                if g:
                    self.udp_by_addr.pop(g.addr, None)
                g = UdpGame(gid, t[1], addr)
                self.udp_games[gid] = g
                self.udp_by_addr[addr] = gid
                self.log("relay: %s (%s) on UDP from %s:%d" % (gid.hex(), t[1]["name"], addr[0], addr[1]))
            g.seen = time.monotonic()
            self.transport.sendto(packet(WELCOME, self.address(addr[0], addr[1])), addr)
        elif kind == FORWARD and len(data) > 12:
            gid = self.udp_by_addr.get(addr)
            g = self.udp_games.get(gid) if gid else None
            if not g or g.addr != addr or not g.allowed():
                return
            g.seen = time.monotonic()
            to, inner = data[6:12], data[12:]
            out = packet(FROM, self.address(addr[0], addr[1]) + inner)
            if to == BROADCAST:
                self.broadcast(gid, out)
            elif self.deliver(to, out):
                self.relayed += 1
                self.service.stats.count("relayed")

    @staticmethod
    def game_id(request):
        gid = request.match_info["id"].lower()
        return bytes.fromhex(gid) if re.fullmatch(r"[0-9a-f]{12}", gid) else None

    def address(self, ip, port=0):
        try:
            return socket.inet_aton(ip) + struct.pack(">H", port)
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
        if not old:
            self.sessions += 1
        self.service.stats.relay_games(len(self.games), came_on=not old)
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
                self.service.stats.relay_games(len(self.games))
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
                        self.broadcast(gid, out)
                    elif self.deliver(to, out):
                        self.relayed += 1
                        self.service.stats.count("relayed")
        except (asyncio.IncompleteReadError, ConnectionResetError, asyncio.CancelledError):
            pass
        return web.Response(status=200)

    def routes(self, app, base):
        app.router.add_get(base + "/api/relay/{id}/udp", self.udp_ticket)
        app.router.add_get(base + "/api/relay/{id}", self.download)
        app.router.add_post(base + "/api/relay/{id}", self.upload)
        app.cleanup_ctx.append(self.start_udp)
