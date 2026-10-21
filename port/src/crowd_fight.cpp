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
#include <cstdlib>
#include <string>

#include <rex/logging.h>

#include "arena_mods.h"

namespace {

constexpr uint32_t kLive = 0x82E3DE00;      // the match's settings: +0 its rule, +64 its arena
constexpr uint32_t kExtremeFirst = 0x4D, kExtremeLast = 0x50;
constexpr uint32_t kBackstageArena = 78;
constexpr uint32_t kMatchCollision = 996, kCrowdCollision = 999;
constexpr uint32_t kLoaderFirst = 0x82187258, kLoaderEnd = 0x821874C8;  // sub_82187258

std::atomic<bool> g_crowd{false};

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

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

bool CrowdMatch() { return g_crowd.load(); }

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
