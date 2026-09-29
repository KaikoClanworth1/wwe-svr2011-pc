// svr2011 - ReXGlue Recompiled Project

#include "generated/default/svr2011_init.h"

#include "svr2011_app.h"

REX_DEFINE_APP(svr2011, Svr2011App::Create)

// Laptops with two GPUs: ask NVIDIA Optimus and AMD switchable graphics for
// the fast one. The drivers only read these from the program itself (the
// SDK's copies are in a DLL).
#if defined(_WIN32)
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
