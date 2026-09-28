// WWE SmackDown vs. Raw 2011 - guest frame timing (see frame_stats.h).

#include "frame_stats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>
#if __has_include(<rex/timeline.h>)  // patched SDK (built from source)
#include <rex/timeline.h>
#define SVR_TIMELINE_PRESENT(n) REX_TIMELINE(kPresent, n)
#else
#define SVR_TIMELINE_PRESENT(n)
#endif

#include "generated/default/svr2011_init.h"
#ifndef SVR2011_D3D_TRACE
#include "native/native_renderer.h"
#endif

namespace svr2011 {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kLogInterval = std::chrono::seconds(5);

std::mutex g_mutex;
Clock::time_point g_last_frame{};
Clock::time_point g_window_start{};
uint64_t g_frames = 0;
uint64_t g_window_frames = 0;
double g_window_worst_ms = 0;
// Frame times this window, by vblank count at 60 Hz: <=1, 2, 3, more.
uint32_t g_window_hist[4] = {};
double g_frame_ms = 0;    // smoothed
double g_fps = 0;         // over the last log window
// Recent frame times (ms) for the on-screen counter.
constexpr size_t kRecent = 256;
double g_recent[kRecent] = {};
size_t g_recent_next = 0;

void OnFramePresented() {
  std::lock_guard lock(g_mutex);
  const auto now = Clock::now();
  if (g_frames == 0) {
    g_window_start = now;
  } else {
    const double ms = std::chrono::duration<double, std::milli>(now - g_last_frame).count();
    g_frame_ms = g_frame_ms == 0 ? ms : g_frame_ms * 0.9 + ms * 0.1;
    g_window_worst_ms = std::max(g_window_worst_ms, ms);
    g_recent[g_recent_next++ % kRecent] = ms;
    ++g_window_hist[ms < 18.0 ? 0 : ms < 35.0 ? 1 : ms < 51.0 ? 2 : 3];
  }
  g_last_frame = now;
  ++g_frames;
  ++g_window_frames;

  const auto elapsed = now - g_window_start;
  if (elapsed >= kLogInterval) {
    const double secs = std::chrono::duration<double>(elapsed).count();
    g_fps = g_window_frames / secs;
    REXLOG_INFO(
        "fps: {:.1f} avg, worst frame {:.1f} ms ({} frames in {:.1f} s; frame times "
        "<18ms {} / <35ms {} / <51ms {} / longer {})",
        g_fps, g_window_worst_ms, g_window_frames, secs, g_window_hist[0], g_window_hist[1],
        g_window_hist[2], g_window_hist[3]);
    g_window_start = now;
    g_window_frames = 0;
    g_window_worst_ms = 0;
    std::fill(std::begin(g_window_hist), std::end(g_window_hist), 0u);
  }
}

}  // namespace

rex::ui::FrameStats GetFrameStats() {
  std::lock_guard lock(g_mutex);
  rex::ui::FrameStats stats;
  stats.frame_count = g_frames;
  stats.frame_time_ms = g_frame_ms;
  stats.fps = g_frame_ms > 0 ? 1000.0 / g_frame_ms : 0;
  return stats;
}

FrameTiming GetFrameTiming() {
  std::lock_guard lock(g_mutex);
  FrameTiming t;
  // The last second of frames, newest first.
  double times[kRecent];
  size_t n = 0;
  double total = 0;
  const size_t available = std::min(g_recent_next, kRecent);
  for (size_t i = 0; i < available && total < 1000.0; ++i) {
    const double ms = g_recent[(g_recent_next - 1 - i) % kRecent];
    times[n++] = ms;
    total += ms;
  }
  if (n == 0) return t;
  t.fps = n * 1000.0 / total;
  t.frame_ms = total / n;
  // 1% low: the frame rate of the slowest 1% of frames (at least one).
  std::sort(times, times + n, std::greater<double>());
  const size_t k = std::max<size_t>(1, n / 100);
  double worst = 0;
  for (size_t i = 0; i < k; ++i) worst += times[i];
  t.low_1pct_fps = 1000.0 / (worst / k);
  return t;
}

}  // namespace svr2011

// The game's D3D Present (Swap(device, front buffer texture)): builds the swap
// packet and calls VdSwap.
REX_EXTERN(__imp__sub_8291AED0);
REX_HOOK_RAW(sub_8291AED0) {
  const uint32_t front_buffer = ctx.r4.u32;
  __imp__sub_8291AED0(ctx, base);
  svr2011::OnFramePresented();
#ifndef SVR2011_D3D_TRACE
  if (svr2011::native::Enabled()) svr2011::native::OnPresent(front_buffer);
#endif
  SVR_TIMELINE_PRESENT(0);
}
