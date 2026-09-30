// WWE SmackDown vs. Raw 2011 - the GRAPHICS page (see graphics_page.h).
//
// Drawn over the game's OPTIONS panel in the same style (the panel, its
// header, one bar per setting, the selected one red with < > around the
// value), and every setting applies at once.

#include "graphics_page.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "achievements_page.h"
#include "fps_overlay.h"
#include "native/native_renderer.h"
#include "touch_controls.h"

namespace svr2011 {

namespace {

using Clock = std::chrono::steady_clock;

struct Resolution {
  int w, h;
  const char* label;
};
// Window sizes (the render scale follows the window: native_renderer.cpp).
constexpr Resolution kResolutions[] = {
    {1280, 720, "1280 X 720"},   {1600, 900, "1600 X 900"},   {1920, 1080, "1920 X 1080"},
    {2560, 1440, "2560 X 1440"}, {3840, 2160, "3840 X 2160"},
};
constexpr int kNumResolutions = int(std::size(kResolutions));
// QUALITY -> RENDER RESOLUTION: native_max_scale (AUTO: follow the window,
// up to 4x; the phone's screen is the window).
struct Scale {
  int value;
  const char* label;
};
constexpr Scale kScales[] = {{4, "AUTO"}, {1, "720P (XBOX 360)"}, {2, "1440P"}, {3, "2160P"}};
constexpr int kNumScales = int(std::size(kScales));

// The rows, on two tabs (LB / RB).
enum Row {
  kResolution, kDisplay, kVsync, kFpsCounter, kRenderer,         // DISPLAY
  kRenderScale, kAntiAliasing, kEffects, kCutsceneFps,           // QUALITY
  kTouch,                                                        // DISPLAY (last)
};
enum Tab { kDisplayTab, kQualityTab, kTabs };
const char* kTabNames[kTabs] = {"DISPLAY", "QUALITY"};
#if defined(__ANDROID__)
// (the phone: the window is the screen - no window size or mode)
const std::vector<Row> kTabRows[kTabs] = {{kVsync, kFpsCounter, kRenderer, kTouch},
                                          {kRenderScale, kAntiAliasing, kEffects, kCutsceneFps}};
#else
const std::vector<Row> kTabRows[kTabs] = {{kResolution, kDisplay, kVsync, kFpsCounter, kRenderer, kTouch},
                                          {kRenderScale, kAntiAliasing, kEffects, kCutsceneFps}};
#endif

std::filesystem::path g_config_path;
rex::ui::Window* g_window = nullptr;
rex::input::InputSystem* g_input = nullptr;
ImFont* g_menu_font = nullptr;
ImFont* g_title_font = nullptr;
std::atomic<bool> g_open_requested{false};
std::atomic<bool> g_open{false};
// After closing, the game keeps seeing an idle pad until every button is
// released, so the button that closed the page does not reach the menu.
std::atomic<bool> g_wait_release{false};

// Sets `key = value` among the settings file's top-level keys (before any
// [section]), keeping everything else (the launcher's format).
void SaveSetting(const std::string& key, const std::string& value) {
  std::vector<std::string> lines;
  {
    std::ifstream in(g_config_path);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
  }
  const std::string entry = key + " = " + value;
  size_t insert_at = lines.size();
  bool done = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& l = lines[i];
    const size_t first = l.find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    if (l[first] == '[') {
      insert_at = std::min(insert_at, i);
      break;
    }
    const size_t eq = l.find('=');
    if (eq == std::string::npos || l[first] == '#') continue;
    std::string k = l.substr(first, eq - first);
    k.erase(k.find_last_not_of(" \t") + 1);
    if (k == key) {
      lines[i] = entry;
      done = true;
      break;
    }
  }
  if (!done) lines.insert(lines.begin() + std::ptrdiff_t(insert_at), entry);
  const std::filesystem::path tmp = g_config_path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    for (const std::string& l : lines) out << l << "\n";
    if (!out) {
      REXLOG_WARN("GRAPHICS: could not save {}", g_config_path.string());
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, g_config_path, ec);
  if (ec) REXLOG_WARN("GRAPHICS: could not save {}: {}", g_config_path.string(), ec.message());
}

#if defined(_WIN32)
// The game's top-level window (this process's, titled like the game).
HWND GameWindow() {
  struct Find {
    DWORD pid;
    HWND found;
  } find{GetCurrentProcessId(), nullptr};
  EnumWindows(
      [](HWND h, LPARAM p) -> BOOL {
        auto* f = reinterpret_cast<Find*>(p);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        wchar_t title[128];
        if (pid == f->pid && IsWindowVisible(h) && GetWindowTextW(h, title, 128) &&
            wcsstr(title, L"SmackDown")) {
          f->found = h;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&find));
  return find.found;
}

// Gives the (windowed) game window a client area of w x h, centred on its
// monitor.
void ResizeWindow(int w, int h) {
  if (!g_window) return;
  g_window->app_context().CallInUIThread([w, h] {
    HWND hwnd = GameWindow();
    if (!hwnd || (g_window && g_window->IsFullscreen())) return;
    RECT rc = {0, 0, w, h};
    const DWORD style = DWORD(GetWindowLongW(hwnd, GWL_STYLE));
    const DWORD ex = DWORD(GetWindowLongW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    const int x = mi.rcWork.left + std::max(0, int(mi.rcWork.right - mi.rcWork.left - ww) / 2);
    const int y = mi.rcWork.top + std::max(0, int(mi.rcWork.bottom - mi.rcWork.top - wh) / 2);
    SetWindowPos(hwnd, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
  });
}
#else
// (Android: the window is the screen.)
void ResizeWindow(int, int) {}
#endif

class GraphicsPage final : public rex::ui::ImGuiDialog {
 public:
  explicit GraphicsPage(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void Load();
  void Change(int row, int dir);
  void Hide();  // (not ImGuiDialog::Close, which deletes the page)
  uint16_t PadButtons();

  int tab_ = kDisplayTab;
  int row_ = 0;  // (in the tab)
  int resolution_ = 2;
  int scale_ = 0;  // (kScales)
  bool touch_ = false;  // the on-screen controller
  bool msaa_ = false, fps_ = true, vsync_ = true, fullscreen_ = false, native_ = true;
  bool effects_ = true, fps60_ = true;
  bool native_at_start_ = false;
  // Native on Vulkan (gpu_backend = vulkan) chosen / running: the API is picked
  // when the game starts, so a change takes effect at the next start.
  bool vulkan_ = false, vulkan_at_start_ = false;
  uint16_t prev_buttons_ = 0;
  bool wait_release_ = false;
  Clock::time_point repeat_at_{};
  Clock::time_point opened_at_{};
};

void GraphicsPage::Load() {
  const int w = rex::cvar::Query<int32_t>("window_width");
  const int h = rex::cvar::Query<int32_t>("window_height");
  resolution_ = 2;
  for (int i = 0; i < kNumResolutions; ++i) {
    if (kResolutions[i].w == w && kResolutions[i].h == h) resolution_ = i;
  }
  msaa_ = rex::cvar::Query<bool>("native_2x_msaa");
  {
    const int32_t v = rex::cvar::Query<int32_t>("native_max_scale");
    scale_ = 0;
    for (int i = 0; i < kNumScales; ++i) {
      if (kScales[i].value == v) scale_ = i;
    }
  }
  effects_ = rex::cvar::Query<bool>("native_scale_effects");
  fps60_ = rex::cvar::Query<bool>("unlock_30fps");
  native_at_start_ = native::CanSwitch();
  vulkan_at_start_ = rex::cvar::Query<std::string>("gpu_backend") == "vulkan";
  vulkan_ = vulkan_at_start_;
  native_ = native_at_start_ ? native::NativeActive()
                             : rex::cvar::Query<std::string>("native_renderer") != "off";
  fps_ = FpsCounterVisible();
  touch_ = rex::cvar::Query<bool>("touch_controls");
  vsync_ = rex::cvar::Query<bool>("vsync");
  fullscreen_ = g_window ? g_window->IsFullscreen() : false;
}

void GraphicsPage::Change(int row, int dir) {
  switch (row) {
    case kRenderScale:
      scale_ = (scale_ + dir + kNumScales) % kNumScales;
      rex::cvar::SetFlagByName("native_max_scale", std::to_string(kScales[scale_].value));
      SaveSetting("native_max_scale", std::to_string(kScales[scale_].value));
      break;
    case kEffects:
      effects_ = !effects_;
      rex::cvar::SetFlagByName("native_scale_effects", effects_ ? "true" : "false");
      SaveSetting("native_scale_effects", effects_ ? "true" : "false");
      break;
    case kCutsceneFps:
      fps60_ = !fps60_;
      rex::cvar::SetFlagByName("unlock_30fps", fps60_ ? "true" : "false");
      SaveSetting("unlock_30fps", fps60_ ? "true" : "false");
      break;
    case kResolution: {
      resolution_ = (resolution_ + dir + kNumResolutions) % kNumResolutions;
      const Resolution& r = kResolutions[resolution_];
      ResizeWindow(r.w, r.h);
      SaveSetting("window_width", std::to_string(r.w));
      SaveSetting("window_height", std::to_string(r.h));
      // (the emulated renderer's scale, fixed at start)
      SaveSetting("resolution_scale", std::to_string(std::clamp((r.h + 719) / 720, 1, 3)));
      break;
    }
    case kAntiAliasing:
      msaa_ = !msaa_;
      rex::cvar::SetFlagByName("native_2x_msaa", msaa_ ? "true" : "false");
      SaveSetting("native_2x_msaa", msaa_ ? "true" : "false");
      break;
    case kTouch:
      touch_ = !touch_;
      rex::cvar::SetFlagByName("touch_controls", touch_ ? "true" : "false");
      SaveSetting("touch_controls", touch_ ? "true" : "false");
      break;
    case kFpsCounter:
      fps_ = !fps_;
      SetFpsCounterVisible(fps_);
      SaveSetting("show_fps", fps_ ? "true" : "false");
      break;
    case kVsync:
      vsync_ = !vsync_;
      rex::cvar::SetFlagByName("vsync", vsync_ ? "true" : "false");
      SaveSetting("vsync", vsync_ ? "true" : "false");
      break;
    case kDisplay:
      fullscreen_ = !fullscreen_;
      if (g_window) g_window->SetFullscreen(fullscreen_);
      if (!fullscreen_) {
        const Resolution& r = kResolutions[resolution_];
        ResizeWindow(r.w, r.h);
      }
      SaveSetting("fullscreen", fullscreen_ ? "true" : "false");
      break;
    case kRenderer: {
      // NATIVE, EMULATED, NATIVE VULKAN (EXPERIMENTAL) - the launcher's list.
      int choice = !native_ ? 1 : vulkan_ ? 2 : 0;
      choice = (choice + dir + 3) % 3;
      native_ = choice != 1;
      vulkan_ = choice == 2;
      if (native_at_start_) native::SetNativeActive(native_);
      SaveSetting("native_renderer", native_ ? "\"main\"" : "\"off\"");
      SaveSetting("gpu_backend", vulkan_ ? "\"vulkan\"" : "\"any\"");
      break;
    }
    default:
      break;
  }
}

void GraphicsPage::Hide() {
  g_wait_release = true;
  g_open = false;
}

uint16_t GraphicsPage::PadButtons() {
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

// Text in `font` at `size` pixels.
void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text) {
  dl->AddText(font ? font : ImGui::GetFont(), size, at, colour, text);
}
ImVec2 TextSize(ImFont* font, float size, const char* text) {
  return (font ? font : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

void GraphicsPage::OnDraw(ImGuiIO& io) {
  using namespace rex::input;
  if (g_open_requested.exchange(false)) {
    Load();
    tab_ = kDisplayTab;
    row_ = 0;
    // The A that opened the page is still down: wait for everything to be
    // released, judged only on pad states the game polled after opening.
    wait_release_ = true;
    opened_at_ = Clock::now();
    g_open = true;
  }
  if (!g_open) {
    if (g_wait_release && (!g_input || PadButtons() == 0)) g_wait_release = false;
    return;
  }
  if (g_window) fullscreen_ = g_window->IsFullscreen();  // (Alt+Enter)

  // Input: controller (edges, with auto-repeat for held directions) and keys.
  const uint16_t buttons = PadButtons();
  if (wait_release_) {
    if (buttons == 0 && Clock::now() - opened_at_ > std::chrono::milliseconds(150)) {
      wait_release_ = false;
    }
    prev_buttons_ = buttons;
  }
  const uint16_t pressed = wait_release_ ? 0 : uint16_t(buttons & ~prev_buttons_);
  constexpr uint16_t kDirs = X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN |
                             X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_RIGHT;
  uint16_t act = pressed;
  const auto now = Clock::now();
  if (pressed & kDirs) {
    repeat_at_ = now + std::chrono::milliseconds(400);
  } else if (!wait_release_ && (buttons & kDirs) && now >= repeat_at_) {
    act |= buttons & kDirs;
    repeat_at_ = now + std::chrono::milliseconds(130);
  }
  if (!wait_release_) prev_buttons_ = buttons;
  auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
  int move = 0, dir = 0;
  bool back = false;
  if ((act & X_INPUT_GAMEPAD_DPAD_UP) || key(ImGuiKey_UpArrow)) move = -1;
  if ((act & X_INPUT_GAMEPAD_DPAD_DOWN) || key(ImGuiKey_DownArrow)) move = 1;
  if ((act & X_INPUT_GAMEPAD_DPAD_LEFT) || key(ImGuiKey_LeftArrow)) dir = -1;
  if ((act & X_INPUT_GAMEPAD_DPAD_RIGHT) || key(ImGuiKey_RightArrow)) dir = 1;
  if ((pressed & X_INPUT_GAMEPAD_A) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) dir = 1;
  if ((pressed & (X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_START | X_INPUT_GAMEPAD_BACK)) ||
      ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    back = true;
  }
  if ((pressed & (X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_RIGHT_SHOULDER)) ||
      ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
    tab_ = (tab_ + 1) % kTabs;
    row_ = 0;
  }
  const std::vector<Row>& rows = kTabRows[tab_];
  const int n = int(rows.size());
  if (move) row_ = (row_ + move + n) % n;
  if (dir) Change(rows[row_], dir);

  // Layout in the game's 1280 x 720 frame (letterboxed into the window), over
  // the OPTIONS panel: the panel 144..1134 x 78..542, its header 78..112.
  float fw = io.DisplaySize.x, fh = fw * 9.0f / 16.0f;
  if (fh > io.DisplaySize.y) fh = io.DisplaySize.y, fw = fh * 16.0f / 9.0f;
  const float ox = (io.DisplaySize.x - fw) * 0.5f, oy = (io.DisplaySize.y - fh) * 0.5f;
  const float s = fh / 720.0f;
  auto P = [&](float x, float y) { return ImVec2(ox + x * s, oy + y * s); };

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  // Panel: dark glass with a thin white rim, as the game's.
  dl->AddRectFilled(P(144, 78), P(1134, 542), IM_COL32(8, 8, 12, 255), 10 * s);
  dl->AddRectFilledMultiColor(P(146, 80), P(1132, 300), IM_COL32(40, 40, 48, 120),
                              IM_COL32(40, 40, 48, 120), IM_COL32(8, 8, 12, 0),
                              IM_COL32(8, 8, 12, 0));
  dl->AddRect(P(144, 78), P(1134, 542), IM_COL32(235, 235, 240, 255), 10 * s, 0, 2.5f * s);
  // Header: the dotted strip, the white tab with the title, the page name.
  dl->AddRectFilled(P(150, 84), P(1128, 118), IM_COL32(24, 24, 30, 255), 6 * s);
  for (float x = 160; x < 1120; x += 7) {
    for (float y = 90; y < 114; y += 7) {
      dl->AddCircleFilled(P(x, y), 1.2f * s, IM_COL32(70, 70, 80, 255));
    }
  }
  dl->AddQuadFilled(P(248, 84), P(690, 84), P(672, 118), P(236, 118), IM_COL32(12, 12, 16, 255));
  dl->AddQuad(P(248, 84), P(690, 84), P(672, 118), P(236, 118), IM_COL32(235, 235, 240, 255),
              2 * s);
  {
    const char* title = "OPTIONS";
    const float ts = 34 * s;
    const ImVec2 sz = TextSize(g_title_font, ts, title);
    Text(dl, g_title_font, ts, ImVec2(P(462, 0).x - sz.x * 0.5f, P(0, 101).y - sz.y * 0.5f),
         IM_COL32(255, 255, 255, 255), title);
    // GRAPHICS: its tabs (LB / RB), the chosen one white on red.
    const float ps = 20 * s;
    float x = P(712, 0).x;
    const float cy = P(0, 101).y;
    for (int i = 0; i < kTabs; ++i) {
      const ImVec2 z = TextSize(g_menu_font, ps, kTabNames[i]);
      const ImVec2 a(x - 10 * s, cy - 13 * s), b(x + z.x + 10 * s, cy + 13 * s);
      if (i == tab_) {
        dl->AddRectFilled(a, b, IM_COL32(150, 18, 20, 255), 3 * s);
        dl->AddRect(a, b, IM_COL32(230, 60, 60, 255), 3 * s, 0, 1.2f * s);
      }
      Text(dl, g_menu_font, ps, ImVec2(x, cy - z.y * 0.5f),
           i == tab_ ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 150, 158, 255), kTabNames[i]);
      x += z.x + 30 * s;
    }
    const float hs = 15 * s;
    const char* hint = "LB / RB";
    const ImVec2 hz = TextSize(g_menu_font, hs, hint);
    Text(dl, g_menu_font, hs, ImVec2(P(1112, 0).x - hz.x, cy - hz.y * 0.5f), IM_COL32(150, 150, 158, 255),
         hint);
  }

  // Rows.
  const bool restart_renderer = (!native_at_start_ && native_) || vulkan_ != vulkan_at_start_;
  auto value = [&](Row id) -> const char* {
    switch (id) {
      case kResolution: return fullscreen_ ? "FULL SCREEN" : kResolutions[resolution_].label;
      case kDisplay: return fullscreen_ ? "FULL SCREEN" : "WINDOWED";
      case kVsync: return vsync_ ? "ON" : "OFF";
      case kFpsCounter: return fps_ ? "ON" : "OFF";
      case kTouch: return touch_ ? "ON" : "OFF";
      case kRenderer: return native_ ? (vulkan_ ? "NATIVE VULKAN" : "NATIVE") : "EMULATED";
      case kRenderScale: return kScales[scale_].label;
      case kAntiAliasing: return msaa_ ? "ON" : "OFF";
      case kEffects: return effects_ ? "HIGH" : "NORMAL";
      case kCutsceneFps: return fps60_ ? "60 FPS" : "30 FPS (ORIGINAL)";
    }
    return "";
  };
  auto label = [](Row id) -> const char* {
    switch (id) {
      case kResolution: return "RESOLUTION";
      case kDisplay: return "DISPLAY MODE";
      case kVsync: return "VSYNC";
      case kFpsCounter: return "FPS COUNTER";
      case kTouch: return "TOUCH CONTROLS";
      case kRenderer: return "RENDERER";
      case kRenderScale: return "RENDER RESOLUTION";
      case kAntiAliasing: return "ANTI-ALIASING";
      case kEffects: return "SHADOWS & EFFECTS";
      case kCutsceneFps: return "ENTRANCE FRAME RATE";
    }
    return "";
  };
  auto help_for = [](Row id) -> const char* {
    switch (id) {
      case kResolution: return "The window's size. The game renders at the scale that fills it.";
      case kDisplay: return "Play in a window or full screen (Alt+Enter also switches).";
      case kVsync: return "Waits for the monitor's refresh: no tearing.";
      case kFpsCounter: return "Shows the frame rate at the top of the screen (F2).";
      case kTouch: return "The on-screen controller. Its EDIT button moves, resizes and remaps it.";
      case kRenderer: return "Native: the PC renderer (fastest). Emulated: the Xbox 360 GPU emulation.";
      case kRenderScale: return "The most the game renders at. AUTO fills the screen; lower is faster.";
      case kAntiAliasing: return "Renders at twice the resolution and averages it down: smooth edges, slower.";
      case kEffects: return "HIGH: shadows, reflections and glow at the render resolution. NORMAL: faster.";
      case kCutsceneFps: return "Entrances and cutscenes at 60 fps, or 30 as on the Xbox 360 (half the work).";
    }
    return "";
  };
  row_ = std::clamp(row_, 0, n - 1);
  const float row_h = 34, gap = 13;
  const float top = 310 - (float(n) * (row_h + gap) - gap) * 0.5f;
  const float fs = 22 * s;
  for (int i = 0; i < n; ++i) {
    const Row id = rows[i];
    const float y = top + i * (row_h + gap);
    const bool sel = i == row_;
    const ImVec2 a = P(166, y), b = P(1112, y + row_h);
    if (sel) {
      dl->AddRectFilledMultiColor(a, b, IM_COL32(150, 18, 20, 255), IM_COL32(95, 8, 10, 255),
                                  IM_COL32(95, 8, 10, 255), IM_COL32(150, 18, 20, 255));
      dl->AddRect(a, b, IM_COL32(230, 60, 60, 255), 3 * s, 0, 1.5f * s);
    } else {
      dl->AddRectFilled(a, b, IM_COL32(34, 34, 40, 235), 3 * s);
    }
    const float ty = P(0, y + row_h * 0.5f).y - TextSize(g_menu_font, fs, "A").y * 0.5f;
    Text(dl, g_menu_font, fs, ImVec2(P(186, 0).x, ty), IM_COL32(255, 255, 255, 255), label(id));
    const char* v = value(id);
    const ImVec2 vs = TextSize(g_menu_font, fs, v);
    const float vx = P(930, 0).x - vs.x * 0.5f;
    const ImU32 vc = (id == kRenderer && restart_renderer) ? IM_COL32(255, 200, 60, 255)
                                                            : IM_COL32(255, 255, 255, 255);
    Text(dl, g_menu_font, fs, ImVec2(vx, ty), vc, v);
    if (sel) {
      const float cy = P(0, y + row_h * 0.5f).y, h = 8 * s;
      const float lx = P(780, 0).x, rx = P(1080, 0).x;
      dl->AddTriangleFilled(ImVec2(lx - h, cy), ImVec2(lx + h * 0.6f, cy - h),
                            ImVec2(lx + h * 0.6f, cy + h), IM_COL32(255, 255, 255, 255));
      dl->AddTriangleFilled(ImVec2(rx + h, cy), ImVec2(rx - h * 0.6f, cy - h),
                            ImVec2(rx - h * 0.6f, cy + h), IM_COL32(255, 255, 255, 255));
    }
  }
  // Description of the selected setting, as the game's panels have.
  {
    const Row id = rows[row_];
    std::string help = help_for(id);
    if (id == kRenderer && native_ && vulkan_) help = "EXPERIMENTAL: the native renderer on Vulkan.";
    if (id == kRenderer && restart_renderer)
      help = native_ && vulkan_ ? "EXPERIMENTAL - takes effect the next time the game starts."
                                : "Takes effect the next time the game starts.";
    const float hs = 18 * s;
    const ImVec2 sz = TextSize(g_menu_font, hs, help.c_str());
    Text(dl, g_menu_font, hs, ImVec2(P(639, 0).x - sz.x * 0.5f, P(0, 508).y - sz.y * 0.5f),
         IM_COL32(200, 200, 205, 255), help.c_str());
  }

  if (back) Hide();
}

}  // namespace

void InstallGraphicsPage(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                         rex::input::InputSystem* input,
                         const std::filesystem::path& config_path) {
  g_window = window;
  g_input = input;
  g_config_path = config_path;
  new GraphicsPage(drawer);  // lives for the whole run
  if (input) {
    // (one hold for the port's pages: this one and ACHIEVEMENTS)
    input->SetGuestInputHold(
        [] {
          return g_open.load() || g_wait_release.load() || AchievementsPageHoldsInput() ||
                 TouchControlsHoldInput();
        });
  }
}

void SetGraphicsPageFonts(ImFont* menu, ImFont* title) {
  g_menu_font = menu;
  g_title_font = title;
}

void OpenGraphicsPage() { g_open_requested = true; }

void RequestExit() {
  rex::ui::Window* w = g_window;
  if (!w) return;
  w->app_context().CallInUIThread([w] { w->RequestClose(); });
}

}  // namespace svr2011
