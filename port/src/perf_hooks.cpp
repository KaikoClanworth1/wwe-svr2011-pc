// WWE SmackDown vs. Raw 2011 - CPU savings in the game's D3D library.
//
// The render thread waits for the GPU (ring-buffer space, and so the frame
// rate and vsync) in a poll loop: the caller calls sub_82919F18 until it
// returns 0, hundreds of thousands of times a frame. On the Xbox 360 each call
// first idles the hardware thread (8 x db16cyc); recompiled, those are gone and
// the loop spins a whole host core flat out - a core the game's simulation,
// audio and emulated GPU threads need on 4-core PCs, the Steam Deck and
// phones (and heat that lowers clocks on laptops and phones). Here a wait
// yields the core: a pause per poll, and after a short spin the thread gives
// its time slice away, then sleeps briefly on long waits.

#include "perf_hooks.h"

#include <chrono>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"

REXCVAR_DEFINE_BOOL(gpu_wait_yield, true, "GPU",
                    "Let the render thread's GPU waits yield the CPU core instead of spinning");
REXCVAR_DEFINE_INT32(process_priority, 0, "GPU",
                     "The game's CPU priority: 0 normal, 1 above normal (busy PCs: background programs at normal "
                     "priority no longer take the game's cores)");

namespace svr2011 {

// The process's CPU priority (perf_hooks.h).
void InstallPerfSettings() {
#if defined(_WIN32)
  if (REXCVAR_GET(process_priority) >= 1)
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
#endif
}

}  // namespace svr2011

namespace {

inline void CpuPause() {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield");
#endif
}

// Polls in the current wait on this thread, when it began and the last poll.
thread_local uint32_t t_polls = 0;
thread_local std::chrono::steady_clock::time_point t_wait_start, t_last_poll;

// Sleeps about `us` microseconds (Windows: a high-resolution timer; Sleep's
// granularity is a millisecond, too coarse to wait for a vsync).
void ShortSleep(int us) {
#if defined(_WIN32)
  thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                     TIMER_ALL_ACCESS);
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -int64_t(us) * 10;  // (relative, 100 ns units)
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
  SwitchToThread();
#else
  std::this_thread::sleep_for(std::chrono::microseconds(us));
#endif
}

}  // namespace

#ifndef SVR2011_D3D_TRACE  // (the census build hooks it to count calls)
// GPU wait poll: r3 = 1 while the GPU hasn't caught up (keep waiting), 0 to
// give up (a GPU hang).
REX_EXTERN(__imp__sub_82919F18);
REX_HOOK_RAW(sub_82919F18) {
  __imp__sub_82919F18(ctx, base);
  if (ctx.r3.u32 != 1 || !REXCVAR_GET(gpu_wait_yield)) {
    t_polls = 0;
    return;
  }
  // A new wait: the caller's loop stops calling once the GPU catches up, so a
  // gap between polls starts the next one.
  const auto now = std::chrono::steady_clock::now();
  if (t_polls == 0 || now - t_last_poll > std::chrono::microseconds(500)) {
    t_polls = 0;
    t_wait_start = now;
  }
  ++t_polls;
  for (int i = 0; i < 8; ++i) CpuPause();  // (the console's db16cyc idle)
  if (t_polls >= 64) {                      // short waits: stay on the core
    if (now - t_wait_start < std::chrono::microseconds(300)) {
      std::this_thread::yield();            // give other ready threads the core
    } else {
      ShortSleep(100);                      // a long wait (vsync, a busy GPU)
    }
  }
  t_last_poll = std::chrono::steady_clock::now();
}
#endif
