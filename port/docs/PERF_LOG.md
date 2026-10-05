# Performance log (weak / busy hardware)

The Optimization work: make the game run as well as possible on slow or few
CPU cores, busy PCs, weak GPUs and phones without breaking anything. Every
change is measured on the same benchmark before and after, and kept only when
it is clearly better.

## The benchmark - tools/opt_bench.ps1

One scripted run in its own game folder (`port\runs\opt_bench`: the program
files of the build under test, the game data linked in), its own test saves
(`runs\test_userdata_opt`) and settings (`runs\test_config_opt.toml`, a fresh
install's defaults: native renderer, 1280x720, 60 fps), muted and off-screen.
It stops only its own game.

Route (the same every run):

| phase | what |
|---|---|
| title | boot (first 10 s left out), the title screen and its demo match (100 s after launch) |
| menus | `route normal` (main menu with its background match -> PLAY -> ONE ON ONE -> NORMAL MATCH), select screen, `SVR2011_TEST_MATCH="people=BATISTA,KANE arena=1"` |
| load+entr | the match load and both entrances (Batista's pyro is the heaviest: 3,000-5,000 draws a frame) |
| match | 60 s of the match (player 1 idle, Kane's AI attacking) |

Conditions:

- **normal**: the PC as it is (note the background load column).
- **weak**: the game kept on 2 physical cores (affinity 0x5; the runtime
  resets the process's affinity as it starts, so the script re-applies it
  every few seconds) plus one busy thread at normal priority on the same
  cores.
- **phone**: the Galaxy Z Fold 7 via tools/phone_session.ps1 (only when the
  Android session says the phone is free).

Measures (per phase and all): average fps, median and p99 frame time, frames
over 51 ms (a visible stutter: 3+ vblanks), frames over 20 ms, worst frame.
Builds with `SVR2011_FRAME_TIMES` (frame_stats.cpp) give every frame's time;
older builds only the 5 s "fps:" / "frame times:" lines (p99 = the worst 5 s
window's). Raw results: `port\runs\opt_results.md`; logs `runs\opt_<name>.log`.

Test PC: Ryzen 7 5700X3D (8 cores / 16 threads), RTX 4080 SUPER, game on an
HDD (D:). The user's PC is always in use: HandBrake encodes at normal priority
(~8 of 16 threads) during most runs, so runs are repeated and compared by
median.

## Results

All rows are a whole run (title + menus + load/entrances + match). "exact"
= every frame (SVR2011_FRAME_TIMES); Game Files rows: p99 is the worst 5 s
window's. Builds: **GF** = Game Files (c27d191-era combine), **cur** = main
2ac47b9 / df03183 + this work, settings off unless named.

### 2026-10-05 baseline

| run | build | condition | avg fps | p99 ms | frames >51 ms | frames >20 ms | worst ms | note |
|---|---|---|---|---|---|---|---|---|
| b2_gf_n1 | GF | normal | 59.4 | 48.0 (window) | 17 | 38 | 888 | PC 25% busy |
| b2_cur_n1 | cur | normal | 59.8 | 17.9 | 12 | 41 | 228 | PC 24% busy |
| b2_gf_w1 | GF | weak | 56.7 | 140.7 (window) | 35 | 1,509 | 247 | |
| b2_cur_w1 | cur | weak | 55.5 | 42.8 | 90 | 1,532 | 811 | |
| phone_202_1 | 2.0.2 (e9d5ef7) | Fold 7 | 58.4 | 49.3 (window) | 11 | 1,891 | 140 | thermal 0 before/after |

- Quiet PC: 60 fps; the 12-17 stutters a run are loads (title demo start,
  menu changes, match load, the match's start).
- Weak (2 cores + a busy normal-priority thread): ~1,500 frames over 20 ms a
  run; the game gets ~1.07 cores of the 2 (thread_cpu.ps1: render thread
  F800005C 25%, logic F800002C 25%, GPU Commands 16%, job thread F8000074
  12%, the runtime's TimerQueue 8% and GPU VSync 2-5% doing nothing useful).
- 1 crash in 4 weak runs of 2ac47b9 (render command on an object the second
  world update freed) - fixed by Limit Breaking's df03183; 6/6 weak runs of
  that fix clean (no crash, purecall, hang).

### process_priority = 1 (above-normal CPU priority) - KEPT, opt-in

| run | condition | avg fps | p99 ms | frames >51 ms | frames >20 ms | worst ms |
|---|---|---|---|---|---|---|
| prio_on_w1 | weak | **59.3** | **23.0** | **18** | **349** | 243 |
| prio_on_w2 | weak | **58.5** | **30.1** | **23** | **634** | 526 |
| prio_off_w1 | weak | 55.1 | 37.6 | 57 | 1,837 | 307 |
| prio_off_w2 | weak | 54.3 | 41.0 | 61 | 2,130 | 332 |
| prio_on_n1 | normal | 59.7 | 17.9 | 12 | 37 | 214 |
| prio_on_n2 | normal | 59.3 | 17.8 | 15 | 34 | 1,291 |
| prio_off_n1 | normal | 59.8 | 18.0 | 12 | 44 | 197 |

Entrances on the weak setup: ~50 -> 56-58 fps. A normal PC: no change.
Windows only (SetPriorityClass ABOVE_NORMAL). Opt-in (the user runs other
programs beside the game; at above-normal the game wins over them).

### timer_sleep (runtime TimerQueue sleeps instead of spin/yield) - REVERTED

The TimerQueue thread spins because the kernel's 1 ms KeTimeStampBundle timer
is always armed. Sleeping on a high-resolution waitable timer until the next
due time made that thread use MORE CPU: 14.5% / 16.2% of a core vs 8.7% / 7.9%
with the stock spin/yield (thread_cpu.ps1, title, 15 s); fps on the weak setup
within noise (58.0 / 56.9 on vs 57.0 / 57.5 off). Not kept.

### power_throttling_off - dropped untested

Windows only throttles a background / hidden window's process (EcoQoS, coarse
timers): nothing for a player playing it.
