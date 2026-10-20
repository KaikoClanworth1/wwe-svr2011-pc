// WWE SmackDown vs. Raw 2011 - roster rules the port lifts.
//
// Divas and male superstars in one match: character select only lets a pick
// join when it has the gender of the ones already picked. sub_82734CA0
// (match, slot) - may this character take this slot - asks sub_827336A0
// (match, -1, a, b) for the gender the picks so far share (0 male, 1 diva,
// 2 none / mixed) and refuses a pick of the other one; with a diva picked the
// select screen also switches to the Divas roster and greys the SUPERSTARS
// tile (sub_8244C4F8 tries both genders through sub_82734CA0). Answering
// "none" lets either gender join. The engine plays mixed matches already
// (Mixed Tag: men and divas in one match), and nothing refuses to start one.
// (Divas title matches keep their own rule: sub_824518A8.)
//
// In the match itself the game still treats a man and a diva as people who
// don't fight: a character's gender is byte +504 of its data (character +2588
// - the superstar's own data, so a mod diva or a female CAW is a diva too).
// - The referee (sub_8221E4C8 on a hit, with DQs on): a man hitting a diva,
//   sub_8221E420(attacker, victim), is disqualified ("WINS BY WAY OF DQ"); the
//   same test turns a man's submission hold on a diva into a break
//   (sub_82219830).
// - The CPU: its behaviour script asks the gender of itself (AI +0, its
//   person) and of its target (AI +20) - sub_827F99E8 is male, sub_827F9A10 is
//   a diva - and "a man facing a diva" (sub_8280F9A8), and branches away from
//   fighting her; its "nearest opponent" action sub_82804C00 only looks at men.
// With mixed_gender_matches (and always online, where every game must play the
// same rules) none of that applies: the hit isn't a DQ, the hold can win, and
// the CPU sees its target as the same kind of opponent as itself (its own
// gender) and picks its targets from everyone. Everything else that reads the
// gender stays: moves, motions, voices and commentary, and the game's own
// MIXED TAG match (live +61), whose rules are the point of it.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "generated/default/svr2011_init.h"
#include "p2p.h"

REXCVAR_DEFINE_BOOL(mixed_gender_matches, true, "Gameplay",
                    "Divas and male superstars may be picked for the same match");

REX_EXTERN(__imp__sub_827336A0);
REX_HOOK_RAW(sub_827336A0) {
  if (REXCVAR_GET(mixed_gender_matches)) {
    ctx.r3.u64 = 2;  // (no shared gender: anyone may join)
    return;
  }
  __imp__sub_827336A0(ctx, base);
}

namespace {

constexpr uint32_t kLive = 0x82E3DE00, kChars = 0x82E3CC50, kMatchFrames = 0x82E3CD0C;
constexpr uint32_t kData = 2588, kGender = 504;  // (character +2588 -> its data; +504: 1 diva)
constexpr uint32_t kMixedTag = 61;               // (live +61: the MIXED TAG match's own rules)

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// Men and divas fight as anyone does in this match.
bool SameRules(const uint8_t* base) {
  if (base[kLive + kMixedTag]) return false;
  return REXCVAR_GET(mixed_gender_matches) || svr2011::P2POnlineMode();
}

// (each kind of change, the first few times: the log shows what happened - from the game's thread and
// the AI's)
enum Kind { kReferee, kHold, kCpu, kTarget };
void Note(Kind kind, uint32_t a, uint32_t b, const uint8_t* base) {
  static std::atomic<int> count[4] = {};
  static const char* kNames[4] = {"referee (no DQ)", "hold (no forced break)", "cpu (fights her / him)",
                                  "cpu target (anyone)"};
  if (++count[kind] <= 5)
    REXLOG_INFO("mixed gender: {} - {:08X} and {:08X} as any two opponents (match frame {})", kNames[kind], a, b,
                Rd32(base + kMatchFrames));
}

}  // namespace

// sub_8221E420(attacker, victim): 1 when a man hits a diva (the referee's DQ, a hold's break).
REX_EXTERN(__imp__sub_8221E420);
REX_HOOK_RAW(sub_8221E420) {
  const uint32_t a = ctx.r3.u32, v = ctx.r4.u32, lr = uint32_t(ctx.lr);
  __imp__sub_8221E420(ctx, base);
  if (ctx.r3.u32 && SameRules(base)) {
    Note(lr > 0x8221E4C8 && lr < 0x8221E560 ? kReferee : kHold, a, v, base);  // (sub_8221E4C8: the referee)
    ctx.r3.u64 = 0;
  }
}

// sub_8280F9A8(ai, a, b): the CPU's "a man facing a diva".
REX_EXTERN(__imp__sub_8280F9A8);
REX_HOOK_RAW(sub_8280F9A8) {
  const uint32_t a = ctx.r4.u32, b = ctx.r5.u32;
  __imp__sub_8280F9A8(ctx, base);
  if (ctx.r3.u32 && SameRules(base)) {
    Note(kCpu, a, b, base);
    ctx.r3.u64 = 0;
  }
}

// sub_827F99E8 / sub_827F9A10(ai, character): the CPU asks whether someone is a man / a diva - about
// anyone but itself (its target), it gets its own answer.
namespace {
void AskAsSelf(PPCContext& ctx, uint8_t* base) {
  const uint32_t ai = ctx.r3.u32, who = ctx.r4.u32;
  if (!ai || !who || !SameRules(base)) return;
  const uint32_t self = Rd32(base + ai);
  if (!self || self == who || !Rd32(base + self + kData) || !Rd32(base + who + kData)) return;
  if (base[Rd32(base + self + kData) + kGender] != base[Rd32(base + who + kData) + kGender]) {
    Note(kCpu, self, who, base);
    ctx.r4.u64 = self;
  }
}
}  // namespace

REX_EXTERN(__imp__sub_827F99E8);
REX_HOOK_RAW(sub_827F99E8) {
  AskAsSelf(ctx, base);
  __imp__sub_827F99E8(ctx, base);
}

REX_EXTERN(__imp__sub_827F9A10);
REX_HOOK_RAW(sub_827F9A10) {
  AskAsSelf(ctx, base);
  __imp__sub_827F9A10(ctx, base);
}

// sub_82804C00(ai): the CPU's target (AI +2728) = the nearest person in the match (a wrestler, manager or
// valet still in it, role +2624 0 / 2 / 3, state +446 0) - but only men. The same here with everyone (done
// here, not by hiding the divas' gender for the call: the AI runs on its own thread).
REX_EXTERN(__imp__sub_82804C00);
REX_HOOK_RAW(sub_82804C00) {
  if (!SameRules(base)) return __imp__sub_82804C00(ctx, base);
  const uint32_t ai = ctx.r3.u32, self = Rd32(base + ai);
  auto rdf = [&](uint32_t a) {
    const uint32_t v = Rd32(base + a);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  };
  float nearest = rdf(0x82069900);  // (the game's limit)
  int32_t best = -1;
  bool diva = false;
  for (uint32_t i = 0; i < 9; ++i) {
    const uint32_t c = Rd32(base + kChars + i * 4);
    if (!c || c == self) continue;
    const uint32_t role = Rd32(base + c + 2624);
    if ((role != 0 && role != 2 && role != 3) || base[c + 446]) continue;
    const float dx = rdf(self + 288) - rdf(c + 288), dz = rdf(self + 296) - rdf(c + 296);
    const float d = dx * dx + dz * dz;
    if (d < nearest) {
      nearest = d, best = int32_t(i);
      const uint32_t data = Rd32(base + c + kData);
      diva = data && base[data + kGender];
    }
  }
  if (best >= 0) {
    const uint32_t v = uint32_t(best);
    for (uint32_t k = 0; k < 4; ++k) base[ai + 2728 + k] = uint8_t(v >> (24 - 8 * k));
    if (diva) Note(kTarget, self, Rd32(base + kChars + v * 4), base);
  }
}
