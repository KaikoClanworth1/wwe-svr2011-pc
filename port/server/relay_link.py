"""
What the Community Creations server (relay.py, "home") and a relay edge
(relay_edge.py, on the VPS that takes the players' UDP) share: the signed UDP
tickets and the link between the two. No aiohttp: the edge needs only Python.

Why an edge: with the server on a PC behind a VPS, every relayed packet went
player -> VPS -> PC -> VPS -> player; the edge relays UDP players on the VPS
itself (player -> VPS -> player) while accounts, storage and the https
relay streams stay on the PC.

Tickets: home signs them with the shared secret, the edge checks them - it
never needs the accounts. 16 bytes, as the game sends them:
    expiry[4] (unix seconds, big-endian) + HMAC-SHA256(secret, "ticket" + id[6] + expiry[4])[:12]

The link: UDP datagrams over the private tunnel, home -> edge first (the
edge answers wherever home's last good datagram came from), each
    "SVRL" kind[1] body  HMAC-SHA256(secret, "link" + kind + body)[:16]
    1 state   home -> edge: nothing (a keepalive, every second)
              edge -> home: relayed[4] (since the last), n[2], n x id[6]
                            (its UDP games now; also at once when one comes on)
    2 forward to[6] packet   deliver `packet` (a relay "from", already built) to game `to`
    3 search  from[6] packet deliver to every game there but `from`
A forward the edge can't deliver (not one of its UDP games) goes back to
home, which delivers on the https download only - a packet never loops.
"""
import hashlib
import hmac
import struct
import time

LINK_MAGIC = b"SVRL"
STATE, FORWARD, SEARCH = 1, 2, 3
MAC = 16
TICKET_LIFE = 3600


def read_secret(path):
    with open(path, "rb") as f:
        secret = f.read().strip()
    if len(secret) < 16:
        raise SystemExit("relay secret %s: too short (at least 16 characters)" % path)
    return secret


def make_ticket(secret, gid, now=None):
    expiry = struct.pack(">I", int((now or time.time()) + TICKET_LIFE))
    return expiry + hmac.new(secret, b"ticket" + gid + expiry, hashlib.sha256).digest()[:12]


def check_ticket(secret, gid, ticket, now=None):
    if len(ticket) != 16:
        return False
    expiry = ticket[:4]
    want = hmac.new(secret, b"ticket" + gid + expiry, hashlib.sha256).digest()[:12]
    return hmac.compare_digest(want, ticket[4:]) and struct.unpack(">I", expiry)[0] >= (now or time.time())


def link_packet(secret, kind, body):
    head = LINK_MAGIC + bytes([kind])
    return head + body + hmac.new(secret, b"link" + bytes([kind]) + body, hashlib.sha256).digest()[:MAC]


def read_link(secret, data):
    """(kind, body) of a good link datagram, else None."""
    if len(data) < 5 + MAC or data[:4] != LINK_MAGIC:
        return None
    kind, body, mac = data[4], data[5:-MAC], data[-MAC:]
    if not hmac.compare_digest(mac, hmac.new(secret, b"link" + bytes([kind]) + body, hashlib.sha256).digest()[:MAC]):
        return None
    return kind, body


def state_body(relayed, ids):
    ids = list(ids)[:4096]
    return struct.pack(">IH", relayed, len(ids)) + b"".join(ids)


def read_state(body):
    if len(body) < 6:
        return 0, []
    relayed, n = struct.unpack(">IH", body[:6])
    return relayed, [body[6 + 6 * i:12 + 6 * i] for i in range(n) if 12 + 6 * i <= len(body)]
