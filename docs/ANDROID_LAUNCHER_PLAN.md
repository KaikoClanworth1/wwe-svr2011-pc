# Android launcher (plan)

The APK opens a launcher, `LauncherActivity`, once the game is installed. It is the PC launcher's tabs
made for a touch screen: a Java UI built in code, the same `svr2011.toml` keys, and Play starting the game.
The PC launcher's heavy logic (Bink movie maker, Paint Tool storage) is portable C, so it should go to the
phone through JNI (a small `liblauncher.so` built with the NDK) rather than be rewritten.

## Status

| Tab | PC launcher | Android | Notes |
|---|---|---|---|
| Play | Play, game folder, Check for updates | **Done** (91011ed): Play, version | Updates: phase 4 |
| Settings | display, input, audio, language, prepare graphics | **Done**: phone rows (touch, FPS, VSync, renderer, language, quality, mute) | No window or resolution on a phone |
| Online | play online, name, server | **Done** | |
| Saves | back up, restore, export, import, delete | Phase 2 | Java (plain files); Android share sheet for export / import |
| DLC | add DLC packages | Phase 2 | Java; pick files with the system picker (SAF) |
| Install | install from the disc image | Phase 3 | The package zip install exists (InstallActivity); add ISO install (XDVDFS in Java or the C extractor through JNI) and reinstall |
| Paint Tool | 10 pages of logos: view, import image, export PNG, delete | Phase 3 | JNI to the launcher's C for the storage format; image import via the system picker |
| Movies | user movies: make from video / image, superstar, preview, delete | Phase 3 | JNI to movie_maker.c / bink_decode.c; Android's MediaCodec decodes the source video |
| Updates | GitHub releases, downloads the zip | Phase 4 | Download the release's APK and hand it to the package installer (REQUEST_INSTALL_PACKAGES, INTERNET) |
| Android Install | create the APK package | n/a | PC only |

## Notes

- **Layout:** cards of rows (label, hint, control), each change saved at once, insets for edge-to-edge
  (Android 15+), switch colours set explicitly (red on, grey off).
- **Tests:** `adb shell am start -n io.github.kaikoclanworth1.svr2011/.InstallActivity`. Screenshots use
  the display id (`screencap -d <id>`; the Fold has two). Taps use `input -d 0` on the cover screen.
- **Game tests:** automated tests with SVR2011_* extras go straight to GameActivity (phone_session.ps1
  keeps working).
