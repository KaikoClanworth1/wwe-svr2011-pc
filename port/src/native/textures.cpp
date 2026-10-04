// WWE SmackDown vs. Raw 2011 - native renderer textures (see textures.h).

#include "native/textures.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>  // xxHash (inline)
#include <rex/logging.h>
#include <fmt/format.h>
#include <rex/system/xmemory.h>

#include "pad_icons.h"

namespace svr2011::native::textures {

namespace {

namespace xenos = rex::graphics::xenos;
using plume::RenderFormat;
namespace texture_util = rex::graphics::texture_util;
using rex::graphics::FormatInfo;
using TF = xenos::TextureFormat;

constexpr uint32_t kPhysicalSize = 0x20000000;
constexpr uint64_t kRecheckFrames = 120;  // re-hash guest data this often
// ... but more often while a texture is new: a video's texture is created
// with its first frame and must be seen changing right away (with 120 its
// start stayed frozen for up to 2 s).
constexpr uint64_t kNewFrames = 240, kNewRecheckFrames = 4;
// A texture drawn again after a few frames unused is checked at once: the
// game loads new content into the same memory while it isn't shown (on
// character select the next superstar appeared for a moment with the last
// one's textures - Jimmy Snuka in John Cena's jeans).
constexpr uint64_t kUnusedRecheckFrames = 2;
// Textures seen changing are "dynamic": re-hashed at every frame they're
// used, and each check that finds no change doubles the wait before the next
// (up to kRecheckFrames). Video frames keep being checked at every frame; a
// texture the game rewrote once (streamed into memory it had used for
// another) settles back - before, it stayed checked at every frame for good,
// and over a long session (Universe) that grew to 8-13 MB hashed per frame.

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
  RenderFormat format = RenderFormat::UNKNOWN;
  RenderFormat gamma = RenderFormat::UNKNOWN;  // format for gamma (sign 3) textures
  Convert convert = Convert::kNone;
  uint32_t components = 4;  // host channels: missing ones replicate the last
  bool block_compressed = false;
};

HostFormat GetHostFormat(TF f) {
  switch (f) {
    case TF::k_8:
    case TF::k_8_A:
    case TF::k_8_B:
      return {RenderFormat::R8_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 1};
    case TF::k_8_8:
      return {RenderFormat::R8G8_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 2};
    case TF::k_8_8_8_8:
    case TF::k_8_8_8_8_A:
    case TF::k_8_8_8_8_AS_16_16_16_16:
      return {RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_UNORM_SRGB, Convert::kCopy, 4};
    case TF::k_2_10_10_10:
    case TF::k_2_10_10_10_AS_16_16_16_16:
      return {RenderFormat::R10G10B10A2_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 4};
    case TF::k_5_6_5:
      return {RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_UNORM_SRGB, Convert::k565, 4};
    case TF::k_6_5_5:
      return {RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_UNORM_SRGB, Convert::k655, 4};
    case TF::k_1_5_5_5:
      return {RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_UNORM_SRGB, Convert::k1555, 4};
    case TF::k_4_4_4_4:
      return {RenderFormat::R8G8B8A8_UNORM, RenderFormat::R8G8B8A8_UNORM_SRGB, Convert::k4444, 4};
    case TF::k_DXT1:
    case TF::k_DXT1_AS_16_16_16_16:
      return {RenderFormat::BC1_UNORM, RenderFormat::BC1_UNORM_SRGB, Convert::kCopy, 4, true};
    case TF::k_DXT2_3:
    case TF::k_DXT2_3_AS_16_16_16_16:
      return {RenderFormat::BC2_UNORM, RenderFormat::BC2_UNORM_SRGB, Convert::kCopy, 4, true};
    case TF::k_DXT4_5:
    case TF::k_DXT4_5_AS_16_16_16_16:
      return {RenderFormat::BC3_UNORM, RenderFormat::BC3_UNORM_SRGB, Convert::kCopy, 4, true};
    case TF::k_DXN:
      return {RenderFormat::BC5_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 2, true};
    case TF::k_DXT5A:
      return {RenderFormat::BC4_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 1, true};
    case TF::k_DXT3A:
      return {RenderFormat::R8_UNORM, RenderFormat::UNKNOWN, Convert::kDXT3A, 1};
    case TF::k_CTX1:
      return {RenderFormat::R8G8_UNORM, RenderFormat::UNKNOWN, Convert::kCTX1, 2};
    case TF::k_16:
      return {RenderFormat::R16_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 1};
    case TF::k_16_16:
      return {RenderFormat::R16G16_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 2};
    case TF::k_16_16_16_16:
      return {RenderFormat::R16G16B16A16_UNORM, RenderFormat::UNKNOWN, Convert::kCopy, 4};
    case TF::k_16_EXPAND:
    case TF::k_16_FLOAT:
      return {RenderFormat::R16_FLOAT, RenderFormat::UNKNOWN, Convert::kCopy, 1};
    case TF::k_16_16_EXPAND:
    case TF::k_16_16_FLOAT:
      return {RenderFormat::R16G16_FLOAT, RenderFormat::UNKNOWN, Convert::kCopy, 2};
    case TF::k_16_16_16_16_EXPAND:
    case TF::k_16_16_16_16_FLOAT:
      return {RenderFormat::R16G16B16A16_FLOAT, RenderFormat::UNKNOWN, Convert::kCopy, 4};
    case TF::k_32_FLOAT:
      return {RenderFormat::R32_FLOAT, RenderFormat::UNKNOWN, Convert::kCopy, 1};
    case TF::k_32_32_FLOAT:
      return {RenderFormat::R32G32_FLOAT, RenderFormat::UNKNOWN, Convert::kCopy, 2};
    case TF::k_32_32_32_32_FLOAT:
      return {RenderFormat::R32G32B32A32_FLOAT, RenderFormat::UNKNOWN, Convert::kCopy, 4};
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
  std::shared_ptr<plume::RenderTexture> resource;
  std::shared_ptr<plume::RenderTextureView> view;
  uint32_t srv = UINT32_MAX;
  bool failed = false;
  bool dynamic = false;  // its guest data has changed after upload
  uint32_t interval = 1;  // (dynamic) frames until its next check
  uint64_t created_frame = 0;
  uint64_t hash = 0;
  uint64_t checked_frame = 0;
  uint64_t used_frame = 0;  // the last frame it was drawn with
  uint32_t base_address = 0, base_size = 0, mip_address = 0, mip_size = 0;
};

rex::memory::Memory* g_memory = nullptr;
const uint8_t* g_physical = nullptr;
uint32_t g_srv_next = 0, g_srv_end = 0;
uint32_t g_sampler_next = 0, g_sampler_end = 0;
std::unordered_map<uint64_t, Entry> g_textures;   // key hash -> texture
struct SamplerEntry {
  uint32_t index;
  std::unique_ptr<plume::RenderSampler> sampler;
};
std::unordered_map<uint64_t, SamplerEntry> g_samplers;  // key -> table index
// Resolve destinations (base page -> the renderer's copy of the target).
struct Resolved {
  plume::RenderTexture* resource = nullptr;
  RenderFormat format = RenderFormat::UNKNOWN, gamma_format = RenderFormat::UNKNOWN;
  uint32_t components = 4;
  bool swap_rb = false;
  uint32_t generation = 0;  // changes with the resource (views are per generation)
};
std::unordered_map<uint32_t, Resolved> g_resolved;
uint32_t g_resolved_generation = 0;
struct ResolvedViewEntry {
  uint32_t index;
  plume::RenderTexture* texture;
  std::shared_ptr<plume::RenderTextureView> view;
};
std::unordered_map<uint64_t, ResolvedViewEntry> g_resolved_views;  // (generation, swizzle, gamma) ->
Stats g_stats;
const bool g_no_cache = std::getenv("SVR2011_NATIVE_NOCACHE") != nullptr;  // (debug)

bool IsBlockCompressed(uint32_t format) {
  switch (format) {
    case 18: case 19: case 20:  // k_DXT1, k_DXT2_3, k_DXT4_5
    case 49:                    // k_DXN
    case 51: case 52: case 53:  // k_DXT*_AS_16_16_16_16
    case 58: case 59: case 60: case 61:  // k_DXT3A, k_DXT5A, k_CTX1, k_DXT3A_AS_1_1_1_1
      return true;
    default:
      return false;
  }
}

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

// Character select: the "?" tile opens the managers (src/managers.cpp), so its
// pictures (DXT5 textures packed in the menu files: the 64 x 64 grid tile and
// the 128 x 64 banner beside the name) become an "M" (art/managers_*.png,
// tools/make_managers_icon.py). Recognized by their data (FNV-1a of the
// converted base level).
#include "native/managers_icon.inc"

struct TileSwap {
  uint32_t width;
  uint64_t hash;
  const uint8_t* data;
};
const TileSwap kTileSwaps[] = {
    {64, 0xE4E1A919C5A0D7F9ull, kManagersTile},
    {128, 0x823D7E7ED0BC1410ull, kManagersBanner},
};
static_assert(sizeof(kManagersTile) == 64 * 64 && sizeof(kManagersBanner) == 128 * 64);

void ReplaceRandomTile(uint32_t width, uint8_t* data, uint32_t row_pitch) {
  const uint32_t row_bytes = width * 4, rows = 16;  // (4 x 4 blocks, 16 bytes each; 64 high)
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint32_t y = 0; y < rows; ++y)
    for (uint32_t x = 0; x < row_bytes; ++x) h = (h ^ data[size_t(y) * row_pitch + x]) * 0x100000001B3ull;
  for (const TileSwap& t : kTileSwaps) {
    if (t.width != width || t.hash != h) continue;
    if (!rex::cvar::Query<bool>("managers_tile")) return;
    for (uint32_t y = 0; y < rows; ++y) std::memcpy(data + size_t(y) * row_pitch, t.data + y * row_bytes, row_bytes);
    return;
  }
}

plume::RenderSwizzle Swizzle(uint32_t c) {
  switch (c) {
    case 0: return plume::RenderSwizzle::R;
    case 1: return plume::RenderSwizzle::G;
    case 2: return plume::RenderSwizzle::B;
    case 3: return plume::RenderSwizzle::A;
    case 5: return plume::RenderSwizzle::ONE;
    default: return plume::RenderSwizzle::ZERO;
  }
}

uint32_t ResolvedView(const Context& ctx, const Resolved& res,
                      const xenos::xe_gpu_texture_fetch_t& fetch) {
  const bool gamma = fetch.sign_x == xenos::TextureSign::kGamma &&
                     res.gamma_format != RenderFormat::UNKNOWN;
  const uint64_t key = uint64_t(res.generation) | (uint64_t(fetch.swizzle) << 32) |
                       (uint64_t(gamma) << 44) | (uint64_t(res.swap_rb) << 45) |
                       (uint64_t(res.format) << 46);
  auto it = g_resolved_views.find(key);
  if (it != g_resolved_views.end()) return it->second.index;
  if (g_srv_next >= g_srv_end) return UINT32_MAX;
  const uint32_t srv = g_srv_next++;
  plume::RenderTextureViewDesc vd = plume::RenderTextureViewDesc::Texture2D(gamma ? res.gamma_format : res.format);
  vd.mipLevels = 1;
  vd.componentMapping = ComponentMapping(fetch.swizzle, res.components, res.swap_rb);
  std::shared_ptr<plume::RenderTextureView> view = res.resource->createTextureView(vd);
  ctx.texture_sets[0]->setTexture(srv, res.resource, plume::RenderTextureLayout::SHADER_READ, view.get());
  g_resolved_views.emplace(key, ResolvedViewEntry{srv, res.resource, std::move(view)});
  return srv;
}

// One subresource's place in the staging buffer (rows 256-byte aligned,
// subresources 512-byte aligned: valid copy sources for D3D12 and Vulkan).
struct Footprint {
  uint64_t offset;
  uint32_t width, height, depth;  // in texels (block-aligned for BC formats)
  uint32_t row_pitch, rows;       // bytes per row of blocks, rows of blocks
};

// PlayStation button pictures (pad_icons.h): the fonts' icon pages are shown
// as a taller picture, one resource for all the copies of a page (each font
// has its own). Recognized by the converted base level's FNV-1a 64.
struct PadPicture {
  std::shared_ptr<plume::RenderTexture> resource;
};
std::map<std::pair<const void*, bool>, PadPicture> g_pad_pictures;  // (picture, gamma)

bool IsPadPicture(const std::shared_ptr<plume::RenderTexture>& r) {
  for (const auto& [key, p] : g_pad_pictures)
    if (p.resource == r) return true;
  return false;
}

// The resource for a picture (created and uploaded the first time).
std::shared_ptr<plume::RenderTexture> PadPictureResource(const Context& ctx, const svr2011::PadIconsPicture& pic,
                                                         RenderFormat format) {
  PadPicture& p = g_pad_pictures[{pic.rgba, format == RenderFormat::R8G8B8A8_UNORM_SRGB}];
  if (p.resource) return p.resource;
  std::shared_ptr<plume::RenderTexture> resource = ctx.device->createTexture(plume::RenderTextureDesc::Texture(
      plume::RenderTextureDimension::TEXTURE_2D, pic.width, pic.height, 1, 1, 1, format, plume::RenderTextureFlag::NONE));
  if (!resource) return nullptr;
  resource->setName(fmt::format("pad icons {}x{}", pic.width, pic.height));
  const uint32_t row_pitch = (pic.width * 4 + 255) & ~255u;
  std::shared_ptr<plume::RenderBuffer> staging =
      ctx.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(uint64_t(row_pitch) * pic.height));
  if (!staging) return nullptr;
  auto* mapped = static_cast<uint8_t*>(staging->map());
  for (uint32_t y = 0; y < pic.height; ++y)
    std::memcpy(mapped + size_t(y) * row_pitch, pic.rgba->data() + size_t(y) * pic.width * 4, pic.width * 4);
  staging->unmap();
  ctx.list->barriers(plume::RenderBarrierStage::COPY,
                     plume::RenderTextureBarrier(resource.get(), plume::RenderTextureLayout::COPY_DEST));
  ctx.list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(resource.get(), 0, 0),
                              plume::RenderTextureCopyLocation::PlacedFootprint(staging.get(), format, pic.width,
                                                                                pic.height, 1, row_pitch / 4, 0));
  ctx.list->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE,
                     plume::RenderTextureBarrier(resource.get(), plume::RenderTextureLayout::SHADER_READ));
  ctx.retire(staging);
  p.resource = resource;
  REXLOG_INFO("[svr2011] pad icons: picture {}x{} uploaded", pic.width, pic.height);
  return p.resource;
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
  RenderFormat resource_format = gamma && host.gamma != RenderFormat::UNKNOWN ? host.gamma : host.format;
  // (an icon page with PlayStation pictures - pad_icons.h - recognized below)
  const bool pad_candidate = format == TF::k_DXT4_5 && !is_3d && !is_cube && array_size == 1 && levels == 1 &&
                             svr2011::PadIconsCandidate(width, height);

  // A re-upload (guest data changed) reuses the texture and its view: the
  // entry's key fixes the layout, and frames still in flight keep a valid
  // descriptor (queue order serializes the copy after their reads).
  // (a texture shown as a pad icons picture before - its data has changed -
  // gets a resource of its own)
  const bool reuse = e.resource != nullptr && !IsPadPicture(e.resource);
  std::shared_ptr<plume::RenderTexture> resource = reuse ? e.resource : nullptr;
  auto create_resource = [&]() -> bool {
    if (reuse) return true;
    plume::RenderTextureDesc desc =
        is_3d ? plume::RenderTextureDesc::Texture3D(host_width, host_height, depth, levels, resource_format)
              : plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, host_width,
                                                  host_height, 1, levels, array_size, resource_format,
                                                  is_cube ? plume::RenderTextureFlag::CUBE
                                                          : plume::RenderTextureFlag::NONE);
    resource = ctx.device->createTexture(desc);
    if (!resource) return false;
    resource->setName(fmt::format("texture {:08X} {}x{}x{} fmt {}", base_page << 12, width, height,
                                  is_3d ? depth : array_size, uint32_t(format)));
    return true;
  };

  const uint32_t subresources = levels * array_size;
  const uint32_t host_bw = plume::RenderFormatBlockWidth(resource_format);
  const uint32_t host_bpb = plume::RenderFormatSize(resource_format);
  std::vector<Footprint> footprints(subresources);
  uint64_t total = 0;
  for (uint32_t slice = 0; slice < array_size; ++slice) {
    for (uint32_t level = 0; level < levels; ++level) {
      Footprint& fp = footprints[level + slice * levels];
      const uint32_t w = std::max(host_width >> level, 1u), h = std::max(host_height >> level, 1u);
      fp.width = (w + host_bw - 1) / host_bw * host_bw;
      fp.height = (h + host_bw - 1) / host_bw * host_bw;
      fp.depth = std::max(depth >> level, 1u);
      fp.row_pitch = ((fp.width / host_bw) * host_bpb + 255) & ~255u;
      fp.rows = fp.height / host_bw;
      fp.offset = (total + 511) & ~511ull;
      total = fp.offset + uint64_t(fp.row_pitch) * fp.rows * fp.depth;
    }
  }
  // Staging: the frame's upload ring (textures that change every frame -
  // videos - would otherwise create and free a buffer per frame), or a buffer
  // of its own when the ring has no room.
  std::shared_ptr<plume::RenderBuffer> staging;
  plume::RenderBuffer* staging_buffer = nullptr;
  uint64_t staging_offset = 0;
  uint8_t* mapped = nullptr;
  if (ctx.allocate) {
    const Context::UploadSpace space = ctx.allocate(total, 512);
    mapped = space.cpu;
    staging_buffer = space.buffer;
    staging_offset = space.offset;
  }
  if (!mapped) {
    staging = ctx.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(total));
    if (!staging) return false;
    mapped = static_cast<uint8_t*>(staging->map());
    staging_buffer = staging.get();
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
      const Footprint& fp = footprints[sub];
      const uint32_t slice_address = level_address + slice * gl.array_slice_stride_bytes;
      for (uint32_t z = 0; z < ld; ++z) {
        uint8_t* dst = mapped + fp.offset + size_t(z) * fp.rows * fp.row_pitch;
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
            std::memcpy(dst + size_t(by) * fp.row_pitch, row.data(), row_bytes_n);
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
            StoreBlock(host.convert, block, bpb, dst, fp.row_pitch, lw, lh, bx, by);
          }
        }
      }
    }
  }
  g_stats.convert_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - convert_t0).count();
  if (format == TF::k_DXT4_5 && (width == 64 || width == 128) && height == 64 && array_size == 1)
    ReplaceRandomTile(width, mapped + footprints[0].offset, footprints[0].row_pitch);
  // An icon page: the shared taller picture instead (pad_icons.h).
  if (pad_candidate) {
    uint64_t h = 0xCBF29CE484222325ull;
    const uint32_t row_bytes = (width / 4) * 16;
    for (uint32_t y = 0; y < footprints[0].rows; ++y) {
      const uint8_t* row = mapped + footprints[0].offset + size_t(y) * footprints[0].row_pitch;
      for (uint32_t x = 0; x < row_bytes; ++x) h = (h ^ row[x]) * 0x100000001B3ull;
    }
    const svr2011::PadIconsPicture pic = svr2011::PadIconsAtlas(width, height, h);
    if (pic.rgba) {
      const RenderFormat rgba = gamma ? RenderFormat::R8G8B8A8_UNORM_SRGB : RenderFormat::R8G8B8A8_UNORM;
      if (auto shared = PadPictureResource(ctx, pic, rgba)) {
        if (staging) {
          staging->unmap();
          ctx.retire(staging);
        }
        resource = shared;
        resource_format = rgba;
        goto view;
      }
    }
  }
  if (!create_resource()) {
    if (staging) staging->unmap();
    return false;
  }
  ctx.list->barriers(plume::RenderBarrierStage::COPY,
                     plume::RenderTextureBarrier(resource.get(), plume::RenderTextureLayout::COPY_DEST));
  for (uint32_t level = 0; level < levels; ++level) {
    DumpLevel0(format, footprints[level].width, footprints[level].height, mapped + footprints[level].offset,
               footprints[level].row_pitch, footprints[level].rows,
               size_t(footprints[level].width / host_bw) * host_bpb);
  }
  if (staging) staging->unmap();

  for (uint32_t sub = 0; sub < subresources; ++sub) {
    const Footprint& fp = footprints[sub];
    const auto src = plume::RenderTextureCopyLocation::PlacedFootprint(
        staging_buffer, resource_format, fp.width, fp.height, fp.depth, fp.row_pitch / host_bpb * host_bw,
        staging_offset + fp.offset);
    const auto dst = plume::RenderTextureCopyLocation::Subresource(resource.get(), sub % levels, sub / levels);
    ctx.list->copyTextureRegion(dst, src);
  }
  ctx.list->barriers(plume::RenderBarrierStage::GRAPHICS_AND_COMPUTE,
                     plume::RenderTextureBarrier(resource.get(), plume::RenderTextureLayout::SHADER_READ));
  if (staging) ctx.retire(staging);

  // The view, with the fetch constant's swizzle (Xenos and D3D12 encode
  // component selection the same way: 0-3 xyzw, 4 zero, 5 one).
view:
  if (reuse && resource == e.resource) {
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
  plume::RenderTextureViewDesc vd;
  vd.format = resource_format;
  vd.dimension = is_3d ? plume::RenderTextureViewDimension::TEXTURE_3D
                 : is_cube ? plume::RenderTextureViewDimension::TEXTURE_CUBE
                           : plume::RenderTextureViewDimension::TEXTURE_2D;
  vd.mipLevels = levels;
  vd.componentMapping = ComponentMapping(fetch.swizzle, host.components);
  e.view = resource->createTextureView(vd);
  ctx.texture_sets[dimension]->setTexture(e.srv, resource.get(), plume::RenderTextureLayout::SHADER_READ,
                                          e.view.get());

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
  // A block-compressed texture where a render target was copied: the game
  // reused that memory (the GPU can't resolve to DXT), so the copy is stale -
  // after Superstar Threads, Tyson Kidd's roster picture showed its editor
  // preview (a white square).
  if (IsBlockCompressed(uint32_t(fetch.format))) g_resolved.erase(fetch.base_address);
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
  } else if (g_no_cache || ctx.frame > e.used_frame + kUnusedRecheckFrames ||
             ctx.frame >= e.checked_frame +
                              (e.dynamic ? e.interval
                               : ctx.frame < e.created_frame + kNewFrames ? kNewRecheckFrames
                                                                          : kRecheckFrames)) {
    e.checked_frame = ctx.frame;
    if (GuestHash(e) != e.hash) {
      static const bool log_changes = std::getenv("SVR2011_LOG_TEXTURE_CHANGES") != nullptr;  // (debug)
      if (log_changes) {
        REXLOG_INFO("native renderer: texture {:08X} changed (frame {})", e.base_address, ctx.frame);
      }
      if (!e.dynamic) {
        static uint32_t logged = 0;
        if (logged++ < 40) {
          REXLOG_INFO("native renderer: dynamic texture {:08X} format {} {}x{} tiled {} ({} KB)",
                      e.base_address, uint32_t(fetch.format), fetch.size_2d.width + 1,
                      fetch.size_2d.height + 1, uint32_t(fetch.tiled), e.base_size >> 10);
        }
      }
      e.dynamic = true;
      e.interval = 1;
      Upload(ctx, fetch, dimension, e);
    } else if (e.dynamic) {
      e.interval = uint32_t(std::min<uint64_t>(uint64_t(e.interval) * 2, kRecheckFrames));
    }
  }
  e.used_frame = ctx.frame;
  return e.srv;
}

uint32_t Sampler(const Context& ctx, const uint32_t fetch[6]) {
  const uint64_t key = uint64_t((fetch[0] >> 10) & 0x1FF) | (uint64_t((fetch[3] >> 19) & 0x7FF) << 9) |
                       (uint64_t((fetch[4] >> 2) & 0xFF) << 20) |
                       (uint64_t((fetch[4] >> 12) & 0x3FF) << 28) | (uint64_t(fetch[5] & 3) << 38);
  auto it = g_samplers.find(key);
  if (it != g_samplers.end()) return it->second.index;
  if (g_sampler_next >= g_sampler_end) return 0;  // the default linear-wrap sampler

  using plume::RenderTextureAddressMode;
  static const RenderTextureAddressMode kAddress[8] = {
      RenderTextureAddressMode::WRAP,   RenderTextureAddressMode::MIRROR,
      RenderTextureAddressMode::CLAMP,  RenderTextureAddressMode::MIRROR_ONCE,
      RenderTextureAddressMode::CLAMP,  RenderTextureAddressMode::MIRROR_ONCE,
      RenderTextureAddressMode::BORDER, RenderTextureAddressMode::MIRROR_ONCE};
  const uint32_t mag = (fetch[3] >> 19) & 3, min = (fetch[3] >> 21) & 3, mip = (fetch[3] >> 23) & 3;
  const uint32_t aniso = (fetch[3] >> 25) & 7;
  auto filter = [](uint32_t f) {  // point / linear (+ fetch-const)
    return f != 0 ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
  };
  plume::RenderSamplerDesc sd;
  if (aniso >= 2 && aniso <= 5) {
    sd.minFilter = sd.magFilter = plume::RenderFilter::LINEAR;
    sd.mipmapMode = plume::RenderMipmapMode::LINEAR;
    sd.anisotropyEnabled = true;
    sd.maxAnisotropy = 1u << (aniso - 1);
  } else {
    sd.minFilter = filter(min);
    sd.magFilter = filter(mag);
    sd.mipmapMode = mip == 1 || mip == 3 ? plume::RenderMipmapMode::LINEAR : plume::RenderMipmapMode::NEAREST;
    sd.anisotropyEnabled = false;
    sd.maxAnisotropy = 1;
  }
  sd.addressU = kAddress[(fetch[0] >> 10) & 7];
  sd.addressV = kAddress[(fetch[0] >> 13) & 7];
  sd.addressW = kAddress[(fetch[0] >> 16) & 7];
  const int32_t bias = int32_t((fetch[4] >> 12) & 0x3FF) << 22 >> 22;
  sd.mipLODBias = std::clamp(bias / 32.0f, -16.0f, 15.99f);
  sd.minLOD = float((fetch[4] >> 2) & 15);
  sd.maxLOD = mip == 2 ? sd.minLOD : float((fetch[4] >> 6) & 15);
  static const bool lod0 = std::getenv("SVR2011_NATIVE_LOD0") != nullptr;  // debug
  if (lod0) sd.minLOD = sd.maxLOD = 0.0f;
  sd.comparisonEnabled = false;
  sd.borderColor = (fetch[5] & 3) == 0   ? plume::RenderBorderColor::TRANSPARENT_BLACK
                   : (fetch[5] & 3) == 1 ? plume::RenderBorderColor::OPAQUE_WHITE
                                         : plume::RenderBorderColor::OPAQUE_BLACK;
  std::unique_ptr<plume::RenderSampler> sampler = ctx.device->createSampler(sd);
  if (!sampler) return 0;
  const uint32_t index = g_sampler_next++;
  ctx.sampler_set->setSampler(index, sampler.get());
  g_samplers.emplace(key, SamplerEntry{index, std::move(sampler)});
  return index;
}

void ForgetResolved() { g_resolved.clear(); }

void ForgetResolved(uint32_t base_address) { g_resolved.erase(base_address >> 12); }

void RegisterResolved(uint32_t base_address, plume::RenderTexture* resource, RenderFormat format,
                      RenderFormat gamma_format, uint32_t components, bool swap_rb) {
  Resolved& r = g_resolved[base_address >> 12];
  if (r.resource == resource && r.format == format && r.swap_rb == swap_rb) return;
  // Views are per resource generation (and format/swap, in their key), so a
  // target alternating formats reuses its views.
  const uint32_t generation = r.resource == resource ? r.generation : ++g_resolved_generation;
  r = {resource, format, gamma_format, components, swap_rb, generation};
}

void ReleaseResolved(const Context& ctx, plume::RenderTexture* texture) {
  for (auto it = g_resolved_views.begin(); it != g_resolved_views.end();) {
    if (it->second.texture == texture) {
      ctx.retire(std::move(it->second.view));
      it = g_resolved_views.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = g_resolved.begin(); it != g_resolved.end();)
    it = it->second.resource == texture ? g_resolved.erase(it) : std::next(it);
}

plume::RenderComponentMapping ComponentMapping(uint32_t swizzle, uint32_t components, bool swap_rb) {
  plume::RenderSwizzle out[4];
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t c = (swizzle >> (3 * i)) & 7;
    if (swap_rb && (c == 0 || c == 2)) c ^= 2;
    if (c <= 3) c = std::min(c, components - 1);
    else if (c > 5) c = 4;
    static const bool alpha1 = std::getenv("SVR2011_NATIVE_ALPHA1") != nullptr;  // debug
    if (alpha1 && i == 3) c = 5;
    out[i] = Swizzle(c);
  }
  return plume::RenderComponentMapping(out[0], out[1], out[2], out[3]);
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
