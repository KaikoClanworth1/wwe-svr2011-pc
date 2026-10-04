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
#include <string>

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

// The match's end (logged, for reports of a won match that never ended):
// sub_82322DA0, each frame, steps the end - state (+16) 0: waits while the
// object of sub_821650F8 is busy (its +156 -> +1504 not 0 or 5); 1: the result;
// 2: waits for a fade (sub_8217A8D8(+8)), then the highlights step
// (sub_823EDC78 -> sub_82413698, logged above); 3-5: on to the results. Each
// change is logged, and a state still waiting after 10 s logs what it waits on.
REX_EXTERN(__imp__sub_82322DA0);
REX_HOOK_RAW(sub_82322DA0) {
  const uint32_t task = ctx.r3.u32;
  const auto saved = ctx;
  __imp__sub_82322DA0(ctx, base);
  using Clock = std::chrono::steady_clock;
  static uint32_t last_task = 0, last_state = ~0u;
  static Clock::time_point since;
  static bool told = false;
  const uint32_t state = Rd32(base + task + 16);
  if (task != last_task || state != last_state) {
    REXLOG_INFO("[svr2011] match end: step {} (task {:08X}, +20 {})", state, task, Rd32(base + task + 20));
    last_task = task, last_state = state, since = Clock::now(), told = false;
    return;
  }
  if (told || state == 3 || Clock::now() - since < std::chrono::seconds(8)) return;  // (3: the highlights play)
  told = true;
  // Step 0's object: sub_821650F8(**0x82E35468) (as the step does); its part
  // at +156 runs a handshake with the frame's helper thread (sub_826E1868):
  // +1504 the update's side (0/5 = done), +1508 the helper's, +1496 a buffer.
  uint32_t obj = 0, part = 0;
  if (const uint32_t p = Rd32(base + 0x82E35468)) {
    PPCContext c = saved;
    c.r3.u64 = Rd32(base + p);
    sub_821650F8(c, base);
    obj = c.r3.u32;
    if (obj) part = Rd32(base + obj + 156);
  }
  std::string queue;  // (the helper's callbacks, sub_826E0F78: both buffers)
  if (const uint32_t q = Rd32(base + 0x82F6D96C)) {
    queue = fmt::format(" write {} read {}:", Rd32(base + q + 40), Rd32(base + q + 44));
    for (uint32_t i = 0; i < 64; ++i) {
      const uint32_t e = q + 168 + i * 16;
      if (Rd32(base + e) || Rd32(base + e + 4))
        queue += fmt::format(" [{}.{} {:08X} {:08X} {:08X} {}]", i / 32, i % 32, Rd32(base + e), Rd32(base + e + 4),
                             Rd32(base + e + 8), Rd32(base + e + 12));
    }
  }
  // The replay worker (*0x82DE0368, interface +16 = the part's +1512): its
  // "done" (sub_82167768) wants both results (+48, +52); +152/+160 its queue.
  std::string worker;
  if (const uint32_t w = part ? Rd32(base + part + 1512) : 0)
    worker = fmt::format(", replay worker {:08X} (+32 {} +48 {:08X} +52 {:08X} +152 {} +160 {})", w, Rd32(base + w + 32),
                         Rd32(base + w + 48), Rd32(base + w + 52), Rd32(base + w + 152), Rd32(base + w + 160));
  REXLOG_WARN("[svr2011] match end: step {} waiting 8 s - +20 {}, object {:08X} (+56 {} +88 {} +112 {}), part {:08X} "
              "(+1504 {} +1508 {} +1496 {}), fade {} (global {:08X}); queue{}",
              state, Rd32(base + task + 20), obj, obj ? Rd32(base + obj + 56) : 0, obj ? Rd32(base + obj + 88) : 0,
              obj ? Rd32(base + obj + 112) : 0, part, part ? Rd32(base + part + 1504) : 0,
              part ? Rd32(base + part + 1508) : 0, part ? Rd32(base + part + 1496) : 0, Rd32(base + task + 8),
              Rd32(base + 0x82DEA30C), queue + worker);
  // Step 0 waits for the part to finish with the replay worker (+1504 4 ->
  // 5, or 6 -> 0, once the worker's "done"); on Android (and the Steam Deck: the
  // recorder above) the worker can stay unfinished after a win - the match
  // never ended. On, as the game would: 5 or 0 (the end then goes on; the
  // highlights' own fallback above covers a recorder that stays busy).
  const uint32_t side = part ? Rd32(base + part + 1504) : 0;
  if (state == 0 && (side == 4 || side == 6)) {  // (6: the same wait, then 0)
    Wr32(base + part + 1504, side == 4 ? 5 : 0);
    REXLOG_WARN("[svr2011] match end: the replay worker never finished - went on without it");
  }
}

// Test aid: SVR2011_TEST_REPLAY_WORKER_STALL=1 - the replay worker's "done"
// (sub_82167768) never true, as on the Fold 7 after a Steel Cage escape.
REX_EXTERN(__imp__sub_82167768);
REX_HOOK_RAW(sub_82167768) {
  static const bool stall = std::getenv("SVR2011_TEST_REPLAY_WORKER_STALL") != nullptr;
  __imp__sub_82167768(ctx, base);
  if (stall) ctx.r3.u64 = 0;
}
