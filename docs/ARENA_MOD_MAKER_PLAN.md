# Arena Mod Maker - plan and Phase 0 findings

Branch `arenas` (worktree `D:\Xbox Games Ports\SvR2011 Arenas`). This work stays
apart from `main` until a future update combines it. The full plan, with mockups and
diagrams, is in the plan artifact (https://claude.ai/artifact/YWqYNczKJju4Qo56tVNhvF).

**Decided (user, 2026-10-02):**
- Launcher Mods tab and a separate `SvR2011 Mod Maker.exe`.
- New arena slots via arena-select pages that work like the Paint Tool pages.
- Ring Kit: rope styles, 0-4 ropes, size, and six sides behind a go/no-go report.
- ufbx for FBX import.
- Run Phase 0 through Milestone A (a custom arena in game) without stopping.

## Isolation

- **Source:** branch `arenas` only; nothing goes to `main`, nothing is pushed.
- **Build:** `port/out/build/arena_build_low.cmd` builds `port/out/build/SourceArenas` (below-normal priority).
  - **Own SDK copy** in `sdk/rexglue-sdk` (git-ignored).
    - The SDK always writes its libraries, DLLs and `rexglue.exe` into `<sdk>/out/win-amd64`, whatever the build folder.
    - The first two arenas builds used the main SDK and so wrote there; those files were built from the same SDK source.
  - `recomp/` is a junction to the main checkout (plume, libadrenotools, tools: read only).
  - `port/generated` and `port/assets` are copies.
- **Test game:** `port/runs/arena_game`, made by `tools/arena_game_setup.ps1`.
  - Read-only junctions into the main install.
  - Real copies of `pac/bg`, `pac/menu` and the top-level `pac/*.pac`, so tests can change them.
- **Test saves:** `port/runs/test_userdata_arena`. **Config:** `port/runs/test_config.toml`.
- **Sessions:** `tools/arena_session.ps1` and `tools/arena_nav.ps1` stop only the arena game, and refuse to start while a player's game runs.

### Shared files touched (for the combine)

- `port/CMakeLists.txt`: adds `src/arena_mods.cpp`.
- `port/src/svr2011_app.cpp`: includes `arena_mods.h` and calls `InstallArenaMods` after `InstallMatchTypes`.

## Phase 0 findings

### Containers (tools/svrfmt.py)

- **EPAC.** Little-endian.
  - Groups at 0x800; entries are `{name[4], sector (0x800 B, from 0x4000), size/0x100}`.
  - Data starts at 0x4000.
  - The file ends with a 0x800-byte packer footer: "EOP5/plugin version 1.2x", machine name, IP, time. It is kept on repack.
  - 141 of 145 game archives repack byte-identically, including all arena, menu and crowd files. The 4 that differ are gm, casThumb, evtgdhd and evtgdsd; none is used by arenas.
- **PACH.** `"PACH"`, count, then `{id, offset, size}`. Offsets count from the end of the table; entries are 4-byte aligned.
- **BPE.** Philip Gage byte-pair blocks with a u16 LE length.
  - The writer's "stored" mode writes an identity pair table (skip 128, literal 128, skip 127) followed by raw data. Any decoder expands it, and it is instant.

### Textures

- **Bundle (one PACH entry per arena, e.g. 0x190).** Header: u32 count, 0x100, 0, 0x10.
  - 32-byte records `{name[16], "dds", size, offset}`; data on 16-byte boundaries.
  - All 97 bundles in the arena files round-trip byte-identically.
- **Pixels.** Standard PC DDS (DXT1/3/5), *not* Xenos-tiled. Pillow opens every one of them.

### Models: JBOY (tools/jboy.py)

- **Container.** `"JBOY" len | data | "POF0" len | pointer-offset list`.
  - Pointers are offsets from byte 8.
  - POF0 codes each gap/4 with a 2-bit size tag: 01 = 6 bits, 10 = 14, 11 = 30.
- **Header (0x48 bytes).** Mesh count/ptr, node count, texture count, node/texture/group ptrs.
- **Mesh (0xB4 bytes).**
  - Strip count, palette (up to 20 bones), weight-block count.
  - Pointers to vertices (28 B: pos, normal, ARGB), weights (8 B: u8 bone[4], f32) and UVs (8 B).
  - Material index, shader name (yDefault, yBumpMap, yUVScroll, yReflect), vertex format.
  - Named params (f32x4, f32, int, texture slot, bool), index headers (triangle strips, prim 6), bounding sphere.
- **Node (80 bytes).** Name, translation, rotation, parent, sphere.
- **Group (32 bytes).** Name, (1, 0), (mesh count, 0).
- **Coverage.** All 8633 JBOY models in the 35 arena files parse with every byte accounted for (except padding).
  - The writer reproduces 6610 byte-identically; the other 2023 differ only in junk padding, and their content is identical.
- **Units.** 1 unit = 10 cm (the ring mat is +/-31.9).
- **Axes.** Game **Y points down**: the floor is y~0, the mat y -10..-12, the posts reach -25. The export turns the scene 180 degrees about X (no mirroring); import turns it back.
- **Half arenas.** Arenas are only modelled on the side the hard camera faces. The stands, roof and trusses exist for x <= 0 only. The visibility tree (entry 0xC364, a 3ds Max node list with bounds) confirms there is no mirroring.
- **Crowd people** (nested PACH 0x4E20, e.g. `800_body`) are skinned to 17 bones and have 2 weight blocks.

### The ring (bg00 ids 0x3B7-0x3CE)

| Part | Model |
|---|---|
| Posts | `ar_post` |
| Turnbuckle | `ar_tb1` |
| Mat | `ar_ring`, +/-31.9, y -12..-10 |
| Ropes | `ar_rope10..13`, one per side, 25-bone chains, +/-28.8 long |
| Rope shadows | `_shd` versions, +/-35.3 |
| Corner pads | `ar_cover10..13` |
| Apron | `bs_ar_epron` / `bs_ar_epron2`, yBumpMap, 45 bones |

- **One rope model per side, drawn three times.** Rope count and heights are set by code. This matters for R2 (0-4 ropes).

### Collision (HMD)

- `"HMD "`, LE u32 count, then 96-byte LE records.
- Each record is a 4x4 frame, a 2D outline and flags `ffffff00`.
- bg00 0x3E4 holds 130 pieces.
- Entries 0x3E3/0x3E5 (`00 00 11 00`, 2124 B) look like a u16 index table for it. Not decoded yet.

### Code

- **File table.** Arena file names are fixed strings `GAME:\PAC\BG\BGnn.PAC` in a file table at 0x82DAC7C0.
  - The table is registered at boot through `sub_826A7328(obj, mode, table, count)`: 2 entries in mode 4, then 145 in mode 8 (MPSP.PAC .. EDIT.PAC).
  - In that table, BG00-BG18 are entries 29-47, followed by BG19, 20, 22, 23, 25, 26, 65, 66, 67 and BGETC.
  - Plan for new slots: point a host arena's entry at a mod file for one match. Still to confirm at runtime: when the file is opened, and whether anything is cached at boot.
- **Cache paths.** The game also lists `CACHE:GAME:\PAC\BG` and similar. No PAC has been seen cached in any UserData so far.

## Phase 0 status

- [x] EPAC/PACH/BPE read and write; byte-identical repack.
- [x] Texture bundle read and write; DDS untiled.
- [x] JBOY read, validate and write on all arena models.
- [x] In-game load of a hand-edited arena: bg17 (WWE Superstars) with a checker apron and 11 rewritten barrier models plays a match (`tools/arena_lab_mod.py`, `tools/arena_load_test.ps1`).
  - **Rule: a re-packed entry must stay smaller than unpacked.** Entries are loaded in place. A stored-BPE entry 36 bytes larger than unpacked crashed the load with a guest write fault at a page boundary.
  - Changed entries use the C++ compressor (`svrmod bpe`; ours is about 6% larger than Yuke's).
  - BPE blocks must unpack to at most 4000 bytes.
- [x] When arena files are opened: at arena load, not held (see above).
- [x] Redirect: a VFS symlink sends `PAC\BG` to `Mods\ArenaOverlay`, a folder of hard links to the originals (copy fallback). The chosen tile's entry is swapped for the mod's `arena.pac` just before the load (`src/arena_mods.cpp`).
- [x] **Blender round trip in game.**
  - Steps: `svrmod export` bg17, then `tools/blender_edit_test.py` (headless Blender: a new 2 m cube with a new texture, ropes repainted red/black, exported with Blender's own FBX exporter), then `svrmod import`. The match plays with the new cube, its texture and the red ropes.
  - Import keeps untouched models byte for byte. They are matched by svr_id; a model is "the same" when its triangles match to 0.05 units and its UVs to 1/512.
- **Memory budget.** Each arena must stay within its shipped size. A file 6 KB larger crashed (guest write at 0x70960000); 94 KB larger with a new texture hung at NOW LOADING; the same size or smaller loads.
  - `Arena::FitBudget` halves the largest textures the modder didn't supply until every texture set is back within its shipped size.
  - Raising the budget is a later limit-lifting job. The decoder is `sub_826AEF10(src, dst)`, called through `sub_826A1318`; the test aid `SVR2011_TEST_BPE_LOG=1` logs every decode.
- [x] Runtime: arena select pages (as the Paint Tool's).
  - The grid widget (vtable 0x8201D200; update `sub_823D4FC0`) moving past its left or right edge turns the page.
  - Page 1 is the 20 shipped arenas. Each later page holds up to 20 custom arenas, each drawn on its host tile.
  - The banners (DXT5 256x128, untiled with a 16-bit swap) are found in 4K-view physical memory by content when the screen opens, then overwritten per page.
  - An ImGui label shows "< n / N >" and the name of the custom arena under the cursor.
- [x] Ring code: rope instances and heights, rope rebound and break, corners and sides (see "Phases 4-6" below).
- [ ] HMD decoded enough to rebuild barrier and floor collision.

## Milestone A: a custom arena in game

- **Mods tab** (launcher, `launcher/mods_tab.c`): the list, + (install a `.svrmod`), - (remove), on/off, and Open Mod Maker. `--mods-add <game> <file>` does the + for tests.
- **Mod layout**:
  - `Mods/Arenas/<id>/` holds `manifest.txt` (type, id, name, author, version), `arena.pac` and `banner.dds`.
  - A `disabled` file turns the mod off.
  - A `.svrmod` is a store-method zip of these files.
- **SvR2011 Mod Maker.exe** (`modmaker/mod_maker.cpp`, Dear ImGui on D3D11):
  - an arena grid with the real banners;
  - Export to Blender and Import from Blender;
  - name, author, version and banner picture;
  - Save as mod and Install into game.
  - `svrmod makemod` does the same steps without the window.
- **End-to-end test** (`tools/arena_page_test.ps1`):
  1. bg17 is edited in Blender.
  2. `svrmod makemod` builds the mod.
  3. The launcher's `--mods-add` installs it.
  4. Arena select page 2 shows the mod's banner and name.
  5. The match plays the mod: a new cube, its texture, and red ropes.
- Later work: see "Phases 4-6" below.

## Phases 4-6

### Ring code

- **The ring constants** are in one block at 0x82D9D700:
  - +0x04 = 64;
  - +0x08 = 31.3 (edge);
  - +0x0C = -12 (mat);
  - +0x1C = 4.2 (rope gap);
  - +0x80 = the four corner posts (±31.7).

  Start-up code (0x82D35DD0-0x82D36100) derives globals from it:
  - 0x82E354E0 = 28.8, the inside-the-ring line;
  - 0x82E354E4 = bottom rope height (-12 - 3.4).
- **Ring object** at 0x82E354CC (built by sub_82187A78):
  - +8..+20: rope models;
  - +184: rope model count;
  - +104..+116: corner pads;
  - +120: turnbuckle.

  When the arena file has entries 900-911, the game loads **one model per rope** (900 + side + 4 × rope). No shipped arena uses this mode.
- **Draws:**
  - sub_8219A508: ropes;
  - sub_82199550: turnbuckles and pads, three per corner at the rope heights.
- **Rope physics object** at 0x82E354C8 (sub_82198C58): four sides × 24 nodes per rope.
- **Fighters:**
  - the list is at 0x82E3CC50;
  - +212 = motion;
  - +288 = position.

  sub_822EE2E0(control) runs the 116 control handlers at 0x82008790 until one takes the frame. The run handler (entry 7) means a hook on a later handler misses running fighters.

### Ring Kit

`modmaker/svrfmt/ring_kit`, game side in `src/ring_rules.cpp`.

- **R1, looks:**
  - rope colour per rope (material colours);
  - a picture per rope;
  - turnbuckles and corner pads on or off.

  Uses the 12-model mode. Tested in game.
- **Rope height and gap:** the manifest's `ring.rope_base` / `ring.rope_gap` set 0x82E354E4 and 0x82D9D71C while the arena is picked. Every rope user follows (draws, physics, moves). Tested at 2.0 / 5.0.
- **R2, no ropes.** With all three left out:
  - runners and whipped wrestlers brake (motion 53) instead of rebounding: hooks sub_823091A0, sub_82309398, sub_823054F0;
  - there are no rope breaks: hook sub_821BD388.

  Tested: no rebound, five brakes, the wrestlers stand back up. Details in `docs/ARENA_ROPE_GAMEPLAY.md`.
- **R2, one or two ropes:** those ropes are not drawn, but gameplay still uses all three heights (the Mod Maker says so).
- **More ropes (4): not done.** The rope physics, draw loops and moves are built for three.
- **R3, ring size: no-go.**
  - The central block scales, but the ring / apron / floor tests (sub_8218EB88 and the zone tests) also use literal 30, 32 and 55.3.
  - Those values come from constant pools that hundreds of functions share.
  - A bigger ring would sink wrestlers through the mat at its edge.
- **R4, six sides: no-go** for real gameplay. About 400 functions assume a square: 177 use the fighter's side byte +440 as 0..3, there are about 76 inline square tests, and the rope physics is built for four sides. See `docs/ARENA_SIX_SIDES_REPORT.md`.

### Arena Editor (Phase 5)

`modmaker/editor.cpp`, "Open in Arena Editor".

- **3D view:** the arena's DXT textures; glows drawn additive.
- **Object list** grouped by zone. Pick an object in the view.
- **Editing:**
  - move along an axis or on the floor, turn, scale (snap, undo);
  - duplicate, hide, delete added objects;
  - change the texture, or use a new picture.
- **Add:**
  - a box;
  - OBJ / FBX objects (added to the floor model).
- **Ring Kit panel**, previewed where the game draws the parts.
- **Lighting presets** (material colours).
- **Size budget** (file and memory against the shipped arena).
- **Test in game.**
- **Limits:**
  - Ringside and entrance parts move at most 50 cm.
  - Rigged parts can't move.
  - Objects added in an earlier session become part of the floor model when the mod is reopened.
- **Open a mod:**
  - the shipped ring parts are put back, and the per-rope models removed;
  - the lighting is divided out of the materials.

  Saving then applies the kit and lighting once. Tested: open + save gives the same models and colours.
- **Test aids:**
  - `--editor <tile>`, `--editor-view`, `--select`, `--open`;
  - `--test-edit-save <file>` (with `--editor`: move, add a box, red top rope, warmer light; with `--open`: save again);
  - `tools/modmaker_shot.ps1 -Extra`.

### Also done

- A custom arena plays in place of the arena it was made from (manifest `base=`).
- **Android launcher Mods tab** (`ModsPage.java`): list, on/off, Remove, Add a `.svrmod`. Compiled, not yet run on a phone.
- The APK package and Install to phone leave out `Mods/ArenaOverlay`.
- **Tools:**
  - `tools/ppc_xref.py`: cross-references in the generated code;
  - `tools/ring_rope_test.ps1`: exhibition rope test; `-AutoRun` uses `SVR2011_TEST_RUN`;
  - `SVR2011_TEST_STATE_LOG`, `SVR2011_TEST_RING`.

### Not done

- **VS screen:** each arena has its own VS-screen set (menu/MatchHD.pac M00I..M66I). A custom arena shows its base arena's.
- **Crowd:** not editable (nested PACH 0x4E20).
- **Collision (HMD):** not rebuilt, so moved ringside parts keep their old collision. That is the reason for the 50 cm limit.

### Shared files touched on this branch (for the combine)

- `CMakeLists.txt`:
  - arena_mods.cpp, ring_rules.cpp, svrfmt pac/texture in the port;
  - mods_tab.c in the launcher;
  - the modmaker target (mod_maker, editor, svrfmt, ufbx).
- `src/svr2011_app.cpp`: InstallArenaMods, InstallArenaModsOverlay, InstallRingRules.
- `src/script_input.cpp`: the stick command can hold buttons and both triggers.
- `launcher/svr2011_launcher.c`: Mods tab, `--mods-add`.
- `launcher/apk_package.c`: skips Mods/ArenaOverlay.
- **Android:** LauncherActivity.java (Mods tab), ModsPage.java.
- **New hooks:** sub_8219A508, sub_822EE2E0, sub_823091A0, sub_82309398, sub_823054F0, sub_821BD388, sub_823D4FC0, sub_826AEF10.
- **Test game:** runs/opt_arena_game. The `opt_` prefix keeps the main checkout's Stop-TestGames from closing it.

## New arenas and custom VS screens

- **Start an empty arena** (Mod Maker, Arenas page): the selected arena's file with everything but the ring, the floor and the ringside parts emptied. Unused textures shrink to 4 x 4 (`svrfmt/arena_build` MakeEmpty), which frees about 2 MB on Superstars. The crowd is off by default (`crowd=0`, HideCrowd empties the people in nested pack 0x4E20).
- **Library** (Arena Editor): any model from any of the 20 arenas, in its place or at the view centre. It is copied as static meshes on the floor model, with its textures; a texture is renamed (`lbNNN_`) when a different one already uses the name. Tested: an empty Superstars with WrestleMania's ramp, played in game with no crowd.
- **Slots:** each custom arena sits on the tile of the arena it was made from (`base=`), on pages 2, 3 and so on, so it always loads in that arena's place, with its memory room and VS theme. Arenas without a base fill the free tiles.
- **Custom VS screen** (Mod Maker, VS screen page):
  - The VS theme is menu/MatchHD.pac group M<nn>I: DXT5 textures, untiled in memory with 16-bit words swapped.
  - A mod can replace any of them (`vs/<name>.dds`, same size). While that arena is chosen, a background thread in arena_mods.cpp (VsLoop) finds the theme textures by content and writes the pictures over them; the originals go back when another arena is chosen.
  - Tested: Superstars' mh_super (the background) replaced in game. plate_ss holds the name plate.
- **Arena memory:** the game's 54 heaps come from a (heap, KB) table at 0x82DAC2F8.
  - Heap 16 (20 MB) holds the arena file, beside menu and stage data.
  - A file bigger than shipped fails to allocate there, and the load retries forever.
  - Growing heap 16 by taking room from heap 53 (`SVR2011_ARENA_HEAP_EXTRA_MB`, off by default) still failed loads, so custom arenas keep their slot's size. An empty arena frees that room for new content.
- **Test aids:**
  - `--new-arena <tile>`, `--test-lib <tile>`, `--page <n>`;
  - `SVR2011_TEST_HEAP_LOG`;
  - `svrmod grow`;
  - `arena_page_test.ps1 -Right <n>`;
  - `arena_find_probe.ps1 -NoSelect`.
