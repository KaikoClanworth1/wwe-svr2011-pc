# Shader precompiling (plan, draft)

Goal: no hitches the first time a scene, camera cut or finisher appears. The native renderer builds
its GPU pipelines in the background while the player is in the menus, and a small progress bar shows
it at the bottom right.

## Why (from a low-end laptop's logs: i5 4th gen 2C/4T, HDD, Radeon R5 M430)

- **New pipelines:** about 220 are built in a session's first minutes. Before b42cf33 that happened
  again at every launch, and some builds took 70-90 ms. The D3D12 pipeline library (b42cf33) now
  keeps them between launches, so it's first-time only.
- **Shader files:** each new shader reads 4 files from `native_shaders\` (2,427 files, 26 MB). On an
  HDD that's ~10-15 ms of seek per file, so a scene with a few dozen new shaders freezes for
  hundreds of ms. The 120-270 ms frames in those logs are where new pipelines appeared, while the
  build timer itself was small.
- **On the render thread:** both happen at the moment the game first draws with that state, which
  is the worst time to do them.

## How the native renderer builds pipelines today

- **Shaders:** `Shader`s are loaded on first use (`LoadShader`: `<hash>.vs|ps[.s1|.s2]<ext>` +
  `.inputs` / `.textures`) and compiled for the device on first use (`Compiled`).
- **Pipelines:** `Pipeline(...)` makes a `PipelineKey` from:
  - vs/ps hash and variant
  - topology and render-target format
  - depth/stencil, blend, colour mask, cull and bias state
  - a hash of the vertex input layout

  It looks the key up in `r->pipelines` and builds the pipeline on a miss. The input layout comes
  from the game's vertex declaration at draw time, so it's part of what has to be recorded.

## Plan

1. **Pipeline list (what to build).** Each new pipeline's full description is recorded to
   `UserData\cache\native_pipelines.list`, one record per pipeline:
   - the key
   - the input elements and slots
   - the shader names and variants

   The list doesn't depend on the GPU or the driver, unlike the pipeline library.
   - A **seed list** ships with the game (`native_shaders\pipelines.list`), collected by us from
     scripted tours: every arena, entrances of the whole roster, match types, Universe cutscenes,
     Create modes.
   - The player's own list adds whatever they meet that the seed lacks.
2. **Shader pack (HDD).** `tools/package.ps1` packs `native_shaders\` into one file per API
   (`shaders_d3d12.pak` / `shaders_vulkan.pak`: an index plus the blobs). It's read in one
   sequential pass at startup, about 11 MB for D3D12, instead of 2,400 seeks. Loose files stay
   supported for development.
3. **Background builder.** It starts after the device is up, and runs at below-normal priority on
   1 thread (2 threads on CPUs with 8+ logical cores) so the game's own threads keep their CPU.
   - **Work:** it walks the list and, for each entry, loads or compiles the shaders and creates the
     pipeline. It creates pipelines the same way the render thread does, so the D3D12 library or
     the Vulkan cache stores the result.
   - **Done pipelines** go into a map shared with the render thread (under a lock). On a miss the
     render thread checks it first. If the builder is busy on that exact pipeline, the render
     thread waits for it rather than building it twice.
   - **Order:** the builder works in the order things are needed. Menus come first, then the
     superstars and arena of the match being set up when that's known, then the rest.
   - **Pausing:** it stops for the rest of a match, entrance or cutscene once it starts. It only
     works while menus or loading screens are up, so it never takes CPU from gameplay. This uses
     the existing match/entrance hooks (`SetMatchScene`).
4. **Progress bar.** A small ImGui panel at the bottom right shows "Preparing graphics 412 /
   1,830" with a thin bar. It appears only while the builder is working and a menu is showing, and
   fades out when done.
   - **Later launches:** the list's entries are already in the pipeline library, so they load in a
     few seconds or less, and the bar flashes briefly or not at all. It's skipped when a "done"
     stamp (list version + driver + GPU) matches.
5. **Settings.** "Prepare graphics in the menus" (on by default) on the in-game GRAPHICS page and
   in the launcher. It applies to D3D12 and Vulkan, including Android, where phones' drivers keep
   no cache of their own.

## Open questions

- **Where the list comes from:** shipped seed plus the player's own (recommended), or player-only
  (no tours needed, but the first session still hitches)?
- **Builder threads on low-end CPUs:** 1 at low priority (recommended), or more threads for a
  faster first launch?
- **First launch:** never block (recommended: the bar runs during the menus and anything not ready
  is built on demand as now), or hold at the title screen until the menu set is ready?

## Measuring it

- **Before / after,** on the low-end log's numbers:
  - frames over 50 ms in the first 10 minutes of a fresh install, then of a second launch
  - the time until the bar finishes
- **Test PCs:** our PC with the shader cache cleared (`UserData\cache\*`, plus the driver cache), a
  Proton/Deck run, and the phone (Vulkan).
