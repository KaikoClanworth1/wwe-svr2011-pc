// A stand-in for an overlay DLL (Discord, Steam, RTSS, NVIDIA...) built against the current VC++ runtime:
// its DllMain uses the C++ runtime (a std::mutex - 14.40+ code needs a 14.40+ msvcp140 - and a std::string).
// Loaded into the game with SVR2011_TEST_LOAD_DLL=<this dll> (svr2011_app.cpp) to check that the game folder
// gives it a runtime it can start with (2.1.0's 14.29 copies next to the game didn't: 0xc0000142).
//   clang++ -shared -O2 -fms-runtime-lib=dll overlay_test_dll.cpp -o overlay_test.dll
#include <windows.h>

#include <mutex>
#include <string>

static std::mutex g_mutex;
static std::string g_text;

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_text.assign(200, 'x');
    OutputDebugStringA("overlay_test.dll: started\n");
  }
  return TRUE;
}
