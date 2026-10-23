// WWE SmackDown vs. Raw 2011 - SvR 2008's crowd fighting, on arenas ported from SvR 2008.
//
// Every SvR 2008 arena's stage has two collision sets: HMD 996 (the match's) and HMD 999, the same with the back
// barricade (z -85.4, x -45..45) and two pieces at the ramp opened, outer walls at z -112 and a stepped corner of
// the stands (x 60..177, z -112..-58: risers of 10, a platform 38 high) - the crowd area. The arena port keeps
// both; 2011 only ever loads 996 (sub_82187258: ids 996 + 2k, one for an arena, eight for the backstage rooms).
// In an Extreme Rules match (rules 0x4D-0x50) on such an arena the loader is handed 999 instead.
// Which arenas: the custom arena in use, when its manifest says crowd=1 or it was ported from SvR 2008 (its
// author line) - crowd=0 turns it off. Every game in an online lobby plays the same arena mod and rule, so
// their collision stays the same. Test aid: SVR2011_TEST_CROWD=1 - any Extreme Rules match (an arena served
// by SVR2011_TEST_ARENA_REDIRECT has no mod to read).

#include "crowd_fight.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "arena_mods.h"
#include "weapons_page.h"

#include "generated/default/svr2011_init.h"

namespace {

constexpr uint32_t kLive = 0x82E3DE00;      // the match's settings: +0 its rule, +64 its arena
constexpr uint32_t kExtremeFirst = 0x4D, kExtremeLast = 0x50;
constexpr uint32_t kBackstageArena = 78;
constexpr uint32_t kMatchCollision = 996, kCrowdCollision = 999;
constexpr uint32_t kLoaderFirst = 0x82187258, kLoaderEnd = 0x821874C8;  // sub_82187258

std::atomic<bool> g_crowd{false};

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
float RdF(const uint8_t* p) {
  const uint32_t v = Rd32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// The fan's hand-off (crowd_fight.h). People: *0x82E3CC50 + 4i (+288 / +292 / +296 x y z: the floor y 0, the ring
// -12). A taunt asked (slots 9-12) by one of them on the floor at the barricade (beyond 62 from the ring's middle on
// x or z; the crowd area behind it too) - once per kCooldown match frames each, kMax a match.
constexpr uint32_t kChars = 0x82E3CC50, kMatchFrames = 0x82E3CD0C;
constexpr uint32_t kTauntFirst = 9, kTauntLast = 12;
constexpr uint32_t kCooldown = 600, kMax = 8;
constexpr float kBarricade = 62.f;
// The weapons handed over, in turn: models every Extreme Rules match makes (the trash can and guitar of its nine:
// OPTIONS -> WEAPONS picks in their place).
constexpr int kHanded[] = {4, 63, 72, 89, 28, 102};
std::atomic<uint32_t> g_asked[8];  // (the fighters whose taunt was asked this update - the update's jobs run on
                                   // several threads: a set, handled in person order after it)
uint32_t g_last_frames = 0, g_given = 0, g_turn = 0;
uint32_t g_fighter_at[6] = {};  // (the match frame of each person's last hand-off + 1)

bool HasKey(const std::string& text, const std::string& line) {
  size_t at = 0;
  while ((at = text.find(line, at)) != std::string::npos) {
    if (at == 0 || text[at - 1] == '\n') return true;
    at += line.size();
  }
  return false;
}

// The custom arena in use opens its crowd area.
bool CrowdArena() {
  const std::string manifest = svr2011::ArenaModManifest();
  if (manifest.empty() || HasKey(manifest, "crowd=0")) return false;
  return HasKey(manifest, "crowd=1") || manifest.find("SvR 2008 arena") != std::string::npos;
}

}  // namespace

namespace svr2011 {
void Handoff(PPCContext& ctx, uint8_t* base, int person, uint32_t asked, uint32_t frames);

bool CrowdMatch() { return g_crowd.load(); }

void CrowdMoveAsked(uint32_t fighter, uint32_t slot) {
  if (slot < kTauntFirst || slot > kTauntLast || !fighter || !g_crowd.load()) return;
  for (auto& a : g_asked) {
    uint32_t was = 0;
    if (a.compare_exchange_strong(was, fighter) || was == fighter) return;
  }
}

void CrowdHandoffUpdate(PPCContext& ctx, uint8_t* base) {
  uint32_t asked_set[8];
  for (int i = 0; i < 8; ++i) asked_set[i] = g_asked[i].exchange(0);
  const uint32_t frames = Rd32(base + kMatchFrames);
  if (frames < g_last_frames) {  // (a new match)
    g_given = g_turn = 0;
    for (auto& f : g_fighter_at) f = 0;
  }
  g_last_frames = frames;
  if (!g_crowd.load()) return;
  for (int person = 0; person < 6 && g_given < kMax; ++person) {
    const uint32_t c = Rd32(base + kChars + uint32_t(person) * 4);
    if (c && std::find(std::begin(asked_set), std::end(asked_set), c) != std::end(asked_set)) Handoff(ctx, base, person, c, frames);
  }
}

void Handoff(PPCContext& ctx, uint8_t* base, int person, uint32_t asked, uint32_t frames) {
  if (g_fighter_at[person] && frames < g_fighter_at[person] - 1 + kCooldown) return;
  const float x = RdF(base + asked + 288), y = RdF(base + asked + 292), z = RdF(base + asked + 296);
  // (test aid: SVR2011_TEST_HANDOFF_ANYWHERE=1 - wherever they taunt, the ring too)
  static const bool anywhere = std::getenv("SVR2011_TEST_HANDOFF_ANYWHERE") != nullptr;
  if (!anywhere && (y < -6.f || (std::fabs(x) < kBarricade && std::fabs(z) < kBarricade))) return;  // (not by it)
  // At their feet, a step toward the ring's middle (out of the barricade).
  const float d = std::max(std::sqrt(x * x + z * z), 1.f);
  const float wx = x - x / d * 6.f, wz = z - z / d * 6.f;
  const int model = svr2011::ExtremeWeaponsPlacedModel(1, kHanded[g_turn % std::size(kHanded)]);
  // A weapon lying there: sub_821D2070(any free slot, position, rotation, model; stack: motion 15010 lying, place 1
  // floor, flag 1) - as a backstage room's (hctp_room.cpp).
  const auto saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  Wr32(base + ctx.r1.u32 + 84, 15010);
  Wr32(base + ctx.r1.u32 + 92, y < -6.f ? 0 : 1);  // (place: 0 the ring, 1 the floor)
  Wr32(base + ctx.r1.u32 + 100, 1);
  ctx.r3.u64 = 0xFFFFFFFFu;
  ctx.f1.f64 = wx, ctx.f2.f64 = y < -6.f ? -12.0 : 0.0, ctx.f3.f64 = wz;  // (the floor; in the ring its mat)
  ctx.f4.f64 = 0, ctx.f5.f64 = 0, ctx.f6.f64 = 0;
  ctx.r7.u64 = uint32_t(model);
  sub_821D2070(ctx, base);
  const uint32_t made = ctx.r3.u32;
  ctx = saved;
  ++g_turn;
  g_fighter_at[person] = frames + 1;
  if (made) ++g_given;
  REXLOG_INFO("[svr2011] crowd: a fan hands person {} a weapon (model {}) at ({:.0f},{:.0f}), frame {} - {}", person, model,
              wx, wz, frames, made ? "made" : "not made (no free slot)");
}

bool CrowdAreaFor(uint8_t* base) {
  static const bool test = std::getenv("SVR2011_TEST_CROWD") != nullptr;
  const uint32_t rule = base[kLive], arena = Rd32(base + kLive + 64);  // (the rule: the settings' first byte)
  return rule >= kExtremeFirst && rule <= kExtremeLast && arena != kBackstageArena && (test || CrowdArena());
}

uint32_t CrowdCollisionId(uint8_t* base, uint32_t lr, uint32_t id) {
  if (lr < kLoaderFirst || lr >= kLoaderEnd || id != kMatchCollision) return id;
  const uint32_t rule = base[kLive], arena = Rd32(base + kLive + 64);
  static const bool test = std::getenv("SVR2011_TEST_CROWD") != nullptr;
  const bool crowd = rule >= kExtremeFirst && rule <= kExtremeLast && arena != kBackstageArena &&
                     (test || CrowdArena());
  g_crowd = crowd;
  if (lr == 0x821872BC && crowd)  // (once a stage: its first look-up)
    REXLOG_INFO("crowd fight: the crowd area is open - SvR 2008's crowd collision (rule {:02X}, arena {})", rule, arena);
  return crowd ? kCrowdCollision : id;
}

}  // namespace svr2011
