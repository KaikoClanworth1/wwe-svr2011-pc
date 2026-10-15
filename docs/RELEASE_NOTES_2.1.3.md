💬 **Join the Discord:** https://discord.com/invite/QhGSuDKHwk

**Download:** `SvR2011-PC-v2.1.3.zip` for PC (Windows 8.1 or newer, Linux / Steam Deck through Proton) - or `SvR2011-Android-v2.1.3.apk` on its own for Android. `SvR2011-Mods-v2.1.3.zip` holds the bundled mods - the Android app downloads it by itself. On 2.1.x? The launcher and the Android app update themselves - **close the Mod Maker before updating**.

⚠️ **Only download the port from this GitHub page.** It's free. If someone sends you to Telegram or asks you to pay, it isn't us.

🧪 = experimental: it works in our tests but hasn't had wide testing on real hardware yet - reports welcome.

## 🛠️ Fixes
- **Superstar mods whose CPU only reversed moves** (e.g. Jillian) - fixed. With DLC Vol. 1 / 2 installed, the DLC's own move list hid the mod's moves from the CPU in matches, so a superstar with mostly new moves had nothing to attack with.
- **"Can't get online - I don't have the newest update, but I do"**: the update had only half finished (new launcher, old game - usually because the Mod Maker was open). Updates are now all-or-nothing: the launcher waits until the game and the Mod Maker are closed, swaps every file in at once and checks them. If your game and launcher are different versions, the launcher says so and offers **Finish the update**.
- **Antivirus false alarms on the launcher** (Avast, Windows Defender): the launcher is smaller and more ordinary - it uses Windows' own C runtime - and the programs now carry proper version and publisher information. If your antivirus still complains, please send us its detection name.
- **Black screen at start** for players with a very long MY MUSIC list - fixed. The launcher cut that list when saving the settings and the settings file stopped working; it now reads any length, and the game repairs a file that was cut and still starts if the settings can't be read.
- **A controller that connects late took over player 1** (your pick stood still while you played the other wrestler): whoever pressed START now stays player 1, keyboard included. PC and Android.
- **Mod Maker: Arena > From my other games** works again for SvR 2007 - 2010 discs ("'Namespace' object has no attribute 'item'").
- **Created Superstar uploads**: when a Created Superstar can't be uploaded, the game's log now says why (for example a DLC part) - send us a problem report if yours are refused.

Everything new in 2.1.2 is in the [2.1.2 notes](https://github.com/KaikoClanworth1/wwe-svr2011-pc/releases/tag/v2.1.2).

☕ Enjoying the port? **Support me on Ko-fi:** https://ko-fi.com/kaikoclanworth

💬 **Join the Discord:** https://discord.com/invite/QhGSuDKHwk
