// Superstar page: a new playable character under the character select's
// M tile (the game: superstar_mods.cpp, docs/SUPERSTAR_MODS.md). A fighting
// style picks the roster template its moves, entrance motions and starting
// attributes come from; the mod brings its own model pac, name, theme song,
// entrance movie, attires, select picture, crowd signs and name recording.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app.h"
#include "char_preview.h"
#include "svrfmt/pac.h"

namespace mm {
namespace star_page {

using namespace svrfmt;

namespace {

// The Created Superstar nicknames the announcer and commentary can say
// (call=<index>; the game's CAS_* list, sub_8261EAB0's table at 0x82043F98).
const char* const kNickNames[84] = {
    "Alpha", "Andersen", "Angel", "Angus", "Archangel", "Azrael", "Bison", "Brittany", "Chen", "Dregs", "Dynamite",
    "El Jefe", "Franco", "Gonzalez", "Griffin", "Grime", "Hero", "Icon", "Jessica", "Jester", "Justice", "Kowalczyk",
    "Lassiter", "Lee", "Lilith", "Mantis", "Matsuda", "Matsumoto", "Maverick", "Mercer", "Amazing", "Black", "Macho",
    "Omega", "Quinn", "Rodriguez", "Santos", "Savior", "Silva", "Skinner", "Sokolov", "Sophia", "Sullivan", "Sunshine",
    "The Bad Guy", "The Barbarian", "The Bruiser", "Champ", "The Cowboy", "The Disaster", "The Dog", "The Dominator",
    "The Future", "The Gangster", "The Hardcore Icon", "(no commentary)", "The King", "The Maniac", "The Masked Man",
    "The Mastodon", "The Mechanic", "The Monster", "The Motor", "The Natural", "The Nightmare", "The Ninja",
    "The Olympian", "The Phenom", "The Prince", "The Princess", "The Professor", "The Rocker", "The Samoan",
    "The Samurai", "The Scorpion", "The Show", "The Soldier", "The Superstar", "The Tornado", "Thunder", "Vega",
    "Williams", "Yosef", "Youngblood"};

// Fighting styles: a new superstar's moves, entrance motions and starting
// attributes come from one (the game needs a full move-set). Each is a
// template from the roster underneath.
struct Style {
  const char* name;
  const char* about;
  int template_id;  // a playable superstar (its move-set, entrance motions)
  bool female;
};
const Style kStyles[] = {
    {"Powerhouse", "Big, slow and strong: power slams and bear hugs.", 125, false},
    {"Brawler", "Strikes, clotheslines and a power finisher.", 160, false},
    {"All-rounder", "A bit of everything: the main-event style.", 139, false},
    {"Technical", "Holds, counters and a submission finisher.", 104, false},
    {"Submission specialist", "Wears opponents down with holds.", 267, false},
    {"High flyer", "Fast, light, off the top rope.", 123, false},
    {"Showman", "Flashy moves and taunts.", 218, false},
    {"Diva: powerhouse", "A strong Diva: slams and power moves.", 224, true},
    {"Diva: high flyer", "A quick, acrobatic Diva.", 164, true},
    {"Diva: technical", "A Diva who wrestles holds and counters.", 143, true},
};
const char* const kAttributes[7] = {"Grapple", "Submission", "Speed", "Strikes", "Hardcore", "Charisma", "Durability"};

// Abilities (record +230..: up to 8 ids; the name's string id is 0x9EA8 + id,
// sub_828B73D8). The eleven the 2011 roster uses; the other ids in the string
// table are SvR 2009/2010 leftovers no record has.
struct Ability {
  int id;
  const char* name;
  const char* about;
};
const Ability kAbilities[] = {
    {1, "Dirty Pin", "Pins with the feet on the ropes or a handful of tights."},
    {7, "Move Thief", "Steals the opponent's finisher."},
    {9, "Hammer Throw", "Irish whips with more force: the opponent hits harder."},
    {11, "Resiliency", "Kicks out of a pin once when nearly beaten."},
    {12, "Durability", "Takes longer to wear down."},
    {14, "Kip-Up", "Springs back to the feet when down."},
    {19, "Outside Dives", "Dives to the outside of the ring."},
    {20, "Springboard Dives", "Springboard attacks off the ropes."},
    {22, "Leverage Pin", "A quick pin from a reversal."},
    {23, "Fired Up", "Finishers pass to a partner / a comeback when down."},
    {24, "Ring Escape", "Rolls out of the ring to recover."},
};

struct StarProject {
  int style = 0;
  int ratings[7] = {74, 74, 74, 74, 74, 74, 74};
  bool ratings_set = false;  // (the style's, until moved)
  char name[32] = "", short_name[32] = "", author[64] = "", version[16] = "1.0";
  std::wstring model, song, movie, voice;
  std::wstring attires[4];        // [1..3]: attires 2-4 from other model pacs (their first attire)
  char attire_names[4][32] = {};  // [1..3]: their names on CHANGE ATTIRE ("" = ATTIRE N)
  Picture picture;                // its own select picture (512): else a silhouette
  Image picture_small;            // the 256 bust made with it
  int call = -1;                  // -1: "The Superstar"
  float height = 1.0f;            // height=<scale> from the feet (0.80-1.25; 1: left out)
  std::vector<Picture> signs;     // its fans' signs (up to 4; 128 x 64)
  // the SvR 2010-style extras (the game: superstar_mods.cpp)
  int entrance = -1;              // entrance= number (-1: the style's); a superstar's id, or 535 (Jeff Hardy's, unused)
  char announcer[32] = "";        // announcer=<NAME>: the ring announcer's own clips of that name
  std::vector<int> abilities;     // abilities=a,b,... (empty: the style's)
  bool own_abilities = false;
  std::wstring moves;             // moves=moves.txt: lines 0xOFF=<move id>
  std::wstring pack;              // a move pack folder (pack.txt + motions/) copied in as moves/
};
StarProject g_star;
// test aids: --star <id> picks the model, --star-* files, --test-star-save <file> saves the mod and quits
int g_test_id = 0;
std::wstring g_test_model, g_test_picture, g_test_save;
std::string g_test_name;
bool g_test_done = false;

// Cheap per-file checks for the Problems list (cached by path + size + time).
struct FileCheck {
  bool exists = false, ok = false;
  size_t size = 0;
  int w = 0, h = 0;  // (a movie's)
  std::string note;
};
const FileCheck& Check(const std::wstring& path, const char* kind) {
  static std::map<std::wstring, std::pair<fs::file_time_type, FileCheck>> cache;
  std::error_code ec;
  const auto t = fs::last_write_time(path, ec);
  auto it = cache.find(path);
  if (it != cache.end() && (ec || it->second.first == t)) return it->second.second;
  FileCheck c;
  c.exists = !ec;
  if (c.exists) {
    c.size = size_t(fs::file_size(path, ec));
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
      uint8_t head[32] = {};
      const size_t n = std::fread(head, 1, sizeof head, f);
      std::fclose(f);
      if (!std::strcmp(kind, "pac")) {
        c.ok = n >= 4 && !std::memcmp(head, "EPK8", 4) && c.size >= 0x4000;
        if (!c.ok) c.note = n >= 4 && !std::memcmp(head, "EPAC", 4) ? "an EPAC (an arena or menu pac), not a character model pac (EPK8)"
                                                                      : "not a character model pac (EPK8)";
      } else if (!std::strcmp(kind, "bik")) {
        c.ok = n >= 28 && !std::memcmp(head, "BIK", 3);
        if (c.ok) c.w = int(Le32(head + 20)), c.h = int(Le32(head + 24));
        else c.note = "not a Bink (.bik) movie";
      } else {
        c.ok = n > 0;
      }
    }
  }
  cache[path] = {t, c};
  return cache[path].second;
}

std::string StarId() { return IdFrom(g_star.name, "superstar"); }

void SetPicture(const Image& src) {
  Image render, bust, icon;
  MakeRenders(src, render, bust, icon);
  g_star.picture.Set(std::move(render));
  g_star.picture_small = std::move(bust);
  Touch();
}

// Everything the build needs, copied off the UI thread.
struct Job {
  std::string id, manifest;
  std::wstring model, song, movie, voice, attires[4];
  std::string attire_names[4];
  Image picture, picture_small;
  std::vector<Image> signs;
  std::wstring moves, pack;
};

// height= as written (2 decimals), "" at 1.00 (the key left out)
std::string HeightText() {
  const int c = int(std::lround(std::clamp(g_star.height, 0.8f, 1.25f) * 100));
  if (c == 100) return "";
  char b[8];
  std::snprintf(b, sizeof b, "%d.%02d", c / 100, c % 100);
  return b;
}

bool PrepareJob(Job& j) {
  const Style& st = kStyles[g_star.style];
  if (!g_star.ratings_set) {
    for (int k = 0; k < 7; ++k) g_star.ratings[k] = StarRating(st.template_id, k);
    g_star.ratings_set = true;
  }
  j.id = StarId();
  j.manifest = "type=superstar\nid=" + j.id + "\nname=" + g_star.name + "\nshort=" +
               (g_star.short_name[0] ? g_star.short_name : g_star.name) + "\nbase=" + std::to_string(st.template_id) +
               "\nstyle=" + st.name + "\nauthor=" + g_star.author + "\nversion=" + g_star.version + "\n" + MadeWith();
  j.manifest += "ratings=";
  for (int k = 0; k < 7; ++k) j.manifest += std::to_string(g_star.ratings[k]) + (k < 6 ? "," : "\n");
  if (g_star.call >= 0) j.manifest += "call=" + std::to_string(g_star.call) + "\n";
  if (const std::string h = HeightText(); !h.empty()) j.manifest += "height=" + h + "\n";
  if (g_star.entrance >= 0) j.manifest += "entrance=" + std::to_string(g_star.entrance) + "\n";
  if (g_star.announcer[0]) j.manifest += std::string("announcer=") + g_star.announcer + "\n";
  if (g_star.own_abilities) {
    j.manifest += "abilities=";
    for (size_t k = 0; k < g_star.abilities.size(); ++k) j.manifest += (k ? "," : "") + std::to_string(g_star.abilities[k]);
    j.manifest += "\n";
  }
  if (!g_star.moves.empty()) j.manifest += "moves=moves.txt\n";
  j.moves = g_star.moves, j.pack = g_star.pack;
  j.model = g_star.model, j.song = g_star.song, j.movie = g_star.movie, j.voice = g_star.voice;
  for (int a = 1; a < 4; ++a) j.attires[a] = g_star.attires[a], j.attire_names[a] = g_star.attire_names[a];
  j.picture = g_star.picture.image, j.picture_small = g_star.picture_small;
  for (const auto& s : g_star.signs) j.signs.push_back(s.image);
  return true;
}

// The mod's files (in the background).
bool BuildFiles(Job& j, std::vector<ZipEntry>& files) {
  Progress("Reading the model ...");
  Bytes ch;
  if (!ReadFile(Utf8(j.model), ch) || ch.size() < 0x4000 || std::memcmp(ch.data(), "EPK8", 4)) {
    Status("The model is not a character model pac (EPK8): " + Utf8(j.model));
    return false;
  }
  std::string man = j.manifest;
  files.push_back({"ch.pac", std::move(ch)});
  auto add = [&](const std::wstring& src, const std::string& stem, const char* key, const char* what) {
    if (src.empty()) return true;
    Bytes d;
    if (!ReadFile(Utf8(src), d)) {
      Status(std::string("Could not read the ") + what + ": " + Utf8(src));
      return false;
    }
    const std::string n = stem + Utf8(fs::path(src).extension().wstring());
    files.push_back({n, std::move(d)});
    man += std::string(key) + "=" + n + "\n";
    return true;
  };
  if (!add(j.song, "theme", "song", "theme song")) return false;
  for (int a = 1; a < 4; ++a) {
    if (j.attires[a].empty()) continue;
    Bytes pac;
    if (!ReadFile(Utf8(j.attires[a]), pac) || pac.size() < 0x4000 || std::memcmp(pac.data(), "EPK8", 4)) {
      Status("Attire " + std::to_string(a + 1) + " is not a character model pac (EPK8).");
      return false;
    }
    const std::string n = "attire" + std::to_string(a + 1) + ".pac";
    files.push_back({n, std::move(pac)});
    man += "attire" + std::to_string(a + 1) + "=" + n + "\n";
    if (!j.attire_names[a].empty()) man += "attire" + std::to_string(a + 1) + "_name=" + j.attire_names[a] + "\n";
  }
  if (!add(j.voice, "voice", "voice", "name recording")) return false;
  if (!j.movie.empty()) {
    Bytes m;
    if (!ReadFile(Utf8(j.movie), m) || m.size() < 4 || std::memcmp(m.data(), "BIK", 3)) {
      Status("The entrance movie must be a Bink file (.bik): the launcher's Movies tab makes them.");
      return false;
    }
    files.push_back({"movie.bik", std::move(m)});
    man += "movie=movie.bik\n";
  }
  if (!j.moves.empty()) {
    Bytes t;
    if (!ReadFile(Utf8(j.moves), t)) { Status("Could not read the moves list: " + Utf8(j.moves)); return false; }
    files.push_back({"moves.txt", std::move(t)});
  }
  if (!j.pack.empty()) {  // the move pack: pack.txt and motions/* as moves/
    Progress("Copying the move pack ...");
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(j.pack, ec)) {
      if (!e.is_regular_file(ec)) continue;
      Bytes f;
      if (!ReadFile(PathStr(e.path()), f)) { Status("Could not read " + PathStr(e.path())); return false; }
      files.push_back({"moves/" + fs::relative(e.path(), j.pack, ec).generic_string(), std::move(f)});
    }
  }
  Progress("Encoding the pictures ...");
  for (size_t k = 0; k < j.signs.size() && k < 4; ++k)
    files.push_back({"sign" + std::to_string(k + 1) + ".dds", DdsEncode(j.signs[k], DxtFormat::kDxt1, true)});
  if (j.picture.w) {
    files.push_back({"render.dds", DdsEncode(j.picture, DxtFormat::kDxt5, false)});
    files.push_back({"render_small.dds", DdsEncode(j.picture_small, DxtFormat::kDxt5, false)});
  }
  files.insert(files.begin(), ZipEntry{"manifest.txt", Bytes(man.begin(), man.end())});
  return true;
}

void SaveMod() {
  Job j;
  if (!PrepareJob(j)) return;
  const std::wstring f = PickFile(true, L"Save the superstar mod", kModFilter, 1, L"svrmod", (Wide(j.id) + L".svrmod").c_str());
  if (f.empty()) return;
  const std::string out = Utf8(f);
  RunInBackground([j, out]() mutable {
    std::vector<ZipEntry> files;
    if (!BuildFiles(j, files)) return;
    if (WriteFile(out, ZipWrite(files))) Status("Saved " + out + " (add it in the launcher's Mods tab with +).");
    else Status("The mod could not be written: " + out);
  });
}

void Install(bool start) {
  if (start && GameRunning()) {
    Status("The game is running: close it first (mods load when it starts).");
    return;
  }
  Job j;
  if (!PrepareJob(j)) return;
  RunInBackground([j, start]() mutable {
    std::vector<ZipEntry> files;
    if (!BuildFiles(j, files)) return;
    const fs::path dir = fs::path(g_game) / L"Mods" / L"Superstars" / fs::u8path(j.id);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    for (const auto& f : files)
      if (!WriteFile(PathStr(dir / fs::u8path(f.name)), f.data)) {
        Status("Could not write into " + PathStr(dir) + " (is the game running?)");
        return;
      }
    Status("Installed: under the M tile of the character select from the next game start (" + PathStr(dir) + ").");
    if (start) StartGame();
  });
}

void Problems(std::vector<Problem>& p) {
  if (!g_star.name[0]) p.push_back(Error("The superstar has no name."));
  if (g_star.model.empty()) {
    p.push_back(Error("No model picked.", "Model (ch.pac): a character model pac, the game's (pac\\ch\\chNNN.pac) or a converted one."));
  } else {
    const FileCheck& c = Check(g_star.model, "pac");
    if (!c.exists) p.push_back(Error("The model file is missing: " + FileName(g_star.model)));
    else if (!c.ok) p.push_back(Error("The model is " + c.note + ": " + FileName(g_star.model)));
  }
  for (int a = 1; a < 4; ++a) {
    if (g_star.attires[a].empty()) continue;
    const FileCheck& c = Check(g_star.attires[a], "pac");
    if (!c.exists) p.push_back(Error("Attire " + std::to_string(a + 1) + "'s file is missing: " + FileName(g_star.attires[a])));
    else if (!c.ok) p.push_back(Error("Attire " + std::to_string(a + 1) + " is " + c.note + "."));
    if (a > 1 && g_star.attires[a - 1].empty()) p.push_back(Warning("Attire " + std::to_string(a + 1) + " is set but attire " + std::to_string(a) + " isn't: the game lists attires in order."));
  }
  if (!g_star.movie.empty()) {
    const FileCheck& c = Check(g_star.movie, "bik");
    if (!c.exists) p.push_back(Error("The entrance movie file is missing: " + FileName(g_star.movie)));
    else if (!c.ok) p.push_back(Error("The entrance movie is " + c.note + ".", "The launcher's Movies tab makes .bik files from any video."));
    else if (c.w != 320 || c.h != 320) p.push_back(Warning("The entrance movie is " + std::to_string(c.w) + " x " + std::to_string(c.h) + "; the game's are 320 x 320 (it still plays, stretched)."));
  }
  for (const std::wstring* f : {&g_star.song, &g_star.voice})
    if (!f->empty() && !Check(*f, "any").exists) p.push_back(Error("A sound file is missing: " + FileName(*f)));
  if (!g_star.song.empty()) p.push_back(Warning("Theme songs play at full level: the game's own are about 7 dB quieter. Make yours about -24 LUFS.", ""));
  if (g_star.picture.Empty()) p.push_back(Warning("No select picture: the select screen shows a silhouette where other modes show a render."));
  if (!g_star.moves.empty()) {
    const FileCheck& c = Check(g_star.moves, "any");
    if (!c.exists) p.push_back(Error("The moves list file is missing: " + FileName(g_star.moves)));
  }
  if (!g_star.pack.empty()) {
    std::error_code ec;
    if (!fs::exists(fs::path(g_star.pack) / L"pack.txt", ec)) p.push_back(Error("The move pack folder has no pack.txt: " + FileName(g_star.pack), "A pack is a folder with pack.txt and motions/ (tools/movepack.py or the Moves page make one)."));
  }
  for (char c : std::string(g_star.announcer))
    if (!std::isalnum(uint8_t(c))) { p.push_back(Error("The announcer name takes letters and digits only (e.g. JEFFHARDY).")); break; }
  if (g_star.own_abilities && g_star.abilities.empty()) p.push_back(Warning("Own abilities ticked but none chosen: the superstar gets no abilities."));
  if (std::strlen(g_star.name) > 20 && !g_star.short_name[0]) p.push_back(Warning("A long name with no short name: the short name goes on the match screens."));
  if (!g_game.empty()) {
    int mods = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / L"Superstars", ec))
      if (e.is_directory(ec) && fs::exists(e.path() / L"manifest.txt", ec)) ++mods;
    if (mods >= 50) p.push_back(Error("The game already has 50 superstar mods installed: that is the limit.", "Remove one in the launcher's Mods tab."));
    else if (mods >= 45) p.push_back(Warning(std::to_string(mods) + " of 50 superstar mod slots are in use."));
  }
}

void Preview() {
  char_preview::SetModel(g_star.model, g_game);
  char_preview::SetHeight(g_star.height * StarScale(kStyles[g_star.style].template_id));  // as the game: base x height=
  ImGui::BeginGroup();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float w = std::max(220 * g_scale, avail.x);
  const float h = std::clamp(avail.y - (g_star.picture.Empty() ? 40 : 150) * g_scale, 300 * g_scale, w * 1.5f);
  char_preview::Draw(w, h);
  if (!char_preview::Status().empty()) ImGui::TextDisabled("%s", char_preview::Status().c_str());
  if (!g_star.picture.Empty()) {
    g_star.picture.Draw(110 * g_scale, 110 * g_scale);
    ImGui::SameLine();
    ImGui::TextDisabled("The select screen's picture.");
  }
  ImGui::EndGroup();
}

void Draw() {
  if (!g_test_done) {  // the test aids, once the page shows
    g_test_done = true;
    if (g_test_id) {
      for (int i = 0; i < int(std::size(kStyles)); ++i)
        if (kStyles[i].template_id == g_test_id) g_star.style = i;
      if (const StarInfo* s = StarById(g_test_id)) std::snprintf(g_star.name, sizeof g_star.name, "Test %s", s->name.c_str());
      g_star.model = (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(g_test_id) + L".pac")).wstring();
    }
    if (!g_test_model.empty()) g_star.model = g_test_model;
    if (!g_test_name.empty()) std::snprintf(g_star.name, sizeof g_star.name, "%s", g_test_name.c_str());
    if (!g_test_picture.empty()) {
      Image img;
      if (LoadPicture(g_test_picture, img)) SetPicture(img);
    }
    if (!g_test_save.empty()) {
      Job j;
      PrepareJob(j);
      std::vector<ZipEntry> files;
      if (BuildFiles(j, files) && WriteFile(Utf8(g_test_save), ZipWrite(files))) Log("test: saved " + Utf8(g_test_save));
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
  }
  Heading("Superstar", "A new playable character under the M tile of the character select (up to 50 mods).");
  std::vector<Problem> problems;
  Problems(problems);
  ImGui::BeginChild("form", ImVec2(-360 * g_scale, -48 * g_scale), false);
  TextField("Name", g_star.name, sizeof g_star.name, "the superstar's name (up to 31 letters)");
  TextField("Short name (optional)", g_star.short_name, sizeof g_star.short_name, "on the match screens");
  ImGui::SetNextItemWidth(320 * g_scale);
  if (ImGui::BeginCombo("Fighting style", kStyles[g_star.style].name)) {
    for (int i = 0; i < int(std::size(kStyles)); ++i)
      if (ImGui::Selectable(kStyles[i].name, g_star.style == i)) {
        g_star.style = i;
        g_star.ratings_set = false;
      }
    ImGui::EndCombo();
  }
  Hint("The style decides the move-set, the entrance motions and the starting attributes (from one of the game's "
       "superstars underneath). The moves can be changed in the game's CREATE A MOVE-SET.");
  ImGui::TextDisabled("%s", kStyles[g_star.style].about);
  if (!g_star.ratings_set) {
    for (int k = 0; k < 7; ++k) g_star.ratings[k] = StarRating(kStyles[g_star.style].template_id, k);
    g_star.ratings_set = true;
  }
  ImGui::Spacing();
  ImGui::TextUnformatted("Attributes");
  for (int k = 0; k < 7; ++k) {
    ImGui::SetNextItemWidth(320 * g_scale);
    if (ImGui::SliderInt(kAttributes[k], &g_star.ratings[k], 1, 99)) g_star.ratings_set = true;
  }
  {  // height=: the model's size from its feet
    ImGui::SetNextItemWidth(236 * g_scale);
    ImGui::SliderFloat("##height", &g_star.height, 0.8f, 1.25f, "%.2f");
    ImGui::SameLine(0, 4 * g_scale);
    ImGui::SetNextItemWidth(80 * g_scale);
    ImGui::InputFloat("Height (scale)", &g_star.height, 0, 0, "%.2f");
    g_star.height = std::clamp(std::round(g_star.height * 100) / 100, 0.8f, 1.25f);
    const float base = StarScale(kStyles[g_star.style].template_id);
    Hint("The superstar's size against the base superstar's (the style's): 1.00 is the base's own size (the key "
         "is left out), 1.05 is 5% taller. The game stretches the skeleton, so grapples and moves follow. For "
         "converted or older models that come out too tall or too short. The game's moves are made for sizes "
         "near the roster's: about 0.90 - 1.15 plays best. The preview shows the size the game will use.");
    ImGui::SameLine();
    ImGui::TextDisabled("%+d%%", int(std::lround((g_star.height - 1) * 100)));
    if (const float tall = char_preview::ModelHeight(); tall > 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("about %d cm", int(std::lround(tall * 10 * base * g_star.height)));
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Measured on the model in its stance, at the size the game uses (the base's own size is x%.2f).", base);
    }
    if (g_star.height != 1.0f) {
      ImGui::SameLine();
      if (ImGui::SmallButton("1.00##hreset")) g_star.height = 1.0f;
    }
  }
  ImGui::SetNextItemWidth(320 * g_scale);
  if (ImGui::BeginCombo("Name call (optional)", g_star.call < 0 ? "The Superstar" : kNickNames[g_star.call])) {
    if (ImGui::Selectable("The Superstar##d", g_star.call < 0)) g_star.call = -1;
    for (int i = 0; i < 84; ++i)
      if (ImGui::Selectable((std::string(kNickNames[i]) + "##n" + std::to_string(i)).c_str(), g_star.call == i))
        g_star.call = i;
    ImGui::EndCombo();
  }
  Hint("What the ring announcer and the commentators call the superstar: one of the nicknames a Created Superstar "
       "can have (or record the name below).");
  TextField("Author (optional)", g_star.author, sizeof g_star.author);
  TextField("Version (optional)", g_star.version, sizeof g_star.version, "1.0");
  if (ImGui::CollapsingHeader("More: entrance, announcer, abilities, moves")) {
    ImGui::Indent();
    // entrance: a superstar's (number = id) or Jeff Hardy's 2010 one the game ships unused
    const auto& stars = Stars();
    std::string cur = "The style's";
    if (g_star.entrance == 535) cur = "Jeff Hardy's (SvR 2010, unused by the game)";
    else if (g_star.entrance >= 0) {
      const StarInfo* s = StarById(g_star.entrance);
      cur = (s ? s->name : "Superstar " + std::to_string(g_star.entrance)) + "'s";
    }
    ImGui::SetNextItemWidth(320 * g_scale);
    if (ImGui::BeginCombo("Entrance", cur.c_str())) {
      if (ImGui::Selectable("The style's", g_star.entrance < 0)) g_star.entrance = -1;
      if (ImGui::Selectable("Jeff Hardy's (SvR 2010, unused by the game)", g_star.entrance == 535)) g_star.entrance = 535;
      for (const auto& s : stars)
        if (ImGui::Selectable((s.name + "'s##e" + std::to_string(s.id)).c_str(), g_star.entrance == s.id)) g_star.entrance = s.id;
      ImGui::EndCombo();
    }
    Hint("The entrance motions and pyro (the music and movie are the files below). Each of the game's superstars has "
         "one; the game also ships Jeff Hardy's from SvR 2010 without using it.");
    TextField("Announcer name (optional)", g_star.announcer, sizeof g_star.announcer, "e.g. JEFFHARDY");
    Hint("A name the ring announcer's sound banks have clips of (letters and digits, as the banks spell it: "
         "JEFFHARDY). The announcer then says it. It wins over a name recording; the name call above stays for the "
         "commentators.");
    if (ImGui::Checkbox("Own abilities", &g_star.own_abilities)) Touch();
    Hint("The superstar's abilities (up to 8). Unticked: the style's.");
    if (g_star.own_abilities) {
      ImGui::Indent();
      int col = 0;
      for (const auto& a : kAbilities) {
        bool on = std::find(g_star.abilities.begin(), g_star.abilities.end(), a.id) != g_star.abilities.end();
        if (col++ % 3) ImGui::SameLine(0, 20 * g_scale);
        ImGui::BeginDisabled(!on && g_star.abilities.size() >= 8);
        if (ImGui::Checkbox(a.name, &on)) {
          if (on) g_star.abilities.push_back(a.id);
          else g_star.abilities.erase(std::remove(g_star.abilities.begin(), g_star.abilities.end(), a.id), g_star.abilities.end());
          Touch();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.about);
        ImGui::EndDisabled();
      }
      ImGui::Unindent();
    }
    const COMDLG_FILTERSPEC txt[] = {{L"Moves list (*.txt)", L"*.txt"}};
    FileRow("Moves list (optional)...", g_star.moves, "the style's moves", txt, 1,
            "moves.txt: lines 0xOFF=<move id> that change the style's move-set (OFF a byte offset in the profile's "
            "move block, 0..0x1BF, even). docs/SUPERSTAR_MODS.md explains it.");
    ImGui::PushID("pack");
    if (ImGui::Button("Move pack folder (optional)...", ImVec2(220 * g_scale, 0))) {
      const std::wstring f = PickFolder(L"The move pack folder (pack.txt + motions)");
      if (!f.empty()) g_star.pack = f, Touch();
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Moves the game doesn't have, carried inside the mod (moves/): a folder with pack.txt and "
                        "motions/, as the Moves page saves one.");
    ImGui::SameLine();
    if (g_star.pack.empty()) ImGui::TextDisabled("none");
    else ImGui::TextUnformatted(FileName(g_star.pack).c_str());
    if (!g_star.pack.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) g_star.pack.clear(), Touch();
    }
    ImGui::PopID();
    ImGui::Unindent();
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Files");
  FileRow("Model (ch.pac)...", g_star.model, "required: the superstar's model", kPacFilter, 1,
          "A character model pac: one of the game's (pac\\ch\\chNNN.pac) or one converted from another game.");
  FileRow("Theme song (optional)...", g_star.song, "the style's", kSoundFilter, 1);
  FileRow("Entrance movie (optional)...", g_star.movie, "the style's", kBinkFilter, 1,
          "A 320 x 320 Bink movie: From a video... makes one here, the launcher's Movies tab too.");
  ImGui::SameLine();
  BinkFromVideoButton("From a video...", g_star.movie, g_star.name);
  for (int a = 1; a < 4; ++a) {
    const std::string label = "Attire " + std::to_string(a + 1) + " (optional)...";
    FileRow(label.c_str(), g_star.attires[a], "none", kPacFilter, 1, "Another model pac: its first attire becomes this attire.");
    if (!g_star.attires[a].empty()) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(160 * g_scale);
      ImGui::InputTextWithHint(("##an" + std::to_string(a)).c_str(), ("ATTIRE " + std::to_string(a + 1)).c_str(),
                               g_star.attire_names[a], sizeof g_star.attire_names[a]);
    }
  }
  ImGui::TextDisabled("Attire 1 is the model above; each extra attire is another pac's first attire.");
  {  // its fans' crowd signs
    ImGui::BeginDisabled(g_star.signs.size() >= 4);
    if (ImGui::Button("Crowd signs (optional)...", ImVec2(220 * g_scale, 0)))
      for (const auto& f : PickFiles(L"Signs for the superstar's fans (up to 4)", kPictureFilter, 1)) {
        Image img;
        if (g_star.signs.size() < 4 && LoadPicture(f, img)) {
          Picture p;
          p.Set(signs_page::SignPicture(img));
          g_star.signs.push_back(std::move(p));
          Touch();
        }
      }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (g_star.signs.empty()) ImGui::TextDisabled("none");
    for (auto& s : g_star.signs) {
      ImGui::SameLine();
      ImGui::Image(s.Id(), ImVec2(64 * g_scale, 32 * g_scale));
    }
    if (!g_star.signs.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("x##signs")) g_star.signs.clear(), Touch();
    }
  }
  FileRow("Name recording (optional)...", g_star.voice, "none (the name call above)", kSoundFilter, 1,
          "A short recording of the name, said the ring announcer's way: it plays when he announces the superstar.");
  ImGui::PushID("pic");
  if (ImGui::Button("Select picture (optional)...", ImVec2(220 * g_scale, 0))) {
    const std::wstring f = PickFile(false, L"Select screen picture (a PNG with a transparent background is best)", kPictureFilter, 1);
    Image img;
    if (!f.empty() && LoadPicture(f, img)) SetPicture(img), Log("Select picture set from " + Utf8(f));
  }
  ImGui::SameLine();
  if (g_star.picture.Empty()) ImGui::TextDisabled("a silhouette");
  else ImGui::TextUnformatted("your picture");
  if (!g_star.picture.Empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) g_star.picture.Clear(), g_star.picture_small = Image(), Touch();
  }
  ImGui::PopID();
  ImGui::Spacing();
  DrawProblems(problems);
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("preview", ImVec2(0, -48 * g_scale), true);
  Preview();
  ImGui::EndChild();
  switch (ModButtons(problems, true)) {
    case 1: SaveMod(); break;
    case 2: Install(false); break;
    case 3: Install(true); break;
  }
}

std::string StateText() {
  std::string s = std::string("name=") + g_star.name + "\nshort=" + g_star.short_name + "\nstyle=" +
                  std::to_string(g_star.style) + "\ncall=" + std::to_string(g_star.call) + "\nauthor=" + g_star.author +
                  "\nversion=" + g_star.version + "\nheight=" + HeightText() + "\nmodel=" + Utf8(g_star.model) + "\nsong=" + Utf8(g_star.song) +
                  "\nmovie=" + Utf8(g_star.movie) + "\nvoice=" + Utf8(g_star.voice) + "\nratings=";
  for (int k = 0; k < 7; ++k) s += std::to_string(g_star.ratings[k]) + ",";
  for (int a = 1; a < 4; ++a) s += "\nattire" + std::to_string(a + 1) + "=" + Utf8(g_star.attires[a]) + "|" + g_star.attire_names[a];
  s += "\nentrance=" + std::to_string(g_star.entrance) + "\nannouncer=" + g_star.announcer + "\nmoves=" + Utf8(g_star.moves) +
       "\npack=" + Utf8(g_star.pack) + "\nabil=" + std::to_string(g_star.own_abilities);
  for (int a : g_star.abilities) s += "," + std::to_string(a);
  return s;
}

void Reset() { g_star = StarProject(); }

void WriteProject(ProjectOut& out) {
  out.Key("name", g_star.name);
  out.Key("short", g_star.short_name);
  out.Key("style", kStyles[g_star.style].name);
  out.Key("author", g_star.author);
  out.Key("version", g_star.version);
  std::string r;
  for (int k = 0; k < 7; ++k) r += std::to_string(g_star.ratings[k]) + (k < 6 ? "," : "");
  out.Key("ratings", r);
  if (g_star.call >= 0) out.Key("call", g_star.call);
  if (const std::string h = HeightText(); !h.empty()) out.Key("height", h);
  out.File("model", g_star.model, "star/model");
  out.File("song", g_star.song, "star/theme");
  out.File("movie", g_star.movie, "star/movie");
  out.File("voice", g_star.voice, "star/voice");
  for (int a = 1; a < 4; ++a) {
    out.File("attire" + std::to_string(a + 1), g_star.attires[a], "star/attire" + std::to_string(a + 1));
    out.Key("attire" + std::to_string(a + 1) + "_name", g_star.attire_names[a]);
  }
  if (g_star.entrance >= 0) out.Key("entrance", g_star.entrance);
  out.Key("announcer", g_star.announcer);
  if (g_star.own_abilities) {
    std::string a = "-";  // (a lone '-': own abilities, none chosen)
    for (size_t k = 0; k < g_star.abilities.size(); ++k) a = (k ? a + "," : std::string()) + std::to_string(g_star.abilities[k]);
    out.Key("abilities", a);
  }
  out.File("moves", g_star.moves, "star/moves");
  if (!g_star.pack.empty()) {  // the pack's files, as the mod carries them
    out.Key("pack", "moves");
    out.Key("pack.from", Utf8(g_star.pack));
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(g_star.pack, ec)) {
      if (!e.is_regular_file(ec)) continue;
      Bytes f;
      if (ReadFile(PathStr(e.path()), f)) out.files.push_back({"moves/" + fs::relative(e.path(), g_star.pack, ec).generic_string(), std::move(f)});
    }
  }
  out.Png("picture", g_star.picture.image, "star/picture");
  for (size_t k = 0; k < g_star.signs.size(); ++k) out.Png("sign" + std::to_string(k + 1), g_star.signs[k].image, "star/sign" + std::to_string(k + 1));
}

// A project or a mod back into the page. Files inside come out into the
// project's cache folder so the page keeps paths.
bool Read(const ProjectIn& in, bool mod) {
  std::snprintf(g_star.name, sizeof g_star.name, "%s", in.Get("name").c_str());
  std::snprintf(g_star.short_name, sizeof g_star.short_name, "%s", in.Get("short").c_str());
  if (!std::strcmp(g_star.short_name, g_star.name)) g_star.short_name[0] = 0;
  std::snprintf(g_star.author, sizeof g_star.author, "%s", in.Get("author").c_str());
  std::snprintf(g_star.version, sizeof g_star.version, "%s", in.Get("version").c_str());
  if (!g_star.version[0]) std::snprintf(g_star.version, sizeof g_star.version, "1.0");
  const std::string style = in.Get("style");
  const int base = in.GetInt("base", -1);
  for (int i = 0; i < int(std::size(kStyles)); ++i)
    if (style == kStyles[i].name || (style.empty() && base == kStyles[i].template_id)) g_star.style = i;
  const std::string ratings = in.Get("ratings");
  if (!ratings.empty()) {
    int k = 0;
    for (size_t at = 0; at < ratings.size() && k < 7; ++k) {
      g_star.ratings[k] = std::clamp(std::atoi(ratings.c_str() + at), 1, 99);
      const size_t c = ratings.find(',', at);
      at = c == std::string::npos ? ratings.size() : c + 1;
    }
    g_star.ratings_set = true;
  }
  g_star.call = in.GetInt("call", -1);
  if (const std::string h = in.Get("height"); !h.empty()) g_star.height = std::clamp(float(std::atof(h.c_str())), 0.8f, 1.25f);
  g_star.entrance = in.GetInt("entrance", -1);
  std::snprintf(g_star.announcer, sizeof g_star.announcer, "%s", in.Get("announcer").c_str());
  if (const std::string a = in.Get("abilities"); !a.empty()) {
    g_star.own_abilities = true;
    for (size_t at = 0; at < a.size();) {
      if (const int id = std::atoi(a.c_str() + at); id > 0 && id < 256) g_star.abilities.push_back(id);
      const size_t c = a.find(',', at);
      at = c == std::string::npos ? a.size() : c + 1;
    }
  }
  auto file = [&](const char* key, const char* mod_name) -> std::wstring {
    const std::string n = in.Get(key);
    if (mod) {  // (a mod: the manifest names the file inside)
      const std::string inner = n.empty() ? (mod_name ? mod_name : "") : n;
      if (inner.empty() || !in.Find(inner)) return {};
      return in.Extract(inner);
    }
    if (n.empty()) return {};
    // the original path still there? else the project's copy
    const std::string from = in.Get((std::string(key) + ".from").c_str());
    if (!from.empty() && fs::exists(Wide(from))) return Wide(from);
    return in.Extract(n);
  };
  g_star.model = file("model", "ch.pac");
  if (mod && g_star.model.empty() && in.Find("ch.pac")) g_star.model = in.Extract("ch.pac");
  g_star.moves = file("moves", "moves.txt");
  if (mod && g_star.moves.empty() && !in.Get("moves").empty() && in.Find(in.Get("moves"))) g_star.moves = in.Extract(in.Get("moves"));
  {  // the move pack: every moves/* file out, the folder is the pack
    bool any = false;
    std::wstring folder;
    for (const auto& e : in.files)
      if (e.name.rfind("moves/", 0) == 0) {
        const std::wstring f = in.Extract(e.name);
        if (!any && !f.empty()) folder = fs::path(f).parent_path().wstring(), any = true;
        if (e.name.find('/', 6) != std::string::npos && !f.empty())  // (a sub folder: the pack root is one up)
          folder = fs::path(f).parent_path().parent_path().wstring();
      }
    const std::string from = in.Get("pack.from");
    if (!mod && !from.empty() && fs::exists(fs::path(Wide(from)) / L"pack.txt")) g_star.pack = Wide(from);
    else if (any) g_star.pack = folder;
  }
  g_star.song = file("song", nullptr);
  g_star.movie = file("movie", nullptr);
  g_star.voice = file("voice", nullptr);
  for (int a = 1; a < 4; ++a) {
    g_star.attires[a] = file(("attire" + std::to_string(a + 1)).c_str(), nullptr);
    std::snprintf(g_star.attire_names[a], sizeof g_star.attire_names[a], "%s",
                  in.Get(("attire" + std::to_string(a + 1) + "_name").c_str()).c_str());
  }
  Image img;
  if (mod) {
    if (const ZipEntry* r = in.Find("render.dds"); r && DdsDecode(r->data, img)) {
      g_star.picture.Set(img);
      if (const ZipEntry* s = in.Find("render_small.dds"); !s || !DdsDecode(s->data, g_star.picture_small)) {
        Image render, bust, icon;
        MakeRenders(img, render, bust, icon);
        g_star.picture_small = bust;
      }
    }
    for (int k = 1; k <= 4; ++k)
      if (const ZipEntry* s = in.Find("sign" + std::to_string(k) + ".dds"); s && DdsDecode(s->data, img)) {
        Picture p;
        p.Set(img);
        g_star.signs.push_back(std::move(p));
      }
  } else {
    if (in.Png("picture", img)) SetPicture(img);
    for (int k = 1; k <= 4; ++k)
      if (in.Png(("sign" + std::to_string(k)).c_str(), img)) {
        Picture p;
        p.Set(img);
        g_star.signs.push_back(std::move(p));
      }
  }
  if (mod && g_star.model.empty()) Log("The mod has no ch.pac: pick a model.");
  return true;
}

}  // namespace

void TestStart(int id, const std::wstring& model, const std::string& name, const std::wstring& picture,
               const std::wstring& save) {
  g_test_id = id, g_test_model = model, g_test_name = name, g_test_picture = picture, g_test_save = save;
}

void SetModel(const std::wstring& ch_pac) {
  g_star.model = ch_pac;
  Touch();
}

void TestHeight(float scale) { g_star.height = std::clamp(scale, 0.8f, 1.25f); }

void TestFiles(const std::wstring& song, const std::wstring& movie, const std::wstring& voice, int call) {
  if (!song.empty()) g_star.song = song;
  if (!movie.empty()) g_star.movie = movie;
  if (!voice.empty()) g_star.voice = voice;
  if (call >= 0) g_star.call = call;
}

PageHooks hooks = {
    "superstar", Draw, Problems, Reset, StateText, WriteProject,
    [](const ProjectIn& in) { return Read(in, false); },
    [](const ProjectIn& in) { return Read(in, true); },
};

}  // namespace star_page
}  // namespace mm
