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

#include <rex/system/kernel_state.h>
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
    by_label[label] += 100.0 * double(delta) / wall_ns;
  }
  g_last_cpu = std::move(next);
  if (first) return {};
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
