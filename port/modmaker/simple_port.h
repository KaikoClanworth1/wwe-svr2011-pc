// Simple porting: three guided menus, in plain words, for bringing things over from the user's other WWE discs (My
// game discs) - the Simple step frame (simple_steps.h) over the porting chats' converters:
//   Entrance  a game -> a superstar (with their picture) -> watch their entrance in the Full preview -> Add to my
//             game: in Create an Entrance's motion lists as "MOD: <their name>"; giving it to a superstar mod or a
//             roster superstar's attire is an optional extra   (entrance_port.py)
//   Moves     a game -> a superstar -> their moves by group, marked same / similar / can be brought over / not
//             available, each with Play -> a mod superstar or a move pack for Create-a-Moveset -> Install
//             (superstar_port.py moveset / moveset-pack, moves_port.py convert for Play)
//   Taunts    the same for a superstar's taunts -> a mod's taunt slots or a move pack
// Each ends with "Add to my game": the problems with "Fix it" first, the mod doctor after. A converter
// tool that's missing opens the Mod Maker tools panel (tool_run.h) with what to install.
#pragma once

#include <string>
#include <vector>

namespace mm {
namespace simple_port {

enum Kind { kEntrance, kMoves, kTaunts, kKindCount };

// The page (the rail's "Port an entrance" / "Port moves" / "Port taunts").
void Draw(Kind kind);
// Simple superstar's Look step, "From an older game": a game (My game discs), its superstars with pictures, "Bring
// <name> in" (superstar_port convert) with a "Bring everything too" tick. True once, when the converted superstar
// (.svrmod in `mod`) is ready; `everything` the tick (simple_star.cpp then takes it: TakeFromMod).
bool SuperstarImport(std::wstring& mod, bool& everything);
// With "Bring everything too", their moves and taunts come after the look (the slow part, ported one by one): true
// once, when they're in - `pack` the move pack folder ("" when none was needed) and `lines` moves.txt's 0xOFF=id lines
// for the slots that came over (simple_star.cpp puts them in the project). Polled every frame of the Simple page.
bool SuperstarImportMoves(std::wstring& pack, std::vector<std::string>& lines);

// Test aid --simple-port <entrance|moves|taunts|import>,<game key>[,<item key>[,<step>[,add]]]: that menu with the game (its
// disc from My game discs) and the item picked, on that step (--shot shows it); add: Add to my game once ready. import:
// Simple superstar's Look step (step 1: "Bring everything too"; add: Bring in; a 6th field "cancel": Cancel 30 s into the
// moves).
void TestStart(const std::string& spec);

}  // namespace simple_port
}  // namespace mm
