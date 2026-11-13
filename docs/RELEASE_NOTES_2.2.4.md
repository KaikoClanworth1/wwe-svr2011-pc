💬 **Join the Discord:** https://discord.com/invite/QhGSuDKHwk

**Download:** `SvR2011-PC-v2.2.4.zip` for PC (Windows 8.1 or newer, Linux / Steam Deck through Proton) - or `SvR2011-Android-v2.2.4.apk` on its own for Android. `SvR2011-Mods-v2.2.4.zip` holds the bundled mods - the Android app downloads it by itself. `SvR2011-Switch-v2.2.4.nsp` for a modded Nintendo Switch (see `docs/SWITCH_INSTALL.md`). On 2.1.x or 2.2.x? The launcher and the Android app update themselves - **close the Mod Maker before updating**.

⚠️ **Only download the port from this GitHub page.** It's free. If someone sends you to Telegram or asks you to pay, it isn't us.

Fixes marked "potential" couldn't be fully proven here, so please report it if the problem is still there.

## 🆕 New
- **SHADOW QUALITY** in Settings: OFF / LOW / MED / HIGH. OFF and LOW help slower PCs and phones.
- **Online feels quicker**: on a good connection your button presses show up about twice as fast (LAN: 40 ms -> 20 ms). When one player has a slower connection, all the games in the match now agree on one delay, so it stays smooth for everyone.
- **WWE '13 Attitude Era DLC** superstars can be converted in the Mod Maker's porters.
- **Match recordings** now also cover Royal Rumbles and battle royals.

## 🎮 Nintendo Switch
- **Runs at the console's normal clocks** while you play (it used to raise them), so it uses less battery and runs cooler. If your matches run slower than before, set `switch_boost_60 = true` in `SvR2011/svr2011.toml` to get the higher clocks back.
- **Automatic resolution**: in heavy scenes the 3D picture drops a little in sharpness (85%, 75%, down to 480p) to keep the frame rate, then goes back up. Turn it off with `native_dynamic_scale = false`.
- **A faster build of the game** (link-time and profile-guided optimisation).

## 🛠️ Fixes
- **Universe: title holders reshuffled** after adding or removing a mod - fixed (broken since 2.1.9).
- **Entrance music and motions moving to other superstars** after a mod change - potential fix.
- **Create An Entrance: some of your own songs were silent** - songs with long file names (over 39 characters) now work. Pick the song again in Create An Entrance after updating.
- **Crash after a Universe match with many mods** (the game ran out of room for its menu text) - potential fix.
- **Android: crash at the start of a match** (seen with Cena) - potential fix.
- **Crashes from damaged game memory** after some matches - potential fix.
- **Mod Maker: the Durability ability couldn't be ticked** - fixed.
- **Create a Move-Set froze when previewing some mod moves** - moves the game can't play are no longer listed, and a saved move-set that has one uses a normal move instead (offline) - potential fix.
- **Community Creations move-sets** now apply to big roster superstars - potential fix.
- **Ported arenas: mats, barricades and stands only showed from some camera angles** - fixed.
- **Ported entrances: the face didn't move** - converted entrances now get a face track.
- **Phones at about 50 fps: wrestlers shook** - fixed.
- **Android, Universe: the "appear as Match #1?" question ignored taps** - tap YES or NO now.
- **Android, Universe: after a match the menus kept the match's touch buttons** - the menu buttons come back.
- **Phones: 1080P drew more than the Max scale setting** allowed - fixed.
- **Online: a player was dropped after a 5-second hitch** - now 8 seconds, and a dropped game no longer crashes when it catches up.
- **A crash report when closing the game** - fixed (potential).

☕ Enjoying the port? **Support me (optional, everything stays free):** https://www.patreon.com/c/KaikoClanworth · https://ko-fi.com/kaikoclanworth · https://www.paypal.com/donate/?hosted_button_id=MS4C2WC38N3W2

💬 **Join the Discord:** https://discord.com/invite/QhGSuDKHwk
