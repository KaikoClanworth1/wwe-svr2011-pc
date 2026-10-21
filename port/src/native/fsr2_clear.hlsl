// The FSR 2 backend's clear job (fsr2.cpp): mip 0 of a float storage texture set to one colour.
RWTexture2D<float4> rw_target : register(u0);
cbuffer cbClear : register(b0) {
  float4 color;
};

[numthreads(8, 8, 1)]
void CS(uint2 id : SV_DispatchThreadID) {
  uint w, h;
  rw_target.GetDimensions(w, h);
  if (id.x < w && id.y < h) rw_target[id] = color;
}
