// WWE SmackDown vs. Raw 2011 - THREE STAGES OF HELL: the first side to win
// two falls, each fall under another stipulation. In every mode
// (match_types.cpp: EXTREME RULES -> THREE STAGES OF HELL, or its row in
// 6-MAN / HANDICAP), on the mode's normal match: one on one 0x00, tag 0x03,
// triple threat 0x0D, fatal-4-way 0x0E, 6-man tag 0x08, one on two tornado
// 0x0F.
//
// One match, not three: the falls / match-end judge (sub_82245618, hooked in
// slobber_knocker.cpp, which calls ThreeStagesBeforeJudge first) sees a fall
// as a character's lost byte (+447) - in tag the legal man pinned; here a
// fall that doesn't win the match is counted for the side that beat him (his
// +448 "beaten by": that person's team, +1800; with two sides, the other
// one) and taken back (lost 0, the judge goes on), and the live rules
// (0x82E3DE00, the 64-byte option record the match runs with) switch to the
// next stage. A side is a team: a wrestler in triple threat / fatal-4-way, a
// team in tag / 6-man / handicap. Health and damage carry over.
//   fall 1: normal (the rule's own);
//   fall 2: FALLS COUNT ANYWHERE (as rule 0x2B: +1 pinfall 2 = anywhere, +17
//           1; no count-out +5, no rope break +3);
//   fall 3: LAST MAN STANDING (as rule 0x3B: +26 1, no pinfall +1, no give
//           up +7, no count-out). In tag and triple threat the engine plays
//           it with any number of people (one down for 10: his side loses;
//           the match ends from the count, not the falls judge). In 6-man tag
//           it never ended (8 minutes: partners and tags break the count), so
//           there fall 3 is NO DISQUALIFICATION (Extreme Rules' option bytes,
//           without +56: no rope breaks, no DQ, no ring-out; pin or give up
//           in the ring).
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
constexpr uint32_t kLost = 447, kBeatenBy = 448, kTeam = 1800, kPerson = 1156;
constexpr int kMaxSides = 6, kTeams = 16;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr const char* kStageName[3] = {"NORMAL", "FALLS COUNT ANYWHERE", "LAST MAN STANDING"};
constexpr uint32_t kSixManTag = 0x08;  // (fall 3: NO DISQUALIFICATION, see above)
std::atomic<bool> g_no_dq{false};      // this match's fall 3 is that one
const char* StageName(int stage) {
  stage = std::clamp(stage, 0, 2);
  return stage == 2 && g_no_dq.load() ? "NO DISQUALIFICATION" : kStageName[stage];
}

std::atomic<bool> g_on{false};
std::atomic<int> g_stage{0};               // 0-2
std::atomic<int> g_sides{0};               // sides in the match now (their teams, in person order)
std::atomic<uint32_t> g_side_team[kMaxSides] = {};
std::atomic<int> g_falls[kTeams] = {};     // falls won by each team
std::atomic<int64_t> g_seen{0};            // ms: the match's last update (the score is shown)

// The sides: the people's teams now, in person order (teams are set as the
// match starts - kept from earlier, a handicap match showed three sides).
void FindSides(uint8_t* base, const uint32_t (&chars)[6]) {
  uint32_t teams[kMaxSides];
  int n = 0;
  for (uint32_t c : chars) {
    if (!c) continue;
    const uint32_t team = Rd32(base + c + kTeam);
    bool seen = team >= uint32_t(kTeams);
    for (int s = 0; s < n; ++s) seen = seen || teams[s] == team;
    if (!seen && n < kMaxSides) teams[n++] = team;
  }
  for (int s = 0; s < n; ++s) g_side_team[s] = teams[s];
  g_sides = n;
}
uint32_t TeamOf(uint8_t* base, uint32_t c) { return std::min<uint32_t>(Rd32(base + c + kTeam), kTeams - 1); }

void ApplyStage(uint8_t* base, int stage) {
  uint8_t* live = base + kLive;
  if (stage == 1) {  // FALLS COUNT ANYWHERE
    live[1] = 2;
    live[17] = 1;
    live[3] = 0;
    live[5] = 0;
    live[27] = 0;
  } else if (stage == 2 && live[0] == kSixManTag) {  // NO DISQUALIFICATION
    g_no_dq = true;
    live[1] = 1;   // (pinfall in the ring)
    live[17] = 0;
    live[3] = 0;
    live[10] = 0;
    live[27] = 0x80;
  } else if (stage == 2) {  // LAST MAN STANDING
    live[1] = 0;
    live[7] = 0;
    live[17] = 0;
    live[26] = 1;
    live[3] = 0;
    live[5] = 0;
    live[27] = 0;
  }
  REXLOG_INFO("three stages: fall {} - {}", stage + 1, StageName(stage));
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
    char score[48] = "0 - 0";
    if (const int n = g_sides.load(); n >= 2) {
      int at = 0;
      for (int s = 0; s < n && at < int(sizeof(score)) - 8; ++s)
        at += std::snprintf(score + at, sizeof(score) - size_t(at), s ? " - %d" : "%d",
                            g_falls[std::min<uint32_t>(g_side_team[s].load(), kTeams - 1)].load());
    }
    char text[128];
    std::snprintf(text, sizeof(text), "THREE STAGES OF HELL   %s   FALL %d: %s", score, g_stage.load() + 1,
                  StageName(g_stage.load()));
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
  g_no_dq = false;
  g_sides = 0;
  for (auto& f : g_falls) f = 0;
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
  if (!g_on.load()) return;
  uint32_t chars[6] = {};
  int people = 0;
  for (uint32_t i = 0; i < 6; ++i)
    if ((chars[i] = Rd32(base + kChars + i * 4))) ++people;
  if (people < 2) return;
  g_seen = NowMs();
  FindSides(base, chars);  // (in person order: the score's order)
  if (Rd32(base + kLive + kMatchOver)) return;
  // Test aid: SVR2011_TEST_3S_FALL=<s> - every <s> seconds a person of the
  // second side (then the first, in turn) loses a fall - falls 1 and 2 only.
  static const int every = [] { const char* v = std::getenv("SVR2011_TEST_3S_FALL"); return v ? std::atoi(v) : 0; }();
  static int64_t next = 0;
  if (every > 0 && g_stage.load() < 2 && g_sides.load() >= 2) {  // (falls 1 and 2: the third as it comes)
    const int64_t now = NowMs();
    if (!next) next = now + every * 1000;
    if (now >= next) {
      next = now + every * 1000;
      const int loser_side = (g_stage.load() % 2) ? 0 : 1, winner_side = 1 - loser_side;
      uint32_t loser = 0, winner = 0;
      for (uint32_t c : chars) {
        if (!c) continue;
        const uint32_t team = TeamOf(base, c);
        if (team == g_side_team[loser_side].load() && !loser) loser = c;
        if (team == g_side_team[winner_side].load() && !winner) winner = c;
      }
      if (loser && winner && !base[loser + kLost]) {
        base[loser + kLost] = 1;
        base[loser + kBeatenBy] = uint8_t(Rd32(base + winner + kPerson));
        REXLOG_INFO("three stages: test - side {} loses a fall", loser_side);
      }
    }
  }
  for (uint32_t c : chars) {
    if (!c || !base[c + kLost]) continue;
    const int loser = int(TeamOf(base, c));
    // The team that beat him: the team of the person +448 names, else (two sides) the other one.
    int winner = -1;
    const uint32_t by = base[c + kBeatenBy];
    for (uint32_t o : chars)
      if (o && o != c && Rd32(base + o + kPerson) == by && int(TeamOf(base, o)) != loser) winner = int(TeamOf(base, o));
    if (winner < 0 && g_sides.load() == 2)
      winner = int(g_side_team[0].load() == uint32_t(loser) ? g_side_team[1].load() : g_side_team[0].load());
    REXLOG_INFO("three stages: person {} (team {}) lost (code {}, by {}) - fall to team {}", Rd32(base + c + kPerson),
                loser, base[c + kLost], by, winner);
    if (winner < 0 || winner >= kTeams) return;  // (unknown: the judge decides, as a normal match)
    const int won = ++g_falls[winner];
    std::string score;
    for (int s = 0; s < g_sides.load(); ++s)
      score += fmt::format("{}{}", s ? " - " : "", g_falls[std::min<uint32_t>(g_side_team[s].load(), kTeams - 1)].load());
    REXLOG_INFO("three stages: fall {} to team {} - {}", g_stage.load() + 1, winner, score);
    if (won >= 2) return;  // (the judge ends the match)
    base[c + kLost] = 0;   // (taken back: the match goes on)
    base[c + kBeatenBy] = 0;
    g_stage = std::min(g_stage.load() + 1, 2);
    ApplyStage(base, g_stage.load());
    return;
  }
}

}  // namespace svr2011
