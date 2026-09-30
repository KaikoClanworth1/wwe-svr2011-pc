// WWE SmackDown vs. Raw 2011 - the native renderer's command recording on a
// second thread.
//
// The renderer runs on the game's render thread (its D3D hooks), which also
// runs the game's own rendering code: in heavy entrances (3,000+ draws) the
// two together took ~19 ms a frame on a phone, over the 16.7 ms of 60 fps.
// This list stands in for the frame's plume command list: each call is
// copied into a command arena and a worker thread replays them, in order, on
// the real list - so the driver's command recording (~a quarter of the
// renderer's time on Adreno) runs alongside the game instead of in it.
// Everything a call refers to (pipelines, buffers, textures) outlives the
// frame's submission, which waits for the worker (Finish).

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <plume_render_interface.h>

namespace svr2011::native {

class RecordingList final : public plume::RenderCommandList {
 public:
  RecordingList();
  ~RecordingList() override;

  // Replays onto `target` from now on (the frame's real list; set before begin()).
  void SetTarget(plume::RenderCommandList* target) { target_ = target; }
  plume::RenderCommandList* target() const { return target_; }
  // Waits until everything recorded so far has been replayed (before the
  // real list is submitted), then reuses the arena.
  void Finish();

  // plume::RenderCommandList (the calls the renderer makes; others abort).
  void begin() override;
  void end() override;
  void barriers(plume::RenderBarrierStages stages, const plume::RenderBufferBarrier* bufferBarriers,
                uint32_t bufferBarriersCount, const plume::RenderTextureBarrier* textureBarriers,
                uint32_t textureBarriersCount) override;
  void dispatch(uint32_t, uint32_t, uint32_t) override { Unsupported(); }
  void traceRays(uint32_t, uint32_t, uint32_t, plume::RenderBufferReference,
                 const plume::RenderShaderBindingGroupsInfo&) override {
    Unsupported();
  }
  void drawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertexLocation,
                     uint32_t startInstanceLocation) override;
  void drawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation,
                            int32_t baseVertexLocation, uint32_t startInstanceLocation) override;
  void setPipeline(const plume::RenderPipeline* pipeline) override;
  void setComputePipelineLayout(const plume::RenderPipelineLayout*) override { Unsupported(); }
  void setComputePushConstants(uint32_t, const void*, uint32_t, uint32_t) override { Unsupported(); }
  void setComputeDescriptorSet(plume::RenderDescriptorSet*, uint32_t) override { Unsupported(); }
  void setGraphicsPipelineLayout(const plume::RenderPipelineLayout* pipelineLayout) override;
  void setGraphicsPushConstants(uint32_t rangeIndex, const void* data, uint32_t offset, uint32_t size) override;
  void setGraphicsDescriptorSet(plume::RenderDescriptorSet* descriptorSet, uint32_t setIndex) override;
  void setGraphicsRootDescriptor(plume::RenderBufferReference bufferReference, uint32_t rootDescriptorIndex) override;
  void setRaytracingPipelineLayout(const plume::RenderPipelineLayout*) override { Unsupported(); }
  void setRaytracingPushConstants(uint32_t, const void*, uint32_t, uint32_t) override { Unsupported(); }
  void setRaytracingDescriptorSet(plume::RenderDescriptorSet*, uint32_t) override { Unsupported(); }
  void setIndexBuffer(const plume::RenderIndexBufferView* view) override;
  void setVertexBuffers(uint32_t startSlot, const plume::RenderVertexBufferView* views, uint32_t viewCount,
                        const plume::RenderInputSlot* inputSlots) override;
  void setViewports(const plume::RenderViewport* viewports, uint32_t count) override;
  void setScissors(const plume::RenderRect* scissorRects, uint32_t count) override;
  void setFramebuffer(const plume::RenderFramebuffer* framebuffer) override;
  void setDepthBias(float depthBias, float depthBiasClamp, float slopeScaledDepthBias) override;
  void setStencilReference(uint32_t stencilReference) override;
  void setBlendFactor(const float blendFactor[4]) override;
  void clearColor(uint32_t attachmentIndex, plume::RenderColor colorValue, const plume::RenderRect* clearRects,
                  uint32_t clearRectsCount) override;
  void clearDepthStencil(bool clearDepth, bool clearStencil, float depthValue, uint32_t stencilValue,
                         const plume::RenderRect* clearRects, uint32_t clearRectsCount) override;
  void copyBufferRegion(plume::RenderBufferReference, plume::RenderBufferReference, uint64_t) override {
    Unsupported();
  }
  void copyTextureRegion(const plume::RenderTextureCopyLocation& dstLocation,
                         const plume::RenderTextureCopyLocation& srcLocation, uint32_t dstX, uint32_t dstY,
                         uint32_t dstZ, const plume::RenderBox* srcBox) override;
  void copyBuffer(const plume::RenderBuffer*, const plume::RenderBuffer*) override { Unsupported(); }
  void copyTexture(const plume::RenderTexture* dstTexture, const plume::RenderTexture* srcTexture) override;
  void resolveTexture(const plume::RenderTexture*, const plume::RenderTexture*) override { Unsupported(); }
  void resolveTextureRegion(const plume::RenderTexture*, uint32_t, uint32_t, const plume::RenderTexture*,
                            const plume::RenderRect*, plume::RenderResolveMode) override {
    Unsupported();
  }
  void buildBottomLevelAS(const plume::RenderAccelerationStructure*, plume::RenderBufferReference,
                          const plume::RenderBottomLevelASBuildInfo&) override {
    Unsupported();
  }
  void buildTopLevelAS(const plume::RenderAccelerationStructure*, plume::RenderBufferReference,
                       plume::RenderBufferReference, const plume::RenderTopLevelASBuildInfo&) override {
    Unsupported();
  }
  void discardTexture(const plume::RenderTexture*) override { Unsupported(); }
  void resetQueryPool(const plume::RenderQueryPool*, uint32_t, uint32_t) override { Unsupported(); }
  void writeTimestamp(const plume::RenderQueryPool*, uint32_t) override { Unsupported(); }

 private:
  struct Chunk;
  enum class Op : uint16_t;

  [[noreturn]] static void Unsupported();
  // Room for an op of `bytes` (header included) in the arena.
  uint8_t* Reserve(Op op, size_t bytes);
  void Publish();  // makes the reserved ops visible to the worker
  void Worker();
  // Replays the op at `p` onto target_; returns its size.
  size_t Replay(const uint8_t* p);

  plume::RenderCommandList* target_ = nullptr;
  std::vector<std::unique_ptr<Chunk>> chunks_;
  size_t write_chunk_ = 0, write_offset_ = 0;  // (the producer's)
  size_t ops_since_publish_ = 0;
  // Published position (chunk << 32 | offset) and the worker's.
  std::atomic<uint64_t> published_{0};
  std::atomic<uint64_t> replayed_{0};
  std::atomic<uint64_t> epoch_{0};          // frames finished (positions restart at 0)
  std::atomic<bool> sleeping_{false};       // the worker waits on work_
  std::atomic<bool> waiting_finish_{false};  // Finish waits on done_
  std::mutex mutex_;
  std::condition_variable work_, done_;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace svr2011::native
