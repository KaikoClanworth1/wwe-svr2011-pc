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
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
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
#include "frame_rate.h"
#include "menu_hooks.h"
#include "p2p.h"
#include "pad_types.h"

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
constexpr uint32_t kTableIndexes = 0x82E3689C;  // (the random table's four draw indexes: sub_821C6BE0 zeroes them)

// rand() per call site in a match (see sub_828F2430 below)
std::mutex g_rand_mutex;
std::map<uint32_t, uint32_t> g_rand_callers;
thread_local bool t_logic = false;  // (the world update's thread: MatchRecordUpdate runs on it every frame)
std::atomic<bool> g_match_rand_on{false};
uint32_t g_match_seed = 0;                     // (under g_rand_mutex)
std::map<uint32_t, uint32_t> g_site_rand;     // (call site -> its state, under g_rand_mutex)
std::map<uint32_t, uint32_t> g_site_draws;    // (call site -> draws since the bell, under g_rand_mutex)
uint8_t* g_base = nullptr;                    // (the guest memory: MatchRecordUpdate's)
uint64_t g_load_frame = 0;                    // (the frame of the last match load)
std::array<uint8_t, 4> g_load_table{};        // (the random table's indexes then)
std::atomic<bool> g_loaded{false};            // (a match loaded since the last recording: the menus' attract matches
                                              // - one has a referee - never come through the match's load)
constexpr uint32_t kObjects = 0x82DE0220, kObjectSlots = 72;
constexpr uint32_t kSeedCounters = 0x82EC6A2C;  // (u8 x2: the seed table's next index, by caller kind)
constexpr uint32_t kInputsVersion = 1;  // (1: + the frames' world updates, user 0xFF)

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
  // match.json "seeds" (exact replays later): the rand() seed, the random table's four draw indexes at the match's
  // load and at the bell, the frames between them, and each index's first steps after the bell (its timer's phase)
  uint32_t seed = 0;
  std::array<uint8_t, 4> table_load{}, table_bell{};
  uint64_t frames_load_to_bell = 0;
  std::array<std::vector<uint64_t>, 4> table_steps;
  std::array<uint8_t, 4> last_table{};
  uint32_t trail_second = ~0u;
  std::string start;
  std::chrono::steady_clock::time_point started;
  uint32_t rule = 0, arena = 0;
  std::string arena_manifest, rule_name, arena_name;  // (the names: the game's strings, a mod arena's manifest)
  std::vector<svr2011::MatchPerson> people;
  std::map<uint32_t, std::pair<bool, int>> cpu_attire;  // (character index: a CPU, the attire 1-4 - wrestlers only)
  std::map<uint32_t, uint32_t> ports;                   // (character index: the player's controller 0-3 playing him)
  int last_passes = 1;                                  // (the last frame's world updates written to inputs.bin)
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

std::string SeedsJson() {  // (under g_rec)
  const auto list = [](const auto& a) {
    std::string t;
    for (const auto& v : a) t += (t.empty() ? "" : ", ") + std::to_string(uint64_t(v));
    return "[" + t + "]";
  };
  std::string steps, sites;
  for (const auto& v : g.table_steps) steps += (steps.empty() ? "" : ", ") + list(v);
  {
    std::lock_guard lock(g_rand_mutex);
    for (const auto& [lr, n] : g_site_draws)
      sites += fmt::format("{}\"{:08X}{}\": {}", sites.empty() ? "" : ", ", lr & ~1u, lr & 1 ? "+" : "", n);
  }
  // (rand_sites: draws per call site since the bell - "+": another thread's than the logic thread's)
  return fmt::format("{{\"rand\": \"{:08X}\", \"rand_sites\": {{{}}}, \"table_load\": {}, \"table_bell\": {}, "
                     "\"table_steps\": [{}], \"frames_load_to_bell\": {}}}",
                     g.seed, sites, list(g.table_load), list(g.table_bell), steps, g.frames_load_to_bell);
}

// inputs.bin: after the 16-byte head ("SVRI", version, the bell's frame, 0) 22-byte records - u32 frames since the
// bell, u8 user, then for a pad (users 0-7, one record per change): u8 connected (0) or not (1) and the 16-byte
// XINPUT_STATE as the game got it (its packet number 0); for user 0xFF (version 1): u8 0, then the frame's world
// updates (1, or 2 at 30 fps / a slow frame; 0 the free camera's hold) in the next byte, when it changes.
void WriteInput(uint32_t frame, uint8_t user, const std::array<uint8_t, 17>& data) {  // (under g_rec)
  if (!g.inputs) return;
  std::fwrite(&frame, 4, 1, g.inputs);
  std::fwrite(&user, 1, 1, g.inputs);
  std::fwrite(data.data(), 1, data.size(), g.inputs);
}

void WriteJson(bool complete) {  // (under g_rec)
  const auto len = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g.started).count();
  std::string people;
  for (const auto& p : g.people) {
    if (!people.empty()) people += ",\n    ";
    const auto ca = g.cpu_attire.find(p.index);
    const bool known = ca != g.cpu_attire.end();
    people += fmt::format(
        "{{\"index\": {}, \"person\": {}, \"name\": {}, \"superstar\": {}, \"cpu\": {}, \"attire\": {}, \"port\": {}, "
        "\"mod\": {}, \"caw\": null}}",
        p.index, p.person, Json(p.name), p.superstar, known ? (ca->second.first ? "true" : "false") : "null",
        known && ca->second.second ? std::to_string(ca->second.second) : std::string("null"),
        g.ports.count(p.index) ? std::to_string(g.ports.at(p.index)) : std::string("null"),
        p.mod.empty() ? std::string("null") : ModJson("Superstars", p.mod));
  }
  const std::string arena_id = Key(g.arena_manifest, "id");
  const std::string s = fmt::format(
      "{{\n  \"format\": 1,\n  \"complete\": {},\n  \"game\": {{\"version\": {}, \"commit\": {}}},\n  \"start\": {},\n"
      "  \"length_s\": {},\n  \"result\": {},\n  \"rule\": {{\"id\": {}, \"name\": {}}},\n"
      "  \"arena\": {{\"id\": {}, \"name\": {}, \"mod\": {}}},\n  \"people\": [\n    {}\n  ],\n  \"frame_rate\": {},\n"
      "  \"online\": {},\n  \"renderer\": {},\n  \"cpu_tab\": {{\"reversal_chance\": {}, \"reversal_rates\": {}, "
      "\"damage_level\": {}, \"pin_level\": {}, \"momentum_level\": {}}},\n  \"weapons\": {{\"picks\": {}}},\n"
      "  \"inputs\": {{\"file\": \"inputs.bin\", \"version\": {}}},\n  \"frames\": {},\n  \"seeds\": {}\n}}\n",
      complete ? "true" : "false", Json(SVR2011_PORT_VERSION), Json(SVR2011_BUILD_COMMIT), Json(g.start), len, Json(g.result),
      g.rule, Json(g.rule_name), g.arena, Json(g.arena_name),
      arena_id.empty() ? std::string("null") : ModJson("Arenas", arena_id), people, REXCVAR_GET(frame_rate),
      svr2011::P2POnlineMode() ? "true" : "false", Json(rex::cvar::Query<std::string>("gpu_backend")),
      REXCVAR_GET(cpu_reversal_chance), REXCVAR_GET(cpu_reversal_rates), REXCVAR_GET(cpu_damage_level),
      REXCVAR_GET(cpu_pin_level), REXCVAR_GET(cpu_momentum_level),
      Json(rex::cvar::Query<std::string>("extreme_weapons")), kInputsVersion, g_updates - g.bell, SeedsJson());
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

// -- Replays (SVR2011_REPLAY=<a recording's folder>) ------------------------------------------------------------
// The match set up as the recording's (tools/replay_match.ps1 from its match.json: people, attires, controllers,
// rule, arena, the rand() seed), entrances as the player had them. From the bell, each frame gets the recorded
// pads and the recorded number of world updates; after the recording's frames the pads are the player's again.
// Approximate: the people stand where the bell found them in this run, and the game's random table and loading
// pace aren't replayed (docs/MATCH_RECORDINGS.md). The replay's own recording (a folder of its own) gets
// replay.txt: second by second, how far each person is from the recording's trail.txt - where the two part.
struct Replay {
  bool loaded = false, on = false, done = false;
  fs::path from;                        // (the recording)
  uint64_t bell = 0, frames = 0;        // (this run's bell frame; the recording's frames from its bell)
  std::array<std::vector<std::pair<uint32_t, std::array<uint8_t, 17>>>, 8> pads;
  std::array<size_t, 8> at{};
  std::map<uint32_t, int> passes;       // (frame -> world updates from then on)
};
Replay g_replay;  // (under g_rec)

void ReplayLoad() {  // (under g_rec)
  g_replay.loaded = true;
  const char* v = std::getenv("SVR2011_REPLAY");
  if (!v || !*v) return;
  g_replay.from = fs::u8path(v);
  const std::string json = ReadText(g_replay.from / "match.json");
  if (const size_t k = json.find("\"frames\": "); k != std::string::npos)
    g_replay.frames = std::strtoull(json.c_str() + k + 10, nullptr, 10);
  FILE* f = std::fopen((g_replay.from / "inputs.bin").string().c_str(), "rb");
  uint32_t head[4] = {};
  if (!f || std::fread(head, 4, 4, f) != 4 || head[0] != 0x49525653u) {
    REXLOG_WARN("[svr2011] replay: no inputs.bin in {} - nothing replayed", g_replay.from.string());
    if (f) std::fclose(f);
    return;
  }
  uint8_t rec[22];
  uint32_t last = 0, count = 0;
  while (std::fread(rec, 1, sizeof(rec), f) == sizeof(rec)) {
    uint32_t frame;
    std::memcpy(&frame, rec, 4);
    std::array<uint8_t, 17> data;
    std::memcpy(data.data(), rec + 5, 17);
    if (rec[4] < 8) g_replay.pads[rec[4]].push_back({frame, data});
    else if (rec[4] == 0xFF) g_replay.passes[frame] = data[1];
    last = std::max(last, frame), ++count;
  }
  std::fclose(f);
  if (!g_replay.frames) g_replay.frames = last + 600;  // (no "frames": a crash's recording - 10 s past its last input)
  REXLOG_INFO("[svr2011] replay: {} - {} records (inputs.bin v{}), {} frames from the bell", g_replay.from.string(), count,
              head[1], g_replay.frames);
}

void ReplayBell() {  // (under g_rec: this run's bell - the recording's frame 0 from the next frame)
  if (!g_replay.loaded) ReplayLoad();
  if (g_replay.from.empty() || g_replay.on || g_replay.done) return;
  g_replay.on = true, g_replay.bell = g_updates, g_replay.at = {};
  REXLOG_INFO("[svr2011] replay: from the bell (frame {}), the recording's pads for {} frames", g_updates,
              g_replay.frames);
}

// The per-second trails (trail.txt): second -> person -> x, y, z, motion.
std::map<int, std::map<int, std::array<double, 4>>> ReadTrail(const fs::path& file) {
  std::map<int, std::map<int, std::array<double, 4>>> t;
  std::istringstream in(ReadText(file));
  for (std::string line; std::getline(in, line);) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream l(line);
    int second;
    if (!(l >> second)) continue;
    for (std::string part; std::getline(l, part, '|');) {
      std::istringstream p(part);
      int person;
      std::array<double, 4> v{};
      if (p >> person >> v[0] >> v[1] >> v[2] >> v[3]) t[second][person] = v;
    }
  }
  return t;
}

void ReplayReport() {  // (under g_rec: the replay's end - replay.txt in this run's own folder)
  if (g.trail) std::fflush(g.trail);
  const auto a = ReadTrail(g_replay.from / "trail.txt"), b = ReadTrail(g.dir / "trail.txt");
  std::string text = fmt::format("replay of {}\n(second: the farthest any person is from the recording's place, "
                                 "in game units - the ring is about 50 across; * a motion differs)\n",
                                 g_replay.from.filename().string());
  int parts = -1;
  std::string why;
  for (const auto& [second, people] : a) {
    const auto it = b.find(second);
    if (it == b.end()) continue;
    double farthest = 0;
    bool motions = false;
    for (const auto& [person, v] : people) {
      const auto o = it->second.find(person);
      if (o == it->second.end()) continue;
      const double d = std::sqrt((v[0] - o->second[0]) * (v[0] - o->second[0]) + (v[2] - o->second[2]) * (v[2] - o->second[2]) +
                                 (v[1] - o->second[1]) * (v[1] - o->second[1]));
      if (person < 6 && v[3] != o->second[3]) motions = true;
      if (d > farthest) farthest = d;
      if (parts < 0 && person < 6 && d > 2.0)
        parts = second, why = fmt::format("person {} is {:.1f} from the recording's place (motion {} - recorded {})", person,
                                          d, int(o->second[3]), int(v[3]));
    }
    text += fmt::format("{} {:.1f}{}\n", second, farthest, motions ? " *" : "");
  }
  const std::string head =
      parts < 0 ? std::string("the replay stays with the recording (no person 2.0 or more from it at a second)")
                : fmt::format("the replay parts from the recording at second {}: {}", parts, why);
  if (FILE* f = std::fopen((g.dir / "replay.txt").string().c_str(), "wb"))
    std::fputs((head + "\n" + text).c_str(), f), std::fclose(f);
  REXLOG_INFO("[svr2011] replay: {} - replay.txt in {}", head, g.dir.string());
}

void ReplayFrame(uint8_t* base, uint32_t clock) {  // (under g_rec: each frame, after it ran)
  (void)base, (void)clock;
  if (!g_replay.on || g_updates - g_replay.bell < g_replay.frames) return;
  g_replay.on = false, g_replay.done = true;
  REXLOG_INFO("[svr2011] replay: done - the recording's {} frames played; the pads are the players' again", g_replay.frames);
  if (g.on) ReplayReport();
}

void Start(PPCContext& ctx, uint8_t* base, uint32_t clock) {  // (under g_rec)
  const int keep = REXCVAR_GET(match_recordings);
  if (keep <= 0) return;
  if (g.mods.joinable()) g.mods.join();  // (an earlier match's, ended without its end task)
  g = Recording{};
  g.people = svr2011::MatchPeople(base);
  for (auto& p : g.people)  // (MatchPeople's names end " (id N)": the id is its own key)
    if (const size_t at = p.name.rfind(" (id "); at != std::string::npos) p.name.resize(at);
  // Each wrestler's CPU flag and attire, from the match's pick slots (*0x82EDEB48 +432, 2116 bytes a person: -8
  // the controller, 1 the CPU; +8 superstar * 100 + (attire - 1) * 10 + 2) and the character's player +1152 - a
  // CPU only while no pad of a local player plays him (a test's cpu=all marks the picks too: match_types.cpp).
  if (const uint32_t match = Rd32(base + 0x82EDEB48); match >= 0x40000000u && !(match & 3))
    for (const auto& p : g.people) {
      if (p.person >= 6) continue;
      const uint32_t c = Rd32(base + kChars + p.index * 4);
      const uint8_t* slot = base + match + 432 + p.person * 2116;
      const uint32_t port = c ? Rd32(base + c + 1152) : ~0u, pick = Rd32(slot + 8);
      const bool pad = port < svr2011::kMaxPlayers && svr2011::PlayerPadType(port) != svr2011::PadType::kNone;
      const int attire = pick != 51200 && pick / 100 < 512 ? int(pick % 100 / 10) + 1 : 0;
      g.cpu_attire[p.index] = {slot[-8] == 1 && !pad, attire};
      if (pad) g.ports[p.index] = port;
    }
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
  {
    std::lock_guard lock(g_rand_mutex);
    g.seed = g_match_seed;
  }
  g.table_load = g_load_table;
  std::memcpy(g.table_bell.data(), base + kTableIndexes, 4);
  g.last_table = g.table_bell;
  g.frames_load_to_bell = g_load_frame ? g_updates - g_load_frame : 0;
  g.rule = base[kLive], g.arena = Rd32(base + kLive + 64);
  g.arena_manifest = svr2011::ArenaModManifest();
  // The names as the game shows them (port/server/match_names.py has the English ones): the rule record's +56 is
  // its name's string id (misc.pac RUL, *0x82EDE954, 100 bytes a rule); arena n's name is string 0x9C42 + n; a mod
  // arena plays on a stock arena's number, so its manifest's name.
  if (const uint32_t rules = Rd32(base + 0x82EDE954); rules && g.rule < 119)
    g.rule_name = svr2011::GameString(ctx, base, Rd32(base + rules + g.rule * 100 + 56));
  if (g.rule_name.empty()) g.rule_name = fmt::format("rule {:02X}", g.rule);
  g.arena_name = Key(g.arena_manifest, "name");
  if (g.arena_name.empty() && g.arena < 200) g.arena_name = svr2011::GameString(ctx, base, 0x9C42 + g.arena);
  if (g.arena_name.empty()) g.arena_name = fmt::format("arena {}", g.arena);
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

// The CRT rand() keeps a state per thread, seeded from the clock: a match's draws (who starts how, the AI - on the
// logic thread and the characters' job threads) differ run to run. From each match's load (sub_823EEB98,
// MatchRecordMatchLoads) every call site draws from a state of its own (the same generator: x 0x343FD + 0x269EC3,
// bits 16-30), seeded from the match's seed and the site - so the order the threads call in doesn't matter. The
// seed goes into match.json, so a replay draws the same.
// Replay research: SVR2011_TEST_RAND_LOG=1 - who calls rand() in a match, every 10 s (logic thread or not).
REX_EXTERN(__imp__sub_828F2430);
REX_HOOK_RAW(sub_828F2430) {
  static const bool log = std::getenv("SVR2011_TEST_RAND_LOG") != nullptr;
  if (log) {
    std::lock_guard lock(g_rand_mutex);
    ++g_rand_callers[uint32_t(ctx.lr) | (t_logic ? 0u : 1u)];  // (bit 0: another thread's call)
  }
  if (g_match_rand_on.load(std::memory_order_relaxed)) {
    std::lock_guard lock(g_rand_mutex);
    const uint32_t lr = uint32_t(ctx.lr) | (t_logic ? 0u : 1u);  // (a site's logic-thread calls: a stream of their own)
    auto [it, fresh] = g_site_rand.try_emplace(lr, 0);
    if (fresh) it->second = g_match_seed ^ (lr * 2654435761u);
    it->second = it->second * 0x343FD + 0x269EC3;
    ctx.r3.u64 = (it->second >> 16) & 0x7FFF;
    ++g_site_draws[lr];
    return;
  }
  __imp__sub_828F2430(ctx, base);
}

namespace svr2011 {

void MatchRecordMatchLoads() {
  if (!t_logic) {
    REXLOG_WARN("[svr2011] record: a match loads off the logic thread - its rand() stays the CRT's");
    return;
  }
  // (test aid: SVR2011_TEST_RAND_SEED=<n> - this seed for every match)
  static const char* test = std::getenv("SVR2011_TEST_RAND_SEED");
  const uint32_t seed = test ? uint32_t(std::strtoul(test, nullptr, 0))
                             : uint32_t(std::chrono::steady_clock::now().time_since_epoch().count() * 2654435761u);
  {
    std::lock_guard lock(g_rand_mutex);
    g_match_seed = seed;
    g_site_rand.clear();
  }
  g_match_rand_on = true;
  g_loaded = true;
  g_load_frame = g_updates;
  if (g_base) std::memcpy(g_load_table.data(), g_base + kTableIndexes, 4);
  REXLOG_INFO("[svr2011] record: a match loads - its rand() seed {:08X}", seed);
}

void MatchRecordUpdate(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  t_logic = true;
  g_base = base;
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
  // The bell: the draws start again from the match's seed (before it, the people's loading - its pace the disk's -
  // decides how many frames the intro gets, so a replay is anchored here).
  if (static uint32_t last_clock = 0; clock == 1 && last_clock != 1 && g_match_rand_on) {
    std::lock_guard lock(g_rand_mutex);
    g_site_rand.clear(), g_site_draws.clear();
    last_clock = clock;
  } else {
    last_clock = clock;
  }
  {
    std::lock_guard lock(g_rec);
    const bool match = clock && c0 >= 0x40000000u && !(c0 & 3);
    if (match && !g.on && clock <= 2) {  // (the bell: with a referee, after a match's load - not an attract match)
      bool referee = false;
      for (uint32_t i = 0; i < 10; ++i)
        if (const uint32_t c = Rd32(base + kChars + i * 4); c >= 0x40000000u && !(c & 3) && Rd32(base + c + 1156) == 6)
          referee = true;
      if (referee && g_loaded.exchange(false)) Start(ctx, base, clock), ReplayBell();
    }
    if (g.on && g.bell < g_updates)
      if (const int passes = svr2011::WorldPassesThisFrame(); passes != g.last_passes) {
        g.last_passes = passes;
        std::array<uint8_t, 17> data{};
        data[1] = uint8_t(passes);
        WriteInput(uint32_t(g_updates - 1 - g.bell), 0xFF, data);  // (this frame's: the pads read in it have its number)
      }
    ReplayFrame(base, clock);
    if (g.on && match) {
      g.fallback_peak = std::max(g.fallback_peak, FallbackBytesInUse());
      for (size_t i = 0; i < 4; ++i)  // (each index's first steps after the bell: its timer's phase)
        if (g.table_steps[i].size() < 8 && base[kTableIndexes + i] != g.last_table[i])
          g.table_steps[i].push_back(g_updates - g.bell);
      std::memcpy(g.last_table.data(), base + kTableIndexes, 4);
      // (once a second of the match's clock - 60 world updates; frames can hold 2 at 30 fps or when slow)
      if (g.trail && clock / 60 != g.trail_second) {
        g.trail_second = clock / 60;
        std::string line = fmt::format("{}", clock / 60);
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
  // (before the bell - the clock at 0 - its lines are "pre <frame>", the frames counted from the characters' coming)
  static uint32_t pre = 0;
  if (!trail || !c0 || c0 < 0x40000000u) {
    pre = 0;
    return;
  }
  if (!g_trail_test) g_trail_test = std::fopen(trail, "w");
  if (!g_trail_test) return;
  std::string line = clock ? fmt::format("{}", clock) : fmt::format("pre {}", ++pre);
  if (clock) pre = 0;
  // (the random table's four draw indexes, 0x82E3689C: inlined draws all over the game advance them)
  line += fmt::format(" | r {} {} {} {}", base[0x82E3689C], base[0x82E3689D], base[0x82E3689E], base[0x82E3689F]);
  for (uint32_t i = 0; i < 10; ++i) {
    const uint32_t c = Rd32(base + kChars + i * 4);
    if (c < 0x40000000u || (c & 3)) continue;
    line += fmt::format(" | {} {:.3f} {:.3f} {:.3f} m{} f{:.2f}", Rd32(base + c + 1156), RdF(base + c + 288), RdF(base + c + 292),
                        RdF(base + c + 296), Rd32(base + c + 212), RdF(base + c + 24));
  }
  std::fputs((line + "\n").c_str(), g_trail_test);
  if (clock % 600 == 0) std::fflush(g_trail_test);
}

bool MatchReplayPad(uint32_t user, uint32_t& result, uint8_t* state) {
  if (user >= 8 || !state) return false;
  std::lock_guard lock(g_rec);
  if (!g_replay.on) return false;
  const uint64_t frame = g_updates - g_replay.bell;
  auto& v = g_replay.pads[user];
  size_t& i = g_replay.at[user];
  while (i + 1 < v.size() && v[i + 1].first <= frame) ++i;
  if (v.empty() || v[i].first > frame) return false;
  const auto& d = v[i].second;
  result = d[0] ? 0x48F : 0;  // (ERROR_DEVICE_NOT_CONNECTED)
  std::memcpy(state, d.data() + 1, 16);
  const uint32_t packet = uint32_t(frame + 1);  // (the packet number: a new one each frame)
  state[0] = uint8_t(packet >> 24), state[1] = uint8_t(packet >> 16), state[2] = uint8_t(packet >> 8), state[3] = uint8_t(packet);
  return true;
}

int MatchReplayPasses() {
  std::lock_guard lock(g_rec);
  if (!g_replay.on) return 0;
  const auto it = g_replay.passes.upper_bound(uint32_t(g_updates - g_replay.bell));
  return it == g_replay.passes.begin() ? 1 : std::prev(it)->second;
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
  WriteInput(uint32_t(g_updates - g.bell), uint8_t(user), now);
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
  if (g_replay.on) {  // (the replay's match ended before the recording's frames ran out)
    g_replay.on = false, g_replay.done = true;
    ReplayReport();
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
