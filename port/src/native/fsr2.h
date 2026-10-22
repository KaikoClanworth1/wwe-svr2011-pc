// WWE SmackDown vs. Raw 2011 - the native renderer's FSR 2 upscaling (experimental, Direct3D 12 / Vulkan):
// AMD FidelityFX Super Resolution 2.2.1 (port/third_party/ffx-fsr2, MIT) on its own backend over plume - the
// passes from tools/fsr2_shaders.py (fsr2_<pass>.cs in the shader packs), one descriptor set per dispatch.
//
// The renderer gives it, each frame, the scene smaller than the window (colour, a depth copy, motion vectors in
// render pixels, the reactive / composition masks) and the frame's jitter; it writes the window-size scene.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <plume_render_interface.h>

namespace svr2011::fsr2 {

struct Host {
  plume::RenderDevice* device = nullptr;
  plume::RenderShaderFormat format = plume::RenderShaderFormat::UNKNOWN;
  bool vulkan = false;  // (the register kinds apart in one set: fsr2_shaders.py's VK_SHIFT)
  // A shader's code for this API by name ("fsr2_accumulate.cs"); empty if missing.
  std::function<std::vector<uint8_t>(const std::string&)> read_shader;
};

// A texture the renderer owns, in `layout` when given; FSR 2 leaves it in the layout it ends in (Dispatch's
// `layouts_after`).
struct Texture {
  plume::RenderTexture* texture = nullptr;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  uint32_t width = 0, height = 0;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
};

struct Frame {
  Texture color;       // the scene (render size, upper-left render_w x render_h used)
  Texture depth;       // R32_FLOAT: device depth, 0 near
  Texture motion;      // R16G16_FLOAT: where this pixel was in the last frame, render pixels (previous - current)
  Texture reactive;    // R8_UNORM or none: 1 = trust the history less (particles, blended)
  Texture composition; // R8_UNORM or none: moving without motion vectors (the wrestlers)
  Texture output;      // the window-size result (a storage texture)
  uint32_t render_w = 0, render_h = 0;
  float jitter_x = 0, jitter_y = 0;  // render pixels (g_jitter)
  float frame_ms = 16.7f;
  float camera_near = 1.0f, camera_far = 20000.0f, fov_y = 0.5f;
  float sharpness = 0.8f;
  bool reset = false;
};

class Upscaler {
 public:
  virtual ~Upscaler() = default;
  // Records FSR 2 into `list` (frame slot `slot` of the renderer's frames in flight); false if it couldn't.
  // Every texture of `frame` ends in the layout written back into it.
  virtual bool Dispatch(plume::RenderCommandList* list, uint32_t slot, Frame& frame) = 0;
  // The camera's motion vectors into `motion` (a storage texture, R16G16_FLOAT) from `depth`: `reproject` (row
  // vectors) takes this frame's clip space, unjittered, to the last frame's; `jitter` this frame's in clip space
  // (FSR 2's Frame::jitter_x * 2 / width, -jitter_y * 2 / height). Recorded before Dispatch, same slot.
  // `depth_out` (a storage texture, R32_FLOAT, the depth's size): the depth copied into it (DLSS's input).
  virtual bool Motion(plume::RenderCommandList* list, uint32_t slot, Texture& depth, Texture& motion,
                      Texture& depth_out, const float reproject[16], const float jitter[2], uint32_t render_w,
                      uint32_t render_h) = 0;
  virtual uint32_t MaxRenderWidth() const = 0;
  virtual uint32_t MaxRenderHeight() const = 0;
  virtual uint32_t DisplayWidth() const = 0;
  virtual uint32_t DisplayHeight() const = 0;
};

// Null (logged) if FSR 2 can't run here: no compute (Direct3D 11), shaders missing, a pipeline failed.
std::unique_ptr<Upscaler> Create(const Host& host, uint32_t max_render_w, uint32_t max_render_h, uint32_t display_w,
                                 uint32_t display_h, uint32_t frames_in_flight);

}  // namespace svr2011::fsr2
