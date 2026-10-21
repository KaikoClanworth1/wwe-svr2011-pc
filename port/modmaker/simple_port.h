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
bool SuperstarImport(std::wstring& mod, bool& everything, bool offer_everything = true);  // (false: no "Bring everything too")
// With "Bring everything too", their moves and taunts come after the look (the slow part, ported one by one): true
// once, when they're in - `pack` the move pack folder ("" when none was needed) and `lines` moves.txt's 0xOFF=id lines
// for the slots that came over (simple_star.cpp puts them in the project). Polled every frame of the Simple page.
bool SuperstarImportMoves(std::wstring& pack, std::vector<std::string>& lines);

// Simple superstar's Entrance / Moves / Taunts steps, "From another game": that menu's steps in the step (the game -
// a disc, a folder of its files or one file -, the superstar - the Look's own when it came from that game -, Watch /
// choose), then "Use ... for my superstar": true once with what to put in the project. The same state as the stand-
// alone tab of that kind.
struct ForStarResult {
  std::wstring pack;               // a move pack folder for the project's pack ("": none needed)
  std::vector<std::string> lines;  // moves / taunts: moves.txt lines (0xOFF=id) for the move-set
  int entrance = -1;               // entrance: its number (entrance=)
  std::wstring movie;              // entrance: its titantron video ("": none)
  std::string what;                // "<name>'s moves" (for the status line)
};
bool ForStar(Kind k, ForStarResult& out);
// Test aid (--simple-chain): that menu in ForStar picks `game` and `item`, then Use once ready (moves / taunts: all
// theirs that can come over).
void ForStarTest(Kind k, const std::string& game, const std::string& item);
// The project's move pack with another merged in: a new folder (neither is changed); either empty: the other.
std::wstring CombinePacks(const std::wstring& have, const std::wstring& add);

// The attire picker: a PS2 superstar's attires as pictures (superstar_port attires), up to four picked in the order
// they become the mod's attires 1-4 - a small "Choose attires..." line until opened; nothing for other games (their
// own four). game: the converter's id, src: the disc / file, item: list key (a file: the character's hex id).
void AttirePicker(const std::string& game, const std::wstring& src, const std::string& item);
// The choice as convert's --attires ("": none made - the converter's first four).
std::string AttireChoice(const std::string& game, const std::wstring& src, const std::string& item);
// Test aid --attire-pick open|<n,n,...>: the next picker opened (and those attires picked once they're drawn).
void AttireTest(const std::string& spec);

// Test aid --simple-port <entrance|moves|taunts|import>,<game key>[,<item key>[,<step>[,add]]]: that menu with the game (its
// disc from My game discs) and the item picked, on that step (--shot shows it); add: Add to my game once ready. import:
// Simple superstar's Look step (step 1: "Bring everything too"; add: Bring in; a 6th field "cancel": Cancel 30 s into the
// moves).
void TestStart(const std::string& spec);

}  // namespace simple_port
}  // namespace mm
