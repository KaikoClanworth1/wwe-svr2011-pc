// WWE SmackDown vs. Raw 2011 - the native renderer's NVIDIA DLSS (see dlss.h): NGX on the renderer's plume device.
//
// Direct3D 12: NGX records into the frame's command list with the D3D12 resources behind plume's textures; plume's
// cached command list state (descriptor heaps, layouts, pipeline, framebuffer) is forgotten after it, as NGX sets
// its own. The colour is copied into a typed texture first (the resolve copies are typeless).

#include "native/dlss.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <filesystem>
#include <mutex>
#include <vector>

#include <rex/logging.h>

#include <windows.h>
#include <d3d12.h>
#include <plume_d3d12.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>

namespace svr2011::dlss {
namespace {

using plume::RenderFormat;
using plume::RenderTextureLayout;

// (NGX's project id for a custom engine: any fixed UUID)
constexpr const char* kProjectId = "6b3e2a10-5c4d-4e8f-9a21-73d0c5e12011";

std::mutex g_mutex;
bool g_probed = false, g_ok = false;
std::string g_why;
ID3D12Device* g_d3d = nullptr;
NVSDK_NGX_Parameter* g_caps = nullptr;

std::string Hex(NVSDK_NGX_Result r) { return fmt::format("{:#x}", uint32_t(r)); }

void NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  std::string m = message ? message : "";
  while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
  REXLOG_INFO("NGX: {}", m);
}

ID3D12Resource* D3d(plume::RenderTexture* t) { return t ? static_cast<plume::D3D12Texture*>(t)->d3d : nullptr; }

NVSDK_NGX_PerfQuality_Value QualityValue(int quality) {
  switch (quality) {
    case 0: return NVSDK_NGX_PerfQuality_Value_UltraQuality;
    case 1: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case 2: return NVSDK_NGX_PerfQuality_Value_Balanced;
    default: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
  }
}

class D3D12Dlss final : public Upscaler {
 public:
  Host host;
  uint32_t render_w = 0, render_h = 0, display_w = 0, display_h = 0;
  int quality = 1;
  NVSDK_NGX_Parameter* params = nullptr;
  NVSDK_NGX_Handle* handle = nullptr;
  bool failed = false;
  std::unique_ptr<plume::RenderTexture> color;  // (the typed copy)
  uint32_t color_w = 0, color_h = 0;
  RenderFormat color_format = RenderFormat::UNKNOWN;

  ~D3D12Dlss() override {
    if (handle) NVSDK_NGX_D3D12_ReleaseFeature(handle);
    if (params) NVSDK_NGX_D3D12_DestroyParameters(params);
  }

  uint32_t MaxRenderWidth() const override { return render_w; }
  uint32_t MaxRenderHeight() const override { return render_h; }
  uint32_t DisplayWidth() const override { return display_w; }
  uint32_t DisplayHeight() const override { return display_h; }
  int Quality() const override { return quality; }

  static void Barrier(plume::RenderCommandList* list, plume::RenderTexture* t, RenderTextureLayout layout) {
    if (!t) return;
    const plume::RenderTextureBarrier b(t, layout);
    list->barriers(plume::RenderBarrierStage::COMPUTE | plume::RenderBarrierStage::COPY, nullptr, 0, &b, 1);
  }

  // (NGX set its own heaps, root signature and pipeline: plume binds its own again)
  static void ForgetState(plume::RenderCommandList* list) {
    auto* l = static_cast<plume::D3D12CommandList*>(list);
    l->descriptorHeapsSet = false;
    l->activeComputePipelineLayout = nullptr;
    l->activeGraphicsPipelineLayout = nullptr;
    l->activeGraphicsPipeline = nullptr;
    l->activeTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    l->targetFramebuffer = nullptr;
  }

  bool Dispatch(plume::RenderCommandList* list, fsr2::Frame& f) override {
    if (failed || !f.color.texture || !f.depth.texture || !f.motion.texture || !f.output.texture) return false;
    auto* cl = static_cast<plume::D3D12CommandList*>(list)->d3d;
    if (!handle) {
      NVSDK_NGX_DLSS_Create_Params c{};
      c.Feature.InWidth = render_w;
      c.Feature.InHeight = render_h;
      c.Feature.InTargetWidth = display_w;
      c.Feature.InTargetHeight = display_h;
      c.Feature.InPerfQualityValue = QualityValue(quality);
      // (SDR colour, render-size motion vectors without the jitter, depth 0 near)
      c.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
      // DLSS 4: the transformer model (preset K) for every quality level.
      for (const char* p : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance})
        NVSDK_NGX_Parameter_SetUI(params, p, NVSDK_NGX_DLSS_Hint_Render_Preset_K);
      const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(cl, 1, 1, &handle, params, &c);
      ForgetState(list);
      if (NVSDK_NGX_FAILED(r) || !handle) {
        REXLOG_WARN("native renderer: DLSS feature not made ({}) - FSR 1 instead", Hex(r));
        failed = true;
        return false;
      }
      REXLOG_INFO("native renderer: DLSS ready, {}x{} -> {}x{} (quality {}, preset K)", render_w, render_h, display_w,
                  display_h, quality);
    }
    // The colour in a typed texture of its own (the resolve copy is typeless).
    if (!color || color_w != f.color.width || color_h != f.color.height || color_format != f.color.format) {
      color = host.device->createTexture(
          plume::RenderTextureDesc::Texture2D(f.color.width, f.color.height, 1, f.color.format));
      if (!color) return false;
      color->setName("DLSS colour");
      color_w = f.color.width, color_h = f.color.height, color_format = f.color.format;
    }
    Barrier(list, f.color.texture, RenderTextureLayout::COPY_SOURCE);
    Barrier(list, color.get(), RenderTextureLayout::COPY_DEST);
    list->copyTexture(color.get(), f.color.texture);
    Barrier(list, color.get(), RenderTextureLayout::SHADER_READ);
    Barrier(list, f.color.texture, RenderTextureLayout::SHADER_READ);
    Barrier(list, f.depth.texture, RenderTextureLayout::SHADER_READ);
    Barrier(list, f.motion.texture, RenderTextureLayout::SHADER_READ);
    if (f.reactive.texture) Barrier(list, f.reactive.texture, RenderTextureLayout::SHADER_READ);
    Barrier(list, f.output.texture, RenderTextureLayout::GENERAL);
    NVSDK_NGX_D3D12_DLSS_Eval_Params e{};
    e.Feature.pInColor = D3d(color.get());
    e.Feature.pInOutput = D3d(f.output.texture);
    e.pInDepth = D3d(f.depth.texture);
    e.pInMotionVectors = D3d(f.motion.texture);
    // The jitter as FSR 2's (render pixels, y down): compared on a still camera with all four signs against a 3x
    // reference - any other leaves stairs along the edges.
    // (test aid SVR2011_TEST_DLSS_JITTER=<x sign>,<y sign>: other signs, to compare)
    static const std::pair<float, float> jitter_sign = [] {
      std::pair<float, float> v{1.0f, 1.0f};
      if (const char* e = std::getenv("SVR2011_TEST_DLSS_JITTER")) std::sscanf(e, "%f,%f", &v.first, &v.second);
      return v;
    }();
    e.InJitterOffsetX = jitter_sign.first * f.jitter_x;
    e.InJitterOffsetY = jitter_sign.second * f.jitter_y;
    e.InRenderSubrectDimensions = {f.render_w, f.render_h};
    e.InReset = f.reset ? 1 : 0;
    e.InMVScaleX = 1.0f;
    e.InMVScaleY = 1.0f;
    e.pInBiasCurrentColorMask = D3d(f.reactive.texture);
    e.InPreExposure = 1.0f;
    e.InExposureScale = 1.0f;
    e.InFrameTimeDeltaInMsec = f.frame_ms;
    const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(cl, handle, params, &e);
    ForgetState(list);
    Barrier(list, f.output.texture, RenderTextureLayout::SHADER_READ);
    if (NVSDK_NGX_FAILED(r)) {
      static int logged = 0;
      if (logged++ < 5) REXLOG_WARN("native renderer: DLSS evaluation failed ({})", Hex(r));
      return false;
    }
    return true;
  }
};

}  // namespace

bool Probe(const Host& host, std::string* why) {
  std::lock_guard lock(g_mutex);
  if (!g_probed) {
    g_probed = true;
    std::error_code ec;
    if (host.vulkan) {
      g_why = "DLSS on Vulkan isn't in this build yet (Direct3D 12 only)";
    } else if (!host.device) {
      g_why = "no device";
    } else if (!std::filesystem::exists(std::filesystem::path(host.dll_dir) / L"nvngx_dlss.dll", ec)) {
      g_why = "nvngx_dlss.dll is not beside the game";
    } else {
      g_d3d = static_cast<plume::D3D12Device*>(host.device)->d3d;
      NVSDK_NGX_FeatureCommonInfo info{};
      const wchar_t* paths[1] = {host.dll_dir.c_str()};
      info.PathListInfo.Path = paths;
      info.PathListInfo.Length = 1;
      info.LoggingInfo.LoggingCallback = NgxLog;
      info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
      std::filesystem::create_directories(host.data_path, ec);
      NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "2.1",
                                                               host.data_path.c_str(), g_d3d, &info);
      if (NVSDK_NGX_FAILED(r)) {
        g_why = "NVIDIA NGX didn't start (" + Hex(r) + ": not an NVIDIA GPU, or its driver is too old)";
      } else if (NVSDK_NGX_FAILED(r = NVSDK_NGX_D3D12_GetCapabilityParameters(&g_caps)) || !g_caps) {
        g_why = "NGX capabilities unreadable (" + Hex(r) + ")";
      } else {
        int available = 0, needs_driver = 0;
        unsigned int major = 0, minor = 0;
        NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
        NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
        NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
        if (needs_driver)
          g_why = fmt::format("the NVIDIA driver is too old for DLSS (needs {}.{} or newer)", major, minor);
        else if (!available)
          g_why = "this GPU has no DLSS (NVIDIA RTX GPUs only)";
        else
          g_ok = true;
      }
      if (!g_ok) {
        if (g_caps) NVSDK_NGX_D3D12_DestroyParameters(g_caps);
        g_caps = nullptr;
        NVSDK_NGX_D3D12_Shutdown1(g_d3d);
      }
    }
    if (g_ok)
      REXLOG_INFO("native renderer: DLSS available (NVIDIA NGX, Direct3D 12)");
    else
      REXLOG_INFO("native renderer: DLSS not available - {}", g_why);
  }
  if (why) *why = g_why;
  return g_ok;
}

std::unique_ptr<Upscaler> Create(const Host& host, uint32_t max_render_w, uint32_t max_render_h, uint32_t display_w,
                                 uint32_t display_h, int quality) {
  if (!Probe(host, nullptr) || host.vulkan) return nullptr;
  auto d = std::make_unique<D3D12Dlss>();
  d->host = host;
  d->render_w = max_render_w, d->render_h = max_render_h, d->display_w = display_w, d->display_h = display_h;
  d->quality = std::clamp(quality, 0, 3);
  if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&d->params)) || !d->params) return nullptr;
  return d;
}

}  // namespace svr2011::dlss
