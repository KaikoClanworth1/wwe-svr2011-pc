// WWE SmackDown vs. Raw 2011 - online PORT RULES: the port's match types in
// online Player Matches (online_rules.cpp). The host picks them on CREATE
// SESSION in a row drawn in the game's menu style; the choice travels in the
// session (property 0x1000F001 = mode << 16 | set: p2p.h SetPortRules /
// PortRules) and both sides switch the same mode on before the match is set
// up. Ranked sessions never carry it.
#pragma once

#include <cstdint>

namespace rex::ui {
class ImGuiDrawer;
}

namespace svr2011 {

// The row and the lobby panel (svr2011_app.cpp).
void InstallOnlineRulesOverlay(rex::ui::ImGuiDrawer* drawer);

// Player 1's buttons each controller reading (frame_rate.cpp): LB / RB cycle the row.
void OnlineRulesPad(uint16_t buttons);

// The match set up (match_types.cpp sub_827374A0): this session's port rules
// for `rule` - on: play `rule` (maybe another: Weapons Everywhere on a normal
// match is its Extreme Rules one) with these modes.
struct OnlineRulesSetup {
  bool on = false;
  uint32_t rule = 0;
  bool weapons = false, stages = false;
  int set = 0;
};
OnlineRulesSetup OnlineRulesFor(uint32_t rule);

}  // namespace svr2011
