// Native renderer upscaling (native_upscale fsr1), pass 1: FSR 1 EASU - the
// front buffer (rendered smaller than the window) scaled up into the frame-size
// intermediate, edge-adaptive: a 12-texel neighbourhood gives the local edge's
// direction and strength, a Lanczos-2-like window stretched along it weighs the
// texels, the result clamped to the nearest four (no ringing).
// After AMD FidelityFX Super Resolution 1 (EASU), Copyright (c) 2021 Advanced
// Micro Devices, Inc. - MIT licence (the release's third-party notices).
#include "own_constants.hlsli"
OWN_TEXTURE_TABLES
#ifdef __spirv__
#define g_Texture OWN_CONSTANT(uint, 0)
#define g_UvScale OWN_CONSTANT(float2, 8)
#define g_OutputSize OWN_CONSTANT(float2, 16)
#elif defined(SVR_D3D11)
cbuffer PresentConstants : register(b3) {
  uint2 g_Unused;
  float2 g_UvScale;
  float2 g_OutputSize;
};
#define g_Texture 0
#else
cbuffer PresentConstants : register(b3, space4) {
  uint g_Texture;  // SRV heap index
  uint g_Sampler;  // (unused: texels are loaded)
  float2 g_UvScale;  // part of the texture shown
  float2 g_OutputSize;  // the frame's size, pixels
};
#endif

float Luma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

// One of the four bilinear neighbours' edge estimate (a, b, c, d, e: up, left, centre, right, down lumas).
void Edge(inout float2 dir, inout float len, float w, float a, float b, float c, float d, float e) {
  const float dirX = d - b;
  float lenX = abs(dirX) / max(max(abs(d - c), abs(c - b)), 1.0 / 32768.0);
  lenX = saturate(lenX);
  dir.x += dirX * w;
  len += lenX * lenX * w;
  const float dirY = e - a;
  float lenY = abs(dirY) / max(max(abs(e - c), abs(c - a)), 1.0 / 32768.0);
  lenY = saturate(lenY);
  dir.y += dirY * w;
  len += lenY * lenY * w;
}

void Tap(inout float3 acc, inout float accW, float2 off, float2 dir, float2 len2, float lob, float clp, float3 c) {
  float2 v = float2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len2;
  const float d2 = min(v.x * v.x + v.y * v.y, clp);
  float wB = 0.4 * d2 - 1.0;
  float wA = lob * d2 - 1.0;
  wB *= wB;
  wA *= wA;
  wB = 1.5625 * wB - 0.5625;
  const float w = wB * wA;
  acc += c * w;
  accW += w;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
  Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_Texture];
  float2 size;
  tex.GetDimensions(size.x, size.y);
  const float2 region = size * g_UvScale;  // the source texels shown
  float2 pp = uv * region - 0.5;  // (uv over the drawn quad, as present.ps)
  const float2 fp = floor(pp);
  pp -= fp;
  const int2 base = int2(fp);
  const int2 hi = int2(region) - 1;
#define T(X, Y) tex.Load(int3(clamp(base + int2(X, Y), int2(0, 0), hi), 0)).rgb
  const float3 b = T(0, -1), c = T(1, -1);
  const float3 e = T(-1, 0), f = T(0, 0), g = T(1, 0), h = T(2, 0);
  const float3 i = T(-1, 1), j = T(0, 1), k = T(1, 1), l = T(2, 1);
  const float3 n = T(0, 2), o = T(1, 2);
#undef T
  const float bL = Luma(b), cL = Luma(c), eL = Luma(e), fL = Luma(f), gL = Luma(g), hL = Luma(h);
  const float iL = Luma(i), jL = Luma(j), kL = Luma(k), lL = Luma(l), nL = Luma(n), oL = Luma(o);
  // The edge: direction and strength from the four bilinear neighbours.
  float2 dir = 0;
  float len = 0;
  Edge(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
  Edge(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
  Edge(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
  Edge(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
  const float dirR2 = dir.x * dir.x + dir.y * dir.y;
  const bool flat = dirR2 < 1.0 / 32768.0;
  dir = flat ? float2(1, 0) : dir * rsqrt(dirR2);
  len = len * 0.5;
  len *= len;
  const float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
  const float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
  const float lob = 0.5 + (0.25 - 0.04 - 0.5) * len;
  const float clp = 1.0 / lob;
  float3 acc = 0;
  float accW = 0;
  Tap(acc, accW, float2(0, -1) - pp, dir, len2, lob, clp, b);
  Tap(acc, accW, float2(1, -1) - pp, dir, len2, lob, clp, c);
  Tap(acc, accW, float2(-1, 1) - pp, dir, len2, lob, clp, i);
  Tap(acc, accW, float2(0, 1) - pp, dir, len2, lob, clp, j);
  Tap(acc, accW, float2(0, 0) - pp, dir, len2, lob, clp, f);
  Tap(acc, accW, float2(-1, 0) - pp, dir, len2, lob, clp, e);
  Tap(acc, accW, float2(1, 1) - pp, dir, len2, lob, clp, k);
  Tap(acc, accW, float2(2, 1) - pp, dir, len2, lob, clp, l);
  Tap(acc, accW, float2(2, 0) - pp, dir, len2, lob, clp, h);
  Tap(acc, accW, float2(1, 0) - pp, dir, len2, lob, clp, g);
  Tap(acc, accW, float2(1, 2) - pp, dir, len2, lob, clp, o);
  Tap(acc, accW, float2(0, 2) - pp, dir, len2, lob, clp, n);
  const float3 lo = min(min(f, g), min(j, k)), hi3 = max(max(f, g), max(j, k));
  const float3 rgb = clamp(acc / max(accW, 1e-6), lo, hi3);
  return float4(rgb, 1.0);
}
