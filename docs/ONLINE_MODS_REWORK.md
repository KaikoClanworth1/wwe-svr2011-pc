# Online mods rework (brief, 27 Oct 2026)

Owner: the Online chat. Main merges. Work on your own branch and worktree, branched from main.

## What the user wants

When a player joins a session that uses mods:

1. The game checks which of the host's mods the joiner is missing, or has in a different version.
2. It asks: "This match uses N mods you don't have (X MB). Download them?" If the answer is No, the player doesn't join.
3. If the answer is Yes, it downloads them **directly from the host** (P2P only, no server copy and no relay for files: user's choice) into a temp folder. Then it enables them while the game loads. Show a progress bar at the bottom right, or reuse the Community Creations download screen.
4. The player then plays the match with those mods.
5. When the player leaves online mode, or closes the game, the temp folder is cleared. Also clear any leftovers at the next start, in case the game crashed.

Only allow safe files in a transfer: an **allow-list** of the game and mod file types (.pac, .bik, .mp4, .png, .dds, manifest/txt/toml, .svrmod parts...). Refuse .exe, .dll, .bat, .ps1, .iso and everything else. Check this on the host before sending, and on the joiner before writing. Also check:
- paths: no `..`, no absolute paths, nothing outside the temp folder;
- a size cap per mod and per session;
- CRCs.

## What exists today (read first)

- online_mods.cpp/.h: D15 per-lobby mod keys (folder\tversion\tCRC), id translation, refusal notes, OnlineModsMayStart. Memory: svr2011-online-mods.
- The "You have mods enabled, relaunch with Start without mods" block and the Mod Online setting. The rework replaces the block with the download offer.
- p2p.cpp: the transport (kMods = 14 message). It needs a chunked file stream with resume and back-pressure, so a big .bik doesn't stall the lockstep or the lobby pings.
- Community Creations download UI: the download screen and the CAW temp handling.
- Move packs, backstage and match types are currently reduced per lobby (331eda8, 8810d4a). Decide whether they join the download too.

## The hard part (a mod-loading rework)

Mods are found and mounted at start-up today: slots.txt, the VFS overlay, arena pages, move packs. To enable them mid-session we need:
- a second mod root, `<user data>\OnlineMods\<session>`, that the mod scan can add and remove at runtime, or a narrow "hot add" path for each kind (superstar slot, arena, entrance/media, move pack);
- slot ids for temp superstars that never collide with, or get saved over, the player's own mods (the virtual-slot or side-table work in svr2011-universe-limit may help);
- nothing from a temp mod written into saves, the CAW list, the Universe or Community Creations uploads;
- unloading cleanly on leave.

Research first: which mod kinds can be added at runtime, and which need a restart. Send Main a short plan before building. Per kind, it should say: add at runtime, or "rejoin needed".

## Rules

- Test games hidden and muted, with scripted input only; at most 4 across all chats.
- Use a private test server and --p2p_port 365xx+. Never use the live server or the VPS.
- Use a private test data copy, with no junctions to Game Files.
- Run the NDK syntax check before committing game code.
- Don't push. Main merges.
