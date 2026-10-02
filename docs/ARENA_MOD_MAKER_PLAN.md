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
  - The SDK source and recompiler tools are read from the main checkout.
  - `port/generated` is a copy, not a link.
- **Test game:** `port/runs/arena_game`, made by `tools/arena_game_setup.ps1`.
  - Read-only junctions into the main install.
  - Real copies of `pac/bg`, `pac/menu` and the top-level `pac/*.pac`, so tests can change them.
- **Test saves:** `port/runs/test_userdata_arena`. **Config:** `port/runs/test_config.toml`.
- **Sessions:** `tools/arena_session.ps1` and `tools/arena_nav.ps1` stop only the arena game, and refuse to start while a player's game runs.

### Shared files touched (for the combine)

None yet.

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
- **Units.** 1 unit = 10 cm (the ring mat is +/-31.9). Y is up.
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
- [~] In-game load of a hand-edited arena.
  - `tools/arena_lab_mod.py` writes it: checker apron, barriers +15 units.
  - Test: `tools/arena_load_test.ps1`. Waiting until no player game is running.
- [ ] Runtime: when arena files are opened (debug log), arena id in match settings, arena select tiles and cursor.
- [ ] Ring code: rope instances and heights, rope rebound and break, corners and sides (the six-sides report).
- [ ] HMD decoded enough to rebuild barrier and floor collision.
