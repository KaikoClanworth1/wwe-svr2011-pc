// WWE SmackDown vs. Raw 2011 - scripted controller for automated testing.
//
// With SVR2011_INPUT_FILE=<path> set, the game's only controller is a virtual
// pad driven by commands appended to that file (one per line), so a test can
// play the game while it runs in the background:
//
//   press <buttons> [ms]        tap buttons (default 120 ms), then release
//   hold <buttons> <ms>         same, explicit duration
//   stick <L|R> <x> <y> <ms>    hold a stick at x,y (-32768..32767)
//   trigger <L|R> <0-255> <ms>  hold a trigger
//   wait <ms>                   idle
//   type <text>                 type on the PC keyboard (keyboard_typing.h)
//   key <BACK|ENTER|ESC|LEFT|RIGHT|UP|DOWN>   press a PC keyboard key
//   touch <x> <y> <ms>          a finger on the on-screen controller at x, y
//                               (fractions of the window) for ms
//   drag <x0> <y0> <x1> <y1> <ms>   a finger down at x0, y0, moved to x1, y1
//
// <buttons> is one or more of A B X Y START BACK LB RB LS RS UP DOWN LEFT RIGHT
// joined with '+'. Lines starting with '#' are ignored.

#pragma once

#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>

#include <rex/input/input_driver.h>

namespace svr2011 {

using rex::X_RESULT;
using rex::X_STATUS;
using namespace rex::input;  // X_INPUT_* types and button bits

class ScriptInputDriver final : public rex::input::InputDriver {
 public:
  explicit ScriptInputDriver(std::filesystem::path command_file);

  X_STATUS Setup() override;
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  X_RESULT GetDeviceState(rex::input::DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(rex::input::DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

 private:
  using Clock = std::chrono::steady_clock;

  struct Step {
    uint16_t buttons = 0;
    uint8_t left_trigger = 0, right_trigger = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    int ms = 0;
    std::string text;  // typed when the step starts
    uint16_t key = 0;  // PC keyboard key pressed when the step starts
    bool touch = false;  // a finger (touch_controls.h) from x0, y0 to x1, y1
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  };

  void PollFile();
  void ParseLine(const std::string& line);
  Step CurrentStep();

  std::filesystem::path file_;
  size_t consumed_ = 0;  // bytes of the file already parsed
  Clock::time_point last_poll_{};
  std::mutex mutex_;
  std::deque<Step> queue_;
  bool running_ = false;
  Step current_{};
  Clock::time_point step_end_{};
  uint32_t packet_ = 0;
};

}  // namespace svr2011
