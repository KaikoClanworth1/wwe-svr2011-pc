// WWE SmackDown vs. Raw 2011 - the debug / test suite (see debug_suite.h).

#include "debug_suite/debug_suite.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "frame_rate.h"
#include "frame_stats.h"
#include "match_diag.h"
#include "native/texture_packs.h"
#include "perf_stats.h"

REXCVAR_DEFINE_STRING(dbg_from, "match", "Debug", "SVRDBG: the cues' zero: match (its start) or start (the game's)");
REXCVAR_DEFINE_STRING(dbg_shot, "", "Debug", "SVRDBG: shots at these seconds (5,10,every:2:5)");
REXCVAR_DEFINE_STRING(dbg_state, "", "Debug", "SVRDBG: state dumps at these seconds");
REXCVAR_DEFINE_STRING(dbg_state_clock, "", "Debug",
                      "SVRDBG: state dumps at these match clock values (60 a second; on the game's thread, exact "
                      "positions - two lockstep games compare)");
REXCVAR_DEFINE_INT32(dbg_burst, 0, "Debug", "SVRDBG: frames shot one by one when the world starts being held (a cinematic)");
REXCVAR_DEFINE_DOUBLE(dbg_quit, 0.0, "Debug", "SVRDBG: the game ends itself at this second (0: never)");
REXCVAR_DEFINE_INT32(dbg_speed, 1, "Debug", "SVRDBG: world updates a frame (fast-forward)");
REXCVAR_DEFINE_STRING(dbg_out, "", "Debug", "SVRDBG: the folder for shots and states (default: dbg)");

namespace svr2011::dbg {

// (used, so the linker keeps it: the release guard's proof that a binary is a debug-suite one)
const char kMarker[] = "SVRDBG: the debug / test suite build (svr2011_dbg) - never part of a release";

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

uint8_t* g_base = nullptr;
fs::path g_out;
std::atomic<int64_t> g_zero{0};       // the cues' zero (Clock ticks; 0: not yet - a match not started)
std::atomic<bool> g_shot_due{false};  // the renderer takes the next frame
std::atomic<int64_t> g_shot_ms{0};    // (its name)
std::atomic<int> g_shots_done{0};
std::atomic<int> g_burst_left{0}, g_burst_n{0};  // (dbg_burst: frames still to shoot, the burst's number)
std::atomic<int64_t> g_held_at{0};               // (the world's last held update)
std::atomic<uint32_t> g_burst_frame{0};

// "5,10,every:2:5" -> seconds, sorted.
std::vector<double> Times(const std::string& spec) {
  std::vector<double> out;
  size_t at = 0;
  while (at <= spec.size()) {
    const size_t end = std::min(spec.find(',', at), spec.size());
    const std::string part = spec.substr(at, end - at);
    if (part.rfind("every:", 0) == 0) {
      const double step = std::atof(part.c_str() + 6);
      const size_t colon = part.find(':', 6);
      const int n = colon == std::string::npos ? 10 : std::atoi(part.c_str() + colon + 1);
      for (int k = 1; step > 0 && k <= n; ++k) out.push_back(step * k);
    } else if (!part.empty()) {
      out.push_back(std::atof(part.c_str()));
    }
    at = end + 1;
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string Json(const std::string& s) {
  std::string o = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (uint8_t(c) < 0x20) o += fmt::format("\\u{:04x}", int(uint8_t(c)));
    else o += c;
  }
  return o + "\"";
}

void WriteState(int64_t ms, const std::string& name = "", bool exact = false) {
  const auto people = MatchPeople(g_base);
  const FrameTiming t = GetFrameTiming();
  const PerfSample p = SamplePerf();
  std::string j = "{\n";
  j += fmt::format("  \"at_ms\": {},\n  \"from\": {},\n  \"in_match\": {},\n  \"match_clock\": {},\n", ms,
                   Json(REXCVAR_GET(dbg_from)), InMatch() ? "true" : "false", MatchClock(g_base));
  j += fmt::format("  \"fps\": {:.1f},\n  \"frame_ms\": {:.2f},\n  \"low_1pct_fps\": {:.1f},\n", t.fps, t.frame_ms,
                   t.low_1pct_fps);
  j += fmt::format("  \"perf\": {},\n  \"people\": [", Json(PerfLine(p)));
  for (size_t i = 0; i < people.size(); ++i) {
    const MatchPerson& m = people[i];
    j += fmt::format("{}\n    {{\"index\": {}, \"person\": {}, \"superstar\": {}, \"name\": {}, \"mod\": {}, "
                     "\"last_move\": {}, \"move_age_s\": {:.1f}",
                     i ? "," : "", m.index, m.person, m.superstar, Json(m.name), Json(m.mod),
                     m.last_move == 0xFFFF ? std::string("null") : std::to_string(m.last_move), m.move_age_s);
    if (m.has_position) j += fmt::format(", \"position\": [{:.2f}, {:.2f}, {:.2f}]", m.x, m.y, m.z);
    if (m.has_position && exact) {  // (the floats' bits: lockstep games have the very same)
      uint32_t b[3];
      std::memcpy(&b[0], &m.x, 4), std::memcpy(&b[1], &m.y, 4), std::memcpy(&b[2], &m.z, 4);
      j += fmt::format(", \"position_bits\": \"{:08X} {:08X} {:08X}\"", b[0], b[1], b[2]);
    }
    j += fmt::format(", \"motion\": {}, \"root\": [{:.2f}, {:.2f}, {:.2f}]", m.motion, m.root_x, m.root_y, m.root_z);
    if (exact) {
      uint32_t b[3];
      std::memcpy(&b[0], &m.root_x, 4), std::memcpy(&b[1], &m.root_y, 4), std::memcpy(&b[2], &m.root_z, 4);
      j += fmt::format(", \"root_bits\": \"{:08X} {:08X} {:08X}\"", b[0], b[1], b[2]);
    }
    j += "}";
  }
  j += people.empty() ? "]\n}\n" : "\n  ]\n}\n";
  const fs::path file = g_out / (name.empty() ? fmt::format("state_{:06d}.json", ms) : name);
  std::ofstream f(file, std::ios::binary | std::ios::trunc);
  f << j;
  REXLOG_INFO("SVRDBG: state {} ({} people)", file.filename().string(), people.size());
}

// The cue thread: every 20 ms, what is due (each list in order); a new match starts the lists again.
void CueThread() {
  const std::vector<double> shots = Times(REXCVAR_GET(dbg_shot)), states = Times(REXCVAR_GET(dbg_state));
  const double quit = REXCVAR_GET(dbg_quit);
  int64_t zero = 0;
  size_t next_shot = 0, next_state = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const int64_t z = g_zero.load();
    if (!z) continue;
    if (z != zero) zero = z, next_shot = next_state = 0;  // (a new match: from the start)
    const double s = std::chrono::duration<double>(Clock::now() - Clock::time_point(Clock::duration(zero))).count();
    if (next_shot < shots.size() && s >= shots[next_shot] && !g_shot_due.load()) {
      g_shot_ms = int64_t(shots[next_shot] * 1000 + 0.5);
      g_shot_due = true;
      ++next_shot;
    }
    if (next_state < states.size() && s >= states[next_state]) WriteState(int64_t(states[next_state++] * 1000 + 0.5));
    if (quit > 0 && s >= quit) {
      // (the shots before it first - a frame or two; at most 3 s)
      const size_t want = size_t(std::count_if(shots.begin(), shots.end(), [&](double t) { return t <= quit; }));
      for (int i = 0; i < 150 && (size_t(g_shots_done.load()) < want || g_shot_due.load()); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      REXLOG_WARN("SVRDBG: quit at {:.1f} s (dbg_quit)", s);
      rex::FlushLogging();
      std::_Exit(0);
    }
  }
}

}  // namespace

void Install(rex::memory::Memory* memory) {
  REXLOG_WARN("{}", kMarker);
  g_base = memory ? memory->virtual_membase() : nullptr;
  g_out = REXCVAR_GET(dbg_out).empty() ? fs::current_path() / "dbg" : fs::path(REXCVAR_GET(dbg_out));
  std::error_code ec;
  fs::create_directories(g_out, ec);
  if (REXCVAR_GET(dbg_from) == "start") g_zero = Clock::now().time_since_epoch().count();
  REXLOG_INFO("SVRDBG: cues from {} - shots [{}], states [{}], quit {}, speed {}, out {}", REXCVAR_GET(dbg_from),
              REXCVAR_GET(dbg_shot), REXCVAR_GET(dbg_state), REXCVAR_GET(dbg_quit), REXCVAR_GET(dbg_speed),
              g_out.string());
  if (!REXCVAR_GET(dbg_shot).empty() || !REXCVAR_GET(dbg_state).empty() || REXCVAR_GET(dbg_quit) > 0)
    std::thread(CueThread).detach();
  InstallBoot();
}

bool ShotWanted() { return g_shot_due.load(std::memory_order_relaxed) || g_burst_left.load(std::memory_order_relaxed) > 0; }

void OnWorldHeld() {
  const int n = REXCVAR_GET(dbg_burst);
  if (n <= 0) return;
  const int64_t now = Clock::now().time_since_epoch().count();
  const int64_t last = g_held_at.exchange(now);
  // (a new hold: none for half a second before)
  if (!last || std::chrono::duration<double>(Clock::duration(now - last)).count() > 0.5) {
    g_burst_frame = 0;
    g_burst_n.fetch_add(1);
    g_burst_left = n;
    REXLOG_INFO("SVRDBG: burst {} - the world held (match clock {}), {} frames shot", g_burst_n.load(), MatchClock(g_base), n);
  }
}

void WriteShot(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t row_bytes) {
  const int64_t ms = g_shot_ms.load();
  std::string name;
  if (g_burst_left.load() > 0 && !g_shot_due.load()) {  // (a burst's frame)
    g_burst_left.fetch_sub(1);
    name = fmt::format("burst{}_{:03d}.png", g_burst_n.load(), g_burst_frame.fetch_add(1));
  } else {
    name = fmt::format("shot_{:06d}.png", ms);
  }
  g_shot_due = false;
  std::vector<uint8_t> px(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    std::copy_n(rgba + size_t(y) * row_bytes, size_t(width) * 4, px.data() + size_t(y) * width * 4);
    for (uint32_t x = 0; x < width; ++x) px[(size_t(y) * width + x) * 4 + 3] = 255;  // (the image's alpha: opaque)
  }
  // (encoded and written on a thread of its own: the renderer goes on)
  std::thread([px = std::move(px), width, height, name] {
    const std::vector<uint8_t> png = native::texture_packs::EncodePng(px.data(), width, height);
    const fs::path file = g_out / name;
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    f.close();
    ++g_shots_done;
    REXLOG_INFO("SVRDBG: shot {} ({}x{})", file.filename().string(), width, height);
  }).detach();
}

void ClockFrame() {
  static const std::vector<double> at = Times(REXCVAR_GET(dbg_state_clock));
  static size_t next = 0;
  static uint32_t last = 0;
  if (at.empty() || !g_base) return;
  const uint32_t clock = MatchClock(g_base);
  if (clock < last) next = 0;  // (a new match)
  last = clock;
  if (next < at.size() && InMatch() && clock >= uint32_t(at[next])) {
    WriteState(0, fmt::format("state_clock_{:06d}.json", uint32_t(at[next])), true);
    ++next;
  }
}

void OnMatchStarted() {
  if (REXCVAR_GET(dbg_from) != "match") return;
  g_zero = Clock::now().time_since_epoch().count();
  REXLOG_INFO("SVRDBG: match started - the cues count from here");
}

int SpeedTicks() {
  const int n = REXCVAR_GET(dbg_speed);
  return n > 1 ? std::min(n, 8) : 0;
}

}  // namespace svr2011::dbg
