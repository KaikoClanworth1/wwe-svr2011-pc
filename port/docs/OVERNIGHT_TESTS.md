# Overnight test plan (mass bug hunt)

Goal: run every mode, every superstar, every match type and every arena the
port has, find crashes, hangs, broken pictures and slow spots, fix what can be
fixed cleanly overnight, and leave a detailed report for the morning.

## Rules while the user sleeps / watches shows

- **Up to 4 PC game instances at once** (the user pauses HandBrake for us).
- **Never flash the screen.** Every game opens on the second screen
  (`--monitor=2`, placed before the window shows), behind every other window
  there (HWND_BOTTOM, no activation), muted. No console windows.
- Each instance has its **own game folder and user data** (`runs\night\w<N>`:
  its own exe copy, `UserData` with the pipeline cache, saves copy; the disc
  data, DLC, Mods and shaders linked read-only from `Game Files`).
- Builds may run at full speed (HandBrake paused).
- Android: the Fold (stay-awake on) runs one instance at a time; the Mali
  tablet one more, if attached.

## What counts as a bug (detected from logs and screenshots)

| Signal | Source |
| --- | --- |
| Crash | `SvR 2011 crash`, `FATAL`, process exit before the end, a crash report |
| Hang | `world update stuck`, no `fps:` line for 20 s, a match past its time cap |
| GPU trouble | `device lost`, `native renderer: can't draw`, `Unhandled guest access` |
| Game data trouble | `moveset: ... not in its list`, `superstar mods:` errors, missing files the game needs |
| Broken picture | screenshot mostly black / one colour / unchanged for a whole match |
| Slow | per scenario: average fps, worst frame, frames over 33 ms (weak-hardware runs too) |

Each run records: scenario, build, settings, start/end, result, the first
error lines, screenshots, fps summary.

## Suites (in this order; each one finishes before the next)

### A. Menu crawl - every menu leaf (135)
`crawl_menus.ps1` across 4 workers: walk to each leaf, probe it (match types:
through setup into the match for 40 s; other screens: a few presses), record
crash/hang. Covers Universe, Road to WrestleMania, Create modes (CAW, CAE,
Paint Tool, Story Designer, Create a Moveset/Finisher, Highlight Reel), My WWE
(Options, Jukebox both tabs, Achievements, Language), Shop, online menus.

### B. Every match type, CPU vs CPU, to the finish
`SVR2011_TEST_RULE` over every rule id the rule table has (0x00 upward, incl.
the port's added ones: Falls Count Anywhere x4, cage pin & give-up, Rumble
15/25/30, battle royals, gauntlet, special referee, lumberjack + attacks,
backstage areas and free-roam, Weapons Everywhere, Slobber Knocker, Three
Stages of Hell, Elimination TT / F4W, Championship Scramble, Mystery Opponent
when they land), `cpu=all`, the right people count, run until the match ends
(cap 8 min; Rumble/Scramble 20 min). Pass = the match ends with a winner and
the results/replay screens come and go.

### C. Every superstar
Each of the 96 roster superstars + the 22 superstar mods + managers (M tile)
+ the test saves' CAWs, in CPU 1v1 matches (pairs rotated so each appears
twice, once per side), entrances on: load, entrance (theme, video, pyro),
2 min of match, finisher if it happens. Pass = no crash/hang, the picture
isn't broken, the superstar's own model shows.

### D. Every arena
Every arena id (the game's ~35, DLC arenas, bundled RAW IS WAR, SmackDown
1999, Royal Rumble 1998, King of the Ring 1998), a short CPU match with an
entrance each: full hall (not empty), crowd, ring, no crash.

### E. Settings variants (one fixed match each)
D3D12 / Vulkan; Vulkan with forced D32 depth (the AMD path); 60 / new 30 fps
mode; texture quality high / medium / low; AA off / 2x / 4x; render scale 1x /
3x; texture packs on (a test pack) and dumping on; mixed genders off; unlock
everything off; touch controls on (PC); MY MUSIC on with test songs.

### F. Weak hardware
The same fixed match on 2 CPU cores (affinity) and with
`SVR2011_TEST_SLOW_MS`, 30 / 60 fps, D3D12 / Vulkan: record fps and worst
frames per scene (menus, entrance, match, replay). Profile the worst ones for
clean optimisations.

### G. Soak
One instance: back-to-back CPU matches for 2+ hours (memory growth, hangs,
the rare boot crash at sub_826E02A8 command 24).

### Android (Fold, and the Mali tablet if attached)
Through `phone_session.ps1`, one instance: B (all match types), a superstar
sample (every 4th + all mods), D (all arenas), E's phone settings (texture
quality, 30 / 60, touch controls on the custom pages), MY MUSIC, and a soak.
Fps per scene recorded the same way.

## Fixing overnight

A bug goes to the session that owns the code (match types: Limit Breaking;
mods/arenas/Story Designer: ModMaker; renderer/Android/touch/texture packs:
Android; Mali: Mali; jukebox/music/Mystery Opponent: Extras; online/P2P:
Online; launcher/DLC/reports/test tools: the PC port session). Fixes are
committed to main, rebuilt and the failing scenario rerun.

## The report (for the morning)

`runs\night\REPORT.md` (and a shared page): totals per suite, every failure
with its scenario, log lines, screenshot and status (fixed in <commit> /
open / owner), the slowest scenes per platform, and the optimisations done.
