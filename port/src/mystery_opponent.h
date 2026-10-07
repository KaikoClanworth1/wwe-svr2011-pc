// WWE SmackDown vs. Raw 2011 - MYSTERY OPPONENT: ONE ON ONE -> NORMAL MATCH
// -> MYSTERY OPPONENT (a row match_types.cpp adds). The player picks only
// their own superstar; the opponent is a random one, hidden until their
// entrance (always played) shows who it is.

#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class ImGuiDrawer;
}

namespace svr2011 {

// The row's label and description (match_types.cpp MatchTypeString).
constexpr uint32_t kMysteryLabel = 0x0FA0B108, kMysteryText = 0x0FA0B109;
uint32_t MysteryOpponentString(uint32_t id, rex::memory::Memory* memory);  // (0: not one of them)

// A match is set up (sub_827374A0, before the game's own): on for a MYSTERY
// OPPONENT one (the 1 on 1 rule 0x00, not in a story).
void MysteryOpponentSetup(uint8_t* base, bool on);
bool MysteryOpponentMatch();

// The match's people are built (sub_828BBEF0, before the game's own): the
// opponent's slot filled with the hidden pick.
void MysteryOpponentFill(uint8_t* base, uint32_t match);

// The live rules are built (sub_828C48E8, after the game's own): entrances on.
void MysteryOpponentLive(uint8_t* base, uint32_t live);

// The character select screen draws a panel (sub_82467C18, pad_icons.cpp).
void MysterySelectFrame(uint8_t* base);

// Every sound event the game posts by name (jukebox.cpp's JukeboxEvent): the
// entrances starting take the "?" away.
void MysteryOpponentEvent(const char* e);

// The game sees an idle controller while a MYSTERY OPPONENT match's
// entrances run (graphics_page.cpp's input hold): nothing skips the reveal.
bool MysteryOpponentHoldsInput();

// The "?" over the VS screen until the entrances start.
void InstallMysteryOpponentOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace svr2011
