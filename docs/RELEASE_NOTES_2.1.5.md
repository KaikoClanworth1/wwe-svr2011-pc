💬 **Join the Discord:** https://discord.com/invite/QhGSuDKHwk

**Download:** `SvR2011-PC-v2.1.5.zip` for PC (Windows 8.1 or newer, Linux / Steam Deck through Proton) - or `SvR2011-Android-v2.1.5.apk` on its own for Android. `SvR2011-Mods-v2.1.5.zip` holds the bundled mods - the Android app downloads it by itself. On 2.1.x? The launcher and the Android app update themselves - **close the Mod Maker before updating**.

⚠️ **Only download the port from this GitHub page.** It's free. If someone sends you to Telegram or asks you to pay, it isn't us.

🧪 = experimental: it works in our tests but hasn't had wide testing on real hardware yet - reports welcome.

## ✨ New
- **Upscaling** (GRAPHICS > UPSCALING and the launcher): the game draws at a lower resolution and sharpens it back up to your screen - more frames on weaker PCs.
  - **FSR 1** (AMD): works with any graphics card.
  - **FSR 2** 🧪 (AMD): sharper than FSR 1; Direct3D 12 and Vulkan only. The crowd can look smeared.
  - **DLSS 4** 🧪 (NVIDIA RTX cards, Direct3D 12): the cleanest picture. On Vulkan it uses FSR 2 instead.
  - Upscaling is **OFF** by default.
- **Create A Superstar without limits**: no more "remaining points", and you can layer as many parts as you like (a T-shirt, a shirt and a hoodie at once). Up to 36 layers a block. OPTIONS > DISPLAY > CAW LIMITS switches back to the original rules.
  - ⚠️ A very heavy Created Superstar (30+ layers) from Community Creations can crash the CAW editor on **older versions** - please update.
- **Match camera**: choose NORMAL, FAR, CLOSE, HIGH, LOW or RAMP (a fixed wide view toward the ramp, like the Royal Rumble) (GRAPHICS > DISPLAY > MATCH CAMERA, or the launcher), or switch any time in a match with **BACK + R3** (keyboard **F4**, Android: the **CAM** button). Online, everyone picks their own view.
- **Keyboard players see their own keys** on the prompts above the wrestler in matches (reversals, finishers and the rest), following your key bindings.
- **Real bikes in entrances**: the 2K14 / HCTP bike models, ridden properly, with a working headlight - and the Undertaker's black bike.
- **SvR 2008 crowd fighting** 🧪 for superstars ported from SvR 2008: fight into the crowd in Extreme Rules matches.
- **Road to WrestleMania**: play the Undertaker's Streak as a mod superstar or Created Superstar 🧪.
- **Weapon mods** appear in OPTIONS > WEAPONS and in Extreme Rules matches.
- **Mod Maker**:
  - **Bink 2 titantrons** (WWE 2K15-2K17 `.bk2`) now convert: install RAD Video Tools once (free), and the Mod Maker does the rest in the background.
  - A reworked **Import FBX** (step by step, faces and materials fixed, a Materials tab) and a guide.
  - Pick which **PS2 attire** to port.
  - Clearer errors: no more bare "KeyError" - the Mod Maker says which file and why.

## 🛠️ Fixes
- **Black screen at every start** after a game was closed while merging move packs - fixed.
- **The game hung after the entrances** in every match for some heavy mod setups - fixed (and the log names the file if it ever happens again).
- **The first entrance in a mod arena showed the stock arena** of that slot - fixed.
- **Bundled mods were reinstalled at every launch** and your edits to them overwritten - the launcher now only checks them, and asks before fixing a real problem (PC and Android).
- **Play** waits while the launcher checks files instead of doing nothing.
- **Road to WrestleMania week 12 froze** (also on Android) - fixed.
- **WWE Universe teams with mod superstars fell apart** - fixed.
- **Pin count stopped at 2** / the referee said "two" - fixed.
- **A second backstage brawl crashed** - fixed.
- **Superstar Threads froze** on Android - fixed.
- **Big WWE 2K14 ports crashed** while loading - fixed.
- **Custom arena titantrons**: small screens were blank - fixed.
- **Created entrance music was silent** after Windows changed the sound device - the game now opens it again.
- **Mod Maker ports**:
  - WWE '12-2K15 entrance outfits crashed at match start - fixed.
  - Hollywood Hogan's bandana was invisible and his entrance shirt missing - fixed (other models with parts inside the hair, too).
  - SvR 2007-2009 **divas** had stretched "spider" fingers - fixed.
  - WWE '12 arms and legs bent the wrong way - fixed.
  - Here Comes the Pain / Shut Your Mouth superstars looked up (head tipped back) - fixed.
  - WWE '13 / 2K14 weapon models no longer crash; they're left out.
  - Imported FBX models no longer stand hunched in the 3D view.
- **Online**: P2P matches work again after our server move.
- Android: the performance overlay no longer leaves marks under the touch controls.

Everything new in 2.1.4 is in the [2.1.4 notes](https://github.com/KaikoClanworth1/wwe-svr2011-pc/releases/tag/v2.1.4).

Third-party: AMD FidelityFX Super Resolution 1 and 2 (MIT licence) and NVIDIA DLSS (NVIDIA's licence, in the zip). See "Third-party notices.txt".

☕ Enjoying the port? **Support me:** https://ko-fi.com/kaikoclanworth · https://www.paypal.com/donate/?hosted_button_id=MS4C2WC38N3W2

💬 **Join the Discord:** https://discord.com/invite/QhGSuDKHwk
