// WWE SmackDown vs. Raw 2011 - instant replays off (the replays setting).
//
// The game has no option for them ("POST MATCH REPLAY" is an unused string).
// Two kinds, both skipped the way the game skips them itself:
// - Finisher replays: a finisher (move ids 3500-3649) creates a task
//   (sub_8230C140); each frame sub_8230C2D0 runs it: state 0 waits ~70
//   frames, then it takes the camera and the HUD and replays the move from a
//   few angles. Off, the task deletes itself while still waiting (state 0) -
//   what the game does when a finisher is interrupted (0x8230C33C) - before
//   it has touched anything.
// - The match-end highlights: the match flow's replay step (ctor
//   sub_82413698) picks up to 5 clips and plays them before the celebration.
//   With no clips the ctor sets its "done" flag (+788) and the step goes
//   straight on to the celebration and results; off, that flag is set.
//
// Stuck highlights: the step's update (sub_82413280) waits in state 1 until
// the replay recorder hands it the next clip (sub_82413170 ->
// sub_8217B550 -> sub_8219F9F0, which only succeeds while the recorder's
// state, +0, is 1 = idle). If the recorder never gets back to idle (seen on
// a Steam Deck: the match was won and never ended), state 1 retried forever
// with the match still on screen. After 8 s in state 1 (or 30 s in state 2,
// the wait before a clip; no scene is made yet in either) the step is done -
// the game's own "no highlights" path - and the recorder's state is logged.

#include <chrono>
#include <cstdint>
#include <cstdlib>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"

REXCVAR_DEFINE_BOOL(replays, true, "Gameplay", "Instant replays (after finishers and at the end of a match)");

namespace {
uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
}  // namespace

REX_EXTERN(__imp__sub_8230C2D0);
REX_EXTERN(__imp__sub_8230C248);
REX_HOOK_RAW(sub_8230C2D0) {
  if (!REXCVAR_GET(replays) && Rd32(base + ctx.r3.u32 + 80) == 0) {
    ctx.r4.u64 = 1;  // (delete)
    __imp__sub_8230C248(ctx, base);
    return;
  }
  __imp__sub_8230C2D0(ctx, base);
}

REX_EXTERN(__imp__sub_82413698);
REX_HOOK_RAW(sub_82413698) {
  const uint32_t step = ctx.r3.u32;
  __imp__sub_82413698(ctx, base);
  REXLOG_INFO("[svr2011] match-end highlights: {} clips", Rd32(base + step + 780));
  if (!REXCVAR_GET(replays)) Wr32(base + step + 788, 1);  // (no highlights: done)
}

namespace {
constexpr int64_t kStuckMs = 8000;       // state 1: no clip from the recorder
constexpr int64_t kStuckWaitMs = 30000;  // state 2: the wait before a clip
uint32_t g_step = 0, g_state = 0;  // the highlight step and its state last frame
int64_t g_since = 0;               // when it got there
uint32_t g_recorder_state = 0;     // the recorder's state at the last refused clip
uint32_t g_recorder = 0;           // and the recorder
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

// The recorder hands out a clip: sub_8219F9F0(recorder, clip), 0 = not now.
REX_EXTERN(__imp__sub_8219F9F0);
REX_HOOK_RAW(sub_8219F9F0) {
  const uint32_t recorder = ctx.r3.u32;
  // Test aid: SVR2011_TEST_HIGHLIGHT_STALL=1 - the recorder never hands out a clip.
  static const bool stall = std::getenv("SVR2011_TEST_HIGHLIGHT_STALL") != nullptr;
  if (stall) {
    g_recorder = recorder;
    g_recorder_state = Rd32(base + recorder);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8219F9F0(ctx, base);
  if (ctx.r3.u32 == 0) g_recorder = recorder, g_recorder_state = Rd32(base + recorder);
}

REX_EXTERN(__imp__sub_82413280);
REX_HOOK_RAW(sub_82413280) {
  const uint32_t step = ctx.r3.u32;
  const uint32_t state = Rd32(base + step + 776);
  const int64_t now = NowMs();
  if (step != g_step || state != g_state) {
    if (step == g_step || state != 0)
      REXLOG_INFO("[svr2011] match-end highlights: state {} (clip {} of {})", state, Rd32(base + step + 784),
                  Rd32(base + step + 780));
    g_step = step, g_state = state, g_since = now;
  } else if (((state == 1 && now - g_since > kStuckMs) || (state == 2 && now - g_since > kStuckWaitMs)) &&
             Rd32(base + step + 788) != 1) {
    REXLOG_WARN(
        "[svr2011] match-end highlights stuck in state {} for {} s (clip {} of {}; recorder {:08X} state {}, now {}, "
        "clips {}, stream {:08X}) - skipped",
        state, (now - g_since) / 1000, Rd32(base + step + 784), Rd32(base + step + 780), g_recorder, g_recorder_state,
        g_recorder ? Rd32(base + g_recorder) : 0, g_recorder ? Rd32(base + g_recorder + 8) : 0,
        g_recorder ? Rd32(base + g_recorder + 20) : 0);
    Wr32(base + step + 788, 1);  // (done: on to the celebration and results)
  }
  __imp__sub_82413280(ctx, base);
}
