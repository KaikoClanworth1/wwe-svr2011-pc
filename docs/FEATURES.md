# WWE SmackDown vs. Raw 2011 PC Port: features

Everything the port adds to the game, as of 2 October 2026 (v1.0.3 plus
unreleased work on `main`). "Unreleased" marks what isn't in a published
release yet.

## Platforms

- **Windows** PC: the recompiled game, Direct3D 12 or Vulkan.
- **Steam Deck / Linux** through Proton: Deck defaults, updates without `tar.exe`.
- **Android** phones and tablets (arm64, Vulkan): the APK is released beside the PC game and is all a player needs (it installs the game from their own disc image on the phone), landscape only, 720p default render scale on phones. Runs the screen at 60 Hz on 120 Hz phones. *(60 Hz: unreleased.)*
- **Laptops with two GPUs** use the fast GPU, and one GPU for both renderers.

## Graphics and performance

- **Native renderer**: the game's Direct3D calls drawn on plume (Direct3D 12 or Vulkan) instead of the emulated Xenos GPU. Falls back to the emulated renderer when stuck or when its shaders are missing.
- **Resolutions** from 720p (the console's) to 4K, anti-aliasing, VSync, steady 60 fps.
- **FRAME RATE**: 30, 60, 120, 144 or 240 fps (DISPLAY tab and the launcher's Settings), a cap with the game at its normal speed at any rate; 30 is the console's own 30 fps mode. VSync off no longer makes the game run too fast. *(Unreleased.)*
- **60 fps entrances and cutscenes** (the game's 30 fps scenes run its own 60 fps mode), optional.
- **Any screen shape**: matches and entrances fill ultrawide, 16:10, Steam Deck and foldable screens. Menus stay 16:9.
- **GRAPHICS page** in game: *My WWE → Options → Graphics*, DISPLAY and QUALITY tabs.
- **Display modes**: WINDOWED, BORDERLESS and FULL SCREEN (exclusive); **F11** switches window / full screen. *(Unreleased.)*
- **Sharp wide shots**: the game's own soft filter (made for 720p) blurred wrestlers in wide shots at higher resolutions; off by default, with a SOFT FILTER option to bring it back. *(Unreleased.)*
- **DEPTH OF FIELD** and **MOTION BLUR** options (QUALITY tab). *(Unreleased.)*
- **Shader work done ahead**: compiled pipelines saved between runs (D3D12 pipeline library, Vulkan pipeline cache), the 298 known pipelines built in the menus with a progress bar ("Prepare graphics"), and shaders shipped as one pack file per API. *(Unreleased.)*
- **Performance work**: XMP 10 ms throttle removed (39 to 60 fps), GPU waits yield the CPU, cheaper register writes, threaded command recording on phones, cheaper per-draw work in heavy entrances, texture checks back off.
- **Entrance frame drops fixed**: since v1.0.3, entrances could fall to single digits as they started (graphics copied back to the game's memory that it never reads); now only the images the game reads are copied. *(Unreleased.)*
- **Rendering fixes**: Superstar Threads attire colours, saved outfits, Tyson Kidd's portrait, MATCH HIGHLIGHTS free space.
- **Created Superstar portraits**: FINALIZE now stores a real portrait (they were black or garbled), so menus and Community Creations previews show the superstar. *(Unreleased.)*
- **Custom GPU drivers on Android** (Mesa Turnip and Qualcomm packages through libadrenotools), with crash and start-up checks that fall back to the phone's driver. *(Unreleased.)*
- **Frame-time logs**: median / slow-frame figures every 5 s, and on phones the thermal state and CPU clocks (for stutter reports). *(Unreleased.)*

## Game additions

- **User entrance music**: *Create An Entrance → Music → USER PLAYLIST* plays songs from the `Music` folder, on Windows and Android. *(Android: unreleased.)*
- **User entrance movies**: *Create An Entrance → Movie → USER MOVIES*, any video turned into a Bink titantron movie by the launcher's movie maker; movies fill the big screen, with the superstar's bottom strip.
- **10 HD logos** per Created Superstar instead of 2 (`Saves\.logos`).
- **Paint Tool: 10 pages of 20 logos** (200 instead of 20); grid edges change pages, the Superstar logo picker scrolls all pages. *(Unreleased.)*
- **Divas against male superstars**: pick a diva, switch back to WWE SUPERSTARS and pick a man; the match plays normally. *(Unreleased.)*
- **Managers playable**: the "?" tile on character select is now an **M** tile named EXTRA; it opens a list of Stephanie McMahon, Theodore Long, Hornswoggle, Paul Bearer and Tiffany (instead of the random pick), and they wrestle full matches. *(Unreleased.)*
- **More match types** in the PLAY menus: Falls Count Anywhere (1v1, tornado tag, triple threat, fatal 4-way), 15- and 25-man Royal Rumble, 6-man Lumberjack, Backstage brawls for triple threat, fatal 4-way and 6-man (7 areas), and **FREE-ROAMING BACKSTAGE** (Road to WrestleMania's whole backstage, all rooms, with a closer backstage camera) in every BACKSTAGE list. *(Unreleased.)*
- **Replays can be switched off**: REPLAYS (DISPLAY tab) skips the instant replays after finishers and the highlights at the end of a match. *(Unreleased.)*
- **Five languages**: English, French, German, Spanish, Italian, in the launcher and in game; **LANGUAGE** has its own entry in *My WWE → Options*. *(Own entry: unreleased.)*
- **Achievements page**: *My WWE → Achievements*, console-style, tracked by the port. Since it was added, B did nothing in *My WWE → Options*; fixed, existing installs are repaired at start-up. *(Fix: unreleased.)*
- **DLC** counts as fully licensed (Bret Hart and the rest).
- **Discord Rich Presence**: menus, entrances, matches.

## Online *(unreleased)*

- **Community Creations** works again on the port's own server (https://sho-ti.me/svr), with player accounts (name and password).
- Upload and download Created Superstars (with their extra logos), Paint Tool logos (all 200 slots), highlight reels and more.
- **Superstars bring their entrance**: an uploaded Superstar carries its custom song and entrance movie (shrunk for upload); downloads put them in `Music` and `Custom Movies`. Movies on Android: not yet tested on a phone.
- The game's GameSpy traffic goes through a relay in the port; Android uses its own https bridge.
- Xbox LIVE texts and prompts removed from the menus.
- Server: `port/server/server.py`, with an admin dashboard (the live server needs a restart for the entrance transfer).

## Input

- **Keyboard controls**: the PC keyboard plays as player 1 (alongside a controller). *(Unreleased.)*
- **Type on the PC keyboard** wherever the game shows its on-screen keyboard.
- **On-screen touch controller** (Android default, any touch screen): MENU and MATCH layouts that switch automatically, an editor to rearrange it, and a **KEYBOARD** button that opens the phone's keyboard while the game's keyboard is up. Hidden as soon as a real controller is connected, with any controller backend; a touch shows it again for 15 s. *(KEYBOARD, controller hiding on every backend: unreleased.)*
- Controllers through SDL.

## Saves

- Every save is a **plain file** in `Saves` (headers in `Saves\.info`), easy to back up and share; old saves are moved over automatically.
- Copied saves without headers load again.

## Launcher (Windows)

- **Install** from the Xbox 360 disc image (ISO / XISO) or a game package.
- **Updates itself** from GitHub releases.
- **Settings**: renderer, resolution, VSync, input, audio, FPS counter, language, "Prepare graphics in the menus".
- **Online tab**: sign in, create an account, sign out; Server: Default or Custom. *(Unreleased.)*
- **Saves**: backups, export, import, delete.
- **DLC**: add packages or folders, checked before install.
- **Paint Tool**: the 10 pages of logos, import any image, export PNGs.
- **Movies**: the movie maker for custom entrance movies.
- **Android**: install over USB, or **Create APK Package** (the game and the app in one folder).

## Android app *(unreleased)*

- A launcher on the phone with the same tabs as the PC one: Play, Settings, Online, Saves, Paint Tool, DLC, Movies, Install, plus updates and the Graphics driver choice.
- Installs the game from a disc image or a package on the phone.

## Requests

Requested 1 October 2026:

- [x] Keyboard controls (play with the PC keyboard). *(6f64494)*
- [x] Android: a KEYBOARD button that opens the phone's keyboard for typing. *(c366c12)*
- [x] F11 toggles fullscreen. *(6f64494)*
- [x] Language selector as its own button under *Options*. *(6f64494)*
- [x] Windowed, borderless and fullscreen choices in *Options*. *(6f64494)*
- [ ] Android stuttering: 60 Hz screen and frame-time / thermal / clock logs added (c366c12); about 20-25% of frames still take 20-28 ms on the phone (2% on PC) - still open, waiting for a log from a bad session.
- [x] Android: custom songs from the `Music` folder, as on Windows. *(8d0de6b)*
- [x] Divas against male superstars: remove the restriction, matches play normally. *(7903c71)*
- [x] Managers playable and selectable in the modes: the "?" tile opens them. *(63d9358)*
- [x] Limit Breaking session: research unused match types in the game files (docs/MATCH_TYPES_RESEARCH.md, 247a26e).
- [x] Online session: uploaded superstars carry their custom movies and music (into `Custom Movies` / `Music` on download). *(44c6b30, Android movies 27a1ff7)*

Requested later on 1 October 2026:

- [x] Entrances dropping to single-digit fps since the update. *(2188b98)*
- [ ] Textures turn black at the start of a match with entrances off: not reproduced in three tests; waiting for the reporter's log, version and screenshot.
- [x] Replays can be switched off completely. *(63d9358)*
- [x] Managers: the "?" tile shows an "M" (tile and banner, from the user's art) and reads EXTRA instead of RANDOM.
- [ ] Match types: 50-man Royal Rumble - the engine holds 30 people in a match (not done).
- [x] Match types: Lumberjack, Falls Count Anywhere, Backstage for triple threat / fatal 4-way / 6-man, 15- and 25-man Rumble. *(7b7e952)*
- [x] Road to WrestleMania's whole backstage as a fighting area: FREE-ROAMING BACKSTAGE, all rooms, backstage camera, working AI. *(Limit Breaking, d773d35)*
- [x] Graphics options: depth of field, motion blur. *(c76de77)*
- [x] Blurry image in wide shots above 720p: the game's soft filter, now off by default. *(c76de77)*
- [ ] Test the Android entrance movies (Community Creations) on a phone.

Requested 2 October 2026:

- [x] Frame rate choices (Limit Breaking: 30 / 60 / 120 / 144 / 240, 1d0fa31; SDK side f3c397d).
- [x] Test 30 fps fully: menus, entrances (same length as at 60), matches, pause, walking speed equal to 60 fps (Direct3D 12 and Vulkan), VSync off, switching live in GRAPHICS. Not yet: a match to its finish (replays, results), a timed match's clock, a phone.
- [x] B did nothing in *My WWE → Options* (out of order menu records since ACHIEVEMENTS).
- [x] The APK released beside the PC game, all an Android player needs: `package.ps1` also writes `SvR2011-Android-v<version>.apk` (the name the app's updater looks for); README updated.
- [x] A connected controller hides the on-screen controller (it only worked when SDL saw the pad, never with the XInput backend). *(b249724)*
- [ ] Online: peer-to-peer matches, leaderboards on the VPS, P2P working even with the VPS down. Pull request #3 reviewed by the Online session: matchmaking plumbing only, needs a central server; parts reusable. Not started.
- [ ] Steam Deck: a match that's won never ends (player's log `svr2011_020 2.log`) - with the Limit Breaking session.
