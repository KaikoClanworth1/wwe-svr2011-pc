// The renderer's own shaders: their constants (b3 space4 on D3D12). On Vulkan
// they are read through a buffer address in the push constants, after the
// three the converted game shaders use (shader_common.h).
#ifdef __spirv__
struct PushConstants {
  uint64_t VertexShaderConstants;
  uint64_t PixelShaderConstants;
  uint64_t SharedConstants;
  uint64_t OwnConstants;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;
#define OWN_CONSTANT(TYPE, OFFSET) vk::RawBufferLoad<TYPE>(g_PushConstants.OwnConstants + (OFFSET))
#define OWN_CONSTANT16(TYPE, OFFSET) vk::RawBufferLoad<TYPE>(g_PushConstants.OwnConstants + (OFFSET), 16)
#endif
