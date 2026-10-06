// The game's frame rate - see frame_rate.h.
//
// The timing block at 0x82EDDBE8 (written by sub_826E1AE8 SetFrameRate):
//   +8  int   fps                  +28 float 1/fps (Havok's step)
//   +32 float fps                  +36 float 1000/fps     +40 int 1000/fps
//   +60 float 60/fps (the step in 60 Hz frames, what most code scales by)
//   +64 float fps/60
// It only knows 25 / 30 / 50 / 60, and much of the game steps by whole
// frames or tells 30 from 60 only - so it stays at the game's own 60 here,
// and the frame rate is made by how often the world update runs.
//
// The game's loop (logic thread, sub_821595D8): wait for the render thread,
// then the world update sub_8269D768 - task list upkeep (sub_8269D3F0) and
// the task manager sub_8269C728(manager):
//   update pass: the systems' slot 1 (input among them), then group by group
//     (the scheduler +68's slot 4 allows the group) every live task's slot 1;
//   draw pass:   the systems' slot 2, the render kick (sub_826A6018 /
//     sub_826A6080), list upkeep, the systems' slot 3.
// Here the update pass runs once per 60 Hz tick of real time, the draw pass
// once a frame. A frame always gets at least one update (the menus are drawn
// in it), so the game draws at most 60 frames a second; the frame clock (the
// guest vblank, guest_vblank_hz) runs at the chosen rate up to 60. At 30 fps,
// or when the PC can't make 60, a frame runs two (up to four) updates; the
// extra ones repeat the controller reading and leave the characters' job to
// the frame's last update (below). Online lockstep: one update a frame.
#include "frame_rate.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/ui/presenter.h>

#include "generated/default/svr2011_init.h"
#include "jukebox.h"
#include "match_types.h"
#include "online_overlay.h"
#include "players.h"

REXCVAR_DEFINE_INT32(frame_rate, 60, "GPU", "Frames a second: 30 or 60");
REXCVAR_DEFINE_BOOL(full_speed, true, "GPU",
                    "Keep the game at full speed when frames are slower than 60 (extra world updates); off: one "
                    "update a frame, as the console (slower when the device can't make 60)");

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kTiming = 0x82EDDBE8;
constexpr uint32_t kMatchFrames = 0x82E3CD0C;  // the match's frame count (its clock: / fps)
constexpr uint32_t kChars = 0x82E3CC50;        // the wrestlers

uint8_t* g_base = nullptr;            // the guest's memory
rex::memory::Memory* g_memory = nullptr;  // (the thread dump: readable pages)
std::atomic<bool> g_scene_thirty{false};  // an entrance at the original's 30 fps
std::atomic<bool> g_lockstep{false};      // online: one world update per frame at 60
std::atomic<bool> g_in_match{false};      // 30 fps applies in matches only
std::atomic<void (*)()> g_match_start{nullptr};  // armed match start (ArmMatchStart)

void Wr32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
uint32_t Rd32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
float RdF(const uint8_t* p) {
  const uint32_t v = Rd32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void WrF(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Wr32(p, v);
}

// The frame clock: the frames the game draws a second.
void SetFrameClock() {
  const int fps = g_lockstep ? 60 : g_scene_thirty ? 30 : svr2011::FrameRateNow();
  rex::cvar::SetFlagByName("guest_vblank_hz", std::to_string(fps));
}

void StartDeveloperAids(uint8_t* base);

}  // namespace

namespace svr2011 {

int TargetFrameRate() {
  return REXCVAR_GET(frame_rate) == 30 ? 30 : 60;  // (older settings' 120 / 144 / 240: 60)
}

int FrameRateNow() {
  // (test aid: SVR2011_TEST_IN_MATCH=1 - the chosen rate everywhere, e.g. the training ring)
  static const bool always = std::getenv("SVR2011_TEST_IN_MATCH") != nullptr;
  if (!REXCVAR_GET(full_speed)) return 60;  // (30 needs two updates a frame)
  return g_in_match || always ? TargetFrameRate() : 60;
}

void SetFrameRateInMatch(bool on) {
  if (!on) g_match_start = nullptr;
  if (g_in_match.exchange(on) != on) SetFrameClock();
}

void ArmMatchStart(void (*on_start)()) { g_match_start = on_start; }

void InstallFrameRate(rex::memory::Memory* memory) {
  if (!memory) return;
  g_memory = memory;
  g_base = memory->virtual_membase();
  SetFrameClock();
  REXLOG_INFO("frame rate: {} fps (the game draws {})", TargetFrameRate(), FrameRateNow());
  StartDeveloperAids(g_base);
}

// The timing block for `fps` frames a second.
void WriteTiming(uint8_t* base, int fps) {
  uint8_t* t = base + kTiming;
  Wr32(t + 8, uint32_t(fps));
  WrF(t + 28, 1.0f / float(fps));
  WrF(t + 32, float(fps));
  WrF(t + 36, 1000.0f / float(fps));
  Wr32(t + 40, uint32_t(1000 / fps));
  WrF(t + 60, 60.0f / float(fps));
  WrF(t + 64, float(fps) / 60.0f);
}

void ApplyFrameRate(uint8_t* base) { WriteTiming(base, 60); }

void SetLockstep(bool on) {
  if (g_lockstep.exchange(on) == on) return;
  SetFrameClock();
  REXLOG_INFO("frame rate: online lockstep {}", on ? "on (one world update a frame, 60 Hz)" : "off");
}

void SetSceneThirtyFps(bool on) {
  if (g_scene_thirty.exchange(on) != on) SetFrameClock();
}

// GRAPHICS -> FRAME RATE: the chosen rate, now.
void SetTargetFrameRate(int fps) {
  rex::cvar::SetFlagByName("frame_rate", std::to_string(fps));
  SetFrameClock();
  REXLOG_INFO("frame rate: {} fps (GRAPHICS; the game draws {})", TargetFrameRate(), FrameRateNow());
}

}  // namespace svr2011

// -- The world at 60 Hz ------------------------------------------------------

namespace {

Clock::time_point g_world_last;
double g_world_acc = 0;  // 60 Hz ticks of real time not yet run
int g_world_ticks = 1;   // update passes this frame

constexpr int kMaxTicks = 2;  // (below 30 fps the game slows down rather than racing: 3-4 updates a frame - a
                               // window in the background, tabbing out - crashed a worker thread, sub_8281F5F0)

uint32_t G32(uint8_t* base, uint32_t a) { return Rd32(base + a); }
void P32(uint8_t* base, uint32_t a, uint32_t v) { Wr32(base + a, v); }

// What the world update is running: a watchdog logs it when the update is
// stuck (a wait that a frame with two updates would never see end).
std::atomic<uint32_t> g_dbg_obj{0}, g_dbg_fn{0}, g_dbg_pass{0};
std::atomic<rex::system::KernelState*> g_kernel{nullptr};

// Every guest thread: id, name, where it is (lr) and its call stack (the
// back chain from r1: saved lr at each frame's -8) - for a stuck update. Only
// reads inside the thread's own stack (its PCR: +0x70 base, high; +0x74 end,
// low), and stops at the first link that leaves it or doesn't go up.

void DumpGuestThreads(uint8_t* base) {
  auto* kernel = g_kernel.load();
  if (!kernel || !g_memory) return;
  // (a stack's range can hold pages never mapped - its untouched end: each
  // word is read only from a readable page)
  const auto readable = [](uint32_t a) {
    auto* heap = g_memory->LookupHeap(a);
    return heap && heap->QueryRangeAccess(a, a + 3) != rex::memory::PageAccess::kNoAccess;
  };
  for (auto& t : kernel->object_table()->GetObjectsByType<rex::system::XThread>()) {
    if (!t || !t->is_guest_thread() || !t->thread_state() || !t->pcr_ptr()) continue;
    const PPCContext* c = t->thread_state()->context();
    const uint32_t hi = Rd32(base + t->pcr_ptr() + 0x70), lo = Rd32(base + t->pcr_ptr() + 0x74);
    const auto in_stack = [&](uint32_t a) { return lo < hi && a >= lo && a + 4 <= hi && readable(a); };
    std::string chain = fmt::format("{:08X}", uint32_t(c->lr));
    uint32_t sp = c->r1.u32;
    for (int i = 0; i < 16 && in_stack(sp); ++i) {
      const uint32_t prev = Rd32(base + sp);
      if (prev <= sp || !in_stack(prev) || !in_stack(prev - 8)) break;
      chain += fmt::format(" {:08X}", Rd32(base + prev - 8));
      sp = prev;
    }
    REXLOG_WARN("frame rate: thread {:08X} '{}' at {}", t->thread_id(), t->name(), chain);
  }
}
std::atomic<int64_t> g_dbg_since{0};

// obj->vtable[slot](obj, r4, r5)
void Virt(PPCContext& ctx, uint8_t* base, uint32_t obj, int slot, uint32_t r4 = 0, uint32_t r5 = 0) {
  g_dbg_obj = obj;
  g_dbg_fn = G32(base, G32(base, obj) + slot * 4);
  g_dbg_since = Clock::now().time_since_epoch().count();
  ctx.r3.u64 = obj;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.ctr.u64 = G32(base, G32(base, obj) + slot * 4);
  REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

// The manager's systems (list +0..+8, then +68): their slot `slot`.
void Systems(PPCContext& ctx, uint8_t* base, uint32_t m, int slot, bool scheduler_first) {
  // (as the original: list ends read once before each loop, +68 read when used)
  if (scheduler_first)
    if (const uint32_t o = G32(base, m + 68)) Virt(ctx, base, o, slot);
  for (uint32_t p = G32(base, m + 0), end = G32(base, m + 8); p != end; p += 4) {
    const uint32_t o = G32(base, p);
    if (o != 0xFFFFFFFF) Virt(ctx, base, o, slot);
  }
  if (!scheduler_first)
    if (const uint32_t o = G32(base, m + 68)) Virt(ctx, base, o, slot);
}

// Drops the removed (-1) entries of the list at `l` (begin +0, end +8, dirty +12).
void Compact(uint8_t* base, uint32_t l) {
  if (G32(base, l + 12) == 0) return;
  uint32_t out = G32(base, l);
  for (uint32_t p = G32(base, l); p != G32(base, l + 8); p += 4)
    if (const uint32_t v = G32(base, p); v != 0xFFFFFFFF) P32(base, out, v), out += 4;
  P32(base, l + 8, out);
  P32(base, l + 12, 0);
}

void UpdatePass(PPCContext& ctx, uint8_t* base, uint32_t m) {
  Systems(ctx, base, m, 1, false);
  P32(base, m + 96, 1);
  const uint32_t groups = (G32(base, m + 40) - G32(base, m + 32)) >> 4;
  for (uint32_t g = 0; g < groups; ++g) {
    if (const uint32_t scheduler = G32(base, m + 68)) {
      Virt(ctx, base, scheduler, 4, g, G32(base, m + 80));
      if (ctx.r3.u32 == 0) continue;
    }
    const uint32_t list = G32(base, m + 32) + g * 16;
    for (uint32_t p = G32(base, list), end = G32(base, list + 8); p != end; p += 4) {
      const uint32_t t = G32(base, p);
      if (t != 0xFFFFFFFF && G32(base, t + 8) != 0) Virt(ctx, base, t, 1);
    }
  }
  ctx.r3.u64 = m;
  sub_8269C648(ctx, base);  // (the groups' lists)
  P32(base, m + 96, 0);
}

void DrawPass(PPCContext& ctx, uint8_t* base, uint32_t m) {
  Systems(ctx, base, m, 2, false);
  P32(base, m + 100, 1);
  constexpr uint32_t kRenderQueue = 0x82ED6030;
  ctx.r3.u64 = G32(base, kRenderQueue);
  sub_826A6018(ctx, base);
  ctx.r3.u64 = G32(base, kRenderQueue);
  sub_826A6080(ctx, base);
  P32(base, m + 100, 0);
  ctx.r3.u64 = m;
  sub_8269C6B8(ctx, base);
  Compact(base, m + 16);
  Systems(ctx, base, m, 3, true);
}

// The controllers are read in the update pass (a system's slot 1 ->
// sub_826CA070 -> XamInputGetState through sub_82905058), and some menus look
// at the presses after the update. With two updates in a frame a press would
// be new in the first and held in the second - lost to those menus - so all
// but the last update of a frame (an extra update) see the previous reading
// again (no change): a press arrives in the frame's last update.
bool g_extra_update = false;
std::atomic<uint64_t> g_lat_drawn{0};  // (latency test aid: frames drawn)
// Render-thread commands the logic thread queued this frame (see
// sub_8269B2D0 below).
std::vector<uint32_t> g_frame_commands;
std::atomic<std::thread::id> g_logic_thread{};  // (the thread running the world update)
// The characters' job's round (sub_8216F4C8, below) or its paused round
// (sub_8216ED38 + sub_8216E458) an extra update left, for the frame's last
// update - or, when that one reached neither, for before the draw.
enum class JobOwed { kNone, kRound, kPaused };
JobOwed g_job_owed = JobOwed::kNone;
uint32_t g_job_paused_obj = 0;   // (sub_8216ED38's r3)
bool g_job_paused_e458 = false;  // (sub_8216E458 followed it)
struct InputReading {
  bool valid = false;
  uint32_t result = 0;
  uint8_t state[16] = {};  // XINPUT_STATE
};
InputReading g_input[8];

// Test aid: SVR2011_TEST_MATCH_TIME=<s> - 20 s into a match its time jumps to
// <s> (a timed match then ends on its own).
void TestMatchTime(uint8_t* base) {
  static const double test_time = [] {
    const char* v = std::getenv("SVR2011_TEST_MATCH_TIME");
    return v ? std::atof(v) : 0.0;
  }();
  if (test_time <= 0) return;
  static uint32_t last = 0;
  static bool jumped = false;
  const uint32_t frames = G32(base, kMatchFrames);
  if (frames < last) jumped = false;  // (a new match)
  last = frames;
  if (!jumped && frames > 20 * 60 && frames < test_time * 60) {
    jumped = true;
    P32(base, kMatchFrames, uint32_t(test_time * 60));
    REXLOG_INFO("frame rate: test - match time set to {} s", test_time);
  }
}

}  // namespace

// The world update, once a frame of the game's loop: how many 60 Hz ticks
// this frame runs (at least one).
REX_EXTERN(__imp__sub_8269D768);
REX_HOOK_RAW(sub_8269D768) {
  if (!g_kernel) g_kernel = REX_KERNEL_STATE();  // (for the stuck-update thread dump)
  // Test aid: SVR2011_TEST_SLOW_MS=<ms> - a slow PC (each frame that much longer).
  static const int slow_ms = [] { const char* v = std::getenv("SVR2011_TEST_SLOW_MS"); return v ? std::atoi(v) : 0; }();
  // (SVR2011_TEST_SLOW_IN_MATCH=1: only while a match runs - the menus at speed for the scripted route)
  static const bool slow_in_match = std::getenv("SVR2011_TEST_SLOW_IN_MATCH") != nullptr;
  if (slow_ms > 0 && (!slow_in_match || g_in_match)) std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));
  const auto now = Clock::now();
  if (g_world_last == Clock::time_point{}) g_world_last = now - std::chrono::microseconds(16667);
  g_world_acc += std::chrono::duration<double>(now - g_world_last).count() * 60.0;
  g_world_last = now;
  g_world_ticks = std::clamp(int(g_world_acc), 1, kMaxTicks);
  g_world_acc = std::clamp(g_world_acc - g_world_ticks, -0.5, 1.0);
  if (g_lockstep || !REXCVAR_GET(full_speed)) g_world_ticks = 1, g_world_acc = 0;
  __imp__sub_8269D768(ctx, base);
  TestMatchTime(base);
  svr2011::MatchTypesUpdate(ctx, base);  // (match_types.h: the lumberjacks, Slobber Knocker)
  svr2011::JukeboxUpdate(ctx, base);     // (jukebox.h: a song stopped, skipped or previewed)
  // An armed match start (ArmMatchStart): once the match's frame count has
  // gone up 30 updates running.
  static uint32_t last_frames = 0;
  static int counting = 0;
  const uint32_t frames = G32(base, kMatchFrames);
  counting = frames > last_frames ? counting + 1 : 0;
  last_frames = frames;
  if (counting >= 30)
    if (auto start = g_match_start.exchange(nullptr)) start();

}

// The task manager: the update pass per tick, the draw pass once (see top).
REX_EXTERN(__imp__sub_8269C728);
REX_HOOK_RAW(sub_8269C728) {
  g_logic_thread = std::this_thread::get_id();
  const auto saved = ctx;
  const uint32_t m = ctx.r3.u32;
  constexpr uint32_t kOwner = 0x82EC5F94;
  const uint32_t owner = G32(base, kOwner);
  ctx.r3.u64 = owner + 108;
  REX_CALL_INDIRECT_FUNC(0x82D4753Cu);  // (RtlEnterCriticalSection)
  P32(base, owner + 136, 1);
  g_job_owed = JobOwed::kNone;
  g_frame_commands.clear();  // (last frame's ran at the barrier)
  for (int i = 0; i < g_world_ticks; ++i) {
    g_dbg_pass = 10 + i;
    g_extra_update = i + 1 < g_world_ticks;
    UpdatePass(ctx, base, m);
  }
  g_extra_update = false;
  if (g_job_owed != JobOwed::kNone) {  // (only an extra update reached the job this frame)
    const auto before = ctx;
    const bool round = g_job_owed == JobOwed::kRound;
    if (round) {
      sub_8216F4C8(ctx, base);
    } else {
      ctx.r3.u64 = g_job_paused_obj;
      sub_8216ED38(ctx, base);
      if (g_job_paused_e458) {
        ctx.r3.u64 = g_job_paused_obj;
        sub_8216E458(ctx, base);
      }
    }
    g_job_owed = JobOwed::kNone;
    ctx = before;
    static int late[2] = {};
    const int n = ++late[round];
    if (n == 1 || n % 100 == 0)
      REXLOG_INFO("frame rate: the characters' job {} ran before the draw ({} times)", round ? "round" : "paused round", n);
  }
  // Test aid: SVR2011_TEST_STUCK=1 - one update 5 s long, 60 s into the run
  // (the stuck-update log and its thread dump).
  static const bool test_stuck = std::getenv("SVR2011_TEST_STUCK") != nullptr;
  static const auto started = Clock::now();
  static bool stuck_done = false;
  if (test_stuck && !stuck_done && Clock::now() - started > std::chrono::seconds(60)) {
    stuck_done = true;
    g_dbg_pass = 20;
    g_dbg_since = Clock::now().time_since_epoch().count();
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }
  g_dbg_pass = 20;
  DrawPass(ctx, base, m);
  ++g_lat_drawn;
  g_dbg_pass = 0;
  g_dbg_since = Clock::now().time_since_epoch().count();
  static std::once_flag watchdog;
  // Test aid: SVR2011_TEST_HITCH_MS=<ms> - an update or draw pass running
  // longer than that: every guest thread's back chain (once per pass; what a
  // stutter waits on).
  static std::once_flag hitch;
  std::call_once(hitch, [] {
    const char* v = std::getenv("SVR2011_TEST_HITCH_MS");
    const int ms = v ? std::atoi(v) : 0;
    if (ms <= 0) return;
    std::thread([ms] {
      int64_t logged_for = -1;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const int64_t s = g_dbg_since.load();
        const auto since = Clock::time_point(Clock::duration(s));
        if (g_dbg_pass != 0 && s != logged_for && g_base && Clock::now() - since > std::chrono::milliseconds(ms)) {
          logged_for = s;
          REXLOG_WARN("frame rate: test - pass {} over {} ms - object {:08X} function {:08X}", g_dbg_pass.load(), ms,
                      g_dbg_obj.load(), g_dbg_fn.load());
          DumpGuestThreads(g_base);
        }
      }
    }).detach();
  });
  std::call_once(watchdog, [] {
    std::thread([] {
      // (test aid: SVR2011_TEST_DUMP_AT=<s> - every guest thread's back chain once, <s> s after the start)
      static const int dump_at = [] { const char* v = std::getenv("SVR2011_TEST_DUMP_AT"); return v ? std::atoi(v) : 0; }();
      const auto started = Clock::now();
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        static bool dumped = false;
        if (dump_at > 0 && !dumped && g_base && Clock::now() - started > std::chrono::seconds(dump_at)) {
          dumped = true;
          REXLOG_INFO("frame rate: test - thread dump");
          DumpGuestThreads(g_base);
        }
        const auto since = Clock::time_point(Clock::duration(g_dbg_since.load()));
        if (g_dbg_pass != 0 && Clock::now() - since > std::chrono::seconds(3)) {
          std::string jobs;
          if (uint8_t* b = g_base) {
            const uint32_t q = Rd32(b + 0x82ED6030);
            const uint32_t n = q ? Rd32(b + q + 8) : 0;
            for (uint32_t i = 0; i < n && i < 16; ++i) {
              const uint32_t w = Rd32(b + Rd32(b + q + 4) + i * 4);
              if (w)
                jobs += fmt::format(" [{}: {:08X} sync {} fn {:08X} n {} +24 {:08X} +28 {:08X} +32 {:08X} +44 {:08X} +48 {:08X}]",
                                    i, w, Rd32(b + w + 20), Rd32(b + w + 52), Rd32(b + w + 16), Rd32(b + w + 24),
                                    Rd32(b + w + 28), Rd32(b + w + 32), Rd32(b + w + 44), Rd32(b + w + 48));
            }
          }
          REXLOG_WARN("frame rate: world update stuck {} s in pass {} (1x: update x+1, 20: draw) - object {:08X} "
                      "function {:08X}; jobs{}",
                      std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - since).count(),
                      g_dbg_pass.load(), g_dbg_obj.load(), g_dbg_fn.load(), jobs);
          static int64_t dumped_for = -1;  // (once per stuck update)
          if (dumped_for != g_dbg_since.load() && g_base) {
            dumped_for = g_dbg_since.load();
            DumpGuestThreads(g_base);
          }
        }
      }
    }).detach();
  });
  const uint32_t owner_now = G32(base, kOwner);
  ctx.r3.u64 = owner_now + 108;
  REX_CALL_INDIRECT_FUNC(0x82D4754Cu);  // (RtlLeaveCriticalSection)
  P32(base, owner_now + 136, 0);
  ctx.r1 = saved.r1;
  ctx.lr = saved.lr;
}

// The characters' job ('CHPH': the wrestlers' poses and the physics world, on
// its own thread). sub_8216F4C8, from the update's group gate, waits for the
// job the last update started, takes its results and starts the next one;
// the job (sub_82171940 a run) steps by the timing block's 1/60. The job and
// the frame's draw depend on each other (double-buffered poses), so it runs
// once a frame, as the game expects, in the frame's last update: extra updates
// leave it, and the job does the work of every tick since it was last started
// (sub_82171940 that many times on the job thread). The update's gate runs
// either the round or, while the game's world is held (a finisher's
// cinematic, a match's end), the paused round (sub_8216ED38, sub_8216E458) -
// which starts the job too: both count as the frame's one start. Only when
// the frame's last update reached neither does what an extra update left run
// before the draw (which waits for the job). (2.0.1-2.0.2 ran a left round
// before the draw even after the last update's paused round - two starts: a
// draw frozen on job 826DFD40, a match end that never came. Running the round
// in an extra update itself freezes that update - sub_8243C568 waits for the
// draw.)
namespace {
std::atomic<int> g_job_ticks{1};  // ticks the next / running job stands for
int g_job_pending = 0;            // ticks since the job was last started
}  // namespace

REX_EXTERN(__imp__sub_8216F4C8);
REX_HOOK_RAW(sub_8216F4C8) {
  ++g_job_pending;
  if (g_extra_update) {  // (an extra update of this frame)
    g_job_owed = JobOwed::kRound;
    return;
  }
  g_job_owed = JobOwed::kNone;
  // (the running job must be done before its tick count changes: waited for
  // as the game does just after - the event stays set for it)
  constexpr uint32_t kJob = 0x82DE9C88;
  const auto saved = ctx;
  sub_8216E750(ctx, base);
  const uint32_t job = G32(base, kJob);
  if (ctx.r3.u32 == 0 && job && G32(base, job + 104) == 0 && G32(base, job + 92) != 0) {
    ctx.r3.u64 = G32(base, job + 276);
    ctx.r4.u64 = uint64_t(-1);
    sub_8215A8C0(ctx, base);
  }
  ctx = saved;
  g_job_ticks = std::clamp(g_job_pending, 1, kMaxTicks);
  g_job_pending = 0;
  __imp__sub_8216F4C8(ctx, base);
}

// The paused round (see above). Test aid SVR2011_TEST_JOB_LAST=1: 2.0.2's way
// (paused rounds in every update; a left round still runs before the draw).
REX_EXTERN(__imp__sub_8216ED38);
REX_HOOK_RAW(sub_8216ED38) {
  static const bool old_way = std::getenv("SVR2011_TEST_JOB_LAST") != nullptr;
  if (g_extra_update && !old_way) {
    g_job_owed = JobOwed::kPaused, g_job_paused_obj = ctx.r3.u32, g_job_paused_e458 = false;
    return;
  }
  if (!old_way) g_job_owed = JobOwed::kNone;
  __imp__sub_8216ED38(ctx, base);
}

REX_EXTERN(__imp__sub_8216E458);
REX_HOOK_RAW(sub_8216E458) {
  static const bool old_way = std::getenv("SVR2011_TEST_JOB_LAST") != nullptr;
  if (g_extra_update && !old_way) {
    if (g_job_owed == JobOwed::kPaused) g_job_paused_e458 = true;
    return;
  }
  __imp__sub_8216E458(ctx, base);
}

REX_EXTERN(__imp__sub_82171940);
REX_HOOK_RAW(sub_82171940) {
  const auto saved = ctx;
  const int ticks = g_job_ticks.load();
  for (int i = 0; i < ticks; ++i) {
    ctx = saved;
    __imp__sub_82171940(ctx, base);
  }
}

// XamInputGetState(user, state) for the game: sub_82905058.
REX_EXTERN(__imp__sub_82905058);
REX_HOOK_RAW(sub_82905058) {
  ctx.r3.u64 = ctx.r3.u32 + svr2011::PadUserOffset();  // (pads 5-8: players.h)
  const uint32_t user = ctx.r3.u32, out = ctx.r4.u32;
  InputReading* r = user < 8 ? &g_input[user] : nullptr;
  if (g_extra_update && r && r->valid && out) {
    std::memcpy(base + out, r->state, sizeof(r->state));
    ctx.r3.u64 = r->result;
    return;
  }
  __imp__sub_82905058(ctx, base);
  if (r && out) {
    r->valid = true;
    r->result = ctx.r3.u32;
    std::memcpy(r->state, base + out, sizeof(r->state));
    if (user == 0 && ctx.r3.u32 == 0) svr2011::OnlineOverlayPad(uint16_t(base[out + 4] << 8 | base[out + 5]));  // (online_overlay.h)
  }
}

// -- Developer aids ----------------------------------------------------------

namespace {

void StartDeveloperAids(uint8_t* base) {
  // SVR2011_FPS_PROBE=1 logs, every second, the match frames (60 a second at
  // the game's speed), the frames shown and wrestler 1's position.
  if (const char* v = std::getenv("SVR2011_FPS_PROBE"); v && (*v == '1' || *v == '2')) {
    std::thread([base, v] {
      uint32_t last = Rd32(base + kMatchFrames);
      uint64_t last_presents = rex::ui::HostPresentCount(), last_new = rex::ui::HostNewGuestFramePresentCount();
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const uint32_t frames = Rd32(base + kMatchFrames);
        const uint32_t ch = Rd32(base + kChars), ch2 = Rd32(base + kChars + 4);
        const float x = ch ? RdF(base + ch + 288) : 0, z = ch ? RdF(base + ch + 296) : 0;
        const float x2 = ch2 ? RdF(base + ch2 + 288) : 0, z2 = ch2 ? RdF(base + ch2 + 296) : 0;
        const uint64_t presents = rex::ui::HostPresentCount(), shown = rex::ui::HostNewGuestFramePresentCount();
        REXLOG_INFO("fps probe: {} game frames/s, {} shown ({} presents), wrestler 1 at ({:.1f}, {:.1f}), 2 at "
                    "({:.1f}, {:.1f})",
                    frames - last, shown - last_new, presents - last_presents, x, z, x2, z2);
        if (*v == '2') {  // (SVR2011_FPS_PROBE=2: all six, x y z)
          std::string all;
          for (uint32_t i = 0; i < 6; ++i)
            if (const uint32_t c = Rd32(base + kChars + i * 4))
              all += fmt::format(" {}:({:.0f},{:.0f},{:.0f} ai {})", i, RdF(base + c + 288), RdF(base + c + 292),
                                 RdF(base + c + 296), Rd32(base + c + 2572) ? Rd32(base + Rd32(base + c + 2572) + 168) : 99);
          REXLOG_INFO("fps probe: all{}", all);
        }
        last = frames;
        last_presents = presents;
        last_new = shown;
      }
    }).detach();
  }
  // SVR2011_FPS_DUMP=<file> appends, every 100 ms, the real time, the match
  // frames and the first 64 KB of wrestlers 1 and 2 (tools/rate_diff.py
  // compares two runs).
  if (const char* path = std::getenv("SVR2011_FPS_DUMP"); path && *path) {
    std::thread([base, file = std::string(path)] {
      constexpr uint32_t kSize = 0x10000;
      const auto t0 = Clock::now();
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const uint32_t a = Rd32(base + kChars), b = Rd32(base + kChars + 4);
        if (!a || !b) continue;
        if (FILE* f = std::fopen(file.c_str(), "ab")) {
          const double t = std::chrono::duration<double>(Clock::now() - t0).count();
          const uint32_t frames = Rd32(base + kMatchFrames);
          std::fwrite(&t, 8, 1, f);
          std::fwrite(&frames, 4, 1, f);
          std::fwrite(&a, 4, 1, f);
          std::fwrite(&b, 4, 1, f);
          std::fwrite(base + a, 1, kSize, f);
          std::fwrite(base + b, 1, kSize, f);
          std::fclose(f);
        }
      }
    }).detach();
  }
}

}  // namespace


// A frame's callbacks for another thread (sub_826E0F78 queues them,
// sub_826E1260 runs them once a frame): with two updates in a frame each one
// is queued twice, and the second update may let go of what the first one's
// entry points at. sub_8217A718 (queued by sub_8217BC90 with the object at
// *0x82DEA30C) takes the object's part at +156 - crashed on a PC at 30 fps
// (2.0.2) when it was already gone: without it there is nothing to do.
REX_EXTERN(__imp__sub_8217A718);
REX_HOOK_RAW(sub_8217A718) {
  if (ctx.r3.u32 == 0 || Rd32(base + ctx.r3.u32 + 156) == 0) {
    static int skipped = 0;
    if (++skipped == 1 || skipped % 100 == 0)
      REXLOG_INFO("frame rate: a queued callback's object was gone - skipped ({} times)", skipped);
    return;
  }
  __imp__sub_8217A718(ctx, base);
}

// A pure virtual call (sub_828F3F38, the runtime's _purecall: error R6025,
// the game quits): with two updates in a frame the second may destroy an
// object the first queued for the render thread (render command 24,
// sub_826DE6B0: obj->vtable[6] there), its vtable then the base class's - a
// PC at 30 fps crashed so (2.0.2). The call on the destroyed object is
// skipped instead (the caller's lr and object logged).
REX_EXTERN(__imp__sub_828F3F38);
REX_HOOK_RAW(sub_828F3F38) {
  static int count = 0;
  if (++count <= 20)
    REXLOG_WARN("frame rate: a pure virtual call skipped (caller {:08X}, object {:08X}) - {} so far", uint32_t(ctx.lr),
                ctx.r3.u32, count);
  ctx.r3.u64 = 0;
}

// The job system's "wait for job idx" (sub_8216A450(jobs, idx): if job
// idx's pending flag (+(idx+14)*4) is set, wait on its event (+(idx+5)*4)
// for ever, then clear the flag). In an extra update a task may wait so for
// the characters' job, which only the frame's last update starts: a world
// update stuck for ever (a player tabbing out of fullscreen - long frames,
// 3-4 updates each - froze in pass 12, function 82225508, c27d191). There
// the wait is at most 50 ms: a running job ends well within it, as before;
// one not started, the flag stays for the frame's last update or the draw.
REX_EXTERN(__imp__sub_8216A450);
REX_HOOK_RAW(sub_8216A450) {
  if (!g_extra_update || std::this_thread::get_id() != g_logic_thread.load()) {
    __imp__sub_8216A450(ctx, base);
    return;
  }
  const uint32_t jobs = ctx.r3.u32, idx = ctx.r4.u32;
  const uint32_t flag = jobs + (idx + 14) * 4;
  if (Rd32(base + flag) == 0) return;
  const auto saved = ctx;
  ctx.r3.u64 = Rd32(base + jobs + (idx + 5) * 4);
  ctx.r4.u64 = 50;  // (ms)
  sub_8215A8C0(ctx, base);
  const uint32_t status = ctx.r3.u32;
  ctx = saved;
  if (status == 0) {  // (signalled: done)
    base[flag] = base[flag + 1] = base[flag + 2] = base[flag + 3] = 0;
    return;
  }
  static int count = 0;
  if (++count == 1 || count % 1000 == 0)
    REXLOG_INFO("frame rate: an extra update didn't wait for job {} (not done yet; {} times)", idx, count);
}

// -- Latency test aid (SVR2011_TEST_LATENCY=1) -------------------------------
//
// Frames drawn (the draw pass) against frames the native renderer published
// (PublishFrame, in order: one per game swap) = frames in flight from the
// game's draw to the renderer's hand-over; then the time from a publish to
// the presenter's next new game frame (a 1 ms poll). Once a second.
namespace {
std::atomic<uint64_t> g_lat_published{0}, g_lat_swapped{0};
std::atomic<int64_t> g_lat_publish_at{0};
}  // namespace

namespace svr2011 {
void LatencyOnSwap() { ++g_lat_swapped; }
void LatencyOnPublish() {
  static const bool on = std::getenv("SVR2011_TEST_LATENCY") != nullptr;
  if (!on) return;
  ++g_lat_published;
  g_lat_publish_at = Clock::now().time_since_epoch().count();
  static std::once_flag started;
  std::call_once(started, [] {
    std::thread([] {
      uint64_t seen = rex::ui::HostNewGuestFramePresentCount();
      double sum_ms = 0, max_ms = 0;
      int n = 0;
      int64_t min_inflight = INT64_MAX, max_inflight = INT64_MIN;
      auto last_log = Clock::now();
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const uint64_t shown = rex::ui::HostNewGuestFramePresentCount();
        const int64_t inflight = int64_t(g_lat_drawn.load()) - int64_t(g_lat_published.load());
        min_inflight = std::min(min_inflight, inflight), max_inflight = std::max(max_inflight, inflight);
        if (shown != seen) {
          seen = shown;
          const auto at = Clock::time_point(Clock::duration(g_lat_publish_at.load()));
          const double ms = std::chrono::duration<double, std::milli>(Clock::now() - at).count();
          sum_ms += ms, max_ms = std::max(max_ms, ms), ++n;
        }
        if (Clock::now() - last_log >= std::chrono::seconds(1)) {
          last_log = Clock::now();
          REXLOG_INFO("frame rate: latency - drawn {} swapped {} published {}: drawn-published {}..{}, swapped-published "
                      "{}; publish -> shown avg {:.1f} ms, max {:.1f} ms ({} new frames shown)",
                      g_lat_drawn.load(), g_lat_swapped.load(), g_lat_published.load(), min_inflight, max_inflight,
                      int64_t(g_lat_swapped.load()) - int64_t(g_lat_published.load()), n ? sum_ms / n : 0.0, max_ms, n);
          sum_ms = max_ms = 0, n = 0, min_inflight = INT64_MAX, max_inflight = INT64_MIN;
        }
      }
    }).detach();
  });
}
}  // namespace svr2011

// The game's render-thread commands: sub_826DFFA0(queue, ?) hands out a
// command record (+0 type); type 24 (sub_826DE6B0, "set texture") calls its
// object's (+20) vtable[6] on the render thread, which runs the queue at the
// frame barrier. With two world updates in a frame the second may free that
// object first: the render thread then calls into freed memory - a pure
// virtual call (c36b4fd skips those) or, the memory reused, garbage (a weak
// PC crashed so, 2ac47b9). So the logic thread's commands of the frame are
// noted, and a free (operator delete: sub_8269B2D0(ptr, ?)) of a block
// holding a pending type-24 command's object turns that command into type 0
// (nothing). The free itself is made as always.
REX_EXTERN(__imp__sub_826DFFA0);
REX_HOOK_RAW(sub_826DFFA0) {
  __imp__sub_826DFFA0(ctx, base);
  if (ctx.r3.u32 && std::this_thread::get_id() == g_logic_thread.load() && g_frame_commands.size() < 100000)
    g_frame_commands.push_back(ctx.r3.u32);
}

REX_EXTERN(__imp__sub_8269B2D0);
REX_HOOK_RAW(sub_8269B2D0) {
  const uint32_t ptr = ctx.r3.u32;
  if (ptr && !g_frame_commands.empty() && std::this_thread::get_id() == g_logic_thread.load()) {
    constexpr uint32_t kWithin = 0x4000;  // (the object: at or just after the block's start)
    for (const uint32_t cmd : g_frame_commands) {
      if (Rd32(base + cmd) != 24) continue;
      const uint32_t obj = Rd32(base + cmd + 20);
      if (obj >= ptr && obj - ptr < kWithin) {
        Wr32(base + cmd, 0);
        static int count = 0;
        if (++count <= 20 || count % 1000 == 0)
          REXLOG_INFO("frame rate: a queued render command's object was freed - command dropped ({} times)", count);
      }
    }
  }
  __imp__sub_8269B2D0(ctx, base);
}
