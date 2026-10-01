# Online mode plan (draft, 30 September 2026)

Goal: the ONLINE menu works without "Online Axxess": P2P Player Matches (LAN and internet) and Community Creations against a self-hosted server.

## Status (1 October 2026): Community Creations works
- **Server:** `port/server/server.py` (aiohttp; GameSpy Sake/Auth in `gamespy.py`).
  - Player accounts with open registration: PBKDF2 passwords and session tokens. Each account's XUID is 0x0009000100000000 + its id.
  - Uploads are compressed and de-duplicated (lzma, sha256). Limits: 100 requests at once, 8 MB per file, 256 MB per player.
  - An admin dashboard shows connections, bandwidth, storage, players, uploads and accounts.
  - Run on its own, it serves 127.0.0.1:8411 (`tools/online_server.ps1`, `server/test_server.py`).
  - The public one is https://sho-ti.me/svr: a Project Index app running it on port 8101 with base `/svr`.
- **Game (`src/online.cpp`, `src/online_net.cpp`):** the SDK sends the title server THQSVR11_GSGW's connections to a relay on this PC.
  - The relay answers GameSpy's presence login itself, from the account's session.
  - It forwards the game's HTTP to `<online_server>/game/...` with the player's `online_token`: WinHTTP on Windows, `NetBridge.java` on Android.
  - Downloads are validated, and the save they replace is backed up first. Superstars' extra Paint Tool logos travel with them.
- **Entrances:** a Superstar's .cas carries its whole profile record (0x6BC bytes at 0x1483C4), entrance included, so the game uploads the entrance itself.
  - The song travels by name.
  - The game resets a user movie (row 254, ids 700-899) to none on upload.
  - `src/entrance_media.cpp` sends the USER PLAYLIST song and the USER MOVIE (shrunk to H.264 MP4, `movie_transcode.cpp`) to `/api/media` and ties them to the Superstar with `PUT /api/entrance/<file>`.
  - On download it installs them (`Music\<name>\`, `Custom Movies\<name>.bik`, "<name> (2)" if the name plays something else). When the save job starts (sub_824DCA98) it points the downloaded entrance at them.
  - Android sends and installs songs; movies are Windows-only for now (Media Foundation).
- **Launchers:** the Online tab (PC and Android) signs in or creates an account, and picks the Default or a Custom server.
- **Menus:** the gamer card, party, Xbox LIVE and Online Axxess texts and prompts are gone.

## Key finding
Xenia Canary's netplay fork (AdrianCassar/xenia-canary, BSD 3-Clause, the same code family as our SDK) lists **SvR 2011 (5451085D) as "Working Public"** for player matches, with no game patches. Its server, **Xenia-WebServices** (NestJS + MongoDB, MIT, Docker), is the session directory. So Player Matches are a port of known-working code, not new reverse engineering. Neither implements GameSpy, so Community Creations are new work.

## What the game uses (research)

### Xbox LIVE (player matches, lobbies, ranked)
- XLAST/SPA (title 0x5451085D):
  - Contexts: GAME_TYPE 0x800A (RANKED=0, STANDARD=1); game modes NORMAL_MATCH / ROYAL_RUMBLE / CHAMPIONSHIP.
  - Matchmaking: one query, 0 "CUSTOM_MATCH" (25 results), with properties 0x1000000A-0x10000022 (match type, arena, DLC version, rating ...).
  - Stats: 33 arbitrated views (leaderboard properties RATED, FLAG, SUPERSTAR_RANK, MATCH/WIN/LOSS/DRAW/DQ/DISCONNECT).
  - Presence: "PLAYING Xbox LIVE", "PLAYER MATCH", "IN A MENU".
- XGI messages (XMsg app 0xFB): B0006/7 context/property, B0010 create, B0011 delete, B0012 join, **B0013 leave (unhandled today: X_E_FAIL)**, B0014/15 start/end, B0018 modify, B001A arbitration register, B001B/B001C/B0060 search, B001E migrate host, B0021 read stats, B0025 write stats.
- XLiveBase (app 0xFC): the game sends 58009, 5800E, 58019, 5801E, 58032, 58035, 58044. None of these is handled today; each returns X_E_FAIL.
- NetDll: BSD sockets (IPPROTO_VDP 254 mapped to UDP), XNetGetTitleXnAddr, XNetXnAddrToInAddr, XNetConnect/GetConnectStatus, XNetQos*, XNetDnsLookup, XNetServerToInAddr, XNetUnregisterInAddr, XNetGetEthernetLinkStatus. Not imported: XNetInAddrToXnAddr, XNetCreateKey/RegisterKey, WSARecvFrom.
- Others: XamSessionCreateHandle/RefObjByHandle, XamUserCheckPrivilege, XamUserGetSigninState, XamUserCreateStatsEnumerator, XamShowSigninUI / GameInviteUI / FriendsUI / PlayerReviewUI / MarketplaceUI, XamVoice*.

### GameSpy (Community Creations, ranked reports)
- **Library:** the Open GameSpy SDK is linked in (wrapper class `CGameSpyResSVR`). It does its own HTTP with ghttp ("GameSpyHTTP/1.0") over NetDll sockets, and DNS through XNetDnsLookup. There is no XHttp.
- **Identity:** gamename `svsr11x360`, secret key `4q9ULG`. 2886 is probably the gameid and 12190 the productid (unconfirmed). Accounts are `<XUID>@SVR11TEST.com` (AuthService LoginProfile).
- **Hosts:**
  - `http://svsr11x360.sake.gamespy.com/SakeStorageServer/StorageServer.asmx`
  - `.../SakeFileServer/upload.aspx?gameid=&pid=` and `download.aspx?fileid=&gameid=&pid=`
  - `https://svsr11x360.auth.pubsvs.gamespy.com/AuthService/AuthService.asmx`: the only HTTPS. It uses GameSpy's own SSL engine (SSLv3; the certificate chain is not verified). The login certificate must be signed by the RSA key embedded at 0x82087370, so a new server needs a patched modulus (a hook on our side).
  - `http://svsr11x360.comp.pubsvs.gamespy.com/CompetitionService/CompetitionService.asmx` (CreateSession, SetReportIntention, SubmitReport)
  - `gpcm.gamespy.com:29900` and `gpsp.gamespy.com:29901`: GP presence/search. Its role here is unknown; it may be unused.
- **Sake actions:** CreateRecord, UpdateRecord, DeleteRecord, SearchForRecords (SQL-like filters), GetMyRecords, GetSpecificRecords, GetRandomRecords, GetRecordCount, GetRecordLimit, RateRecord.
- **Sake tables:** UserContent, Comments, CASData, Newsfeed, ContentsRanking, USERSTATS_DL, Report.
- **Main fields:** ownerid, AuthorName, ContentType, Category, Flags, Version, Language, PTitle, ThumbData0-7, F00 (the file), Rating, Moderated, Deleted.
- **Categories:** SS, HR, EM, CAM, PL, CAF, WSD, CAS. These probably stand for Superstar, Highlight Reel, entrance movie, moveset, paint/logo, finisher, story and attire (unconfirmed).
- **No other servers:** there are no THQ, Yuke's, telemetry or ads hosts.

### SDK today (why nothing works)
- **Sign-in and privileges:** sign-in state is always 1 (local), never 2 (LIVE). XamUserCheckPrivilege denies every privilege, and it is the main blocker. Online Axxess is the ONLINE_AXXESS item of the Fan Axxess key.
- **Addressing:**
  - XNADDR is 127.0.0.1 with a fake MAC and no online flag.
  - XnAddrToInAddr fails.
  - XNetInAddrToString writes "666.666.666.666".
  - EthernetLinkStatus reports no cable.
  - DnsLookup always fails.
  - QosListen fails.
- **Garbage returns:** XNetConnect, GetConnectStatus, QosLookup, ServerToInAddr, UnregisterInAddr, getsockname, XNetLogonGetTitleID and XamUserCreateStatsEnumerator are REX_EXPORT_STUB, so r3 holds garbage.
- **XGI:** session messages only log and return success. They fill no results (search returns garbage) and no session info or nonce.
- **What works:** sockets are real host sockets. The SDK has no XNet security layer, which is fine because peers use plain UDP.
- **Port code:** port/src has no networking yet (only the "Xbox LIVE" to "Online" string patch).

## How Xenia netplay does it (reference design)
- **Startup:** `GET /whoami` returns the public IP. Then `POST /players` sends xuid, machineId, host IP, MAC and gamertag.
- **XNADDR:** `ina` is the LAN IP, `inaOnline` the public IP, `wPortOnline` 36000, and `abEnet` the MAC, so each MAC identifies a player.
  - XnAddrToInAddr returns the peer's real IP.
  - InAddrToXnAddr looks the MAC up in a cache filled from session JSON.
  - Traffic is plain UDP/TCP straight to the peer.
- **Sessions:** REST on `/title/:titleId/sessions`:
  - create, search (POST with filters), details, join/prejoin/leave, modify, migrate, arbitration
  - qos (raw bytes), context/properties (base64), leaderboards
- **NAT:** no hole punching and no relay. Players need UPnP (miniupnpc, opt-in) or manual port forwarding, and NAT is reported as OPEN.
- **Licenses:** client code BSD-3 (keep notices), server MIT, so both are reusable.

## Phases (revised)
1. **Access:** signed in to LIVE (state 2, Gold), every privilege granted, a stable random XUID per install, an "Online name" setting in the launcher, and Online Axxess always owned. Also make every REX_EXPORT_STUB the game imports return defined values.
   - Checkpoint: the ONLINE menu opens.
2. **Netplay core:** port Xenia netplay's XLiveAPI/XSession/XNet pieces into the SDK patch:
   - a real XNADDR, the XnAddr/InAddr/MAC tables, XNetConnect/status, QoS listen/lookup
   - XGI session create/search/join/leave(B0013)/start/end/modify/arbitration with real results
   - XLiveBase 58xxx replies
   - Config: `online_api` (server URL) and `online_mode` (off / LAN / internet).
3. **Session server:** run Xenia-WebServices (MIT, Docker) locally for tests; two game instances on one PC need distinct MACs/ports.
   - Choose between protocol-compatible (could share a server with Xenia players, since SvR 2011 is title 5451085D) and our own fork/rewrite.
   - A LAN backend (UDP broadcast, no server) is optional.
   - Checkpoint: two local instances play a Player Match.
4. **Internet:** UPnP (miniupnpc) as in Xenia; then what Xenia lacks:
   - the server records each client's UDP-observed port
   - simultaneous-probe hole punching (libjuice, MPL-2.0, or custom)
   - a UDP relay fallback for symmetric NAT (coturn BSD-3, or relay packets in our server). XNADDR can carry the relay address, so the game does not notice.
5. **Community Creations:** our own server for the GameSpy SOAP APIs. OpenSpy's webservices implements Auth/Sake/Comp, but it has **no license**: study the protocol only, do not copy code.
   - Redirect `*.gamespy.com` in XNetDnsLookup to the configured server.
   - AuthService: serve HTTPS with any certificate (the game does not verify it) or divert it to HTTP in-process. Replace the embedded RSA modulus at 0x82087370 with our key and sign login certificates with it.
   - Implement the 7 tables, the SQL-like filter subset the game sends, file upload/download (F00, thumbnails), ratings and GetRecordLimit.
   - First local (files on disk), then Docker on the self-hosted server.
6. **(Optional) Ranked and leaderboards:** XUserReadStats/WriteStats plus arbitration (33 views), and CompetitionService reports.

Cross-play PC / Steam Deck / Android works if versions match. Voice chat stays off (XamVoiceCreate denied, as today).

## Open decisions
1. **Server:** Xenia-WebServices as is (NestJS/MongoDB), protocol-compatible so Xenia players could join? Or one service of our own (Python/Go) for sessions + Community Creations?
2. **Identity:** online name + random ID, or real accounts?
3. **First-release scope:** Player Matches + Community Creations, or ranked/leaderboards too?
4. **Community Creations moderation at launch?** The game has Moderated/Report fields.
5. **Order:** Phase 1, then P2P (2-4), then Community Creations (5)?
