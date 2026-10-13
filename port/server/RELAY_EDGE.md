# The relay edge: online match UDP relayed on the VPS

Before: player -> VPS -> (tunnel) -> PC -> (tunnel) -> VPS -> player.
The PC <-> VPS round trip (~31 ms, measured 13 Oct 2026) was added to every
relayed packet.

After: between two UDP players, player -> VPS -> player. Only the small UDP
relay (`relay_edge.py`) runs on the VPS. It has no accounts, no files and no
database. Everything else stays on the PC: accounts, Community Creations
storage, leaderboards, the dashboard, logs, and the https relay streams that
games fall back to.

The game doesn't change. It already sends its relay UDP to the server's
address (sho-ti.me, which is the VPS) on the port the server tells it.

## How it fits together

- **Tickets:** a game asks the PC for a UDP ticket, the same as now. The PC
  checks the account and signs the ticket with a secret that only the PC and
  the VPS know. The edge checks the signature, and an hour later the ticket
  runs out.
- **The link:** the PC and the edge talk over the WireGuard tunnel (UDP, the
  edge's port 36501, every packet signed). The PC sends a keepalive every
  second. The edge answers with its list of UDP games and its packet count,
  so the dashboard still counts relayed packets.
- **Between a UDP player and an https (fallback) player:** packets cross the
  link, which is no worse than today.

## Setting it up (the user does this, or OKs each step)

1. **A secret.** On the PC, in the service folder:
   `python -c "import secrets; print(secrets.token_hex(32))" > relay_secret.txt`.
   Copy the same file to the VPS, for example `/opt/svr-relay/secret.txt`,
   with `chmod 600`.
2. **The edge on the VPS.** Copy `relay_edge.py` and `relay_link.py` to
   `/opt/svr-relay/`. It needs Python 3.8 or newer and nothing else. Then add
   a systemd unit:

       [Unit]
       Description=SvR2011 online relay edge
       After=network-online.target wg-quick@wg0.service
       [Service]
       ExecStart=/usr/bin/python3 /opt/svr-relay/relay_edge.py --udp-port 36500 --link 10.8.0.1:36501 --secret /opt/svr-relay/secret.txt
       DynamicUser=yes
       Restart=always
       [Install]
       WantedBy=multi-user.target

   `10.8.0.1` is the VPS's own address in the tunnel. Check it with
   `ip addr show wg0`. The link must never be on the public address.
3. **The VPS firewall.**
   - Remove the forward of UDP 36500 to 10.8.0.2. The masquerade stays if
     anything else uses it.
   - Open UDP 36500 to the public.
   - Allow UDP 36501 from the tunnel only (10.8.0.0/24).
4. **The PC server.** Add `--relay-edge 10.8.0.1:36501 --relay-secret relay_secret.txt`
   to its arguments, keep `--relay-udp-port 36500`, and restart it. Its log
   should show `relay: UDP on the edge ...` and then `relay: edge linked`.
   The PC firewall rule for inbound UDP 36500 is no longer needed: the link's
   replies come back to the connection the PC opened.
5. **Check.** Run `python test_relay.py --server https://sho-ti.me/svr`. It
   should report all ok, including the `udp: b (udp) -> c (udp)` lines.
   Delete the three throwaway accounts it prints. Once games have relayed,
   the edge's log (`journalctl -u svr-relay-edge`) shows
   `edge: relayed N here, ...`.

**Going back:** stop the edge, put the VPS forward back, remove the two
server arguments and restart the server.

## Tests

`python test_relay_edge.py` runs on any PC. It starts an edge and a server
on test ports (37650-37661) with a throwaway data folder, then checks:

- all of `test_relay.py` through the edge
- that packets between UDP games stayed on the edge
- that forged and expired tickets are refused, and that link packets
  without the secret are ignored
- that the server without an edge still works
