// WWE SmackDown vs. Raw 2011 - per-thread CPU use, see thread_cpu.h.
//
// A thread of its own, every 5 s: every thread's CPU time (Windows:
// GetThreadTimes on handles kept open - the process's threads are listed with
// a Toolhelp snapshot only once a minute, as it walks every thread on the
// system and takes ~150 ms; Android / Linux: /proc/self/task/<tid>/stat) and
// the difference over the 5 s. Never on a game thread: done on the frame
// thread, the snapshot was a 150 ms hitch every 5 s.
// Labels: the roles noted by hooks (the game's world update = logic, its
// D3D Clear = render); host threads by name (GPU..., p2p..., audio); guest
// threads otherwise by their start address ("g8216E098"), the same in every
// build of this game - the address tells which game thread it is.
#include "thread_cpu.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#endif

#include <fmt/format.h>

#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/logging.h>
#include <rex/thread.h>

namespace svr2011 {

namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_mutex;
std::unordered_map<uint32_t, std::string> g_roles;   // host thread id -> role
std::unordered_map<uint32_t, uint64_t> g_last_cpu;   // host thread id -> CPU time (ns)
Clock::time_point g_last_at{};

struct ThreadTime {
  uint32_t tid = 0;
  uint64_t cpu_ns = 0;
  std::string name;
};

#if defined(_WIN32)
using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

struct Cached {
  HANDLE handle = nullptr;
  std::string name;
};
std::unordered_map<uint32_t, Cached> g_handles;  // (the worker's)
Clock::time_point g_listed{};

// The process's threads, again once a minute (handles kept open between).
void ListThreads() {
  static const auto describe = reinterpret_cast<GetThreadDescriptionFn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));  // (Windows 10 1607+)
  const DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  std::unordered_map<uint32_t, Cached> now;
  THREADENTRY32 te = {};
  te.dwSize = sizeof(te);
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    if (auto it = g_handles.find(te.th32ThreadID); it != g_handles.end()) {
      now[te.th32ThreadID] = it->second;
      g_handles.erase(it);
      continue;
    }
    Cached c;
    c.handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    if (!c.handle) continue;
    PWSTR d = nullptr;
    if (describe && SUCCEEDED(describe(c.handle, &d)) && d) {
      for (const wchar_t* p = d; *p; ++p) c.name.push_back(*p < 128 ? char(*p) : '?');
      LocalFree(d);
    }
    now[te.th32ThreadID] = std::move(c);
  }
  CloseHandle(snap);
  for (auto& [tid, c] : g_handles) CloseHandle(c.handle);  // (threads gone)
  g_handles = std::move(now);
}

std::vector<ThreadTime> ReadThreads() {
  if (g_listed == Clock::time_point{} || Clock::now() - g_listed > std::chrono::seconds(60)) {
    ListThreads();
    g_listed = Clock::now();
  }
  std::vector<ThreadTime> out;
  for (const auto& [tid, c] : g_handles) {
    FILETIME cr, e, k, u;
    if (!GetThreadTimes(c.handle, &cr, &e, &k, &u)) continue;
    ThreadTime t;
    t.tid = tid;
    t.cpu_ns = ((uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
                (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime)) * 100;
    t.name = c.name;
    out.push_back(std::move(t));
  }
  return out;
}
#else
std::vector<ThreadTime> ReadThreads() {
  std::vector<ThreadTime> out;
  static const long ticks = sysconf(_SC_CLK_TCK);
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return out;
  while (dirent* e = readdir(dir)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    ThreadTime t;
    t.tid = uint32_t(std::atoi(e->d_name));
    char path[64], buf[512];
    std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e->d_name);
    if (FILE* f = std::fopen(path, "r")) {
      const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
      std::fclose(f);
      buf[n] = 0;
      // (fields after the ")" closing the name: 14 utime, 15 stime)
      if (const char* close = std::strrchr(buf, ')')) {
        unsigned long long utime = 0, stime = 0;
        if (std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) == 2 &&
            ticks > 0)
          t.cpu_ns = (utime + stime) * 1000000000ull / uint64_t(ticks);
      }
    }
    std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
    if (FILE* f = std::fopen(path, "r")) {
      if (std::fgets(buf, sizeof(buf), f)) {
        t.name = buf;
        while (!t.name.empty() && (t.name.back() == '\n' || t.name.back() == '\r')) t.name.pop_back();
      }
      std::fclose(f);
    }
    out.push_back(std::move(t));
  }
  closedir(dir);
  return out;
}
#endif

bool Has(const std::string& s, const char* what) {
  std::string a = s, b = what;
  for (char& c : a) c = char(std::tolower(static_cast<unsigned char>(c)));
  for (char& c : b) c = char(std::tolower(static_cast<unsigned char>(c)));
  return a.find(b) != std::string::npos;
}

}  // namespace

void NoteThreadRole(const char* role) {
  thread_local const char* noted = nullptr;
  if (noted == role) return;
  noted = role;
  std::lock_guard lock(g_mutex);
  g_roles[rex::thread::current_thread_system_id()] = role;
}

namespace {

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// A hang note: where the guest thread `host_tid` is (its last call boundary:
// lr = the last call made, ctr, r3, then the stack's saved return addresses).
void LogSpinningThread(uint32_t host_tid, double pct, double logic) {
  auto* kernel = REX_KERNEL_STATE();
  auto* memory = kernel ? kernel->memory() : nullptr;
  if (!memory) return;
  uint8_t* base = memory->virtual_membase();
  const auto readable = [&](uint32_t a) {
    auto* heap = memory->LookupHeap(a);
    return heap && heap->QueryRangeAccess(a, a + 3) != rex::memory::PageAccess::kNoAccess;
  };
  for (auto& t : kernel->object_table()->GetObjectsByType<rex::system::XThread>()) {
    if (!t || !t->thread() || t->thread()->system_id() != host_tid) continue;
    if (!t->is_guest_thread() || !t->thread_state() || !t->thread_state()->context() || !t->pcr_ptr()) return;
    const PPCContext* c = t->thread_state()->context();
    const uint32_t hi = Rd32(base + t->pcr_ptr() + 0x70), lo = Rd32(base + t->pcr_ptr() + 0x74);
    const auto in_stack = [&](uint32_t a) { return lo < hi && a >= lo && a + 4 <= hi && readable(a); };
    std::string chain;
    uint32_t sp = c->r1.u32;
    for (int i = 0; i < 12 && in_stack(sp); ++i) {
      const uint32_t prev = Rd32(base + sp);
      if (prev <= sp || !in_stack(prev) || !in_stack(prev - 8)) break;
      chain += fmt::format(" {:08X}", Rd32(base + prev - 8));
      sp = prev;
    }
    REXLOG_WARN("hang? the game's logic thread at {:.1f}% for 20 s while thread {:08X} (g{:08X}) runs at {:.0f}%: lr {:08X} "
                "ctr {:08X} r3 {:08X}, stack{}",
                logic, t->thread_id(), t->creation_params()->start_address, pct, uint32_t(c->lr), c->ctr.u32,
                c->r3.u32, chain.empty() ? " -" : chain);
    return;
  }
}

// Logic under 5% while one guest thread is above 90%, 4 samples (20 s) in a
// row: noted once (again only after it ends). A start-up hang (2.1.1 report:
// a job thread at 97% after the created superstars loaded, no main menu) left
// nothing in the log saying where.
void CheckHang(bool logic_seen, double logic, uint32_t top_tid, double top_pct) {
  static int samples = 0;
  static bool noted = false;
  static const double busy = std::getenv("SVR2011_TEST_HANG_PCT") ? std::atof(std::getenv("SVR2011_TEST_HANG_PCT")) : 90.0;  // (test: lower)
  if (!logic_seen || logic >= 5.0 || top_pct <= busy) {
    samples = 0, noted = false;
    return;
  }
  if (++samples >= 4 && !noted) {
    noted = true;
    LogSpinningThread(top_tid, top_pct, logic);
  }
}

std::string Line() {
  const auto now = Clock::now();
  const std::vector<ThreadTime> threads = ReadThreads();
  // guest threads: host id -> start address (again once a minute)
  static std::unordered_map<uint32_t, uint32_t> guest;
  static Clock::time_point guest_at{};
  if (guest_at == Clock::time_point{} || now - guest_at > std::chrono::seconds(60)) {
    guest_at = now;
    guest.clear();
    if (auto* kernel = REX_KERNEL_STATE()) {
      for (auto& t : kernel->object_table()->GetObjectsByType<rex::system::XThread>()) {
        if (auto* host = t->thread()) guest[host->system_id()] = t->creation_params()->start_address;
      }
    }
  }
  std::lock_guard lock(g_mutex);
  const bool first = g_last_at == Clock::time_point{};
  const double wall_ns = first ? 0 : double(std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_last_at).count());
  g_last_at = now;
  std::map<std::string, double> by_label;
  std::unordered_map<uint32_t, uint64_t> next;
  uint32_t top_tid = 0;  // (the busiest guest thread: the hang note)
  double top_pct = 0;
  for (const ThreadTime& t : threads) {
    next[t.tid] = t.cpu_ns;
    if (first || wall_ns <= 0) continue;
    const auto was = g_last_cpu.find(t.tid);
    const uint64_t delta = t.cpu_ns - (was != g_last_cpu.end() ? std::min(was->second, t.cpu_ns) : 0);
    std::string label;
    if (auto r = g_roles.find(t.tid); r != g_roles.end()) label = r->second;
    else if (t.name.rfind("GPU", 0) == 0) label = "gpu";
    else if (Has(t.name, "p2p")) label = "p2p";
    else if (Has(t.name, "audio") || Has(t.name, "xma") || Has(t.name, "music")) label = "audio";
    else if (auto g = guest.find(t.tid); g != guest.end()) label = fmt::format("g{:08X}", g->second);
    else if (Has(t.name, "timer")) label = "timers";
    else label = "other";
    const double pct = 100.0 * double(delta) / wall_ns;
    by_label[label] += pct;
    if (label[0] == 'g' && label != "gpu" && pct > top_pct) top_tid = t.tid, top_pct = pct;
  }
  g_last_cpu = std::move(next);
  if (first) return {};
  bool logic_seen = false;
  for (const auto& [tid, role] : g_roles) logic_seen |= std::strcmp(role.c_str(), "logic") == 0;
  CheckHang(logic_seen, by_label.count("logic") ? by_label["logic"] : 0.0, top_tid, top_pct);
  std::vector<std::pair<double, std::string>> sorted;
  double total = 0;
  for (const auto& [label, pct] : by_label) sorted.emplace_back(pct, label), total += pct;
  std::sort(sorted.rbegin(), sorted.rend());
  std::string line = fmt::format("threads (% of a core): total {:.0f}", total);
  for (const auto& [pct, label] : sorted) {
    if (pct < 1.0) break;
    line += fmt::format(", {} {:.0f}", label, pct);
  }
  return line;
}

}  // namespace

void StartThreadCpuLog() {
  static std::once_flag once;
  if (std::getenv("SVR2011_TEST_NO_THREAD_CPU")) return;  // (test: without it)
  std::call_once(once, [] {
    std::thread([] {
      rex::thread::set_current_thread_name("thread cpu log");
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (const std::string line = Line(); !line.empty()) REXLOG_INFO("{}", line);
      }
    }).detach();
  });
}

}  // namespace svr2011
