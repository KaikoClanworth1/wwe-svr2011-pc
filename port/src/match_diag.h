// Problem-report diagnostics for a match that freezes (port/docs/PROBLEM_REPORTS.md): what each wrestler last
// asked for, and the mod he comes from - logged once when the world update is stuck, or when the match clock
// runs on with no wrestler asking for a move.
#pragma once

#include <cstdint>
#include <string>

namespace svr2011 {

// A match's move getter answered `move` for `slot` of `fighter` (cam_mod_moves.cpp).
void NoteMove(uint32_t fighter, uint32_t slot, uint32_t move);

// The match line's people (match_types.cpp LogMatch): cleared, then character index (0-9) -> its superstar id
// (the roster record's +0x20) and name.
void ClearMatchPeople();
void NoteMatchPerson(uint32_t index, uint32_t superstar, const std::string& name);

// Every wrestler's last move, its age, and its mod - one line each (`why` says what happened).
void LogMatchState(uint8_t* base, const char* why);

// Once a second from the frame-rate watchdog thread: the "no wrestler asks for a move" check.
void MatchDiagTick(uint8_t* base, bool in_match);

}  // namespace svr2011
