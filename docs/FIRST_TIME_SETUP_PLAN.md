# First time setup (CHECKLIST E1)

A launcher page that checks and installs what a player needs, on Windows and
under Proton 10. Nothing here is built yet. Sizes and versions were read from
the official pages on 2026-10-13 (header requests only, nothing downloaded).

## What a player needs today, and how it's found

| Item | Who needs it | Found today | Missing today |
|---|---|---|---|
| Game files (from the player's own disc image) | everyone | launcher Install tab: reads the ISO/XISO itself (XDVDFS), checks title id 5451085D, Verify / Repair | first start opens the Install tab |
| VC++ runtime (MSVCP140, VCRUNTIME140, VCRUNTIME140_1, MSVCP140_ATOMIC_WAIT) | everyone: svr2011.exe, rexruntime.dll, rexgpu-xenos.dll, launcher, Mod Maker (clang, dynamic CRT) | not checked, not bundled | the exes don't start (Windows' "VCRUNTIME140.dll was not found") |
| UCRT (api-ms-win-crt-*) | everyone | part of Windows 10/11 | - |
| Media Foundation | user music, launcher Movie Maker, HD .mp4 entrance movies, Community Creations movies | MFStartup | **Windows N without the Media Feature Pack: mfplat / mfreadwrite are normal imports of all three exes, so they probably don't start at all** (the launcher's own "install the Media Feature Pack" message can't show) |
| Python 3 | Mod Maker "Other games" + "Port" pages, Bink 2 | tool_run.cpp: python.exe / python3.exe on PATH (any path with "WindowsApps" skipped - real Store Pythons too), then py.exe | "Python 3 (python.org; tick 'Add to PATH')" |
| numpy, Pillow | the converters (svr10_bruise, w13_char, svr08_*, wwe13_entrance, w13_retarget; Pillow lazily in the ports) | `python -c "import numpy, PIL"` | "pip install numpy pillow" |
| ffmpeg | SvR 2010 themes (svr10_superstar.py: loudness + mp3), superstar_port | PATH, then C:\ffmpeg\bin | "ffmpeg is needed too ..." |
| Bink 2 tool (RAD Video Tools, or a Bink 2 ffmpeg) | .bk2 input only | bink2_convert.py find | opens RAD's download page (stays opt-in, never automatic) |
| adb | Android tab | <launcher>\platform-tools, PATH, Android SDK | downloads on request (7 MB, Google, terms shown) - stays as is |

Neither the game nor the launcher needs Python or ffmpeg. They are Mod Maker
extras.

## Proposal

### 1. No download needed for playing

- **VC++ runtime: bundle it, don't download it.** package.ps1 copies the four
  DLLs app-local next to the exes (Microsoft allows redistributing these
  files; about 1.5 MB). The official VC_redist.x64.exe (24.4 MB,
  aka.ms/vs/17/release/vc_redist.x64.exe) needs admin rights to install, so it
  doesn't fit "no admin, nothing system-wide". The setup page only checks
  (LoadLibrary) and points at that link if something is still wrong.
- **Media Foundation: delay-load mfplat.dll / mfreadwrite.dll** in svr2011.exe,
  the launcher and the Mod Maker (CMake `/DELAYLOAD`, like d3d12), and check
  for them before the first MF call. Windows N players can then play, with
  only music, movies and Movie Maker off. The setup row says "Media Feature
  Pack missing (Windows N)" with a button to Settings > Optional features
  (`ms-settings:optionalfeatures`). Installing it is Windows' job (it needs
  admin), so nothing to download.
- **Game files:** a row "installed / not installed" whose button goes to the
  Install tab. No change to the installer.

### 2. Mod Maker tools (optional group, "Install missing")

Into `<game>\Tools\` (no admin, nothing system-wide):

| Item | Source (official) | Version | Download | Licence |
|---|---|---|---|---|
| Python, embeddable 64-bit | https://www.python.org/ftp/python/3.14.8/python-3.14.8-embed-amd64.zip | 3.14.8 | 12.6 MB (12,601,679 B) | PSF |
| numpy wheel | files.pythonhosted.org (URL + sha256 from https://pypi.org/pypi/numpy/2.5.4/json), numpy-2.5.4-cp314-cp314-win_amd64.whl | 2.5.4 | 12.7 MB | BSD-3 (+ bundled OpenBLAS / gfortran runtime licences in the wheel) |
| Pillow wheel | files.pythonhosted.org (https://pypi.org/pypi/pillow/12.3.0/json), pillow-12.3.0-cp314-cp314-win_amd64.whl | 12.3.0 | 7.2 MB | MIT-CMU |
| ffmpeg (Windows build linked from ffmpeg.org) | https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip (+ .sha256) | 9.0.2 | 109 MB (114,768,076 B) | GPLv3 |

About 142 MB download in all, roughly 250 MB on disk (estimate).

- **Why Python 3.14 and not 3.15:** numpy 2.5.4 ships wheels for 3.12-3.14
  only, so 3.15 would have no numpy wheel.
- **ffmpeg format:** the 34 MB .7z needs tar.exe's 7z support, which isn't
  there under Proton. The zip works everywhere with the launcher's own unzip.
  Only `bin\ffmpeg.exe` is kept.
- **Pinned and verified:** versions, URLs and SHA-256s are fixed in the
  launcher. Every file is checked before it's unpacked; that's new code, since
  updater.c only checks Content-Length today (BCrypt SHA-256). An update to the
  pins is a launcher release.
- **No pip.** The wheels are zips. The launcher unpacks them into
  `Tools\python\Lib\site-packages` and adds `Lib\site-packages` + `import site`
  to `python314._pth` (the embeddable package ignores site-packages
  otherwise). Then it checks with `python -c "import numpy, PIL"`.
- **Licences:** each item's licence text goes into `Tools\LICENSES\` and a
  "Tools" section in the Read Me (the Python, numpy and Pillow licence files
  ship inside their packages; ffmpeg's GPLv3 + source link from gyan.dev's
  page).
- **Optional extras (unchanged):**
  - Bink 2: a row "optional" with RAD's page button.
  - adb: a row "on demand (Android tab)".

### 3. Mod Maker side (agree with ModMakerUIRework)

- **Lookup order:** tool_run.cpp `Check()` looks in `<game>\Tools\python\python.exe`
  and `<game>\Tools\ffmpeg\ffmpeg.exe` first, then today's order.
- **ffmpeg for the scripts:** when it runs a script, it puts `Tools\ffmpeg` first
  on the child's PATH, so `shutil.which('ffmpeg')` in svr10_superstar.py /
  bink2_convert.py finds it with no script change. Same for `SVR2011_FFMPEG`, if
  we'd rather be explicit.
- **Missing-tool messages:** "Set up the Mod Maker tools in the launcher
  (First time setup)" instead of "python.org; tick Add to PATH".
- **Small fixes while there:**
  - the WindowsApps test should skip only the Store *stub* (a 0-byte app
    execution alias), not a real Store Python;
  - the gate could ask for numpy/Pillow only for the scripts that import them.

### 4. UI

- **When it shows:**
  - first start (launcher.ini `SetupShown=0`): a Setup page before the Play /
    Install tab;
  - later from a "Setup / check" button on the Play tab (and Help).
- **Groups and rows:**
  - "To play": Game files, VC++ runtime, Media Foundation.
  - "Mod Maker tools (optional)": Python, numpy + Pillow, ffmpeg.
  - "Extras": Bink 2, adb.
  - Each row shows: found (path) / missing / optional, and the size and source
    when missing.
- **"Install missing":** a confirm dialog lists each file (name, URL host,
  size, licence) and the total, then Yes/No. Then a progress bar per item and
  a re-check. Nothing downloads without that Yes.
- **CLI for tests** (no GUI): `--setup-check` (prints the rows) and
  `--setup-install <items> --yes`.

## Proton 10 (Steam Deck / Linux)

The launcher would detect Wine the way the game already does
(`wine_get_version` in ntdll, `STEAM_COMPAT_DATA_PATH` for Proton). Under
Proton the page hides the Windows-only rows: Media Feature Pack, and the
VC++ link (Proton has its own msvcp140 and also installs the redist).

| Item | Expected under Proton 10 (Wine 10) | To test |
|---|---|---|
| VC++ runtime | Wine builtin msvcp140 / vcruntime140; the app-local DLLs also work | starts |
| Media Foundation | Wine's mfplat with winegstreamer: H.264 .mp4 decode depends on Proton's GStreamer plugins (normally included). Fallback: .bik movies already exist; user music may be off | user music + an HD movie |
| Python 3.14 embeddable + numpy/Pillow wheels | Windows CPython runs under Wine; numpy's OpenBLAS DLLs generally do | `python -c "import numpy, PIL"`, then one converter run |
| ffmpeg Windows build | runs under Wine | a theme conversion |
| downloads (WinHTTP) | Wine's winhttp, HTTPS via GnuTLS | the setup download |
| unzip | launcher's own code, no tar | - |

Testing needs a Proton/Wine machine (the user's Deck, or Wine on Linux). I
can't do it on this PC. Native Linux python/ffmpeg aren't reachable from the
prefix in a reliable way, so the plan uses the Windows builds in both places.

## Found while looking (separate fixes)

1. **Windows N:** mfplat / mfreadwrite are hard imports of all three exes
   (above). This is probably a start failure today.
2. **Releases:** package.ps1 doesn't copy `tools/wwe13_pack.py`, which
   arena_port.py imports. WWE '13 arena porting probably fails in a release.
3. **Port page:** `moves_port.py` and `attire_port.py` are listed on the Port
   page and in package.ps1 but don't exist in the tree.
4. **Downloads:** they have no hash check (updater.c). The setup's SHA-256
   check could cover the update download too.

## Decisions (2026-10-13)

- ffmpeg is its own optional row ("Install ffmpeg (115 MB)"); "Install
  Python + numpy + Pillow (33 MB)" is the other button.
- Python 3.14.8 pinned.
- The four VC++ DLLs ship app-local in the release (package.ps1, from the
  build machine's Visual Studio Redist folder; credited in the Read Me).
- The Windows N fix (mfplat / mfreadwrite delay-loaded) is its own commit.

## As built

- `launcher/setup_tab.c`: the Setup tab (sidebar "Setup", page "First time
  setup", shown once on the first start: launcher.ini `SetupShown`), the
  checks, the pinned downloads (size + SHA-256, BCrypt), the installs, and
  `Tools\LICENSES\README.txt`.
- CLI: `--setup-check <game folder>`, `--setup-install <game folder>
  <python|ffmpeg|all> --yes`.
- The Mod Maker looks in `Tools\` first (ModMakerUIRework's tool_run.cpp).
