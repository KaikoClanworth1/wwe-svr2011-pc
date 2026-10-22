// Native renderer FSR 2 (native_upscale fsr2): a scene draw drawn again into FSR 2's masks - the reactive one
// (target 0: trust the history less) and the transparency / composition one (target 1) - a value per draw
// (the HUD, blended draws, the wrestlers: native_renderer.cpp DrawFsr2Masks).
#include "own_constants.hlsli"
#ifdef __spirv__
#define g_Mask OWN_CONSTANT16(float4, 0)
#else
cbuffer MaskConstants : register(b3, space4) { float4 g_Mask; };  // x: reactive, y: composition
#endif

struct Masks {
  float4 reactive : SV_Target0;
  float4 composition : SV_Target1;
};

Masks main(float4 pos : SV_Position) {
  Masks m;
  m.reactive = g_Mask.xxxx;
  m.composition = g_Mask.yyyy;
  return m;
}
