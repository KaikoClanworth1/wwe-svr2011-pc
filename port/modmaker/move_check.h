// A move of a move pack looked into (the move inspector) and checked before it is saved ("Will it work in-game?"):
// docs/MOVE_MODDING.md. The rules are the game's (src/move_packs.cpp MoveUsable: a record with category bits, every
// body motion file there) and the packs' (docs/MOVE_PACKS.md: phases, the partner's track, op 49 follow-ups).
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mm::move_check {

struct Finding {
  bool error = false;  // false: a warning (it plays, maybe not as meant)
  std::string what, fix;
};

struct Track {
  int x = 0, y = 0, frames = 0;  // frames: pack.txt's
  std::string file, bank;        // the motion file, the bank it goes in (<pac> <GROUP/ENTRY/child>)
  bool file_ok = false;
  int keys = 0;                   // as decoded (0: not decoded)
  int next = -1;                  // the op 49 follow-up (-1 none said, 0 none)
  std::string decode_err;
  std::vector<std::pair<int, int>> attack;  // hit windows (key, group mask)
  std::map<int, int> ops;                   // event op -> count
};

struct Info {
  int id = 0;
  std::string name;
  std::vector<std::string> source;  // the pack's notes about this move, and the porter's report (report.json)
  std::vector<int> phases;          // x of its body tracks (y 0 / 1), sorted
  std::vector<Track> tracks;
  std::string record;               // the move record: "waze <bits>", "copy of <id>", "the game's", "none"
  bool usable = false;              // a record with category bits (the CPU can pick it, a moveset can name it)
  std::vector<std::string> exh, evt, mbd;  // its per-key rows in the pack (hex)
  std::vector<std::string> slots;   // superstar mods' movesets that name it: "<mod>: <slot>"
  std::vector<Finding> findings;    // the check
};

// Move `id` of the pack in `pack` (a folder with pack.txt), with the game folder's m.pac / misc.pac for what the
// pack leaves to the game (its follow-ups, a record it doesn't carry).
Info Inspect(const std::filesystem::path& pack, int id);
// Its plain-text report (for a bug report: what it is, its tracks, the check).
std::string Report(const Info& info);
// The Moves / Installed pages' panel: the inspector and the check, collapsed, with Export report.
void DrawPanel(const std::filesystem::path& pack, int id);
// Test aid --move-check <pack>,<id>,<out.txt> (Test), run from the Moves page's Draw (Tick).
void Test(const std::wstring& spec);
void Tick();

}  // namespace mm::move_check
