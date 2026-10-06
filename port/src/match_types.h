// More exhibition match types: rows added to the PLAY menus for rule
// records the game has but doesn't offer (and new ones). Research and plan:
// docs/MATCH_TYPES_RESEARCH.md.
#pragma once

#include <cstdint>

struct PPCContext;

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class ImGuiDrawer;
}

namespace svr2011 {

void InstallMatchTypes(rex::memory::Memory* memory);

// Each world update (frame_rate.cpp): the Lumberjack match's lumberjacks
// (match_types.cpp: their controller).
void MatchTypesUpdate(PPCContext& ctx, uint8_t* base);

// Menu text of the match rows added at run time (string id), else 0.
uint32_t MatchTypeString(uint32_t id);

// Slobber Knocker (slobber_knocker.cpp): the match set up is one
// (match_types.cpp), a new one starts, and each world update while it runs.
bool SlobberKnockerMatch();
void SlobberKnockerStart();
void SlobberKnockerUpdate(PPCContext& ctx, uint8_t* base);

// The count over a Slobber Knocker match (slobber_knocker.cpp).
void InstallSlobberKnockerOverlay(rex::ui::ImGuiDrawer* drawer);

// THREE STAGES OF HELL (three_stages.cpp): set up (on or not) with a match,
// before each call of the falls judge, its score overlay.
void ThreeStagesSetup(bool on);
bool ThreeStagesMatch();
void ThreeStagesUpdate(uint8_t* base);  // (each world update)
void ThreeStagesBeforeJudge(uint8_t* base);

// Lumberjack (match_types.cpp): before each call of the falls judge - a
// lumberjack's interference isn't a disqualification.
void LumberjackBeforeJudge(uint8_t* base);
void InstallThreeStagesOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace svr2011
