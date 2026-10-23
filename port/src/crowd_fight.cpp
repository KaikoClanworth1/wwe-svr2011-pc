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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/format.h>

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
// (each collision's z-band index: the entry before it - 995 for 996, 997 for 999; svr08_stage.index_build)
constexpr uint32_t kMatchIndex = 995, kCrowdIndex = 997;
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
void WrF(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Wr32(p, v);
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

// A weapon lying at (x, z) - on the floor, or the ring's mat: sub_821D2070(any free slot, position, rotation, model;
// stack: motion 15010 lying, place, flag 1) - as a backstage room's (hctp_room.cpp). The object, or 0 (no free slot,
// or the model isn't loaded).
uint32_t SpawnWeapon(PPCContext& ctx, uint8_t* base, int model, float x, float z, bool ring) {
  const auto saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  Wr32(base + ctx.r1.u32 + 84, 15010);
  Wr32(base + ctx.r1.u32 + 92, ring ? 0 : 1);  // (place: 0 the ring, 1 the floor)
  Wr32(base + ctx.r1.u32 + 100, 1);
  ctx.r3.u64 = 0xFFFFFFFFu;
  ctx.f1.f64 = x, ctx.f2.f64 = ring ? -12.0 : 0.0, ctx.f3.f64 = z;
  ctx.f4.f64 = 0, ctx.f5.f64 = 0, ctx.f6.f64 = 0;
  ctx.r7.u64 = uint32_t(model);
  sub_821D2070(ctx, base);
  const uint32_t made = ctx.r3.u32;
  ctx = saved;
  return made;
}

// Test aid: SVR2011_TEST_GIVE_WEAPON=<model>[,<match frame>[,<person>]] - once a match (frame 30), that weapon lies at
// the person's feet (0; 1 the CPU opponent, who goes for a weapon that near): a weapon's hits and breaks without walking
// to one. The model must be loaded.
void TestGiveWeapon(PPCContext& ctx, uint8_t* base, uint32_t frames, bool new_match) {
  static const char* give = std::getenv("SVR2011_TEST_GIVE_WEAPON");
  static bool given = false;
  if (!give) return;
  if (new_match) given = false;
  int model = 0, at = 30, person = 0;
  std::sscanf(give, "%d,%d,%d", &model, &at, &person);
  const uint32_t c = Rd32(base + kChars + uint32_t(std::clamp(person, 0, 5)) * 4);
  if (given || !c || frames < uint32_t(at) || model <= 0) return;
  given = true;
  const float x = RdF(base + c + 288), y = RdF(base + c + 292), z = RdF(base + c + 296);
  const float d = std::max(std::sqrt(x * x + z * z), 1.f);
  const float wx = x - x / d * 4.f, wz = z - z / d * 4.f;  // (a step toward the ring's middle)
  const uint32_t made = SpawnWeapon(ctx, base, model, wx, wz, y < -6.f);
  REXLOG_INFO("[svr2011] test: weapon {} at person {}'s feet ({:.0f},{:.0f}), frame {} - {}", model, person, wx, wz,
              frames, made ? "made" : "not made");
}

bool CrowdMatch() { return g_crowd.load(); }

void CrowdMoveAsked(uint32_t fighter, uint32_t slot) {
  if (slot < kTauntFirst || slot > kTauntLast || !fighter || !g_crowd.load()) return;
  for (auto& a : g_asked) {
    uint32_t was = 0;
    if (a.compare_exchange_strong(was, fighter) || was == fighter) return;
  }
}

// Test aids (crowd fighting): SVR2011_TEST_PLACE="<person>:<x>,<z>[;<person>:<x>,<z>...]" - at match frame
// SVR2011_TEST_PLACE_AT (300) those people are put there on the floor (the real position vector *(+304), its copy
// +288; y 0; the area +444 1, outside the ring), once a match; SVR2011_TEST_CROWD_LOG=1 - each half second people 0-3: place, area bytes (+441, +444),
// motion (+212).
void CrowdTestAids(uint8_t* base, uint32_t frames, bool new_match) {
  static const std::string place = [] { const char* v = std::getenv("SVR2011_TEST_PLACE"); return std::string(v ? v : ""); }();
  static const uint32_t place_at = [] { const char* v = std::getenv("SVR2011_TEST_PLACE_AT"); return uint32_t(v ? std::atoi(v) : 300); }();
  static const bool log = std::getenv("SVR2011_TEST_CROWD_LOG") != nullptr;
  static bool placed = false;
  if (new_match) placed = false;
  if (!place.empty() && !placed && frames >= place_at && g_crowd.load()) {  // (crowd matches only: not the demo)
    placed = true;
    for (size_t at = 0; at < place.size();) {
      size_t semi = place.find(';', at);
      if (semi == std::string::npos) semi = place.size();
      int person = -1;
      float x = 0, z = 0;
      if (std::sscanf(place.substr(at, semi - at).c_str(), "%d:%f,%f", &person, &x, &z) == 3 && person >= 0 && person < 6)
        if (const uint32_t c = Rd32(base + kChars + uint32_t(person) * 4)) {
          if (const uint32_t v = Rd32(base + c + 304)) WrF(base + v, x), WrF(base + v + 4, 0.f), WrF(base + v + 8, z);
          WrF(base + c + 288, x), WrF(base + c + 292, 0.f), WrF(base + c + 296, z);
          base[c + 444] = 1;  // (outside the ring: 0 is in it, 10 the cell's roof)
          REXLOG_INFO("crowd: test - person {} put at ({}, {})", person, x, z);
        }
      at = semi + 1;
    }
  }
  static uint32_t logged_at = ~0u;
  if (log && g_crowd.load() && frames && frames % 30 == 0 && frames != logged_at) {
    logged_at = frames;
    std::string line;
    for (uint32_t i = 0; i < 4; ++i)
      if (const uint32_t c = Rd32(base + kChars + i * 4))
        line += fmt::format(" | {}: ({:.1f}, {:.1f}, {:.1f}) area {} {} motion {}", i, RdF(base + c + 288),
                            RdF(base + c + 292), RdF(base + c + 296), base[c + 441], base[c + 444], Rd32(base + c + 212));
    REXLOG_INFO("crowd: frame {}{}", frames, line);
  }
}

void CrowdHandoffUpdate(PPCContext& ctx, uint8_t* base) {
  uint32_t asked_set[8];
  for (int i = 0; i < 8; ++i) asked_set[i] = g_asked[i].exchange(0);
  const uint32_t frames = Rd32(base + kMatchFrames);
  const bool new_match = frames < g_last_frames;
  CrowdTestAids(base, frames, new_match);
  if (new_match) {  // (a new match)
    g_given = g_turn = 0;
    for (auto& f : g_fighter_at) f = 0;
  }
  g_last_frames = frames;
  TestGiveWeapon(ctx, base, frames, new_match);
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
  const uint32_t made = SpawnWeapon(ctx, base, model, wx, wz, y < -6.f);
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
  if (lr < kLoaderFirst || lr >= kLoaderEnd) return id;
  static const bool log = std::getenv("SVR2011_TEST_CROWD_LOG") != nullptr;  // (the stage loader's look-ups, once each)
  if (log) {
    static uint64_t told = 0;
    if (id >= 990 && id < 1054 && !(told >> (id - 990) & 1)) {
      told |= 1ull << (id - 990);
      REXLOG_INFO("crowd: stage loader asks {} (lr {:08X})", id, lr);
    }
  }
  // (the index: asked right after the collision (996, then 995) - that look-up's decision, not a second one)
  if (id == kMatchIndex) return g_crowd.load() ? kCrowdIndex : id;
  if (id != kMatchCollision) return id;
  const uint32_t rule = base[kLive], arena = Rd32(base + kLive + 64);
  static const bool test = std::getenv("SVR2011_TEST_CROWD") != nullptr;
  const bool crowd = rule >= kExtremeFirst && rule <= kExtremeLast && arena != kBackstageArena &&
                     (test || CrowdArena());
  g_crowd = crowd;
  if (lr == 0x821872BC && crowd)  // (once a stage: its first look-up)
    REXLOG_INFO("crowd fight: the crowd area is open - SvR 2008's crowd collision (rule {:02X}, arena {})", rule, arena);
  // (the collision and its index together: 999's triangles looked up through 996's index found the wrong ones -
  // the crowd area's walls and floors were missing from it)
  return crowd ? kCrowdCollision : id;
}

}  // namespace svr2011
