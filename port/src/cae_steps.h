// WWE SmackDown vs. Raw 2011 - CREATE AN ENTRANCE's editor steps in the log (report 9f6377).
//
// A report said adding pyro "goes back to the previous menu" and its log couldn't tell a player's back-out from the
// game's: nothing was logged between the editor's start and the select screen. Now each page change in the editor -
// SELECT TEMPLATE, a list (MOTION, PYRO, LIGHTING...), the pyro timeline (TRIGGER PYRO), nothing drawn (a preview, a
// load), leaving it - logs one info line with the press or touch before it:
//   [svr2011] create an entrance: page "PYRO list" (after A, pad)
// PC and Android alike. The page is the title the game draws (menu_touch.h MenuTouchPageTitle), in its language.
#pragma once

#include <cstdint>

namespace svr2011 {

// The editor loads a superstar's entrance (sub_8287E960): the steps are logged from here on.
void CaeStepsEditorOpen();
// Leaving the editor - its last line: its entrance scene ended (ring_rules.cpp: the entrance state object removed),
// a character select opened (managers.cpp), a main-menu entry chosen (menu_hooks.cpp).
void CaeStepsLeave(const char* to);
// Player 1's pad as the game read it (frame_rate.cpp, once a read; the touch controller's presses are in it too):
// the presses, and the page check.
void CaeStepsPad(const uint8_t* state);
// A touch: a control of the touch controller ("a", "start", "dpad"...) or a tap on the game's picture (x, y fractions).
void CaeStepsTouchControl(const char* id);
void CaeStepsTap(float x, float y);

}  // namespace svr2011
