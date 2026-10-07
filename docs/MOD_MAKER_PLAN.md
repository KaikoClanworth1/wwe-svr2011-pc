# Mod Maker 2 - plan (2026-10-07)

**Status (2026-10-07, arenas up to 706c021):** phases 1, 2, 3 and 5 done;
phase 4 partly (redo, multi-select, grid, snap step, gizmo marks, nudges,
view pictures, rope pictures; collision rebuild and crowd editing still
open). Also done beyond the plan: CAW pictures page (the Created Superstar
render in the save), match moves played in the game with a dummy, the
made_with stamp + launcher column, --behind / --shot no-window tests.
Not yet verified in the game: CAW picture replacement (offline checks pass),
Play in game, move packs saved from the Moves page.

The Mod Maker (`port/modmaker/`) becomes a complete, friendly tool for the
`.svrmod` format: every mod type the game loads, projects that reopen, viewers
for the game's own assets, warnings before anything goes wrong, and a manual.

## Where it stands (survey, 2026-10-07)

- One 2342-line `mod_maker.cpp`: eight rail pages (Arenas, Arena Editor, VS
  screen, Superstars, Crowd signs, Titantron videos, Menus & renders, Audio),
  a log pane, all state in globals, nothing saved between runs.
- **No project file.** Only arena `.svrmod`s reopen; backstage, superstar,
  sign and media mods can't be reopened. The game folder isn't remembered.
- **Backstage mods can't be saved from the UI** (`arena < 0` disables Save).
- Superstar and media builds run on the UI thread (the window freezes).
- Manifest keys the game reads but the Mod Maker never writes: arena
  `load.dds`; backstage `row=`, `gimmick=`, `camera=`, `camera_height=`,
  `box=`; superstar `announcer=`, `entrance=`, `abilities=`, `moves=`,
  `attire1=`, and the `moves/pack.txt` move pack. No Moves page at all; the
  launcher doesn't list or install `Mods/Moves`.
- Ring Kit rope pictures exist in `ring_kit` but not in the UI.
- Arena Editor: undo only for transforms, no redo, no multi-select, 50 cm
  limit (HMD collision isn't rebuilt), crowd not editable.
- Viewers: only the editor's arena view and the superstar idle preview.
- Docs: developer notes per mod type; no user manual, no format reference.

## Phases

### 1. Foundation - shell, projects, problems, speed
- Split `mod_maker.cpp` into `app` (window, D3D, log, dialogs, settings),
  one file per page, `project` (save/load), `problems` (validation).
- New shell: title bar with **Project** (New / Open / Save / Save as /
  recent), the mod's name and a dirty mark; rail grouped as **Make** (Arena,
  Backstage, Superstar, Moves, Crowd signs, Media), **Look** (Game assets,
  Animations, Icons & renders), **Help**; a status line; the log folds away.
- **Projects:** `.svrproj` (text, `key=value`, paths relative to the project
  folder) holding every page's state. Reopen any `.svrmod` type into its page.
- **Problems panel:** every page lists its errors (block Save / Install) and
  warnings (shown, with the fix). Checks: names/lengths, required files,
  formats (EPK8, BIK 320x320, DDS sizes), arena size budget, theme loudness,
  slot count (50), duplicate ids, game running while installing.
- **Settings** remembered in `%APPDATA%\SvR2011 Mod Maker\settings.txt`:
  game folder, window placement, recent projects.
- Every build and load in the background with a progress line; the UI never
  blocks. Caches: decoded banners/renders kept per game folder.
- Fixes: backstage Save; Install clears the old folder; Media version field.

### 2. The whole `.svrmod` format
- Arena: loading screen (`load.dds`), rope pictures, `base=` choice shown.
- Backstage: `row=`, `gimmick=` (+ `gimmick.pac`), `camera=`, `camera_height=`,
  `box=` with a picker in the editor.
- Superstar: `attire1=`, `announcer=`, `entrance=` (list of the game's
  entrances by superstar), `abilities=` (checklist), `moves=` (moveset editor:
  slot -> move picked by name from WAZE), move pack inside the mod.
- **Moves page:** make / open a move pack (`pack.txt` + motions): import from
  `tools/svr10_moves.py` output or another game's export, view each motion,
  save as `type=moves` `.svrmod`; launcher lists and installs `Mods/Moves`.
- Format reference: `docs/SVRMOD_FORMAT.md` (every key, every file, every
  limit, which game code reads it).

### 3. Game assets - browser and viewers
- **Game assets** page: tree of `pac/` (EPAC / EPK8 -> groups -> entries ->
  PACH -> BPE -> bundles), with a viewer per kind: texture (DDS, channels,
  mips, export PNG), model (JBOY in 3D, textures, nodes), text/hex, bank
  (YMKs/YMBs directory), tables (CHAR/DAT, WAZE, COS). Export any entry.
- **Animations** page: a character (any ch.pac or mod) playing motions from
  the m.pac banks, listed by category with move names from WAZE (taunts,
  stances, finishers...), play / pause / scrub / speed, camera orbit/zoom.
  Decodes YMBs (done for the idle) and extends to nseg > 0 / types > 1;
  YMKs (match moves) shown through the game: **Play in game** starts a test
  match with the mod and a dummy opponent and forces the move
  (`SVR2011_TEST_MATCH` + `SVR2011_TEST_FORCE_MOVE` / `TEST_MOTION_SWAP`).
- **Icons & renders** page: SSFA-D renders and face icons per superstar,
  arena banners, VS themes, loading screens, crowd signs, pad icons; export
  and "use as" shortcuts into the mod pages.
- **CAW render modifier:** find where a Created Superstar's list picture
  lives (.cas / SaveData), decode it, let the user replace it from a
  picture; needs research first (only the 128x128 community thumbnail is
  decoded today).

### 4. Arena Editor
- Redo, undo for textures and structure, multi-select, box select.
- Better gizmo (per-axis move/turn/scale with handles), grid and ground
  snapping, numeric input, local/world, duplicate with offset, mirror copy.
- Orthographic views, focus, measure, screenshot, texture/material panel
  (all textures listed, swap, export/import), object search and filters.
- Rope pictures and the Ring Kit preview; VS screen and loading screen in the
  same project.
- Collision: rebuild HMD boxes for moved ringside parts (lifts the 50 cm
  limit) - research first.
- Crowd on/off per section if the nested PACH allows.

### 5. Documentation and polish
- `docs/MOD_MAKER_MANUAL.md` (user manual, page by page, with pictures) and
  the in-app Help page that shows it; tooltips on every control.
- Tests: `tools/modmaker_shot.ps1` pages, `--test-*` saves, a project
  round trip, every mod type installed and loaded by the game once.

## Working rules
- Branch `arenas`, worktree `D:\Xbox Games Ports\SvR2011 Arenas`; build
  `ninja svr2011_modmaker` in `port/out/build/SourceArenas`.
- Screenshots: `tools/modmaker_shot.ps1` (off-screen, never takes focus).
- The game is never written; mods overlay at runtime. Test games under
  `port/runs` only.
