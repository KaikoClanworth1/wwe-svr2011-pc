# Debug / test suite - design note (for us only, never in a release)

Goal: fast, cheap tests. One command boots straight into what a test needs (a match with chosen superstars,
attires, arena and rule; CREATE modes, Superstar Threads, CREATE AN ENTRANCE, a backstage room, a LAN pair),
takes screenshots / state dumps on cue, drives scripted input, can fast-forward, and quits when done. Hidden,
muted, own p2p port, private data, stopped by PID - the standing test rules, built in.

## 1. What exists (in the release exe today)

| Piece | Where | What it does |
|---|---|---|
| Scripted pad | `src/script_input.*`, `SVR2011_INPUT_FILE` | press / hold / stick / wait / type / key / set / touch; waits: title, menu LABEL, until TEXT, pressuntil; routes |
| Routes | `script_input.cpp` kRoutes | main, exhibition, normal, cage, match_creator, online, options, jukebox, match |
| Test match | `src/test_match.cpp`, `SVR2011_TEST_MATCH` | people / attires / arena / cpu override at the select -> match hand-off; `SVR2011_TEST_RULE` (match_types.cpp) the rule. CAW people crash ~2 s in (the select screen's CAW set-up isn't done) |
| ~140 `SVR2011_TEST_*` aids | 43 source files | logs (MOVE, ITEM, PRO, LOBBY, IMG, VFS...), dumps (POSE, BONE, RULE, entrance blocks), behaviour switches (ROOF_*, WE_*, SK_*, STUCK, SLOW_MS, CRASH...), DLL / mode / people pickers |
| Debug dumps | `SVR2011_DUMP_STRINGS`, `SVR2011_NATIVE_DUMP_FRAME`, `SVR2011_TEXTURE_HASH_LOG` | strings, a frame's draws, texture hashes |
| Free camera | `src/free_cam.*` (F5-F8) | fly camera, hide UI, pause - shipped feature, also a test aid |
| match_diag | `src/match_diag.*` | frozen-match report - a shipped diagnostic (problem reports), stays |
| Fast start | `src/fast_start.cpp` skip_intros | no logos / intro movie |
| Perf | `perf_log_seconds`, the overlay (perf_stats.h), `opt_bench.ps1` | numbers per N s, leak warning |
| Scripts | `tools/session.ps1` (start / input / route / shot / stop), `test_guard.ps1` (player-game guard, test saves, park the window), `run_test.ps1`, `opt_bench.ps1`, ~90 one-off `*_test.ps1` / probes | each test re-does folder set-up, env vars, ports, waits, shots and the PID stop by hand |

Gaps seen in this batch's tests: every test hand-builds a private game folder (exe + DLLs copied, data junctions,
own Mods, private UserData / DLC / Music / Movies); `session.ps1 stop` doesn't stop -GameDir games (PID by hand);
waits are keyed on log lines that differ per route; ~60 s to a match (title 28 s + menus + select);
`route match` skips entrances so entrance tests need another recipe; no state dump on cue; no fast-forward; no
"quit after"; CAW people crash; no LAN-pair one-liner; no JSON result to read back.

## 2. What the suite adds

**A. `svrtest` CLI** (`tools/dev/svrtest.ps1` + `svrtest.py` core; tools/dev is never packaged - see 3):

```
svrtest match --p1 136 --p2 110 [--p3 ..] [--attire 2,1] --arena 9 --rule 0 [--cpu all|p2] [--entrances]
              [--shot 5s] [--shots every:2s:5] [--state 10s] [--fast 4] [--quit 15s]
svrtest entrance --p1 136 [--attire 2] --shots every:6s --until "entrances over"
svrtest open create|caw|cae|threads|moveset|story|jukebox|options|backstage:<room> [--shot 3s] [--quit 10s]
svrtest lan --p1 136 --p2 110 --arena 9 [--shot 10s]      (two instances, a host and a guest, own ports)
svrtest route <name> | svrtest script <file>              (the existing steps)
common: --backend d3d12|vulkan|d3d11  --mods <folder,...|none|all>  --save <test save name>  --perf 2
        --log-level  --keep (don't quit)  --name <run name>
```

Each run: refuses when the player's game runs or 4 test games already run (any chat); makes / refreshes a
private game folder `runs/svrtest/game` (the dbg exe + DLLs copied, `pac`/`sound`/`movies` read-only
junctions, its own `Mods` with only `--mods`, private `UserData` with a copied DLC, private Music / Movies);
picks a free p2p port in 36500-36599; starts hidden and muted; follows the cues; stops by PID; writes
`runs/svrtest/<stamp>_<name>/` (log, shots, state.json, result.json) and prints result.json (ok, timings, fps,
errors, the match line, paths).

**B. Game side, dbg build only** (`src/debug_suite/*.cpp`, compiled only with the option in 3):

- *Direct boot*: `--dbg_boot=match|create|cae|threads|backstage:<n>|...` - no logos, no title: the dbg build
  drives the game's own menu state from the first frame (the menu ids from the route work) instead of
  pressing through it, and for a match fills the select screen's result itself (the same hand-off
  test_match.cpp uses) and starts the load. Research step 1: find the call the main menu's PLAY -> ONE ON
  ONE -> NORMAL chain ends in, to jump there directly; target < 10 s to a match on PC.
- *CAW people* that don't crash: run the select screen's CAW set-up for them (the step test_match skips).
- *Cues*: `dbg_shot=<s>[,every]`, `dbg_state=<s>`, `dbg_quit=<s>` (seconds after the match starts / the page
  opens; or a log text) - shots from the back buffer (no window capture: works hidden / minimised).
- *State dump*: state.json - the match line (people, ids, attires, rule, arena), each wrestler's damage /
  position / current move (match_diag's data), the frame rate, perf_stats numbers.
- *Fast-forward*: `dbg_speed=N` - N world updates a frame (frame_rate.cpp's fixed 60 Hz update already runs
  extra updates to catch up), audio off; for "does it crash 3 minutes in" tests.
- *Test hooks move here over time*: the `SVR2011_TEST_*` aids that are test-only (not problem-report
  diagnostics) move behind the dbg build in later steps, one commit per group, each test script updated.
  match_diag, free cam, perf_log_seconds, skip_intros stay in the release.

## 3. The fence (never in a release)

- CMake option `SVR2011_DEBUG_SUITE` (default OFF). ON adds `src/debug_suite/*.cpp`, defines
  `SVR2011_DEBUG_SUITE=1` and names the exe `svr2011_dbg.exe`. Built only in its own dir `out/build/DebugSuite`
  (`tools/dev/build_dbg.cmd`); SourceRelease never sets it (and fails configure if a cache sets both).
- Every dbg-only line is in `src/debug_suite/` or inside `#if SVR2011_DEBUG_SUITE`; its strings carry a marker
  (`SVRDBG:` prefix in a constant compiled only there).
- `package.ps1` fails when: a staged file is `*_dbg*`, under `debug_suite` / `tools/dev`, or the staged
  `svr2011.exe` / Android `.so` contains `SVRDBG:` or `--dbg_` (a byte search) - so a release can't carry it
  by mistake.
- `tools/dev/` (svrtest and its helpers) is outside the Mod Maker Tools allow-list; package.ps1's audit
  already copies only listed files.
- Android: the same option in the NDK build (a dbg .so for the phone tests), same package check on the APK.

## 4. Order (a commit and a report each)

1. The fence: the option, the empty dbg target, `build_dbg.cmd`, the package.ps1 checks (and a test that a
   fake dbg file makes package.ps1 fail).
2. `svrtest` on today's pieces (folder set-up, guards, ports, routes, SVR2011_TEST_MATCH, shots, PID stop,
   result.json) - useful at once, still ~60 s to a match.
3. Game side: cues (shot / state / quit), state.json, fast-forward.
4. Direct boot (research + build), then CAW people.
5. LAN pair, `open <page>` targets, then moving test-only aids behind the fence.
