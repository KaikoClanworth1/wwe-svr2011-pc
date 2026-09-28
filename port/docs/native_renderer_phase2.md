# Native DirectX renderer — phase 2

Phase 1: `native_renderer_phase1.md`. Data: `docs/d3d_map.csv` (47 named D3D
functions), `runs/d3dtrace/` (census output), tools listed at the end.

## 1. Resource objects (decoded, all captured objects parse)

Every D3D object starts with a 24-byte header; `Common & 0xF` is the type.

| Type | Object | Layout after the header |
|---|---|---|
| 1 | vertex buffer | `+0x18` GPU vertex fetch constant (2 words: address, size in dwords, endian) |
| 2 | index buffer | `+0x18` GPU address, `+0x1C` size in bytes (`Common` = `0x20100002`) |
| 3 | texture | `+0x18` MipFlush, `+0x1C` GPU texture fetch constant (6 words) |
| 4 | surface (render target / depth) | `+0x18…` EDRAM surface description (to decode) |
| 5 | vertex declaration | `+0x18` element count, `+0x34` 12-byte `D3DVERTEXELEMENT`s; the element type encodes the GPU vertex format in its low 6 bits |

In the training ring (`tools/decode_d3d_objects.py`):

* **Textures (189):** DXT5 106, DXT1 73, DXT3 4, 8-bit 3, RGBA8/R32F/D24S8 1 each;
  184 2D + 5 cube; 186 linear, 3 tiled; nearly all 8-in-16 byte order;
  8×8 … 1024×512.
* **Vertex buffers (400):** all 8-in-32 byte order, 128 B … 234 KB.
* **Index buffers:** 16-bit (inferred from sizes and draw counts).
* **Vertex declarations (6):** e.g. the wrestler skinning format —
  `POSITION float4, BLENDWEIGHT float3, BLENDINDICES ubyte4, NORMAL 11:11:10,
  COLOR ubyte4, TEXCOORD half2`.

## 2. One frame in the ring (frame log)

~25,000 D3D calls, all on one thread. Passes:

1. Main target `AD97E550` (1280×720): Clear.
2. `ADE6A960` 512×512: Clear, quad draws, **Resolve → texture** (render to texture).
3. `ADE702F0` 1120×1120: Clear, 71 draws, **Resolve** — shadow map.
4. Main target: Clear, ~560 scene draws, colour Resolve, **depth Resolve**,
   53 post-process / UI draws, Present.

Pass structure: `SetRenderTarget` → setup chain (viewport/scissor) → `Clear`
(+ a full-surface rect draw) → `SetShaderGPRAllocation` → draws → `Resolve`.

## 3. Newly identified

Clear (high), Resolve (high), SetShaderGPRAllocation (high: its two
arguments always sum to the Xenos' 128 registers), SetRenderTarget (medium),
plus the render-target setup chain and Clear/Resolve helpers (low–medium).

## 4. Render state: the device's register mirror (found)

The device object (`0xAD977E80`) keeps a **mirror of the Xenos GPU
registers** it programs, in groups; internal functions `82924E40`,
`82925080` and `829251D8` copy them into the command buffer at draw time as
(first register, device offset) pairs, which gave the exact layout:

| Registers | Device offset | Contents |
|---|---|---|
| 0x4800–0x48BF | +0x0480 | fetch constants: 32 slots × 6 words (textures; vertex fetch vfN = slot N/3) |
| 0x4000–0x47FF | +0x0780 | shader float constants (vertex c0–255, pixel from 0x4400 = +0x1780) |
| 0x4900– | +0x2780 | boolean constants |
| 0x2000–0x2012 | +0x2880 | surface/colour/depth info, screen scissor |
| 0x2100–0x2114 | +0x28CC | index limits, colour mask, blend colour, stencil ref, alpha ref, **viewport** |
| 0x2180–0x2184 | +0x2920 | shader program control, interpolators |
| 0x2200–0x220B | +0x2934 | **depth, blend 0–3, colour control, cull**, clip, mode |
| 0x2280–0x2294 | +0x2964 | point/line/tessellation |
| 0x2300–0x2325 | +0x29B8 | AA, constant partition, **resolve (RB_COPY_*)** |

Verified on an in-ring snapshot (shadow-map pass): viewport 560/560/−560/560
= 1120×1120, surface pitch 1120, depth test LESS-EQUAL + write, polygon
offset on, fetch slot 12 = the 1120×1120 `32_FLOAT` shadow depth, slots
0–7 = the pass's DXT textures, `vf95` = the draw's vertex buffer.

Current shaders: **pixel at device+0x3244, vertex at +0x3248**, set by
`SetPixelShader` (82920B58, objects of type 7 that embed their container
header) and `SetVertexShader` (82920D60, type 6).

So at every draw, the native renderer can read the complete state —
shaders, textures, vertex buffers, constants, render state, render target —
as documented Xenos registers from one object. `src/native/guest_d3d.h`
encodes all of it (compile-checked offsets).

Still to decode: the surface object (EDRAM base / format) for render targets,
and cube-map size fields in the fetch-constant decoder.

## 5. Shader precompilation and caching (design)

* The 467 shaders are converted to DXIL at **build time** and embedded in
  the exe (as Unleashed Recompiled does): no shader translation at runtime.
* What still stutters on first use is creating **pipeline state objects**
  (shader pair + vertex format + blend/depth/raster state + render target
  formats). Plan:
  1. Record every PSO combination the game uses (a small file, built up while
     playing and shipped with the port).
  2. Create them all on worker threads while the **main menu / loading
     screens** are up, with a progress indicator.
  3. Store the compiled PSOs in a D3D12 pipeline library
     (`ID3D12PipelineLibrary`) in `UserData\cache`, so later launches load
     them instantly; rebuild automatically after driver changes.

## 6. Tools added in phase 2

* `src/d3d_trace.cpp` (`build.ps1 -Trace`): object dumps
  (`SVR2011_D3D_TRACE_DUMP=<fn>:r<N>:<bytes>`), ordered frame log
  (`SVR2011_D3D_TRACE_FRAMELOG=<frame>`), device snapshot
  (`SVR2011_D3D_TRACE_DEVICE_FRAME=<frame>`).
* `tools/decode_d3d_objects.py` — decodes textures, buffers, declarations.

## 7. Next

1. ~~Find the render state block~~ — done (section 4).
2. ~~Identify SetVertexShader / SetPixelShader~~ — done.
3. Prototype: hook Present + draws, and draw the ring natively in a second
   debug window from the captured state, next to the emulated output.
