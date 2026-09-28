// WWE SmackDown vs. Raw 2011 - D3D library call census (SVR2011_D3D_TRACE).
//
// Every function of the game's statically linked D3D library is hooked
// (d3d_trace_hooks.cpp, generated) to count calls. With
// SVR2011_D3D_TRACE_DIR=<dir> set:
//   <dir>/calls.csv    every 5 s: calls in the last window, per presented
//                      frame, total, and one caller address per function
//   <dir>/xsc/*.xsc    every XDK shader container passed to a D3D function
//                      (argument registers r3-r6 are checked), and
//   <dir>/xsc.csv      which function / register received it
// - the raw material for naming the D3D API a native renderer replaces, and
// for converting the game's shaders offline (XenosRecomp).

#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace svr2011::d3d_trace {

extern const uint32_t kFunctionCount;
extern const uint32_t kFunctions[];

void OnCall(uint32_t index, const PPCContext& ctx, uint8_t* base);

}  // namespace svr2011::d3d_trace
