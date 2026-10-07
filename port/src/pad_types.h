// WWE SmackDown vs. Raw 2011 - which controller each player uses.
//
// Players and devices (PlayerSlots, the input system's assignment):
//  - controllers take the players in the order they were connected (as the
//    SDK's SlotAssignment): the first one is player 1;
//  - the keyboard (mnk_mode) is a player of its own after every controller -
//    player 2 with one controller, player 1 with none;
//  - the on-screen touch controller stays with player 1.
// Each player's controller type (PlayerPadType) chooses the button pictures
// the game shows them (Xbox, PlayStation or keyboard).
//
// On Windows the XInput backend also gets an SDL driver that sees only the
// controllers XInput can't (PlayStation 3 / 4 / 5 and others, through HIDAPI),
// so a PlayStation pad works next to an Xbox one.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input_driver.h>

namespace svr2011 {

enum class PadType : uint8_t { kNone, kXbox, kPlayStation, kKeyboard };

constexpr uint32_t kMaxPlayers = 4;

// Player user_index's controller (kNone: no device).
PadType PlayerPadType(uint32_t user_index);
// The types the connected players use (bit 1 << PadType).
uint32_t PadTypesInUse();
// Bumped each time the players' devices change.
uint32_t PadTypesVersion();

class PlayerSlots final : public rex::input::DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override;
  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override;

 private:
  std::array<std::vector<rex::input::DeviceId>, kMaxPlayers> users_;
};

// The player who pressed START at the press-START screen becomes player 1
// (user 0, the signed-in profile: its saves and achievements). On PC whoever
// presses START plays: their devices and player 1's swap places (the others
// stay players 2-8). The game's choice (sub_8258B5F8) is hooked in
// pad_types.cpp. Test aid: SVR2011_TEST_SCRIPT_PLAYER=<n> - the scripted
// controller is player n (a second pad pressing START, without one).
void MakeLeadPlayer(uint32_t user_index);

// The PlayStation (non-XInput) controllers through SDL, next to XInput.
std::unique_ptr<rex::input::InputDriver> CreatePlayStationDriver();

}  // namespace svr2011
