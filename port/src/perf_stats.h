// WWE SmackDown vs. Raw 2011 - performance numbers for the overlay and the leak log.
//
// The process's RAM (working set, private bytes), the guest's own memory (the Xbox 360's 512 MB physical heap and
// the virtual heaps), the GPU's memory (gpu.h VideoMemory: DXGI on D3D12 / D3D11, VK_EXT_memory_budget on Vulkan),
// CPU use (the process and its busiest threads) and the native renderer's frame costs (native_renderer.h
// LastFrameCosts). Sampled only when asked (the overlay at level 2, or perf_log_seconds): nothing runs otherwise.
//
// perf_log_seconds=N: a "perf:" log line every N s with the same numbers, and a warning when RAM or VRAM keeps
// growing from one match to the next (a leak to hunt).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

struct PerfSample {
  double ram_ws_mb = 0, ram_private_mb = 0;     // the process
  double guest_physical_mb = 0, guest_physical_total_mb = 0, guest_virtual_mb = 0;
  bool vram = false;                              // the API told
  double vram_used_mb = 0, vram_budget_mb = 0;
  double cpu_pct = 0;                             // the process, of the whole machine
  int cores = 1;
  struct Thread {
    std::string name;
    double pct = 0;                               // of one core
  };
  std::vector<Thread> busiest;                    // the 3 busiest threads
  double draw_ms = 0, gpu_wait_ms = 0, submit_ms = 0, draws = 0, texture_mb = 0;  // the renderer, per frame
  double targets_mb = 0;  // the renderer's render targets (game VRAM = textures + these)
};

// The latest numbers (sampled again when the last are older than half a second).
PerfSample SamplePerf();
// A one-line summary (the log line, without the "perf:").
std::string PerfLine(const PerfSample& s);

// Keeps the guest memory for the heaps; starts the log thread when perf_log_seconds is set.
void InstallPerfStats(rex::memory::Memory* memory);

}  // namespace svr2011
