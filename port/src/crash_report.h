// WWE SmackDown vs. Raw 2011 - crash reports.
//
// On abort(), std::terminate (uncaught C++ exception) or an unhandled
// structured exception, writes <dir>/crash_<time>.dmp (minidump) and
// <dir>/crash_<time>.txt (reason + symbolized stack of the crashing thread),
// and the same lines to the log, then lets the process die as it would have.
// Android / Linux (crash_report_posix.cpp): fatal signals, the text report
// and the log lines (no minidump).

#pragma once

#include <filesystem>

namespace svr2011 {

void InstallCrashReporter(const std::filesystem::path& dir);

// Called (once) when a crash is reported, before the log is flushed: lets a
// subsystem log what it knows (the native renderer logs GPU fault details).
void SetCrashHook(void (*hook)());

}  // namespace svr2011
