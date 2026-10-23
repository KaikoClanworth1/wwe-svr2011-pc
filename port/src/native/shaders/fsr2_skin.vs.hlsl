// Native renderer FSR 2 / DLSS: a skinned draw (the wrestlers, the referee) drawn again into the motion vectors -
// its bind-pose vertices skinned with this frame's and the last frame's bones (vertex constants c74-c136: 4x3
// matrices, a bone = its blend index x 3 rows), each through its frame's view x projection (c54-c57), as the
// game's skinning shaders do: the first bones weighted by BLENDWEIGHT0, the last one by the rest (native_renderer.cpp
// DrawFsr2SkinMotion). Positioned exactly as the game drew it (its NDC scale and offset, the jitter included);
// the pixel shader gets both frames' clip positions without it.
#include "own_constants.hlsli"

#define kBoneRows 64
#ifdef __spirv__
#define g_CurVp(I) OWN_CONSTANT(float4, 16 * (I))
#define g_PrevVp(I) OWN_CONSTANT(float4, 64 + 16 * (I))
#define g_CurBone(I) OWN_CONSTANT(float4, 128 + 16 * (I))
#define g_PrevBone(I) OWN_CONSTANT(float4, 128 + 16 * kBoneRows + 16 * (I))
#define g_Params OWN_CONSTANT(float4, 128 + 32 * kBoneRows)   // x bones a vertex, zw: NDC offset (jittered)
#define g_Params2 OWN_CONSTANT(float4, 144 + 32 * kBoneRows)  // xy: NDC scale
#else
cbuffer SkinConstants : register(b3, space4) {
  float4 g_CurVpA[4];
  float4 g_PrevVpA[4];
  float4 g_CurBoneA[kBoneRows];
  float4 g_PrevBoneA[kBoneRows];
  float4 g_Params;
  float4 g_Params2;
};
#define g_CurVp(I) g_CurVpA[I]
#define g_PrevVp(I) g_PrevVpA[I]
#define g_CurBone(I) g_CurBoneA[min(I, kBoneRows - 1)]
#define g_PrevBone(I) g_PrevBoneA[min(I, kBoneRows - 1)]
#endif

struct Out {
  float4 pos : SV_Position;
  float4 cur : TEXCOORD0;   // this frame's clip position (unjittered)
  float4 prev : TEXCOORD1;  // the last frame's
};

float3 Bone(bool prev, uint index, float4 p) {
  const uint row = index * 3;
  if (prev) return float3(dot(p, g_PrevBone(row)), dot(p, g_PrevBone(row + 1)), dot(p, g_PrevBone(row + 2)));
  return float3(dot(p, g_CurBone(row)), dot(p, g_CurBone(row + 1)), dot(p, g_CurBone(row + 2)));
}

float3 Skin(bool prev, float4 p, float4 w, uint4 idx, uint n) {
  float3 world = 0;
  float rest = 1.0;
  [unroll] for (uint i = 0; i < 3; ++i) {
    if (i + 1 < n) {
      world += Bone(prev, idx[i], p) * w[i];
      rest -= w[i];
    }
  }
  return world + Bone(prev, idx[n - 1], p) * rest;
}

float4 Clip(bool prev, float3 world) {
  const float4 w = float4(world, 1.0);
  if (prev) return float4(dot(g_PrevVp(0), w), dot(g_PrevVp(1), w), dot(g_PrevVp(2), w), dot(g_PrevVp(3), w));
  return float4(dot(g_CurVp(0), w), dot(g_CurVp(1), w), dot(g_CurVp(2), w), dot(g_CurVp(3), w));
}

Out main([[vk::location(0)]] float4 position : POSITION0, [[vk::location(10)]] float4 weight : BLENDWEIGHT0,
         [[vk::location(9)]] uint4 index : BLENDINDICES0) {
  const uint n = clamp(uint(g_Params.x), 1u, 4u);
  const float4 p = float4(position.xyz, 1.0);
  Out o;
  o.cur = Clip(false, Skin(false, p, weight, index, n));
  o.prev = Clip(true, Skin(true, p, weight, index, n));
  o.pos = o.cur;
  o.pos.xy = o.pos.xy * g_Params2.xy + g_Params.zw * o.pos.w;
  return o;
}
