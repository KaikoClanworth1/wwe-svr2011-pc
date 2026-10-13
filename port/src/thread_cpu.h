// WWE SmackDown vs. Raw 2011 - per-thread CPU use for the log (thread_cpu.cpp):
// where a weak PC's time goes, in every player's report.
#pragma once

#include <string>

namespace svr2011 {

// This thread's role in the line ("logic", "render"); cheap after the first call.
void NoteThreadRole(const char* role);

// From now on, every 5 s on a thread of its own, a log line with every
// thread's CPU over those 5 s as % of one core, grouped: the noted roles,
// GPU / audio / p2p host threads by name, the busiest other guest threads by
// start address.
void StartThreadCpuLog();

}  // namespace svr2011
