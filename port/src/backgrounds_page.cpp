// WWE SmackDown vs. Raw 2011 - MY WWE -> OPTIONS -> BACKGROUNDS (see
// backgrounds_page.h; research: docs/MENU_BACKGROUNDS.md).
//
// The picture number lives in the game's data struct (*0x82E3DDFC + 8680),
// 0 at every start. The main menu's background object (sub_82458F58, its
// constructor) reads it into +68 and loads "/MENU/EB%02d"; the main menu's
// destructor moves it on with sub_8258A990 (+1, back to 0 at 12). Here that
// step skips the pictures that are off, and a new background object whose
// number is off (the first one after start, or after the page changed the
// set) takes the next one that is on.

#include "backgrounds_page.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"
#include "graphics_page.h"

REXCVAR_DEFINE_STRING(menu_backgrounds_off, "", "UI",
                      "MY WWE -> OPTIONS -> BACKGROUNDS: the main menu pictures that are off (their numbers 1-12, "
                      "comma-separated; all off = all on)");

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kCount = 12;  // menuHD.pac MENU/EB00-EB11 (the game's own limit, sub_8258A990)
// From the pictures (no names in the game's data); the two that can't be
// told for sure are numbered.
const char* const kName[kCount] = {"THE MIZ",        "BACKGROUND 2",  "JOHN CENA",     "CM PUNK",
                                   "EDGE",           "TRIPLE H",      "SHAWN MICHAELS", "REY MYSTERIO",
                                   "RANDY ORTON",    "BACKGROUND 10", "JACK SWAGGER",  "THE UNDERTAKER"};
const char* const kWhat[kCount] = {"With the microphone",       "Flying",
                                   "Close-up",                  "With the Money in the Bank briefcase",
                                   "With the WWE Championship", "Entrance",
                                   "Ladder dive",               "In the ring",
                                   "From behind",               "Shouting",
                                   "Arms up",                   "Entrance"};

constexpr uint32_t kGameData = 0x82E3DDFC;  // -> +8680: the picture number
constexpr uint32_t kPictureAt = 8680;
constexpr uint32_t kObjectPicture = 68;  // the background object's number

std::mutex g_mutex;
std::array<bool, kCount> g_on;  // under g_mutex
uint8_t* g_base = nullptr;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

void LoadSettings() {
  std::array<bool, kCount> on;
  on.fill(true);
  std::stringstream ss(rex::cvar::Query<std::string>("menu_backgrounds_off"));
  for (std::string n; std::getline(ss, n, ',');) {
    const int i = std::atoi(n.c_str()) - 1;
    if (i >= 0 && i < kCount) on[i] = false;
  }
  std::lock_guard lock(g_mutex);
  g_on = on;
}

void SaveSettings() {
  std::string off;
  {
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < kCount; ++i)
      if (!g_on[i]) off += (off.empty() ? "" : ",") + std::to_string(i + 1);
  }
  rex::cvar::SetFlagByName("menu_backgrounds_off", off);
  svr2011::SaveConfigSetting("menu_backgrounds_off", "\"" + off + "\"");
}

// Picture i plays (every one when all are off).
bool Allowed(int i) {
  std::lock_guard lock(g_mutex);
  if (std::find(g_on.begin(), g_on.end(), true) == g_on.end()) return i >= 0 && i < kCount;
  return i >= 0 && i < kCount && g_on[i];
}

// The first picture from `from` on (wrapping) that plays.
int FirstAllowed(int from) {
  for (int k = 0; k < kCount; ++k) {
    const int i = ((from % kCount) + kCount + k) % kCount;
    if (Allowed(i)) return i;
  }
  return 0;
}

// The picture on screen now (-1: unknown).
int Current() {
  if (!g_base) return -1;
  const uint32_t data = Rd32(g_base + kGameData);
  if (!data) return -1;
  const int i = int(Rd32(g_base + data + kPictureAt));
  return i >= 0 && i < kCount ? i : -1;
}

}  // namespace

// The main menu moves on to the next picture (sub_8258A990(&number): +1, 0
// at 12): the next one that is on.
REX_EXTERN(__imp__sub_8258A990);
REX_HOOK_RAW(sub_8258A990) {
  g_base = base;
  const uint32_t at = ctx.r3.u32;
  if (!at) return;
  const int now = int(Rd32(base + at));
  const int next = FirstAllowed(now + 1);
  Wr32(base + at, uint32_t(next));
}

// A main-menu background is made (sub_82458F58(object, ...)): its picture
// number (+68, read from the game's data) is one that is on.
REX_EXTERN(__imp__sub_82458F58);
REX_HOOK_RAW(sub_82458F58) {
  g_base = base;
  const uint32_t object = ctx.r3.u32;
  __imp__sub_82458F58(ctx, base);
  if (!object) return;
  const int i = int(Rd32(base + object + kObjectPicture));
  if (Allowed(i)) return;
  const int next = FirstAllowed(i + 1);
  Wr32(base + object + kObjectPicture, uint32_t(next));
  if (const uint32_t data = Rd32(base + kGameData)) Wr32(base + data + kPictureAt, uint32_t(next));
  REXLOG_INFO("[svr2011] backgrounds: picture {} is off - {} ({}) instead", i + 1, next + 1, kName[next]);
}

// ---------------------------------------------------------------------------
// The page (the JUKEBOX page's look).

namespace {

rex::input::InputSystem* g_input = nullptr;
ImFont* g_menu_font = nullptr;
ImFont* g_title_font = nullptr;
std::atomic<bool> g_open_requested{false};
std::atomic<bool> g_open{false};
std::atomic<bool> g_wait_release{false};

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text) {
  dl->AddText(font ? font : ImGui::GetFont(), size, at, colour, text);
}
ImVec2 TextSize(ImFont* font, float size, const char* text) {
  return (font ? font : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

// A controller button glyph (a coloured disc with its letter) and a label.
float Hint(ImDrawList* dl, float x, float cy, float s, const char* button, ImU32 colour, const char* label) {
  const float r = 13 * s;
  dl->AddCircleFilled(ImVec2(x + r, cy), r, colour, 24);
  const float gs = 16 * s;
  const ImVec2 gz = TextSize(g_menu_font, gs, button);
  Text(dl, g_menu_font, gs, ImVec2(x + r - gz.x * 0.5f, cy - gz.y * 0.5f), IM_COL32(255, 255, 255, 255), button);
  const float ls = 18 * s;
  const ImVec2 lz = TextSize(g_menu_font, ls, label);
  Text(dl, g_menu_font, ls, ImVec2(x + r * 2 + 8 * s, cy - lz.y * 0.5f), IM_COL32(230, 230, 235, 255), label);
  return x + r * 2 + 8 * s + lz.x + 26 * s;
}

class BackgroundsPage final : public rex::ui::ImGuiDialog {
 public:
  explicit BackgroundsPage(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  uint16_t PadButtons();
  void Set(int i, bool on);

  int sel_ = 0, top_ = 0;
  uint16_t prev_buttons_ = 0;
  bool wait_release_ = false;
  Clock::time_point repeat_at_{}, opened_at_{};
};

uint16_t BackgroundsPage::PadButtons() {
  using namespace rex::input;
  X_INPUT_STATE s = {};
  if (!g_input || !g_input->HeldState(0, &s)) return 0;
  uint16_t b = s.gamepad.buttons;
  constexpr int kDead = 16000;
  if (s.gamepad.thumb_ly > kDead) b |= X_INPUT_GAMEPAD_DPAD_UP;
  if (s.gamepad.thumb_ly < -kDead) b |= X_INPUT_GAMEPAD_DPAD_DOWN;
  return b;
}

// i < 0: every picture.
void BackgroundsPage::Set(int i, bool on) {
  {
    std::lock_guard lock(g_mutex);
    for (int k = 0; k < kCount; ++k)
      if (i < 0 || k == i) g_on[k] = on;
  }
  SaveSettings();
}

void BackgroundsPage::OnDraw(ImGuiIO& io) {
  using namespace rex::input;
  constexpr int kVisible = 7;
  if (g_open_requested.exchange(false)) {
    sel_ = top_ = 0;
    wait_release_ = true;  // (the A that opened the page is still down)
    opened_at_ = Clock::now();
    g_open = true;
    REXLOG_INFO("[svr2011] backgrounds: page open");
  }
  if (!g_open) {
    if (g_wait_release && (!g_input || PadButtons() == 0)) g_wait_release = false;
    return;
  }

  // Input: controller (edges, auto-repeat for held directions), keys, touch.
  const uint16_t buttons = PadButtons();
  if (wait_release_) {
    if (buttons == 0 && Clock::now() - opened_at_ > std::chrono::milliseconds(150)) wait_release_ = false;
    prev_buttons_ = buttons;
  }
  const uint16_t pressed = wait_release_ ? 0 : uint16_t(buttons & ~prev_buttons_);
  constexpr uint16_t kRepeat = X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN;
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
  if ((act & X_INPUT_GAMEPAD_DPAD_UP) || key(ImGuiKey_UpArrow)) sel_ = (sel_ + kCount - 1) % kCount;
  if ((act & X_INPUT_GAMEPAD_DPAD_DOWN) || key(ImGuiKey_DownArrow)) sel_ = (sel_ + 1) % kCount;
  std::array<bool, kCount> on;
  {
    std::lock_guard lock(g_mutex);
    on = g_on;
  }
  bool toggle = (pressed & X_INPUT_GAMEPAD_A) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false);
  bool all = (pressed & X_INPUT_GAMEPAD_Y) || ImGui::IsKeyPressed(ImGuiKey_Y, false);
  bool back = (pressed & (X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_START | X_INPUT_GAMEPAD_BACK)) ||
              ImGui::IsKeyPressed(ImGuiKey_Escape, false);

  // Layout in the game's 1280 x 720 frame, letterboxed into the window.
  float fw = io.DisplaySize.x, fh = fw * 9.0f / 16.0f;
  if (fh > io.DisplaySize.y) fh = io.DisplaySize.y, fw = fh * 16.0f / 9.0f;
  const float ox = (io.DisplaySize.x - fw) * 0.5f, oy = (io.DisplaySize.y - fh) * 0.5f;
  const float s = fh / 720.0f;
  auto P = [&](float x, float y) { return ImVec2(ox + x * s, oy + y * s); };
  auto inside = [&](ImVec2 a, ImVec2 b) {
    const ImVec2 m = io.MousePos;
    return m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
  };
  const bool tap = !wait_release_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const ImU32 kWhite = IM_COL32(255, 255, 255, 255), kGrey = IM_COL32(170, 170, 178, 255),
              kDim = IM_COL32(120, 120, 128, 255), kGreen = IM_COL32(90, 200, 90, 255),
              kGold = IM_COL32(255, 200, 60, 255);
  const float row_h = 62, gap = 6, list_top = 134;
  // (touch: a row chooses and turns on / off; the footer's buttons work too)
  if (tap) {
    for (int r = 0; r < kVisible && top_ + r < kCount; ++r) {
      const float y = list_top + r * (row_h + gap);
      if (inside(P(80, y), P(744, y + row_h))) {
        sel_ = top_ + r;
        toggle = true;
      }
    }
  }
  if (toggle) Set(sel_, !on[sel_]);
  const bool some_off = std::find(on.begin(), on.end(), false) != on.end();
  if (all) Set(-1, some_off);
  {
    std::lock_guard lock(g_mutex);
    on = g_on;
  }
  if (sel_ < top_) top_ = sel_;
  if (sel_ >= top_ + kVisible) top_ = sel_ - kVisible + 1;
  top_ = std::clamp(top_, 0, kCount - kVisible);
  const int showing = Current();
  const int n_on = int(std::count(on.begin(), on.end(), true));

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
  dl->AddRectFilled(P(60, 40), P(1220, 684), IM_COL32(8, 8, 12, 235), 10 * s);
  dl->AddRectFilledMultiColor(P(62, 42), P(1218, 260), IM_COL32(40, 40, 48, 120), IM_COL32(40, 40, 48, 120),
                              IM_COL32(8, 8, 12, 0), IM_COL32(8, 8, 12, 0));
  dl->AddRect(P(60, 40), P(1220, 684), IM_COL32(235, 235, 240, 255), 10 * s, 0, 2.5f * s);
  // Header: the dotted strip, the white-rimmed tab with the title, the page name.
  dl->AddRectFilled(P(66, 46), P(1214, 80), IM_COL32(24, 24, 30, 255), 6 * s);
  for (float x = 76; x < 1206; x += 7)
    for (float y = 52; y < 76; y += 7) dl->AddCircleFilled(P(x, y), 1.2f * s, IM_COL32(70, 70, 80, 255));
  dl->AddQuadFilled(P(164, 46), P(606, 46), P(588, 80), P(152, 80), IM_COL32(12, 12, 16, 255));
  dl->AddQuad(P(164, 46), P(606, 46), P(588, 80), P(152, 80), IM_COL32(235, 235, 240, 255), 2 * s);
  {
    const char* title = "OPTIONS";
    const float ts = 34 * s;
    const ImVec2 sz = TextSize(g_title_font, ts, title);
    Text(dl, g_title_font, ts, ImVec2(P(378, 0).x - sz.x * 0.5f, P(0, 63).y - sz.y * 0.5f), kWhite, title);
    const float ps = 22 * s;
    const ImVec2 psz = TextSize(g_menu_font, ps, "BACKGROUNDS");
    Text(dl, g_menu_font, ps, ImVec2(P(634, 0).x, P(0, 63).y - psz.y * 0.5f), kWhite, "BACKGROUNDS");
  }
  // Summary: pictures on, and a bar.
  {
    const float ss = 21 * s, cy = P(0, 108).y;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%d OF %d PICTURES ON", n_on, kCount);
    Text(dl, g_menu_font, ss, ImVec2(P(84, 0).x, cy - TextSize(g_menu_font, ss, buf).y * 0.5f), kWhite, buf);
    const char* note = n_on ? "THE MAIN MENU SHOWS THE PICTURES THAT ARE ON" : "ALL OFF: THE MAIN MENU SHOWS THEM ALL";
    const ImVec2 nz = TextSize(g_menu_font, 17 * s, note);
    Text(dl, g_menu_font, 17 * s, ImVec2(P(1196, 0).x - nz.x, cy - nz.y * 0.5f), n_on ? kGrey : kGold, note);
    const ImVec2 a = P(360, 102), b(P(1196, 0).x - nz.x - 24 * s, P(0, 114).y);
    if (b.x > a.x) {
      dl->AddRectFilled(a, b, IM_COL32(40, 40, 48, 255), 4 * s);
      if (n_on)
        dl->AddRectFilledMultiColor(a, ImVec2(a.x + (b.x - a.x) * n_on / kCount, b.y), IM_COL32(200, 30, 32, 255),
                                    IM_COL32(240, 70, 60, 255), IM_COL32(240, 70, 60, 255), IM_COL32(200, 30, 32, 255));
    }
  }
  // The list.
  for (int r = 0; r < kVisible && top_ + r < kCount; ++r) {
    const int i = top_ + r;
    const float y = list_top + r * (row_h + gap);
    const bool sel = i == sel_;
    const ImVec2 a = P(80, y), b = P(744, y + row_h);
    if (sel) {
      dl->AddRectFilledMultiColor(a, b, IM_COL32(150, 18, 20, 255), IM_COL32(95, 8, 10, 255), IM_COL32(95, 8, 10, 255),
                                  IM_COL32(150, 18, 20, 255));
      dl->AddRect(a, b, IM_COL32(230, 60, 60, 255), 3 * s, 0, 1.5f * s);
    } else {
      dl->AddRectFilled(a, b, IM_COL32(34, 34, 40, 235), 3 * s);
    }
    const ImVec2 sa = P(94, y + 18), sb = P(150, y + 44);
    dl->AddRectFilled(sa, sb, on[i] ? kGreen : IM_COL32(70, 70, 78, 255), 13 * s);
    const float kx = on[i] ? sb.x - 13 * s : sa.x + 13 * s;
    dl->AddCircleFilled(ImVec2(kx, (sa.y + sb.y) * 0.5f), 10 * s, kWhite, 20);
    const float ns = 21 * s, ds = 16 * s;
    char num[8];
    std::snprintf(num, sizeof num, "%02d", i + 1);
    Text(dl, g_menu_font, ns, P(166, y + 7), on[i] ? kGrey : kDim, num);
    Text(dl, g_menu_font, ns, P(206, y + 7), on[i] || sel ? kWhite : kGrey, kName[i]);
    Text(dl, g_menu_font, ds, P(206, y + 35), sel ? IM_COL32(235, 205, 205, 255) : kDim, kWhat[i]);
    if (i == showing) {
      const ImVec2 tz = TextSize(g_menu_font, ds, "ON SCREEN");
      Text(dl, g_menu_font, ds, ImVec2(P(726, 0).x - tz.x, P(0, y + 22).y), kGold, "ON SCREEN");
    } else if (!on[i]) {
      const ImVec2 oz = TextSize(g_menu_font, ds, "OFF");
      Text(dl, g_menu_font, ds, ImVec2(P(726, 0).x - oz.x, P(0, y + 22).y), kDim, "OFF");
    }
  }
  {  // scroll bar
    const float tp = list_top, h = kVisible * (row_h + gap) - gap;
    dl->AddRectFilled(P(750, tp), P(754, tp + h), IM_COL32(40, 40, 48, 255), 2 * s);
    const float bh = h * kVisible / kCount, by = tp + (h - bh) * top_ / float(kCount - kVisible);
    dl->AddRectFilled(P(750, by), P(754, by + bh), IM_COL32(200, 200, 208, 255), 2 * s);
  }
  // The side panel: how it works (the picture itself is behind the page).
  const ImVec2 da = P(766, list_top), db = P(1200, list_top + kVisible * (row_h + gap) - gap);
  dl->AddRectFilled(da, db, IM_COL32(24, 24, 30, 245), 6 * s);
  dl->AddRect(da, db, IM_COL32(70, 70, 80, 255), 6 * s, 0, 1.0f * s);
  {
    Text(dl, g_menu_font, 16 * s, P(790, list_top + 14), kDim, "MAIN MENU");
    auto line = [&](float y, float size, const char* text, ImU32 colour) {
      Text(dl, g_menu_font, size, P(790, y), colour, text);
    };
    line(list_top + 52, 24 * s, kName[sel_], kWhite);
    line(list_top + 84, 17 * s, on[sel_] ? "ON - IN THE CYCLE" : "OFF - SKIPPED", on[sel_] ? kGreen : kGold);
    line(list_top + 140, 16 * s, "The main menu shows one picture and", kGrey);
    line(list_top + 164, 16 * s, "moves on to the next every time you", kGrey);
    line(list_top + 188, 16 * s, "leave it. Pictures that are off are", kGrey);
    line(list_top + 212, 16 * s, "skipped; with all off, all play.", kGrey);
    char buf[64];
    std::snprintf(buf, sizeof buf, "ON SCREEN NOW: %s", showing >= 0 ? kName[showing] : "-");
    line(list_top + 270, 16 * s, buf, kGold);
  }
  // Footer: the buttons (tappable).
  {
    const float cy = P(0, 660).y;
    float x = P(84, 0).x;
    const float x_b = x;
    x = Hint(dl, x, cy, s, "B", IM_COL32(200, 40, 40, 255), "BACK");
    const float x_a = x;
    x = Hint(dl, x, cy, s, "A", IM_COL32(60, 160, 60, 255), on[sel_] ? "TURN OFF" : "TURN ON");
    const float x_y = x;
    x = Hint(dl, x, cy, s, "Y", IM_COL32(215, 170, 20, 255), some_off ? "ALL ON" : "ALL OFF");
    if (tap && io.MousePos.y > cy - 16 * s && io.MousePos.y < cy + 16 * s) {
      if (io.MousePos.x >= x_b && io.MousePos.x < x_a) back = true;
      else if (io.MousePos.x >= x_a && io.MousePos.x < x_y) Set(sel_, !on[sel_]);
      else if (io.MousePos.x >= x_y && io.MousePos.x < x) Set(-1, some_off);
    }
  }

  if (back) {
    REXLOG_INFO("[svr2011] backgrounds: page closed");
    g_wait_release = true;
    g_open = false;
  }
}

}  // namespace

namespace svr2011 {

void InstallBackgroundsPage(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input) {
  g_input = input;
  LoadSettings();
  new BackgroundsPage(drawer);  // lives for the whole run
}

void SetBackgroundsPageFonts(ImFont* menu, ImFont* title) {
  g_menu_font = menu;
  g_title_font = title;
}

void OpenBackgroundsPage() { g_open_requested = true; }

bool BackgroundsPageHoldsInput() { return g_open.load() || g_wait_release.load(); }

}  // namespace svr2011
