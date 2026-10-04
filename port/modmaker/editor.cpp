// The Arena Editor (Mod Maker, arenas branch). See editor.h.
//
// Game space: 1 unit = 10 cm, y points down (floor ~0, ring mat -12). The
// view keeps game space and looks with "up" = -y. Objects are the arena's
// models (its PACH entries); an object added in the editor is a group of
// meshes appended to a model the game always draws (the floor), as the
// Blender import does. Movable objects are static (one bone): their edit
// transform (offset, turn about the vertical, uniform scale, around a pivot
// at the bottom centre) is baked into the vertices whenever it changes.
// Ring parts are placed and drawn by the game's code (heights 3.4 above the
// mat, then 4.2 apart; ropes 28.5 from the centre), so the editor previews
// them the same way and edits them only through the Ring Kit.
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <d3dcompiler.h>

#include "editor.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "imgui.h"
#include "svrfmt/arena_build.h"
#include "svrfmt/arena_import.h"
#include "svrfmt/png.h"

namespace fs = std::filesystem;
using namespace svrfmt;

namespace editor {

namespace {

// ---------------------------------------------------------------- math

struct V3 {
  float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Norm(V3 a) {
  const float l = std::sqrt(Dot(a, a));
  return l > 1e-9f ? a * (1.0f / l) : a;
}

// row-major, row vectors (HLSL row_major + mul(v, M))
struct M4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};
M4 Mul(const M4& a, const M4& b) {
  M4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
      r.m[i * 4 + j] = s;
    }
  return r;
}
M4 LookAt(V3 eye, V3 at, V3 up) {
  const V3 z = Norm(at - eye), x = Norm(Cross(up, z)), y = Cross(z, x);
  M4 r;
  const float v[16] = {x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -Dot(x, eye), -Dot(y, eye), -Dot(z, eye), 1};
  std::memcpy(r.m, v, sizeof v);
  return r;
}
M4 Perspective(float fovy, float aspect, float zn, float zf, bool mirror) {
  const float ys = 1.0f / std::tan(fovy / 2), xs = ys / aspect * (mirror ? -1.0f : 1.0f);
  M4 r;
  const float v[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
  std::memcpy(r.m, v, sizeof v);
  return r;
}
// yaw about the vertical (game y), then a move
M4 Place(V3 t, float yaw) {
  M4 r;
  const float c = std::cos(yaw), s = std::sin(yaw);
  const float v[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, t.x, t.y, t.z, 1};
  std::memcpy(r.m, v, sizeof v);
  return r;
}
bool Invert(const M4& a, M4& out) {
  const float* m = a.m;
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::fabs(det) < 1e-12f) return false;
  det = 1.0f / det;
  for (int i = 0; i < 16; ++i) out.m[i] = inv[i] * det;
  return true;
}
// row vector * M, with the divide
V3 Project(V3 p, const M4& m, float* w_out = nullptr) {
  const float x = p.x * m.m[0] + p.y * m.m[4] + p.z * m.m[8] + m.m[12];
  const float y = p.x * m.m[1] + p.y * m.m[5] + p.z * m.m[9] + m.m[13];
  const float z = p.x * m.m[2] + p.y * m.m[6] + p.z * m.m[10] + m.m[14];
  const float w = p.x * m.m[3] + p.y * m.m[7] + p.z * m.m[11] + m.m[15];
  if (w_out) *w_out = w;
  return {x / w, y / w, z / w};
}

constexpr float kPi = 3.14159265f;

// ---------------------------------------------------------------- ring

constexpr float kMatY = -12.0f, kGameBase = 3.4f, kGameGap = 4.2f, kRopeOut = 28.5f, kApronOut = 31.9f;
constexpr uint32_t kSideRope[4] = {962, 961, 960, 963};

enum class Role { kNormal, kRopeSide, kRopePerRope, kPad, kTurnbuckle, kApron, kNotDrawn };

// rope model -> its side's offset from the centre (the model lies along one axis through 0)
V3 RopeOffset(uint32_t side_model) {
  switch (side_model) {
    case 960: return {0, 0, kRopeOut};
    case 961: return {-kRopeOut, 0, 0};
    case 962: return {0, 0, -kRopeOut};
    default: return {kRopeOut, 0, 0};  // 963
  }
}

// ---------------------------------------------------------------- state

struct Obj {
  int model = -1;
  std::vector<int> meshes;
  std::string label;
  Zone zone = Zone::kFree;
  Role role = Role::kNormal;
  bool movable = false, added = false, hidden = false;
  bool glow = false;  // light glows / beams: drawn additive
  // moved by the arena's own animation (its flag table, entry 50001: "m(<n>)"):
  // light rigs, beams, glows. Their meshes sit at the origin until the game
  // animates them into place, so the view doesn't draw them (they'd pile up
  // in the ring); they stay in the arena as they are.
  bool effect = false;
  // drawn twice by the game: as it is and mirrored across x = 0 (flag "r";
  // half an arena's fences, stands and truss are stored once)
  bool mirrored = false;
  // another area of a multi-area file (backstage: bg78 holds seven rooms):
  // not shown, not listed, kept as it is
  bool outside = false;
  bool ceiling = false;  // a room's ceiling / lights rig (backstage): hidden in the view unless "Show ceilings"
  V3 pos;           // edit offset
  float yaw = 0;    // radians
  float scale = 1;
  V3 pivot;         // bottom centre of the original geometry
  std::vector<std::vector<Vertex>> base;
  std::vector<std::vector<Strip>> strips;
};

struct Snapshot {
  std::vector<std::array<float, 6>> xf;  // pos, yaw, scale, hidden
};

struct GpuMesh {
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  UINT count = 0;
  std::string tex;
};

struct GpuVertex {
  float pos[3], n[3];
  uint8_t rgba[4];
  float uv[2];
};

struct Cb {
  float world[16], viewproj[16];
  float tint[4], flat[4];
};

Hooks g_hooks;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11InputLayout* g_layout = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11RasterizerState* g_solid = nullptr;
ID3D11RasterizerState* g_wire = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11DepthStencilState* g_depth_ro = nullptr;  // (glows: test, no write)
ID3D11BlendState* g_add = nullptr;               // (glows: additive)
ID3D11ShaderResourceView* g_white = nullptr;
// the viewport's target
ID3D11Texture2D* g_rt = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11ShaderResourceView* g_rt_srv = nullptr;
ID3D11DepthStencilView* g_dsv = nullptr;
int g_rt_w = 0, g_rt_h = 0;

Arena* g_arena = nullptr;
std::string g_title;
std::vector<Obj> g_objs;
std::map<std::pair<int, int>, GpuMesh> g_gpu;  // (model, mesh)
std::map<std::string, ID3D11ShaderResourceView*> g_tex;
std::vector<bool> g_model_dirty;
int g_sel = -1;
RingSpec g_ring;
Lighting g_light;
std::vector<Snapshot> g_undo;
int g_test_view = -1;  // (TestStart)
std::string g_test_select;

// camera
V3 g_target{0, -12, 0};
int g_area_lo = -1;  // SetArea: >= 0 an area is shown (its model ids: g_area)
std::vector<std::pair<int, int>> g_area;
bool InArea(uint32_t id) {
  for (const auto& [lo, hi] : g_area)
    if (id >= uint32_t(lo) && id <= uint32_t(hi)) return true;
  return false;
}
bool g_show_ceilings = false;
// SetArea: a spot in the room the game's cameras see (TestEdit puts its box
// there); half x 0: none
float g_spot[5] = {};
float g_yaw = 0.6f, g_pitch = 0.5f, g_dist = 180;
bool g_mirror = false;
bool g_show_ring = true;
M4 g_viewproj;
ImVec2 g_vp_min, g_vp_size;

enum class Tool { kSelect, kMove, kRotate, kScale };
Tool g_tool = Tool::kMove;
bool g_snap = true;
// a drag in the viewport
int g_drag_axis = -1;  // -1 none, 0..2 axis, 3 plane
V3 g_drag_start_pos;
float g_drag_start_yaw = 0, g_drag_start_scale = 1;
V3 g_drag_hit;
ImVec2 g_drag_mouse;

// budget
std::atomic<int> g_edits{0};
int g_budget_edits = -1;
std::chrono::steady_clock::time_point g_last_edit;
std::thread g_budget_thread;
std::atomic<bool> g_budget_busy{false};
std::mutex g_budget_mutex;
size_t g_budget_file = 0, g_budget_unpacked = 0, g_budget_file_max = 0, g_budget_unpacked_max = 0;

bool g_quiet = false;  // (budget builds)
bool g_glow_pass = false;

void Log(const std::string& s) {
  if (g_hooks.log && !g_quiet) g_hooks.log(s);
}

std::string U8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

void Edited() {
  ++g_edits;
  g_last_edit = std::chrono::steady_clock::now();
}

// ---------------------------------------------------------------- GPU

const char* kShader = R"(
cbuffer C : register(b0) {
  row_major float4x4 world;
  row_major float4x4 viewproj;
  float4 tint;
  float4 flat;
};
struct VI { float3 p : POSITION; float3 n : NORMAL; float4 c : COLOR; float2 uv : TEXCOORD; };
struct VO { float4 p : SV_Position; float3 n : NORMAL; float4 c : COLOR; float2 uv : TEXCOORD; };
VO vs(VI i) {
  VO o;
  float4 w = mul(float4(i.p, 1), world);
  o.p = mul(w, viewproj);
  o.n = mul(i.n, (float3x3)world);
  o.c = i.c;
  o.uv = i.uv;
  return o;
}
Texture2D t : register(t0);
SamplerState s : register(s0);
float4 ps(VO i) : SV_Target {
  if (flat.w > 0.5) return float4(flat.rgb, 1);
  float4 tx = t.Sample(s, i.uv);
  if (flat.w < -0.5) return float4(tx.rgb * tx.a * i.c.rgb * tint.rgb * 0.6, 0);
  if (tx.a < 0.25) discard;
  float3 n = normalize(i.n + 1e-5);
  float d = abs(dot(n, normalize(float3(0.35, -1.0, 0.25))));
  return float4(tx.rgb * i.c.rgb * tint.rgb * (0.55 + 0.45 * d), 1);
}
)";

bool CreatePipeline() {
  ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
  if (FAILED(D3DCompile(kShader, std::strlen(kShader), "editor", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
      FAILED(D3DCompile(kShader, std::strlen(kShader), "editor", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
    if (err) Log(std::string("editor shader: ") + static_cast<const char*>(err->GetBufferPointer()));
    return false;
  }
  g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
  g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
  const D3D11_INPUT_ELEMENT_DESC il[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  g_dev->CreateInputLayout(il, 4, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout);
  vsb->Release();
  psb->Release();
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(Cb);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  g_dev->CreateBuffer(&bd, nullptr, &g_cb);
  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_ANISOTROPIC;
  sd.MaxAnisotropy = 8;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  g_dev->CreateSamplerState(&sd, &g_sampler);
  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  g_dev->CreateRasterizerState(&rd, &g_solid);
  rd.FillMode = D3D11_FILL_WIREFRAME;
  rd.DepthBias = -100;
  rd.SlopeScaledDepthBias = -1.0f;
  g_dev->CreateRasterizerState(&rd, &g_wire);
  D3D11_DEPTH_STENCIL_DESC dd = {};
  dd.DepthEnable = TRUE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  g_dev->CreateDepthStencilState(&dd, &g_depth);
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  g_dev->CreateDepthStencilState(&dd, &g_depth_ro);
  D3D11_BLEND_DESC bl = {};
  bl.RenderTarget[0].BlendEnable = TRUE;
  bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
  bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  g_dev->CreateBlendState(&bl, &g_add);
  // a white texture for meshes without one
  const uint32_t white = 0xFFFFFFFFu;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = td.Height = 1;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA srd = {&white, 4, 0};
  ID3D11Texture2D* tex = nullptr;
  g_dev->CreateTexture2D(&td, &srd, &tex);
  g_dev->CreateShaderResourceView(tex, nullptr, &g_white);
  tex->Release();
  return true;
}

void ResizeTarget(int w, int h) {
  if (w == g_rt_w && h == g_rt_h && g_rt) return;
  if (g_rt_srv) g_rt_srv->Release(), g_rt_srv = nullptr;
  if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
  if (g_rt) g_rt->Release(), g_rt = nullptr;
  if (g_dsv) g_dsv->Release(), g_dsv = nullptr;
  g_rt_w = w, g_rt_h = h;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = UINT(w);
  td.Height = UINT(h);
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  g_dev->CreateTexture2D(&td, nullptr, &g_rt);
  g_dev->CreateRenderTargetView(g_rt, nullptr, &g_rtv);
  g_dev->CreateShaderResourceView(g_rt, nullptr, &g_rt_srv);
  td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ID3D11Texture2D* depth = nullptr;
  g_dev->CreateTexture2D(&td, nullptr, &depth);
  g_dev->CreateDepthStencilView(depth, nullptr, &g_dsv);
  depth->Release();
}

ID3D11ShaderResourceView* TextureFor(const std::string& name) {
  if (name.empty()) return g_white;
  auto it = g_tex.find(name);
  if (it != g_tex.end()) return it->second ? it->second : g_white;
  ID3D11ShaderResourceView* srv = nullptr;
  if (const BundleTexture* t = g_arena ? g_arena->FindTexture(name) : nullptr) {
    DdsInfo info;
    if (DdsInfoOf(t->data, info)) {
      DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
      int block = 0;
      switch (info.format) {
        case DxtFormat::kDxt1: f = DXGI_FORMAT_BC1_UNORM, block = 8; break;
        case DxtFormat::kDxt3: f = DXGI_FORMAT_BC2_UNORM, block = 16; break;
        case DxtFormat::kDxt5: f = DXGI_FORMAT_BC3_UNORM, block = 16; break;
        case DxtFormat::kArgb: f = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        default: break;
      }
      std::vector<D3D11_SUBRESOURCE_DATA> levels;
      size_t off = 128;
      int w = info.w, h = info.h;
      for (int l = 0; l < info.mips && f != DXGI_FORMAT_UNKNOWN; ++l) {
        const size_t pitch = block ? size_t(std::max(1, (w + 3) / 4)) * block : size_t(w) * 4;
        const size_t rows = block ? size_t(std::max(1, (h + 3) / 4)) : size_t(h);
        if (off + pitch * rows > t->data.size()) break;
        levels.push_back({t->data.data() + off, UINT(pitch), 0});
        off += pitch * rows;
        w = std::max(1, w / 2), h = std::max(1, h / 2);
      }
      if (!levels.empty()) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = UINT(info.w);
        td.Height = UINT(info.h);
        td.MipLevels = UINT(levels.size());
        td.ArraySize = 1;
        td.Format = f;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(g_dev->CreateTexture2D(&td, levels.data(), &tex))) {
          g_dev->CreateShaderResourceView(tex, nullptr, &srv);
          tex->Release();
        }
      }
    }
  }
  g_tex[name] = srv;
  return srv ? srv : g_white;
}

void ForgetTextures() {
  for (auto& [n, s] : g_tex)
    if (s) s->Release();
  g_tex.clear();
}

void ForgetGpu(int model = -1) {
  for (auto it = g_gpu.begin(); it != g_gpu.end();) {
    if (model >= 0 && it->first.first != model) { ++it; continue; }
    if (it->second.vb) it->second.vb->Release();
    if (it->second.ib) it->second.ib->Release();
    it = g_gpu.erase(it);
  }
}

int DiffuseSlot(const Mesh& s) {
  for (const auto& p : s.params)
    if (p.type == 0x0f && p.name == "texDiffuse" && p.value.size() >= 4) return int(Be32(p.value.data()));
  return -1;
}

GpuMesh* Gpu(int model, int mesh) {
  auto key = std::make_pair(model, mesh);
  auto it = g_gpu.find(key);
  if (it != g_gpu.end()) return &it->second;
  const Model& m = g_arena->models[model].model;
  const Mesh& s = m.meshes[mesh];
  GpuMesh g;
  const int slot = DiffuseSlot(s);
  g.tex = slot >= 0 && slot < int(m.textures.size()) ? m.textures[slot] : (m.textures.empty() ? "" : m.textures[0]);
  std::vector<GpuVertex> vs(s.verts.size());
  for (size_t i = 0; i < s.verts.size(); ++i) {
    const Vertex& v = s.verts[i];
    GpuVertex& o = vs[i];
    std::memcpy(o.pos, v.pos, 12);
    std::memcpy(o.n, v.normal, 12);
    o.rgba[0] = uint8_t(v.color >> 16), o.rgba[1] = uint8_t(v.color >> 8), o.rgba[2] = uint8_t(v.color);
    o.rgba[3] = uint8_t(v.color >> 24);
    o.uv[0] = i < s.uvs.size() ? s.uvs[i][0] : 0;
    o.uv[1] = i < s.uvs.size() ? s.uvs[i][1] : 0;
  }
  std::vector<uint16_t> idx;
  for (const auto& st : s.strips)
    for (const auto& t : StripToTriangles(st.indices))
      if (t[0] < vs.size() && t[1] < vs.size() && t[2] < vs.size()) idx.insert(idx.end(), t.begin(), t.end());
  g.count = UINT(idx.size());
  if (!vs.empty() && !idx.empty()) {
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = UINT(vs.size() * sizeof(GpuVertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = {vs.data(), 0, 0};
    g_dev->CreateBuffer(&bd, &sd, &g.vb);
    bd.ByteWidth = UINT(idx.size() * 2);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = idx.data();
    g_dev->CreateBuffer(&bd, &sd, &g.ib);
  }
  return &(g_gpu[key] = g);
}

void SetCb(const M4& world, const float tint[3], const float* flat = nullptr) {
  D3D11_MAPPED_SUBRESOURCE ms;
  if (FAILED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
  Cb* c = static_cast<Cb*>(ms.pData);
  std::memcpy(c->world, world.m, 64);
  std::memcpy(c->viewproj, g_viewproj.m, 64);
  c->tint[0] = tint[0], c->tint[1] = tint[1], c->tint[2] = tint[2], c->tint[3] = 1;
  if (flat) c->flat[0] = flat[0], c->flat[1] = flat[1], c->flat[2] = flat[2], c->flat[3] = 1;
  else c->flat[3] = g_glow_pass ? -1.0f : 0.0f;
  g_ctx->Unmap(g_cb, 0);
}

void DrawMesh(int model, int mesh) {
  GpuMesh* g = Gpu(model, mesh);
  if (!g->vb || !g->count) return;
  const UINT stride = sizeof(GpuVertex), offset = 0;
  g_ctx->IASetVertexBuffers(0, 1, &g->vb, &stride, &offset);
  g_ctx->IASetIndexBuffer(g->ib, DXGI_FORMAT_R16_UINT, 0);
  ID3D11ShaderResourceView* srv = TextureFor(g->tex);
  g_ctx->PSSetShaderResources(0, 1, &srv);
  g_ctx->DrawIndexed(g->count, 0, 0);
}

// ---------------------------------------------------------------- objects

float RopeBase() { return g_ring.rope_base > 0 ? g_ring.rope_base : kGameBase; }
float RopeGap() { return g_ring.rope_gap > 0 ? g_ring.rope_gap : kGameGap; }
float RopeY(int k) { return kMatY - RopeBase() - k * RopeGap(); }

void LightTint(float out[3]) {
  for (int k = 0; k < 3; ++k) out[k] = g_light.color[k] * g_light.strength;
}

bool IsStatic(const Mesh& s) { return s.palette.size() <= 1 && s.weights.size() <= 1; }

void ComputePivot(Obj& o) {
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const auto& vs : o.base)
    for (const auto& v : vs)
      for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v.pos[k]), hi[k] = std::max(hi[k], v.pos[k]);
  if (lo[0] > hi[0]) return;
  o.pivot = {(lo[0] + hi[0]) / 2, hi[1], (lo[2] + hi[2]) / 2};  // y down: hi = bottom
}

// The edit transform into the model's vertices (and strips, for hidden).
void ApplyObj(Obj& o) {
  Model& m = g_arena->models[o.model].model;
  const float c = std::cos(o.yaw), s = std::sin(o.yaw);
  for (size_t k = 0; k < o.meshes.size(); ++k) {
    Mesh& mesh = m.meshes[o.meshes[k]];
    if (o.movable) {
      const auto& base = o.base[k];
      mesh.verts.resize(base.size());
      for (size_t i = 0; i < base.size(); ++i) {
        Vertex v = base[i];
        const float dx = (v.pos[0] - o.pivot.x) * o.scale, dy = (v.pos[1] - o.pivot.y) * o.scale,
                    dz = (v.pos[2] - o.pivot.z) * o.scale;
        v.pos[0] = o.pivot.x + o.pos.x + dx * c + dz * s;
        v.pos[1] = o.pivot.y + o.pos.y + dy;
        v.pos[2] = o.pivot.z + o.pos.z - dx * s + dz * c;
        const float nx = v.normal[0], nz = v.normal[2];
        v.normal[0] = nx * c + nz * s;
        v.normal[2] = -nx * s + nz * c;
        mesh.verts[i] = v;
      }
      // bounding sphere
      float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
      for (const auto& v : mesh.verts)
        for (int j = 0; j < 3; ++j) lo[j] = std::min(lo[j], v.pos[j]), hi[j] = std::max(hi[j], v.pos[j]);
      if (!mesh.verts.empty()) {
        const float cx = (lo[0] + hi[0]) / 2, cy = (lo[1] + hi[1]) / 2, cz = (lo[2] + hi[2]) / 2;
        float r = 0;
        for (const auto& v : mesh.verts)
          r = std::max(r, (v.pos[0] - cx) * (v.pos[0] - cx) + (v.pos[1] - cy) * (v.pos[1] - cy) +
                              (v.pos[2] - cz) * (v.pos[2] - cz));
        mesh.sphere = {cx, cy, cz, std::sqrt(r)};
      }
    }
    if (o.hidden) {
      for (auto& st : mesh.strips) st.indices.clear();
    } else {
      mesh.strips = o.strips[k];
    }
  }
  g_arena->models[o.model].changed = true;
  ForgetGpu(o.model);
  Edited();
}

Role RoleOf(uint32_t id) {
  if (id >= 960 && id <= 963) return Role::kRopeSide;
  if (id >= 900 && id <= 911) return Role::kRopePerRope;
  if (id >= 964 && id <= 967) return Role::kPad;
  if (id == 952) return Role::kTurnbuckle;
  if (id == 973 || id == 974) return Role::kApron;
  if ((id >= 956 && id <= 959) || id == 40200) return Role::kNotDrawn;
  return Role::kNormal;
}

std::string Label(const ArenaModel& am) {
  char b[64];
  std::snprintf(b, sizeof b, "%s  (%X)", am.model.name.c_str(), am.id);
  return b;
}

// The arena's per-model flag table (entry 50001, text: "<model id> <flags>"):
// "m(<n>)" placed by animation track n, "r" also drawn mirrored (x -> -x).
std::map<uint32_t, std::string> ModelFlags() {
  std::map<uint32_t, std::string> flags;
  if (!g_arena) return flags;
  for (const auto& e : g_arena->entries) {
    if (e.id != 50001) continue;
    const Bytes t = Unpack(e.data);
    std::string text(t.begin(), t.end()), line;
    size_t pos = 0;
    while (pos < text.size()) {
      size_t nl = text.find('\n', pos);
      line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      pos = nl == std::string::npos ? text.size() : nl + 1;
      if (line.empty() || !std::isdigit(uint8_t(line[0]))) continue;
      const uint32_t id = uint32_t(std::atoi(line.c_str()));
      flags[id] = " " + line.substr(line.find_first_not_of("0123456789")) + " ";
    }
  }
  return flags;
}

void BuildObjects() {
  g_objs.clear();
  g_undo.clear();
  g_sel = -1;
  ForgetGpu();
  ForgetTextures();
  if (!g_arena) return;
  const std::map<uint32_t, std::string> flags = ModelFlags();
  for (size_t mi = 0; mi < g_arena->models.size(); ++mi) {
    const ArenaModel& am = g_arena->models[mi];
    Obj o;
    if (const auto f = flags.find(am.id); f != flags.end() && !am.added) {
      o.effect = f->second.find("m(") != std::string::npos;
      for (char sep : {' ', '\t', '\r'})  // (a standalone "r")
        for (char sep2 : {' ', '\t', '\r'}) o.mirrored |= f->second.find(std::string(1, sep) + "r" + sep2) != std::string::npos;
    }
    o.model = int(mi);
    o.label = Label(am);
    o.zone = am.zone;
    o.role = RoleOf(am.id);
    bool all_static = !am.model.meshes.empty();
    for (size_t k = 0; k < am.model.meshes.size(); ++k) {
      o.meshes.push_back(int(k));
      o.base.push_back(am.model.meshes[k].verts);
      o.strips.push_back(am.model.meshes[k].strips);
      all_static &= IsStatic(am.model.meshes[k]);
    }
    o.outside = g_area_lo >= 0 && !am.added && !InArea(am.id);
    if (g_area_lo >= 0) {
      const std::string& n = am.model.name;
      o.ceiling = n.find("_top") != std::string::npos || n.find("_ten") != std::string::npos ||
                  n.find("roof") != std::string::npos || n.find("_lig") != std::string::npos;
    }
    if (g_area_lo >= 0) o.zone = Zone::kFree, o.role = am.added ? o.role : Role::kNormal;  // (no ring here)
    o.movable = all_static && o.zone != Zone::kRing && o.role == Role::kNormal && !o.effect;
    o.glow = am.model.name.find("glow") != std::string::npos || am.model.name.find("beam") != std::string::npos ||
             am.model.name.find("_ev") != std::string::npos;
    ComputePivot(o);
    g_objs.push_back(std::move(o));
  }
}

Snapshot Snap() {
  Snapshot s;
  for (const auto& o : g_objs) s.xf.push_back({o.pos.x, o.pos.y, o.pos.z, o.yaw, o.scale, o.hidden ? 1.f : 0.f});
  return s;
}

void PushUndo() {
  g_undo.push_back(Snap());
  if (g_undo.size() > 64) g_undo.erase(g_undo.begin());
}

void Undo() {
  if (g_undo.empty()) return;
  const Snapshot s = g_undo.back();
  g_undo.pop_back();
  for (size_t i = 0; i < g_objs.size() && i < s.xf.size(); ++i) {
    Obj& o = g_objs[i];
    const auto& x = s.xf[i];
    if (o.pos.x == x[0] && o.pos.y == x[1] && o.pos.z == x[2] && o.yaw == x[3] && o.scale == x[4] &&
        o.hidden == (x[5] > 0.5f))
      continue;
    o.pos = {x[0], x[1], x[2]}, o.yaw = x[3], o.scale = x[4], o.hidden = x[5] > 0.5f;
    ApplyObj(o);
  }
}

// Ringside / entrance parts: the game's moves aim at fixed spots near them.
void Constrain(Obj& o) {
  if (o.zone == Zone::kRingside || o.zone == Zone::kEntrance) {
    const float lim = 5.0f;  // 50 cm
    o.pos.x = std::clamp(o.pos.x, -lim, lim);
    o.pos.y = std::clamp(o.pos.y, -lim, lim);
    o.pos.z = std::clamp(o.pos.z, -lim, lim);
    o.scale = std::clamp(o.scale, 0.8f, 1.25f);
  }
  o.scale = std::clamp(o.scale, 0.05f, 20.0f);
}

int HostModel() {
  int best = -1;
  size_t best_n = 0;
  for (size_t i = 0; i < g_arena->models.size(); ++i) {
    const auto& am = g_arena->models[i];
    if (g_area_lo >= 0) {  // (backstage: a model of the shown room, else the game hides it)
      if (am.added || !InArea(am.id) || am.id >= 1000) continue;  // (1000+: the room's movable objects)
      if (am.model.name.find("floor") != std::string::npos) return int(i);
    } else if (am.model.name.rfind("ar_ground", 0) == 0) {
      return int(i);
    }
    if ((g_area_lo < 0 && am.zone != Zone::kFree) || am.model.meshes.empty() || !IsStatic(am.model.meshes[0])) continue;
    size_t n = 0;
    for (const auto& s : am.model.meshes) n += s.verts.size();
    if (n > best_n) best = int(i), best_n = n;
  }
  return best;
}

// New meshes on a model (from an import) -> one added object
int AdoptNewMeshes(int model, size_t first, const std::string& label) {
  Obj o;
  o.model = model;
  o.label = label;
  o.zone = Zone::kFree;
  o.added = true;
  o.movable = true;
  const Model& m = g_arena->models[model].model;
  for (size_t k = first; k < m.meshes.size(); ++k) {
    o.meshes.push_back(int(k));
    o.base.push_back(m.meshes[k].verts);
    o.strips.push_back(m.meshes[k].strips);
  }
  if (o.meshes.empty()) return -1;
  ComputePivot(o);
  g_objs.push_back(std::move(o));
  ForgetGpu(model);
  ForgetTextures();
  Edited();
  return int(g_objs.size() - 1);
}

void Duplicate(int i) {
  if (i < 0 || !g_objs[i].movable) return;
  const Obj src = g_objs[i];
  Model& m = g_arena->models[src.model].model;
  const size_t first = m.meshes.size();
  for (size_t k = 0; k < src.meshes.size(); ++k) {
    Mesh s = m.meshes[src.meshes[k]];
    s.verts = src.base[k];
    s.strips = src.strips[k];
    m.meshes.push_back(std::move(s));
  }
  const int n = AdoptNewMeshes(src.model, first, src.label + " copy");
  if (n < 0) return;
  Obj& o = g_objs[n];
  o.pivot = src.pivot;
  o.pos = src.pos + V3{6, 0, 6};
  o.yaw = src.yaw;
  o.scale = src.scale;
  ApplyObj(o);
  g_sel = n;
  Log("Duplicated " + src.label + ".");
}

void Delete(int i) {
  if (i < 0) return;
  Obj& o = g_objs[i];
  if (!o.added) {
    PushUndo();
    o.hidden = true;
    ApplyObj(o);
    return;
  }
  // remove its meshes from the model; later mesh indices move down
  Model& m = g_arena->models[o.model].model;
  std::vector<int> gone = o.meshes;
  std::sort(gone.rbegin(), gone.rend());
  for (int k : gone) m.meshes.erase(m.meshes.begin() + k);
  for (auto& other : g_objs) {
    if (&other == &o || other.model != o.model) continue;
    for (int& k : other.meshes) {
      int below = 0;
      for (int g : gone) below += g < k;
      k -= below;
    }
  }
  g_arena->models[o.model].changed = true;
  ForgetGpu(o.model);
  g_objs.erase(g_objs.begin() + i);
  g_sel = -1;
  g_undo.clear();  // (structure changed)
  Edited();
}

void SetObjTexture(Obj& o, const std::string& name) {
  Model& m = g_arena->models[o.model].model;
  int slot = -1;
  for (size_t i = 0; i < m.textures.size(); ++i)
    if (m.textures[i] == name) slot = int(i);
  if (slot < 0) {
    m.textures.push_back(name);
    slot = int(m.textures.size() - 1);
  }
  for (int k : o.meshes)
    for (auto& p : m.meshes[k].params)
      if (p.type == 0x0f && p.name == "texDiffuse" && p.value.size() >= 4) PutBe32(p.value.data(), uint32_t(slot));
  g_arena->models[o.model].changed = true;
  ForgetGpu(o.model);
  Edited();
}

// A picture as a new texture in the arena's first set (DXT5 when it has alpha)
std::string AddPicture(const std::string& path) {
  Image img;
  if (!LoadImageFile(path, img) || g_arena->bundles.empty()) return "";
  bool alpha = false;
  for (size_t i = 3; i < img.rgba.size() && !alpha; i += 4) alpha = img.rgba[i] < 250;
  // power-of-two, at most 512
  auto p2 = [](int v) { int p = 16; while (p < v && p < 512) p *= 2; return p; };
  img = Resize(img, p2(img.w), p2(img.h));
  static int n = 0;
  std::string name;
  do {
    char b[16];
    std::snprintf(b, sizeof b, "ed_tex%02d", n++);
    name = b;
  } while (g_arena->FindTexture(name));
  g_arena->bundles[0].textures.push_back({name, "dds", DdsEncode(img, alpha ? DxtFormat::kDxt5 : DxtFormat::kDxt1, true)});
  g_arena->bundles[0].changed = true;
  ForgetTextures();
  Edited();
  return name;
}

// An OBJ/FBX file's objects -> one added object on the host model
int ImportObject(const std::string& path, const std::string& label) {
  const int host = HostModel();
  if (host < 0) { Log("This arena has no model to add objects to."); return -1; }
  const size_t first = g_arena->models[host].model.meshes.size();
  ImportOptions opt;
  opt.host_model = g_arena->models[host].model.name;
  ImportReport rep;
  const bool ok = ImportFbx(*g_arena, path, opt, rep);
  for (const auto& w : rep.warnings) Log("  " + w);
  for (const auto& e : rep.errors) Log("  error: " + e);
  if (!ok) return -1;
  return AdoptNewMeshes(host, first, label);
}

// A box (w x h x d units) as an OBJ in the import's space (Y up, metres-ish),
// then sized and placed exactly on the camera target.
void AddBox(float w, float h, float d) {
  const fs::path tmp = fs::temp_directory_path() / "svr2011_modmaker_box.obj";
  if (FILE* f = std::fopen(tmp.string().c_str(), "wb")) {
    std::fprintf(f, "o box\n");
    const float x[2] = {-0.5f, 0.5f}, y[2] = {0, 1}, z[2] = {-0.5f, 0.5f};
    for (int i = 0; i < 8; ++i) std::fprintf(f, "v %g %g %g\n", x[i & 1], y[(i >> 1) & 1], z[(i >> 2) & 1]);
    std::fprintf(f, "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n");
    const int faces[6][4] = {{1, 2, 4, 3}, {5, 7, 8, 6}, {1, 5, 6, 2}, {3, 4, 8, 7}, {1, 3, 7, 5}, {2, 6, 8, 4}};
    for (const auto& q : faces) std::fprintf(f, "f %d/1 %d/2 %d/3 %d/4\n", q[0], q[1], q[2], q[3]);
    std::fclose(f);
  }
  const int i = ImportObject(tmp.string(), "box");
  if (i < 0) return;
  Obj& o = g_objs[i];
  // normalise to the wanted size, bottom centre on the floor below the camera target
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const auto& vs : o.base)
    for (const auto& v : vs)
      for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v.pos[k]), hi[k] = std::max(hi[k], v.pos[k]);
  const float size[3] = {w, h, d};
  for (auto& vs : o.base)
    for (auto& v : vs)
      for (int k = 0; k < 3; ++k) {
        const float t = hi[k] > lo[k] ? (v.pos[k] - lo[k]) / (hi[k] - lo[k]) : 0.5f;  // 0..1
        const float c = k == 0 ? g_target.x : k == 2 ? g_target.z : 0.0f;
        // y down: the box stands on the floor (y = 0; an area's: its lowest point) and rises to -h
        const float floor = g_area_lo >= 0 ? g_target.y : 0.0f;
        v.pos[k] = k == 1 ? floor - (1.0f - t) * size[1] : c + (t - 0.5f) * size[k];
      }
  ComputePivot(o);
  ApplyObj(o);
  g_sel = i;
  Log("Added a box. Pick its texture on the right.");
}

// ---------------------------------------------------------------- view

V3 Eye() {
  return g_target + V3{std::cos(g_pitch) * std::sin(g_yaw), -std::sin(g_pitch), std::cos(g_pitch) * std::cos(g_yaw)} * g_dist;
}

void Ray(ImVec2 mouse, V3& origin, V3& dir) {
  M4 inv;
  Invert(g_viewproj, inv);
  const float nx = (mouse.x - g_vp_min.x) / g_vp_size.x * 2 - 1, ny = 1 - (mouse.y - g_vp_min.y) / g_vp_size.y * 2;
  const V3 a = Project({nx, ny, 0}, inv), b = Project({nx, ny, 1}, inv);
  origin = a;
  dir = Norm(b - a);
}

bool ToScreen(V3 p, ImVec2& out) {
  float w;
  const V3 c = Project(p, g_viewproj, &w);
  if (w <= 0) return false;
  out = {g_vp_min.x + (c.x + 1) * 0.5f * g_vp_size.x, g_vp_min.y + (1 - c.y) * 0.5f * g_vp_size.y};
  return true;
}

// nearest object hit by the ray (Moller-Trumbore), -1 if none
int Pick(V3 o, V3 d, float* t_out = nullptr) {
  int best = -1;
  float best_t = 1e30f;
  for (size_t i = 0; i < g_objs.size(); ++i) {
    const Obj& ob = g_objs[i];
    if (ob.hidden || ob.effect || ob.outside || (ob.ceiling && !g_show_ceilings) || ob.role != Role::kNormal) continue;
    const Model& m = g_arena->models[ob.model].model;
    for (int k : ob.meshes) {
      const Mesh& s = m.meshes[k];
      // sphere test first
      const V3 c{s.sphere[0], s.sphere[1], s.sphere[2]};
      const V3 oc = c - o;
      const float along = Dot(oc, d);
      if (Dot(oc, oc) - along * along > s.sphere[3] * s.sphere[3] + 1) continue;
      for (const auto& st : s.strips)
        for (const auto& t : StripToTriangles(st.indices)) {
          if (t[0] >= s.verts.size() || t[1] >= s.verts.size() || t[2] >= s.verts.size()) continue;
          const V3 p0{s.verts[t[0]].pos[0], s.verts[t[0]].pos[1], s.verts[t[0]].pos[2]};
          const V3 p1{s.verts[t[1]].pos[0], s.verts[t[1]].pos[1], s.verts[t[1]].pos[2]};
          const V3 p2{s.verts[t[2]].pos[0], s.verts[t[2]].pos[1], s.verts[t[2]].pos[2]};
          const V3 e1 = p1 - p0, e2 = p2 - p0, pv = Cross(d, e2);
          const float det = Dot(e1, pv);
          if (std::fabs(det) < 1e-8f) continue;
          const float inv = 1 / det;
          const V3 tv = o - p0;
          const float u = Dot(tv, pv) * inv;
          if (u < 0 || u > 1) continue;
          const V3 qv = Cross(tv, e1);
          const float v = Dot(d, qv) * inv;
          if (v < 0 || u + v > 1) continue;
          const float tt = Dot(e2, qv) * inv;
          if (tt > 0.01f && tt < best_t) best_t = tt, best = int(i);
        }
    }
  }
  if (t_out) *t_out = best_t;
  return best;
}

void RenderScene() {
  const float clear[4] = {0.05f, 0.055f, 0.065f, 1};
  g_ctx->ClearRenderTargetView(g_rtv, clear);
  g_ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
  if (!g_arena) return;
  D3D11_VIEWPORT vp = {0, 0, float(g_rt_w), float(g_rt_h), 0, 1};
  g_ctx->RSSetViewports(1, &vp);
  g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
  g_ctx->OMSetDepthStencilState(g_depth, 0);
  g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  g_ctx->RSSetState(g_solid);
  g_ctx->IASetInputLayout(g_layout);
  g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g_ctx->VSSetShader(g_vs, nullptr, 0);
  g_ctx->PSSetShader(g_ps, nullptr, 0);
  g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
  g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
  g_ctx->PSSetSamplers(0, 1, &g_sampler);
  const M4 view = LookAt(Eye(), g_target, {0, -1, 0});
  const M4 proj = Perspective(0.9f, float(g_rt_w) / float(std::max(1, g_rt_h)), 1.0f, 5000.0f, g_mirror);
  g_viewproj = Mul(view, proj);
  float light[3];
  LightTint(light);
  const bool per_rope = [] {
    for (const auto& o : g_objs)
      if (o.role == Role::kRopePerRope) return true;
    return false;
  }();
  const M4 ident;
  for (const Obj& o : g_objs) {
    if (o.hidden || o.effect || o.outside || (o.ceiling && !g_show_ceilings)) continue;
    switch (o.role) {
      case Role::kNotDrawn:
        break;
      case Role::kNormal:
        if (o.glow) break;  // (after the solid ones)
        SetCb(ident, light);
        for (int k : o.meshes) DrawMesh(o.model, k);
        if (o.mirrored) {  // (the game's mirrored copy)
          M4 mirror;
          mirror.m[0] = -1;
          SetCb(mirror, light);
          for (int k : o.meshes) DrawMesh(o.model, k);
        }
        break;
      case Role::kRopeSide:
      case Role::kRopePerRope: {
        if (!g_show_ring) break;
        const uint32_t id = g_arena->models[o.model].id;
        if (o.role == Role::kRopeSide && per_rope) break;  // the game draws the twelve instead
        for (int r = 0; r < 3; ++r) {
          int side_model = int(id);
          if (o.role == Role::kRopePerRope) {
            if ((int(id) - 900) / 4 != r) continue;
            side_model = int(kSideRope[(id - 900) % 4]);
          }
          if (!g_ring.ropes[r].visible) continue;
          const uint32_t c = g_ring.ropes[r].tint;
          const float tint[3] = {light[0] * ((c >> 16) & 255) / 255.f, light[1] * ((c >> 8) & 255) / 255.f,
                                 light[2] * (c & 255) / 255.f};
          SetCb(Place(RopeOffset(uint32_t(side_model)) + V3{0, RopeY(r), 0}, 0), tint);
          for (int k : o.meshes) DrawMesh(o.model, k);
        }
        break;
      }
      case Role::kPad:
        if (!g_show_ring || !g_ring.pads) break;
        for (int r = 0; r < 3; ++r) {
          SetCb(Place({0, RopeY(r), 0}, 0), light);
          for (int k : o.meshes) DrawMesh(o.model, k);
        }
        break;
      case Role::kTurnbuckle:
        if (!g_show_ring || !g_ring.turnbuckles) break;
        for (int corner = 0; corner < 4; ++corner)
          for (int r = 0; r < 3; ++r) {
            SetCb(Place({0, RopeY(r), 0}, corner * kPi / 2), light);
            for (int k : o.meshes) DrawMesh(o.model, k);
          }
        break;
      case Role::kApron: {
        if (!g_show_ring) break;
        const bool second = g_arena->models[o.model].id == 974;
        for (int side = 0; side < 2; ++side) {
          const float yaw = (second ? kPi / 2 : 0) + side * kPi;
          const float c = std::cos(yaw), s = std::sin(yaw);
          // the panel faces +z at z = 0: move it out along its facing
          SetCb(Place({kApronOut * s, 0, kApronOut * c}, yaw), light);
          for (int k : o.meshes) DrawMesh(o.model, k);
        }
        break;
      }
    }
  }
  // light glows and beams, added on top
  g_ctx->OMSetBlendState(g_add, nullptr, 0xFFFFFFFF);
  g_ctx->OMSetDepthStencilState(g_depth_ro, 0);
  g_glow_pass = true;
  SetCb(ident, light);
  g_glow_pass = false;
  for (const Obj& o : g_objs)
    if (o.glow && !o.hidden && !o.effect && !o.outside && !(o.ceiling && !g_show_ceilings) && o.role == Role::kNormal)
      for (int k : o.meshes) DrawMesh(o.model, k);
  {  // (the mirrored copies)
    M4 mirror;
    mirror.m[0] = -1;
    g_glow_pass = true;
    SetCb(mirror, light);
    g_glow_pass = false;
    for (const Obj& o : g_objs)
      if (o.glow && o.mirrored && !o.hidden && !o.effect && !o.outside && o.role == Role::kNormal)
        for (int k : o.meshes) DrawMesh(o.model, k);
  }
  g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  g_ctx->OMSetDepthStencilState(g_depth, 0);
  // the selection, as a wireframe on top
  if (g_sel >= 0 && g_sel < int(g_objs.size()) && g_objs[g_sel].role == Role::kNormal && !g_objs[g_sel].hidden) {
    g_ctx->RSSetState(g_wire);
    const float orange[3] = {1.0f, 0.55f, 0.1f};
    SetCb(ident, light, orange);
    for (int k : g_objs[g_sel].meshes) DrawMesh(g_objs[g_sel].model, k);
    g_ctx->RSSetState(g_solid);
  }
  ID3D11RenderTargetView* none = nullptr;
  g_ctx->OMSetRenderTargets(1, &none, nullptr);
}

// ---------------------------------------------------------------- budget

void UpdateBudget() {
  if (!g_arena || g_budget_busy || g_budget_edits == g_edits) return;
  if (std::chrono::steady_clock::now() - g_last_edit < std::chrono::milliseconds(1200) && g_budget_edits >= 0) return;
  if (g_budget_thread.joinable()) g_budget_thread.join();
  g_budget_edits = g_edits;
  g_budget_busy = true;
  auto copy = std::make_shared<Arena>(*g_arena);
  g_quiet = true;
  ApplyBuild(*copy);
  g_quiet = false;
  g_budget_thread = std::thread([copy] {
    size_t unpacked = 0;
    for (const auto& e : copy->entries) {
      bool done = false;
      for (const auto& m : copy->models)
        if (m.id == e.id && m.changed) unpacked += JboyWrite(m.model).size(), done = true;
      for (const auto& b : copy->bundles)
        if (b.id == e.id && b.changed) unpacked += BundleWrite(b.textures).size(), done = true;
      if (!done) unpacked += Unpack(e.data).size();
    }
    for (const auto& m : copy->models)
      if (m.added) unpacked += JboyWrite(m.model).size();
    const size_t file = copy->Save().size();
    std::lock_guard lock(g_budget_mutex);
    g_budget_file = file, g_budget_unpacked = unpacked;
    g_budget_file_max = copy->original_file, g_budget_unpacked_max = copy->original_unpacked;
    g_budget_busy = false;
  });
}

// ---------------------------------------------------------------- lighting

struct Preset {
  const char* name;
  float color[3], strength;
};
const Preset kPresets[] = {
    {"As made", {1, 1, 1}, 1.0f},          {"Brighter", {1, 1, 1}, 1.25f},
    {"Darker", {1, 1, 1}, 0.7f},           {"Night show", {0.75f, 0.8f, 1.0f}, 0.6f},
    {"Warm", {1.1f, 0.95f, 0.8f}, 1.0f},   {"Cool", {0.85f, 0.95f, 1.15f}, 1.0f},
    {"Blood red", {1.25f, 0.55f, 0.55f}, 1.0f}, {"Toxic green", {0.7f, 1.2f, 0.7f}, 1.0f},
    {"Royal purple", {0.95f, 0.7f, 1.2f}, 1.0f}, {"Custom", {1, 1, 1}, 1.0f},
};

// ---------------------------------------------------------------- UI parts

const char* ZoneTitle(Zone z) {
  switch (z) {
    case Zone::kRing: return "Ring kit";
    case Zone::kRingside: return "Ringside";
    case Zone::kEntrance: return "Entrance";
    default: return "Arena";
  }
}

void Outliner() {
  static char filter[64] = "";
  if (g_area_lo >= 0) ImGui::Checkbox("Show ceilings", &g_show_ceilings);
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##filter", "Find an object", filter, sizeof filter);
  const Zone zones[] = {Zone::kRing, Zone::kRingside, Zone::kEntrance, Zone::kFree};
  for (int pass = 0; pass < 6; ++pass) {
    const bool added_pass = pass == 4, effect_pass = pass == 5;
    const char* title = effect_pass ? "Effects (placed by the game)" : added_pass ? "Added"
                        : g_area_lo >= 0                                    ? "Area"
                                                                            : ZoneTitle(zones[pass]);
    auto in_pass = [&](const Obj& o) {
      if (o.outside) return false;
      return effect_pass ? o.effect : added_pass ? o.added : (!o.added && !o.effect && o.zone == zones[pass]);
    };
    int count = 0;
    for (const auto& o : g_objs) count += in_pass(o);
    if (!count) continue;
    char head[64];
    std::snprintf(head, sizeof head, "%s (%d)###z%d", title, count, pass);
    ImGui::SetNextItemOpen(pass == 0 || pass == 4, ImGuiCond_Once);
    const bool open = ImGui::TreeNode(head);
    if (effect_pass && ImGui::IsItemHovered())
      ImGui::SetTooltip("Light rigs, beams and glows the arena's own animation moves into place during the show.\n"
                        "Not drawn here (they would all sit in the ring); untick one to remove it from the arena.");
    if (!open) continue;
    for (size_t i = 0; i < g_objs.size(); ++i) {
      Obj& o = g_objs[i];
      if (!in_pass(o)) continue;
      if (filter[0] && o.label.find(filter) == std::string::npos) continue;
      ImGui::PushID(int(i));
      bool vis = !o.hidden;
      if (ImGui::Checkbox("##v", &vis)) {
        PushUndo();
        o.hidden = !vis;
        ApplyObj(o);
      }
      ImGui::SameLine();
      if (o.hidden) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      if (ImGui::Selectable(o.label.c_str(), g_sel == int(i))) g_sel = int(i);
      if (o.hidden) ImGui::PopStyleColor();
      ImGui::PopID();
    }
    ImGui::TreePop();
  }
}

void RingKitPanel() {
  bool changed = false;
  ImGui::TextWrapped("The ring's ropes, turnbuckles and corner pads. Bottom rope = 0.");
  for (int r = 2; r >= 0; --r) {
    ImGui::PushID(r);
    const char* names[] = {"Bottom rope", "Middle rope", "Top rope"};
    changed |= ImGui::Checkbox(names[r], &g_ring.ropes[r].visible);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
    float col[3] = {((g_ring.ropes[r].tint >> 16) & 255) / 255.f, ((g_ring.ropes[r].tint >> 8) & 255) / 255.f,
                    (g_ring.ropes[r].tint & 255) / 255.f};
    if (ImGui::ColorEdit3("##c", col, ImGuiColorEditFlags_NoInputs)) {
      g_ring.ropes[r].tint = uint32_t(col[0] * 255 + 0.5f) << 16 | uint32_t(col[1] * 255 + 0.5f) << 8 |
                             uint32_t(col[2] * 255 + 0.5f);
      changed = true;
    }
    ImGui::PopID();
  }
  changed |= ImGui::Checkbox("Turnbuckles", &g_ring.turnbuckles);
  ImGui::SameLine();
  changed |= ImGui::Checkbox("Corner pads", &g_ring.pads);
  float base = RopeBase() * 10, gap = RopeGap() * 10;  // cm
  ImGui::SetNextItemWidth(-90);
  if (ImGui::SliderFloat("Low rope (cm)", &base, 15, 70, "%.0f")) g_ring.rope_base = base / 10, changed = true;
  ImGui::SetNextItemWidth(-90);
  if (ImGui::SliderFloat("Rope gap (cm)", &gap, 25, 55, "%.0f")) g_ring.rope_gap = gap / 10, changed = true;
  if (RopeBase() + 2 * RopeGap() > 12.5f)
    ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "The top rope would be above the posts.");
  if (ImGui::Button("Reset ring")) g_ring = RingSpec(), changed = true;
  if (g_ring.VisibleRopes() == 0)
    ImGui::TextWrapped("No ropes: runners and whipped wrestlers stop at the edge instead of rebounding, and there "
                       "are no rope breaks.");
  else if (g_ring.VisibleRopes() < 3) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.7f, 0.2f, 1));
    ImGui::TextWrapped("Ropes left out are not drawn, but wrestlers still use all three rope heights (rebounds, "
                       "rope moves). Leave out all three for a ring without rope gameplay.");
    ImGui::PopStyleColor();
  }
  if (changed) Edited();
}

void LightingPanel() {
  int& p = g_light.preset;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##preset", kPresets[p].name)) {
    for (int i = 0; i < int(std::size(kPresets)); ++i)
      if (ImGui::Selectable(kPresets[i].name, p == i)) {
        p = i;
        if (i != int(std::size(kPresets)) - 1) {
          std::memcpy(g_light.color, kPresets[i].color, sizeof g_light.color);
          g_light.strength = kPresets[i].strength;
        }
        Edited();
      }
    ImGui::EndCombo();
  }
  if (ImGui::ColorEdit3("Colour", g_light.color)) p = int(std::size(kPresets)) - 1, Edited();
  if (ImGui::SliderFloat("Strength", &g_light.strength, 0.3f, 1.6f, "%.2f")) p = int(std::size(kPresets)) - 1, Edited();
  ImGui::TextDisabled("Every arena material's colour (not the crowd).");
  if (ImGui::Checkbox("Crowd in the seats", &g_light.crowd)) Edited();
}

void Inspector() {
  if (g_sel < 0 || g_sel >= int(g_objs.size())) {
    ImGui::TextDisabled("Click an object in the view or the list.");
    return;
  }
  Obj& o = g_objs[g_sel];
  const ArenaModel& am = g_arena->models[o.model];
  ImGui::TextWrapped("%s", o.label.c_str());
  ImGui::TextDisabled("%s, %zu meshes%s", ZoneTitle(o.zone), o.meshes.size(), o.added ? ", added" : "");
  if (o.zone == Zone::kRing) {
    ImGui::TextWrapped("Ring parts are placed by the game: change them with the Ring Kit, or retexture them.");
  } else if (!o.movable) {
    ImGui::TextWrapped("This object is rigged (moves with bones): it can be hidden or retextured, not moved.");
  } else {
    if (o.zone == Zone::kRingside || o.zone == Zone::kEntrance)
      ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "Wrestlers' moves aim at this: moves are limited to 50 cm.");
    float p[3] = {o.pos.x / 10, -o.pos.y / 10, o.pos.z / 10};  // metres, up positive
    bool ch = false;
    ImGui::SetNextItemWidth(-60);
    ch |= ImGui::DragFloat3("Move (m)", p, 0.02f, -200, 200, "%.2f");
    if (ImGui::IsItemActivated()) PushUndo();
    float deg = o.yaw * 180 / kPi;
    ImGui::SetNextItemWidth(-60);
    ch |= ImGui::DragFloat("Turn (deg)", &deg, 0.5f, -360, 360, "%.1f");
    if (ImGui::IsItemActivated()) PushUndo();
    ImGui::SetNextItemWidth(-60);
    ch |= ImGui::DragFloat("Scale", &o.scale, 0.005f, 0.05f, 20, "%.3f");
    if (ImGui::IsItemActivated()) PushUndo();
    if (ch) {
      o.pos = {p[0] * 10, -p[1] * 10, p[2] * 10};
      o.yaw = deg * kPi / 180;
      Constrain(o);
      ApplyObj(o);
    }
    if (ImGui::Button("Duplicate")) Duplicate(g_sel);
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
      PushUndo();
      o.pos = {}, o.yaw = 0, o.scale = 1;
      ApplyObj(o);
    }
  }
  if (ImGui::Button(o.hidden ? "Show" : "Hide")) {
    PushUndo();
    o.hidden = !o.hidden;
    ApplyObj(o);
  }
  if (o.added) {
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
      Delete(g_sel);
      return;
    }
  }
  // texture
  if (!o.meshes.empty()) {
    const Model& m = am.model;
    const int slot = DiffuseSlot(m.meshes[o.meshes[0]]);
    const std::string cur = slot >= 0 && slot < int(m.textures.size()) ? m.textures[slot] : "";
    ImGui::SetNextItemWidth(-60);
    if (ImGui::BeginCombo("Texture", cur.c_str())) {
      for (const auto& b : g_arena->bundles)
        for (const auto& t : b.textures)
          if (ImGui::Selectable(t.name.c_str(), t.name == cur)) SetObjTexture(o, t.name);
      ImGui::EndCombo();
    }
    if (ImGui::Button("Use a picture...")) {
      wchar_t file[MAX_PATH] = L"";
      OPENFILENAMEW ofn = {sizeof ofn};
      ofn.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0";
      ofn.lpstrFile = file;
      ofn.nMaxFile = MAX_PATH;
      ofn.Flags = OFN_FILEMUSTEXIST;
      if (GetOpenFileNameW(&ofn)) {
        const std::string path = U8(fs::path(file));
        const std::string name = AddPicture(path);
        if (name.empty()) Log("That picture could not be read.");
        else SetObjTexture(o, name), Log("New texture " + name + " on " + o.label + ".");
      }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(this object only)");
  }
}

// -- The prop library: models from any of the game's arenas ------------------

struct Library {
  int arena = -1;                  // index in g_hooks.library (loaded)
  std::unique_ptr<Arena> src;
  std::vector<int> models;         // LibraryModels(src)
  std::thread loader;
  std::atomic<bool> loading{false};
  std::unique_ptr<Arena> pending;  // (from the loader)
  int pending_arena = -1;
  std::mutex mutex;
  int pick = -1;
  char filter[64] = "";
};
Library g_lib;

void LibraryLoad(int i) {
  if (g_lib.loading || i < 0 || i >= int(g_hooks.library.size())) return;
  if (g_lib.loader.joinable()) g_lib.loader.join();
  g_lib.loading = true;
  const std::string path = g_hooks.library[i].second;
  g_lib.loader = std::thread([i, path] {
    auto a = std::make_unique<Arena>();
    std::string err;
    const bool ok = a->Load(path, &err);
    std::lock_guard lock(g_lib.mutex);
    if (ok) g_lib.pending = std::move(a), g_lib.pending_arena = i;
    g_lib.loading = false;
  });
}

void AddFromLibrary(int src_model, bool at_view) {
  const int host = HostModel();
  if (host < 0) { Log("This arena has no floor model to add objects to."); return; }
  std::string err;
  const int first = CopyModelInto(*g_arena, host, *g_lib.src, src_model, &err);
  if (first < 0) { Log("Could not add it: " + err); return; }
  const std::string label = g_lib.src->models[src_model].model.name + " (" + g_hooks.library[g_lib.arena].first + ")";
  const int i = AdoptNewMeshes(host, size_t(first), label);
  if (i < 0) return;
  Obj& o = g_objs[i];
  if (at_view) {
    o.pos = {g_target.x - o.pivot.x, 0, g_target.z - o.pivot.z};
    ApplyObj(o);
  }
  g_sel = i;
  Log("Added " + label + (at_view ? " at the view centre." : " in its place."));
}

void LibraryPanel() {
  {
    std::lock_guard lock(g_lib.mutex);
    if (g_lib.pending) {
      g_lib.src = std::move(g_lib.pending);
      g_lib.arena = g_lib.pending_arena;
      g_lib.models = LibraryModels(*g_lib.src);
      g_lib.pick = -1;
    }
  }
  if (g_hooks.library.empty()) {
    ImGui::TextDisabled("No game folder.");
    return;
  }
  ImGui::SetNextItemWidth(-1);
  const char* cur = g_lib.arena >= 0 ? g_hooks.library[g_lib.arena].first.c_str() : "Take models from...";
  if (ImGui::BeginCombo("##libarena", cur)) {
    for (int i = 0; i < int(g_hooks.library.size()); ++i)
      if (ImGui::Selectable(g_hooks.library[i].first.c_str(), i == g_lib.arena)) LibraryLoad(i);
    ImGui::EndCombo();
  }
  if (g_lib.loading) {
    ImGui::TextDisabled("Loading...");
    return;
  }
  if (!g_lib.src) {
    ImGui::TextWrapped("Any object of any arena: stages, titantrons, trusses, lights, signs, barriers.");
    return;
  }
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##libfilter", "Find", g_lib.filter, sizeof g_lib.filter);
  const float scale = ImGui::GetFontSize() / 17.0f;
  ImGui::BeginChild("liblist", ImVec2(0, 180 * scale), true);
  for (int k : g_lib.models) {
    const auto& am = g_lib.src->models[k];
    if (g_lib.filter[0] && am.model.name.find(g_lib.filter) == std::string::npos) continue;
    size_t tris = 0;
    for (const auto& sm : am.model.meshes)
      for (const auto& st : sm.strips) tris += st.indices.size();
    char b[96];
    std::snprintf(b, sizeof b, "%s  (%s, %zuk)##%d", am.model.name.c_str(), ZoneName(am.zone), (tris + 999) / 1000, k);
    if (ImGui::Selectable(b, g_lib.pick == k)) g_lib.pick = k;
  }
  ImGui::EndChild();
  ImGui::BeginDisabled(g_lib.pick < 0);
  if (ImGui::Button("Add in its place")) AddFromLibrary(g_lib.pick, false);
  ImGui::SameLine();
  if (ImGui::Button("Add at the view centre")) AddFromLibrary(g_lib.pick, true);
  ImGui::EndDisabled();
}

void AddPanel() {
  static float box[3] = {1.0f, 1.0f, 1.0f};  // metres
  ImGui::SetNextItemWidth(-90);
  ImGui::DragFloat3("Box (m)", box, 0.05f, 0.1f, 50, "%.2f");
  if (ImGui::Button("Add box at the view centre")) AddBox(box[0] * 10, box[1] * 10, box[2] * 10);
  if (ImGui::Button("Add object from a file (.obj / .fbx)...")) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {sizeof ofn};
    ofn.lpstrFilter = L"3D objects\0*.obj;*.fbx\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
      const fs::path p(file);
      const int i = ImportObject(U8(p), U8(p.stem()));
      if (i >= 0) {
        g_sel = i;
        Log("Added " + U8(p.filename()) + " (textures: a textures folder beside it, or pick one).");
      }
    }
  }
  ImGui::TextDisabled("Files in game units (10 cm), as Export to Blender writes.");
}

void BudgetPanel() {
  UpdateBudget();
  std::lock_guard lock(g_budget_mutex);
  if (!g_budget_file_max) {
    ImGui::TextDisabled("Measuring...");
    return;
  }
  auto bar = [](const char* what, size_t v, size_t max) {
    const float f = float(double(v) / double(max));
    char b[96];
    std::snprintf(b, sizeof b, "%s %.1f / %.1f MB", what, v / 1048576.0, max / 1048576.0);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, f > 1 ? ImVec4(0.85f, 0.45f, 0.1f, 1) : ImVec4(0.2f, 0.65f, 0.35f, 1));
    ImGui::ProgressBar(std::min(f, 1.0f), ImVec2(-1, 0), b);
    ImGui::PopStyleColor();
  };
  bar("File", g_budget_file, g_budget_file_max);
  bar("Memory", g_budget_unpacked, g_budget_unpacked_max);
  if (g_budget_file > g_budget_file_max || g_budget_unpacked > g_budget_unpacked_max)
    ImGui::TextWrapped("Over the game's room for this arena: saving halves the biggest textures until it fits.");
  if (g_budget_busy) ImGui::TextDisabled("Measuring...");
}

// mouse in the viewport
void ViewportInput(bool hovered) {
  ImGuiIO& io = ImGui::GetIO();
  if (hovered) {
    if (io.MouseWheel != 0) g_dist = std::clamp(g_dist * std::pow(0.88f, io.MouseWheel), 10.0f, 2000.0f);
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
      const ImVec2 d = io.MouseDelta;
      g_yaw -= d.x * 0.006f;
      g_pitch = std::clamp(g_pitch + d.y * 0.006f, -0.2f, 1.55f);
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
      const ImVec2 d = io.MouseDelta;
      const V3 fwd = Norm(g_target - Eye()), right = Norm(Cross({0, -1, 0}, fwd)), up = Cross(fwd, right);
      g_target = g_target - right * (d.x * g_dist * 0.0015f) + up * (d.y * g_dist * 0.0015f);
    }
  }
  Obj* sel = g_sel >= 0 && g_sel < int(g_objs.size()) ? &g_objs[g_sel] : nullptr;
  const bool can_edit = sel && sel->movable && g_tool != Tool::kSelect;
  // gizmo: axes from the selection's pivot (screen space), 6 m long
  ImVec2 base_s, axis_s[3];
  bool gizmo = false;
  V3 at;
  if (can_edit) {
    at = sel->pivot + sel->pos;
    const V3 axes[3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, 1}};
    gizmo = ToScreen(at, base_s);
    for (int a = 0; a < 3 && gizmo; ++a) gizmo &= ToScreen(at + axes[a] * (g_dist * 0.12f), axis_s[a]);
    if (gizmo && g_tool == Tool::kMove) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImU32 cols[3] = {IM_COL32(235, 60, 60, 255), IM_COL32(70, 220, 90, 255), IM_COL32(70, 130, 255, 255)};
      for (int a = 0; a < 3; ++a) {
        dl->AddLine(base_s, axis_s[a], cols[a], 3);
        dl->AddCircleFilled(axis_s[a], 5, cols[a]);
      }
    }
  }
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    g_drag_axis = -1;
    const ImVec2 m = io.MousePos;
    if (gizmo && g_tool == Tool::kMove) {
      for (int a = 0; a < 3; ++a) {
        // distance from the mouse to the axis segment
        const ImVec2 s = base_s, e = axis_s[a];
        const float vx = e.x - s.x, vy = e.y - s.y, l2 = vx * vx + vy * vy;
        const float t = l2 > 0 ? std::clamp(((m.x - s.x) * vx + (m.y - s.y) * vy) / l2, 0.0f, 1.0f) : 0;
        const float dx = s.x + vx * t - m.x, dy = s.y + vy * t - m.y;
        if (dx * dx + dy * dy < 64) { g_drag_axis = a; break; }
      }
    }
    V3 ro, rd;
    Ray(m, ro, rd);
    if (g_drag_axis < 0) {
      float t;
      const int hit = Pick(ro, rd, &t);
      if (hit >= 0 && hit == g_sel && can_edit && g_tool == Tool::kMove) {
        g_drag_axis = 3;  // drag on the floor plane
        g_drag_hit = ro + rd * t;
      } else {
        g_sel = hit;
      }
    }
    if (g_drag_axis >= 0 && sel) {
      PushUndo();
      g_drag_start_pos = sel->pos;
      g_drag_start_yaw = sel->yaw;
      g_drag_start_scale = sel->scale;
      g_drag_mouse = m;
    }
    if (can_edit && g_drag_axis < 0 && g_sel == int(sel - g_objs.data()) && g_tool != Tool::kMove &&
        g_tool != Tool::kSelect) {
      PushUndo();
      g_drag_axis = 4;  // rotate / scale by horizontal drag
      g_drag_start_yaw = sel->yaw;
      g_drag_start_scale = sel->scale;
      g_drag_mouse = m;
    }
  }
  if (g_drag_axis >= 0 && sel && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const ImVec2 m = io.MousePos;
    if (g_drag_axis <= 2) {
      // along an axis: the mouse's move projected on the axis's screen direction
      const float vx = axis_s[g_drag_axis].x - base_s.x, vy = axis_s[g_drag_axis].y - base_s.y;
      const float l = std::sqrt(vx * vx + vy * vy);
      if (l > 1) {
        const float px = ((m.x - g_drag_mouse.x) * vx + (m.y - g_drag_mouse.y) * vy) / l;
        float units = px / l * (g_dist * 0.12f);
        if (g_snap) units = std::round(units);
        V3 p = g_drag_start_pos;
        if (g_drag_axis == 0) p.x += units;
        if (g_drag_axis == 1) p.y -= units;
        if (g_drag_axis == 2) p.z += units;
        sel->pos = p;
      }
    } else if (g_drag_axis == 3) {
      // on the horizontal plane through the grab point
      V3 ro, rd;
      Ray(m, ro, rd);
      if (std::fabs(rd.y) > 1e-4f) {
        const float t = (g_drag_hit.y - ro.y) / rd.y;
        const V3 now = ro + rd * t;
        V3 p = g_drag_start_pos + V3{now.x - g_drag_hit.x, 0, now.z - g_drag_hit.z};
        if (g_snap) p.x = std::round(p.x), p.z = std::round(p.z);
        sel->pos = p;
      }
    } else if (g_drag_axis == 4) {
      const float dx = m.x - g_drag_mouse.x;
      if (g_tool == Tool::kRotate) {
        float yaw = g_drag_start_yaw + dx * 0.01f;
        if (g_snap) yaw = std::round(yaw / (kPi / 12)) * (kPi / 12);
        sel->yaw = yaw;
      } else if (g_tool == Tool::kScale) {
        sel->scale = g_drag_start_scale * std::exp(dx * 0.005f);
      }
    }
    Constrain(*sel);
    ApplyObj(*sel);
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_drag_axis = -1;
  // keys
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_Z) && io.KeyCtrl) Undo();
    if (ImGui::IsKeyPressed(ImGuiKey_D) && io.KeyCtrl) Duplicate(g_sel);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) Delete(g_sel);
    if (ImGui::IsKeyPressed(ImGuiKey_W)) g_tool = Tool::kMove;
    if (ImGui::IsKeyPressed(ImGuiKey_E)) g_tool = Tool::kRotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) g_tool = Tool::kScale;
    if (ImGui::IsKeyPressed(ImGuiKey_Q)) g_tool = Tool::kSelect;
    if (ImGui::IsKeyPressed(ImGuiKey_F) && sel) g_target = sel->pivot + sel->pos;
  }
}

void ViewPreset(int which) {
  switch (which) {
    case 0: g_target = {0, -12, 0}, g_yaw = 0.0f, g_pitch = 0.32f, g_dist = 190; break;   // hard camera
    case 1: g_target = {0, -12, 0}, g_yaw = 0.0f, g_pitch = 1.5f, g_dist = 420; break;    // top
    case 2: g_target = {0, -12, 0}, g_yaw = kPi, g_pitch = 0.25f, g_dist = 260; break;    // from the stage
  }
}

}  // namespace

bool Lighting::Default() const {
  return color[0] == 1 && color[1] == 1 && color[2] == 1 && strength == 1 && crowd;
}

void Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Hooks& hooks) {
  g_dev = dev;
  g_ctx = ctx;
  g_hooks = hooks;
  CreatePipeline();
}

void FocusArea();  // (below)

void SetArena(Arena* arena, const std::string& title) {
  if (g_budget_thread.joinable()) g_budget_thread.join();
  g_arena = arena;
  g_title = title;
  g_budget_edits = -1;
  {
    std::lock_guard lock(g_budget_mutex);
    g_budget_file_max = 0;
  }
  BuildObjects();
  Edited();
  if (g_area_lo >= 0) FocusArea();
  if (g_test_view >= 0) ViewPreset(g_test_view);
  if (!g_test_select.empty())
    for (size_t i = 0; i < g_objs.size(); ++i)
      if (g_objs[i].label.rfind(g_test_select, 0) == 0) {
        g_sel = int(i);
        g_target = g_objs[i].pivot;
        break;
      }
}

void SetArea(const std::vector<std::pair<int, int>>& ids, const float* spot) {
  g_area = ids;
  g_area_lo = ids.empty() ? -1 : ids[0].first;
  for (int k = 0; k < 5; ++k) g_spot[k] = spot ? spot[k] : 0.0f;
  if (g_arena) BuildObjects();
}

// The camera on the shown area (its models' middle, from above at an angle).
void FocusArea() {
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const Obj& o : g_objs) {
    if (o.outside || o.effect) continue;
    for (const auto& vs : o.base)
      for (const auto& v : vs)
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v.pos[k]), hi[k] = std::max(hi[k], v.pos[k]);
  }
  if (lo[0] > hi[0]) return;
  g_target = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
  g_target.y = hi[1];  // (y down: the floor)
  g_dist = std::clamp(std::max(hi[0] - lo[0], hi[2] - lo[2]) * 1.3f, 60.0f, 600.0f);
  g_yaw = 0.6f, g_pitch = 1.0f;
}

bool Busy() { return g_budget_busy; }

void TestEdit() {
  if (!g_arena) return;
  if (g_sel >= 0 && g_objs[g_sel].movable) {
    g_objs[g_sel].pos.x += 20;
    ApplyObj(g_objs[g_sel]);
    Log("test edit: moved " + g_objs[g_sel].label + " 2 m");
  }
  if (g_area_lo < 0) g_target = {0, 0, 45};  // ringside, in front of the hard camera (else: the area's middle)
  if (g_area_lo >= 0) {
    if (g_spot[3] > 0) g_target = {g_spot[0] + g_spot[3] * 0.5f, g_spot[1], g_spot[2]};
    AddBox(15, 15, 15);
    char at[96];
    std::snprintf(at, sizeof at, "test edit: box at %.0f %.0f %.0f on %s", g_target.x, g_target.y, g_target.z,
                  g_sel >= 0 ? g_arena->models[g_objs[g_sel].model].model.name.c_str() : "?");
    Log(at);
  } else {
    AddBox(20, 20, 20);
  }
  g_ring.ropes[2].tint = 0xFF2020;
  g_light.color[2] = 0.8f;  // a little warmer
  Log("test edit: box added, top rope red, light warmer");
}

void TestLibrary(int lib) {
  if (!g_arena || lib < 0 || lib >= int(g_hooks.library.size())) return;
  g_lib.src = std::make_unique<Arena>();
  if (!g_lib.src->Load(g_hooks.library[lib].second)) return;
  g_lib.arena = lib;
  g_lib.models = LibraryModels(*g_lib.src);
  int n = 0;
  for (int k : g_lib.models)
    if (g_lib.src->models[k].zone == Zone::kEntrance) AddFromLibrary(k, false), ++n;
  Log("test library: " + std::to_string(n) + " entrance models from " + g_hooks.library[lib].first);
}

void TestStart(int view, const std::string& select) {
  g_test_view = view;
  g_test_select = select;
}

void Shutdown() {
  if (g_budget_thread.joinable()) g_budget_thread.join();
  if (g_lib.loader.joinable()) g_lib.loader.join();
}
RingSpec& Ring() { return g_ring; }
Lighting& Light() { return g_light; }

// A changed static model's node sphere (world space: what the game culls the
// model by) grown to hold its meshes, added ones too.
void GrowSpheres(Arena& a) {
  for (auto& am : a.models) {
    if (!am.changed && !am.added) continue;
    if (am.model.nodes.size() != 1) continue;
    float* ns = am.model.nodes[0].sphere;
    for (const auto& s : am.model.meshes) {
      if (s.verts.size() <= 1) continue;
      const V3 c{s.sphere[0], s.sphere[1], s.sphere[2]}, n{ns[0], ns[1], ns[2]};
      const float d = std::sqrt(Dot(c - n, c - n));
      if (d + s.sphere[3] <= ns[3]) continue;
      if (ns[3] <= 0 || d + ns[3] <= s.sphere[3]) {  // (none yet, or the mesh's holds it)
        ns[0] = c.x, ns[1] = c.y, ns[2] = c.z, ns[3] = s.sphere[3];
        continue;
      }
      const float r = (d + ns[3] + s.sphere[3]) / 2;
      const V3 m = n + (c - n) * ((r - ns[3]) / std::max(d, 1e-6f));
      ns[0] = m.x, ns[1] = m.y, ns[2] = m.z, ns[3] = r;
    }
  }
}

void ApplyBuild(Arena& a) {
  GrowSpheres(a);
  if (g_light.color[0] != 1 || g_light.color[1] != 1 || g_light.color[2] != 1 || g_light.strength != 1) {
    const float c[3] = {g_light.color[0] * g_light.strength, g_light.color[1] * g_light.strength,
                        g_light.color[2] * g_light.strength};
    for (auto& am : a.models) {
      if (g_area_lo >= 0 && !am.added && !InArea(am.id)) continue;  // (backstage: the room only)
      for (auto& s : am.model.meshes)
        for (auto& p : s.params)
          if (p.type == 0x0d && p.value.size() >= 16 && (p.name == "g_f4MatAmbCol" || p.name == "g_f4MatDifCol"))
            for (int k = 0; k < 3; ++k) PutBeF(&p.value[4 * k], BeF(&p.value[4 * k]) * c[k]);
      am.changed = true;
    }
  }
  if (g_area_lo >= 0) return;  // (backstage: no crowd, no ring)
  if (!g_light.crowd) {
    const int n = HideCrowd(a);
    Log("  crowd left out (" + std::to_string(n) + " crowd models)");
  }
  RingReport rep;
  ApplyRing(a, g_ring, rep);
  for (const auto& w : rep.warnings) Log("  ring: " + w);
}

std::string ManifestLines() {
  std::string s = g_ring.Default() ? "" : g_ring.ManifestLines();
  if (!g_light.Default()) {
    char b[128];
    std::snprintf(b, sizeof b, "light.color=%.3f %.3f %.3f\nlight.strength=%.3f\n", g_light.color[0], g_light.color[1],
                  g_light.color[2], g_light.strength);
    s += b;
    if (!g_light.crowd) s += "crowd=0\n";
  }
  return s;
}

void FromManifest(const std::string& text) {
  g_ring = RingSpec();
  g_ring.FromManifest(text);
  g_light = Lighting();
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("light.color=", 0) == 0) {
      std::istringstream v(line.substr(12));
      v >> g_light.color[0] >> g_light.color[1] >> g_light.color[2];
      g_light.preset = int(std::size(kPresets)) - 1;
    } else if (line == "crowd=0") {
      g_light.crowd = false;
    } else if (line.rfind("light.strength=", 0) == 0) {
      g_light.strength = std::strtof(line.c_str() + 15, nullptr);
      g_light.preset = int(std::size(kPresets)) - 1;
    }
  }
}

void Draw() {
  if (!g_arena) {
    ImGui::TextDisabled("Pick an arena on the Arenas page first.");
    return;
  }
  const float scale = ImGui::GetFontSize() / 17.0f;  // (the Mod Maker font is 17 px at 96 dpi)
  // toolbar
  auto tool = [](const char* label, Tool t) {
    const bool on = g_tool == t;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.06f, 0.18f, 1));
    if (ImGui::Button(label)) g_tool = t;
    if (on) ImGui::PopStyleColor();
    ImGui::SameLine();
  };
  tool("Select (Q)", Tool::kSelect);
  tool("Move (W)", Tool::kMove);
  tool("Turn (E)", Tool::kRotate);
  tool("Scale (R)", Tool::kScale);
  ImGui::Checkbox("Snap", &g_snap);
  ImGui::SameLine();
  if (ImGui::Button("Undo")) Undo();
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  if (ImGui::Button("Hard camera")) ViewPreset(0);
  ImGui::SameLine();
  if (ImGui::Button("Top")) ViewPreset(1);
  ImGui::SameLine();
  if (ImGui::Button("From the stage")) ViewPreset(2);
  ImGui::SameLine();
  ImGui::Checkbox("Ring", &g_show_ring);
  ImGui::SameLine();
  ImGui::Checkbox("Mirror", &g_mirror);
  if (g_hooks.test_in_game) {
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - 150 * scale));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.42f, 0.25f, 1));
    if (ImGui::Button("Test in game", ImVec2(150 * scale, 0))) g_hooks.test_in_game();
    ImGui::PopStyleColor();
  }
  const float left = 210 * scale, right = 290 * scale;
  ImGui::BeginChild("outliner", ImVec2(left, 0), true);
  Outliner();
  ImGui::EndChild();
  ImGui::SameLine();
  // viewport
  ImGui::BeginChild("viewport", ImVec2(-right, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const int w = std::max(16, int(avail.x)), h = std::max(16, int(avail.y));
  ResizeTarget(w, h);
  g_vp_min = ImGui::GetCursorScreenPos();
  g_vp_size = ImVec2(float(w), float(h));
  RenderScene();
  ImGui::InvisibleButton("vp", g_vp_size,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();
  ImGui::GetWindowDrawList()->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(g_rt_srv)), g_vp_min,
                                       ImVec2(g_vp_min.x + w, g_vp_min.y + h));
  ViewportInput(hovered);
  ImGui::GetWindowDrawList()->AddText(ImVec2(g_vp_min.x + 8, g_vp_min.y + 6), IM_COL32(200, 200, 210, 200),
                                      "Right drag: orbit   Middle drag: pan   Wheel: zoom   F: focus   Ctrl+Z: undo");
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("inspector", ImVec2(0, 0), true);
  if (ImGui::CollapsingHeader("Object", ImGuiTreeNodeFlags_DefaultOpen)) Inspector();
  if (ImGui::CollapsingHeader("Add", ImGuiTreeNodeFlags_DefaultOpen)) AddPanel();
  if (ImGui::CollapsingHeader("Library (other arenas)")) LibraryPanel();
  if (g_area_lo < 0 && ImGui::CollapsingHeader("Ring Kit", ImGuiTreeNodeFlags_DefaultOpen)) RingKitPanel();
  if (ImGui::CollapsingHeader("Lighting")) LightingPanel();
  if (ImGui::CollapsingHeader("Size budget", ImGuiTreeNodeFlags_DefaultOpen)) BudgetPanel();
  ImGui::EndChild();
}

}  // namespace editor
