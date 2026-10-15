<!-- DRAFT release notes for 2.1.2 (GitHub release text). No version bump yet. TODO before release: the items still in
     progress (Mod Maker "Save as .svrmod", moves from a folder of files) - add or drop; check the 🧪 marks. -->

💬 **Join the Discord:** https://discord.com/invite/QhGSuDKHwk

**Download:** `SvR2011-PC-v2.1.2.zip` for PC (Windows 8.1 or newer, Linux / Steam Deck through Proton) - or `SvR2011-Android-v2.1.2.apk` on its own for Android. `SvR2011-Mods-v2.1.2.zip` holds the bundled mods - the Android app downloads it by itself. On 2.1.0 or 2.1.1? The launcher and the Android app update themselves.

⚠️ **Only download the port from this GitHub page.** It's free. If someone sends you to Telegram or asks you to pay, it isn't us.

🧪 = experimental: it works in our tests but hasn't had wide testing on real hardware yet - reports welcome.

## 🛠️ Fixes
- **Online: "Your mods don't match"** - sessions no longer get hidden because you and the other player have different match types switched on or off. In an online match both games play the host's PORT RULES pick.
- **Mod Maker: "Could not write into ...\Mods\Superstars\... (is the game running?)"** when the game wasn't running - fixed. Ported superstars (mods with a moves folder) couldn't install. A failed install now leaves your installed copy as it was, and the message says what actually went wrong (which program has a file open, access denied, path too long, drive full).
- **Bundled superstars disappearing after an update** (still in the Mods list, gone from the game, some files missing) - fixed. If a file was in use during the update, the old copy got half-deleted. The launcher now swaps mods in safely and tells you which program was in the way. **Mods already broken this way repair themselves** the next time you start the launcher - no reinstall needed. The Android app replaces mods the same safe way.
- **A superstar mod's CPU only reversing moves and doing nothing else** (e.g. Jillian) - fixed. Moves whose motion files were missing from the mod are now skipped, so the CPU uses the base character's move instead.
- **Asuka (ported from WWE 2K17) crashing or freezing on NOW LOADING** - fixed in the converter. Superstars whose hair moves could be too big for the game's room (their blood overlays are now left out), and WWE 2K16 / 2K17 superstars with two or more attires had their second attire mixed up with the first (it never showed, or froze the game when picked).
  ⚠️ **If you ported superstars from WWE '12 - 2K17 since 2.1.0, convert them again with this Mod Maker** - the list is [here](https://github.com/KaikoClanworth1/wwe-svr2011-pc/blob/main/docs/HAIR_RECONVERT.md). The Mod Maker's mod doctor marks the ones that need it.
- 🧪 **Android: the left stick turning into the D-pad** at random (until you restarted) - fixed.
- **Start-up hangs**: the game's log now notes where it's stuck, so reports about a game that never gets past loading can be tracked down.

## 🧩 Mod Maker
- **Simple menus for porting**, plain words, step by step:
  - **My game discs** - point it at a folder once and it finds every supported game disc or extracted folder inside.
  - **Make an arena step by step** - start from a 2011 arena, another game's arena or an empty hall, then aprons and screens, crowd signs, ring and lights, loading screens, and try it.
  - **Port an entrance**, **Port moves**, **Port taunts** - pick the superstar, pick what you want, done. A ported entrance shows in CREATE AN ENTRANCE as "MOD: <name>".
  - **Simple superstar: From an older game** - the game's superstars as a list with their pictures. "Bring everything too" also brings their moves and taunts, with progress and a Cancel button.
- **Port from a single file** - one character, arena or entrance file from any supported game, no whole disc needed.
- **Every arena of WWE '12 - 2K17** in a grid of pictures with their real names.
- **Full EPAC support in the pac editor** - open any pac from SvR 2007 to WWE 2K17, including the lists inside it; preview textures and models; export, replace, add, remove and rename; save in the game's own format. It tells you plainly what a format can't do (e.g. WWE 2K17 entries can't be added).
- **The mod doctor speaks plain English and has Fix buttons** that do the work for you - e.g. port a superstar's missing entrance, or use a lighter copy of a model that's too big. In Simple mode "Fix it" picks the best fix.
- **Report a problem...** (title bar) - describe what happened and the Mod Maker packs its logs, your open project and its tools check into one zip to send us.
- **Stage lights in the 3D views** - the arena editor and the Full preview light the arena with its own stage lights, beams and glows, like the game.
- Faster ports (SvR 2007 - 2009 arenas in seconds), "move 7 of 33" progress, and lots of first-run fixes for new players.

## 🌍 Online
- **Lobby chat up to 300 characters** a message.
- 🧪 **Other players' Created Superstar pictures** show in the online lobby.

<!-- TODO: "Save as .svrmod" (Mod Maker) and moves from a folder of files - still in progress; add here if they make 2.1.2. -->

Everything new in 2.1.0 and 2.1.1 is in the [2.1.0 notes](https://github.com/KaikoClanworth1/wwe-svr2011-pc/releases/tag/v2.1.0) and the [2.1.1 notes](https://github.com/KaikoClanworth1/wwe-svr2011-pc/releases/tag/v2.1.1).

☕ Enjoying the port? **Support me on Ko-fi:** https://ko-fi.com/kaikoclanworth

💬 **Join the Discord:** https://discord.com/invite/QhGSuDKHwk
