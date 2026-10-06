// WWE SmackDown vs. Raw 2011 - Slobber Knocker: player 1 against an endless
// line of opponents, one at a time, until pinned (HANDICAP -> SLOBBER
// KNOCKER, match_types.cpp: the gauntlet rule 0x52, player 1 and the first
// opponent picked, the 2 waiting opponents random).
//
// The gauntlet (research: docs/MATCH_TYPES_RESEARCH.md):
// - characters (0x82E3CC50, u32[9]: people 0-5, then the referee and the
//   commentators): +446 state (0 in the match, 1 waiting, 4 beaten / out),
//   +447 how it lost (0 still in), +448 who beat it, +1800 team, +2624 role
//   (3 waiting entrant), +80 its superstar (sub_828B7028 -> id);
// - the falls / match-end judge sub_82245618: for a character that lost
//   (+447) it brings in a waiting one of its team (+446 = 0) - the match ends
//   only when that team has none left.
// Here a beaten opponent's slot is loaded with a new random superstar who
// then waits, so the opponents' team always has someone to come in; the
// match ends when player 1 loses. The number beaten is shown over the match.
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"
#include "match_types.h"

namespace {

constexpr uint32_t kLive = 0x82E3DE00, kChars = 0x82E3CC50, kGauntlet = 0x52;
constexpr uint32_t kState = 446, kLost = 447, kBeatenBy = 448, kTeam = 1800, kRole = 2624;
constexpr uint32_t kOpponents = 1;  // (their team; player 1's is 0)
constexpr uint32_t kBusy = 476;  // (0 once a beaten character has left - the Royal Rumble's test before it unloads one)

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
void Wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::atomic<int> g_beaten{0};    // opponents beaten this match
std::atomic<int64_t> g_seen{0};  // ms: the match's last update (the count is shown)
std::set<uint32_t> g_counted;    // characters counted (their fall)
uint32_t g_frames = 0;

bool Running(uint8_t* base) { return svr2011::SlobberKnockerMatch() && base[kLive] == kGauntlet; }

std::string Characters(uint8_t* base) {
  std::string all;
  const uint32_t infos = Rd32(base + 0x82E3DDFC);
  for (uint32_t i = 0; i < 6; ++i)
    if (const uint32_t c = Rd32(base + kChars + i * 4)) {
      const uint32_t person = Rd32(base + c + 2660);
      const uint32_t info = infos && person < 33 ? infos + 128 + person * 52 : 0;
      all += fmt::format(" {}:{:08X} state {} lost {} by {} team {} role {} area {} +476 {:08X} +2348 {} person {} entry {}",
                         i, c, base[c + kState], base[c + kLost], base[c + kBeatenBy], Rd32(base + c + kTeam),
                         Rd32(base + c + kRole), base[c + 444], Rd32(base + c + kBusy), Rd32(base + c + 2348), person,
                         info ? int16_t(Rd16(base + info + 40)) : -1);
    }
  return all;
}

// Falls of player 1's opponents, counted once each.
void CountFalls(uint8_t* base) {
  const uint32_t player = Rd32(base + kChars);
  if (!player) return;
  const uint32_t team = Rd32(base + player + kTeam);
  for (uint32_t i = 1; i < 6; ++i) {
    const uint32_t c = Rd32(base + kChars + i * 4);
    if (!c || !base[c + kLost] || Rd32(base + c + kTeam) == team || g_counted.count(c)) continue;
    g_counted.insert(c);
    ++g_beaten;
    REXLOG_INFO("slobber knocker: opponent {} beaten - {} so far", i, g_beaten.load());
  }
}

bool WaiterReady(uint8_t* base, uint32_t team) {
  for (uint32_t i = 1; i < 6; ++i) {
    const uint32_t c = Rd32(base + kChars + i * 4);
    if (c && base[c + kState] == 1 && !base[c + kLost] && Rd32(base + c + kTeam) == team &&
        Rd32(base + c + kRole) == 3)
      return true;
  }
  return false;
}

// -- The next opponent ---------------------------------------------------
//
// A beaten opponent, once gone (state 4, the judge done with it - lost back
// to 0 - and +476 0: it has left), waits again for its turn like a gauntlet
// entrant: state 1, role 3, not walked in yet (+2348 0), an entry time (info
// +40, s16: the gauntlet's entry task sub_82328AA0 walks a waiting entrant
// to the ring once the match time has passed it; 0 never). So the opponents'
// team always has someone to come in, and the line goes round the 4
// opponents (the one picked and 3 random).
// Not done: a new superstar each time. Reloading a beaten opponent's slot
// mid-match (the Royal Rumble's unload sub_8217CCD8 + the run-in task's
// load: sub_822F5AD8, sub_8217CB48 / CB68, sub_8224AB90) works most times but
// the characters' job (thread 22, sub_82252610 -> sub_82258CC0, a component
// of the character) can still hold the old character - a crash; the game's
// swap task (sub_825BDF60) is for empty person slots only (the draw crashed).
// A safe point to free a character would be needed (the Royal Rumble's
// eliminated ones: sub_8223FD78).
constexpr uint32_t kOutFrames = 60;  // (gone 1 s before it waits again)

uint32_t g_out_frames[6] = {};

uint32_t Call(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint32_t r3) {
  const auto saved = ctx;
  ctx.r3.u64 = r3;
  fn(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

void RecycleStep(PPCContext& ctx, uint8_t* base) {
  const uint32_t player = Rd32(base + kChars);
  if (!player || Rd32(base + kLive + 288)) return;  // (+288: the match is decided)
  for (uint32_t i = 1; i < 6; ++i) {
    const uint32_t c = Rd32(base + kChars + i * 4);
    const bool out = c && base[c + kState] == 4 && !base[c + kLost] && !Rd32(base + c + kBusy) &&
                     Rd32(base + c + kTeam) != Rd32(base + player + kTeam);
    g_out_frames[i] = out ? g_out_frames[i] + 1 : 0;
    if (!out || g_out_frames[i] < kOutFrames) continue;
    g_out_frames[i] = 0;
    base[c + kState] = 1;
    Wr32(base + c + kRole, 3);
    Wr32(base + c + 2348, 0);
    if (const uint32_t info = Call(ctx, base, sub_82573EC8, Call(ctx, base, sub_8257C178, Rd32(base + c + 2660))))
      base[info + 40] = 0, base[info + 41] = 1;
    g_counted.erase(c);
    REXLOG_INFO("slobber knocker: opponent {} waits to come in again", i);
  }
}

// The count, over the match.
class Count final : public rex::ui::ImGuiDialog {
 public:
  explicit Count(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (NowMs() - g_seen.load() > 500) return;
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    const float gw = std::min(w, h * 16.0f / 9.0f), gh = gw * 9.0f / 16.0f;
    const float scale = gh / 720.0f;
    char text[48];
    std::snprintf(text, sizeof(text), "SLOBBER KNOCKER   BEATEN: %d", g_beaten.load());
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = 24.0f * scale;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 at((w - ts.x) * 0.5f, (h - gh) * 0.5f + gh * 0.035f);
    const ImVec2 pad(14.0f * scale, 6.0f * scale);
    dl->AddRectFilled(ImVec2(at.x - pad.x, at.y - pad.y), ImVec2(at.x + ts.x + pad.x, at.y + ts.y + pad.y),
                      IM_COL32(10, 12, 18, 200), 6.0f * scale);
    dl->AddText(font, size, at, IM_COL32(255, 255, 255, 255), text);
  }
};

}  // namespace

namespace svr2011 {

void InstallSlobberKnockerOverlay(rex::ui::ImGuiDrawer* drawer) { new Count(drawer); }

void SlobberKnockerStart() {
  g_beaten = 0;
  g_counted.clear();
  g_frames = 0;
  for (uint32_t& f : g_out_frames) f = 0;
}

void SlobberKnockerUpdate(PPCContext& ctx, uint8_t* base) {
  ++g_frames;
  if (Rd32(base + kChars) && int32_t(Rd32(base + 0x82E3CD0C)) > 0) g_seen = NowMs();  // (a match running)
  // Test aids (with the count shown: g_frames counts from the select screen):
  // SVR2011_TEST_SK_LOG=1 - the characters once a second;
  // SVR2011_TEST_SK_BEAT=<s> - every <s> seconds the opponent in the match
  // loses (as if pinned by player 1).
  static const bool log = std::getenv("SVR2011_TEST_SK_LOG") != nullptr;
  static const int beat = [] { const char* v = std::getenv("SVR2011_TEST_SK_BEAT"); return v ? std::atoi(v) : 0; }();
  if (log && g_frames % 60 == 0) REXLOG_INFO("slobber knocker: characters{}", Characters(base));
  // SVR2011_TEST_SK_LOSE=<s> - after <s> seconds player 1 loses.
  static const int lose = [] { const char* v = std::getenv("SVR2011_TEST_SK_LOSE"); return v ? std::atoi(v) : 0; }();
  if (lose > 0 && g_frames == uint32_t(lose * 60))
    if (const uint32_t player = Rd32(base + kChars)) {
      base[player + kLost] = 1;
      REXLOG_INFO("slobber knocker: test - player 1 loses");
    }
  if (beat > 0 && g_frames % uint32_t(beat * 60) == 0) {
    const uint32_t player = Rd32(base + kChars);
    for (uint32_t i = 1; player && i < 6; ++i) {
      const uint32_t c = Rd32(base + kChars + i * 4);
      if (!c || base[c + kState] != 0 || base[c + kLost] || base[c + 444] != 0 ||
          Rd32(base + c + kTeam) == Rd32(base + player + kTeam))
        continue;
      base[c + kLost] = 1;
      base[c + kBeatenBy] = 0;
      REXLOG_INFO("slobber knocker: test - opponent {} loses", i);
      break;
    }
  }
}

}  // namespace svr2011

// The falls / match-end judge: sub_82245618(...) -> 1 when the match is over.
REX_EXTERN(__imp__sub_82245618);
REX_HOOK_RAW(sub_82245618) {
  const bool running = Running(base);
  if (running) CountFalls(base);
  if (running) RecycleStep(ctx, base);
  // An opponent lost with no one waiting to come in (the beaten ones not gone
  // yet): one of those waits now - else the gauntlet would end.
  if (running) {
    const uint32_t player = Rd32(base + kChars);
    for (uint32_t i = 1; player && i < 6; ++i) {
      const uint32_t c = Rd32(base + kChars + i * 4);
      if (!c || !base[c + kLost] || Rd32(base + c + kTeam) == Rd32(base + player + kTeam) ||
          WaiterReady(base, Rd32(base + c + kTeam)))
        continue;
      for (uint32_t k = 1; k < 6; ++k) {
        const uint32_t w = Rd32(base + kChars + k * 4);
        if (!w || w == c || base[w + kState] != 4 || base[w + kLost] || Rd32(base + w + kTeam) != Rd32(base + c + kTeam))
          continue;
        base[w + kState] = 1;
        Wr32(base + w + kRole, 3);
        Wr32(base + w + 2348, 0);
        if (const uint32_t info = Call(ctx, base, sub_82573EC8, Call(ctx, base, sub_8257C178, Rd32(base + w + 2660))))
          base[info + 40] = 0, base[info + 41] = 1;
        g_counted.erase(w);
        REXLOG_INFO("slobber knocker: opponent {} waits to come in again (early)", k);
        break;
      }
      break;
    }
  }
  __imp__sub_82245618(ctx, base);
  if (running && ctx.r3.u32 == 1)
    REXLOG_INFO("slobber knocker: the match is over - {} beaten;{}", g_beaten.load(), Characters(base));
}
