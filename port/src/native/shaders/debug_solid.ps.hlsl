// Native renderer debug pixel shader: a flat colour per draw (b3 space4).
#include "own_constants.hlsli"
#ifdef __spirv__
#define g_DebugColour OWN_CONSTANT16(float4, 0)
#else
cbuffer DebugConstants : register(b3, space4) { float4 g_DebugColour; };
#endif
float4 main(float4 pos : SV_Position) : SV_Target0 { return g_DebugColour; }
