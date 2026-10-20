// WWE SmackDown vs. Raw 2011 - the debug / test suite (docs/DEBUG_SUITE_PLAN.md).
//
// For us only: compiled into svr2011_dbg.exe (CMake option SVR2011_DEBUG_SUITE, its own build folder
// out/build/DebugSuite*), never into a release - tools/release_guard.ps1 (package.ps1) fails a package that holds
// a debug-suite file or the kMarker bytes. Everything here is inside src/debug_suite/ or #if SVR2011_DEBUG_SUITE.

#pragma once

#if !SVR2011_DEBUG_SUITE
#error "debug_suite.h is only for the debug-suite build (SVR2011_DEBUG_SUITE)"
#endif

namespace rex::memory {
class Memory;
}

namespace svr2011::dbg {

// The marker every debug-suite build carries (the release guard looks for its first bytes, "SVRDBG:").
extern const char kMarker[];

// At start (svr2011_app.cpp): logs the marker; later steps add the cues, the state dumps and the direct boot.
void Install(rex::memory::Memory* memory);

}  // namespace svr2011::dbg
