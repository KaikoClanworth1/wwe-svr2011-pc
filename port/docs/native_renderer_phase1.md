# Native DirectX renderer — phase 1: D3D map and shaders

Goal of phase 1: learn enough about how SvR 2011 uses Direct3D to replace the
Xenos GPU emulation with a native Direct3D 12 renderer (as Unleashed
Recompiled does), and prove its shaders can be converted ahead of time.

## 1. The game's D3D library

The Xbox 360 XDK's Direct3D is linked into `default.xex` without symbols.

* **Location:** `0x82914800 – 0x82933240`, 562 functions. Found from the
  callers of the kernel video imports (`Vd*`) and call-graph locality: almost
  all calls into this range come from inside it.
* **Usage (boot → menus → training ring):** 293 functions are called at all;
  118 run every frame in the ring. The device object is at guest
  `0xAD977E80`; 222 of the 293 are device methods (device in r3).
* **Load per frame in the ring:** ~683 draws (629 indexed triangle-strip
  draws + 54 quad draws), ~11,000 shader-constant updates, ~1,250 texture
  binds, ~600 shader binds.
* **Map:** `docs/d3d_map.csv` — per function: name + confidence + evidence,
  calls per frame (title / menus / ring), argument ranges r3–r8, PM4 packets
  it builds, imports, a caller. 37 functions named; they cover **80% of all
  per-frame D3D calls**.

| Address | Function | Confidence |
|---|---|---|
| 8291AED0 | D3DDevice_Present (Swap) | certain |
| 82921548 | D3DDevice_CreateVertexShader | certain |
| 82921360 | D3DDevice_CreatePixelShader | certain |
| 82921B58 | D3DDevice_DrawIndexedVertices | high |
| 82921698 | D3DDevice_DrawVerticesUP (quads) | medium |
| 829208D8 / 82920800 | Set{Vertex,Pixel}ShaderConstantF | high |
| 829209B0 / 82920A10 | Set{Vertex,Pixel}ShaderConstantB | medium |
| 8291DD70 | SetStreamSource | high |
| 8291DE90 | SetIndices | medium |
| 82917EC8 | SetTexture | high |
| 82920F78 | SetVertexDeclaration | medium |
| 8291D200… (5) | SetSamplerState_* | medium |
| 8291BA88… (13) | SetRenderState_* | medium |
| 8291F168 | Clear | low |
| 82918A88 | Resolve | low |
| 82925D78, 82926000 | internal: shader bind / program load at draw | medium |
| 82919F18 | internal: GPU spin-wait (~430k calls/frame) | medium |

## 2. Shaders

* The disc stores shaders inside compressed `.pac` archives (Yuke's LZ), so
  they are captured at the API instead: every container passed to
  CreateVertexShader / CreatePixelShader is saved.
* **467 unique shaders** (118 vertex, 349 pixel) from boot to the ring —
  most of the game's library is created at startup (408 already at the title
  screen). Arenas/modes not yet visited may add more.
* Converted with **XenosRecomp** (hedge-dev), adapted for this game
  (`tools/patch_xenosrecomp.py`, `patches/xenosrecomp-svr2011.patch`):
  COLOR2–7 interpolators, 32 texture slots (the game uses slots 16–17, and
  unnamed slots), vertex texture fetch via `SampleLevel`.
* **All 467 compile to DXIL** (`tools/convert_shaders.py`). Compiling is not
  yet proof of correct output — that needs the renderer.
* The HLSL keeps the game's constant names (`g_f4DifLghVec`, `g_f3SkyCol`,
  `g_f4MatDifCol`…) from the containers' reflection data.

## 3. Tools (all in `port/tools/`)

| Tool | Does |
|---|---|
| `callgraph.py` | call graph + constants of all 55,964 recompiled functions |
| `gen_d3d_trace.py` + `.\build.ps1 -Trace` | census build: a counting hook on every D3D function |
| `d3d_census.ps1` | runs it boot → ring in the background; call counts, argument ranges, shader capture |
| `d3d_map.py` | builds `docs/d3d_map.csv` |
| `convert_shaders.py` | containers → HLSL → DXIL, with a failure report |
| `scan_guest_shaders.py` | scan live guest memory for shader containers |

## 4. Next (phase 2)

1. Confirm the remaining per-frame functions (render targets, viewport,
   Clear/Resolve details, vertex/index buffer creation, textures) by
   argument logging of the specific functions.
2. Decode the resource objects the game passes (texture, vertex buffer,
   declaration layouts) — needed to create native D3D12 resources.
3. Prototype: keep Xenos emulation running and draw a *second* native frame
   from hooked Draw calls (debug window), shader by shader, comparing against
   the emulated output. Switch over once the ring renders correctly.
