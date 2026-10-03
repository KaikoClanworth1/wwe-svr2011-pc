// WWE SmackDown vs. Raw 2011 - scripted controller (see script_input.h).

#include "script_input.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include <rex/logging.h>

#include "keyboard_typing.h"
#include "touch_controls.h"

namespace svr2011 {

namespace {

constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x53435254);  // 'SCRT'
constexpr auto kPollInterval = std::chrono::milliseconds(100);

uint16_t ParseButtons(const std::string& spec) {
  static const struct {
    const char* name;
    uint16_t bit;
  } kNames[] = {
      {"UP", X_INPUT_GAMEPAD_DPAD_UP},       {"DOWN", X_INPUT_GAMEPAD_DPAD_DOWN},
      {"LEFT", X_INPUT_GAMEPAD_DPAD_LEFT},   {"RIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT},
      {"START", X_INPUT_GAMEPAD_START},      {"BACK", X_INPUT_GAMEPAD_BACK},
      {"LS", X_INPUT_GAMEPAD_LEFT_THUMB},    {"RS", X_INPUT_GAMEPAD_RIGHT_THUMB},
      {"LB", X_INPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {"A", X_INPUT_GAMEPAD_A},              {"B", X_INPUT_GAMEPAD_B},
      {"X", X_INPUT_GAMEPAD_X},              {"Y", X_INPUT_GAMEPAD_Y},
  };
  uint16_t bits = 0;
  std::stringstream ss(spec);
  std::string name;
  while (std::getline(ss, name, '+')) {
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);
    bool found = false;
    for (const auto& n : kNames) {
      if (name == n.name) {
        bits |= n.bit;
        found = true;
      }
    }
    if (!found) {
      REXLOG_WARN("script input: unknown button '{}'", name);
    }
  }
  return bits;
}

}  // namespace

ScriptInputDriver::ScriptInputDriver(std::filesystem::path command_file)
    : InputDriver(nullptr, 0), file_(std::move(command_file)) {
  // (a copy the game restarted itself into - online_overlay.cpp: the commands
  // already there were the last copy's)
  if (std::getenv("SVR2011_RELAUNCHED")) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file_, ec);
    if (!ec) consumed_ = size_t(size);
  }
}

X_STATUS ScriptInputDriver::Setup() {
  REXLOG_INFO("script input: reading commands from {}", file_.string());
  return X_STATUS_SUCCESS;
}

void ScriptInputDriver::EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) {
  rex::input::DeviceInfo info;
  info.id = kDevice;
  info.name = "Script";
  out.push_back(info);
}

void ScriptInputDriver::ParseLine(const std::string& line) {
  std::istringstream in(line);
  std::string verb;
  if (!(in >> verb) || verb[0] == '#') {
    return;
  }
  std::transform(verb.begin(), verb.end(), verb.begin(), ::tolower);
  Step step;
  if (verb == "press" || verb == "hold") {
    std::string buttons;
    in >> buttons;
    step.buttons = ParseButtons(buttons);
    step.ms = 120;
    in >> step.ms;
    queue_.push_back(step);
    queue_.push_back(Step{.ms = 120});  // release, so repeated presses register
  } else if (verb == "stick") {
    std::string side;
    int x = 0, y = 0;
    in >> side >> x >> y >> step.ms;
    auto clamp = [](int v) { return static_cast<int16_t>(std::clamp(v, -32768, 32767)); };
    (side == "R" || side == "r" ? step.rx : step.lx) = clamp(x);
    (side == "R" || side == "r" ? step.ry : step.ly) = clamp(y);
    // Optional: buttons held meanwhile ("-" none), the right trigger and the left
    // trigger ("stick L 0 32000 2000 LB 255 0").
    std::string buttons;
    if (in >> buttons && buttons != "-") step.buttons = ParseButtons(buttons);
    int rt = 0;
    if (in >> rt) step.right_trigger = static_cast<uint8_t>(std::clamp(rt, 0, 255));
    int lt = 0;  // and the left trigger
    if (in >> lt) step.left_trigger = static_cast<uint8_t>(std::clamp(lt, 0, 255));
    queue_.push_back(step);
  } else if (verb == "trigger") {
    std::string side;
    int v = 0;
    in >> side >> v >> step.ms;
    (side == "R" || side == "r" ? step.right_trigger : step.left_trigger) =
        static_cast<uint8_t>(std::clamp(v, 0, 255));
    queue_.push_back(step);
  } else if (verb == "wait") {
    in >> step.ms;
    queue_.push_back(step);
  } else if (verb == "type") {
    std::getline(in >> std::ws, step.text);
    step.ms = 100;
    queue_.push_back(step);
  } else if (verb == "touch" || verb == "drag") {
    step.touch = true;
    in >> step.x0 >> step.y0;
    step.x1 = step.x0, step.y1 = step.y0;
    if (verb == "drag") in >> step.x1 >> step.y1;
    step.ms = 150;
    in >> step.ms;
    queue_.push_back(step);
    queue_.push_back(Step{.ms = 120});  // (lifted, then a pause)
  } else if (verb == "key") {
    static const struct {
      const char* name;
      uint16_t vk;
    } kKeys[] = {{"BACK", 0x08}, {"ENTER", 0x0D}, {"ESC", 0x1B}, {"LEFT", 0x25},
                 {"UP", 0x26},   {"RIGHT", 0x27}, {"DOWN", 0x28}};
    std::string name;
    in >> name;
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);
    for (const auto& k : kKeys) {
      if (name == k.name) step.key = k.vk;
    }
    if (!step.key) {
      REXLOG_WARN("script input: unknown key '{}'", line);
      return;
    }
    step.ms = 100;
    queue_.push_back(step);
  } else {
    REXLOG_WARN("script input: unknown command '{}'", line);
    return;
  }
  REXLOG_INFO("script input: {}", line);
}

void ScriptInputDriver::PollFile() {
  const auto now = Clock::now();
  if (now - last_poll_ < kPollInterval) {
    return;
  }
  last_poll_ = now;
  std::ifstream f(file_, std::ios::binary);
  if (!f) {
    return;
  }
  f.seekg(0, std::ios::end);
  const size_t size = static_cast<size_t>(f.tellg());
  if (size < consumed_) {
    consumed_ = 0;  // file was truncated: start over
  }
  if (size == consumed_) {
    return;
  }
  f.seekg(static_cast<std::streamoff>(consumed_));
  std::string text(size - consumed_, '\0');
  f.read(text.data(), static_cast<std::streamsize>(text.size()));
  // Only whole lines; a partly written last line waits for the next poll.
  const size_t end = text.rfind('\n');
  if (end == std::string::npos) {
    return;
  }
  consumed_ += end + 1;
  std::istringstream lines(text.substr(0, end));
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    ParseLine(line);
  }
}

ScriptInputDriver::Step ScriptInputDriver::CurrentStep() {
  std::lock_guard lock(mutex_);
  PollFile();
  const auto now = Clock::now();
  if (!running_ || now >= step_end_) {
    constexpr uint32_t kFinger = 7;
    if (running_ && current_.touch) TouchInject(kFinger, 2, current_.x1, current_.y1);
    running_ = !queue_.empty();
    if (running_) {
      current_ = queue_.front();
      queue_.pop_front();
      step_end_ = now + std::chrono::milliseconds(current_.ms);
      if (!current_.text.empty()) TypeText(current_.text);
      if (current_.key) TypeKey(current_.key);
      if (current_.touch) {
        TouchInject(kFinger, 0, current_.x0, current_.y0);
        if (current_.x1 != current_.x0 || current_.y1 != current_.y0) {
          TouchInject(kFinger, 1, current_.x1, current_.y1);
        }
      }
    } else {
      current_ = Step{};
    }
  }
  return current_;
}

X_RESULT ScriptInputDriver::GetDeviceState(rex::input::DeviceId id, X_INPUT_STATE* out_state) {
  if (id != kDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  const Step s = CurrentStep();
  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    out_state->packet_number = ++packet_;
    out_state->gamepad.buttons = s.buttons;
    out_state->gamepad.left_trigger = s.left_trigger;
    out_state->gamepad.right_trigger = s.right_trigger;
    out_state->gamepad.thumb_lx = s.lx;
    out_state->gamepad.thumb_ly = s.ly;
    out_state->gamepad.thumb_rx = s.rx;
    out_state->gamepad.thumb_ry = s.ry;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptInputDriver::GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                                  X_INPUT_CAPABILITIES* out_caps) {
  if (id != kDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptInputDriver::SetDeviceVibration(rex::input::DeviceId id, X_INPUT_VIBRATION*) {
  return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT ScriptInputDriver::GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                                               X_INPUT_KEYSTROKE*) {
  return id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

}  // namespace svr2011
