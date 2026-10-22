// Native renderer upscaling (native_upscale fsr1), pass 2: FSR 1 RCAS - the
// EASU result sharpened into the frame image: a 5-texel cross, its sharpening
// limited so the result stays within the neighbours' range (no halos).
// After AMD FidelityFX Super Resolution 1 (RCAS), Copyright (c) 2021 Advanced
// Micro Devices, Inc. - MIT licence (the release's third-party notices).
#include "own_constants.hlsli"
OWN_TEXTURE_TABLES
#ifdef __spirv__
#define g_Texture OWN_CONSTANT(uint, 0)
#define g_UvScale OWN_CONSTANT(float2, 8)
#elif defined(SVR_D3D11)
cbuffer PresentConstants : register(b3) {
  uint2 g_Unused;
  float2 g_UvScale;
  float2 g_OutputSize;
};
#define g_Texture 0
#else
cbuffer PresentConstants : register(b3, space4) {
  uint g_Texture;  // SRV heap index (the intermediate)
  uint g_Sampler;  // (unused)
  float2 g_UvScale;  // x: the sharpening (exp2(-stops); 1 the most), y: 1 = red / blue swapped
  float2 g_OutputSize;
};
#endif

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
  Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_Texture];
  float2 size;
  tex.GetDimensions(size.x, size.y);
  const int2 hi = int2(size) - 1;
  const int2 sp = int2(pos.xy);
#define T(X, Y) tex.Load(int3(clamp(sp + int2(X, Y), int2(0, 0), hi), 0)).rgb
  const float3 b = T(0, -1), d = T(-1, 0), e = T(0, 0), f = T(1, 0), h = T(0, 1);
#undef T
  const float3 mn4 = min(min(b, d), min(f, h));
  const float3 mx4 = max(max(b, d), max(f, h));
  // How far the cross may push the centre before leaving [0, 1] - per channel, the least of them.
  const float3 hitMin = mn4 / max(4.0 * mx4, 1e-5);
  const float3 hitMax = (1.0 - mx4) / min(4.0 * mn4 - 4.0, -1e-5);
  const float3 lobeRGB = max(-hitMin, hitMax);
  const float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * g_UvScale.x;
  const float3 rgb = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
  // (y: the input's red and blue swapped - FSR 2's result from a swapped resolve copy)
  return float4(saturate(g_UvScale.y > 0.5 ? rgb.bgr : rgb), 1.0);
}
