# Direct3D 11 plan

Goal: PCs whose GPU or driver can run neither Direct3D 12 nor Vulkan can play.
Examples are GeForce 400-600 and Radeon HD 5000-6000 era cards, and Intel HD
2500/4000 (feature level 11_0). Feature level 10_x (Intel HD 2000/3000) and
Windows 7 / 8.1 come later, if they turn out to be feasible. The work is on
branch `d3d11`, worktree `D:\Xbox Games Ports\SvR2011 D3D11`.

## Status (phase 1 built)

Steps 1-4 below are done, and step 5 is partly done.

**How to use it.**
- `gpu_backend = "d3d11"` (GRAPHICS > GRAPHICS API: DIRECT3D 11, or the
  launcher's Settings) draws the whole game on Direct3D 11.
- `"any"` (AUTO) picks it when neither Direct3D 12 nor Vulkan can run the
  renderer (gpu_probe.cpp). The answer is kept per GPU and driver in
  `UserData\cache\graphics_api.txt`.
- Shaders: `native_shaders\shaders.dxbc.pak` (tools/convert_shaders.py
  writes `dxbc/`, tools/pack_shaders.py packs it).

**Pieces.**
- SDK `graphics/d3d11`: a command processor without emulated drawing (the
  gamma ramp as a pixel shader pass).
- SDK `ui/d3d11`: provider, presenter and ImGui drawer. One immediate context
  is shared under a lock. The presenter uses a flip-model swap chain (blt on
  Windows 7) with a frame-latency waitable object.
- plume_d3d11: recorded command lists replayed on a thread of their own, and
  upload rings mapped NO_OVERWRITE that append within a frame.
- Renderer "slot tables" (SlotTables()). Slot swizzles go where the
  descriptor indices would be in g_ResourceIndices.

**Settings for A/B tests.**
- `native_d3d11_flush_draws`: default 1000. Light submissions flush at once.
- `native_d3d11_replay_thread`: default on.
- `d3d11_feature_level`: 0, 110, 101 or 100, to stand in for older GPUs.
- `d3d11_adapter = -2`: WARP, the software rasterizer.
- `d3d11_debug`: the debug layer. With `SVR2011_NATIVE_D3D_DEBUG=1` its
  messages are logged.
- `SVR2011_D3D11_PROBE=1`: logs the average colour of the frame and of the
  guest output every 300 swaps. A cheap check that frames aren't black or
  wrong.

**Tested on this PC (RTX 4080, also at feature level 11_0).**
- Title, menus, the demo match, and a full CPU match from the entrances to
  the highlights.
- The debug layer reports nothing but the expected refusal of a
  constants+vertex buffer.
- A 10_1 cap shows the "needs 11_0" message.
- D3D12 and Vulkan are unchanged on the same build.

**Open.**
- WARP (software) draws the title, the menus and, at render scale 1x, the
  demo match correctly. At 2x (the default 2x AA) the shaded 3D scene is
  near-black, while geometry, depth and 2D are right. NVIDIA is correct at
  2x. The cause is not found yet; it could show on some real AMD / Intel
  drivers.
- Still to test: texture packs, wide screens, CAW / Threads painting
  (write-back) and Mod Maker previews.
- Phase 2: feature level 10_x (vertex shaders linked per pipeline) and
  Windows 7 / 8.1.
- The shared `runs\shaders_native` must be converted again with this header
  for `dxbc/` (deploy and package take it from there).

## Where things stand today

The game draws only through the native renderer (`src/native/*`), and the
renderer draws through plume (D3D12 and Vulkan backends). It always draws on
**the emulator's own device**.

The emulator's GPU plugin (`rexgpu-xenos`) still does several jobs even
though host drawing is off:
- It runs the guest's command stream: fences, interrupts and swaps.
- At each swap it takes the renderer's finished frame (`rex/external_frame.h`).
- It sends that frame through its presenter, which handles gamma, scaling,
  the window swap chain, and every ImGui overlay. The ImGui overlays are the fps
  counter, the GRAPHICS / BACKGROUNDS / ACHIEVEMENTS / jukebox / Paint pages,
  the touch controls, and the "can't run" message.

So D3D11 needs three pieces, not one:

| piece | today | for D3D11 |
|---|---|---|
| guest GPU emulation (SDK `graphics/`) | D3D12 or Vulkan command processor (17k lines each). With host drawing off, it still builds shared memory, texture, render-target and pipeline caches on the device | a **device-less command processor**: the PM4 work in the base class runs on the CPU, draws and copies return, and IssueSwap hands the frame to the presenter |
| presentation (SDK `ui/`) | D3D12 / Vulkan presenter + ImmediateDrawer | a **D3D11 presenter** (DXGI swap chain, letterbox scaling, gamma as a pixel shader, no compute) + a **D3D11 ImmediateDrawer** (ImGui) |
| native renderer (`port/src/native`, plume) | plume D3D12 / Vulkan | a **plume D3D11 backend** + `backend_d3d11.cpp` + DXBC shaders |

Facts behind this plan:
- **memexport.** With host drawing off, only memexport draws still need the
  emulator's GPU. None of the last 400 test logs has a "memexport draw" line.
  The native renderer does its own resolve write-back to guest memory
  (`native_resolve_write_back`, CPU readback). A device-less command processor
  therefore loses nothing the game uses.
- **No fallback today.** `D3D12Provider::Create` calls `FatalError` when D3D12
  can't start. `plugin_main` "any" returns D3D12 whenever it is compiled in.
  So there is no fallback anywhere yet.

## The choice: option A, scoped - a plume D3D11 backend for the subset the renderer uses

The renderer uses only a small part of plume: about 25 command-list calls and
10 device calls. `recording_list.h` already lists every call, and the rest
abort. It uses:
- one queue;
- no compute, UAVs, MSAA, queries, indirect draws or swap chains.

A D3D11 path that is *not* plume (option B) would mean rewriting the
renderer's 130 call sites behind a second interface. A plume backend keeps the
renderer as it is. So the backend implements exactly that subset, and any
other call logs and fails. It is "option B inside plume".

How each plume feature maps to D3D11:

- **Command lists.** Recorded on the CPU (like `recording_list.cpp`), then
  replayed on the immediate context in `executeCommandLists`. The context lock
  (`ID3D11Multithread`) is held while the list replays. The presenter (UI
  thread) uses the same context under the same lock. Each list starts from
  `ClearState`, so neither side depends on the other's state. The optional
  `native_record_thread` keeps working, because recording never touches D3D11.
- **Fences.** Each signal is a `D3D11_QUERY_EVENT`. A wait polls `GetData`
  under the lock, with a short sleep, and `waitForCommandFenceTimeout` works the
  same way. On Windows 10 the plan can later use `ID3D11Fence` (11.3) instead.
- **Barriers and texture layouts.** No-ops.
- **Descriptor sets.** CPU arrays of SRVs and samplers. Binding a set copies
  them to the pipeline layout's slot range (`PS/VSSetShaderResources`, only the
  slots that changed).
- **Root descriptors and dynamic offsets.** The constants already sit in the
  frame's upload ring at 256-byte offsets.
  - On D3D 11.1 with `ConstantBufferOffsetting` (Windows 8+ drivers, and WARP),
    the backend uses `VS/PSSetConstantBuffers1` with offsets, and no copies.
  - Otherwise (Windows 7, or old drivers) it copies the draw's constants into
    small dynamic constant buffers with `Map(DISCARD)`.
- **Upload buffers.**
  - The ring is a `D3D11_USAGE_DEFAULT` constant buffer, filled by
    `UpdateSubresource1(NO_OVERWRITE)` per frame range, or a
    `DYNAMIC + NO_OVERWRITE` buffer. On 11.0, a constant buffer can't be
    mapped NO_OVERWRITE: that again uses the copy path.
  - Texture uploads use `UpdateSubresource` from the mapped upload buffer.
- **Textures.**
  - Targets are TYPELESS resources with typed views. All formats used exist on
    feature level 11_0: RGBA8 (+sRGB), RGB10A2, RGBA16F, RG16F, R32F, RG32F,
    BC1-5, R8, RG8, R16 and RG16.
  - Depth is `R24G8_TYPELESS`: a `D24_UNORM_S8_UINT` DSV, plus an
    `R24_UNORM_X8_TYPELESS` SRV for sampling resolved depth.
  - 2D, array, 3D and cube textures with mips all exist.
- **Copies.**
  - `copyTextureRegion` maps to `CopySubresourceRegion`.
  - Readback (the Threads / CAW write-back) uses a `STAGING` texture and `Map`.
- **Pipelines.**
  - Blend, depth-stencil and rasterizer state objects, cached by value.
  - An input layout per (VS, vertex format).
  - Topologies are already only list and strip, because the renderer turns
    quads into triangle lists itself.
  - Stencil reference and blend factor are set at `OMSet*` time.
- **SRV swizzles: the one real gap.** D3D11 views have no component mapping.
  The renderer uses swizzles for the Xenos fetch swizzle and for formats it
  expands, such as L8 and A8. The fix is a small per-slot swizzle table in a
  D3D11-only constant buffer: a 4x4 select matrix plus a constant 0/1 column.
  The `tfetch*` helpers apply it in the D3D11 shader build. That is 4 dot
  products per fetch, about free next to a texture fetch. The renderer already
  knows each slot's swizzle when it binds the slot.

### Shaders: HLSL to DXBC with fxc (already tried)

DXIL can't become DXBC, so the D3D11 shaders are compiled from the HLSL that
XenosRecomp already writes: `runs/shaders_native/hlsl`, 467 files. The
compiler is fxc (`fxc.exe` from the Windows SDK, or `d3dcompiler_47`).

What the D3D11 build changes:
- **Binding.** Each fetch slot gets its own register: 2D textures t0-31, 3D
  t32-47, cube t48-63, samplers s0-15 (VS slots 16/17 remapped). So
  `g_ResourceIndex(DIM, SLOT)` is the literal slot. Once the helpers are
  inlined, every resource index is a constant, which SM 5.0 needs.
- **Constants.** The constant buffers stay the DXIL layout: b0 vertex, b1
  pixel, b2 shared, b3 own. Only `space4` is dropped.
- **fxc syntax fixes:**
  - `[[vk::…]]` and `[shader(…)]` are removed;
  - `select()` becomes `?:`;
  - `DEFINE_SHARED_CONSTANTS()` loses its empty parameter list;
  - the local variable named `texture` (a keyword in fxc) is renamed;
  - constant arrays that a same-name macro wraps are renamed to `NAME_a`.

Result (scratchpad trial, before any XenosRecomp change):
- **467 / 467 compile as vs_5_0 / ps_5_0.**
- The DXBC totals 3.2 MB, at 2 jobs.

These changes go into XenosRecomp's `shader_common.h` / recompiler (an
`SVR_D3D11` branch in the patch) and `convert_shaders.py` (a `.dxbc` variant).
That gives `native_shaders/shaders.dxbc.pak`. The own shaders (present, debug)
get the same branch.

**Feature level 10_x (SM 4.0):**
- All 349 pixel shaders compile as ps_4_0.
- All 118 vertex shaders fail: they declare 25 outputs, and vs_4_0 allows 16.

Supporting 10_x therefore needs vertex shaders linked to each pipeline's pixel
shader: only the outputs that pixel shader reads, in its order. There are 298
pipelines in `pipelines.list`. That is phase 2. Phase 1 needs feature level
11_0.

## Picking the API

- `gpu_backend = d3d11` forces D3D11. It loads the plugin's D3D11 graphics
  system, and the native renderer's `backend_d3d11.cpp`.
- `gpu_backend = any` tries D3D12, then Vulkan, then D3D11. The test happens in
  `OnPreSetup`, before the plugin loads, with cheap probes:
  - **D3D12:** `D3D12CreateDevice` at 11_0 + `ID3D12Device8`, plus the binding
    tier the renderer's 16384-entry tables need (resource binding tier 2+), on
    Windows build 19041+.
  - **Vulkan:** an instance + a device with descriptor indexing or the compact
    path's needs.
  - **Otherwise:** D3D11 at 11_0.
  
  One log line names the result and why, for example `graphics: D3D12 not
  usable (binding tier 1) - Vulkan not usable (no device) - using Direct3D 11`.
- **Failures after start.**
  - `D3D12Provider::Create` stops calling `FatalError` and returns null, like
    Vulkan's. Its SDK patch also makes `plugin_main` fall through to the next
    backend.
  - The native renderer's `Fail()` message names the GRAPHICS API option
    (D3D11).
- **UI.** The GRAPHICS > DISPLAY GRAPHICS API row (graphics_page.cpp) becomes
  three values: AUTO / VULKAN / DIRECT3D 11. The launcher Settings gets the
  same choice.
- **Delay-loading.** The exe imports `d3d12.dll` directly, so it can't even
  load where there is no d3d12.dll. `d3d12.dll` (and `d3d11.dll`) become
  `/DELAYLOAD`.

## Windows 7 / 8.1 (later, if feasible)

What blocks it today:
- `svr2011.exe` statically imports `d3d12.dll`; the delay-load fixes this.
- `SetThreadDescription` (Windows 10 1607) is imported statically; it would
  have to be looked up at run time.
- The DXGI flip model needs Windows 8+ (Windows 10 for `FLIP_DISCARD`). The
  D3D11 presenter therefore uses the blt model (`DISCARD`) when flip isn't
  there.
- `ConstantBufferOffsetting` isn't there on Windows 7 (the copy path covers
  it).

The rest (the runtime, SDL, the VC++ runtime) needs a check of each DLL's
imports. It can't be tested on this PC, so it stays "untested" until someone
with Windows 7 tries it.

## Steps (each one builds and is tested before the next)

1. **SDK: device-less command processor + D3D11 provider, presenter and
   ImmediateDrawer.**
   - `gpu_backend=d3d11` reaches the title. In this step the "frame" is a
     clear colour, and the ImGui overlays (fps, GRAPHICS page) work.
   - Also: the D3D12 provider returns null instead of `FatalError`, and
     `CountSwap` is called on D3D12 too (it is missing there today).
2. **plume D3D11 backend (the subset) + `backend_d3d11.cpp` (PublishFrame
   with a D3D11 texture in `external_frame::Frame`).**
3. **DXBC shaders:** the XenosRecomp `SVR_D3D11` branch, `convert_shaders.py`
   `.dxbc`, the pack, the own shaders, and the swizzle constants.
4. **Selection:** the `any` fallback chain, the GRAPHICS row, the launcher,
   the Fail message, and the delay-load.
5. **Tests on this PC with `gpu_backend=d3d11`:**
   - title, menus, a full CPU match, entrances, replays;
   - texture packs, widescreen, AA / scale;
   - Threads / CAW painting (the write-back);
   - feature-level stand-ins: WARP (`SVR2011_D3D11_WARP=1`) and a forced
     11_0 cap.
   
   Performance goes in PERF_LOG.md, compared with D3D12 on the same benchmark
   (opt_bench).
6. **Later:** feature level 10_x (VS linked per pipeline), and Windows 7 / 8.1.

## Risks

- **Draw-call cost.** D3D11 costs more CPU per draw than D3D12. Entrances
  reach 3,000-5,000 draws a frame. Mitigations:
  - state objects cached by value;
  - binding only the slots that changed;
  - constants by offset (11.1).
  
  The opt_bench weak setup will show the cost.
- **Shared context.** One immediate context serves the renderer's replay
  (render thread) and the presenter (UI thread). The lock and `ClearState` at
  each list start are the safety, and they are measured, not assumed.
- **Swizzles.** A missed swizzle case shows up as wrong colours. The texture
  tests (packs, CAW logos, menus) cover the formats.
