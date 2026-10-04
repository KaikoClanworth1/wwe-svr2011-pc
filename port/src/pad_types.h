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

// The PlayStation (non-XInput) controllers through SDL, next to XInput.
std::unique_ptr<rex::input::InputDriver> CreatePlayStationDriver();

}  // namespace svr2011
