// Match recordings (docs/MATCH_RECORDINGS.md): every match writes logs\matches\<YYYYMMDD-HHMMSS>_<people>\ - what it
// needs to be set up again (match.json), its pads per world update (inputs.bin), the black box (box.txt), a trail of
// positions and motions once a second (trail.txt), what it loaded (loaded.txt) and the mods (mods.txt); done.txt last.
// The newest match_recordings folders are kept (20; 0: none). Written as the match goes: a crash leaves what's there.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

struct PPCContext;

namespace svr2011 {

// A match loads (menu_hooks.cpp sub_823EEB98, on the logic thread): its rand() seed (match.json "seeds").
void MatchRecordMatchLoads();
// A character's CPU AI update runs (slobber_knocker.cpp sub_8280CFD0). In svrtest's cpu=all test match only, 10 s
// after the bell a "CPU" with none is given in match.json as no AI ("no_ai", cpu false - its person 0); a player's
// match.json is left alone (a tag partner on the apron, a Rumble entrant waiting can have none yet).
void MatchRecordAiUpdate(uint32_t character);
// test_match.cpp: SVR2011_TEST_MATCH's cpu=all (svrtest --cpu all).
bool TestMatchAllCpu();

// Each world update (match_types.cpp MatchTypesUpdate, after the update's jobs): the folder at the bell, the trail.
void MatchRecordUpdate(PPCContext& ctx, uint8_t* base);

// A pad's state as the game got it (frame_rate.cpp, XamInputGetState: `state` its 16 bytes, `result` 0 = connected).
void MatchRecordPad(uint32_t user, uint32_t result, const uint8_t* state);

// A replay (SVR2011_REPLAY=<folder>): the recorded pad for this frame instead of the player's (false: none -
// the player's stands), and the recorded world updates for the frame about to run (0: not replaying).
bool MatchReplayPad(uint32_t user, uint32_t& result, uint8_t* state);
int MatchReplayPasses();

// The black box logged a case (match_box.cpp MatchBoxLog): its text into box.txt.
void MatchRecordBox(const std::string& text);

// The match ended (match_box.cpp MatchBoxMatchEnded): the whole box, match.json complete, done.txt.
void MatchRecordEnded(const std::string& box);

// The crash report's line (crash_report.cpp, through the box's text hook): the match's folder, its match.json marked.
void MatchRecordCrashText(FILE* f);
// REPORT A PROBLEM (report_page.cpp): the recording of the match being played (its files written out so far) or of
// the last one this run; "" when none.
std::string MatchRecordFolder();

}  // namespace svr2011
