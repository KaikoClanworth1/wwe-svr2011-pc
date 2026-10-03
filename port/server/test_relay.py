"""Checks the online match relay (relay.py) of a running server: three games
open their streams, forward to each other and search (broadcast); a game
without an account, or with someone else's game id, is refused. It makes
throwaway accounts: run it against a test server, not a public one.

    python test_relay.py [--server http://127.0.0.1:8411]
"""
import argparse
import asyncio
import secrets
import struct
import sys

import aiohttp

HDR = b"SVRR\x01"
REGISTER, WELCOME, FORWARD, FROM = 1, 2, 3, 4


def frame(data):
    return struct.pack(">H", len(data)) + data


class Game:
    def __init__(self, session, server, token, gid):
        self.session, self.server, self.token, self.gid = session, server, token, gid
        self.up = asyncio.Queue()
        self.got = asyncio.Queue()
        self.status = None

    def url(self):
        return "%s/api/relay/%s" % (self.server, self.gid.hex())

    async def open(self):
        headers = {"Authorization": "Bearer " + self.token}
        self.down = await self.session.get(self.url(), headers=headers)
        self.status = self.down.status
        if self.status != 200:
            return False
        asyncio.ensure_future(self.read())

        async def body():
            while True:
                data = await self.up.get()
                if data is None:
                    return
                yield frame(data)
        self.uploading = asyncio.ensure_future(self.session.post(self.url(), headers=headers, data=body()))
        self.send(HDR + bytes([REGISTER]) + self.gid)
        return True

    async def read(self):
        buf = b""
        try:
            async for chunk in self.down.content.iter_any():
                buf += chunk
                while len(buf) >= 2 and len(buf) >= 2 + struct.unpack(">H", buf[:2])[0]:
                    size = struct.unpack(">H", buf[:2])[0]
                    data, buf = buf[2:2 + size], buf[2 + size:]
                    if data:
                        await self.got.put(data)
        except aiohttp.ClientError:
            pass  # (closed at the end)

    def send(self, data):
        self.up.put_nowait(data)

    async def next(self, timeout=3):
        return await asyncio.wait_for(self.got.get(), timeout)


async def account(session, server, name):
    async with session.post(server + "/api/register", json={"name": name, "password": secrets.token_hex(8)}) as r:
        body = await r.json()
        return body.get("token")


async def main(server):
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok &= bool(cond)

    async with aiohttp.ClientSession() as session:
        tokens = [await account(session, server, "rly%s%d" % (secrets.token_hex(3), i)) for i in range(3)]
        check(all(tokens), "three throwaway accounts")
        games = [Game(session, server, t, bytes([2]) + secrets.token_bytes(5)) for t in tokens]
        for g in games:
            check(await g.open(), "game %s streams open" % g.gid.hex())
            welcome = await g.next()
            check(welcome[:6] == HDR + bytes([WELCOME]), "  it's welcomed (its address: %s)" %
                  ".".join(str(b) for b in welcome[6:10]))
        a, b, c = games

        a.send(HDR + bytes([FORWARD]) + b.gid + b"hello b")
        got = await b.next()
        check(got[:6] == HDR + bytes([FROM]) and got[12:] == b"hello b", "a -> b by id")

        a.send(HDR + bytes([FORWARD]) + b"\xff" * 6 + b"search")
        check((await b.next())[12:] == b"search" and (await c.next())[12:] == b"search", "a's search reaches b and c")
        try:
            await a.next(0.5)
            check(False, "the searcher doesn't get its own search")
        except asyncio.TimeoutError:
            check(True, "the searcher doesn't get its own search")

        # a burst, in order
        for i in range(200):
            c.send(HDR + bytes([FORWARD]) + a.gid + b"n%03d" % i)
        burst = [(await a.next())[12:] for _ in range(200)]
        check(burst == [b"n%03d" % i for i in range(200)], "200 packets c -> a, all in order")

        # refused: no account; someone else's game id
        async with session.get("%s/api/relay/%s" % (server, a.gid.hex())) as r:
            check(r.status == 401, "no account: refused (%d)" % r.status)
        thief = Game(session, server, tokens[1], a.gid)
        check(not await thief.open() and thief.status == 409, "someone else's game id: refused (%s)" % thief.status)
        async with session.get(server + "/api/relay/xyz", headers={"Authorization": "Bearer " + tokens[0]}) as r:
            check(r.status == 400, "a bad game id: refused (%d)" % r.status)
        for g in games:
            g.send(None)
            await asyncio.gather(g.uploading, return_exceptions=True)
            g.down.close()
    print("relay: all ok" if ok else "relay: FAILED")
    return ok


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="http://127.0.0.1:8411", help="its address, with its base (e.g. .../svr)")
    args = ap.parse_args()
    server = (args.server if "://" in args.server else "http://" + args.server).rstrip("/")
    sys.exit(0 if asyncio.run(main(server)) else 1)
