# WWE SmackDown vs. Raw 2011 PC Port: features

Everything the port adds to the game, as of 1 October 2026 (v1.0.3 plus
unreleased work on `main`). "Unreleased" marks what isn't in a published
release yet.

## Platforms

- **Windows** PC: the recompiled game, Direct3D 12 or Vulkan.
- **Steam Deck / Linux** through Proton: Deck defaults, updates without `tar.exe`.
- **Android** phones and tablets (arm64, Vulkan): installs from the player's own game package, landscape only, 720p default render scale on phones.
- **Laptops with two GPUs** use the fast GPU, and one GPU for both renderers.

## Graphics and performance

- **Native renderer**: the game's Direct3D calls drawn on plume (Direct3D 12 or Vulkan) instead of the emulated Xenos GPU. Falls back to the emulated renderer when stuck or when its shaders are missing.
- **Resolutions** from 720p (the console's) to 4K, anti-aliasing, VSync, steady 60 fps.
- **60 fps entrances and cutscenes** (the game's 30 fps scenes run its own 60 fps mode), optional.
- **Any screen shape**: matches and entrances fill ultrawide, 16:10, Steam Deck and foldable screens. Menus stay 16:9.
- **GRAPHICS page** in game: *My WWE → Options → Graphics*, with a QUALITY tab.
- **Shader work done ahead**: compiled pipelines saved between runs (D3D12 pipeline library, Vulkan pipeline cache), the 298 known pipelines built in the menus with a progress bar ("Prepare graphics"), and shaders shipped as one pack file per API. *(Unreleased.)*
- **Performance work**: XMP 10 ms throttle removed (39 to 60 fps), GPU waits yield the CPU, cheaper register writes, threaded command recording on phones, cheaper per-draw work in heavy entrances, texture checks back off.
- **Rendering fixes**: Superstar Threads attire colours, saved outfits, Tyson Kidd's portrait, MATCH HIGHLIGHTS free space.
- **Created Superstar portraits**: FINALIZE now stores a real portrait (they were black or garbled), so menus and Community Creations previews show the superstar. *(Unreleased.)*
- **Custom GPU drivers on Android** (Mesa Turnip and Qualcomm packages through libadrenotools), with crash and start-up checks that fall back to the phone's driver. *(Unreleased.)*

## Game additions

- **User entrance music**: *Create An Entrance → Music → USER PLAYLIST* plays songs from the `Music` folder (Windows).
- **User entrance movies**: *Create An Entrance → Movie → USER MOVIES*, any video turned into a Bink titantron movie by the launcher's movie maker; movies fill the big screen, with the superstar's bottom strip.
- **10 HD logos** per Created Superstar instead of 2 (`Saves\.logos`).
- **Paint Tool: 10 pages of 20 logos** (200 instead of 20); grid edges change pages, the Superstar logo picker scrolls all pages. *(Unreleased.)*
- **Five languages**: English, French, German, Spanish, Italian, in the launcher and in game.
- **Achievements page**: *My WWE → Achievements*, console-style, tracked by the port.
- **DLC** counts as fully licensed (Bret Hart and the rest).
- **Discord Rich Presence**: menus, entrances, matches.

## Online *(unreleased)*

- **Community Creations** works again on the port's own server (https://sho-ti.me/svr), with player accounts (name and password).
- Upload and download Created Superstars (with their extra logos), Paint Tool logos (all 200 slots), highlight reels and more.
- The game's GameSpy traffic goes through a relay in the port; Android uses its own https bridge.
- Xbox LIVE texts and prompts removed from the menus.
- Server: `port/server/server.py`, with an admin dashboard.

## Input

- **Type on the PC keyboard** wherever the game shows its on-screen keyboard.
- **On-screen touch controller** (Android default, any touch screen): MENU and MATCH layouts that switch automatically, an editor to rearrange it.
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

## Next (requested 1 October 2026)

- [ ] Keyboard controls (play with the PC keyboard).
- [ ] Android: a button that opens the phone's keyboard for typing.
- [ ] F11 toggles fullscreen.
- [ ] Language selector as its own button under *Options*.
- [ ] Windowed, borderless and fullscreen choices in *Options*.
- [ ] Android stuttering: investigate on the user's phone, log everything.
- [ ] Android: custom songs from the `Music` folder, as on Windows.
- [ ] Divas against male superstars: remove the restriction, matches play normally.
- [ ] Managers playable and selectable in the modes.
- [ ] Limit Breaking session: research unused match types in the game files.
- [ ] Online session: uploaded superstars carry their custom movies and music (into `Custom Movies` / `Music` on download).
