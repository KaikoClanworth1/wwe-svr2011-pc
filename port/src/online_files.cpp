// What online needs to trust - see online_files.h.
#include "online_files.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "match_types.h"
#include "online_net.h"

#ifndef SVR2011_PORT_VERSION
#define SVR2011_PORT_VERSION "dev"
#endif

namespace fs = std::filesystem;

namespace {

// -- The launcher's list --------------------------------------------------

#define VERIFY_MENU_TABLE 1  // (as launcher/verify.c)
constexpr uint64_t kMenuTableAt = 0x25F000, kMenuTableSize = 0x6800;
struct GameFile {
  const wchar_t* path;
  uint64_t size;
  uint32_t crc, crc_ported;
  int flags;
};
const GameFile kFiles[] = {
#include "../launcher/game_files_crc.inc"
};

// Files a game folder may lack (as the launcher): the disc's system update and dashboard art.
bool Optional(const std::wstring& p) { return p.rfind(L"$SystemUpdate\\", 0) == 0 || p == L"nxeart"; }

// -- CRC-32 (slicing by 8) ------------------------------------------------

uint32_t g_crc[8][256];
void CrcInit() {
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    g_crc[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; ++i)
    for (int t = 1; t < 8; ++t) g_crc[t][i] = g_crc[0][g_crc[t - 1][i] & 0xFF] ^ (g_crc[t - 1][i] >> 8);
}
uint32_t CrcUpdate(uint32_t c, const uint8_t* p, size_t n) {
  c = ~c;
  while (n >= 8) {
    uint32_t a, b;
    std::memcpy(&a, p, 4);
    std::memcpy(&b, p + 4, 4);
    a ^= c;
    c = g_crc[7][a & 0xFF] ^ g_crc[6][(a >> 8) & 0xFF] ^ g_crc[5][(a >> 16) & 0xFF] ^ g_crc[4][a >> 24] ^
        g_crc[3][b & 0xFF] ^ g_crc[2][(b >> 8) & 0xFF] ^ g_crc[1][(b >> 16) & 0xFF] ^ g_crc[0][b >> 24];
    p += 8, n -= 8;
  }
  while (n--) c = g_crc[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
  return ~c;
}
std::once_flag g_crc_once;

// A file's CRC-32 (menu.pac: its menu table left out); false if unreadable.
bool FileCrc(const fs::path& p, bool skip_menu_table, uint32_t* out) {
  std::call_once(g_crc_once, CrcInit);
  FILE* f = std::fopen(p.string().c_str(), "rb");
  if (!f) return false;
  std::vector<uint8_t> buf(1 << 20);
  uint32_t c = 0;
  uint64_t at = 0;
  for (size_t got; (got = std::fread(buf.data(), 1, buf.size(), f)) > 0; at += got) {
    if (!skip_menu_table || at + got <= kMenuTableAt || at >= kMenuTableAt + kMenuTableSize) {
      c = CrcUpdate(c, buf.data(), got);
      continue;
    }
    if (at < kMenuTableAt) c = CrcUpdate(c, buf.data(), size_t(kMenuTableAt - at));
    if (at + got > kMenuTableAt + kMenuTableSize) {
      const size_t from = size_t(kMenuTableAt + kMenuTableSize - at);
      c = CrcUpdate(c, buf.data() + from, got - from);
    }
  }
  std::fclose(f);
  *out = c;
  return true;
}

uint64_t MTime(const fs::path& p) {
  std::error_code ec;
  const auto t = fs::last_write_time(p, ec);
  return ec ? 0 : uint64_t(t.time_since_epoch().count());
}

// -- The check ------------------------------------------------------------

std::atomic<svr2011::GameFiles> g_state{svr2011::GameFiles::kChecking};
std::mutex g_bad_mutex;
std::string g_first_bad;

struct Cached {
  uint64_t size = 0, mtime = 0;
  uint32_t crc = 0;
};

void Check(fs::path game, fs::path user_data) {
  const fs::path cache_file = user_data / "game_files_check.txt";
  std::map<std::string, Cached> cache;
  if (std::ifstream in(cache_file); in) {
    std::string line;
    while (std::getline(in, line)) {
      std::istringstream s(line);
      std::string path;
      Cached c;
      if (std::getline(s, path, '\t') && (s >> c.size >> c.mtime >> std::hex >> c.crc)) cache[path] = c;
    }
  }
  std::map<std::string, Cached> now;
  const auto t0 = std::chrono::steady_clock::now();
  int read = 0, bad = 0;
  std::string first_bad;
  for (const GameFile& g : kFiles) {
    std::wstring wpath = g.path;
    std::string rel(wpath.begin(), wpath.end());  // (the list is ASCII)
    std::replace(rel.begin(), rel.end(), '\\', '/');
    const fs::path p = game / fs::path(rel);
    std::error_code ec;
    const uint64_t size = fs::file_size(p, ec);
    if (ec) {
      if (!Optional(wpath) && !bad++) first_bad = rel + " (missing)";
      continue;
    }
    Cached c{size, MTime(p), 0};
    if (auto it = cache.find(rel); it != cache.end() && it->second.size == c.size && it->second.mtime == c.mtime) {
      c.crc = it->second.crc;
    } else if (size == g.size) {
      if (!FileCrc(p, g.flags & VERIFY_MENU_TABLE, &c.crc)) c.crc = ~g.crc;
      ++read;
    }
    now[rel] = c;
    if (size != g.size || (c.crc != g.crc && (!g.crc_ported || c.crc != g.crc_ported)))
      if (!bad++) first_bad = rel;
  }
  if (std::ofstream out(cache_file, std::ios::trunc); out)
    for (const auto& [path, c] : now) out << path << '\t' << c.size << ' ' << c.mtime << ' ' << std::hex << c.crc << std::dec << '\n';
  {
    std::lock_guard lock(g_bad_mutex);
    g_first_bad = first_bad;
  }
  g_state = bad ? svr2011::GameFiles::kModified : svr2011::GameFiles::kClean;
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (bad)
    REXLOG_WARN("online: game files modified ({} differ, first {}) - online is off ({:.1f} s, {} files read)", bad,
                first_bad, s, read);
  else
    REXLOG_INFO("online: game files clean ({} files, {} read, {:.1f} s)", std::size(kFiles), read, s);
}

// -- The newest version ---------------------------------------------------

constexpr const char* kReleases = "https://api.github.com/repos/KaikoClanworth1/wwe-svr2011-pc/releases?per_page=10";
constexpr int64_t kVersionCacheSeconds = 6 * 3600;
enum class Version { kChecking, kLatest, kOlder };
std::atomic<Version> g_version{Version::kChecking};
std::mutex g_latest_mutex;
std::string g_latest;  // the newest release's version (when known)

// a newer than b (dotted numbers, as the launcher's version_newer).
bool VersionNewer(const std::string& a, const std::string& b) {
  const char *x = a.c_str(), *y = b.c_str();
  while (*x || *y) {
    char *xe, *ye;
    const long p = std::strtol(x, &xe, 10), q = std::strtol(y, &ye, 10);
    if (p != q) return p > q;
    x = xe, y = ye;
    if (*x == '.') ++x;
    if (*y == '.') ++y;
    if ((*x && !std::isdigit(static_cast<unsigned char>(*x))) || (*y && !std::isdigit(static_cast<unsigned char>(*y))))
      break;
  }
  return false;
}

void CheckVersion(fs::path user_data) {
  const std::string mine = svr2011::PortVersion();
  if (mine.empty() || !std::isdigit(static_cast<unsigned char>(mine[0]))) {  // (a developer build: no check)
    g_version = Version::kLatest;
    return;
  }
  const fs::path cache = user_data / "online_version.txt";
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
  std::string latest;
  if (std::ifstream in(cache); in) {
    int64_t at = 0;
    std::string v;
    if ((in >> at >> v) && now - at >= 0 && now - at < kVersionCacheSeconds) latest = v;
  }
  if (latest.empty()) {
    auto r = svr2011::net::Request("GET", kReleases,
                                   {{"Accept", "application/vnd.github+json"}, {"User-Agent", "SvR2011-PC-Game"}});
    // (the first release's tag: newest first, drafts not listed)
    if (r && r->status == 200)
      if (size_t at = r->body.find("\"tag_name\""); at != std::string::npos)
        if (size_t q1 = r->body.find('"', r->body.find(':', at) + 1); q1 != std::string::npos)
          if (size_t q2 = r->body.find('"', q1 + 1); q2 != std::string::npos) {
            latest = r->body.substr(q1 + 1, q2 - q1 - 1);
            if (!latest.empty() && (latest[0] == 'v' || latest[0] == 'V')) latest.erase(0, 1);
          }
    if (latest.empty()) {
      REXLOG_WARN("online: the newest version couldn't be asked (GitHub {}) - online stays on",
                  r ? std::to_string(r->status) : std::string("out of reach"));
      g_version = Version::kLatest;
      return;
    }
    if (std::ofstream out(cache, std::ios::trunc); out) out << now << ' ' << latest << '\n';
  }
  {
    std::lock_guard lock(g_latest_mutex);
    g_latest = latest;
  }
  const bool older = VersionNewer(latest, mine);
  g_version = older ? Version::kOlder : Version::kLatest;
  if (older)
    REXLOG_WARN("online: version {} - the newest is {}: online is off until the update", mine, latest);
  else
    REXLOG_INFO("online: version {} (the newest: {})", mine, latest);
}

// -- The gameplay mod set -------------------------------------------------

std::string ManifestValue(const fs::path& manifest, const char* key) {
  std::ifstream in(manifest);
  std::string line;
  const std::string want = std::string(key) + "=";
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind(want, 0) == 0) return line.substr(want.size());
  }
  return "";
}

// Pictures, sounds and movies: cosmetic (left out of a mod's CRC).
bool Cosmetic(const fs::path& p) {
  std::string e = p.extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
  for (const char* x : {".png", ".jpg", ".jpeg", ".bmp", ".dds", ".tga", ".ogg", ".wav", ".mp3", ".xma", ".bik",
                        ".wmv", ".mp4", ".db", ".ini"})
    if (e == x) return true;
  return false;
}

std::string BuildModSet() {
  std::vector<std::string> lines;
  const fs::path mods = rex::filesystem::GetExecutableFolder() / "Mods";
  std::error_code ec;
  for (const char* kind : {"Superstars", "Moves", "Arenas", "Backstage"}) {
    for (const auto& e : fs::directory_iterator(mods / kind, ec)) {
      if (!e.is_directory() || fs::exists(e.path() / "disabled", ec)) continue;
      // (the files in a stable order, then their CRCs folded)
      std::vector<fs::path> files;
      for (const auto& f : fs::recursive_directory_iterator(e.path(), ec))
        if (f.is_regular_file() && !Cosmetic(f.path())) files.push_back(f.path());
      std::sort(files.begin(), files.end());
      uint32_t crc = 0;
      for (const fs::path& f : files) {
        uint32_t c = 0;
        if (!FileCrc(f, false, &c)) continue;
        const std::string rel = fs::relative(f, e.path(), ec).generic_string();
        crc = CrcUpdate(crc, reinterpret_cast<const uint8_t*>(rel.data()), rel.size());
        crc = CrcUpdate(crc, reinterpret_cast<const uint8_t*>(&c), 4);
      }
      const fs::path manifest = e.path() / "manifest.txt";
      std::string name = ManifestValue(manifest, "name"), version = ManifestValue(manifest, "version");
      const std::string id = e.path().filename().string();
      if (name.empty()) name = id;
      if (version.empty()) version = "-";
      char hex[16];
      std::snprintf(hex, sizeof(hex), "%08X", crc);
      lines.push_back(std::string(kind) + "\t" + id + "\t" + version + "\t" + hex + "\t" + name);
    }
  }
  // The match type switches: the ones off (all on - the default - adds nothing).
  for (const char* id : {"falls_count_anywhere", "championship_scramble", "royal_rumble_15_25", "lumberjack",
                         "free_roaming_backstage", "backstage_more_people", "weapons_everywhere", "slobber_knocker",
                         "three_stages_of_hell", "elimination", "mystery_opponent"})
    if (!svr2011::MatchTypeOn(id)) lines.push_back(std::string("MatchTypes\t") + id + "\toff\t-\t" + id);
  // Test aid: SVR2011_TEST_MOD_SET_EXTRA="<kind>|<id>|<version>|<name>" - one
  // more mod in this game's set (two test games share one Mods folder).
  if (const char* v = std::getenv("SVR2011_TEST_MOD_SET_EXTRA"); v && *v) {
    std::string l = v;
    std::replace(l.begin(), l.end(), '|', '\t');
    const size_t at = l.find('\t', l.find('\t', l.find('\t') + 1) + 1);  // (after the version: a made-up CRC)
    if (at != std::string::npos) lines.push_back(l.substr(0, at) + "\t00000000" + l.substr(at));
  }
  std::sort(lines.begin(), lines.end());
  std::string out;
  for (const std::string& l : lines) out += l + "\n";
  return out;
}

struct Line {
  std::string kind, id, version, crc, name;
};
std::map<std::string, Line> ParseSet(const std::string& set) {
  std::map<std::string, Line> out;
  std::istringstream s(set);
  std::string l;
  while (std::getline(s, l)) {
    Line x;
    std::istringstream f(l);
    if (std::getline(f, x.kind, '\t') && std::getline(f, x.id, '\t') && std::getline(f, x.version, '\t') &&
        std::getline(f, x.crc, '\t') && std::getline(f, x.name))
      out[x.kind + "/" + x.id] = x;
  }
  return out;
}

std::string Describe(const Line& l) {
  if (l.kind == "MatchTypes") return l.name + " switched off";
  return l.version == "-" ? l.name : l.name + " " + l.version;
}

}  // namespace

namespace svr2011 {

namespace {
fs::path g_game_root, g_user_data;
}  // namespace

void SetGameFilesPaths(const fs::path& game_root, const fs::path& user_data) {
  g_game_root = game_root;
  g_user_data = user_data;
}

void StartGameFilesCheck() {
  static std::once_flag once;
  std::call_once(once, [game_root = g_game_root, user_data = g_user_data] {
    // Test aid: SVR2011_TEST_FILES_ROOT=<folder> - that folder checked instead
    // (a copy with an edited file, the game still running from its own).
    fs::path root = game_root;
    if (const char* v = std::getenv("SVR2011_TEST_FILES_ROOT"); v && *v) root = v;
    std::thread([root, user_data] { Check(root, user_data); }).detach();
    std::thread([user_data] { CheckVersion(user_data); }).detach();
  });
}

GameFiles GameFilesState(std::string* first_bad) {
  const GameFiles s = g_state.load();
  if (first_bad) {
    std::lock_guard lock(g_bad_mutex);
    *first_bad = g_first_bad;
  }
  return s;
}

std::string GameFilesOnlineMessage() {
  const GameFiles files = GameFilesState();
  if (files == GameFiles::kModified)
    return "Your game files are modified - online is off.\nUse Verify game files in the launcher to repair them.";
  if (g_version.load() == Version::kOlder) {
    std::lock_guard lock(g_latest_mutex);
    return "Unable to play online due to a difference in game versions.\nUpdate to v" + g_latest +
           " (the launcher's Check for updates, or GitHub) to play online.";
  }
  if (files == GameFiles::kChecking || g_version.load() == Version::kChecking)
    return "Checking your game files.\nOnline will be ready in a moment - please try again.";
  return "";
}

const std::string& PortVersion() {
  static const std::string v = [] {
    const char* t = std::getenv("SVR2011_TEST_VERSION");
    return std::string(t && *t ? t : SVR2011_PORT_VERSION);
  }();
  return v;
}

std::string VersionDifference(const std::string& theirs) {
  if (theirs == PortVersion()) return "";
  if (!theirs.empty() && VersionNewer(theirs, PortVersion()))
    return "They're on v" + theirs + ", you're on v" + PortVersion() + ": update to v" + theirs +
           " (launcher or GitHub) to play them.";
  return "They're on " + (theirs.empty() ? std::string("an older version") : "v" + theirs) + ", you're on v" +
         PortVersion() + ": they need to update.";
}

const std::string& GameplayModSet() {
  static const std::string set = [] {
    std::string s = BuildModSet();
    REXLOG_INFO("online: gameplay mods for online: {}", s.empty() ? std::string("none") : "\n" + s);
    return s;
  }();
  return set;
}

uint64_t GameplayModSetHash() {
  const std::string& s = GameplayModSet();
  if (s.empty()) return 0;
  uint64_t h = 0xCBF29CE484222325ull;
  for (unsigned char ch : s) h = (h ^ ch) * 0x100000001B3ull;
  return h;
}

std::string ModSetDifference(const std::string& mine, const std::string& theirs) {
  const auto a = ParseSet(mine), b = ParseSet(theirs);
  std::vector<std::string> parts;
  for (const auto& [key, l] : b)
    if (!a.count(key)) parts.push_back("they have " + Describe(l) + ", you don't");
  for (const auto& [key, l] : a)
    if (!b.count(key)) parts.push_back("you have " + Describe(l) + ", they don't");
  for (const auto& [key, l] : a)
    if (auto it = b.find(key); it != b.end() && (it->second.version != l.version || it->second.crc != l.crc))
      parts.push_back(it->second.version != l.version
                          ? l.name + ": yours is " + l.version + ", theirs " + it->second.version
                          : l.name + ": your files differ from theirs");
  if (parts.empty()) return "";
  std::string out = "Your mods don't match: ";
  // (the game's message box holds about three lines: two named, then a count)
  for (size_t i = 0; i < parts.size() && i < 2; ++i) out += (i ? "; " : "") + parts[i];
  if (parts.size() > 2) out += "; and " + std::to_string(parts.size() - 2) + " more";
  return out + ".";
}

}  // namespace svr2011
