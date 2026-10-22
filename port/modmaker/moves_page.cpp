// Moves page: a move pack - moves the game doesn't have (the game:
// move_packs.cpp, docs/MOVE_PACKS.md). A pack is a folder with pack.txt and
// motions/: tools/svr10_moves.py + movepack.py make one from SvR 2010 data.
// Here it is checked, shown move by move, and saved as a .svrmod
// (type=moves, installed into <game>/Mods/Moves/<id>) or handed to a
// superstar mod (its moves/ folder).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>

#include "app.h"
#include "mod_check.h"

namespace mm {
namespace moves_page {

using namespace svrfmt;

namespace {

struct Motion {
  std::string pac, path, file;
  int id = 0, x = 0, y = 0, frames = 0;
  size_t size = 0;
  bool missing = false;
};
struct Pack {
  std::wstring folder;
  char name[64] = "", author[64] = "", version[16] = "1.0";
  std::vector<Motion> motions;
  std::map<int, std::string> names;   // wazename
  std::map<int, std::string> waze;    // waze bits (hex)
  std::map<int, int> copies;          // wazecopy
  int exh = 0, evt = 0, mbd = 0, unknown = 0;
  std::vector<std::string> errors;
  bool loaded = false;
  size_t bytes = 0;
};
Pack g_pack;
int g_sel = -1;  // move id selected in the list

void ParsePack(Pack& p) {
  p.motions.clear(), p.names.clear(), p.waze.clear(), p.copies.clear(), p.errors.clear();
  p.exh = p.evt = p.mbd = p.unknown = 0;
  p.bytes = 0;
  p.loaded = false;
  if (p.folder.empty()) return;
  Bytes t;
  if (!ReadFile(PathStr(fs::path(p.folder) / L"pack.txt"), t)) {
    p.errors.push_back("No pack.txt in the folder.");
    return;
  }
  p.loaded = true;
  std::istringstream in(std::string(t.begin(), t.end()));
  std::string line;
  int n = 0;
  std::error_code ec;
  while (std::getline(in, line)) {
    ++n;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string kind;
    ls >> kind;
    if (kind == "motion") {
      Motion m;
      ls >> m.pac >> m.path >> m.id >> m.x >> m.y >> m.frames >> m.file;
      if (m.file.empty()) { p.errors.push_back("line " + std::to_string(n) + ": motion line incomplete"); continue; }
      const fs::path f = fs::path(p.folder) / fs::u8path(m.file);
      m.missing = !fs::exists(f, ec);
      if (!m.missing) m.size = size_t(fs::file_size(f, ec)), p.bytes += m.size;
      p.motions.push_back(std::move(m));
    } else if (kind == "waze") {
      int id = 0;
      std::string hex;
      ls >> id >> hex;
      if (hex.size() != 32) p.errors.push_back("line " + std::to_string(n) + ": waze needs 16 bytes (32 hex digits)");
      else p.waze[id] = hex;
    } else if (kind == "wazename") {
      int id = 0;
      std::string name;
      ls >> id;
      std::getline(ls, name);
      name.erase(0, name.find_first_not_of(' '));
      if (id) p.names[id] = name;
    } else if (kind == "wazecopy") {
      int id = 0, from = 0;
      ls >> id >> from;
      p.copies[id] = from;
    } else if (kind == "exh" || kind == "evt" || kind == "mbd") {
      int group = 0;
      std::string rec;
      ls >> group >> rec;
      const size_t want = kind == "exh" ? 72 : kind == "evt" ? 32 : 16;
      if (rec.size() != want) p.errors.push_back("line " + std::to_string(n) + ": " + kind + " record is " + std::to_string(rec.size() / 2) + " bytes, " + std::to_string(want / 2) + " expected");
      else (kind == "exh" ? p.exh : kind == "evt" ? p.evt : p.mbd)++;
    } else {
      ++p.unknown;
    }
  }
  // duplicate keys
  std::map<std::string, int> seen;
  for (const auto& m : p.motions) {
    const std::string k = m.pac + "|" + m.path + "|" + std::to_string(m.id) + "/" + std::to_string(m.x) + "/" + std::to_string(m.y);
    if (++seen[k] == 2) p.errors.push_back("motion " + std::to_string(m.id) + " x" + std::to_string(m.x) + " y" + std::to_string(m.y) + " is in " + m.pac + " " + m.path + " twice");
  }
  if (p.motions.empty() && p.waze.empty()) p.errors.push_back("pack.txt has no motion or waze lines.");
}

// The pack's own move records: waze lines and wazecopy (a stock record copied under a new id).
size_t RecordCount(const Pack& p) {
  std::set<int> ids;
  for (const auto& w : p.waze) ids.insert(w.first);
  for (const auto& c : p.copies) ids.insert(c.first);
  return ids.size();
}

void OpenFolder(const std::wstring& f) {
  g_pack.folder = f;
  ParsePack(g_pack);
  if (!g_pack.name[0]) std::snprintf(g_pack.name, sizeof g_pack.name, "%s", FileName(f).c_str());
  g_sel = -1;
  Touch();
  Status(g_pack.loaded ? "Pack: " + std::to_string(g_pack.motions.size()) + " motions, " + std::to_string(RecordCount(g_pack)) + " moves"
                       : "That folder has no pack.txt.");
}

std::string PackId() { return IdFrom(g_pack.name, "moves"); }

// Every file of the pack folder (pack.txt, motions/*), as the mod carries them.
bool PackFiles(const std::wstring& folder, std::vector<ZipEntry>& files, const std::string& prefix) {
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(folder, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string rel = fs::relative(e.path(), folder, ec).generic_string();
    if (rel == "manifest.txt" || rel == "disabled") continue;
    Bytes f;
    if (!ReadFile(PathStr(e.path()), f)) { Status("Could not read " + PathStr(e.path())); return false; }
    files.push_back({prefix + rel, std::move(f)});
  }
  return true;
}

// What the build needs, copied off the UI thread.
struct Job {
  std::string id, manifest, project;  // (project: ModProjectText, into the mod)
  std::wstring folder;
};
Job PrepareJob() {
  if (!g_pack.name[0]) std::snprintf(g_pack.name, sizeof g_pack.name, "My Moves");
  Job j;
  j.id = PackId();
  j.manifest = "type=moves\nid=" + j.id + "\nname=" + g_pack.name + "\nauthor=" + g_pack.author + "\nversion=" +
               g_pack.version + "\n" + MadeWith();
  j.folder = g_pack.folder;
  j.project = ModProjectText(PageId::kMoves);
  return j;
}

bool Build(const Job& j, std::vector<ZipEntry>& files) {
  files.push_back({"manifest.txt", Bytes(j.manifest.begin(), j.manifest.end())});
  AddModProject(files, j.project);
  return PackFiles(j.folder, files, "");
}

void SaveMod() {
  const Job j = PrepareJob();
  const std::wstring f = PickFile(true, L"Save the move pack", kModFilter, 1, L"svrmod", (Wide(j.id) + L".svrmod").c_str());
  if (f.empty()) return;
  const std::string out = Utf8(f);
  RunInBackground([j, out] {
    Progress("Packing the moves ...");
    std::vector<ZipEntry> files;
    if (!Build(j, files)) return;
    if (WriteFile(out, ZipWrite(files))) Status("Saved " + out + " (add it in the launcher's Mods tab with +).");
    else Status("The move pack could not be written: " + out);
  });
}

void Install() {
  const Job j = PrepareJob();
  RunInBackground([j] {
    Progress("Installing the moves ...");
    std::vector<ZipEntry> files;
    if (!Build(j, files)) return;
    const fs::path dir = fs::path(g_game) / L"Mods" / L"Moves" / fs::u8path(j.id);
    std::error_code ec;
    std::string why;
    if (!InstallModFiles(dir, files, &why)) {
      Status("Not installed: " + why);
      return;
    }
    Status("Installed: the game merges the moves into its files when it next starts (a few seconds) (" + PathStr(dir) + ")." + InstallReport("Moves", dir));
  });
}

// ---- movesets: a pack's move plays only from a wrestler's moveset (the match bank is gathered by profile)
struct ModSet {
  fs::path dir;        // Mods\Superstars\<folder>
  std::string name;    // its manifest name
  std::string moves;   // its moves file (manifest moves=, else moves.txt)
  std::map<int, int> slots;  // slot -> move id
};
std::vector<ModSet> g_modsets;
std::set<int> g_stock_used;  // move ids in the stock profiles
double g_sets_time = -100;

void LoadMovesets() {
  if (ImGui::GetTime() - g_sets_time < 3) return;  // (re-read every few seconds: other pages and tools write them)
  g_sets_time = ImGui::GetTime();
  if (g_stock_used.empty())
    for (const auto& s : Stars())
      for (int id : ProfileMoves(s.id)) g_stock_used.insert(id);
  g_modsets.clear();
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / L"Superstars", ec)) {
    Bytes man;
    if (!e.is_directory(ec) || !ReadFile(PathStr(e.path() / L"manifest.txt"), man)) continue;
    ModSet m;
    m.dir = e.path();
    const std::string t(man.begin(), man.end());
    m.name = Value(t, "name");
    m.moves = Value(t, "moves");
    if (m.moves.empty()) m.moves = "moves.txt";
    Bytes mv;
    if (ReadFile(PathStr(m.dir / fs::u8path(m.moves)), mv)) {
      const std::string s(mv.begin(), mv.end());
      for (size_t at = 0; at < s.size();) {
        size_t nl = s.find('\n', at);
        if (nl == std::string::npos) nl = s.size();
        const std::string line = s.substr(at, nl - at);
        at = nl + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const int off = int(std::strtol(line.c_str(), nullptr, 0)), id = std::atoi(line.c_str() + eq + 1);
        if (off >= 0 && off < 2 * kMoveSlots && !(off & 1) && id > 0) m.slots[off / 2] = id;
      }
    }
    g_modsets.push_back(std::move(m));
  }
}

int UsedBy(int id, std::string* who) {
  int n = g_stock_used.count(id) ? 1 : 0;
  if (n && who) *who = "a stock superstar";
  for (const auto& m : g_modsets)
    for (const auto& [slot, mv] : m.slots)
      if (mv == id) {
        if (who) *who += (who->empty() ? "" : ", ") + m.name;
        ++n;
        break;
      }
  return n;
}

// A move into a superstar mod's moves.txt (the line for that slot replaced), and its manifest's moves= if missing.
bool PutInModset(const ModSet& m, int slot, int id, std::string& err) {
  const fs::path f = m.dir / fs::u8path(m.moves);
  Bytes old;
  ReadFile(PathStr(f), old);
  std::string s(old.begin(), old.end()), out;
  char key[16];
  std::snprintf(key, sizeof key, "0x%03X", slot * 2);
  for (size_t at = 0; at < s.size();) {
    size_t nl = s.find('\n', at);
    if (nl == std::string::npos) nl = s.size();
    std::string line = s.substr(at, nl - at);
    at = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const size_t eq = line.find('=');
    if (eq != std::string::npos && std::strtol(line.c_str(), nullptr, 0) == slot * 2) continue;  // (that slot: replaced)
    out += line + "\r\n";
  }
  out += std::string(key) + "=" + std::to_string(id) + "\r\n";
  if (!WriteFile(PathStr(f), Bytes(out.begin(), out.end()))) return err = "could not write " + PathStr(f), false;
  Bytes man;
  if (ReadFile(PathStr(m.dir / L"manifest.txt"), man)) {
    std::string t(man.begin(), man.end());
    if (Value(t, "moves").empty()) {
      if (!t.empty() && t.back() != '\n') t += "\r\n";
      t += "moves=" + m.moves + "\r\n";
      if (!WriteFile(PathStr(m.dir / L"manifest.txt"), Bytes(t.begin(), t.end()))) return err = "could not write its manifest", false;
    }
  }
  g_sets_time = -100;
  return true;
}

// The stock move a pack move's record is copied from (wazecopy), else the move itself: what the slots judge.
int FitId(int id) {
  const auto c = g_pack.copies.find(id);
  return c != g_pack.copies.end() ? c->second : id;
}

int g_put_mod = -1, g_put_slot = -1;
std::wstring g_put_test;  // --moves-put <superstar mod folder>,<slot>,<move>: PutInModset on that folder, then quits

void RunPutTest() {
  const size_t c = g_put_test.find(L',');
  int slot = -1, id = 0;
  if (c != std::wstring::npos) swscanf_s(g_put_test.c_str() + c + 1, L"%d,%d", &slot, &id);
  ModSet m;
  m.dir = g_put_test.substr(0, c);
  Bytes man;
  if (ReadFile(PathStr(m.dir / L"manifest.txt"), man)) {
    const std::string t(man.begin(), man.end());
    m.name = Value(t, "name"), m.moves = Value(t, "moves");
  }
  if (m.moves.empty()) m.moves = "moves.txt";
  std::string err;
  Log(PutInModset(m, slot, id, err) ? "test: put move " + std::to_string(id) + " in slot " + std::to_string(slot) + " of " + m.name
                                    : "test: not put: " + err);
  WriteLogTo(g_put_test.substr(0, c) + L".put.log");
  g_put_test.clear();
  PostMessageW(g_wnd, WM_CLOSE, 0, 0);
}

void PutPanel() {
  if (g_sel < 0 || (!g_pack.waze.count(g_sel) && !g_pack.copies.count(g_sel))) return;
  ImGui::Separator();
  const auto nm = g_pack.names.find(g_sel);
  ImGui::Text("Put move %d%s in a superstar mod's moveset", g_sel, nm != g_pack.names.end() ? (" (" + nm->second + ")").c_str() : "");
  Hint("The game plays a move only from a wrestler's moveset: it gathers each one's moves by profile, so a move "
       "that is only in a pack is never found. Put it in an installed superstar mod's moveset here, or in your "
       "superstar project's Moveset section (the Superstar page lists installed packs' moves too).");
  if (g_modsets.empty()) {
    ImGui::TextDisabled("No superstar mods installed (Mods\\Superstars).");
    return;
  }
  ImGui::SetNextItemWidth(260 * g_scale);
  if (ImGui::BeginCombo("##putmod", g_put_mod >= 0 && g_put_mod < int(g_modsets.size()) ? g_modsets[size_t(g_put_mod)].name.c_str() : "a superstar mod")) {
    for (int i = 0; i < int(g_modsets.size()); ++i)
      if (ImGui::Selectable((g_modsets[size_t(i)].name + "##m" + std::to_string(i)).c_str(), g_put_mod == i)) g_put_mod = i;
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  const int fit = FitId(g_sel);
  ImGui::SetNextItemWidth(300 * g_scale);
  if (ImGui::BeginCombo("##putslot", g_put_slot >= 0 ? star_page::SlotName(g_put_slot).c_str() : "a slot it fits", ImGuiComboFlags_HeightLarge)) {
    int shown = 0;
    for (int s = 0; s < kMoveSlots; ++s)
      if (MoveFitsSlot(s, fit) && ImGui::Selectable((star_page::SlotName(s) + "##s" + std::to_string(s)).c_str(), g_put_slot == s)) g_put_slot = s, ++shown;
      else if (MoveFitsSlot(s, fit)) ++shown;
    if (!shown) ImGui::TextDisabled("(no slot takes the move it copies: %d)", fit);
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(g_put_mod < 0 || g_put_mod >= int(g_modsets.size()) || g_put_slot < 0);
  if (ImGui::Button("Put it in")) {
    const ModSet& m = g_modsets[size_t(g_put_mod)];
    std::string err;
    if (PutInModset(m, g_put_slot, g_sel, err))
      Status("Move " + std::to_string(g_sel) + " is now " + m.name + "'s " + star_page::SlotName(g_put_slot) + " (" + PathStr(m.dir / fs::u8path(m.moves)) + ").");
    else
      Status("Not put in: " + err + ".");
  }
  ImGui::EndDisabled();
  std::error_code ec;
  if (!fs::exists(fs::path(g_game) / L"Mods" / L"Moves" / fs::u8path(PackId()), ec))
    ImGui::TextColored(kWarn, "The pack isn't installed yet: Install into game too, or the move has no motions in matches.");
}

void Problems(std::vector<Problem>& p) {
  if (g_pack.folder.empty()) {
    p.push_back(Error("No moves yet: open a folder of moves to work on."));
    return;
  }
  for (const auto& e : g_pack.errors) p.push_back(Error(e));
  int missing = 0;
  for (const auto& m : g_pack.motions) missing += m.missing;
  if (missing) p.push_back(Error(std::to_string(missing) + " of the moves' animation files are missing from the folder: those moves wouldn't play."));
  if (!g_pack.name[0]) p.push_back(Warning("The moves have no name yet: they'd be called \"My Moves\".")
                                       .With("Call them \"My Moves\"", [] { std::snprintf(g_pack.name, sizeof g_pack.name, "My Moves"); Touch(); }));
  std::vector<int> no_bits;
  for (const auto& m : g_pack.motions)
    if (m.y == 0 && !g_pack.waze.count(m.id) && !g_pack.copies.count(m.id) && std::find(no_bits.begin(), no_bits.end(), m.id) == no_bits.end())
      no_bits.push_back(m.id);
  if (!no_bits.empty() && no_bits.size() < g_pack.motions.size())
    p.push_back(Warning(std::to_string(no_bits.size()) + " animation(s) aren't set up as moves, so no wrestler can be given them (fine for reactions)."));
  {  // moves no moveset uses: the game never plays them
    LoadMovesets();
    std::vector<int> unused;
    std::set<int> ids;
    for (const auto& w : g_pack.waze) ids.insert(w.first);
    for (const auto& c : g_pack.copies) ids.insert(c.first);
    for (int id : ids)
      if (!UsedBy(id, nullptr)) unused.push_back(id);
    if (!unused.empty()) {
      std::string list;
      for (size_t k = 0; k < unused.size() && k < 6; ++k) list += (k ? ", " : "") + std::to_string(unused[k]);
      p.push_back(Warning(std::to_string(unused.size()) + " move(s) are in no installed moveset (" + list + (unused.size() > 6 ? ", ..." : "") +
                              "): the game only plays moves from a wrestler's moveset.",
                          "Select the move and put it in a superstar mod's moveset below, or in a superstar project's Moveset section."));
    }
  }
  if (g_pack.unknown) p.push_back(Warning(std::to_string(g_pack.unknown) + " line(s) of the moves' list aren't understood: the game skips them."));
  if (g_pack.bytes > 16000000) p.push_back(Warning("These moves are big (" + Human(g_pack.bytes) + "): a match has room for about 16 MB of moves for everyone in it."));
}

void MoveList() {
  // moves = distinct ids, with their tracks
  std::map<int, std::vector<const Motion*>> by_id;
  for (const auto& m : g_pack.motions) by_id[m.id].push_back(&m);
  for (const auto& [id, bits] : g_pack.waze) by_id[id];
  if (ImGui::BeginTable("moves", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Move", ImGuiTableColumnFlags_WidthFixed, 70 * g_scale);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Tracks", ImGuiTableColumnFlags_WidthFixed, 150 * g_scale);
    ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, 70 * g_scale);
    ImGui::TableSetupColumn("Used by", ImGuiTableColumnFlags_WidthFixed, 150 * g_scale);
    ImGui::TableHeadersRow();
    for (const auto& [id, ms] : by_id) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      char label[32];
      std::snprintf(label, sizeof label, "%d", id);
      if (ImGui::Selectable(label, g_sel == id, ImGuiSelectableFlags_SpanAllColumns)) g_sel = id;
      ImGui::TableNextColumn();
      const auto nm = g_pack.names.find(id);
      if (nm != g_pack.names.end()) ImGui::TextUnformatted(nm->second.c_str());
      else if (const std::string& gn = MoveName(id); !gn.empty()) ImGui::TextUnformatted(gn.c_str());
      else if (const auto c = g_pack.copies.find(id); c != g_pack.copies.end()) ImGui::TextDisabled("as move %d (%s)", c->second, MoveName(c->second).c_str());
      else ImGui::TextDisabled("(new move: no name in the game's table)");
      ImGui::TableNextColumn();
      std::string tracks;
      int frames = 0;
      bool missing = false;
      std::vector<std::pair<int, int>> seen;  // (x, y) once, whichever banks carry it
      for (const Motion* m : ms) {
        if (m->y == 0) frames = std::max(frames, m->frames);
        missing |= m->missing;
        if (std::find(seen.begin(), seen.end(), std::make_pair(m->x, m->y)) != seen.end()) continue;
        seen.push_back({m->x, m->y});
        tracks += (tracks.empty() ? "" : " ") + std::string(m->y == 0 ? "A" : m->y == 1 ? "V" : std::to_string(m->y)) + (m->x ? "." + std::to_string(m->x) : "");
      }
      if (missing) ImGui::TextColored(kBad, "%s (file missing)", tracks.c_str());
      else ImGui::TextUnformatted(tracks.empty() ? "-" : tracks.c_str());
      if (ImGui::IsItemHovered() && !tracks.empty()) ImGui::SetTooltip("A = attacker, V = victim, numbers = props / cameras; .n = variant");
      ImGui::TableNextColumn();
      if (frames) ImGui::Text("%d", frames);
      ImGui::TableNextColumn();
      if (g_pack.waze.count(id) || g_pack.copies.count(id)) {
        std::string who;
        const int n = UsedBy(id, &who);
        if (n) ImGui::TextColored(kGood, "%d moveset%s", n, n == 1 ? "" : "s");
        else ImGui::TextColored(kWarn, "no moveset");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", n ? ("In the moveset of " + who + ".").c_str() : "No moveset has it: the game never plays it. Select it to put it in one.");
      } else {
        ImGui::TextDisabled("- (no record)");
      }
    }
    ImGui::EndTable();
  }
}

}  // namespace
void TestPut(const std::wstring& spec) { g_put_test = spec; }
namespace {

void Draw() {
  if (!g_put_test.empty()) RunPutTest();
  Heading("Move pack", "Moves the game doesn't have, in a move pack: motions for the attacker, the victim and the "
                   "cameras, plus the move's table records. Packs install on their own or ride inside a superstar mod.");
  std::vector<Problem> problems;
  Problems(problems);
  ImGui::BeginChild("moves_body", ImVec2(0, -48 * g_scale), false);
  if (ImGui::Button("Open a pack folder...", ImVec2(220 * g_scale, 0))) {
    const std::wstring f = PickFolder(L"The move pack folder (pack.txt + motions)");
    if (!f.empty()) OpenFolder(f);
  }
  Hint("A pack is a folder with pack.txt and motions/*.ymk. tools/svr10_moves.py ports moves from SvR 2010 and "
       "tools/movepack.py writes the folder. docs/MOVE_PACKS.md has the format.");
  ImGui::SameLine();
  if (g_pack.folder.empty()) ImGui::TextDisabled("none");
  else ImGui::TextUnformatted(Utf8(g_pack.folder).c_str());
  if (!g_pack.folder.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) ParsePack(g_pack);
  }
  TextField("Pack name", g_pack.name, sizeof g_pack.name, "My Moves");
  TextField("Author (optional)", g_pack.author, sizeof g_pack.author);
  TextField("Version (optional)", g_pack.version, sizeof g_pack.version, "1.0");
  if (g_pack.loaded) {
    ImGui::Separator();
    ImGui::Text("%zu motions (%s) for %zu moves; %zu move records (waze / wazecopy), %d exh, %d event and %d mbd records",
                g_pack.motions.size(), Human(g_pack.bytes).c_str(),
                [&] { std::set<int> ids; for (const auto& m : g_pack.motions) ids.insert(m.id); for (const auto& w : g_pack.waze) ids.insert(w.first);
                      for (const auto& c : g_pack.copies) ids.insert(c.first); return ids.size(); }(),
                RecordCount(g_pack), g_pack.exh, g_pack.evt, g_pack.mbd);
    ImGui::BeginChild("list", ImVec2(0, -(problems.empty() ? 0 : 30 * g_scale * float(std::min<size_t>(problems.size(), 4)))), false);
    MoveList();
    PutPanel();
    ImGui::EndChild();
  }
  DrawProblems(problems);
  ImGui::EndChild();
  switch (ModButtons(problems, false)) {
    case 1: SaveMod(); break;
    case 2: Install(); break;
  }
}

std::string StateText() {
  return std::string("name=") + g_pack.name + "\nauthor=" + g_pack.author + "\nversion=" + g_pack.version + "\nfolder=" +
         Utf8(g_pack.folder) + "\n";
}

void Reset() { g_pack = Pack(); g_sel = -1; }

void WriteProject(ProjectOut& out) {
  out.Key("name", g_pack.name);
  out.Key("author", g_pack.author);
  out.Key("version", g_pack.version);
  if (g_pack.folder.empty()) return;
  out.Key("pack", "pack");
  out.Key("pack.from", Utf8(g_pack.folder));
  PackFiles(g_pack.folder, out.files, "pack/");
}

bool Read(const ProjectIn& in, bool mod) {
  std::snprintf(g_pack.name, sizeof g_pack.name, "%s", in.Get("name").c_str());
  std::snprintf(g_pack.author, sizeof g_pack.author, "%s", in.Get("author").c_str());
  std::snprintf(g_pack.version, sizeof g_pack.version, "%s", in.Get("version").c_str());
  if (!g_pack.version[0]) std::snprintf(g_pack.version, sizeof g_pack.version, "1.0");
  const std::string from = in.Get("pack.from");
  std::wstring folder;
  if (!mod && !from.empty() && fs::exists(fs::path(Wide(from)) / L"pack.txt")) {
    folder = Wide(from);
  } else {
    const std::string prefix = mod ? "" : "pack/";
    for (const auto& e : in.files) {
      if (e.name == "manifest.txt" || e.name == "project.txt") continue;
      if (!prefix.empty() && e.name.rfind(prefix, 0) != 0) continue;
      const std::wstring f = in.Extract(e.name);
      if (e.name == prefix + "pack.txt") folder = fs::path(f).parent_path().wstring();
    }
  }
  if (folder.empty()) {
    Log("The pack has no pack.txt.");
    return true;
  }
  g_pack.folder = folder;
  ParsePack(g_pack);
  return true;
}

}  // namespace

void TestOpen(const std::wstring& folder) { OpenFolder(folder); }
void OpenPack(const std::wstring& folder) { OpenFolder(folder); }

// test aid: --test-moves-save <file>: the pack as a mod, now (after --moves-pack)
void TestSave(const std::wstring& file) {
  const Job j = PrepareJob();
  std::vector<ZipEntry> files;
  if (Build(j, files) && WriteFile(Utf8(file), ZipWrite(files))) Log("test: saved " + Utf8(file));
  else Log("test: moves build failed");
}

PageHooks hooks = {
    "moves", Draw, Problems, Reset, StateText, WriteProject,
    [](const ProjectIn& in) { return Read(in, false); },
    [](const ProjectIn& in) { return Read(in, true); },
};

}  // namespace moves_page
}  // namespace mm
