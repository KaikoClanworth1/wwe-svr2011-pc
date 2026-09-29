// WWE SmackDown vs. Raw 2011 - native Direct3D 12 renderer (see native_renderer.h).
//
// Every draw is made with the game's own (converted) shaders, vertex/index data,
// constants and render state, into host render targets standing in for the
// EDRAM surfaces; resolves copy them into the textures the game samples
// (textures.cpp loads the rest from guest memory), and the front buffer is
// shown at Present.

#include "native/native_renderer.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <windows.h>
#include <tmmintrin.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <rex/cvar.h>
#include <rex/external_frame.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "crash_report.h"
#include "native/guest_d3d.h"
#include "native/textures.h"

// Default "main" (as the launcher): settings files without the key use it.
REXCVAR_DEFINE_STRING(native_renderer, "main", "GPU",
                      "Native D3D12 renderer: off, main (draws the game in the main window; the "
                      "emulated renderer only runs the GPU command stream, and takes over if the "
                      "native one fails), shadow (second window beside the emulated renderer, "
                      "for comparison)")
    .allowed({"off", "main", "shadow"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

using Microsoft::WRL::ComPtr;

namespace svr2011::native {

namespace {

// ---------------------------------------------------------------------------
// constants and small helpers

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr uint32_t kFrames = 3;                   // frames in flight = back buffers
// Main mode: frames drawn into these (RTV slots 0..kOutputs-1) and handed to
// the emulator's presentation; enough that one is never redrawn while the
// emulator's queue may still read it.
constexpr uint32_t kOutputs = 6;
constexpr uint64_t kRingSize = 64ull << 20;       // per-frame upload ring
// Mapped past the ring's end: shaders indexing constants may read up to a
// constant buffer view's 64 KB past an allocation near the end.
constexpr uint64_t kRingSlack = 64ull << 10;
constexpr uint32_t kSrvHeapSize = 16384;   // 0-2 placeholders, then textures
constexpr uint32_t kSamplerHeapSize = 2048;  // 0 default linear wrap, then per fetch state
constexpr uint32_t kRtvHeapSize = 256;       // back buffers, then render targets
constexpr uint32_t kDsvHeapSize = 64;
constexpr wchar_t kWindowTitle[] = L"SvR 2011 - native renderer";

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

void Swap32(uint8_t* dst, const uint8_t* src, size_t bytes) {
  const size_t n = bytes / 4;
  auto* d = reinterpret_cast<uint32_t*>(dst);
  auto* s = reinterpret_cast<const uint32_t*>(src);
  for (size_t i = 0; i < n; ++i) d[i] = _byteswap_ulong(s[i]);
}

uint64_t Fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// ---------------------------------------------------------------------------
// guest memory

rex::memory::Memory* g_memory = nullptr;

const uint8_t* Virtual(uint32_t address) { return g_memory->TranslateVirtual(address); }
const uint8_t* Physical(uint32_t address) { return g_memory->TranslatePhysical(address); }
// Addresses stored in the game's D3D objects (vertex/index buffers, textures)
// are guest *virtual* (0xA0000000+ / 0xE0000000+ physical-memory views); the
// 0xE... view is offset by a page on the host, so they must be translated as
// virtual. The fetch constants in the device's register mirror hold true
// physical addresses instead (Physical()).
const uint8_t* ObjectData(uint32_t address) { return g_memory->TranslateVirtual(address); }
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
};

struct Shader {
  uint64_t hash = 0;
  bool pixel = false;
  bool loaded = false;
  bool missing = false;
  std::vector<uint8_t> dxil[3];  // [0] base, [1] packed normals (vs), [2] alpha test (ps)
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

void LoadShader(Shader& s) {
  if (s.loaded) return;
  s.loaded = true;
  static const std::filesystem::path dir = ShaderDirectory();
  char name[64];
  std::snprintf(name, sizeof(name), "%016llX.%s", static_cast<unsigned long long>(s.hash),
                s.pixel ? "ps" : "vs");
  s.dxil[0] = ReadFile(dir / (std::string(name) + ".dxil"));
  s.dxil[s.pixel ? 2 : 1] = ReadFile(dir / (std::string(name) + (s.pixel ? ".s2" : ".s1") + ".dxil"));
  if (!s.pixel) {
    std::ifstream in(dir / (std::string(name) + ".inputs"));
    ShaderInput i;
    std::string type;
    while (in >> i.semantic >> i.index >> type) {
      i.is_uint = type == "uint4";
      s.inputs.push_back(i);
    }
  }
  {
    std::ifstream in(dir / (std::string(name) + ".textures"));
    uint32_t slot, dimension;
    while (in >> slot >> dimension) {
      if (slot < 32 && dimension < 3) s.textures.emplace_back(slot, dimension);
    }
  }
  s.missing = s.dxil[0].empty();
  if (s.missing) REXLOG_WARN("native renderer: no DXIL for shader {}", name);
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

struct UploadRing {
  ComPtr<ID3D12Resource> buffer;
  uint8_t* cpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
  uint64_t offset = 0;
};

struct CachedBuffer {
  ComPtr<ID3D12Resource> resource;
  uint64_t hash = 0;
  uint64_t checked_frame = ~0ull;
  uint32_t size = 0;
  uint32_t changes = 0;  // times the contents changed
  bool dynamic = false;  // changes often: converted into the ring when changed
  // The dynamic buffer's latest conversion in this frame's ring.
  uint64_t ring_frame = ~0ull, ring_hash = 0;
  D3D12_GPU_VIRTUAL_ADDRESS ring_va = 0;
};

struct Stream {
  uint32_t buffer = 0;  // guest vertex buffer object
  uint32_t offset = 0;
  uint32_t stride = 0;
};

struct Stats {
  uint32_t draws = 0, drawn = 0, skipped_target = 0, skipped_shader = 0, skipped_prim = 0;
  uint32_t targets = 0, resolves = 0;
};

// A render target: the host image of one EDRAM colour surface
// (base tile, pitch, format, height) - EDRAM itself isn't emulated.
struct ColorTarget {
  ComPtr<ID3D12Resource> resource;  // typeless family of `format`
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // the RTV format
  DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN, view_gamma_format = DXGI_FORMAT_UNKNOWN;
  uint32_t components = 4;
  uint32_t width = 0, height = 0;
  uint32_t rtv = 0;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  uint64_t used_frame = 0;
};

struct DepthTarget {
  ComPtr<ID3D12Resource> resource;
  uint32_t width = 0, height = 0;
  uint32_t dsv = 0;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
};

// The copy a resolve made of a target (what the game samples as a texture).
struct ResolvedTexture {
  ComPtr<ID3D12Resource> resource;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0, height = 0;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
};

// A DrawVerticesUP waiting for its vertices (the game writes them into the
// space the call returns, then calls EndVertices).
struct PendingUP {
  bool active = false;
  uint32_t primitive = 0, count = 0, stride = 0, data = 0;
};

struct Renderer {
  HWND hwnd = nullptr;
  std::thread window_thread;

  ComPtr<IDXGIFactory6> factory;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<IDXGISwapChain3> swapchain;
  ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap, srv_heap, sampler_heap;
  uint32_t rtv_size = 0;
  ComPtr<ID3D12Resource> back_buffers[kFrames];
  ComPtr<ID3D12Resource> outputs[kOutputs];  // main mode
  uint32_t output_index = 0;
  D3D12_RESOURCE_STATES rest_state = D3D12_RESOURCE_STATE_PRESENT;  // of the frame image
  uint32_t dsv_size = 0;
  uint32_t next_rtv = kOutputs, next_dsv = 0;
  std::unordered_map<uint64_t, ColorTarget> color_targets;
  std::unordered_map<uint64_t, DepthTarget> depth_targets;
  std::unordered_map<uint32_t, ResolvedTexture> resolved;  // destination base address ->
  uint32_t present_source = 0;  // the last full-screen colour resolve (the front buffer)
  ColorTarget* main_target = nullptr;  // the 1280-pitch target at tile 0
  PendingUP pending_up;
  ComPtr<ID3D12CommandAllocator> allocators[kFrames];
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE fence_event = nullptr;
  uint64_t fence_value = 0;
  uint64_t frame_fence[kFrames] = {};
  UploadRing rings[kFrames];
  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12Resource> null_2d, null_3d, null_cube;  // (0, 0, 0, 0) placeholders
  ComPtr<ID3D12Resource> zero_buffer;  // for vertex inputs the declaration lacks

  std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> pipelines;
  std::unordered_map<uint64_t, CachedBuffer> vertex_buffers;  // (address << 32 | size)
  std::deque<std::pair<uint64_t, ComPtr<ID3D12Resource>>> garbage;  // (fence, resource)

  bool frame_open = false;
  uint32_t back_index = 0;
  uint64_t frames = 0;

  std::chrono::steady_clock::time_point stat_start = std::chrono::steady_clock::now();
  uint32_t stat_frames = 0;
  Stats frame_stats, window_stats;
  // Performance log (main mode, every 5 s): time in the renderer's draw
  // translation, waiting for the GPU (frames in flight), and frame span.
  double perf_draw_ms = 0, perf_wait_ms = 0, perf_span_ms = 0;
  uint64_t perf_draws = 0, perf_drawn = 0;
  uint32_t perf_frames = 0;
  std::chrono::steady_clock::time_point perf_start = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point frame_begin;
  // The command list's state as Draw last set it, so unchanged state is not
  // recorded again (reset at every frame and by the other code that sets it).
  struct ListState {
    bool bound = false;  // descriptor heaps, root signature, descriptor tables
    ID3D12PipelineState* pso = nullptr;
    int topology = -1;
    uint64_t rtv = 0, dsv = 1;
    uint32_t stencil_ref = UINT32_MAX;
    float blend[4] = {-1, -1, -1, -1};
    D3D12_VIEWPORT viewport = {-1, -1, -1, -1, -1, -1};
    D3D12_RECT scissor = {-1, -1, -1, -1};
    D3D12_INDEX_BUFFER_VIEW ibv = {};
  } list_state;
  textures::Context texture_context;  // this frame's (BeginFrame)
  // The last upload of each constant range (vertex, pixel): reused while the
  // mirror's range is unchanged (consecutive draws of a model share them).
  struct ConstantUpload {
    uint64_t hash = 0;
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
  } constant_uploads[2];
  uint64_t perf_const_uploads = 0, perf_const_reused = 0;
};

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
// Main mode: every target, resolve copy and frame image has g_scale x g_scale
// times the guest's pixels (the emulator's resolution_scale setting); sizes the
// game sees and computes with stay the guest's.
uint32_t g_scale = 1;
// Main mode: the frame images' size - the window's 16:9 area, so the
// emulator's presentation shows them 1:1 (Present averages the scaled image
// down to it). Both follow the window and the anti-aliasing setting live
// (ApplyOutputSettings).
uint32_t g_out_w = kWidth, g_out_h = kHeight;
std::function<std::pair<uint32_t, uint32_t>()> g_window_size;
// The last finished frame (main mode), for the emulator's swap.
std::mutex g_frame_mutex;
ComPtr<ID3D12Resource> g_frame;
uint32_t g_frame_w = kWidth, g_frame_h = kHeight;
ComPtr<ID3D12Fence> g_frame_fence;
uint64_t g_frame_fence_value = 0;

// The native renderer stopped working: in main mode the emulated renderer
// draws again (it is the backup).
void Fail() {
  g_failed = true;
  if (g_main) {
    rex::external_frame::SetHostDrawingDisabled(false);
    REXLOG_WARN("native renderer: failed - the emulated renderer takes over");
  }
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

// Debug aids (environment): SVR2011_NATIVE_D3D_DEBUG=1 enables the D3D12
// debug layer and logs its messages; SVR2011_NATIVE_STOP_AT_RESOLVE=1 draws
// only up to the first Resolve of each frame (shows the scene before
// post-processing).
bool g_debug_layer = false;
bool g_stop_at_resolve = false;
bool g_resolved_this_frame = false;
// SVR2011_NATIVE_DEBUG_SOLID=1: every draw with a flat per-draw colour, no
// depth test, no culling (checks geometry independently of pixel shading).
bool g_debug_solid = false;
bool g_no_depth = false;  // SVR2011_NATIVE_NO_DEPTH=1
bool g_no_blend = false;  // SVR2011_NATIVE_NO_BLEND=1
// SVR2011_NATIVE_DUMP_FRAME=<frame>: describe the first 40 draws of that
// native frame in native_draws.txt (next to the log).
uint64_t g_dump_frame = ~0ull;
uint32_t g_dump_first = 0, g_dump_count = 40;  // SVR2011_NATIVE_DUMP_FIRST / _COUNT
FILE* g_dump = nullptr;
std::vector<uint8_t> g_debug_ps;

void DrainDebugMessages(ID3D12Device* device) {
  if (!g_debug_layer) return;
  ComPtr<ID3D12InfoQueue> q;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&q)))) return;
  static uint32_t logged = 0;
  const UINT64 n = q->GetNumStoredMessages();
  for (UINT64 i = 0; i < n && logged < 200; ++i) {
    SIZE_T len = 0;
    q->GetMessage(i, nullptr, &len);
    std::vector<uint8_t> buf(len);
    auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
    if (SUCCEEDED(q->GetMessage(i, m, &len)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
      REXLOG_WARN("native renderer: D3D12: {}", m->pDescription);
      ++logged;
    }
  }
  q->ClearStoredMessages();
}

// Names a resource for DRED's page-fault report.
template <typename... Args>
void Name(ID3D12Object* o, fmt::format_string<Args...> f, Args&&... args) {
  if (!o) return;
  const std::string n = fmt::format(f, std::forward<Args>(args)...);
  o->SetName(std::wstring(n.begin(), n.end()).c_str());
}

bool Check(HRESULT hr, const char* what) {
  if (FAILED(hr)) {
    REXLOG_ERROR("native renderer: {} failed ({:08X})", what, static_cast<uint32_t>(hr));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// window

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_CLOSE) {
    ShowWindow(hwnd, SW_MINIMIZE);  // the game owns the lifetime
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

POINT WindowPosition() {  // SVR2011_NATIVE_WINDOW_POS=x,y (tests park it off-screen)
  POINT p = {CW_USEDEFAULT, CW_USEDEFAULT};
  char* v = nullptr;
  size_t n = 0;
  if (_dupenv_s(&v, &n, "SVR2011_NATIVE_WINDOW_POS") == 0 && v) {
    std::sscanf(v, "%ld,%ld", &p.x, &p.y);
    free(v);
  }
  return p;
}

void WindowThread(Renderer* r, std::atomic<bool>* ready) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
  wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));      // svr2011.rc
  wc.lpszClassName = L"SvR2011NativeRenderer";
  RegisterClassW(&wc);
  RECT rc = {0, 0, LONG(kWidth), LONG(kHeight)};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  const POINT pos = WindowPosition();
  r->hwnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
                            pos.x, pos.y, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                            nullptr, wc.hInstance, nullptr);
  ShowWindow(r->hwnd, SW_SHOWNOACTIVATE);
  *ready = true;
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

// ---------------------------------------------------------------------------
// resources

// Every buffer this renderer created (GPU VA, size), live or since freed.
struct BufferLogEntry {
  D3D12_GPU_VIRTUAL_ADDRESS va;
  uint64_t size;
};
std::mutex g_buffer_log_mutex;
std::vector<BufferLogEntry> g_buffer_log;

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE heap,
                                    D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = heap;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> res;
  device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                  IID_PPV_ARGS(&res));
  if (res) {  // for the page-fault report
    std::lock_guard lock(g_buffer_log_mutex);
    if (g_buffer_log.size() < 65536) g_buffer_log.push_back({res->GetGPUVirtualAddress(), size});
  }
  return res;
}

// A 1x1 transparent black texture: what Xenia (and so the reference) samples
// for unbound / invalid fetch constants and unsupported textures.
ComPtr<ID3D12Resource> CreatePlaceholder(Renderer* r, D3D12_RESOURCE_DIMENSION dim, uint16_t array,
                                   uint32_t heap_index, D3D12_SRV_DIMENSION srv_dim) {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = dim;
  d.Width = 1;
  d.Height = 1;
  d.DepthOrArraySize = array;
  d.MipLevels = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  ComPtr<ID3D12Resource> tex;
  r->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));
  // Upload the texels through a temporary buffer.
  auto upload = CreateBuffer(r->device.Get(), 512 * array, D3D12_HEAP_TYPE_UPLOAD,
                             D3D12_RESOURCE_STATE_GENERIC_READ);
  uint8_t* p = nullptr;
  upload->Map(0, nullptr, reinterpret_cast<void**>(&p));
  std::memset(p, 0x00, 512 * array);
  upload->Unmap(0, nullptr);
  for (uint16_t i = 0; i < array; ++i) {
    D3D12_TEXTURE_COPY_LOCATION dst = {tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex = i;
    D3D12_TEXTURE_COPY_LOCATION src = {upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint.Offset = 512ull * i;  // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 256};
    r->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  D3D12_RESOURCE_BARRIER b = {};
  b.Transition.pResource = tex.Get();
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  r->list->ResourceBarrier(1, &b);
  r->garbage.emplace_back(1, upload);

  D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
  sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sv.ViewDimension = srv_dim;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (srv_dim == D3D12_SRV_DIMENSION_TEXTURE2D) sv.Texture2D.MipLevels = 1;
  if (srv_dim == D3D12_SRV_DIMENSION_TEXTURE3D) sv.Texture3D.MipLevels = 1;
  if (srv_dim == D3D12_SRV_DIMENSION_TEXTURECUBE) sv.TextureCube.MipLevels = 1;
  D3D12_CPU_DESCRIPTOR_HANDLE h = r->srv_heap->GetCPUDescriptorHandleForHeapStart();
  h.ptr += heap_index * r->device->GetDescriptorHandleIncrementSize(
                            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  r->device->CreateShaderResourceView(tex.Get(), &sv, h);
  return tex;
}

bool CreateRootSignature(Renderer* r) {
  D3D12_DESCRIPTOR_RANGE1 ranges[4] = {};
  for (int i = 0; i < 4; ++i) {
    ranges[i].RangeType = i == 3 ? D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
                                 : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[i].NumDescriptors = UINT_MAX;  // unbounded (bindless)
    ranges[i].BaseShaderRegister = 0;
    ranges[i].RegisterSpace = i;  // t0 space0/1/2 (2D/3D/cube), s0 space3
    ranges[i].Flags = i == 3 ? D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE
                             : D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                                   D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
  }
  D3D12_ROOT_PARAMETER1 params[8] = {};
  for (int i = 0; i < 3; ++i) {  // b0/b1/b2 space4: vertex, pixel, shared constants
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[i].Descriptor.ShaderRegister = i;
    params[i].Descriptor.RegisterSpace = 4;
    params[i].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  for (int i = 0; i < 4; ++i) {
    params[3 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3 + i].DescriptorTable.NumDescriptorRanges = 1;
    params[3 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
    params[3 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // b3 space4: debug constants
  params[7].Descriptor.ShaderRegister = 3;
  params[7].Descriptor.RegisterSpace = 4;
  params[7].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
  params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1.NumParameters = 8;
  desc.Desc_1_1.pParameters = params;
  desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> blob, error;
  if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error))) {
    REXLOG_ERROR("native renderer: root signature: {}",
                 error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    return false;
  }
  return Check(r->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              IID_PPV_ARGS(&r->root_signature)),
               "CreateRootSignature");
}


// Main mode: the frame images, g_out_w x g_out_h (RTV slots 0..kOutputs-1).
bool CreateOutputs(Renderer* r) {
  for (uint32_t i = 0; i < kOutputs; ++i) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = g_out_w;
    d.Height = g_out_h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    r->outputs[i].Reset();
    if (!Check(r->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, r->rest_state,
                                                  nullptr, IID_PPV_ARGS(&r->outputs[i])),
               "frame image")) {
      return false;
    }
    Name(r->outputs[i].Get(), "frame image {}", i);
    D3D12_CPU_DESCRIPTOR_HANDLE h = r->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += i * r->rtv_size;
    r->device->CreateRenderTargetView(r->outputs[i].Get(), nullptr, h);
  }
  return true;
}

bool Initialize() {
  if (!g_memory) {
    REXLOG_ERROR("native renderer: guest memory not attached");
    return false;
  }
  auto* r = new Renderer();
  if (!g_main) {  // shadow: its own window
    std::atomic<bool> ready{false};
    r->window_thread = std::thread(WindowThread, r, &ready);
    while (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  g_debug_layer = EnvFlag("SVR2011_NATIVE_D3D_DEBUG");
  g_stop_at_resolve = EnvFlag("SVR2011_NATIVE_STOP_AT_RESOLVE");
  g_debug_solid = EnvFlag("SVR2011_NATIVE_DEBUG_SOLID");
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
  if (g_debug_solid) g_debug_ps = ReadFile(ShaderDirectory() / "debug_solid.ps.dxil");
  // SVR2011_NATIVE_D3D_DEBUG logs the debug layer's messages for this device.
  // The layer itself must come from --d3d12_debug=true: enabling it once the
  // emulator's device exists removes that device.
  if (!Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&r->factory)), "CreateDXGIFactory2")) return false;
  // The emulator's GPU: its presentation shows these frames, which only
  // works on the same device (a laptop has two GPUs to pick from).
  ComPtr<IDXGIAdapter1> adapter;
  if (auto* emulator = static_cast<ID3D12Device*>(rex::external_frame::Device())) {
    r->factory->EnumAdapterByLuid(emulator->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
  }
  if (!adapter) {
    r->factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                           IID_PPV_ARGS(&adapter));
  }
  if (adapter) {
    DXGI_ADAPTER_DESC1 desc = {};
    adapter->GetDesc1(&desc);
    const std::wstring name = desc.Description;
    REXLOG_INFO("native renderer: GPU {}", std::string(name.begin(), name.end()));
  }
  // Device Removed Extended Data: if the GPU faults, the log names the
  // allocation (see ReportDeviceRemoved).
  {
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
      dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
  }
  if (!Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&r->device)),
             "D3D12CreateDevice"))
    return false;
  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (!Check(r->device->CreateCommandQueue(&qd, IID_PPV_ARGS(&r->queue)), "CreateCommandQueue"))
    return false;

  if (!g_main) {
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = kWidth;
  sd.Height = kHeight;
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = kFrames;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  ComPtr<IDXGISwapChain1> sc1;
  if (!Check(r->factory->CreateSwapChainForHwnd(r->queue.Get(), r->hwnd, &sd, nullptr, nullptr,
                                                &sc1),
             "CreateSwapChainForHwnd"))
    return false;
  sc1.As(&r->swapchain);
  r->factory->MakeWindowAssociation(r->hwnd, DXGI_MWA_NO_ALT_ENTER);
  }

  auto heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t n, bool visible,
                  ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = type;
    hd.NumDescriptors = n;
    hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    r->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out));
  };
  heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapSize, false, r->rtv_heap);
  heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapSize, false, r->dsv_heap);
  r->dsv_size = r->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize, true, r->srv_heap);
  heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize, true, r->sampler_heap);
  r->rtv_size = r->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  if (g_main) {
    // The frame images, read by the emulator's presentation: shader-readable
    // between frames.
    r->rest_state =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (!CreateOutputs(r)) return false;
  }
  for (uint32_t i = 0; i < kFrames; ++i) {
    if (!g_main) {
      r->swapchain->GetBuffer(i, IID_PPV_ARGS(&r->back_buffers[i]));
      D3D12_CPU_DESCRIPTOR_HANDLE h = r->rtv_heap->GetCPUDescriptorHandleForHeapStart();
      h.ptr += i * r->rtv_size;
      r->device->CreateRenderTargetView(r->back_buffers[i].Get(), nullptr, h);
    }
    r->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      IID_PPV_ARGS(&r->allocators[i]));
    r->rings[i].buffer = CreateBuffer(r->device.Get(), kRingSize + kRingSlack, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
    r->rings[i].buffer->Map(0, nullptr, reinterpret_cast<void**>(&r->rings[i].cpu));
    r->rings[i].gpu = r->rings[i].buffer->GetGPUVirtualAddress();
    Name(r->rings[i].buffer.Get(), "upload ring {}", i);
  }

  // Sampler 0: linear wrap (placeholder until sampler state is decoded).
  {
    D3D12_SAMPLER_DESC sd2 = {};
    sd2.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sd2.AddressU = sd2.AddressV = sd2.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sd2.MaxLOD = D3D12_FLOAT32_MAX;
    r->device->CreateSampler(&sd2, r->sampler_heap->GetCPUDescriptorHandleForHeapStart());
  }

  r->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r->allocators[0].Get(), nullptr,
                               IID_PPV_ARGS(&r->list));
  Name(r->list.Get(), "native frame");
  r->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r->fence));
  r->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  if (!CreateRootSignature(r)) return false;
  r->null_2d = CreatePlaceholder(r, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1, 0,
                            D3D12_SRV_DIMENSION_TEXTURE2D);
  r->null_3d = CreatePlaceholder(r, D3D12_RESOURCE_DIMENSION_TEXTURE3D, 1, 1,
                            D3D12_SRV_DIMENSION_TEXTURE3D);
  r->null_cube = CreatePlaceholder(r, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 6, 2,
                              D3D12_SRV_DIMENSION_TEXTURECUBE);
  r->zero_buffer = CreateBuffer(r->device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
  {
    uint8_t* p = nullptr;
    r->zero_buffer->Map(0, nullptr, reinterpret_cast<void**>(&p));
    std::memset(p, 0, 256);
    r->zero_buffer->Unmap(0, nullptr);
  }
  // Submit the setup work (texture uploads).
  r->list->Close();
  ID3D12CommandList* lists[] = {r->list.Get()};
  r->queue->ExecuteCommandLists(1, lists);
  r->queue->Signal(r->fence.Get(), ++r->fence_value);

  textures::Initialize(g_memory, 3, kSrvHeapSize - 3, 1, kSamplerHeapSize - 1);
  g_r = r;
  REXLOG_INFO("native renderer: D3D12 ready; shaders from {}", ShaderDirectory().string());
  // Without its converted shaders (native_shaders\ beside the exe, installed
  // by the launcher) nothing could be drawn: a black screen. Let the emulated
  // renderer draw instead.
  {
    std::error_code ec;
    if (!std::filesystem::exists(ShaderDirectory() / "present.vs.dxil", ec)) {
      REXLOG_ERROR("native renderer: its shaders are missing ({} has no present.vs.dxil) - reinstall "
                   "with the launcher to get the native_shaders folder",
                   ShaderDirectory().string());
      Fail();
    }
  }
  return true;
}

// The GPU faulted (device removed): logs why (and, with DRED, the faulting
// allocation) and turns the native renderer off for the rest of the run.
void ReportDeviceRemoved(Renderer* r, HRESULT hr) {
  const HRESULT reason = r->device->GetDeviceRemovedReason();
  REXLOG_ERROR("native renderer: Present failed ({:08X}), device removed reason {:08X} - native "
               "renderer disabled", uint32_t(hr), uint32_t(reason));
  ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (SUCCEEDED(r->device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
      for (auto* n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
        const uint32_t done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
        if (done >= n->BreadcrumbCount) continue;  // finished list
        REXLOG_ERROR("native renderer: list '{}' stopped after {} of {} ops",
                     n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "?", done,
                     n->BreadcrumbCount);
        for (uint32_t i = done > 6 ? done - 6 : 0; i < std::min(done + 3, n->BreadcrumbCount); ++i)
          REXLOG_ERROR("native renderer:   [{}] op {}{}", i, int(n->pCommandHistory[i]),
                       i == done ? "  <- faulting" : "");
      }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT fault = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault))) {
      REXLOG_ERROR("native renderer: page fault at GPU VA {:016X}", fault.PageFaultVA);
      {
        std::lock_guard lock(g_buffer_log_mutex);
        uint32_t shown = 0;
        for (const BufferLogEntry& b : g_buffer_log) {
          if (fault.PageFaultVA + (64ull << 20) >= b.va && fault.PageFaultVA < b.va + b.size + (64ull << 20) && shown++ < 16)
            REXLOG_ERROR("native renderer:   {} native buffer {:016X} size {:X}",
                         fault.PageFaultVA >= b.va && fault.PageFaultVA < b.va + b.size ? "IN" : "near",
                         b.va, b.size);
        }
        REXLOG_ERROR("native renderer:   ({} native buffers created)", g_buffer_log.size());
      }
      for (auto* n = fault.pHeadExistingAllocationNode; n; n = n->pNext)
        REXLOG_ERROR("native renderer:   existing allocation '{}' type {}",
                     n->ObjectNameA ? n->ObjectNameA : "?", int(n->AllocationType));
      for (auto* n = fault.pHeadRecentFreedAllocationNode; n; n = n->pNext)
        REXLOG_ERROR("native renderer:   recently freed allocation '{}' type {}",
                     n->ObjectNameA ? n->ObjectNameA : "?", int(n->AllocationType));
    }
  }
  Fail();
}

// Waits (CPU) until the renderer's fence reaches `value`. A GPU that stops
// answering must not hang the game: under Proton (vkd3d-proton) a stuck GPU
// job can leave the fence short for good, where Windows would remove the
// device. After kGpuTimeout the native renderer gives up - the emulated
// renderer takes over, and the fence is set from the CPU so the emulator's
// queue, which waits on it for the frame, runs on.
constexpr DWORD kGpuTimeoutMs = 3000;
bool WaitForFence(Renderer* r, uint64_t value, const char* what) {
  if (r->fence->GetCompletedValue() >= value) return true;
  r->fence->SetEventOnCompletion(value, r->fence_event);
  if (WaitForSingleObject(r->fence_event, kGpuTimeoutMs) == WAIT_OBJECT_0) return true;
  if (r->fence->GetCompletedValue() >= value) return true;
  const HRESULT reason = r->device->GetDeviceRemovedReason();
  REXLOG_ERROR("native renderer: GPU did not finish ({}) in {} ms - fence at {} of {}, device {:08X}",
               what, kGpuTimeoutMs, r->fence->GetCompletedValue(), value, uint32_t(reason));
  if (reason != S_OK) ReportDeviceRemoved(r, reason);
  Fail();
  r->fence->Signal(UINT64_MAX);  // releases the emulator's wait for the frame
  return false;
}

Renderer* Get() {
  std::call_once(g_init_once, [] {
    if (!Initialize()) Fail();
  });
  return g_failed ? nullptr : g_r;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = res;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  list->ResourceBarrier(1, &b);
}

// The image this frame is shown from: the window's back buffer, or in main
// mode the frame image handed to the emulator.
D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRtv(Renderer* r) {
  D3D12_CPU_DESCRIPTOR_HANDLE h = r->rtv_heap->GetCPUDescriptorHandleForHeapStart();
  h.ptr += (g_main ? r->output_index : r->back_index) * r->rtv_size;
  return h;
}

ID3D12Resource* BackBuffer(Renderer* r) {
  return g_main ? r->outputs[r->output_index].Get() : r->back_buffers[r->back_index].Get();
}

// Allocates `size` bytes of this frame's upload ring.
std::pair<uint8_t*, D3D12_GPU_VIRTUAL_ADDRESS> Allocate(Renderer* r, uint64_t size,
                                                        uint64_t align = 256) {
  UploadRing& ring = r->rings[r->back_index];
  ring.offset = (ring.offset + align - 1) & ~(align - 1);
  if (ring.offset + size > kRingSize) return {nullptr, 0};
  auto result = std::make_pair(ring.cpu + ring.offset, ring.gpu + ring.offset);
  ring.offset += size;
  return result;
}

textures::Context TextureContext(Renderer* r) {
  textures::Context ctx;
  ctx.device = r->device.Get();
  ctx.list = r->list.Get();
  ctx.srv_heap = r->srv_heap.Get();
  ctx.sampler_heap = r->sampler_heap.Get();
  ctx.frame = r->frames;
  ctx.retire = [r](ComPtr<ID3D12Resource> res) {
    r->garbage.emplace_back(r->fence_value + kFrames, std::move(res));
  };
  ctx.allocate = [r](uint64_t size, uint64_t align) -> textures::Context::UploadSpace {
    auto [cpu, gpu] = Allocate(r, size, align);
    if (!cpu) return {nullptr, nullptr, 0};
    UploadRing& ring = r->rings[r->back_index];
    return {cpu, ring.buffer.Get(), gpu - ring.gpu};
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
  uint32_t out_w = win_w, out_h = win_w * 9 / 16;
  if (out_h > win_h) out_h = win_h, out_w = win_h * 16 / 9;
  out_w = std::max(out_w & ~1u, 64u);
  out_h = std::max(out_h & ~1u, 36u);
  static bool aa = rex::cvar::Query<bool>("native_2x_msaa");
  static uint64_t aa_checked = 0;
  if (r->frames >= aa_checked + 30) {  // a cvar query isn't free: twice a second
    aa = rex::cvar::Query<bool>("native_2x_msaa");
    aa_checked = r->frames;
  }
  // Enough guest pixels for every output pixel - for two per axis with
  // anti-aliasing (2x2 supersampling at 1080p: 3x).
  const uint32_t need = out_h * (aa ? 2 : 1);
  const uint32_t scale = std::clamp<uint32_t>((need + kHeight - 1) / kHeight, 1, 4);
  if (scale == g_scale && out_w == g_out_w && out_h == g_out_h) return;

  // Idle: nothing in flight may still use what is replaced.
  r->queue->Signal(r->fence.Get(), ++r->fence_value);
  if (!WaitForFence(r, r->fence_value, "output change")) return;
  for (uint64_t& f : r->frame_fence) f = 0;
  r->garbage.clear();
  if (scale != g_scale) {
    r->color_targets.clear();
    r->depth_targets.clear();
    r->resolved.clear();
    r->main_target = nullptr;
    r->present_source = 0;
    r->next_rtv = kOutputs;
    r->next_dsv = 0;
    textures::ForgetResolved();
    g_scale = scale;
  }
  if (out_w != g_out_w || out_h != g_out_h) {
    g_out_w = out_w;
    g_out_h = out_h;
    CreateOutputs(r);
  }
  r->list_state = {};
  REXLOG_INFO("native renderer: output {}x{}, render scale {}x{}", g_out_w, g_out_h, g_scale,
              aa ? " (anti-aliasing)" : "");
}

// False: the GPU stopped answering and the native renderer gave up.
bool BeginFrame(Renderer* r) {
  if (r->frame_open) return true;
  if (g_main) ApplyOutputSettings(r);
  if (g_failed) return false;
  r->back_index = g_main ? uint32_t(r->frames % kFrames) : r->swapchain->GetCurrentBackBufferIndex();
  r->output_index = uint32_t(r->frames % kOutputs);
  r->frame_begin = std::chrono::steady_clock::now();
  if (r->fence->GetCompletedValue() < r->frame_fence[r->back_index]) {
    ScopeTimer wait(r->perf_wait_ms);
    if (!WaitForFence(r, r->frame_fence[r->back_index], "previous frame")) return false;
  }
  while (!r->garbage.empty() && r->garbage.front().first <= r->fence->GetCompletedValue()) {
    r->garbage.pop_front();
  }
  r->allocators[r->back_index]->Reset();
  r->list->Reset(r->allocators[r->back_index].Get(), nullptr);
  r->list_state = {};
  r->constant_uploads[0] = r->constant_uploads[1] = {};
  r->rings[r->back_index].offset = 0;
  r->frame_open = true;
  r->frame_stats = {};
  r->texture_context = TextureContext(r);
  g_resolved_this_frame = false;
  return true;
}

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
  const uint32_t n = std::max(1u, r->stat_frames);
  if (!r->hwnd) {  // main mode: no window of its own
    const double psecs = std::chrono::duration<double>(now - r->perf_start).count();
    if (psecs >= 5.0 && r->perf_frames) {
      const double f = r->perf_frames;
      REXLOG_INFO("native perf: {:.1f} fps, per frame: draw {:.2f} ms, gpu wait {:.2f} ms, "
                  "span {:.2f} ms, draws {:.0f} (drawn {:.0f})",
                  f / psecs, r->perf_draw_ms / f, r->perf_wait_ms / f, r->perf_span_ms / f,
                  r->perf_draws / f, r->perf_drawn / f);
      r->perf_draw_ms = r->perf_wait_ms = r->perf_span_ms = 0;
      r->perf_draws = r->perf_drawn = 0;
      r->perf_frames = 0;
      r->perf_start = now;
    }
    r->stat_start = now;
    r->stat_frames = 0;
    r->window_stats = {};
    return;
  }
  wchar_t title[256];
  const textures::Stats ts = textures::FrameStats();
  if (BlankedSlot(r) != -2) {
    swprintf_s(title, L"blank slot %d  |  %.0f FPS", BlankedSlot(r), r->stat_frames / secs);
    SetWindowTextW(r->hwnd, title);
    r->stat_start = now;
    r->stat_frames = 0;
    r->window_stats = {};
    return;
  }
  swprintf_s(title,
             L"%s  |  %.0f FPS  |  draws/frame %u: drawn %u, other targets %u, no shader %u, "
             L"unsupported %u  |  textures %u",
             kWindowTitle, r->stat_frames / secs, w.draws / n, w.drawn / n, w.skipped_target / n,
             w.skipped_shader / n, w.skipped_prim / n, ts.textures);
  SetWindowTextW(r->hwnd, title);
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
// height) gets a host render target, and each depth surface a D24S8 buffer;
// a Resolve copies a target into a texture the game then samples
// (textures::RegisterResolved), and Present shows the last full-screen
// colour resolve (the front buffer).

// Surface sizes from SetRenderTarget (the registers only hold the pitch):
// (pitch << 12 | EDRAM base tile) -> (width, height).
std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> g_surface_sizes;

struct TargetFormat {
  DXGI_FORMAT resource, rtv, view, view_gamma;
  uint32_t components;
};

// Xenos colour render target format (RB_COLOR_INFO bits 16-19) -> D3D12.
TargetFormat ColorFormat(uint32_t xenos) {
  switch (xenos) {
    case 1:  // 8_8_8_8_GAMMA: the hardware gamma-encodes on write
      return {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
              DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4};
    case 2:
    case 10:
      return {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM,
              DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN, 4};
    case 3:  // 2_10_10_10_FLOAT (7e3)
    case 5:
    case 7:
    case 12:
      return {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT,
              DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN, 4};
    case 4:
    case 6:
      return {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
              DXGI_FORMAT_UNKNOWN, 2};
    case 14:
      return {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
              DXGI_FORMAT_UNKNOWN, 1};
    case 15:
      return {DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
              DXGI_FORMAT_UNKNOWN, 2};
    default:  // 0: 8_8_8_8
      return {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
              DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 4};
  }
}

D3D12_CPU_DESCRIPTOR_HANDLE Handle(ID3D12DescriptorHeap* heap, uint32_t index, uint32_t size) {
  D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
  h.ptr += size_t(index) * size;
  return h;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
                D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES to) {
  if (state == to) return;
  Barrier(list, res, state, to);
  state = to;
}

ColorTarget* GetColorTarget(Renderer* r, uint32_t base, uint32_t pitch, uint32_t format,
                            uint32_t height) {
  const uint64_t key = uint64_t(base) | (uint64_t(pitch) << 12) | (uint64_t(format) << 26) |
                       (uint64_t(height) << 32);
  ColorTarget& t = r->color_targets[key];
  if (t.resource) return &t;
  if (r->next_rtv >= kRtvHeapSize) return nullptr;
  const TargetFormat f = ColorFormat(format);
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = pitch * g_scale;
  d.Height = height * g_scale;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = f.resource;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE cv = {};
  cv.Format = f.rtv;
  if (!Check(r->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                                IID_PPV_ARGS(&t.resource)),
             "render target")) {
    return nullptr;
  }
  Name(t.resource.Get(), "render target {}x{} fmt {} tile {}", pitch, height, format, base);
  t.format = f.rtv;
  t.view_format = f.view;
  t.view_gamma_format = f.view_gamma;
  t.components = f.components;
  t.width = pitch;
  t.height = height;
  t.rtv = r->next_rtv++;
  t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  D3D12_RENDER_TARGET_VIEW_DESC rv = {};
  rv.Format = f.rtv;
  rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  r->device->CreateRenderTargetView(t.resource.Get(), &rv,
                                    Handle(r->rtv_heap.Get(), t.rtv, r->rtv_size));
  REXLOG_INFO("native renderer: render target {}x{} format {} at EDRAM tile {}", pitch, height,
              format, base);
  return &t;
}

DepthTarget* GetDepthTarget(Renderer* r, uint32_t base, uint32_t pitch, uint32_t height) {
  const uint64_t key = uint64_t(base) | (uint64_t(pitch) << 12) | (uint64_t(height) << 32);
  DepthTarget& t = r->depth_targets[key];
  if (t.resource) return &t;
  if (r->next_dsv >= kDsvHeapSize) return nullptr;
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = pitch * g_scale;
  d.Height = height * g_scale;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = DXGI_FORMAT_R24G8_TYPELESS;  // copyable to a sampled depth texture
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE cv = {};
  cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  cv.DepthStencil.Depth = 1.0f;
  if (!Check(r->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                IID_PPV_ARGS(&t.resource)),
             "depth target")) {
    return nullptr;
  }
  Name(t.resource.Get(), "depth target {}x{} tile {}", pitch, height, base);
  t.width = pitch;
  t.height = height;
  t.dsv = r->next_dsv++;
  t.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
  dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  r->device->CreateDepthStencilView(t.resource.Get(), &dv,
                                    Handle(r->dsv_heap.Get(), t.dsv, r->dsv_size));
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
  auto* list = r->list.Get();
  Transition(list, t.color->resource.Get(), t.color->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Handle(r->rtv_heap.Get(), t.color->rtv, r->rtv_size);
  // (keyed on the resources too: a descriptor slot can be reused)
  const uint64_t ckey = rtv.ptr ^ reinterpret_cast<uintptr_t>(t.color->resource.Get());
  if (t.depth) {
    Transition(list, t.depth->resource.Get(), t.depth->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = Handle(r->dsv_heap.Get(), t.depth->dsv, r->dsv_size);
    const uint64_t dkey = dsv.ptr ^ reinterpret_cast<uintptr_t>(t.depth->resource.Get());
    if (r->list_state.rtv != ckey || r->list_state.dsv != dkey) {
      list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
      r->list_state.rtv = ckey;
      r->list_state.dsv = dkey;
    }
  } else if (r->list_state.rtv != ckey || r->list_state.dsv != 0) {
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    r->list_state.rtv = ckey;
    r->list_state.dsv = 0;
  }
  t.color->used_frame = r->frames;
  return true;
}

D3D12_COMPARISON_FUNC Compare(uint32_t xenos) {  // Xenos 0 never .. 7 always
  return static_cast<D3D12_COMPARISON_FUNC>(D3D12_COMPARISON_FUNC_NEVER + (xenos & 7));
}

D3D12_BLEND Blend(uint32_t xenos) {
  switch (xenos) {
    case 0: return D3D12_BLEND_ZERO;
    case 1: return D3D12_BLEND_ONE;
    case 4: return D3D12_BLEND_SRC_COLOR;
    case 5: return D3D12_BLEND_INV_SRC_COLOR;
    case 6: return D3D12_BLEND_SRC_ALPHA;
    case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_COLOR;
    case 9: return D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_DEST_ALPHA;
    case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
    case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
  }
}

D3D12_BLEND BlendAlpha(uint32_t xenos) {  // colour factors are invalid for alpha
  switch (Blend(xenos)) {
    case D3D12_BLEND_SRC_COLOR: return D3D12_BLEND_SRC_ALPHA;
    case D3D12_BLEND_INV_SRC_COLOR: return D3D12_BLEND_INV_SRC_ALPHA;
    case D3D12_BLEND_DEST_COLOR: return D3D12_BLEND_DEST_ALPHA;
    case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA;
    default: return Blend(xenos);
  }
}

D3D12_BLEND_OP BlendOp(uint32_t xenos) {
  switch (xenos) {
    case 1: return D3D12_BLEND_OP_SUBTRACT;
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
  }
}

D3D12_STENCIL_OP StencilOp(uint32_t xenos) {  // keep, zero, replace, incr_wrap, decr_wrap, invert, incr_sat, decr_sat
  static const D3D12_STENCIL_OP ops[8] = {
      D3D12_STENCIL_OP_KEEP,   D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_REPLACE,
      D3D12_STENCIL_OP_INCR,   D3D12_STENCIL_OP_DECR, D3D12_STENCIL_OP_INVERT,
      D3D12_STENCIL_OP_INCR_SAT, D3D12_STENCIL_OP_DECR_SAT};
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

DXGI_FORMAT VertexFormat(uint32_t type, bool as_uint) {
  const bool is_signed = type & 0x100;
  switch (type & 0x3F) {
    case 6:
      if (as_uint) return is_signed ? DXGI_FORMAT_R8G8B8A8_SINT : DXGI_FORMAT_R8G8B8A8_UINT;
      if (((type >> 10) & 0xFFF) == kSwizzleZYXW) return DXGI_FORMAT_B8G8R8A8_UNORM;  // D3DCOLOR
      return is_signed ? DXGI_FORMAT_R8G8B8A8_SNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    case 7:  return as_uint ? DXGI_FORMAT_R10G10B10A2_UINT : DXGI_FORMAT_R10G10B10A2_UNORM;
    case 16: case 17: return DXGI_FORMAT_R32_UINT;  // 10_11_11 / 11_11_10, unpacked in the shader
    case 25:
      if (as_uint) return is_signed ? DXGI_FORMAT_R16G16_SINT : DXGI_FORMAT_R16G16_UINT;
      return is_signed ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16_UNORM;
    case 26:
      if (as_uint) return is_signed ? DXGI_FORMAT_R16G16B16A16_SINT : DXGI_FORMAT_R16G16B16A16_UINT;
      return is_signed ? DXGI_FORMAT_R16G16B16A16_SNORM : DXGI_FORMAT_R16G16B16A16_UNORM;
    case 31: return DXGI_FORMAT_R16G16_FLOAT;
    case 32: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 33: case 36: return as_uint ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT;
    case 34: case 37: return as_uint ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32_FLOAT;
    case 35: case 38: return as_uint ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 57: return as_uint ? DXGI_FORMAT_R32G32B32_UINT : DXGI_FORMAT_R32G32B32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
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

// Vertex buffer object -> converted (little-endian) D3D12 buffer, cached per
// guest buffer (and layout) and re-converted when its contents change. A
// buffer that keeps changing is converted into the frame's upload ring at
// every draw instead (the game refills those between draws of a frame).
// Returns the GPU address (0 if none).
D3D12_GPU_VIRTUAL_ADDRESS VertexBuffer(Renderer* r, uint32_t object, uint32_t stream,
                                       const Stream& s, const std::vector<Element>& decl,
                                       uint32_t* out_size) {
  constexpr uint32_t kDynamicAfterChanges = 3;
  constexpr uint32_t kMaxDynamicSize = 512u << 10;
  const uint8_t* obj = Virtual(object);
  const uint32_t w0 = Be32(obj + 0x18), w1 = Be32(obj + 0x1C);
  const uint32_t address = w0 & ~3u;
  const uint32_t size = ((w1 >> 2) & 0xFFFFFF) * 4;
  *out_size = size;
  if (!size) return 0;
  uint32_t words[16], count;
  uint64_t layout = HalfWords(decl, stream, words, &count);
  if (layout) layout ^= (uint64_t(s.stride) << 32) ^ s.offset;
  CachedBuffer& c = r->vertex_buffers[((uint64_t(address) << 32) | size) ^ (layout * 0x9E3779B97F4A7C15ull)];
  const uint8_t* src = ObjectData(address);
  if (c.dynamic) {
    // Re-converted only when the contents changed since this frame's last
    // conversion (a hash is far cheaper than the conversion).
    const uint64_t hash = XXH3_64bits(src, size);
    if (c.ring_frame == r->frames && c.ring_hash == hash && c.ring_va) return c.ring_va;
    auto [cpu, gpu] = Allocate(r, size, 256);
    if (cpu) {
      ConvertVertices(cpu, src, size, s.stride, s.offset, decl, stream);
      c.ring_frame = r->frames;
      c.ring_hash = hash;
      c.ring_va = gpu;
      return gpu;
    }
    // Ring full: fall back to the cached buffer (re-checked below).
  }
  if (c.checked_frame == r->frames && c.resource) return c.resource->GetGPUVirtualAddress();
  const uint64_t hash = XXH3_64bits(src, size);
  c.checked_frame = r->frames;
  if (c.resource && c.hash == hash) return c.resource->GetGPUVirtualAddress();
  if (c.resource) {
    r->garbage.emplace_back(r->fence_value + kFrames, c.resource);
    c.resource.Reset();
    if (++c.changes >= kDynamicAfterChanges && size <= kMaxDynamicSize && !c.dynamic) {
      c.dynamic = true;
      return VertexBuffer(r, object, stream, s, decl, out_size);
    }
  }
  c.resource = CreateBuffer(r->device.Get(), size, D3D12_HEAP_TYPE_UPLOAD,
                            D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!c.resource) return 0;
  Name(c.resource.Get(), "vertex buffer {:08X} size {}", address, size);
  c.hash = hash;
  c.size = size;
  uint8_t* dst = nullptr;
  c.resource->Map(0, nullptr, reinterpret_cast<void**>(&dst));
  ConvertVertices(dst, src, size, s.stride, s.offset, decl, stream);  // 8in32 (all this game's)
  c.resource->Unmap(0, nullptr);
  return c.resource->GetGPUVirtualAddress();
}

struct PipelineKey {
  uint64_t vs, ps;
  uint8_t vs_variant, ps_variant, topology;
  uint8_t rt_format;  // DXGI_FORMAT of the render target
  uint32_t depth, blend, colour_mask, cull, stencil_ref_mask;
  uint32_t layout_hash;
  float bias_scale, bias_offset;  // polygon offset (0 when off)
};

ID3D12PipelineState* Pipeline(Renderer* r, Shader* vs, int vs_variant, Shader* ps,
                              int ps_variant, const std::vector<D3D12_INPUT_ELEMENT_DESC>& layout,
                              uint32_t layout_hash, D3D12_PRIMITIVE_TOPOLOGY_TYPE topology,
                              DXGI_FORMAT rt_format) {
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
  if (it != r->pipelines.end()) return it->second.Get();

  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = r->root_signature.Get();
  const auto& vsb = vs->dxil[vs_variant].empty() ? vs->dxil[0] : vs->dxil[vs_variant];
  d.VS = {vsb.data(), vsb.size()};
  // SVR2011_NATIVE_DEBUG_PS=<name> + SVR2011_NATIVE_DEBUG_PS_FOR=<hash>,...:
  // those pixel shaders are replaced by <shader dir>/<name>.ps.dxil.
  static const std::pair<std::vector<uint8_t>, std::string> debug_override = [] {
    std::pair<std::vector<uint8_t>, std::string> o;
    char* e = nullptr;
    size_t n = 0;
    if (_dupenv_s(&e, &n, "SVR2011_NATIVE_DEBUG_PS") == 0 && e) {
      o.first = ReadFile(ShaderDirectory() / (std::string(e) + ".ps.dxil"));
      free(e);
    }
    if (_dupenv_s(&e, &n, "SVR2011_NATIVE_DEBUG_PS_FOR") == 0 && e) {
      o.second = e;
      free(e);
    }
    return o;
  }();
  char ps_name[20];
  std::snprintf(ps_name, sizeof(ps_name), "%016llX", static_cast<unsigned long long>(key.ps));
  if (g_debug_solid && !g_debug_ps.empty()) {
    d.PS = {g_debug_ps.data(), g_debug_ps.size()};
  } else if (ps && !debug_override.first.empty() &&
             debug_override.second.find(ps_name) != std::string::npos) {
    d.PS = {debug_override.first.data(), debug_override.first.size()};
  } else if (ps) {
    const auto& psb = ps->dxil[ps_variant].empty() ? ps->dxil[0] : ps->dxil[ps_variant];
    d.PS = {psb.data(), psb.size()};
  }
  // Blend (render target 0).
  const uint32_t bc = key.blend;
  auto& bt = d.BlendState.RenderTarget[0];
  bt.SrcBlend = Blend(bc & 0x1F);
  bt.BlendOp = BlendOp((bc >> 5) & 7);
  bt.DestBlend = Blend((bc >> 8) & 0x1F);
  bt.SrcBlendAlpha = BlendAlpha((bc >> 16) & 0x1F);
  bt.BlendOpAlpha = BlendOp((bc >> 21) & 7);
  bt.DestBlendAlpha = BlendAlpha((bc >> 24) & 0x1F);
  bt.BlendEnable = !(bt.SrcBlend == D3D12_BLEND_ONE && bt.DestBlend == D3D12_BLEND_ZERO &&
                     bt.BlendOp == D3D12_BLEND_OP_ADD && bt.SrcBlendAlpha == D3D12_BLEND_ONE &&
                     bt.DestBlendAlpha == D3D12_BLEND_ZERO && bt.BlendOpAlpha == D3D12_BLEND_OP_ADD);
  bt.RenderTargetWriteMask = uint8_t(key.colour_mask);
  d.SampleMask = UINT_MAX;
  // Rasterizer.
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  const bool cull_front = key.cull & 1, cull_back = key.cull & 2, front_cw = key.cull & 4;
  d.RasterizerState.CullMode = cull_front ? D3D12_CULL_MODE_FRONT
                               : cull_back ? D3D12_CULL_MODE_BACK
                                           : D3D12_CULL_MODE_NONE;
  d.RasterizerState.FrontCounterClockwise = !front_cw;
  d.RasterizerState.DepthClipEnable = TRUE;
  // Xenos offsets are in depth units of the 24-bit buffer and slopes in 1/16
  // (as Xenia converts them).
  d.RasterizerState.DepthBias = INT(std::lround(double(key.bias_offset) * double(1 << 24)));
  d.RasterizerState.SlopeScaledDepthBias = key.bias_scale * (1.0f / 16.0f);
  // Depth / stencil.
  const uint32_t dc = key.depth;
  d.DepthStencilState.DepthEnable = (dc >> 1) & 1;
  d.DepthStencilState.DepthWriteMask =
      (dc >> 2) & 1 ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  d.DepthStencilState.DepthFunc = Compare((dc >> 4) & 7);
  d.DepthStencilState.StencilEnable = dc & 1;
  d.DepthStencilState.StencilReadMask = uint8_t(key.stencil_ref_mask >> 8);
  d.DepthStencilState.StencilWriteMask = uint8_t(key.stencil_ref_mask >> 16);
  d.DepthStencilState.FrontFace = {StencilOp((dc >> 11) & 7), StencilOp((dc >> 17) & 7),
                                   StencilOp((dc >> 14) & 7), Compare((dc >> 8) & 7)};
  d.DepthStencilState.BackFace = (dc >> 7) & 1
                                     ? D3D12_DEPTH_STENCILOP_DESC{StencilOp((dc >> 23) & 7),
                                                                  StencilOp((dc >> 29) & 7),
                                                                  StencilOp((dc >> 26) & 7),
                                                                  Compare((dc >> 20) & 7)}
                                     : d.DepthStencilState.FrontFace;
  if (g_no_depth) {
    d.DepthStencilState.DepthEnable = FALSE;
    d.DepthStencilState.StencilEnable = FALSE;
  }
  if (g_no_blend) {
    bt.BlendEnable = FALSE;
    bt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  }
  if (g_debug_solid) {
    bt.BlendEnable = FALSE;
    bt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.DepthStencilState.DepthEnable = FALSE;
    d.DepthStencilState.StencilEnable = FALSE;
  }
  d.InputLayout = {layout.data(), static_cast<UINT>(layout.size())};
  d.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
  d.PrimitiveTopologyType = topology;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = rt_format;
  d.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
  d.SampleDesc.Count = 1;
  // Logged before it is built: a GPU driver that crashes compiling it (seen
  // under Proton) leaves this as the log's last pipeline.
  REXLOG_INFO("native renderer: pipeline {} vs {:016X}.{} ps {:016X}.{} rt {} blend {:08X}{} mask {:X} "
              "depth {:08X} cull {} bias {}/{} topology {}",
              r->pipelines.size(), key.vs, key.vs_variant, key.ps, key.ps_variant, int(rt_format),
              key.blend, bt.BlendEnable ? " on" : "", key.colour_mask, key.depth, key.cull,
              key.bias_offset, key.bias_scale, int(topology));
  ComPtr<ID3D12PipelineState> pso;
  if (FAILED(r->device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
    REXLOG_WARN("native renderer: pipeline creation failed (vs {:016X} ps {:016X})", key.vs,
                key.ps);
  }
  r->pipelines[h] = pso;
  return pso.Get();
}

// Copies the shader constants from the device mirror, little-endian.
D3D12_GPU_VIRTUAL_ADDRESS Constants(Renderer* r, uint32_t device_offset, uint32_t bytes) {
  auto [cpu, gpu] = Allocate(r, bytes);
  if (!cpu) return 0;
  Swap32(cpu, Device() + device_offset, bytes);
  return gpu;
}

// Uploads float4 constants [first, first + bytes/16) of the GPU constant file.
// The device mirror holds them all (the XDK also writes it inline, with no
// hookable call), big-endian: byte-swapped 16 bytes at a time straight into
// the upload; the constants loaded from memory for this draw (so far only
// the shaders' literal c252-c255) take precedence.
D3D12_GPU_VIRTUAL_ADDRESS GpuConstants(Renderer* r, uint32_t first, uint32_t bytes) {
  const uint32_t count = bytes / 16;
  const uint8_t* mirror = Device() + 0x780 + 16 * first;
  bool loaded_here = false;
  for (uint32_t k = 0; k < g_loaded_draw_count; ++k)
    if (g_loaded_draw[k] >= first && g_loaded_draw[k] < first + count) loaded_here = true;
  Renderer::ConstantUpload& last = r->constant_uploads[first ? 1 : 0];
  const uint64_t hash = loaded_here ? 0 : XXH3_64bits(mirror, bytes);
  if (hash && last.va && last.hash == hash) {
    ++r->perf_const_reused;
    return last.va;
  }
  auto [cpu, gpu] = Allocate(r, bytes);
  if (!cpu) return 0;
  ++r->perf_const_uploads;
  last = {hash, gpu};
  const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  for (uint32_t i = 0; i < count; ++i) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(mirror + 16 * i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(cpu + 16 * i), _mm_shuffle_epi8(v, swap));
  }
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
D3D12_GPU_VIRTUAL_ADDRESS SharedConstants(Renderer* r, bool alpha_test, const Shader* vs,
                                          const Shader* ps, const float ndc[4]) {
  auto [cpu, gpu] = Allocate(r, kSharedConstantsBytes);
  if (!cpu) return 0;
  auto* u = reinterpret_cast<uint32_t*>(cpu);
  std::memset(cpu, 0, kSharedConstantsBytes);
  // g_ResourceIndices[32] (uint4): 2D, 3D, cube, sampler blocks of 8 x uint4.
  for (uint32_t slot = 0; slot < 32; ++slot) {
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
      if (srv == UINT32_MAX) continue;
      u[(dimension * 8 + slot / 4) * 4 + slot % 4] = srv;
      u[(3 * 8 + slot / 4) * 4 + slot % 4] = textures::Sampler(ctx, fetch);
    }
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

// `up`: a DrawVerticesUP (stream 0 is the game's vertex data, not a buffer).
void Draw(Renderer* r, uint32_t primitive, int32_t base_vertex, uint32_t start, uint32_t count,
          bool indexed, const PendingUP* up = nullptr) {
  ScopeTimer timer(r->perf_draw_ms);
  ApplyPendingConstants();
  ++r->frame_stats.draws;
  if (g_stop_at_resolve && g_resolved_this_frame) return;
  const Targets targets = CurrentTargets(r);
  if (!targets.color) {
    ++r->frame_stats.skipped_target;
    return;
  }
  Shader* vs = FindShader(Be32(Device() + guest::kDeviceCurrentVertexShader));
  Shader* ps = FindShader(Be32(Device() + guest::kDeviceCurrentPixelShader));
  if (!vs || !ps) {
    ++r->frame_stats.skipped_shader;
    return;
  }

  // Topology; quad lists become triangle lists.
  D3D12_PRIMITIVE_TOPOLOGY topology;
  D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  bool quads = false;
  switch (primitive) {
    case 1: topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; break;
    case 2: topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST; topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
    case 3: topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
    case 4: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    case 6: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    case 13: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; quads = true; break;
    default:
      ++r->frame_stats.skipped_prim;
      return;
  }

  // Input layout: every input the vertex shader declares, from the game's
  // vertex declaration (or a zero stream when the declaration lacks it).
  const std::vector<Element> decl = Declaration();
  std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
  bool packed_normals = false;
  bool uses_zero_stream = false;
  for (const ShaderInput& in : vs->inputs) {
    D3D12_INPUT_ELEMENT_DESC e = {};
    e.SemanticName = in.semantic.c_str();
    e.SemanticIndex = in.index;
    e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    const Element* found = nullptr;
    for (const Element& el : decl) {
      if (el.usage < std::size(kUsageNames) && in.semantic == kUsageNames[el.usage] &&
          el.index == in.index) {
        found = &el;
        break;
      }
    }
    DXGI_FORMAT fmt = found ? VertexFormat(found->type, in.is_uint) : DXGI_FORMAT_UNKNOWN;
    if (found && !SwizzleSupported(found->type)) {
      static std::unordered_map<uint32_t, bool> logged;
      if (logged.emplace(found->type, true).second)
        REXLOG_WARN("native renderer: vertex type {:08X} has an unsupported swizzle", found->type);
    }
    if (found && fmt != DXGI_FORMAT_UNKNOWN && found->stream < 15) {
      e.Format = fmt;
      e.InputSlot = found->stream;
      e.AlignedByteOffset = found->offset;
      if (in.semantic == "NORMAL" && (found->type & 0x3F) == 17) packed_normals = true;
    } else {
      e.Format = in.is_uint ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
      e.InputSlot = 15;  // zero stream
      e.AlignedByteOffset = 0;
      uses_zero_stream = true;
    }
    layout.push_back(e);
  }
  uint32_t layout_hash = 0;
  for (const auto& e : layout) {
    layout_hash = layout_hash * 31 + e.Format * 7 + e.InputSlot * 131 + e.AlignedByteOffset * 17 +
                  e.SemanticIndex;
  }

  // Alpha test: RB_COLORCONTROL bit 3 with a "greater" function (the only
  // kind the converted shaders implement: discard below the threshold).
  const uint32_t cc = Reg(RB_COLORCONTROL);
  const bool alpha_test = (cc & 8) && ((cc & 7) == 4 || (cc & 7) == 6);

  ID3D12PipelineState* pso =
      Pipeline(r, vs, packed_normals ? 1 : 0, ps, alpha_test ? 2 : 0, layout, layout_hash,
               topology_type, targets.color->format);
  if (!pso) {
    ++r->frame_stats.skipped_shader;
    return;
  }

  // Vertex buffers for the streams the layout uses.
  D3D12_VERTEX_BUFFER_VIEW views[16] = {};
  uint32_t max_slot = 0;
  if (up) {
    // The game's vertices, converted into the ring.
    const uint64_t bytes = uint64_t(up->count) * up->stride;
    auto [cpu, gpu] = Allocate(r, bytes, 16);
    if (!cpu || !bytes) return;
    ConvertVertices(cpu, ObjectData(up->data), bytes, up->stride, 0, decl, 0);
    views[0] = {gpu, uint32_t(bytes), up->stride};
  }
  for (const auto& e : layout) {
    max_slot = std::max(max_slot, e.InputSlot);
    if (e.InputSlot == 15 || up) continue;
    const Stream& s = g_streams[e.InputSlot];
    if (!s.buffer || views[e.InputSlot].BufferLocation) continue;
    uint32_t size = 0;
    const D3D12_GPU_VIRTUAL_ADDRESS vb = VertexBuffer(r, s.buffer, e.InputSlot, s, decl, &size);
    if (!vb || s.offset >= size) continue;
    views[e.InputSlot] = {vb + s.offset, size - s.offset, s.stride};
  }
  if (uses_zero_stream) {
    views[15] = {r->zero_buffer->GetGPUVirtualAddress(), 256, 0};
  }

  // Indices (16-bit big-endian), converted into the ring.
  uint32_t draw_count = count;
  bool draw_indexed = indexed;
  int32_t draw_base = base_vertex;
  D3D12_INDEX_BUFFER_VIEW ibv = {};
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
      for (uint32_t i = 0; i < count; ++i) dst[i] = Be16(src + 2 * i);
    }
    ibv = {gpu, out_count * 2, DXGI_FORMAT_R16_UINT};
    draw_count = out_count;
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
    ibv = {gpu, out_count * 2, DXGI_FORMAT_R16_UINT};
    draw_count = out_count;
    draw_indexed = true;
    draw_base = int32_t(start);
  }

  // Viewport from the Xenos viewport transform.
  const float xs = RegFloat(PA_CL_VPORT_XSCALE), xo = RegFloat(PA_CL_VPORT_XOFFSET);
  const float ys = RegFloat(PA_CL_VPORT_YSCALE), yo = RegFloat(PA_CL_VPORT_YOFFSET);
  const float zs = RegFloat(PA_CL_VPORT_ZSCALE), zo = RegFloat(PA_CL_VPORT_ZOFFSET);
  D3D12_VIEWPORT vp = {xo - std::fabs(xs), yo - std::fabs(ys), 2 * std::fabs(xs),
                       2 * std::fabs(ys), zo, zo + zs};
  const float tw = float(targets.color->width), th = float(targets.color->height);
  // The shaders' position goes through g_NdcScale / g_HalfPixelOffset: with
  // the viewport transform off (PA_CL_VTE_CNTL) positions are in pixels of
  // the target; with it on, the Xenos viewport's flips are kept (D3D12
  // viewports can't flip). D3D9-style pixel centres (PA_SU_VTX_CNTL bit 0
  // clear) put pixel centres on integer coordinates: half a pixel is added.
  const uint32_t vte = Reg(PA_CL_VTE_CNTL);
  float ndc[4] = {xs < 0 ? -1.0f : 1.0f, ys > 0 ? -1.0f : 1.0f, 0.0f, 0.0f};
  if (!(vte & 1)) {  // x scale/offset off
    vp.TopLeftX = 0;
    vp.Width = tw;
    ndc[0] = 2.0f / tw;
    ndc[2] = -1.0f;
  }
  if (!(vte & 4)) {  // y scale/offset off
    vp.TopLeftY = 0;
    vp.Height = th;
    ndc[1] = -2.0f / th;
    ndc[3] = 1.0f;
  }
  if (!(vte & 0x10)) {  // z scale/offset off
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
  }
  if (vp.Width <= 0 || vp.Height <= 0) vp = {0, 0, tw, th, 0, 1};
  if (!(Reg(PA_SU_VTX_CNTL) & 1)) {
    ndc[2] += 1.0f / vp.Width;
    ndc[3] -= 1.0f / vp.Height;
  }
  vp.MinDepth = std::clamp(vp.MinDepth, 0.0f, 1.0f);
  vp.MaxDepth = std::clamp(vp.MaxDepth, 0.0f, 1.0f);
  // (ndc above is in guest pixels; the target has g_scale times as many.)
  vp.TopLeftX *= g_scale;
  vp.TopLeftY *= g_scale;
  vp.Width *= g_scale;
  vp.Height *= g_scale;
  // Window scissor: D3D keeps it in the mirrored 0x2000 group (0x2011 top-left,
  // 0x2012 bottom-right; x in bits 0-14, y in bits 16-30), already clamped to
  // the scissor rect when the game enables one (device +0x2F00).
  const uint32_t sc_tl = Reg(PA_SC_WINDOW_SCISSOR_TL), sc_br = Reg(PA_SC_WINDOW_SCISSOR_BR);
  const D3D12_RECT scissor = {
      LONG(std::min<uint32_t>(sc_tl & 0x7FFF, targets.color->width) * g_scale),
      LONG(std::min<uint32_t>((sc_tl >> 16) & 0x7FFF, targets.color->height) * g_scale),
      LONG(std::min<uint32_t>(sc_br & 0x7FFF, targets.color->width) * g_scale),
      LONG(std::min<uint32_t>((sc_br >> 16) & 0x7FFF, targets.color->height) * g_scale)};

  if (r->frames == g_dump_frame && r->frame_stats.drawn >= g_dump_first &&
      r->frame_stats.drawn < g_dump_first + g_dump_count) {
    if (!g_dump) g_dump = std::fopen("native_draws.txt", "w");
    FILE* f = g_dump;
    std::fprintf(f, "=== draw %u: prim %u base %d start %u count %u indexed %d | vs %016llX v%d ps %016llX v%d\n",
                 r->frame_stats.drawn, primitive, base_vertex, start, count, indexed,
                 (unsigned long long)vs->hash, packed_normals ? 1 : 0, (unsigned long long)ps->hash,
                 alpha_test ? 2 : 0);
    std::fprintf(f, "viewport %.1f %.1f %.1f %.1f z %.3f..%.3f  depthctl %08X cull %u\n", vp.TopLeftX,
                 vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth, Reg(RB_DEPTHCONTROL),
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
      std::fprintf(f, "  input %s%u fmt %d slot %u +%u\n", e.SemanticName, e.SemanticIndex, e.Format,
                   e.InputSlot, e.AlignedByteOffset);
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
    for (uint32_t k = 0; k < 512; ++k)
      std::fprintf(f, "  %s%u = %g %g %g %g\n", k < 256 ? "gc" : "pc", k & 255,
                   g_gpu_constants[k][0], g_gpu_constants[k][1], g_gpu_constants[k][2],
                   g_gpu_constants[k][3]);
    std::fflush(f);
  }

  auto* list = r->list.Get();
  auto& ls = r->list_state;
  if (!ls.bound) {
    ID3D12DescriptorHeap* heaps[] = {r->srv_heap.Get(), r->sampler_heap.Get()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetGraphicsRootSignature(r->root_signature.Get());
    for (int i = 0; i < 3; ++i) {
      list->SetGraphicsRootDescriptorTable(3 + i, r->srv_heap->GetGPUDescriptorHandleForHeapStart());
    }
    list->SetGraphicsRootDescriptorTable(6, r->sampler_heap->GetGPUDescriptorHandleForHeapStart());
    ls.bound = true;
  }
  list->SetGraphicsRootConstantBufferView(0, GpuConstants(r, 0, kVertexConstantsBytes));
  list->SetGraphicsRootConstantBufferView(1, GpuConstants(r, 256, kPixelConstantsBytes));
  list->SetGraphicsRootConstantBufferView(2, SharedConstants(r, alpha_test, vs, ps, ndc));
  {
    auto [cpu, gpu] = Allocate(r, 256);
    if (cpu) {
      const uint32_t n = r->frame_stats.drawn * 2654435761u;
      const float c[4] = {0.25f + ((n >> 8) & 0xFF) / 340.0f, 0.25f + ((n >> 16) & 0xFF) / 340.0f,
                          0.25f + ((n >> 24) & 0xFF) / 340.0f, 1.0f};
      std::memcpy(cpu, c, sizeof(c));
      list->SetGraphicsRootConstantBufferView(7, gpu);
    }
  }
  if (ls.pso != pso) {
    list->SetPipelineState(pso);
    ls.pso = pso;
  }
  if (ls.topology != int(topology)) {
    list->IASetPrimitiveTopology(topology);
    ls.topology = int(topology);
  }
  list->IASetVertexBuffers(0, max_slot + 1, views);
  BindTargets(r, targets);
  const uint32_t stencil_ref = Reg(RB_STENCILREFMASK) & 0xFF;
  if (ls.stencil_ref != stencil_ref) {
    list->OMSetStencilRef(stencil_ref);
    ls.stencil_ref = stencil_ref;
  }
  const float blend_factor[4] = {RegFloat(RB_BLEND_RED), RegFloat(RB_BLEND_RED + 1),
                                 RegFloat(RB_BLEND_RED + 2), RegFloat(RB_BLEND_RED + 3)};
  if (std::memcmp(ls.blend, blend_factor, sizeof(blend_factor))) {
    list->OMSetBlendFactor(blend_factor);
    std::memcpy(ls.blend, blend_factor, sizeof(blend_factor));
  }
  if (std::memcmp(&ls.viewport, &vp, sizeof(vp))) {
    list->RSSetViewports(1, &vp);
    ls.viewport = vp;
  }
  if (std::memcmp(&ls.scissor, &scissor, sizeof(scissor))) {
    list->RSSetScissorRects(1, &scissor);
    ls.scissor = scissor;
  }
  if (draw_indexed) {
    if (std::memcmp(&ls.ibv, &ibv, sizeof(ibv))) {
      list->IASetIndexBuffer(&ibv);
      ls.ibv = ibv;
    }
    list->DrawIndexedInstanced(draw_count, 1, 0, draw_base, 0);
  } else {
    list->DrawInstanced(draw_count, 1, start, 0);
  }
  ++r->frame_stats.drawn;
}

}  // namespace

// ---------------------------------------------------------------------------
// entry points

std::string RendererLabel() {
  if (!g_main) return "Emulated";
  if (g_failed) return "Emulated (native failed)";
  if (g_suspended) return "Emulated";
  return fmt::format("Native {}x", g_scale);
}

bool Enabled() {
  static const bool on = REXCVAR_GET(native_renderer) != "off";
  return on && !g_suspended.load(std::memory_order_relaxed);
}

bool CanSwitch() { return g_main && !g_failed; }

void SetNativeActive(bool active) {
  if (!CanSwitch() || active == !g_suspended) return;
  g_suspended = !active;
  rex::external_frame::SetHostDrawingDisabled(active);
  REXLOG_INFO("native renderer: {}", active ? "drawing again" : "suspended (the emulated renderer draws)");
}

bool NativeActive() { return g_main && !g_failed && !g_suspended; }

void SetWindowSizeSource(std::function<std::pair<uint32_t, uint32_t>()> source) {
  g_window_size = std::move(source);
}

void Attach(rex::memory::Memory* memory) {
  g_memory = memory;
  // On a crash (usually the emulator aborting on a lost GPU device - the
  // same device as this renderer's), log the device's fault details.
  svr2011::SetCrashHook([] {
    Renderer* r = g_r;
    if (!r || !r->device) return;
    const HRESULT reason = r->device->GetDeviceRemovedReason();
    if (reason != S_OK) ReportDeviceRemoved(r, reason);
  });
  if (REXCVAR_GET(native_renderer) == "main") {
    g_main = true;
    g_scale = 1;  // set with the frame images' size at the first frame (ApplyOutputSettings)
    // The emulated GPU keeps running the command stream but no longer draws;
    // its presentation shows this renderer's frames. Fail() reverts both.
    rex::external_frame::SetHostDrawingDisabled(true);
    rex::external_frame::SetProvider([](rex::external_frame::Frame& f) {
      if (g_failed || g_suspended) return false;
      std::lock_guard lock(g_frame_mutex);
      if (!g_frame) return false;
      f.resource = g_frame.Get();
      f.srv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
      f.component_mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      f.width = g_frame_w;
      f.height = g_frame_h;
      f.fence = g_frame_fence.Get();
      f.fence_value = g_frame_fence_value;
      return true;
    });
    REXLOG_INFO("native renderer: main renderer at {}x resolution (the emulated renderer is the backup)", g_scale);
  }
}

// Present: the front buffer (the last full-screen colour resolve of the
// frame; with SVR2011_NATIVE_STOP_AT_RESOLVE the main target before
// post-processing) is copied to the window.
// The pipeline showing a texture in the window (shaders/present.*.hlsl).
ID3D12PipelineState* PresentPipeline(Renderer* r) {
  static ComPtr<ID3D12PipelineState> pso;
  static bool tried = false;
  if (tried) return pso.Get();
  tried = true;
  const std::vector<uint8_t> vs = ReadFile(ShaderDirectory() / "present.vs.dxil");
  const std::vector<uint8_t> ps = ReadFile(ShaderDirectory() / "present.ps.dxil");
  if (vs.empty() || ps.empty()) {
    REXLOG_WARN("native renderer: present shaders missing");
    return nullptr;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = r->root_signature.Get();
  d.VS = {vs.data(), vs.size()};
  d.PS = {ps.data(), ps.size()};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  Check(r->device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)), "present pipeline");
  return pso.Get();
}

// Draws the front buffer texture Swap named into the window's back buffer;
// false if it can't be shown.
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
  ID3D12PipelineState* pso = PresentPipeline(r);
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
  const float out_size[2] = {float(g_main ? g_out_w : kWidth * g_scale),
                             float(g_main ? g_out_h : kHeight * g_scale)};
  std::memcpy(cpu, indices, 8);
  std::memcpy(cpu + 8, scale, 8);
  std::memcpy(cpu + 16, out_size, 8);
  auto* list = r->list.Get();
  ID3D12Resource* back = BackBuffer(r);
  Barrier(list, back, r->rest_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  ID3D12DescriptorHeap* heaps[] = {r->srv_heap.Get(), r->sampler_heap.Get()};
  list->SetDescriptorHeaps(2, heaps);
  list->SetGraphicsRootSignature(r->root_signature.Get());
  list->SetGraphicsRootDescriptorTable(3, r->srv_heap->GetGPUDescriptorHandleForHeapStart());
  list->SetGraphicsRootDescriptorTable(6, r->sampler_heap->GetGPUDescriptorHandleForHeapStart());
  list->SetGraphicsRootConstantBufferView(7, gpu);
  list->SetPipelineState(pso);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = BackBufferRtv(r);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT vp = {0, 0, out_size[0], out_size[1], 0, 1};
  const D3D12_RECT sc = {0, 0, LONG(out_size[0]), LONG(out_size[1])};
  list->RSSetViewports(1, &vp);
  list->RSSetScissorRects(1, &sc);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
  Barrier(list, back, D3D12_RESOURCE_STATE_RENDER_TARGET, r->rest_state);
  r->list_state = {};
  return true;
}

void OnPresent(uint32_t front_buffer) {
  std::lock_guard lock(g_mutex);
  Renderer* r = Get();
  if (!r) return;
  if (!BeginFrame(r)) return;
  auto* list = r->list.Get();
  ID3D12Resource* back = BackBuffer(r);
  const bool shown = PresentFrontBuffer(r, front_buffer);
  ID3D12Resource* src = nullptr;
  D3D12_RESOURCE_STATES* src_state = nullptr;
  uint32_t w = 0, h = 0;
  DXGI_FORMAT src_format = DXGI_FORMAT_UNKNOWN;
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
      src = it->second.resource.Get();
      src_state = &it->second.state;
      w = it->second.width;
      h = it->second.height;
      src_format = it->second.format;
    }
  }
  if (!shown && !src && r->main_target) {
    src = r->main_target->resource.Get();
    src_state = &r->main_target->state;
    w = r->main_target->width;
    h = r->main_target->height;
    src_format = r->main_target->resource->GetDesc().Format;
  }
  // The window is RGBA8: copy only what is in the same format family.
  if (src && src_format != DXGI_FORMAT_R8G8B8A8_TYPELESS) {
    static bool logged = false;
    if (!logged) REXLOG_WARN("native renderer: front buffer format {} is not RGBA8", int(src_format));
    logged = true;
    src = nullptr;
  }
  if (src) {
    Barrier(list, back, r->rest_state, D3D12_RESOURCE_STATE_COPY_DEST);
    const D3D12_RESOURCE_STATES before = *src_state;
    Transition(list, src, *src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst_loc = {back, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src_loc = {src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    // (a fallback: unscaled, cut to the frame image)
    const uint32_t dst_w = g_main ? g_out_w : kWidth * g_scale, dst_h = g_main ? g_out_h : kHeight * g_scale;
    const D3D12_BOX box = {0, 0, 0, std::min(std::min(w, kWidth) * g_scale, dst_w),
                           std::min(std::min(h, kHeight) * g_scale, dst_h), 1};
    list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &box);
    Transition(list, src, *src_state, before);
    Barrier(list, back, D3D12_RESOURCE_STATE_COPY_DEST, r->rest_state);
  } else if (!shown) {
    Barrier(list, back, r->rest_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float clear[4] = {0.05f, 0.05f, 0.08f, 1.0f};
    list->ClearRenderTargetView(BackBufferRtv(r), clear, 0, nullptr);
    Barrier(list, back, D3D12_RESOURCE_STATE_RENDER_TARGET, r->rest_state);
  }
  r->list->Close();
  ID3D12CommandList* lists[] = {r->list.Get()};
  r->queue->ExecuteCommandLists(1, lists);
  if (!g_main) {
    const HRESULT present = r->swapchain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
    if (FAILED(present)) {
      ReportDeviceRemoved(r, present);
      return;
    }
  }
  // Debug: SVR2011_NATIVE_TEST_STALL=<frame> stalls the GPU queue for good at
  // that frame (as a stuck GPU job would), to test WaitForFence's way out.
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
    static ComPtr<ID3D12Fence> never;
    r->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&never));
    r->queue->Wait(never.Get(), 1);
    REXLOG_WARN("native renderer: test stall at frame {}", r->frames);
  }
  r->frame_fence[r->back_index] = ++r->fence_value;
  r->queue->Signal(r->fence.Get(), r->fence_value);
  if (g_main) {
    const HRESULT reason = r->device->GetDeviceRemovedReason();
    if (reason != S_OK) {
      ReportDeviceRemoved(r, reason);
      return;
    }
    // This frame, for the emulator's next swap (rex/external_frame.h).
    std::lock_guard frame_lock(g_frame_mutex);
    g_frame = r->outputs[r->output_index];
    g_frame_w = g_out_w;
    g_frame_h = g_out_h;
    g_frame_fence = r->fence;
    g_frame_fence_value = r->fence_value;
  }
  r->frame_open = false;
  ++r->frames;
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
  DrainDebugMessages(r->device.Get());
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
    r->list->ClearRenderTargetView(Handle(r->rtv_heap.Get(), t.color->rtv, r->rtv_size), colour,
                                   0, nullptr);
  }
  D3D12_CLEAR_FLAGS ds = D3D12_CLEAR_FLAGS(0);
  if (flags & 0x10) ds |= D3D12_CLEAR_FLAG_DEPTH;
  if (flags & 0x20) ds |= D3D12_CLEAR_FLAG_STENCIL;
  if (ds && t.depth) {
    const float z = std::clamp(static_cast<float>(ctx.f1.f64), 0.0f, 1.0f);
    const uint8_t stencil = uint8_t(Be32(Virtual(ctx.r1.u32 + 0x5C)));
    r->list->ClearDepthStencilView(Handle(r->dsv_heap.Get(), t.depth->dsv, r->dsv_size), ds, z,
                                   stencil, 0, nullptr);
  }
}

// D3DDevice_Resolve(device, flags, ..., destination texture in r8): flags 0-3
// colour target n, 4 depth. Copies the current target into the texture the
// game will sample (always the whole surface from (0, 0) in this game).
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
  ID3D12Resource* src = nullptr;
  D3D12_RESOURCE_STATES* src_state = nullptr;
  uint32_t src_w = 0, src_h = 0;
  if (depth) {
    if (!t.depth) return;
    src = t.depth->resource.Get();
    src_state = &t.depth->state;
    src_w = t.depth->width;
    src_h = t.depth->height;
  } else {
    if (!t.color) return;
    src = t.color->resource.Get();
    src_state = &t.color->state;
    src_w = t.color->width;
    src_h = t.color->height;
  }
  const DXGI_FORMAT family = src->GetDesc().Format;
  {
    static std::unordered_map<uint64_t, bool> logged;
    if (logged.size() < 256 &&
        logged.emplace((uint64_t(base) << 20) ^ (uint64_t(width) << 8) ^ height ^ (uint64_t(family) << 50), true).second) {
      REXLOG_INFO("native renderer: resolve flags {} -> texture {:08X} base {:08X} {}x{} fetch {:08X} {:08X} {:08X} from {}x{} format {}",
                  ctx.r4.u32, ctx.r8.u32, base, width, height, fetch[0], fetch[1], fetch[2], src_w,
                  src_h, int(family));
    }
  }
  // Depth can only be copied whole, so its copy has the target's size.
  const uint32_t dst_w = depth ? src_w : std::min(width, src_w);
  const uint32_t dst_h = depth ? src_h : std::min(height, src_h);
  ResolvedTexture& dst = r->resolved[base];
  if (!dst.resource || dst.width != dst_w || dst.height != dst_h || dst.format != family) {
    if (dst.resource) r->garbage.emplace_back(r->fence_value + kFrames, dst.resource);
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = dst_w * g_scale;
    d.Height = dst_h * g_scale;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = family;
    d.SampleDesc.Count = 1;
    dst.resource.Reset();
    if (!Check(r->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&dst.resource)),
               "resolve texture")) {
      r->resolved.erase(base);
      return;
    }
    Name(dst.resource.Get(), "resolve {:08X} {}x{}", base, dst_w, dst_h);
    dst.width = dst_w;
    dst.height = dst_h;
    dst.format = family;
    dst.state = D3D12_RESOURCE_STATE_COPY_DEST;
  }
  auto* list = r->list.Get();
  const D3D12_RESOURCE_STATES src_before = *src_state;
  Transition(list, src, *src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(list, dst.resource.Get(), dst.state, D3D12_RESOURCE_STATE_COPY_DEST);
  if (depth) {
    list->CopyResource(dst.resource.Get(), src);
  } else {
    D3D12_TEXTURE_COPY_LOCATION dst_loc = {dst.resource.Get(),
                                           D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src_loc = {src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    const D3D12_BOX box = {0, 0, 0, dst_w * g_scale, dst_h * g_scale, 1};
    list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &box);
  }
  Transition(list, dst.resource.Get(), dst.state,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(list, src, *src_state, src_before);
  if (depth) {
    textures::RegisterResolved(base, dst.resource.Get(), DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                               DXGI_FORMAT_UNKNOWN, 1, false);
  } else {
    // Resolves to ARGB textures store red and blue swapped (copy_dest_swap),
    // undone by those textures' ZYXW fetch swizzle; the copy here is RGBA,
    // so the view swaps them back. (The mirror doesn't hold the resolve's
    // RB_COPY_DEST_INFO: the destination's swizzle tells.)
    const bool swap_rb = t.color->components == 4 && ((fetch[3] >> 1) & 7) == 2;
    textures::RegisterResolved(base, dst.resource.Get(), t.color->view_format,
                               t.color->view_gamma_format, t.color->components, swap_rb);
    if (dst_w >= kWidth && dst_h >= kHeight) r->present_source = base;
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

void OnSetShaderConstants(bool, uint32_t, uint32_t) {
  // Nothing to track: each draw takes all constants from the mirror.
}

// D3D writes the dirty mirror ranges to the GPU: take them from the mirror.
void OnFlushShaderConstants() {
  // Nothing to do: each draw takes the constants from the mirror
  // (GpuConstants), which D3D has just updated.
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
