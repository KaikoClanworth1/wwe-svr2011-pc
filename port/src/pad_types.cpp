// WWE SmackDown vs. Raw 2011 - which controller each player uses (see
// pad_types.h).

#include "pad_types.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include <SDL3/SDL_hints.h>

#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>

namespace svr2011 {

namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using namespace rex::input;  // (X_INPUT_* types)

std::array<std::atomic<PadType>, kMaxPlayers> g_types{};
std::atomic<uint32_t> g_version{0};

constexpr const char* kKeyboardName = "Keyboard and Mouse";  // (the SDK's MnK driver)

// The USB vendor in an SDL GUID (bytes 4-5, little-endian: hex chars 8-11).
uint32_t GuidVendor(const std::string& guid) {
  if (guid.size() < 12) return 0;
  auto hex = [&](size_t i) { return std::stoul(guid.substr(i, 2), nullptr, 16); };
  try {
    return uint32_t(hex(8) | hex(10) << 8);
  } catch (...) {
    return 0;
  }
}

PadType TypeOf(const DeviceInfo& d) {
  if (d.name == kKeyboardName) return PadType::kKeyboard;
  constexpr uint32_t kSony = 0x054C;
  if (GuidVendor(d.guid) == kSony) return PadType::kPlayStation;
  std::string name = d.name;
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  for (const char* ps : {"ps3", "ps4", "ps5", "dualshock", "dualsense", "playstation", "sony"})
    if (name.find(ps) != std::string::npos) return PadType::kPlayStation;
  return PadType::kXbox;  // (XInput pads, and others with an Xbox-style layout)
}

const char* TypeName(PadType t) {
  switch (t) {
    case PadType::kXbox: return "Xbox";
    case PadType::kPlayStation: return "PlayStation";
    case PadType::kKeyboard: return "keyboard";
    default: return "none";
  }
}

// The SDL driver for the PlayStation controllers next to XInput: SDL sees
// only HIDAPI devices other than Xbox ones (no XInput, raw input, DirectInput,
// WGI or GameInput), so no pad is seen twice.
class PlayStationDriver final : public rex::input::InputDriver {
 public:
  PlayStationDriver() : InputDriver(nullptr, 0), sdl_(nullptr, 0) {}

  X_STATUS Setup() override {
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");  // (a DualShock 3 with the DsHidMini driver)
    return sdl_.Setup();
  }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override { sdl_.EnumerateDevices(out); }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* s) override { return sdl_.GetDeviceState(id, s); }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags, X_INPUT_CAPABILITIES* c) override {
    return sdl_.GetDeviceCapabilities(id, flags, c);
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* v) override {
    return sdl_.SetDeviceVibration(id, v);
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags, X_INPUT_KEYSTROKE* k) override {
    return sdl_.GetDeviceKeystroke(id, flags, k);
  }

 private:
  rex::input::sdl::SDLInputDriver sdl_;
};

}  // namespace

PadType PlayerPadType(uint32_t user_index) {
  return user_index < kMaxPlayers ? g_types[user_index].load(std::memory_order_relaxed) : PadType::kNone;
}

uint32_t PadTypesInUse() {
  uint32_t mask = 0;
  for (const auto& t : g_types) mask |= 1u << uint32_t(t.load(std::memory_order_relaxed));
  return mask & ~(1u << uint32_t(PadType::kNone));
}

uint32_t PadTypesVersion() { return g_version.load(std::memory_order_relaxed); }

void PlayerSlots::OnDevicesChanged(const std::vector<DeviceInfo>& devices) {
  for (auto& u : users_) u.clear();
  std::array<PadType, kMaxPlayers> types{};
  // Controllers by connection order (ordinal N -> player N, as SlotAssignment:
  // unplugging pad one doesn't make pad two player one).
  const DeviceInfo* keyboard = nullptr;
  std::vector<const DeviceInfo*> shared;  // (the touch controller: player 1's)
  uint32_t next_free = 0;
  for (const DeviceInfo& d : devices) {
    if (d.name == kKeyboardName) {
      keyboard = &d;
    } else if (d.synthetic) {
      shared.push_back(&d);
    } else if (d.ordinal < kMaxPlayers) {
      users_[d.ordinal].push_back(d.id);
      if (types[d.ordinal] == PadType::kNone) types[d.ordinal] = TypeOf(d);
      next_free = std::max(next_free, d.ordinal + 1);
    }
  }
  // The keyboard: the player after the controllers (player 1 with none).
  if (keyboard) {
    const uint32_t user = std::min(next_free, kMaxPlayers - 1);
    users_[user].push_back(keyboard->id);
    if (types[user] == PadType::kNone) types[user] = PadType::kKeyboard;
  }
  for (const DeviceInfo* d : shared) users_[0].push_back(d->id);
  // Test aid: SVR2011_TEST_PAD_TYPES=xbox,ps,kb,none - the players' types as
  // if those controllers were connected (button pictures without the pads).
  if (const char* v = std::getenv("SVR2011_TEST_PAD_TYPES"); v && *v) {
    std::string list = v;
    for (uint32_t i = 0; i < kMaxPlayers && !list.empty(); ++i) {
      const std::string t = list.substr(0, list.find(','));
      list = list.find(',') == std::string::npos ? "" : list.substr(list.find(',') + 1);
      types[i] = t == "ps" ? PadType::kPlayStation : t == "kb" ? PadType::kKeyboard
               : t == "xbox" ? PadType::kXbox : PadType::kNone;
    }
  }

  std::string log;
  bool changed = false;
  for (uint32_t i = 0; i < kMaxPlayers; ++i) {
    changed |= g_types[i].exchange(types[i]) != types[i];
    if (types[i] != PadType::kNone) log += fmt::format("{}P{} {}", log.empty() ? "" : ", ", i + 1, TypeName(types[i]));
  }
  if (changed) {
    ++g_version;
    std::string names;
    for (const DeviceInfo& d : devices)
      if (!d.synthetic || d.name == kKeyboardName) names += (names.empty() ? "" : "; ") + d.name;
    REXLOG_INFO("[svr2011] players: {} ({})", log.empty() ? "none" : log, names);
  }
}

void PlayerSlots::DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const {
  out.clear();
  if (user_index < kMaxPlayers) out = users_[user_index];
}

std::unique_ptr<rex::input::InputDriver> CreatePlayStationDriver() { return std::make_unique<PlayStationDriver>(); }

}  // namespace svr2011
