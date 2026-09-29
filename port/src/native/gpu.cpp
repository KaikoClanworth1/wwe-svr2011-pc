// WWE SmackDown vs. Raw 2011 - the native renderer's graphics API (gpu.h):
// the backend matching the emulator's.

#include "native/gpu.h"

#include <rex/external_frame.h>
#include <rex/logging.h>

namespace svr2011::native::backend {

#if !defined(_WIN32)
std::unique_ptr<Backend> CreateD3D12Backend() { return nullptr; }  // (Windows only)
#endif

namespace {
std::unique_ptr<Backend> g_backend;
}  // namespace

std::unique_ptr<plume::RenderInterface> CreateInterface(std::string* device_name) {
  g_backend = rex::external_frame::GetVulkanDevice() ? CreateVulkanBackend() : CreateD3D12Backend();
  if (!g_backend) {
    REXLOG_ERROR("native renderer: no backend for the emulator's graphics API");
    return nullptr;
  }
  REXLOG_INFO("native renderer: {} backend", g_backend->api() == Api::kVulkan ? "Vulkan" : "D3D12");
  return g_backend->CreateInterface(device_name);
}

Api ActiveApi() { return g_backend ? g_backend->api() : Api::kNone; }

plume::RenderShaderFormat ShaderFormat() { return g_backend->ShaderFormat(); }
const char* ShaderExtension() { return g_backend ? g_backend->ShaderExtension() : ".dxil"; }

void PublishFrame(const std::shared_ptr<plume::RenderTexture>& image, uint32_t width,
                  uint32_t height, plume::RenderCommandFence* fence) {
  g_backend->PublishFrame(image, width, height, fence);
}
bool GetFrame(rex::external_frame::Frame& frame) { return g_backend && g_backend->GetFrame(frame); }
void ClearFrame() {
  if (g_backend) g_backend->ClearFrame();
}
bool DeviceLost(plume::RenderDevice* device) { return g_backend && g_backend->DeviceLost(device); }
void ReportDeviceLost(plume::RenderDevice* device) {
  if (g_backend) g_backend->ReportDeviceLost(device);
}
void ReleaseFence(plume::RenderCommandFence* fence) {
  if (g_backend) g_backend->ReleaseFence(fence);
}
void DrainDebugMessages(plume::RenderDevice* device) {
  if (g_backend) g_backend->DrainDebugMessages(device);
}
void LogBuffer(plume::RenderBuffer* buffer, uint64_t size) {
  if (g_backend) g_backend->LogBuffer(buffer, size);
}
bool PipelineCreated(plume::RenderPipeline* pipeline) {
  return g_backend && g_backend->PipelineCreated(pipeline);
}
void StallQueue(plume::RenderCommandQueue* queue) {
  if (g_backend) g_backend->StallQueue(queue);
}

}  // namespace svr2011::native::backend
