// WWE SmackDown vs. Raw 2011 - guest memory faults no handler took.
//
// The SDK's log line for such a fault says only the guest address; on Android
// its signal handler then returns and the same instruction faults again,
// forever - a silent freeze (24 Oct: an Extreme Rules match on a phone, tens
// of thousands of "write of guest 0x00000000" lines and nothing else). Here,
// after the SDK's handler: the place, once - the host pc as library + offset
// (llvm-symbolizer on that release's unstripped libmain.so names the guest
// function) and the guest's lr (the last call that function made) - and after
// kGiveUp faults in a row at one place the crash reporter takes the signal
// again, so the next one is a crash report and the game closes.

#include "guest_faults.h"

#if !defined(_WIN32)

#include <dlfcn.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/system/thread_state.h>

#include "crash_report.h"

namespace svr2011 {
namespace {

constexpr uint64_t kGiveUp = 2000;

// The host pc as "libmain.so+0x1234" (or the symbol when the library has one).
void Place(uint64_t pc, char* out, size_t size) {
  Dl_info dl = {};
  if (dladdr(reinterpret_cast<void*>(pc), &dl) && dl.dli_fname) {
    const char* lib = std::strrchr(dl.dli_fname, '/');
    lib = lib ? lib + 1 : dl.dli_fname;
    if (dl.dli_sname)
      std::snprintf(out, size, "%s!%s+0x%zx", lib, dl.dli_sname, size_t(pc - uintptr_t(dl.dli_saddr)));
    else
      std::snprintf(out, size, "%s+0x%zx", lib, size_t(pc - uintptr_t(dl.dli_fbase)));
  } else {
    std::snprintf(out, size, "0x%zx", size_t(pc));
  }
}

bool OnFault(rex::arch::Exception* ex, void*) {
  if (ex->code() != rex::arch::Exception::Code::kAccessViolation || !ex->thread_context()) return false;
  const uint64_t pc = ex->pc();
  static thread_local uint64_t last_pc = 0, repeats = 0;
  if (pc != last_pc) last_pc = pc, repeats = 0;
  ++repeats;
  if (repeats == 1 || repeats == kGiveUp) {
    char place[256];
    Place(pc, place, sizeof(place));
    uint32_t lr = 0;
    if (auto* state = rex::runtime::ThreadState::Get(); state && state->context()) lr = uint32_t(state->context()->lr);
    REXLOG_ERROR("guest fault: {} of host 0x{:X} at {}, guest lr {:08X} (thread {}){}",
                 ex->access_violation_operation() == rex::arch::Exception::AccessViolationOperation::kWrite ? "write"
                                                                                                            : "read",
                 ex->fault_address(), place, lr, gettid(),
                 repeats == kGiveUp ? " - the same fault again and again: the game is stopped (crash report)" : "");
    if (repeats == kGiveUp) {
      rex::FlushLogging();
      ReclaimCrashSignals();
    }
  }
  return false;
}

}  // namespace

void InstallGuestFaultLog() { rex::arch::ExceptionHandler::Install(OnFault, nullptr); }

}  // namespace svr2011

#else

namespace svr2011 {
void InstallGuestFaultLog() {}
}  // namespace svr2011

#endif
