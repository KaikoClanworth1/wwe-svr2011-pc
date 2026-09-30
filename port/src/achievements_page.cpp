// WWE SmackDown vs. Raw 2011 - the ACHIEVEMENTS page (see achievements_page.h).
//
// Drawn in the game's panel style, as the GRAPHICS page is: a list on the left
// (icon, name, what to do, gamerscore; locked ones dimmed with a padlock) and
// the selected achievement on the right (large icon, when it was unlocked, how
// to unlock it).

#include "achievements_page.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

namespace svr2011 {

namespace {

using Clock = std::chrono::steady_clock;
using rex::system::AchievementInfo;

rex::Runtime* g_runtime = nullptr;
rex::ui::ImmediateDrawer* g_immediate = nullptr;
rex::input::InputSystem* g_input = nullptr;
ImFont* g_menu_font = nullptr;
ImFont* g_title_font = nullptr;
std::atomic<bool> g_open_requested{false};
std::atomic<bool> g_open{false};
std::atomic<bool> g_wait_release{false};

// Xbox LIVE only: these can't be earned offline (the port has no Xbox LIVE).
bool OnlineOnly(uint32_t id) { return id == 37 || id == 38 || id == 39; }

enum Filter { kAll, kUnlocked, kLocked, kFilters };
const char* kFilterNames[kFilters] = {"ALL", "UNLOCKED", "LOCKED"};

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text,
          float wrap = 0.0f) {
  dl->AddText(font ? font : ImGui::GetFont(), size, at, colour, text, nullptr, wrap);
}
ImVec2 TextSize(ImFont* font, float size, const char* text, float wrap = 0.0f) {
  return (font ? font : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, wrap, text);
}
// `text` cut to `width` with "..." if it doesn't fit.
std::string Fit(ImFont* font, float size, const std::string& text, float width) {
  if (TextSize(font, size, text.c_str()).x <= width) return text;
  std::string s = text;
  while (!s.empty() && TextSize(font, size, (s + "...").c_str()).x > width) s.pop_back();
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s + "...";
}

// "29 SEP 2026" from a FILETIME.
std::string Date(uint64_t filetime) {
  if (!filetime) return "";
  const std::time_t t = std::time_t((filetime - 116444736000000000ull) / 10000000ull);
  std::tm tm = {};
  localtime_s(&tm, &t);
  static const char* kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  char buf[32];
  std::snprintf(buf, sizeof buf, "%d %s %d", tm.tm_mday, kMonths[tm.tm_mon % 12], tm.tm_year + 1900);
  return buf;
}

// A padlock centred on `c`, `h` high.
void Lock(ImDrawList* dl, ImVec2 c, float h, ImU32 colour) {
  const float w = h * 0.72f, body = h * 0.56f;
  const ImVec2 a(c.x - w * 0.5f, c.y + h * 0.5f - body), b(c.x + w * 0.5f, c.y + h * 0.5f);
  dl->PathArcTo(ImVec2(c.x, a.y), w * 0.32f, 3.14159265f, 2 * 3.14159265f, 12);
  dl->PathStroke(colour, 0, h * 0.11f);
  dl->AddLine(ImVec2(c.x - w * 0.32f, a.y), ImVec2(c.x - w * 0.32f, a.y + 1), colour, h * 0.11f);
  dl->AddLine(ImVec2(c.x + w * 0.32f, a.y), ImVec2(c.x + w * 0.32f, a.y + 1), colour, h * 0.11f);
  dl->AddRectFilled(a, b, colour, h * 0.08f);
}

// A tick centred on `c`, `h` high.
void Tick(ImDrawList* dl, ImVec2 c, float h, ImU32 colour) {
  const ImVec2 pts[3] = {ImVec2(c.x - h * 0.42f, c.y), ImVec2(c.x - h * 0.12f, c.y + h * 0.32f),
                         ImVec2(c.x + h * 0.45f, c.y - h * 0.36f)};
  dl->AddPolyline(pts, 3, colour, 0, h * 0.16f);
}

// A controller button glyph (a coloured disc with its letter) and a label.
float Hint(ImDrawList* dl, float x, float cy, float s, const char* button, ImU32 colour,
           const char* label) {
  const float r = 13 * s;
  const bool bumper = button[1] != 0;  // LB / RB: a rounded bar
  const float bw = bumper ? 22 * s : r;
  if (bumper) {
    dl->AddRectFilled(ImVec2(x, cy - 10 * s), ImVec2(x + bw * 2, cy + 10 * s), colour, 5 * s);
  } else {
    dl->AddCircleFilled(ImVec2(x + r, cy), r, colour, 24);
  }
  const float gs = 16 * s;
  const ImVec2 gz = TextSize(g_menu_font, gs, button);
  Text(dl, g_menu_font, gs, ImVec2(x + bw - gz.x * 0.5f, cy - gz.y * 0.5f), IM_COL32(255, 255, 255, 255),
       button);
  const float ls = 18 * s;
  const ImVec2 lz = TextSize(g_menu_font, ls, label);
  Text(dl, g_menu_font, ls, ImVec2(x + bw * 2 + 8 * s, cy - lz.y * 0.5f), IM_COL32(230, 230, 235, 255),
       label);
  return x + bw * 2 + 8 * s + lz.x + 26 * s;
}

class AchievementsPage final : public rex::ui::ImGuiDialog {
 public:
  explicit AchievementsPage(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct Entry {
    AchievementInfo info;
    bool unlocked = false;
    uint64_t when = 0;
  };

  void Load();
  void Rebuild();  // `shown_` from `all_` and the filter
  uint16_t PadButtons();
  rex::ui::ImmediateTexture* Icon(const AchievementInfo& info);

  std::vector<Entry> all_;
  std::vector<int> shown_;
  int filter_ = kAll;
  int sel_ = 0, top_ = 0;
  uint16_t prev_buttons_ = 0;
  bool wait_release_ = false;
  Clock::time_point repeat_at_{}, opened_at_{}, refreshed_at_{};
  std::unique_ptr<rex::ui::AchievementIconCache> icons_;
};

rex::ui::ImmediateTexture* AchievementsPage::Icon(const AchievementInfo& info) {
  if (!icons_ && g_immediate && g_runtime) {
    icons_ = std::make_unique<rex::ui::AchievementIconCache>(g_immediate, g_runtime);
  }
  return icons_ ? icons_->GetIcon(info) : nullptr;
}

void AchievementsPage::Load() {
  const int keep = shown_.empty() ? -1 : shown_[std::clamp(sel_, 0, int(shown_.size()) - 1)];
  all_.clear();
  auto* kernel = g_runtime ? g_runtime->kernel_state() : nullptr;
  if (kernel) {
    auto& manager = kernel->achievements();
    for (AchievementInfo& info : manager.ListAchievements()) {
      Entry e;
      e.unlocked = manager.IsUnlocked(info.id);
      e.when = e.unlocked ? manager.GetUnlockTime(info.id) : 0;
      e.info = std::move(info);
      all_.push_back(std::move(e));
    }
  }
  std::sort(all_.begin(), all_.end(), [](const Entry& a, const Entry& b) { return a.info.id < b.info.id; });
  Rebuild();
  if (keep >= 0) {
    for (int i = 0; i < int(shown_.size()); ++i) {
      if (shown_[i] == keep) sel_ = i;
    }
  }
}

void AchievementsPage::Rebuild() {
  shown_.clear();
  for (int i = 0; i < int(all_.size()); ++i) {
    const bool u = all_[i].unlocked;
    if (filter_ == kAll || (filter_ == kUnlocked && u) || (filter_ == kLocked && !u)) shown_.push_back(i);
  }
  sel_ = std::clamp(sel_, 0, std::max(0, int(shown_.size()) - 1));
}

uint16_t AchievementsPage::PadButtons() {
  using namespace rex::input;
  X_INPUT_STATE s = {};
  if (!g_input || !g_input->HeldState(0, &s)) return 0;
  uint16_t b = s.gamepad.buttons;
  const int lx = s.gamepad.thumb_lx, ly = s.gamepad.thumb_ly;
  constexpr int kDead = 16000;
  if (ly > kDead) b |= X_INPUT_GAMEPAD_DPAD_UP;
  if (ly < -kDead) b |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (lx < -kDead) b |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (lx > kDead) b |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  return b;
}

void AchievementsPage::OnDraw(ImGuiIO& io) {
  using namespace rex::input;
  constexpr int kVisible = 7;  // list rows on the page
  if (g_open_requested.exchange(false)) {
    filter_ = kAll;
    sel_ = top_ = 0;
    shown_.clear();
    Load();
    wait_release_ = true;  // (the A that opened the page is still down)
    opened_at_ = refreshed_at_ = Clock::now();
    g_open = true;
  }
  if (!g_open) {
    if (g_wait_release && (!g_input || PadButtons() == 0)) g_wait_release = false;
    return;
  }
  // Unlocks while the page is open (twice a second).
  if (Clock::now() - refreshed_at_ > std::chrono::milliseconds(500)) {
    refreshed_at_ = Clock::now();
    Load();
  }

  // Input: controller (edges, auto-repeat for held directions) and keys.
  const uint16_t buttons = PadButtons();
  if (wait_release_) {
    if (buttons == 0 && Clock::now() - opened_at_ > std::chrono::milliseconds(150)) wait_release_ = false;
    prev_buttons_ = buttons;
  }
  const uint16_t pressed = wait_release_ ? 0 : uint16_t(buttons & ~prev_buttons_);
  constexpr uint16_t kRepeat = X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN |
                               X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  uint16_t act = pressed;
  const auto now = Clock::now();
  if (pressed & kRepeat) {
    repeat_at_ = now + std::chrono::milliseconds(400);
  } else if (!wait_release_ && (buttons & kRepeat) && now >= repeat_at_) {
    act |= buttons & kRepeat;
    repeat_at_ = now + std::chrono::milliseconds(110);
  }
  if (!wait_release_) prev_buttons_ = buttons;
  auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
  int move = 0;
  if ((act & X_INPUT_GAMEPAD_DPAD_UP) || key(ImGuiKey_UpArrow)) move = -1;
  if ((act & X_INPUT_GAMEPAD_DPAD_DOWN) || key(ImGuiKey_DownArrow)) move = 1;
  if ((act & X_INPUT_GAMEPAD_LEFT_SHOULDER) || key(ImGuiKey_PageUp)) move = -kVisible;
  if ((act & X_INPUT_GAMEPAD_RIGHT_SHOULDER) || key(ImGuiKey_PageDown)) move = kVisible;
  if ((pressed & (X_INPUT_GAMEPAD_Y | X_INPUT_GAMEPAD_X)) || ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
    filter_ = (filter_ + 1) % kFilters;
    sel_ = top_ = 0;
    Rebuild();
  }
  const bool back = (pressed & (X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_START | X_INPUT_GAMEPAD_BACK)) ||
                    ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  const int n = int(shown_.size());
  if (move && n) {
    if (std::abs(move) == 1) {
      sel_ = (sel_ + move + n) % n;  // (wraps, as the game's lists do)
    } else {
      sel_ = std::clamp(sel_ + move, 0, n - 1);
    }
  }
  if (sel_ < top_) top_ = sel_;
  if (sel_ >= top_ + kVisible) top_ = sel_ - kVisible + 1;
  top_ = std::clamp(top_, 0, std::max(0, n - kVisible));

  // Layout in the game's 1280 x 720 frame, letterboxed into the window.
  float fw = io.DisplaySize.x, fh = fw * 9.0f / 16.0f;
  if (fh > io.DisplaySize.y) fh = io.DisplaySize.y, fw = fh * 16.0f / 9.0f;
  const float ox = (io.DisplaySize.x - fw) * 0.5f, oy = (io.DisplaySize.y - fh) * 0.5f;
  const float s = fh / 720.0f;
  auto P = [&](float x, float y) { return ImVec2(ox + x * s, oy + y * s); };
  const ImU32 kWhite = IM_COL32(255, 255, 255, 255), kGrey = IM_COL32(170, 170, 178, 255),
              kDim = IM_COL32(120, 120, 128, 255), kGreen = IM_COL32(110, 200, 90, 255),
              kGold = IM_COL32(255, 200, 60, 255);

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
  // Panel: dark glass with a thin white rim, as the game's.
  dl->AddRectFilled(P(60, 40), P(1220, 684), IM_COL32(8, 8, 12, 250), 10 * s);
  dl->AddRectFilledMultiColor(P(62, 42), P(1218, 260), IM_COL32(40, 40, 48, 120), IM_COL32(40, 40, 48, 120),
                              IM_COL32(8, 8, 12, 0), IM_COL32(8, 8, 12, 0));
  dl->AddRect(P(60, 40), P(1220, 684), IM_COL32(235, 235, 240, 255), 10 * s, 0, 2.5f * s);
  // Header: the dotted strip, the white-rimmed tab with the title, the page name.
  dl->AddRectFilled(P(66, 46), P(1214, 80), IM_COL32(24, 24, 30, 255), 6 * s);
  for (float x = 76; x < 1206; x += 7) {
    for (float y = 52; y < 76; y += 7) dl->AddCircleFilled(P(x, y), 1.2f * s, IM_COL32(70, 70, 80, 255));
  }
  dl->AddQuadFilled(P(164, 46), P(606, 46), P(588, 80), P(152, 80), IM_COL32(12, 12, 16, 255));
  dl->AddQuad(P(164, 46), P(606, 46), P(588, 80), P(152, 80), IM_COL32(235, 235, 240, 255), 2 * s);
  {
    const char* title = "MY WWE";
    const float ts = 34 * s;
    const ImVec2 sz = TextSize(g_title_font, ts, title);
    Text(dl, g_title_font, ts, ImVec2(P(378, 0).x - sz.x * 0.5f, P(0, 63).y - sz.y * 0.5f), kWhite, title);
    const float ps = 22 * s;
    const ImVec2 psz = TextSize(g_menu_font, ps, "ACHIEVEMENTS");
    Text(dl, g_menu_font, ps, ImVec2(P(634, 0).x, P(0, 63).y - psz.y * 0.5f), kWhite, "ACHIEVEMENTS");
  }

  // Summary: unlocked count, gamerscore and a bar.
  int unlocked = 0;
  uint32_t score = 0, total = 0;
  for (const Entry& e : all_) {
    total += e.info.gamerscore;
    if (e.unlocked) ++unlocked, score += e.info.gamerscore;
  }
  {
    const float ss = 21 * s, cy = P(0, 108).y;
    char buf[96];
    std::snprintf(buf, sizeof buf, "%d OF %d UNLOCKED", unlocked, int(all_.size()));
    Text(dl, g_menu_font, ss, ImVec2(P(84, 0).x, cy - TextSize(g_menu_font, ss, buf).y * 0.5f), kWhite, buf);
    std::snprintf(buf, sizeof buf, "%u / %u G", score, total);
    const ImVec2 gz = TextSize(g_menu_font, ss, buf);
    Text(dl, g_menu_font, ss, ImVec2(P(1196, 0).x - gz.x, cy - gz.y * 0.5f), kGold, buf);
    const ImVec2 a = P(380, 102), b(P(1196, 0).x - gz.x - 24 * s, P(0, 114).y);
    dl->AddRectFilled(a, b, IM_COL32(40, 40, 48, 255), 4 * s);
    const float f = all_.empty() ? 0.0f : float(unlocked) / float(all_.size());
    if (f > 0) {
      dl->AddRectFilledMultiColor(a, ImVec2(a.x + (b.x - a.x) * f, b.y), IM_COL32(200, 30, 32, 255),
                                  IM_COL32(240, 70, 60, 255), IM_COL32(240, 70, 60, 255),
                                  IM_COL32(200, 30, 32, 255));
    }
  }

  // The list.
  const float row_h = 62, gap = 6, list_top = 134;
  if (shown_.empty()) {
    const char* msg = filter_ == kUnlocked ? "No achievements unlocked yet." : "Every achievement is unlocked!";
    const float ms = 21 * s;
    const ImVec2 mz = TextSize(g_menu_font, ms, msg);
    Text(dl, g_menu_font, ms, ImVec2(P(412, 0).x - mz.x * 0.5f, P(0, 360).y - mz.y * 0.5f), kGrey, msg);
  }
  for (int r = 0; r < kVisible && top_ + r < n; ++r) {
    const int idx = top_ + r;
    const Entry& e = all_[shown_[idx]];
    const float y = list_top + r * (row_h + gap);
    const bool sel = idx == sel_;
    const ImVec2 a = P(80, y), b = P(744, y + row_h);
    if (sel) {
      dl->AddRectFilledMultiColor(a, b, IM_COL32(150, 18, 20, 255), IM_COL32(95, 8, 10, 255),
                                  IM_COL32(95, 8, 10, 255), IM_COL32(150, 18, 20, 255));
      dl->AddRect(a, b, IM_COL32(230, 60, 60, 255), 3 * s, 0, 1.5f * s);
    } else {
      dl->AddRectFilled(a, b, IM_COL32(34, 34, 40, 235), 3 * s);
    }
    // Icon (dimmed while locked).
    const ImVec2 ia = P(86, y + 5), ib = P(138, y + 57);
    if (auto* tex = Icon(e.info)) {
      dl->AddImage(reinterpret_cast<ImTextureID>(tex), ia, ib, ImVec2(0, 0), ImVec2(1, 1),
                   e.unlocked ? kWhite : IM_COL32(85, 85, 90, 255));
    } else {
      dl->AddRectFilled(ia, ib, IM_COL32(50, 50, 58, 255), 3 * s);
    }
    if (!e.unlocked) Lock(dl, P(112, y + 31), 24 * s, IM_COL32(235, 235, 240, 230));
    // Name and what to do.
    const float ns = 21 * s, ds = 16 * s;
    const std::string name = Fit(g_menu_font, ns, e.info.label, 470 * s);
    Text(dl, g_menu_font, ns, P(152, y + 7), e.unlocked || sel ? kWhite : kGrey, name.c_str());
    const std::string& how = e.info.description.empty() ? e.info.unachieved_description : e.info.description;
    const std::string line = Fit(g_menu_font, ds, how, 500 * s);
    Text(dl, g_menu_font, ds, P(152, y + 35), sel ? IM_COL32(235, 205, 205, 255) : kDim, line.c_str());
    // Gamerscore and state.
    char g[16];
    std::snprintf(g, sizeof g, "%uG", e.info.gamerscore);
    const ImVec2 gz = TextSize(g_menu_font, ns, g);
    Text(dl, g_menu_font, ns, ImVec2(P(726, 0).x - gz.x, P(0, y + 7).y), e.unlocked ? kGold : kDim, g);
    if (e.unlocked) Tick(dl, P(715, y + 45), 16 * s, kGreen);
  }
  // Scroll bar.
  if (n > kVisible) {
    const float t = list_top, h = kVisible * (row_h + gap) - gap;
    dl->AddRectFilled(P(750, t), P(754, t + h), IM_COL32(40, 40, 48, 255), 2 * s);
    const float bh = h * kVisible / n, by = t + (h - bh) * top_ / float(n - kVisible);
    dl->AddRectFilled(P(750, by), P(754, by + bh), IM_COL32(200, 200, 208, 255), 2 * s);
  }

  // The selected achievement.
  const ImVec2 da = P(766, list_top), db = P(1200, list_top + kVisible * (row_h + gap) - gap);
  dl->AddRectFilled(da, db, IM_COL32(24, 24, 30, 245), 6 * s);
  dl->AddRect(da, db, IM_COL32(70, 70, 80, 255), 6 * s, 0, 1.0f * s);
  if (n) {
    const Entry& e = all_[shown_[sel_]];
    const float cx = 983;
    const ImVec2 ia = P(cx - 64, 152), ib = P(cx + 64, 280);
    if (auto* tex = Icon(e.info)) {
      dl->AddImage(reinterpret_cast<ImTextureID>(tex), ia, ib, ImVec2(0, 0), ImVec2(1, 1),
                   e.unlocked ? kWhite : IM_COL32(85, 85, 90, 255));
    }
    dl->AddRect(ia, ib, e.unlocked ? kGold : IM_COL32(90, 90, 100, 255), 4 * s, 0, 2 * s);
    if (!e.unlocked) Lock(dl, P(cx, 216), 52 * s, IM_COL32(235, 235, 240, 235));
    float y = 294;
    const float wrap = 400 * s;
    auto centred = [&](ImFont* font, float size, const std::string& text, ImU32 colour) {
      const ImVec2 z = TextSize(font, size, text.c_str(), wrap);
      // (each wrapped line centred would need per-line layout: centre the block)
      Text(dl, font, size, ImVec2(P(cx, 0).x - std::min(z.x, wrap) * 0.5f, P(0, y).y), colour, text.c_str(),
           wrap);
      y += z.y / s + 6;
    };
    centred(g_title_font, 27 * s, e.info.label, kWhite);
    char g[32];
    std::snprintf(g, sizeof g, "%u GAMERSCORE", e.info.gamerscore);
    centred(g_menu_font, 19 * s, g, kGold);
    if (e.unlocked) {
      const std::string when = e.when ? "UNLOCKED  " + Date(e.when) : "UNLOCKED";
      centred(g_menu_font, 19 * s, when, kGreen);
    } else {
      centred(g_menu_font, 19 * s, "LOCKED", kGrey);
    }
    y += 10;
    dl->AddLine(P(790, y), P(1176, y), IM_COL32(70, 70, 80, 255), 1.0f * s);
    y += 10;
    Text(dl, g_menu_font, 16 * s, P(790, y), kDim, "HOW TO UNLOCK");
    y += 24;
    const std::string& how = e.info.description.empty() ? e.info.unachieved_description : e.info.description;
    const float hs = 19 * s, hw = 386 * s;
    Text(dl, g_menu_font, hs, P(790, y), IM_COL32(230, 230, 235, 255), how.c_str(), hw);
    y += TextSize(g_menu_font, hs, how.c_str(), hw).y / s + 12;
    if (OnlineOnly(e.info.id) && !e.unlocked) {
      Text(dl, g_menu_font, 17 * s, P(790, y), IM_COL32(255, 180, 70, 255),
           "Needs Xbox LIVE, which the PC port doesn't have: it can't be earned.", hw);
    }
  }

  // Footer: the buttons.
  {
    const float cy = P(0, 660).y;
    char filter[40];
    std::snprintf(filter, sizeof filter, "SHOW: %s", kFilterNames[filter_]);
    float x = P(84, 0).x;
    x = Hint(dl, x, cy, s, "B", IM_COL32(200, 40, 40, 255), "BACK");
    x = Hint(dl, x, cy, s, "Y", IM_COL32(215, 170, 20, 255), filter);
    x = Hint(dl, x, cy, s, "LB", IM_COL32(90, 90, 100, 255), "");
    Hint(dl, x - 26 * s, cy, s, "RB", IM_COL32(90, 90, 100, 255), "PAGE");
  }

  if (back) {
    g_wait_release = true;
    g_open = false;
  }
}

}  // namespace

void InstallAchievementsPage(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                             rex::Runtime* runtime, rex::input::InputSystem* input) {
  g_immediate = immediate;
  g_runtime = runtime;
  g_input = input;
  new AchievementsPage(drawer);  // lives for the whole run
}

void SetAchievementsPageFonts(ImFont* menu, ImFont* title) {
  g_menu_font = menu;
  g_title_font = title;
}

void OpenAchievementsPage() { g_open_requested = true; }

bool AchievementsPageHoldsInput() { return g_open.load() || g_wait_release.load(); }

}  // namespace svr2011
