// Online PORT RULES - see online_rules.h.
//
// The CREATE SESSION screen (class vtable 0x82023D50: made by sub_824EAD88,
// freed by sub_824EBB78 when the session is created): sub_824EBFF0 runs each
// frame of its life, sub_824EAAF8 twice a frame while it is on top (once
// while the MATCH TYPE list is open over it). The match picked in the game's
// MATCH TYPE list is the rule the screen reads its name for (the rule-name
// helper sub_824CCB38, called from 0x824EB3B8 as the screen is made and when
// the list returns). The lobby panel shows while a session's port rules are
// set and no CREATE SESSION is up.
// (Found by tracing: docs/MATCH_TYPES_RESEARCH.md.)
#include "online_rules.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"
#include "match_hud.h"
#include "match_types.h"
#include "frame_rate.h"
#include "p2p.h"

namespace {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

enum Mode : uint32_t { kNone = 0, kWeapons = 1, kStages = 2, kTablesLadders = 3, kWeaponsInside = 4 };
struct Option {
  Mode mode;
  int set;
  const char* name;
  const char* about;
};
constexpr Option kOff{kNone, 0, "NONE", "The match as the game has it."};
constexpr Option kWe{kWeapons, 0, "WEAPONS EVERYWHERE", "Extreme Rules with weapons lying in and around the ring."};
constexpr Option kStage[] = {
    {kStages, 0, "THREE STAGES: NORMAL / FCA / LMS", "First to two falls: Normal, Falls Count Anywhere, Last Man Standing."},
    {kStages, 1, "THREE STAGES: NORMAL / BLOOD / FINISHER", "First to two falls: Normal, First Blood, Finishers."},
    {kStages, 2, "THREE STAGES: NORMAL / LMS / SUBMISSION", "First to two falls: Normal, Last Man Standing, Submission."},
    {kStages, 4, "THREE STAGES: CELL BLOOD / LMS / SUB", "Inside the cell: First Blood, Last Man Standing, Submission."}};
constexpr Option kTl{kTablesLadders, 0, "TABLES & LADDERS", "Tables, ladders and chairs lying inside the cell."};
constexpr Option kInside{kWeaponsInside, 0, "WEAPONS INSIDE", "Chairs and other weapons lying in the ring."};

// The mode's normal matches (one on one, two on two, 3-man, 4-man) and
// their Extreme Rules ones; 6-man tag and handicap have no Extreme Rules
// rule (Weapons Everywhere plays them as they are).
constexpr uint32_t kNormal[] = {0x00, 0x03, 0x0D, 0x0E}, kExtreme[] = {0x4D, 0x4E, 0x4F, 0x50};

std::vector<Option> OptionsFor(uint32_t rule) {
  std::vector<Option> o{kOff};
  const bool normal = std::find(std::begin(kNormal), std::end(kNormal), rule) != std::end(kNormal);
  if (normal || rule == 0x08 || rule == 0x0F) {
    o.push_back(kWe);
    for (int k = 0; k < 3; ++k) o.push_back(kStage[k]);
  } else if (rule >= 0x4D && rule <= 0x50) {
    o.push_back(kWe);
  } else if (rule >= 0x31 && rule <= 0x34) {
    o.push_back(kTl);
    o.push_back(kStage[3]);
  } else if ((rule >= 0x3C && rule <= 0x3F) || rule == 0x51) {
    o.push_back(kInside);
  }
  return o;
}
uint32_t Value(const Option& o) { return o.mode == kNone ? 0 : uint32_t(o.mode) << 16 | uint32_t(o.set); }

std::atomic<int64_t> g_cs_at{0};                  // ms: CREATE SESSION last on top
std::atomic<bool> g_cs_alive{false};             // CREATE SESSION exists
std::atomic<int> g_cs_calls{0};                  // sub_824EAAF8 calls this frame
std::atomic<uint32_t> g_cs_rule{~0u};             // the match picked on CREATE SESSION
std::atomic<int> g_choice{0};                     // its row's option (index)
std::atomic<int> g_step{0};                       // presses / clicks to apply (-1 / +1)
uint32_t g_set_value = ~0u;                       // last given to SetPortRules
uint16_t g_last_buttons = 0;

bool CreateSessionShown() { return NowMs() - g_cs_at.load() < 300; }

// The picked match (CREATE SESSION's name read).
uint32_t PickedRule() { return g_cs_rule.load(); }
std::string Describe(uint32_t value) {
  for (uint32_t rule : {0x00u, 0x31u, 0x3Cu})
    for (const Option& o : OptionsFor(rule))
      if (Value(o) == value) return o.name;
  return "UNKNOWN";
}

// The row, the lobby panel; the choice handed to the session.
class Overlay final : public rex::ui::ImGuiDialog {
 public:
  explicit Overlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 private:
  bool shown_ = false;

 protected:
  void OnDraw(ImGuiIO& io) override {
    // (rule not known yet: NONE only; never in Ranked - its sessions carry no port rules)
    const bool row = CreateSessionShown() && !svr2011::P2PRanked().value_or(false);
    const uint32_t rule = PickedRule();
    if (row != shown_) {
      shown_ = row;
      REXLOG_INFO("online rules: the PORT RULES row {} (rule {:02X})", row ? "shown" : "hidden", rule);
    }
    if (row) {
      const std::vector<Option> options = OptionsFor(rule);
      int choice = std::clamp(g_choice.load(), 0, int(options.size()) - 1);
      if (const int step = g_step.exchange(0); step && options.size() > 1)
        choice = (choice + step + int(options.size())) % int(options.size());
      if (options.size() == 1) choice = 0;
      g_choice = choice;
      const Option& o = options[size_t(choice)];
      const svr2011::HudRowHits hits = svr2011::DrawHudRow(
          io.DisplaySize.x, io.DisplaySize.y, 0.715f, options.size() > 1 ? "PORT RULES  LB/RB" : "PORT RULES", o.name, "", "");
      // (mouse / touch: the arrows)
      if (ImGui::IsMouseClicked(0) && options.size() > 1) {
        const ImVec2 m = io.MousePos;
        const auto in = [&](const float (&r)[4]) { return m.x >= r[0] && m.x <= r[2] && m.y >= r[1] && m.y <= r[3]; };
        if (in(hits.left)) g_step = -1;
        if (in(hits.right)) g_step = 1;
      }
      const uint32_t value = Value(o);
      if (value != g_set_value) {
        g_set_value = value;
        svr2011::SetPortRules(value);
        REXLOG_INFO("online rules: PORT RULES {} ({:08X}) for rule {:02X}", o.name, value, rule);
      }
      return;
    }
    if (!g_cs_alive.load() && !svr2011::InMatch())
      if (const auto v = svr2011::PortRules(); v && *v)
        svr2011::DrawHudPanel(io.DisplaySize.x, io.DisplaySize.y, "PORT RULES", Describe(*v));
  }
};

}  // namespace

namespace svr2011 {

void InstallOnlineRulesOverlay(rex::ui::ImGuiDrawer* drawer) { new Overlay(drawer); }

void OnlineRulesPad(uint16_t buttons) {
  const uint16_t pressed = buttons & ~g_last_buttons;
  g_last_buttons = buttons;
  if (!CreateSessionShown()) return;
  if (pressed & 0x0100) g_step = -1;  // LB
  if (pressed & 0x0200) g_step = 1;   // RB
}

OnlineRulesSetup OnlineRulesFor(uint32_t rule) {
  OnlineRulesSetup s;
  // (not while CREATE SESSION is up: the MATCH TYPE list sets up matches as it's browsed)
  if (g_cs_alive.load()) return s;
  const auto v = PortRules();
  if (!v || !*v) return s;
  const Mode mode = Mode(*v >> 16);
  const int set = int(*v & 0xFFFF);
  bool valid = false;
  for (const Option& o : OptionsFor(rule)) valid = valid || (o.mode == mode && o.set == set && mode != kNone);
  if (!valid) {
    REXLOG_WARN("online rules: {:08X} doesn't fit rule {:02X} - played as it is", *v, rule);
    return s;
  }
  s.on = true;
  s.rule = rule;
  s.set = set;
  if (mode == kWeapons) {
    s.weapons = true;
    for (int i = 0; i < 4; ++i)
      if (rule == kNormal[i]) s.rule = kExtreme[i];  // (Weapons Everywhere: the Extreme Rules match)
  } else if (mode == kStages) {
    s.stages = true;
  } else {
    s.weapons = true;  // (Tables & Ladders, Weapons Inside: the structure's weapons)
  }
  REXLOG_INFO("online rules: {} - rule {:02X} played as {:02X}", Describe(*v), rule, s.rule);
  return s;
}

}  // namespace svr2011

// CREATE SESSION made / freed.
REX_EXTERN(__imp__sub_824EAD88);
REX_HOOK_RAW(sub_824EAD88) {
  g_cs_rule = ~0u;  // (before: the screen reads its match's name as it's made)
  g_choice = 0;
  g_cs_alive = true;
  __imp__sub_824EAD88(ctx, base);
  REXLOG_INFO("online rules: CREATE SESSION made (rule {:02X})", g_cs_rule.load());
}
REX_EXTERN(__imp__sub_824EBB78);
REX_HOOK_RAW(sub_824EBB78) {
  g_cs_alive = false;
  __imp__sub_824EBB78(ctx, base);
}

// Each frame of CREATE SESSION: on top when sub_824EAAF8 ran twice since the last.
REX_EXTERN(__imp__sub_824EBFF0);
REX_HOOK_RAW(sub_824EBFF0) {
  if (g_cs_calls.exchange(0) >= 2) g_cs_at = NowMs();
  __imp__sub_824EBFF0(ctx, base);
}
REX_EXTERN(__imp__sub_824EAAF8);
REX_HOOK_RAW(sub_824EAAF8) {
  ++g_cs_calls;
  __imp__sub_824EAAF8(ctx, base);
}

// The game's rule-name helper (rule in r3): CREATE SESSION reads its match's
// name through it at 0x824EB3B8 - the match picked (as it's made, and again
// when the MATCH TYPE list returns).
REX_EXTERN(__imp__sub_824CCB38);
REX_HOOK_RAW(sub_824CCB38) {
  if (uint32_t(ctx.lr) == 0x824EB3B8 && g_cs_alive.load()) {
    const uint32_t rule = ctx.r3.u32;
    if (rule != g_cs_rule.exchange(rule)) {
      g_choice = 0;  // (another match: NONE again)
      REXLOG_INFO("online rules: CREATE SESSION match is rule {:02X}", rule);
    }
  }
  __imp__sub_824CCB38(ctx, base);
}
