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

// A load watch, for reports of a loading screen that never ends (2.1.5: CREATE A MOVE-SET stuck on its
// loading screen, steady 60 fps, nothing more in the log): a mode chosen from the menus ends the practice
// ring behind them ("match end: step 0" with no match placed since that menu select) and loads its own
// scene; when neither its menu table is read nor a match placed within 25 s, every guest thread's place
// (last call, ctr, r3, the stack's return addresses) is logged once - where the load waits. CREATE A
// MOVE-SET only (SVR2011_TEST_LOAD_WATCH=all: every mode, =stall: the loads' ends ignored - the note's proof;
// SVR2011_TEST_LOAD_WATCH_LOG: a load in time logged too).
void LoadWatchMenuSelect(const char* label);  // (menu_hooks: a menu row chosen - also ends a watch)
void LoadWatchStart();                         // (replays: "match end: step 0")
void LoadWatchDone(const char* what);          // (match_types: the menu table read / a match placed)

}  // namespace svr2011
