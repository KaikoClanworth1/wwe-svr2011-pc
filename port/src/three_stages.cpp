// WWE SmackDown vs. Raw 2011 - THREE STAGES OF HELL: one on one, the first to
// win two falls, each fall under another stipulation (ONE ON ONE -> EXTREME
// RULES -> THREE STAGES OF HELL, match_types.cpp: a normal one on one, rule
// 0x00).
//
// One match, not three: the falls / match-end judge (sub_82245618, hooked in
// slobber_knocker.cpp, which calls ThreeStagesBeforeJudge first) sees a fall
// as a character's lost byte (+447); here a fall that doesn't win the match
// is counted for the other wrestler and taken back (lost 0, the judge goes
// on), and the live rules (0x82E3DE00, the 64-byte option record the match
// runs with) switch to the next stage. Health and damage carry over.
//   fall 1: normal (the rule's own);
//   fall 2: FALLS COUNT ANYWHERE (as rule 0x2B: +1 pinfall 2 = anywhere, +17
//           1; no count-out +5, no rope break +3);
//   fall 3: LAST MAN STANDING (as rule 0x3B: +26 1, no pinfall +1, no give
//           up +7, no count-out).
// The score is shown over the match.
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include "match_types.h"

namespace {

constexpr uint32_t kLive = 0x82E3DE00, kChars = 0x82E3CC50, kMatchOver = 288;
constexpr uint32_t kLost = 447, kBeatenBy = 448;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr const char* kStageName[3] = {"NORMAL", "FALLS COUNT ANYWHERE", "LAST MAN STANDING"};

std::atomic<bool> g_on{false};
std::atomic<int> g_stage{0};       // 0-2
std::atomic<int> g_falls[2] = {};  // falls won by person 0 / 1
std::atomic<int64_t> g_seen{0};    // ms: the match's last update (the score is shown)

void ApplyStage(uint8_t* base, int stage) {
  uint8_t* live = base + kLive;
  if (stage == 1) {  // FALLS COUNT ANYWHERE
    live[1] = 2;
    live[17] = 1;
    live[3] = 0;
    live[5] = 0;
    live[27] = 0;
  } else if (stage == 2) {  // LAST MAN STANDING
    live[1] = 0;
    live[7] = 0;
    live[17] = 0;
    live[26] = 1;
    live[3] = 0;
    live[5] = 0;
    live[27] = 0;
  }
  REXLOG_INFO("three stages: fall {} - {}", stage + 1, kStageName[stage]);
}

class Score final : public rex::ui::ImGuiDialog {
 public:
  explicit Score(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!g_on.load() || NowMs() - g_seen.load() > 500) return;
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    const float gw = std::min(w, h * 16.0f / 9.0f), gh = gw * 9.0f / 16.0f;
    const float scale = gh / 720.0f;
    char text[96];
    std::snprintf(text, sizeof(text), "THREE STAGES OF HELL   %d - %d   FALL %d: %s", g_falls[0].load(),
                  g_falls[1].load(), g_stage.load() + 1, kStageName[std::clamp(g_stage.load(), 0, 2)]);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = 22.0f * scale;
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

void InstallThreeStagesOverlay(rex::ui::ImGuiDrawer* drawer) { new Score(drawer); }

void ThreeStagesSetup(bool on) {
  g_on = on;
  g_stage = 0;
  g_falls[0] = g_falls[1] = 0;
  if (on) REXLOG_INFO("three stages: on");
}

bool ThreeStagesMatch() { return g_on.load(); }

// Each world update: the score stays shown (the judge isn't asked during a
// count). Test aid: SVR2011_TEST_3S_LOG=1 - the characters once a second.
void ThreeStagesUpdate(uint8_t* base) {
  if (!Rd32(base + kChars) || int32_t(Rd32(base + 0x82E3CD0C)) <= 0) return;  // (a match running)
  g_seen = NowMs();
  static const bool log = std::getenv("SVR2011_TEST_3S_LOG") != nullptr;
  static uint32_t frames = 0;
  if (!log || ++frames % 60) return;
  std::string all;
  for (uint32_t i = 0; i < 9; ++i)
    if (const uint32_t c = Rd32(base + kChars + i * 4))
      all += fmt::format(" {}:{:08X} state {} lost {} by {} team {} role {} area {} person {}", i, c, base[c + 446],
                         base[c + kLost], base[c + kBeatenBy], Rd32(base + c + 1800), Rd32(base + c + 2624),
                         base[c + 444], Rd32(base + c + 2660));
  std::string opt;
  for (int i = 0; i < 40; ++i) opt += fmt::format("{:02X}", base[kLive + i]);
  REXLOG_INFO("three stages: live {} characters{}", opt, all);
}

void ThreeStagesBeforeJudge(uint8_t* base) {
  if (!g_on.load() || base[kLive] != 0x00) return;
  const uint32_t a = Rd32(base + kChars), b = Rd32(base + kChars + 4);
  if (!a || !b) return;
  g_seen = NowMs();
  if (Rd32(base + kLive + kMatchOver)) return;
  // Test aid: SVR2011_TEST_3S_FALL=<s> - every <s> seconds person 1 (then 0,
  // in turn) loses a fall - falls 1 and 2 only.
  static const int every = [] { const char* v = std::getenv("SVR2011_TEST_3S_FALL"); return v ? std::atoi(v) : 0; }();
  static int64_t next = 0;
  if (every > 0 && g_stage.load() < 2) {  // (falls 1 and 2: the third as it comes)
    const int64_t now = NowMs();
    if (!next) next = now + every * 1000;
    if (now >= next) {
      next = now + every * 1000;
      const uint32_t loser = (g_falls[0] + g_falls[1]) % 2 ? a : b;
      if (!base[loser + kLost]) {
        base[loser + kLost] = 1;
        REXLOG_INFO("three stages: test - person {} loses a fall", loser == a ? 0 : 1);
      }
    }
  }
  for (int i = 0; i < 2; ++i) {
    const uint32_t c = i ? b : a;
    if (!base[c + kLost]) continue;
    const int winner = 1 - i;
    REXLOG_INFO("three stages: person {} lost (code {}, by {})", i, base[c + kLost], base[c + kBeatenBy]);
    const int won = ++g_falls[winner];
    REXLOG_INFO("three stages: fall {} to person {} - {} - {}", g_stage.load() + 1, winner, g_falls[0].load(),
                g_falls[1].load());
    if (won >= 2) return;  // (the judge ends the match)
    base[c + kLost] = 0;   // (taken back: the match goes on)
    base[c + kBeatenBy] = 0;
    g_stage = std::min(g_stage.load() + 1, 2);
    ApplyStage(base, g_stage.load());
    return;
  }
}

}  // namespace svr2011
