// Profile-guided optimisation, step 1 (a build with -DSVR_NX_PGO=generate only): the counts of every branch the
// game and the port ran, written to the card as /SvR2011/pgo/svr2011_<session>.stream. On the PC
// tools/pgo_merge.sh turns the sessions into the profile the next build reads (-DSVR_NX_PGO=use).
// The whole session is written again every 30 seconds (the counts only grow), into a .tmp that then replaces the
// last one: the game is closed from HOME without exiting, and a write cut short must leave the one before it.
// The stream is the one GCC's manual describes for systems without libgcov's own files ("Profiling and Test
// Coverage in Freestanding Environments"): per object, its file name then its counts.
#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

extern "C" {
#include <gcov.h>
// One pointer per counted object (-fprofile-info-section=gcov_info); the linker brackets the section.
extern const struct gcov_info* const __start_gcov_info[];
extern const struct gcov_info* const __stop_gcov_info[];
Result __real_threadStart(Thread* t);
}

namespace {

constexpr u64 kPeriodNs = 30ull * 1000000000ull;
constexpr int kMaxBlocks = 64;

struct Out {
  FILE* file;
  bool ok;
  void* blocks[kMaxBlocks];  // libgcov's scratch memory for this write
  int nblocks;
};

void Dump(const void* data, unsigned length, void* arg) {
  Out* out = static_cast<Out*>(arg);
  if (out->ok && length && fwrite(data, 1, length, out->file) != length) out->ok = false;
}

void Name(const char* name, void* arg) { __gcov_filename_to_gcfn(name, Dump, arg); }

void* Allocate(unsigned length, void* arg) {
  Out* out = static_cast<Out*>(arg);
  void* p = malloc(length);
  if (p && out->nblocks < kMaxBlocks) out->blocks[out->nblocks++] = p;
  return p;
}

char g_path[64], g_tmp[72];

bool WriteAll() {
  static char buffer[1 << 20];  // (newlib writes a small block at a time otherwise)
  Out out = {};
  out.file = fopen(g_tmp, "wb");
  if (!out.file) return false;
  setvbuf(out.file, buffer, _IOFBF, sizeof(buffer));
  out.ok = true;
  for (const struct gcov_info* const* info = __start_gcov_info; info < __stop_gcov_info; ++info) {
    if (*info) __gcov_info_to_gcda(*info, Name, Dump, Allocate, &out);
  }
  for (int i = 0; i < out.nblocks; ++i) free(out.blocks[i]);
  if (fclose(out.file) != 0) out.ok = false;
  if (!out.ok) {
    remove(g_tmp);
    return false;
  }
  remove(g_path);
  return rename(g_tmp, g_path) == 0;
}

void Run(void*) {
  int writes = 0;
  for (;;) {
    svcSleepThread(kPeriodNs);
    const u64 start = armGetSystemTick();
    const bool ok = WriteAll();
    const u64 ms = armTicksToNs(armGetSystemTick() - start) / 1000000;
    if (FILE* log = fopen("/SvR2011/logs/nx_pgo.txt", writes ? "a" : "w")) {
      fprintf(log, "write %d: %s, %llu ms, %s\n", ++writes, ok ? "ok" : "FAILED", (unsigned long long)ms, g_path);
      fclose(log);
    }
  }
}

}  // namespace

extern "C" void NxPgoStart(void) {
  mkdir("/SvR2011/pgo", 0777);
  int session = 1;
  for (; session < 1000; ++session) {
    snprintf(g_path, sizeof(g_path), "/SvR2011/pgo/svr2011_%03d.stream", session);
    struct stat st;
    if (stat(g_path, &st) != 0) break;
  }
  snprintf(g_tmp, sizeof(g_tmp), "%s.tmp", g_path);
  static Thread thread;
  // (lowest priority, any core: it must never take time from the game)
  if (R_SUCCEEDED(threadCreate(&thread, Run, nullptr, nullptr, 0x10000, 0x3F, -2))) __real_threadStart(&thread);
}
