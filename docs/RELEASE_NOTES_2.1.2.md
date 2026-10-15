💬 **Join the Discord:** https://discord.com/invite/QhGSuDKHwk

**Download:** `SvR2011-PC-v2.1.2.zip` for PC (Windows 8.1 or newer, Linux / Steam Deck through Proton) - or `SvR2011-Android-v2.1.2.apk` on its own for Android. `SvR2011-Mods-v2.1.2.zip` holds the bundled mods - the Android app downloads it by itself. On 2.1.0 or 2.1.1? The launcher and the Android app update themselves.

⚠️ **Only download the port from this GitHub page.** It's free. If someone sends you to Telegram or asks you to pay, it isn't us.

🧪 = experimental: it works in our tests but hasn't had wide testing on real hardware yet - reports welcome.

## 🛠️ Fixes
- **Mod moves now play in matches if you have DLC Vol. 2 / 3.** The DLC brings its own copies of the game's move animation files, so ported moves - and the bundled superstars' own moves (Sabu, The Sandman ...) - were missing in matches for most players. The port now builds merged copies beside your mods and points the game at them; your DLC files are never changed.
- **Bundled superstars disappearing after an update** (still in the Mods list, gone from the game, or showing up as someone else, e.g. Jeff Hardy as a second Million Dollar Man) - fixed. If a file was in use during the update the old copy got half-deleted. The launcher now swaps mods in safely and tells you which program was in the way. **Mods already broken this way repair themselves** the next time you start the launcher. The Android app replaces mods the same safe way.
- **Online: "Your mods don't match"** - sessions no longer get hidden because you and the other player have different match types switched on or off. In an online match both games play the host's PORT RULES pick.
- **Mod Maker: "Could not write into ...\Mods\... (is the game running?)"** when the game wasn't running - fixed (ported superstars with a moves folder, arenas with VS screen pictures). A failed install now leaves your installed copy as it was and says what actually went wrong.
- **A superstar mod's CPU only reversing moves** (e.g. Jillian) - fixed: moves whose motion files are missing from the mod are skipped, so the CPU uses the base character's move instead.
- **Asuka (ported from WWE 2K17) crashing or freezing on NOW LOADING** - fixed in the converter: superstars with moving hair could be too big for the game (their blood overlays are now left out), and WWE 2K16 / 2K17 superstars with two or more attires had their second attire mixed up with the first.
  ⚠️ **If you ported superstars from WWE '12 - 2K17 with 2.1.0 or 2.1.1, convert them again with this Mod Maker** - the list is [here](https://github.com/KaikoClanworth1/wwe-svr2011-pc/blob/main/docs/HAIR_RECONVERT.md). The Mod Maker's mod doctor marks the ones that need it.
- **PS2 superstars (HCTP, SvR 2005 - 2010) had wrong texture colours** - green / yellow patches (SvR 2006 / 2007 Kane), shifted colours, a white strip on 2007 heads, and a forehead bandage all match. Fixed - ⚠️ **convert PS2 superstars again** to get the right colours. Superstars with no picture in their game now get one drawn from their model.
- **The WWE Universe welcome screen showing at every start** - fixed: accepting it is saved right away. If your Saves folder can't be written (read-only, Windows' Controlled folder access, OneDrive), the launcher's Saves tab now warns you instead of the game silently keeping nothing.
- **Royal Rumble entrants with custom music** no longer start a random song from your Music folder that never stops.
- 🧪 **Android: the left stick turning into the D-pad** at random (until you restarted) - fixed.
- 🧪 **Android on Mali GPUs: the game closing right after it starts** - fixed. The launcher always shows, and the game switches Mali mode on by itself.
- 🧪 **Android: vibration on handhelds** (AYN, Retroid, Odin ...) - their built-in controllers have no motor of their own, so the device vibrates instead (Settings > Controls).
- **Start-up hangs**: the game's log now notes where it's stuck, so reports about a game that never gets past loading can be tracked down.

## 🌍 WWE Universe
- **Superstar mods are part of WWE Universe**: they're booked in matches, get rivalries and allies, and can hold titles - up to 137 mods beside Universe's own 150. Their Universe data is kept in its own file beside your save.

## 🧩 Mod Maker
- **A simpler sidebar** - Superstar, Arena, Moves & entrances, Media, Match types - with tabs, and **My game discs** as the one place your discs are set.
- **Simple menus for porting**, plain words, step by step:
  - **Make an arena step by step** - start from a 2011 arena, another game's arena or an empty hall, then aprons and screens, crowd signs, ring and lights, loading screens.
  - **Port an entrance**, **Port moves**, **Port taunts** - pick the game, the superstar and what you want. A ported entrance shows in CREATE AN ENTRANCE as "MOD: <name>"; ported moves show in CREATE A MOVE-SET.
  - **Simple superstar: from another game** - the game's superstars as a list with their pictures, and every step (entrance, moves, taunts, attires, music, pictures, extras) can come from another game too. "Bring everything too" brings it all, with progress and a Cancel button.
- **No disc needed** - every port takes one file (a character, arena or entrance file) or a folder of files copied off a disc.
- **Save as a mod file (.svrmod)** at the end of every porting menu, for sharing or installing with the launcher.
- **Every arena of WWE '12 - 2K17** in a grid of pictures with their real names.
- **Full EPAC support in the pac editor** - open any pac from SvR 2007 to WWE 2K17, including the lists inside it; preview textures and models; export, replace, add, remove and rename; save in the game's own format.
- **The mod doctor speaks plain English and has Fix buttons** that do the work for you. In Simple mode "Fix it" picks the best fix.
- **Report a problem...** (title bar) - describe what happened and the Mod Maker packs its logs, your open project and its tools check into one zip to send us.
- **Stage lights in the 3D views** - the arena editor and the Full preview light the arena with its own stage lights, beams and glows, like the game.
- PS2 taunts (HCTP, SYM, Just Bring It) are matched to 2011's taunts, faster ports with "move 7 of 33" progress, and lots of first-run fixes for new players.

## 🌍 Online
- **Lobby chat up to 300 characters** a message.
- 🧪 **Other players' Created Superstar pictures** show in the online lobby.

Everything new in 2.1.0 and 2.1.1 is in the [2.1.0 notes](https://github.com/KaikoClanworth1/wwe-svr2011-pc/releases/tag/v2.1.0) and the [2.1.1 notes](https://github.com/KaikoClanworth1/wwe-svr2011-pc/releases/tag/v2.1.1).

☕ Enjoying the port? **Support me on Ko-fi:** https://ko-fi.com/kaikoclanworth

💬 **Join the Discord:** https://discord.com/invite/QhGSuDKHwk
