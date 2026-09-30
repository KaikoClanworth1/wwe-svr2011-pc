// WWE SmackDown vs. Raw 2011 - the native renderer's Vulkan backend (gpu.h).
//
// plume adopts the emulator's own Vulkan device and queue (external_frame
// VulkanDevice; see plume's VulkanExistingDevice): the frame images are
// shown by the emulator's presentation directly, and as both submit to one
// queue, submission order and a barrier on the emulator's side are all the
// synchronization the handoff needs.

#include "native/gpu.h"

#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <plume_vulkan.h>

#include <rex/external_frame.h>
#include <rex/logging.h>

namespace svr2011::native::backend {

namespace {

class VulkanBackend final : public Backend {
 public:
  Api api() const override { return Api::kVulkan; }

  std::unique_ptr<plume::RenderInterface> CreateInterface(std::string* device_name) override {
    device_name->clear();
    const rex::external_frame::VulkanDevice* d = rex::external_frame::GetVulkanDevice();
    if (!d) return nullptr;
    if (!d->app_renderer_features) {
      REXLOG_ERROR("native renderer: the Vulkan device lacks descriptor indexing, buffer device "
                   "addresses or 64-bit shader integers");
      return nullptr;
    }
    if (d->api_version < VK_API_VERSION_1_2) {
      REXLOG_ERROR("native renderer: Vulkan 1.2 needed");
      return nullptr;
    }
    plume::VulkanExistingDevice existing;
    existing.instance = static_cast<VkInstance>(d->instance);
    existing.apiVersion = d->api_version;
    existing.physicalDevice = static_cast<VkPhysicalDevice>(d->physical_device);
    existing.device = static_cast<VkDevice>(d->device);
    existing.queueFamilyIndex = d->queue_family;
    existing.queue = static_cast<VkQueue>(d->queue);
    existing.queueMutex = d->queue_mutex;
    existing.descriptorIndexing = true;
    existing.bufferDeviceAddress = true;
    existing.scalarBlockLayout = d->scalar_block_layout;
    existing.nullDescriptor = d->null_descriptor;
    existing.samplerMirrorClampToEdge = d->sampler_mirror_clamp_to_edge;
    existing.geometryShader = d->geometry_shader;
    return plume::CreateVulkanInterface(existing);
  }

  plume::RenderShaderFormat ShaderFormat() const override { return plume::RenderShaderFormat::SPIRV; }

  void LoadPipelineCache(plume::RenderDevice* device, const std::filesystem::path& file) override {
    std::vector<uint8_t> data;
    if (std::ifstream in{file, std::ios::binary}) {
      data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    auto* vk = static_cast<plume::VulkanDevice*>(device);
    saved_size_ = data.size();
    if (vk->setPipelineCacheData(data.empty() ? nullptr : data.data(), data.size())) {
      REXLOG_INFO("native renderer: pipeline cache {} ({} KB)", data.empty() ? "new" : "loaded",
                  data.size() / 1024);
    }
  }

  void SavePipelineCache(plume::RenderDevice* device, const std::filesystem::path& file) override {
    const std::vector<uint8_t> data = static_cast<plume::VulkanDevice*>(device)->getPipelineCacheData();
    if (data.empty() || data.size() == saved_size_) return;  // (only cache hits: nothing new)
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
      if (!out) return;
    }
    std::filesystem::rename(tmp, file, ec);
    if (!ec) {
      saved_size_ = data.size();
      REXLOG_INFO("native renderer: pipeline cache saved ({} KB)", data.size() / 1024);
    }
  }
  const char* ShaderExtension() const override { return ".spv"; }

  void PublishFrame(const std::shared_ptr<plume::RenderTexture>& image, uint32_t width,
                    uint32_t height, plume::RenderCommandFence*) override {
    std::lock_guard lock(mutex_);
    ++serial_;
    Held& held = held_[image.get()];
    if (!held.texture) {
      held.texture = image;
      held.view = image->createTextureView(
          plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM));
    }
    held.serial = serial_;
    // Images not shown for a while (replaced by a window resize) go: the
    // emulator's presentation is long done with them.
    for (auto it = held_.begin(); it != held_.end();) {
      it = it->second.serial + kHoldFrames < serial_ ? held_.erase(it) : std::next(it);
    }
    frame_image_ = uint64_t(static_cast<plume::VulkanTexture*>(image.get())->vk);
    frame_view_ = uint64_t(static_cast<plume::VulkanTextureView*>(held.view.get())->vk);
    frame_w_ = width;
    frame_h_ = height;
  }

  bool GetFrame(rex::external_frame::Frame& frame) override {
    std::lock_guard lock(mutex_);
    if (!frame_view_) return false;
    frame.vk_image = frame_image_;
    frame.vk_image_view = frame_view_;
    frame.width = frame_w_;
    frame.height = frame_h_;
    return true;
  }

  void ClearFrame() override {
    std::lock_guard lock(mutex_);
    frame_image_ = frame_view_ = 0;
  }

  // A lost device shows up in the emulator too (the same device), which
  // reports it.
  bool DeviceLost(plume::RenderDevice*) override { return false; }
  void ReportDeviceLost(plume::RenderDevice*) override {}
  // The emulator doesn't wait for the renderer's fences (one queue).
  void ReleaseFence(plume::RenderCommandFence*) override {}
  void DrainDebugMessages(plume::RenderDevice*) override {}
  void LogBuffer(plume::RenderBuffer*, uint64_t) override {}

  bool PipelineCreated(plume::RenderPipeline* pipeline) override {
    return pipeline && static_cast<plume::VulkanGraphicsPipeline*>(pipeline)->vk != VK_NULL_HANDLE;
  }

  void StallQueue(plume::RenderCommandQueue*) override {
    REXLOG_WARN("native renderer: the test stall is D3D12 only");
  }

 private:
  size_t saved_size_ = 0;  // (the pipeline cache file's)
  static constexpr uint64_t kHoldFrames = 16;
  struct Held {
    std::shared_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    uint64_t serial = 0;
  };
  std::mutex mutex_;
  std::unordered_map<const plume::RenderTexture*, Held> held_;
  uint64_t serial_ = 0;
  uint64_t frame_image_ = 0, frame_view_ = 0;
  uint32_t frame_w_ = 0, frame_h_ = 0;
};

}  // namespace

std::unique_ptr<Backend> CreateVulkanBackend() { return std::make_unique<VulkanBackend>(); }

}  // namespace svr2011::native::backend
