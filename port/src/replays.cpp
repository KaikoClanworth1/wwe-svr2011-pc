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

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
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
  if (!REXCVAR_GET(replays)) Wr32(base + step + 788, 1);  // (no highlights: done)
}
