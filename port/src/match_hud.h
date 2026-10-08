// WWE SmackDown vs. Raw 2011 - the port's match HUD (Three Stages of Hell's
// score, the Slobber Knocker count): when it shows, and its look.
//
// It shows only while the match itself runs: in a match (frame_rate.h
// InMatch) and its clock (0x82E3CD0C, the match's frame count) going up -
// which it doesn't in the menus, select, entrances, pauses, replays and the
// end screens. Its look follows the game's own menus and versus screen: a
// slanted black panel with a white rim and a red edge, bold italic white
// text with a shadow.
#pragma once

#include <cstdint>
#include <string>

struct ImFont;
struct ImDrawList;

namespace svr2011 {

// Each world update (match_types.cpp MatchTypesUpdate): the clock.
void MatchHudTick(uint8_t* base);

// True while the match runs (see above).
bool MatchHudVisible();

// The fonts (OnConfigureFonts; null: ImGui's).
void SetMatchHudFonts(ImFont* menu, ImFont* title);

// A panel at the top centre of the game's 16:9 area: `left` (gold) and
// `right` (white), e.g. "THREE STAGES OF HELL", "1 - 0   FALL 2: FALLS COUNT
// ANYWHERE" (an ImGui foreground draw; the display size in pixels).
void DrawHudPanel(float display_w, float display_h, const std::string& left, const std::string& right);

// A banner across the middle (a stage change): `big` over `sub`; `t` 0..1
// of its time (it slides in, holds, fades).
void DrawHudBanner(float display_w, float display_h, const std::string& big, const std::string& sub, float t);

}  // namespace svr2011
