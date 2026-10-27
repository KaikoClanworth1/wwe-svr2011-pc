// WWE SmackDown vs. Raw 2011 - Road to WrestleMania's Streak story ("vs Undertaker") played as a superstar mod.
//
// The story is for Created Superstars only: its select is a list menu of Create-a-Superstar slots (sort_filters.cpp
// keeps the game's lists there), and the story keeps the player as that CAW slot throughout - its saves, events and
// cutscenes. Here, in Road to WrestleMania, the MODS category of that list shows the superstar mods; picking one
// hands the story a CAW (the list's first) so it starts, and the new story remembers the mod (UserData/
// rtwm_streak.txt: the mod's folder and the CAW slot the story got - kept for CONTINUE). In its matches, the person
// who is that CAW becomes the mod (the match's people, test_match.cpp's hook: RtwmStreakPeople). Undertaker stays
// himself. Scenes with the player alone (the backstage walks) keep the CAW: with the mod there the walk never loaded
// (its loader spun at the night-drive scene). Picking a CAW plays the story as it is.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "rtwm_streak.h"
#include "superstar_mods.h"

namespace svr2011 {
bool InRoadToWrestleMania();  // (menu_hooks.cpp: the main menu's last choice)
}

namespace {

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
void Wr16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
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
uint32_t g_solo_frames = 0;  // (world frames since the last scene with the player alone was set up; 0: none)

std::filesystem::path StateFile() {
  std::filesystem::path dir = rex::filesystem::GetExecutableFolder() / "UserData";
  if (const char* v = std::getenv("SVR2011_USER_DATA"); v && *v) dir = v;
  return dir / "rtwm_streak.txt";
}

// The choice: the mod (its folder) and the CAW slot the story was given (-1: none - the story as it is).
struct Choice {
  std::string folder;
  int caw = -1;
};
Choice& State() {
  static Choice c = [] {
    Choice r;
    std::ifstream in(StateFile());
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("mod=", 0) == 0) r.folder = line.substr(4);
      if (line.rfind("caw=", 0) == 0) r.caw = std::atoi(line.c_str() + 4);
    }
    if (!r.folder.empty() && r.caw >= 0) REXLOG_INFO("[svr2011] rtwm streak: plays as the mod {} (CAW slot {})", r.folder, r.caw);
    return r;
  }();
  return c;
}
void Save(const Choice& c) {
  std::error_code ec;
  std::filesystem::create_directories(StateFile().parent_path(), ec);
  std::ofstream out(StateFile(), std::ios::trunc);
  out << "# Road to WrestleMania, the Streak story played as a superstar mod (the game writes this)\n";
  if (!c.folder.empty()) out << "mod=" << c.folder << "\ncaw=" << c.caw << "\n";
}

// The mod's id now (a mod keeps its id, slots.txt; 0: not installed / off).
uint32_t ModNow(const std::string& folder) {
  if (folder.empty()) return 0;
  for (uint32_t id : svr2011::SuperstarModIds(false))
    if (svr2011::SuperstarModFolder(id) == folder) return id;
  return 0;
}

uint32_t g_first_caw = 0;  // (the Streak list's first CAW slot: what the story gets for a mod)
bool g_cas_list = false;    // (the list shown is the Streak's - Create-a-Superstar slots)
// The select's last pick, for the new story (taken at its first people): the mod's folder, or "" for a CAW. Picks
// only browse until then - the select's own pick of its first entry as it opens too.
bool g_pending = false;
std::string g_pending_folder;

}  // namespace

namespace svr2011 {

void RtwmCasList(uint32_t first_caw, bool on) {
  g_cas_list = on;
  if (on) g_first_caw = first_caw;
}

void RtwmStreakPeople(uint8_t* base, uint32_t match) {
  if (!InRoadToWrestleMania()) return;
  Choice& c = State();
  if (g_pending) {  // (a new story: the select's last pick)
    g_pending = false;
    if (g_pending_folder != c.folder || (!c.folder.empty() && c.caw != int(g_first_caw))) {
      c.folder = g_pending_folder, c.caw = c.folder.empty() ? -1 : int(g_first_caw);
      Save(c);
      if (c.folder.empty()) REXLOG_INFO("[svr2011] rtwm streak: a new story with a CAW - played as it is");
      else REXLOG_INFO("[svr2011] rtwm streak: a new story - CAW slot {} plays as the mod {} in its matches", c.caw, c.folder);
    }
  }
  if (c.caw < 0 || c.folder.empty()) return;
  const uint32_t id = ModNow(c.folder);
  if (!id) {
    static bool said = false;
    if (!said) {
      REXLOG_WARN("[svr2011] rtwm streak: the mod {} isn't installed (or is off) - the CAW plays", c.folder);
      said = true;
    }
    return;
  }
  constexpr uint32_t kSlots = 432, kSlotSize = 2116, kNoPick = 51200;
  uint32_t people = 0;
  for (uint32_t i = 0; i < 6; ++i) people += Rd32(base + match + kSlots + i * kSlotSize + 8) != kNoPick;
  if (people < 2) {  // (the player alone: a backstage walk - the CAW, see above)
    g_solo_frames = 1;  // (SVR2011_TEST_RTWM_WARP counts from here)
    static uint32_t said = 0;
    if (said++ < 3) REXLOG_INFO("[svr2011] rtwm streak: a scene with the player alone - the CAW plays it (the mod {} in matches)", c.folder);
    return;
  }
  for (uint32_t i = 0; i < 6; ++i) {
    uint8_t* slot = base + match + kSlots + i * kSlotSize;
    const uint32_t v = Rd32(slot + 8);
    if (v == kNoPick || v / 100 != uint32_t(c.caw)) continue;
    Wr32(slot + 8, id * 100 + v % 100);  // (the attire digits kept: the mod's attire 1 for the CAW's first)
    Wr16(slot + 54, id);
    REXLOG_INFO("[svr2011] rtwm streak: person {} - the CAW (slot {}) plays as the mod {} (id {})", i, c.caw, c.folder, id);
  }
}

void RtwmBeforeFrame(uint8_t* base) {
  static const bool pos_log = std::getenv("SVR2011_TEST_POS_LOG") != nullptr;
  static const char* warp = std::getenv("SVR2011_TEST_RTWM_WARP");
  if ((!pos_log && !warp) || !InRoadToWrestleMania()) return;
  constexpr uint32_t kChars = 0x82E3CC50;  // (the match's characters: hctp_room.cpp)
  const uint32_t f = Rd32(base + kChars);
  if (!f) return;
  static uint32_t frame = 0;
  if (pos_log && ++frame % 60 == 0) {
    constexpr uint32_t kCharCount = 0x82E3CD0C;
    std::string s;
    for (uint32_t i = 0; i < 8; ++i)
      if (const uint32_t c = Rd32(base + kChars + i * 4))
        s += fmt::format(" [{} {:08X} {:.1f} {:.1f} {:.1f}]", i, c, RdF(base + c + 288), RdF(base + c + 292), RdF(base + c + 296));
    REXLOG_INFO("[svr2011] rtwm: characters {}:{}", Rd32(base + kCharCount), s);
  }
  if (!warp || !g_solo_frames) return;
  ++g_solo_frames;
  // (5 s in, then each next place of "x,z[,y];x,z..." 6 s later; each held for 10 frames, as hctp_room's doors)
  constexpr uint32_t kAt = 300, kEvery = 360, kHeld = 10;
  if (g_solo_frames < kAt || (g_solo_frames - kAt) % kEvery >= kHeld) return;
  const uint32_t k = (g_solo_frames - kAt) / kEvery;
  const char* q = warp;
  for (uint32_t i = 0; i < k && q; ++i) q = std::strchr(q, ';'), q = q ? q + 1 : nullptr;
  if (!q) return;
  float x = 0, z = 0, y = RdF(base + f + 292);
  if (std::sscanf(q, "%f,%f,%f", &x, &z, &y) < 2) return;
  // The place is kept in more copies than the character's +288: its +0xF90 / +0xFD0, the root bones' matrices and
  // the walk's own controller (elsewhere in the heaps) - with +288 alone the player was back at the old place as
  // soon as he moved. A test aid, so plainly: every copy of the old place (x and z 8 bytes apart) in the game heaps,
  // found at the warp, is written. WARNING: test-only - the count grows each warp (14 -> 23 -> 42 in one walk: other
  // data matching by chance gets written too). Week 1's objective marker sits at about (-1, 340) (the entrance
  // stage); warping there or walking in from (-12, 330) did not start the next scene (23 Oct) - the walk asks none
  // of the event conditions (SVR2011_TEST_EVENT_LOG).
  static std::vector<uint32_t> copies;
  if ((g_solo_frames - kAt) % kEvery == 0) {
    const float ox = RdF(base + f + 288), oz = RdF(base + f + 296);
    copies.clear();
    for (uint32_t a = 0xA0000000; a < 0xB0000000; a += 4)  // (the game heaps)
      if (std::fabs(RdF(base + a) - ox) < 0.01f && std::fabs(RdF(base + a + 8) - oz) < 0.01f) copies.push_back(a);
    REXLOG_INFO("[svr2011] rtwm: test - fighter 0 put at {:.1f} {:.1f} {:.1f} (was {:.1f} {:.1f}; {} copies)", x, y, z, ox, oz,
                copies.size());
  }
  for (uint32_t a : copies) WrF(base + a, x), WrF(base + a + 4, y), WrF(base + a + 8, z);
}

}  // namespace svr2011

// A player's pick in a select list: sub_82732938(state, player, id). In Road to WrestleMania's CAW list (the
// Streak story) a mod is remembered and the story gets a CAW; a CAW picked plays the story as it is.
REX_EXTERN(__imp__sub_82732938);
REX_HOOK_RAW(sub_82732938) {
  const uint32_t id = ctx.r5.u32;
  static const bool log = std::getenv("SVR2011_TEST_PICK_LOG") != nullptr;
  if (log) REXLOG_INFO("[svr2011] pick: player {} id {:X} (rtwm {})", ctx.r4.u32, id, svr2011::InRoadToWrestleMania());
  if (svr2011::InRoadToWrestleMania() && g_cas_list) {
    g_pending = true;
    g_pending_folder = svr2011::IsSuperstarMod(id) ? svr2011::SuperstarModFolder(id) : std::string();
    if (!g_pending_folder.empty()) {
      REXLOG_INFO("[svr2011] rtwm streak: {} picked - the story gets CAW slot {}, its matches the mod", g_pending_folder,
                  g_first_caw);
      ctx.r5.u64 = g_first_caw;
    }
  }
  __imp__sub_82732938(ctx, base);
}
