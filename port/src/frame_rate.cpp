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

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>

#include "generated/default/svr2011_init.h"

REXCVAR_DEFINE_INT32(frame_rate, 60, "GPU", "Frames a second: 30 or 60");

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kTiming = 0x82EDDBE8;
constexpr uint32_t kMatchFrames = 0x82E3CD0C;  // the match's frame count (its clock: / fps)
constexpr uint32_t kChars = 0x82E3CC50;        // the wrestlers

uint8_t* g_base = nullptr;            // the guest's memory
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
  return g_in_match || always ? TargetFrameRate() : 60;
}

void SetFrameRateInMatch(bool on) {
  if (!on) g_match_start = nullptr;
  if (g_in_match.exchange(on) != on) SetFrameClock();
}

void ArmMatchStart(void (*on_start)()) { g_match_start = on_start; }

void InstallFrameRate(rex::memory::Memory* memory) {
  if (!memory) return;
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

constexpr int kMaxTicks = 4;  // (below 15 fps the game slows down rather than racing)

uint32_t G32(uint8_t* base, uint32_t a) { return Rd32(base + a); }
void P32(uint8_t* base, uint32_t a, uint32_t v) { Wr32(base + a, v); }

// What the world update is running: a watchdog logs it when the update is
// stuck (a wait that a frame with two updates would never see end).
std::atomic<uint32_t> g_dbg_obj{0}, g_dbg_fn{0}, g_dbg_pass{0};
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
struct InputReading {
  bool valid = false;
  uint32_t result = 0;
  uint8_t state[16] = {};  // XINPUT_STATE
};
InputReading g_input[4];

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
  // Test aid: SVR2011_TEST_SLOW_MS=<ms> - a slow PC (each frame that much longer).
  static const int slow_ms = [] { const char* v = std::getenv("SVR2011_TEST_SLOW_MS"); return v ? std::atoi(v) : 0; }();
  if (slow_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));
  const auto now = Clock::now();
  if (g_world_last == Clock::time_point{}) g_world_last = now - std::chrono::microseconds(16667);
  g_world_acc += std::chrono::duration<double>(now - g_world_last).count() * 60.0;
  g_world_last = now;
  g_world_ticks = std::clamp(int(g_world_acc), 1, kMaxTicks);
  g_world_acc = std::clamp(g_world_acc - g_world_ticks, -0.5, 1.0);
  if (g_lockstep) g_world_ticks = 1, g_world_acc = 0;
  __imp__sub_8269D768(ctx, base);
  TestMatchTime(base);
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
  const auto saved = ctx;
  const uint32_t m = ctx.r3.u32;
  constexpr uint32_t kOwner = 0x82EC5F94;
  const uint32_t owner = G32(base, kOwner);
  ctx.r3.u64 = owner + 108;
  REX_CALL_INDIRECT_FUNC(0x82D4753Cu);  // (RtlEnterCriticalSection)
  P32(base, owner + 136, 1);
  for (int i = 0; i < g_world_ticks; ++i) {
    g_dbg_pass = 10 + i;
    g_extra_update = i + 1 < g_world_ticks;
    UpdatePass(ctx, base, m);
  }
  g_extra_update = false;
  g_dbg_pass = 20;
  DrawPass(ctx, base, m);
  g_dbg_pass = 0;
  g_dbg_since = Clock::now().time_since_epoch().count();
  static std::once_flag watchdog;
  std::call_once(watchdog, [] {
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
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
// once a frame, as the game expects: extra updates leave it, and the job does
// the work of every tick since it was last started (sub_82171940 that many
// times on the job thread).
namespace {
std::atomic<int> g_job_ticks{1};  // ticks the next / running job stands for
int g_job_pending = 0;            // ticks since the job was last started
}  // namespace

REX_EXTERN(__imp__sub_8216F4C8);
REX_HOOK_RAW(sub_8216F4C8) {
  ++g_job_pending;
  if (g_extra_update) return;  // (an extra update of this frame)
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
  const uint32_t user = ctx.r3.u32, out = ctx.r4.u32;
  InputReading* r = user < 4 ? &g_input[user] : nullptr;
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

