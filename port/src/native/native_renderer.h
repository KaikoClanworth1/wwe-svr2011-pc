// WWE SmackDown vs. Raw 2011 - native Direct3D 12 renderer (prototype).
//
// Instead of emulating the Xenos GPU, the game's own D3D calls are hooked
// (native_hooks.cpp) and drawn directly with D3D12, using the game's shaders
// converted ahead of time (XenosRecomp -> DXIL, tools/convert_shaders.py).
// The game state at each draw is read from the D3D device's register mirror
// (guest_d3d.h).
//
// native_renderer = main    (default) draws the game; the emulated renderer
//                           is the backup
//                 = off     not used (the emulated renderer draws)
//                 = shadow  runs beside the emulated renderer, drawing into a
//                           second window "SvR 2011 - native renderer" so the
//                           two can be compared
// The window title shows the native frame rate and draw statistics.
// Shaders are loaded from SVR2011_NATIVE_SHADERS (default <exe>\native_shaders).

#pragma once

#include <functional>
#include <string>
#include <utility>

#include <cstdint>

struct PPCContext;

namespace rex::memory {
class Memory;
}

namespace svr2011::native {

bool Enabled();

// Main mode: switch between this renderer and the emulated one while the
// game runs (the GRAPHICS page). CanSwitch: started as the main renderer
// and still working.
bool CanSwitch();
void SetNativeActive(bool active);
bool NativeActive();

// What draws the game right now, for the FPS counter: "Native 2x",
// "Emulated", or "Emulated (native failed)".
std::string RendererLabel();

// Game state (any thread): a match (entrances on) or the menus - on a wide
// window, matches fill it (native_widescreen), menus stay 16:9.
void SetMatchScene(bool in_match);

// The main window's client size in pixels (main mode: the frames follow it).
void SetWindowSizeSource(std::function<std::pair<uint32_t, uint32_t>()> source);

// Called once guest memory exists (before the game runs).
void Attach(rex::memory::Memory* memory);

// Hook entry points (guest render thread).
// front_buffer: the texture object Swap shows (guest pointer).
void OnPresent(uint32_t front_buffer);
void OnDrawIndexed(const PPCContext& ctx);
void OnDrawUP(const PPCContext& args, uint32_t data);  // data: the space it returned
void OnEndVertices();
void OnClear(const PPCContext& ctx);
void OnResolve(const PPCContext& ctx);
void OnSetRenderTarget(uint32_t index, uint32_t surface);
void OnSetStreamSource(uint32_t stream, uint32_t buffer, uint32_t offset, uint32_t stride);
void OnShaderCreated(uint32_t container, uint32_t object, bool pixel);

// Shader constants, tracked as the GPU's constant file sees them:
//   SetVertex/PixelShaderConstantF write the device mirror (marked dirty),
//   the D3D flush copies dirty mirror ranges to the GPU,
//   and per-draw blocks are loaded straight from memory (LOAD_ALU_CONSTANT).
void OnSetShaderConstants(bool pixel, uint32_t start, uint32_t count);
// The D3D constant flush: `mask` has a bit per 4 dirty constants of the stage
// whose GPU registers start at `reg` (0x4000 vertex, 0x4400 pixel).
void OnFlushShaderConstants(uint64_t mask, uint32_t reg);
void OnLoadShaderConstants(uint32_t shader_object, uint32_t base);

}  // namespace svr2011::native
