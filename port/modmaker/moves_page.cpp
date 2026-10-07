// Moves page: a move pack - moves the game doesn't have (the game:
// move_packs.cpp, docs/MOVE_PACKS.md). A pack is a folder with pack.txt and
// motions/: tools/svr10_moves.py + movepack.py make one from SvR 2010 data.
// Here it is checked, shown move by move, and saved as a .svrmod
// (type=moves, installed into <game>/Mods/Moves/<id>) or handed to a
// superstar mod (its moves/ folder).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "app.h"

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

void OpenFolder(const std::wstring& f) {
  g_pack.folder = f;
  ParsePack(g_pack);
  if (!g_pack.name[0]) std::snprintf(g_pack.name, sizeof g_pack.name, "%s", FileName(f).c_str());
  g_sel = -1;
  Touch();
  Status(g_pack.loaded ? "Pack: " + std::to_string(g_pack.motions.size()) + " motions, " + std::to_string(g_pack.waze.size()) + " moves"
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
  std::string id, manifest;
  std::wstring folder;
};
Job PrepareJob() {
  if (!g_pack.name[0]) std::snprintf(g_pack.name, sizeof g_pack.name, "My Moves");
  Job j;
  j.id = PackId();
  j.manifest = "type=moves\nid=" + j.id + "\nname=" + g_pack.name + "\nauthor=" + g_pack.author + "\nversion=" +
               g_pack.version + "\n" + MadeWith();
  j.folder = g_pack.folder;
  return j;
}

bool Build(const Job& j, std::vector<ZipEntry>& files) {
  files.push_back({"manifest.txt", Bytes(j.manifest.begin(), j.manifest.end())});
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
    fs::remove_all(dir, ec);
    for (const auto& f : files) {
      fs::create_directories((dir / fs::u8path(f.name)).parent_path(), ec);
      if (!WriteFile(PathStr(dir / fs::u8path(f.name)), f.data)) {
        Status("Could not write into " + PathStr(dir) + " (is the game running?)");
        return;
      }
    }
    Status("Installed: the game merges the moves into its files when it next starts (a few seconds) (" + PathStr(dir) + ").");
  });
}

void Problems(std::vector<Problem>& p) {
  if (g_pack.folder.empty()) {
    p.push_back(Error("No move pack yet.", "Open a pack folder (pack.txt + motions/), e.g. one made by tools/movepack.py."));
    return;
  }
  for (const auto& e : g_pack.errors) p.push_back(Error(e));
  int missing = 0;
  for (const auto& m : g_pack.motions) missing += m.missing;
  if (missing) p.push_back(Error(std::to_string(missing) + " motion file(s) named in pack.txt are missing from the folder."));
  if (!g_pack.name[0]) p.push_back(Warning("No pack name: \"My Moves\" is used."));
  std::vector<int> no_bits;
  for (const auto& m : g_pack.motions)
    if (m.y == 0 && !g_pack.waze.count(m.id) && !g_pack.copies.count(m.id) && std::find(no_bits.begin(), no_bits.end(), m.id) == no_bits.end())
      no_bits.push_back(m.id);
  if (!no_bits.empty() && no_bits.size() < g_pack.motions.size())
    p.push_back(Warning(std::to_string(no_bits.size()) + " move(s) have motions but no waze line: they can't be put in a move-set (fine for reactions and extra tracks)."));
  if (g_pack.unknown) p.push_back(Warning(std::to_string(g_pack.unknown) + " line(s) of pack.txt aren't understood (the game skips them)."));
  if (g_pack.bytes > 16000000) p.push_back(Warning("A big pack (" + Human(g_pack.bytes) + "): the game's match banks hold about 16 MB of motions for everyone."));
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
    ImGui::TableSetupColumn("In move-sets", ImGuiTableColumnFlags_WidthFixed, 110 * g_scale);
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
      if (g_pack.waze.count(id) || g_pack.copies.count(id)) ImGui::TextColored(kGood, "yes");
      else ImGui::TextDisabled("no");
    }
    ImGui::EndTable();
  }
}

void Draw() {
  Heading("Moves", "Moves the game doesn't have, in a move pack: motions for the attacker, the victim and the "
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
    ImGui::Text("%zu motions (%s) for %zu moves; %zu waze, %d exh, %d event and %d mbd records", g_pack.motions.size(),
                Human(g_pack.bytes).c_str(), [&] { std::map<int, int> ids; for (const auto& m : g_pack.motions) ids[m.id]++; for (const auto& w : g_pack.waze) ids[w.first]++; return ids.size(); }(),
                g_pack.waze.size(), g_pack.exh, g_pack.evt, g_pack.mbd);
    ImGui::BeginChild("list", ImVec2(0, -(problems.empty() ? 0 : 30 * g_scale * float(std::min<size_t>(problems.size(), 4)))), false);
    MoveList();
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
