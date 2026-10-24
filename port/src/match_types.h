// More exhibition match types: rows added to the PLAY menus for rule
// records the game has but doesn't offer (and new ones). Research and plan:
// docs/MATCH_TYPES_RESEARCH.md.
#pragma once

#include <cstdint>
#include <map>
#include <string>

struct PPCContext;

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class ImGuiDrawer;
}

namespace svr2011 {

void InstallMatchTypes(rex::memory::Memory* memory);

// The last match placed (its rule and arena; ~0 before one): the online live report.
void LastMatch(uint32_t* rule, uint32_t* arena);
// How many times a match's people were placed (sub_822AD738, as a match loads) - it changes when a new match starts.
uint32_t MatchPlacements();
// A backstage match's rule (its rooms are bg78 - where backstage mods play).
bool BackstageRule(uint32_t rule);

// Match type mods: each match type the port adds is on unless
// <game>\Mods\MatchTypes\<id>\disabled exists (the launcher's Mods tab; read
// once, at the first ask). Ids: falls_count_anywhere, championship_scramble,
// royal_rumble_15_25, lumberjack, free_roaming_backstage,
// backstage_more_people, weapons_everywhere, slobber_knocker,
// three_stages_of_hell, elimination, mystery_opponent, hiac_tables_ladders, hiac_roof_start,
// gauntlet_eliminator, cage_weapons, inferno_weapons, online_first_blood,
// three_stages_tables_ladders.
// MatchTypeSwitchOn: the player's own switch - the menus (which rows the
// match menus and the online MATCH TYPE lists offer). MatchTypeOn: what a
// match plays - the switch offline; in an online match session (p2p.h
// P2PMatchLobby: hosted or joined) every type is on, so two games in one
// lockstep match play the host's pick the same whatever their switches (the
// switches aren't in the online mod set: online_files.h).
bool MatchTypeSwitchOn(const char* id);
// Every restored type's switch (id -> on); all off with Start Without Mods.
const std::map<std::string, bool>& MatchTypeSwitches();
bool MatchTypeOn(const char* id);

// Each world update (frame_rate.cpp): the Lumberjack match's lumberjacks
// (match_types.cpp: their controller).
void MatchTypesUpdate(PPCContext& ctx, uint8_t* base);

// Menu text of the match rows added at run time (string id), else 0.
uint32_t MatchTypeString(uint32_t id);

// A fighter's input object (its control +36, seen once a frame; 0 if none):
// +68 buttons pressed this frame, +76 held (X = 0x10).
uint32_t FighterInput(uint32_t fighter);

// Each world update (frame_rate.cpp): an HCTP backstage room's gimmicks
// (hctp_room.cpp; only in a match from a row with hctp=).
void HctpRoomUpdate(PPCContext& ctx, uint8_t* base);

// The HCTP backstage as one stage (hctp_room.cpp): after each CFRS room-visibility
// update (match_types.cpp), only the current room's models and objects are shown.
void HctpWorldVisibility(PPCContext& ctx, uint8_t* base);
// The door prompt over the match (a player at a door: his Y button, "GO TO <room>").
void InstallHctpOverlay(rex::ui::ImGuiDrawer* drawer);
// (hctp_room.cpp) A world frame's start, before the game's update: wrestlers put through a door held there.
void HctpBeforeFrame(uint8_t* base);
// (match_types.cpp) The backstage fight box set up for the match: a new centre and
// half sizes (the fighters went through a door into another room); the match
// camera snapped onto them (not panning across).
void SetFightBox(uint8_t* base, float cx, float cz, float hx, float hz);
void SnapMatchCamera();

// Slobber Knocker (slobber_knocker.cpp): the match set up is one
// (match_types.cpp), a new one starts, and each world update while it runs.
bool SlobberKnockerMatch();
void SlobberKnockerStart();
void SlobberKnockerUpdate(PPCContext& ctx, uint8_t* base);

// The count over a Slobber Knocker match (slobber_knocker.cpp).
void InstallSlobberKnockerOverlay(rex::ui::ImGuiDrawer* drawer);

// GAUNTLET ELIMINATOR (gauntlet_eliminator.cpp): the match set up is one
// (match_types.cpp), a new one starts, each world update while it runs,
// before each call of the falls judge, its panel over the match.
// A character's AI target (its pair's +20, last update; slobber_knocker.cpp) - research aid.
uint32_t AiTargetOf(uint32_t character);
bool GauntletEliminatorMatch();
void GauntletEliminatorStart();
void GauntletEliminatorUpdate(PPCContext& ctx, uint8_t* base);
void GauntletEliminatorBeforeJudge(PPCContext& ctx, uint8_t* base);
void InstallGauntletEliminatorOverlay(rex::ui::ImGuiDrawer* drawer);

// THREE STAGES OF HELL (three_stages.cpp): set up (on or not) with a match,
// before each call of the falls judge, its score overlay.
constexpr int kThreeStagesSets = 6;  // (its submenu's sets of stages: three_stages.cpp kSets)
void ThreeStagesSetup(bool on, int set);
// FINISHERS: a move cinematic (a finisher or signature: the world held) now.
void ThreeStagesCinematic();
bool ThreeStagesMatch();
void ThreeStagesUpdate(uint8_t* base);  // (each world update)
void ThreeStagesBeforeJudge(PPCContext& ctx, uint8_t* base);

// Lumberjack (match_types.cpp): before each call of the falls judge - a
// lumberjack's interference isn't a disqualification.
void LumberjackBeforeJudge(uint8_t* base);
void InstallThreeStagesOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace svr2011
