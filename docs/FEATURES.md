# WWE SmackDown vs. Raw 2011 PC Port: features

Everything the port adds to the game, as of v2.0.2 (4 October 2026).
"New in 2.0" marks what this release adds; "Experimental" marks what is
still being tested.

## Platforms

- **Windows** PC: the recompiled game, Direct3D 12 (or Vulkan: *Experimental*).
- **Steam Deck / Linux** through Proton: Deck defaults, updates without `tar.exe`.
- **Android** phones and tablets (arm64, Vulkan): the APK is released beside the PC game and is all a player needs (it installs the game from their own disc image on the phone), landscape only, 720p default render scale on phones. Runs the screen at 60 Hz on 120 Hz phones. *(60 Hz: new in 2.0.)*
- **Laptops with two GPUs** use the fast GPU, and one GPU for both renderers.

## Graphics and performance

- **Native renderer**: the game's Direct3D calls drawn on plume (Direct3D 12 or Vulkan) instead of the emulated Xenos GPU. Falls back to the emulated renderer when stuck or when its shaders are missing.
- **Resolutions** from 720p (the console's) to 4K, anti-aliasing, VSync, steady 60 fps.
- **FRAME RATE** (*30 fps: Experimental*): 30 or 60 fps (DISPLAY tab and the launcher's Settings). The game always runs its own 60 Hz timing (its world steps once per 60 Hz tick, drawing once a frame), so speed and physics stay right at 30 fps and on a slow PC. 30 applies in matches; menus always run at 60. VSync off no longer makes the game run too fast. *(New in 2.0; 9e529d3, 224faa9, 0b15618.)*
- **60 fps entrances and cutscenes** (the game's 30 fps scenes run its own 60 fps mode), optional.
- **Any screen shape**: matches and entrances fill ultrawide, 16:10, Steam Deck and foldable screens. Menus stay 16:9.
- **GRAPHICS page** in game: *My WWE → Options → Graphics*, DISPLAY and QUALITY tabs.
- **Display modes**: WINDOWED, BORDERLESS and FULL SCREEN (exclusive); **F11** switches window / full screen. *(New in 2.0.)*
- **Sharp wide shots**: the game's own soft filter (made for 720p) blurred wrestlers in wide shots at higher resolutions; off by default, with a SOFT FILTER option to bring it back. *(New in 2.0.)*
- **DEPTH OF FIELD** and **MOTION BLUR** options (QUALITY tab). *(New in 2.0.)*
- **Shader work done ahead**: compiled pipelines saved between runs (D3D12 pipeline library, Vulkan pipeline cache), the 298 known pipelines built in the menus with a progress bar ("Prepare graphics"), and shaders shipped as one pack file per API. *(New in 2.0.)*
- **Performance work**: XMP 10 ms throttle removed (39 to 60 fps), GPU waits yield the CPU, cheaper register writes, threaded command recording on phones, cheaper per-draw work in heavy entrances, texture checks back off.
- **Entrance frame drops fixed**: since v1.0.3, entrances could fall to single digits as they started (graphics copied back to the game's memory that it never reads); now only the images the game reads are copied. *(New in 2.0.)*
- **Rendering fixes**: Superstar Threads attire colours, saved outfits, Tyson Kidd's portrait, MATCH HIGHLIGHTS free space.
- **Created Superstar portraits**: FINALIZE now stores a real portrait (they were black or garbled), so menus and Community Creations previews show the superstar. *(New in 2.0.)*
- **Custom GPU drivers on Android** (*Experimental*; Mesa Turnip and Qualcomm packages through libadrenotools), with crash and start-up checks that fall back to the phone's driver. *(New in 2.0.)*
- **Frame-time logs**: median / slow-frame figures every 5 s, and on phones the thermal state and CPU clocks (for stutter reports). *(New in 2.0.)*

## Game additions

- **User entrance music**: *Create An Entrance → Music → USER PLAYLIST* plays songs from the `Music` folder, on Windows and Android. *(Android: new in 2.0.)*
- **User entrance movies**: *Create An Entrance → Movie → USER MOVIES*, any video turned into a Bink titantron movie by the launcher's movie maker; movies fill the big screen, with the superstar's bottom strip.
- **10 HD logos** per Created Superstar instead of 2 (`Saves\.logos`).
- **Paint Tool: 10 pages of 20 logos** (200 instead of 20); grid edges change pages, the Superstar logo picker scrolls all pages. *(New in 2.0.)*
- **Divas against male superstars**: pick a diva, switch back to WWE SUPERSTARS and pick a man; the match plays normally. *(New in 2.0.)*
- **Managers playable**: the "?" tile on character select is now an **M** tile named EXTRA; it opens a list of Stephanie McMahon, Theodore Long, Hornswoggle, Paul Bearer and Tiffany (instead of the random pick), and they wrestle full matches. *(New in 2.0.)*
- **More match types** in the PLAY menus: Falls Count Anywhere (1v1, tornado tag, triple threat, fatal 4-way), 15- and 25-man Royal Rumble, 6-man Lumberjack, Backstage brawls for triple threat, fatal 4-way and 6-man (7 areas), and **FREE-ROAMING BACKSTAGE** (Road to WrestleMania's whole backstage, all rooms, with a closer backstage camera; *Experimental*) in every BACKSTAGE list. *(New in 2.0.)*
- **Replays can be switched off**: REPLAYS (DISPLAY tab) skips the instant replays after finishers and the highlights at the end of a match. Highlights that stall (seen on a Steam Deck: a won match never ended) are skipped after 8 s. *(New in 2.0.)*
- **Five languages**: English, French, German, Spanish, Italian, in the launcher and in game; **LANGUAGE** has its own entry in *My WWE → Options*. *(Own entry: new in 2.0.)*
- **Achievements page**: *My WWE → Achievements*, console-style, tracked by the port. Since it was added, B did nothing in *My WWE → Options*; fixed, existing installs are repaired at start-up. *(Fix: new in 2.0.)*
- **DLC** counts as fully licensed (Bret Hart and the rest).
- **Discord Rich Presence**: menus, entrances, matches.

## Online *(new in 2.0)*

- **Online matches, peer to peer**: Player Matches between games with no server needed on a LAN; over the internet a direct connection first (each game's public address; a **Friends** list of addresses in the launcher's Online tab), else through the server's relay, going direct as soon as it can. Both games step the match together (lockstep). Created Superstars, with their Paint Tool logos, can be used in online matches (63aabc9). Tested: two games on one PC, and PC <-> phone through sho-ti.me. ***Experimental*** *(leaderboards on the server to come).*
- **Community Creations** works again on the port's own server (https://sho-ti.me/svr), with player accounts (name and password).
- Upload and download Created Superstars (with their extra logos), Paint Tool logos (all 200 slots), highlight reels and more.
- **Superstars bring their entrance**: an uploaded Superstar carries its custom song and entrance movie (shrunk for upload); downloads put them in `Music` and `Custom Movies`. Movies on Android: not yet tested on a phone.
- The game's GameSpy traffic goes through a relay in the port; Android uses its own https bridge.
- Xbox LIVE texts and prompts removed from the menus.
- Server: `port/server/server.py`, with an admin dashboard and the match relay (`relay.py`).

## Input

- **Keyboard controls**: the PC keyboard plays as player 1 (alongside a controller). *(New in 2.0.)*
- **Type on the PC keyboard** wherever the game shows its on-screen keyboard.
- **On-screen touch controller** (Android default, any touch screen): MENU and MATCH layouts that switch automatically, an editor to rearrange it, and a **KEYBOARD** button that opens the phone's keyboard while the game's keyboard is up. Hidden as soon as a real controller is connected, with any controller backend; a touch shows it again for 15 s. *(KEYBOARD, controller hiding on every backend: new in 2.0.)*
- Controllers through SDL.

## Saves

- Every save is a **plain file** in `Saves` (headers in `Saves\.info`), easy to back up and share; old saves are moved over automatically.
- Copied saves without headers load again.

## Launcher (Windows)

- **Install** from the Xbox 360 disc image (ISO / XISO) or a game package.
- **Updates itself** from GitHub releases.
- **Settings**: renderer, resolution, VSync, input, audio, FPS counter, language, "Prepare graphics in the menus".
- **Online tab**: sign in, create an account, sign out; Server: Default or Custom; **Friends** (addresses to find for online matches). *(New in 2.0.)*
- **Saves**: backups, export, import, delete, and the **saves folder** (keep two installs' saves apart or shared; `saves_folder` in `svr2011.toml`, read by the game too). *(2.0.1.)*
- **DLC**: add packages or folders, checked before install.
- **Paint Tool**: the 10 pages of logos, import any image, export PNGs.
- **Movies**: the movie maker for custom entrance movies.
- **Android**: install over USB, or **Create APK Package** (the game and the app in one folder).

## Android app *(new in 2.0)*

- A launcher on the phone with the same tabs as the PC one: Play, Settings, Online, Saves, Paint Tool, DLC, Movies, Install, plus updates and the Graphics driver choice.
- Installs the game from a disc image or a package on the phone.

## After 2.0.2 (next update)

- **Bring your Xbox 360 save over** (launcher, Saves tab: *Import Xbox 360 save...*): pick the 360 save folder (5451085D, from a USB drive or a save download) and its saves - Created Superstars, Paint Tool logos, replays, created content and the main save - go into a save preset of their own (3a55cc3).
- **Import someone else's Created Superstar** (launcher, Saves tab: *Import Superstar...*): pick their .cas files (from a PC save or an Xbox 360 save, as they are) and each goes into a free slot; its Paint Tool logos are added to free Paint Tool slots (logos you already have are left out). Your saves are backed up first (09f38fe).
- **Save presets** (launcher, Saves tab): whole sets of saves to switch between (a 360 save, a fresh start, ...); *New...* starts one empty or as a copy of the saves in use (3a55cc3).
- **Verify game files** (launcher, Install tab): checks every file the game disc installed against the disc's sizes and checksums, with a green tick when they're all fine. Damaged files (a bad copy or extraction - they can crash the game while it loads) are listed; **Repair** sets them aside so **Install** copies them again (Install skips files that already have the right size). `--verify <game folder>` does the same without a window.
- **Jukebox** (*My WWE → Jukebox*): turn each of the 19 menu songs (10 original entrance themes, 9 menu themes) on or off; the menus then only play songs that are on (a song turned off while it plays stops, and another starts), and with all off they're silent. **Preview** (X) plays the chosen song now. It shows the song playing now. Saved as `jukebox_off` in the settings file. Song names are the soundtrack rips' (the game has none).
- **M tile in Match Creator** opens the managers list too (it gave a random superstar) (e5bd675).
- **Finisher freezes and won matches with no ending fixed** (Android, slower devices; Steel Cage, Match Creator and others): on frames with two world updates the characters' job could start twice; now once a frame, as on the console. Tested at simulated phone speed (2f1233e).
- **Match-end logging**: each step of the ending is logged, and a step waiting over 10 s says what it waits on; the match line has the right names; FULL SPEED is in the settings line (2f1233e).
- **Won matches that never ended, fixed for real** (Steel Cage escape and others, Android): the replay helper ran only one of two jobs sent before it woke, and the ending waited forever on the other; it now runs every queued job. Tested on the Fold (c3fb4da).
- Safety net: if the ending still waits 8 s on the replay helper, it goes on without it (no highlight clips that match) (282aa39).
- A guard for a rare crash at 30 fps on PC (untested against the crash itself) (2f1233e).
- **Built-in graphics drivers for Adreno 710 / 720 / 722** (Android): two Mesa Turnip drivers that players found run the game well (vauzi's v4.1, recommended, and v4.0) come with the app. On those GPUs the launcher offers v4.1 once by itself, and **Settings, Graphics driver** has a button for each. These phones' own drivers are often too old for the Native renderer (a Redmi Pad Pro's Adreno 710 driver: Vulkan 1.1), so they ran the slower Emulated one. The launcher shows the phone's GPU there, and problem reports include it (8d2b399; `port/android/drivers/README.md`).
- 🧪 **Mali GPUs: alpha mode** (Android, untested - no Mali phone yet): a **Mali GPU (alpha)** card in Settings, and Play asks to turn it on first. It turns off the emulator's need for geometry shaders and line drawing, which Mali lacks; without that the game couldn't start at all. The Native renderer decodes DXT textures on the CPU on GPUs that can't read them (Mali). Every log now has a **GPU report** (device, features, formats) that tells what a GPU is missing (ff90d80, 8d2b399).

- **Mods** (ModMaker session, arenas branch merged 7dc4e74): a **Mods** tab in the launcher and the **SvR2011 Mod Maker** (with a 3D arena editor): custom **arenas** (with their own VS screen, ring and ropes), **superstar mods** (50 slots, theme and movie, shown with a MODDED badge), **crowd sign** packs and **media** packs (themes, videos). Mods live in `Mods\` (Arenas, Superstars, Signs, Media). Docs: `port/docs/ARENA_MOD_MAKER_PLAN.md`, `SUPERSTAR_MODS.md`, `CROWD_SIGNS.md`, `MEDIA_MODS.md`.
- The M tile's EXTRA list: Stephanie McMahon, Theodore Long, Paul Bearer, Tiffany and The Hurricane (Hornswoggle removed: broken).

## New in 2.0.2

- **FULL SPEED** (GRAPHICS → DISPLAY; on by default, as before): off, the game makes one world update a frame like the Xbox 360, so a device that can't reach 60 slows down instead of catching up (30 FPS then plays at half speed). A workaround to try for the remaining freeze on some phones (31234c4).
- **Freeze reports**: the thread dump a freeze writes no longer faults (it flooded 2.0.1 Android logs), so the next freeze log shows which thread holds it (57975df).

- **ONLINE overlay on Android**: an ONLINE button in the touch MENU layout (beside EDIT) opens it (d592e58; not yet tried on a phone).

## New in 2.0.1

- **Character select texture flashes fixed**: moving the cursor could show the next superstar for a moment with the last one's textures (Jimmy Snuka in John Cena's jeans).

- **ONLINE overlay** (F1, or BACK + RB; or X INVITE FRIENDS in a lobby): friends with who's online, invites to public or private sessions, and accepting them (the session comes first in CUSTOM MATCH search) (7861d84; the live server needs a restart for invites).

- **Freeze fix**: entrances and finishers could freeze for good on slower PCs and phones (a deadlock in 2.0's 60 Hz timing when a frame needed extra world updates); fixed (c181189).
- **Keyboard controls page**: *GRAPHICS → CONTROLS* rebinds every input (add a key, clear, reset to defaults), saved and applied at once; works with a pad too (7feb9b8).
- **Better logs and problem reports**: the log starts with the version, build and settings; crashes are written into the log; Android now has crash reports; menu choices are logged by name; freezes dump every game thread; each match logs its rule, arena and wrestlers; **Report a problem** (launcher Play tab and the Android app) makes one zip of the logs, crash reports and settings, without the online password (a09766e, c181189).

- **Friends list** in the launcher's Online tab (PC and Android): add / remove friends, see who's online, in an online match or offline (195917e; server /api/friends, eb2e1e9).
- **Online logos fixed**: a Created Superstar's extra High Resolution logos now come from its owner when the server supplies the Superstar (6178e5e).
- **Community Creations uploads**: multi-part uploads (2 MB parts) were always refused; fixed. **Server dashboard** (/svr) shows its totals and relay metrics again (eb2e1e9; the live server needs a restart).
- **Lumberjack (partly)**: now 2 wrestlers and 4 lumberjacks, all starting on the floor around the ring; they still walk in and brawl (the game has no lumberjack AI; a custom version is planned in docs/MATCH_TYPES_RESEARCH.md) (8a8298e).
- **Free-roaming backstage**: the CPU now moves and fights (played as the Parking Lot rule over the whole backstage), and the camera is pulled back (70412c3).

- **Anti-aliasing levels**: OFF / 2x / 3x / 4x supersampling (4, 9 or 16 samples a pixel; GRAPHICS → QUALITY and the launcher's Settings), within the 4x render scale cap (4d363c8).
- **Android drivers** (cb15eb3): **driver variables** (e.g. `FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1`, the fix for HyperOS 3's glitches with Turnip; a one-tap "HyperOS 3 fix" button); any driver zip (with or without `meta.json`) or a bare `.so` can be added, and zips labelled oddly by file managers show up in the picker; a `gpu_driver` set by hand to a file in Download is copied into the app's storage before loading. *Experimental (not yet tried on a phone).*

- **Android black screen on APK-only installs fixed**: the app saved its settings on Play before the game had ever run, so the game's defaults (its GPU plugin among them) were never written and nothing could draw. The game now adds any default a settings file lacks, which also repairs phones that already have the broken file.

- **Saves folder** can be chosen in the launcher's Saves tab (17c9df4).
- **Online without Online Axxess**: players whose DLC lacks THQ's online pass got "Information related to your trial period couldn't be retrieved" on ONLINE; the port now supplies just that pass when online is on and no installed package has it (cdf6346). No paid unlocks are granted.
- **Match-only behaviour no longer switches on in the ONLINE menu** (30 fps, touch MATCH layout, wide view, Discord): it waits until a match is running (60182bb).

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

- [x] Frame rate choices (Limit Breaking, 1d0fa31; SDK side f3c397d). The user's play test found 30 fps broke physics and 120+ ran too fast; redone with the game's own 60 Hz timing (9e529d3), now 30 or 60 only (224faa9).
- [ ] Test 30 fps fully. The first version (1d0fa31) passed the automated checks but the user's play test found broken physics. The redone version is in Game Files (224faa9): in the practice ring it shows 30 fps with the game stepping 60 times a second and walking at the same speed as at 60. The user's play test then found an odd running animation and laggy menus at 30: fixed in 0b15618 (characters' animation stepped per tick; menus at 60, 30 only in matches), in Game Files (ff79af7). Found in the check: with entrances off a match stayed at 60 (the "in a match" switch only happened when entrances start); fixed in 322e5fb (a match-load hook switches frame rate, touch layout, wide screen and Discord together, with or without entrances); in Game Files, checked: an entrances-off match draws 30 frames a second while the game steps 60 times. Waiting on the user's play test.
- [x] B did nothing in *My WWE → Options* (out of order menu records since ACHIEVEMENTS).
- [x] The APK released beside the PC game, all an Android player needs: `package.ps1` also writes `SvR2011-Android-v<version>.apk` (the name the app's updater looks for); README updated.
- [x] A connected controller hides the on-screen controller (it only worked when SDL saw the pad, never with the XInput backend). *(b249724)*
- [ ] Online: peer-to-peer matches, leaderboards on the VPS, P2P working even with the VPS down. Lockstep (0a0c585); more fixes in progress (Online session). LAN play works: two games find each other, share a Player Match lobby and play a synced One on One, no server needed *(Online, b93b434; from PR #3's reusable parts)*. Internet matches (61c6f10): direct first (STUN public address, and a **Friends** list of addresses in the launcher's Online tab), else through the server's relay; while relayed the games keep trying to go direct. Tested PC <-> phone through sho-ti.me (a synced match). Game ports moved +20000 for Android (e56a8e7). Still to do: leaderboards on the server; checking internet play with the server down (direct + Friends only).
- [x] Steam Deck: a won match never ended (player's log, v1.0.x). The match-end highlights waited forever for the replay recorder; now they're skipped after 8 s and the match goes on to the celebration and results, with the recorder's state logged. Waiting for a log from the player on the next build to find why the Deck's recorder stalls. *(Limit Breaking, c43d482)*
