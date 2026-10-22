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

#include <chrono>
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

#include "caw_limits.h"
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

// -- The file loader's cache ---------------------------------------------------
//
// The game's file loader thread (g821593A8, sub_826A1D38) reads each file into a buffer from its cache
// (*0x82ED5FE8: sub_826A1C70(size, heap) -> sub_826A1C08 -> sub_826BA928(cache, size, 2048); other loader steps
// call sub_826BA928 too). When the cache's heap (cache+76: heap 16, 20 MB) has no room, sub_826BA928 frees the
// oldest idle file (state 11, sub_826BA590) and tries again - forever: with nothing idle left it spins at 100%
// and the game waits on the load screen (2.1.4.1 report 21 Oct: "hangs after the entrances, every match"; hang
// note lr 826BA5BC / 826B9FE0, stack 826BA9A8 826A1C50 826A1CE0 826A1F1C). After 3 s with nothing idle the
// file and what holds the cache are logged (each entry: name +50, size +44 x 256, state +100, buffer +96; the
// list from cache+4, nodes {next, prev, entry}) and the buffer comes from the fallback memory instead
// (caw_limits.h FallbackBlock: the emulator's physical system heap; the cache frees its buffers with
// sub_8269B2D0, whose hook hands such a block back), so the match loads instead of hanging.
// Test aids: SVR2011_TEST_CACHE_LOG=1 - every request of 1 MB or more; SVR2011_TEST_CACHE_FULL=<bytes> - the
// cache finds no room for a request of at least that size (the hang, on purpose).
namespace {
constexpr uint32_t kLoaderCache = 0x82ED5FE8;

uint32_t Rd32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

std::string GuestName(const uint8_t* base, uint32_t a) {
  std::string s;
  for (uint32_t i = 0; a && i < 72; ++i) {
    const char c = char(base[a + i]);
    if (!c) break;
    s += (c >= 32 && c < 127) ? c : '?';
  }
  return s;
}

struct CacheAlloc {
  bool active = false, stuck = false, given = false;
  uint32_t size = 0, tries = 0;
  std::chrono::steady_clock::time_point t0;
};
thread_local CacheAlloc t_alloc;
thread_local uint32_t t_request = 0;  // (the loader's request in sub_826A1C70: its file name at +50)

std::string RequestName(const uint8_t* base) {
  const std::string s = GuestName(base, t_request ? t_request + 50 : 0);
  return s.empty() ? std::string("(a loader step without a file name)") : "'" + s + "'";
}

void LogLoaderCache(const uint8_t* base, uint32_t cache, const CacheAlloc& r, double waited) {
  const uint32_t heap = Rd32(base, cache + 76);
  const uint32_t table = Rd32(base, 0x82EC5F48);
  uint32_t lo = 0, hi = 0;
  if (table && heap < 54) lo = Rd32(base, table + heap * 36), hi = Rd32(base, table + heap * 36 + 4);
  REXLOG_WARN("[svr2011] file cache: the loader has waited {:.0f} s ({} tries) for {} bytes for {} - cache heap {} "
              "{:08X}-{:08X} ({:.1f} MB) has no room and nothing idle to free",
              waited, r.tries, r.size, RequestName(base), heap, lo, hi, (hi - lo) / 1048576.0);
  uint64_t held = 0;
  uint32_t n = 0, shown = 0, node = Rd32(base, cache + 4);
  for (; node && n < 400; ++n) {
    const uint32_t e = Rd32(base, node + 8);
    if (!e) break;
    const uint32_t size = Rd32(base, e + 44) << 8, state = Rd32(base, e + 100), buf = Rd32(base, e + 96);
    if (buf) held += size;
    if ((state || buf) && shown++ < 40)  // (the pool's unused entries left out)
      REXLOG_WARN("[svr2011] file cache:   '{}' {} bytes, state {}{}", GuestName(base, e + 50), size, state,
                  buf ? fmt::format(", buffer {:08X}", buf) : std::string());
    const uint32_t next = Rd32(base, node);
    if (next == node) break;
    node = next;
  }
  REXLOG_WARN("[svr2011] file cache: {} entries ({} in use), {:.1f} MB in their buffers", n, shown, held / 1048576.0);
  LogHeaps();
}
}  // namespace

REX_EXTERN(__imp__sub_826A1C70);
REX_HOOK_RAW(sub_826A1C70) {
  const bool loader = uint32_t(ctx.lr) == 0x826A1F1C;  // (sub_826A1D38's file read: r31 = the request)
  if (loader) t_request = ctx.r31.u32;
  __imp__sub_826A1C70(ctx, base);
  if (loader) t_request = 0;
}

// (sub_826A1D38's second buffer for a file, sub_826BAB40(cache, request): its unpacked copy)
REX_EXTERN(__imp__sub_826BAB40);
REX_HOOK_RAW(sub_826BAB40) {
  const bool loader = uint32_t(ctx.lr) == 0x826A2268;
  if (loader) t_request = ctx.r4.u32;
  __imp__sub_826BAB40(ctx, base);
  if (loader) t_request = 0;
}

REX_EXTERN(__imp__sub_826BA928);
REX_HOOK_RAW(sub_826BA928) {
  const bool loader = ctx.r3.u32 == Rd32(base, kLoaderCache);
  if (loader) t_alloc = CacheAlloc{true, false, false, ctx.r4.u32, 0, std::chrono::steady_clock::now()};
  const uint32_t size = ctx.r4.u32;
  const std::string name = loader ? RequestName(base) : std::string();
  __imp__sub_826BA928(ctx, base);
  if (!loader) return;
  static const bool log_all = std::getenv("SVR2011_TEST_CACHE_LOG") != nullptr;
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_alloc.t0).count();
  if (t_alloc.stuck || s >= 1.0 || (log_all && size >= (1u << 20)))
    REXLOG_INFO("[svr2011] file cache: {} {} bytes -> {:08X} after {:.1f} s ({} tries)", name, size, ctx.r3.u32, s,
                t_alloc.tries);
  t_alloc.active = false;
}

// (inside sub_826BA928's retry loop: one call per failed try; r3 after it = the idle files it could still free)
REX_EXTERN(__imp__sub_826B9FB8);
REX_HOOK_RAW(sub_826B9FB8) {
  const uint32_t cache = ctx.r3.u32;
  __imp__sub_826B9FB8(ctx, base);
  CacheAlloc& r = t_alloc;
  if (!r.active) return;
  ++r.tries;
  if (r.stuck || ctx.r3.u32 != 0) return;
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - r.t0).count();
  if (s < 3.0) return;
  r.stuck = true;
  LogLoaderCache(base, cache, r, s);
}

namespace svr2011 {
// (sub_8269BBA0's hook, superstar_mods.cpp: the cache's allocation from inside sub_826BA928's retry loop)
bool LoaderCacheForcedFull(uint32_t size) {
  static const uint32_t full = [] {
    const char* e = std::getenv("SVR2011_TEST_CACHE_FULL");
    return e ? uint32_t(std::strtoul(e, nullptr, 0)) : 0u;
  }();
  return full && size >= full;
}

uint32_t LoaderCacheFallback(uint32_t size) {
  CacheAlloc& r = t_alloc;
  if (!r.active || !r.stuck) return 0;
  const uint32_t p = FallbackBlock(size);
  if (!r.given) {
    r.given = true;
    REXLOG_WARN("[svr2011] file cache: {} bytes {}", size,
                p ? fmt::format("from the fallback memory instead, at {:08X}", p) : std::string("- no fallback memory either"));
  }
  return p;
}
}  // namespace svr2011
