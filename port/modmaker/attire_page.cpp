// Attires page (checklist D6): a new or replaced attire for a superstar the
// game has (or a superstar mod), as a mod of its own - type=attire, installed
// into <game>/Mods/Attires/<id> (format agreed with Superstar Porting, who
// builds the game side): superstar=<id>, attire=<1-4>|new, name=, model=
// attire.pac (a ch pac; its first attire is used), entrance= (optional).
// A superstar has 4 attires at most; the renders stay the superstar's.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app.h"
#include "char_preview.h"
#include "pac_attires.h"
#include "svrfmt/pac.h"

namespace mm {
namespace attire_page {

using namespace svrfmt;

namespace {

struct AttireProject {
  int star = -1;          // 2011 id
  int slot = 0;           // 1-4: replace that attire; 0: a new one
  char name[32] = "", author[64] = "", version[16] = "1.0";
  std::wstring model;     // a ch pac
  int from = 1;           // which of its attires (attire.pac carries only that one, as attire 1)
  int entrance = -1;      // entrance= (-1: the superstar's)
};
// The editor of the attires inside a pac (a copy: saved elsewhere, the game's pac folder never written)
struct {
  std::wstring file, src;
  Epac pac, src_pac;
  bool loaded = false, src_loaded = false, dirty = false;
  int src_attire = 1, target = 0;  // target 0: a new attire
} g_ed;
std::wstring g_ed_test;  // --pac-attires <pac>,<op>[:...],...: edits, saves, quits
void Editor();
void RunEditorTest();
void Editor();
void RunEditorTest();
AttireProject g_at;
std::wstring g_test_save;
struct {
  int star = -1;
  std::wstring model;
  int slot = 0;
  bool pending = false;
} g_test;

fs::path StockPac(int id) { return fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(id) + L".pac"); }

// The superstar's attires now: its model pac's EMD entries "%06d%02d" (attire * 10 + kind; kind 2 = a model).
std::vector<int> CurrentAttires(int id) {
  static std::map<int, std::vector<int>> cache;
  if (cache.count(id)) return cache[id];
  std::vector<int> out;
  PacIndex idx;
  if (ReadPacIndex(StockPac(id), idx))
    for (const auto& e : idx.entries)
      if (e.group == "EMD " && e.name.size() == 8 && e.name[7] == '2') {
        const int a = (e.name[6] - '0') + 1;
        if (a >= 1 && a <= 4 && std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
      }
  std::sort(out.begin(), out.end());
  return cache[id] = out;
}

std::string ModId() {
  const StarInfo* s = StarById(g_at.star);
  return IdFrom(std::string(g_at.name[0] ? g_at.name : "attire") + "_" + (s ? s->name : std::to_string(g_at.star)), "attire");
}

bool Build(std::vector<ZipEntry>& files) {
  Bytes pac;
  if (g_at.star < 0 || g_at.model.empty() || !ReadFile(Utf8(g_at.model), pac) || pac.size() < 0x4000 || std::memcmp(pac.data(), "EPK8", 4)) {
    Status("Pick the superstar and a character model pac first.");
    return false;
  }
  if (g_at.from > 1) {  // (another attire of the pac: only it, as attire 1)
    Epac e;
    std::string err;
    if (!EpacRead(pac, e) || !PacOnlyAttire(e, g_at.from, &err)) {
      Status("Attire " + std::to_string(g_at.from) + " of the model could not be taken: " + err + ".");
      return false;
    }
    pac = EpacWrite(e);
  }
  std::string man = "type=attire\nid=" + ModId() + "\nsuperstar=" + std::to_string(g_at.star) +
                    "\nattire=" + (g_at.slot ? std::to_string(g_at.slot) : std::string("new")) + "\nname=" + g_at.name +
                    "\nmodel=attire.pac\nauthor=" + g_at.author + "\nversion=" + g_at.version + "\n" + MadeWith();
  if (g_at.entrance > 0) man += "entrance=" + std::to_string(g_at.entrance) + "\n";
  files.push_back({"manifest.txt", Bytes(man.begin(), man.end())});
  files.push_back({"attire.pac", std::move(pac)});
  return true;
}

void Save() {
  std::vector<ZipEntry> files;
  if (!Build(files)) return;
  const std::wstring f = PickFile(true, L"Save the attire mod", kModFilter, 1, L"svrmod", (Wide(ModId()) + L".svrmod").c_str());
  if (f.empty()) return;
  Status(WriteFile(Utf8(f), ZipWrite(files)) ? "Saved " + Utf8(f) + " (add it in the launcher's Mods tab with +)." : "The mod could not be written.");
}

void Install() {
  std::vector<ZipEntry> files;
  if (!Build(files)) return;
  const fs::path dir = fs::path(g_game) / L"Mods" / L"Attires" / fs::u8path(ModId());
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  for (const auto& f : files)
    if (!WriteFile(PathStr(dir / fs::u8path(f.name)), f.data)) {
      Status("Could not write into " + PathStr(dir) + " (is the game running?)");
      return;
    }
  Status("Installed into " + PathStr(dir) + ": the attire is on CHANGE ATTIRE from the next game start.");
}

// Other installed attire mods taking the same superstar and slot (the first A-Z wins).
std::string Conflict() {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / L"Attires", ec)) {
    if (!e.is_directory(ec) || PathStr(e.path().filename()) == ModId() || fs::exists(e.path() / L"disabled", ec)) continue;
    Bytes t;
    if (!ReadFile(PathStr(e.path() / L"manifest.txt"), t)) continue;
    const std::string s(t.begin(), t.end());
    if (Value(s, "superstar") == std::to_string(g_at.star) && g_at.slot && Value(s, "attire") == std::to_string(g_at.slot))
      return PathStr(e.path().filename());
  }
  return "";
}

void Problems(std::vector<Problem>& p) {
  if (g_at.star < 0) p.push_back(Error("No superstar yet.", "Pick whose attire this is."));
  if (g_at.model.empty()) p.push_back(Error("No model yet.", "A character model pac (ch.pac): its first attire becomes this attire."));
  else {
    Bytes head;
    std::error_code ec;
    const bool exists = fs::exists(g_at.model, ec);
    if (FILE* f = exists ? _wfopen(g_at.model.c_str(), L"rb") : nullptr) {
      head.resize(4);
      if (std::fread(head.data(), 1, 4, f) != 4) head.clear();
      std::fclose(f);
    }
    if (!exists) p.push_back(Error("The model file is missing: " + FileName(g_at.model)));
    else if (head.size() < 4 || std::memcmp(head.data(), "EPK8", 4))
      p.push_back(Error("That isn't a character model pac (EPK8)" + std::string(head.size() == 4 && !std::memcmp(head.data(), "EPAC", 4) ? ": an EPAC is an arena or menu pac." : ".")));
    else {
      BudgetProblems(g_at.model, "This attire", true, p);
      if (star_page::EntranceOutfit(g_at.model))
        p.push_back(Warning("The model carries an entrance outfit (a coat or the like): it is worn during this attire's entrance."));
    }
  }
  if (g_at.star >= 0) {
    const auto now = CurrentAttires(g_at.star);
    if (!g_at.slot && now.size() >= 4)
      p.push_back(Error("The superstar already has 4 attires - the game's limit.", "Replace one of them instead."));
    if (g_at.slot && std::find(now.begin(), now.end(), g_at.slot) == now.end())
      p.push_back(Warning("The superstar has no attire " + std::to_string(g_at.slot) + " now: this adds it as attire " + std::to_string(g_at.slot) + "."));
    if (const std::string c = Conflict(); !c.empty())
      p.push_back(Warning("The installed attire mod " + c + " takes the same attire: the first by name (A-Z) wins."));
  }
  if (!g_at.name[0]) p.push_back(Warning("No name: CHANGE ATTIRE shows ATTIRE N."));
}

void Draw() {
  if (!g_ed_test.empty()) RunEditorTest();
  if (g_test.pending) {
    g_test.pending = false;
    g_at.star = g_test.star, g_at.model = g_test.model, g_at.slot = g_test.slot;
    std::snprintf(g_at.name, sizeof g_at.name, "TEST GEAR");
  }
  if (!g_test_save.empty() && g_at.star >= 0 && !g_at.model.empty()) {
    std::vector<ZipEntry> files;
    if (Build(files) && WriteFile(Utf8(g_test_save), ZipWrite(files))) Log("test: saved " + Utf8(g_test_save));
    g_test_save.clear();
  }
  Heading("Attires", "A new attire for a superstar the game has (or a superstar mod), or one replacing an attire: "
                     "its model, name and entrance. It shows on CHANGE ATTIRE; the superstar's pictures stay.");
  std::vector<Problem> problems;
  Problems(problems);
  ImGui::BeginChild("attire_body", ImVec2(ImGui::GetContentRegionAvail().x * 0.58f, -48 * g_scale), false);
  const auto& stars = Stars();
  const StarInfo* who = StarById(g_at.star);
  ImGui::SetNextItemWidth(320 * g_scale);
  if (ImGui::BeginCombo("Superstar", who ? who->name.c_str() : g_at.star >= 0 ? std::to_string(g_at.star).c_str() : "(pick one)")) {
    for (const auto& s : stars)
      if (ImGui::Selectable((s.name + "##" + std::to_string(s.id)).c_str(), g_at.star == s.id)) g_at.star = s.id, g_at.slot = 0, Touch();
    ImGui::EndCombo();
  }
  if (g_at.star >= 0) {
    const auto now = CurrentAttires(g_at.star);
    std::string list;
    for (int a : now) list += (list.empty() ? "" : ", ") + std::to_string(a);
    ImGui::TextDisabled("Attires in the model pac: %s (of 4; some may be locked in the game)", list.empty() ? "none found" : list.c_str());
    if (ImGui::RadioButton("A new attire", g_at.slot == 0)) g_at.slot = 0, Touch();
    for (int a = 1; a <= 4; ++a) {
      ImGui::SameLine();
      const bool exists = std::find(now.begin(), now.end(), a) != now.end();
      if (ImGui::RadioButton(((exists ? "Replace " : "Attire ") + std::to_string(a) + (exists ? "" : " (free?)")).c_str(), g_at.slot == a)) g_at.slot = a, Touch();
      if (!exists && ImGui::IsItemHovered())
        ImGui::SetTooltip("No model for it in the superstar's pac: free if the game allows (its attire table decides how many a superstar has).");
    }
  }
  if (FileRow("Model (ch.pac)...", g_at.model, "none", kPacFilter, 1, "A character model pac - one of the game's, a superstar mod's or a "
              "converted one (Port page): the attire picked below becomes this attire."))
    g_at.from = 1;
  if (!g_at.model.empty()) {  // which of the pac's attires
    const std::vector<int> own = ModelAttires(g_at.model);
    if (own.size() > 1) {
      ImGui::TextDisabled("Its attire");
      for (int a : own) {
        ImGui::SameLine();
        if (ImGui::RadioButton((std::to_string(a) + "##from").c_str(), g_at.from == a)) g_at.from = a, Touch();
      }
      Hint("The model pac holds several attires: the one picked becomes this attire (the mod carries only it).");
    }
  }
  TextField("Name on CHANGE ATTIRE", g_at.name, sizeof g_at.name, "e.g. 2003 GEAR");
  if (star_page::PickEntrance("Entrance (optional)", g_at.entrance, "the superstar's")) Touch();
  TextField("Author (optional)", g_at.author, sizeof g_at.author);
  TextField("Version (optional)", g_at.version, sizeof g_at.version, "1.0");
  ImGui::Spacing();
  DrawProblems(problems);
  ImGui::Spacing();
  if (ImGui::CollapsingHeader("Attires inside a model pac")) Editor();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginGroup();
  char_preview::SetAttire(g_at.from);
  char_preview::SetModel(g_at.model, g_game);
  char_preview::SetHeight(1.0f);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  char_preview::Draw(avail.x, std::max(200.0f, avail.y - 96 * g_scale));
  ViewFooter();
  if (!char_preview::Status().empty()) ImGui::TextDisabled("%s", char_preview::Status().c_str());
  ImGui::EndGroup();
  switch (ModButtons(problems, false)) {
    case 1: Save(); break;
    case 2: Install(); break;
  }
}

// ---- the editor of a pac's attires
bool LoadPac(const std::wstring& f, Epac& out) {
  Bytes d;
  return ReadFile(Utf8(f), d) && d.size() >= 0x4000 && !std::memcmp(d.data(), "EPK8", 4) && EpacRead(d, out);
}

bool InGamePac(const std::wstring& f) {  // (the game's own pac folder is never written)
  std::error_code ec;
  const fs::path a = fs::weakly_canonical(fs::path(f), ec), b = fs::weakly_canonical(fs::path(g_game) / L"pac", ec);
  const std::string as = Lower(Utf8(a.wstring())), bs = Lower(Utf8(b.wstring()));
  return !bs.empty() && as.rfind(bs, 0) == 0;
}

std::string KindsText(const Epac& e, int a) {
  std::string s;
  for (int k : PacAttireKinds(e, a)) s += (s.empty() ? "" : ", ") + std::string(k == 2 ? "model" : k == 4 ? "entrance outfit" : "kind " + std::to_string(k));
  return s;
}

void Editor() {
  ImGui::TextWrapped("Several attires can live in one model pac (the game uses a superstar mod's first 4). Open a pac, "
                     "take attires in from other pacs or remove them, then save it as a copy - for a superstar mod's model, "
                     "or as the model of an attire mod above.");
  if (FileRow("Pac to edit...", g_ed.file, "none", kPacFilter, 1, "A character model pac. It is read; the edits go into a copy you save."))
    g_ed.loaded = !g_ed.file.empty() && LoadPac(g_ed.file, g_ed.pac), g_ed.dirty = false, g_ed.target = 0;
  if (g_ed.file.empty()) return;
  if (!g_ed.loaded) {
    ImGui::TextColored(kBad, "That isn't a character model pac (EPK8).");
    return;
  }
  const std::vector<int> have = PacAttires(g_ed.pac);
  if (ImGui::BeginTable("pac_attires", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
    for (int a : have) {
      ImGui::PushID(a);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("Attire %d%s", a, a > 4 ? " (unused: over 4)" : "");
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", KindsText(g_ed.pac, a).c_str());
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("Remove")) {
        std::string err;
        if (PacRemoveAttire(g_ed.pac, a, &err)) g_ed.dirty = true, Status("Attire " + std::to_string(a) + " removed (the later ones moved down).");
        else Status("Not removed: " + err + ".");
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  ImGui::TextUnformatted("Take an attire in");
  if (FileRow("From pac...", g_ed.src, "none", kPacFilter, 1, "Another character model pac: one of its attires is copied in."))
    g_ed.src_loaded = !g_ed.src.empty() && LoadPac(g_ed.src, g_ed.src_pac), g_ed.src_attire = 1;
  if (g_ed.src_loaded) {
    const std::vector<int> theirs = PacAttires(g_ed.src_pac);
    ImGui::TextDisabled("its attire");
    for (int a : theirs) {
      ImGui::SameLine();
      if (ImGui::RadioButton((std::to_string(a) + "##src").c_str(), g_ed.src_attire == a)) g_ed.src_attire = a;
    }
    ImGui::TextDisabled("as");
    ImGui::SameLine();
    if (ImGui::RadioButton("a new attire", g_ed.target == 0)) g_ed.target = 0;
    for (int a : have) {
      ImGui::SameLine();
      if (ImGui::RadioButton(("attire " + std::to_string(a) + " (replace)##t").c_str(), g_ed.target == a)) g_ed.target = a;
    }
    const int to = g_ed.target ? g_ed.target : (have.empty() ? 1 : have.back() + 1);
    ImGui::BeginDisabled(to > 4 && !g_ed.target);
    if (ImGui::Button(("Copy it in as attire " + std::to_string(to)).c_str())) {
      std::string err;
      if (PacCopyAttire(g_ed.pac, to, g_ed.src_pac, g_ed.src_attire, &err)) g_ed.dirty = true, Status("Attire " + std::to_string(to) + " taken from " + FileName(g_ed.src) + ".");
      else Status("Not copied: " + err + ".");
    }
    ImGui::EndDisabled();
    if (to > 4 && !g_ed.target) ImGui::TextDisabled("(4 attires already: the game uses no more)");
  }
  ImGui::Spacing();
  ImGui::BeginDisabled(!g_ed.dirty);
  PushAccent();
  if (ImGui::Button("Save the pac as...", ImVec2(220 * g_scale, 0))) {
    const std::wstring f = PickFile(true, L"Save the edited model pac (a copy)", kPacFilter, 1, L"pac", fs::path(g_ed.file).filename().c_str());
    if (!f.empty() && InGamePac(f)) Status("Not into the game's pac folder: save the copy elsewhere (a superstar mod carries it).");
    else if (!f.empty()) {
      const bool ok = WriteFile(Utf8(f), EpacWrite(g_ed.pac));
      Status(ok ? "Saved " + Utf8(f) + "." : "Could not write " + Utf8(f) + ".");
      if (ok) g_ed.file = f, g_ed.dirty = false;
    }
  }
  PopAccent();
  ImGui::EndDisabled();
  if (g_ed.dirty) {
    ImGui::SameLine();
    ImGui::TextColored(kWarn, "edited, not saved");
  }
}

// --pac-attires <pac>,<op>,...: copy:<to>:<src pac>:<from> | remove:<a> | save:<out>; quits after
void RunEditorTest() {
  std::vector<std::wstring> parts;
  for (size_t at = 0; at <= g_ed_test.size();) {
    size_t c = g_ed_test.find(L',', at);
    if (c == std::wstring::npos) c = g_ed_test.size();
    parts.push_back(g_ed_test.substr(at, c - at));
    at = c + 1;
  }
  g_ed_test.clear();
  Epac pac;
  if (parts.empty() || !LoadPac(parts[0], pac)) { Log("test: pac-attires: can't read the pac"); PostMessageW(g_wnd, WM_CLOSE, 0, 0); return; }
  for (size_t i = 1; i < parts.size(); ++i) {
    const std::wstring& op = parts[i];
    std::string err;
    bool ok = true;
    if (op.rfind(L"remove:", 0) == 0) ok = PacRemoveAttire(pac, _wtoi(op.c_str() + 7), &err);
    else if (op.rfind(L"save:", 0) == 0) ok = !InGamePac(op.substr(5)) && WriteFile(Utf8(op.substr(5)), EpacWrite(pac));
    else if (op.rfind(L"copy:", 0) == 0) {  // copy:<to>:<src>:<from>  (the src path holds a ':' after the drive)
      const std::wstring rest = op.substr(5);
      const size_t a = rest.find(L':'), b = rest.rfind(L':');
      Epac src;
      ok = a != b && LoadPac(rest.substr(a + 1, b - a - 1), src) && PacCopyAttire(pac, _wtoi(rest.c_str()), src, _wtoi(rest.c_str() + b + 1), &err);
    }
    std::string list;
    for (int x : PacAttires(pac)) list += " " + std::to_string(x);
    Log("test: pac-attires " + Utf8(op) + (ok ? " ok" : " FAILED " + err) + "; attires:" + list);
  }
  PostMessageW(g_wnd, WM_CLOSE, 0, 0);
}

std::string StateText() {
  return "star=" + std::to_string(g_at.star) + "\nslot=" + std::to_string(g_at.slot) + "\nname=" + g_at.name +
         "\nmodel=" + Utf8(g_at.model) + "\nentrance=" + std::to_string(g_at.entrance) + "\nauthor=" + g_at.author +
         "\nversion=" + g_at.version;
}

void Reset() { g_at = AttireProject(); }

void WriteProject(ProjectOut& out) {
  out.Key("superstar", g_at.star);
  out.Key("attire", g_at.slot ? std::to_string(g_at.slot) : std::string("new"));
  out.Key("name", g_at.name);
  out.Key("author", g_at.author);
  out.Key("version", g_at.version);
  if (g_at.entrance > 0) out.Key("entrance", g_at.entrance);
  if (g_at.from > 1) out.Key("from_attire", g_at.from);
  out.File("model", g_at.model, "attire/model");
}

bool Read(const ProjectIn& in, bool mod) {
  g_at.star = in.GetInt("superstar", -1);
  const std::string a = in.Get("attire");
  g_at.slot = a == "new" || a.empty() ? 0 : std::clamp(std::atoi(a.c_str()), 1, 4);
  std::snprintf(g_at.name, sizeof g_at.name, "%s", in.Get("name").c_str());
  std::snprintf(g_at.author, sizeof g_at.author, "%s", in.Get("author").c_str());
  std::snprintf(g_at.version, sizeof g_at.version, "%s", in.Get("version").c_str());
  if (!g_at.version[0]) std::snprintf(g_at.version, sizeof g_at.version, "1.0");
  g_at.entrance = in.GetInt("entrance", -1);
  g_at.from = mod ? 1 : std::clamp(in.GetInt("from_attire", 1), 1, 9);  // (a mod's attire.pac holds only its attire)
  if (mod) {
    const std::string m = in.Get("model").empty() ? "attire.pac" : in.Get("model");
    if (in.Find(m)) g_at.model = in.Extract(m);
  } else {
    const std::string from = in.Get("model.from");
    g_at.model = !from.empty() && fs::exists(Wide(from)) ? Wide(from) : in.Extract(in.Get("model"));
  }
  return true;
}

}  // namespace

void TestPacAttires(const std::wstring& spec) { g_ed_test = spec; }

void TestStart(int star, const std::wstring& model, int slot, const std::wstring& save) {
  g_test = {star, model, slot, true};  // (applied on the page's first frame: the projects are reset after the command line)
  g_test_save = save;
}

PageHooks hooks = {
    "attire", Draw, Problems, Reset, StateText, WriteProject,
    [](const ProjectIn& in) { return Read(in, false); },
    [](const ProjectIn& in) { return Read(in, true); },
};

}  // namespace attire_page
}  // namespace mm
