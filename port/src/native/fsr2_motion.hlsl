// FSR 2's motion vectors from the camera (fsr2.cpp, native/native_renderer.cpp): each pixel's scene depth taken
// back to where the last frame's camera saw it - the world standing still (the wrestlers' own moves: the
// composition mask). Written as FSR 2 wants them: last frame's position - this one's, render pixels, unjittered.
Texture2D<float> r_depth : register(t0);
RWTexture2D<float2> rw_motion : register(u0);
RWTexture2D<float> rw_depth : register(u1);  // (the depth as R32_FLOAT too: DLSS reads it - not a depth format)
cbuffer cbMotion : register(b0) {
  row_major float4x4 reproject;  // (row vectors) this frame's clip space, unjittered -> the last frame's
  float4 jitter_size;            // xy: this frame's jitter in clip space, zw: the render size
};

[numthreads(8, 8, 1)]
void CS(uint2 id : SV_DispatchThreadID) {
  if (any(float2(id) >= jitter_size.zw)) return;
  const float depth = r_depth.Load(int3(id, 0));
  const float2 ndc = float2((id.x + 0.5) / jitter_size.z * 2.0 - 1.0, 1.0 - (id.y + 0.5) / jitter_size.w * 2.0) -
                     jitter_size.xy;
  const float4 prev = mul(float4(ndc, depth, 1.0), reproject);
  float2 motion = 0;
  if (prev.w > 1e-6) motion = (prev.xy / prev.w - ndc) * float2(0.5, -0.5) * jitter_size.zw;
  rw_motion[id] = motion;
  rw_depth[id] = depth;
}
