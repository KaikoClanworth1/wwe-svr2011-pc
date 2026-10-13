// Animations page: a character playing the game's motions. The menu banks
// in m.pac MVMT (YMBs: stances, taunts, finisher previews...) play here, on
// the superstar and, for two-person motions, a dummy. The match moves (YMKs
// banks: MOT/...) play here too ("Watch here": the superstar does it, the
// dummy takes it), or in a test match with a dummy where the game forces the
// move (SVR2011_TEST_MATCH + SVR2011_TEST_FORCE_MOVE).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <thread>

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
// the match moves (YMKs: only the game plays them): picked by name from the
// game's move table, started in a test match by the game's own test aids
char g_move_filter[64] = "";
int g_move_id = 0;
int g_every = 6;
int g_game_dummy = -1;  // index in Stars() (-1: the Dummy above)

// ---- the match moves' banks (YMKs in m.pac: MOT/..., MVMT/...), indexed once
struct BankPath {
  PacEntryInfo entry;
  std::vector<uint32_t> kids;  // PACH child ids down to the bank
  int x = 0;                   // the variant played (0, else the first the bank has)
};
std::map<int, BankPath> g_kindex;  // move id -> where its motion is (the first bank with its track 0, x 0 preferred)

// A track-0 entry into the index: the first bank that has the move, x 0 over other variants.
void Note(std::map<int, BankPath>& out, const uint8_t* d, const PacEntryInfo& e, const std::vector<uint32_t>& path) {
  if (d[0] != 0) return;
  const int id = Le16(d + 2);
  const auto it = out.find(id);
  if (it == out.end() || (it->second.x != 0 && d[1] == 0)) out[id] = {e, path, d[1]};
}
std::atomic<int> g_kindex_state{0};  // 0 not built, 1 building, 2 ready
std::map<int, BankPath> g_kindex_next;
std::set<int> g_all_motions, g_all_motions_next;  // every motion id in m.pac (YMKs and YMBs, any track)
int g_watch_pending = 0;  // (test aid: a move to watch once the index is there)

void Walk(const Bytes& raw, const PacEntryInfo& e, std::vector<uint32_t>& path, std::map<int, BankPath>& out, std::set<int>& all) {
  if (IsPach(raw)) {
    std::vector<PachEntry> kids;
    if (!PachRead(raw, kids)) return;
    for (auto& k : kids) {
      path.push_back(k.id);
      Walk(Unpack(k.data), e, path, out, all);
      path.pop_back();
    }
  } else if (raw.size() >= 0x114 && !std::memcmp(raw.data(), "YMBs", 4)) {
    const uint32_t n = Le32(&raw[0x110]);
    for (uint32_t i = 0; i < n && 0x114 + 16 * (i + 1) <= raw.size(); ++i) {
      const uint8_t* d = &raw[0x114 + 16 * i];
      all.insert(Le16(d + 2));
      Note(out, d, e, path);  // (taunts, submissions: YMBs)
    }
  } else if (raw.size() >= 0x114 && !std::memcmp(raw.data(), "YMKs", 4)) {
    const uint32_t n = Le32(&raw[0x110]);
    for (uint32_t i = 0; i < n && 0x114 + 16 * (i + 1) <= raw.size(); ++i) all.insert(Le16(&raw[0x114 + 16 * i + 2]));
    for (uint32_t i = 0; i < n && 0x114 + 16 * (i + 1) <= raw.size(); ++i) {
      const uint8_t* d = &raw[0x114 + 16 * i];
      Note(out, d, e, path);
    }
  }
}

void BuildIndex() {
  int expect = 0;
  if (!g_kindex_state.compare_exchange_strong(expect, 1)) return;
  std::thread([game = g_game] {
    std::map<int, BankPath> out;
    std::set<int> all;
    const fs::path mpac = fs::path(game) / L"pac" / L"m.pac";
    PacIndex idx;
    if (ReadPacIndex(mpac, idx))
      for (const auto& e : idx.entries) {
        Bytes raw;
        if (!ReadPacEntry(mpac, e, raw)) continue;
        std::vector<uint32_t> path;
        Walk(Unpack(raw), e, path, out, all);
      }
    g_kindex_next = std::move(out);
    g_all_motions_next = std::move(all);
    g_kindex_state = 2;
  }).detach();
}

bool IndexReady() {
  if (g_kindex_state == 0) BuildIndex();
  if (g_kindex_state == 2 && g_kindex.empty() && !g_kindex_next.empty()) {
    g_kindex = std::move(g_kindex_next);
    g_all_motions = std::move(g_all_motions_next);
    Log("anims: " + std::to_string(g_kindex.size()) + " match moves indexed in m.pac");
  }
  return g_kindex_state == 2;
}

// The move's bank read in the background, then played here (track 0 the superstar, 1 the dummy).
void Watch(int id) {
  const auto it = g_kindex.find(id);
  if (it == g_kindex.end()) {
    Status("Move " + std::to_string(id) + " has no motion in m.pac (a move pack's, or one only the game builds): use Play in game.");
    return;
  }
  const BankPath where = it->second;
  RunInBackground([id, where] {
    Progress("Reading the move's motions ...");
    Bytes raw;
    bool ok = ReadPacEntry(fs::path(g_game) / L"pac" / L"m.pac", where.entry, raw);
    Bytes cur = ok ? Unpack(raw) : Bytes();
    for (uint32_t kid : where.kids) {
      std::vector<PachEntry> kids;
      ok = ok && PachRead(cur, kids);
      Bytes next;
      bool hit = false;
      for (auto& k : kids)
        if (ok && k.id == kid) next = Unpack(k.data), hit = true;
      ok = ok && hit;
      cur = std::move(next);
    }
    auto bank = std::make_shared<const Bytes>(std::move(cur));
    OnUiThread([id, ok, bank, x = where.x] {
      std::string err;
      if (!ok || !char_preview::SetMotion(bank, id, x, &err)) {
        Status("That move's motion can't be shown here: " + (err.empty() ? std::string("the bank didn't read") : err) + ".");
        return;
      }
      g_sel_id = -1;
      Status("Watching " + MoveName(id) + " (move " + std::to_string(id) + ").");
    });
  });
}

std::wstring ChPac(int id) { return (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(id) + L".pac")).wstring(); }

// A bank read from m.pac in the background.
void LoadBank(int which) {
  if (g_banks.count(which) || g_loading || Busy()) return;  // (Busy: tried again next frame)
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
  if (id >= 18000 && id < 19000) return "Finisher preview " + std::to_string(id);
  if (id >= 30000 && id < 31000) return "Finisher preview " + std::to_string(id);
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
  bool hits = char_preview::Hits();
  if (ImGui::Checkbox("Hit spheres", &hits)) char_preview::SetHits(hits);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("The game's hit volumes: green the spheres a superstar is hit on, red the attacking ones while a "
                      "move turns them on (strikes and weapon moves; a few moves switch them in code and show none here).");
  bool blood = char_preview::Blood();
  if (char_preview::HasBlood()) {
    ImGui::SameLine();
    if (ImGui::Checkbox("Blood", &blood)) char_preview::SetBlood(blood);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The model's blood overlays, as a bloodied superstar shows them in a match.");
    if (blood) {
      ImGui::SameLine();
      int stage = char_preview::BloodStage();
      ImGui::SetNextItemWidth(90 * g_scale);
      if (ImGui::Combo("##bs", &stage, "light\0medium\0heavy\0")) char_preview::SetBloodStage(stage);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Camera")) char_preview::ResetCamera();
  ImGui::SameLine();
  ImGui::TextDisabled("%s", p.keys ? (std::to_string(p.keys / 30.0f).substr(0, 4) + " s at 30 keys/s").c_str() : "");
}

// The game plays the move: a test match of the superstar against the dummy
// (both computer-controlled, straight from the start), the attacker starting
// the move on the dummy every few seconds (match_types.cpp FORCE_MOVE).
void PlayInGame() {
  const auto& stars = Stars();
  if (GameRunning()) {
    Status("The game is running: close it first.");
    return;
  }
  const int dummy = g_game_dummy >= 0 ? g_game_dummy : g_dummy;
  if (g_star < 0 && g_model.empty()) { Status("Pick who plays the move first."); return; }
  if (dummy < 0) { Status("Pick a dummy for the move to be done to."); return; }
  if (!g_move_id) { Status("Pick a move."); return; }
  // people= takes roster names: a superstar's, or a mod's name (its folder's manifest)
  std::string who;
  if (g_star >= 0) who = stars[g_star].name;
  else {
    // a model file: a mod's ch.pac next to its manifest?
    Bytes man;
    if (ReadFile(PathStr(fs::path(g_model).parent_path() / L"manifest.txt"), man)) who = Value(std::string(man.begin(), man.end()), "name");
    if (who.empty()) { Status("That model isn't an installed mod: install it as a superstar first, then pick it by name."); return; }
  }
  // the route's menu steps run through the scripted controller, which needs its input file
  // (empty is fine); without it the game's title demo plays instead of the chosen match
  const fs::path work = fs::path(g_game) / L"Mods" / L".convert";
  std::error_code ec;
  fs::create_directories(work, ec);
  const fs::path input = work / L"play_input.txt";
  if (FILE* f = _wfopen(input.c_str(), L"wb")) std::fclose(f);
  const std::wstring env = L"SVR2011_ROUTE=match;SVR2011_INPUT_FILE=" + input.wstring() + L";SVR2011_TEST_MATCH=people=" +
                           Wide(who) + L"," + Wide(stars[dummy].name) + L" cpu=all;SVR2011_TEST_FORCE_MOVE=0,1," +
                           std::to_wstring(std::max(2, g_every)) + L"," + std::to_wstring(g_move_id) + L";SVR2011_TEST_MOTION_LOG=1";
  StartGame(env);
  Status("The game starts a match: " + who + " does " + MotionLabel(g_move_id) + " to " + stars[dummy].name + " every " +
         std::to_string(std::max(2, g_every)) + " s (both computer-controlled).");
}

void InGameSection() {
  const auto& stars = Stars();
  ImGui::TextDisabled("Match moves, done to a dummy in a test match.");
  Hint("The match moves (grapples, strikes, the moves of a move pack) are in another format only the game plays. "
       "This starts a test match - the superstar against a dummy, both computer-controlled - where the move is "
       "done every few seconds. Close the game when done.");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##mf", "find a move by name or number", g_move_filter, sizeof g_move_filter);
  const std::string f = Lower(g_move_filter);
  ImGui::BeginChild("movelist", ImVec2(0, -150 * g_scale), true);
  int shown = 0;
  for (const auto& [id, name] : MoveNames()) {
    if (!f.empty() && Lower(name).find(f) == std::string::npos && std::to_string(id).find(f) == std::string::npos) continue;
    if (++shown > 400) { ImGui::TextDisabled("... type more to narrow it down"); break; }
    char line[128];
    std::snprintf(line, sizeof line, "%s##g%d", name.c_str(), id);
    if (ImGui::Selectable(line, g_move_id == id)) g_move_id = id;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("move %d", id);
  }
  ImGui::EndChild();
  ImGui::SetNextItemWidth(120 * g_scale);
  ImGui::InputInt("Move number", &g_move_id);
  ImGui::SetNextItemWidth(120 * g_scale);
  ImGui::SliderInt("Every (s)", &g_every, 2, 20);
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##gd", g_game_dummy >= 0 ? stars[g_game_dummy].name.c_str() : "Done to: the dummy above")) {
    if (ImGui::Selectable("the dummy above", g_game_dummy < 0)) g_game_dummy = -1;
    for (int i = 0; i < int(stars.size()); ++i)
      if (ImGui::Selectable((stars[i].name + "##gd" + std::to_string(i)).c_str(), g_game_dummy == i)) g_game_dummy = i;
    ImGui::EndCombo();
  }
  const bool ready = IndexReady();
  ImGui::BeginDisabled(!ready || Busy() || g_move_id <= 0);
  PushAccent();
  if (ImGui::Button(ready ? "Watch here" : "Watch here (reading m.pac ...)", ImVec2(-1, 0))) Watch(g_move_id);
  PopAccent();
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("The move's motion played here: the superstar does it, the dummy takes it.");
  ImGui::PushStyleColor(ImGuiCol_Button, kGood);
  if (ImGui::Button("Play in game", ImVec2(-1, 0))) PlayInGame();
  ImGui::PopStyleColor();
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
  if (!g_model.empty()) {  // the model's attires (its EMD kind-2 entries)
    const std::vector<int> att = ModelAttires(g_model);
    if (att.size() > 1) {
      ImGui::TextDisabled("Attire");
      for (int a : att) {
        ImGui::SameLine();
        const bool on = a == char_preview::Attire();
        if (on) PushAccent();
        if (ImGui::SmallButton((std::to_string(a) + "##aa").c_str())) char_preview::SetAttire(a);
        if (on) PopAccent();
      }
    } else if (char_preview::Attire() != 1) {
      char_preview::SetAttire(1);
    }
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
  if (!ImGui::BeginTabBar("anim_tabs")) return;
  if (!ImGui::BeginTabItem("Watch here")) {
    if (ImGui::BeginTabItem("In the game")) {
      InGameSection();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    return;
  }
  ImGui::TextDisabled("The menu motions: stances, taunts, finisher previews.");
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
  const float list_h = 0;
  if (it == g_banks.end() || !it->second.error.empty()) {
    if (it != g_banks.end()) ImGui::TextColored(kBad, "%s", it->second.error.c_str());
    else if (g_bank >= 0) ImGui::TextDisabled("Loading ...");
    ImGui::EndTabItem();
    ImGui::EndTabBar();
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
  ImGui::BeginChild("list", ImVec2(0, list_h), true);
  for (const Row& r : rows) {
    const std::string label = MotionLabel(r.id) + (r.x ? ", variant " + std::to_string(r.x) : "");
    if (!filter.empty() && Lower(label).find(filter) == std::string::npos && std::to_string(r.id).find(filter) == std::string::npos) continue;
    const bool on = g_sel_id == r.id && g_sel_x == r.x;
    char line[160];
    std::snprintf(line, sizeof line, "%s%s##%d_%d", label.c_str(), r.victim ? "  - 2 people" : "", r.id, r.x);
    if (ImGui::Selectable(line, on)) Pick(r.id, r.x);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Motion %d%s, %d frames (%.1f s).%s%s", r.id, r.x ? (" variant " + std::to_string(r.x)).c_str() : "", r.frames,
                        r.frames / 30.0f, r.victim ? "\n2 people: the other person's part plays on the dummy." : "",
                        r.x ? "\nVariants: the same move for another situation (the game picks one by size or position)." : "");
  }
  ImGui::EndChild();
  ImGui::EndTabItem();
  if (ImGui::BeginTabItem("In the game")) {
    InGameSection();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

}  // namespace

int g_test_key = -1;
void TestKey(int key) { g_test_key = key; }

// --pose-compare <dump>,<star>,<dummy>,<motion>[,<x>]: the game's pose dump against this view (phase B)
struct {
  std::wstring dump;
  int star = 0, dummy = 0, motion = 0, x = -1;  // (x -1: any variant)
  std::shared_ptr<const Bytes> bank;
  bool started = false, bank_tried = false;
} g_cmp;
void TestCompare(const std::wstring& spec) {
  const size_t c = spec.find(L',');
  if (c == std::wstring::npos) return;
  g_cmp.dump = spec.substr(0, c);
  swscanf_s(spec.c_str() + c + 1, L"%d,%d,%d,%d", &g_cmp.star, &g_cmp.dummy, &g_cmp.motion, &g_cmp.x);
}
void RunCompare() {
  if (!g_cmp.started) {
    g_cmp.started = true;
    char_preview::SetModel(ChPac(g_cmp.star), g_game);
    char_preview::SetDummy(ChPac(g_cmp.dummy));
  }
  if (!g_cmp.bank_tried) {  // the match banks (m.pac MOT BM00..BM09): the first YMBs bank with the motion
    g_cmp.bank_tried = true;
    for (int k = 0; k < 11 && !g_cmp.bank; ++k) {  // (BM00-BM09, then CAF's MOT: the match idles)
      Bytes entry;
      const std::string name = k < 10 ? "BM0" + std::to_string(k) : std::string("MOT ");
      if (!ReadPacEntry(fs::path(g_game) / L"pac" / L"m.pac", name.c_str(), entry, k < 10 ? nullptr : "CAF ")) continue;
      std::vector<PachEntry> kids;
      if (!PachRead(Unpack(entry), kids)) continue;
      for (const auto& kd : kids) {
        auto raw = std::make_shared<Bytes>(Unpack(kd.data));
        if (raw->size() < 0x114 || std::memcmp(raw->data(), "YMBs", 4)) continue;
        const uint32_t n = Le32(&(*raw)[0x110]);
        for (uint32_t i = 0; i < n && 0x114 + 16 * (i + 1) <= raw->size(); ++i) {
          const uint8_t* e = &(*raw)[0x114 + 16 * i];
          if (int(Le16(e + 2)) == g_cmp.motion && (g_cmp.x < 0 || e[1] == g_cmp.x)) {
            g_cmp.bank = raw;
            Log("compare: motion " + std::to_string(g_cmp.motion) + " in m.pac MOT/" + name + " child " + std::to_string(kd.id));
            break;
          }
        }
        if (g_cmp.bank) break;
      }
    }
    if (!g_cmp.bank) Log("compare: motion " + std::to_string(g_cmp.motion) + " not in m.pac's match banks");
  }
  const std::string r = char_preview::ComparePoseDump(g_cmp.dump, g_cmp.bank, g_cmp.motion, g_cmp.x);
  if (r.empty() && g_cmp.bank) return;  // (the models still loading)
  Log("compare:\n" + r);
  WriteLogTo(g_cmp.dump + L".compare.log");
  g_cmp.dump.clear();
  PostMessageW(g_wnd, WM_CLOSE, 0, 0);
}

void TestStart(int star_id, int bank, int motion, int dummy_id) {
  g_test_star = star_id, g_test_bank = bank, g_test_motion = motion, g_test_dummy = dummy_id;
}

bool MovesIndexed() { return IndexReady(); }
bool AnyMotion(int id) { return IndexReady() && g_all_motions.count(id); }
bool HasMotion(int id) { return IndexReady() && g_kindex.count(id); }
void WatchMove(int id) { Watch(id); }

void TestWatch(int star_id, int dummy_id, int move) {
  g_test_star = star_id, g_test_dummy = dummy_id, g_watch_pending = move;
}

int g_test_play_move = 0;
void TestPlay(int star_id, int dummy_id, int move) {
  g_test_star = star_id, g_test_dummy = dummy_id, g_test_play_move = move;
}

void Draw() {
  if (!g_cmp.dump.empty()) RunCompare();
  if (g_watch_pending && IndexReady() && !Busy()) Watch(g_watch_pending), g_move_id = g_watch_pending, g_watch_pending = 0;
  if (g_test_star > 0 && !g_game.empty()) {  // the test aid, once
    const auto& stars = Stars();
    for (int i = 0; i < int(stars.size()); ++i) {
      if (stars[i].id == g_test_star) g_star = i, g_model = ChPac(stars[i].id), char_preview::SetModel(g_model, g_game);
      if (stars[i].id == g_test_dummy) g_dummy = i, char_preview::SetDummy(ChPac(stars[i].id));
    }
    if (g_test_bank >= 0 && g_test_bank < int(std::size(kBanks))) g_bank = g_test_bank, LoadBank(g_bank);
    g_test_star = 0;
    if (g_test_play_move) {
      g_move_id = g_test_play_move;
      g_test_play_move = 0;
      PlayInGame();
    }
  }
  if (g_test_motion >= 0 && g_bank >= 0 && g_banks.count(g_bank) && !g_loading) {
    Pick(g_test_motion, 0);
    g_test_motion = -1;
    if (g_test_key >= 0) char_preview::Play().key = float(g_test_key), char_preview::Play().playing = false;
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
  char_preview::SetHeight(1.0f);  // (the Superstar page's height= is its own)
  char_preview::Draw(avail.x, std::max(100.0f, avail.y - 64 * g_scale));
  const std::string st = char_preview::Status();
  if (!st.empty()) ImGui::TextDisabled("%s", st.c_str());
  Controls();
  ImGui::EndChild();
}

}  // namespace anims_page
}  // namespace mm
