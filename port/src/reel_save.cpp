// WWE SmackDown vs. Raw 2011 - saving a video clip (pause menu > HIGHLIGHT
// REEL > VIDEO CLIP > SAVE) when the game's block heap is full.
//
// The block heap (sub_821DC338(size, align) -> a handle, 0 if no room): up to
// 8 regions, their bases at 0x82E368A8 (the init: sub_821DB908); region i's
// descriptor is 16 bytes at region 0's base + 16 * i (+0 first block, +4 size,
// +8 free bytes, +12 kind). Blocks are a list: +0 next, +4 previous, +8 size
// (bit 31: in use), +12 align, +16 the handle, data at +20. A handle is +0 the
// data, +4 the size asked for; the heap compacts (sub_821DC110 /
// sub_821DBF00) and tries again before it gives up. sub_821DB638(handle)
// frees. In a match: two 6.5 MB regions; region 0 holds ~3.6 MB (179 blocks,
// code at 0x82254000-0x82262000), region 1 is empty.
//
// The save (sub_8242D708, a state machine, state at +0x8CF4) in state 37 gets
// a 5.5 MB stream buffer from it (0x580000, align 16). For the post-match
// MATCH HIGHLIGHTS save (+2760 = 1) the game checks the handle and goes to
// its failure state 43 (frees what the save holds, a cancel sound, state 44
// leaves); for the in-match video clip it doesn't, and read through the null
// handle (a player's crash 12 minutes into a Match Creator match, CAW vs The
// Sandman, 2.0.6). Here state 37 gets the buffer first: if there's no room,
// it goes to 43 as the other save does and the screen says so; else the
// game's own call is handed that buffer. Both saves write NNRecordingDat.rec.
//
// The heap's state is logged at each video clip save, and at every failed
// allocation (once per caller): each region's size, free bytes and largest
// free block, and the biggest blocks in use with the code that asked for them
// (recorded here).
//
// Test aid: SVR2011_TEST_REEL_NO_ROOM=<n> - the n-th video clip save of the
// run finds no room for its buffer (1 = the first).

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "fps_overlay.h"
#include "generated/default/svr2011_init.h"

namespace {
// (guest 0xE0000000 and up - physical pages, where the heap's regions are - sit 4 KB further on the host)
uint8_t* At(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

constexpr uint32_t kRegions = 0x82E368A8;
constexpr uint32_t kSaveState = 0x8CF4;     // the save's state
constexpr uint32_t kSaveKind = 2760;        // 1 = post-match highlights, else the in-match video clip
constexpr uint32_t kStreamSize = 0x580000;  // the save's stream buffer
constexpr uint32_t kReelCall = 0x8242EBD8;  // (return address of the clip save's own allocation)

struct Owner { uint32_t caller, size; };
std::mutex g_mutex;
std::unordered_map<uint32_t, Owner> g_owners;  // handle -> who asked for it (the latest use of a handle)
std::set<uint32_t> g_logged;                   // callers whose failure was logged
uint32_t g_stash = 0;                          // the clip buffer got ahead (sub_8242D708 below)

// The heap's state (a save, or a failed allocation).
void LogHeap(uint8_t* base) {
  const uint32_t desc0 = Rd32(At(base, kRegions));
  if (!desc0) return;
  struct Used { uint32_t size, handle; };
  std::vector<Used> used;
  for (int i = 0; i < 8; ++i) {
    const uint32_t region = Rd32(At(base, kRegions + 4 * i));
    if (!region) continue;
    const uint32_t d = desc0 + 16 * i;
    const uint32_t region_size = Rd32(At(base, d + 4));
    uint32_t blocks = 0, in_use = 0, largest = 0, used_bytes = 0;
    uint32_t b = Rd32(At(base, d));
    for (; b && blocks < 200000; b = Rd32(At(base, b)), ++blocks) {
      if (b < region || b + 20 > region + region_size) break;  // (not this region's: stop reading)
      const uint32_t sz = Rd32(At(base, b + 8));
      if (sz & 0x80000000) {
        ++in_use, used_bytes += sz & 0x7FFFFFFF;
        used.push_back({sz & 0x7FFFFFFF, Rd32(At(base, b + 16))});
      } else {
        largest = std::max(largest, sz);
      }
    }
    REXLOG_WARN("[svr2011] block heap: region {} at {:08X}: size {} KB, free {} KB (largest {} KB), {} blocks in use "
                "({} KB){}",
                i, region, region_size / 1024, Rd32(At(base, d + 8)) / 1024, largest / 1024, in_use, used_bytes / 1024,
                b ? fmt::format(" - list left the region at block {} ({:08X}; descriptor {:08X}: {:08X} {:08X} {:08X} {:08X})",
                                blocks, b, d, Rd32(At(base, d)), Rd32(At(base, d + 4)), Rd32(At(base, d + 8)), Rd32(At(base, d + 12)))
                  : std::string());
  }
  std::sort(used.begin(), used.end(), [](const Used& a, const Used& b) { return a.size > b.size; });
  // Totals per caller too: many small blocks add up.
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> by_caller;  // caller -> (bytes, count)
  std::string top;
  {
    std::lock_guard lock(g_mutex);
    for (size_t k = 0; k < used.size(); ++k) {
      const auto it = g_owners.find(used[k].handle);
      const uint32_t caller = it != g_owners.end() ? it->second.caller : 0;
      auto& c = by_caller[caller];
      c.first += used[k].size, ++c.second;
      if (k < 16) top += fmt::format(" {}K@{:08X}", used[k].size / 1024, caller);
    }
  }
  REXLOG_WARN("[svr2011] block heap: biggest blocks (size@caller, 0 = before logging):{}", top);
  std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> callers(by_caller.begin(), by_caller.end());
  std::sort(callers.begin(), callers.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
  std::string per;
  for (size_t k = 0; k < callers.size() && k < 12; ++k)
    per += fmt::format(" {:08X}:{}K/{}", callers[k].first, callers[k].second.first / 1024, callers[k].second.second);
  REXLOG_WARN("[svr2011] block heap: in use by caller (KB/blocks):{}", per);
}
}  // namespace

REX_EXTERN(__imp__sub_821DC338);
REX_HOOK_RAW(sub_821DC338) {
  const uint32_t caller = ctx.lr;
  const uint32_t size = ctx.r3.u32, align = ctx.r4.u32;
  if (caller == kReelCall && size == kStreamSize) {
    std::lock_guard lock(g_mutex);
    if (g_stash) {
      ctx.r3.u64 = g_stash;
      g_stash = 0;
      return;
    }
  }
  __imp__sub_821DC338(ctx, base);
  const uint32_t handle = ctx.r3.u32;
  if (handle) {
    std::lock_guard lock(g_mutex);
    g_owners[handle] = {caller, size};
    return;
  }
  bool first;
  {
    std::lock_guard lock(g_mutex);
    first = g_logged.insert(caller).second;
  }
  REXLOG_WARN("[svr2011] block heap: no room for {} KB (align {}), asked by {:08X}{}", size / 1024, align, caller,
              first ? "" : " (again)");
  if (first) LogHeap(base);
}

// The save, each frame.
REX_EXTERN(__imp__sub_8242D708);
REX_EXTERN(__imp__sub_821DB638);
REX_HOOK_RAW(sub_8242D708) {
  const uint32_t save = ctx.r3.u32;
  if (Rd32(At(base, save + kSaveState)) != 37 || Rd32(At(base, save + kSaveKind)) == 1) {
    __imp__sub_8242D708(ctx, base);
    return;
  }
  static const int no_room_at = [] { const char* v = std::getenv("SVR2011_TEST_REEL_NO_ROOM"); return v ? std::atoi(v) : 0; }();
  static int saves = 0;
  const bool no_room = ++saves == no_room_at;
  REXLOG_INFO("[svr2011] video clip: saving; the block heap before its buffer:");
  LogHeap(base);
  const auto saved = ctx;
  uint32_t handle = 0;
  if (!no_room) {
    ctx.r3.u64 = kStreamSize, ctx.r4.u64 = 16;
    ctx.lr = 0x8242D708;  // (not the save's own call)
    sub_821DC338(ctx, base);
    handle = ctx.r3.u32;
  }
  ctx = saved;
  if (!handle) {
    REXLOG_WARN("[svr2011] video clip: no room for its {} KB buffer - not saved", kStreamSize / 1024);
    Wr32(At(base, save + kSaveState), 43);  // (the game's failure state, as for a replay)
    svr2011::ShowNotice("The video clip couldn't be saved: not enough memory left.", 8.0f);
    __imp__sub_8242D708(ctx, base);
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    g_stash = handle;
  }
  __imp__sub_8242D708(ctx, base);
  uint32_t left;
  {
    std::lock_guard lock(g_mutex);
    left = g_stash;
    g_stash = 0;
  }
  if (left) {  // (the state didn't get to the allocation)
    const auto after = ctx;
    ctx.r3.u64 = left;
    __imp__sub_821DB638(ctx, base);
    ctx = after;
  }
}
