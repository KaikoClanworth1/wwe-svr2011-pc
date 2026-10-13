// Port page (checklist D3): something from another WWE disc into SvR 2011, in
// three steps - a disc, what to port (superstar, arena, moves, entrance,
// belt, weapon), the thing itself - through the porting chats' converters
// (tools/<kind>_port.py, the contract in docs/MOD_MAKER_3_PLAN.md section 4:
// `games`, `list`, `convert`, JSON on stdout). The list says what 2011
// already has; the converted mod opens on its page to finish and save.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <thread>

#include "app.h"
#include "full_preview.h"
#include "json.h"
#include "svrfmt/disc.h"
#include "tool_run.h"

#include <shellapi.h>

namespace mm {
namespace port_page {

namespace {

struct Kind {
  const char* title;     // the tab
  const wchar_t* script; // tools/<kind>_port.py
  const char* about;
};
const Kind kKinds[] = {
    {"Superstars", L"superstar_port.py", "A whole superstar mod: model, pictures, theme, titantron, ratings, entrance and calls where 2011 has them."},
    {"Moves", L"moves_port.py", "Moves 2011 lacks, as a move pack (the Moves page saves it; a superstar mod can carry it)."},
    {"Entrances", L"entrance_port.py", "An entrance as a move pack with its own number, for a superstar mod's entrance= (and per attire)."},
    {"Arenas", L"arena_port.py", "An arena mod on one of 2011's arena slots, with its banner, loading and VS pictures."},
    {"Attires", L"attire_port.py", "Attires for a superstar - a mod's own, or new ones for 2011's roster."},
    {"Belts", L"belt_port.py", "Title belts' looks, as a texture pack."},
    {"Weapons", L"weapon_port.py", "Weapons' looks, as a texture pack."},
};
constexpr int kKindCount = int(sizeof kKinds / sizeof kKinds[0]);

int g_kind = 0;
int g_disc = -1;               // index into Discs()
std::wstring g_folder;         // or an extracted folder
std::map<int, Json> g_games;   // kind -> its `games` answer
std::map<int, bool> g_games_asked;
Json g_list;                   // the `list` answer for g_list_for
std::string g_list_for;        // kind|game|src
std::string g_list_error;
int g_pick = -1;               // the chosen row
char g_find[64] = "";
bool g_hide_have = false;
Json g_result;                 // the last `convert` answer
std::string g_result_error;
char g_options[256] = "";      // extra converter options (advanced)
// --port-test: the steps run by themselves (list, convert, open)
std::string g_test_item;
int g_force_kind = -1;  // (the test's tab: ImGui reports the first tab selected until SetSelected takes)
bool g_test_fp = false;         // (--port-fullpreview: an entrance test's result opens in the Full preview)
int g_test_stage = -1;          // 0 list, 1 convert, 2 open, 3 done

std::wstring g_image;  // a disc image picked by path (test aid), not from the list

// ---- the work folder (<game>\Mods\.convert\port): the converters' results and their copies of disc files
constexpr uint64_t kTrimAbove = 3ull << 30, kTrimTo = 3ull << 29;  // 3 GB -> 1.5 GB
std::atomic<uint64_t> g_cache_bytes{0};
std::atomic<bool> g_cache_busy{false};
bool g_cache_known = false;

fs::path WorkDir() { return fs::path(g_game) / L"Mods" / L".convert" / L"port"; }

uint64_t FolderBytes(const fs::path& f) {
  uint64_t n = 0;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(f, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec)) n += it->file_size(ec);
  return n;
}

// Measure (and with keep, trim: oldest folders out until under kTrimTo; "clear": everything) on a thread.
void CacheJob(bool trim, bool clear_all, fs::path keep = {}) {
  if (g_cache_busy.exchange(true)) return;
  std::thread([dir = WorkDir(), trim, clear_all, keep] {
    std::error_code ec;
    struct Item {
      fs::path path;
      fs::file_time_type when;
      uint64_t bytes;
    };
    std::vector<Item> items;
    uint64_t total = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      if (!e.is_directory(ec)) continue;
      const uint64_t b = FolderBytes(e.path());
      items.push_back({e.path(), e.last_write_time(ec), b});
      total += b;
    }
    std::string removed;
    if (clear_all || (trim && total > kTrimAbove)) {
      std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.when < b.when; });
      for (const auto& it : items) {
        if (!clear_all && total <= kTrimTo) break;
        if (!keep.empty() && fs::equivalent(it.path, keep, ec)) continue;
        fs::remove_all(it.path, ec);
        if (!ec) total -= it.bytes, removed += (removed.empty() ? "" : ", ") + Utf8(it.path.filename().wstring());
      }
    }
    g_cache_bytes = total;
    g_cache_busy = false;
    OnUiThread([removed] {
      g_cache_known = true;
      if (!removed.empty()) Log("port: work folder trimmed: " + removed);
    });
  }).detach();
}

void CacheRow() {
  if (!g_cache_known && !g_cache_busy) CacheJob(false, false);
  const uint64_t b = g_cache_bytes;
  ImGui::TextDisabled("Port work folder: %s", g_cache_busy && !g_cache_known ? "measuring ..." : Human(size_t(b)).c_str());
  ImGui::SameLine();
  Hint("The converters' results and their copies of the disc's files (Mods\\.convert\\port in the game folder). "
       "Converted mods open from here; once saved or installed they aren't needed. Above 3 GB the oldest are "
       "removed after a conversion.");
  ImGui::SameLine();
  ImGui::BeginDisabled(Busy() || g_cache_busy || !b);
  if (ImGui::SmallButton("Clear it")) CacheJob(false, true);
  ImGui::EndDisabled();
}

std::wstring Source() {
  if (!g_image.empty()) return g_image;
  if (!g_folder.empty()) return g_folder;
  const auto& d = Discs();
  return g_disc >= 0 && g_disc < int(d.size()) ? d[size_t(g_disc)].path : L"";
}
std::string SourceId() {
  if (!g_image.empty()) {
    static std::wstring for_image;
    static std::string id;
    if (for_image != g_image) {
      svrfmt::Disc disc;
      for_image = g_image;
      id = disc.Open(g_image) ? disc.Id() : "";
    }
    return id;
  }
  const auto& d = Discs();
  return g_folder.empty() && g_disc >= 0 && g_disc < int(d.size()) ? d[size_t(g_disc)].id : "";
}

bool HaveScript(int k) {
  std::error_code ec;
  return !ToolsDir().empty() && fs::exists(fs::path(ToolsDir()) / kKinds[k].script, ec);
}

// The converter's game for the picked disc: {id, name, status} (null when it doesn't know the disc).
const Json& GameFor(int k) {
  static const Json kNone;
  const auto it = g_games.find(k);
  if (it == g_games.end() || it->second.type != Json::Type::kArray) return kNone;
  const std::string id = SourceId();
  if (id.empty()) return kNone;
  for (const auto& g : it->second.items) {  // disc_hint, or any of disc_hints (one game on several discs: x360 + PS2)
    if (g["disc_hint"].Str() == id) return g;
    for (const auto& h : g["disc_hints"].items)
      if (h.Str() == id) return g;
  }
  return kNone;
}

void AskGames(int k) {
  if (g_games_asked[k] || !HaveScript(k) || Busy() || PythonExe().empty()) return;
  g_games_asked[k] = true;
  RunToolJson(kKinds[k].script, {L"games"}, [k](int code, const std::string& out) {
    Json j;
    if (code == 0 && ParseJson(out, j)) g_games[k] = std::move(j);
    else Log(std::string("port: ") + Utf8(kKinds[k].script) + " games failed (exit " + std::to_string(code) + ")");
  });
}

std::vector<std::wstring> Options() {  // the advanced field, split on spaces ("quotes" keep spaces)
  std::vector<std::wstring> v;
  const std::string t = g_options;
  std::string cur;
  bool q = false;
  for (char c : t) {
    if (c == '"') q = !q;
    else if (c == ' ' && !q) {
      if (!cur.empty()) v.push_back(Wide(cur)), cur.clear();
    } else cur += c;
  }
  if (!cur.empty()) v.push_back(Wide(cur));
  return v;
}

// An item's handle for convert: "key" (most converters), "id", or entrance_port's "number".
std::string ItemKey(const Json& item) {
  for (const char* f : {"key", "id", "number"})
    if (const std::string v = item[f].Str(); !v.empty()) return v;
  return {};
}

void List(int k, const std::string& game) {
  const std::wstring src = Source();
  g_list_for = std::to_string(k) + "|" + game + "|" + Utf8(src);
  g_list = Json();
  g_list_error.clear();
  g_pick = -1;
  RunToolJson(kKinds[k].script, {L"list", L"--game", Wide(game), L"--src", src, L"--pac11", (fs::path(g_game) / L"pac").wstring()},
              [want = g_list_for](int code, const std::string& out) {
                if (want != g_list_for) return;
                Json j;
                if (code == 0 && ParseJson(out, j) && j.type == Json::Type::kArray) {
                  g_list = std::move(j);
                  Status("Listed " + std::to_string(g_list.items.size()) + " items.");
                } else {
                  Json e;
                  g_list_error = ParseJson(out, e) && !e["error"].Str().empty() ? e["error"].Str()
                                 : "the converter stopped (exit " + std::to_string(code) + "): see the log";
                }
              });
}

int g_template = 0;          // moves: --template <2011 move id> (0: the converter's own pick)
char g_template_find[48] = "";

// A free 2011 move record for a converted move (moves_port --id): a WAZE record with no motion in any m.pac bank and
// not used by an installed move pack - the one with the move's name first; 0 = let the converter pick.
int FreeMoveRecord(const std::string& name) {
  std::set<int> taken;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / L"Moves", ec)) {
    Bytes t;
    if (!e.is_directory(ec) || !svrfmt::ReadFile(Utf8((e.path() / L"pack.txt").wstring()), t)) continue;
    const std::string s(t.begin(), t.end());
    for (size_t at = 0; at < s.size();) {
      size_t nl = s.find('\n', at);
      if (nl == std::string::npos) nl = s.size();
      char word[16] = "";
      int id = 0;
      if (std::sscanf(s.substr(at, nl - at).c_str(), "%15s %d", word, &id) == 2 &&
          (!std::strcmp(word, "waze") || !std::strcmp(word, "wazecopy") || !std::strcmp(word, "wazename")))
        taken.insert(id);
      at = nl + 1;
    }
  }
  int first = 0;
  for (const auto& [id, m] : MoveInfos()) {
    if (anims_page::AnyMotion(id) || taken.count(id)) continue;
    if (!name.empty() && Lower(m.name) == Lower(name)) return id;
    if (!first) first = id;
  }
  return first;
}

void Convert(int k, const std::string& game, const Json& item) {
  const std::string key = ItemKey(item);
  std::string safe;
  for (char c : game + "_" + key) safe += std::isalnum(uint8_t(c)) ? c : '_';
  const fs::path out = WorkDir() / Wide(std::string(kKinds[k].title) + "_" + safe);
  std::error_code ec;
  fs::create_directories(out, ec);
  const bool entrance = std::wstring(kKinds[k].script) == L"entrance_port.py";
  std::vector<std::wstring> args = {L"convert", L"--game", Wide(game), L"--src", Source(), L"--out", out.wstring(),
                                    L"--pac11", (fs::path(g_game) / L"pac").wstring()};
  if (entrance) args.push_back(L"--entrance"), args.push_back(Wide(key));  // (its own options: --entrance, --number)
  else args.push_back(L"--item"), args.push_back(Wide(key));
  const bool moves = std::wstring(kKinds[k].script) == L"moves_port.py";
  if (moves && g_template > 0) args.push_back(L"--template"), args.push_back(std::to_wstring(g_template));
  if (moves) {  // (a free 2011 record, so packs don't collide: the converter's own pick can't see installed packs)
    if (!anims_page::MovesIndexed()) {
      Status("Reading m.pac's moves first (a few seconds) - press Convert again.");
      return;
    }
    if (const int id = FreeMoveRecord(item["name"].Str()); id > 0) {
      args.push_back(L"--id"), args.push_back(std::to_wstring(id));
      Log("port: move record " + std::to_string(id) + " for " + item["name"].Str());
    }
  }
  if ((entrance || moves) && (game == "wwe13" || game == "wwe12" || game == "wwe2k14")) {  // xcompress: its svrmod.exe
    for (const fs::path& cli : {fs::path(ToolsDir()) / L"svrmod.exe", fs::path(ToolsDir()) / L".." / L"out" / L"modmaker" / L"svrmod.exe"})
      if (fs::exists(cli, ec)) {
        args.push_back(L"--svrmod"), args.push_back(fs::weakly_canonical(cli, ec).wstring());
        break;
      }
  }
  if (entrance && !g_settings.bink2_tool.empty())  // (its --titantron: Bink 2 movies through the player's tool)
    args.push_back(L"--bink2-tool"), args.push_back(g_settings.bink2_tool);
  for (auto& o : Options()) args.push_back(o);
  g_result = Json();
  g_result_error.clear();
  auto run = [k, out, entrance](std::vector<std::wstring> a) {
    RunToolJson(kKinds[k].script, a, [out, entrance](int code, const std::string& text) {
    Json j;
    if (!ParseJson(text, j) || j.type != Json::Type::kObject) {
      g_result_error = "the converter stopped (exit " + std::to_string(code) + "): see the log";
      return;
    }
    g_result = std::move(j);
    if (g_result["out"].Str().empty()) {  // (entrance_port's report has no out / opens: its pack is <out>\pack)
      Json o;
      o.type = Json::Type::kString;
      o.s = Utf8((entrance ? out / L"pack" : out).wstring());
      g_result.fields.push_back({"out", std::move(o)});
      if (entrance && g_result["opens"].Str().empty()) {
        Json m;
        m.type = Json::Type::kString;
        m.s = "moves";
        g_result.fields.push_back({"opens", std::move(m)});
      }
    }
    if (code != 0 || !g_result["ok"].Truthy())
      g_result_error = g_result["error"].Str().empty() ? "the conversion failed (exit " + std::to_string(code) + ")" : g_result["error"].Str();
    else
      Status("Converted: " + g_result["out"].Str());
    CacheJob(true, false, fs::path(Wide(g_result["out"].Str())).parent_path());  // (measure, trim above 3 GB)
    });
  };
  if (!entrance) {
    run(args);
    return;
  }
  // an entrance needs a free 2011 entrance number: the converter's free-numbers first
  const std::wstring user = [] {  // (a number given in the options wins)
    const std::string o = g_options;
    const size_t at = o.find("--number");
    return at == std::string::npos ? std::wstring() : Wide(o.substr(at));
  }();
  if (!user.empty()) {
    run(args);
    return;
  }
  RunToolJson(kKinds[k].script, {L"free-numbers", L"--game-pac", (fs::path(g_game) / L"pac").wstring(), L"--mods",
                                 (fs::path(g_game) / L"Mods").wstring()},
              [run, args](int code, const std::string& text) {
                Json j;
                if (code != 0 || !ParseJson(text, j) || j.items.empty()) {
                  g_result_error = "no free 2011 entrance number found (free-numbers: exit " + std::to_string(code) + ")";
                  return;
                }
                auto a = args;
                a.push_back(L"--number"), a.push_back(Wide(j.items.front().Str()));
                Log("port: entrance number " + j.items.front().Str());
                WhenIdle([run, a] { run(a); });  // (after this job: one job at a time)
              });
}

// ---- "native" games (arenas of WWE '12 / '13 / 2K14): the Mod Maker's own converter (Other games page) takes
// the arena pac; here the disc's (or folder's) pac\bg\bgNN.pac are listed and the picked one is copied out to it.
struct NativePac {
  std::string name;  // bgNN.pac
  int entry = -1;    // in the disc (-1: g_folder file)
  uint64_t size = 0;
};
std::string g_native_for;
std::vector<NativePac> g_native;
int g_native_pick = -1;
std::string g_native_test;  // (--port-test: the item to use)

void NativeArenas(const std::string& game, const std::string& game_name) {
  const std::string want = game + "|" + Utf8(Source());
  if (g_native_for != want) {  // the bg pacs, once per source (the disc's table is in memory: quick)
    g_native_for = want, g_native.clear(), g_native_pick = -1;
    if (!g_image.empty()) {
      svrfmt::Disc disc;
      if (disc.Open(g_image)) {
        const int bg = disc.Find("pac/bg");
        if (bg >= 0)
          for (int c : disc.At(bg).children)
            if (const auto& e = disc.At(c); !e.dir && Lower(e.name).rfind("bg", 0) == 0 && Lower(e.name).ends_with(".pac"))
              g_native.push_back({e.name, c, e.size});
      }
    } else {
      std::error_code ec;
      for (const auto& e : fs::directory_iterator(fs::path(g_folder) / L"pac" / L"bg", ec)) {
        const std::string n = Utf8(e.path().filename().wstring());
        if (e.is_regular_file(ec) && Lower(n).rfind("bg", 0) == 0 && Lower(n).ends_with(".pac")) g_native.push_back({n, -1, e.file_size(ec)});
      }
    }
    std::sort(g_native.begin(), g_native.end(), [](const NativePac& a, const NativePac& b) { return a.name < b.name; });
  }
  ImGui::TextWrapped("The Mod Maker converts %s arenas itself: pick the arena's pac, then finish on the Other games page "
                     "(host arena, options, Convert).", game_name.c_str());
  if (g_native.empty()) {
    ImGui::TextColored(kWarn, "No pac\bg\bgNN.pac found here.");
    return;
  }
  ImGui::SetNextItemWidth(260 * g_scale);
  const std::string cur = g_native_pick >= 0 ? g_native[size_t(g_native_pick)].name : "pick an arena pac";
  if (ImGui::BeginCombo("3. The arena", cur.c_str(), ImGuiComboFlags_HeightLarge)) {
    for (int i = 0; i < int(g_native.size()); ++i) {
      char mb[32];
      std::snprintf(mb, sizeof mb, "  %.1f MB", g_native[size_t(i)].size / 1048576.0);
      if (ImGui::Selectable((g_native[size_t(i)].name + mb).c_str(), g_native_pick == i)) g_native_pick = i;
    }
    ImGui::EndCombo();
  }
  if (!g_native_test.empty()) {  // (test: pick by name)
    for (int i = 0; i < int(g_native.size()); ++i)
      if (Lower(g_native[size_t(i)].name) == Lower(g_native_test)) g_native_pick = i;
    if (g_native_pick < 0) Log("port test: no arena pac " + g_native_test), g_native_test.clear();
  }
  ImGui::BeginDisabled(g_native_pick < 0 || Busy());
  PushAccent();
  const bool use = ImGui::Button("4. Use this arena (Other games page)", ImVec2(320 * g_scale, 0)) || (!g_native_test.empty() && g_native_pick >= 0 && !Busy());
  PopAccent();
  ImGui::EndDisabled();
  if (!use) return;
  g_native_test.clear();
  const NativePac p = g_native[size_t(g_native_pick)];
  const std::wstring image = g_image, folder = g_folder;
  const fs::path out = WorkDir() / Wide("Arenas_" + game + "_" + p.name);
  RunInBackground([p, image, folder, out, game] {
    Progress("Copying " + p.name + " off the disc ...");
    Bytes b;
    bool ok = false;
    if (p.entry >= 0) {
      svrfmt::Disc disc;
      ok = disc.Open(image) && disc.ReadAll(p.entry, b);
    } else {
      ok = svrfmt::ReadFile(Utf8((fs::path(folder) / L"pac" / L"bg" / Wide(p.name)).wstring()), b);
    }
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    if (!ok || !svrfmt::WriteFile(Utf8(out.wstring()), b)) { Status("Could not copy " + p.name + "."); return; }
    OnUiThread([out, p, game] {
      Log("port: " + p.name + " -> " + Utf8(out.wstring()) + " (Other games page)");
      other_page::UseArenaFile(out.wstring(), IdFrom(game + "_" + fs::path(p.name).stem().string(), "arena"));
      GoTo(PageId::kOther);
      CacheJob(true, false, out.parent_path());
    });
  });
}

// The converted thing onto its page.
// Embedded (the Superstar page's Simple mode): a converted superstar is handed back, not opened as a project.
bool g_embedded = false;
std::wstring g_embedded_result;

void OpenResult() {
  const std::string out = g_result["out"].Str(), opens = g_result["opens"].Str();
  const fs::path p = Wide(out);
  std::error_code ec;
  if (g_embedded && Lower(p.extension().string()) == ".svrmod") {
    g_embedded_result = p.wstring();
    return;
  }
  if (Lower(p.extension().string()) == ".svrmod") {
    ProjectOpen(p.wstring());
    return;
  }
  if (opens == "moves" && fs::exists(p / L"pack.txt", ec)) {
    moves_page::TestOpen(p.wstring());
    GoTo(PageId::kMoves);
    return;
  }
  if (opens == "belt" || opens == "weapon") {  // a texture pack folder: into the game's Texture Packs
    const fs::path dst = fs::path(g_game) / L"Texture Packs" / p.filename();
    fs::create_directories(dst.parent_path(), ec);
    fs::copy(p, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    Status(ec ? "Could not copy the pack: " + ec.message()
              : "Installed into Texture Packs\\" + Utf8(p.filename().wstring()) + ": tick it in the launcher's Texture packs tab.");
    return;
  }
  if (out.empty() || !fs::exists(p, ec)) {
    Status("The converter didn't say where its result is: see the log.");
    return;
  }
  if (g_test_stage >= 0) {  // (tests never open windows)
    Log("port test: would open " + out);
    return;
  }
  ShellOpen(p.wstring());
}

void DiscRow() {
  const auto& discs = Discs();
  std::string label = !g_image.empty()  ? Utf8(fs::path(g_image).filename().wstring())
                      : !g_folder.empty() ? Utf8(fs::path(g_folder).filename().wstring()) + " (folder)"
                      : g_disc >= 0 && g_disc < int(discs.size()) ? discs[size_t(g_disc)].game
                                                                   : "pick a disc";
  ImGui::SetNextItemWidth(420 * g_scale);
  if (ImGui::BeginCombo("1. The disc", label.c_str())) {
    if (!DiscsReady()) ImGui::TextDisabled("Reading your disc images ...");
    else if (discs.empty()) ImGui::TextDisabled("No disc images found (Game assets: Disc folders...).");
    for (int i = 0; i < int(discs.size()); ++i)
      if (ImGui::Selectable((discs[size_t(i)].game + "##" + std::to_string(i)).c_str(), g_folder.empty() && g_disc == i))
        g_disc = i, g_folder.clear(), g_list = Json(), g_list_for.clear(), g_result = Json();
    ImGui::Separator();
    if (ImGui::Selectable("An extracted folder ...")) {
      const std::wstring f = PickFolder(L"A folder with another game's files (an extracted disc)");
      if (!f.empty()) g_folder = f, g_list = Json(), g_list_for.clear(), g_result = Json();
    }
    ImGui::EndCombo();
  }
  Hint("Your WWE disc images, read as they are (nothing extracted): Xbox 360 SvR 2007 to 2K17 and the PS2 games, "
       "from the disc folders set on the Game assets page.");
}

}  // namespace

void TestClear() { CacheJob(false, true); }
void TestFullPreview() { g_test_fp = true; }
// The ported entrance (g_result: its move pack and number), played whole by `who` before it is installed.
int g_fp_who = 139;
void PortedEntrancePreview(const Json& item) {
  full_preview::Request r;
  r.entrance = std::atoi(g_result["number"].Str().c_str());
  r.star = g_fp_who;
  r.model = (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(g_fp_who) + L".pac")).wstring();
  r.pack = Wide(g_result["out"].Str());
  r.title = (item["name"].Str().empty() ? ItemKey(item) : item["name"].Str()) + " (ported)";
  Log("port: full preview of entrance " + std::to_string(r.entrance) + " from " + g_result["out"].Str());
  full_preview::Open(r);
}

// A kind's steps: the game, the list, convert, the result.
void KindBody(int k) {
  ImGui::TextDisabled("%s", kKinds[k].about);
  if (!HaveScript(k)) {
    ImGui::TextDisabled("This converter (%s) isn't in this build yet.", Utf8(kKinds[k].script).c_str());
    return;
  }
  AskGames(k);
  if (!g_games.count(k)) {
    ImGui::TextDisabled(g_games_asked[k] ? "Asking the converter which games it knows ..." : "");
    return;
  }
  std::string game;
  const Json& g = GameFor(k);
  if (g.IsNull() && g_folder.empty()) {
    ImGui::TextColored(kWarn, "This converter doesn't know this disc (%s).", SourceId().c_str());
    return;
  }
  if (!g.IsNull()) {
    game = g["id"].Str();
    const std::string status = g["status"].Str();
    ImGui::Text("2. %s: %s", g["name"].Str().c_str(), status == "ready" ? "ready" : status.c_str());
    if (status == "native") {
      NativeArenas(game, g["name"].Str());
      return;
    }
    if (status != "ready") {
      ImGui::TextColored(kWarn, "Not portable yet: the format of this game is still being worked out.");
      return;
    }
  } else {  // a folder: pick the game
    static int pick = 0;
    const auto& all = g_games[k].items;
    if (all.empty()) return;
    pick = std::clamp(pick, 0, int(all.size()) - 1);
    ImGui::SetNextItemWidth(320 * g_scale);
    if (ImGui::BeginCombo("2. The folder's game", all[size_t(pick)]["name"].Str().c_str())) {
      for (int i = 0; i < int(all.size()); ++i)
        if (ImGui::Selectable((all[size_t(i)]["name"].Str() + "##" + std::to_string(i)).c_str(), pick == i)) pick = i;
      ImGui::EndCombo();
    }
    game = all[size_t(pick)]["id"].Str();
  }
  const std::string want = std::to_string(k) + "|" + game + "|" + Utf8(Source());
  if (g_test_stage == 0 && !Busy()) List(k, game), g_test_stage = 1;
  const bool index_wait = std::wstring(kKinds[k].script) == L"moves_port.py" && !anims_page::MovesIndexed();  // (Convert needs it)
  if (g_test_stage == 1 && !Busy() && !index_wait && g_list_for == want && (!g_list.items.empty() || !g_list_error.empty())) {
    if (!g_list_error.empty()) Log("port test: list failed: " + g_list_error), g_test_stage = 3;
    for (int i = 0; g_test_stage == 1 && i < int(g_list.items.size()); ++i)
      if (ItemKey(g_list.items[size_t(i)]) == g_test_item) {
        Log("port test: " + std::to_string(g_list.items.size()) + " listed; converting " + g_test_item);
        g_pick = i;
        Convert(k, game, g_list.items[size_t(i)]);
        g_test_stage = 2;
      }
    if (g_test_stage == 1) Log("port test: no item " + g_test_item), g_test_stage = 3;
  }
  if (g_test_stage == 2 && !Busy() && (g_result.type == Json::Type::kObject || !g_result_error.empty())) {
    Log("port test: convert " + (g_result_error.empty() ? "ok: " + g_result["out"].Str() : "FAILED: " + g_result_error));
    g_test_stage = 3;
    if (g_result_error.empty()) {
      if (g_test_fp && std::wstring(kKinds[k].script) == L"entrance_port.py" && g_pick >= 0 && g_pick < int(g_list.items.size()))
        PortedEntrancePreview(g_list.items[size_t(g_pick)]);
      else
        OpenResult();
    }
  }
  ImGui::BeginDisabled(Busy());
  if (ImGui::Button(g_list_for == want ? "List again" : "3. List what's on the disc")) List(k, game);
  ImGui::EndDisabled();
  if (g_list_for != want) return;
  if (!g_list_error.empty()) ImGui::TextColored(kBad, "%s", g_list_error.c_str());
  if (g_list.items.empty()) {
    if (Busy()) ImGui::TextDisabled("Reading the disc ...");
    return;
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(220 * g_scale);
  ImGui::InputTextWithHint("##find", "find", g_find, sizeof g_find);
  ImGui::SameLine();
  ImGui::Checkbox("hide what 2011 already has", &g_hide_have);
  const std::string f = Lower(g_find);
  const float list_h = std::max(160 * g_scale, ImGui::GetContentRegionAvail().y - 190 * g_scale);
  if (ImGui::BeginTable("items", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, list_h))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("2011", ImGuiTableColumnFlags_WidthFixed, 150 * g_scale);
    ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (int i = 0; i < int(g_list.items.size()); ++i) {
      const Json& it = g_list.items[size_t(i)];
      const bool have = it["in_2011"].Truthy();  // (a name / id, or true; false / missing = new)
      if (have && g_hide_have) continue;
      const std::string name = it["name"].Str().empty() ? ItemKey(it) : it["name"].Str();
      if (!f.empty() && Lower(name).find(f) == std::string::npos && Lower(ItemKey(it)).find(f) == std::string::npos) continue;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (ImGui::Selectable((name + "##" + std::to_string(i)).c_str(), g_pick == i, ImGuiSelectableFlags_SpanAllColumns)) g_pick = i;
      ImGui::TableNextColumn();
      if (have) ImGui::TextColored(kWarn, "has it (%s)", it["in_2011"].Str().c_str());
      else ImGui::TextDisabled("new");
      if (it["in_svr07"].Truthy()) {  // (svr06 entrances: SvR 2007 has the same one, a better source)
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "SvR 2007 has it");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("SvR 2007's version of this entrance is the better source: port it from that disc.");
      }
      ImGui::TableNextColumn();
      std::string details;  // the converter's other fields, short
      for (const auto& [key, v] : it.fields) {
        if (key == "name" || key == "in_2011" || key == "in_svr07" || key == "key" || v.type == Json::Type::kArray || v.type == Json::Type::kObject || v.IsNull()) continue;
        if (!details.empty()) details += "  ";
        details += key + " " + v.Str();
      }
      ImGui::TextDisabled("%s", details.c_str());
    }
    ImGui::EndTable();
  }
  if (g_pick < 0 || g_pick >= int(g_list.items.size())) {
    ImGui::TextDisabled("4. Pick one to convert.");
    return;
  }
  const Json& item = g_list.items[size_t(g_pick)];
  ImGui::SetNextItemWidth(360 * g_scale);
  ImGui::InputTextWithHint("Options (advanced)", "e.g. --height 1.05 --no-moves", g_options, sizeof g_options);
  Hint("Extra options the converter takes (each converter's --help lists them), e.g. --base <id> --height <scale> "
       "--no-moves for superstars. Usually none are needed.");
  if (std::wstring(kKinds[k].script) == L"moves_port.py") {  // the 2011 move it is built on
    ImGui::SetNextItemWidth(360 * g_scale);
    const std::string cur = g_template > 0 ? MoveName(g_template) + "  (" + std::to_string(g_template) + ")" : "automatic (the converter picks one of the same kind)";
    if (ImGui::BeginCombo("Template (a 2011 move)", cur.c_str(), ImGuiComboFlags_HeightLarge)) {
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      ImGui::SetNextItemWidth(-1);
      ImGui::InputTextWithHint("##tf", "find a 2011 move by name or number", g_template_find, sizeof g_template_find);
      if (ImGui::Selectable("automatic", g_template <= 0)) g_template = 0;
      const std::string f = Lower(g_template_find);
      int shown = 0;
      for (const auto& [id, m] : MoveInfos()) {
        if (!anims_page::AnyMotion(id)) continue;  // (only moves with motions can be templates)
        if (!f.empty() && Lower(m.name).find(f) == std::string::npos && std::to_string(id).find(f) == std::string::npos) continue;
        if (++shown > 300) { ImGui::TextDisabled("... type more to narrow it down"); break; }
        if (ImGui::Selectable((m.name + "  (" + std::to_string(id) + ")##t" + std::to_string(id)).c_str(), g_template == id)) g_template = id;
      }
      ImGui::EndCombo();
    }
    Hint("The 2011 move the ported move is built on: its table record (category, damage, where it can be done), its "
         "timing events and banks. Pick one of the same kind - a powerbomb for a powerbomb - when the automatic "
         "choice looks wrong (shown after converting).");
  }
  ImGui::BeginDisabled(Busy());
  PushAccent();
  if (ImGui::Button(("4. Convert " + (item["name"].Str().empty() ? ItemKey(item) : item["name"].Str())).c_str())) Convert(k, game, item);
  PopAccent();
  ImGui::EndDisabled();
  if (!g_result_error.empty()) ImGui::TextColored(kBad, "%s", g_result_error.c_str());
  if (g_result.type == Json::Type::kObject && g_result["ok"].Truthy()) {
    ImGui::SameLine();
    if (ImGui::Button(g_result["opens"].Str() == "belt" || g_result["opens"].Str() == "weapon" ? "Install the pack" : g_embedded ? "5. Use it" : "5. Open it to finish")) OpenResult();
    if (const int t = std::atoi(g_result["template"].Str().c_str()); t > 0)  // (moves: what it was built on)
      ImGui::TextDisabled("Built on the 2011 move %s (%d)%s; record %s. Not right? Pick a template above and convert again.",
                          MoveName(t).c_str(), t, g_template > 0 ? "" : ", picked automatically", g_result["id"].Str().c_str());
    if (g_result["opens"].Str() == "moves" && std::wstring(kKinds[k].script) != L"entrance_port.py")
      ImGui::TextColored(kWarn, "To use it in matches, put it in a superstar's moveset (the Moves page, or a superstar's Moveset section).");
    if (std::wstring(kKinds[k].script) == L"entrance_port.py" && std::atoi(g_result["number"].Str().c_str()) > 0) {
      // the ported entrance, played whole before it is installed: its own pack is looked in first
      const StarInfo* w = StarById(g_fp_who);
      ImGui::SetNextItemWidth(220 * g_scale);
      if (ImGui::BeginCombo("##fpwho", w ? w->name.c_str() : "a superstar", ImGuiComboFlags_HeightLarge)) {
        for (const auto& st : Stars())
          if (ImGui::Selectable((st.name + "##fp" + std::to_string(st.id)).c_str(), st.id == g_fp_who)) g_fp_who = st.id;
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      if (ImGui::Button("Full preview")) PortedEntrancePreview(item);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("The whole ported entrance in the practice arena, worn by the superstar picked here.");
    }
    for (const char* list : {"warnings", "dropped", "notes"}) {
      const Json& v = g_result[list];
      for (const auto& w : v.items)
        ImGui::TextColored(std::string(list) == "warnings" ? kWarn : ImVec4(0.65f, 0.65f, 0.7f, 1), "%s%s",
                           std::string(list) == "dropped" ? "left out: " : "", w.Str().c_str());
    }
  }
}


void TestStart(const std::string& spec) {
  std::vector<std::string> p;
  for (size_t at = 0; at <= spec.size();) {
    size_t c = spec.find(',', at);
    if (c == std::string::npos || p.size() == 3) c = spec.size();  // (the options may hold commas)
    p.push_back(spec.substr(at, c - at));
    at = c + 1;
  }
  if (p.size() < 3) return;
  for (int k = 0; k < kKindCount; ++k)
    if (Lower(kKinds[k].title) == Lower(p[0]) || Utf8(kKinds[k].script) == p[0] + "_port.py") g_kind = g_force_kind = k;
  const std::wstring src = Wide(p[1]);
  std::error_code ec;
  if (fs::is_directory(src, ec)) g_folder = src;
  else g_image = src;
  g_test_item = p[2];
  g_native_test = g_kind >= 0 && std::wstring(kKinds[g_kind].script) == L"arena_port.py" ? p[2] : "";
  if (p.size() > 3) std::snprintf(g_options, sizeof g_options, "%s", p[3].c_str());
  g_test_stage = 0;
}

void Draw() {
  Heading("Port from another game", "Bring a superstar, arena, moves, an entrance or more from your other WWE discs into "
                                    "SvR 2011: pick the disc, what to port and the item; it opens on its page to finish.");
  if (PythonExe().empty() || !PythonMissing().empty()) {
    ImGui::TextColored(kWarn, "The converters need Python 3 with numpy and Pillow%s.",
                       PythonExe().empty() ? " (install the Mod Maker tools from the launcher's Setup page (Install Python + numpy + Pillow))" : (": " + PythonMissing()).c_str());
    if (ImGui::SmallButton("Check again")) RecheckTools();
    return;
  }
  DiscRow();
  CacheRow();
  if (Source().empty()) return;
  ImGui::Spacing();
  if (ImGui::BeginTabBar("kinds")) {
    for (int k = 0; k < kKindCount; ++k)
      if (ImGui::BeginTabItem(kKinds[k].title, nullptr, k == g_force_kind ? ImGuiTabItemFlags_SetSelected : 0)) {
        if (g_force_kind < 0 || g_force_kind == k) {
          g_force_kind = -1;
          if (g_kind != k) g_kind = k, g_result = Json(), g_result_error.clear();
        }
        ImGui::EndTabItem();
      }
    ImGui::EndTabBar();
  }
  KindBody(g_kind);
}

void SuperstarFlow() {
  if (PythonExe().empty() || !PythonMissing().empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(kWarn, "Bringing superstars from older games needs Python 3 with numpy and Pillow%s.",
                       PythonExe().empty() ? " (the launcher's Setup page installs them)" : (": " + PythonMissing()).c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton("Check again")) RecheckTools();
    return;
  }
  DiscRow();
  if (Source().empty()) return;
  int k = 0;
  for (int i = 0; i < kKindCount; ++i)
    if (std::wstring(kKinds[i].script) == L"superstar_port.py") k = i;
  if (g_kind != k) g_kind = k, g_result = Json(), g_result_error.clear();
  g_embedded = true;
  KindBody(k);
  g_embedded = false;
}
std::wstring EmbeddedResult() { return g_embedded_result; }
void ClearEmbeddedResult() { g_embedded_result.clear(); }

}  // namespace port_page
}  // namespace mm
