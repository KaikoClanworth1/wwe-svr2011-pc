// Match recordings (match_record.h, docs/MATCH_RECORDINGS.md). The folder starts at the bell (the match clock's run
// with a referee in the match: not the menu's attract match) and ends with the match's end task (match_box.cpp). The
// world update's thread writes the trail and starts / ends it; the pad reads note the pads; a thread of its own lists
// the mods (their files' quick hash) so the match doesn't wait for the disk.
//
// The game's random numbers: sub_821C6BE0 seeds a 256-entry table (the LCG x * 0x343FD + 0x269EC3) from one byte - 0
// when the session object (sub_82573F18) +0 is set, else the next byte of the table at 0x82037B98 by the counters at
// 0x82EC6A2C / +1 (loaded.txt). The C runtime's rand() (sub_828F2430, its state per thread) is used besides, about 20
// places in a match (replay research: SVR2011_TEST_RAND_LOG / SVR2011_TEST_RAND_FIXED).
#include "match_record.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "arena_mods.h"
#include "caw_limits.h"
#include "match_diag.h"
#include "p2p.h"

#include "generated/default/svr2011_init.h"

#ifndef SVR2011_PORT_VERSION
#define SVR2011_PORT_VERSION "dev"
#endif
#ifndef SVR2011_BUILD_COMMIT
#define SVR2011_BUILD_COMMIT "unknown"
#endif

REXCVAR_DEFINE_INT32(match_recordings, 20, "Logs",
                     "Matches kept in logs\\matches: each match's setup, pads, black box and trail, for problem reports "
                     "(0: none)");
REXCVAR_DECLARE(int32_t, frame_rate);
REXCVAR_DECLARE(int32_t, cpu_reversal_chance);
REXCVAR_DECLARE(int32_t, cpu_reversal_rates);
REXCVAR_DECLARE(int32_t, cpu_damage_level);
REXCVAR_DECLARE(int32_t, cpu_pin_level);
REXCVAR_DECLARE(int32_t, cpu_momentum_level);

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kChars = 0x82E3CC50, kMatchFrames = 0x82E3CD0C, kLive = 0x82E3DE00;
constexpr uint32_t kObjects = 0x82DE0220, kObjectSlots = 72;
constexpr uint32_t kSeedCounters = 0x82EC6A2C;  // (u8 x2: the seed table's next index, by caller kind)
constexpr uint32_t kInputsVersion = 0;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
float RdF(const uint8_t* p) {
  const uint32_t v = Rd32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

std::string Json(const std::string& s) {  // (a JSON string, quoted)
  std::string o = "\"";
  for (const unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += char(c);
    else if (c < 0x20) o += fmt::format("\\u{:04x}", c);
    else o += char(c);
  }
  return o + "\"";
}

std::string Key(const std::string& text, const char* key) {  // (a manifest's key=value)
  const std::string k = std::string(key) + "=";
  for (size_t at = 0; (at = text.find(k, at)) != std::string::npos; at += k.size())
    if (at == 0 || text[at - 1] == '\n') {
      std::string v = text.substr(at + k.size(), text.find('\n', at) - at - k.size());
      while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
      return v;
    }
  return {};
}

std::string ReadText(const fs::path& p) {
  std::string s;
  if (FILE* f = std::fopen(p.string().c_str(), "rb")) {
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) s.append(buf, n);
    std::fclose(f);
  }
  return s;
}

// A mod folder's quick identity: FNV-1a 64 over its files - sorted lower-case relative paths ('/'), each with its
// size and write time - leaving out dot files and "disabled". (The report client hashes the bytes itself.)
std::string FilesHash(const fs::path& dir) {
  std::vector<std::tuple<std::string, uint64_t, int64_t>> files;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string name = it->path().filename().string();
    if (name.empty() || name[0] == '.' || name == "disabled") continue;
    std::string rel = fs::relative(it->path(), dir, ec).generic_string();
    std::transform(rel.begin(), rel.end(), rel.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    files.emplace_back(rel, uint64_t(it->file_size(ec)), int64_t(it->last_write_time(ec).time_since_epoch().count()));
  }
  std::sort(files.begin(), files.end());
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
  };
  for (const auto& [rel, size, time] : files) mix(rel.data(), rel.size() + 1), mix(&size, 8), mix(&time, 8);
  return fmt::format("{:016x}", h);
}

struct ModInfo {
  std::string type, id, version, name, files;
  bool on = true, used = false;
};

struct Recording {
  bool on = false;
  fs::path dir;
  uint64_t bell = 0;  // (the world update at the bell: inputs.bin counts from it)
  std::string start;
  std::chrono::steady_clock::time_point started;
  uint32_t rule = 0, arena = 0;
  std::string arena_manifest;
  std::vector<svr2011::MatchPerson> people;
  FILE* inputs = nullptr;
  FILE* trail = nullptr;
  std::array<std::array<uint8_t, 17>, 8> pads{};  // (the last noted: connected + state, per user)
  std::array<bool, 8> pad_seen{};
  uint32_t fallback_peak = 0;
  std::set<std::pair<std::string, std::string>> used;  // (type, folder) the match used
  std::thread mods;
  bool mods_done = false;         // (the mods thread joined: mod_list readable)
  std::vector<ModInfo> mod_list;  // (the mods thread's: read after it's joined)
  std::string result = "running";
};

std::mutex g_rec;
Recording g;
uint64_t g_updates = 0;
FILE* g_trail_test = nullptr;  // (SVR2011_TEST_TRAIL)

fs::path LogsDir() {
  const std::string log = rex::cvar::Query<std::string>("log_file");
  std::error_code ec;
  return log.empty() ? fs::current_path(ec) / "logs" : fs::u8path(log).parent_path();
}

std::string Safe(const std::string& name) {  // (a folder-name part: letters, digits, '-')
  std::string o;
  for (const unsigned char c : name)
    if (std::isalnum(c)) o += char(c);
    else if (c == ' ' || c == '-' || c == '.') o += '-';
  return o.empty() ? "X" : o;
}

std::string ModJson(const std::string& type, const std::string& id) {
  for (const ModInfo& m : g.mods_done ? g.mod_list : std::vector<ModInfo>{})
    if (m.type == type && m.id == id)
      return fmt::format("{{\"type\": {}, \"id\": {}, \"version\": {}, \"files\": {}}}", Json(m.type), Json(m.id),
                         Json(m.version), Json(m.files));
  return fmt::format("{{\"type\": {}, \"id\": {}}}", Json(type), Json(id));
}

void WriteJson(bool complete) {  // (under g_rec)
  const auto len = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g.started).count();
  std::string people;
  for (const auto& p : g.people) {
    if (!people.empty()) people += ",\n    ";
    people += fmt::format("{{\"index\": {}, \"person\": {}, \"name\": {}, \"superstar\": {}, \"mod\": {}, \"caw\": null}}",
                          p.index, p.person, Json(p.name), p.superstar,
                          p.mod.empty() ? std::string("null") : ModJson("Superstars", p.mod));
  }
  const std::string arena_id = Key(g.arena_manifest, "id");
  const std::string arena_name = Key(g.arena_manifest, "name");
  const std::string s = fmt::format(
      "{{\n  \"format\": 1,\n  \"complete\": {},\n  \"game\": {{\"version\": {}, \"commit\": {}}},\n  \"start\": {},\n"
      "  \"length_s\": {},\n  \"result\": {},\n  \"rule\": {{\"id\": {}, \"name\": {}}},\n"
      "  \"arena\": {{\"id\": {}, \"name\": {}, \"mod\": {}}},\n  \"people\": [\n    {}\n  ],\n  \"frame_rate\": {},\n"
      "  \"online\": {},\n  \"renderer\": {},\n  \"cpu_tab\": {{\"reversal_chance\": {}, \"reversal_rates\": {}, "
      "\"damage_level\": {}, \"pin_level\": {}, \"momentum_level\": {}}},\n  \"weapons\": {{\"picks\": {}}},\n"
      "  \"inputs\": {{\"file\": \"inputs.bin\", \"version\": {}}}\n}}\n",
      complete ? "true" : "false", Json(SVR2011_PORT_VERSION), Json(SVR2011_BUILD_COMMIT), Json(g.start), len, Json(g.result),
      g.rule, Json(fmt::format("rule {:02X}", g.rule)), g.arena,
      Json(arena_name.empty() ? fmt::format("arena {}", g.arena) : arena_name),
      arena_id.empty() ? std::string("null") : ModJson("Arenas", arena_id), people, REXCVAR_GET(frame_rate),
      svr2011::P2POnlineMode() ? "true" : "false", Json(rex::cvar::Query<std::string>("gpu_backend")),
      REXCVAR_GET(cpu_reversal_chance), REXCVAR_GET(cpu_reversal_rates), REXCVAR_GET(cpu_damage_level),
      REXCVAR_GET(cpu_pin_level), REXCVAR_GET(cpu_momentum_level),
      Json(rex::cvar::Query<std::string>("extreme_weapons")), kInputsVersion);
  if (FILE* f = std::fopen((g.dir / "match.json").string().c_str(), "wb")) std::fwrite(s.data(), 1, s.size(), f), std::fclose(f);
}

// The mods, in load order per kind (sorted folder names, as the game reads them), into mods.txt - a thread of its own.
void ListMods(fs::path game, fs::path out, std::set<std::pair<std::string, std::string>> used, std::vector<ModInfo>* list) {
  static const std::set<std::string> kGenerated = {"ArenaOverlay", "ArenaCrowdless", "PacOverlay", "ArenaTest"};
  std::error_code ec;
  std::vector<fs::path> kinds;
  for (const auto& e : fs::directory_iterator(game / "Mods", ec))
    if (e.is_directory(ec) && e.path().filename().string()[0] != '.' && !kGenerated.count(e.path().filename().string()))
      kinds.push_back(e.path());
  std::sort(kinds.begin(), kinds.end());
  std::string text;
  for (const auto& kind : kinds) {
    std::vector<fs::path> mods;
    for (const auto& e : fs::directory_iterator(kind, ec))
      if (e.is_directory(ec) && e.path().filename().string()[0] != '.') mods.push_back(e.path());
    std::sort(mods.begin(), mods.end());
    for (const auto& m : mods) {
      ModInfo i;
      i.type = kind.filename().string(), i.id = m.filename().string();
      const std::string manifest = ReadText(m / "manifest.txt");
      i.version = Key(manifest, "version"), i.name = Key(manifest, "name");
      i.on = !fs::exists(m / "disabled", ec);
      i.used = used.count({i.type, i.id}) > 0;
      i.files = FilesHash(m);
      text += fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\n", i.type, i.id, i.version, i.on ? "on" : "off", i.used ? "used" : "-",
                          i.files, i.name.empty() ? i.id : i.name);
      list->push_back(std::move(i));
    }
  }
  if (FILE* f = std::fopen((out / "mods.txt").string().c_str(), "wb")) std::fwrite(text.data(), 1, text.size(), f), std::fclose(f);
}

void Start(uint8_t* base, uint32_t clock) {  // (under g_rec)
  const int keep = REXCVAR_GET(match_recordings);
  if (keep <= 0) return;
  if (g.mods.joinable()) g.mods.join();  // (an earlier match's, ended without its end task)
  g = Recording{};
  g.people = svr2011::MatchPeople(base);
  for (auto& p : g.people)  // (MatchPeople's names end " (id N)": the id is its own key)
    if (const size_t at = p.name.rfind(" (id "); at != std::string::npos) p.name.resize(at);
  std::string names;
  for (const auto& p : g.people)
    if (p.person < 6) names += (names.empty() ? "" : "_vs_") + Safe(p.name);
  if (names.size() > 80) names.resize(80);
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char stamp[32], iso[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
  std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tm);
  g.start = iso;
  const fs::path root = LogsDir() / "matches";
  std::error_code ec;
  fs::create_directories(root, ec);
  // (the newest `keep` folders: the oldest go - the names start with their time)
  std::vector<fs::path> old;
  for (const auto& e : fs::directory_iterator(root, ec))
    if (e.is_directory(ec)) old.push_back(e.path());
  std::sort(old.begin(), old.end());
  while (!old.empty() && int(old.size()) >= keep) fs::remove_all(old.front(), ec), old.erase(old.begin());
  g.dir = root / (std::string(stamp) + "_" + names);
  fs::create_directories(g.dir, ec);
  if (ec) {
    REXLOG_WARN("[svr2011] record: {} not made ({}) - no recording this match", g.dir.string(), ec.message());
    g = Recording{};
    return;
  }
  g.on = true, g.bell = g_updates, g.started = std::chrono::steady_clock::now();
  g.rule = base[kLive], g.arena = Rd32(base + kLive + 64);
  g.arena_manifest = svr2011::ArenaModManifest();
  for (const auto& p : g.people)
    if (!p.mod.empty()) g.used.insert({"Superstars", p.mod});
  if (const std::string id = Key(g.arena_manifest, "id"); !id.empty()) g.used.insert({"Arenas", id});
  const std::string picks = rex::cvar::Query<std::string>("extreme_weapons");
  for (size_t at = 0; (at = picks.find("mod:", at)) != std::string::npos; at += 4)
    g.used.insert({"Weapons", picks.substr(at + 4, picks.find(':', at + 4) - at - 4)});
  WriteJson(false);
  if ((g.inputs = std::fopen((g.dir / "inputs.bin").string().c_str(), "wb"))) {
    const uint32_t head[4] = {0x49525653u /* "SVRI" */, kInputsVersion, uint32_t(g_updates), 0};
    std::fwrite(head, 4, 4, g.inputs);
  }
  if ((g.trail = std::fopen((g.dir / "trail.txt").string().c_str(), "wb")))
    std::fputs("# second | person x y z motion (once a second from the bell; y down, the ring's mat -12)\n", g.trail);
  g.fallback_peak = svr2011::FallbackBytesInUse();
  std::string loaded = fmt::format("at the bell (update {}, clock {}):\n  fallback memory {} KB\n  weapons (slot:model):",
                                   g_updates, clock, g.fallback_peak / 1024);
  for (uint32_t i = 0; i < kObjectSlots; ++i)
    if (const uint32_t o = Rd32(base + kObjects + i * 4))
      if (const uint32_t model = Rd32(base + o + 80); model < 1000) loaded += fmt::format(" {}:{}", i, model);
  loaded += fmt::format("\n  random table counters {} / {}\n", base[kSeedCounters], base[kSeedCounters + 1]);
  if (FILE* f = std::fopen((g.dir / "loaded.txt").string().c_str(), "wb")) std::fputs(loaded.c_str(), f), std::fclose(f);
  if (FILE* f = std::fopen((g.dir / "box.txt").string().c_str(), "wb")) std::fclose(f);
  g.mods = std::thread(ListMods, fs::path(rex::filesystem::GetExecutableFolder()), g.dir, g.used, &g.mod_list);
  REXLOG_INFO("[svr2011] record: this match in {}", g.dir.string());
}

}  // namespace

// The random table is seeded (a match's load, and other places): logged with SVR2011_TEST_SEED_LOG (replay research).
REX_EXTERN(__imp__sub_821C6BE0);
REX_HOOK_RAW(sub_821C6BE0) {
  static const bool log = std::getenv("SVR2011_TEST_SEED_LOG") != nullptr;
  const uint32_t arg = ctx.r3.u32, lr = uint32_t(ctx.lr);
  const uint8_t a = base[kSeedCounters], b = base[kSeedCounters + 1];
  __imp__sub_821C6BE0(ctx, base);
  if (log)
    REXLOG_INFO("[svr2011] record: random table seeded (arg {}, counters {} / {} -> {} / {}, lr {:08X}, update {})", arg, a, b,
                base[kSeedCounters], base[kSeedCounters + 1], lr, g_updates);
}

// Replay research: SVR2011_TEST_RAND_LOG=1 - who calls the CRT rand() in a match, every 10 s; SVR2011_TEST_RAND_FIXED=1
// - one rand() state for every thread, reset when a match's clock starts.
std::mutex g_rand_mutex;
std::map<uint32_t, uint32_t> g_rand_callers;
uint32_t g_rand_state = 1;
REX_EXTERN(__imp__sub_828F2430);
REX_HOOK_RAW(sub_828F2430) {
  static const bool log = std::getenv("SVR2011_TEST_RAND_LOG") != nullptr;
  static const bool fixed = std::getenv("SVR2011_TEST_RAND_FIXED") != nullptr;
  if (log) {
    std::lock_guard lock(g_rand_mutex);
    ++g_rand_callers[uint32_t(ctx.lr)];
  }
  if (fixed) {
    std::lock_guard lock(g_rand_mutex);
    g_rand_state = g_rand_state * 0x343FD + 0x269EC3;
    ctx.r3.u64 = (g_rand_state >> 16) & 0x7FFF;
    return;
  }
  __imp__sub_828F2430(ctx, base);
}

namespace svr2011 {

void MatchRecordUpdate(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  ++g_updates;
  const uint32_t clock = Rd32(base + kMatchFrames);
  const uint32_t c0 = Rd32(base + kChars);
  if (static const bool log = std::getenv("SVR2011_TEST_RAND_LOG") != nullptr; log && g_updates % 600 == 0) {
    std::lock_guard lock(g_rand_mutex);
    std::string s;
    for (const auto& [lr, n] : g_rand_callers) s += fmt::format(" {:08X}x{}", lr, n);
    if (!s.empty()) REXLOG_INFO("[svr2011] record: rand() callers in the last 10 s:{}", s);
    g_rand_callers.clear();
  }
  if (static const bool fixed = std::getenv("SVR2011_TEST_RAND_FIXED") != nullptr; fixed && clock == 1) {
    std::lock_guard lock(g_rand_mutex);
    g_rand_state = 1;
  }
  {
    std::lock_guard lock(g_rec);
    const bool match = clock && c0 >= 0x40000000u && !(c0 & 3);
    if (match && !g.on && clock <= 2) {  // (the bell: with a referee - not the menu's attract match)
      bool referee = false;
      for (uint32_t i = 0; i < 10; ++i)
        if (const uint32_t c = Rd32(base + kChars + i * 4); c >= 0x40000000u && !(c & 3) && Rd32(base + c + 1156) == 6)
          referee = true;
      if (referee) Start(base, clock);
    }
    if (g.on && match) {
      g.fallback_peak = std::max(g.fallback_peak, FallbackBytesInUse());
      if (g.trail && (g_updates - g.bell) % 60 == 0) {
        std::string line = fmt::format("{}", (g_updates - g.bell) / 60);
        for (uint32_t i = 0; i < 10; ++i) {
          const uint32_t c = Rd32(base + kChars + i * 4);
          if (c < 0x40000000u || (c & 3)) continue;
          line += fmt::format(" | {} {:.1f} {:.1f} {:.1f} {}", Rd32(base + c + 1156), RdF(base + c + 288), RdF(base + c + 292),
                              RdF(base + c + 296), Rd32(base + c + 212));
        }
        std::fputs((line + "\n").c_str(), g.trail);
        std::fflush(g.trail);
      }
    }
  }
  // Test aid: SVR2011_TEST_TRAIL=<file> - every world update of a match: the clock and each person's position, motion
  // and motion frame (two runs of one match compared line by line: does the game play it the same?)
  static const char* trail = std::getenv("SVR2011_TEST_TRAIL");
  if (!trail || !clock || !c0) return;
  if (!g_trail_test) g_trail_test = std::fopen(trail, "w");
  if (!g_trail_test) return;
  std::string line = fmt::format("{}", clock);
  for (uint32_t i = 0; i < 10; ++i) {
    const uint32_t c = Rd32(base + kChars + i * 4);
    if (c < 0x40000000u || (c & 3)) continue;
    line += fmt::format(" | {} {:.3f} {:.3f} {:.3f} m{} f{:.2f}", Rd32(base + c + 1156), RdF(base + c + 288), RdF(base + c + 292),
                        RdF(base + c + 296), Rd32(base + c + 212), RdF(base + c + 24));
  }
  std::fputs((line + "\n").c_str(), g_trail_test);
  if (clock % 600 == 0) std::fflush(g_trail_test);
}

void MatchRecordPad(uint32_t user, uint32_t result, const uint8_t* state) {
  if (user >= 8 || !state) return;
  std::lock_guard lock(g_rec);
  if (!g.on || !g.inputs) return;
  std::array<uint8_t, 17> now{};
  now[0] = uint8_t(result ? 1 : 0);
  std::memcpy(now.data() + 1, state, 16);
  std::memset(now.data() + 1, 0, 4);  // (dwPacketNumber: it counts every read, not changes - not kept)
  if (g.pad_seen[user] && g.pads[user] == now) return;
  g.pad_seen[user] = true, g.pads[user] = now;
  // inputs.bin v0: after the 16-byte head ("SVRI", version, the bell's update, 0) one record per change of a pad -
  // u32 updates since the bell, u8 user, u8 connected (0) or not (1), the 16-byte XINPUT_STATE as the game got it (its packet number 0).
  const uint32_t at = uint32_t(g_updates - g.bell);
  std::fwrite(&at, 4, 1, g.inputs);
  const uint8_t u = uint8_t(user);
  std::fwrite(&u, 1, 1, g.inputs);
  std::fwrite(now.data(), 1, now.size(), g.inputs);
}

void MatchRecordBox(const std::string& text) {
  std::lock_guard lock(g_rec);
  if (!g.on) return;
  if (FILE* f = std::fopen((g.dir / "box.txt").string().c_str(), "ab")) std::fputs((text + "\n").c_str(), f), std::fclose(f);
}

void MatchRecordEnded(const std::string& box) {
  std::thread mods;
  {
    std::lock_guard lock(g_rec);
    if (!g.on) return;
    mods = std::move(g.mods);
  }
  if (mods.joinable()) mods.join();  // (the mod list's thread: done long before, unless the disk is slow)
  std::lock_guard lock(g_rec);
  g.mods_done = true;
  if (!box.empty())
    if (FILE* f = std::fopen((g.dir / "box.txt").string().c_str(), "ab")) std::fputs(box.c_str(), f), std::fclose(f);
  if (FILE* f = std::fopen((g.dir / "loaded.txt").string().c_str(), "ab")) {
    std::fputs(fmt::format("at the end (update {}):\n  fallback memory {} KB (peak {} KB)\n", g_updates,
                           FallbackBytesInUse() / 1024, g.fallback_peak / 1024).c_str(), f);
    std::fclose(f);
  }
  if (g.inputs) std::fclose(g.inputs);
  if (g.trail) std::fclose(g.trail);
  g.inputs = g.trail = nullptr;
  g.result = "ended";
  WriteJson(true);
  if (FILE* f = std::fopen((g.dir / "done.txt").string().c_str(), "wb")) std::fclose(f);
  REXLOG_INFO("[svr2011] record: the match's folder done ({})", g.dir.filename().string());
  g.on = false;
}

void MatchRecordCrashText(FILE* f) {
  if (!f || !g_rec.try_lock()) return;
  if (g.on) {
    std::fprintf(f, "match recording: %s (unfinished: no done.txt)\n", g.dir.string().c_str());
    g.result = "crash";
    if (g.inputs) std::fflush(g.inputs);
    WriteJson(false);
  }
  g_rec.unlock();
}

}  // namespace svr2011
