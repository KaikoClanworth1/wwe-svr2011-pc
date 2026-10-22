// WWE SmackDown vs. Raw 2011 - the native renderer's FSR 2 backend (see fsr2.h).
//
// FSR 2's runtime (ffx_fsr2.cpp) asks its backend (FfxFsr2Interface) for resources and pipelines and hands it GPU
// jobs (compute dispatches, copies, clears) to record. This one does that on plume:
//  - resources: plume textures (storage textures when FSR 2 writes them), a full-chain view to read and one view
//    per mip to write; initial data uploaded at the first record;
//  - pipelines: fsr2_<pass>.cs on one layout - a set with 16 textures, 16 storage textures, 2 constant buffers and
//    the point / linear clamp samplers (Direct3D 12: t0, u0, b0, s0 in space 0; Vulkan: bindings 0, 32, 48, 56 -
//    fsr2_shaders.py), the bindings from fsr2_bindings.inc;
//  - jobs: per dispatch a descriptor set and its constant buffers from the frame slot's pool; barriers from each
//    resource's tracked layout (a storage texture written twice in a row: a UAV barrier).

#include "native/fsr2.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>

#include <rex/logging.h>

#include "../../third_party/ffx-fsr2/ffx_fsr2.h"

namespace svr2011::fsr2 {
namespace {

using plume::RenderFormat;
using plume::RenderTextureLayout;

struct Fsr2Binding {
  char kind;  // 't' / 'u' / 'b'
  uint32_t reg;
  const wchar_t* name;
};
struct Fsr2Pass {
  const char* shader;
  const Fsr2Binding* bindings;
  uint32_t count;
};
#include "native/fsr2_bindings.inc"

constexpr uint32_t kTextures = 16, kStorage = 16, kConstants = 2;
constexpr uint32_t kSetTexture = 0, kSetStorage = kTextures, kSetConstant = kTextures + kStorage;
constexpr uint32_t kDispatchesPerFrame = 24;  // (FSR 2: about 8 a frame, the clears on a reset)
constexpr uint32_t kConstantBytes = 256;
constexpr int32_t kClearPass = FFX_FSR2_PASS_COUNT;       // (our own: fsr2_clear.cs)
constexpr int32_t kMotionPass = FFX_FSR2_PASS_COUNT + 1;  // (fsr2_motion.cs: the camera's motion vectors)

RenderFormat Format(FfxSurfaceFormat f) {
  switch (f) {
    case FFX_SURFACE_FORMAT_R32G32B32A32_TYPELESS:
    case FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT: return RenderFormat::R32G32B32A32_FLOAT;
    case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT: return RenderFormat::R16G16B16A16_FLOAT;
    case FFX_SURFACE_FORMAT_R16G16B16A16_UNORM: return RenderFormat::R16G16B16A16_UNORM;
    case FFX_SURFACE_FORMAT_R32G32_FLOAT: return RenderFormat::R32G32_FLOAT;
    case FFX_SURFACE_FORMAT_R32_UINT: return RenderFormat::R32_UINT;
    case FFX_SURFACE_FORMAT_R8G8B8A8_TYPELESS:
    case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM: return RenderFormat::R8G8B8A8_UNORM;
    case FFX_SURFACE_FORMAT_R11G11B10_FLOAT: return RenderFormat::R16G16B16A16_FLOAT;  // (plume has no R11G11B10)
    case FFX_SURFACE_FORMAT_R16G16_FLOAT: return RenderFormat::R16G16_FLOAT;
    case FFX_SURFACE_FORMAT_R16G16_UINT: return RenderFormat::R16G16_UINT;
    case FFX_SURFACE_FORMAT_R16_FLOAT: return RenderFormat::R16_FLOAT;
    case FFX_SURFACE_FORMAT_R16_UINT: return RenderFormat::R16_UINT;
    case FFX_SURFACE_FORMAT_R16_UNORM: return RenderFormat::R16_UNORM;
    case FFX_SURFACE_FORMAT_R16_SNORM: return RenderFormat::R16_SNORM;
    case FFX_SURFACE_FORMAT_R8_UNORM: return RenderFormat::R8_UNORM;
    case FFX_SURFACE_FORMAT_R8_UINT: return RenderFormat::R8_UINT;
    case FFX_SURFACE_FORMAT_R8G8_UNORM: return RenderFormat::R8G8_UNORM;
    case FFX_SURFACE_FORMAT_R32_FLOAT: return RenderFormat::R32_FLOAT;
    default: return RenderFormat::UNKNOWN;
  }
}

FfxSurfaceFormat SurfaceFormat(RenderFormat f) {
  switch (f) {
    case RenderFormat::R32G32B32A32_FLOAT: return FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT;
    case RenderFormat::R16G16B16A16_FLOAT: return FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case RenderFormat::R32G32_FLOAT: return FFX_SURFACE_FORMAT_R32G32_FLOAT;
    case RenderFormat::R32_UINT: return FFX_SURFACE_FORMAT_R32_UINT;
    case RenderFormat::R8G8B8A8_UNORM: return FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case RenderFormat::R16G16_FLOAT: return FFX_SURFACE_FORMAT_R16G16_FLOAT;
    case RenderFormat::R16_FLOAT: return FFX_SURFACE_FORMAT_R16_FLOAT;
    case RenderFormat::R8_UNORM: return FFX_SURFACE_FORMAT_R8_UNORM;
    case RenderFormat::R8G8_UNORM: return FFX_SURFACE_FORMAT_R8G8_UNORM;
    case RenderFormat::R32_FLOAT: return FFX_SURFACE_FORMAT_R32_FLOAT;
    default: return FFX_SURFACE_FORMAT_UNKNOWN;
  }
}

uint32_t BytesPerTexel(RenderFormat f) {
  switch (f) {
    case RenderFormat::R32G32B32A32_FLOAT: return 16;
    case RenderFormat::R16G16B16A16_FLOAT:
    case RenderFormat::R16G16B16A16_UNORM:
    case RenderFormat::R32G32_FLOAT: return 8;
    case RenderFormat::R32_UINT:
    case RenderFormat::R8G8B8A8_UNORM:
    case RenderFormat::R16G16_FLOAT:
    case RenderFormat::R16G16_UINT:
    case RenderFormat::R32_FLOAT: return 4;
    case RenderFormat::R16_FLOAT:
    case RenderFormat::R16_UINT:
    case RenderFormat::R16_UNORM:
    case RenderFormat::R16_SNORM:
    case RenderFormat::R8G8_UNORM: return 2;
    default: return 1;
  }
}

struct Resource {
  std::unique_ptr<plume::RenderTexture> owned;
  plume::RenderTexture* texture = nullptr;
  RenderFormat format = RenderFormat::UNKNOWN;
  FfxResourceDescription desc{};
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  bool storage = false;
  uint32_t mips = 1;
  std::unique_ptr<plume::RenderTextureView> read;
  std::vector<std::unique_ptr<plume::RenderTextureView>> write;  // (per mip)
  std::vector<uint8_t> init;                                     // (uploaded at the next record)
  Texture* external = nullptr;                                   // (a renderer's texture: its layout kept there)
};

struct Pipeline {
  std::unique_ptr<plume::RenderShader> shader;
  std::unique_ptr<plume::RenderPipeline> pipeline;
};

struct Slot {
  std::unique_ptr<plume::RenderDescriptorSet> set;
  std::array<std::unique_ptr<plume::RenderBuffer>, kConstants> constants;
  std::array<void*, kConstants> mapped{};
};

class Backend final : public Upscaler {
 public:
  Host host;
  uint32_t render_w = 0, render_h = 0, display_w = 0, display_h = 0;
  std::unique_ptr<plume::RenderSampler> point, linear;
  std::unique_ptr<plume::RenderPipelineLayout> layout;
  std::array<Pipeline, FFX_FSR2_PASS_COUNT + 2> pipelines;
  uint32_t motion_used = 0;  // (the frame's descriptor sets taken by Motion before Dispatch's)
  Resource motion_in, motion_out, motion_depth;  // (Motion's depth and motion: views kept while the textures are the same)
  std::vector<Resource> resources;  // FSR 2's (internal) then this dispatch's registered ones
  size_t internal_count = 0;
  std::vector<FfxGpuJobDescription> jobs;
  std::vector<std::vector<Slot>> slots;  // [frame slot][dispatch]
  std::vector<uint8_t> scratch;
  FfxFsr2Context context{};
  bool context_made = false;
  bool failed = false;
  // The upload buffers of the initial data, kept until their copies have run (a frame slot's next use).
  std::vector<std::vector<std::unique_ptr<plume::RenderBuffer>>> uploads;
  uint32_t record_slot = 0;

  ~Backend() override {
    if (context_made) ffxFsr2ContextDestroy(&context);
  }
  uint32_t MaxRenderWidth() const override { return render_w; }
  uint32_t MaxRenderHeight() const override { return render_h; }
  uint32_t DisplayWidth() const override { return display_w; }
  uint32_t DisplayHeight() const override { return display_h; }

  bool MakeLayout() {
    plume::RenderSamplerDesc s;
    s.addressU = s.addressV = s.addressW = plume::RenderTextureAddressMode::CLAMP;
    s.minFilter = s.magFilter = plume::RenderFilter::NEAREST;
    s.mipmapMode = plume::RenderMipmapMode::NEAREST;
    point = host.device->createSampler(s);
    s.minFilter = s.magFilter = plume::RenderFilter::LINEAR;
    s.mipmapMode = plume::RenderMipmapMode::LINEAR;
    linear = host.device->createSampler(s);
    if (!point || !linear) return false;
    static const plume::RenderSampler* samplers[2];
    samplers[0] = point.get();
    samplers[1] = linear.get();
    // (Direct3D 12: each register kind from 0; Vulkan: apart in the one set)
    const uint32_t t = 0, u = host.vulkan ? 32 : 0, b = host.vulkan ? 48 : 0, smp = host.vulkan ? 56 : 0;
    // (one range a register: Vulkan makes a range one binding - an array - and the shaders have a binding each)
    size_t n = 0;
    auto add = [&](plume::RenderDescriptorRangeType type, uint32_t binding, const plume::RenderSampler** immutable) {
      plume::RenderDescriptorRange& rg = set_ranges[n++];
      rg.type = type;
      rg.count = 1;
      rg.binding = binding;
      rg.immutableSampler = immutable;
    };
    for (uint32_t i = 0; i < kTextures; ++i) add(plume::RenderDescriptorRangeType::TEXTURE, t + i, nullptr);
    for (uint32_t i = 0; i < kStorage; ++i) add(plume::RenderDescriptorRangeType::READ_WRITE_TEXTURE, u + i, nullptr);
    for (uint32_t i = 0; i < kConstants; ++i) add(plume::RenderDescriptorRangeType::CONSTANT_BUFFER, b + i, nullptr);
    add(plume::RenderDescriptorRangeType::SAMPLER, smp, &samplers[0]);
    add(plume::RenderDescriptorRangeType::SAMPLER, smp + 1, &samplers[1]);
    set_desc = plume::RenderDescriptorSetDesc(set_ranges, uint32_t(n));
    plume::RenderPipelineLayoutDesc d;
    d.descriptorSetDescs = &set_desc;
    d.descriptorSetDescsCount = 1;
    layout = host.device->createPipelineLayout(d);
    return layout != nullptr;
  }
  plume::RenderDescriptorRange set_ranges[kTextures + kStorage + kConstants + 2];
  plume::RenderDescriptorSetDesc set_desc;

  bool MakePipeline(int pass, const char* shader) {
    Pipeline& p = pipelines[size_t(pass)];
    if (p.pipeline) return true;
    const std::vector<uint8_t> code = host.read_shader(shader);
    if (code.empty()) {
      REXLOG_WARN("native renderer: FSR 2 shader {} missing", shader);
      return false;
    }
    p.shader = host.device->createShader(code.data(), code.size(), "CS", host.format);
    if (!p.shader) return false;
    plume::RenderComputePipelineDesc d(layout.get(), p.shader.get(), 8, 8, 1);
    p.pipeline = host.device->createComputePipeline(d);
    if (!p.pipeline) REXLOG_ERROR("native renderer: FSR 2 pipeline {} failed", shader);
    return p.pipeline != nullptr;
  }

  Slot* NextSlot(uint32_t frame_slot, uint32_t& used) {
    auto& pool = slots[frame_slot];
    if (used >= pool.size()) {
      if (pool.size() >= kDispatchesPerFrame * 4) return nullptr;
      Slot s;
      s.set = host.device->createDescriptorSet(set_desc);
      for (auto& c : s.constants) {
        c = host.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(kConstantBytes, plume::RenderBufferFlag::CONSTANT));
        if (!c) return nullptr;
      }
      if (!s.set) return nullptr;
      for (size_t i = 0; i < kConstants; ++i) s.mapped[i] = s.constants[i]->map();
      pool.push_back(std::move(s));
    }
    return &pool[used++];
  }

  // A resource into `layout` (the same storage layout twice: a UAV barrier).
  void Transition(plume::RenderCommandList* list, Resource& r, RenderTextureLayout layout) {
    if (!r.texture) return;
    if (r.layout == layout && layout != RenderTextureLayout::GENERAL) return;
    const plume::RenderTextureBarrier b(r.texture, layout);
    list->barriers(plume::RenderBarrierStage::COMPUTE | plume::RenderBarrierStage::COPY, nullptr, 0, &b, 1);
    r.layout = layout;
    if (r.external) r.external->layout = layout;
  }

  plume::RenderTextureView* WriteView(Resource& r, uint32_t mip) {
    if (mip >= r.write.size()) return nullptr;
    if (!r.write[mip]) {
      plume::RenderTextureViewDesc v = plume::RenderTextureViewDesc::Texture2D(r.format);
      v.mipSlice = mip;
      v.mipLevels = 1;
      r.write[mip] = r.texture->createTextureView(v);
    }
    return r.write[mip].get();
  }

  bool Record(plume::RenderCommandList* list, uint32_t frame_slot) {
    uint32_t used = motion_used;
    motion_used = 0;
    uploads[frame_slot].clear();
    // Initial data first (FSR 2's look-up tables, the default exposure ...).
    for (auto& r : resources) {
      if (r.init.empty() || !r.texture) continue;
      const uint32_t bpp = BytesPerTexel(r.format);
      const uint32_t row = (r.desc.width * bpp + 255) / 256 * 256;
      auto buf = host.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(uint64_t(row) * r.desc.height));
      if (!buf) continue;
      auto* dst = static_cast<uint8_t*>(buf->map());
      const size_t src_row = size_t(r.desc.width) * bpp;
      for (uint32_t y = 0; y < r.desc.height; ++y)
        if ((y + 1) * src_row <= r.init.size()) std::memcpy(dst + size_t(y) * row, r.init.data() + y * src_row, src_row);
      buf->unmap();
      Transition(list, r, RenderTextureLayout::COPY_DEST);
      list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(r.texture, 0),
                              plume::RenderTextureCopyLocation::PlacedFootprint(buf.get(), r.format, r.desc.width,
                                                                                r.desc.height, 1, row / bpp));
      r.init.clear();
      uploads[frame_slot].push_back(std::move(buf));
    }
    for (const auto& job : jobs) {
      if (job.jobType == FFX_GPU_JOB_COPY) {
        Resource& src = resources[size_t(job.copyJobDescriptor.src.internalIndex)];
        Resource& dst = resources[size_t(job.copyJobDescriptor.dst.internalIndex)];
        Transition(list, src, RenderTextureLayout::COPY_SOURCE);
        Transition(list, dst, RenderTextureLayout::COPY_DEST);
        list->copyTexture(dst.texture, src.texture);
        continue;
      }
      Slot* slot = NextSlot(frame_slot, used);
      if (!slot) return false;
      int pass = kClearPass;
      const FfxComputeJobDescription* c = job.jobType == FFX_GPU_JOB_COMPUTE ? &job.computeJobDescriptor : nullptr;
      uint32_t groups[3] = {1, 1, 1};
      if (c) {
        pass = int(reinterpret_cast<intptr_t>(c->pipeline.pipeline)) - 1;
        for (uint32_t i = 0; i < c->pipeline.srvCount; ++i) {
          const int32_t id = c->srvs[i].internalIndex;
          if (id < 0 || size_t(id) >= resources.size()) continue;
          Resource& r = resources[size_t(id)];
          if (!r.texture) continue;
          Transition(list, r, RenderTextureLayout::SHADER_READ);
          slot->set->setTexture(kSetTexture + c->pipeline.srvResourceBindings[i].slotIndex, r.texture,
                                RenderTextureLayout::SHADER_READ, r.read.get());
        }
        for (uint32_t i = 0; i < c->pipeline.uavCount; ++i) {
          const int32_t id = c->uavs[i].internalIndex;
          if (id < 0 || size_t(id) >= resources.size()) continue;
          Resource& r = resources[size_t(id)];
          if (!r.texture) continue;
          Transition(list, r, RenderTextureLayout::GENERAL);
          slot->set->setTexture(kSetStorage + c->pipeline.uavResourceBindings[i].slotIndex, r.texture,
                                RenderTextureLayout::GENERAL, WriteView(r, c->uavMip[i]));
        }
        for (uint32_t i = 0; i < c->pipeline.constCount && i < kConstants; ++i) {
          const uint32_t reg = c->pipeline.cbResourceBindings[i].slotIndex;
          if (reg >= kConstants) continue;
          std::memcpy(slot->mapped[reg], c->cbs[i].data, std::min<size_t>(c->cbs[i].uint32Size * 4, kConstantBytes));
          slot->set->setBuffer(kSetConstant + reg, slot->constants[reg].get(), kConstantBytes);
        }
        groups[0] = c->dimensions[0], groups[1] = c->dimensions[1], groups[2] = c->dimensions[2];
      } else {  // FFX_GPU_JOB_CLEAR_FLOAT: fsr2_clear.cs over mip 0
        Resource& r = resources[size_t(job.clearJobDescriptor.target.internalIndex)];
        if (!r.texture) continue;
        Transition(list, r, RenderTextureLayout::GENERAL);
        slot->set->setTexture(kSetStorage, r.texture, RenderTextureLayout::GENERAL, WriteView(r, 0));
        std::memcpy(slot->mapped[0], job.clearJobDescriptor.color, 16);
        slot->set->setBuffer(kSetConstant, slot->constants[0].get(), kConstantBytes);
        groups[0] = (r.desc.width + 7) / 8, groups[1] = (r.desc.height + 7) / 8;
      }
      const Pipeline& p = pipelines[size_t(std::clamp(pass, 0, kClearPass))];
      if (!p.pipeline) return false;
      list->setComputePipelineLayout(layout.get());
      list->setPipeline(p.pipeline.get());
      list->setComputeDescriptorSet(slot->set.get(), 0);
      list->dispatch(groups[0], groups[1], groups[2]);
    }
    jobs.clear();
    return true;
  }

  // (a renderer's texture as a Resource: views made again when it changes)
  void Wrap(Resource& r, Texture& t, bool storage) {
    if (r.texture != t.texture || r.format != t.format) {
      r = {};
      r.texture = t.texture;
      r.format = t.format;
      r.storage = storage;
      plume::RenderTextureViewDesc v = plume::RenderTextureViewDesc::Texture2D(t.format);
      v.mipLevels = 1;
      r.read = t.texture->createTextureView(v);
      r.write.resize(storage ? 1 : 0);
    }
    r.desc.width = t.width, r.desc.height = t.height;
    r.layout = t.layout;
    r.external = &t;
  }

  bool Motion(plume::RenderCommandList* list, uint32_t frame_slot, Texture& depth, Texture& motion,
              Texture& depth_out, const float reproject[16], const float jitter[2], uint32_t rw,
              uint32_t rh) override {
    if (failed || frame_slot >= slots.size() || !depth.texture || !motion.texture || !depth_out.texture) return false;
    if (!MakePipeline(kMotionPass, "fsr2_motion.cs")) return false;
    uint32_t used = 0;
    Slot* slot = NextSlot(frame_slot, used);
    if (!slot) return false;
    Wrap(motion_in, depth, false);
    Wrap(motion_out, motion, true);
    Wrap(motion_depth, depth_out, true);
    Transition(list, motion_in, RenderTextureLayout::SHADER_READ);
    Transition(list, motion_out, RenderTextureLayout::GENERAL);
    Transition(list, motion_depth, RenderTextureLayout::GENERAL);
    slot->set->setTexture(kSetTexture, motion_in.texture, RenderTextureLayout::SHADER_READ, motion_in.read.get());
    slot->set->setTexture(kSetStorage, motion_out.texture, RenderTextureLayout::GENERAL, WriteView(motion_out, 0));
    slot->set->setTexture(kSetStorage + 1, motion_depth.texture, RenderTextureLayout::GENERAL,
                          WriteView(motion_depth, 0));
    float cb[20];
    std::memcpy(cb, reproject, 64);
    cb[16] = jitter[0], cb[17] = jitter[1], cb[18] = float(rw), cb[19] = float(rh);
    std::memcpy(slot->mapped[0], cb, sizeof(cb));
    slot->set->setBuffer(kSetConstant, slot->constants[0].get(), kConstantBytes);
    list->setComputePipelineLayout(layout.get());
    list->setPipeline(pipelines[kMotionPass].pipeline.get());
    list->setComputeDescriptorSet(slot->set.get(), 0);
    list->dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
    // (FSR 2 reads it next)
    Transition(list, motion_out, RenderTextureLayout::SHADER_READ);
    Transition(list, motion_depth, RenderTextureLayout::SHADER_READ);
    motion_in.external = motion_out.external = motion_depth.external = nullptr;
    motion_used = used;
    return true;
  }

  bool Dispatch(plume::RenderCommandList* list, uint32_t frame_slot, Frame& f) override {
    if (failed || !context_made || frame_slot >= slots.size()) return false;
    record_slot = frame_slot;
    auto resource = [&](Texture& t, const wchar_t* name) {
      FfxResource r{};
      if (!t.texture) return r;
      r.resource = &t;
      std::wcsncpy(r.name, name, 63);
      r.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
      r.description.format = SurfaceFormat(t.format);
      r.description.width = t.width;
      r.description.height = t.height;
      r.description.depth = 1;
      r.description.mipCount = 1;
      r.state = FFX_RESOURCE_STATE_COMPUTE_READ;
      return r;
    };
    FfxFsr2DispatchDescription d{};
    d.commandList = list;
    d.color = resource(f.color, L"color");
    d.depth = resource(f.depth, L"depth");
    d.motionVectors = resource(f.motion, L"motion");
    d.reactive = resource(f.reactive, L"reactive");
    d.transparencyAndComposition = resource(f.composition, L"composition");
    d.output = resource(f.output, L"output");
    d.output.state = FFX_RESOURCE_STATE_UNORDERED_ACCESS;
    d.jitterOffset.x = f.jitter_x;
    d.jitterOffset.y = f.jitter_y;
    d.motionVectorScale.x = 1.0f;  // (render pixels)
    d.motionVectorScale.y = 1.0f;
    d.renderSize.width = f.render_w;
    d.renderSize.height = f.render_h;
    d.enableSharpening = f.sharpness > 0.0f;
    d.sharpness = std::clamp(f.sharpness, 0.0f, 1.0f);
    d.frameTimeDelta = f.frame_ms;
    d.preExposure = 1.0f;
    d.reset = f.reset;
    d.cameraNear = f.camera_near;
    d.cameraFar = f.camera_far;
    d.cameraFovAngleVertical = f.fov_y;
    d.viewSpaceToMetersFactor = 0.1f;  // (the game's units: 10 cm)
    const FfxErrorCode e = ffxFsr2ContextDispatch(&context, &d);
    if (e != FFX_OK) {
      static int logged = 0;
      if (logged++ < 5) REXLOG_WARN("native renderer: FSR 2 dispatch failed ({:#x})", uint32_t(e));
      jobs.clear();
      return false;
    }
    return true;
  }
};

Backend* Of(FfxFsr2Interface* i) { return *static_cast<Backend**>(i->scratchBuffer); }

FfxErrorCode CreateBackendContext(FfxFsr2Interface*, FfxDevice) { return FFX_OK; }
FfxErrorCode DestroyBackendContext(FfxFsr2Interface*) { return FFX_OK; }

FfxErrorCode GetDeviceCapabilities(FfxFsr2Interface*, FfxDeviceCapabilities* caps, FfxDevice) {
  caps->minimumSupportedShaderModel = FFX_SHADER_MODEL_6_2;
  caps->waveLaneCountMin = 32;
  caps->waveLaneCountMax = 64;
  caps->fp16Supported = false;
  caps->raytracingSupported = false;
  return FFX_OK;
}

FfxErrorCode CreateResource(FfxFsr2Interface* i, const FfxCreateResourceDescription* d, FfxResourceInternal* out) {
  Backend* b = Of(i);
  Resource r;
  r.desc = d->resourceDescription;
  r.format = Format(r.desc.format);
  if (r.format == RenderFormat::UNKNOWN || r.desc.type != FFX_RESOURCE_TYPE_TEXTURE2D) {
    REXLOG_ERROR("native renderer: FSR 2 resource {} unsupported", uint32_t(r.desc.format));
    return FFX_ERROR_INVALID_ARGUMENT;
  }
  uint32_t mips = r.desc.mipCount;
  if (!mips) {
    mips = 1;
    for (uint32_t s = std::max(r.desc.width, r.desc.height); s > 1; s >>= 1) ++mips;
  }
  r.mips = mips;
  r.desc.mipCount = mips;
  r.storage = (d->usage & FFX_RESOURCE_USAGE_UAV) != 0;
  plume::RenderTextureDesc td = plume::RenderTextureDesc::Texture2D(
      std::max(1u, r.desc.width), std::max(1u, r.desc.height), mips, r.format,
      r.storage ? plume::RenderTextureFlag::STORAGE | plume::RenderTextureFlag::UNORDERED_ACCESS
                : plume::RenderTextureFlag::NONE);
  r.owned = b->host.device->createTexture(td);
  if (!r.owned) return FFX_ERROR_OUT_OF_MEMORY;
  r.texture = r.owned.get();
  plume::RenderTextureViewDesc v = plume::RenderTextureViewDesc::Texture2D(r.format);
  v.mipLevels = mips;
  r.read = r.texture->createTextureView(v);
  r.write.resize(r.storage ? mips : 0);
  if (d->initData && d->initDataSize)
    r.init.assign(static_cast<const uint8_t*>(d->initData), static_cast<const uint8_t*>(d->initData) + d->initDataSize);
  out->internalIndex = int32_t(b->resources.size());
  b->resources.push_back(std::move(r));
  b->internal_count = b->resources.size();
  return FFX_OK;
}

FfxErrorCode RegisterResource(FfxFsr2Interface* i, const FfxResource* in, FfxResourceInternal* out) {
  Backend* b = Of(i);
  Texture* t = static_cast<Texture*>(in->resource);
  if (!t || !t->texture) {
    out->internalIndex = -1;  // (an optional input left out: FSR 2 uses its default)
    return FFX_OK;
  }
  Resource r;
  r.texture = t->texture;
  r.format = t->format;
  r.desc = in->description;
  r.layout = t->layout;
  r.external = t;
  r.storage = in->state == FFX_RESOURCE_STATE_UNORDERED_ACCESS;
  plume::RenderTextureViewDesc v = plume::RenderTextureViewDesc::Texture2D(r.format);
  v.mipLevels = 1;
  r.read = r.texture->createTextureView(v);
  r.write.resize(r.storage ? 1 : 0);
  out->internalIndex = int32_t(b->resources.size());
  b->resources.push_back(std::move(r));
  return FFX_OK;
}

FfxErrorCode UnregisterResources(FfxFsr2Interface* i) {
  Backend* b = Of(i);
  // (only after the jobs using them have been recorded: Execute calls this last)
  b->resources.resize(b->internal_count);
  return FFX_OK;
}

FfxResourceDescription GetResourceDescription(FfxFsr2Interface* i, FfxResourceInternal r) {
  Backend* b = Of(i);
  if (r.internalIndex < 0 || size_t(r.internalIndex) >= b->resources.size()) return {};
  return b->resources[size_t(r.internalIndex)].desc;
}

FfxErrorCode DestroyResource(FfxFsr2Interface* i, FfxResourceInternal r) {
  Backend* b = Of(i);
  if (r.internalIndex >= 0 && size_t(r.internalIndex) < b->resources.size()) {
    Resource& res = b->resources[size_t(r.internalIndex)];
    res.read.reset();
    res.write.clear();
    res.owned.reset();
    res.texture = nullptr;
  }
  return FFX_OK;
}

FfxErrorCode CreatePipeline(FfxFsr2Interface* i, FfxFsr2Pass pass, const FfxPipelineDescription*,
                            FfxPipelineState* out) {
  Backend* b = Of(i);
  std::memset(out, 0, sizeof(*out));
  if (pass < 0 || pass >= FFX_FSR2_PASS_COUNT) return FFX_ERROR_INVALID_ARGUMENT;
  const Fsr2Pass& p = kFsr2Passes[pass];
  // (the optional passes - reactive / composition from colour - aren't used: no shader needed)
  const bool needed = pass != FFX_FSR2_PASS_GENERATE_REACTIVE && pass != FFX_FSR2_PASS_TCR_AUTOGENERATE;
  if (!b->MakePipeline(pass, p.shader) && needed) return FFX_ERROR_BACKEND_API_ERROR;
  out->pipeline = reinterpret_cast<FfxPipeline>(intptr_t(pass) + 1);
  for (uint32_t k = 0; k < p.count; ++k) {
    const Fsr2Binding& bd = p.bindings[k];
    FfxResourceBinding* dst = nullptr;
    if (bd.kind == 't' && out->srvCount < FFX_MAX_NUM_SRVS) dst = &out->srvResourceBindings[out->srvCount++];
    if (bd.kind == 'u' && out->uavCount < FFX_MAX_NUM_UAVS) dst = &out->uavResourceBindings[out->uavCount++];
    if (bd.kind == 'b' && out->constCount < FFX_MAX_NUM_CONST_BUFFERS) dst = &out->cbResourceBindings[out->constCount++];
    if (!dst) continue;
    dst->slotIndex = bd.reg;
    std::wcsncpy(dst->name, bd.name, 63);
  }
  return FFX_OK;
}

FfxErrorCode DestroyPipeline(FfxFsr2Interface*, FfxPipelineState*) { return FFX_OK; }

FfxErrorCode ScheduleGpuJob(FfxFsr2Interface* i, const FfxGpuJobDescription* job) {
  Of(i)->jobs.push_back(*job);
  return FFX_OK;
}

FfxErrorCode ExecuteGpuJobs(FfxFsr2Interface* i, FfxCommandList list) {
  Backend* b = Of(i);
  return b->Record(static_cast<plume::RenderCommandList*>(list), b->record_slot) ? FFX_OK
                                                                                   : FFX_ERROR_BACKEND_API_ERROR;
}

}  // namespace

std::unique_ptr<Upscaler> Create(const Host& host, uint32_t max_render_w, uint32_t max_render_h, uint32_t display_w,
                                 uint32_t display_h, uint32_t frames_in_flight) {
  auto b = std::make_unique<Backend>();
  b->host = host;
  b->render_w = max_render_w, b->render_h = max_render_h, b->display_w = display_w, b->display_h = display_h;
  b->slots.resize(std::max(1u, frames_in_flight));
  b->uploads.resize(b->slots.size());
  if (!host.device || !b->MakeLayout() || !b->MakePipeline(kClearPass, "fsr2_clear.cs")) {
    REXLOG_WARN("native renderer: FSR 2 not available here");
    return nullptr;
  }
  b->scratch.resize(sizeof(Backend*));
  Backend* self = b.get();
  std::memcpy(b->scratch.data(), &self, sizeof(self));
  FfxFsr2ContextDescription d{};
  d.flags = FFX_FSR2_ENABLE_AUTO_EXPOSURE;  // (SDR colour, depth 0 near, render-size motion vectors)
  d.maxRenderSize = {max_render_w, max_render_h};
  d.displaySize = {display_w, display_h};
  d.callbacks.fpCreateBackendContext = CreateBackendContext;
  d.callbacks.fpGetDeviceCapabilities = GetDeviceCapabilities;
  d.callbacks.fpDestroyBackendContext = DestroyBackendContext;
  d.callbacks.fpCreateResource = CreateResource;
  d.callbacks.fpRegisterResource = RegisterResource;
  d.callbacks.fpUnregisterResources = UnregisterResources;
  d.callbacks.fpGetResourceDescription = GetResourceDescription;
  d.callbacks.fpDestroyResource = DestroyResource;
  d.callbacks.fpCreatePipeline = CreatePipeline;
  d.callbacks.fpDestroyPipeline = DestroyPipeline;
  d.callbacks.fpScheduleGpuJob = ScheduleGpuJob;
  d.callbacks.fpExecuteGpuJobs = ExecuteGpuJobs;
  d.callbacks.scratchBuffer = b->scratch.data();
  d.callbacks.scratchBufferSize = b->scratch.size();
  d.device = host.device;
  const FfxErrorCode e = ffxFsr2ContextCreate(&b->context, &d);
  if (e != FFX_OK) {
    REXLOG_WARN("native renderer: FSR 2 context failed ({:#x})", uint32_t(e));
    return nullptr;
  }
  b->context_made = true;
  REXLOG_INFO("native renderer: FSR 2 ready, {}x{} -> {}x{} ({} resources)", max_render_w, max_render_h, display_w,
              display_h, b->resources.size());
  return b;
}

}  // namespace svr2011::fsr2
