// WWE SmackDown vs. Raw 2011 - native renderer (see native_renderer.h).
//
// Every draw is made with the game's own (converted) shaders, vertex/index data,
// constants and render state, into host render targets standing in for the
// EDRAM surfaces; resolves copy them into the textures the game samples
// (textures.cpp loads the rest from guest memory), and the front buffer is
// shown at Present.

#include "frame_rate.h"
#include "native/native_renderer.h"

#include <algorithm>
#include <array>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#include <fmt/format.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <tmmintrin.h>
#else
// (ARM: the SSSE3 shuffle through SIMDe, as the recompiled code does)
#define SIMDE_ENABLE_NATIVE_ALIASES
#include <simde/x86/ssse3.h>
#endif

#include <plume_render_interface.h>

#include <rex/cvar.h>
#include <rex/external_frame.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

#include "crash_report.h"
#include "native/gpu.h"
#include "native/recording_list.h"
#include "native/guest_d3d.h"
#include "native/textures.h"

// Default "main" (as the launcher): settings files without the key use it.
#if defined(__ANDROID__)
constexpr bool kScaleEffectsDefault = false;  // (phones: shadows and effects at the console's size)
#else
constexpr bool kScaleEffectsDefault = true;
#endif
REXCVAR_DEFINE_BOOL(native_scale_effects, kScaleEffectsDefault, "GPU",
                    "Native renderer: render shadows, reflections and glow at the render scale too "
                    "(false: at the Xbox 360's size, faster on weak GPUs)");

REXCVAR_DEFINE_BOOL(native_shadows, true, "GPU",
                    "Native renderer: the characters' and the arena's real-time shadows (false: nothing "
                    "is drawn into the shadow map - no shadows, fewer draws). Applies at once.");

// On by default only on phones, where the driver's command recording is a
// large part of the render thread; on a PC (D3D12, Batista's entrance at
// 5,400 draws) handing the commands over cost more than recording them.
#if defined(__ANDROID__)
constexpr bool kRecordThreadDefault = true;
#else
constexpr bool kRecordThreadDefault = false;
#endif
REXCVAR_DEFINE_BOOL(native_record_thread, kRecordThreadDefault, "GPU",
                    "Native renderer: record the GPU commands on a second thread (recording_list.h), "
                    "off the game's render thread");

REXCVAR_DEFINE_BOOL(native_widescreen, true, "GPU",
                    "Native renderer: matches as wide as a wider-than-16:9 window (the HUD and "
                    "menus stay 16:9)");

REXCVAR_DEFINE_BOOL(native_prepare_pipelines, true, "GPU",
                    "Native renderer: build the known pipelines in the background while in the menus, "
                    "so scenes don't stutter compiling them");

REXCVAR_DEFINE_BOOL(native_resolve_write_back, true, "GPU",
                    "Native renderer: copy one-off render-to-texture results back to the game's memory "
                    "(Superstar Threads attire baking reads them)");

REXCVAR_DEFINE_BOOL(native_constant_check, false, "GPU",
                    "Debug: also hash the shader constants and log when a reused upload was "
                    "stale (checks the D3D dirty tracking the native renderer relies on)");

REXCVAR_DEFINE_INT32(native_aa, 0, "GPU",
                     "Native renderer anti-aliasing (supersampling): 1 off, 2 / 3 / 4 = that many times the "
                     "screen's resolution per side (4 / 9 / 16 samples a pixel, within native_max_scale); "
                     "0: native_2x_msaa decides (on = 2)");

REXCVAR_DEFINE_DOUBLE(native_render_scale, 1.0, "GPU",
                      "Native renderer: the scene's resolution below the Xbox 360's 720p for weak GPUs "
                      "(0.5 = 640 x 360, scaled up to the screen; 0.25 - 1). The HUD and menus' 2D art "
                      "are drawn into the same scene, so they get softer too.");

REXCVAR_DEFINE_INT32(native_max_scale, 4, "GPU",
                     "Native renderer: the largest render scale (1 = the Xbox 360's 720p, up to 4). "
                     "The scale follows the window; phones start at 1.");

REXCVAR_DEFINE_STRING(native_texture_quality, "high", "GPU",
                      "Native renderer texture quality: high - as the game has them; medium / low - big "
                      "mipmapped textures start at the game's own half / quarter size mipmap (a quarter / "
                      "a sixteenth of the memory and upload); menus, fonts and pictures stay sharp")
    .allowed({"high", "medium", "low"});

REXCVAR_DEFINE_STRING(native_renderer, "main", "GPU",
                      "Native renderer: main - the game always draws with it (the emulator only "
                      "runs the GPU command stream). (off and shadow are old values: they mean main.)")
    .allowed({"off", "main", "shadow"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace texture_util = rex::graphics::texture_util;

namespace plume {
extern std::atomic<uint32_t> g_svr_render_passes;  // (plume_vulkan.cpp: render passes begun)
extern std::atomic<uint64_t> g_svr_render_pass_pixels;
}  // namespace plume

namespace svr2011::native {

using plume::RenderFormat;
using plume::RenderTextureLayout;
namespace RenderBarrierStage = plume::RenderBarrierStage;

namespace {

// ---------------------------------------------------------------------------
// constants and small helpers

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr uint32_t kFrames = 3;                   // frames in flight
// Frames are drawn into these and handed to the emulator's presentation;
// enough that one is never redrawn while the emulator's queue may still
// read it.
constexpr uint32_t kOutputs = 6;
constexpr uint64_t kRingSize = 64ull << 20;       // per-frame upload ring
// Mapped past the ring's end: shaders indexing constants may read up to a
// constant buffer view's 64 KB past an allocation near the end.
constexpr uint64_t kRingSlack = 64ull << 10;
constexpr uint32_t kSrvHeapSize = 16384;   // per texture table: 0-2 placeholders, then textures
constexpr uint32_t kSamplerHeapSize = 2000;  // 0 default linear wrap, then per fetch state
constexpr uint32_t kMaxColorTargets = 256;
constexpr uint32_t kMaxDepthTargets = 64;
constexpr uint32_t kGpuTimeoutMs = 3000;

// Shader constant sizes (XenosRecomp contract).
constexpr uint32_t kVertexConstantsBytes = 256 * 16;
constexpr uint32_t kPixelConstantsBytes = 256 * 16;  // all 256 (the skin shaders use c224+)
constexpr uint32_t kSharedConstantsBytes = 34 * 16;

inline uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline float BeFloat(const uint8_t* p) {
  const uint32_t u = Be32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// Byte-swapped copies of 32-bit / 16-bit words, 16 bytes at a time (NEON's
// vrev on ARM - SIMDe's emulated SSSE3 shuffle cost ~3% of the phone's render
// thread - and SSSE3 on x86). Whole 16-byte stores also suit the upload
// ring, which on phones is uncached memory.
void Swap32(uint8_t* dst, const uint8_t* src, size_t bytes) {
  size_t i = 0;
#if defined(__aarch64__)
  for (; i + 16 <= bytes; i += 16) vst1q_u8(dst + i, vrev32q_u8(vld1q_u8(src + i)));
#else
  const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  for (; i + 16 <= bytes; i += 16) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), _mm_shuffle_epi8(v, swap));
  }
#endif
  for (; i + 4 <= bytes; i += 4) {
    uint32_t w;
    std::memcpy(&w, src + i, 4);
    w = _byteswap_ulong(w);
    std::memcpy(dst + i, &w, 4);
  }
}

void Swap16(uint8_t* dst, const uint8_t* src, size_t bytes) {
  size_t i = 0;
#if defined(__aarch64__)
  for (; i + 16 <= bytes; i += 16) vst1q_u8(dst + i, vrev16q_u8(vld1q_u8(src + i)));
#else
  const __m128i swap = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
  for (; i + 16 <= bytes; i += 16) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), _mm_shuffle_epi8(v, swap));
  }
#endif
  for (; i + 2 <= bytes; i += 2) {
    dst[i] = src[i + 1];
    dst[i + 1] = src[i];
  }
}

uint64_t Fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// ---------------------------------------------------------------------------
// guest memory

rex::memory::Memory* g_memory = nullptr;
// TranslateVirtual without its out-of-line heap lookup (~3% of a phone's
// render thread in heavy entrances): the host offset of each 1 MB of the
// guest address space (the heaps' bounds are 1 MB multiples), set at Attach.
uint8_t* g_virtual_base = nullptr;
uint32_t g_virtual_offset[4096];

inline const uint8_t* Virtual(uint32_t address) {
  return g_virtual_base + address + g_virtual_offset[address >> 20];
}
const uint8_t* Physical(uint32_t address) { return g_memory->TranslatePhysical(address); }
// Addresses stored in the game's D3D objects (vertex/index buffers, textures)
// are guest *virtual* (0xA0000000+ / 0xE0000000+ physical-memory views); the
// 0xE... view is offset by a page on the host, so they must be translated as
// virtual. The fetch constants in the device's register mirror hold true
// physical addresses instead (Physical()).
const uint8_t* ObjectData(uint32_t address) { return Virtual(address); }
const uint8_t* Device() { return Virtual(guest::kDeviceAddress); }
uint32_t Reg(uint32_t reg) { return Be32(Device() + guest::RegisterOffset(reg)); }
float RegFloat(uint32_t reg) { return BeFloat(Device() + guest::RegisterOffset(reg)); }

// Xenos register numbers used here.
enum : uint32_t {
  RB_SURFACE_INFO = 0x2000,
  PA_SC_WINDOW_SCISSOR_TL = 0x2011,  // (D3D's mirror slots; see Draw)
  PA_SC_WINDOW_SCISSOR_BR = 0x2012,
  RB_COLOR_INFO = 0x2001,
  RB_DEPTH_INFO = 0x2002,
  RB_COLOR_MASK = 0x2104,
  RB_BLEND_RED = 0x2105,
  RB_STENCILREFMASK = 0x210D,
  RB_ALPHA_REF = 0x210E,
  PA_CL_VPORT_XSCALE = 0x210F,
  PA_CL_VPORT_XOFFSET = 0x2110,
  PA_CL_VPORT_YSCALE = 0x2111,
  PA_CL_VPORT_YOFFSET = 0x2112,
  PA_CL_VPORT_ZSCALE = 0x2113,
  PA_CL_VPORT_ZOFFSET = 0x2114,
  RB_DEPTHCONTROL = 0x2200,
  RB_BLENDCONTROL0 = 0x2201,
  RB_COLORCONTROL = 0x2202,
  PA_SU_SC_MODE_CNTL = 0x2205,
  PA_CL_VTE_CNTL = 0x2206,
  PA_SU_VTX_CNTL = 0x2302,
  PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380,  // then FRONT_OFFSET, BACK_SCALE, BACK_OFFSET
  RB_DEPTH_CLEAR = 0x231D,
  RB_COLOR_CLEAR = 0x231E,
};

// ---------------------------------------------------------------------------
// shaders

struct ShaderInput {
  std::string semantic;
  uint32_t index;
  bool is_uint;
  int location;  // Vulkan input location (-1: in declaration order)
};

struct Shader {
  uint64_t hash = 0;
  bool pixel = false;
  bool loaded = false;
  bool missing = false;
  std::vector<uint8_t> code[3];  // [0] base, [1] packed normals (vs), [2] alpha test (ps)
  std::unique_ptr<plume::RenderShader> compiled[3];
  std::vector<ShaderInput> inputs;
  std::vector<std::pair<uint32_t, uint32_t>> textures;  // (fetch slot, 0 2D / 1 3D / 2 cube)
};

std::mutex g_shader_mutex;
std::unordered_map<uint32_t, Shader> g_shaders;  // guest shader object -> shader

std::filesystem::path ShaderDirectory() {
  char* v = nullptr;
  size_t n = 0;
  std::filesystem::path dir;
  if (_dupenv_s(&v, &n, "SVR2011_NATIVE_SHADERS") == 0 && v) {
    dir = v;
    free(v);
  } else {
    dir = rex::filesystem::GetExecutableFolder() / "native_shaders";
  }
  return dir;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// A converted shader: "<name><backend extension>". Installed, the DXIL and
// SPIR-V files share native_shaders\; in the converter's output (a dev
// SVR2011_NATIVE_SHADERS pointing at .../dxil) SPIR-V is in ../spirv.
std::filesystem::path ShaderFile(const std::string& name) {
  static const std::filesystem::path dir = ShaderDirectory();
  const std::string file = name + backend::ShaderExtension();
  std::error_code ec;
  if (!std::filesystem::exists(dir / file, ec) && dir.filename() == "dxil")
    return dir.parent_path() / "spirv" / file;
  return dir / file;
}

// The shaders' folder packed in one file (tools/pack_shaders.py):
// native_shaders\shaders<extension>.pak - read once at start instead of a
// thousand small files (on a hard disk each one is a seek: a new scene's
// shaders froze it for hundreds of ms). "SVRPAK1\0", u32 count, then per
// file u16 name length, name, u32 offset, u32 size; then the files.
// Without one (a dev shader folder), the loose files.
struct ShaderPack {
  std::vector<uint8_t> data;
  std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> files;  // name -> offset, size
};

const ShaderPack& Pack() {
  static const ShaderPack pack = [] {
    ShaderPack p;
    const std::filesystem::path path =
        ShaderDirectory() / (std::string("shaders") + backend::ShaderExtension() + ".pak");
    p.data = ReadFile(path);
    if (p.data.empty()) return p;
    auto u16 = [&](size_t at) { return uint32_t(p.data[at]) | uint32_t(p.data[at + 1]) << 8; };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    bool ok = p.data.size() >= 12 && std::memcmp(p.data.data(), "SVRPAK1", 8) == 0;
    size_t at = 12;
    for (uint32_t i = 0, n = ok ? u32(8) : 0; ok && i < n; ++i) {
      if (at + 2 > p.data.size()) { ok = false; break; }
      const uint32_t len = u16(at);
      if (at + 2 + len + 8 > p.data.size()) { ok = false; break; }
      std::string name(reinterpret_cast<const char*>(&p.data[at + 2]), len);
      const uint32_t offset = u32(at + 2 + len), size = u32(at + 6 + len);
      if (uint64_t(offset) + size > p.data.size()) { ok = false; break; }
      p.files.emplace(std::move(name), std::make_pair(offset, size));
      at += 2 + len + 8;
    }
    if (!ok) {
      REXLOG_WARN("native renderer: {} is damaged - using the loose shader files", path.string());
      p.files.clear();
      p.data.clear();
    } else {
      REXLOG_INFO("native renderer: shader pack {} ({} files, {} KB)", path.filename().string(), p.files.size(),
                  p.data.size() / 1024);
    }
    return p;
  }();
  return pack;
}

// A file of the shaders' folder by name ("<hash>.vs.inputs").
std::vector<uint8_t> ShaderFolderFile(const std::string& file) {
  const ShaderPack& pack = Pack();
  if (auto it = pack.files.find(file); it != pack.files.end()) {
    const uint8_t* b = pack.data.data() + it->second.first;
    return std::vector<uint8_t>(b, b + it->second.second);
  }
  static const std::filesystem::path dir = ShaderDirectory();
  return ReadFile(dir / file);
}

// A converted shader ("<name>" + the backend's extension).
std::vector<uint8_t> ReadShader(const std::string& name) {
  const ShaderPack& pack = Pack();
  if (auto it = pack.files.find(name + backend::ShaderExtension()); it != pack.files.end()) {
    const uint8_t* b = pack.data.data() + it->second.first;
    return std::vector<uint8_t>(b, b + it->second.second);
  }
  return ReadFile(ShaderFile(name));
}

bool ShaderExists(const std::string& name) {
  if (Pack().files.count(name + backend::ShaderExtension())) return true;
  std::error_code ec;
  return std::filesystem::exists(ShaderFile(name), ec);
}

void LoadShader(Shader& s) {
  if (s.loaded) return;
  s.loaded = true;
  char name[64];
  std::snprintf(name, sizeof(name), "%016llX.%s", static_cast<unsigned long long>(s.hash),
                s.pixel ? "ps" : "vs");
  const std::string ext = backend::ShaderExtension();
  s.code[0] = ReadShader(name);
  s.code[s.pixel ? 2 : 1] = ReadShader(std::string(name) + (s.pixel ? ".s2" : ".s1"));
  auto text = [](const std::vector<uint8_t>& b) { return std::string(b.begin(), b.end()); };
  if (!s.pixel) {
    // SEMANTIC INDEX TYPE [LOCATION]
    std::istringstream in(text(ShaderFolderFile(std::string(name) + ".inputs")));
    std::string line, type;
    while (std::getline(in, line)) {
      std::istringstream fields(line);
      ShaderInput i;
      if (!(fields >> i.semantic >> i.index >> type)) continue;
      i.is_uint = type == "uint4";
      if (!(fields >> i.location)) i.location = -1;
      s.inputs.push_back(i);
    }
  }
  {
    std::istringstream in(text(ShaderFolderFile(std::string(name) + ".textures")));
    uint32_t slot, dimension;
    while (in >> slot >> dimension) {
      if (slot < 32 && dimension < 3) s.textures.emplace_back(slot, dimension);
    }
  }
  s.missing = s.code[0].empty();
  if (s.missing) REXLOG_WARN("native renderer: no converted shader {}{}", name, ext);
}

Shader* FindShader(uint32_t object) {
  std::lock_guard lock(g_shader_mutex);
  auto it = g_shaders.find(object);
  if (it == g_shaders.end()) return nullptr;
  LoadShader(it->second);
  return it->second.missing ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// renderer state

using plume::RenderBufferReference;

struct UploadRing {
  std::unique_ptr<plume::RenderBuffer> buffer;
  uint8_t* cpu = nullptr;
  uint64_t offset = 0;
};

struct CachedBuffer {
  std::shared_ptr<plume::RenderBuffer> resource;
  uint64_t hash = 0;
  uint64_t checked_frame = ~0ull;
  uint32_t size = 0;
  uint32_t changes = 0;  // times the contents changed
  bool dynamic = false;  // changes often: converted into the ring when changed
  // The dynamic buffer's latest conversion in this frame's ring.
  uint64_t ring_frame = ~0ull, ring_hash = 0;
  bool ring_hashed = false;  // (ring_hash known: only when used again, below)
  RenderBufferReference ring_ref;
  // Its uses in this frame and the last: a buffer drawn once a frame is just
  // converted (hashing it to spot a repeat would be wasted).
  uint64_t use_frame = ~0ull;
  uint32_t uses = 0, last_uses = 0;
  // Buffers drawn many times a frame (Batista's pyro: dozens of 3 KB
  // particle buffers, ~90 draws each) that the game only rewrites between
  // frames are checked once a frame - and at every use every 8th frame; a
  // change seen within a frame puts them back to checks at every use.
  bool changed_in_frame = false;
  bool per_frame = false;
  uint16_t stable_frames = 0;  // frames in a row without a change within them
  uint8_t demotions = 0;       // (twice: always checked at every use)
};

struct Stream {
  uint32_t buffer = 0;  // guest vertex buffer object
  uint32_t offset = 0;
  uint32_t stride = 0;
};

struct Stats {
  uint32_t draws = 0, drawn = 0, skipped_target = 0, skipped_shader = 0, skipped_prim = 0;
  uint32_t targets = 0, resolves = 0;
  uint32_t wide_3d = 0;  // depth-tested draws into wide targets (none: a 2D screen)
};

// A render target: the host image of one EDRAM colour surface
// (base tile, pitch, format, height) - EDRAM itself isn't emulated.
struct DepthTarget;

struct ColorTarget {
  std::shared_ptr<plume::RenderTexture> resource;  // typeless family of `format`
  std::unique_ptr<plume::RenderTextureView> rtv;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  // With each depth target (or none) it was drawn with.
  std::unordered_map<const DepthTarget*, std::unique_ptr<plume::RenderFramebuffer>> framebuffers;
  RenderFormat resource_format = RenderFormat::UNKNOWN;
  RenderFormat format = RenderFormat::UNKNOWN;  // the render target view format
  RenderFormat view_format = RenderFormat::UNKNOWN, view_gamma_format = RenderFormat::UNKNOWN;
  uint32_t components = 4;
  uint32_t width = 0, height = 0;
  uint32_t scale = 1;  // its host pixels per guest pixel (TargetScale)
  uint32_t host_w = 0;  // its host width (wider than width * scale on wide screens: HostWidth)
  uint32_t host_h = 0;  // and height (taller on screens narrower than 16:9: HostHeight)
  uint64_t used_frame = 0;
};

struct DepthTarget {
  std::shared_ptr<plume::RenderTexture> resource;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  uint32_t width = 0, height = 0;
  uint32_t scale = 1;
  uint32_t host_w = 0, host_h = 0;
};

// The copy a resolve made of a target (what the game samples as a texture).
struct ResolvedTexture {
  std::shared_ptr<plume::RenderTexture> resource;
  uint64_t last_frame = ~0ull;  // the frame it was last resolved in (WriteBackResolve)
  uint32_t streak = 0;          // consecutive frames it was resolved in
  uint32_t host_w = 0, host_h = 0;  // (larger than the guest size * scale when copied from a wide / tall target)
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  RenderFormat format = RenderFormat::UNKNOWN;
  uint32_t width = 0, height = 0;
  uint32_t scale = 1;  // (the resolved target's)
};

// A DrawVerticesUP waiting for its vertices (the game writes them into the
// space the call returns, then calls EndVertices).
struct PendingUP {
  bool active = false;
  uint32_t primitive = 0, count = 0, stride = 0, data = 0;
};

struct Renderer {
  std::unique_ptr<plume::RenderInterface> api;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  std::unique_ptr<plume::RenderCommandList> lists[kFrames];
  std::unique_ptr<plume::RenderCommandFence> fences[kFrames];
  bool submitted[kFrames] = {};  // the slot's fence has a submission to wait for
  plume::RenderCommandList* list = nullptr;  // this frame's
  std::unique_ptr<plume::RenderPipelineLayout> layout;
  // The shaders' texture tables (2D, 3D, cube) and samplers. Vulkan: the
  // three tables are one set (texture_sets[0], bindings 0-2 - with the
  // samplers and the constants three sets, within the four every GPU has);
  // texture_set(i) / texture_base[i] address a table either way.
  std::unique_ptr<plume::RenderDescriptorSet> texture_sets[3], sampler_set;
  plume::RenderDescriptorSet* texture_set[3] = {};
  uint32_t texture_base[3] = {};
  // Vulkan: each upload ring bound as the shaders' constant buffer (set 2):
  // the push constants hold the offsets of the draw's constants in it - no
  // buffer addresses or 64-bit integers in the shaders (shader_common.h).
  std::unique_ptr<plume::RenderDescriptorSet> constant_sets[kFrames];
  // Compact tables (backend::CompactTables(): Vulkan GPUs without descriptor
  // indexing): texture_sets[0] and sampler_set only record what each index
  // holds (TableRecord), and each draw binds two small sets of its own
  // textures and samplers (draw_sets), made once per contents and kept while
  // used (CompactSet).
  struct CompactSet {
    std::unique_ptr<plume::RenderDescriptorSet> set;
    uint64_t used = 0;  // (frame)
  };
  // (keyed by the 16 places' stamps - no allocation or string hash per draw)
  struct CompactKey {
    std::array<uint32_t, 16> stamps;
    bool operator==(const CompactKey& o) const { return stamps == o.stamps; }
  };
  struct CompactKeyHash {
    size_t operator()(const CompactKey& k) const {
      uint64_t h = 1469598103934665603ull;
      for (uint32_t v : k.stamps) h = (h ^ v) * 1099511628211ull;
      return size_t(h ^ (h >> 32));
    }
  };
  std::unordered_map<CompactKey, CompactSet, CompactKeyHash> compact_sets[2];  // textures, samplers
  plume::RenderDescriptorSet* draw_sets[2] = {};
  plume::RenderDescriptorSet* default_sets[2] = {};  // (placeholders only)
  uint64_t compact_swept = 0;
  std::unique_ptr<plume::RenderSampler> default_sampler;
  std::shared_ptr<plume::RenderTexture> outputs[kOutputs];
  std::unique_ptr<plume::RenderFramebuffer> output_framebuffers[kOutputs];
  uint32_t output_index = 0;
  std::unordered_map<uint64_t, ColorTarget> color_targets;
  std::unordered_map<uint64_t, DepthTarget> depth_targets;
  std::unordered_map<uint32_t, ResolvedTexture> resolved;  // destination base address ->
  uint32_t present_source = 0;  // the last full-screen colour resolve (the front buffer)
  ColorTarget* main_target = nullptr;  // the 1280-pitch target at tile 0
  PendingUP pending_up;
  UploadRing rings[kFrames];
  std::shared_ptr<plume::RenderTexture> null_2d, null_3d, null_cube;  // (0, 0, 0, 0) placeholders
  std::vector<std::shared_ptr<void>> null_views;
  std::unique_ptr<plume::RenderBuffer> zero_buffer;  // for vertex inputs the declaration lacks

  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> pipelines;
  std::unordered_map<uint64_t, CachedBuffer> vertex_buffers;  // (address << 32 | size)
  std::unordered_map<uint64_t, CachedBuffer> index_buffers;   // (address << 32 | size)
  std::deque<std::pair<uint64_t, std::shared_ptr<void>>> garbage;  // (frame, object)

  bool frame_open = false;
  bool hidden_frame = false;  // (30 fps at 60 Hz: SetHalfFrames - this frame isn't shown, its draws are skipped)
  uint32_t back_index = 0;  // frame slot (frames % kFrames)
  uint64_t frames = 0;

  std::chrono::steady_clock::time_point stat_start = std::chrono::steady_clock::now();
  uint32_t stat_frames = 0;
  Stats frame_stats, window_stats;
  // Performance log (every 5 s): time in the renderer's draw translation,
  // waiting for the GPU (frames in flight), and frame span.
  double perf_draw_ms = 0, perf_wait_ms = 0, perf_span_ms = 0, perf_submit_ms = 0;
  uint64_t perf_draws = 0, perf_drawn = 0;
  uint32_t perf_frames = 0;
  std::chrono::steady_clock::time_point perf_start = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point frame_begin;
  // The command list's state as Draw last set it, so unchanged state is not
  // recorded again (reset at every frame and by the other code that sets it).
  struct ListState {
    bool bound = false;  // pipeline layout and descriptor sets
    const plume::RenderPipeline* pso = nullptr;
    const plume::RenderFramebuffer* framebuffer = nullptr;
    uint32_t stencil_ref = UINT32_MAX;
    float blend[4] = {-1, -1, -1, -1};
    plume::RenderViewport viewport = {-1, -1, -1, -1, -1, -1};
    plume::RenderRect scissor = {-1, -1, -1, -1};
    plume::RenderIndexBufferView ibv = {};
    // The vertex buffers bound (consecutive draws of a mesh share them).
    plume::RenderVertexBufferView views[16] = {};
    plume::RenderInputSlot slots[16] = {};
    uint32_t view_count = 0;
    const plume::RenderDescriptorSet* compact[2] = {};  // (compact tables: the bound sets 0, 1)
  } list_state;
  // native_record_thread: stands in for the frame's list (r->list), replaying
  // onto it on its own thread.
  std::unique_ptr<RecordingList> recorder;
  textures::Context texture_context;  // this frame's (BeginFrame)
  // The last upload of each constant range (vertex, pixel): reused while the
  // mirror's range is unchanged (consecutive draws of a model share them).
  struct ConstantUpload {
    uint64_t hash = 0;
    RenderBufferReference ref;
  } constant_uploads[2];
  uint64_t perf_const_uploads = 0, perf_const_reused = 0;
};

// Keeps `object` alive until the GPU has finished the frame being recorded.
void Retire(Renderer* r, std::shared_ptr<void> object) {
  if (object) r->garbage.emplace_back(r->frames, std::move(object));
}

// Accumulates the scope's duration (milliseconds) into `ms`.
struct ScopeTimer {
  double& ms;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  explicit ScopeTimer(double& m) : ms(m) {}
  ~ScopeTimer() {
    ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
};

Renderer* g_r = nullptr;
Stream g_streams[16];

// The GPU's ALU constant file as the game programs it: float4 0-255 vertex,
// 256-511 pixel (little-endian floats), plus dirty flags for mirror ranges.
float g_gpu_constants[512][4];
bool g_constants_loaded[512];
// Constants loaded from memory since the last draw (OnLoadShaderConstants),
// and those of the draw in progress (taken at its start).
uint16_t g_loaded_pending[512], g_loaded_draw[512];
uint32_t g_loaded_pending_count = 0, g_loaded_draw_count = 0;  // loaded from memory for the draw in progress  // set by SetStreamSource (also before the first frame)
std::once_flag g_init_once;
bool g_failed = false;
// Main mode, switched to the emulated renderer from the GRAPHICS page: the
// hooks do nothing and the emulator draws and presents again.
std::atomic<bool> g_suspended{false};
std::recursive_mutex g_mutex;
// native_renderer=main: frames go to the main window through the emulator's
// presentation (rex/external_frame.h) instead of this renderer's own window.
bool g_main = false;
// Main mode: every target (see TargetScale), resolve copy and frame image has g_scale x g_scale
// times the guest's pixels (the emulator's resolution_scale setting); sizes the
// game sees and computes with stay the guest's.
uint32_t g_scale = 1;
// native_scale_effects off: only the main scene's targets (and their resolves
// and the frame images) are scaled; the effect buffers (the 1120 x 1120 shadow
// map, reflections, glow and blur chains) keep the guest's size.
bool g_scale_effects = true;
// native_shadows off: draws into the shadow map (the 1120 x 1120 32-bit float
// target) are skipped; its clear stays, so nothing reads as shadowed.
bool g_shadows = true;

// native_render_scale: the main scene's targets (and their resolves and the
// frame images) have this fraction of the scale's pixels per side - weak GPUs.
float g_res = 1.0f;

// Whether a target of this guest size is the main scene (1280 x 720).
bool SceneTarget(uint32_t width, uint32_t height) { return width >= 1152 && height >= 640; }

// Host pixels per guest pixel for a target of this guest size.
uint32_t TargetScale(uint32_t width, uint32_t height) {
  if (g_scale_effects) return g_scale;
  return width >= 1152 && height >= 640 ? g_scale : 1;  // (the scene: 1280 x 720)
}

// Wide screens (native_widescreen): in matches the scene is as wide as the
// window (up to 32:9). The camera's aspect ratio is patched to match - a
// 16:9 constant only the projection (sub_825818B8) reads, so the game also
// culls for the wider view - and the full-screen targets (and their resolves)
// get g_wide times the width. Screen-space passes follow on their own; the
// 2D HUD's sprites are drawn into the 16:9 middle (Draw). Menus stay 16:9.
float g_wide = 1.0f;
// Screens narrower than 16:9 (a foldable's inner screen, 16:10): the same
// the other way - g_tall times the height, the camera shows more above and
// below (its vertical field of view widened: CameraFovScale), the HUD in
// the vertical middle.
float g_tall = 1.0f;
std::atomic<float> g_fov_scale{1.0f};
std::atomic<bool> g_match_scene{false};
// The last frame drew nothing in 3D (a pause menu, a loading or transition
// screen): 2D art filling the screen - all of it goes in the 16:9 middle,
// with black sides.
bool g_frame_2d = false;
constexpr uint32_t kAspectConstant = 0x82007724;
constexpr float kAspect16x9 = 16.0f / 9.0f;
constexpr float kMaxWide = 2.0f;  // (32:9)

// The host width of a target of this guest size.
uint32_t HostWidth(uint32_t width, uint32_t height, uint32_t scale) {
  const uint32_t w = width * scale;
  const float f = SceneTarget(width, height) ? std::max(g_wide, 1.0f) * g_res : 1.0f;
  if (f == 1.0f) return w;
  return (uint32_t(std::lround(float(w) * f)) + 1) & ~1u;
}

// The host height of a target of this guest size.
uint32_t HostHeight(uint32_t width, uint32_t height, uint32_t scale) {
  const uint32_t h = height * scale;
  const float f = SceneTarget(width, height) ? std::max(g_tall, 1.0f) * g_res : 1.0f;
  if (f == 1.0f) return h;
  return (uint32_t(std::lround(float(h) * f)) + 1) & ~1u;
}

void SetCameraAspect(float aspect) {
  if (!g_memory) return;
  uint8_t* p = const_cast<uint8_t*>(g_memory->TranslateVirtual(kAspectConstant));
  uint32_t bits;
  std::memcpy(&bits, &aspect, 4);
  const uint8_t be[4] = {uint8_t(bits >> 24), uint8_t(bits >> 16), uint8_t(bits >> 8), uint8_t(bits)};
  if (std::memcmp(p, be, 4)) std::memcpy(p, be, 4);
}
// Main mode: the frame images' size - the window's 16:9 area, so the
// emulator's presentation shows them 1:1 (Present averages the scaled image
// down to it). Both follow the window and the anti-aliasing setting live
// (ApplyOutputSettings).
uint32_t g_out_w = kWidth, g_out_h = kHeight;
std::function<std::pair<uint32_t, uint32_t>()> g_window_size;

// The native renderer couldn't start or stopped working. The game draws only
// with it - no emulated fallback: the screen stays black with the reason on
// it (FailureReason, fps_overlay.cpp), and on Android the launcher shows it
// next time (native_failed.txt in the app's cache).
std::mutex g_fail_mutex;
std::string g_fail_reason;

void Fail(const char* reason = "it stopped working") {
  g_failed = true;
  {
    std::lock_guard lock(g_fail_mutex);
    if (g_fail_reason.empty()) g_fail_reason = reason;
  }
  REXLOG_ERROR("native renderer: can't draw the game - {} (no emulated fallback)", reason);
#if defined(__ANDROID__)
  if (const char* cache = std::getenv("SVR2011_CACHE_DIR")) {
    if (FILE* f = std::fopen((std::string(cache) + "/native_failed.txt").c_str(), "w")) {
      std::fprintf(f, "%s\n", reason);
      std::fclose(f);
    }
  }
#endif
}

bool EnvFlag(const char* name) {
  char* v = nullptr;
  size_t n = 0;
  bool on = false;
  if (_dupenv_s(&v, &n, name) == 0 && v) {
    on = v[0] == '1';
    free(v);
  }
  return on;
}

// Debug: SVR2011_NATIVE_NOCACHE=1 - buffers and textures re-checked at every use.
bool NoCache() {
  static const bool on = EnvFlag("SVR2011_NATIVE_NOCACHE");
  return on;
}

// Debug aids (environment): SVR2011_NATIVE_STOP_AT_RESOLVE=1 draws
// only up to the first Resolve of each frame (shows the scene before
// post-processing).
bool g_stop_at_resolve = false;
bool g_resolved_this_frame = false;
// SVR2011_NATIVE_DEBUG_SOLID=1: every draw with a flat per-draw colour, no
// depth test, no culling (checks geometry independently of pixel shading).
bool g_debug_solid = false;
bool g_debug_solid_state = false;  // (=2: the shader only - depth, culling and blending as the game set them)
bool g_no_depth = false;  // SVR2011_NATIVE_NO_DEPTH=1
double g_pipeline_ms = 0;  // pipeline builds since the last perf line
// Bytes hashed since the last perf line: static / dynamic vertex buffers, constants.
uint64_t g_hash_vb_static = 0, g_hash_vb_dynamic = 0, g_hash_const = 0;
uint32_t g_vb_demotions = 0;
uint64_t g_repeat_same = 0, g_repeat_changed = 0;  // (total) dynamic buffers seen changing within a frame after trusting them
// Debug (SVR2011_NATIVE_VB_STATS=1): hashed uses per dynamic buffer (address << 32 | size).
std::unordered_map<uint64_t, uint32_t> g_vb_stats;
// Vertex / pixel constant blocks written since their last upload: D3D's
// constant flush (sub_829251D8) reports which 4-constant blocks it sends at
// each draw (bit 63 - n = block n, its order). A draw compares only those
// blocks with the last upload's copy (g_const_shadow, as in the mirror) and
// reuses the upload when they're unchanged - instead of hashing both 4 KB
// constant files every draw (~17-26 MB a frame in entrances: the render
// thread's largest cost on phones).
uint64_t g_const_dirty[2] = {~0ull, ~0ull};
bool g_const_force[2] = {true, true};  // (the last upload had loaded constants)
alignas(16) uint8_t g_const_shadow[2][256 * 16];
uint32_t g_const_stale = 0;  // (native_constant_check: reuses found stale)
// Pipelines built since the pipeline cache was saved, and when the last was.
std::atomic<bool> g_pipeline_cache_dirty{false};
std::atomic<int64_t> g_last_pipeline_build{0};  // (steady_clock ticks)

// The pipeline cache (gpu.h): the user data's cache folder, one per API.
std::filesystem::path PipelineCacheFile() {
  std::filesystem::path dir = rex::filesystem::GetExecutableFolder() / "UserData";
  char* v = nullptr;
  size_t n = 0;
  if (_dupenv_s(&v, &n, "SVR2011_USER_DATA") == 0 && v) {  // (tests)
    if (*v) dir = v;
    free(v);
  }
  return dir / "cache" /
         (backend::ActiveApi() == backend::Api::kVulkan ? "native_vulkan_pipelines.bin" : "native_d3d12_pipelines.bin");
}
bool g_no_blend = false;  // SVR2011_NATIVE_NO_BLEND=1
// SVR2011_NATIVE_DUMP_FRAME=<frame>: describe the first 40 draws of that
// native frame in native_draws.txt (next to the log).
uint64_t g_dump_frame = ~0ull;
uint32_t g_dump_first = 0, g_dump_count = 40;  // SVR2011_NATIVE_DUMP_FIRST / _COUNT
FILE* g_dump = nullptr;
// Debug: SVR2011_DRAWLOG=<file>: when <file>.go appears, one line per draw of
// the next frame goes to <file> (screen-space or not, depth, textures, the
// positions' extent) - for telling the HUD from the scene.
uint64_t g_drawlog_frame = ~0ull, g_drawlog_end = 0;  // (SVR2011_DRAWLOG_FRAMES: frames logged)
FILE* g_drawlog = nullptr;
std::vector<uint8_t> g_debug_ps;

// ---------------------------------------------------------------------------
// resources

// An upload-heap buffer (vertex / index / constant data written by the CPU).
std::shared_ptr<plume::RenderBuffer> CreateBuffer(Renderer* r, uint64_t size) {
  std::shared_ptr<plume::RenderBuffer> b = r->device->createBuffer(plume::RenderBufferDesc::UploadBuffer(
      size, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX | plume::RenderBufferFlag::CONSTANT));
  backend::LogBuffer(b.get(), size);
  return b;
}

// A 1x1 transparent black texture in texture table `dimension` (0 2D, 1 3D,
// 2 cube) at index `dimension`: what Xenia (and so the reference) samples for
// unbound / invalid fetch constants and unsupported textures.
std::shared_ptr<plume::RenderTexture> CreatePlaceholder(Renderer* r, uint32_t dimension) {
  const uint32_t array = dimension == 2 ? 6 : 1;
  const plume::RenderTextureDesc desc =
      dimension == 1 ? plume::RenderTextureDesc::Texture3D(1, 1, 1, 1, RenderFormat::R8G8B8A8_UNORM)
                     : plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1,
                                                         array, RenderFormat::R8G8B8A8_UNORM,
                                                         dimension == 2 ? plume::RenderTextureFlag::CUBE
                                                                        : plume::RenderTextureFlag::NONE);
  std::shared_ptr<plume::RenderTexture> tex = r->device->createTexture(desc);
  // Upload the texels through a temporary buffer.
  std::shared_ptr<plume::RenderBuffer> upload =
      r->device->createBuffer(plume::RenderBufferDesc::UploadBuffer(512ull * array));
  std::memset(upload->map(), 0x00, 512 * array);
  upload->unmap();
  r->list->barriers(RenderBarrierStage::COPY, plume::RenderTextureBarrier(tex.get(), RenderTextureLayout::COPY_DEST));
  for (uint32_t i = 0; i < array; ++i) {
    r->list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(tex.get(), 0, i),
        plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), RenderFormat::R8G8B8A8_UNORM, 1, 1, 1,
                                                          64, 512ull * i));  // (256-byte rows)
  }
  r->list->barriers(RenderBarrierStage::GRAPHICS_AND_COMPUTE,
                    plume::RenderTextureBarrier(tex.get(), RenderTextureLayout::SHADER_READ));
  Retire(r, upload);
  plume::RenderTextureViewDesc vd;
  vd.format = RenderFormat::R8G8B8A8_UNORM;
  vd.dimension = dimension == 1 ? plume::RenderTextureViewDimension::TEXTURE_3D
                 : dimension == 2 ? plume::RenderTextureViewDimension::TEXTURE_CUBE
                                  : plume::RenderTextureViewDimension::TEXTURE_2D;
  vd.mipLevels = 1;
  std::shared_ptr<plume::RenderTextureView> view = tex->createTextureView(vd);
  r->texture_set[dimension]->setTexture(r->texture_base[dimension] + dimension, tex.get(),
                                        RenderTextureLayout::SHADER_READ, view.get());
  r->null_views.push_back(view);
  return tex;
}

// The texture tables' layout (register spaces 0-3) - the same for the
// pipeline layout and the tables themselves.
plume::RenderDescriptorRange g_table_ranges[4];
plume::RenderDescriptorSetDesc g_table_descs[4];

// Vulkan: textures (set 0: 2D, 3D, cube - bindings 0-2), samplers (set 1)
// and this frame's upload ring as the constant buffer (set 2).
plume::RenderDescriptorRange g_vk_texture_ranges[3], g_vk_sampler_range, g_vk_constant_range;
plume::RenderDescriptorSetDesc g_vk_set_descs[3];

// Compact tables (backend::CompactTables()): set 0 is 13 2D textures, one 3D
// and two cube (bindings 0-2, 16 in all - old Mali drivers allow 16 a stage),
// set 1 16 samplers; the shaders' indices are places in them (.spvc shaders,
// shader_common.h). The game's shaders use at most 12 textures a draw.
constexpr uint32_t kCompactPlaces[3] = {13, 1, 2}, kCompactFirst[3] = {0, 13, 14};
constexpr uint32_t kCompactSamplers = 16;
plume::RenderDescriptorRange g_compact_texture_ranges[3], g_compact_sampler_range;
// Compact tables' constants (set 2): the upload ring as a storage buffer (the
// renderer's own shaders), and the vertex, pixel and shared constants as
// uniform buffers on it, at the draw's offsets (dynamic) - old Mali drivers
// read uniform buffers much faster than storage buffers (shader_common.h).
plume::RenderDescriptorRange g_compact_constant_ranges[4];
plume::RenderDescriptorSetDesc g_compact_descs[2];

// Compact tables: stands in for the big tables - remembers what each index
// holds (and a stamp, new at every write), for the draws' own sets.
class TableRecord final : public plume::RenderDescriptorSet {
 public:
  struct Entry {
    const plume::RenderTexture* texture = nullptr;
    plume::RenderTextureLayout layout = plume::RenderTextureLayout::SHADER_READ;
    const plume::RenderTextureView* view = nullptr;
    const plume::RenderSampler* sampler = nullptr;
    uint32_t stamp = 0;
  };
  explicit TableRecord(uint32_t size) : entries(size) {}
  void setBuffer(uint32_t, const plume::RenderBuffer*, uint64_t, const plume::RenderBufferStructuredView*,
                 const plume::RenderBufferFormattedView*) override {}
  void setTexture(uint32_t index, const plume::RenderTexture* texture, plume::RenderTextureLayout layout,
                  const plume::RenderTextureView* view) override {
    if (index >= entries.size() || !texture) return;
    Entry& e = entries[index];
    e.texture = texture;
    e.layout = layout;
    e.view = view;
    e.stamp = ++stamps;
  }
  void setSampler(uint32_t index, const plume::RenderSampler* sampler) override {
    if (index >= entries.size() || !sampler) return;
    entries[index].sampler = sampler;
    entries[index].stamp = ++stamps;
  }
  void setAccelerationStructure(uint32_t, const plume::RenderAccelerationStructure*) override {}
  std::vector<Entry> entries;
  uint32_t stamps = 0;
};

// Compact tables: the set holding these table indices (16 places; UINT32_MAX:
// a placeholder) - made the first time, then reused. Sets not used for a
// while are dropped (well after the GPU is done with them).
plume::RenderDescriptorSet* CompactSet(Renderer* r, int samplers, const uint32_t indices[16]) {
  auto* table = static_cast<TableRecord*>(samplers ? r->sampler_set.get() : r->texture_sets[0].get());
  Renderer::CompactKey key;
  for (int i = 0; i < 16; ++i)
    key.stamps[size_t(i)] = indices[i] < table->entries.size() ? table->entries[indices[i]].stamp : 0;
  auto& cache = r->compact_sets[samplers];
  if (r->frames - r->compact_swept > 600) {
    r->compact_swept = r->frames;
    for (int k = 0; k < 2; ++k)
      for (auto it = r->compact_sets[k].begin(); it != r->compact_sets[k].end();) {
        const bool keep = it->second.used + 2 * kFrames + 4 >= r->frames ||
                          it->second.set.get() == r->default_sets[0] || it->second.set.get() == r->default_sets[1];
        it = keep ? std::next(it) : r->compact_sets[k].erase(it);
      }
  }
  Renderer::CompactSet& c = cache[key];
  c.used = r->frames;
  if (c.set) return c.set.get();
  c.set = r->device->createDescriptorSet(g_compact_descs[samplers]);
  if (!c.set) return nullptr;
  for (uint32_t i = 0; i < 16; ++i) {
    if (samplers) {
      const uint32_t index = indices[i] < table->entries.size() ? indices[i] : 0;  // (0: linear wrap)
      c.set->setSampler(i, table->entries[index].sampler);
      continue;
    }
    const uint32_t dimension = i < kCompactFirst[1] ? 0 : i < kCompactFirst[2] ? 1 : 2;
    const uint32_t index =
        indices[i] < table->entries.size() ? indices[i] : r->texture_base[dimension] + dimension;
    const TableRecord::Entry& e = table->entries[index];
    c.set->setTexture(i, e.texture, e.layout, e.view);
  }
  return c.set.get();
}

// Compact tables: binds the draw's sets (r->draw_sets) where they changed.
void BindCompactSets(Renderer* r, plume::RenderCommandList* list) {
  auto& ls = r->list_state;
  for (uint32_t k = 0; k < 2; ++k) {
    plume::RenderDescriptorSet* set = r->draw_sets[k] ? r->draw_sets[k] : r->default_sets[k];
    if (set && ls.compact[k] != set) {
      list->setGraphicsDescriptorSet(set, k);
      ls.compact[k] = set;
    }
  }
}

bool CreateVulkanCompactLayout(Renderer* r) {
  for (uint32_t i = 0; i < 3; ++i)
    g_compact_texture_ranges[i] =
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, i, kCompactPlaces[i]);
  g_compact_sampler_range =
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 0, kCompactSamplers);
  g_compact_constant_ranges[0] =
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER, 0, 1);
  for (uint32_t i = 1; i < 4; ++i)
    g_compact_constant_ranges[i] =
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::CONSTANT_BUFFER_DYNAMIC, i, 1);
  g_compact_descs[0] = plume::RenderDescriptorSetDesc(g_compact_texture_ranges, 3);
  g_compact_descs[1] = plume::RenderDescriptorSetDesc(&g_compact_sampler_range, 1);
  g_vk_set_descs[0] = g_compact_descs[0];
  g_vk_set_descs[1] = g_compact_descs[1];
  g_vk_set_descs[2] = plume::RenderDescriptorSetDesc(g_compact_constant_ranges, 4);
  const plume::RenderPushConstantRange push(0, 0, 0, 4 * sizeof(uint32_t),
                                            plume::RenderShaderStageFlag::VERTEX |
                                                plume::RenderShaderStageFlag::PIXEL);
  plume::RenderPipelineLayoutDesc desc;
  desc.descriptorSetDescs = g_vk_set_descs;
  desc.descriptorSetDescsCount = 3;
  desc.pushConstantRanges = &push;
  desc.pushConstantRangesCount = 1;
  desc.allowInputLayout = true;
  r->layout = r->device->createPipelineLayout(desc);
  if (!r->layout) {
    REXLOG_ERROR("native renderer: could not create the pipeline layout (compact tables)");
    return false;
  }
  r->texture_sets[0] = std::make_unique<TableRecord>(3 * kSrvHeapSize);
  r->sampler_set = std::make_unique<TableRecord>(kSamplerHeapSize);
  for (uint32_t i = 0; i < 3; ++i) {
    r->texture_set[i] = r->texture_sets[0].get();
    r->texture_base[i] = i * kSrvHeapSize;
  }
  for (uint32_t i = 0; i < kFrames; ++i) {
    r->constant_sets[i] = r->device->createDescriptorSet(g_vk_set_descs[2]);
    if (!r->constant_sets[i] || !r->rings[i].buffer) return false;
    r->constant_sets[i]->setBuffer(0, r->rings[i].buffer.get(), kRingSize + kRingSlack);
    r->constant_sets[i]->setBuffer(1, r->rings[i].buffer.get(), kVertexConstantsBytes);
    r->constant_sets[i]->setBuffer(2, r->rings[i].buffer.get(), kPixelConstantsBytes);
    r->constant_sets[i]->setBuffer(3, r->rings[i].buffer.get(), kSharedConstantsBytes);
  }
  REXLOG_INFO("native renderer: compact tables ({} 2D / {} 3D / {} cube textures and {} samplers a draw)",
              kCompactPlaces[0], kCompactPlaces[1], kCompactPlaces[2], kCompactSamplers);
  return true;
}

bool CreateVulkanPipelineLayout(Renderer* r) {
  if (backend::CompactTables()) return CreateVulkanCompactLayout(r);
  for (uint32_t i = 0; i < 3; ++i)
    g_vk_texture_ranges[i] = plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, i, kSrvHeapSize);
  g_vk_sampler_range = plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 0, kSamplerHeapSize);
  g_vk_constant_range = plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER, 0, 1);
  g_vk_set_descs[0] = plume::RenderDescriptorSetDesc(g_vk_texture_ranges, 3, true, kSrvHeapSize);
  g_vk_set_descs[1] = plume::RenderDescriptorSetDesc(&g_vk_sampler_range, 1, true, kSamplerHeapSize);
  g_vk_set_descs[2] = plume::RenderDescriptorSetDesc(&g_vk_constant_range, 1);
  const plume::RenderPushConstantRange push(0, 0, 0, 4 * sizeof(uint32_t),
                                            plume::RenderShaderStageFlag::VERTEX |
                                                plume::RenderShaderStageFlag::PIXEL);
  plume::RenderPipelineLayoutDesc desc;
  desc.descriptorSetDescs = g_vk_set_descs;
  desc.descriptorSetDescsCount = 3;
  desc.pushConstantRanges = &push;
  desc.pushConstantRangesCount = 1;
  desc.allowInputLayout = true;
  r->layout = r->device->createPipelineLayout(desc);
  if (!r->layout) {
    REXLOG_ERROR("native renderer: could not create the pipeline layout");
    return false;
  }
  r->texture_sets[0] = r->device->createDescriptorSet(g_vk_set_descs[0]);
  r->sampler_set = r->device->createDescriptorSet(g_vk_set_descs[1]);
  for (uint32_t i = 0; i < 3; ++i) {
    r->texture_set[i] = r->texture_sets[0].get();
    r->texture_base[i] = i * kSrvHeapSize;
  }
  for (uint32_t i = 0; i < kFrames; ++i) {
    r->constant_sets[i] = r->device->createDescriptorSet(g_vk_set_descs[2]);
    if (!r->constant_sets[i] || !r->rings[i].buffer) return false;
    r->constant_sets[i]->setBuffer(0, r->rings[i].buffer.get(), kRingSize + kRingSlack);
  }
  return r->texture_sets[0] && r->sampler_set;
}

// Binds the texture tables, samplers and (Vulkan) this frame's constant buffer.
void BindTables(Renderer* r, plume::RenderCommandList* list) {
  if (backend::CompactTables()) {
    // (the placeholders' sets until a draw binds its own)
    if (!r->default_sets[0]) {
      uint32_t none[16];
      std::fill(std::begin(none), std::end(none), UINT32_MAX);
      for (int k = 0; k < 2; ++k) r->default_sets[k] = CompactSet(r, k, none);
    }
    r->list_state.compact[0] = r->list_state.compact[1] = nullptr;
    r->draw_sets[0] = r->draw_sets[1] = nullptr;
    BindCompactSets(r, list);
    const uint32_t zero[3] = {};
    list->setGraphicsDescriptorSetDynamic(r->constant_sets[r->back_index].get(), 2, zero, 3);
    return;
  }
  if (backend::ActiveApi() == backend::Api::kVulkan) {
    list->setGraphicsDescriptorSet(r->texture_sets[0].get(), 0);
    list->setGraphicsDescriptorSet(r->sampler_set.get(), 1);
    list->setGraphicsDescriptorSet(r->constant_sets[r->back_index].get(), 2);
    return;
  }
  for (uint32_t i = 0; i < 3; ++i) list->setGraphicsDescriptorSet(r->texture_sets[i].get(), i);
  list->setGraphicsDescriptorSet(r->sampler_set.get(), 3);
}

bool CreatePipelineLayout(Renderer* r) {
  if (backend::ActiveApi() == backend::Api::kVulkan) return CreateVulkanPipelineLayout(r);
  for (int i = 0; i < 4; ++i) {
    // (the count: Vulkan's upper bound for the table; D3D12 ignores it)
    g_table_ranges[i] = plume::RenderDescriptorRange(
        i == 3 ? plume::RenderDescriptorRangeType::SAMPLER : plume::RenderDescriptorRangeType::TEXTURE, 0,
        i == 3 ? kSamplerHeapSize : kSrvHeapSize);
    g_table_descs[i] = plume::RenderDescriptorSetDesc(&g_table_ranges[i], 1, true,
                                                      i == 3 ? kSamplerHeapSize : kSrvHeapSize);
  }
  // The constants: vertex, pixel, shared, the renderer's own (debug / present).
  // D3D12: root CBVs b0-b3 space4. Vulkan: their buffer addresses in the
  // push constants (shader_common.h, shaders/own_constants.hlsli).
  plume::RenderRootDescriptorDesc roots[4];
  for (uint32_t i = 0; i < 4; ++i)
    roots[i] = plume::RenderRootDescriptorDesc(i, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  const plume::RenderPushConstantRange push(0, 0, 0, 4 * sizeof(uint64_t),
                                            plume::RenderShaderStageFlag::VERTEX |
                                                plume::RenderShaderStageFlag::PIXEL);
  plume::RenderPipelineLayoutDesc desc;
  desc.descriptorSetDescs = g_table_descs;
  desc.descriptorSetDescsCount = 4;
  if (backend::ActiveApi() == backend::Api::kVulkan) {
    desc.pushConstantRanges = &push;
    desc.pushConstantRangesCount = 1;
  } else {
    desc.rootDescriptorDescs = roots;
    desc.rootDescriptorDescsCount = 4;
  }
  desc.allowInputLayout = true;
  r->layout = r->device->createPipelineLayout(desc);
  if (!r->layout) {
    REXLOG_ERROR("native renderer: could not create the pipeline layout");
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    r->texture_sets[i] = r->device->createDescriptorSet(g_table_descs[i]);
    r->texture_set[i] = r->texture_sets[i].get();
  }
  r->sampler_set = r->device->createDescriptorSet(g_table_descs[3]);
  return r->texture_sets[0] && r->texture_sets[1] && r->texture_sets[2] && r->sampler_set;
}

// The frame images, g_out_w x g_out_h, handed to the emulator's presentation
// (committed, so the reference the presentation holds keeps one alive).
bool CreateOutputs(Renderer* r) {
  for (uint32_t i = 0; i < kOutputs; ++i) {
    plume::RenderTextureDesc d = plume::RenderTextureDesc::Texture2D(
        g_out_w, g_out_h, 1, RenderFormat::R8G8B8A8_UNORM, plume::RenderTextureFlag::RENDER_TARGET);
    d.committed = true;
    r->output_framebuffers[i].reset();
    r->outputs[i] = r->device->createTexture(d);
    if (!r->outputs[i]) {
      REXLOG_ERROR("native renderer: could not create the frame images");
      return false;
    }
    r->outputs[i]->setName(fmt::format("frame image {}", i));
    const plume::RenderTexture* color = r->outputs[i].get();
    r->output_framebuffers[i] = r->device->createFramebuffer(plume::RenderFramebufferDesc(&color, 1));
  }
  return true;
}

void ReportDeviceRemoved(Renderer* r);
void StartPrebuildingPipelines(Renderer* r);

bool Initialize() {
  if (!g_memory) {
    REXLOG_ERROR("native renderer: guest memory not attached");
    return false;
  }
  auto* r = new Renderer();
  g_stop_at_resolve = EnvFlag("SVR2011_NATIVE_STOP_AT_RESOLVE");
  g_debug_solid = EnvFlag("SVR2011_NATIVE_DEBUG_SOLID");
  if (const char* v = std::getenv("SVR2011_NATIVE_DEBUG_SOLID")) g_debug_solid_state = g_debug_solid && std::strcmp(v, "2") != 0;
  g_no_depth = EnvFlag("SVR2011_NATIVE_NO_DEPTH");
  g_no_blend = EnvFlag("SVR2011_NATIVE_NO_BLEND");
  {
    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, "SVR2011_NATIVE_DUMP_FRAME") == 0 && v) {
      g_dump_frame = std::strtoull(v, nullptr, 10);
      free(v);
    }
    if (_dupenv_s(&v, &n, "SVR2011_NATIVE_DUMP_FIRST") == 0 && v) {
      g_dump_first = std::strtoul(v, nullptr, 10);
      free(v);
    }
    if (_dupenv_s(&v, &n, "SVR2011_NATIVE_DUMP_COUNT") == 0 && v) {
      g_dump_count = std::strtoul(v, nullptr, 10);
      free(v);
    }
  }
  std::string device_name;
  r->api = backend::CreateInterface(&device_name);
  if (!r->api) {
    REXLOG_ERROR("native renderer: no graphics API");
    return false;
  }
  r->device = r->api->createDevice(device_name);
  if (!r->device) {
    REXLOG_ERROR("native renderer: could not create the device");
    return false;
  }
  REXLOG_INFO("native renderer: GPU {}", r->device->getDescription().name);
  backend::LoadPipelineCache(r->device.get(), PipelineCacheFile());
  if (g_debug_solid) g_debug_ps = ReadShader("debug_solid.ps");
  r->queue = r->device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  for (uint32_t i = 0; i < kFrames; ++i) {
    r->lists[i] = r->queue->createCommandList();
    r->fences[i] = r->device->createCommandFence();
    r->rings[i].buffer = r->device->createBuffer(plume::RenderBufferDesc::UploadBuffer(
        kRingSize + kRingSlack,
        plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX | plume::RenderBufferFlag::CONSTANT |
            plume::RenderBufferFlag::STORAGE));
    if (!r->lists[i] || !r->fences[i] || !r->rings[i].buffer) {
      REXLOG_ERROR("native renderer: could not create the frame resources");
      return false;
    }
    r->rings[i].cpu = static_cast<uint8_t*>(r->rings[i].buffer->map());
    r->rings[i].buffer->setName(fmt::format("upload ring {}", i));
    backend::LogBuffer(r->rings[i].buffer.get(), kRingSize + kRingSlack);
  }
  if (!CreatePipelineLayout(r)) return false;
  if (!CreateOutputs(r)) return false;
  // Sampler 0: linear wrap (for fetch slots without a texture).
  r->default_sampler = r->device->createSampler(plume::RenderSamplerDesc());
  r->sampler_set->setSampler(0, r->default_sampler.get());

  // The setup work (placeholder uploads), submitted and waited for.
  r->list = r->lists[0].get();
  r->list->begin();
  r->null_2d = CreatePlaceholder(r, 0);
  r->null_3d = CreatePlaceholder(r, 1);
  r->null_cube = CreatePlaceholder(r, 2);
  r->list->end();
  r->queue->executeCommandLists(r->list, r->fences[0].get());
  r->queue->waitForCommandFence(r->fences[0].get());
  r->garbage.clear();
  r->zero_buffer = r->device->createBuffer(plume::RenderBufferDesc::UploadBuffer(256, plume::RenderBufferFlag::VERTEX));
  std::memset(r->zero_buffer->map(), 0, 256);
  r->zero_buffer->unmap();

  textures::Initialize(g_memory, 3, kSrvHeapSize - 3, 1, kSamplerHeapSize - 1);
  g_r = r;
  if (REXCVAR_GET(native_record_thread)) {
    r->recorder = std::make_unique<RecordingList>();
    REXLOG_INFO("native renderer: GPU commands recorded on a second thread");
  }
  REXLOG_INFO("native renderer: ready; shaders from {}", ShaderDirectory().string());
  StartPrebuildingPipelines(r);
  // Without its converted shaders (native_shaders\ beside the exe, installed
  // by the launcher) nothing could be drawn: a black screen. Let the emulated
  // renderer draw instead.
  {
    std::error_code ec;
    if (!ShaderExists("present.vs")) {
      REXLOG_ERROR("native renderer: its shaders are missing ({} has no present.vs{}) - reinstall "
                   "with the launcher to get the native_shaders folder",
                   ShaderDirectory().string(), backend::ShaderExtension());
      Fail("its shaders are missing - reinstall the game");
    }
  }
  return true;
}

// The GPU faulted (device removed): logs why (and the faulting allocation,
// when the driver tells) and turns the native renderer off for the rest of
// the run.
void ReportDeviceRemoved(Renderer* r) {
  backend::ReportDeviceLost(r->device.get());
  Fail("the GPU stopped (device lost)");
}

// Waits (CPU) for frame slot `slot`'s last submission. A GPU that stops
// answering must not hang the game: under Proton (vkd3d-proton) a stuck GPU
// job can leave the fence short for good, where Windows would remove the
// device. After kGpuTimeoutMs the native renderer gives up - the emulated
// renderer takes over, and the fence is set from the CPU so the emulator's
// queue, which waits on it for the frame, runs on.
bool WaitForFrame(Renderer* r, uint32_t slot, const char* what) {
  if (!r->submitted[slot]) return true;
  if (r->queue->waitForCommandFenceTimeout(r->fences[slot].get(), kGpuTimeoutMs)) {
    r->submitted[slot] = false;
    return true;
  }
  const bool lost = backend::DeviceLost(r->device.get());
  REXLOG_ERROR("native renderer: GPU did not finish ({}) in {} ms{}", what, kGpuTimeoutMs,
               lost ? " - device lost" : "");
  if (lost) backend::ReportDeviceLost(r->device.get());
  Fail(lost ? "the GPU stopped (device lost)" : "the GPU stopped answering");
  // Releases the emulator's waits for the frames in flight (each slot's).
  for (auto& fence : r->fences) backend::ReleaseFence(fence.get());
  return false;
}

// Waits for every frame in flight.
bool WaitIdle(Renderer* r, const char* what) {
  for (uint32_t i = 0; i < kFrames; ++i)
    if (!WaitForFrame(r, i, what)) return false;
  return true;
}

Renderer* Get() {
  std::call_once(g_init_once, [] {
    if (!Initialize())
      Fail("the GPU or its graphics driver lacks what it needs (the log's GPU report says what)");
  });
  return g_failed ? nullptr : g_r;
}

void Barrier(Renderer* r, plume::RenderTexture* texture, RenderTextureLayout layout) {
  const RenderBarrierStage::Bits stages =
      layout == RenderTextureLayout::SHADER_READ ? RenderBarrierStage::GRAPHICS_AND_COMPUTE
      : layout == RenderTextureLayout::COPY_SOURCE || layout == RenderTextureLayout::COPY_DEST
          ? RenderBarrierStage::COPY
          : RenderBarrierStage::GRAPHICS;
  r->list->barriers(stages, plume::RenderTextureBarrier(texture, layout));
}

// Allocates `size` bytes of this frame's upload ring.
std::pair<uint8_t*, RenderBufferReference> Allocate(Renderer* r, uint64_t size, uint64_t align = 256) {
  UploadRing& ring = r->rings[r->back_index];
  ring.offset = (ring.offset + align - 1) & ~(align - 1);
  if (ring.offset + size > kRingSize) return {nullptr, {}};
  auto result = std::make_pair(ring.cpu + ring.offset, ring.buffer->at(ring.offset));
  ring.offset += size;
  return result;
}

textures::Context TextureContext(Renderer* r) {
  textures::Context ctx;
  ctx.device = r->device.get();
  ctx.list = r->list;
  for (int i = 0; i < 3; ++i) {
    ctx.texture_sets[i] = r->texture_set[i];
    ctx.texture_base[i] = r->texture_base[i];
  }
  ctx.sampler_set = r->sampler_set.get();
  ctx.frame = r->frames;
  ctx.retire = [r](std::shared_ptr<void> object) { Retire(r, std::move(object)); };
  ctx.allocate = [r](uint64_t size, uint64_t align) -> textures::Context::UploadSpace {
    auto [cpu, ref] = Allocate(r, size, align);
    if (!cpu) return {nullptr, nullptr, 0};
    return {cpu, const_cast<plume::RenderBuffer*>(ref.ref), ref.offset};
  };
  return ctx;
}

// Main mode, at a frame's start: the frame images follow the window (its
// 16:9 area) and the render scale is the smallest that covers them - twice
// over with anti-aliasing (supersampling, averaged down by Present), at
// most 4x. A change rebuilds the scaled resources once the GPU is idle.
void ApplyOutputSettings(Renderer* r) {
  uint32_t win_w = kWidth, win_h = kHeight;
  if (g_window_size) {
    const auto [w, h] = g_window_size();
    if (w >= 64 && h >= 64) win_w = w, win_h = h;
  }
  static bool widescreen = REXCVAR_GET(native_widescreen);
  // A match on a window wider than 16:9: as wide as the window.
  float wide = 1.0f;
  float tall = 1.0f;
  if (widescreen && g_match_scene.load() && uint64_t(win_w) * 9 > uint64_t(win_h) * 16 + 16) {
    wide = std::min(float(win_w) / float(win_h) / kAspect16x9, kMaxWide);
  } else if (widescreen && g_match_scene.load() && uint64_t(win_w) * 9 + 16 < uint64_t(win_h) * 16) {
    tall = std::min(float(win_h) / float(win_w) * kAspect16x9, kMaxWide);  // (up to 8:9)
  }
  uint32_t out_w = win_w, out_h = win_w * 9 / 16;
  if (out_h > win_h) out_h = win_h, out_w = win_h * 16 / 9;
  if (wide > 1.0f) out_h = win_h, out_w = std::min(win_w, uint32_t(std::lround(win_h * kAspect16x9 * wide)));
  if (tall > 1.0f) out_w = win_w, out_h = std::min(win_h, uint32_t(std::lround(win_w / kAspect16x9 * tall)));
  out_w = std::max(out_w & ~1u, 64u);
  out_h = std::max(out_h & ~1u, 36u);
  // Anti-aliasing: native_aa, else the older on / off setting (on = 2x).
  auto aa_level = [] {
    const int32_t level = REXCVAR_GET(native_aa);
    return level > 0 ? uint32_t(std::clamp<int32_t>(level, 1, 4))
                     : (rex::cvar::Query<bool>("native_2x_msaa") ? 2u : 1u);
  };
  static uint32_t aa = aa_level();
  static int32_t max_scale = REXCVAR_GET(native_max_scale);
  static bool effects = REXCVAR_GET(native_scale_effects);
  static const bool shadows_once = (g_shadows = REXCVAR_GET(native_shadows));
  (void)shadows_once;
  static float res = float(REXCVAR_GET(native_render_scale));
  static uint64_t aa_checked = 0;
  if (r->frames >= aa_checked + 30) {  // a cvar query isn't free: twice a second
    aa = aa_level();
    max_scale = REXCVAR_GET(native_max_scale);
    effects = REXCVAR_GET(native_scale_effects);
    g_shadows = REXCVAR_GET(native_shadows);
    res = float(REXCVAR_GET(native_render_scale));
    widescreen = REXCVAR_GET(native_widescreen);
    aa_checked = r->frames;
  }
  // Enough guest pixels for every output pixel - for `aa` per axis with
  // anti-aliasing (2x2 supersampling at 1080p: 3x; up to native_max_scale).
  const uint32_t need = out_h * aa;
  const uint32_t limit = uint32_t(std::clamp<int32_t>(max_scale, 1, 4));
  const uint32_t scale = std::clamp<uint32_t>((need + kHeight - 1) / kHeight, 1, limit);
  const bool wide_changed = std::fabs(wide - g_wide) > 0.002f || std::fabs(tall - g_tall) > 0.002f;
  res = std::clamp(res, 0.25f, 1.0f);
  if (scale > 1) res = 1.0f;  // (only below the Xbox 360's own resolution)
  const bool res_changed = std::fabs(res - g_res) > 0.001f;
  if (scale == g_scale && effects == g_scale_effects && out_w == g_out_w && out_h == g_out_h &&
      !wide_changed && !res_changed) {
    return;
  }

  // Idle: nothing in flight may still use what is replaced.
  if (!WaitIdle(r, "output change")) return;
  r->garbage.clear();
  if (scale != g_scale || effects != g_scale_effects || wide_changed || res_changed) {
    g_res = res;
    textures::ForgetResolved();
    r->color_targets.clear();
    r->depth_targets.clear();
    r->resolved.clear();
    r->main_target = nullptr;
    r->present_source = 0;
    g_scale = scale;
    g_scale_effects = effects;
    g_wide = wide;
    g_tall = tall;
    SetCameraAspect(kAspect16x9 * wide / tall);
    g_fov_scale = tall;
  }
  if (out_w != g_out_w || out_h != g_out_h) {
    g_out_w = out_w;
    g_out_h = out_h;
    CreateOutputs(r);
  }
  r->list_state = {};
  REXLOG_INFO("native renderer: output {}x{}, render scale {}x{}{}{}{}", g_out_w, g_out_h, g_scale,
              g_res < 1.0f ? fmt::format(" (scene at {:.0f}%)", g_res * 100) : std::string(),
              aa > 1 ? fmt::format(" (anti-aliasing {}x)", aa) : std::string(), g_scale_effects ? "" : ", effects unscaled",
              g_wide > 1.0f   ? fmt::format(", wide {:.3f} (aspect {:.3f})", g_wide, kAspect16x9 * g_wide)
              : g_tall > 1.0f ? fmt::format(", tall {:.3f} (aspect {:.3f})", g_tall, kAspect16x9 / g_tall)
                              : std::string());
}

// Texture quality (native_texture_quality, twice a second): a change waits
// for the GPU, then the textures are uploaded again as they're used.
void ApplyTextureQuality(Renderer* r) {
  static uint64_t checked = 0;
  static uint32_t applied = 0;
  if (r->frames && r->frames < checked + 30) return;
  checked = r->frames;
  const std::string q = REXCVAR_GET(native_texture_quality);
  const uint32_t skip = q == "low" ? 2 : q == "medium" ? 1 : 0;
  if (skip == applied) return;
  if (!WaitIdle(r, "texture quality change")) return;
  r->garbage.clear();
  textures::SetQuality(skip);
  applied = skip;
}

// False: the GPU stopped answering and the native renderer gave up.
// 30 fps with the world at 60 Hz (frame_rate.cpp): every other frame isn't
// shown and its draws and clears aren't made (the GPU work of the frames no
// one sees); resolves still run (copies of what the last shown frame drew).
std::atomic<bool> g_half_frames{false};
std::atomic<bool> g_half_suspended{false};  // (the PC can't keep 60 Hz: every frame shown - SetHalfSuspended)
std::atomic<bool> g_last_hidden{false};  // (OnPresent's frame wasn't shown)

bool BeginFrame(Renderer* r) {
  if (r->frame_open) return true;
  if (g_main) ApplyTextureQuality(r);
  if (g_main) ApplyOutputSettings(r);
  if (g_failed) return false;
  r->back_index = uint32_t(r->frames % kFrames);
  r->output_index = uint32_t(r->frames % kOutputs);
  r->frame_begin = std::chrono::steady_clock::now();
  if (r->submitted[r->back_index]) {
    ScopeTimer wait(r->perf_wait_ms);
    if (!WaitForFrame(r, r->back_index, "previous frame")) return false;
  }
  // The frame kFrames back (this slot's last) has finished, and so have the
  // ones before it.
  while (!r->garbage.empty() && r->garbage.front().first + kFrames <= r->frames) {
    r->garbage.pop_front();
  }
  r->list = r->lists[r->back_index].get();
  if (r->recorder) {
    r->recorder->SetTarget(r->list);
    r->list = r->recorder.get();
  }
  r->list->begin();
  r->list_state = {};
  r->constant_uploads[0] = r->constant_uploads[1] = {};
  r->rings[r->back_index].offset = 0;
  r->frame_open = true;
  r->hidden_frame = g_half_frames.load() && !g_half_suspended.load() && (r->frames & 1);
  r->frame_stats = {};
  r->texture_context = TextureContext(r);
  g_resolved_this_frame = false;
  return true;
}

uint32_t g_write_backs = 0, g_write_back_w = 0, g_write_back_h = 0;  // (native perf line)
double g_write_back_ms = 0;

int BlankedSlot(Renderer* r);

void UpdateTitle(Renderer* r) {
  ++r->stat_frames;
  ++r->perf_frames;
  r->perf_draws += r->frame_stats.draws;
  r->perf_drawn += r->frame_stats.drawn;
  r->perf_span_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                               r->frame_begin).count();
  Stats& w = r->window_stats;
  const Stats& f = r->frame_stats;
  w.draws += f.draws;
  w.drawn += f.drawn;
  w.skipped_target += f.skipped_target;
  w.skipped_shader += f.skipped_shader;
  w.skipped_prim += f.skipped_prim;
  const auto now = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(now - r->stat_start).count();
  if (secs < 0.5) return;
  const double psecs = std::chrono::duration<double>(now - r->perf_start).count();
  if (psecs >= 5.0 && r->perf_frames) {
    const double f = r->perf_frames;
    if (backend::ActiveApi() == backend::Api::kVulkan) {
      const uint32_t passes = plume::g_svr_render_passes.exchange(0);
      const uint64_t pixels = plume::g_svr_render_pass_pixels.exchange(0);
      REXLOG_INFO("native perf: render passes per frame {:.1f} ({:.1f} Mpixels loaded and stored)", passes / f,
                  pixels / f / 1e6);
    }
    REXLOG_INFO("native perf: {:.1f} fps, per frame: draw {:.2f} ms, gpu wait {:.2f} ms, submit {:.2f} ms, "
                "span {:.2f} ms, draws {:.0f} (drawn {:.0f}), pipeline builds {:.0f} ms",
                f / psecs, r->perf_draw_ms / f, r->perf_wait_ms / f, r->perf_submit_ms / f, r->perf_span_ms / f,
                r->perf_draws / f, r->perf_drawn / f, g_pipeline_ms);
    if (g_write_backs) {
      REXLOG_INFO("native perf: {} resolves written back to guest memory ({:.0f} ms waiting; last {}x{})",
                  g_write_backs, g_write_back_ms, g_write_back_w, g_write_back_h);
      g_write_backs = 0;
      g_write_back_ms = 0;
    }
    {
      const textures::Stats ts = textures::TakePerf();
      REXLOG_INFO("native perf: textures: {} uploads ({:.1f} MB, {:.1f} ms, converting {:.1f} ms) in {:.1f} s; "
                  "{} resident ({:.1f} MB, {} with mip levels left out)",
                  ts.uploads_total, ts.upload_bytes / 1048576.0, ts.upload_ms, ts.convert_ms, psecs,
                  ts.textures, ts.resident_bytes / 1048576.0, ts.reduced);
      REXLOG_INFO("native perf: hashed per frame: vertex buffers {:.0f} KB static + {:.0f} KB dynamic, "
                  "constants compared {:.0f} KB ({:.0f}% reused), textures {:.0f} KB; "
                  "buffers back to checks at every use {}; repeat checks {:.0f} unchanged / {:.0f} changed",
                  g_hash_vb_static / f / 1024, g_hash_vb_dynamic / f / 1024, g_hash_const / f / 1024,
                  r->perf_const_uploads + r->perf_const_reused
                      ? 100.0 * r->perf_const_reused / double(r->perf_const_uploads + r->perf_const_reused)
                      : 0.0,
                  ts.hash_bytes / f / 1024, g_vb_demotions, g_repeat_same / f, g_repeat_changed / f);
      g_repeat_same = g_repeat_changed = 0;
      g_hash_vb_static = g_hash_vb_dynamic = g_hash_const = 0;
      if (!g_vb_stats.empty()) {
        std::vector<std::pair<uint64_t, uint32_t>> top(g_vb_stats.begin(), g_vb_stats.end());
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
          return uint64_t(a.second) * uint32_t(a.first) > uint64_t(b.second) * uint32_t(b.first);
        });
        for (size_t i = 0; i < top.size() && i < 6; ++i) {
          REXLOG_INFO("native perf: dynamic buffer {:08X} {} KB: {:.1f} hashed uses a frame",
                      uint32_t(top[i].first >> 32), uint32_t(top[i].first) / 1024, top[i].second / f);
        }
        g_vb_stats.clear();
      }
      r->perf_const_uploads = r->perf_const_reused = 0;
    }
    g_pipeline_ms = 0;
    r->perf_draw_ms = r->perf_wait_ms = r->perf_span_ms = r->perf_submit_ms = 0;
    r->perf_draws = r->perf_drawn = 0;
    r->perf_frames = 0;
    r->perf_start = now;
    // New pipelines, none for a few seconds (a scene loaded): keep them.
    if (g_pipeline_cache_dirty &&
        now - std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(g_last_pipeline_build.load())) >
            std::chrono::seconds(3)) {
      g_pipeline_cache_dirty = false;
      backend::SavePipelineCache(r->device.get(), PipelineCacheFile());
    }
  }
  r->stat_start = now;
  r->stat_frames = 0;
  r->window_stats = {};
}

// ---------------------------------------------------------------------------
// state translation

// ---------------------------------------------------------------------------
// render targets
//
// Each EDRAM colour surface the game renders to (base tile, pitch, format,
// height) gets a host render target, and each depth surface a D24S8 buffer
// (D32F S8 where the GPU has no D24S8: backend::DepthFormat);
// a Resolve copies a target into a texture the game then samples
// (textures::RegisterResolved), and Present shows the last full-screen
// colour resolve (the front buffer).

// Surface sizes from SetRenderTarget (the registers only hold the pitch):
// (pitch << 12 | EDRAM base tile) -> (width, height).
std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> g_surface_sizes;

struct TargetFormat {
  RenderFormat resource, rtv, view, view_gamma;
  uint32_t components;
};

// Xenos colour render target format (RB_COLOR_INFO bits 16-19) -> host.
TargetFormat ColorFormat(uint32_t xenos) {
  switch (xenos) {
    case 1:  // 8_8_8_8_GAMMA: the hardware gamma-encodes on write
      return {RenderFormat::R8G8B8A8_TYPELESS, RenderFormat::R8G8B8A8_UNORM_SRGB,
              RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_UNORM_SRGB, 4};
    case 2:
    case 10:
      return {RenderFormat::R10G10B10A2_TYPELESS, RenderFormat::R10G10B10A2_UNORM,
              RenderFormat::R10G10B10A2_UNORM, RenderFormat::UNKNOWN, 4};
    case 3:  // 2_10_10_10_FLOAT (7e3)
    case 5:
    case 7:
    case 12:
      return {RenderFormat::R16G16B16A16_TYPELESS, RenderFormat::R16G16B16A16_FLOAT,
              RenderFormat::R16G16B16A16_FLOAT, RenderFormat::UNKNOWN, 4};
    case 4:
    case 6:
      return {RenderFormat::R16G16_TYPELESS, RenderFormat::R16G16_FLOAT, RenderFormat::R16G16_FLOAT,
              RenderFormat::UNKNOWN, 2};
    case 14:
      return {RenderFormat::R32_TYPELESS, RenderFormat::R32_FLOAT, RenderFormat::R32_FLOAT,
              RenderFormat::UNKNOWN, 1};
    case 15:
      return {RenderFormat::R32G32_TYPELESS, RenderFormat::R32G32_FLOAT, RenderFormat::R32G32_FLOAT,
              RenderFormat::UNKNOWN, 2};
    default:  // 0: 8_8_8_8
      return {RenderFormat::R8G8B8A8_TYPELESS, RenderFormat::R8G8B8A8_UNORM,
              RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_UNORM_SRGB, 4};
  }
}

// A barrier for a texture whose layout the renderer keeps (`layout`).
void Transition(Renderer* r, plume::RenderTexture* texture, RenderTextureLayout& layout,
                RenderTextureLayout to) {
  if (layout == to) return;
  Barrier(r, texture, to);
  layout = to;
}

ColorTarget* GetColorTarget(Renderer* r, uint32_t base, uint32_t pitch, uint32_t format,
                            uint32_t height) {
  const uint64_t key = uint64_t(base) | (uint64_t(pitch) << 12) | (uint64_t(format) << 26) |
                       (uint64_t(height) << 32);
  if (auto it = r->color_targets.find(key); it != r->color_targets.end()) return &it->second;
  if (r->color_targets.size() >= kMaxColorTargets) return nullptr;
  const TargetFormat f = ColorFormat(format);
  const uint32_t scale = TargetScale(pitch, height);
  const uint32_t host_w = HostWidth(pitch, height, scale), host_h = HostHeight(pitch, height, scale);
  plume::RenderTextureDesc d = plume::RenderTextureDesc::Texture2D(
      host_w, host_h, 1, f.resource, plume::RenderTextureFlag::RENDER_TARGET);
  d.committed = true;
  const plume::RenderClearValue cv = plume::RenderClearValue::Color(plume::RenderColor(0, 0, 0, 0), f.rtv);
  d.optimizedClearValue = &cv;
  std::shared_ptr<plume::RenderTexture> resource = r->device->createTexture(d);
  if (!resource) {
    REXLOG_ERROR("native renderer: could not create a render target");
    return nullptr;
  }
  ColorTarget& t = r->color_targets[key];
  t.resource = std::move(resource);
  t.resource->setName(fmt::format("render target {}x{} fmt {} tile {}", pitch, height, format, base));
  t.resource_format = f.resource;
  t.format = f.rtv;
  t.view_format = f.view;
  t.view_gamma_format = f.view_gamma;
  t.components = f.components;
  t.width = pitch;
  t.height = height;
  t.scale = scale;
  t.host_w = host_w;
  t.host_h = host_h;
  t.rtv = t.resource->createTextureView(plume::RenderTextureViewDesc::Texture2D(f.rtv));
  REXLOG_INFO("native renderer: render target {}x{} format {} at EDRAM tile {}", pitch, height,
              format, base);
  return &t;
}

DepthTarget* GetDepthTarget(Renderer* r, uint32_t base, uint32_t pitch, uint32_t height) {
  const uint64_t key = uint64_t(base) | (uint64_t(pitch) << 12) | (uint64_t(height) << 32);
  if (auto it = r->depth_targets.find(key); it != r->depth_targets.end()) return &it->second;
  if (r->depth_targets.size() >= kMaxDepthTargets) return nullptr;
  // D24S8 (or D32F S8: backend::DepthFormat), copyable to a sampled depth
  // texture (typeless underneath).
  const uint32_t scale = TargetScale(pitch, height);
  const uint32_t host_w = HostWidth(pitch, height, scale), host_h = HostHeight(pitch, height, scale);
  plume::RenderTextureDesc d = plume::RenderTextureDesc::Texture2D(
      host_w, host_h, 1, backend::DepthFormat(),
      plume::RenderTextureFlag::DEPTH_TARGET);
  d.committed = true;
  const plume::RenderClearValue cv =
      plume::RenderClearValue::Depth(plume::RenderDepth(1.0f), backend::DepthFormat());
  d.optimizedClearValue = &cv;
  std::shared_ptr<plume::RenderTexture> resource = r->device->createTexture(d);
  if (!resource) {
    REXLOG_ERROR("native renderer: could not create a depth target");
    return nullptr;
  }
  DepthTarget& t = r->depth_targets[key];
  t.resource = std::move(resource);
  t.resource->setName(fmt::format("depth target {}x{} tile {}", pitch, height, base));
  t.width = pitch;
  t.height = height;
  t.scale = scale;
  t.host_w = host_w;
  t.host_h = host_h;
  return &t;
}

struct Targets {
  ColorTarget* color = nullptr;
  DepthTarget* depth = nullptr;
};

// The targets the EDRAM registers select now.
Targets CurrentTargets(Renderer* r) {
  const uint32_t si = Reg(RB_SURFACE_INFO), ci = Reg(RB_COLOR_INFO), di = Reg(RB_DEPTH_INFO);
  const uint32_t pitch = si & 0x3FFF;
  if (!pitch || pitch > 4096) return {};
  const uint32_t base = ci & 0xFFF;
  uint32_t height = pitch == 1280 ? 720 : pitch;
  if (auto it = g_surface_sizes.find((pitch << 12) | base); it != g_surface_sizes.end()) {
    height = it->second.second;
  }
  Targets t;
  t.color = GetColorTarget(r, base, pitch, (ci >> 16) & 0xF, height);
  t.depth = GetDepthTarget(r, di & 0xFFF, pitch, height);
  if (t.color && base == 0 && pitch == 1280) r->main_target = t.color;
  return t;
}

// Binds the current targets for drawing / clearing.
bool BindTargets(Renderer* r, const Targets& t) {
  if (!t.color) return false;
  Transition(r, t.color->resource.get(), t.color->layout, RenderTextureLayout::COLOR_WRITE);
  if (t.depth) Transition(r, t.depth->resource.get(), t.depth->layout, RenderTextureLayout::DEPTH_WRITE);
  std::unique_ptr<plume::RenderFramebuffer>& fb = t.color->framebuffers[t.depth];
  if (!fb) {
    const plume::RenderTexture* color = t.color->resource.get();
    const plume::RenderTextureView* view = t.color->rtv.get();
    plume::RenderFramebufferDesc desc(&color, 1, t.depth ? t.depth->resource.get() : nullptr);
    desc.colorAttachmentViews = &view;
    fb = r->device->createFramebuffer(desc);
    if (!fb) return false;
  }
  if (r->list_state.framebuffer != fb.get()) {
    r->list->setFramebuffer(fb.get());
    r->list_state.framebuffer = fb.get();
  }
  t.color->used_frame = r->frames;
  return true;
}

plume::RenderComparisonFunction Compare(uint32_t xenos) {  // Xenos 0 never .. 7 always
  return plume::RenderComparisonFunction(int(plume::RenderComparisonFunction::NEVER) + (xenos & 7));
}

plume::RenderBlend Blend(uint32_t xenos) {
  using B = plume::RenderBlend;
  switch (xenos) {
    case 0: return B::ZERO;
    case 1: return B::ONE;
    case 4: return B::SRC_COLOR;
    case 5: return B::INV_SRC_COLOR;
    case 6: return B::SRC_ALPHA;
    case 7: return B::INV_SRC_ALPHA;
    case 8: return B::DEST_COLOR;
    case 9: return B::INV_DEST_COLOR;
    case 10: return B::DEST_ALPHA;
    case 11: return B::INV_DEST_ALPHA;
    case 12: case 14: return B::BLEND_FACTOR;
    case 13: case 15: return B::INV_BLEND_FACTOR;
    case 16: return B::SRC_ALPHA_SAT;
    default: return B::ONE;
  }
}

plume::RenderBlend BlendAlpha(uint32_t xenos) {  // colour factors are invalid for alpha
  using B = plume::RenderBlend;
  switch (Blend(xenos)) {
    case B::SRC_COLOR: return B::SRC_ALPHA;
    case B::INV_SRC_COLOR: return B::INV_SRC_ALPHA;
    case B::DEST_COLOR: return B::DEST_ALPHA;
    case B::INV_DEST_COLOR: return B::INV_DEST_ALPHA;
    default: return Blend(xenos);
  }
}

plume::RenderBlendOperation BlendOp(uint32_t xenos) {
  using O = plume::RenderBlendOperation;
  switch (xenos) {
    case 1: return O::SUBTRACT;
    case 2: return O::MIN;
    case 3: return O::MAX;
    case 4: return O::REV_SUBTRACT;
    default: return O::ADD;
  }
}

plume::RenderStencilOp StencilOp(uint32_t xenos) {  // keep, zero, replace, incr_wrap, decr_wrap, invert, incr_sat, decr_sat
  using S = plume::RenderStencilOp;
  static const S ops[8] = {S::KEEP, S::ZERO, S::REPLACE, S::INCREMENT_AND_WRAP,
                           S::DECREMENT_AND_WRAP, S::INVERT, S::INCREMENT_AND_CLAMP,
                           S::DECREMENT_AND_CLAMP};
  return ops[xenos & 7];
}

// Xenos vertex format (+ whether the shader reads it as uint) -> DXGI.
// A 360 D3DDECLTYPE is: bits 0-5 format, 8 signed, 9 integer (not
// normalized), 10-21 the component swizzle (3 bits per xyzw: 0-3 a
// component, 4 zero, 5 one). The XDK applies the swizzle by patching the
// shader's vertex fetches, which the converted shaders (captured unpatched)
// don't have, so it is applied here: missing components already read as
// 0/0/0/1, and D3DCOLOR's ZYXW is a BGRA format.
constexpr uint32_t kSwizzleXYZW = 0 | 1 << 3 | 2 << 6 | 3 << 9;
constexpr uint32_t kSwizzleZYXW = 2 | 1 << 3 | 0 << 6 | 3 << 9;

bool SwizzleSupported(uint32_t type) {
  const uint32_t swizzle = (type >> 10) & 0xFFF;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t c = (swizzle >> (3 * i)) & 7;
    if (c != i && c != 4 && c != 5) return swizzle == kSwizzleZYXW && (type & 0x3F) == 6;
  }
  return true;
}

RenderFormat VertexFormat(uint32_t type, bool as_uint) {
  using F = RenderFormat;
  const bool is_signed = type & 0x100;
  switch (type & 0x3F) {
    case 6:
      if (as_uint) return is_signed ? F::R8G8B8A8_SINT : F::R8G8B8A8_UINT;
      if (((type >> 10) & 0xFFF) == kSwizzleZYXW) return F::B8G8R8A8_UNORM;  // D3DCOLOR
      return is_signed ? F::R8G8B8A8_SNORM : F::R8G8B8A8_UNORM;
    case 7:  return as_uint ? F::R10G10B10A2_UINT : F::R10G10B10A2_UNORM;
    case 16: case 17: return F::R32_UINT;  // 10_11_11 / 11_11_10, unpacked in the shader
    case 25:
      if (as_uint) return is_signed ? F::R16G16_SINT : F::R16G16_UINT;
      return is_signed ? F::R16G16_SNORM : F::R16G16_UNORM;
    case 26:
      if (as_uint) return is_signed ? F::R16G16B16A16_SINT : F::R16G16B16A16_UINT;
      return is_signed ? F::R16G16B16A16_SNORM : F::R16G16B16A16_UNORM;
    case 31: return F::R16G16_FLOAT;
    case 32: return F::R16G16B16A16_FLOAT;
    case 33: case 36: return as_uint ? F::R32_UINT : F::R32_FLOAT;
    case 34: case 37: return as_uint ? F::R32G32_UINT : F::R32G32_FLOAT;
    case 35: case 38: return as_uint ? F::R32G32B32A32_UINT : F::R32G32B32A32_FLOAT;
    case 57: return as_uint ? F::R32G32B32_UINT : F::R32G32B32_FLOAT;
    default: return F::UNKNOWN;
  }
}

const char* kUsageNames[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE",
                             "TEXCOORD", "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT",
                             "COLOR", "FOG", "DEPTH", "SAMPLE"};

struct Element {
  uint32_t stream, offset, type, usage, index;
};

std::vector<Element> Declaration() {
  std::vector<Element> out;
  const uint32_t decl = Be32(Device() + guest::kDeviceVertexDeclaration);
  if (!decl) return out;
  const uint8_t* p = Virtual(decl);
  const uint32_t n = std::min<uint32_t>(Be32(p + 0x18), 16);
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = p + 0x34 + 12 * i;
    out.push_back({Be16(e), Be16(e + 2), Be32(e + 4), e[9], e[10]});
  }
  return out;
}

// Vertex data is 8in32: each 32-bit word is byte-swapped, which is right for
// 32-bit and 8-bit components (Xenos takes x from the low byte of the swapped
// word, as DXGI does). 16-bit components come in memory order instead (x is
// the first half), so the words holding them get their halves swapped back:
// the stream's elements (declaration) decide which words those are.
// Returns a signature of the words it fixes (0 = none), for caching.
uint64_t HalfWords(const std::vector<Element>& decl, uint32_t stream, uint32_t* words,
                   uint32_t* count) {
  *count = 0;
  for (const Element& e : decl) {
    if (e.stream != stream) continue;
    const uint32_t f = e.type & 0x3F;
    const uint32_t bytes = (f == 25 || f == 31) ? 4 : (f == 26 || f == 32) ? 8 : 0;
    for (uint32_t b = 0; b < bytes && *count < 16; b += 4) words[(*count)++] = e.offset + b;
  }
  return *count ? XXH3_64bits(words, *count * 4) : 0;
}

void FixHalfWords(uint8_t* data, uint64_t size, uint32_t stride, uint32_t offset,
                  const std::vector<Element>& decl, uint32_t stream) {
  uint32_t words[16], count;
  if (!HalfWords(decl, stream, words, &count) || stride < 4) return;
  for (uint64_t v = offset; v + stride <= size; v += stride) {
    for (uint32_t i = 0; i < count; ++i) {
      if (words[i] + 4 > stride) continue;
      uint32_t w;
      std::memcpy(&w, data + v + words[i], 4);
      w = (w >> 16) | (w << 16);
      std::memcpy(data + v + words[i], &w, 4);
    }
  }
}

// Guest vertices -> little-endian into `dst` (mapped upload memory: write-
// combined, so it is only ever written, never read back): converted in a
// scratch buffer, then copied over in one go.
void ConvertVertices(uint8_t* dst, const uint8_t* src, uint64_t size, uint32_t stride,
                     uint32_t offset, const std::vector<Element>& decl, uint32_t stream) {
  uint32_t words[16], count;
  if (!HalfWords(decl, stream, words, &count) || stride < 4) {
    Swap32(dst, src, size);
    return;
  }
  thread_local std::vector<uint8_t> scratch;
  if (scratch.size() < size) scratch.resize(size);
  Swap32(scratch.data(), src, size);
  FixHalfWords(scratch.data(), size, stride, offset, decl, stream);
  std::memcpy(dst, scratch.data(), size);
}

// A guest index buffer (16-bit big-endian), converted whole and cached like a
// static vertex buffer (re-checked every 4th frame) - rather than converting
// each draw's indices into the ring (~4% of a phone's render thread in
// heavy entrances). One the game keeps rewriting is left to the ring (null).
RenderBufferReference IndexBuffer(Renderer* r, uint32_t address, uint32_t size) {
  constexpr uint32_t kRingAfterChanges = 3;
  CachedBuffer& c = r->index_buffers[(uint64_t(address) << 32) | size];
  if (c.dynamic || size < 2) return {};
  if (c.resource && r->frames - c.checked_frame < 4 && !NoCache()) return c.resource->at(0);
  const uint8_t* src = ObjectData(address);
  const uint64_t hash = XXH3_64bits(src, size);
  g_hash_vb_static += size;
  c.checked_frame = r->frames;
  if (c.resource && c.hash == hash) return c.resource->at(0);
  if (c.resource) {
    Retire(r, std::move(c.resource));
    c.resource.reset();
    if (++c.changes >= kRingAfterChanges) {
      c.dynamic = true;
      return {};
    }
  }
  c.resource = CreateBuffer(r, size);
  if (!c.resource) return {};
  c.resource->setName(fmt::format("index buffer {:08X} size {}", address, size));
  c.hash = hash;
  c.size = size;
  Swap16(static_cast<uint8_t*>(c.resource->map()), src, size & ~1u);
  c.resource->unmap();
  return c.resource->at(0);
}

// Vertex buffer object -> converted (little-endian) GPU buffer, cached per
// guest buffer (and layout) and re-converted when its contents change. A
// buffer that keeps changing is converted into the frame's upload ring at
// every draw instead (the game refills those between draws of a frame).
// Returns the buffer (a null reference if none).
RenderBufferReference VertexBuffer(Renderer* r, uint32_t object, uint32_t stream, const Stream& s,
                                   const std::vector<Element>& decl, uint32_t* out_size) {
  constexpr uint32_t kDynamicAfterChanges = 3;
  constexpr uint32_t kMaxDynamicSize = 512u << 10;
  const uint8_t* obj = Virtual(object);
  const uint32_t w0 = Be32(obj + 0x18), w1 = Be32(obj + 0x1C);
  const uint32_t address = w0 & ~3u;
  const uint32_t size = ((w1 >> 2) & 0xFFFFFF) * 4;
  *out_size = size;
  if (!size) return {};
  uint32_t words[16], count;
  uint64_t layout = HalfWords(decl, stream, words, &count);
  if (layout) layout ^= (uint64_t(s.stride) << 32) ^ s.offset;
  CachedBuffer& c = r->vertex_buffers[((uint64_t(address) << 32) | size) ^ (layout * 0x9E3779B97F4A7C15ull)];
  const uint8_t* src = ObjectData(address);
  if (c.dynamic) {
    // Re-converted only when the contents changed since this frame's last
    // conversion (a hash is far cheaper than the conversion) - for buffers
    // drawn more than once a frame; the others are just converted.
    if (c.use_frame != r->frames) {
      c.last_uses = c.use_frame + 1 == r->frames ? c.uses : 0;
      // (Its last frame of use need not be the previous frame.)
      if (c.uses > 1 && !c.changed_in_frame && c.demotions < 2) {
        if (++c.stable_frames >= 30) c.per_frame = true;
      } else if (c.changed_in_frame) {
        c.stable_frames = 0;
      }
      c.changed_in_frame = false;
      c.uses = 0;
      c.use_frame = r->frames;
    }
    ++c.uses;
    const bool repeat = c.ring_frame == r->frames && c.ring_ref.ref;
    if (repeat && c.per_frame && (r->frames & 7)) return c.ring_ref;
    const bool hashing = repeat || c.last_uses > 1;
    const uint64_t hash = hashing ? XXH3_64bits(src, size) : 0;
    if (hashing) g_hash_vb_dynamic += size;
    static const bool vb_stats = EnvFlag("SVR2011_NATIVE_VB_STATS");
    if (vb_stats && hashing) g_vb_stats[(uint64_t(address) << 32) | size] += 1;
    if (repeat && c.ring_hashed) {
      if (c.ring_hash == hash) {
        ++g_repeat_same;
        return c.ring_ref;
      }
      ++g_repeat_changed;
      c.changed_in_frame = true;
      if (c.per_frame) {
        c.per_frame = false;
        c.stable_frames = 0;
        ++c.demotions;
        ++g_vb_demotions;
      }
    }
    auto [cpu, ref] = Allocate(r, size, 256);
    if (cpu) {
      ConvertVertices(cpu, src, size, s.stride, s.offset, decl, stream);
      c.ring_frame = r->frames;
      c.ring_hash = hash;
      c.ring_hashed = hashing;
      c.ring_ref = ref;
      return ref;
    }
    // Ring full: fall back to the cached buffer (re-checked below).
  }
  // A static buffer is re-checked every 4th frame (buffers the game rewrites
  // turn dynamic after a few changes, then are checked at every use).
  if (c.resource && r->frames - c.checked_frame < 4 && !NoCache()) return c.resource->at(0);
  const uint64_t hash = XXH3_64bits(src, size);
  g_hash_vb_static += size;
  c.checked_frame = r->frames;
  if (c.resource && c.hash == hash) return c.resource->at(0);
  if (c.resource) {
    Retire(r, std::move(c.resource));
    c.resource.reset();
    if (++c.changes >= kDynamicAfterChanges && size <= kMaxDynamicSize && !c.dynamic) {
      c.dynamic = true;
      return VertexBuffer(r, object, stream, s, decl, out_size);
    }
  }
  c.resource = CreateBuffer(r, size);
  if (!c.resource) return {};
  c.resource->setName(fmt::format("vertex buffer {:08X} size {}", address, size));
  c.hash = hash;
  c.size = size;
  auto* dst = static_cast<uint8_t*>(c.resource->map());
  ConvertVertices(dst, src, size, s.stride, s.offset, decl, stream);  // 8in32 (all this game's)
  c.resource->unmap();
  return c.resource->at(0);
}

struct PipelineKey {
  uint64_t vs, ps;
  uint8_t vs_variant, ps_variant, topology;
  uint8_t rt_format;  // RenderFormat of the render target
  uint32_t depth, blend, colour_mask, cull, stencil_ref_mask;
  uint32_t layout_hash;
  float bias_scale, bias_offset;  // polygon offset (0 when off)
};

// A shader's compiled form for this device (made at its first use).
plume::RenderShader* Compiled(Renderer* r, const std::vector<uint8_t>& code,
                              std::unique_ptr<plume::RenderShader>& compiled) {
  if (!compiled && !code.empty())
    compiled = r->device->createShader(code.data(), code.size(), "main", backend::ShaderFormat());
  return compiled.get();
}

plume::RenderShader* Compiled(Renderer* r, Shader* s, int variant) {
  if (s->code[variant].empty()) variant = 0;
  return Compiled(r, s->code[variant], s->compiled[variant]);
}

void NotePipeline(const PipelineKey& key, uint64_t h, uint32_t layout_base_hash,
                  const std::vector<plume::RenderInputElement>& layout, const plume::RenderInputSlot* slots,
                  uint32_t slot_count);
std::unique_ptr<plume::RenderPipeline> TakePrebuilt(uint64_t h);
std::unique_ptr<plume::RenderPipeline> CreatePipeline(Renderer* r, const PipelineKey& key, plume::RenderShader* vs,
                                                      plume::RenderShader* ps, bool has_ps,
                                                      const plume::RenderInputElement* layout,
                                                      uint32_t layout_count, const plume::RenderInputSlot* slots,
                                                      uint32_t slot_count, bool render_thread);

const plume::RenderPipeline* Pipeline(Renderer* r, Shader* vs, int vs_variant, Shader* ps,
                                      int ps_variant, const std::vector<plume::RenderInputElement>& layout,
                                      const plume::RenderInputSlot* slots, uint32_t slot_count,
                                      uint32_t layout_hash, uint32_t layout_base_hash,
                                      plume::RenderPrimitiveTopology topology, RenderFormat rt_format) {
  PipelineKey key = {};
  key.rt_format = uint8_t(rt_format);
  key.vs = vs->hash;
  key.ps = ps ? ps->hash : 0;
  key.vs_variant = uint8_t(vs_variant);
  key.ps_variant = uint8_t(ps_variant);
  key.topology = uint8_t(topology);
  key.depth = Reg(RB_DEPTHCONTROL);
  key.blend = Reg(RB_BLENDCONTROL0);
  key.colour_mask = Reg(RB_COLOR_MASK) & 0xF;
  key.cull = Reg(PA_SU_SC_MODE_CNTL) & 0x7;
  key.stencil_ref_mask = Reg(RB_STENCILREFMASK) & 0xFFFF00;  // masks (ref is dynamic)
  key.layout_hash = layout_hash;
  // Polygon offset (PA_SU_SC_MODE_CNTL bit 11 front / 12 back enable): the
  // front values (the game culls or uses the same for both).
  if (Reg(PA_SU_SC_MODE_CNTL) & (3u << 11)) {
    key.bias_scale = RegFloat(PA_SU_POLY_OFFSET_FRONT_SCALE);
    key.bias_offset = RegFloat(PA_SU_POLY_OFFSET_FRONT_SCALE + 1);
  }
  const uint64_t h = XXH3_64bits(&key, sizeof(key));
  auto it = r->pipelines.find(h);
  if (it != r->pipelines.end()) return it->second.get();
  // Built ahead (the known pipelines, in the menus)?
  if (std::unique_ptr<plume::RenderPipeline> built = TakePrebuilt(h)) {
    return (r->pipelines[h] = std::move(built)).get();
  }
  NotePipeline(key, h, layout_base_hash, layout, slots, slot_count);

  plume::RenderShader* vs_compiled = Compiled(r, vs, vs_variant);
  plume::RenderShader* ps_compiled = nullptr;
  // SVR2011_NATIVE_DEBUG_PS=<name> + SVR2011_NATIVE_DEBUG_PS_FOR=<hash>,...:
  // those pixel shaders are replaced by <shader dir>/<name>.ps<extension>.
  static const std::pair<std::vector<uint8_t>, std::string> debug_override = [] {
    std::pair<std::vector<uint8_t>, std::string> o;
    char* e = nullptr;
    size_t n = 0;
    if (_dupenv_s(&e, &n, "SVR2011_NATIVE_DEBUG_PS") == 0 && e) {
      o.first = ReadShader(std::string(e) + ".ps");
      free(e);
    }
    if (_dupenv_s(&e, &n, "SVR2011_NATIVE_DEBUG_PS_FOR") == 0 && e) {
      o.second = e;
      free(e);
    }
    return o;
  }();
  static std::unique_ptr<plume::RenderShader> debug_solid_shader, debug_override_shader;
  char ps_name[20];
  std::snprintf(ps_name, sizeof(ps_name), "%016llX", static_cast<unsigned long long>(key.ps));
  if (g_debug_solid && !g_debug_ps.empty()) {
    ps_compiled = Compiled(r, g_debug_ps, debug_solid_shader);
  } else if (ps && !debug_override.first.empty() &&
             debug_override.second.find(ps_name) != std::string::npos) {
    ps_compiled = Compiled(r, debug_override.first, debug_override_shader);
  } else if (ps) {
    ps_compiled = Compiled(r, ps, ps_variant);
  }
  std::unique_ptr<plume::RenderPipeline> pso =
      CreatePipeline(r, key, vs_compiled, ps_compiled, ps != nullptr, layout.data(), uint32_t(layout.size()),
                     slots, slot_count, true);
  return (r->pipelines[h] = std::move(pso)).get();
}

// A pipeline from its key (no draw state is read: the background builder
// makes the known ones with it too). `render_thread`: logged, timed.
std::unique_ptr<plume::RenderPipeline> CreatePipeline(Renderer* r, const PipelineKey& key, plume::RenderShader* vs,
                                                      plume::RenderShader* ps, bool has_ps,
                                                      const plume::RenderInputElement* layout,
                                                      uint32_t layout_count, const plume::RenderInputSlot* slots,
                                                      uint32_t slot_count, bool render_thread) {
  const auto topology = plume::RenderPrimitiveTopology(key.topology);
  const auto rt_format = RenderFormat(key.rt_format);
  plume::RenderGraphicsPipelineDesc d;
  d.pipelineLayout = r->layout.get();
  d.vertexShader = vs;
  d.pixelShader = ps;
  // Blend (render target 0).
  const uint32_t bc = key.blend;
  plume::RenderBlendDesc& bt = d.renderTargetBlend[0];
  bt.srcBlend = Blend(bc & 0x1F);
  bt.blendOp = BlendOp((bc >> 5) & 7);
  bt.dstBlend = Blend((bc >> 8) & 0x1F);
  bt.srcBlendAlpha = BlendAlpha((bc >> 16) & 0x1F);
  bt.blendOpAlpha = BlendOp((bc >> 21) & 7);
  bt.dstBlendAlpha = BlendAlpha((bc >> 24) & 0x1F);
  using B = plume::RenderBlend;
  using O = plume::RenderBlendOperation;
  bt.blendEnabled = !(bt.srcBlend == B::ONE && bt.dstBlend == B::ZERO && bt.blendOp == O::ADD &&
                      bt.srcBlendAlpha == B::ONE && bt.dstBlendAlpha == B::ZERO &&
                      bt.blendOpAlpha == O::ADD);
  bt.renderTargetWriteMask = uint8_t(key.colour_mask);
  // Rasterizer.
  const bool cull_front = key.cull & 1, cull_back = key.cull & 2, front_cw = key.cull & 4;
  d.cullMode = cull_front ? plume::RenderCullMode::FRONT
               : cull_back ? plume::RenderCullMode::BACK
                           : plume::RenderCullMode::NONE;
  d.frontFace = front_cw ? plume::RenderFrontFace::CLOCKWISE : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  d.depthClipEnabled = true;
  // Xenos offsets are in depth units of the 24-bit buffer and slopes in 1/16
  // (as Xenia converts them). (D32F's unit is 2^(exponent - 23): the same
  // 2^-24 at depths 0.5..1, where scenes are; nearer, a smaller offset.)
  d.depthBias = int32_t(std::lround(double(key.bias_offset) * double(1 << 24)));
  d.slopeScaledDepthBias = key.bias_scale * (1.0f / 16.0f);
  // Depth / stencil.
  const uint32_t dc = key.depth;
  d.depthEnabled = (dc >> 1) & 1;
  d.depthWriteEnabled = (dc >> 2) & 1;
  d.depthFunction = Compare((dc >> 4) & 7);
  d.stencilEnabled = dc & 1;
  d.stencilReadMask = uint8_t(key.stencil_ref_mask >> 8);
  d.stencilWriteMask = uint8_t(key.stencil_ref_mask >> 16);
  d.stencilFrontFace.failOp = StencilOp((dc >> 11) & 7);
  d.stencilFrontFace.depthFailOp = StencilOp((dc >> 17) & 7);
  d.stencilFrontFace.passOp = StencilOp((dc >> 14) & 7);
  d.stencilFrontFace.compareFunction = Compare((dc >> 8) & 7);
  if ((dc >> 7) & 1) {
    d.stencilBackFace.failOp = StencilOp((dc >> 23) & 7);
    d.stencilBackFace.depthFailOp = StencilOp((dc >> 29) & 7);
    d.stencilBackFace.passOp = StencilOp((dc >> 26) & 7);
    d.stencilBackFace.compareFunction = Compare((dc >> 20) & 7);
  } else {
    d.stencilBackFace = d.stencilFrontFace;
  }
  d.dynamicStencilReferenceEnabled = true;
  d.dynamicBlendFactorEnabled = true;
  if (g_no_depth) {
    d.depthEnabled = false;
    d.stencilEnabled = false;
  }
  if (g_no_blend) {
    bt.blendEnabled = false;
    bt.renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::ALL);
  }
  if (g_debug_solid_state) {
    bt.blendEnabled = false;
    bt.renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::ALL);
    d.cullMode = plume::RenderCullMode::NONE;
    d.depthEnabled = false;
    d.stencilEnabled = false;
  }
  d.inputElements = layout;
  d.inputElementsCount = layout_count;
  d.inputSlots = slots;
  d.inputSlotsCount = slot_count;
  d.primitiveTopology = topology;
  d.renderTargetCount = 1;
  d.renderTargetFormat[0] = rt_format;
  d.depthTargetFormat = backend::DepthFormat();
  // Logged before it is built: a GPU driver that crashes compiling it (seen
  // under Proton) leaves this as the log's last pipeline.
  if (render_thread) REXLOG_INFO("native renderer: pipeline {} vs {:016X}.{} ps {:016X}.{} rt {} blend {:08X}{} mask {:X} "
              "depth {:08X} cull {} bias {}/{} topology {}",
              r->pipelines.size(), key.vs, key.vs_variant, key.ps, key.ps_variant, int(rt_format),
              key.blend, bt.blendEnabled ? " on" : "", key.colour_mask, key.depth, key.cull,
              key.bias_offset, key.bias_scale, int(topology));
  std::unique_ptr<plume::RenderPipeline> pso;
  const auto build_start = std::chrono::steady_clock::now();
  if (d.vertexShader && (d.pixelShader || !has_ps)) pso = r->device->createGraphicsPipeline(d);
  if (pso && !backend::PipelineCreated(pso.get())) pso.reset();
  g_pipeline_cache_dirty = true;
  g_last_pipeline_build = std::chrono::steady_clock::now().time_since_epoch().count();
  if (render_thread) {
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - build_start).count();
    g_pipeline_ms += ms;
    if (ms >= 20.0) REXLOG_INFO("native renderer: pipeline {} took {:.0f} ms", r->pipelines.size(), ms);
  }
  if (!pso && render_thread) {
    REXLOG_WARN("native renderer: pipeline creation failed (vs {:016X} ps {:016X})", key.vs,
                key.ps);
  }
  return pso;
}

// ---------------------------------------------------------------------------
// known pipelines: built ahead, in the menus
//
// A pipeline is compiled the first time the game draws with its state - in
// the middle of a scene, a hitch on slow CPUs and drivers. So every pipeline
// the renderer builds is noted (its key, vertex layout and slot strides:
// what it takes to build it again without the game's draw state) in
// UserData\cache\native_pipelines.list, beside the shipped list
// (native_shaders\pipelines.list). At start a background thread builds all
// the known ones at low priority while no match, entrance or cutscene is on
// (it waits in them); the render thread takes them from it instead of
// compiling them. They go into the pipeline cache too (D3D12 library, Vulkan
// cache), so on later starts they only load.

struct PipelineRecord {
  PipelineKey key;  // (layout_hash: the layout's own; Vulkan adds the strides)
  std::vector<std::string> names;
  std::vector<plume::RenderInputElement> layout;  // (semantic names point into names)
  uint32_t strides[16] = {};
};

struct PrebuiltPipelines {
  std::mutex mutex;
  std::condition_variable changed;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> ready;  // built, not taken yet
  std::unordered_set<uint64_t> claimed;  // the render thread builds these itself
  uint64_t busy = 0;                     // the one being built now
  std::unordered_set<uint64_t> known;    // listed (render thread)
  std::ofstream list;                    // the player's list, appended to (render thread)
  std::atomic<uint32_t> total{0}, done{0};
  std::atomic<bool> running{false};
} g_prebuilt;

// The key's hash (the pipelines' map key) for this API: Vulkan pipelines
// have the strides of the slots they use built in.
uint64_t RecordHash(PipelineRecord& rec) {
  PipelineKey key = rec.key;
  if (backend::ActiveApi() == backend::Api::kVulkan) {
    for (const auto& e : rec.layout) key.layout_hash = key.layout_hash * 31 + rec.strides[e.slotIndex & 15] * 977;
  }
  return XXH3_64bits(&key, sizeof(key));
}

uint32_t FloatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

float BitsFloat(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// One line: the key, the 16 strides, then the elements
// (name index location format slot offset).
std::string FormatRecord(const PipelineKey& k, uint32_t layout_base_hash,
                         const std::vector<plume::RenderInputElement>& layout, const plume::RenderInputSlot* slots,
                         uint32_t slot_count) {
  std::string s = fmt::format("{:016X} {} {:016X} {} {} {} {:08X} {:08X} {:X} {:X} {:X} {:08X} {:08X} {:08X} |", k.vs,
                              k.vs_variant, k.ps, k.ps_variant, k.topology, k.rt_format, k.depth, k.blend,
                              k.colour_mask, k.cull, k.stencil_ref_mask, layout_base_hash, FloatBits(k.bias_scale),
                              FloatBits(k.bias_offset));
  for (uint32_t i = 0; i < 16; ++i) s += fmt::format(" {}", i < slot_count ? slots[i].stride : 0);
  s += " |";
  for (const auto& e : layout) {
    s += fmt::format(" {} {} {} {} {} {}", e.semanticName, e.semanticIndex, e.location, int(e.format), e.slotIndex,
                     e.alignedByteOffset);
  }
  return s;
}

bool ParseRecord(const std::string& line, PipelineRecord& rec) {
  const size_t bar1 = line.find('|'), bar2 = bar1 == std::string::npos ? bar1 : line.find('|', bar1 + 1);
  if (bar2 == std::string::npos) return false;
  std::istringstream head(line.substr(0, bar1)), strides(line.substr(bar1 + 1, bar2 - bar1 - 1)),
      elements(line.substr(bar2 + 1));
  PipelineKey& k = rec.key;
  k = {};
  uint32_t vsv, psv, topology, rt, scale, offset;
  if (!(head >> std::hex >> k.vs >> std::dec >> vsv >> std::hex >> k.ps >> std::dec >> psv >> topology >> rt >>
        std::hex >> k.depth >> k.blend >> k.colour_mask >> k.cull >> k.stencil_ref_mask >> k.layout_hash >> scale >>
        offset)) {
    return false;
  }
  k.vs_variant = uint8_t(vsv), k.ps_variant = uint8_t(psv), k.topology = uint8_t(topology), k.rt_format = uint8_t(rt);
  k.bias_scale = BitsFloat(scale), k.bias_offset = BitsFloat(offset);
  for (uint32_t i = 0; i < 16; ++i) {
    if (!(strides >> rec.strides[i])) return false;
  }
  std::string name;
  uint32_t index, location, format, slot, element_offset;
  while (elements >> name >> index >> location >> format >> slot >> element_offset) {
    rec.names.push_back(name);
    plume::RenderInputElement e;
    e.semanticIndex = index;
    e.location = location;
    e.format = RenderFormat(format);
    e.slotIndex = slot;
    e.alignedByteOffset = element_offset;
    rec.layout.push_back(e);
  }
  for (size_t i = 0; i < rec.layout.size(); ++i) rec.layout[i].semanticName = rec.names[i].c_str();
  return true;
}

std::filesystem::path PlayerPipelineList() { return PipelineCacheFile().parent_path() / "native_pipelines.list"; }

// The render thread is about to build a pipeline itself: claimed (the
// builder skips it), and listed for next time if it's new.
void NotePipeline(const PipelineKey& key, uint64_t h, uint32_t layout_base_hash,
                  const std::vector<plume::RenderInputElement>& layout, const plume::RenderInputSlot* slots,
                  uint32_t slot_count) {
  {
    std::lock_guard lock(g_prebuilt.mutex);
    g_prebuilt.claimed.insert(h);
  }
  if (!g_prebuilt.known.insert(h).second) return;
  if (!g_prebuilt.list.is_open()) {
    std::error_code ec;
    std::filesystem::create_directories(PlayerPipelineList().parent_path(), ec);
    g_prebuilt.list.open(PlayerPipelineList(), std::ios::app);
  }
  g_prebuilt.list << FormatRecord(key, layout_base_hash, layout, slots, slot_count) << '\n';
  g_prebuilt.list.flush();
}

// A pipeline the builder made (waiting if it's building this one now).
std::unique_ptr<plume::RenderPipeline> TakePrebuilt(uint64_t h) {
  std::unique_lock lock(g_prebuilt.mutex);
  g_prebuilt.changed.wait(lock, [h] { return g_prebuilt.busy != h; });
  auto it = g_prebuilt.ready.find(h);
  if (it == g_prebuilt.ready.end()) return nullptr;
  std::unique_ptr<plume::RenderPipeline> p = std::move(it->second);
  g_prebuilt.ready.erase(it);
  return p;
}

void PrebuildPipelines(Renderer* r, std::vector<PipelineRecord> records) {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#else
  setpriority(PRIO_PROCESS, 0, 10);
#endif
  rex::thread::set_current_thread_name("native pipelines");
  const auto start = std::chrono::steady_clock::now();
  // The builder's own copies of the shaders (files read and compiled here,
  // not on the render thread).
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders;
  auto shader = [&](uint64_t hash, bool pixel) -> Shader* {
    std::unique_ptr<Shader>& s = shaders[hash * 2 + (pixel ? 1 : 0)];
    if (!s) {
      s = std::make_unique<Shader>();
      s->hash = hash;
      s->pixel = pixel;
      char name[64];
      std::snprintf(name, sizeof(name), "%016llX.%s", static_cast<unsigned long long>(hash), pixel ? "ps" : "vs");
      if (ShaderExists(name)) {
        LoadShader(*s);
      } else {
        s->loaded = s->missing = true;  // (a list from an older version)
      }
    }
    return s->missing ? nullptr : s.get();
  };
  uint32_t built = 0, waited_ms = 0;
  // Debug: SVR2011_PREBUILD_DELAY_MS=<ms> - a pause before each (a slow PC).
  const uint32_t delay_ms = [] {
    char* v = nullptr;
    size_t n = 0;
    uint32_t ms = 0;
    if (_dupenv_s(&v, &n, "SVR2011_PREBUILD_DELAY_MS") == 0 && v) {
      ms = uint32_t(std::strtoul(v, nullptr, 10));
      free(v);
    }
    return ms;
  }();
  for (PipelineRecord& rec : records) {
    if (delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    // Not while a match, entrance or cutscene is on: the game needs the CPU.
    while (g_match_scene.load() && !g_failed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      waited_ms += 250;
    }
    if (g_failed || !REXCVAR_GET(native_prepare_pipelines)) break;  // (turned off in game)
    const uint64_t h = RecordHash(rec);
    {
      std::lock_guard lock(g_prebuilt.mutex);
      if (g_prebuilt.claimed.count(h) || g_prebuilt.ready.count(h)) {
        ++g_prebuilt.done;
        continue;
      }
      g_prebuilt.busy = h;
    }
    std::unique_ptr<plume::RenderPipeline> pso;
    Shader* vs = shader(rec.key.vs, false);
    Shader* ps = rec.key.ps ? shader(rec.key.ps, true) : nullptr;
    if (vs && (ps || !rec.key.ps)) {
      plume::RenderInputSlot slots[16];
      for (uint32_t i = 0; i < 16; ++i) slots[i] = plume::RenderInputSlot(i, rec.strides[i]);
      pso = CreatePipeline(r, rec.key, Compiled(r, vs, rec.key.vs_variant),
                           ps ? Compiled(r, ps, rec.key.ps_variant) : nullptr, ps != nullptr, rec.layout.data(),
                           uint32_t(rec.layout.size()), slots, 16, false);
    }
    {
      std::lock_guard lock(g_prebuilt.mutex);
      g_prebuilt.busy = 0;
      if (pso) g_prebuilt.ready[h] = std::move(pso), ++built;
    }
    g_prebuilt.changed.notify_all();
    ++g_prebuilt.done;
  }
  g_prebuilt.running = false;
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  REXLOG_INFO("native renderer: {} of {} known pipelines built ahead in {:.1f} s ({:.1f} s of it waiting for a "
              "scene to end)",
              built, records.size(), secs, waited_ms / 1000.0);
}

void StartPrebuildingPipelines(Renderer* r) {
  if (!REXCVAR_GET(native_prepare_pipelines) || g_debug_solid || g_no_depth || g_no_blend) return;
  std::vector<PipelineRecord> records;
  std::unordered_set<uint64_t> seen;
  // (the shipped list beside the exe even when the shaders come from elsewhere: dev runs)
  const std::filesystem::path shipped = rex::filesystem::GetExecutableFolder() / "native_shaders" / "pipelines.list";
  for (const std::filesystem::path& file : {shipped, PlayerPipelineList()}) {
    std::ifstream in(file);
    std::string line;
    uint32_t count = 0;
    while (std::getline(in, line)) {
      PipelineRecord rec;
      if (!ParseRecord(line, rec)) continue;
      const uint64_t h = RecordHash(rec);
      if (!seen.insert(h).second) continue;
      g_prebuilt.known.insert(h);
      records.push_back(std::move(rec));
      ++count;
    }
    if (count) REXLOG_INFO("native renderer: {} known pipelines in {}", count, file.string());
  }
  if (records.empty()) return;
  g_prebuilt.total = uint32_t(records.size());
  g_prebuilt.running = true;
  std::thread(PrebuildPipelines, r, std::move(records)).detach();
}

}  // namespace

bool PreparingPipelines(uint32_t* done, uint32_t* total, bool* paused) {
  if (!g_prebuilt.running.load()) return false;
  *done = g_prebuilt.done.load();
  *total = g_prebuilt.total.load();
  *paused = g_match_scene.load();
  return true;
}

namespace {

// Copies the shader constants from the device mirror, little-endian.
RenderBufferReference Constants(Renderer* r, uint32_t device_offset, uint32_t bytes) {
  auto [cpu, gpu] = Allocate(r, bytes);
  if (!cpu) return {};
  Swap32(cpu, Device() + device_offset, bytes);
  return gpu;
}

// Uploads float4 constants [first, first + bytes/16) of the GPU constant file.
// The device mirror holds them all (the XDK also writes it inline, with no
// hookable call), big-endian: byte-swapped 16 bytes at a time straight into
// the upload; the constants loaded from memory for this draw (so far only
// the shaders' literal c252-c255) take precedence.
RenderBufferReference GpuConstants(Renderer* r, uint32_t first, uint32_t bytes) {
  const uint32_t count = bytes / 16;
  const uint8_t* mirror = Device() + 0x780 + 16 * first;
  bool loaded_here = false;
  for (uint32_t k = 0; k < g_loaded_draw_count; ++k)
    if (g_loaded_draw[k] >= first && g_loaded_draw[k] < first + count) loaded_here = true;
  const int stage = first ? 1 : 0;
  Renderer::ConstantUpload& last = r->constant_uploads[stage];
  uint8_t* shadow = g_const_shadow[stage];
  static const bool check = REXCVAR_GET(native_constant_check);
  uint64_t& dirty = g_const_dirty[stage];
  if (!loaded_here && !g_const_force[stage] && last.ref.ref && bytes == sizeof(g_const_shadow[0])) {
    bool changed = false;
    for (uint64_t m = dirty; m && !changed;) {
      const uint32_t block = uint32_t(__builtin_clzll(m));  // (bit 63 = block 0)
      m &= ~(1ull << (63 - block));
      changed = std::memcmp(mirror + 64 * block, shadow + 64 * block, 64) != 0;
      g_hash_const += 64;
    }
    if (!changed) {
      dirty = 0;
      if (check && std::memcmp(mirror, shadow, bytes) != 0 && g_const_stale++ < 16) {
        REXLOG_WARN("native renderer: {} constants changed without a flush (upload was stale)",
                    stage ? "pixel" : "vertex");
      } else {
        ++r->perf_const_reused;
        return last.ref;
      }
    }
  }
  // The copy for later comparisons: the written blocks (all when unknown).
  if (bytes == sizeof(g_const_shadow[0])) {
    if (g_const_force[stage] || dirty == ~0ull || check) {
      std::memcpy(shadow, mirror, bytes);
    } else {
      for (uint64_t m = dirty; m;) {
        const uint32_t block = uint32_t(__builtin_clzll(m));
        m &= ~(1ull << (63 - block));
        std::memcpy(shadow + 64 * block, mirror + 64 * block, 64);
      }
    }
  }
  dirty = 0;
  // (loaded constants overlay this draw's upload only: the next one re-uploads)
  g_const_force[stage] = loaded_here;
  auto [cpu, gpu] = Allocate(r, bytes);
  if (!cpu) return {};
  ++r->perf_const_uploads;
  last = {0, gpu};
  Swap32(cpu, mirror, bytes);
  for (uint32_t k = 0; k < g_loaded_draw_count; ++k) {
    const uint32_t index = g_loaded_draw[k];
    if (index >= first && index < first + count)
      std::memcpy(cpu + 16 * (index - first), g_gpu_constants[index], 16);
  }
  // Debug: SVR2011_NATIVE_CONSTANT=<index 0-511>:<x>[,...] overrides a
  // constant's x (pixel constants are 256 + n).
  static const std::vector<std::pair<uint32_t, float>> overrides = [] {
    std::vector<std::pair<uint32_t, float>> v;
    char* e = nullptr;
    size_t n = 0;
    if (_dupenv_s(&e, &n, "SVR2011_NATIVE_CONSTANT") == 0 && e) {
      for (char* p = e; *p;) {
        const uint32_t index = std::strtoul(p, &p, 10);
        if (*p != ':') break;
        const float value = std::strtof(p + 1, &p);
        if (index < 512) v.emplace_back(index, value);
        if (*p == ',') ++p;
        else break;
      }
      free(e);
    }
    return v;
  }();
  for (const auto& [index, value] : overrides)
    if (index >= first && index < first + count) std::memcpy(cpu + 16 * (index - first), &value, 4);
  return gpu;
}

textures::Context TextureContext(Renderer* r);

// Debug: SVR2011_NATIVE_BLANK_CYCLE=a,b,c,... binds the placeholder instead of
// the texture in fetch slot a, then b, ... for 10 s each (-1 = none); the
// window title shows the current one.
int BlankedSlot(Renderer* r) {
  static const std::vector<int> cycle = [] {
    std::vector<int> v;
    char* e = nullptr;
    size_t n = 0;
    if (_dupenv_s(&e, &n, "SVR2011_NATIVE_BLANK_CYCLE") == 0 && e) {
      for (char* p = e; *p;) {
        v.push_back(std::strtol(p, &p, 10));
        if (*p == ',') ++p;
        else break;
      }
      free(e);
    }
    return v;
  }();
  if (cycle.empty()) return -2;
  return cycle[(r->frames / 600) % cycle.size()];
}

// `ndc`: g_NdcScale (x, y) and g_HalfPixelOffset (the NDC offset, z, w).
RenderBufferReference SharedConstants(Renderer* r, bool alpha_test, const Shader* vs,
                                      const Shader* ps, const float ndc[4]) {
  auto [cpu, gpu] = Allocate(r, kSharedConstantsBytes);
  if (!cpu) return {};
  auto* u = reinterpret_cast<uint32_t*>(cpu);
  std::memset(cpu, 0, kSharedConstantsBytes);
  // Compact tables: the draw's textures and samplers get places in its own
  // sets (the indices below are those places; unused slots read place 0).
  const bool compact = backend::CompactTables();
  uint32_t places[2][16], used[3] = {}, samplers = 0;
  if (compact) std::fill(&places[0][0], &places[0][0] + 32, UINT32_MAX);
  // g_ResourceIndices[32] (uint4): 2D, 3D, cube, sampler blocks of 8 x uint4.
  for (uint32_t slot = 0; slot < 32 && !compact; ++slot) {
    u[(0 * 8 + slot / 4) * 4 + slot % 4] = 0;  // placeholder 2D
    u[(1 * 8 + slot / 4) * 4 + slot % 4] = 1;  // placeholder 3D
    u[(2 * 8 + slot / 4) * 4 + slot % 4] = 2;  // placeholder cube
    u[(3 * 8 + slot / 4) * 4 + slot % 4] = 0;  // linear wrap
  }
  // The textures the shaders sample, from the fetch constants.
  const textures::Context& ctx = r->texture_context;
  for (const Shader* s : {vs, ps}) {
    for (const auto& [slot, dimension] : s->textures) {
      const uint8_t* f = Device() + guest::RegisterOffset(0x4800) + slot * 24;
      uint32_t fetch[6];
      for (int k = 0; k < 6; ++k) fetch[k] = Be32(f + 4 * k);
      {
        // debug: SVR2011_NATIVE_BLANK_CYCLE slot gets the placeholder, or with
        // SVR2011_NATIVE_CYCLE_ALPHA=1 its alpha forced to 1 (=2: every other
        // slot's alpha forced to 1)
        static const int alpha_mode = [] {
          char* e = nullptr;
          size_t n = 0;
          int m = 0;
          if (_dupenv_s(&e, &n, "SVR2011_NATIVE_CYCLE_ALPHA") == 0 && e) {
            m = std::atoi(e);
            free(e);
          }
          return m;
        }();
        const int blanked = BlankedSlot(r);
        if (blanked != -2) {
          const bool hit = int(slot) == blanked;
          if (alpha_mode == 0 && hit) continue;
          if ((alpha_mode == 1 && hit) || (alpha_mode == 2 && !hit))
            fetch[3] = (fetch[3] & ~(7u << 10)) | (5u << 10);
        }
      }
      const uint32_t srv = textures::Texture(ctx, fetch, dimension);
      if (compact) {
        if (dimension > 2 || used[dimension] >= kCompactPlaces[dimension]) {
          static bool warned = false;
          if (!warned) REXLOG_WARN("native renderer: a draw has more textures than the compact tables hold");
          warned = true;
          continue;
        }
        const uint32_t place = used[dimension]++;
        places[0][kCompactFirst[dimension] + place] =
            srv == UINT32_MAX ? UINT32_MAX : r->texture_base[dimension] + srv;
        u[(dimension * 8 + slot / 4) * 4 + slot % 4] = place;
        if (srv == UINT32_MAX) continue;
        const uint32_t sampler = textures::Sampler(ctx, fetch);
        uint32_t s = 0;
        while (s < samplers && places[1][s] != sampler) ++s;  // (shared)
        if (s == samplers && samplers < kCompactSamplers) places[1][samplers++] = sampler;
        u[(3 * 8 + slot / 4) * 4 + slot % 4] = s < kCompactSamplers ? s : 0;
        continue;
      }
      if (srv == UINT32_MAX) continue;
      u[(dimension * 8 + slot / 4) * 4 + slot % 4] = srv;
      u[(3 * 8 + slot / 4) * 4 + slot % 4] = textures::Sampler(ctx, fetch);
    }
  }
  if (compact) {
    r->draw_sets[0] = CompactSet(r, 0, places[0]);
    r->draw_sets[1] = CompactSet(r, 1, places[1]);
  }
  // c32: g_Booleans (vertex b0-b31), g_SwappedTexcoords, g_HalfPixelOffset;
  // c33: g_AlphaThreshold, g_NdcScale, g_PsBooleans (pixel b0-b31, the
  // register file's bools 128-159).
  const uint8_t* bools = Device() + guest::RegisterOffset(0x4900);
  u[32 * 4 + 0] = Be32(bools);
  u[33 * 4 + 3] = Be32(bools + 16);
  u[32 * 4 + 1] = 0;  // vertex data is converted per element: no swapped texcoords
  std::memcpy(&u[32 * 4 + 2], ndc + 2, 8);  // g_HalfPixelOffset (the NDC offset)
  std::memcpy(&u[33 * 4 + 1], ndc, 8);      // g_NdcScale
  const float threshold = alpha_test ? RegFloat(RB_ALPHA_REF) : 0.0f;
  std::memcpy(&u[33 * 4 + 0], &threshold, 4);
  return gpu;
}

// Binds the draw's constants (vertex, pixel, shared, own; null: none) - root
// CBVs on D3D12; on Vulkan their offsets in this frame's upload ring (bound
// as set 2) in the push constants. The pipeline layout must be set.
void BindConstants(Renderer* r, const RenderBufferReference constants[4]) {
  if (backend::ActiveApi() == backend::Api::kVulkan) {
    const plume::RenderBuffer* ring = r->rings[r->back_index].buffer.get();
    uint32_t offsets[4];
    for (int i = 0; i < 4; ++i) {
      offsets[i] = constants[i].ref == ring ? uint32_t(constants[i].offset) : 0;
      if (constants[i].ref && constants[i].ref != ring) {
        static bool warned = false;
        if (!warned) REXLOG_ERROR("native renderer: constants outside the frame's upload ring (not readable on Vulkan)");
        warned = true;
      }
    }
    r->list->setGraphicsPushConstants(0, offsets, 0, sizeof(offsets));
    // (compact tables: the vertex, pixel and shared constants as uniform buffers)
    if (backend::CompactTables())
      r->list->setGraphicsDescriptorSetDynamic(r->constant_sets[r->back_index].get(), 2, offsets, 3);
    return;
  }
  for (uint32_t i = 0; i < 4; ++i)
    if (constants[i].ref) r->list->setGraphicsRootDescriptor(constants[i], i);
}

// At every draw (drawn or not): constants set through the mirror have reached
// the GPU; blocks loaded from memory for this draw take precedence.
// The XDK also writes constants into the mirror inline (no hookable call),
// so the whole mirror is taken at every draw; blocks loaded from memory for
// the draw (so far only the shaders' literal c252-c255) take precedence.
void ApplyPendingConstants() {
  std::memcpy(g_loaded_draw, g_loaded_pending, g_loaded_pending_count * sizeof(uint16_t));
  g_loaded_draw_count = g_loaded_pending_count;
  for (uint32_t k = 0; k < g_loaded_pending_count; ++k) g_constants_loaded[g_loaded_pending[k]] = false;
  g_loaded_pending_count = 0;
}

// A vertex shader's input layout for the current vertex declaration.
struct InputLayout {
  std::vector<Element> decl;
  std::vector<plume::RenderInputElement> layout;
  std::vector<std::string> names;  // (the layout's semantic names point here)
  bool packed_normals = false, uses_zero_stream = false;
  uint32_t hash = 0;  // (of the layout; strides are added per draw)
};
std::unordered_map<uint64_t, InputLayout> g_input_layouts;

const InputLayout& GetInputLayout(const Shader* vs) {
  // Keyed by the shader's contents and the declaration's elements (a
  // declaration object's address may be reused for another).
  uint64_t key = vs->hash * 0x9E3779B97F4A7C15ull;
  if (const uint32_t d = Be32(Device() + guest::kDeviceVertexDeclaration)) {
    const uint8_t* p = Virtual(d);
    const uint32_t n = std::min<uint32_t>(Be32(p + 0x18), 16);
    key ^= XXH3_64bits(p + 0x34, 12 * n) + n;
  }
  auto [it, inserted] = g_input_layouts.try_emplace(key);
  InputLayout& il = it->second;
  if (!inserted) return il;
  il.decl = Declaration();
  const std::vector<Element>& decl = il.decl;
  il.names.reserve(vs->inputs.size());
  for (const ShaderInput& in : vs->inputs) il.names.push_back(in.semantic);
  std::vector<plume::RenderInputElement>& layout = il.layout;
  for (size_t k = 0; k < vs->inputs.size(); ++k) {
    const ShaderInput& in = vs->inputs[k];
    plume::RenderInputElement e;
    e.semanticName = il.names[k].c_str();
    e.semanticIndex = in.index;
    e.location = in.location >= 0 ? uint32_t(in.location) : uint32_t(layout.size());
    const Element* found = nullptr;
    for (const Element& el : decl) {
      if (el.usage < std::size(kUsageNames) && in.semantic == kUsageNames[el.usage] &&
          el.index == in.index) {
        found = &el;
        break;
      }
    }
    RenderFormat fmt = found ? VertexFormat(found->type, in.is_uint) : RenderFormat::UNKNOWN;
    if (found && !SwizzleSupported(found->type)) {
      static std::unordered_map<uint32_t, bool> logged;
      if (logged.emplace(found->type, true).second)
        REXLOG_WARN("native renderer: vertex type {:08X} has an unsupported swizzle", found->type);
    }
    if (found && fmt != RenderFormat::UNKNOWN && found->stream < 15) {
      e.format = fmt;
      e.slotIndex = found->stream;
      e.alignedByteOffset = found->offset;
      if (in.semantic == "NORMAL" && (found->type & 0x3F) == 17) il.packed_normals = true;
    } else {
      e.format = in.is_uint ? RenderFormat::R32G32B32A32_UINT : RenderFormat::R32G32B32A32_FLOAT;
      e.slotIndex = 15;  // zero stream
      e.alignedByteOffset = 0;
      il.uses_zero_stream = true;
    }
    layout.push_back(e);
  }
  for (const auto& e : layout) {
    il.hash = il.hash * 31 + uint32_t(e.format) * 7 + e.slotIndex * 131 + e.alignedByteOffset * 17 +
              e.semanticIndex;
  }
  return il;
}

// Whether a draw's positions are flat 2D in the target's pixels (a HUD
// sprite: x, y within the target, one depth in 0..1), from its first 64
// vertices (float positions only; a DrawPrimitiveUP's or its vertex
// buffer's). `full`: they cover the whole target (a fade).
bool FlatPixels(const std::vector<Element>& decl, const PendingUP* up, bool indexed, uint32_t start,
                uint32_t count, int32_t base_vertex, float tw, float th, bool* full) {
  for (const Element& el : decl) {
    if (el.usage != 0 || el.index != 0) continue;
    const RenderFormat f = VertexFormat(el.type, false);
    const int comps = f == RenderFormat::R32G32_FLOAT ? 2 : f == RenderFormat::R32G32B32_FLOAT ? 3
                      : f == RenderFormat::R32G32B32A32_FLOAT ? 4 : 0;
    if (!comps) return false;
    const uint8_t* data = nullptr;
    uint32_t stride = 0;
    if (up) {
      if (el.stream != 0) return false;
      data = ObjectData(up->data), stride = up->stride;
    } else {
      if (el.stream >= 15 || !g_streams[el.stream].buffer) return false;
      const Stream& st = g_streams[el.stream];
      data = ObjectData(Be32(Virtual(st.buffer) + 0x18) & ~3u) + st.offset, stride = st.stride;
    }
    if (!data || !stride || !count) return false;
    const uint8_t* ib = nullptr;
    if (indexed && !up) {
      const uint32_t ibo = Be32(Device() + guest::kDeviceIndexBuffer);
      if (!ibo) return false;
      ib = ObjectData(Be32(Virtual(ibo) + 0x18)) + start * 2;
    }
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t k = 0; k < std::min<uint32_t>(count, 64); ++k) {
      const int64_t v = ib ? int64_t(Be16(ib + 2 * k)) + base_vertex : int64_t(up ? k : start + k);
      if (v < 0) return false;
      const uint8_t* q = data + v * stride + el.offset;
      for (int c = 0; c < std::min(comps, 3); ++c) {
        const float x = BeFloat(q + 4 * c);
        mn[c] = std::min(mn[c], x), mx[c] = std::max(mx[c], x);
      }
    }
    const bool flat = comps == 2 || (mn[2] == mx[2] && mn[2] >= 0.0f && mn[2] <= 1.0f);
    const bool pixels = mn[0] >= -2 && mn[1] >= -2 && mx[0] <= tw + 2 && mx[1] <= th + 2 &&
                        (mx[0] - mn[0] > 2 || mx[1] - mn[1] > 2);
    *full = mn[0] <= 1 && mn[1] <= 1 && mx[0] >= tw - 1 && mx[1] >= th - 1;
    return flat && pixels;
  }
  return false;
}

// `up`: a DrawVerticesUP (stream 0 is the game's vertex data, not a buffer).
void Draw(Renderer* r, uint32_t primitive, int32_t base_vertex, uint32_t start, uint32_t count,
          bool indexed, const PendingUP* up = nullptr) {
  ScopeTimer timer(r->perf_draw_ms);
  ApplyPendingConstants();
  ++r->frame_stats.draws;
  if (r->hidden_frame) return;  // (a frame not shown: SetHalfFrames)
  if (g_stop_at_resolve && g_resolved_this_frame) return;
  const Targets targets = CurrentTargets(r);
  if (!targets.color) {
    ++r->frame_stats.skipped_target;
    return;
  }
  if (!g_shadows && targets.color->width == 1120 && targets.color->height == 1120 &&
      targets.color->format == RenderFormat::R32_FLOAT) {
    ++r->frame_stats.skipped_target;  // (native_shadows off: the shadow map keeps its clear)
    return;
  }
  Shader* vs = FindShader(Be32(Device() + guest::kDeviceCurrentVertexShader));
  Shader* ps = FindShader(Be32(Device() + guest::kDeviceCurrentPixelShader));
  if (!vs || !ps) {
    ++r->frame_stats.skipped_shader;
    return;
  }

  // Topology; quad lists become triangle lists.
  using T = plume::RenderPrimitiveTopology;
  T topology;
  bool quads = false;
  switch (primitive) {
    case 1: topology = T::POINT_LIST; break;
    case 2: topology = T::LINE_LIST; break;
    case 3: topology = T::LINE_STRIP; break;
    case 4: topology = T::TRIANGLE_LIST; break;
    case 6: topology = T::TRIANGLE_STRIP; break;
    case 13: topology = T::TRIANGLE_LIST; quads = true; break;
    default:
      ++r->frame_stats.skipped_prim;
      return;
  }

  // Input layout: every input the vertex shader declares, from the game's
  // vertex declaration (or a zero stream when the declaration lacks it).
  // Built once per (shader, declaration contents) - not per draw.
  const InputLayout& il = GetInputLayout(vs);
  const std::vector<Element>& decl = il.decl;
  const std::vector<plume::RenderInputElement>& layout = il.layout;
  const bool packed_normals = il.packed_normals;
  const bool uses_zero_stream = il.uses_zero_stream;
  uint32_t layout_hash = il.hash;
  // The vertex buffer slots (0-15; strides as set now). Vulkan pipelines
  // have the strides of the slots they use built in.
  plume::RenderInputSlot slots[16];
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t stride = up && i == 0 ? up->stride : i < 15 && !up ? g_streams[i].stride : 0;
    slots[i] = plume::RenderInputSlot(i, stride);
  }
  if (backend::ActiveApi() == backend::Api::kVulkan) {
    for (const auto& e : layout) layout_hash = layout_hash * 31 + slots[e.slotIndex].stride * 977;
  }

  // Alpha test: RB_COLORCONTROL bit 3 with a "greater" function (the only
  // kind the converted shaders implement: discard below the threshold).
  const uint32_t cc = Reg(RB_COLORCONTROL);
  const bool alpha_test = (cc & 8) && ((cc & 7) == 4 || (cc & 7) == 6);

  const plume::RenderPipeline* pso =
      Pipeline(r, vs, packed_normals ? 1 : 0, ps, alpha_test ? 2 : 0, layout, slots, 16,
               layout_hash, il.hash, topology, targets.color->format);
  if (!pso) {
    ++r->frame_stats.skipped_shader;
    return;
  }

  // Vertex buffers for the streams the layout uses.
  plume::RenderVertexBufferView views[16] = {};
  uint32_t max_slot = 0;
  if (up) {
    // The game's vertices, converted into the ring.
    const uint64_t bytes = uint64_t(up->count) * up->stride;
    auto [cpu, ref] = Allocate(r, bytes, 16);
    if (!cpu || !bytes) return;
    ConvertVertices(cpu, ObjectData(up->data), bytes, up->stride, 0, decl, 0);
    views[0] = plume::RenderVertexBufferView(ref, uint32_t(bytes));
  }
  for (const auto& e : layout) {
    max_slot = std::max(max_slot, e.slotIndex);
    if (e.slotIndex == 15 || up) continue;
    const Stream& s = g_streams[e.slotIndex];
    if (!s.buffer || views[e.slotIndex].buffer.ref) continue;
    uint32_t size = 0;
    const RenderBufferReference vb = VertexBuffer(r, s.buffer, e.slotIndex, s, decl, &size);
    if (!vb.ref || s.offset >= size) continue;
    views[e.slotIndex] = plume::RenderVertexBufferView(
        RenderBufferReference(vb.ref, vb.offset + s.offset), size - s.offset);
  }
  if (uses_zero_stream) {
    views[15] = plume::RenderVertexBufferView(r->zero_buffer->at(0), 256);
  }

  // Indices (16-bit big-endian), converted into the ring.
  uint32_t draw_count = count;
  bool draw_indexed = indexed;
  int32_t draw_base = base_vertex;
  plume::RenderIndexBufferView ibv;
  if (indexed) {
    const uint32_t ib = Be32(Device() + guest::kDeviceIndexBuffer);
    if (!ib) {
      ++r->frame_stats.skipped_prim;
      return;
    }
    const uint8_t* obj = Virtual(ib);
    const uint32_t address = Be32(obj + 0x18), size = Be32(obj + 0x1C);
    if ((start + count) * 2 > size) {
      ++r->frame_stats.skipped_prim;
      return;
    }
    const uint8_t* src = ObjectData(address) + start * 2;
    const uint32_t out_count = quads ? count / 4 * 6 : count;
    const RenderBufferReference cached = quads ? RenderBufferReference{} : IndexBuffer(r, address, size);
    if (cached.ref) {
      ibv = plume::RenderIndexBufferView(RenderBufferReference(cached.ref, cached.offset + start * 2ull),
                                         count * 2, RenderFormat::R16_UINT);
      draw_count = count;
    } else {
      auto [cpu, gpu] = Allocate(r, out_count * 2ull, 4);
      if (!cpu) return;
      auto* dst = reinterpret_cast<uint16_t*>(cpu);
      if (quads) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
          const uint16_t a = Be16(src + 2 * q), b = Be16(src + 2 * q + 2),
                         c = Be16(src + 2 * q + 4), d = Be16(src + 2 * q + 6);
          *dst++ = a; *dst++ = b; *dst++ = c;
          *dst++ = a; *dst++ = c; *dst++ = d;
        }
      } else {
        Swap16(cpu, src, count * 2ull);
      }
      ibv = plume::RenderIndexBufferView(gpu, out_count * 2, RenderFormat::R16_UINT);
      draw_count = out_count;
    }
  } else if (quads) {
    // Non-indexed quad list: two triangles per quad, from vertex `start`.
    const uint32_t out_count = count / 4 * 6;
    auto [cpu, gpu] = Allocate(r, out_count * 2ull, 4);
    if (!cpu || !out_count) return;
    auto* dst = reinterpret_cast<uint16_t*>(cpu);
    for (uint32_t q = 0; q + 3 < count; q += 4) {
      *dst++ = uint16_t(q); *dst++ = uint16_t(q + 1); *dst++ = uint16_t(q + 2);
      *dst++ = uint16_t(q); *dst++ = uint16_t(q + 2); *dst++ = uint16_t(q + 3);
    }
    ibv = plume::RenderIndexBufferView(gpu, out_count * 2, RenderFormat::R16_UINT);
    draw_count = out_count;
    draw_indexed = true;
    draw_base = int32_t(start);
  }

  // Viewport from the Xenos viewport transform.
  const float xs = RegFloat(PA_CL_VPORT_XSCALE), xo = RegFloat(PA_CL_VPORT_XOFFSET);
  const float ys = RegFloat(PA_CL_VPORT_YSCALE), yo = RegFloat(PA_CL_VPORT_YOFFSET);
  const float zs = RegFloat(PA_CL_VPORT_ZSCALE), zo = RegFloat(PA_CL_VPORT_ZOFFSET);
  plume::RenderViewport vp(xo - std::fabs(xs), yo - std::fabs(ys), 2 * std::fabs(xs),
                           2 * std::fabs(ys), zo, zo + zs);
  const float tw = float(targets.color->width), th = float(targets.color->height);
  // The shaders' position goes through g_NdcScale / g_HalfPixelOffset: with
  // the viewport transform off (PA_CL_VTE_CNTL) positions are in pixels of
  // the target; with it on, the Xenos viewport's flips are kept (viewports
  // here don't flip). D3D9-style pixel centres (PA_SU_VTX_CNTL bit 0
  // clear) put pixel centres on integer coordinates: half a pixel is added.
  const uint32_t vte = Reg(PA_CL_VTE_CNTL);
  float ndc[4] = {xs < 0 ? -1.0f : 1.0f, ys > 0 ? -1.0f : 1.0f, 0.0f, 0.0f};
  if (!(vte & 1)) {  // x scale/offset off
    vp.x = 0;
    vp.width = tw;
    ndc[0] = 2.0f / tw;
    ndc[2] = -1.0f;
  }
  if (!(vte & 4)) {  // y scale/offset off
    vp.y = 0;
    vp.height = th;
    ndc[1] = -2.0f / th;
    ndc[3] = 1.0f;
  }
  if (!(vte & 0x10)) {  // z scale/offset off
    vp.minDepth = 0;
    vp.maxDepth = 1;
  }
  if (vp.width <= 0 || vp.height <= 0) vp = plume::RenderViewport(0, 0, tw, th, 0, 1);
  if (!(Reg(PA_SU_VTX_CNTL) & 1)) {
    ndc[2] += 1.0f / vp.width;
    ndc[3] -= 1.0f / vp.height;
  }
  vp.minDepth = std::clamp(vp.minDepth, 0.0f, 1.0f);
  vp.maxDepth = std::clamp(vp.maxDepth, 0.0f, 1.0f);
  // (ndc above is in guest pixels; the target has `ts` times as many - and
  // `tsx` across on a wide screen.)
  const uint32_t ts = targets.color ? targets.color->scale : targets.depth ? targets.depth->scale : g_scale;
  const float tsx = targets.color && targets.color->host_w ? float(targets.color->host_w) / tw : float(ts);
  const float tsy = targets.color && targets.color->host_h ? float(targets.color->host_h) / th : float(ts);
  // (the scene at native_render_scale: fewer host pixels per guest pixel)
  const float base = float(ts) * (targets.color && SceneTarget(tw, th) ? g_res : 1.0f);
  const bool reshaped = tsx > base * 1.001f || tsy > base * 1.001f;  // (wide or tall)
  // Wide screen: the HUD's 2D sprites (vertices in the game's 1280 x 720
  // pixels, drawn without depth) go in the 16:9 middle, unstretched; a sprite
  // covering the whole screen (a fade) and everything else fill the width.
  bool middle = false;
  const bool depth_test = (Reg(RB_DEPTHCONTROL) >> 1) & 1;
  if (reshaped && depth_test) ++r->frame_stats.wide_3d;
  if (reshaped && g_frame_2d) {
    middle = (vte & 1) != 0;  // (screen-space passes keep the target's width)
  } else if (reshaped && (vte & 1) && !depth_test) {
    bool full = false;
    middle = FlatPixels(decl, up, indexed, start, count, base_vertex, tw, th, &full) && !full;
  }
  const float pad = middle ? (float(targets.color->host_w) - tw * base) * 0.5f : 0.0f;
  const float pad_y = middle && targets.color->host_h ? (float(targets.color->host_h) - th * base) * 0.5f : 0.0f;
  const float sx = middle ? base : tsx;
  const float sy = middle ? base : tsy;
  vp.x = vp.x * sx + pad;
  vp.y = vp.y * sy + pad_y;
  vp.width *= sx;
  vp.height *= sy;
  // Window scissor: D3D keeps it in the mirrored 0x2000 group (0x2011 top-left,
  // 0x2012 bottom-right; x in bits 0-14, y in bits 16-30), already clamped to
  // the scissor rect when the game enables one (device +0x2F00).
  const uint32_t sc_tl = Reg(PA_SC_WINDOW_SCISSOR_TL), sc_br = Reg(PA_SC_WINDOW_SCISSOR_BR);
  plume::RenderRect scissor(
      int32_t(std::lround(float(std::min<uint32_t>(sc_tl & 0x7FFF, targets.color->width)) * sx + pad)),
      int32_t(std::lround(float(std::min<uint32_t>((sc_tl >> 16) & 0x7FFF, targets.color->height)) * sy + pad_y)),
      int32_t(std::lround(float(std::min<uint32_t>(sc_br & 0x7FFF, targets.color->width)) * sx + pad)),
      int32_t(std::lround(float(std::min<uint32_t>((sc_br >> 16) & 0x7FFF, targets.color->height)) * sy + pad_y)));

  if (r->frames >= g_drawlog_frame && r->frames < g_drawlog_end && g_drawlog) {
    char tex[256] = "";
    for (const Shader* sh : {vs, ps}) {
      for (const auto& [slot, dimension] : sh->textures) {
        const uint8_t* fc = Device() + guest::RegisterOffset(0x4800) + slot * 24;
        const uint32_t base = Be32(fc + 4) & 0xFFFFF000u;
        auto it = r->resolved.find(base);
        char one[64];
        if (it != r->resolved.end()) {
          std::snprintf(one, sizeof(one), " %s%u:R%ux%u@%X/f%u/%ux%u", sh == vs ? "v" : "p", slot,
                        it->second.width, it->second.height, base, Be32(fc + 4) & 0x3F,
                        (Be32(fc + 8) & 0x1FFF) + 1, ((Be32(fc + 8) >> 13) & 0x1FFF) + 1);
        } else {
          std::snprintf(one, sizeof(one), " %s%u:t", sh == vs ? "v" : "p", slot);
        }
        std::strncat(tex, one, sizeof(tex) - std::strlen(tex) - 1);
      }
    }
    // The positions' extent (float positions only; the first 64 vertices).
    char pos[96] = "?";
    for (const Element& el : decl) {
      if (el.usage != 0 || el.index != 0) continue;
      const RenderFormat pf = VertexFormat(el.type, false);
      const int comps = pf == RenderFormat::R32G32_FLOAT ? 2 : pf == RenderFormat::R32G32B32_FLOAT ? 3
                        : pf == RenderFormat::R32G32B32A32_FLOAT ? 4 : 0;
      if (!comps) {
        std::snprintf(pos, sizeof(pos), "fmt%d", int(pf));
        break;
      }
      const uint8_t* data = nullptr;
      uint32_t stride = 0;
      if (up) {
        data = ObjectData(up->data), stride = up->stride;
      } else if (el.stream < 15 && g_streams[el.stream].buffer) {
        const Stream& st = g_streams[el.stream];
        data = ObjectData(Be32(Virtual(st.buffer) + 0x18) & ~3u) + st.offset, stride = st.stride;
      }
      if (!data || !stride) break;
      float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
      const uint32_t n = std::min<uint32_t>(indexed ? count : count, 64);
      const uint8_t* ib = nullptr;
      if (indexed && !up) {
        const uint8_t* obj = Virtual(Be32(Device() + guest::kDeviceIndexBuffer));
        ib = ObjectData(Be32(obj + 0x18)) + start * 2;
      }
      for (uint32_t k = 0; k < n; ++k) {
        const int64_t v = ib ? int64_t(Be16(ib + 2 * k)) + base_vertex : int64_t(up ? k : start + k);
        const uint8_t* q = data + v * stride + el.offset;
        for (int c = 0; c < std::min(comps, 3); ++c) {
          const float f = BeFloat(q + 4 * c);
          mn[c] = std::min(mn[c], f), mx[c] = std::max(mx[c], f);
        }
      }
      std::snprintf(pos, sizeof(pos), "x %.1f..%.1f y %.1f..%.1f z %.2f..%.2f", mn[0], mx[0], mn[1], mx[1],
                    comps > 2 ? mn[2] : 0.0f, comps > 2 ? mx[2] : 0.0f);
      break;
    }
    std::fprintf(g_drawlog, "f%llu %4u tgt %ux%u vte %02X z %u blend %08X prim %u n %u%s vs %08X ps %08X vp %.0f,%.0f %.0fx%.0f |%s | pos %s\n",
                 (unsigned long long)r->frames, r->frame_stats.drawn, targets.color->width, targets.color->height, Reg(PA_CL_VTE_CNTL) & 0x3F,
                 (Reg(RB_DEPTHCONTROL) >> 1) & 1, Reg(RB_BLENDCONTROL0), primitive, count, up ? " UP" : "",
                 uint32_t(vs->hash), uint32_t(ps->hash), vp.x, vp.y, vp.width, vp.height, tex, pos);
  }
  if (r->frames == g_dump_frame && r->frame_stats.drawn >= g_dump_first &&
      r->frame_stats.drawn < g_dump_first + g_dump_count) {
    if (!g_dump) {
      // (SVR2011_NATIVE_DUMP_FILE: elsewhere - phones whose game folder adb can't read)
      const char* file = std::getenv("SVR2011_NATIVE_DUMP_FILE");
      g_dump = std::fopen(file ? file : "native_draws.txt", "w");
    }
    FILE* f = g_dump;
    std::fprintf(f, "=== draw %u: prim %u base %d start %u count %u indexed %d | vs %016llX v%d ps %016llX v%d\n",
                 r->frame_stats.drawn, primitive, base_vertex, start, count, indexed,
                 (unsigned long long)vs->hash, packed_normals ? 1 : 0, (unsigned long long)ps->hash,
                 alpha_test ? 2 : 0);
    std::fprintf(f, "viewport %.1f %.1f %.1f %.1f z %.3f..%.3f  depthctl %08X cull %u\n", vp.x,
                 vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth, Reg(RB_DEPTHCONTROL),
                 Reg(PA_SU_SC_MODE_CNTL) & 7);
    std::fprintf(f, "target %ux%u fmt %d | vte %08X blend %08X colorctl %08X mask %X | surface %08X color %08X depth %08X\n",
                 targets.color->width, targets.color->height, int(targets.color->format),
                 Reg(PA_CL_VTE_CNTL), Reg(RB_BLENDCONTROL0), Reg(RB_COLORCONTROL),
                 Reg(RB_COLOR_MASK) & 0xF, Reg(RB_SURFACE_INFO), Reg(RB_COLOR_INFO),
                 Reg(RB_DEPTH_INFO));
    {
      const uint8_t* b = Device() + guest::RegisterOffset(0x4900);
      std::fprintf(f, "bools %08X %08X %08X %08X | %08X %08X %08X %08X\n", Be32(b), Be32(b + 4),
                   Be32(b + 8), Be32(b + 12), Be32(b + 16), Be32(b + 20), Be32(b + 24), Be32(b + 28));
      std::fprintf(f, "regs 2000-2012:");
      for (uint32_t reg = 0x2000; reg <= 0x2012; ++reg) std::fprintf(f, " %08X", Reg(reg));
      std::fprintf(f, "\n");
    }
    for (const Shader* sh : {vs, ps}) {
      for (const auto& [slot, dimension] : sh->textures) {
        const uint8_t* fc = Device() + guest::RegisterOffset(0x4800) + slot * 24;
        std::fprintf(f, "  %s tex slot %u dim %u: %08X %08X %08X %08X %08X %08X\n",
                     sh == vs ? "vs" : "ps", slot, dimension, Be32(fc), Be32(fc + 4), Be32(fc + 8),
                     Be32(fc + 12), Be32(fc + 16), Be32(fc + 20));
      }
    }
    if (up) {
      const uint8_t* v = ObjectData(up->data);
      for (uint32_t i = 0; i < up->count && i < 4; ++i) {
        std::fprintf(f, "  up v%u:", i);
        for (uint32_t k = 0; k < up->stride / 4; ++k) std::fprintf(f, " %g", BeFloat(v + i * up->stride + 4 * k));
        std::fprintf(f, "\n");
      }
    }
    for (const Element& el : decl)
      std::fprintf(f, "  decl s%u +%u type %08X usage %u.%u\n", el.stream, el.offset, el.type, el.usage,
                   el.index);
    for (const auto& e : layout)
      std::fprintf(f, "  input %s%u fmt %d slot %u +%u\n", e.semanticName, e.semanticIndex,
                   int(e.format), e.slotIndex, e.alignedByteOffset);
    for (uint32_t sl = 0; sl < 4; ++sl) {
      const Stream& st = g_streams[sl];
      if (!st.buffer) continue;
      const uint8_t* obj = Virtual(st.buffer);
      const uint32_t w0 = Be32(obj + 0x18), w1 = Be32(obj + 0x1C);
      std::fprintf(f, "  stream %u: obj %08X fetch %08X %08X stride %u offset %u\n", sl, st.buffer, w0,
                   w1, st.stride, st.offset);
      const uint8_t* vtx = ObjectData(w0 & ~3u);
      for (uint32_t v = 0; v < 3 && st.stride; ++v) {
        std::fprintf(f, "    v%u:", v);
        for (uint32_t k = 0; k < st.stride / 4 && k < 12; ++k) {
          const uint8_t* q = vtx + v * st.stride + 4 * k;
          std::fprintf(f, " %08X(%g)", Be32(q), BeFloat(q));
        }
        std::fprintf(f, "\n");
      }
    }
    if (indexed) {
      const uint8_t* obj = Virtual(Be32(Device() + guest::kDeviceIndexBuffer));
      const uint8_t* ix = ObjectData(Be32(obj + 0x18)) + start * 2;
      std::fprintf(f, "  ib %08X size %u first:", Be32(obj + 0x18), Be32(obj + 0x1C));
      for (uint32_t k = 0; k < 12 && k < count; ++k) std::fprintf(f, " %u", Be16(ix + 2 * k));
      std::fprintf(f, "\n");
    }
    // The constants the draw uses (GPU constant file): vertex gc0-gc255,
    // pixel pc0-pc255.
    // (as GpuConstants uploads them: the device mirror, loaded constants over it)
    for (uint32_t k = 0; k < 512; ++k) {
      float v[4];
      const uint8_t* m = Device() + 0x780 + 16 * k;
      for (int c = 0; c < 4; ++c) v[c] = BeFloat(m + 4 * c);
      for (uint32_t i = 0; i < g_loaded_draw_count; ++i)
        if (g_loaded_draw[i] == k) std::memcpy(v, g_gpu_constants[k], 16);
      std::fprintf(f, "  %s%u = %g %g %g %g\n", k < 256 ? "gc" : "pc", k & 255, v[0], v[1], v[2], v[3]);
    }
    std::fflush(f);
  }

  auto* list = r->list;
  auto& ls = r->list_state;
  BindTargets(r, targets);  // (barriers first: they end a Vulkan render pass)
  if (!ls.bound) {
    list->setGraphicsPipelineLayout(r->layout.get());
    BindTables(r, list);
    ls.bound = true;
  }
  {
    RenderBufferReference constants[4] = {GpuConstants(r, 0, kVertexConstantsBytes),
                                           GpuConstants(r, 256, kPixelConstantsBytes),
                                           SharedConstants(r, alpha_test, vs, ps, ndc), {}};
    auto [cpu, gpu] = Allocate(r, 256);
    if (cpu) {
      const uint32_t n = r->frame_stats.drawn * 2654435761u;
      const float c[4] = {0.25f + ((n >> 8) & 0xFF) / 340.0f, 0.25f + ((n >> 16) & 0xFF) / 340.0f,
                          0.25f + ((n >> 24) & 0xFF) / 340.0f, 1.0f};
      std::memcpy(cpu, c, sizeof(c));
      constants[3] = gpu;
    }
    BindConstants(r, constants);
  }
  if (backend::CompactTables()) BindCompactSets(r, list);
  if (ls.pso != pso) {
    list->setPipeline(pso);
    ls.pso = pso;
  }
  {
    const uint32_t n = max_slot + 1;
    bool same = ls.view_count == n;
    for (uint32_t i = 0; same && i < n; ++i) {
      same = ls.views[i].buffer.ref == views[i].buffer.ref && ls.views[i].buffer.offset == views[i].buffer.offset &&
             ls.views[i].size == views[i].size && ls.slots[i].index == slots[i].index &&
             ls.slots[i].stride == slots[i].stride;
    }
    if (!same) {
      list->setVertexBuffers(0, views, n, slots);
      std::copy(views, views + n, ls.views);
      std::copy(slots, slots + n, ls.slots);
      ls.view_count = n;
    }
  }
  const uint32_t stencil_ref = Reg(RB_STENCILREFMASK) & 0xFF;
  if (ls.stencil_ref != stencil_ref) {
    list->setStencilReference(stencil_ref);
    ls.stencil_ref = stencil_ref;
  }
  const float blend_factor[4] = {RegFloat(RB_BLEND_RED), RegFloat(RB_BLEND_RED + 1),
                                 RegFloat(RB_BLEND_RED + 2), RegFloat(RB_BLEND_RED + 3)};
  if (std::memcmp(ls.blend, blend_factor, sizeof(blend_factor))) {
    list->setBlendFactor(blend_factor);
    std::memcpy(ls.blend, blend_factor, sizeof(blend_factor));
  }
  // (Debug, SVR2011_NATIVE_GPU_TEST=4: a quarter-size viewport - as many triangles, 1/16 the pixels)
  static const bool quarter_viewport = [] {
    const char* t = std::getenv("SVR2011_NATIVE_GPU_TEST");
    return t && t[0] == '4';
  }();
  if (quarter_viewport) {
    vp.width *= 0.25f;
    vp.height *= 0.25f;
  }
  if (std::memcmp(&ls.viewport, &vp, sizeof(vp))) {
    list->setViewports(vp);
    ls.viewport = vp;
  }
  // Debug (GPU cost): SVR2011_NATIVE_GPU_TEST=1 draws into a 1x1 scissor (no
  // rasterisation / pixel work, vertex work kept), =2 skips the draw calls.
  static const int gpu_test = [] {
    const char* v = std::getenv("SVR2011_NATIVE_GPU_TEST");
    return v ? std::atoi(v) : 0;
  }();
  // (=3 with SVR2011_NATIVE_GPU_TEST_RANGE=<first>,<last>: those draws of each frame only)
  static const std::pair<uint32_t, uint32_t> gpu_range = [] {
    std::pair<uint32_t, uint32_t> v{0, ~0u};
    if (const char* e = std::getenv("SVR2011_NATIVE_GPU_TEST_RANGE")) std::sscanf(e, "%u,%u", &v.first, &v.second);
    return v;
  }();
  if (gpu_test == 1 || (gpu_test == 3 && r->frame_stats.drawn >= gpu_range.first &&
                        r->frame_stats.drawn <= gpu_range.second))
    scissor = plume::RenderRect(0, 0, 1, 1);
  if (std::memcmp(&ls.scissor, &scissor, sizeof(scissor))) {
    list->setScissors(scissor);
    ls.scissor = scissor;
  }
  if (gpu_test == 2) {
    ++r->frame_stats.drawn;
    return;
  }
  if (draw_indexed) {
    if (ls.ibv.buffer.ref != ibv.buffer.ref || ls.ibv.buffer.offset != ibv.buffer.offset ||
        ls.ibv.size != ibv.size) {
      list->setIndexBuffer(&ibv);
      ls.ibv = ibv;
    }
    list->drawIndexedInstanced(draw_count, 1, 0, draw_base, 0);
  } else {
    list->drawInstanced(draw_count, 1, start, 0);
  }
  ++r->frame_stats.drawn;
}

}  // namespace

// ---------------------------------------------------------------------------
// entry points

std::string RendererLabel() {
  if (g_failed) return "Native - stopped";
  if (!g_main) return "Native - starting";
  return fmt::format("Native {}x", g_scale);
}

// (always: the game draws only with the native renderer)
bool Enabled() { return true; }

// No switching to the emulated renderer any more.
bool CanSwitch() { return false; }

void SetNativeActive(bool) {}

std::string FailureReason() {
  std::lock_guard lock(g_fail_mutex);
  return g_fail_reason;
}

bool NativeActive() { return g_main && !g_failed && !g_suspended; }

void SetMatchScene(bool in_match) { g_match_scene = in_match; }

float CameraFovScale() { return g_fov_scale.load(std::memory_order_relaxed); }

void SetWindowSizeSource(std::function<std::pair<uint32_t, uint32_t>()> source) {
  g_window_size = std::move(source);
}

void Attach(rex::memory::Memory* memory) {
  g_memory = memory;
  // Debug: SVR2011_HASH_MEM=<hex physical>:<hex size>[,...] - the regions'
  // hashes every 2 s in the log (in either renderer mode).
  if (const char* spec = std::getenv("SVR2011_HASH_MEM")) {
    std::vector<std::pair<uint32_t, uint32_t>> regions;
    for (const char* s = spec; *s;) {
      char* e = nullptr;
      const uint32_t a = uint32_t(std::strtoul(s, &e, 16));
      if (!e || *e != ':') break;
      const uint32_t n = uint32_t(std::strtoul(e + 1, &e, 16));
      regions.emplace_back(a, n);
      s = *e == ',' ? e + 1 : e;
    }
    std::thread([memory, regions] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::string line;
        for (const auto& [a, n] : regions) {
          line += fmt::format(" {:08X}={:016X}", a, XXH3_64bits(memory->TranslatePhysical(a), n));
        }
        REXLOG_INFO("[svr2011] memory hashes:{}", line);
      }
    }).detach();
  }
  g_virtual_base = memory->virtual_membase();
  for (uint32_t i = 0; i < 4096; ++i) {
    const auto* heap = memory->LookupHeap(i << 20);
    g_virtual_offset[i] = heap ? heap->host_address_offset() : 0;
  }
  // The camera's aspect ratio (wide screens) is in read-only data.
  if (auto* heap = memory->LookupHeap(kAspectConstant)) {
    heap->Protect(kAspectConstant & ~0xFFFu, 0x1000,
                  rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  }
  // On a crash (usually the emulator aborting on a lost GPU device - the
  // same device as this renderer's), log the device's fault details.
  svr2011::SetCrashHook([] {
    Renderer* r = g_r;
    if (!r || !r->device) return;
    if (backend::DeviceLost(r->device.get())) backend::ReportDeviceLost(r->device.get());
  });
  if (REXCVAR_GET(native_renderer) != "main")
    REXLOG_WARN("native renderer: native_renderer = {} is no longer used - the game always draws natively",
                REXCVAR_GET(native_renderer));
  {
    g_main = true;
    g_scale = 1;  // set with the frame images' size at the first frame (ApplyOutputSettings)
    // The emulated GPU keeps running the command stream but no longer draws;
    // its presentation shows this renderer's frames (only: no fallback).
    rex::external_frame::SetHostDrawingDisabled(true);
    rex::external_frame::SetProvider([](rex::external_frame::Frame& f) {
      if (g_failed || g_suspended) return false;
      return backend::GetFrame(f);
    });
    REXLOG_INFO("native renderer: main renderer at {}x resolution (the only one: no emulated fallback)", g_scale);
  }
}

// Present: the front buffer (the last full-screen colour resolve of the
// frame; with SVR2011_NATIVE_STOP_AT_RESOLVE the main target before
// post-processing) is copied to the window.
// The pipeline showing a texture in the frame image (shaders/present.*.hlsl).
const plume::RenderPipeline* PresentPipeline(Renderer* r) {
  static std::unique_ptr<plume::RenderShader> vs, ps;
  static std::unique_ptr<plume::RenderPipeline> pso;
  static bool tried = false;
  if (tried) return pso.get();
  tried = true;
  const std::vector<uint8_t> vs_code = ReadShader("present.vs");
  const std::vector<uint8_t> ps_code = ReadShader("present.ps");
  if (vs_code.empty() || ps_code.empty()) {
    REXLOG_WARN("native renderer: present shaders missing");
    return nullptr;
  }
  plume::RenderGraphicsPipelineDesc d;
  d.pipelineLayout = r->layout.get();
  d.vertexShader = Compiled(r, vs_code, vs);
  d.pixelShader = Compiled(r, ps_code, ps);
  d.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  d.cullMode = plume::RenderCullMode::NONE;
  d.depthClipEnabled = true;
  d.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  d.renderTargetCount = 1;
  d.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
  if (d.vertexShader && d.pixelShader) pso = r->device->createGraphicsPipeline(d);
  if (pso && !backend::PipelineCreated(pso.get())) pso.reset();
  if (!pso) REXLOG_ERROR("native renderer: could not create the present pipeline");
  return pso.get();
}

plume::RenderTexture* FrameImage(Renderer* r) { return r->outputs[r->output_index].get(); }

// Draws the front buffer texture Swap named into the frame image; false if
// it can't be shown.
bool PresentFrontBuffer(Renderer* r, uint32_t front_buffer) {
  if (!front_buffer || g_stop_at_resolve) return false;
  const uint8_t* obj = Virtual(front_buffer);
  if ((Be32(obj) & 0xF) != 3) return false;  // not a texture
  uint32_t fetch[6];
  for (int k = 0; k < 6; ++k) fetch[k] = Be32(obj + 0x1C + 4 * k);
  // The object's fetch constant has a virtual address; textures:: expects
  // the physical one the GPU sees.
  const uint32_t physical = g_memory->GetPhysicalAddress(fetch[1] & 0xFFFFF000u);
  fetch[1] = (fetch[1] & 0xFFFu) | physical;
  const plume::RenderPipeline* pso = PresentPipeline(r);
  if (!pso) return false;
  const textures::Context ctx = TextureContext(r);
  const uint32_t srv = textures::Texture(ctx, fetch, 0);
  if (srv == UINT32_MAX) return false;
  const uint32_t sampler = textures::Sampler(ctx, fetch);
  const uint32_t w = (fetch[2] & 0x1FFF) + 1, h = ((fetch[2] >> 13) & 0x1FFF) + 1;
  auto [cpu, gpu] = Allocate(r, 256);
  if (!cpu) return false;
  const uint32_t indices[2] = {srv, sampler};
  const float scale[2] = {std::min(1.0f, float(kWidth) / float(w)),
                          std::min(1.0f, float(kHeight) / float(h))};
  const float out_size[2] = {float(g_out_w), float(g_out_h)};
  std::memcpy(cpu, indices, 8);
  std::memcpy(cpu + 8, scale, 8);
  std::memcpy(cpu + 16, out_size, 8);
  auto* list = r->list;
  Barrier(r, FrameImage(r), RenderTextureLayout::COLOR_WRITE);
  list->setFramebuffer(r->output_framebuffers[r->output_index].get());
  list->setGraphicsPipelineLayout(r->layout.get());
  BindTables(r, list);
  if (backend::CompactTables()) {
    // (compact tables: the texture and sampler in place 0 of the draw's sets)
    uint32_t places[2][16];
    std::fill(&places[0][0], &places[0][0] + 32, UINT32_MAX);
    places[0][0] = r->texture_base[0] + srv;
    places[1][0] = sampler;
    r->draw_sets[0] = CompactSet(r, 0, places[0]);
    r->draw_sets[1] = CompactSet(r, 1, places[1]);
    BindCompactSets(r, list);
    const uint32_t zero[2] = {0, 0};
    std::memcpy(cpu, zero, 8);
  }
  const RenderBufferReference constants[4] = {{}, {}, {}, gpu};
  BindConstants(r, constants);
  list->setPipeline(pso);
  list->setViewports(plume::RenderViewport(0, 0, out_size[0], out_size[1], 0, 1));
  if (g_frame_2d && g_wide > 1.0f) {
    // A 2D screen on a wide one: its 16:9 middle, black sides.
    list->clearColor(0, plume::RenderColor(0, 0, 0, 1));
    const int32_t mid = int32_t(std::lround(out_size[1] * kAspect16x9)), x0 = (int32_t(out_size[0]) - mid) / 2;
    list->setScissors(plume::RenderRect(x0, 0, x0 + mid, int32_t(out_size[1])));
  } else if (g_frame_2d && g_tall > 1.0f) {
    // On a tall one: its 16:9 middle, black above and below.
    list->clearColor(0, plume::RenderColor(0, 0, 0, 1));
    const int32_t mid = int32_t(std::lround(out_size[0] / kAspect16x9)), y0 = (int32_t(out_size[1]) - mid) / 2;
    list->setScissors(plume::RenderRect(0, y0, int32_t(out_size[0]), y0 + mid));
  } else {
    list->setScissors(plume::RenderRect(0, 0, int32_t(out_size[0]), int32_t(out_size[1])));
  }
  list->drawInstanced(3, 1, 0, 0);
  // Shader-readable between frames: the emulator's presentation reads it.
  Barrier(r, FrameImage(r), RenderTextureLayout::SHADER_READ);
  r->list_state = {};
  return true;
}

void OnPresent(uint32_t front_buffer) {
  svr2011::LatencyOnSwap();  // (frame_rate.h: test aid)
  std::lock_guard lock(g_mutex);
  Renderer* r = Get();
  if (!r) return;
  if (!BeginFrame(r)) return;
  auto* list = r->list;
  plume::RenderTexture* back = FrameImage(r);
  const bool shown = PresentFrontBuffer(r, front_buffer);
  plume::RenderTexture* src = nullptr;
  RenderTextureLayout* src_layout = nullptr;
  uint32_t w = 0, h = 0, src_host_w = 0, src_host_h = 0;
  RenderFormat src_format = RenderFormat::UNKNOWN;
  // SVR2011_NATIVE_PRESENT=<hex physical address>: show that resolve instead.
  static const uint32_t forced = [] {
    char* v = nullptr;
    size_t n = 0;
    uint32_t a = 0;
    if (_dupenv_s(&v, &n, "SVR2011_NATIVE_PRESENT") == 0 && v) {
      a = uint32_t(std::strtoul(v, nullptr, 16)) & 0xFFFFF000u;
      free(v);
    }
    return a;
  }();
  if (shown) {
    // done
  } else if (!g_stop_at_resolve && (forced || r->present_source)) {
    auto it = r->resolved.find(forced ? forced : r->present_source);
    if (it != r->resolved.end()) {
      src = it->second.resource.get();
      src_layout = &it->second.layout;
      w = it->second.width;
      h = it->second.height;
      src_host_w = it->second.host_w;
      src_host_h = it->second.host_h;
      src_format = it->second.format;
    }
  }
  if (!shown && !src && r->main_target) {
    src = r->main_target->resource.get();
    src_layout = &r->main_target->layout;
    w = r->main_target->width;
    h = r->main_target->height;
    src_host_w = r->main_target->host_w;
    src_host_h = r->main_target->host_h;
    src_format = r->main_target->resource_format;
  }
  // The frame image is RGBA8: copy only what is in the same format family.
  if (src && src_format != RenderFormat::R8G8B8A8_TYPELESS) {
    static bool logged = false;
    if (!logged) REXLOG_WARN("native renderer: front buffer format {} is not RGBA8", int(src_format));
    logged = true;
    src = nullptr;
  }
  if (src) {
    Barrier(r, back, RenderTextureLayout::COPY_DEST);
    const RenderTextureLayout before = *src_layout;
    Transition(r, src, *src_layout, RenderTextureLayout::COPY_SOURCE);
    // (a fallback: unscaled, cut to the frame image)
    const plume::RenderBox box(0, 0, int32_t(std::min(src_host_w ? src_host_w : std::min(w, kWidth) * g_scale, g_out_w)),
                               int32_t(std::min(src_host_h ? src_host_h : std::min(h, kHeight) * g_scale, g_out_h)));
    list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(back),
                            plume::RenderTextureCopyLocation::Subresource(src), 0, 0, 0, &box);
    if (before != RenderTextureLayout::UNKNOWN) Transition(r, src, *src_layout, before);
    Barrier(r, back, RenderTextureLayout::SHADER_READ);
  } else if (!shown) {
    Barrier(r, back, RenderTextureLayout::COLOR_WRITE);
    list->setFramebuffer(r->output_framebuffers[r->output_index].get());
    list->clearColor(0, plume::RenderColor(0.05f, 0.05f, 0.08f, 1.0f));
    Barrier(r, back, RenderTextureLayout::SHADER_READ);
  }
  r->list->end();
  // Debug: SVR2011_NATIVE_TEST_STALL=<frame> stalls the GPU queue for good at
  // that frame (as a stuck GPU job would), to test WaitForFrame's way out.
  static const uint64_t stall_frame = [] {
    char* v = nullptr;
    size_t n = 0;
    uint64_t f = ~0ull;
    if (_dupenv_s(&v, &n, "SVR2011_NATIVE_TEST_STALL") == 0 && v) {
      f = std::strtoull(v, nullptr, 10);
      free(v);
    }
    return f;
  }();
  if (r->frames == stall_frame) {
    backend::StallQueue(r->queue.get());
    REXLOG_WARN("native renderer: test stall at frame {}", r->frames);
  }
  plume::RenderCommandList* submit = r->list;
  if (r->recorder) {
    r->recorder->Finish();  // (the worker has recorded the whole frame)
    submit = r->recorder->target();
  }
  {
    ScopeTimer t(r->perf_submit_ms);  // (with the wait for the queue, shared with the emulator)
    r->queue->executeCommandLists(submit, r->fences[r->back_index].get());
  }
  r->submitted[r->back_index] = true;
  if (backend::DeviceLost(r->device.get())) {
    ReportDeviceRemoved(r);
    return;
  }
  // This frame, for the emulator's next swap (rex/external_frame.h).
  g_last_hidden = r->hidden_frame;
  if (!r->hidden_frame)  // (a frame not shown keeps the last one on screen)
    backend::PublishFrame(r->outputs[r->output_index], g_out_w, g_out_h, r->fences[r->back_index].get());
  r->frame_open = false;
  ++r->frames;
  g_frame_2d = (g_wide > 1.0f || g_tall > 1.0f) && r->frame_stats.drawn > 0 && r->frame_stats.wide_3d == 0;
  {
    static const std::string drawlog = [] {
      char* e = nullptr;
      size_t n = 0;
      std::string s;
      if (_dupenv_s(&e, &n, "SVR2011_DRAWLOG") == 0 && e) {
        s = e;
        free(e);
      }
      return s;
    }();
    if (g_drawlog && r->frames >= g_drawlog_end) {
      std::fclose(g_drawlog);
      g_drawlog = nullptr;
    }
    std::error_code ec;
    if (!drawlog.empty() && r->frames % 15 == 0 && std::filesystem::exists(drawlog + ".go", ec)) {
      std::filesystem::remove(drawlog + ".go", ec);
      g_drawlog = std::fopen(drawlog.c_str(), "w");
      g_drawlog_frame = r->frames;
      static const int frames = [] {
        char* e = nullptr;
        size_t n = 0;
        int f = 1;
        if (_dupenv_s(&e, &n, "SVR2011_DRAWLOG_FRAMES") == 0 && e) {
          f = std::max(1, std::atoi(e));
          free(e);
        }
        return f;
      }();
      g_drawlog_end = r->frames + frames;
    }
  }
  // Debug: SVR2011_NATIVE_DUMP_TRIGGER=<file>: when the file appears, the next
  // frame is dumped (as SVR2011_NATIVE_DUMP_FRAME) and the file removed.
  static const std::filesystem::path trigger = [] {
    char* e = nullptr;
    size_t n = 0;
    std::filesystem::path t;
    if (_dupenv_s(&e, &n, "SVR2011_NATIVE_DUMP_TRIGGER") == 0 && e) {
      t = e;
      free(e);
    }
    return t;
  }();
  if (!trigger.empty() && r->frames % 15 == 0) {
    std::error_code ec;
    if (std::filesystem::exists(trigger, ec)) {
      std::filesystem::remove(trigger, ec);
      g_dump_frame = r->frames + 1;
      g_dump_first = 0;
      g_dump_count = 100000;
      if (g_dump) {
        std::fclose(g_dump);
        g_dump = nullptr;
      }
    }
  }
  UpdateTitle(r);
  backend::DrainDebugMessages(r->device.get());
}

void OnDrawIndexed(const PPCContext& ctx) {
  std::lock_guard lock(g_mutex);
  Renderer* r = Get();
  if (!r) return;
  if (!BeginFrame(r)) return;
  Draw(r, ctx.r4.u32, static_cast<int32_t>(ctx.r5.u32), ctx.r6.u32, ctx.r7.u32, true);
}

// DrawVerticesUP(device, primitive, count, stride) returns the space (r3) the
// game then fills with the vertices; the draw is made at EndVertices.
void OnDrawUP(const PPCContext& args, uint32_t data) {
  std::lock_guard lock(g_mutex);
  Renderer* r = Get();
  if (!r) return;
  r->pending_up = {data != 0, args.r4.u32, args.r5.u32, args.r6.u32, data};
  static const bool log_up = EnvFlag("SVR2011_NATIVE_LOG_UP");
  if (log_up) {  // debug: VS c1 at DrawVerticesUP
    const uint8_t* c = Device() + 0x780 + 16;
    REXLOG_INFO("native renderer: UP begin vs {:08X} c1 {} {} {} {} (loaded {})",
                Be32(Device() + guest::kDeviceCurrentVertexShader), BeFloat(c), BeFloat(c + 4),
                BeFloat(c + 8), BeFloat(c + 12), g_constants_loaded[1]);
  }
}

void OnEndVertices() {
  std::lock_guard lock(g_mutex);
  Renderer* r = g_failed ? nullptr : g_r;
  if (!r || !r->pending_up.active) return;
  const PendingUP up = r->pending_up;
  r->pending_up.active = false;
  static const bool log_up = EnvFlag("SVR2011_NATIVE_LOG_UP");
  if (log_up) {  // debug: VS c1 at EndVertices
    const uint8_t* c = Device() + 0x780 + 16;
    REXLOG_INFO("native renderer: UP end c1 {} {} {} {} gpu {} {} {} {}", BeFloat(c), BeFloat(c + 4),
                BeFloat(c + 8), BeFloat(c + 12), g_gpu_constants[1][0], g_gpu_constants[1][1],
                g_gpu_constants[1][2], g_gpu_constants[1][3]);
  }
  if (!BeginFrame(r)) return;
  Draw(r, up.primitive, 0, 0, up.count, false, &up);
}

// D3DDevice_Clear(device, flags, target index (-1 all), rect left/top/right/
// bottom (r6-r9), colour* (r10, float4, NULL = black), z (f1), stencil (stack
// argument at caller r1 + 0x5C)): flags 0x0F colour targets, 0x10 depth,
// 0x20 stencil.
void OnClear(const PPCContext& ctx) {
  std::lock_guard lock(g_mutex);
  Renderer* r = Get();
  if (!r) return;
  if (!BeginFrame(r)) return;
  if (r->hidden_frame) return;  // (a frame not shown: SetHalfFrames)
  if (g_stop_at_resolve && g_resolved_this_frame) return;
  const Targets t = CurrentTargets(r);
  if (!BindTargets(r, t)) return;
  const uint32_t flags = ctx.r4.u32;
  if (flags & 0xF) {
    float colour[4] = {0, 0, 0, 0};
    if (ctx.r10.u32) {
      const uint8_t* c = Virtual(ctx.r10.u32);
      for (int i = 0; i < 4; ++i) colour[i] = BeFloat(c + 4 * i);
    }
    r->list->clearColor(0, plume::RenderColor(colour[0], colour[1], colour[2], colour[3]));
  }
  const bool clear_depth = flags & 0x10, clear_stencil = flags & 0x20;
  if ((clear_depth || clear_stencil) && t.depth) {
    const float z = std::clamp(static_cast<float>(ctx.f1.f64), 0.0f, 1.0f);
    const uint8_t stencil = uint8_t(Be32(Virtual(ctx.r1.u32 + 0x5C)));
    r->list->clearDepthStencil(clear_depth, clear_stencil, z, stencil);
  }
}


// ---------------------------------------------------------------------------
// resolve write-back

// Ends the frame's command list, runs it and waits for the GPU, then opens a
// new list for the rest of the frame (for reading GPU results on the CPU).
bool FlushFrameAndWait(Renderer* r) {
  if (!r->frame_open) return true;
  r->list->end();
  plume::RenderCommandList* submit = r->list;
  if (r->recorder) {
    r->recorder->Finish();
    submit = r->recorder->target();
  }
  r->queue->executeCommandLists(submit, r->fences[r->back_index].get());
  r->submitted[r->back_index] = true;
  if (!WaitForFrame(r, r->back_index, "resolve write-back")) return false;
  r->frame_open = false;
  return BeginFrame(r);
}

// Some resolves are read by the game's own code, not only sampled: Superstar
// Threads bakes an attire's textures on the GPU (2048x1024 and its mips)
// and then builds the attire texture from them on the CPU. The native
// renderer's resolves live only on the host GPU, so the game read whatever
// was in guest memory. A resolve that isn't one of the recurring per-frame
// ones (its destination not resolved in the last 30 frames) is copied back
// to guest memory at once: the frame so far runs, the image is read back and
// written at the guest's size, tiled and byte-swapped as the destination
// texture says (32-bit colour, k_8_8_8_8 only).
// The resolves the game's own code reads (the rest are only sampled, and each
// write-back waits for the GPU): Superstar Threads' attire bake (2048x1024
// and its mips, from a 2080-wide target), a Created Superstar's portrait
// (736x1280, compressed into the save and its Community Creations preview)
// and a highlight reel's cover (128x128 from a 160x128 target). A wider rule
// (any power-of-two resolve not repeated every frame) also took entrances'
// 512x512 and 64x64 effect targets, resolved every other frame: hundreds of
// GPU waits a second, the frame rate in single digits as entrances started.
bool CpuReadsResolve(uint32_t dst_w, uint32_t dst_h, uint32_t src_w, uint32_t src_h) {
  if (src_w == 2080 && src_h == 1024) return true;                    // Threads bake
  if (dst_h > dst_w && src_h == 1280) return true;                    // portrait
  if (dst_w == 128 && dst_h == 128 && src_w == 160 && src_h == 128) return true;  // reel cover
  return false;
}

bool WriteBackResolve(Renderer* r, uint32_t base, const ResolvedTexture& dst, const uint32_t fetch[6],
                      bool swap_rb, uint32_t src_w, uint32_t src_h) {
  if (dst.format != RenderFormat::R8G8B8A8_TYPELESS || (fetch[1] & 0x3F) != 6) return false;
  if (!CpuReadsResolve(dst.width, dst.height, src_w, src_h)) return false;
  const uint32_t guest_w = dst.width, guest_h = dst.height;
  const uint32_t host_w = dst.host_w ? dst.host_w : guest_w * dst.scale;
  const uint32_t host_h = dst.host_h ? dst.host_h : guest_h * dst.scale;
  const uint32_t row_texels = (host_w + 63) & ~63u;  // (256-byte rows)
  std::shared_ptr<plume::RenderBuffer> buffer =
      r->device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(row_texels) * host_h * 4));
  if (!buffer) return false;
  auto& layout = const_cast<ResolvedTexture&>(dst).layout;
  Transition(r, dst.resource.get(), layout, RenderTextureLayout::COPY_SOURCE);
  r->list->copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(
                                 buffer.get(), RenderFormat::R8G8B8A8_UNORM, host_w, host_h, 1, row_texels),
                             plume::RenderTextureCopyLocation::Subresource(dst.resource.get()));
  Transition(r, dst.resource.get(), layout, RenderTextureLayout::SHADER_READ);
  const auto wait_start = std::chrono::steady_clock::now();
  if (!FlushFrameAndWait(r)) return false;
  g_write_back_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
  ++g_write_backs;
  g_write_back_w = guest_w;
  g_write_back_h = guest_h;
  const auto* src = static_cast<const uint8_t*>(buffer->map());
  if (!src) return false;
  const bool tiled = fetch[0] >> 31;
  const uint32_t pitch = std::max(((fetch[0] >> 22) & 0x1FFu) * 32, guest_w);
  const uint32_t endian = (fetch[1] >> 6) & 3;
  const uint32_t mask = endian == 1 ? 1 : endian == 2 ? 3 : endian == 3 ? 2 : 0;
  auto* guest = const_cast<uint8_t*>(Physical(base));
  const uint64_t limit = 0x20000000ull - base;
  const uint32_t sx = std::max(1u, host_w / guest_w), sy = std::max(1u, host_h / guest_h);
  for (uint32_t y = 0; y < guest_h; ++y) {
    for (uint32_t x = 0; x < guest_w; ++x) {
      uint32_t sum[4] = {};  // (the host pixels of this guest pixel, averaged)
      for (uint32_t j = 0; j < sy; ++j) {
        const uint8_t* row = src + (size_t(std::min(y * sy + j, host_h - 1)) * row_texels) * 4;
        for (uint32_t i = 0; i < sx; ++i) {
          const uint8_t* q = row + size_t(std::min(x * sx + i, host_w - 1)) * 4;
          for (int c = 0; c < 4; ++c) sum[c] += q[c];
        }
      }
      const uint32_t n = sx * sy;
      uint8_t px[4];
      for (int c = 0; c < 4; ++c) px[c] = uint8_t((sum[c] + n / 2) / n);
      if (swap_rb) std::swap(px[0], px[2]);
      const int64_t off = tiled ? texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), pitch, 2)
                                : (int64_t(y) * pitch + x) * 4;
      if (off < 0 || uint64_t(off) + 4 > limit) continue;
      for (uint32_t c = 0; c < 4; ++c) guest[off + (c ^ mask)] = px[c];
    }
  }
  buffer->unmap();
  // (the span written: tiled textures are laid out in 32 x 32 tiles)
  const uint64_t rows = tiled ? (uint64_t(guest_h) + 31) & ~31ull : guest_h;
  textures::GuestWritten(base, uint32_t(std::min<uint64_t>(uint64_t(pitch) * rows * 4, limit)));
  static uint32_t logged = 0;
  if (logged++ < 32) {
    REXLOG_INFO("native renderer: resolve {:08X} ({}x{}) written back to guest memory", base, guest_w, guest_h);
  }
  return true;
}

// D3DDevice_Resolve(device, flags, ..., destination texture in r8): flags 0-3
// colour target n, 4 depth. Copies the current target into the texture the
// game will sample (always the whole surface from (0, 0) in this game).
void GuestWritten(uint32_t address, uint32_t size) {
  std::lock_guard lock(g_mutex);
  if (!g_memory || !size) return;
  textures::GuestWritten(g_memory->GetPhysicalAddress(address), size);
}

void OnResolve(const PPCContext& ctx) {
  std::lock_guard lock(g_mutex);
  Renderer* r = Get();
  if (!r || !ctx.r8.u32) return;
  if (!BeginFrame(r)) return;
  const Targets t = CurrentTargets(r);
  const bool depth = (ctx.r4.u32 & 7) == 4;
  if (t.color && t.color == r->main_target && !depth) {
    if (g_stop_at_resolve && g_resolved_this_frame) return;
    g_resolved_this_frame = true;
  }
  const uint8_t* obj = Virtual(ctx.r8.u32);
  if ((Be32(obj) & 0xF) != 3) return;  // not a texture
  uint32_t fetch[6];
  for (int k = 0; k < 6; ++k) fetch[k] = Be32(obj + 0x1C + 4 * k);
  // The object's fetch constant holds a guest virtual address (0xE0000000+
  // view); the fetch constants draws use hold the physical address.
  const uint32_t base = g_memory->GetPhysicalAddress(fetch[1] & 0xFFFFF000u);
  const uint32_t width = (fetch[2] & 0x1FFF) + 1, height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  plume::RenderTexture* src = nullptr;
  RenderTextureLayout* src_layout = nullptr;
  RenderFormat family = RenderFormat::UNKNOWN;
  uint32_t src_w = 0, src_h = 0, scale = g_scale, src_host_w = 0, src_host_h = 0;
  if (depth) {
    if (!t.depth) return;
    src = t.depth->resource.get();
    src_layout = &t.depth->layout;
    family = backend::DepthFormat();
    src_w = t.depth->width;
    src_h = t.depth->height;
    scale = t.depth->scale;
    src_host_w = t.depth->host_w;
    src_host_h = t.depth->host_h;
  } else {
    if (!t.color) return;
    src = t.color->resource.get();
    src_layout = &t.color->layout;
    family = t.color->resource_format;
    src_w = t.color->width;
    src_h = t.color->height;
    scale = t.color->scale;
    src_host_w = t.color->host_w;
    src_host_h = t.color->host_h;
  }
  {
    static std::unordered_map<uint64_t, bool> logged;
    static const bool log_all = EnvFlag("SVR2011_LOG_RESOLVES");  // (debug: every resolve)
    if (log_all || logged.size() < 256 &&
        logged.emplace((uint64_t(base) << 20) ^ (uint64_t(width) << 8) ^ height ^ (uint64_t(family) << 50), true).second) {
      REXLOG_INFO("native renderer: resolve flags {} -> texture {:08X} base {:08X} {}x{} fetch {:08X} {:08X} {:08X} from {}x{} format {}",
                  ctx.r4.u32, ctx.r8.u32, base, width, height, fetch[0], fetch[1], fetch[2], src_w,
                  src_h, int(family));
    }
  }
  // Depth can only be copied whole, so its copy has the target's size.
  const uint32_t dst_w = depth ? src_w : std::min(width, src_w);
  const uint32_t dst_h = depth ? src_h : std::min(height, src_h);
  // (the host width: the target's share of it)
  const uint32_t dst_host_w =
      depth || dst_w == src_w ? src_host_w : std::max(1u, uint32_t(uint64_t(src_host_w) * dst_w / src_w));
  const uint32_t dst_host_h =
      depth || dst_h == src_h ? src_host_h : std::max(1u, uint32_t(uint64_t(src_host_h) * dst_h / src_h));
  ResolvedTexture& dst = r->resolved[base];
  if (!dst.resource || dst.width != dst_w || dst.height != dst_h || dst.format != family ||
      dst.scale != scale || dst.host_w != dst_host_w || dst.host_h != dst_host_h) {
    if (dst.resource) {
      textures::ReleaseResolved(r->texture_context, dst.resource.get());
      Retire(r, std::move(dst.resource));
    }
    dst = {};
    plume::RenderTextureDesc d = plume::RenderTextureDesc::Texture2D(
        dst_host_w, dst_host_h, 1, family,
        depth && backend::ActiveApi() == backend::Api::kVulkan ? plume::RenderTextureFlag::DEPTH_TARGET
                                                                : plume::RenderTextureFlag::NONE);
    d.committed = true;
    dst.resource = r->device->createTexture(d);
    if (!dst.resource) {
      REXLOG_ERROR("native renderer: could not create a resolve texture");
      r->resolved.erase(base);
      return;
    }
    dst.resource->setName(fmt::format("resolve {:08X} {}x{}", base, dst_w, dst_h));
    dst.width = dst_w;
    dst.height = dst_h;
    dst.format = family;
    dst.scale = scale;
    dst.host_w = dst_host_w;
    dst.host_h = dst_host_h;
  }
  auto* list = r->list;
  const RenderTextureLayout src_before = *src_layout;
  Transition(r, src, *src_layout, RenderTextureLayout::COPY_SOURCE);
  Transition(r, dst.resource.get(), dst.layout, RenderTextureLayout::COPY_DEST);
  if (depth) {
    list->copyTexture(dst.resource.get(), src);
  } else {
    const plume::RenderBox box(0, 0, int32_t(dst_host_w), int32_t(dst_host_h));
    list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(dst.resource.get()),
                            plume::RenderTextureCopyLocation::Subresource(src), 0, 0, 0, &box);
  }
  Transition(r, dst.resource.get(), dst.layout, RenderTextureLayout::SHADER_READ);
  if (src_before != RenderTextureLayout::UNKNOWN) Transition(r, src, *src_layout, src_before);
  if (depth) {
    textures::RegisterResolved(base, dst.resource.get(), backend::DepthFormat(),
                               RenderFormat::UNKNOWN, 1, false, dst_w * dst_h * 4);
  } else {
    // Resolves to ARGB textures store red and blue swapped (copy_dest_swap),
    // undone by those textures' ZYXW fetch swizzle; the copy here is RGBA,
    // so the view swaps them back. (The mirror doesn't hold the resolve's
    // RB_COPY_DEST_INFO: the destination's swizzle tells.)
    const bool swap_rb = t.color->components == 4 && ((fetch[3] >> 1) & 7) == 2;
    textures::RegisterResolved(base, dst.resource.get(), t.color->view_format,
                               t.color->view_gamma_format, t.color->components, swap_rb,
                               dst_w * dst_h * 4);
    if (dst_w >= kWidth && dst_h >= kHeight) r->present_source = base;
    static const bool write_back = REXCVAR_GET(native_resolve_write_back);
    // Per-frame resolves (the scene, post-processing chains: resolved in
    // every frame) are left alone; bursts such as the attire bake - several
    // resolves into the same texture, one per attire part - are all copied.
    if (dst.last_frame != r->frames) {
      dst.streak = dst.last_frame != ~0ull && dst.last_frame + 1 == r->frames ? dst.streak + 1 : 0;
      dst.last_frame = r->frames;
    }
    const bool recurring = dst.streak >= 30 || t.color == r->main_target;
    if (write_back && !recurring && WriteBackResolve(r, base, dst, fetch, swap_rb, src_w, src_h)) {
      textures::ForgetResolved(base);  // (sampled from memory from now on)
    }
  }
  ++r->frame_stats.resolves;
}

// D3DDevice_SetRenderTarget(device, index, surface): the surface object has
// the EDRAM registers (+0x18 RB_SURFACE_INFO, +0x1C RB_COLOR_INFO) and the
// size (+0x24: width - 1 in bits 18-30, height - 1 in bits 3-15).
void OnSetRenderTarget(uint32_t index, uint32_t surface) {
  if (!g_memory || index != 0 || !surface) return;
  std::lock_guard lock(g_mutex);
  const uint8_t* obj = Virtual(surface);
  if ((Be32(obj) & 0xF) != 4) return;  // not a surface
  const uint32_t si = Be32(obj + 0x18), ci = Be32(obj + 0x1C), dims = Be32(obj + 0x24);
  const uint32_t w = ((dims >> 18) & 0x1FFF) + 1, h = ((dims >> 3) & 0x1FFF) + 1;
  if (w > 4096 || h > 4096) return;
  const uint32_t key = ((si & 0x3FFF) << 12) | (ci & 0xFFF);
  auto [it, added] = g_surface_sizes.try_emplace(key, w, h);
  if (added) {
    REXLOG_INFO("native renderer: surface {:08X}: {}x{} pitch {} tile {} format {}", surface, w,
                h, si & 0x3FFF, ci & 0xFFF, (ci >> 16) & 0xF);
  }
  it->second = {w, h};
}

void OnSetStreamSource(uint32_t stream, uint32_t buffer, uint32_t offset, uint32_t stride) {
  if (stream >= 15) return;  // slot 15 is the renderer's zero stream
  std::lock_guard lock(g_mutex);
  g_streams[stream] = {buffer, offset, stride};
}

void OnSetShaderConstants(bool pixel, uint32_t start, uint32_t count) {
  // (each draw takes all constants from the mirror; this marks the blocks)
  if (!count) return;
  const uint32_t a = std::min(start, 255u) / 4, b = std::min(start + count - 1, 255u) / 4;
  for (uint32_t block = a; block <= b; ++block) g_const_dirty[pixel ? 1 : 0] |= 1ull << (63 - block);
}

// D3D writes the dirty mirror ranges to the GPU: take them from the mirror.
void OnFlushShaderConstants(uint64_t mask, uint32_t reg) {
  // Each draw takes the constants from the mirror (GpuConstants), which D3D
  // has just updated; this tells it which stage changed.
  if (!mask) return;
  g_const_dirty[reg >= 0x4400 ? 1 : 0] |= mask;
  static int logged = 0;
  if (logged < 4 && reg != 0x4000 && reg != 0x4400) {
    ++logged;
    REXLOG_WARN("native renderer: constant flush at register {:04X} (mask {:016X})", reg, mask);
  }
}

// 82925D78(device, shader, base): the shader object's table (offset at +20)
// lists {u16 first float4, u16 dword count, u32 offset}; each block is loaded
// from base + offset into the constant file (LOAD_ALU_CONSTANT).
void OnLoadShaderConstants(uint32_t shader_object, uint32_t base) {
  if (!g_memory || !shader_object) return;
  std::lock_guard lock(g_mutex);
  const uint8_t* obj = Virtual(shader_object);
  const uint32_t table_offset = Be32(obj + 20);
  if (!table_offset) return;
  const uint8_t* table = obj + table_offset + 20;
  const uint8_t* end = table + Be32(obj + table_offset + 16);
  {
    // Log each distinct table once (which constant ranges come from memory).
    static std::unordered_map<uint64_t, int> seen;
    const uint64_t key = end > table ? XXH3_64bits(table, size_t(end - table)) : 0;
    if (seen.size() < 64 && seen.emplace(key, 0).second) {
      std::string entries;
      for (const uint8_t* e = table; e < end && entries.size() < 200; e += 8)
        entries += fmt::format(" [{}+{}]", Be16(e), Be16(e + 2));
      REXLOG_INFO("native renderer: constant table{} (object {:08X}, base {:08X})", entries,
                  shader_object, base);
    }
  }
  for (const uint8_t* e = table; e < end;) {
    const uint32_t first = Be16(e), dwords = Be16(e + 2);
    if (!dwords) break;
    const uint32_t offset = Be32(e + 4);
    e += 8;
    const uint8_t* src = ObjectData(base + offset);
    for (uint32_t d = 0; d < dwords; ++d) {
      const uint32_t index = first + d / 4;
      if (index >= 512) break;
      g_gpu_constants[index][d % 4] = BeFloat(src + 4 * d);
      if (!g_constants_loaded[index]) {
        g_constants_loaded[index] = true;
        g_loaded_pending[g_loaded_pending_count++] = uint16_t(index);
      }
    }
  }
}

void OnShaderCreated(uint32_t container, uint32_t object, bool pixel) {
  if (!g_memory || !container || !object) return;
  const uint8_t* p = Virtual(container);
  const uint32_t size = Be32(p + 4) + Be32(p + 8);
  if (size <= 0x24 || size > 0x100000) return;
  Shader s;
  s.hash = Fnv1a(p, size);
  s.pixel = pixel;
  std::lock_guard lock(g_shader_mutex);
  g_shaders[object] = std::move(s);
}

}  // namespace svr2011::native

namespace svr2011::native {
void SetHalfFrames(bool on) { g_half_frames = on; }
void SetHalfSuspended(bool on) { g_half_suspended = on; }
bool HalfFrames() { return g_half_frames.load() && Enabled(); }
bool LastFrameHidden() { return g_last_hidden.load(); }
}  // namespace svr2011::native
