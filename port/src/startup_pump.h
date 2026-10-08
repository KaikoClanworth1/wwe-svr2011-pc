// Keeps the game window answering Windows while the port prepares mods at
// start (move packs, arena and superstar mods): that work runs on the UI
// thread before the app's event loop, and a window that reads no messages
// for 5 s is "Not Responding" - clicking it then lets Windows close the game
// (a slow first start on a hard drive with a cold file cache looked like a
// crash). SDL_PumpEvents reads the OS messages into SDL's queue; the event
// loop handles them once it runs.
#pragma once

#include <SDL3/SDL_events.h>

#include <chrono>
#include <thread>

namespace svr2011 {

// The UI thread (the one that runs the Install* calls), set by the app.
inline std::thread::id& StartupThread() {
  static std::thread::id id;
  return id;
}

// Cheap enough to call per file: pumps at most every 100 ms, only on the
// startup thread.
inline void KeepWindowAlive() {
  if (std::this_thread::get_id() != StartupThread()) return;
  using clock = std::chrono::steady_clock;
  static clock::time_point last;
  const auto now = clock::now();
  if (now - last < std::chrono::milliseconds(100)) return;
  last = now;
  SDL_PumpEvents();
}

}  // namespace svr2011
