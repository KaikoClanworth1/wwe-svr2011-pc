// WWE SmackDown vs. Raw 2011 - the native renderer's graphics API.
//
// The renderer draws through plume (github.com/renderbag/plume), a thin layer
// over Direct3D 12 and Vulkan. What plume doesn't cover - which GPU to use,
// handing finished frames to the emulator's presentation, GPU fault reports -
// is here, one implementation per API (backend_d3d12.cpp, backend_vulkan.cpp).
// The renderer draws on the emulator's own device with the emulator's API:
// its presentation shows the frames, which only works on the same device.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <plume_render_interface.h>

namespace rex::external_frame {
struct Frame;
}

namespace svr2011::native::backend {

enum class Api { kNone, kD3D12, kVulkan };

class Backend {
 public:
  virtual ~Backend() = default;
  virtual Api api() const = 0;
  virtual std::unique_ptr<plume::RenderInterface> CreateInterface(std::string* device_name) = 0;
  virtual plume::RenderShaderFormat ShaderFormat() const = 0;
  virtual const char* ShaderExtension() const = 0;
  virtual void PublishFrame(const std::shared_ptr<plume::RenderTexture>& image, uint32_t width,
                            uint32_t height, plume::RenderCommandFence* fence) = 0;
  virtual bool GetFrame(rex::external_frame::Frame& frame) = 0;
  virtual void ClearFrame() = 0;
  virtual bool DeviceLost(plume::RenderDevice* device) = 0;
  virtual void ReportDeviceLost(plume::RenderDevice* device) = 0;
  virtual void ReleaseFence(plume::RenderCommandFence* fence) = 0;
  virtual void DrainDebugMessages(plume::RenderDevice* device) = 0;
  virtual void LogBuffer(plume::RenderBuffer* buffer, uint64_t size) = 0;
  virtual bool PipelineCreated(plume::RenderPipeline* pipeline) = 0;
  virtual void StallQueue(plume::RenderCommandQueue* queue) = 0;
  // A pipeline cache kept on disk (Vulkan: a VkPipelineCache; D3D12: a
  // pipeline library).
  virtual void LoadPipelineCache(plume::RenderDevice*, const std::filesystem::path&) {}
  virtual void SavePipelineCache(plume::RenderDevice*, const std::filesystem::path&) {}
};

std::unique_ptr<Backend> CreateD3D12Backend();   // null where D3D12 isn't built
std::unique_ptr<Backend> CreateVulkanBackend();

// Picks the emulator's API (Vulkan when it runs on Vulkan, else D3D12) and
// creates the interface and the name of the GPU to draw on.
std::unique_ptr<plume::RenderInterface> CreateInterface(std::string* device_name);
Api ActiveApi();

// The converted shaders this backend loads: "<hash>.<vs|ps><variant><extension>".
plume::RenderShaderFormat ShaderFormat();
const char* ShaderExtension();

// Main mode: `image` (shader-readable, RGBA8) is this frame, finished when
// `fence` - just signaled by the frame's submission - is. The emulator's
// presentation shows it (rex/external_frame.h) until the next one; the
// backend keeps the image alive while the emulator may still read it.
void PublishFrame(const std::shared_ptr<plume::RenderTexture>& image, uint32_t width,
                  uint32_t height, plume::RenderCommandFence* fence);
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
// The pipeline cache file (per API): read before the first pipeline, written
// when new pipelines were built. Compiled pipelines then load from it the
// next time instead of being compiled again (phones' drivers keep none).
void LoadPipelineCache(plume::RenderDevice* device, const std::filesystem::path& file);
void SavePipelineCache(plume::RenderDevice* device, const std::filesystem::path& file);
// Debug: makes `queue` wait for good before its next work (a stuck GPU job).
void StallQueue(plume::RenderCommandQueue* queue);

}  // namespace svr2011::native::backend
