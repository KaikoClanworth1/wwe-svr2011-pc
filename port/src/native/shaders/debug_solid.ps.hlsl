// Native renderer debug pixel shader: a flat colour per draw (b3 space4).
cbuffer DebugConstants : register(b3, space4) { float4 g_DebugColour; };
float4 main(float4 pos : SV_Position) : SV_Target0 { return g_DebugColour; }
