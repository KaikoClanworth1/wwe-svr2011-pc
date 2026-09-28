// WWE SmackDown vs. Raw 2011 - the game's Direct3D (Xbox 360 XDK) objects,
// as the native renderer reads them from guest memory.
//
// Everything here was established from this game's own data (census build,
// see docs/native_renderer_phase2.md): object layouts from ~1,000 captured
// objects, the device register mirror from (register, device offset) pairs
// the D3D library itself uses to build its command buffer.
// All values are big-endian in guest memory.

#pragma once

#include <cstdint>

namespace svr2011::guest {

// Guest address of the game's D3D device (constant for the whole session).
constexpr uint32_t kDeviceAddress = 0xAD977E80;

// ---- resources -------------------------------------------------------------

// First word of every D3D object: low 4 bits = resource type.
enum class ResourceType : uint32_t {
  kVertexBuffer = 1,
  kIndexBuffer = 2,
  kTexture = 3,
  kSurface = 4,            // render target / depth-stencil (EDRAM)
  kVertexDeclaration = 5,
  kVertexShader = 6,
  kPixelShader = 7,
};

// 24-byte header shared by all objects.
struct ResourceHeader {
  uint32_t common;          // & 0xF = ResourceType
  uint32_t reference_count;
  uint32_t fence;
  uint32_t read_fence;
  uint32_t identifier;
  uint32_t base_flush;
};

struct Texture {            // type 3
  ResourceHeader header;
  uint32_t mip_flush;
  uint32_t fetch[6];        // Xenos texture fetch constant (same as fetch slots)
};

struct VertexBuffer {       // type 1
  ResourceHeader header;
  uint32_t fetch[2];        // Xenos vertex fetch constant: address|3, size_dwords<<2|endian
};

struct IndexBuffer {        // type 2; Common = 0x20100002 (16-bit indices in this game)
  ResourceHeader header;
  uint32_t address;         // GPU physical address
  uint32_t size;            // bytes
};

struct VertexElement {      // 12 bytes, D3DVERTEXELEMENT9
  uint16_t stream;
  uint16_t offset;
  uint32_t type;            // & 0x3F = Xenos vertex format (e.g. 57 = 32_32_32_FLOAT)
  uint8_t method;
  uint8_t usage;            // D3DDECLUSAGE (0 POSITION, 5 TEXCOORD, 10 COLOR, ...)
  uint8_t usage_index;
  uint8_t pad;
};

struct VertexDeclaration {  // type 5
  ResourceHeader header;
  uint32_t element_count;   // +0x18
  uint8_t unknown[0x34 - 0x1C];
  VertexElement elements[1];  // +0x34, element_count entries
};

// Shader objects (types 6/7): the pixel shader object embeds a copy of its
// container header at +0x28; the microcode lives in physical memory
// (+0x18 for pixel, +0x20 for vertex shaders in the objects seen).

// ---- device ----------------------------------------------------------------

// The device keeps a mirror of the GPU registers it programs, in groups.
// Register `reg` of a group lives at device + base + (reg - first) * 4.
struct RegisterGroup {
  uint32_t first_register;
  uint32_t device_offset;
  uint32_t count;
};

constexpr RegisterGroup kRegisterGroups[] = {
    {0x4800, 0x0480, 0x00C0},  // fetch constants: 32 slots x 6 dwords (textures;
                               // vertex fetch vfN = slot N/3, pair N%3)
    {0x4000, 0x0780, 0x0800},  // ALU constants: vertex c0-c255 (0x4000), pixel (0x4400)
    {0x4900, 0x2780, 0x0008},  // boolean constants
    {0x2000, 0x2880, 0x0013},  // RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, ...
    {0x2100, 0x28CC, 0x0015},  // VGT index limits, RB_COLOR_MASK, blend colour,
                               // stencil ref/mask, alpha ref, viewport scale/offset
    {0x2180, 0x2920, 0x0005},  // SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC, interpolators
    {0x2200, 0x2934, 0x000C},  // RB_DEPTHCONTROL, RB_BLENDCONTROL0-3,
                               // RB_COLORCONTROL, PA_SU_SC_MODE_CNTL (cull), ...
    {0x2280, 0x2964, 0x0015},  // point/line size, tessellation, ...
    {0x2300, 0x29B8, 0x0026},  // AA config, SQ_VS/PS_CONST, RB_COPY_* (resolve)
    {0x2380, 0x2A50, 0x0004},  // PA_SU_POLY_OFFSET_FRONT/BACK_SCALE/OFFSET (depth bias)
};

// Other device fields.
constexpr uint32_t kDeviceCurrentPixelShader = 0x3244;   // guest pointer (type 7)
constexpr uint32_t kDeviceCurrentVertexShader = 0x3248;  // guest pointer (type 6)
constexpr uint32_t kDeviceVertexDeclaration = 0x2ED8;    // set by SetVertexDeclaration
constexpr uint32_t kDeviceIndexBuffer = 0x3144;          // set by SetIndices

// Device offset of a mirrored register, or 0 if it is not mirrored.
constexpr uint32_t RegisterOffset(uint32_t reg) {
  for (const auto& g : kRegisterGroups) {
    if (reg >= g.first_register && reg < g.first_register + g.count) {
      return g.device_offset + (reg - g.first_register) * 4;
    }
  }
  return 0;
}

static_assert(RegisterOffset(0x2200) == 0x2934);  // RB_DEPTHCONTROL
static_assert(RegisterOffset(0x2205) == 0x2948);  // PA_SU_SC_MODE_CNTL (the cull setter)
static_assert(RegisterOffset(0x210F) == 0x2908);  // PA_CL_VPORT_XSCALE
static_assert(RegisterOffset(0x2380) == 0x2A50);  // PA_SU_POLY_OFFSET_FRONT_SCALE (frame log)

// ---- D3D functions (see docs/d3d_map.csv) ----------------------------------

namespace fn {
constexpr uint32_t kPresent = 0x8291AED0;
constexpr uint32_t kCreateVertexShader = 0x82921548;
constexpr uint32_t kCreatePixelShader = 0x82921360;
constexpr uint32_t kSetVertexShader = 0x82920D60;
constexpr uint32_t kSetPixelShader = 0x82920B58;
constexpr uint32_t kDrawIndexedVertices = 0x82921B58;
constexpr uint32_t kDrawVerticesUP = 0x82921698;
constexpr uint32_t kSetTexture = 0x82917EC8;
constexpr uint32_t kSetStreamSource = 0x8291DD70;
constexpr uint32_t kSetIndices = 0x8291DE90;
constexpr uint32_t kSetVertexDeclaration = 0x82920F78;
constexpr uint32_t kSetRenderTarget = 0x8291E618;
constexpr uint32_t kClear = 0x8291F168;
constexpr uint32_t kResolve = 0x82918A88;
}  // namespace fn

}  // namespace svr2011::guest
