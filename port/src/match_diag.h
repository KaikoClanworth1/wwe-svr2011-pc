// Problem-report diagnostics for a match that freezes (port/docs/PROBLEM_REPORTS.md): what each wrestler last
// asked for, and the mod he comes from - logged once when the world update is stuck, or when the match clock
// runs on with no wrestler asking for a move.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace svr2011 {

// A match's move getter answered `move` for `slot` of `fighter` (cam_mod_moves.cpp).
void NoteMove(uint32_t fighter, uint32_t slot, uint32_t move);
// The fighter's last answer (false: none noted) - match_box.cpp.
bool LastMoveOf(uint32_t fighter, uint32_t& slot, uint32_t& move);

// The match line's people (match_types.cpp LogMatch): cleared, then character index (0-9) -> its superstar id
// (the roster record's +0x20) and name.
void ClearMatchPeople();
void NoteMatchPerson(uint32_t index, uint32_t superstar, const std::string& name);

// Every wrestler's last move, its age, and its mod - one line each (`why` says what happened).
void LogMatchState(uint8_t* base, const char* why);

// The match's people now (the debug suite's state dump): the match line's, with each one's last move and position.
struct MatchPerson {
  uint32_t index = 0, person = 0, superstar = 0xFFFF;  // character index, the character's person, superstar id
  std::string name, mod;                               // (mod: its folder, empty when not a mod)
  uint32_t last_move = 0xFFFF;                         // (0xFFFF: none asked yet)
  double move_age_s = -1;
  bool has_position = false;
  float x = 0, y = 0, z = 0;             // (the body: the pointer at character +304)
  uint32_t motion = 0;                   // the current motion id (character +212)
  float root_x = 0, root_y = 0, root_z = 0;  // the root position (character +288 / +292 / +296)
};
std::vector<MatchPerson> MatchPeople(uint8_t* base);
uint32_t MatchClock(uint8_t* base);  // the match clock (60 a second)

// Once a second from the frame-rate watchdog thread: the "no wrestler asks for a move" check.
void MatchDiagTick(uint8_t* base, bool in_match);

}  // namespace svr2011
