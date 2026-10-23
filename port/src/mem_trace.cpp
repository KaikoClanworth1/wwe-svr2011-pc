// WWE SmackDown vs. Raw 2011 - test aid SVR2011_TEST_MEMTRACE=1 (mem_trace.h).
//
// Two areas cap big models and arenas: XMemAlloc's allocator 128 (sub_82158C70: the model loader's vertex / index
// buffers - a wrestler's 768 KB window, the stage's ~17 MB) and the game heaps' new (sub_8269BBA0: a wrestler's model
// files, ~1 MB). Before either gets fallback memory, every block they give is followed to the free that gives it back
// - XMemFree (sub_82158940) or the heaps' delete (sub_8269B2D0) - so a fallback block is handed back on that path.
// Logged: each allocator-128 block and its free (with callers); per heap, every 15 s, blocks given / freed by delete /
// still held; and the allocator-128 blocks still held, with their callers.

#include "mem_trace.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <rex/logging.h>

namespace svr2011 {
namespace {

struct Block {
  uint32_t size, tag, caller;  // (tag: allocator 128, or the heap id + 1000)
};
std::mutex g_mutex;
std::unordered_map<uint32_t, Block> g_live;
struct Count {
  uint64_t given = 0, by_xfree = 0, by_delete = 0, bytes = 0;
};
std::map<uint32_t, Count> g_counts;  // (by tag)
// (allocator 128: caller of the alloc << 32 | caller of the free -> count; by XMemFree / by delete)
std::map<uint64_t, uint64_t> g_by_xfree, g_by_delete;

void Summary() {
  std::lock_guard lock(g_mutex);
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> live;  // tag -> blocks, bytes
  for (const auto& [p, b] : g_live) live[b.tag].first++, live[b.tag].second += b.size;
  for (const auto& [tag, c] : g_counts) {
    const auto l = live[tag];
    REXLOG_INFO("[svr2011] memtrace: {} {}: given {} ({} bytes), freed by XMemFree {}, by delete {}, held {} ({} bytes)",
                tag >= 1000 ? "heap" : "allocator", tag >= 1000 ? tag - 1000 : tag, c.given, c.bytes, c.by_xfree,
                c.by_delete, l.first, l.second);
  }
  for (const auto& [k, n] : g_by_xfree)
    REXLOG_INFO("[svr2011] memtrace: allocator 128 alloc from {:08X} freed by XMemFree from {:08X}: {}", uint32_t(k >> 32),
                uint32_t(k), n);
  for (const auto& [k, n] : g_by_delete)
    REXLOG_INFO("[svr2011] memtrace: allocator 128 alloc from {:08X} freed by delete from {:08X}: {}", uint32_t(k >> 32),
                uint32_t(k), n);
  std::map<uint32_t, uint64_t> held;  // (allocator 128 still held: by caller)
  for (const auto& [p, b] : g_live)
    if (b.tag == 128) held[b.caller]++;
  for (const auto& [caller, n] : held) REXLOG_INFO("[svr2011] memtrace: allocator 128 held from {:08X}: {}", caller, n);
}

void Start() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        Summary();
      }
    }).detach();
  });
}

}  // namespace

bool MemTraceOn() {
  static const bool on = std::getenv("SVR2011_TEST_MEMTRACE") != nullptr;
  return on;
}

void MemTraceXAlloc(uint32_t ptr, uint32_t size, uint32_t attr, uint32_t caller) {
  if (!ptr) return;
  Start();
  std::lock_guard lock(g_mutex);
  g_live[ptr] = {size, 128, caller};
  auto& c = g_counts[128];
  c.given++, c.bytes += size;
}

void MemTraceXFree(uint32_t ptr, uint32_t attr, uint32_t caller) {
  std::lock_guard lock(g_mutex);
  const auto it = g_live.find(ptr);
  if (it == g_live.end()) return;
  g_counts[it->second.tag].by_xfree++;
  if (it->second.tag == 128) g_by_xfree[uint64_t(it->second.caller) << 32 | caller]++;
  g_live.erase(it);
}

void MemTraceNew(uint32_t ptr, uint32_t size, uint32_t heap, uint32_t caller) {
  if (!ptr) return;
  Start();
  std::lock_guard lock(g_mutex);
  g_live[ptr] = {size, heap + 1000, caller};
  auto& c = g_counts[heap + 1000];
  c.given++, c.bytes += size;
}

void MemTraceDelete(uint32_t ptr, uint32_t caller) {
  std::lock_guard lock(g_mutex);
  const auto it = g_live.find(ptr);
  if (it == g_live.end()) return;
  g_counts[it->second.tag].by_delete++;
  if (it->second.tag == 128) g_by_delete[uint64_t(it->second.caller) << 32 | caller]++;
  g_live.erase(it);
}

}  // namespace svr2011
