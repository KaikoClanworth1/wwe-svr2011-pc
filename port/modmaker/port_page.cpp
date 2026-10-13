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
#include <thread>

#include "app.h"
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
  for (const auto& g : it->second.items)
    if (!id.empty() && g["disc_hint"].Str() == id) return g;
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

void Convert(int k, const std::string& game, const Json& item) {
  const std::string key = item["key"].Str();
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
  if (entrance && (game == "wwe13" || game == "wwe12" || game == "wwe2k14")) {  // xcompress: its svrmod.exe
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
  auto run = [k](std::vector<std::wstring> a) {
    RunToolJson(kKinds[k].script, a, [](int code, const std::string& text) {
    Json j;
    if (!ParseJson(text, j) || j.type != Json::Type::kObject) {
      g_result_error = "the converter stopped (exit " + std::to_string(code) + "): see the log";
      return;
    }
    g_result = std::move(j);
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

// The converted thing onto its page.
void OpenResult() {
  const std::string out = g_result["out"].Str(), opens = g_result["opens"].Str();
  const fs::path p = Wide(out);
  std::error_code ec;
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
  ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
    if (Lower(kKinds[k].title) == Lower(p[0]) || Utf8(kKinds[k].script) == p[0] + "_port.py") g_kind = k;
  const std::wstring src = Wide(p[1]);
  std::error_code ec;
  if (fs::is_directory(src, ec)) g_folder = src;
  else g_image = src;
  g_test_item = p[2];
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
      if (ImGui::BeginTabItem(kKinds[k].title, nullptr, g_test_stage == 0 && k == g_kind ? ImGuiTabItemFlags_SetSelected : 0)) {
        if (g_kind != k) g_kind = k, g_result = Json(), g_result_error.clear();
        ImGui::EndTabItem();
      }
    ImGui::EndTabBar();
  }
  const int k = g_kind;
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
      ImGui::TextWrapped("The Mod Maker converts these itself: Other games page.");
      if (ImGui::Button("Open the Other games page")) GoTo(PageId::kOther);
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
  if (g_test_stage == 1 && !Busy() && g_list_for == want && (!g_list.items.empty() || !g_list_error.empty())) {
    if (!g_list_error.empty()) Log("port test: list failed: " + g_list_error), g_test_stage = 3;
    for (int i = 0; g_test_stage == 1 && i < int(g_list.items.size()); ++i)
      if (g_list.items[size_t(i)]["key"].Str() == g_test_item) {
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
    if (g_result_error.empty()) OpenResult();
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
      const bool have = !it["in_2011"].IsNull();
      if (have && g_hide_have) continue;
      const std::string name = it["name"].Str().empty() ? it["key"].Str() : it["name"].Str();
      if (!f.empty() && Lower(name).find(f) == std::string::npos && Lower(it["key"].Str()).find(f) == std::string::npos) continue;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (ImGui::Selectable((name + "##" + std::to_string(i)).c_str(), g_pick == i, ImGuiSelectableFlags_SpanAllColumns)) g_pick = i;
      ImGui::TableNextColumn();
      if (have) ImGui::TextColored(kWarn, "has it (%s)", it["in_2011"].Str().c_str());
      else ImGui::TextDisabled("new");
      ImGui::TableNextColumn();
      std::string details;  // the converter's other fields, short
      for (const auto& [key, v] : it.fields) {
        if (key == "name" || key == "in_2011" || v.type == Json::Type::kArray || v.type == Json::Type::kObject || v.IsNull()) continue;
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
  ImGui::BeginDisabled(Busy());
  PushAccent();
  if (ImGui::Button(("4. Convert " + (item["name"].Str().empty() ? item["key"].Str() : item["name"].Str())).c_str())) Convert(k, game, item);
  PopAccent();
  ImGui::EndDisabled();
  if (!g_result_error.empty()) ImGui::TextColored(kBad, "%s", g_result_error.c_str());
  if (g_result.type == Json::Type::kObject && g_result["ok"].Truthy()) {
    ImGui::SameLine();
    if (ImGui::Button(g_result["opens"].Str() == "belt" || g_result["opens"].Str() == "weapon" ? "Install the pack" : "5. Open it to finish")) OpenResult();
    for (const char* list : {"warnings", "dropped", "notes"}) {
      const Json& v = g_result[list];
      for (const auto& w : v.items)
        ImGui::TextColored(std::string(list) == "warnings" ? kWarn : ImVec4(0.65f, 0.65f, 0.7f, 1), "%s%s",
                           std::string(list) == "dropped" ? "left out: " : "", w.Str().c_str());
    }
  }
}

}  // namespace port_page
}  // namespace mm
