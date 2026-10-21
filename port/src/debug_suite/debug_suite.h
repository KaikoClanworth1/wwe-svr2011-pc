// WWE SmackDown vs. Raw 2011 - the debug / test suite (docs/DEBUG_SUITE_PLAN.md).
//
// For us only: compiled into svr2011_dbg.exe (CMake option SVR2011_DEBUG_SUITE, its own build folder
// out/build/DebugSuite*), never into a release - tools/release_guard.ps1 (package.ps1) fails a package that holds
// a debug-suite file or the kMarker bytes. Everything here is inside src/debug_suite/ or #if SVR2011_DEBUG_SUITE.
//
// The cues (settings: --dbg_shot=5,10 on the command line; times in seconds from dbg_from's zero):
//   dbg_from=match|start     the zero: the match's start (the replay recorder recording - after the entrances
//                            when they play; from zero again each match) or the game's start
//   dbg_shot=5,10,every:2:5  shots of the frame image, read back from the GPU (works hidden / minimised): at
//                            5 s, 10 s and every 2 s five times -> <dbg_out>/shot_<ms>.png
//   dbg_state=8              state dumps -> <dbg_out>/state_<ms>.json (the match's people with their last
//                            moves and positions, the match clock, fps, the perf numbers)
//   dbg_quit=15              the game ends itself (exit code 0) at 15 s, after the cues before it
//   dbg_speed=N              N world updates a frame (fast-forward; 1 normal; online lockstep keeps 1)
//   dbg_out=<folder>         where shots and states go (default: dbg in the working folder)
//   dbg_boot=match           straight into dbg_match's match: no intros, no title, no menus (boot.cpp)
//   dbg_match="people=EDGE,KANE attires=1,1 arena=1 rule=0 cpu=all"   (names as the "match:" lines; rule hex)
//   dbg_entrances=true       a booted match plays its entrances (default: skipped, A on pad 1 while they run)
// Each cue logs a "SVRDBG: ..." line (svrtest reads them).

#pragma once

#if !SVR2011_DEBUG_SUITE
#error "debug_suite.h is only for the debug-suite build (SVR2011_DEBUG_SUITE)"
#endif

#include <cstdint>

struct PPCContext;

namespace rex::memory {
class Memory;
}

namespace svr2011::dbg {

// The marker every debug-suite build carries (the release guard looks for its first bytes, "SVRDBG:").
extern const char kMarker[];

// At start (svr2011_app.cpp): logs the marker and the cues, starts the cue thread.
void Install(rex::memory::Memory* memory);

// The renderer (native_renderer.cpp OnPresent): a shot is due - this frame's image is read back and handed to
// WriteShot (RGBA8 rows `row_bytes` apart).
bool ShotWanted();
void WriteShot(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t row_bytes);

// frame_rate.cpp: the match clock started running (the replay recorder recording; dbg_from=match's zero), and
// the world updates this frame (dbg_speed; 0: the normal rule).
void OnMatchStarted();
int SpeedTicks();

// The direct boot (boot.cpp): set up at start (Install); fast_start.cpp presses START on the Start Screen too
// while one is wanted; frame_rate.cpp's world update (the game's thread) starts it once the main menu is up.
void InstallBoot();
bool BootWanted();
void BootFrame(PPCContext& ctx, uint8_t* base);
// menu_hooks.cpp: the entrances start / end; frame_rate.cpp: pad 1's state as the game reads it (A while
// a booted match's entrances run).
void OnEntrances(bool on);
void BootInput(uint8_t* base, uint32_t state);

}  // namespace svr2011::dbg
