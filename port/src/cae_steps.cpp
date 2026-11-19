// WWE SmackDown vs. Raw 2011 - CREATE AN ENTRANCE's editor steps in the log (cae_steps.h).
#include "cae_steps.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

#include <rex/logging.h>

#include "menu_touch.h"

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_open{false};
std::mutex g_mutex;
std::string g_press;  // the last press or touch, for the next line ("A, pad", "a, touch", "tap 0.24,0.16")
Clock::time_point g_press_at;
std::string g_touch;  // (a touch just before a press: the press came from the touch controller)
Clock::time_point g_touch_at;
std::string g_page;     // the page logged last
std::string g_seen;     // the page drawn now, and since when (a page counts once it stays 250 ms: lists slide in)
Clock::time_point g_seen_at;
Clock::time_point g_checked;
uint16_t g_buttons = 0;
bool g_lt = false, g_rt = false;

std::string Buttons(uint16_t b, bool lt, bool rt) {
  static const char* const kNames[16] = {"UP", "DOWN", "LEFT", "RIGHT", "START", "BACK", "LS", "RS",
                                         "LB", "RB",   "?",    "?",     "A",     "B",    "X",  "Y"};
  std::string s;
  for (int i = 0; i < 16; ++i)
    if (b >> i & 1) s += (s.empty() ? "" : "+") + std::string(kNames[i]);
  if (lt) s += s.empty() ? "LT" : "+LT";
  if (rt) s += s.empty() ? "RT" : "+RT";
  return s;
}

// (g_mutex held)
// (each press counts for one line: a page change with none since says so)
std::string After(Clock::time_point now) {
  if (g_press.empty() || now - g_press_at > std::chrono::seconds(15)) return "no press";  // (loads take a while)
  return std::exchange(g_press, std::string());
}

}  // namespace

namespace svr2011 {

void CaeStepsEditorOpen() {
  if (g_open.exchange(true)) return;
  std::lock_guard lock(g_mutex);
  g_page.clear(), g_seen.clear(), g_seen_at = Clock::now();
  REXLOG_INFO("[svr2011] create an entrance: the editor opens (after {})", After(Clock::now()));
}

void CaeStepsLeave(const char* to) {
  if (!g_open.exchange(false)) return;
  std::lock_guard lock(g_mutex);
  REXLOG_INFO("[svr2011] create an entrance: left the editor - {} (after {}; last page \"{}\")", to,
              After(Clock::now()), g_page);
}

void CaeStepsTouchControl(const char* id) {
  if (!id) return;
  std::lock_guard lock(g_mutex);
  g_touch = id, g_touch_at = Clock::now();
}

void CaeStepsTap(float x, float y) {
  char s[48];
  std::snprintf(s, sizeof s, "tap %.2f,%.2f", x, y);
  std::lock_guard lock(g_mutex);
  g_press = s, g_press_at = Clock::now();
  g_touch = s, g_touch_at = g_press_at;  // (its A, if it was on a row, comes next: "A, touch tap ...")
}

void CaeStepsPad(const uint8_t* state) {
  const uint16_t b = uint16_t(state[4] << 8 | state[5]);
  const bool lt = state[6] > 128, rt = state[7] > 128;
  const uint16_t pressed = uint16_t(b & ~g_buttons);
  const bool lt_new = lt && !g_lt, rt_new = rt && !g_rt;
  g_buttons = b, g_lt = lt, g_rt = rt;
  // (presses count before the editor opens too: its opening line names the press that opened it)
  const bool open = g_open.load();
  if (!open && !pressed && !lt_new && !rt_new) return;
  const auto now = Clock::now();
  std::lock_guard lock(g_mutex);
  if (pressed || lt_new || rt_new) {
    const bool touch = !g_touch.empty() && now - g_touch_at < std::chrono::milliseconds(300);
    g_press = Buttons(pressed, lt_new, rt_new) + (touch ? ", touch " + g_touch : ", pad");
    g_press_at = now;
  }
  if (!open || now - g_checked < std::chrono::milliseconds(50)) return;
  g_checked = now;
  const std::string page = MenuTouchPageTitle();
  if (page != g_seen) g_seen = page, g_seen_at = now;
  if (g_seen == g_page || now - g_seen_at < std::chrono::milliseconds(250)) return;
  g_page = g_seen;
  if (g_page.empty())
    REXLOG_INFO("[svr2011] create an entrance: no page drawn - a preview or a load (after {})", After(now));
  else
    REXLOG_INFO("[svr2011] create an entrance: page \"{}\" (after {})", g_page, After(now));
}

}  // namespace svr2011
