// The mod doctor (mod_check.h). Every rule here is the game's or the launcher's (file:line in the comments refer to
// port/src and port/launcher); messages say what goes wrong in the game and what to do.
#include "mod_check.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

#include "svrfmt/pac.h"
#include "svrfmt/texture.h"
#include "svrfmt/zip_write.h"

namespace mm {

namespace fs = std::filesystem;
using namespace svrfmt;

namespace {

struct Manifest {
  bool found = false, bom = false;
  std::vector<std::pair<std::string, std::string>> keys;  // in order, raw values (untrimmed)
  std::vector<std::string> long_lines;
  std::string Get(const std::string& k) const {
    for (const auto& [a, b] : keys)
      if (a == k) return b;
    return "";
  }
  bool Has(const std::string& k) const {
    for (const auto& [a, b] : keys)
      if (a == k) return true;
    return false;
  }
};

Manifest ReadManifest(const fs::path& dir) {
  Manifest m;
  Bytes b;
  if (!ReadFile(PathStr(dir / "manifest.txt"), b)) return m;
  m.found = true;
  size_t at = 0;
  if (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) m.bom = true, at = 3;
  std::string text(b.begin() + long(at), b.end());
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() > 510) m.long_lines.push_back(line.substr(0, 40));
    if (line.empty() || line[0] == '#') continue;
    const size_t e = line.find('=');
    if (e == std::string::npos) continue;
    m.keys.push_back({line.substr(0, e), line.substr(e + 1)});
  }
  return m;
}

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  return s;
}
bool Exists(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}
std::string Ext(const std::string& f) { return Lower(PathStr(fs::u8path(f).extension())); }
bool Ascii(const std::string& s) {
  for (unsigned char c : s)
    if (c >= 0x80) return false;
  return true;
}
bool IsInt(const std::string& s, long lo, long hi, long* out = nullptr) {
  if (s.empty()) return false;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (*end) return false;
  if (out) *out = v;
  return v >= lo && v <= hi;
}

// A DDS picture's size and format ("" fine, else what's off) against w x h and the formats allowed.
std::string DdsIssue(const fs::path& f, int w, int h, std::initializer_list<DxtFormat> formats, const char* format_name) {
  Bytes b;
  DdsInfo info;
  if (!ReadFile(PathStr(f), b) || !DdsInfoOf(b, info)) return "isn't a DDS picture";
  std::string issue;
  if (w && (info.w != w || info.h != h)) issue = "is " + std::to_string(info.w) + " x " + std::to_string(info.h) + " (needs " + std::to_string(w) + " x " + std::to_string(h) + ")";
  bool fmt_ok = formats.size() == 0;
  for (DxtFormat x : formats) fmt_ok |= info.format == x;
  if (!fmt_ok) issue += std::string(issue.empty() ? "is" : ", and") + " not " + format_name;
  return issue;
}

// Bink 1 movie: "BIK", width at +0x14, height at +0x18.
bool BinkSize(const fs::path& f, int& w, int& h) {
  FILE* fp = _wfopen(f.c_str(), L"rb");
  if (!fp) return false;
  uint8_t head[0x1C] = {};
  const bool ok = std::fread(head, 1, sizeof head, fp) == sizeof head;
  std::fclose(fp);
  if (!ok || std::memcmp(head, "BIK", 3)) return false;
  w = int(head[0x14] | head[0x15] << 8), h = int(head[0x18] | head[0x19] << 8);
  return true;
}

bool PacIsEpk8WithModels(const fs::path& f, bool want_attire_entry = false) {
  Bytes b;
  Epac e;
  if (!ReadFile(PathStr(f), b) || b.size() < 0x4000 || std::memcmp(b.data(), "EPK8", 4) || !EpacRead(b, e)) return false;
  for (const auto& g : e.groups) {
    if (g.type.rfind("EMD", 0) != 0) continue;
    for (const auto& en : g.entries)
      if (en.name.size() >= 8 && std::all_of(en.name.begin(), en.name.begin() + 8, [](char c) { return std::isdigit(uint8_t(c)); }) &&
          (!want_attire_entry || en.name[7] == '2'))
        return true;
  }
  return false;
}

const char* const kPcThemes[] = {".mp3", ".wma", ".m4a", ".aac", ".wav", ".flac"};
bool PcTheme(const std::string& ext) { return std::find(std::begin(kPcThemes), std::end(kPcThemes), ext) != std::end(kPcThemes); }

// A theme / song key's file: there, plays on PC.
void SongCheck(const fs::path& dir, const std::string& key, const std::string& raw, std::vector<Problem>& p) {
  const std::string f = Trim(raw);
  if (f.empty()) return;
  if (!Exists(dir / fs::u8path(f))) {
    p.push_back(Error(key + "=" + f + ": that file isn't in the mod.", "Put the file in the mod or fix the name."));
    return;
  }
  const std::string e = Ext(f);
  if (e == ".ogg") p.push_back(Warning(key + "=" + f + ": OGG plays on Android only - on PC it's silent.", "Use MP3, M4A, WAV, FLAC or WMA."));
  else if (!PcTheme(e)) p.push_back(Warning(key + "=" + f + ": the game can't play " + e + " files.", "Use MP3, M4A, WAV, FLAC or WMA."));
}

// A movie key's file: .bik (320 x 320 for a titantron), or .mp4 with a .bik of the same name beside it.
void MovieCheck(const fs::path& dir, const std::string& key, const std::string& raw, bool titantron, std::vector<Problem>& p) {
  const std::string f = Trim(raw);
  if (f.empty()) return;
  const fs::path path = dir / fs::u8path(f);
  const std::string e = Ext(f);
  if (!Exists(path)) {
    p.push_back(Error(key + "=" + f + ": that file isn't in the mod.", "Put the movie in the mod or fix the name."));
    return;
  }
  if (e == ".mp4") {
    fs::path bik = path;
    bik.replace_extension(".bik");
    if (!Exists(bik)) p.push_back(Warning(key + "=" + f + ": an MP4 needs a .bik of the same name beside it (the launcher makes one when it installs the .svrmod).", "Install it through the launcher, or add the .bik."));
    return;
  }
  if (e != ".bik") {
    p.push_back(Warning(key + "=" + f + ": the game plays only Bink (.bik) movies, or an .mp4 with its .bik.", "Make a .bik (the launcher's Movies tab, or From a video... in the Mod Maker)."));
    return;
  }
  int w = 0, h = 0;
  if (!BinkSize(path, w, h)) p.push_back(Error(key + "=" + f + ": that isn't a Bink movie (Bink 1, \"BIK\").", "Make it again (Movies tab / From a video...)."));
  else if (titantron && (w != 320 || h != 320))
    p.push_back(Warning(key + "=" + f + ": " + std::to_string(w) + " x " + std::to_string(h) + " - the big screen is 320 x 320 (it plays stretched).", "Make it 320 x 320."));
}

// Keys whose value is a file name: a trailing space makes the game miss the file (superstar / arena / media
// manifests aren't trimmed by the game).
void TrailingSpaces(const Manifest& m, std::vector<Problem>& p) {
  for (const auto& [k, v] : m.keys)
    if (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
      p.push_back(Warning(k + "= ends with a space: the game reads it with the space and may not find the file.", "Remove the space at the end of the line."));
}

void ManifestBasics(const Manifest& m, const std::string& want_type, std::vector<Problem>& p) {
  if (!m.found) {
    p.push_back(Error("No manifest.txt: the launcher won't install it.", "Save the mod again with the Mod Maker."));
    return;
  }
  if (m.bom) p.push_back(Warning("manifest.txt starts with a UTF-8 marker (BOM): its first line (" + (m.keys.empty() ? std::string("?") : m.keys.front().first) + "=) is lost.",
                                 "Save it as UTF-8 without BOM (Notepad: UTF-8, not UTF-8 with BOM)."));
  for (const auto& l : m.long_lines) p.push_back(Warning("A manifest line over 510 characters (" + l + "...): the game splits it.", "Shorten the line."));
  const std::string type = Lower(Trim(m.Get("type")));
  if (!want_type.empty() && type != want_type)
    p.push_back(Error("type=" + (type.empty() ? std::string("(none)") : type) + ": this folder holds " + want_type + " mods.", "Set type=" + want_type + "."));
  const std::string id = Trim(m.Get("id"));
  if (id.empty()) p.push_back(Error("No id=: the launcher won't install it.", "Add id=<short_name> (letters, digits, _)."));
  else if (id == "." || id == ".." || id.find_first_of("/\\:") != std::string::npos)
    p.push_back(Error("id=" + id + " isn't a usable folder name.", "Use letters, digits and _ only."));
}

void UnknownKeys(const Manifest& m, std::initializer_list<const char*> known, std::initializer_list<const char*> prefixes, std::vector<Problem>& p) {
  static const char* const kCommon[] = {"type", "id", "name", "author", "version", "made_with", "description"};
  std::string unknown;
  for (const auto& [k, v] : m.keys) {
    bool ok = std::find_if(std::begin(kCommon), std::end(kCommon), [&](const char* c) { return k == c; }) != std::end(kCommon);
    for (const char* c : known) ok |= k == c;
    for (const char* c : prefixes) ok |= k.rfind(c, 0) == 0;
    if (!ok) unknown += (unknown.empty() ? "" : ", ") + k;
  }
  if (!unknown.empty())
    p.push_back(Problem{false, "The game doesn't read " + unknown + " (a typo?).", "Check the key names (the manual's .svrmod format page)."});
}

std::set<std::string> LeftOut(const fs::path& superstars) {
  std::set<std::string> s;
  Bytes b;
  if (!ReadFile(PathStr(superstars / "left_out.txt"), b)) return s;
  std::istringstream in(std::string(b.begin(), b.end()));
  std::string l;
  while (std::getline(in, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (!l.empty()) s.insert(l);
  }
  return s;
}

// ---- per type

void CheckPackTxt(const fs::path& dir, const std::string& who, std::vector<Problem>& p) {
  Bytes b;
  if (!ReadFile(PathStr(dir / "pack.txt"), b)) {
    p.push_back(Error(who + ": no pack.txt - the game doesn't see a move pack.", "Save the pack again (Moves page)."));
    return;
  }
  const fs::path pacdir = fs::path(g_game) / L"pac";
  std::istringstream in(std::string(b.begin(), b.end()));
  std::string line;
  int n = 0, missing = 0, bad = 0;
  std::string first_missing, fatal;
  auto is_hex = [](const std::string& s, size_t len) { return s.size() == len && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(uint8_t(c)); }); };
  while (std::getline(in, line)) {
    ++n;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ls(line);
    std::vector<std::string> t;
    for (std::string w; ls >> w;) t.push_back(w);
    if (t.empty() || t[0][0] == '#') continue;
    const std::string& kind = t[0];
    if (kind == "motion") {
      if (t.size() < 8) { ++bad; continue; }
      if (!Exists(dir / fs::u8path(t[7]))) { if (!missing++) first_missing = t[7]; }
      if (!g_game.empty() && !Exists(pacdir / fs::u8path(t[1]))) fatal = t[1];
    } else if (kind == "pacentry") {
      if (t.size() < 5 || t[3].size() > 8 || Ext(t[1]) != ".pac" || t[1].find("..") != std::string::npos) { ++bad; continue; }
      if (!Exists(dir / fs::u8path(t[4]))) { if (!missing++) first_missing = t[4]; }
      if (!g_game.empty() && !Exists(pacdir / fs::u8path(t[1]))) fatal = t[1];
    } else if (kind == "waze") {
      if (t.size() < 3 || !is_hex(t[2], 32)) ++bad;
    } else if (kind == "wazecopy") {
      if (t.size() < 3 || std::atoi(t[1].c_str()) == 0 || t[1] == t[2]) ++bad;
    } else if (kind == "wazename") {
      if (t.size() < 3) ++bad;
    } else if (kind == "exh") {
      if (t.size() < 3 || !is_hex(t[2], 72)) ++bad;
    } else if (kind == "evt") {
      if (t.size() < 4 || !is_hex(t[2], 32)) ++bad;
    } else if (kind == "mbd") {
      if (t.size() < 3 || !is_hex(t[2], 16)) ++bad;
    }
  }
  if (!fatal.empty())
    p.push_back(Error(who + ": pack.txt names pac\\" + fatal + ", which the game doesn't have - the game then plays without ANY move pack.",
                      "Fix that line (the pac's name), or remove it."));
  if (missing)
    p.push_back(Error(who + ": " + std::to_string(missing) + " file" + (missing == 1 ? "" : "s") + " pack.txt names " + (missing == 1 ? "is" : "are") +
                          " missing (" + first_missing + "): those lines are skipped.", "Save the pack again with its motions."));
  if (bad) p.push_back(Warning(who + ": " + std::to_string(bad) + " pack.txt line" + (bad == 1 ? " is" : "s are") + " incomplete or malformed: skipped.", "Fix or remove them (Moves page shows which)."));
  uintmax_t size = 0;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(dir, ec))
    if (e.is_regular_file(ec)) size += e.file_size(ec);
  if (size > 16u * 1024 * 1024) p.push_back(Warning(who + ": " + std::to_string(size >> 20) + " MB - big for a move pack (the game merges it at every start).", "Leave out motions no move uses."));
}

void CheckSuperstar(const fs::path& dir, std::vector<Problem>& p) {
  const Manifest m = ReadManifest(dir);
  ManifestBasics(m, "superstar", p);
  TrailingSpaces(m, p);
  UnknownKeys(m, {"short", "base", "song", "music", "theme_gain", "movie", "voice", "style", "ratings", "call", "hometown", "announcer", "height", "pose",
                  "entrance", "abilities", "moves", "pack", "game_look"},
              {"attire"}, p);
  if (const std::string mu = Trim(m.Get("music")); !mu.empty()) {  // (a theme of the game's own, by its music id)
    const int v = std::atoi(mu.c_str());
    if (v < 1 || v > 253) p.push_back(Warning("music=" + mu + " isn't one of the game's themes (1 - 253): the base's theme plays.", "Pick a theme on the Superstar page (A game theme...)."));
    else if (!Trim(m.Get("song")).empty()) p.push_back(Warning("It has both song= and music=: the song file plays.", "Keep one of the two."));
  }
  const fs::path ch = dir / "ch.pac";
  if (!Exists(ch)) p.push_back(Error("No ch.pac: the game skips this superstar.", "Add the model (Superstar page > Model)."));
  else if (!PacIsEpk8WithModels(ch)) p.push_back(Error("ch.pac isn't a character model (EPK8 with EMD models): the game skips this superstar.", "Pick a character model pac (pac\\ch\\chNNN.pac or a converted one)."));
  else {
    BudgetProblems(ch.wstring(), "The superstar", false, p);
    ModelCheckProblems(ch.wstring(), "The superstar", p);
  }
  if (!IsInt(Trim(m.Get("base")), 100, 321))
    p.push_back(Error("base=" + Trim(m.Get("base")) + ": needs a game superstar's id, 100-321 - without it the game skips the mod.", "Pick a fighting style (Superstar page) and save again."));
  const std::string name = Trim(m.Get("name"));
  if (!Ascii(name)) p.push_back(Warning("The name has letters outside A-Z: the entrance movie and the theme may not be found in game, and the name shows wrong.", "Use plain letters (A-Z, digits)."));
  if (name.size() > 31) p.push_back(Warning("The name is " + std::to_string(name.size()) + " letters: the game keeps 31.", "Shorten it, or add a short name."));
  SongCheck(dir, "song", m.Get("song"), p);
  MovieCheck(dir, "movie", m.Get("movie"), true, p);
  if (const std::string v = Trim(m.Get("voice")); !v.empty() && !Exists(dir / fs::u8path(v)))
    p.push_back(Warning("voice=" + v + ": that file isn't in the mod (no name recording).", "Put the file in the mod or remove the key."));
  if (m.Has("ratings")) {
    int n = 0;
    std::istringstream r(m.Get("ratings"));
    for (std::string x; std::getline(r, x, ',');) ++n;
    if (n != 7) p.push_back(Warning("ratings= has " + std::to_string(n) + " numbers: the game reads 7 (grapple, submission, speed, strikes, hardcore, charisma, durability).", "Set all 7."));
  }
  if (m.Has("call") && !IsInt(Trim(m.Get("call")), 0, 83)) p.push_back(Warning("call=" + m.Get("call") + ": the announcer's calls are 0-83.", "Pick a name call in the Mod Maker."));
  if (const std::string h = Trim(m.Get("height")); !h.empty()) {
    const double v = std::atof(h.c_str());
    if (v < 0.8 || v > 1.25) p.push_back(Warning("height=" + h + ": the game keeps it within 0.80 - 1.25.", "Set a height in that range."));
  }
  auto entrance_check = [&](const std::string& key) {
    long n = 0;
    if (!IsInt(Trim(m.Get(key)), 1, 65535, &n) || n < 100 || !EntrancesReady()) return;
    for (const auto& e : Entrances())
      if (e.number == n) return;
    if (Exists(dir / "moves" / "pack.txt")) return;  // (its own pack may add it)
    p.push_back(Warning(key + "=" + std::to_string(n) + ": no entrance of that number is installed - the superstar's own plays instead.", "Install the move pack that adds it, or pick another entrance."));
  };
  entrance_check("entrance");
  for (int a = 1; a <= 4; ++a) {
    const std::string k = "attire" + std::to_string(a);
    entrance_check(k + "_entrance");
    const std::string f = Trim(m.Get(k));
    if (f.empty()) continue;
    if (!Exists(dir / fs::u8path(f))) p.push_back(Warning(k + "=" + f + ": that file isn't in the mod (no attire " + std::to_string(a) + ").", "Put the file in the mod or fix the name."));
    else if (!PacIsEpk8WithModels(dir / fs::u8path(f))) p.push_back(Error(k + "=" + f + " isn't a character model pac.", "Pick a character model pac for it."));
    else BudgetProblems((dir / fs::u8path(f)).wstring(), "Attire " + std::to_string(a), true, p);
  }
  for (const auto& [f, size] : {std::pair<const char*, int>{"render.dds", 512}, {"render_small.dds", 256}, {"render_full.dds", 512}})
    if (Exists(dir / f))
      if (const std::string is = DdsIssue(dir / f, size, size, {}, ""); !is.empty())
        p.push_back(Warning(std::string(f) + " " + is + ": the game shows the base superstar's picture instead.", "Set the select picture again in the Mod Maker."));
  for (int k = 1; k <= 4; ++k) {
    const fs::path f = dir / ("sign" + std::to_string(k) + ".dds");
    if (!Exists(f)) continue;
    if (const std::string is = DdsIssue(f, 128, 64, {DxtFormat::kDxt1}, "DXT1"); !is.empty())
      p.push_back(Problem{false, "sign" + std::to_string(k) + ".dds " + is + " (the game's signs are 128 x 64 DXT1).", "Make the signs in the Mod Maker."});
  }
  if (const std::string mv = Trim(m.Get("moves")); !mv.empty()) {
    Bytes b;
    if (!ReadFile(PathStr(dir / fs::u8path(mv)), b)) p.push_back(Error("moves=" + mv + ": that file isn't in the mod.", "Save the moveset again."));
    else {
      std::istringstream in(std::string(b.begin(), b.end()));
      int bad = 0, unknown = 0;
      std::string first_unknown;
      const bool pack = Exists(dir / "moves" / "pack.txt");
      for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.empty() || l[0] == '#') continue;
        const size_t e = l.find('=');
        const long off = e == std::string::npos ? -1 : std::strtol(l.substr(0, e).c_str(), nullptr, 16);
        const long id = e == std::string::npos ? 0 : std::atol(l.c_str() + e + 1);
        if (off < 0 || off > 0x1BF || off % 2 || id < 1 || id > 65535) { ++bad; continue; }
        if (!MoveInfos().empty() && !MoveInfos().count(int(id)) && !pack) { if (!unknown++) first_unknown = std::to_string(id); }
      }
      if (bad) p.push_back(Warning(std::to_string(bad) + " moves= line" + (bad == 1 ? " isn't" : "s aren't") + " 0xOFF=id (even offset up to 0x1BF, id 1-65535): skipped.", "Save the moveset again."));
      if (unknown) p.push_back(Warning(std::to_string(unknown) + " move" + (unknown == 1 ? "" : "s") + " the game doesn't have (" + first_unknown + "): the style's move stays in that slot.", "Add the move pack that adds it, or pick another move."));
    }
  }
  if (Exists(dir / "moves" / "pack.txt")) CheckPackTxt(dir / "moves", "Its move pack", p);
  if (LeftOut(dir.parent_path()).count(PathStr(dir.filename())))
    p.push_back(Error("The game left this superstar out at its last start: over the 137-mod limit, or the model over the memory budget even without blood.",
                      "Switch another superstar mod off, or use a lighter model (see the budget above)."));
}

std::vector<int> StarAttiresOf(int star) {  // the attires in the stock superstar's pac
  std::vector<int> v;
  PacIndex idx;
  char id[8];
  std::snprintf(id, sizeof id, "%06d", star);
  if (ReadPacIndex(fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(star) + L".pac"), idx))
    for (const auto& e : idx.entries)
      if (e.group == "EMD " && e.name.size() == 8 && e.name.compare(0, 6, id) == 0 && e.name[7] == '2') v.push_back(e.name[6] - '0' + 1);
  return v;
}
bool GearStar(int star) {
  PacIndex idx;
  char id[8];
  std::snprintf(id, sizeof id, "%06d", star);
  if (ReadPacIndex(fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(star) + L".pac"), idx))
    for (const auto& e : idx.entries)
      if (e.group == "EMD " && e.name.size() == 8 && e.name.compare(0, 6, id) == 0 && e.name[6] == '0' && e.name[7] != '2') return true;
  return false;
}

void CheckAttire(const fs::path& dir, std::vector<Problem>& p) {
  const Manifest m = ReadManifest(dir);
  ManifestBasics(m, "attire", p);
  UnknownKeys(m, {"superstar", "attire", "model", "entrance", "gear"}, {}, p);
  long star = 0;
  if (!IsInt(Trim(m.Get("superstar")), 100, 321, &star))
    p.push_back(Error("superstar=" + Trim(m.Get("superstar")) + ": needs a game superstar's id, 100-321 - the game skips the mod.", "Pick the superstar (Attires page)."));
  const std::string at = Trim(m.Get("attire"));
  long slot = 0;
  if (at != "new" && !IsInt(at, 1, 4, &slot))
    p.push_back(Error("attire=" + at + ": needs 1-4 or new - the game skips the mod.", "Pick an attire slot (Attires page)."));
  const std::string model = Trim(m.Get("model"));
  if (!Exists(dir / "attire.pac")) p.push_back(Error("No attire.pac: the launcher won't install it" + std::string(model.empty() || model == "attire.pac" ? "." : " (it needs that name, whatever model= says)."), "Name the model attire.pac."));
  else if (!PacIsEpk8WithModels(dir / "attire.pac", true)) p.push_back(Error("attire.pac isn't a character model pac (EPK8 with an attire model).", "Pick a character model pac (Attires page)."));
  else BudgetProblems((dir / "attire.pac").wstring(), "This attire", true, p);
  if (star >= 100 && !g_game.empty() && GearStar(int(star))) {
    const auto own = StarAttiresOf(int(star));
    if (at == "new" || (slot && std::find(own.begin(), own.end(), int(slot)) == own.end()))
      p.push_back(Error("This superstar wears entrance gear: new attires for them don't load in the game - the game leaves this mod out.",
                        "Replace one of their own attires (1-" + std::to_string(own.size()) + ") instead."));
  }
  if (slot && !g_game.empty()) {  // another switched-on attire mod on the same slot (the first A-Z keeps it)
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(dir.parent_path(), ec))
      if (e.is_directory(ec) && e.path() != dir && !Exists(e.path() / "disabled")) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    for (const auto& d : dirs) {
      const Manifest o = ReadManifest(d);
      if (Trim(o.Get("superstar")) == std::to_string(star) && Trim(o.Get("attire")) == at) {
        p.push_back(Warning("Attire " + at + " of this superstar is also asked for by \"" + PathStr(d.filename()) + "\": " +
                                (d < dir ? "it comes first (A-Z) and keeps it" : "this one comes first (A-Z) and keeps it") + "; the launcher moves the newer to a free attire at Play.",
                            "Pick another attire for one of them."));
        break;
      }
    }
  }
}

const char* const kArenaBases[] = {"arena_SD", "arena_RAW", "arena_WM26", "arena_slam", "arena_SS", "arena_HinC", "arena_Judg",
                                   "arena_BASH", "arena_NoC", "arena_BP", "arena_RR", "arena_BR", "arena_Series", "arena_TLC",
                                   "arena_ttt", "arena_EC", "arena_back", "arena_EXT", "arena_ECW", "arena_drui"};

void CheckArenaPac(const fs::path& f, std::vector<Problem>& p) {
  Bytes b;
  Epac e;
  if (!Exists(f)) p.push_back(Error("No arena.pac: the game skips it.", "Save the arena again."));
  else if (!ReadFile(PathStr(f), b) || b.size() < 0x800 || std::memcmp(b.data(), "EPAC", 4) || !EpacRead(b, e))
    p.push_back(Error("arena.pac isn't an arena pac (EPAC).", "Save the arena again."));
}

void CheckArena(const fs::path& dir, std::vector<Problem>& p) {
  const Manifest m = ReadManifest(dir);
  if (m.found) ManifestBasics(m, "arena", p), TrailingSpaces(m, p);
  CheckArenaPac(dir / "arena.pac", p);
  if (!Exists(dir / "banner.dds")) p.push_back(Error("No banner.dds: the launcher won't install it (and the select tile is dark).", "Set the arena's banner (Arena page)."));
  else if (const std::string is = DdsIssue(dir / "banner.dds", 256, 128, {DxtFormat::kDxt5}, "DXT5"); !is.empty())
    p.push_back(Warning("banner.dds " + is + ": the select tile shows dark.", "Set the banner again (Arena page)."));
  if (Exists(dir / "load.dds"))
    if (const std::string is = DdsIssue(dir / "load.dds", 1024, 512, {DxtFormat::kDxt5}, "DXT5"); !is.empty())
      p.push_back(Warning("load.dds " + is + ": the loading screen doesn't use it.", "Set the loading picture again."));
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir / "vs", ec))
    if (e.is_regular_file(ec) && Ext(PathStr(e.path())) == ".dds")
      if (const std::string is = DdsIssue(e.path(), 0, 0, {DxtFormat::kDxt5}, "DXT5"); !is.empty())
        p.push_back(Warning("vs\\" + PathStr(e.path().filename()) + " " + is + ": the VS screen skips it.", "Make it DXT5 (Arena page)."));
  if (const std::string src = Trim(m.Get("source")); SpecialStage(src)) {  // (converted from a WWE '12+ special stage)
    if (ArenaPacNumber(src) == 78)
      p.push_back(Error("It was made from " + src + ", a backstage room, not an arena: it crashes on an arena slot.", "Remove it; port an arena (bg00 - bg59) instead."));
    else
      p.push_back(Warning("It was made from " + src + ", a backstage room or studio stage, not an arena: in a match it shows a white mat over "
                          "a black void.", "Port an arena (bg00 - bg59) instead."));
  }
  if (const std::string base = Trim(m.Get("base")); !base.empty() &&
      std::find_if(std::begin(kArenaBases), std::end(kArenaBases), [&](const char* x) { return base == x; }) == std::end(kArenaBases))
    p.push_back(Problem{false, "base=" + base + " isn't one of the 20 arena tiles: the arena goes on an extra page.", "Pick the arena it plays in place of (Arena page)."});
  if (const std::string g = Trim(m.Get("ring.rope_base")); !g.empty() && (std::atof(g.c_str()) < 1.0 || std::atof(g.c_str()) > 8.0))
    p.push_back(Warning("ring.rope_base=" + g + ": the game takes 1.0 - 8.0 (else its own 3.4).", "Set it in range."));
  if (const std::string g = Trim(m.Get("ring.rope_gap")); !g.empty() && (std::atof(g.c_str()) < 1.5 || std::atof(g.c_str()) > 6.0))
    p.push_back(Warning("ring.rope_gap=" + g + ": the game takes 1.5 - 6.0 (else its own 4.2).", "Set it in range."));
}

void CheckBackstage(const fs::path& dir, std::vector<Problem>& p) {
  const Manifest m = ReadManifest(dir);
  ManifestBasics(m, "backstage", p);
  CheckArenaPac(dir / "arena.pac", p);
  if (m.Has("row")) {
    if (!IsInt(Trim(m.Get("area")), 0, 6)) p.push_back(Error("area=" + Trim(m.Get("area")) + ": a backstage row needs area 0-6 - the game drops it.", "Pick the area (Backstage page)."));
  }
  if (const std::string g = Trim(m.Get("gimmick")); !g.empty()) {
    if (g.size() != 4) p.push_back(Warning("gimmick=" + g + ": needs exactly 4 letters - no gimmicks.", "Set the 4-letter gimmick id."));
    if (!Exists(dir / "gimmick.pac")) p.push_back(Warning("gimmick= without gimmick.pac: no gimmicks.", "Add gimmick.pac."));
  }
}

void CheckSigns(const fs::path& dir, std::vector<Problem>& p) {
  std::error_code ec;
  int n = 0, upper = 0;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string ext = PathStr(e.path().extension());
    if (Lower(ext) != ".dds") continue;
    if (ext != ".dds") { ++upper; continue; }
    ++n;
    if (const std::string is = DdsIssue(e.path(), 128, 64, {DxtFormat::kDxt1}, "DXT1"); !is.empty())
      p.push_back(is.rfind("isn't", 0) == 0 ? Warning(PathStr(e.path().filename()) + " isn't a DDS picture: the game skips it.", "Make the sign again (Crowd signs page).")
                                            : Problem{false, PathStr(e.path().filename()) + " " + is + " (the game's signs are 128 x 64 DXT1).", "Make the sign in the Mod Maker."});
  }
  if (upper) p.push_back(Warning(std::to_string(upper) + " sign file" + (upper == 1 ? " ends" : "s end") + " in .DDS (capitals): the game only reads .dds.", "Rename to .dds."));
  if (!n && !upper) p.push_back(Error("No .dds signs in the pack.", "Add signs (Crowd signs page)."));
}

void CheckMedia(const fs::path& dir, std::vector<Problem>& p) {
  const Manifest m = ReadManifest(dir);
  ManifestBasics(m, "media", p);
  TrailingSpaces(m, p);
  int low = 0;
  for (const auto& [k, v] : m.keys) {
    const size_t dot = k.find('.');
    const std::string what = dot == std::string::npos ? k : k.substr(0, dot), id = dot == std::string::npos ? "" : k.substr(dot + 1);
    if (!id.empty() && std::isdigit(uint8_t(id[0])) && std::atoi(id.c_str()) < 50 && (what == "video" || what == "theme")) ++low;
    if (what == "video") MovieCheck(dir, k, v, true, p);
    else if (what == "theme" || k == "menu_music" || what == "sound") SongCheck(dir, k, v, p);
    else if (what == "render" || what == "bust" || what == "icon") {
      const int size = what == "render" ? 512 : what == "bust" ? 256 : 64;
      const std::string f = Trim(v);
      if (!Exists(dir / fs::u8path(f))) p.push_back(Error(k + "=" + f + ": that file isn't in the mod.", "Put the picture in the mod or fix the name."));
      else if (const std::string is = DdsIssue(dir / fs::u8path(f), size, size, {}, ""); !is.empty())
        p.push_back(Warning(k + "=" + f + " " + is + ".", "Make the picture again (Media page)."));
    } else if (what == "arena") {
      const std::string f = Trim(v);
      if (!Exists(dir / fs::u8path(f))) p.push_back(Error(k + "=" + f + ": that file isn't in the mod.", "Put the arena pac in the mod or fix the name."));
    }
  }
  if (low) p.push_back(Warning(std::to_string(low) + " video / theme key" + (low == 1 ? " is" : "s are") + " for ids under 50 (Created Superstars): the game doesn't apply those.", "Use a superstar's id (100 and up)."));
}

void CheckBackgrounds(const fs::path& dir, std::vector<Problem>& p) {
  std::error_code ec;
  int n = 0;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec) || Lower(PathStr(e.path().extension())) != ".dds") continue;
    ++n;
    if (const std::string is = DdsIssue(e.path(), 1024, 512, {DxtFormat::kDxt5}, "DXT5"); !is.empty())
      p.push_back(Warning(PathStr(e.path().filename()) + " " + is + ": the game doesn't use it.", "Make the background again (Menu backgrounds page)."));
  }
  if (!n) p.push_back(Error("No .dds pictures: the launcher won't install it.", "Add pictures (Menu backgrounds page)."));
}

void CheckMatchType(const fs::path& dir, std::vector<Problem>& p) {
  static const char* const kIds[] = {"falls_count_anywhere", "championship_scramble", "royal_rumble_15_25", "lumberjack", "free_roaming_backstage",
                                     "backstage_more_people", "weapons_everywhere", "slobber_knocker", "three_stages_of_hell", "elimination",
                                     "mystery_opponent", "hiac_tables_ladders", "hiac_roof_start", "gauntlet_eliminator", "cage_weapons",
                                     "inferno_weapons", "online_first_blood"};
  const std::string f = PathStr(dir.filename());
  if (std::find_if(std::begin(kIds), std::end(kIds), [&](const char* x) { return f == x; }) == std::end(kIds))
    p.push_back(Warning("The game doesn't know a match type named \"" + f + "\": it does nothing (custom match types aren't in the game yet).", "Use the Match types page's built-in ones."));
}

}  // namespace

std::string KindOfType(const std::string& type) {
  const std::string t = Lower(Trim(type));
  if (t == "superstar") return "Superstars";
  if (t == "attire") return "Attires";
  if (t == "arena") return "Arenas";
  if (t == "moves") return "Moves";
  if (t == "backstage") return "Backstage";
  if (t == "signs") return "Signs";
  if (t == "media") return "Media";
  if (t == "background") return "Backgrounds";
  if (t == "matchtype") return "MatchTypes";
  return "";
}

std::vector<Problem> CheckModFolder(const std::string& kind, const fs::path& dir) {
  std::vector<Problem> p;
  if (kind == "Superstars") CheckSuperstar(dir, p);
  else if (kind == "Attires") CheckAttire(dir, p);
  else if (kind == "Arenas") CheckArena(dir, p);
  else if (kind == "Moves") {
    CheckPackTxt(dir, "The pack", p);
    p.push_back(Problem{false, "Move packs don't install on Android yet (the phone's Mods page takes them for arenas).", "Play it on PC; or carry it inside a superstar mod."});
  } else if (kind == "Backstage") CheckBackstage(dir, p);
  else if (kind == "Signs") CheckSigns(dir, p);
  else if (kind == "Media") CheckMedia(dir, p);
  else if (kind == "Backgrounds") CheckBackgrounds(dir, p);
  else if (kind == "MatchTypes") CheckMatchType(dir, p);
  if (Exists(dir / "disabled")) p.push_back(Problem{false, "It's switched off (a \"disabled\" file): the game doesn't load it.", "Switch it on in the launcher's Mods tab."});
  return p;
}

std::vector<Problem> CheckInstalledSet() {
  std::vector<Problem> p;
  if (g_game.empty()) return p;
  const fs::path mods = fs::path(g_game) / L"Mods";
  std::error_code ec;
  auto on_dirs = [&](const char* kind) {
    std::vector<fs::path> v;
    for (const auto& e : fs::directory_iterator(mods / kind, ec))
      if (e.is_directory(ec) && !Exists(e.path() / "disabled")) v.push_back(e.path());
    return v;
  };
  const auto stars = on_dirs("Superstars");
  int with_pac = 0, movies = 0;
  for (const auto& d : stars) {
    with_pac += Exists(d / "ch.pac");
    if (!Trim(ReadManifest(d).Get("movie")).empty()) ++movies;
  }
  if (with_pac > 137) p.push_back(Error(std::to_string(with_pac) + " superstar mods are on: the game takes 137 and leaves the rest out.", "Switch some off in the launcher's Mods tab."));
  const auto left = LeftOut(mods / "Superstars");
  if (!left.empty()) {
    std::string l;
    for (const auto& f : left) l += (l.empty() ? "" : ", ") + f;
    p.push_back(Error("At its last start the game left out: " + l + " (over the limit or a model over the memory budget).", "Select each to see why; switch others off or use lighter models."));
  }
  for (const auto& d : on_dirs("Media")) {
    const Manifest m = ReadManifest(d);
    for (const auto& [k, v] : m.keys) movies += k.rfind("video.", 0) == 0;
  }
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Custom Movies", ec)) movies += e.is_regular_file(ec);
  if (movies > 200) p.push_back(Warning(std::to_string(movies) + " movies from mods and Custom Movies: the game has room for 200 - the rest don't play.", "Switch some off, or remove some Custom Movies."));
  int signs = 0;
  for (const auto& d : on_dirs("Signs"))
    for (const auto& e : fs::directory_iterator(d, ec)) signs += e.is_regular_file(ec) && PathStr(e.path().extension()) == ".dds";
  if (signs > 400) p.push_back(Warning(std::to_string(signs) + " crowd signs in sign packs: the game takes 400.", "Switch a sign pack off."));
  int rows = 0, plain = 0;
  for (const auto& d : on_dirs("Backstage")) (ReadManifest(d).Has("row") ? rows : plain) += 1;
  if (rows > 8) p.push_back(Warning(std::to_string(rows) + " backstage room mods with their own row: the game takes 8.", "Switch some off."));
  if (plain > 1) p.push_back(Warning(std::to_string(plain) + " backstage area mods without a row: only the last by name plays.", "Switch the others off."));
  return p;
}

std::string InstallReport(const std::string& kind, const fs::path& dir) {
  // (on the UI thread: the model budget caches aren't shared with the install's worker)
  OnUiThread([kind, dir] {
    const auto p = CheckModFolder(kind, dir);
    int errors = 0, warnings = 0;
    for (const auto& x : p) {
      if (x.error) ++errors;
      else ++warnings;
      Log(std::string(x.error ? "  must fix: " : "  check: ") + x.text + (x.fix.empty() ? "" : " - " + x.fix));
    }
    if (errors || warnings)
      Status("Installed - the check found " + (errors ? std::to_string(errors) + " to fix" + (warnings ? ", " : "") : std::string()) +
             (warnings ? std::to_string(warnings) + " to look at" : std::string()) + " (in the log, and on the Installed mods page).");
  });
  return "";
}

std::vector<Problem> CheckSvrmod(const std::wstring& file) {
  std::vector<Problem> p;
  Bytes zip;
  std::vector<ZipEntry> files;
  if (!ReadFile(Utf8(file), zip) || !ZipRead(zip, files)) {
    p.push_back(Error("It isn't a .svrmod (zip) the launcher can open.", "Save the mod again."));
    return p;
  }
  // unpacked into the cache (the checks read files)
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path(ec) / L"svr2011_modcheck" / fs::path(file).stem();
  fs::remove_all(dir, ec);
  for (const auto& f : files) {
    if (f.name.find("..") != std::string::npos || f.name.find(':') != std::string::npos) {
      p.push_back(Error("The zip has an entry the launcher refuses (" + f.name + ").", "Save the mod again."));
      continue;
    }
    const fs::path out = dir / fs::u8path(f.name);
    fs::create_directories(out.parent_path(), ec);
    WriteFile(PathStr(out), f.data);
  }
  const Manifest m = ReadManifest(dir);
  const std::string kind = KindOfType(m.Get("type"));
  if (!m.found) p.push_back(Error("No manifest.txt at the zip's top: the launcher won't install it.", "Save the mod again with the Mod Maker."));
  else if (kind.empty()) p.push_back(Error("type=" + m.Get("type") + ": the launcher doesn't install that type.", "Save the mod again with the Mod Maker."));
  else {
    auto more = CheckModFolder(kind, dir);
    p.insert(p.end(), more.begin(), more.end());
  }
  fs::remove_all(dir, ec);
  return p;
}

}  // namespace mm
