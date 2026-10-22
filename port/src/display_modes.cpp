// WWE SmackDown vs. Raw 2011 - the window sizes the screen offers, see display_modes.h.
#include "display_modes.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/logging.h>

namespace svr2011 {

namespace {

void Sort(std::vector<DisplaySize>& v) {
  std::sort(v.begin(), v.end(), [](const DisplaySize& a, const DisplaySize& b) {
    return int64_t(a.w) * a.h != int64_t(b.w) * b.h ? int64_t(a.w) * a.h < int64_t(b.w) * b.h : a.w < b.w;
  });
  v.erase(std::unique(v.begin(), v.end(), [](const DisplaySize& a, const DisplaySize& b) { return a.w == b.w && a.h == b.h; }),
          v.end());
}

}  // namespace

std::vector<DisplaySize> DisplaySizes(void* hwnd) {
  std::vector<DisplaySize> out;
  if (const char* test = std::getenv("SVR2011_TEST_DISPLAY_MODES"); test && *test) {
    const std::string s = test;
    for (size_t at = 0; at < s.size();) {
      const size_t comma = std::min(s.find(',', at), s.size());
      int w = 0, h = 0;
      if (std::sscanf(s.substr(at, comma - at).c_str(), "%dx%d", &w, &h) == 2 && w >= 640 && h >= 360) out.push_back({w, h});
      at = comma + 1;
    }
    Sort(out);
    return out;
  }
#if defined(_WIN32)
  // (Proton / Wine: the screen's modes there are its own idea - the fixed list)
  if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return out;
  HMONITOR monitor = hwnd ? MonitorFromWindow(static_cast<HWND>(hwnd), MONITOR_DEFAULTTONEAREST)
                          : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  if (!monitor || !GetMonitorInfoW(monitor, &mi)) return out;
  DEVMODEW dm = {};
  dm.dmSize = sizeof(dm);
  if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) || !dm.dmPelsWidth) return out;
  const DisplaySize desktop{int(dm.dmPelsWidth), int(dm.dmPelsHeight)};
  for (DWORD i = 0;; ++i) {
    DEVMODEW m = {};
    m.dmSize = sizeof(m);
    if (!EnumDisplaySettingsW(mi.szDevice, i, &m)) break;
    const int w = int(m.dmPelsWidth), h = int(m.dmPelsHeight);
    if (w >= 854 && h >= 480 && w <= desktop.w && h <= desktop.h) out.push_back({w, h});
  }
  out.push_back(desktop);
  Sort(out);
  if (out.size() < 2) out.clear();
#else
  (void)hwnd;
#endif
  return out;
}

}  // namespace svr2011
