# Online mode plan (draft, 30 September 2026)

Goal: the ONLINE menu works without "Online Axxess": P2P Player Matches (LAN and internet) and Community Creations against a self-hosted server.

## What the game uses
- **Player matches / lobbies / ranked / invites:** Xbox LIVE system services - XSession create/search/join/start/end (XMsg 0xB0010-0xB0065 to the XGI app), XNADDR addresses (XNetGetTitleXnAddr, XNetXnAddrToInAddr, XNetConnect), QoS latency probes (XNetQosListen/Lookup), then plain UDP between consoles (already P2P).
- **Community Creations and ranked results:** GameSpy SOAP/HTTP, dead since 2014:
  - `%s.sake.gamespy.com/SakeStorageServer/StorageServer.asmx` (records: create/search/rate/delete/my records)
  - `%s.sake.gamespy.com/SakeFileServer/upload.aspx` / `download.aspx` (files)
  - `%s.auth.pubsvs.gamespy.com/AuthService/AuthService.asmx` (LoginProfile, HTTPS)
  - `%s.comp.pubsvs.gamespy.com/CompetitionService/CompetitionService.asmx` (ranked reports)
  - GameSpy code in the image around sub_829EC068-sub_82A66B20. OpenSpy documents these services.
- **Why ONLINE is greyed today:** SDK sign-in state is always 1 (local, never 2 = LIVE); XamUserCheckPrivilege denies everything; Online Axxess = the ONLINE_AXXESS item of the Fan Axxess DLC key.
- SDK state: host sockets are real; XNet addressing, QoS, sessions, stats, storage are stubs/fakes; several REX_EXPORT_STUB imports return garbage in r3; WSARecvFrom returns -1; no HTTP client in the game process.

## Phases
1. **Access:** signed in to LIVE (Gold), all privileges, stable per-install XUID, "Online name" (launcher setting), Online Axxess always owned (hook). Checkpoint: ONLINE menu opens.
2. **Network foundation:** real XNADDR (IP:port), XnAddr<->InAddr table, XNetConnect/status, QoS, missing socket calls, fix garbage-returning stubs.
3. **Player Matches (P2P):** session directory with a LAN backend (UDP broadcast; two instances on one PC) and a server backend (small lobby server). Game traffic stays peer to peer.
4. **Internet play:** UPnP port mapping, UDP hole punching via the lobby server, relay fallback.
5. **Community Creations:** local stand-in for Sake storage/files + auth (files on disk); the port resolves `*.gamespy.com` to a configurable server; HTTPS login matched or patched to HTTP; later deployed to the self-hosted server (e.g. Docker).
6. **(Optional) Ranked + leaderboards:** CompetitionService reports + XUser stats to the server.

Cross-play PC / Steam Deck / Android works if versions match. Voice chat stays off.

## Open decisions
1. Server language/hosting (suggested: one Python or Go service in Docker for lobby + Community Creations).
2. Identity: online name + random ID, or real accounts?
3. First-release scope: Player Matches + Community Creations, or ranked/leaderboards too?
4. Community Creations moderation at launch?
5. Order: Phase 1 first, then P2P (2-4), then Community Creations (5)?
