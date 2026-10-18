# Online service: moving it to the VPS (plan)

Research and plan only - nothing on the VPS, firewall, WireGuard or the live
service has been changed. Steps marked **(user)** need the user (VPS access,
provider panel, DNS, the go for downtime).

## 1. What runs where today

| Piece | Where | How |
|---|---|---|
| Game service (`server.py`, aiohttp) | user's PC, 127.0.0.1:8101 | started by Project Index (`Apps\SvR2011\service\service.py` + `config.json`), portal's shared venv (Python 3.11, aiohttp 3.14). `server_dir` = the **main checkout's** `port\server` |
| Public entry `https://sho-ti.me/svr/...` | VPS 152.53.32.77 (Fedora 43, nginx 1.30, Let's Encrypt) | nginx site `project-index` -> `proxy_pass 10.8.0.2:8090` (portal) over WireGuard -> portal proxies `/svr/*` to 127.0.0.1:8101. Streaming settings: `proxy_buffering off`, `proxy_request_buffering off`, `proxy_read_timeout 14400s`, `client_max_body_size 0` |
| GameSpy emulation | PC, inside `server.py` (`gamespy.py`) | **all HTTP** under `/svr/game/...`: AuthService, Sake storage (SOAP), Sake upload/download, CompetitionService. No GameSpy TCP ports: GP presence (gpcm/gpsp) is answered by the game's own local relay (`online_net.cpp`) from `/api/session` |
| Accounts, friends, invites, presence, stats, logos, media, entrances, netreports | PC, `server.py` routes under `/svr/api/...` | |
| Leaderboards | PC, `leaderboards.py` (+ `leaderboards.json`, rows in `lb_rows`) | |
| Match relay, https half | PC, `relay.py` (`GET/POST /svr/api/relay/{id}` long streams) | through nginx + WireGuard + portal |
| Match relay, UDP half | **already on the VPS**: `relay_edge.py`, systemd `svr-relay-edge` (`Restart=always`, `DynamicUser`, secret via `LoadCredential`), public **UDP 36500** | linked to the PC service over WireGuard `10.8.0.1:36501` (signed tickets) |
| P2P game traffic | player to player (UDP, base 36000 + offsets) | not the server; relayed only when direct fails |
| Data | PC, `F:\ClaudeProjects\Project Index\Apps\SvR2011\data\` | `community.db` (SQLite WAL, ~11 MB + 4 MB wal: accounts, sessions, friends, Sake records/files, blobs index, logos, media, entrances, stats, leaderboards), `blobs\` (~1,190 files, ~400 MB, lzma, by sha), `server.log` (~110 MB, never rotated, has IPs) |
| Logs | PC | `data\server.log`; portal `logs\svr2011.out/err.log` |
| Dashboard / admin | PC, `/` and `/api/admin/*`, reached via the portal's `/apps/svr2011` (admin login) | `is_admin` trusts portal headers from loopback; **a direct loopback request without `X-Forwarded-For` counts as admin** |
| Media transcoding | PC, ffmpeg/ffprobe (PATH or `C:\ffmpeg\bin`) | optional: without it files are stored as sent |

Start / restart today: the portal's Scheduled Task runs `start-portal.ps1` 30 s
after the user logs on; nothing respawns the service if it dies. Health:
`/__health`, portal admin page, `watch-uptime.ps1`. Backups of `data\`: none
(only a one-off `.lb-reset` copy of the db).

Secrets (locations only): `Apps\SvR2011\service\relay_secret.txt` (= the edge's
`/opt/svr-relay/secret.txt`), the AuthService signing key in `gamespy.py`
(matches the modulus patched into the game - must not change), portal config
and certs (not needed on the VPS).

Memory-only state lost at any restart or move: open relay streams and UDP
games, pending invites (`self.invites`), presence (`stats.recent`), the
current minute's counters, login throttles, the blob cache. Accounts, tokens,
GP tickets, uploads and leaderboards are on disk and survive.

## 2. Target: a split - the VPS runs the server, the PC keeps only the files

The user's rule: *the server for connections runs on the VPS; players reach
the user's storage only when they download from Community Creations.*

```
players --https--> VPS nginx (sho-ti.me) --/svr/--> core (server.py --role core, 127.0.0.1:8101)
players --UDP 36500--> relay edge (as today) --127.0.0.1:36501--> core
core --(only file bytes, over WireGuard)--> PC storage node (server.py --role storage, 10.8.0.2:8102)
```

Players only ever talk to the VPS (one server URL, `https://sho-ti.me/svr`, as
today). The game and the launcher need no change: the split is inside the
server.

### What runs where

| On the VPS (core: `--role core`) | On the PC (storage: `--role storage`) |
|---|---|
| sign-in, accounts, tokens (`/api/register|login|logout|session`) | the bytes of uploaded files: CAWs, logos, stories, reels, move-sets, entrance songs / movies (`data\blobs\`) |
| presence, friends, invites (`/api/friends`, `/api/invite`) | |
| live session reports, netreports, stats, the dashboard | |
| the relay (https half; the UDP edge is there already) | |
| leaderboards, CompetitionService, AuthService | |
| Sake **metadata**: SearchForRecords, GetSpecificRecords, GetRecordCount, Create / Update / DeleteRecord, RateRecord (records, fields, ratings, owners) | |
| `community.db` (all of the above + the blob index: sha, sizes, refs, quotas) | |
| the 64 MB cache of recently downloaded files | |

**Pure storage calls** (the only ones that reach the PC, through the core):
Sake `upload.aspx` / `download.aspx` (file bytes), `/api/logo/<hash>` GET /
POST (logo bytes), `/api/media` and the entrance song / movie reads (bytes),
the dashboard's upload pictures. Everything else is metadata in the core's
database. (`/api/logos/wanted` is a list from the database: core.)

### How it stays consistent

- The core is the only writer of `community.db`; the storage node is a dumb,
  content-addressed store (`/blob/<sha>`, a shared key, `X-Storage-Key`).
- An upload: the core compresses, sends the bytes to the node, **then** writes
  the rows - with the node down the upload is refused and nothing is written.
- A delete / the hourly garbage collection: the core drops the refs and asks
  the node to delete. A delete missed while the node was off leaves an orphan
  file on the PC (harmless; a later sweep can list the node's shas against the
  core's blob table).
- Downloads: from the core's cache when recent, else from the node.

### When the PC is off

Sign-in, presence, friends, invites, matchmaking, the relay, leaderboards,
Community Creations **lists and details** keep working. Downloading or
uploading a file fails: the game shows its own "Download failed." (and the
upload error). Server-side test: with the node stopped, the 20 non-file
checks of `test_server.py` pass and the file ones fail cleanly.

### Server code (ready: commit 3c38571)

`server.py --role all|core|storage` (`all` = today), `--storage-url`,
`--storage-key`; `gamespy.Store` takes a blob backend (`LocalBlobs` /
`RemoteBlobs`).

### The VPS service

- `/opt/svr2011/` (the repo's `port/server`, a tagged version), its own venv
  (`python3 -m venv /opt/svr2011/venv; pip install aiohttp`), data in
  `/var/lib/svr2011/` (only `community.db*` - no blobs), logs in
  `/var/log/svr2011/`.
- systemd `svr2011.service`: `ExecStart=/opt/svr2011/venv/bin/python
  server.py --supervise --role core --host 127.0.0.1 --port 8101 --base /svr
  --data /var/lib/svr2011 --log /var/log/svr2011/server.log
  --relay-udp-port 36500 --relay-edge 127.0.0.1:36501 --relay-secret
  /etc/svr2011/relay_secret.txt --storage-url http://10.8.0.2:8102
  --storage-key /etc/svr2011/storage_key.txt`; `Restart=always`, its own
  user, `StateDirectory` / `LogsDirectory`, `ProtectSystem=strict`.
  (Settings - max connections, presence window, mods online - in
  `/var/lib/svr2011/settings.json`, edited from the dashboard.)
- nginx `location /svr/ { proxy_pass http://127.0.0.1:8101; }` before the
  portal's catch-all, with the streaming settings of today
  (`proxy_buffering off; proxy_request_buffering off; proxy_read_timeout
  14400s; client_max_body_size 0; proxy_http_version 1.1`), and
  `proxy_set_header X-Forwarded-For $remote_addr; proxy_set_header
  X-Portal-Role ""; proxy_set_header X-Portal-Public "";` (a client must not
  forge the admin headers). The dashboard: through an SSH tunnel to
  127.0.0.1:8101 (or a separate admin location with its own login).
- The relay edge: its link moves from 10.8.0.1:36501 to 127.0.0.1:36501.
- SELinux: `setsebool -P httpd_can_network_connect 1` if not already on.
- Backups (when the user picks them): `sqlite3 community.db ".backup ..."`
  nightly on the VPS (the db is small); the blobs stay on the PC.

### The PC's storage node

- Project Index runs `server.py --supervise --role storage --host 10.8.0.2
  --port 8102 --data <Apps\SvR2011\data> --storage-key <...>` instead of
  today's service (it serves `data\blobs\` as it is: no file moves).
- Windows firewall: TCP 8102 from 10.8.0.1 only (the tunnel).

## 3. Cut-over

Preparation (no downtime):
1. **(user)** VPS: the directories, venv, unit (disabled), nginx location
   (not active), the relay secret and a new storage key in `/etc/svr2011/`.
2. **(user)** PC: the same storage key file; firewall rule for 8102 from the
   tunnel.
3. Dry run: start a storage node on the PC (port 8103) and a core on the VPS
   (port 8102, a copy of the db) and run `test_server.py` / `test_friends.py`
   through an SSH tunnel; stop both, delete the copy.

Switch (a short downtime at a quiet hour; P2P matches in progress keep
playing):
1. Announce it.
2. Stop the PC service (Project Index). The data is frozen.
3. Copy `community.db` (after the clean stop: with its `-wal`, or
   `sqlite3 .backup`) to `/var/lib/svr2011/`. ~15 MB.
4. Start the PC's storage node (Project Index, `--role storage`).
5. Start `svr2011.service` on the VPS; check `/__health`, "relay: edge
   linked", one download from the dashboard.
6. Reload nginx with the `/svr/` location.
7. Smoke test from outside: launcher sign-in, ONLINE menu ("signed in as"),
   a Community Creations list and one download, a relayed test match.

Rollback (first week): stop the VPS unit, remove the nginx location (the
portal path takes over), copy the VPS `community.db` back to the PC (files
uploaded meanwhile are already on the PC: the node stored them), start the PC
service with `--role all`.

## 4. What the user must do

- VPS (SSH, sudo): directories, venv, unit, nginx location, SELinux boolean,
  the key files; later the backup timer.
- PC: the storage key file, the firewall rule for TCP 8102 from 10.8.0.1, the
  Project Index service line `--role storage`.
- The go for the switch time.
- DNS and the provider firewall: nothing new (sho-ti.me points at the VPS;
  443 and UDP 36500 are open already; 8101 / 8102 / 36501 stay private).

## 5. Capacity

- HTTP: small requests (presence every 20 s per game, Sake, leaderboards),
  uploads up to 8 MB (2 MB parts). The VPS gets the CPU / disk load of the
  PC today; SQLite is fine at this size.
- Match traffic is P2P: each game sends its pad inputs to **every** other
  game in the session (a full mesh, see below). The server only carries a
  pair's traffic when they can't reach each other directly (relay, via the
  edge's UDP 36500 - already on the VPS, so the relay load doesn't change with
  this move). See section 6 for a 5+ player Royal Rumble.

## 6. Royal Rumble with 5+ players: the crash and "one leaves, everyone is kicked"

From the code (port/src and the recompiled gss netplay); a crash report from a
5+ player Rumble is still needed for the crash itself.

### 6.1 Traffic: P2P full mesh, not the server

- Every game sends its pad input (gss message 16) to **every** other game,
  every frame (`p2p_send_every_frame`, on by default): 60 x (N-1) packets/s
  per game, plus 1-2 pings/s per peer. With a ~50 byte gss payload and kDataV2
  repeats (packets up to ~230 B + 28 B UDP/IP): **N=5 about 240 packets/s and
  ~0.5 Mbit/s each way per player; N=6 about 300 packets/s, ~0.6 Mbit/s.**
- Each pair goes direct when a direct ping came back in the last 3 s, else
  through the relay (UDP 36500 on the VPS edge, or the https pipe - TCP with
  head-of-line blocking - when UDP doesn't answer). A pair on the relay costs
  the VPS 60 x 2 packets/s; a whole N=6 Rumble forced through the relay
  ~1,800 packets/s (~2-4 Mbit/s): fine for the edge, but every relayed pair
  adds ~one VPS round trip of input lead.
- The online lobby has at most **6 slots** (`SlotCount`, online_cpu.cpp;
  gss's net table has 6 entries) - a 7-12 player online Rumble isn't possible
  today.
- So the VPS move doesn't change Rumble traffic (the UDP relay is already on
  the VPS); the HTTP service moving there doesn't carry match traffic.

### 6.2 Why one player leaving kicks everyone

- **The port tells the game nothing.** `p2p.cpp` never removes a peer: no
  "lost" detection, no socket error or QoS result handed to the game;
  XSessionLeave only updates member sets. The game only finds out through gss's
  own transport timeout (set from `sub_829B3768`: +744 = 15000 ms in a match).
- Lockstep waits for every peer's input: **everyone freezes ~15 s**. Then
  gss's "member left" callback `sub_828AA458` runs:
  - a non-host: `sub_82892A48` ("%s has left and will be replaced by AI"),
    then the per-frame `sub_828937E0` turns that character into a CPU
    (`sub_828DB6B0(ch, 1)`). So a non-host leaving *can* survive - after the
    freeze - **but each game times out on its own**, after a different number
    of the leaver's inputs (gss has no frame numbers, each game has its own
    input lead): the takeover happens on different frames, the games silently
    **desync** (positions/results differ, later "connection" errors);
  - **the host** (gss session owner): `sub_828AA458` sets the "session lost"
    flag (`*(*0x82EDE930)+5500`) - **everyone is disconnected**. That's the
    original game's design: no host migration.
- **The 64-proxy cap** (fixed to 512 in 4e81721): proxies (one per peer port
  ever played with) are never freed; at 64 new peers' packets were dropped
  silently -> that peer looked frozen -> 15 s freeze -> "left" for everyone,
  or a full kick if it was the host. A long evening of Rumbles (many different
  players) hits this first. A likely part of the report.
- Also fixed (c9019c3): pings went to every peer ever met; now only to peers
  of the current session.

### 6.3 The crash with 5+ players

Not found in the code: every per-slot array checked is sized 6 and indexed
< 6 (online_cpu.cpp `g_vacant/g_stand_in/g_cards`, net_stats `kMaxEntries`,
the sync-point hooks; the "left" message buffer fits 5 entries). Candidates:
the post-drop desync above, the proxy cap (a "frozen" peer then a teardown),
a kDataV2 packet over 96 B (Rumble packets batch more messages; those go
without repeats, so a lost packet freezes the game until gss resends). **Need:
the report zip (logs + crash_*.txt) of every player of one crashed Rumble.**

### 6.4 Proposed: a dropped player becomes a CPU on the same frame everywhere

1. **Detect** (p2p.cpp, PingLoop): in a match, a peer silent (no game data, no
   ping) for 3 s is *lost* (gss's own 15 s timeout stays as a backstop).
2. **Agree** (new message kPeerLost: the lost peer's MAC + how many of its
   inputs this game delivered to its own game): every survivor broadcasts it;
   the agreed count = the highest. A survivor that has inputs the others lack
   forwards them (each game keeps the last ~2 s of every peer's delivered
   input packets in OnDataV2) - so every game can reach the same count.
3. **Take over**: hook the gss input path (`sub_82893E18` / `sub_82895078`):
   when that peer's delivered inputs reach the agreed count, call
   `sub_82892A48(N, player)` ourselves and make `sub_8288D468` answer 0 for it,
   so `sub_828937E0` makes it a CPU on the same input frame on every game.
4. **Guard** `sub_82892A48` against a second call for the same entry (entry+32
   set), for gss's own later timeout.
5. AI determinism: the CPU uses the shared RNG (seed index 0 online,
   `sub_821C6BE0`) - already the case for lobby CPUs (online_cpu.cpp), which
   play in sync.

**Host leaving**: two levels.
- **(a) Clean end with a message** (small): hook `sub_828AA458`; when the host
  leaves during a match, show "the host left - the match ended" and return
  everyone to the online menu without the generic disconnect, keeping the
  P2P layer up; results not recorded.
- **(b) Keep playing** (big, risky): treat the host like any peer in a match
  (only the `sub_82892A48` part, not the +5500 flag), keep gss's session state
  from tearing down (`sub_829AB498` / `sub_829AA3A8` set +796 = 0, which
  ends the match via `sub_82893268`), then elect a new session owner after the
  match (lowest MAC) for the lobby. Needs gss session-owner internals we don't
  know yet.

### 6.5 Cost

| Item | Size | Risk |
|---|---|---|
| Proxy cap 512 + log | done (4e81721) | none |
| Ping only current peers + presence fixes | done (c9019c3), game test pending | low |
| Free idle proxies (thread stop, socket close, reuse) | small | low |
| Lost-peer detection + kPeerLost agreement + input forwarding | medium | medium |
| Same-frame CPU takeover hooks + guard | large (incl. RE of the gss input queue) | medium-high |
| Host leaves: clean end with message (a) | small-medium | low-medium |
| Host leaves: keep playing / host migration (b) | very large | high |
| Test kit: 5-6 games (4 test games max at once on this PC: needs a second PC or the phone/tablet) | medium | - |

Order: free idle proxies, then the crash report analysis, then detection +
agreement + takeover (non-host), then (a). (b) only if players ask after (a).

## 7. Risks

- **The AuthService key / patched modulus**: the server must keep the same
  `gamespy.py` key, or every game's login breaks. The move copies code as is.
- **Data copy consistency**: copy the db only after the PC service stopped
  (WAL), or with `sqlite3 .backup`.
- **Admin exposure**: fixed (9e892b4: loopback without the portal's headers
  is no admin); nginx must still clear client-sent X-Portal-* headers.
- **The PC as storage**: its line, its uptime and the tunnel decide whether
  downloads work; online play doesn't depend on it.
- **Two copies of the server code** (main checkout vs worktrees): deploy from
  one tagged version.
- **SELinux / firewalld surprises** on Fedora (proxy to a local port; the
  service binding 36501 on 127.0.0.1 is fine).
- **The VPS is a single host** with other services (git, Plex, game servers):
  CPU/RAM contention during busy evenings; the service is light, but blob
  downloads (~400 MB store) cost VPS bandwidth instead of the user's home line.
- **Memory-only state** (presence, invites, relay streams) resets at the
  switch: players see friends offline for up to a minute and must re-invite.
