💬 **Join the Discord:** https://discord.com/invite/QhGSuDKHwk

**Download:** `SvR2011-PC-v2.1.4.zip` for PC (Windows 8.1 or newer, Linux / Steam Deck through Proton) - or `SvR2011-Android-v2.1.4.apk` on its own for Android. `SvR2011-Mods-v2.1.4.zip` holds the bundled mods - the Android app downloads it by itself. On 2.1.x? The launcher and the Android app update themselves - **close the Mod Maker before updating**.

⚠️ **Only download the port from this GitHub page.** It's free. If someone sends you to Telegram or asks you to pay, it isn't us.

🧪 = experimental: it works in our tests but hasn't had wide testing on real hardware yet - reports welcome.

## 🚀 A new launcher
- **A new look**: a darker launcher with a simpler **Play** page - Play, **Start Without Mods** (the game loads no mods and leaves your mods and saves as they are) and the game's size. The details moved to their tabs.
- **One Setup**: Install, Setup, DLC and Android are one step-by-step **Setup** tab; the Mod Maker's tools are their own optional step.
- **Mods in three tabs**: **Installed Mods**, **Discovery** 🧪 (find, download and upload players' `.svrmod` mods, shared through the online server) and **Texture Packs**.
- **The launcher and the game agree on the settings**: full screen kinds and the render resolution are the game's own keys now (no more 4K in the launcher and 1080p in the game).
- **Saves import**: exported saves now work in a fresh install.
- **Updates check every file**: a separate **SvR2011 Updater** installs each update, checks every file against the release's list and copies any bad one again. If files go missing later, the launcher fetches just those (from the kept update, or from GitHub).

## 🌐 Online
- **The player count** on the online screen now counts players who are actually online, not everyone with the game open.
- **Matchmaking, host-left notice, mods / DLC mismatch messages**, and a fallback when the online port is already in use.
- **News**: the ONLINE screen's NEWS panel can now be changed by us at any time (it shows on your next visit).
- **More reliable matches**: the server can switch every match to direct connections or to the relay when one of them is having trouble.

## 🛠️ Fixes
- **Crash when saving a Created Superstar** with the Direct3D 11 graphics setting - fixed.
- **Road to WrestleMania** (Chris Jericho, weeks 6 and 12) crashes - fixed.
- **Moves ported from WWE '13 / 2K14 froze** in the grab pose and slid along (and sank into the ring) - fixed. Superstars ported with those moves get the fix when you port them again.
- **Pins with no kick-out**, the **Three Stages of Hell First Blood** freeze and **fast replays / stuck at 30 fps** - fixed.
- **Mixed matches**: no more DQ for a man hitting a diva, holds can win, and the CPU fights either gender.
- **Some custom arenas crashed** at the entrance or as the match loaded (e.g. ported 2K arenas) - fixed.
- **Superstar mods with no usable moveset** now get a stock superstar's moveset of the same gender, so nobody stands still.
- **Two mods (or a mod and your own song) with the same theme name** no longer swap themes.
- **Mod Maker**: HCTP backstage ports no longer say "missing: hctp2jboy" when nothing is missing; SvR 2008 Carlito converts again; WWE 2K16 superstars keep all their attires.
- **"The game's graphics can't run"** now says when the cause is missing files.
- Problem reports now carry more of what we need (the game's logs, crash notes, the mods' text files, a CAW save summary), so you won't be asked for your files.

## ✨ New
- **Free camera / photo mode**: F6 fly, F5 back, F7 hide the HUD, F8 pause (offline).
- **CAW logos anywhere**: the game no longer keeps logos off parts of the body.
- **Arena lighting**: ported arenas that were dark in matches are fixed (and the Mod Maker repairs ones you already have); an **ARENA BRIGHTNESS** setting (GRAPHICS and the launcher); arena mods can use their entrance lighting in the match.
- **Titantron**: custom arenas show their own screens during the match instead of the live camera (the entrance video still plays).
- **Mod arenas fill the arena select left to right** in the order you installed them, with no empty spots.
- **Extreme Rules weapons**: pick which weapons appear (MY WWE > OPTIONS > WEAPONS); online the host's set is used.
- **Superstar Threads works with superstar mods** 🧪, and the Mod Maker can make and paint the colour parts (Superstar > Threads parts).
- **Mod Maker**: import a whole moveset; entrance outfits (coats, hats) for converted superstars - their own or another superstar's, including SvR 2008 / 2010 and 2K16; use an entrance you made in CREATE AN ENTRANCE for a mod superstar; the entrance motion presets have names; free move and turn on all three axes in the Arena Editor, with see-through parts shown like in the game; PS2 USA discs and single PS2 character files.
- **Performance overlay**: Off / FPS / Full in a corner of your choice (GRAPHICS > PERFORMANCE, F2) - FPS, the game's video memory and textures.

Everything new in 2.1.3 is in the [2.1.3 notes](https://github.com/KaikoClanworth1/wwe-svr2011-pc/releases/tag/v2.1.3).

☕ Enjoying the port? **Support me:** https://ko-fi.com/kaikoclanworth · https://www.paypal.com/donate/?hosted_button_id=MS4C2WC38N3W2

💬 **Join the Discord:** https://discord.com/invite/QhGSuDKHwk
