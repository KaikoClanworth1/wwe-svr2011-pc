# Mali GPUs (Android): what they lack, what the port needs

Status, October 2026: **alpha**. Nobody on the team has a Mali phone, so all of this comes
from the drivers' own reports on [vulkan.gpuinfo.org](https://vulkan.gpuinfo.org). The Mali
paths have been tested by making another GPU pretend to be Mali (see *Simulating a Mali GPU*
below), not on Mali hardware.

## The drivers looked at

Each is the newest driver found for that GPU. Report pages are
`https://vulkan.gpuinfo.org/displayreport.php?id=N`.

| GPU | Device | Driver (Arm DDK) | Vulkan | Report |
|---|---|---|---|---|
| Mali-G57 MC2 | Infinix X6850 | r54p1 | 1.3.303 | 52115 |
| Mali-G68 MC4 | Lenovo TB370FU | r49p1 | 1.3.278 | 38760 |
| Mali-G78 | Pixel 6 Pro | r54p3 | 1.4.305 | 49273 |
| Mali-G710 | Pixel 7 Pro | r54p3 | 1.4.343 | 50979 |
| Mali-G715 | Pixel 8 | r54p3 | 1.4.343 | 47617 |
| Mali-G720 MC7 | OPPO CPH2761 | r49p1 | 1.3.278 | 43562 |
| Immortalis-G925 MC12 | OPPO PKC110 | r49p1 | 1.3.278 | 52201 |
| Mali-G2-Ultra MC12 | OPPO PMX110 | r56p0 | 1.4.349 | 52183 |

Older drivers are still common. Many phones are on r32 (Vulkan 1.1), for example G57 and
G68 (reports 47374 and 31123).

## What Mali lacks, and what it means for the game

| Missing on Mali | Who needs it | Effect before, and the fix |
|---|---|---|
| **vertexPipelineStoresAndAtomics** (every driver) | The emulator required it to create the device | **The game couldn't start on Mali at all.** Fix: `vulkan_require_vertex_pipeline_stores_and_atomics` is honoured again, and Mali alpha mode turns it off. Memory export from vertex shaders then runs as compute shaders, which the emulator already has; this game issues no such writes. |
| **fillModeNonSolid** (every driver) | The emulator required it | Couldn't start. Fix: alpha mode turns off `vulkan_require_fill_mode_non_solid`, so wireframe falls back to solid fill. |
| **textureCompressionBC / BC formats** (every driver) | Native renderer (DXT textures) | Fix: decoded on the CPU to RGBA8 / RG8 / R8 (ff90d80). The emulated renderer decompresses them in shaders already. |
| **Linear filtering of 16-bit UNORM textures** (G57, G68, G78, G720, G925) | Native renderer (k_16, k_16_16, k_16_16_16_16) | Filtering such a texture is undefined. Fix: converted to 16-bit float on the CPU on GPUs that can't filter them. |
| **shaderClipDistance / CullDistance** (every driver) | Emulator, optional | Already handled: the shader translator skips them when unsupported. The native shaders don't use them. |
| **VK_EXT_shader_stencil_export, fragment_shader_interlock** (every driver) | Emulator, optional | Already handled: extra stencil draws, host render targets. |
| **shaderInt64** (r32 and r38 drivers only) | Native renderer (buffer addresses in shaders) | Native can't start. The emulated renderer runs instead; this is logged. |
| **robustness2 / nullDescriptor** (r49p1 and older) | Both, optional | Already handled: dummy descriptors. |
| **multiDrawIndirect** (G57, G68, G78) | Not used | - |
| **2x MSAA** (before G720: 1/4/8x only) | Emulator's 2x MSAA, optional | Already handled: MSAA off. The native renderer's anti-aliasing is supersampling. |
| **Blending into 32-bit float targets** (G57, G68, G78, G720, G925) | Native R32F / RG32F targets | Unchecked. The game would have to blend into them (not seen). |
| Geometry shaders | Present on every driver, but known to be slow on Mali | Alpha mode makes the emulator expand points, rectangles and quads without them. |

**Not a problem on Mali:**
- D24S8 depth buffers: sampled and as depth targets.
- Descriptor indexing, buffer device addresses (r44 and later), update-after-bind limits (500 000 or more).
- Push constants (256 bytes), mirror-clamp samplers, 16x anisotropy, custom border colours.

## What Mali alpha mode sets (launcher, `Drivers.java`)

| Setting | Value |
|---|---|
| `mali_alpha` | true |
| `vulkan_require_geometry_shader`, `vulkan_require_fill_mode_non_solid`, `vulkan_require_vertex_pipeline_stores_and_atomics` | false |
| `vulkan_force_expand_point_sprites_in_vs`, `vulkan_force_expand_rectangle_lists_in_vs`, `vulkan_force_convert_quad_lists_to_triangle_lists` | true |

On a Mali GPU, Play warns that Mali is unsupported for now, and continuing turns this mode
on. Every log has a **GPU report**: device, driver, features, and what each format the
renderer uses can do. A player's problem report therefore says what their GPU is missing.

## Simulating a Mali GPU

- `vulkan_simulate_mali = true` (any `svr2011.toml`, or `--vulkan_simulate_mali=true`):
  - The Vulkan device hides what every Mali driver lacks: wireframe, vertex-stage stores,
    clip/cull distance, fp64, sparse resources, BC, multi-draw-indirect, stencil export
    and shader interlock.
  - The native renderer then also treats BC and 16-bit UNORM filtering as missing.
  - Use it together with the alpha mode settings above, on the Fold or a PC on Vulkan.
- `SVR2011_NATIVE_NO_BC=1` (environment): only the native renderer's BC fallback. It works
  on D3D12 too.
- **Not covered by these:** format support inside the emulator, and limits. The
  Khronos *Profiles* layer (`VK_LAYER_KHRONOS_profiles`, in the LunarG Vulkan SDK) can load
  a gpuinfo.org report and make any GPU report exactly that device. It needs a download.
- **Speed:** nothing simulates a Mali GPU's speed. The options are:
  - Arm's *Mali Offline Compiler*, in Arm Performance Studio (a free download): per-shader
    cycle counts and register use for a chosen Mali GPU.
  - A real Mali phone, for example a Pixel 6-9, a Galaxy with Exynos, or one from a device
    farm.

## Tested with the simulation (PC, RTX 4080, Vulkan, 5 October 2026)

- **Passed:** all of Mali alpha mode's settings, plus `vulkan_simulate_mali`, with the DXT
  decode turned off. The emulator started, used its fallbacks (compute shaders for memory
  export, solid fill, no clip distances, points, rectangles and quads expanded without
  geometry shaders), and the native renderer converted 16-bit UNORM textures to float. It
  ran through the title demo match at 60 fps.
- **Open bug, the biggest Mali risk:** the native renderer's CPU decode of **DXT5 (BC3)**
  textures ends in a lost GPU device on Vulkan, 3-4 s into the title demo match.
  - It happens every time, whatever the match.
  - DXT1, DXT3, DXN and DXT5A decoded the same way are fine. So is DXT5 decoded on D3D12
    (`SVR2011_NATIVE_NO_BC=1`).
  - The decoder matches a reference decoder to within 1 per channel, so the cause is something
    Vulkan-specific about those textures, not the pixels.
  - It needs the Vulkan validation layers (LunarG Vulkan SDK) to find. Real Mali phones go
    through this path, so expect them to hit it until it's fixed.

## Mali speed notes, untested

- Mali is a tile-based GPU. Every render target switch writes the tile to memory and reads
  it back. The game's Xbox 360 render-target-to-texture resolves are the costly part. The
  load and store operations of the native renderer's render passes are where to look first,
  once there is a device to measure on.
- CPU-decoded DXT textures use 4-8x the memory and upload bandwidth of BC ones. Loads get
  slower; per-frame cost doesn't change.
- Subgroup size is 16 and there's a single queue family, which affects nothing the renderers
  do today.
