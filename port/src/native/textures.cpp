// WWE SmackDown vs. Raw 2011 - native renderer textures (see textures.h).

#include "native/textures.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>  // xxHash (inline)
#include <rex/logging.h>
#include <fmt/format.h>
#include <rex/system/xmemory.h>

using Microsoft::WRL::ComPtr;

namespace svr2011::native::textures {

namespace {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;
using rex::graphics::FormatInfo;
using TF = xenos::TextureFormat;

constexpr uint32_t kPhysicalSize = 0x20000000;
constexpr uint64_t kRecheckFrames = 120;  // re-hash guest data this often
// ... but more often while a texture is new: a video's texture is created
// with its first frame and must be seen changing right away (with 120 its
// start stayed frozen for up to 2 s).
constexpr uint64_t kNewFrames = 240, kNewRecheckFrames = 4;
// (textures seen changing are "dynamic": re-hashed at every frame they're used)

// ---------------------------------------------------------------------------
// formats

// How guest blocks become host data.
enum class Convert : uint8_t {
  kNone,   // unsupported
  kCopy,   // same layout (the guest block is the host block)
  k565,    // 16-bit packed -> RGBA8 (X in the low bits, like every Xenos format)
  k655,
  k1555,
  k4444,
  kDXT3A,  // 4x4 block of 4-bit alpha -> R8
  kCTX1,   // 4x4 block, two RG8 endpoints + 2-bit indices -> RG8
};

struct HostFormat {
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT gamma = DXGI_FORMAT_UNKNOWN;  // format for gamma (sign 3) textures
  Convert convert = Convert::kNone;
  uint32_t components = 4;  // host channels: missing ones replicate the last
  bool block_compressed = false;
};

HostFormat GetHostFormat(TF f) {
  switch (f) {
    case TF::k_8:
    case TF::k_8_A:
    case TF::k_8_B:
      return {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN, Convert::kCopy, 1};
    case TF::k_8_8:
      return {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_UNKNOWN, Convert::kCopy, 2};
    case TF::k_8_8_8_8:
    case TF::k_8_8_8_8_A:
    case TF::k_8_8_8_8_AS_16_16_16_16:
      return {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, Convert::kCopy, 4};
    case TF::k_2_10_10_10:
    case TF::k_2_10_10_10_AS_16_16_16_16:
      return {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN, Convert::kCopy, 4};
    case TF::k_5_6_5:
      return {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, Convert::k565, 4};
    case TF::k_6_5_5:
      return {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, Convert::k655, 4};
    case TF::k_1_5_5_5:
      return {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, Convert::k1555, 4};
    case TF::k_4_4_4_4:
      return {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, Convert::k4444, 4};
    case TF::k_DXT1:
    case TF::k_DXT1_AS_16_16_16_16:
      return {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB, Convert::kCopy, 4, true};
    case TF::k_DXT2_3:
    case TF::k_DXT2_3_AS_16_16_16_16:
      return {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM_SRGB, Convert::kCopy, 4, true};
    case TF::k_DXT4_5:
    case TF::k_DXT4_5_AS_16_16_16_16:
      return {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM_SRGB, Convert::kCopy, 4, true};
    case TF::k_DXN:
      return {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_UNKNOWN, Convert::kCopy, 2, true};
    case TF::k_DXT5A:
      return {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_UNKNOWN, Convert::kCopy, 1, true};
    case TF::k_DXT3A:
      return {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN, Convert::kDXT3A, 1};
    case TF::k_CTX1:
      return {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_UNKNOWN, Convert::kCTX1, 2};
    case TF::k_16:
      return {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_UNKNOWN, Convert::kCopy, 1};
    case TF::k_16_16:
      return {DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_UNKNOWN, Convert::kCopy, 2};
    case TF::k_16_16_16_16:
      return {DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_UNKNOWN, Convert::kCopy, 4};
    case TF::k_16_EXPAND:
    case TF::k_16_FLOAT:
      return {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_UNKNOWN, Convert::kCopy, 1};
    case TF::k_16_16_EXPAND:
    case TF::k_16_16_FLOAT:
      return {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_UNKNOWN, Convert::kCopy, 2};
    case TF::k_16_16_16_16_EXPAND:
    case TF::k_16_16_16_16_FLOAT:
      return {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN, Convert::kCopy, 4};
    case TF::k_32_FLOAT:
      return {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, Convert::kCopy, 1};
    case TF::k_32_32_FLOAT:
      return {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_UNKNOWN, Convert::kCopy, 2};
    case TF::k_32_32_32_32_FLOAT:
      return {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_UNKNOWN, Convert::kCopy, 4};
    default:
      return {};
  }
}

uint32_t Log2(uint32_t v) {
  uint32_t n = 0;
  while ((1u << (n + 1)) <= v) ++n;
  return n;
}

// ---------------------------------------------------------------------------
// state

struct Entry {
  ComPtr<ID3D12Resource> resource;
  uint32_t srv = UINT32_MAX;
  bool failed = false;
  bool dynamic = false;  // its guest data has changed after upload
  uint64_t created_frame = 0;
  uint64_t hash = 0;
  uint64_t checked_frame = 0;
  uint32_t base_address = 0, base_size = 0, mip_address = 0, mip_size = 0;
};

rex::memory::Memory* g_memory = nullptr;
const uint8_t* g_physical = nullptr;
uint32_t g_srv_next = 0, g_srv_end = 0;
uint32_t g_sampler_next = 0, g_sampler_end = 0;
std::unordered_map<uint64_t, Entry> g_textures;   // key hash -> texture
std::unordered_map<uint64_t, uint32_t> g_samplers;  // key -> heap index
// Resolve destinations (base page -> the renderer's copy of the target).
struct Resolved {
  ID3D12Resource* resource = nullptr;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN, gamma_format = DXGI_FORMAT_UNKNOWN;
  uint32_t components = 4;
  bool swap_rb = false;
  uint32_t generation = 0;  // changes with the resource (views are per generation)
};
std::unordered_map<uint32_t, Resolved> g_resolved;
uint32_t g_resolved_generation = 0;
std::unordered_map<uint64_t, uint32_t> g_resolved_views;  // (resource, swizzle, gamma) -> SRV
Stats g_stats;

uint64_t GuestHash(const Entry& e) {
  const auto t0 = std::chrono::steady_clock::now();
  struct Done {
    std::chrono::steady_clock::time_point t0;
    ~Done() {
      g_stats.hash_ms +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
  } done{t0};
  ++g_stats.hashes;
  g_stats.hash_bytes += uint64_t(e.base_size) + e.mip_size;
  uint64_t h = 0;
  if (e.base_size) h ^= XXH3_64bits(g_physical + e.base_address, e.base_size);
  if (e.mip_size) h ^= XXH3_64bits(g_physical + e.mip_address, e.mip_size) * 31;
  return h;
}

// Reads `bytes` guest bytes at physical `address` as the GPU sees them after
// the fetch constant's endian swap: byte o comes from o ^ mask.
void ReadGuest(uint8_t* out, uint32_t address, uint32_t bytes, uint32_t mask) {
  if (address >= kPhysicalSize || bytes > kPhysicalSize - address) {
    std::memset(out, 0, bytes);
    return;
  }
  if (!mask) {
    std::memcpy(out, g_physical + address, bytes);
    return;
  }
  for (uint32_t i = 0; i < bytes; ++i) {
    out[i] = g_physical[((address + i) ^ mask) & (kPhysicalSize - 1)];
  }
}

// ReadGuest for a whole run of bytes starting 4-byte aligned: the endian
// swap works a word at a time.
void ReadGuestRun(uint8_t* out, uint32_t address, uint32_t bytes, uint32_t mask) {
  if ((address & 3) || address >= kPhysicalSize || bytes > kPhysicalSize - address) {
    ReadGuest(out, address, bytes, mask);
    return;
  }
  const uint8_t* src = g_physical + address;
  const uint32_t words = bytes / 4;
  switch (mask) {
    case 0:
      std::memcpy(out, src, bytes);
      return;
    case 3:  // 8in32
      for (uint32_t i = 0; i < words; ++i) {
        uint32_t w;
        std::memcpy(&w, src + 4 * i, 4);
        w = _byteswap_ulong(w);
        std::memcpy(out + 4 * i, &w, 4);
      }
      break;
    case 1:  // 8in16
      for (uint32_t i = 0; i < words; ++i) {
        uint32_t w;
        std::memcpy(&w, src + 4 * i, 4);
        w = ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
        std::memcpy(out + 4 * i, &w, 4);
      }
      break;
    default:  // 2: 16in32
      for (uint32_t i = 0; i < words; ++i) {
        uint32_t w;
        std::memcpy(&w, src + 4 * i, 4);
        w = (w << 16) | (w >> 16);
        std::memcpy(out + 4 * i, &w, 4);
      }
      break;
  }
  if (bytes & 3) ReadGuest(out + 4 * words, address + 4 * words, bytes & 3, mask);
}

inline uint8_t Expand(uint32_t v, uint32_t bits) {
  return uint8_t((v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1));
}

// Writes one guest block (already endian-swapped) to the host subresource.
void StoreBlock(Convert convert, const uint8_t* block, uint32_t bpb, uint8_t* dst_base,
                uint32_t row_pitch, uint32_t host_width, uint32_t host_height, uint32_t bx,
                uint32_t by) {
  switch (convert) {
    case Convert::kCopy:
      std::memcpy(dst_base + size_t(by) * row_pitch + size_t(bx) * bpb, block, bpb);
      return;
    case Convert::k565:
    case Convert::k655:
    case Convert::k1555:
    case Convert::k4444: {
      if (bx >= host_width || by >= host_height) return;
      const uint32_t v = block[0] | (block[1] << 8);
      uint8_t* d = dst_base + size_t(by) * row_pitch + size_t(bx) * 4;
      switch (convert) {
        case Convert::k565:
          d[0] = Expand(v & 31, 5); d[1] = Expand((v >> 5) & 63, 6); d[2] = Expand(v >> 11, 5); d[3] = 255;
          break;
        case Convert::k655:
          d[0] = Expand(v & 63, 6); d[1] = Expand((v >> 6) & 31, 5); d[2] = Expand(v >> 11, 5); d[3] = 255;
          break;
        case Convert::k1555:
          d[0] = Expand(v & 31, 5); d[1] = Expand((v >> 5) & 31, 5); d[2] = Expand((v >> 10) & 31, 5);
          d[3] = (v >> 15) ? 255 : 0;
          break;
        default:
          d[0] = Expand(v & 15, 4); d[1] = Expand((v >> 4) & 15, 4); d[2] = Expand((v >> 8) & 15, 4);
          d[3] = Expand(v >> 12, 4);
          break;
      }
      return;
    }
    case Convert::kDXT3A:
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x >= host_width || y >= host_height) continue;
        dst_base[size_t(y) * row_pitch + x] = uint8_t(((block[i >> 1] >> ((i & 1) * 4)) & 15) * 17);
      }
      return;
    case Convert::kCTX1: {
      const uint8_t e[4][2] = {
          {block[0], block[1]},
          {block[2], block[3]},
          {uint8_t((2 * block[0] + block[2] + 1) / 3), uint8_t((2 * block[1] + block[3] + 1) / 3)},
          {uint8_t((block[0] + 2 * block[2] + 1) / 3), uint8_t((block[1] + 2 * block[3] + 1) / 3)}};
      const uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (uint32_t(block[7]) << 24);
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x >= host_width || y >= host_height) continue;
        const uint8_t* c = e[(indices >> (2 * i)) & 3];
        dst_base[size_t(y) * row_pitch + x * 2] = c[0];
        dst_base[size_t(y) * row_pitch + x * 2 + 1] = c[1];
      }
      return;
    }
    default:
      return;
  }
}

// Debugging: SVR2011_NATIVE_TEXTURE_DUMP=<dir> writes the base level of each
// uploaded DXT texture as <dir>/<n>_<w>x<h>.dds.
void DumpLevel0(TF format, uint32_t width, uint32_t height, const uint8_t* data, uint32_t row_pitch,
                uint32_t rows, size_t row_bytes) {
  static const char* dir = std::getenv("SVR2011_NATIVE_TEXTURE_DUMP");
  static uint32_t count = 0;
  if (!dir) return;
  const char* fourcc = format == TF::k_DXT1 ? "DXT1" : format == TF::k_DXT2_3 ? "DXT3"
                       : format == TF::k_DXT4_5 ? "DXT5" : nullptr;
  if (!fourcc) return;
  char path[512];
  std::snprintf(path, sizeof(path), "%s/%04u_%ux%u.dds", dir, count++, width, height);
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  uint32_t header[32] = {};
  header[0] = 0x20534444;  // "DDS "
  header[1] = 124;
  header[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000;  // caps, height, width, pixel format, linear size
  header[3] = height;
  header[4] = width;
  header[5] = uint32_t(row_bytes * rows);
  header[19] = 32;
  header[20] = 0x4;  // fourcc
  std::memcpy(&header[21], fourcc, 4);
  header[27] = 0x1000;  // texture
  std::fwrite(header, 4, 32, f);
  for (uint32_t y = 0; y < rows; ++y) std::fwrite(data + size_t(y) * row_pitch, 1, row_bytes, f);
  std::fclose(f);
}

// D3D12 view mapping for a Xenos fetch swizzle (both encode 0-3 xyzw, 4 zero,
// 5 one); components the format lacks repeat its last one (as Xenia does).
uint32_t ComponentMapping(uint32_t swizzle, uint32_t components, bool swap_rb = false) {
  uint32_t mapping = D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t c = (swizzle >> (3 * i)) & 7;
    if (swap_rb && (c == 0 || c == 2)) c ^= 2;
    if (c <= 3) c = std::min(c, components - 1);
    else if (c > 5) c = 4;
    static const bool alpha1 = std::getenv("SVR2011_NATIVE_ALPHA1") != nullptr;  // debug
    if (alpha1 && i == 3) c = 5;
    mapping |= c << (3 * i);
  }
  return mapping;
}

uint32_t ResolvedView(const Context& ctx, const Resolved& res,
                      const xenos::xe_gpu_texture_fetch_t& fetch) {
  const bool gamma = fetch.sign_x == xenos::TextureSign::kGamma &&
                     res.gamma_format != DXGI_FORMAT_UNKNOWN;
  const uint64_t key = uint64_t(res.generation) | (uint64_t(fetch.swizzle) << 32) |
                       (uint64_t(gamma) << 44) | (uint64_t(res.swap_rb) << 45) |
                       (uint64_t(res.format) << 46);
  auto it = g_resolved_views.find(key);
  if (it != g_resolved_views.end()) return it->second;
  if (g_srv_next >= g_srv_end) return UINT32_MAX;
  const uint32_t srv = g_srv_next++;
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = gamma ? res.gamma_format : res.format;
  sd.Shader4ComponentMapping = ComponentMapping(fetch.swizzle, res.components, res.swap_rb);
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MipLevels = 1;
  D3D12_CPU_DESCRIPTOR_HANDLE h = ctx.srv_heap->GetCPUDescriptorHandleForHeapStart();
  h.ptr += size_t(srv) *
           ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  ctx.device->CreateShaderResourceView(res.resource, &sd, h);
  g_resolved_views.emplace(key, srv);
  return srv;
}

// Creates the resource and SRV for a texture and records its upload.
bool Upload(const Context& ctx, const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t dimension,
            Entry& e) {
  struct Timed {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~Timed() {
      g_stats.upload_ms +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
  } timed;
  const TF format = fetch.format;
  const HostFormat host = GetHostFormat(format);
  const FormatInfo* info = FormatInfo::Get(format);
  if (host.convert == Convert::kNone || !info) return false;

  const xenos::DataDimension dim = fetch.dimension;
  const bool is_3d = dim == xenos::DataDimension::k3D;
  const bool is_cube = dim == xenos::DataDimension::kCube;
  if (dim == xenos::DataDimension::k1D) return false;
  // The view the shader declares must match the texture's type.
  if ((dimension == 1) != is_3d || (dimension == 2) != is_cube) return false;

  uint32_t w1, h1, d1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &w1, &h1, &d1, &base_page, &mip_page,
                                                 &mip_min, &mip_max);
  const uint32_t width = w1 + 1, height = h1 + 1, depth_or_array = d1 + 1;
  if (!base_page && !mip_page) return false;
  const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      dim, fetch.pitch, width, height, depth_or_array, fetch.tiled, format, fetch.packed_mips,
      base_page != 0, mip_max);

  const uint32_t array_size = is_3d ? 1 : depth_or_array;
  const uint32_t depth = is_3d ? depth_or_array : 1;
  const uint32_t levels = mip_max + 1;
  // Only a 2D view of the first slice is possible for stacked textures.
  if (dimension == 0 && array_size != 1) return false;

  const uint32_t gbw = info->block_width, gbh = info->block_height;
  const uint32_t bpb = info->bytes_per_block();
  if (!bpb || (bpb & (bpb - 1))) return false;
  // Block-compressed host textures need the base size in whole blocks.
  const uint32_t host_width = host.block_compressed ? (width + 3) & ~3u : width;
  const uint32_t host_height = host.block_compressed ? (height + 3) & ~3u : height;

  const bool gamma = fetch.sign_x == xenos::TextureSign::kGamma;
  const DXGI_FORMAT resource_format =
      gamma && host.gamma != DXGI_FORMAT_UNKNOWN ? host.gamma : host.format;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = is_3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = host_width;
  desc.Height = host_height;
  desc.DepthOrArraySize = uint16_t(is_3d ? depth : array_size);
  desc.MipLevels = uint16_t(levels);
  desc.Format = resource_format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  // A re-upload (guest data changed) reuses the resource and its view: the
  // entry's key fixes the layout, and frames still in flight keep a valid
  // descriptor (queue order serializes the copy after their reads).
  ComPtr<ID3D12Resource> resource = e.resource;
  const bool reuse = resource != nullptr;
  if (reuse) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    ctx.list->ResourceBarrier(1, &b);
  } else if (FAILED(ctx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&resource)))) {
    return false;
  }

  if (!reuse) {
    const std::string n = fmt::format("texture {:08X} {}x{}x{} fmt {}", base_page << 12, width,
                                      height, is_3d ? depth : array_size, uint32_t(format));
    resource->SetName(std::wstring(n.begin(), n.end()).c_str());
  }
  const uint32_t subresources = levels * array_size;
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
  std::vector<UINT> rows(subresources);
  std::vector<UINT64> row_bytes(subresources);
  UINT64 total = 0;
  ctx.device->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), rows.data(),
                                    row_bytes.data(), &total);
  // Staging: the frame's upload ring (textures that change every frame -
  // videos - would otherwise create and free a buffer per frame), or a buffer
  // of its own when the ring has no room.
  ComPtr<ID3D12Resource> staging;
  ID3D12Resource* staging_buffer = nullptr;
  uint64_t staging_offset = 0;
  uint8_t* mapped = nullptr;
  if (ctx.allocate) {
    const Context::UploadSpace space =
        ctx.allocate(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    mapped = space.cpu;
    staging_buffer = space.buffer;
    staging_offset = space.offset;
  }
  if (!mapped) {
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(ctx.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&staging)))) {
      return false;
    }
    staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    staging_buffer = staging.Get();
  }
  std::memset(mapped, 0, size_t(total));

  static const uint32_t kEndianMask[4] = {0, 1, 3, 2};
  const uint32_t mask = kEndianMask[uint32_t(fetch.endianness) & 3];
  const uint32_t bpb_log2 = Log2(bpb);
  uint8_t block[16];

  const auto convert_t0 = std::chrono::steady_clock::now();
  for (uint32_t level = 0; level < levels; ++level) {
    const uint32_t page = level ? mip_page : base_page;
    if (!page) continue;  // level not present (left black)
    const uint32_t stored = std::min(level, layout.packed_level);
    const texture_util::TextureGuestLayout::Level& gl = level ? layout.mips[stored] : layout.base;
    const uint32_t level_address = (page << 12) + (level ? layout.mip_offsets_bytes[stored] : 0);
    uint32_t ox = 0, oy = 0, oz = 0;
    if (level >= layout.packed_level) {
      texture_util::GetPackedMipOffset(width, height, depth, format, level, ox, oy, oz);
    }
    const uint32_t lw = std::max(host_width >> level, 1u), lh = std::max(host_height >> level, 1u);
    const uint32_t ld = std::max(depth >> level, 1u);
    const uint32_t blocks_x = (lw + gbw - 1) / gbw, blocks_y = (lh + gbh - 1) / gbh;
    const uint32_t pitch_blocks = gl.row_pitch_bytes / bpb;

    for (uint32_t slice = 0; slice < array_size; ++slice) {
      const uint32_t sub = level + slice * levels;
      const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = footprints[sub];
      const uint32_t slice_address = level_address + slice * gl.array_slice_stride_bytes;
      for (uint32_t z = 0; z < ld; ++z) {
        uint8_t* dst = mapped + fp.Offset + size_t(z) * rows[sub] * fp.Footprint.RowPitch;
        if (!fetch.tiled && host.convert == Convert::kCopy) {
          // Linear and copied as is: whole rows (videos update such
          // textures every frame; per block, 8-bit ones cost a call a texel).
          thread_local std::vector<uint8_t> row;
          const uint32_t row_bytes_n = blocks_x * bpb;
          if (row.size() < row_bytes_n) row.resize(row_bytes_n);
          for (uint32_t by = 0; by < blocks_y; ++by) {
            const int64_t offset = (int64_t(z + oz) * gl.z_slice_stride_block_rows + by + oy) *
                                       gl.row_pitch_bytes + int64_t(ox) * bpb;
            ReadGuestRun(row.data(), uint32_t(slice_address + offset), row_bytes_n, mask);
            std::memcpy(dst + size_t(by) * fp.Footprint.RowPitch, row.data(), row_bytes_n);
          }
          continue;
        }
        for (uint32_t by = 0; by < blocks_y; ++by) {
          for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            const uint32_t gx = bx + ox, gy = by + oy, gz = z + oz;
            int64_t offset;
            if (fetch.tiled) {
              offset = is_3d ? texture_util::GetTiledOffset3D(int32_t(gx), int32_t(gy), int32_t(gz),
                                                              pitch_blocks, gl.z_slice_stride_block_rows,
                                                              bpb_log2)
                             : texture_util::GetTiledOffset2D(int32_t(gx), int32_t(gy), pitch_blocks,
                                                              bpb_log2);
            } else {
              offset = (int64_t(gz) * gl.z_slice_stride_block_rows + gy) * gl.row_pitch_bytes +
                       int64_t(gx) * bpb;
            }
            ReadGuest(block, uint32_t(slice_address + offset), bpb, mask);
            StoreBlock(host.convert, block, bpb, dst, fp.Footprint.RowPitch, fp.Footprint.Width,
                       fp.Footprint.Height, bx, by);
          }
        }
      }
    }
  }
  g_stats.convert_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - convert_t0).count();
  for (uint32_t level = 0; level < levels; ++level) {
    DumpLevel0(format, footprints[level].Footprint.Width, footprints[level].Footprint.Height,
               mapped + footprints[level].Offset, footprints[level].Footprint.RowPitch,
               rows[level], size_t(row_bytes[level]));
  }
  if (staging) staging->Unmap(0, nullptr);

  for (uint32_t sub = 0; sub < subresources; ++sub) {
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = staging_buffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprints[sub];
    src.PlacedFootprint.Offset += staging_offset;
    dst.pResource = resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = sub;
    ctx.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource.Get();
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  b.Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  ctx.list->ResourceBarrier(1, &b);
  if (staging) ctx.retire(staging);

  // The view, with the fetch constant's swizzle (Xenos and D3D12 encode
  // component selection the same way: 0-3 xyzw, 4 zero, 5 one).
  if (reuse) {
    e.hash = GuestHash(e);
    e.checked_frame = ctx.frame;
    ++g_stats.uploads;
    g_stats.upload_bytes += uint64_t(e.base_size) + e.mip_size;
    return true;
  }
  if (e.srv == UINT32_MAX) {
    if (g_srv_next >= g_srv_end) {
      REXLOG_WARN("native renderer: texture descriptor heap full");
      return false;
    }
    e.srv = g_srv_next++;
  }
  const uint32_t mapping = ComponentMapping(fetch.swizzle, host.components);
  D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = resource_format;
  sd.Shader4ComponentMapping = mapping;
  if (is_3d) {
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    sd.Texture3D.MipLevels = levels;
  } else if (is_cube) {
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    sd.TextureCube.MipLevels = levels;
  } else {
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = levels;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE h = ctx.srv_heap->GetCPUDescriptorHandleForHeapStart();
  h.ptr += size_t(e.srv) *
           ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  ctx.device->CreateShaderResourceView(resource.Get(), &sd, h);

  e.resource = resource;
  e.base_address = base_page << 12;
  e.base_size = base_page ? std::min(layout.base.level_data_extent_bytes, kPhysicalSize - e.base_address) : 0;
  e.mip_address = mip_page << 12;
  e.mip_size = mip_page ? std::min(layout.mips_total_extent_bytes, kPhysicalSize - e.mip_address) : 0;
  e.hash = GuestHash(e);
  e.checked_frame = ctx.frame;
  ++g_stats.uploads;
  g_stats.upload_bytes += uint64_t(e.base_size) + e.mip_size;
  static const bool log = std::getenv("SVR2011_NATIVE_TEXTURE_LOG") != nullptr;
  if (log) {
    static const char kSwizzle[] = "xyzw01??";
    char swizzle[5] = {};
    for (uint32_t i = 0; i < 4; ++i) swizzle[i] = kSwizzle[(fetch.swizzle >> (3 * i)) & 7];
    REXLOG_INFO(
        "native texture {}: {} {}x{}x{} dim {} levels {} (min {}) tiled {} packed {} endian {} "
        "swizzle {} sign {}{}{}{} base {:08X} mips {:08X} pitch {} bias {} filter {}/{}/{} aniso {}",
        e.srv, info->name, width, height, depth_or_array, uint32_t(dim), levels, mip_min,
        uint32_t(fetch.tiled), uint32_t(fetch.packed_mips), uint32_t(fetch.endianness), swizzle,
        uint32_t(fetch.sign_x), uint32_t(fetch.sign_y), uint32_t(fetch.sign_z),
        uint32_t(fetch.sign_w), e.base_address, e.mip_address, uint32_t(fetch.pitch),
        int32_t(fetch.lod_bias), uint32_t(fetch.mag_filter), uint32_t(fetch.min_filter),
        uint32_t(fetch.mip_filter), uint32_t(fetch.aniso_filter));
  }
  return true;
}

}  // namespace

void Initialize(rex::memory::Memory* memory, uint32_t srv_first, uint32_t srv_count,
                uint32_t sampler_first, uint32_t sampler_count) {
  g_memory = memory;
  g_physical = memory->TranslatePhysical(0);
  g_srv_next = srv_first;
  g_srv_end = srv_first + srv_count;
  g_sampler_next = sampler_first;
  g_sampler_end = sampler_first + sampler_count;
}

uint32_t Texture(const Context& ctx, const uint32_t fetch_dwords[6], uint32_t dimension) {
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));
  // Type 2 is a texture; the game also samples through type 0 ("invalid")
  // constants (entrances, finishers), which the Xbox 360 treats as textures.
  if (uint32_t(fetch.type) != 2 && uint32_t(fetch.type) != 0) {  // not a texture
    ++g_stats.unsupported;
    return UINT32_MAX;
  }
  if (auto it = g_resolved.find(fetch.base_address); it != g_resolved.end()) {
    // A render target copy: sampled from the renderer's resource.
    if (dimension != 0) {
      ++g_stats.unsupported;
      return UINT32_MAX;
    }
    return ResolvedView(ctx, it->second, fetch);
  }
  // Everything but the sampler state (clamp, filters, LOD bias, border).
  const uint32_t key_dwords[7] = {fetch_dwords[0] & 0xFFC003FFu, fetch_dwords[1], fetch_dwords[2],
                                  fetch_dwords[3] & 0x7FFFFu,     fetch_dwords[4] & 0x3FCu,
                                  fetch_dwords[5] & 0xFFFFFE00u,  dimension};
  const uint64_t key = XXH3_64bits(key_dwords, sizeof(key_dwords));
  Entry& e = g_textures[key];
  if (e.failed) {
    ++g_stats.unsupported;
    return UINT32_MAX;
  }
  if (!e.resource) {
    e.created_frame = ctx.frame;
    if (!Upload(ctx, fetch, dimension, e)) {
      e.failed = true;
      static uint32_t logged = 0;
      if (logged++ < 32) {
        REXLOG_INFO("native renderer: texture not supported: format {} dimension {}/{} {}x{}",
                    uint32_t(fetch.format), uint32_t(fetch.dimension), dimension,
                    fetch.size_2d.width + 1, fetch.size_2d.height + 1);
      }
      ++g_stats.unsupported;
      return UINT32_MAX;
    }
  } else if (ctx.frame >= e.checked_frame +
                              (e.dynamic ? 1
                               : ctx.frame < e.created_frame + kNewFrames ? kNewRecheckFrames
                                                                          : kRecheckFrames)) {
    e.checked_frame = ctx.frame;
    if (GuestHash(e) != e.hash) {
      if (!e.dynamic) {
        static uint32_t logged = 0;
        if (logged++ < 40) {
          REXLOG_INFO("native renderer: dynamic texture {:08X} format {} {}x{} tiled {} ({} KB)",
                      e.base_address, uint32_t(fetch.format), fetch.size_2d.width + 1,
                      fetch.size_2d.height + 1, uint32_t(fetch.tiled), e.base_size >> 10);
        }
      }
      e.dynamic = true;
      Upload(ctx, fetch, dimension, e);
    }
  }
  return e.srv;
}

uint32_t Sampler(const Context& ctx, const uint32_t fetch[6]) {
  const uint64_t key = uint64_t((fetch[0] >> 10) & 0x1FF) | (uint64_t((fetch[3] >> 19) & 0x7FF) << 9) |
                       (uint64_t((fetch[4] >> 2) & 0xFF) << 20) |
                       (uint64_t((fetch[4] >> 12) & 0x3FF) << 28) | (uint64_t(fetch[5] & 3) << 38);
  auto it = g_samplers.find(key);
  if (it != g_samplers.end()) return it->second;
  if (g_sampler_next >= g_sampler_end) return 0;  // the default linear-wrap sampler

  static const D3D12_TEXTURE_ADDRESS_MODE kAddress[8] = {
      D3D12_TEXTURE_ADDRESS_MODE_WRAP,        D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP,       D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP,       D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      D3D12_TEXTURE_ADDRESS_MODE_BORDER,      D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE};
  const uint32_t mag = (fetch[3] >> 19) & 3, min = (fetch[3] >> 21) & 3, mip = (fetch[3] >> 23) & 3;
  const uint32_t aniso = (fetch[3] >> 25) & 7;
  auto linear = [](uint32_t f) { return f != 0 ? 1u : 0u; };  // point / linear (+ fetch-const)
  D3D12_SAMPLER_DESC sd = {};
  if (aniso >= 2 && aniso <= 5) {
    sd.Filter = D3D12_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 1u << (aniso - 1);
  } else {
    sd.Filter = D3D12_ENCODE_BASIC_FILTER(linear(min), linear(mag), mip == 1 || mip == 3 ? 1 : 0,
                                          D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    sd.MaxAnisotropy = 1;
  }
  sd.AddressU = kAddress[(fetch[0] >> 10) & 7];
  sd.AddressV = kAddress[(fetch[0] >> 13) & 7];
  sd.AddressW = kAddress[(fetch[0] >> 16) & 7];
  const int32_t bias = int32_t((fetch[4] >> 12) & 0x3FF) << 22 >> 22;
  sd.MipLODBias = std::clamp(bias / 32.0f, -16.0f, 15.99f);
  sd.MinLOD = float((fetch[4] >> 2) & 15);
  sd.MaxLOD = mip == 2 ? sd.MinLOD : float((fetch[4] >> 6) & 15);
  static const bool lod0 = std::getenv("SVR2011_NATIVE_LOD0") != nullptr;  // debug
  if (lod0) sd.MinLOD = sd.MaxLOD = 0.0f;
  sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  if ((fetch[5] & 3) == 1) {
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.0f;
  } else if ((fetch[5] & 3) != 0) {
    sd.BorderColor[3] = 1.0f;
  }
  const uint32_t index = g_sampler_next++;
  D3D12_CPU_DESCRIPTOR_HANDLE h = ctx.sampler_heap->GetCPUDescriptorHandleForHeapStart();
  h.ptr += size_t(index) * ctx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  ctx.device->CreateSampler(&sd, h);
  g_samplers.emplace(key, index);
  return index;
}

void ForgetResolved() { g_resolved.clear(); }

void RegisterResolved(uint32_t base_address, ID3D12Resource* resource, DXGI_FORMAT format,
                      DXGI_FORMAT gamma_format, uint32_t components, bool swap_rb) {
  Resolved& r = g_resolved[base_address >> 12];
  if (r.resource == resource && r.format == format && r.swap_rb == swap_rb) return;
  // Views are per resource generation (and format/swap, in their key), so a
  // target alternating formats reuses its views.
  const uint32_t generation = r.resource == resource ? r.generation : ++g_resolved_generation;
  r = {resource, format, gamma_format, components, swap_rb, generation};
}

Stats TakePerf() {
  Stats s = g_stats;
  g_stats.hash_bytes = g_stats.upload_bytes = 0;
  g_stats.hashes = 0;
  g_stats.hash_ms = g_stats.upload_ms = g_stats.convert_ms = 0;
  return s;
}

Stats FrameStats() {
  Stats s = g_stats;
  s.textures = uint32_t(g_textures.size());
  g_stats.uploads = g_stats.unsupported = 0;
  return s;
}

}  // namespace svr2011::native::textures
