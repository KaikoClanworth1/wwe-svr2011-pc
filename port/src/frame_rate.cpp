// The game's frame rate - see frame_rate.h.
//
// The timing block at 0x82EDDBE8 (written by sub_826E1AE8):
//   +8  int   fps                  +28 float 1/fps (Havok's step)
//   +32 float fps                  +36 float 1000/fps     +40 int 1000/fps
//   +60 float 60/fps (the step in 60 Hz frames, what most code scales by)
//   +64 float fps/60
// At 30 fps the game also presents every 2nd vblank (sub_826D8A90), so the
// frame clock stays at 60 Hz there.
#include "frame_rate.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/presenter.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"

REXCVAR_DEFINE_INT32(frame_rate, 60, "GPU", "Frames a second: 30, 60, 120, 144 or 240");

namespace {

constexpr uint32_t kTiming = 0x82EDDBE8;

// The rate the game runs at now: the chosen one, or (when the PC can't keep
// up with it - the game would run slower than real time) a lower one.
std::atomic<int> g_rate{60};
uint8_t* g_base = nullptr;  // the guest's memory
bool g_own = true;          // the game's own timing block (60, or 30)

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

}  // namespace

namespace svr2011 {

int TargetFrameRate() {
  const int fps = REXCVAR_GET(frame_rate);
  switch (fps) {
    case 30:
    case 60:
    case 120:
    case 144:
    case 240:
      return fps;
    default:
      return 60;
  }
}

int FrameRateNow() { return g_rate.load(); }

void SetFrameClock(int fps) {
  // 30 fps is the game's own: every 2nd vblank of 60.
  rex::cvar::SetFlagByName("guest_vblank_hz", std::to_string(fps == 30 ? 60 : fps));
}

void InstallFrameRate(rex::memory::Memory* memory) {
  if (memory) g_base = memory->virtual_membase();
  const int fps = TargetFrameRate();
  g_rate = fps;
  SetFrameClock(fps);
  REXLOG_INFO("frame rate: {} fps", fps);
  // Developer aid: SVR2011_FPS_PROBE=1 logs, every second, the game frames
  // (the match frame counter) and wrestler 1's position - the game's speed.
  if (const char* v = std::getenv("SVR2011_FPS_PROBE"); v && *v == '1' && memory) {
    uint8_t* base = memory->virtual_membase();
    std::thread([base] {
      constexpr uint32_t kFrames = 0x82E3CD0C, kChars = 0x82E3CC50;
      uint32_t last = Rd32(base + kFrames);
      uint64_t last_presents = rex::ui::HostPresentCount(), last_new = rex::ui::HostNewGuestFramePresentCount();
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const uint32_t frames = Rd32(base + kFrames);
        const uint32_t ch = Rd32(base + kChars);
        const float x = ch ? RdF(base + ch + 288) : 0, z = ch ? RdF(base + ch + 296) : 0;
        const uint64_t presents = rex::ui::HostPresentCount(), shown = rex::ui::HostNewGuestFramePresentCount();
        REXLOG_INFO("fps probe: {} game frames/s, {} shown ({} presents), fps {} step {:.3f}, wrestler 1 at "
                    "({:.1f}, {:.1f})",
                    frames - last, shown - last_new, presents - last_presents, Rd32(base + kTiming + 8),
                    RdF(base + kTiming + 60), x, z);
        last = frames;
        last_presents = presents;
        last_new = shown;
      }
    }).detach();
  }
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
  g_rate = fps;
}

void ApplyFrameRate(uint8_t* base) {
  // 30: its timing; 60: the game's own (until the PC can't keep up); others:
  // the next frame writes its time.
  if (TargetFrameRate() == 30) WriteTiming(base, 30);
  else if (TargetFrameRate() > 60) WriteTiming(base, TargetFrameRate());
  else g_rate = 60;
}

}  // namespace svr2011

// -- Whole-frame steps -------------------------------------------------------
//
// A few places step by whole frames of 60 Hz: (int)(60/fps) per frame (the
// block's +60, or 60 / +8). At other rates that is wrong (0 above 60 fps -
// fades and light ramps would stand still). They run on 60 Hz ticks instead:
// each frame they see the 60 fps block if a 60 Hz tick has passed (twice
// that if two have), a block of no time otherwise.
namespace {

int g_ticks = 1;       // 60 Hz ticks in this frame
double g_tick_acc = 0;
int g_depth = 0;       // nesting of the functions below
uint8_t g_saved[68];

// The game's own timing (60, or its 30 fps mode): no change needed.
bool OwnRate() { return g_own || svr2011::TargetFrameRate() == 30; }

void Enter(uint8_t* base) {
  if (g_depth++ || OwnRate()) return;
  uint8_t* t = base + kTiming;
  std::memcpy(g_saved, t, sizeof(g_saved));
  const int ticks = g_ticks;
  const float step = float(ticks);
  const int fps = ticks ? 60 : std::max(61, g_rate.load());
  Wr32(t + 8, uint32_t(fps));
  WrF(t + 28, step / 60.0f);
  WrF(t + 32, float(fps));
  WrF(t + 36, step * 1000.0f / 60.0f);
  Wr32(t + 40, uint32_t(ticks * 1000 / 60));
  WrF(t + 60, step);
  WrF(t + 64, ticks ? 1.0f / step : float(fps) / 60.0f);
}

void Leave(uint8_t* base) {
  if (--g_depth || OwnRate()) return;
  std::memcpy(base + kTiming, g_saved, sizeof(g_saved));
}

}  // namespace

// -- Full speed at any frame rate ------------------------------------------
//
// The chosen rate is a cap (the frame clock runs at it). The game steps the
// time in its timing block once a frame, so like a PC game each frame gets
// the time the last one really took: fewer frames when the PC can't make
// more, the game at its normal speed. (At a 60 cap the game's own 60 fps
// block stays while the PC keeps up. Whole fps of 30, 25 and 50 are left out:
// the game treats them as its half-rate and PAL modes.) The match's time -
// its frame count / fps, the clock of timed matches - is kept to the real
// time it has run.
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kMatchFrames = 0x82E3CD0C;
Clock::time_point g_last;
double g_frame_s = 0;      // smoothed frame time (the 60 cap's own-block test)
double g_last_dt = 1.0 / 60;
double g_match_time = 0;   // the match's real time, s
uint32_t g_written = 0;    // the match frame count as last written
bool g_test_jumped = false;

void WriteFrameTime(uint8_t* base, double dt) {
  uint8_t* t = base + kTiming;
  int fps = int(1.0 / dt + 0.5);
  if (fps == 30 || fps == 25 || fps == 50) ++fps;
  Wr32(t + 8, uint32_t(fps));
  WrF(t + 28, float(dt));
  WrF(t + 32, float(1.0 / dt));
  WrF(t + 36, float(dt * 1000.0));
  Wr32(t + 40, uint32_t(dt * 1000.0));
  WrF(t + 60, float(dt * 60.0));
  WrF(t + 64, float(1.0 / (dt * 60.0)));
  g_rate = fps;
}

void CountFrame(uint8_t* base) {
  const auto now = Clock::now();
  double dt = g_last == Clock::time_point{} ? 0 : std::chrono::duration<double>(now - g_last).count();
  g_last = now;
  const int target = svr2011::TargetFrameRate();
  if (target == 30) return;  // the game's own 30 fps mode
  if (dt <= 0 || dt > 0.25) dt = 1.0 / target;  // (the first frame, or a load)
  dt = std::clamp(dt, 0.9 / target, 1.0 / 20);
  // 60 Hz ticks (the whole-frame steps), by real time.
  g_tick_acc += dt * 60.0;
  g_ticks = int(g_tick_acc);
  g_tick_acc -= g_ticks;
  g_frame_s = g_frame_s == 0 ? dt : g_frame_s + (dt - g_frame_s) * std::min(1.0, dt / 0.25);
  // The match's time: what the game counted since the last frame, at that
  // frame's step.
  const uint32_t count = Rd32(base + kMatchFrames);
  const int old_fps = g_rate.load();
  if (count < g_written) {
    g_match_time = count / double(old_fps);  // (a new match)
    g_test_jumped = false;
  } else {
    g_match_time += (count - g_written) * (g_own ? 1.0 / 60 : g_last_dt);
  }
  const bool own = target == 60 && g_frame_s <= 1.0 / 57;
  if (own) {
    if (!g_own) svr2011::WriteTiming(base, 60);
    g_own = true;
  } else {
    g_own = false;
    WriteFrameTime(base, dt);
  }
  // Test aid: SVR2011_TEST_MATCH_TIME=<s> - 20 s into a match its time jumps
  // to <s> (a timed match then ends on its own).
  static const double test_time = [] {
    const char* v = std::getenv("SVR2011_TEST_MATCH_TIME");
    return v ? std::atof(v) : 0.0;
  }();
  if (test_time > 0 && !g_test_jumped && g_match_time > 20 && g_match_time < test_time) {
    g_test_jumped = true;
    g_match_time = test_time;
    REXLOG_INFO("frame rate: test - match time set to {} s", test_time);
  }
  g_last_dt = dt;
  g_written = uint32_t(g_match_time * g_rate.load() + 0.5);
  Wr32(base + kMatchFrames, g_written);
}

}  // namespace

REX_EXTERN(__imp__sub_826E0B38);
REX_HOOK_RAW(sub_826E0B38) {
  CountFrame(base);
  __imp__sub_826E0B38(ctx, base);
}

#define SVR2011_SIXTY_HZ(addr)          \
  REX_EXTERN(__imp__sub_##addr);        \
  REX_HOOK_RAW(sub_##addr) {            \
    Enter(base);                        \
    __imp__sub_##addr(ctx, base);       \
    Leave(base);                        \
  }

SVR2011_SIXTY_HZ(82300410)
SVR2011_SIXTY_HZ(825A7278)
SVR2011_SIXTY_HZ(8276B988)
SVR2011_SIXTY_HZ(8276B250)
SVR2011_SIXTY_HZ(8277D730)
SVR2011_SIXTY_HZ(8277E3D8)
SVR2011_SIXTY_HZ(82872F80)
SVR2011_SIXTY_HZ(82885AC0)
SVR2011_SIXTY_HZ(82885BB0)
SVR2011_SIXTY_HZ(823AB6C8)
SVR2011_SIXTY_HZ(823C7B40)
SVR2011_SIXTY_HZ(8223D7E8)
SVR2011_SIXTY_HZ(825FD418)

namespace svr2011 {

// GRAPHICS -> FRAME RATE: the chosen rate, now.
void SetTargetFrameRate(int fps) {
  rex::cvar::SetFlagByName("frame_rate", std::to_string(fps));
  fps = TargetFrameRate();
  SetFrameClock(fps);
  if (!g_base) return;
  WriteTiming(g_base, fps);
  REXLOG_INFO("frame rate: {} fps (GRAPHICS)", fps);
}

}  // namespace svr2011
