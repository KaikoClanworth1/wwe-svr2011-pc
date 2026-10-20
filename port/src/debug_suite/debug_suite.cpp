// WWE SmackDown vs. Raw 2011 - the debug / test suite (see debug_suite.h).

#include "debug_suite/debug_suite.h"

#include <rex/logging.h>

namespace svr2011::dbg {

// (used, so the linker keeps it: the release guard's proof that a binary is a debug-suite one)
const char kMarker[] = "SVRDBG: the debug / test suite build (svr2011_dbg) - never part of a release";

void Install(rex::memory::Memory* memory) {
  (void)memory;
  REXLOG_WARN("{}", kMarker);
}

}  // namespace svr2011::dbg
