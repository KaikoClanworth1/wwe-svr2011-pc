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
