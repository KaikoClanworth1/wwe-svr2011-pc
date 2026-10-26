# Release tests

The checklist run on every release candidate before it is published (first: 2.1.6 from main). Owner: the
Release Testing chat. It runs when Main says a candidate is ready, never on its own before that.

Each test lists **what it proves**, **how** (the hidden command or route), the **pass rule**, and the **proof**
that gets saved. Proof goes to `port\runs\release_tests\<version>\<id>\` (logs, frame shots, result.json). The
results page (section 9) links to it.

## 0. Rules for every run

- **Hidden.** `SVR2011_WINDOW_HIDDEN=1` (test_guard.ps1 sets it), muted, never focused. Pictures come from frame
  shots (`SVR2011_TEST_FRAME_SHOT(S/_AT/_EVERY)`, `SVR2011_TEST_SHOT_REQUEST`) or `--exe dbg` GPU read-back
  shots. Never PrintWindow, since a hidden window comes out black. Never take the mouse or keyboard. Scripted input
  only (`SVR2011_INPUT_FILE`, routes), with `--mnk_mode=false`.
- **Game load.** Every start goes through `test_guard.ps1`: `Assert-NoPlayerGame` then `Wait-SvrTestSlot`. That
  allows 4 test games machine-wide, or 2 at below-normal priority while VALORANT, VRChat or another listed game
  runs. A game the user started also counts as busy, and the player-game check refuses to test then. Count the
  games before every start, and never start a 5th.
- **Private data.** The candidate goes in its own folder, `port\runs\release_tests\<version>\game`. Saves, DLC,
  Mods and settings are copies there. There are no junctions or hard links into `Game Files`, except the read-only
  folders (bg, ch, edit, evt, evtdsc, movies, sound, native_shaders). `svrtest`'s game_files_changed tripwire must
  stay quiet.
- **Ports and servers.** Each game gets its own `--p2p_port` in 36500-36599. Online tests use only a local private
  server (`test_server.ps1` / `svrtest lan`), never the live server or the VPS. Stop the servers right after.
  `test_guard.ps1`'s `Use-SvrTestServer` runs before every game start. It points any test without a local server
  at a dead address (`REX_ONLINE_SERVER=http://127.0.0.1:9`) and refuses a start on a non-local one.
- **Stopping.** Stop processes by PID, only the ones this run started. Never filter by command line.
- **No GUI.** The launcher and Mod Maker are used through CLI test flags only. First check that the exe has the
  flag (`strings`/timestamp), because an old exe would open its window.
- **No downloads** without asking. The candidate zip comes from Main's `package.ps1` output.
- **Builds.** Builds don't touch the shared SDK out folder (`REXGLUE_USE_VULKAN=ON` in the CMakeCache). Game-code
  fixes found here go to Main; this chat does not patch the candidate.

Legend: `svrtest` = `port\tools\dev\svrtest.ps1`. `--exe <cand>` = the candidate's svr2011.exe. Common flags
for every game run: `--exe <cand> --userdata <private copy> --name <id>`.

## 1. Menus

| id | proves | how | pass | proof |
|---|---|---|---|---|
| M1 | every menu and sub-menu opens and backs out, with no crash and no stuck screen | `crawl_menus.ps1 -Workers 2` on the candidate folder (one boot per leaf of runs\crawl_leaves.json; match leaves play `-MatchSeconds 20`); then `svrtest open <page>` for the pages the crawl doesn't reach (caw, threads, cae, universe, match_creator, options:<ROW> for every OPTIONS row, including the port's own pages) | every leaf `ok`; each `open` reaches its page and returns to the main menu on B | crawl table + one frame shot per leaf |
| M2 | the same in every language | M1's short form (main menu, OPTIONS, each port page, a match setup) with `--arg "--user_language=fr"`, then `de`, `es`, `it` (en = M1) | as M1; no missing / English-only labels on our pages | one shot per page per language |
| M3 | button pictures | `SVR2011_TEST_PAD_TYPES=xbox`, then `ps`, then `keyboard`, over main menu, a match setup and the in-match pause | the right icon set on each; no stale picture (pad-icons-stale) | shots per set |

## 2. Gameplay (CPU vs CPU unless a player is truly needed)

All matches use `svrtest match ... --cpu all`. Use `--exe dbg` only when GPU shots or `--state` are needed. Every
run: `--shots every:5s:6 --quit 40s`, plus "match: rule" and the replay recorder line in the log.

| id | proves | how | pass | proof |
|---|---|---|---|---|
| G1 | stock match | `svrtest match --p1 EDGE --p2 KANE --arena 1 --rule 01 --entrances --cpu all` | entrances, match runs, no stall / crash line | shots + game.log |
| G2 | custom match (Match Creator) | `svrtest route match_creator` + script that makes and starts a 4-way ladder | match starts with the chosen rules | shots |
| G3 | mod superstars | `--mods <bundled set>`, `--p1 SABU --p2 <a 2K15 port> --cpu all` | both load (model, entrance, moves); CPU uses moves | shots + move log |
| G4 | mod arenas | `--arena <WWE '13 RAW IS WAR>`, then a LOWM arena and an HCTP world | arena loads, lit (not dark), crowd present | shots |
| G5 | special match types, cut / restored too | `SVR2011_TEST_RULE=<id>` per row of match_types.cpp (Three Stages of Hell, Scramble, Lumberjack, Slobber Knocker, Weapons Everywhere, Inferno, TLC, Iron Man...) | each rule starts and runs 20 s | one shot each + rule table |
| G6 | RTWM: Undertaker's Streak with a mod superstar | `route` to RTWM Streak, `SVR2011_TEST_RTWM_WARP` + MODS sort, pick a mod | the week's match starts with the mod | shots + event log |
| G7 | Create-a-Story | `open` CREATE A STORY, play a stock story's first scene (scripted presses) | scene plays, match starts, returns | shots |
| G8 | Universe week + save | `open universe`, sim one week (scripted), save | save written (SaveData.dat time / size changed), reload shows the week | before / after hashes + shot |
| G9 | CAW create / save / load | `caw_save_test.ps1` on the private save; heavy CAW (`SVR2011_TEST_CAW_LIMITS`, 36 layers); a ported move on the CAW (CAM pack moves, MOD: rows) | save + reload equal; heavy CAW loads in a match; the move plays | shots + save hashes |
| G10 | 6-man with all mods (memory) | `--p1..--p6` six mod superstars, `--mods` all bundles, bigmem on | no block-heap / bank-drop error; 60 s of match | log + shots |
| G11 | Extreme Rules weapons | `OPTIONS -> WEAPONS` picks + ER match; `SVR2011_TEST_RULE` Weapons Everywhere; SvR08 arena crowd fight (`SVR2011_TEST_CROWD`, `SVR2011_TEST_GIVE_WEAPON`); quit an ER match with extra weapons | picks show; no freeze; crowd hand-off; quit returns to menu (we-quit-crash) | shots + log |
| G12 | online (private server only) | `svrtest lan --p1 EDGE --p2 KANE --play 30s` (player match); same with RANKED; host with mods on while the server's `mods_online` is off (mods block); a relay-routed match (`SVR2011_TEST_NET_NO_DIRECT`) for the relay warning | lockstep with no desync; match_kind PLAYER/RANKED in the server's report; mods block message; relay warning once | both logs, server log, shots |

## 3. Settings

| id | proves | how | pass | proof |
|---|---|---|---|---|
| S1 | every option on our pages works | per option: `svrtest match ... --arg "--<cvar>=<value>"` (AA modes, DLSS, FSR 1, FSR 2, render scale, CPU tab levels / LEGEND+, WEAPONS, BACKGROUNDS, RING MAT, ARENA BRIGHTNESS, match camera, perf overlay, unlock_everything, full_speed) | each value takes effect (log line or visible in the shot), no crash | a shot per value |
| S2 | renderers | G1 with `--backend d3d12`, `vulkan`, `d3d11` | all three draw the match | shots |
| S3 | frame rates | G1 at 60 and 30 (`--arg "--frame_rate=30"`) | 30 / 60 held (perf log) | fps lines |
| S4 | screen shapes | `SVR2011_TEST_OUTPUT_SIZE=1920x1200`, then `3440x1440` | 16:10 and 21:9 matches correct (no squeeze, HUD placed) | shots |
| S5 | settings files, PC | start with: no file; the 2.1.5 file; a file with an unquoted `native_upscale = fsr2`; a truncated file; a file with an unknown key | each loads; the broken line falls back alone; other settings kept; a save writes a valid file | toml before / after + log |
| S6 | settings files, Android | same files pushed to the Fold copy (only in an A run, section 7) | as S5 | as S5 |

## 4. Every fix since the last release

The list is built at candidate time: `git log --first-parent v<last>..<candidate>`, one row per game, launcher or
Mod Maker change. Each row is retested with the proof aid its branch added. Tools-only, docs-only and
dashboard/server merges get a "not in the release" mark and are skipped. Rows for 2.1.6 at this writing
(v2.1.5..87786a66):

| merge | retest |
|---|---|
| sabu-facebuster (saved slots w/o usable record) | Sabu save with an emptied slot -> mod's move back |
| moves-w12 / moves-2k15 a-c / grapple-freeze / missing-chain / stamp-13 | MOVE_PORTING.md hidden recipe: forced move per game (`SVR2011_TEST_FORCE_MOVE`, `FORCE_PLACE`), no stuck pair |
| moves-warning, move-tools, move-preview, movecheck-ymbs | Mod Maker `--move-check` on a 2K15 pack: warning shows |
| user-movie-stale / user-movie-ids | mod swapped into a slot: new mod's entrance + movie |
| cam-load-stall | CREATE A MOVE-SET opens (load watch quiet) |
| weapon-picks-fix, guest-fault-log, we-quit-crash, crowd-weapons, crowd-handoff, crowd-fight2, light-tube-break | G11 |
| match-blackbox, whip-fall | match report written; G1 log |
| movie-shrink / movie-shrink-tools | L4 |
| hidden-window | every run (0 visible windows: vis_watch.ps1 once) |
| cpu-difficulty | S1 CPU tab |
| mankind-210, lowm*, ps2-svr11, old/svr09/star/load-pics, arena-thumbs/scan-name/select-art | one convert per game (L7) + loading picture shot |
| pad-icons-stale | M3 |
| online-route, relay-warning, online-mods-block, report-send, reports-sig, bug-reports | G12 + L5 |
| bundled-off | L1: bundled match-type mods install off |
| rtwm-cond, rtwm-cut | G6 + Cena week 2 (`SVR2011_TEST_EVENT_LOG`) |
| threads-distort | Threads edit + bake shot |
| skin-mv, dlss-vk | S1 FSR 2 / DLSS on D3D12 + Vulkan |
| vs-style / vs-style-0809 | VS screen shot for a 2008, 2009, 2010 arena |
| arena-limits, arena-tools, bigmem, reduce-even, arena-motion-bank, tpose-extras | G4 with an over-limit arena; announcer not T-posed |
| call-gain(-ui) | ring announcer call heard level (log gain) |
| hctp-bike-facing | HCTP biker entrance shot |
| tsoh-back | HANDICAP / 6-MAN > THREE STAGES OF HELL, B backs out |
| cae-advanced-dlc | CAE advanced popup for a mod |
| tall-cut, res8k, ring-mat, settings-quote | S4, S1, S5 |
| mod-slot-handover, mm-saveload, materials-multiselect | Mod Maker round-trip test (`--test-roundtrip`) |
| touch icons (PR #17) | Android A run |

## 5. Launcher + Mod Maker (CLI only)

| id | proves | how | pass | proof |
|---|---|---|---|---|
| L1 | mods install / enable | `svr2011_launcher.exe --game <dir> --mods-add <zip> --yes`, `--bundled`, then `--mods-remove` | Mods\ holds / drops the mod; bundled match types off | dir listing + log |
| L2 | Start Without Mods | the launcher's no-mods start (game arg it passes) on a folder with mods | game log shows no mods mounted | log |
| L3 | update / repair helpers | `--verify`, `--repair-cli` on a copy with one pac damaged | damage found and fixed | output |
| L4 | Shrink movies | `--mods-shrink <dir>` on a mod with a big .bik / .mp4 | smaller mp4, still plays (G3 entrance) | sizes + shot |
| L5 | report send (private server) | `--report-send` and Mod Maker `--test-report-send` to `test_server.ps1` | report lands in the server's Reports with a signature | server listing |
| L6 | Mod Maker pages | `SvR2011 Mod Maker.exe --page <name> --shot <png>` for every page | every page draws, no error panel | one shot per page |
| L7 | one convert per game | Tools Python converters `list` + one superstar convert each (2K14-2K17, '12, '13, SvR 06-10, HCTP, LOWM, PS2) | .svrmod made; `--check-svrmod` clean | outputs |
| L8 | move convert + warning | `--moves-pack` a 2K15 move; `--move-check` | pack made; the new warning shows for the open moves | output |
| L9 | export + Doctor | `--test-project`, `--test-roundtrip`, `--check-mods`, `--tools-panel --shot` | round-trip equal; Doctor lists no false problems | output + shot |

## 6. Install / update

| id | proves | how | pass | proof |
|---|---|---|---|---|
| I1 | fresh install as a player gets it | the exact zip into `port\runs\clean_test\<version>`; `--install <iso> <dir>`, `--vcrt`, `--setup-check`, `--setup-install all --yes`; G1 there with `-GameDir`; Mod Maker shot; converters with the Tools Python | all pass; package.ps1 warnings read; no loader error | outputs + shots |
| I2 | 2.1.5 -> candidate keeps saves, settings, mods | a private 2.1.5 install with saves / toml / mods; `--update-zip <cand zip>` | same save hashes, same toml keys, mods still enabled; game starts | before / after hashes |
| I3 | the Updater | the 2.1.5 launcher `--update <copy> --from 2.1.5` against a local copy of the release assets (after publish: from GitHub) | reaches the candidate version; no "update didn't finish" | output |

## 7. Android (the user's Fold, only when the user says it's free)

The safe-testing recipe in memory svr2011-android-sweep-1021 applies: back up first, use the installed version
code (`build_apk.py --version-code`), restore file times, delete test logs and overlays after.
`svrtest_android.ps1 -Boot match -Match "..." -Shot -State -Quit` for G1, G3, S6, the touch icons (PR #17),
online with `adb reverse`.

## 8. Performance

`opt_bench.ps1 -Name rel_<version> -Game <cand>` and `-Name rel_<prev> -Game <prev release folder>`, run back to
back with nothing else running (2 runs each). Any phase over 5% slower in average fps, or worse in p99 / frames over
51 ms, is flagged. Proof: the rows from runs\opt_results.md.

## 9. Output

One page, `port\runs\release_tests\<version>\RESULTS.html`, with a pass / fail / skipped row per test id, the proof
shots, the failing log lines and the skip reasons. It goes to the user and Main before publishing. Published as a
private artifact for the user.
