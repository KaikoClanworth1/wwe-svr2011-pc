// WWE SmackDown vs. Raw 2011 - MY WWE -> OPTIONS -> WEAPONS: which weapons an
// Extreme Rules match has (as SvR 2008's weapon choice), how many of each.
//
// A match's weapons come from a list the game builds as it loads
// (sub_82301148: {count, ids[72]} - each id one weapon in the match; those
// not lying about at the bell wait under the ring): a set every match has
// (chairs, the steps, the announce tables, ...) and, with the Extreme Rules
// option on (live settings +56: EXTREME RULES, WEAPONS EVERYWHERE, the
// structures' WEAPONS INSIDE), nine more. The page replaces those nine with
// the player's choice (a weapon 0-3 times; cvar extreme_weapons, "" = the
// game's own nine). Weapon mods (weapon_mods.h, pool=extreme) are rows after
// the game's weapons, in the same room. Online the host's choice goes with the
// session (property 0x1000F002: the game's weapons; 0x1000F003: its mod
// weapons' hash - the mods themselves by key in the lobby's tables, those every
// player has: online_mods.h) and every game in it uses that; Ranked matches keep
// the game's.
// Controller (D-pad / stick: choose, A / right: one more, left: one less, Y:
// the game's set, B: back), keyboard (arrows, Enter, Y, Esc) or touch.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <rex/ppc.h>

struct ImFont;

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class ImGuiDrawer;
}
namespace rex::input {
class InputSystem;
}

namespace svr2011 {

void InstallWeaponsPage(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input);
void SetWeaponsPageFonts(ImFont* menu, ImFont* title);
void OpenWeaponsPage();
bool WeaponsPageHoldsInput();

// The weapon list being built (sub_82301148's, guest address `list`) with the
// Extreme Rules option on: its Extreme Rules weapons as chosen (this game's
// choice offline; the host's online, the game's in Ranked): the game's nine entries keep their places, their models
// change. Logged.
void ChooseExtremeWeapons(uint8_t* base, uint32_t list);

// WEAPONS EVERYWHERE's weapon i lying about at the bell (model: the game's): its table, ladder, trash can and guitar
// become the chosen weapons.
int ExtremeWeaponsPlacedModel(int i, int model);

// The match's weapons being made (sub_8227B938: begin before it, end after): the game's Extreme Rules nine become the
// chosen ones as it makes them, the rest are made at the end.
void ExtremeWeaponsMaking(uint8_t* base, bool begin, PPCContext& ctx);
// ... and put in place (after sub_8227D130): the stock under the ring holds the chosen ones.
void ExtremeWeaponsPlaced(uint8_t* base, rex::memory::Memory* memory);

// This game's weapon mods in its choice: {key, count} (the host's for an online lobby: online_mods.cpp).
std::vector<std::pair<std::string, int>> HostModWeaponPicks();

}  // namespace svr2011
