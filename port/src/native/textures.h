// WWE SmackDown vs. Raw 2011 - native renderer textures and samplers.
//
// Guest textures are described by Xenos texture fetch constants (the device
// register mirror, 6 dwords per slot). Each distinct texture is uploaded once
// into a D3D12 resource: the guest data is detiled, endian-swapped and, for
// the few formats D3D12 lacks, converted on the CPU; the layout of the mips
// and the packed mip tail comes from the SDK's Xenia texture utilities, so it
// matches the emulated renderer. Textures are re-checked against guest memory
// every so often (streamed textures reuse memory).

#pragma once

#include <cstdint>
#include <functional>

#include <d3d12.h>
#include <wrl/client.h>

namespace rex::memory {
class Memory;
}

namespace svr2011::native::textures {

struct Context {
  ID3D12Device* device = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12DescriptorHeap* srv_heap = nullptr;
  ID3D12DescriptorHeap* sampler_heap = nullptr;
  uint64_t frame = 0;
  // Keeps a resource alive until the GPU has finished the current frame.
  std::function<void(Microsoft::WRL::ComPtr<ID3D12Resource>)> retire;
  // Space in the frame's upload ring (cpu pointer, the ring buffer, offset in
  // it), or {nullptr, nullptr, 0} when it's full.
  struct UploadSpace {
    uint8_t* cpu;
    ID3D12Resource* buffer;
    uint64_t offset;
  };
  std::function<UploadSpace(uint64_t size, uint64_t align)> allocate;
};

// Descriptors [srv_first, srv_first + srv_count) of the SRV heap and
// [sampler_first, sampler_first + sampler_count) of the sampler heap are the
// texture cache's.
void Initialize(rex::memory::Memory* memory, uint32_t srv_first, uint32_t srv_count,
                uint32_t sampler_first, uint32_t sampler_count);

// SRV heap index of the texture a fetch constant (6 host-order dwords)
// describes, viewed as `dimension` (0 2D, 1 3D, 2 cube, as the shader samples
// it), uploading it if needed; UINT32_MAX when it can't be provided (the
// caller binds a placeholder).
uint32_t Texture(const Context& ctx, const uint32_t fetch[6], uint32_t dimension);

// Sampler heap index for the fetch constant's filtering and addressing.
uint32_t Sampler(const Context& ctx, const uint32_t fetch[6]);

// A resolve wrote the guest texture at `base_address` (physical): the image
// is in `resource` (a copy of the render target, owned by the renderer), not
// in guest memory, so fetches of that address sample the resource.
// `format` / `gamma_format` are the view formats (the latter for fetches that
// ask for gamma), `components` the channels the format has; `swap_rb`: the
// resolve stored red and blue swapped (RB_COPY_DEST_INFO copy_dest_swap),
// which the fetch swizzle of such textures undoes.
// Drops every render target copy (the renderer rebuilt its targets).
void ForgetResolved();

void RegisterResolved(uint32_t base_address, ID3D12Resource* resource, DXGI_FORMAT format,
                      DXGI_FORMAT gamma_format, uint32_t components, bool swap_rb);

struct Stats {
  uint32_t textures = 0;     // resident
  uint32_t uploads = 0;      // this frame
  uint32_t unsupported = 0;  // lookups that got the placeholder, this frame
  // Accumulated until TakePerf (performance log).
  uint64_t hash_bytes = 0, upload_bytes = 0;
  uint32_t hashes = 0;
  double hash_ms = 0, upload_ms = 0, convert_ms = 0;  // convert: of upload_ms
};
// The accumulated performance counters (and resets them).
Stats TakePerf();
Stats FrameStats();  // and resets the per-frame counters

}  // namespace svr2011::native::textures
