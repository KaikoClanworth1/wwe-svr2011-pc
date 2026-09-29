// WWE SmackDown vs. Raw 2011 - PC keyboard typing (see keyboard_typing.h).

#include "keyboard_typing.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

#include "generated/default/svr2011_init.h"

namespace {

using rex::ui::VirtualKey;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kFlagKeyboard = 0x02;    // XINPUT_FLAG_KEYBOARD
constexpr uint16_t kKeyDown = 0x0001;       // XINPUT_KEYSTROKE_KEYDOWN
constexpr uint16_t kKeyUp = 0x0002;         // XINPUT_KEYSTROKE_KEYUP
constexpr uint16_t kVkShift = 0x10;
constexpr uint32_t kErrorEmpty = 0x10D2;    // ERROR_EMPTY: no keystroke waiting
constexpr uint32_t kErrorNotConnected = 0x48F;  // ERROR_DEVICE_NOT_CONNECTED
constexpr uint8_t kDevTypeKeyboard = 0x02;  // XINPUT_DEVTYPE_USB_KEYBOARD
// The game polls every frame while its keyboard is open; a longer gap means
// it closed and the window's keys go back to the controller emulation.
constexpr auto kOpenTimeout = std::chrono::milliseconds(300);

struct Stroke {
  uint16_t vk = 0;
  uint16_t unicode = 0;
  uint16_t flags = kKeyDown;
};

std::mutex g_mutex;
std::deque<Stroke> g_strokes;
std::atomic<int64_t> g_last_poll{0};  // Clock ticks of the game's last keyboard poll
rex::ui::Window* g_window = nullptr;
bool g_text_input = false;  // UI thread: we turned the window's text input on

int64_t Now() { return Clock::now().time_since_epoch().count(); }

bool KeyboardOpen() {
  const int64_t last = g_last_poll.load();
  return last && Now() - last < std::chrono::duration_cast<Clock::duration>(kOpenTimeout).count();
}

void Push(Stroke s) {
  std::lock_guard lock(g_mutex);
  if (g_strokes.size() < 256) g_strokes.push_back(s);
}

// A typed character. The game sets a letter's case from its own Shift state
// (sub_8274ADC0), which an Xbox keyboard drives with Shift key events, so a
// letter goes with Shift down (capital) or up (small).
void PushChar(uint16_t cp) {
  const bool upper = cp >= 'A' && cp <= 'Z', lower = cp >= 'a' && cp <= 'z';
  if (upper) Push({kVkShift, 0, kKeyDown});
  if (lower) Push({kVkShift, 0, kKeyUp});
  Push({0, cp});
  if (upper) Push({kVkShift, 0, kKeyUp});
}

// Text input (SDL's, for OnKeyChar) is on only while the game's keyboard is.
void SetTextInput(bool on) {
  if (on == g_text_input || !g_window) return;
  g_text_input = on;
  g_window->SetTextInputActive(on);
}

// The keys the game handles itself (sub_8274B0A8); their character codes as
// an Xbox keyboard reports them.
bool GameKey(VirtualKey vk, uint16_t* unicode) {
  switch (vk) {
    case VirtualKey::kBack: *unicode = 8; return true;
    case VirtualKey::kReturn: *unicode = 13; return true;
    case VirtualKey::kEscape: *unicode = 27; return true;
    case VirtualKey::kLeft:
    case VirtualKey::kRight:
    case VirtualKey::kUp:
    case VirtualKey::kDown: *unicode = 0; return true;
    default: return false;
  }
}

// Keys that type a character (their text arrives as OnKeyChar).
bool CharacterKey(VirtualKey vk) {
  const auto v = uint16_t(vk);
  return v == 0x20 || (v >= 0x30 && v <= 0x39) || (v >= 0x41 && v <= 0x5A) ||
         (v >= 0x60 && v <= 0x6F) || (v >= 0xBA && v <= 0xC0) || (v >= 0xDB && v <= 0xDF) ||
         v == 0xE2;
}

// Ahead of the controller emulation (keyboard -> pad) and the overlays: while
// the game's keyboard is open, typing goes to it and nowhere else.
class TypingListener final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!KeyboardOpen()) {
      SetTextInput(false);
      return;
    }
    SetTextInput(true);
    if (e.is_ctrl_pressed() || e.is_alt_pressed() || e.is_super_pressed()) return;
    uint16_t unicode = 0;
    if (GameKey(e.virtual_key(), &unicode)) {
      Push({uint16_t(e.virtual_key()), unicode});
      e.set_handled(true);
    } else if (CharacterKey(e.virtual_key())) {
      e.set_handled(true);
    }
  }

  void OnKeyChar(rex::ui::KeyEvent& e) override {
    if (!KeyboardOpen()) return;
    const auto cp = uint32_t(e.virtual_key());
    if (cp < 0x20 || cp == 0x7F || cp > 0xFFFF) return;
    PushChar(uint16_t(cp));
    e.set_handled(true);
  }
};

}  // namespace

namespace svr2011 {

void InstallKeyboardTyping(rex::ui::Window* window) {
  if (!window) return;
  g_window = window;
  static TypingListener listener;
  window->app_context().CallInUIThread([window] { window->AddInputListener(&listener, 1000); });
}

void TypeText(const std::string& utf8) {
  for (size_t i = 0; i < utf8.size();) {
    const uint8_t c = uint8_t(utf8[i]);
    uint32_t cp = c;
    size_t n = 1;
    if (c >= 0xF0) cp = c & 0x07, n = 4;
    else if (c >= 0xE0) cp = c & 0x0F, n = 3;
    else if (c >= 0xC0) cp = c & 0x1F, n = 2;
    for (size_t k = 1; k < n && i + k < utf8.size(); ++k) cp = cp << 6 | (uint8_t(utf8[i + k]) & 0x3F);
    i += n;
    if (cp >= 0x20 && cp <= 0xFFFF) PushChar(uint16_t(cp));
  }
}

void TypeKey(uint16_t vk) {
  uint16_t unicode = 0;
  GameKey(VirtualKey(vk), &unicode);
  Push({vk, unicode});
}

}  // namespace svr2011

// The game's XInputGetKeystroke(user, flags, keystroke) wrapper. Keyboard
// queries (only its on-screen keyboard makes them) get the window's keys.
REX_EXTERN(__imp__sub_82905130);
REX_HOOK_RAW(sub_82905130) {
  if ((ctx.r4.u32 & 0xFF) != kFlagKeyboard) {
    __imp__sub_82905130(ctx, base);
    return;
  }
  if (!KeyboardOpen() && g_window) {  // just opened: characters on
    g_window->app_context().CallInUIThread([] { SetTextInput(true); });
  }
  g_last_poll = Now();
  Stroke s;
  {
    std::lock_guard lock(g_mutex);
    if (g_strokes.empty()) {
      ctx.r3.u64 = kErrorEmpty;
      return;
    }
    s = g_strokes.front();
    g_strokes.pop_front();
  }
  uint8_t* k = base + ctx.r5.u32;  // X_INPUT_KEYSTROKE, big-endian
  k[0] = uint8_t(s.vk >> 8), k[1] = uint8_t(s.vk);
  k[2] = uint8_t(s.unicode >> 8), k[3] = uint8_t(s.unicode);
  k[4] = uint8_t(s.flags >> 8), k[5] = uint8_t(s.flags);
  k[6] = ctx.r3.u32 == 0xFF ? 0 : uint8_t(ctx.r3.u32);
  k[7] = 0;
  ctx.r3.u64 = 0;
}

// XInputGetCapabilities(user, flags, caps): its keyboard finds the player's
// keyboard by asking each user for one (sub_8274AD48); user 0 has it.
REX_EXTERN(__imp__sub_82905050);
REX_HOOK_RAW(sub_82905050) {
  if ((ctx.r4.u32 & 0xFF) != kFlagKeyboard) {
    __imp__sub_82905050(ctx, base);
    return;
  }
  if (ctx.r3.u32 != 0 && ctx.r3.u32 != 0xFF) {
    ctx.r3.u64 = kErrorNotConnected;
    return;
  }
  uint8_t* caps = base + ctx.r5.u32;  // X_INPUT_CAPABILITIES
  std::memset(caps, 0, 20);
  caps[0] = kDevTypeKeyboard;
  ctx.r3.u64 = 0;
}
