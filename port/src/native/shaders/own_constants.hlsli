// The renderer's own shaders: their constants (b3 space4 on D3D12). On Vulkan
// they are read from the frame's upload buffer (set 2) at the byte offset in
// the push constants, after the three the converted game shaders use
// (shader_common.h) - no buffer addresses or 64-bit integers.
#ifdef __spirv__
struct PushConstants {
  uint VertexShaderConstants;
  uint PixelShaderConstants;
  uint SharedConstants;
  uint OwnConstants;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
[[vk::binding(0, 2)]] ByteAddressBuffer g_ConstantBuffer;
#define OWN_CONSTANT(TYPE, OFFSET) g_ConstantBuffer.Load<TYPE>(g_PushConstants.OwnConstants + (OFFSET))
#define OWN_CONSTANT16(TYPE, OFFSET) g_ConstantBuffer.Load<TYPE>(g_PushConstants.OwnConstants + (OFFSET))
// The texture tables as the converted shaders declare them (set 0: 2D, 3D,
// cube; set 1: samplers).
#ifdef SVR_COMPACT_TABLES
// (GPUs without descriptor indexing: the draw's own small tables - shader_common.h)
#define OWN_TEXTURE_TABLES   [[vk::binding(0, 0)]] Texture2D<float4> g_Texture2DDescriptorHeap[13];   [[vk::binding(0, 1)]] SamplerState g_SamplerDescriptorHeap[16];
#else
#define OWN_TEXTURE_TABLES   [[vk::binding(0, 0)]] Texture2D<float4> g_Texture2DDescriptorHeap[];   [[vk::binding(0, 1)]] SamplerState g_SamplerDescriptorHeap[];
#endif
#else
#define OWN_TEXTURE_TABLES   Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);   SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);
#endif
