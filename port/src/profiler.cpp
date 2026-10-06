// Test aid (Android): a sampling profiler inside the game, for phones where
// simpleperf can't attach (user builds). SVR2011_PROFILE="<delay s>,<seconds>,<file>"
// - after the delay, every 5 ms each game thread (guest threads, GPU, SDL) is
// interrupted and where it is noted: its program counter and frame-pointer
// chain, whether it runs or waits. Written as "<thread>\t<module+offset> ..."
// lines; tools/profile_report.py names the functions (llvm-symbolizer).
#if defined(__ANDROID__) && defined(__aarch64__)

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kDepth = 24;
struct Sample {
  int tid;
  int depth;
  uintptr_t pc[kDepth];
};

std::vector<Sample> g_samples;
std::atomic<size_t> g_next{0};
std::atomic<int> g_done{0};

// The interrupted thread's place: pc, lr, then the frame-pointer chain
// (frames lie above sp, 16-byte aligned).
void OnSignal(int, siginfo_t*, void* context) {
  const size_t i = g_next.fetch_add(1, std::memory_order_relaxed);
  if (i < g_samples.size()) {
    Sample& s = g_samples[i];
    const mcontext_t& m = static_cast<ucontext_t*>(context)->uc_mcontext;
    s.tid = int(syscall(SYS_gettid));
    s.pc[0] = m.pc;
    s.pc[1] = m.regs[30];
    int n = 2;
    uintptr_t fp = m.regs[29];
    const uintptr_t sp = m.sp;
    while (n < kDepth && fp >= sp && fp < sp + (8u << 20) && (fp & 15) == 0) {
      const uintptr_t* frame = reinterpret_cast<const uintptr_t*>(fp);
      const uintptr_t next = frame[0], ret = frame[1];
      if (!ret) break;
      s.pc[n++] = ret;
      if (next <= fp) break;
      fp = next;
    }
    s.depth = n;
  }
  g_done.fetch_add(1, std::memory_order_release);
}

std::string Comm(int tid) {
  char path[64], name[64] = {};
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
  if (FILE* f = std::fopen(path, "r")) {
    if (std::fgets(name, sizeof(name), f)) name[std::strcspn(name, "\n")] = 0;
    std::fclose(f);
  }
  return name;
}

void Run(double delay, double seconds, std::string out) {
  std::this_thread::sleep_for(std::chrono::duration<double>(delay));
  // The threads worth sampling (the game's, the GPU's, SDL's).
  std::unordered_map<int, std::string> names;
  std::vector<int> tids;
  if (DIR* d = opendir("/proc/self/task")) {
    while (dirent* e = readdir(d)) {
      const int tid = std::atoi(e->d_name);
      if (tid <= 0) continue;
      const std::string n = Comm(tid);
      if (n.rfind("XThread", 0) == 0 || n.rfind("GPU", 0) == 0 || n.rfind("SDL", 0) == 0 ||
          n.rfind("Audio", 0) == 0 || n.rfind("native", 0) == 0) {
        names[tid] = n;
        tids.push_back(tid);
      }
    }
    closedir(d);
  }
  const size_t rounds = size_t(seconds * 200);
  g_samples.resize(rounds * tids.size() + 16);
  struct sigaction sa = {};
  sa.sa_sigaction = OnSignal;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  struct sigaction old = {};
  sigaction(SIGURG, &sa, &old);
  const pid_t pid = getpid();
  for (size_t r = 0; r < rounds; ++r) {
    const int before = g_done.load();
    int sent = 0;
    for (int tid : tids) sent += syscall(SYS_tgkill, pid, tid, SIGURG) == 0;
    // (each thread notes itself before the next round)
    for (int spin = 0; spin < 2000 && g_done.load(std::memory_order_acquire) - before < sent; ++spin)
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  sigaction(SIGURG, &old, nullptr);
  const size_t n = std::min(g_next.load(), g_samples.size());
  FILE* f = std::fopen(out.c_str(), "w");
  if (!f) return;
  std::unordered_map<uintptr_t, std::string> where;
  auto name = [&](uintptr_t a) -> const std::string& {
    auto it = where.find(a);
    if (it != where.end()) return it->second;
    Dl_info info = {};
    char buf[160];
    if (dladdr(reinterpret_cast<void*>(a), &info) && info.dli_fname) {
      const char* m = std::strrchr(info.dli_fname, '/');
      std::snprintf(buf, sizeof(buf), "%s+0x%lx", m ? m + 1 : info.dli_fname,
                    static_cast<unsigned long>(a - reinterpret_cast<uintptr_t>(info.dli_fbase)));
    } else {
      std::snprintf(buf, sizeof(buf), "0x%lx", static_cast<unsigned long>(a));
    }
    return where.emplace(a, buf).first->second;
  };
  std::fprintf(f, "# %zu samples, %zu threads, %.1f s\n", n, tids.size(), seconds);
  for (size_t i = 0; i < n; ++i) {
    const Sample& s = g_samples[i];
    std::fprintf(f, "%s:%d", names.count(s.tid) ? names[s.tid].c_str() : "?", s.tid);
    for (int k = 0; k < s.depth; ++k) std::fprintf(f, "\t%s", name(s.pc[k]).c_str());
    std::fputc('\n', f);
  }
  std::fclose(f);
}

struct Start {
  Start() {
    std::thread([] {
      // (the activity sets the variables just after the library loads)
      std::this_thread::sleep_for(std::chrono::seconds(2));
      const char* v = std::getenv("SVR2011_PROFILE");
      if (!v) return;
      double delay = 0, seconds = 0;
      char out[512] = {};
      if (std::sscanf(v, "%lf,%lf,%511s", &delay, &seconds, out) != 3) return;
      Run(delay, seconds, out);
    }).detach();
  }
} g_start;

}  // namespace

#endif
