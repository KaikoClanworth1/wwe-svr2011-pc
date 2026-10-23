// Native renderer FSR 2 / DLSS: a skinned draw's own motion (fsr2_skin.vs) - where this pixel was in the last frame,
// render pixels (previous - current, FSR 2's convention), alpha 1: the camera's motion pass keeps it
// (fsr2_motion.hlsl).
#include "own_constants.hlsli"

#ifdef __spirv__
#define g_Size OWN_CONSTANT(float4, 160 + 32 * 64)  // xy: the render size
#else
cbuffer SkinSize : register(b3, space4) {
  float4 g_Pad[4 + 4 + 64 + 64 + 2];
  float4 g_Size;
};
#endif

float4 main(float4 pos : SV_Position, float4 cur : TEXCOORD0, float4 prev : TEXCOORD1) : SV_Target0 {
  float2 motion = 0;
  if (prev.w > 1e-6 && cur.w > 1e-6) motion = (prev.xy / prev.w - cur.xy / cur.w) * float2(0.5, -0.5) * g_Size.xy;
  return float4(motion, 0.0, 1.0);
}
