// WWE SmackDown vs. Raw 2011 - per-thread CPU use for the log (thread_cpu.cpp):
// where a weak PC's time goes, in every player's report.
#pragma once

#include <string>

namespace svr2011 {

// This thread's role in the line ("logic", "render"); cheap after the first call.
void NoteThreadRole(const char* role);

// Every thread's CPU since the last call, as % of one core, grouped: the
// noted roles, GPU / audio / p2p host threads by name, the busiest other guest
// threads by start address. "" the first time.
std::string ThreadCpuLine();

}  // namespace svr2011
