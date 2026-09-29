// WWE SmackDown vs. Raw 2011 - the native renderer's graphics API.
//
// The renderer draws through plume (github.com/renderbag/plume), a thin layer
// over Direct3D 12 and Vulkan. What plume doesn't cover - which GPU to use,
// handing finished frames to the emulator's presentation, GPU fault reports -
// is here, one implementation per backend (backend_d3d12.cpp; Vulkan for
// Android / Linux later).

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <plume_render_interface.h>

namespace rex::external_frame {
struct Frame;
}

namespace svr2011::native::backend {

// The graphics API and the name of the GPU to draw on (the emulator's: its
// presentation shows these frames, which only works on the same device).
std::unique_ptr<plume::RenderInterface> CreateInterface(std::string* device_name);

// The converted shaders this backend loads: "<hash>.<vs|ps><variant><extension>".
plume::RenderShaderFormat ShaderFormat();
const char* ShaderExtension();

// Main mode: `image` (shader-readable, RGBA8) is this frame, finished when
// `fence` - just signaled by the frame's submission - is. The emulator's
// presentation shows it (rex/external_frame.h) until the next one.
void PublishFrame(plume::RenderTexture* image, uint32_t width, uint32_t height,
                  plume::RenderCommandFence* fence);
bool GetFrame(rex::external_frame::Frame& frame);
void ClearFrame();

// The GPU stopped: whether the device is lost, and a report of why (with
// the faulting allocation when the driver tells).
bool DeviceLost(plume::RenderDevice* device);
void ReportDeviceLost(plume::RenderDevice* device);
// Sets `fence` from the CPU, so work waiting on it (the emulator's queue,
// for a frame) runs on after the GPU stopped answering.
void ReleaseFence(plume::RenderCommandFence* fence);
// Debug layer messages (SVR2011_NATIVE_D3D_DEBUG), logged and cleared.
void DrainDebugMessages(plume::RenderDevice* device);
// Names a buffer's GPU range for the fault report.
void LogBuffer(plume::RenderBuffer* buffer, uint64_t size);
// Whether the driver built the pipeline (plume returns one either way).
bool PipelineCreated(plume::RenderPipeline* pipeline);
// Debug: makes `queue` wait for good before its next work (a stuck GPU job).
void StallQueue(plume::RenderCommandQueue* queue);

}  // namespace svr2011::native::backend
