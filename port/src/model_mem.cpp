// WWE SmackDown vs. Raw 2011 - failed vertex / index buffer allocations.
//
// sub_82914DD0(size, align, flags) gets physical memory for the game's D3D
// buffers (through sub_82158C70) and returns 0 when there is no room. The
// character model loader (sub_826E73F8, per mesh: vertex count x a stride
// from the mesh's format flags, then sub_82915D98 locks it) doesn't check,
// and the null crashes the match load (lr 826E7494) - seen with a converted
// SvR 2009 model (Umaga: body + dreadlocks) that loads part by part.
//
// Every failed allocation is logged once per caller with its size, so a
// model that doesn't fit shows what it asked for.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <mutex>
#include <set>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"

namespace {
std::mutex g_mutex;
std::set<uint32_t> g_logged;
uint64_t g_failed = 0;
rex::memory::Memory* g_memory = nullptr;

// how full the physical heaps are: free pages x page size per heap
void LogHeaps() {
  if (!g_memory) return;
  auto* phys = g_memory->GetPhysicalHeap();
  std::string s;
  for (uint32_t ps : {4096u, 65536u, 16u << 20}) {
    auto* h = g_memory->LookupHeapByType(true, ps);
    if (!h) continue;
    char b[96];
    std::snprintf(b, sizeof b, " %uK pages: %u of %u free;", ps >> 10, h->GetUnreservedPageCount(), h->GetTotalPageCount());
    s += b;
  }
  if (phys) {
    char b[96];
    std::snprintf(b, sizeof b, " physical: %u of %u 4K pages free", phys->GetUnreservedPageCount(), phys->GetTotalPageCount());
    s += b;
  }
  REXLOG_WARN("[svr2011] model memory: heaps{}", s);
}
}  // namespace

namespace svr2011 {
void InstallModelMem(rex::memory::Memory* memory) { g_memory = memory; }
}  // namespace svr2011

REX_EXTERN(__imp__sub_82914DD0);
REX_HOOK_RAW(sub_82914DD0) {
  const uint32_t size = ctx.r3.u32, align = ctx.r4.u32, flags = ctx.r5.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_82914DD0(ctx, base);
  // test aid SVR2011_TEST_BUFFER_LOG: every model-loader vertex buffer (caller 826E7468) and the running total
  static const bool log_all = std::getenv("SVR2011_TEST_BUFFER_LOG") != nullptr;
  if (log_all && caller == 0x826E7468) {
    static uint64_t total = 0;
    total += size;
    REXLOG_INFO("[svr2011] model memory: vertex buffer {} bytes ({}), total {}", size, ctx.r3.u32 ? "ok" : "FAILED", total);
  }
  if (ctx.r3.u32 == 0 && size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_failed;
    if (g_logged.insert(caller).second)
      REXLOG_WARN("[svr2011] model memory: a {} byte buffer (align {}, flags {:x}) has no room (caller {:08X}, {} failed so far)",
                  size, align, flags, caller, g_failed);
    if (g_failed <= 3) LogHeaps();
  }
}

// The game's XMemAlloc (sub_82158C70(size, attributes): heaps by the allocator id in
// attributes byte 1, table at 0x82DDBC18): failures logged with the attributes, once each.
REX_EXTERN(__imp__sub_82158C70);
REX_HOOK_RAW(sub_82158C70) {
  const uint32_t size = ctx.r3.u32, attr = ctx.r4.u32, caller = uint32_t(ctx.lr);
  __imp__sub_82158C70(ctx, base);
  if (ctx.r3.u32 == 0 && size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    static std::set<uint64_t> seen;
    if (seen.insert(uint64_t(attr) << 32 | caller).second)
      REXLOG_WARN("[svr2011] model memory: XMemAlloc {} bytes, attributes {:08X} (allocator {}) failed (caller {:08X})", size,
                  attr, (attr >> 16) & 0xFF, caller);
  }
}

// sub_8215A380(size, min, max, align, protect) -> MmAllocatePhysicalMemoryEx: an
// allocator heap growing inside its physical address window. Failures logged.
//
// A window that's full is tried again anywhere in physical memory: the match's block heap (sub_821DB908 at the
// match start, after the entrances: 2 x 6.5 MB, the first in 19EC0000-1A540000 - exactly its size) found a page
// of its window taken - two big 2K14 ports in a mod arena (2.1.3 / 2.1.4.1 reports, every time: Eddie Guerrero
// and Undertaker Biker in "Smackdown 13") - and the game cleared the null it got (memset at lr 821DBA14), a
// crash 2.5 s after the entrances. The heap's users take its base as given (sub_821DB908 lays its blocks from
// the address returned), so another place works; no room anywhere is still 0 (logged with the heaps).
REX_EXTERN(__imp__sub_8215A380);
REX_HOOK_RAW(sub_8215A380) {
  const uint32_t size = ctx.r3.u32, lo = ctx.r4.u32, hi = ctx.r5.u32, align = ctx.r6.u32, prot = ctx.r7.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  const PPCContext in = ctx;
  // Test aid: SVR2011_TEST_BLOCKHEAP_SQUAT=1 - 64 KB of the block heap's window taken first (once), as another
  // allocation spilling into it would: the window is then too full for it.
  if (static const bool squat = std::getenv("SVR2011_TEST_BLOCKHEAP_SQUAT") != nullptr;
      squat && size == 0x680000 && lo == 0x19EC0000) {
    static bool done = false;
    if (!done) {
      done = true;
      ctx.r3.u64 = 0x10000;
      __imp__sub_8215A380(ctx, base);
      REXLOG_WARN("[svr2011] model memory: test - 64 KB of {:08X}-{:08X} taken first (at {:08X})", lo, hi, ctx.r3.u32);
      ctx = in;
    }
  }
  __imp__sub_8215A380(ctx, base);
  static const bool log_all = std::getenv("SVR2011_TEST_BUFFER_LOG") != nullptr;
  if (ctx.r3.u32 == 0 || log_all)
    REXLOG_WARN("[svr2011] model memory: physical {} bytes in {:08X}-{:08X} (align {:X}, protect {:X}) -> {:08X} (caller {:08X})",
                size, lo, hi, align, prot, ctx.r3.u32, caller);
  if (ctx.r3.u32 == 0 && size && (lo != 0 || hi < 0x1FFFFFFF)) {
    LogHeaps();
    ctx = in;
    ctx.r4.u64 = 0, ctx.r5.u64 = 0x1FFFFFFF;  // (anywhere)
    __imp__sub_8215A380(ctx, base);
    REXLOG_WARN("[svr2011] model memory: ... its window full: {}", ctx.r3.u32 ? fmt::format("placed at {:08X} instead", ctx.r3.u32)
                                                                          : std::string("no room anywhere either"));
  }
}
