// WWE SmackDown vs. Raw 2011 - the native renderer's NVIDIA DLSS (experimental: NVIDIA RTX GPUs, Direct3D 12 /
// Vulkan). Built only with the DLSS SDK (CMake SVR2011_DLSS_SDK - outside the repository, NVIDIA RTX SDK licence);
// nvngx_dlss.dll beside the exe (the release zip carries it).
//
// It takes FSR 2's inputs (fsr2.h Frame: the scene colour, the depth as R32_FLOAT, the camera's motion vectors in
// render pixels, the reactive mask as its "bias current colour" mask, the jitter) and writes the window-size scene.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "native/fsr2.h"

namespace svr2011::dlss {

struct Host {
  plume::RenderDevice* device = nullptr;
  bool vulkan = false;
  std::wstring data_path;  // (NGX's logs and caches: UserData\cache)
  std::wstring dll_dir;    // (where nvngx_dlss.dll is: the exe's folder)
};

// Whether DLSS can run on this device (NGX started, the GPU and driver support DLSS); `why` when not. Once per
// device; the render thread, after the device is made.
bool Probe(const Host& host, std::string* why);

class Upscaler {
 public:
  virtual ~Upscaler() = default;
  // Records DLSS into `list`; false if it couldn't. `frame.depth`: R32_FLOAT. Textures are in SHADER_READ (the
  // output in GENERAL) before and after.
  virtual bool Dispatch(plume::RenderCommandList* list, fsr2::Frame& frame) = 0;
  virtual uint32_t MaxRenderWidth() const = 0;
  virtual uint32_t MaxRenderHeight() const = 0;
  virtual uint32_t DisplayWidth() const = 0;
  virtual uint32_t DisplayHeight() const = 0;
  virtual int Quality() const = 0;
};

// `quality`: native_upscale_quality (0 ultra quality .. 3 performance). Null if DLSS can't run (Probe first).
std::unique_ptr<Upscaler> Create(const Host& host, uint32_t max_render_w, uint32_t max_render_h, uint32_t display_w,
                                 uint32_t display_h, int quality);

}  // namespace svr2011::dlss
