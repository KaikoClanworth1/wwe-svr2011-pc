// Native renderer present: shows the front buffer texture Swap names (any
// texture the renderer can bind: a resolve copy or one loaded from memory)
// in the frame image, which has the window's size. Rendered larger than
// that (resolution scale, anti-aliasing), it is averaged down over each
// output pixel's footprint - a single bilinear tap would drop most texels and
// make fine detail shimmer.
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);
cbuffer PresentConstants : register(b3, space4) {
  uint g_Texture;  // SRV heap index
  uint g_Sampler;  // sampler heap index
  float2 g_UvScale;  // part of the texture shown (front buffer larger than the window)
  float2 g_OutputSize;  // the frame image's size, pixels
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
  Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_Texture];
  SamplerState smp = g_SamplerDescriptorHeap[g_Sampler];
  float2 size;
  tex.GetDimensions(size.x, size.y);
  // Texels of the source per output pixel, per axis.
  const float2 ratio = size * g_UvScale / max(g_OutputSize, float2(1, 1));
  const float2 center = uv * g_UvScale;
  if (ratio.x <= 1.0 && ratio.y <= 1.0) {
    return float4(tex.SampleLevel(smp, center, 0).rgb, 1.0);
  }
  // A grid of bilinear taps (each averages 2x2 texels) covering the pixel's
  // footprint: a box filter over it.
  const int2 taps = clamp(int2(ceil(ratio * 0.5)), int2(1, 1), int2(4, 4));
  const float2 footprint = ratio / size;  // in uv
  float3 sum = 0;
  [loop] for (int y = 0; y < taps.y; ++y) {
    [loop] for (int x = 0; x < taps.x; ++x) {
      const float2 o = (float2(x, y) + 0.5) / float2(taps) - 0.5;
      sum += tex.SampleLevel(smp, center + o * footprint, 0).rgb;
    }
  }
  return float4(sum / float(taps.x * taps.y), 1.0);
}
