// Animations page: a character playing the game's motions. The menu banks
// in m.pac MVMT (YMBs: stances, taunts, finisher previews...) play here, on
// the superstar and, for two-person motions, a dummy; the match moves (YMKs
// banks) only the game plays - "Play in game" starts a test match with a
// dummy and forces the move (SVR2011_TEST_MATCH + SVR2011_TEST_FORCE_MOVE).
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app.h"
#include "char_preview.h"
#include "svrfmt/pac.h"

namespace mm {
namespace anims_page {

using namespace svrfmt;

namespace {

// The menu banks: m.pac MVMT entries and their PACH children that are YMBs
// (decodable here). Names: what the game uses them for.
struct BankDesc {
  const char* entry;
  int child;
  const char* name;
  const char* about;
};
const BankDesc kBanks[] = {
    {"STAT", 3, "Stances", "The ring stances (the idle 20000 and the others the move-set offers)."},
    {"BASE", 6, "Taunts", "Taunts and poses (9000s)."},
    {"ANAS", 23, "Entrances (ring)", "Motions of the entrance's ring part (21000s), both people."},
    {"FINI", 24, "Finishers A", "Finisher previews, part 1."},
    {"FINI", 25, "Finishers B", "Finisher previews, part 2."},
    {"FINI", 26, "Finishers C", "Finisher previews, part 3."},
    {"FINI", 27, "Finishers D", "Finisher previews, part 4 (30000s)."},
    {"ABIL", 32, "Abilities", "The abilities' preview motions."},
};

struct Entry {
  int id, x, y, frames;
};
struct Bank {
  std::shared_ptr<const Bytes> data;
  std::vector<Entry> entries;
  std::string error;
};

int g_star = -1;       // index in Stars(), the model (-1: a file)
std::wstring g_model;  // its ch.pac
int g_dummy = -1;      // index in Stars() (-1: none)
int g_bank = -1;       // index in kBanks
std::map<int, Bank> g_banks;
int g_sel_id = -1, g_sel_x = 0;
char g_filter[64] = "";
bool g_loading = false;
int g_test_star = 0, g_test_bank = -1, g_test_motion = -1, g_test_dummy = 0;  // --anims <star>,<bank>,<motion>[,<dummy>]

std::wstring ChPac(int id) { return (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(id) + L".pac")).wstring(); }

// A bank read from m.pac in the background.
void LoadBank(int which) {
  if (g_banks.count(which) || g_loading) return;
  g_loading = true;
  const BankDesc d = kBanks[which];
  RunInBackground([which, d] {
    Progress(std::string("Reading the ") + d.name + " bank ...");
    Bank b;
    Bytes entry;
    if (!ReadPacEntry(fs::path(g_game) / L"pac" / L"m.pac", d.entry, entry)) {
      b.error = std::string("pac/m.pac has no MVMT/") + d.entry;
    } else {
      std::vector<PachEntry> kids;
      if (!PachRead(Unpack(entry), kids)) b.error = "the bank list is not a PACH";
      for (const auto& k : kids) {
        if (int(k.id) != d.child) continue;
        auto raw = std::make_shared<Bytes>(Unpack(k.data));
        if (raw->size() < 0x114 || std::memcmp(raw->data(), "YMBs", 4)) { b.error = "the bank is not YMBs"; break; }
        const uint32_t n = Le32(&(*raw)[0x110]);
        for (uint32_t i = 0; i < n && 0x114 + 16 * (i + 1) <= raw->size(); ++i) {
          const uint8_t* e = &(*raw)[0x114 + 16 * i];
          b.entries.push_back({int(Le16(e + 2)), e[1], e[0], int(Le32(e + 8))});
        }
        b.data = raw;
      }
      if (!b.data && b.error.empty()) b.error = "the bank isn't in the list";
    }
    OnUiThread([which, b] {
      g_banks[which] = b;
      g_loading = false;
    });
  });
}

std::string MotionLabel(int id) {
  const std::string& n = MoveName(id);
  if (!n.empty()) return n;
  if (id >= 20000 && id < 21000) return id == 20000 ? "Idle stance" : "Stance " + std::to_string(id - 20000);
  if (id >= 21000 && id < 22000) return "Entrance motion " + std::to_string(id - 21000);
  if (id >= 9000 && id < 10000) return "Taunt " + std::to_string(id - 9000);
  return "Motion " + std::to_string(id);
}

void Pick(int id, int x) {
  g_sel_id = id, g_sel_x = x;
  const auto it = g_banks.find(g_bank);
  if (it == g_banks.end() || !it->second.data) return;
  std::string err;
  if (!char_preview::SetMotion(it->second.data, id, x, &err)) Status("That motion can't be shown: " + err + ".");
  else Status(MotionLabel(id) + (x ? " (variant " + std::to_string(x) + ")" : ""));
}

void Controls() {
  char_preview::Playback& p = char_preview::Play();
  if (ImGui::Button(p.playing ? "Pause" : "Play", ImVec2(70 * g_scale, 0))) p.playing = !p.playing;
  ImGui::SameLine();
  if (ImGui::Button("|<", ImVec2(34 * g_scale, 0))) p.key = 0, p.playing = false;
  ImGui::SameLine();
  if (ImGui::Button("<", ImVec2(34 * g_scale, 0))) p.key = std::max(0.0f, std::floor(p.key) - 1), p.playing = false;
  ImGui::SameLine();
  if (ImGui::Button(">", ImVec2(34 * g_scale, 0))) p.key = std::min(float(std::max(0, p.keys - 1)), std::floor(p.key) + 1), p.playing = false;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x - 330 * g_scale));
  float key = p.key;
  char fmt[32];
  std::snprintf(fmt, sizeof fmt, "key %%.0f / %d", p.keys);
  if (ImGui::SliderFloat("##key", &key, 0, float(std::max(0, p.keys - 1)), fmt)) p.key = key, p.playing = false;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110 * g_scale);
  ImGui::SliderFloat("##speed", &p.speed, 0.1f, 2.0f, "x%.1f");
  ImGui::SameLine();
  ImGui::Checkbox("Loop", &p.loop);
  ImGui::SameLine();
  if (ImGui::Button("Camera")) char_preview::ResetCamera();
  ImGui::SameLine();
  ImGui::TextDisabled("%s", p.keys ? (std::to_string(p.keys / 30.0f).substr(0, 4) + " s at 30 keys/s").c_str() : "");
}

void Left() {
  const auto& stars = Stars();
  ImGui::TextUnformatted("Who");
  ImGui::SetNextItemWidth(-1);
  const std::string cur = g_star >= 0 ? stars[g_star].name : g_model.empty() ? "(pick a superstar)" : FileName(g_model);
  if (ImGui::BeginCombo("##who", cur.c_str())) {
    for (int i = 0; i < int(stars.size()); ++i)
      if (ImGui::Selectable((stars[i].name + "##s" + std::to_string(i)).c_str(), g_star == i)) {
        g_star = i;
        g_model = ChPac(stars[i].id);
        char_preview::SetModel(g_model, g_game);
      }
    ImGui::EndCombo();
  }
  if (ImGui::Button("A model file (ch.pac)...", ImVec2(-1, 0))) {
    const std::wstring f = PickFile(false, L"A character model pac", kPacFilter, 1);
    if (!f.empty()) g_star = -1, g_model = f, char_preview::SetModel(g_model, g_game);
  }
  ImGui::Spacing();
  ImGui::TextUnformatted("Dummy (the other person)");
  Hint("Two-person motions - finishers, the entrance's ring part - have a track for the other person: it plays on the dummy.");
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##dummy", g_dummy >= 0 ? stars[g_dummy].name.c_str() : "none")) {
    if (ImGui::Selectable("none", g_dummy < 0)) g_dummy = -1, char_preview::SetDummy(L"");
    for (int i = 0; i < int(stars.size()); ++i)
      if (ImGui::Selectable((stars[i].name + "##d" + std::to_string(i)).c_str(), g_dummy == i)) {
        g_dummy = i;
        char_preview::SetDummy(ChPac(stars[i].id));
      }
    ImGui::EndCombo();
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Motions");
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##bank", g_bank >= 0 ? kBanks[g_bank].name : "(pick a set)")) {
    for (int i = 0; i < int(std::size(kBanks)); ++i) {
      if (ImGui::Selectable(kBanks[i].name, g_bank == i)) g_bank = i, g_sel_id = -1, LoadBank(i);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kBanks[i].about);
    }
    ImGui::EndCombo();
  }
  if (g_bank >= 0 && g_banks.count(g_bank) == 0) LoadBank(g_bank);
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##filter", "find by name or number", g_filter, sizeof g_filter);
  if (ImGui::SmallButton("Back to the idle")) g_sel_id = -1, char_preview::ClearMotion();
  const auto it = g_banks.find(g_bank);
  if (it == g_banks.end()) {
    if (g_bank >= 0) ImGui::TextDisabled("Loading ...");
    return;
  }
  if (!it->second.error.empty()) {
    ImGui::TextColored(kBad, "%s", it->second.error.c_str());
    return;
  }
  // one row per (id, x) with a track 0; the tracks it has
  struct Row { int id, x, frames; bool victim; };
  std::vector<Row> rows;
  for (const Entry& e : it->second.entries) {
    if (e.id == 32767 || e.y > 1) continue;
    auto r = std::find_if(rows.begin(), rows.end(), [&](const Row& x) { return x.id == e.id && x.x == e.x; });
    if (r == rows.end()) rows.push_back({e.id, e.x, e.y == 0 ? e.frames : 0, e.y == 1});
    else if (e.y == 1) r->victim = true;
    else r->frames = e.frames;
  }
  const std::string filter = Lower(g_filter);
  ImGui::BeginChild("list", ImVec2(0, 0), true);
  for (const Row& r : rows) {
    const std::string label = MotionLabel(r.id) + (r.x ? "  v" + std::to_string(r.x) : "");
    if (!filter.empty() && Lower(label).find(filter) == std::string::npos && std::to_string(r.id).find(filter) == std::string::npos) continue;
    const bool on = g_sel_id == r.id && g_sel_x == r.x;
    char line[160];
    std::snprintf(line, sizeof line, "%s%s##%d_%d", label.c_str(), r.victim ? "  (2)" : "", r.id, r.x);
    if (ImGui::Selectable(line, on)) Pick(r.id, r.x);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("motion %d x%d, %d frames%s", r.id, r.x, r.frames, r.victim ? ", with the other person's track" : "");
  }
  ImGui::EndChild();
}

}  // namespace

void TestStart(int star_id, int bank, int motion, int dummy_id) {
  g_test_star = star_id, g_test_bank = bank, g_test_motion = motion, g_test_dummy = dummy_id;
}

void Draw() {
  if (g_test_star > 0 && !g_game.empty()) {  // the test aid, once
    const auto& stars = Stars();
    for (int i = 0; i < int(stars.size()); ++i) {
      if (stars[i].id == g_test_star) g_star = i, g_model = ChPac(stars[i].id), char_preview::SetModel(g_model, g_game);
      if (stars[i].id == g_test_dummy) g_dummy = i, char_preview::SetDummy(ChPac(stars[i].id));
    }
    if (g_test_bank >= 0 && g_test_bank < int(std::size(kBanks))) g_bank = g_test_bank, LoadBank(g_bank);
    g_test_star = 0;
  }
  if (g_test_motion >= 0 && g_bank >= 0 && g_banks.count(g_bank) && !g_loading) {
    Pick(g_test_motion, 0);
    g_test_motion = -1;
  }
  Heading("Animations", "A superstar playing the game's motions: stances, taunts, finisher previews, the entrance's "
                        "ring part. Pick who, pick a set, click a motion.");
  if (g_game.empty()) {
    ImGui::TextDisabled("No game folder.");
    return;
  }
  ImGui::BeginChild("left", ImVec2(300 * g_scale, 0), true);
  Left();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("view", ImVec2(0, 0), false);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  char_preview::Draw(avail.x, std::max(100.0f, avail.y - 64 * g_scale));
  const std::string st = char_preview::Status();
  if (!st.empty()) ImGui::TextDisabled("%s", st.c_str());
  Controls();
  ImGui::EndChild();
}

}  // namespace anims_page
}  // namespace mm
