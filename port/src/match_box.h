// A match's black box (docs/LOG_DIAGNOSTICS.md): always on, cheap. Each world update notes every person's motion
// changes (with the move asked last, its slot, and the pack it comes from), position and lying / standing state, and
// a match-level ring of events (rule changes, the referee's motions, weapons made / picked up / dropped / gone,
// motion look-ups no bank answered). Detectors log the box (a few times a match at most) when something looks wrong;
// the crash report and logs\matchbox_<n>.txt (each match's end; the problem report takes them) carry the whole box.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

struct PPCContext;

namespace svr2011 {

// Each world update (match_types.cpp MatchTypesUpdate, after the update's jobs): the box notes and the detectors.
void MatchBoxUpdate(PPCContext& ctx, uint8_t* base);

// A motion look-up's answer (superstar_mods.cpp sub_823941F8: a body track, y 0 / 1): `found` in that bank list or
// not - from any thread. A motion is "missing" when some list missed it and none ever had it this match.
void MatchBoxMotionLooked(uint32_t id, bool found);
// Whether the game's bank search (the sorted banks and the unsorted ones after it) missed motion `id` this match and
// never found it: the missing-motion guard (match_types.cpp) asks.
bool MatchBoxMotionMissing(uint32_t id);

// The scene view was just built (match_types.cpp sub_82227150, view 0x82E3CC00) - any thread. `lr` says who built
// it: the match camera (sub_822285F0, 0x82228894), its cut snap (sub_822280B0, 0x822281B4: one frame as a big move's
// camera cut begins or ends), the Rumble's, backstage's ...; `camera` (their r31) is the match camera object, whose
// +268 is set while a big move's camera cut is on. The box notes the camera each world update: its kind, the game's
// camera mode (0x82E3CCF8) and, for a cut, the moves that were on when it began (report ec87bf: "camera not moving
// after a move").
void MatchBoxCameraView(uint32_t lr, uint32_t camera, float eye_x, float eye_y, float eye_z);

// trail.txt's camera part for this second (match_record.cpp): " | cam <kind> <mode>[ <person>:<motion>... <s>s]".
std::string MatchBoxCameraTrail();

// The box as text (`why`: what asked for it; `full`: every motion and event, else the last few) - any thread.
std::string MatchBoxText(const char* why, bool full);

// The box into the log (compact), once per `why` a match and at most 3 a match: the stuck world update, the
// held pair (match_diag.cpp) and the box's own detectors.
void MatchBoxLog(const char* why);

// A match ended (replays.cpp: the match's end task starts): the whole box to <the log's folder>\matchbox_<n>.txt,
// the newest 3 kept (not the menu's attract match: no referee).
void MatchBoxMatchEnded();

// The crash report's extra text (crash_report.cpp): the whole box, if a match was on.
void MatchBoxCrashText(FILE* f);

}  // namespace svr2011
