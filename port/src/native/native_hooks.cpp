// WWE SmackDown vs. Raw 2011 - hooks feeding the native renderer.
//
// Each hook runs the game's own D3D function first (so the emulated renderer
// keeps working in "shadow" mode) and then hands the call to the native
// renderer. Present is hooked in frame_stats.cpp. Not built into the D3D
// census build (SVR2011_D3D_TRACE), which hooks every D3D function itself.

#include <rex/hook.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "native/native_renderer.h"

namespace nr = svr2011::native;

// D3DDevice_DrawIndexedVertices(device, primitive, base, start, count)
REX_EXTERN(__imp__sub_82921B58);
REX_HOOK_RAW(sub_82921B58) {
  if (nr::Enabled()) {
    const PPCContext args = ctx;  // the call clobbers argument registers
    __imp__sub_82921B58(ctx, base);
    nr::OnDrawIndexed(args);
    return;
  }
  __imp__sub_82921B58(ctx, base);
}

// D3DDevice_DrawVerticesUP(device, primitive, count, stride): quads for 2D /
// post-processing. Works like BeginVertices: returns (r3) the space the game
// then writes the vertices to, before calling EndVertices.
REX_EXTERN(__imp__sub_82921698);
REX_HOOK_RAW(sub_82921698) {
  if (nr::Enabled()) {
    const PPCContext args = ctx;
    __imp__sub_82921698(ctx, base);
    nr::OnDrawUP(args, ctx.r3.u32);
    return;
  }
  __imp__sub_82921698(ctx, base);
}

// D3DDevice_EndVertices(device): the vertices of the last DrawVerticesUP are in.
REX_EXTERN(__imp__sub_82921688);
REX_HOOK_RAW(sub_82921688) {
  __imp__sub_82921688(ctx, base);
  if (nr::Enabled()) nr::OnEndVertices();
}

// D3DDevice_SetRenderTarget(device, index, surface)
REX_EXTERN(__imp__sub_8291E618);
REX_HOOK_RAW(sub_8291E618) {
  if (nr::Enabled()) nr::OnSetRenderTarget(ctx.r4.u32, ctx.r5.u32);
  __imp__sub_8291E618(ctx, base);
}

// D3DDevice_Clear(device, flags, rects, ...)
REX_EXTERN(__imp__sub_8291F168);
REX_HOOK_RAW(sub_8291F168) {
  if (nr::Enabled()) {
    const PPCContext args = ctx;
    __imp__sub_8291F168(ctx, base);
    nr::OnClear(args);
    return;
  }
  __imp__sub_8291F168(ctx, base);
}

// D3DDevice_Resolve(device, flags, ..., destination texture in r8)
REX_EXTERN(__imp__sub_82918A88);
REX_HOOK_RAW(sub_82918A88) {
  if (nr::Enabled()) {
    const PPCContext args = ctx;
    __imp__sub_82918A88(ctx, base);
    nr::OnResolve(args);
    return;
  }
  __imp__sub_82918A88(ctx, base);
}

// D3DDevice_CreateVertexShader / CreatePixelShader(container) -> shader object in r3
REX_EXTERN(__imp__sub_82921548);
REX_HOOK_RAW(sub_82921548) {
  const uint32_t container = ctx.r3.u32;
  __imp__sub_82921548(ctx, base);
  if (nr::Enabled()) nr::OnShaderCreated(container, ctx.r3.u32, false);
}

REX_EXTERN(__imp__sub_82921360);
REX_HOOK_RAW(sub_82921360) {
  const uint32_t container = ctx.r3.u32;
  __imp__sub_82921360(ctx, base);
  if (nr::Enabled()) nr::OnShaderCreated(container, ctx.r3.u32, true);
}

// D3DDevice_SetStreamSource(device, stream, buffer, offset, stride)
REX_EXTERN(__imp__sub_8291DD70);
REX_HOOK_RAW(sub_8291DD70) {
  if (nr::Enabled()) nr::OnSetStreamSource(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  __imp__sub_8291DD70(ctx, base);
}

// D3DDevice_SetVertexShaderConstantF / SetPixelShaderConstantF(device, start, data, count)
REX_EXTERN(__imp__sub_829208D8);
REX_HOOK_RAW(sub_829208D8) {
  const uint32_t start = ctx.r4.u32, count = ctx.r6.u32;
  __imp__sub_829208D8(ctx, base);
  if (nr::Enabled()) nr::OnSetShaderConstants(false, start, count);
}

REX_EXTERN(__imp__sub_82920800);
REX_HOOK_RAW(sub_82920800) {
  const uint32_t start = ctx.r4.u32, count = ctx.r6.u32;
  __imp__sub_82920800(ctx, base);
  if (nr::Enabled()) nr::OnSetShaderConstants(true, start, count);
}

// D3D internal: write dirty shader constants from the device mirror to the GPU.
REX_EXTERN(__imp__sub_829251D8);
REX_HOOK_RAW(sub_829251D8) {
  if (nr::Enabled()) nr::OnFlushShaderConstants();
  __imp__sub_829251D8(ctx, base);
}

// D3D internal: load a shader's constant blocks from memory (LOAD_ALU_CONSTANT).
REX_EXTERN(__imp__sub_82925D78);
REX_HOOK_RAW(sub_82925D78) {
  if (nr::Enabled()) nr::OnLoadShaderConstants(ctx.r4.u32, ctx.r5.u32);
  __imp__sub_82925D78(ctx, base);
}
