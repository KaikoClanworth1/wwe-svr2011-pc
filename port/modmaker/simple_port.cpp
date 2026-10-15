// Simple porting (simple_port.h): Simple entrance / moves / taunt porting - the Simple step frame over the
// converters of docs/MOD_MAKER_3_PLAN.md section 4 (JSON on stdout, exit 0 / 2 / 3):
//   entrance_port.py  games, list, free-numbers, convert --entrance <n> --number <2011 n>
//   superstar_port.py games, list, thumbs (the superstars' pictures), moveset (a superstar's moves with same /
//                     similar / port / none), moveset-pack (the chosen ones as one move pack + moves.txt lines)
//   moves_port.py     convert --item <source move>: one move for Play
// The discs come from My game discs (disc_list.h). Work files go to <game>\Mods\.convert\simple.
//
// What the user sees has no numbers, folders or pack talk (the user, 14 Oct: "it needs to be REAL easy"): an
// entrance is added to the game as an entrance pack whose manifest name= is the superstar's name, so Create an
// Entrance lists it as "MOD: <name>" (src/move_packs.cpp SetModEntrances); giving it to a superstar is an extra.
#include "simple_port.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include "app.h"
#include "char_preview.h"
#include "disc_list.h"
#include "full_preview.h"
#include "json.h"
#include "mod_check.h"
#include "simple_steps.h"
#include "svrfmt/pac.h"
#include "tool_run.h"

namespace mm {
namespace simple_port {

namespace {

using simple::Card;
using simple::CardWidth;
using simple::CardsPerRow;

// ---- the menus' steps: what each step does, and each menu's order (the entrance menu has no "Where to": it always
// goes into the game, and giving it to a superstar is an extra on its last step)

enum { kStepGame, kStepItem, kStepPick, kStepTarget, kStepFinish };

const simple::Step kEntranceSteps[] = {
    {"Game", "The game the entrance comes from. Your discs are set once in My game discs."},
    {"Superstar", "Whose entrance: the walk, the poses, the lights and the fireworks of one superstar."},
    {"Watch", "See it before you bring it over: the whole entrance plays here, with the camera as in the game."},
    {"Add to my game", "One click: the entrance goes into the game. In Create an Entrance it's in the motion lists as "
                       "\"MOD: <their name>\", like any other entrance."},
};
const simple::Step kMovesSteps[] = {
    {"Game", "The game the moves come from. Your discs are set once in My game discs."},
    {"Superstar", "Whose moves: you get that superstar's whole moveset to choose from."},
    {"Moves", "Their moves by kind. Some 2011 already has, some can be brought over, a few can't. Play any of them."},
    {"Where to", "Give the moves to one of your superstars, or add them to Create-a-Moveset for anyone."},
    {"Finish", "Check everything, then put the moves in the game."},
};
const simple::Step kTauntSteps[] = {
    {"Game", "The game the taunts come from. Your discs are set once in My game discs."},
    {"Superstar", "Whose taunts: the poses and gestures they show off with during a match."},
    {"Taunts", "Their taunts. Some 2011 already has, the others can be brought over. Play any of them."},
    {"Where to", "Give the taunts to one of your superstars, or add them to Create-a-Moveset for anyone."},
    {"Finish", "Check everything, then put the taunts in the game."},
};
const simple::Step* StepsOf(Kind k) { return k == kEntrance ? kEntranceSteps : k == kMoves ? kMovesSteps : kTauntSteps; }
const int kEntranceOrder[] = {kStepGame, kStepItem, kStepPick, kStepFinish};
const int kFullOrder[] = {kStepGame, kStepItem, kStepPick, kStepTarget, kStepFinish};
constexpr int kMaxSteps = 5;
int StepCount(Kind k) { return k == kEntrance ? 4 : 5; }
int StepAt(Kind k, int i) { return k == kEntrance ? kEntranceOrder[i] : kFullOrder[i]; }  // the step shown at index i
int IndexOf(Kind k, int step) {                                                         // and back
  for (int i = 0; i < StepCount(k); ++i)
    if (StepAt(k, i) == step) return i;
  return StepCount(k) - 1;
}

// The moveset's groups, in the order shown (superstar_port moveset's `group`).
const std::array<std::pair<const char*, const char*>, 11> kGroups = {{
    {"strikes", "Strikes"},     {"grapples", "Grapples"}, {"running", "Running"},   {"ground", "On the ground"},
    {"corner", "In the corner"}, {"ropes", "Off the ropes"}, {"aerial", "From the top"}, {"signatures", "Signatures"},
    {"finishers", "Finishers"}, {"taunts", "Taunts"},      {"other", "Other moves"},
}};

// A row's status in plain words and its colour.
struct StatusText {
  const char* text;
  ImVec4 col;
};
StatusText StatusOf(const std::string& s) {
  if (s == "same") return {"2011 has it", ImVec4(0.45f, 0.9f, 0.5f, 1)};
  if (s == "similar") return {"2011 has a similar one", ImVec4(0.45f, 0.8f, 0.95f, 1)};
  if (s == "port") return {"can be brought over", ImVec4(1, 0.8f, 0.35f, 1)};
  return {"not available", ImVec4(0.6f, 0.6f, 0.6f, 1)};
}

// ---- state (one per menu, kept while the Mod Maker runs)

struct State {
  int step = 0;            // the index in the menu's order
  std::string game;        // My game discs' key
  // the item: an entrance (entrance_port list) or a superstar (superstar_port list)
  Json list;
  std::string list_for, list_error;
  std::string item, item_name;
  int item_star11 = 0;     // the 2011 roster superstar of the same name (its picture; the views' model)
  char find[64] = "";
  // entrance: converted
  Json result;
  std::string result_for, result_error;
  int number = -1;          // its 2011 entrance number (never shown)
  int shown_on = 0;         // the Full preview's superstar
  bool other = false;       // shown on someone else than the 2011 superstar of the same name
  // moves / taunts: the superstar's moveset
  Json rows;
  std::string rows_for, rows_error;
  std::set<std::string> chosen;  // row slots
  int play_star = 0;             // the 3D view's superstar
  // where to (moves / taunts: 0 a superstar mod, 1 Create-a-Moveset; the entrance's extra: 0 a mod, 1 a roster attire)
  int target = -1;
  std::wstring mod;         // the superstar mod's folder
  int roster = 0, roster_attire = 1;
  bool titantron = true;    // the entrance's titantron video too (when it has one)
  // finish
  std::string installed;    // what Install did ("" not yet)
  std::wstring installed_dir;
  std::string installed_kind;
  std::string label;        // the entrance's name in Create an Entrance (after "MOD: ")
  std::string extra_done;   // the entrance's extra, done
  bool installing = false;
  bool saved = false;       // saved as a mod file
  // the entrance menu without a disc: one entrance file (entrance_port detect / list / convert take it as --src)
  std::wstring file;
  std::string file_game, file_game_name, file_error;  // (detect: the converter's game id and name)
  Json file_info;                                      // (detect's answer: limits[] - what one file can't bring)
  char typed_name[64] = "";                            // the superstar's name when the file has numbers only
};
State g_s[kKindCount];

fs::path WorkDir() { return fs::path(g_game) / L"Mods" / L".convert" / L"simple"; }
std::wstring GameChPac(int id) { return (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(id) + L".pac")).wstring(); }
std::string Safe(std::string s) {
  for (char& c : s)
    if (!std::isalnum(uint8_t(c)) && c != '-' && c != '_') c = '_';
  return s;
}
std::string GameName(const std::string& key) {
  const disc_list::Game* g = disc_list::GameByKey(key);
  return g ? g->name : key;
}

// ---- the converters' games for My game discs' keys

const wchar_t* ScriptOf(Kind k) { return k == kEntrance ? L"entrance_port.py" : L"superstar_port.py"; }

struct Games {
  Json answer;
  bool asked = false;
};
std::map<std::wstring, Games> g_games;  // script -> its `games` answer

const Json& GamesOf(const wchar_t* script) {
  Games& g = g_games[script];
  if (!g.asked && !Busy() && !PythonExe().empty() && !ToolsDir().empty()) {
    g.asked = true;
    RunToolJson(script, {L"games"}, [s = std::wstring(script)](int code, const std::string& out) {
      Json j;
      if (code == 0 && ParseJson(out, j) && j.type == Json::Type::kArray) g_games[s].answer = std::move(j);
      else Log("simple porting: " + Utf8(s) + " games failed (exit " + std::to_string(code) + ")");
    });
  }
  return g.answer;
}

// The converter's game id for a My game discs game ("" when the converter doesn't know it or it isn't ready):
// matched by the disc ids (its disc_hint / disc_hints against the game's ids and the disc's own).
std::string ConverterGame(const wchar_t* script, const disc_list::Game& game) {
  std::set<std::string> ids(game.ids.begin(), game.ids.end());
  if (const std::string own = disc_list::IdFor(game.key); !own.empty()) ids.insert(own);
  for (const auto& g : GamesOf(script).items) {
    if (g["status"].Str() != "ready") continue;
    bool hit = ids.count(g["disc_hint"].Str()) > 0;
    for (const auto& h : g["disc_hints"].items) hit = hit || ids.count(h.Str());
    if (hit) return g["id"].Str();
  }
  return {};
}
std::string ConverterGame(Kind k, const std::string& key) {
  const disc_list::Game* g = disc_list::GameByKey(key);
  return g ? ConverterGame(ScriptOf(k), *g) : std::string();
}

// Where a menu reads from: its one file, else the game's disc from My game discs; the converter's game id for it; the
// game's name.
std::wstring SrcOf(const State& s) { return s.file.empty() ? disc_list::PathFor(s.game) : s.file; }
std::string GameOf(Kind k, const State& s) { return s.file.empty() ? ConverterGame(k, s.game) : s.file_game; }
std::string TitleOf(const State& s) { return s.file.empty() ? GameName(s.game) : s.file_game_name; }
bool HasSource(const State& s) { return s.file.empty() ? !s.game.empty() : !s.file_game.empty(); }

// The converters' --svrmod for WWE '12 - 2K17 (the Mod Maker's own pac tool; RunToolJson opens the tools panel when
// it's missing).
void AddSvrmod(std::vector<std::wstring>& args, const std::string& game) {
  if (game.rfind("wwe", 0) == 0) args.push_back(L"--svrmod"), args.push_back(SvrmodExe());
}

std::wstring Pac11() { return (fs::path(g_game) / L"pac").wstring(); }

// A converter's error answer in plain words.
std::string ErrorOf(int code, const std::string& out, const char* what) {
  Json e;
  if (ParseJson(out, e) && !e["error"].Str().empty()) return e["error"].Str();
  return std::string("the ") + what + " stopped (exit " + std::to_string(code) + "): the log has the details";
}

// ---- pictures: the 2011 roster's own renders by name, else the converter's thumbnails (superstar_port thumbs)

struct Thumbs {
  std::map<std::string, std::wstring> by_key, by_name;  // key / normalised name -> png
  std::map<std::wstring, Picture> loaded;
  bool asked = false;
};
std::map<std::string, Thumbs> g_thumbs;  // converter game -> its superstars' thumbnails

std::string Norm(const std::string& s) {
  std::string o;
  for (char c : Lower(s))
    if (std::isalnum(uint8_t(c))) o += c;
  if (o.rfind("the", 0) == 0 && o.size() > 6) o.erase(0, 3);  // ("The Undertaker" / "Undertaker")
  return o;
}

int RosterByName(const std::string& name) {
  const std::string n = Norm(name);
  if (n.empty()) return 0;
  for (const auto& s : Stars())
    if (Norm(s.name) == n) return s.id;
  return 0;
}

void AskThumbs(const std::string& game, const std::string& key) {
  if (game.empty()) return;  // (the converter's games not read yet)
  Thumbs& t = g_thumbs[game];
  if (t.asked || Busy()) return;
  t.asked = true;
  const std::wstring src = disc_list::PathFor(key);
  if (src.empty()) return;
  std::vector<std::wstring> args = {L"thumbs", L"--game", Wide(game), L"--src", src,
                                    L"--out", (WorkDir() / Wide("thumbs_" + game)).wstring()};
  AddSvrmod(args, game);
  RunToolJson(L"superstar_port.py", args, [game](int code, const std::string& out) {
    Json j;
    if (code != 0 || !ParseJson(out, j) || j.type != Json::Type::kArray) {
      Log("simple porting: no superstar pictures from " + game + " (" + ErrorOf(code, out, "picture list") + ")");
      return;
    }
    Thumbs& t = g_thumbs[game];
    for (const auto& r : j.items) {
      const std::wstring f = Wide(r["thumb"].Str());
      if (f.empty()) continue;
      t.by_key[r["key"].Str()] = f;
      t.by_name[Norm(r["name"].Str())] = f;
    }
  });
}

// A superstar's picture: 2011's own render for the same name, else the converter's thumbnail (by key, else name).
Picture* PictureFor(const std::string& game, const std::string& key, const std::string& name) {
  if (const int id = RosterByName(name)) {
    Picture* p = StarRender(id);
    if (p && !p->Empty()) return p;
  }
  auto it = g_thumbs.find(game);
  if (it == g_thumbs.end()) return nullptr;
  Thumbs& t = it->second;
  std::wstring f;
  if (!key.empty() && t.by_key.count(key)) f = t.by_key[key];
  else if (t.by_name.count(Norm(name))) f = t.by_name[Norm(name)];
  if (f.empty()) return nullptr;
  Picture& p = t.loaded[f];
  if (p.Empty()) {
    Image img;
    if (LoadPicture(f, img)) p.Set(std::move(img));
  }
  return p.Empty() ? nullptr : &p;
}

// ---- the item list (entrances / superstars)

void AskList(Kind k, State& s) {
  const std::string game = GameOf(k, s);
  const std::wstring src = SrcOf(s);
  const std::string want = std::to_string(k) + "|" + game + "|" + Utf8(src);
  if (game.empty() || src.empty() || s.list_for == want || Busy()) return;
  if (!ConvertersReady()) return;  // (the tools panel says what's missing)
  s.list_for = want;
  s.list = Json();
  s.list_error.clear();
  std::vector<std::wstring> args = {L"list", L"--game", Wide(game), L"--src", src};
  if (k == kEntrance) args.push_back(L"--game-pac"), args.push_back(Pac11());
  else args.push_back(L"--pac11"), args.push_back(Pac11());
  AddSvrmod(args, game);
  RunToolJson(ScriptOf(k), args, [sp = &s, want](int code, const std::string& out) {  // (s: a menu's, or the import's)
    State& s = *sp;
    if (s.list_for != want) return;
    Json j;
    if (code == 0 && ParseJson(out, j) && j.type == Json::Type::kArray) s.list = std::move(j);
    else s.list_error = ErrorOf(code, out, "list");
  });
}

// A name as the player reads it: the entrance scripts' "Chavo_Guerrero" -> "Chavo Guerrero".
std::string Shown(std::string n) {
  for (char& c : n)
    if (c == '_') c = ' ';
  return n;
}

std::string ItemKeyOf(const Json& r) {
  const std::string k = r["key"].Str();
  return k.empty() ? r["number"].Str() : k;
}

void Pick(Kind k, State& s, const std::string& key, const std::string& name) {
  const std::string game = s.game;
  const int step = s.step;
  Json list = std::move(s.list);
  const std::string list_for = s.list_for;
  State fresh;
  fresh.game = game, fresh.step = step, fresh.list = std::move(list), fresh.list_for = list_for;
  fresh.file = s.file, fresh.file_game = s.file_game, fresh.file_game_name = s.file_game_name, fresh.file_info = s.file_info;
  s = std::move(fresh);
  s.item = key, s.item_name = name, s.item_star11 = RosterByName(name);
  s.shown_on = s.play_star = s.item_star11;
  (void)k;
}

// ---- one entrance file instead of a disc (entrance_port detect: the game it's from, what one file can't bring)

void LoneFile(State& s, const std::wstring& f) {
  State fresh;
  fresh.step = s.step;
  fresh.file = f;
  s = std::move(fresh);
  std::vector<std::wstring> args = {L"detect", L"--src", f, L"--svrmod", SvrmodExe()};
  RunToolJson(L"entrance_port.py", args, [f](int code, const std::string& o) {
    State& s = g_s[kEntrance];
    if (s.file != f) return;
    Json j;
    if (code == 0 && ParseJson(o, j) && j["ok"].Truthy() && !j["game"].Str().empty()) {
      s.file_game = j["game"].Str(), s.file_game_name = j["name"].Str(), s.file_info = std::move(j);
      Status("That's an entrance file from " + s.file_game_name + ".");
    } else {
      s.file_error = ErrorOf(code, o, "file check");
    }
  });
}

// What the file is and what one file can't bring, in plain words.
void LoneNotes(const State& s) {
  ImGui::Text("Your file: %s", FileName(s.file).c_str());
  if (!s.file_error.empty()) {
    ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "It can't be read as an entrance:");
    ImGui::TextWrapped("%s", simple::Plain(s.file_error).c_str());
    return;
  }
  if (s.file_game.empty()) return ImGui::TextDisabled("Reading it...");
  ImGui::Text("From %s. Press Next to pick the superstar.", s.file_game_name.c_str());
  for (const auto& l : s.file_info["limits"].items) {
    std::string t = l.Str();
    if (const size_t a = t.find(": --titantron needs the disc"); a != std::string::npos) t = t.substr(0, a) + ": it needs the disc";
    ImGui::Bullet();
    ImGui::TextWrapped("%s", t.c_str());
  }
}

// ---- moves / taunts (and Simple superstar's import) without a disc: a folder with some of the game's files, set as
// that game's "disc" in My game discs; what's missing for moves is said in plain words (disc_list::MissingFiles)

std::wstring g_folder;  // a picked folder waiting for its game

void FolderOfFiles(Kind k, State& s) {
  ImGui::Spacing();
  ImGui::Separator();
  if (!s.game.empty())
    if (const std::string note = disc_list::FolderNote(s.game); !note.empty()) {
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "%s", note.c_str());
      ImGui::PopTextWrapPos();
    }
  if (g_folder.empty()) {
    ImGui::TextDisabled("No disc any more?");
    if (ImGui::Button("Use a folder of the game's files instead...")) {
      g_folder = PickFolder(L"A folder with the game's files (the one holding its pac folder)");
    }
    return;
  }
  ImGui::Text("Which game are the files in %s from?", FileName(g_folder).c_str());
  const float w = CardWidth(), h = 60 * g_scale;
  const int per_row = CardsPerRow(w);
  int n = 0;
  for (const auto& g : disc_list::Games()) {
    if (ConverterGame(ScriptOf(k), g).empty()) continue;
    if (n++ % per_row) ImGui::SameLine(0, 8 * g_scale);
    if (Card(g.key, g.name, g.xbox ? "Xbox 360" : "PS2", false, w, h)) {
      disc_list::Set(g.key, g_folder, disc_list::Identify(g_folder));
      State fresh;
      fresh.game = g.key;
      fresh.step = s.step;
      s = std::move(fresh);
      g_folder.clear();
      Status(std::string(g.name) + " now reads from your folder (My game discs has it).");
    }
  }
  if (ImGui::SmallButton("Cancel")) g_folder.clear();
}

// ---- step: the game

void StepGame(Kind k, State& s) {
  const wchar_t* script = ScriptOf(k);
  if (GamesOf(script).items.empty()) {
    if (PythonExe().empty() || ToolsDir().empty()) {
      ImGui::TextWrapped("The Mod Maker's porting tools aren't installed yet.");
      PushAccent();
      if (ImGui::Button("Show what's missing")) OpenToolsPanel("Simple porting needs them");
      PopAccent();
    } else {
      ImGui::TextDisabled("Reading the games the converter knows...");
    }
    return;
  }
  auto supported = [&](const disc_list::Game& g) { return !ConverterGame(script, g).empty(); };
  if (simple::GamePicker("games", s.game, supported)) {
    State fresh;
    fresh.game = s.game;
    fresh.step = s.step;
    s = std::move(fresh);
  }
  if (k != kEntrance) return FolderOfFiles(k, s);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextDisabled("No disc any more?");
  if (ImGui::Button("Use one entrance file instead...")) {
    static const COMDLG_FILTERSPEC kPac[1] = {{L"An entrance file (*.pac)", L"*.pac"}};
    const std::wstring f = PickFile(false, L"An entrance file from another game (Nyujyo0.pac, Entrance01.pac, EVTENT.PAC ...)", kPac, 1);
    if (!f.empty()) LoneFile(s, f);
  }
  if (!s.file.empty()) LoneNotes(s);
}

// ---- step: the item (a superstar's entrance, or a superstar)

void StepItem(Kind k, State& s) {
  if (!HasSource(s)) {
    ImGui::TextDisabled(s.file.empty() ? "Pick the game first." : "Reading your file...");
    return;
  }
  if (s.file.empty() && disc_list::PathFor(s.game).empty()) {
    ImGui::TextWrapped("The disc for this game isn't there any more.");
    if (ImGui::Button("Open My game discs")) GoTo(PageId::kDiscs);
    return;
  }
  AskList(k, s);
  const std::string pics = s.file.empty() ? ConverterGame(kMoves, s.game) : std::string();  // (the pictures: superstar_port's
  // thumbs, its own game id; one file: 2011's renders by name only)
  AskThumbs(pics, s.game);
  if (!s.list_error.empty()) {
    ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "Couldn't read the list:");
    ImGui::TextWrapped("%s", simple::Plain(s.list_error).c_str());
    if (ImGui::Button("Try again")) s.list_for.clear();
    return;
  }
  if (s.list.type != Json::Type::kArray) {
    ImGui::TextDisabled("Reading the superstars from your disc...");
    return;
  }
  ImGui::SetNextItemWidth(260 * g_scale);
  ImGui::InputTextWithHint("##find", "Find a superstar...", s.find, sizeof s.find);
  ImGui::BeginChild("items", ImVec2(0, 0), false);
  const float tw = 96 * g_scale, th = 128 * g_scale;
  const int per_row = std::max(1, int((ImGui::GetContentRegionAvail().x + 6 * g_scale) / (tw + 6 * g_scale)));
  const std::string f = Lower(s.find);
  int n = 0;
  for (const auto& r : s.list.items) {
    const std::string name = Shown(r["name"].Str()), key = ItemKeyOf(r);
    if (name.empty() || (!f.empty() && Lower(name).find(f) == std::string::npos)) continue;
    if (k != kEntrance && !r["model"].IsNull() && !r["model"].Truthy()) continue;  // (no body on the disc: nothing to show)
    if (n++ % per_row) ImGui::SameLine(0, 6 * g_scale);
    ImGui::PushID(key.c_str());
    ImGui::BeginGroup();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::IsRectVisible(ImVec2(tw, th))) {
      if (Picture* pic = PictureFor(pics, k == kEntrance ? std::string() : key, name)) pic->Draw(tw, tw);
      else ImGui::Dummy(ImVec2(tw, tw));
    } else {
      ImGui::Dummy(ImVec2(tw, tw));
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
    ImGui::TextUnformatted(name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const bool on = s.item == key;
    if (on) ImGui::GetWindowDrawList()->AddRect(p, ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_Text), 4 * g_scale, 0, 2 * g_scale);
    if (ImGui::IsItemClicked() && !on) Pick(k, s, key, name);
    if (ImGui::IsItemHovered()) {
      std::string tip = name;
      if (k == kEntrance) {
        if (r["has_pyro"].Truthy()) tip += "\nWith fireworks";
        if (r["has_movie"].Truthy()) tip += "\nWith their titantron video";
      }
      ImGui::SetTooltip("%s", tip.c_str());
    }
    ImGui::PopID();
  }
  if (!n) ImGui::TextDisabled("Nobody found.");
  ImGui::EndChild();
}

// ---- entrance: converted on the Watch step (a free number first), watched in the Full preview

fs::path ResultDir(const State& s) { return WorkDir() / Wide("entrance_" + GameOf(kEntrance, s) + "_" + Safe(s.item)); }
fs::path ResultPack(const State& s) { return ResultDir(s) / L"pack"; }
fs::path ResultMovie(const State& s) {
  std::error_code ec;
  for (const wchar_t* m : {L"movie.bik", L"movie.mp4"})
    if (fs::exists(ResultDir(s) / m, ec)) return ResultDir(s) / m;
  return {};
}
bool Converted(const State& s) { return s.result["ok"].Truthy() && s.number > 0; }

// A name that's only a number (a lone PS2 entrance file: the names are in another pac on the disc) needs typing.
bool NeedsName(const State& s) {
  return s.item_name.empty() || s.item_name.find_first_not_of("0123456789 ") == std::string::npos;
}
std::string EntranceName(const State& s) { return NeedsName(s) && s.typed_name[0] ? std::string(s.typed_name) : s.item_name; }

void Convert(State& s) {
  const std::string game = GameOf(kEntrance, s);
  const std::string want = game + "|" + s.item;
  if (s.result_for == want || Busy() || !ConvertersReady()) return;
  s.result_for = want;
  s.result = Json();
  s.result_error.clear();
  const std::wstring out = ResultDir(s).wstring();
  RunToolJson(L"entrance_port.py", {L"free-numbers", L"--game-pac", Pac11()}, [want, game, out](int code, const std::string& o) {
    State& s = g_s[kEntrance];
    if (s.result_for != want) return;
    Json j;
    if (code != 0 || !ParseJson(o, j) || j.type != Json::Type::kArray || j.items.empty()) {
      s.result_error = code == 0 ? "the game has no room for more entrances (your added ones fill it)" : ErrorOf(code, o, "check");
      return;
    }
    s.number = j.items.front().Int(-1);
    WhenIdle([want, game, out] {
      State& s = g_s[kEntrance];
      if (s.result_for != want) return;
      std::error_code ec;
      fs::remove_all(out, ec);
      std::vector<std::wstring> args = {L"convert", L"--game", Wide(game), L"--src", SrcOf(s),
                                        L"--entrance", Wide(s.item), L"--number", std::to_wstring(s.number),
                                        L"--name", Wide(s.item_name), L"--game-pac", Pac11(), L"--titantron", L"--out", out};
      AddSvrmod(args, game);
      RunToolJson(L"entrance_port.py", args, [want](int code, const std::string& o) {
        State& s = g_s[kEntrance];
        if (s.result_for != want) return;
        Json j;
        if (code == 0 && ParseJson(o, j) && j["ok"].Truthy()) s.result = std::move(j);
        else s.result_error = ErrorOf(code, o, "entrance converter");
      });
    });
  });
}

// The converter's notes in plain words (warnings, what was left out).
void ReportNotes(const Json& r) {
  int n = 0;
  for (const char* f : {"warnings", "dropped"})
    for (const auto& w : r[f].items) {
      if (!n++) ImGui::TextUnformatted("Good to know:");
      ImGui::Bullet();
      ImGui::TextWrapped("%s", simple::Plain(w.Str()).c_str());
    }
}

// The roster superstar picker (a grid of renders): id in `star`.
bool RosterGrid(const char* id, int& star, float height) {
  bool changed = false;
  ImGui::BeginChild(id, ImVec2(0, height), true);
  const float tw = 72 * g_scale;
  const int per_row = std::max(1, int((ImGui::GetContentRegionAvail().x + 6 * g_scale) / (tw + 6 * g_scale)));
  int n = 0;
  for (const auto& st : Stars()) {
    if (n++ % per_row) ImGui::SameLine(0, 6 * g_scale);
    ImGui::PushID(st.id);
    ImGui::BeginGroup();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::IsRectVisible(ImVec2(tw, tw))) {
      if (Picture* pic = StarRender(st.id); pic && !pic->Empty()) pic->Draw(tw, tw);
      else ImGui::Dummy(ImVec2(tw, tw));
    } else {
      ImGui::Dummy(ImVec2(tw, tw));
    }
    ImGui::EndGroup();
    if (star == st.id) ImGui::GetWindowDrawList()->AddRect(p, ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_Text), 4 * g_scale, 0, 2 * g_scale);
    if (ImGui::IsItemClicked() && star != st.id) star = st.id, changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.name.c_str());
    ImGui::PopID();
  }
  ImGui::EndChild();
  return changed;
}

void OpenFullPreview(const State& s) {
  const StarInfo* on = StarById(s.shown_on);
  if (!on) return;
  full_preview::Request r;
  r.entrance = s.number;
  r.star = s.shown_on;
  r.model = GameChPac(s.shown_on);
  r.pack = ResultPack(s).wstring();
  r.movie = ResultMovie(s).wstring();
  r.title = s.item_name + (s.shown_on == s.item_star11 ? std::string() : " (shown on " + on->name + ")");
  full_preview::Open(r);
}

void StepWatch(State& s) {
  if (s.item.empty()) {
    ImGui::TextDisabled("Pick the superstar first.");
    return;
  }
  Convert(s);
  if (!s.result_error.empty()) {
    ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "This entrance couldn't be brought over:");
    ImGui::TextWrapped("%s", simple::Plain(s.result_error).c_str());
    if (ImGui::Button("Try again")) s.result_for.clear();
    return;
  }
  if (!Converted(s)) {
    ImGui::TextDisabled("Bringing %s's entrance over from your disc... (a minute at most)", s.item_name.c_str());
    return;
  }
  ImGui::Text("%s's entrance is ready to watch.", s.item_name.c_str());
  if (!ResultMovie(s).empty()) ImGui::TextDisabled("Their titantron video came along.");
  ImGui::Spacing();
  if (!s.shown_on) s.shown_on = s.item_star11 ? s.item_star11 : (Stars().empty() ? 0 : Stars().front().id);
  const StarInfo* on = StarById(s.shown_on);
  PushAccent();
  if (ImGui::Button("Watch the entrance", ImVec2(220 * g_scale, 40 * g_scale))) OpenFullPreview(s);
  PopAccent();
  ImGui::SameLine();
  ImGui::TextDisabled(s.item_star11 ? "Shown on %s." : "Shown on %s - pick someone else below.", on ? on->name.c_str() : "?");
  if (!s.item_star11 || s.other) RosterGrid("shown_on", s.shown_on, 170 * g_scale);
  else if (ImGui::SmallButton("Show it on someone else")) s.other = true;
  ImGui::Spacing();
  ReportNotes(s.result);
}

// ---- moves / taunts: the superstar's moveset (superstar_port moveset), Play

bool IsTaunt(const Json& r) { return r["group"].Str() == "taunts"; }
bool Mine(Kind k, const Json& r) { return IsTaunt(r) == (k == kTaunts); }

void AskRows(Kind k, State& s) {
  const std::string game = ConverterGame(k, s.game);
  const std::string want = game + "|" + s.item;
  if (s.rows_for == want || Busy() || !ConvertersReady()) return;
  s.rows_for = want;
  s.rows = Json();
  s.rows_error.clear();
  std::vector<std::wstring> args = {L"moveset", L"--game", Wide(game), L"--src", disc_list::PathFor(s.game),
                                    L"--item", Wide(s.item), L"--pac11", Pac11()};
  AddSvrmod(args, game);
  RunToolJson(L"superstar_port.py", args, [k, want](int code, const std::string& out) {
    State& s = g_s[k];
    if (s.rows_for != want) return;
    Json j;
    if (code == 0 && ParseJson(out, j) && j["rows"].type == Json::Type::kArray) {
      s.rows = std::move(j);
      s.chosen.clear();
      for (const auto& r : s.rows["rows"].items)  // (everything that can come, chosen at first)
        if (Mine(k, r) && r["status"].Str() != "none") s.chosen.insert(r["slot"].Str());
    } else {
      s.rows_error = ErrorOf(code, out, "moveset reader");
    }
  });
}

// Play: 2011's own motion (same / similar: its 2011 id), else the move converted for a look (moves_port convert into
// the work folder: seconds for a move, a few minutes for a taunt) and played from there.
struct PlayJob {
  std::string for_;
  bool busy = false;
};
PlayJob g_play;

// A pack's motions for one move as a bank the 3D view plays: the target bank's header (pack.txt `motion` lines name
// it: <pac> <GROUP/ENTRY/child>), then the pack's entries and data.
std::shared_ptr<const Bytes> PackBank(const fs::path& pack, int id) {
  Bytes t;
  if (!svrfmt::ReadFile(PathStr(pack / L"pack.txt"), t)) return nullptr;
  const std::string text(t.begin(), t.end());
  struct Part {
    int x, y, frames;
    std::string file;
  };
  std::vector<Part> parts;
  std::string pac, where;
  for (size_t at = 0; at < text.size();) {
    size_t nl = text.find('\n', at);
    if (nl == std::string::npos) nl = text.size();
    const std::string line = text.substr(at, nl - at);
    at = nl + 1;
    char word[16] = "", p[64] = "", w[96] = "", file[260] = "";
    int mid = 0, x = 0, y = 0, frames = 0;
    if (std::sscanf(line.c_str(), "%15s %63s %95s %d %d %d %d %259s", word, p, w, &mid, &x, &y, &frames, file) == 8 &&
        !std::strcmp(word, "motion") && mid == id) {
      if (pac.empty()) pac = p, where = w;
      parts.push_back({x, y, frames, file});
    }
  }
  if (parts.empty()) return nullptr;
  const size_t s1 = where.find('/'), s2 = s1 == std::string::npos ? s1 : where.find('/', s1 + 1);
  if (s2 == std::string::npos) return nullptr;
  std::string group = where.substr(0, s1);
  const std::string entry = where.substr(s1 + 1, s2 - s1 - 1);
  const int child = std::atoi(where.c_str() + s2 + 1);
  while (group.size() < 4) group += ' ';
  Bytes raw, head;
  if (!ReadPacEntry(fs::path(g_game) / L"pac" / fs::u8path(pac), entry.c_str(), raw, group.c_str())) return nullptr;
  std::vector<svrfmt::PachEntry> kids;
  if (svrfmt::PachRead(svrfmt::Unpack(raw), kids))
    for (const auto& kid : kids)
      if (int(kid.id) == child) head = svrfmt::Unpack(kid.data);
  if (head.size() < 0x114) return nullptr;
  auto bank = std::make_shared<Bytes>(head.begin(), head.begin() + 0x110);
  const uint32_t n = uint32_t(parts.size());
  bank->resize(0x114 + 16 * size_t(n));
  svrfmt::PutLe32(&(*bank)[0x110], n);
  Bytes data;
  for (uint32_t i = 0; i < n; ++i) {
    Bytes d;
    if (!svrfmt::ReadFile(PathStr(pack / fs::u8path(parts[i].file)), d)) return nullptr;
    uint8_t* e = &(*bank)[0x114 + 16 * i];
    e[0] = uint8_t(parts[i].y), e[1] = uint8_t(parts[i].x), e[2] = uint8_t(id), e[3] = uint8_t(id >> 8);
    svrfmt::PutLe32(e + 4, uint32_t(data.size()));
    svrfmt::PutLe32(e + 8, uint32_t(parts[i].frames));
    svrfmt::PutLe32(e + 12, 0);
    data.insert(data.end(), d.begin(), d.end());
  }
  bank->insert(bank->end(), data.begin(), data.end());
  return bank;
}

void PlayRow(Kind k, State& s, const Json& r) {
  const std::string st = r["status"].Str();
  const int id11 = r["id11"].Int(0);
  if ((st == "same" || st == "similar") && id11 > 0) {
    if (!anims_page::HasMotion(id11)) return Status("The Mod Maker can't show that one: only the game plays it.");
    anims_page::WatchMove(id11);  // (2011's own motion, read from m.pac)
    return;
  }
  if (st != "port" || Busy() || !ConvertersReady()) return;
  const std::string game = ConverterGame(k, s.game), src_id = r["source_id"].Str();
  const fs::path out = WorkDir() / Wide("play_" + game + "_" + Safe(src_id));
  g_play = {game + "|" + src_id, true};
  std::vector<std::wstring> args = {L"convert", L"--game", Wide(game), L"--src", disc_list::PathFor(s.game),
                                    L"--item", Wide(src_id), L"--out", out.wstring(),
                                    L"--cache", (WorkDir() / Wide("cache_" + game)).wstring()};
  AddSvrmod(args, game);
  Status(k == kTaunts ? "Getting the taunt ready to play (this can take a few minutes)..." : "Getting the move ready to play...");
  RunToolJson(L"moves_port.py", args, [want = g_play.for_, out](int code, const std::string& o) {
    if (g_play.for_ != want) return;
    g_play.busy = false;
    Json j;
    if (code != 0 || !ParseJson(o, j) || !j["ok"].Truthy()) {
      Status("That move can't be shown: " + simple::Plain(ErrorOf(code, o, "move converter")));
      return;
    }
    const int id = j["id"].Int(0);
    fs::path pack = fs::u8path(j["folder"].Str().empty() ? j["out"].Str() : j["folder"].Str());
    if (!pack.is_absolute()) pack = out / pack;
    const auto bank = PackBank(pack, id);
    std::string err;
    if (!bank || !char_preview::SetMotion(bank, id, 0, &err))
      Status("That move can't be shown: " + (bank ? err : std::string("its motions didn't read")) + ".");
    else
      Status("Playing " + j["name"].Str() + ".");
  });
}

void StepRows(Kind k, State& s) {
  if (s.item.empty()) {
    ImGui::TextDisabled("Pick the superstar first.");
    return;
  }
  AskRows(k, s);
  if (!s.rows_error.empty()) {
    ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "Couldn't read %s's moves:", s.item_name.c_str());
    ImGui::TextWrapped("%s", simple::Plain(s.rows_error).c_str());
    if (ImGui::Button("Try again")) s.rows_for.clear();
    return;
  }
  if (s.rows["rows"].type != Json::Type::kArray) {
    ImGui::TextDisabled("Reading %s's moves from your disc...", s.item_name.c_str());
    return;
  }
  if (ImGui::SmallButton("Choose all")) {
    for (const auto& r : s.rows["rows"].items)
      if (Mine(k, r) && r["status"].Str() != "none") s.chosen.insert(r["slot"].Str());
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Choose none")) s.chosen.clear();
  ImGui::SameLine();
  ImGui::TextDisabled("%d chosen", int(s.chosen.size()));
  ImGui::BeginChild("rows", ImVec2(0, 0), false);
  const float row_w = ImGui::GetContentRegionAvail().x;  // (the columns follow the card's width: name, status, Play)
  const float col_status = std::max(200 * g_scale, row_w * 0.48f), col_play = std::max(col_status + 150 * g_scale, row_w - 70 * g_scale);
  for (const auto& [gkey, gname] : kGroups) {
    if ((k == kTaunts) != !std::strcmp(gkey, "taunts")) continue;
    bool header = false;
    for (const auto& r : s.rows["rows"].items) {
      const std::string grp = r["group"].Str();
      const bool known = std::any_of(kGroups.begin(), kGroups.end(), [&](const auto& g) { return grp == g.first; });
      if (grp != gkey && !(!known && !std::strcmp(gkey, "other"))) continue;
      if (!header) {
        header = true;
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.75f, 1, 1), "%s", gname);
        ImGui::Separator();
      }
      const std::string slot = r["slot"].Str(), st = r["status"].Str();
      ImGui::PushID(slot.c_str());
      bool on = s.chosen.count(slot) > 0;
      ImGui::BeginDisabled(st == "none");
      if (ImGui::Checkbox("##on", &on)) {
        if (on) s.chosen.insert(slot);
        else s.chosen.erase(slot);
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(r["name"].Str().c_str());
      ImGui::SameLine(col_status);
      const StatusText t = StatusOf(st);
      ImGui::TextColored(t.col, "%s", t.text);
      if (ImGui::IsItemHovered()) {
        std::string tip = r["reason"].Str();
        if (st == "similar" && !r["name11"].Str().empty()) tip = "2011's " + r["name11"].Str() + (tip.empty() ? "" : "\n" + tip);
        if (!tip.empty()) ImGui::SetTooltip("%s", simple::Plain(tip).c_str());
      }
      ImGui::SameLine(col_play);
      const bool playing = g_play.busy && g_play.for_ == ConverterGame(k, s.game) + "|" + r["source_id"].Str();
      const bool own = st == "same" || st == "similar";  // (2011's own motion: shown when m.pac has it, else game-only)
      const bool shown = !own || !anims_page::MovesIndexed() || anims_page::HasMotion(r["id11"].Int(0));
      ImGui::BeginDisabled(!shown);
      if (ImGui::SmallButton(playing ? "Loading..." : "Play") && !playing) PlayRow(k, s, r);
      ImGui::EndDisabled();
      if (!shown && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Only the game can show this one.");
      ImGui::EndDisabled();
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
}

// ---- where to

struct ModRef {
  std::wstring dir;
  std::string name;
};
std::vector<ModRef> SuperstarMods() {
  std::vector<ModRef> v;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / L"Superstars", ec)) {
    Bytes t;
    if (!e.is_directory(ec) || !svrfmt::ReadFile(PathStr(e.path() / L"manifest.txt"), t)) continue;
    std::string name = Value(std::string(t.begin(), t.end()), "name");
    if (name.empty()) name = Utf8(e.path().filename().wstring());
    v.push_back({e.path().wstring(), name});
  }
  std::sort(v.begin(), v.end(), [](const ModRef& a, const ModRef& b) { return Lower(a.name) < Lower(b.name); });
  return v;
}

std::string ModName(const std::wstring& dir) {
  for (const auto& m : SuperstarMods())
    if (m.dir == dir) return m.name;
  return {};
}

void ModPicker(State& s) {
  const auto mods = SuperstarMods();
  if (mods.empty()) {
    ImGui::TextWrapped("You have no superstars of your own yet. Make one on the Superstar page (step by step), then come back.");
    return;
  }
  ImGui::BeginChild("mods", ImVec2(0, 180 * g_scale), true);
  for (const auto& m : mods)
    if (ImGui::Selectable((m.name + "##" + Utf8(m.dir)).c_str(), s.mod == m.dir)) s.mod = m.dir;
  ImGui::EndChild();
}

void RosterAttirePicker(State& s) {
  if (RosterGrid("roster", s.roster, 190 * g_scale)) s.roster_attire = 1;
  if (const StarInfo* st = StarById(s.roster)) {
    const int count = std::max<int>(1, int(ModelAttires(GameChPac(s.roster)).size()));
    ImGui::SetNextItemWidth(160 * g_scale);
    if (ImGui::BeginCombo("Attire", ("Attire " + std::to_string(s.roster_attire)).c_str())) {
      for (int a = 1; a <= count; ++a)
        if (ImGui::Selectable(("Attire " + std::to_string(a)).c_str(), s.roster_attire == a)) s.roster_attire = a;
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s walks out with it in attire %d.", st->name.c_str(), s.roster_attire);
  }
}

void StepTarget(Kind k, State& s) {  // (moves / taunts)
  const float w = CardWidth(), h = 86 * g_scale;
  const int per_row = CardsPerRow(w);
  if (Card("mod", "One of my superstars", k == kTaunts ? "The taunts go in their taunt slots." : "Their moveset gets these moves.", s.target == 0, w, h)) s.target = 0;
  if (1 % per_row) ImGui::SameLine(0, 8 * g_scale);
  if (Card("cam", "Create-a-Moveset", "Anyone can be given them there.", s.target == 1, w, h)) s.target = 1;
  ImGui::Spacing();
  if (s.target == 0) {
    ImGui::TextUnformatted("Which superstar?");
    ModPicker(s);
  }
}

// ---- finish

std::vector<Problem> Problems(Kind k, const State& s) {
  std::vector<Problem> p;
  auto add = [&](int step, std::string text, bool error = true) {
    Problem e = error ? Error(std::move(text)) : Warning(std::move(text));
    e.step = IndexOf(k, step);
    p.push_back(e);
  };
  if (!HasSource(s)) add(kStepGame, s.file.empty() ? "No game picked yet." : "Your file isn't read yet.");
  else if (s.file.empty() && disc_list::PathFor(s.game).empty()) add(kStepGame, "The game's disc isn't in My game discs any more.");
  if (s.item.empty()) add(kStepItem, "No superstar picked yet.");
  if (k == kEntrance) {
    if (!s.item.empty() && !Converted(s))
      add(kStepPick, s.result_error.empty() ? "The entrance isn't ready yet: open Watch first." : "The entrance couldn't be brought over.");
    if (!s.item.empty() && NeedsName(s) && !s.typed_name[0])
      add(kStepFinish, "This file has numbers only: type the superstar's name (it's what Create an Entrance shows).");
    return p;
  }
  if (!s.item.empty()) {
    int chosen = 0;
    bool port = false;
    for (const auto& r : s.rows["rows"].items)
      if (Mine(k, r) && s.chosen.count(r["slot"].Str())) ++chosen, port = port || r["status"].Str() == "port";
    if (!chosen) add(kStepPick, k == kTaunts ? "No taunts chosen." : "No moves chosen.");
    else if (s.target == 1 && !port)
      add(kStepPick, "Everything you chose is in the game already: Create-a-Moveset has these now.", false);
  }
  if (s.target < 0) add(kStepTarget, "Pick who gets them.");
  else if (s.target == 0 && s.mod.empty()) add(kStepTarget, "Pick the superstar.");
  return p;
}

// Copies a folder's files into another (overwrites).
bool CopyInto(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to, ec);
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec) Log("simple porting: copy " + PathStr(from) + " -> " + PathStr(to) + " failed: " + ec.message());
  return !ec;
}

// A pack merged into a superstar mod's moves/ pack: its files copied, its pack.txt lines added (not twice).
bool MergePack(const fs::path& pack, const fs::path& moves) {
  Bytes add, have;
  if (!svrfmt::ReadFile(PathStr(pack / L"pack.txt"), add)) return false;
  svrfmt::ReadFile(PathStr(moves / L"pack.txt"), have);
  std::error_code ec;
  fs::create_directories(moves, ec);
  for (const auto& e : fs::directory_iterator(pack, ec)) {  // (everything but pack.txt: its lines are merged below)
    if (e.path().filename() == L"pack.txt" || e.path().filename() == L"manifest.txt") continue;
    if (e.is_directory(ec)) {
      if (!CopyInto(e.path(), moves / e.path().filename())) return false;
    } else if (fs::copy_file(e.path(), moves / e.path().filename(), fs::copy_options::overwrite_existing, ec); ec) {
      return false;
    }
  }
  std::string text(have.begin(), have.end());
  std::set<std::string> lines;
  auto each = [](const std::string& t, auto fn) {
    for (size_t at = 0; at < t.size();) {
      size_t nl = t.find('\n', at);
      if (nl == std::string::npos) nl = t.size();
      std::string l = t.substr(at, nl - at);
      if (!l.empty() && l.back() == '\r') l.pop_back();
      fn(l);
      at = nl + 1;
    }
  };
  each(text, [&](const std::string& l) { lines.insert(l); });
  if (!text.empty() && text.back() != '\n') text += '\n';
  each(std::string(add.begin(), add.end()), [&](const std::string& l) {
    if (!l.empty() && !lines.count(l)) text += l + "\n", lines.insert(l);
  });
  return svrfmt::WriteFile(PathStr(moves / L"pack.txt"), Bytes(text.begin(), text.end()));
}

// A manifest's key set (replaced, or added at the end).
bool SetManifestKey(const fs::path& manifest, const std::string& key, const std::string& value) {
  Bytes t;
  svrfmt::ReadFile(PathStr(manifest), t);
  const std::string text(t.begin(), t.end());
  std::string out;
  bool done = false;
  for (size_t at = 0; at < text.size();) {
    size_t nl = text.find('\n', at);
    if (nl == std::string::npos) nl = text.size();
    std::string l = text.substr(at, nl - at);
    at = nl + 1;
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (l.rfind(key + "=", 0) == 0) {
      if (done) continue;
      l = key + "=" + value, done = true;
    }
    out += l + "\n";
  }
  if (!done) out += key + "=" + value + "\n";
  return svrfmt::WriteFile(PathStr(manifest), Bytes(out.begin(), out.end()));
}

// moves.txt lines (0xOFF=id) merged into a mod's moves.txt: a slot already there is replaced.
bool MergeMovesTxt(const fs::path& file, const Json& lines) {
  Bytes t;
  svrfmt::ReadFile(PathStr(file), t);
  std::map<std::string, std::string> by;
  std::vector<std::string> order;
  auto put = [&](std::string l) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (l.empty()) return;
    const size_t eq = l.find('=');
    const std::string k = eq == std::string::npos || l[0] == '#' ? l : Lower(l.substr(0, eq));
    if (!by.count(k)) order.push_back(k);
    by[k] = l;
  };
  const std::string text(t.begin(), t.end());
  for (size_t at = 0; at < text.size();) {
    size_t nl = text.find('\n', at);
    if (nl == std::string::npos) nl = text.size();
    put(text.substr(at, nl - at));
    at = nl + 1;
  }
  for (const auto& l : lines.items) put(l.Str());
  std::string out;
  for (const auto& k : order) out += by[k] + "\n";
  return svrfmt::WriteFile(PathStr(file), Bytes(out.begin(), out.end()));
}

// The chosen rows' slots, for moveset-pack --rows.
std::wstring ChosenSlots(Kind k, const State& s) {
  std::wstring v;
  for (const auto& r : s.rows["rows"].items)
    if (Mine(k, r) && s.chosen.count(r["slot"].Str())) v += (v.empty() ? L"" : L",") + Wide(r["slot"].Str());
  return v;
}

// A pack of the game's own (Mods\Moves\<folder>) with a manifest whose name= is what the user sees: the folder from
// the name and the game; the name gets the game's when another pack has it already (two games' entrances of one
// superstar). The label Create an Entrance shows is the name (src/move_packs.cpp).
std::string PackId(const std::string& name, const std::string& game) { return IdFrom(name + " " + game, "pack"); }

// The name a pack is shown by: another installed pack with that name already -> the game's added.
std::string PackLabel(const std::string& name, const std::string& game) {
  const fs::path moves = fs::path(g_game) / L"Mods" / L"Moves";
  const fs::path own = moves / Wide(PackId(name, game));
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(moves, ec)) {
    Bytes t;
    if (e.path() == own || !svrfmt::ReadFile(PathStr(e.path() / L"manifest.txt"), t)) continue;
    if (Lower(Value(std::string(t.begin(), t.end()), "name")) == Lower(name)) return name + " (" + game + ")";
  }
  return name;
}

// A pack's manifest: what the launcher's Mods tab installs (type=moves into Mods\Moves\<id>) and what the game labels
// its entrances by (name=).
std::string PackManifest(const std::string& id, const std::string& label) {
  return "type=moves\nid=" + id + "\nname=" + label + "\nversion=1.0\n" + MadeWith();
}

fs::path InstallPack(const fs::path& from, const std::string& name, const std::string& game, std::string& label) {
  const std::string id = PackId(name, game);
  const fs::path dir = fs::path(g_game) / L"Mods" / L"Moves" / Wide(id);
  label = PackLabel(name, game);
  std::error_code ec;
  fs::remove_all(dir, ec);
  if (!CopyInto(from, dir)) return {};
  const std::string m = PackManifest(id, label);
  if (!svrfmt::WriteFile(PathStr(dir / L"manifest.txt"), Bytes(m.begin(), m.end()))) return {};
  return dir;
}

// ---- Save as a mod file (.svrmod): the same files Add to my game installs, zipped for the launcher's Mods tab

std::wstring g_test_save;  // (test aid: the file a save goes to, no dialog)

// Every file under a folder as zip entries ('/' separated, under `prefix`); dot-files and dot-folders left out.
void FolderEntries(const fs::path& dir, const std::string& prefix, std::vector<svrfmt::ZipEntry>& out) {
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const std::string leaf = Utf8(it->path().filename().wstring());
    if (!leaf.empty() && leaf[0] == '.') {
      if (it->is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec)) continue;
    svrfmt::ZipEntry e;
    e.name = prefix + Utf8(fs::relative(it->path(), dir, ec).generic_wstring());
    if (svrfmt::ReadFile(PathStr(it->path()), e.data)) out.push_back(std::move(e));
  }
}

// Asks where (a Save dialog, <name>.svrmod), writes the zip; the status line says how it's used. False: not saved.
bool SaveModFile(const std::vector<svrfmt::ZipEntry>& files, const std::string& name, const std::string& note) {
  std::wstring f = g_test_save;
  if (f.empty()) f = PickFile(true, L"Save as a mod file", kModFilter, 1, L"svrmod", Wide(IdFrom(name, "mod") + ".svrmod").c_str());
  if (f.empty()) return false;
  if (!svrfmt::WriteFile(PathStr(fs::path(f)), svrfmt::ZipWrite(files))) {
    Status("The mod file couldn't be written: " + Utf8(f));
    return false;
  }
  Log("simple porting: saved " + Utf8(f) + " (" + std::to_string(files.size()) + " files)");
  Status("Saved " + FileName(f) + ". " + note);
  return true;
}

// A pack folder as a mod file (the entrance, or moves / taunts for Create-a-Moveset).
bool SavePack(const fs::path& pack, const std::string& name, const std::string& game) {
  std::vector<svrfmt::ZipEntry> files;
  FolderEntries(pack, "", files);
  files.erase(std::remove_if(files.begin(), files.end(), [](const svrfmt::ZipEntry& e) { return e.name == "manifest.txt"; }), files.end());
  const std::string m = PackManifest(PackId(name, game), PackLabel(name, game));
  files.push_back({"manifest.txt", Bytes(m.begin(), m.end())});
  return SaveModFile(files, name, "Anyone can install it with the launcher's Mods tab (or Open it here).");
}

// The mod doctor on what was added, in this page's words (no "move pack").
std::vector<Problem> Checked(const State& s, int step) {
  std::vector<Problem> out;
  for (Problem p : CheckModFolder(s.installed_kind, s.installed_dir)) {
    if (p.text.rfind("Move packs don't install on Android", 0) == 0) p.text = "This is for the game on PC: the phone version doesn't take added moves and entrances yet.", p.fix.clear();
    p.step = step;
    out.push_back(p);
  }
  return out;
}

void Done(State& s, const std::string& what, const fs::path& dir, const std::string& kind) {
  s.installing = false;
  s.installed = what;
  s.installed_dir = dir.wstring();
  s.installed_kind = kind;
  const std::string note = InstallReport(kind, dir);
  Status(note.empty() ? what : what + " " + note);
  ReloadEntrances();
}

void SaveEntrance(State& s) {
  if (SavePack(ResultPack(s), EntranceName(s), TitleOf(s))) s.saved = true;
}

void AddEntrance(State& s) {
  const fs::path dir = InstallPack(ResultPack(s), EntranceName(s), TitleOf(s), s.label);
  if (dir.empty()) {
    s.installing = false;
    return Status("The entrance couldn't be added (the log says why).");
  }
  Done(s, "Added. In Create an Entrance, pick \"MOD: " + s.label + "\" in the motion lists.", dir, "Moves");
}

// The entrance's extra: one of the user's superstar mods gets it (entrance=, and the titantron video), or a roster
// superstar's attire (an attire mod with only an entrance, docs/SUPERSTAR_MODS.md).
void UseForSuperstar(State& s) {
  const fs::path mods = fs::path(g_game) / L"Mods";
  if (s.target == 0) {
    const fs::path mod = s.mod;
    if (!SetManifestKey(mod / L"manifest.txt", "entrance", std::to_string(s.number)))
      return Status("That superstar couldn't be changed (the log says why).");
    if (s.titantron && !ResultMovie(s).empty()) {
      const fs::path movie = ResultMovie(s);
      std::error_code ec;
      fs::copy_file(movie, mod / movie.filename(), fs::copy_options::overwrite_existing, ec);
      if (!ec) SetManifestKey(mod / L"manifest.txt", "movie", Utf8(movie.filename().wstring()));
    }
    s.extra_done = ModName(s.mod) + " walks out with it now.";
  } else {
    const StarInfo* st = StarById(s.roster);
    if (!st) return;
    const fs::path dir = mods / L"Attires" / Wide(IdFrom(st->name + " attire " + std::to_string(s.roster_attire) + " entrance", "attire"));
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string m = "type=attire\nsuperstar=" + std::to_string(s.roster) + "\nattire=" + std::to_string(s.roster_attire) +
                          "\nentrance=" + std::to_string(s.number) + "\nname=" + s.label + "\n" + MadeWith();
    if (!svrfmt::WriteFile(PathStr(dir / L"manifest.txt"), Bytes(m.begin(), m.end())))
      return Status("That attire couldn't be changed (the log says why).");
    s.extra_done = st->name + " walks out with it in attire " + std::to_string(s.roster_attire) + " now.";
    const std::string note = InstallReport("Attires", dir);
    if (!note.empty()) s.extra_done += " " + note;
  }
  Status(s.extra_done);
}

void InstallMoves(Kind k, State& s, bool save = false) {
  const std::string game = ConverterGame(k, s.game);
  const fs::path out = WorkDir() / Wide(std::string(k == kTaunts ? "taunts_" : "moves_") + game + "_" + Safe(s.item));
  std::error_code ec;
  fs::remove_all(out, ec);
  std::vector<std::wstring> args = {L"moveset-pack", L"--game", Wide(game), L"--src", disc_list::PathFor(s.game),
                                    L"--item", Wide(s.item), L"--rows", ChosenSlots(k, s), L"--pac11", Pac11(),
                                    L"--out", out.wstring(), L"--cache", (WorkDir() / Wide("cache_" + game)).wstring()};
  if (k == kTaunts) args.push_back(L"--taunts-only");
  AddSvrmod(args, game);
  Status(k == kTaunts ? "Bringing the taunts over (a few minutes each)..." : "Bringing the moves over...");
  RunToolJson(L"superstar_port.py", args, [k, save](int code, const std::string& o) {
    State& s = g_s[k];
    Json j;
    const std::string what = k == kTaunts ? "taunts" : "moves";
    if (code != 0 || !ParseJson(o, j) || !j["ok"].Truthy()) {
      s.installing = false;
      return Status("The " + what + " couldn't be brought over: " + simple::Plain(ErrorOf(code, o, "converter")));
    }
    const fs::path pack = fs::u8path(j["out"].Str());
    std::error_code ec;
    const bool has_pack = !pack.empty() && fs::exists(pack / L"pack.txt", ec);
    if (save) {  // (a mod file: for a superstar, the whole superstar mod with them - a copy; else the pack)
      s.installing = false;
      if (s.target == 0) {
        const fs::path copy = WorkDir() / Wide("save_" + Safe(Utf8(fs::path(s.mod).filename().wstring())));
        fs::remove_all(copy, ec);
        if (!CopyInto(s.mod, copy) || (has_pack && !MergePack(pack, copy / L"moves")) || !MergeMovesTxt(copy / L"moves.txt", j["moves_txt"]))
          return Status("The mod file couldn't be made (the log says why).");
        std::vector<svrfmt::ZipEntry> files;
        FolderEntries(copy, "", files);
        if (SaveModFile(files, ModName(s.mod), "It's " + ModName(s.mod) + " with " + s.item_name + "'s " + what +
                                                    ": installing it (launcher's Mods tab) puts them in.")) s.saved = true;
        return;
      }
      if (!has_pack) return Status("Nothing to save: the game has all of these " + what + " already.");
      if (SavePack(pack, s.item_name + (k == kTaunts ? " taunts" : " moves"), GameName(s.game))) s.saved = true;
      return;
    }
    if (s.target == 0) {
      const fs::path mod = s.mod;
      if ((has_pack && !MergePack(pack, mod / L"moves")) || !MergeMovesTxt(mod / L"moves.txt", j["moves_txt"])) {
        s.installing = false;
        return Status("The " + what + " couldn't be given to that superstar (the log says why).");
      }
      return Done(s, ModName(s.mod) + " has " + s.item_name + "'s " + what + " now.", mod, "Superstars");
    }
    if (!has_pack) {
      s.installing = false;
      return Status("Nothing to add: the game has all of these " + what + " already.");
    }
    const fs::path dir = InstallPack(pack, s.item_name + (k == kTaunts ? " taunts" : " moves"), GameName(s.game), s.label);
    if (dir.empty()) {
      s.installing = false;
      return Status("The " + what + " couldn't be added (the log says why).");
    }
    Done(s, s.item_name + "'s " + what + " are in Create-a-Moveset now.", dir, "Moves");
  });
}

void BigButton(const char* label, bool enabled, const std::function<void()>& press) {
  ImGui::BeginDisabled(!enabled);
  PushAccent();
  ImGui::SetWindowFontScale(1.25f);
  if (ImGui::Button(label, ImVec2(300 * g_scale, 56 * g_scale))) press();
  ImGui::SetWindowFontScale(1.0f);
  PopAccent();
  ImGui::EndDisabled();
}

// "Save as a mod file (.svrmod)...": beside Add to my game, for sharing or installing later with the launcher.
void SaveButton(bool enabled, bool saved, const std::function<void()>& press) {
  ImGui::SameLine();
  ImGui::BeginDisabled(!enabled);
  if (ImGui::Button("Save as a mod file (.svrmod)...", ImVec2(0, 56 * g_scale))) press();
  ImGui::EndDisabled();
  if (saved) ImGui::TextDisabled("Saved: install it any time with the launcher's Mods tab, or share it.");
}

void FinishEntrance(State& s) {
  const int finish = IndexOf(kEntrance, kStepFinish);
  if (s.installed.empty()) {
    if (!s.item.empty() && NeedsName(s)) {
      ImGui::TextUnformatted("Whose entrance is it? This file has numbers only.");
      TextField("Their name", s.typed_name, sizeof s.typed_name, "what Create an Entrance shows after MOD:", 320);
    }
    if (!s.item.empty())
      ImGui::Text("%s's entrance%s", EntranceName(s).c_str(), TitleOf(s).empty() ? "" : (", from " + TitleOf(s)).c_str());
    std::vector<Problem> problems = Problems(kEntrance, s);
    simple::ProblemList(problems, s.step, finish);
    ImGui::Spacing();
    BigButton(s.installing ? "Adding..." : "Add to my game", ErrorCount(problems) == 0 && !s.installing && !Busy(), [&] {
      s.installing = true;
      AddEntrance(s);
    });
    SaveButton(ErrorCount(problems) == 0 && !s.installing && !Busy(), s.saved, [&] { SaveEntrance(s); });
    return;
  }
  ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "Done!");
  ImGui::TextWrapped("%s", s.installed.c_str());
  const std::vector<Problem> problems = Checked(s, finish);  // (the mod doctor on what was made)
  if (!problems.empty()) simple::ProblemList(problems, s.step, finish);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  if (ImGui::CollapsingHeader("Also use it for a superstar (optional)")) {
    const float w = CardWidth(), h = 80 * g_scale;
    const int per_row = CardsPerRow(w);
    if (Card("mod", "One of my superstars", "It becomes their entrance (and their titantron video, if it has one).", s.target == 0, w, h)) s.target = 0;
    if (1 % per_row) ImGui::SameLine(0, 8 * g_scale);
    if (Card("attire", "A superstar in the game", "One of their attires walks out with it.", s.target == 1, w, h)) s.target = 1;
    ImGui::Spacing();
    if (s.target == 0) {
      ModPicker(s);
      if (!ResultMovie(s).empty()) ImGui::Checkbox("Their titantron video too", &s.titantron);
    } else if (s.target == 1) {
      RosterAttirePicker(s);
    }
    const bool ready = (s.target == 0 && !s.mod.empty()) || (s.target == 1 && s.roster);
    ImGui::BeginDisabled(!ready);
    PushAccent();
    if (ImGui::Button("Use it", ImVec2(160 * g_scale, 34 * g_scale))) UseForSuperstar(s);
    PopAccent();
    ImGui::EndDisabled();
    if (!s.extra_done.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "%s", s.extra_done.c_str());
    }
  }
  ImGui::Spacing();
  SaveButton(!Busy(), s.saved, [&] { SaveEntrance(s); });
  ImGui::Spacing();
  if (ImGui::Button("Bring over another entrance")) {
    State fresh;
    fresh.game = s.game;
    fresh.step = IndexOf(kEntrance, kStepItem);
    s = std::move(fresh);
  }
}

void FinishMoves(Kind k, State& s) {
  const simple::Step* steps = StepsOf(k);
  const int finish = IndexOf(k, kStepFinish);
  ImGui::TextUnformatted(k == kMoves ? "Your moves" : "Your taunts");
  ImGui::Indent();
  simple::SummaryRow(steps, IndexOf(k, kStepGame), s.game.empty() ? std::string("(none yet)") : GameName(s.game), s.step);
  simple::SummaryRow(steps, IndexOf(k, kStepItem), s.item.empty() ? std::string("(none yet)") : s.item_name, s.step);
  int n = 0, port = 0;
  for (const auto& r : s.rows["rows"].items)
    if (Mine(k, r) && s.chosen.count(r["slot"].Str())) ++n, port += r["status"].Str() == "port";
  simple::SummaryRow(steps, IndexOf(k, kStepPick), std::to_string(n) + " chosen, " + std::to_string(port) + " to bring over", s.step);
  simple::SummaryRow(steps, IndexOf(k, kStepTarget),
                     s.target == 0 && !s.mod.empty() ? ModName(s.mod) : s.target == 1 ? std::string("Create-a-Moveset") : std::string("(not picked yet)"),
                     s.step);
  ImGui::Unindent();
  ImGui::Spacing();
  std::vector<Problem> problems = Problems(k, s);
  if (!s.installed.empty())  // (after Install: the mod doctor on what was made)
    for (const Problem& p : Checked(s, finish)) problems.push_back(p);
  simple::ProblemList(problems, s.step, finish);
  ImGui::Spacing();
  if (!s.installed.empty()) {
    ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "Done!");
    ImGui::TextWrapped("%s", s.installed.c_str());
    if (ImGui::Button("Bring over more")) {
      State fresh;
      fresh.game = s.game;
      fresh.step = IndexOf(k, kStepItem);
      s = std::move(fresh);
    }
    return;
  }
  BigButton(s.installing ? "Adding..." : "Add to my game", ErrorCount(problems) == 0 && !s.installing && !Busy(), [&] {
    s.installing = true;
    InstallMoves(k, s);
  });
  SaveButton(ErrorCount(problems) == 0 && !s.installing && !Busy(), s.saved, [&] {
    s.installing = true;
    InstallMoves(k, s, true);
  });
}

// ---- test aid

struct Test {
  Kind kind = kEntrance;
  std::string game, item;
  int step = -1;
  bool on = false;
  bool add = false;     // (then Add to my game / Bring in, once ready)
  bool import = false;  // (Simple superstar's import, SuperstarImport)
  bool cancel = false;  // (the import's moves cancelled 30 s in)
  std::string mod;      // (moves / taunts add: the superstar mod folder they go to, Mods\Superstars\<mod>)
};
Test g_test;

void TestTick(Kind k, State& s) {
  if (g_test.import) return;
  if (g_test.add && g_test.kind == k && !g_test.on && k != kEntrance && !s.item.empty()) {  // (moves / taunts: to a mod)
    AskRows(k, s);
    if (s.rows["rows"].type == Json::Type::kArray && !s.installing && !Busy()) {
      s.target = g_test.mod.empty() ? 1 : 0;  // (no mod folder given: Create-a-Moveset)
      if (!g_test.mod.empty()) s.mod = (fs::path(g_game) / L"Mods" / L"Superstars" / Wide(g_test.mod)).wstring();
      s.step = IndexOf(k, kStepFinish);
      g_test.add = false;
      if (ErrorCount(Problems(k, s))) {
        for (const auto& p : Problems(k, s)) Log("simple porting test: " + p.text);
      } else {
        s.installing = true;
        InstallMoves(k, s, !g_test_save.empty());
      }
    }
    if (!s.rows_error.empty()) Log("simple porting test: " + s.rows_error), g_test.add = false;
  }
  if (g_test.add && g_test.kind == k && !g_test.on && k == kEntrance && !s.item.empty()) {
    Convert(s);
    if (Converted(s) && s.installed.empty() && !s.installing && !Busy()) {
      g_test.add = false;
      if (!g_test_save.empty()) SaveEntrance(s);
      else s.installing = true, AddEntrance(s);
    }
    if (!s.result_error.empty()) Log("simple porting test: " + s.result_error), g_test.add = false;
  }
  if (!g_test.on || g_test.kind != k) return;
  if (g_test.game.rfind("file:", 0) == 0) {  // (one entrance file instead of a disc)
    if (s.file.empty()) LoneFile(s, Wide(g_test.game.substr(5)));
    if (s.file_game.empty()) return;
  } else if (s.game != g_test.game) {
    s.game = g_test.game;
  }
  if (!g_test.item.empty() && s.item.empty() && s.list.type == Json::Type::kArray) {
    std::string name;  // (found first: Pick resets the state the list is in)
    for (const auto& r : s.list.items)
      if (ItemKeyOf(r) == g_test.item) name = Shown(r["name"].Str());
    if (!name.empty()) Pick(k, s, g_test.item, name);
    else Log("simple porting test: no item " + g_test.item + " in the list"), g_test.on = false;
  }
  if (g_test.item.empty() || !s.item.empty()) {
    if (g_test.step >= 0) s.step = std::min(g_test.step, StepCount(k) - 1);
    g_test.on = false;
  } else {
    AskList(k, s);  // (the list is needed for the item: asked whatever step is shown)
  }
}

// ---- Simple superstar's Look step: a superstar from another game, in two stages - the look (superstar_port convert
// --no-moves --no-taunts: model, pictures, theme, entrance, ratings; a minute or so) handed back at once, then with
// "Bring everything too" their moves and taunts (superstar_port moveset-pack --rows all: the slow part, ported one
// by one) handed back after (SuperstarImportMoves). Both show the converter's progress and can be cancelled.

State g_import;               // the game and the superstar picked (the moves menu's kind: superstar_port's list)
bool g_import_all = false;    // "Bring everything too"
std::string g_import_for, g_import_error;
std::wstring g_import_ready;  // a converted .svrmod not handed back yet
bool g_import_ready_all = false;
// stage 2: the moves
std::string g_moves_for, g_moves_error;  // (busy: g_moves_for set, no result / error yet)
bool g_moves_done = false;
std::wstring g_moves_pack;               // its move pack folder ("": none needed)
std::vector<std::string> g_moves_lines;  // its moves.txt lines (0xOFF=id) for the rows that came over

bool ImportRunning() { return !g_import_for.empty() && g_import_ready.empty() && g_import_error.empty(); }
bool MovesRunning() { return !g_moves_for.empty() && !g_moves_done && g_moves_error.empty(); }

void ImportConvert() {
  State& s = g_import;
  const std::string game = ConverterGame(kMoves, s.game);
  if (game.empty() || s.item.empty() || Busy() || !ConvertersReady()) return;
  const std::string want = game + "|" + s.item;
  g_import_for = want;
  g_import_error.clear();
  g_import_ready.clear();
  g_moves_for.clear(), g_moves_error.clear(), g_moves_done = false;
  const fs::path out = WorkDir() / Wide("star_" + game + "_" + Safe(s.item));
  std::error_code ec;
  fs::remove_all(out, ec);
  fs::create_directories(out, ec);
  std::vector<std::wstring> args = {L"convert", L"--game", Wide(game), L"--src", disc_list::PathFor(s.game), L"--out", out.wstring(),
                                    L"--pac11", Pac11(), L"--item", Wide(s.item), L"--no-moves", L"--no-taunts"};
  AddSvrmod(args, game);
  Status("Bringing " + s.item_name + " over from your disc...");
  RunToolJson(L"superstar_port.py", args, [want, all = g_import_all](int code, const std::string& o) {
    if (g_import_for != want) return;
    Json j;
    if (code == 0 && ParseJson(o, j) && j["ok"].Truthy() && !j["out"].Str().empty()) {
      g_import_ready = Wide(j["out"].Str());
      g_import_ready_all = all;
    } else {
      g_import_error = ToolCancelled() ? "cancelled" : ErrorOf(code, o, "superstar converter");
      Status(ToolCancelled() ? "Cancelled." : "That superstar couldn't be brought over: " + simple::Plain(g_import_error));
    }
  });
}

// Stage 2: the superstar's whole moveset (moves and taunts) ported into one pack; only the rows that came over
// (same / similar / port) are handed back - a "none" row keeps the project's own move.
void MovesConvert() {
  State& s = g_import;
  const std::string game = ConverterGame(kMoves, s.game);
  if (game.empty() || s.item.empty() || Busy() || !ConvertersReady()) return;
  const std::string want = game + "|" + s.item;
  g_moves_for = want, g_moves_error.clear(), g_moves_done = false, g_moves_pack.clear(), g_moves_lines.clear();
  const fs::path out = WorkDir() / Wide("star_moves_" + game + "_" + Safe(s.item));
  std::error_code ec;
  fs::remove_all(out, ec);
  std::vector<std::wstring> args = {L"moveset-pack", L"--game", Wide(game), L"--src", disc_list::PathFor(s.game),
                                    L"--item", Wide(s.item), L"--rows", L"all", L"--pac11", Pac11(), L"--out", out.wstring(),
                                    L"--cache", (WorkDir() / Wide("cache_" + game)).wstring()};
  AddSvrmod(args, game);
  Status("Bringing " + s.item_name + "'s moves over (one by one: this is the slow part)...");
  RunToolJson(L"superstar_port.py", args, [want](int code, const std::string& o) {
    if (g_moves_for != want) return;
    Json j;
    if (code != 0 || !ParseJson(o, j) || !j["ok"].Truthy()) {
      g_moves_error = ToolCancelled() ? "cancelled" : ErrorOf(code, o, "moves converter");
      Status(ToolCancelled() ? "Moves cancelled: your superstar keeps their fighting style's moves."
                             : "Their moves couldn't be brought over: " + simple::Plain(g_moves_error));
      return;
    }
    std::set<std::string> came;  // (slots whose move came over)
    for (const auto& r : j["rows"].items)
      if (r["status"].Str() != "none") came.insert(Lower(r["slot"].Str()));
    for (const auto& l : j["moves_txt"].items) {
      const std::string line = l.Str();
      const size_t eq = line.find('=');
      if (eq != std::string::npos && came.count(Lower(line.substr(0, eq)))) g_moves_lines.push_back(line);
    }
    g_moves_pack = j["out"].Str().empty() ? std::wstring() : Wide(j["out"].Str());
    g_moves_done = true;
  });
}

// The running stage's progress (the converter's own "<n>%  <text>") and Cancel.
void StageLine(const char* what) {
  ImGui::TextDisabled("%s", what);
  const std::string now = ProgressNow();
  if (!now.empty()) {
    ImGui::SameLine();
    ImGui::TextUnformatted(now.c_str());
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Cancel")) CancelTool();
}

}  // namespace

namespace {
bool ImportList(std::wstring& mod, bool& everything) {
  State& s = g_import;
  if (g_test.on && g_test.import) {  // (test aid: the game and the superstar, then Bring in)
    s.game = g_test.game;
    if (!g_test.item.empty() && s.item.empty() && s.list.type == Json::Type::kArray) {
      std::string name;  // (found first: Pick resets the state the list is in)
      for (const auto& r : s.list.items)
        if (ItemKeyOf(r) == g_test.item) name = Shown(r["name"].Str());
      if (!name.empty()) Pick(kMoves, s, g_test.item, name);
      else Log("simple porting test: no superstar " + g_test.item), g_test.on = false;
    }
    if (!s.item.empty()) g_import_all = g_test.step == 1, g_test.on = false;
  }
  if (g_test.import && g_test.add && !g_test.on && !s.item.empty() && !Busy() && g_import_for.empty()) g_test.add = false, ImportConvert();
  if (!g_import_ready.empty()) {  // (handed back once; then the moves, if asked for)
    mod = g_import_ready, everything = g_import_ready_all;
    g_import_ready.clear();
    g_import_for.clear();
    Status("Here's " + s.item_name + (everything ? ". Their moves come next." : "."));
    if (everything) WhenIdle([] { MovesConvert(); });
    return true;
  }
  if (s.game.empty()) {
    ImGui::TextUnformatted("Which game?");
    StepGame(kMoves, s);
    return false;
  }
  ImGui::Text("From %s", GameName(s.game).c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton("Another game")) {
    s = State();
    g_import_for.clear(), g_import_error.clear();
    return false;
  }
  ImGui::BeginChild("import", ImVec2(0, 330 * g_scale), false);
  StepItem(kMoves, s);
  ImGui::EndChild();
  if (s.item.empty()) return false;
  ImGui::Spacing();
  const bool running = ImportRunning() && g_import_for == ConverterGame(kMoves, s.game) + "|" + s.item;
  ImGui::BeginDisabled(running || MovesRunning() || Busy());
  PushAccent();
  if (ImGui::Button(("Bring " + s.item_name + " in").c_str(), ImVec2(260 * g_scale, 36 * g_scale))) ImportConvert();
  PopAccent();
  ImGui::SameLine();
  ImGui::Checkbox("Bring everything too (moves, theme, entrance, ratings)", &g_import_all);
  ImGui::EndDisabled();
  if (running) StageLine("Their look, pictures and music:");
  if (MovesRunning()) StageLine("Their moves (one by one):");
  static double moves_since = 0;  // (test aid: Cancel 30 s into the moves)
  if (!MovesRunning()) moves_since = 0;
  else if (g_test.cancel && !moves_since) moves_since = ImGui::GetTime();
  else if (g_test.cancel && ImGui::GetTime() - moves_since > 30) g_test.cancel = false, Log("simple porting test: Cancel"), CancelTool();
  for (const std::string* e : {&g_import_error, &g_moves_error})
    if (!e->empty() && *e != "cancelled") {
      ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "That didn't work:");
      ImGui::SameLine();
      ImGui::TextWrapped("%s", simple::Plain(*e).c_str());
    }
  return false;
}

}  // namespace

bool SuperstarImport(std::wstring& mod, bool& everything) {
  if (const std::wstring got = port_page::EmbeddedResult(); !got.empty()) {  // (a lone character file's, below)
    port_page::ClearEmbeddedResult();
    mod = got, everything = g_import_all;
    return true;
  }
  const bool got = ImportList(mod, everything);
  ImGui::Spacing();
  if (ImGui::TreeNode("Use one character file instead...")) {  // (a chNNN.pac / a PS2 CH.PAC on its own: port_page)
    port_page::SuperstarFileFlow();
    ImGui::TreePop();
  }
  return got;
}

bool SuperstarImportMoves(std::wstring& pack, std::vector<std::string>& lines) {
  if (!g_moves_done) return false;
  g_moves_done = false, g_moves_for.clear();
  pack = std::move(g_moves_pack), lines = std::move(g_moves_lines);
  g_moves_pack.clear(), g_moves_lines.clear();
  return true;
}

void Draw(Kind k) {
  State& s = g_s[k];
  TestTick(k, s);
  const int count = StepCount(k);
  s.step = std::clamp(s.step, 0, count - 1);
  const int step = StepAt(k, s.step);
  Heading(k == kEntrance ? "Port an entrance" : k == kMoves ? "Port moves" : "Port taunts",
          k == kEntrance ? "Bring a superstar's entrance over from one of your other WWE games."
          : k == kMoves  ? "Bring a superstar's moves over from one of your other WWE games."
                         : "Bring a superstar's taunts over from one of your other WWE games.");
  const std::vector<Problem> problems = Problems(k, s);
  bool has_error[kMaxSteps] = {};
  simple::ErrorSteps(problems, count, has_error);
  simple::StepList(StepsOf(k), count, s.step, has_error);
  ImGui::SameLine();
  const bool view = k != kEntrance && step == kStepPick;  // (moves / taunts: the 3D view beside the list)
  ImGui::BeginChild("card", ImVec2(view ? -330 * g_scale : 0, -52 * g_scale), true);
  simple::StepTitle(StepsOf(k)[s.step]);
  switch (step) {
    case kStepGame: StepGame(k, s); break;
    case kStepItem: StepItem(k, s); break;
    case kStepPick: k == kEntrance ? StepWatch(s) : StepRows(k, s); break;
    case kStepTarget: StepTarget(k, s); break;
    default: k == kEntrance ? FinishEntrance(s) : FinishMoves(k, s); break;
  }
  ImGui::EndChild();
  if (view) {
    ImGui::SameLine();
    ImGui::BeginChild("view", ImVec2(0, -52 * g_scale), true);
    if (!s.play_star) s.play_star = StarById(139) ? 139 : (Stars().empty() ? 0 : Stars().front().id);  // (none of the same name: John Cena)
    char_preview::SetModel(GameChPac(s.play_star), g_game);
    // (the one the move is done to - only while a move plays: idle, the two stood in one spot)
    char_preview::SetDummy(char_preview::Play().id >= 0 ? GameChPac(s.play_star == 139 ? 125 : 139) : std::wstring());
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    char_preview::Draw(avail.x, avail.y - 34 * g_scale);
    const StarInfo* on = StarById(s.play_star);
    ImGui::TextDisabled("Shown on %s%s.", on ? on->name.c_str() : "a 2011 superstar", s.item_star11 ? " (2011's own)" : "");
    ImGui::EndChild();
  }
  simple::BottomBar(s.step, count, simple::StepError(problems, s.step));
}

void TestStart(const std::string& spec) {
  std::vector<std::string> f;
  for (size_t a = 0, b; a <= spec.size(); a = b + 1) {
    b = spec.find(',', a);
    if (b == std::string::npos) b = spec.size();
    f.push_back(spec.substr(a, b - a));
  }
  if (f.size() < 2) return;
  g_test.import = f[0] == "import";  // (Simple superstar's Look step; step 1 = with "Bring everything too")
  g_test.kind = f[0] == "moves" || g_test.import ? kMoves : f[0] == "taunts" ? kTaunts : kEntrance;
  g_test.game = f[1];
  g_test.item = f.size() > 2 ? f[2] : "";
  g_test.step = f.size() > 3 && !f[3].empty() ? std::atoi(f[3].c_str()) : -1;
  g_test.add = f.size() > 4 && (f[4] == "add" || f[4].rfind("save=", 0) == 0);
  g_test_save = f.size() > 4 && f[4].rfind("save=", 0) == 0 ? Wide(f[4].substr(5)) : L"";
  g_test.cancel = f.size() > 5 && f[5] == "cancel";
  g_test.mod = f.size() > 5 && f[5] != "cancel" ? f[5] : "";
  g_test.on = true;
}

}  // namespace simple_port
}  // namespace mm
