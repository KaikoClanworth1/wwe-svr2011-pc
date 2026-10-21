# PS2 superstar models

`port/tools/ps2_char.py` turns the wrestler models of Yuke's PS2 SmackDowns into SvR 2011 character pacs
through `svr11_build.py`. Superstar Porting's `ps2_port.py` / `superstar_port.py` call it and add names,
records, themes and movesets; the Mod Maker reaches it through the Superstars tab.

```
py ps2_char.py --list <disc iso or extract folder>
py ps2_char.py <disc> <id hex> <out dir> --template <2011 chNNN.pac> [--attire all|N|0,2,3] [--no-gear]
```

Output: `model.json` (the builder's neutral model), `tex_*.png`, `ch.pac`, `render.dds` /
`render_small.dds` (when the game has select pictures) and `report.json` (the builder's report plus
`game, id, sources[], scale, height_m, bones_folded, renders, fitted[]`).

## Games

| Game | Serial (PAL / NTSC-U) | Characters / attires | Notes |
|---|---|---|---|
| WWF SmackDown! Just Bring It | SLES_504.77 / SLUS_203.16 | 139 models | `CH.PAC` at the root; one model per id; one 1024 x 128 TIM2 strip each |
| WWE SmackDown! Shut Your Mouth | SLES_512.83 / SLUS_204.83 | 71 / 140 | ids character * 16 + n |
| WWE SmackDown! Here Comes the Pain | SLES_520.36 / SLUS_207.87 | 80 / 134 | ids XXYZ; select pictures |
| WWE SmackDown! vs. Raw (2004) | SLES_527.81 / SLUS_210.60 | 87 / 106 | select pictures |
| WWE SmackDown! vs. Raw 2006 | SLES_536.76 / SLUS_212.86 | 79 / 103 | parts BPE'd |
| WWE SmackDown vs. Raw 2007 (PS2) | SLES_544.89 / SLUS_214.27 | 80 / 86 | `PAC/CH/`, parts BPE'd |
| WWE SmackDown vs. Raw 2008 (PS2) | SLES_548.79 / SLUS_216.45 | 74 / 81 | |
| WWE SmackDown vs. Raw 2009 (PS2) | SLES_552.51 / SLUS_218.10 | 83 / 105 | named EMD groups ("6601") |
| WWE SmackDown vs. Raw 2010 (PS2) | SLES_555.45 / SLUS_219.01 | 66 / 91 | DPK8 archives |

NTSC-U discs are recognised by serial (all tools and the Mod Maker); the file layouts were only checked on
PAL discs, so a USA disc that fails adds a "USA disc" note to the convert report.

Not done: the PS2 SvR 2011 disc (the same game as ours) and WWE Crush Hour (below).

## Archives

- **DPAC** (`CH.PAC`, `CH2.PAC`, `CH3.PAC`): groups from 0x800, data from 0x4000. A numbered group is
  `u16 n | 0x8000, u16 start sector, n x {u16 id, u16 size / 0x100}`, entries 0x800-aligned; a named group
  is `u32 n, n x {name4, u16 sector, u16 size / 0x100}` (SvR 2009 names its models in ASCII hex: "6601";
  SYM doubles the counts).
- **One entry on its own** (an inner `PAC ` cut out of a CH.PAC, as players send them): `lone_pac.py`
  converts it when it holds the body (part 2, YOBJ, skeleton included) and its textures (part 9; JBI part 1),
  wrapped as a one-entry DPAC (character 1, attire 0; the mod is named after the file). The game goes by the
  parts (every PAL entry checked, 20 Oct): BPE'd 0/1/10 JBI; plain 10/11 SYM; plain 8 (RTX3) only HCTP; plain
  100/101 HCTP or SvR 2004 (asked); BPE'd 26/27 SvR 2006-2008 (asked); BPE'd 5/100 SvR 2009. No name, pictures
  or entrance gear (other entries). Without a body or textures the error says where the whole CH.PAC is.
- **DPK8** (PS2 SvR 2010): groups `{tag4, u32 n (0x4000: 12-byte entries)}`, entries
  `{name8, u16 sector, u16 size / 0x100}`; model names are a 4-digit prefix plus the XXYZ id ("00010201").
- Model ids **XXYZ**: XX the character, Y the attire, Z the part. What Z means differs by game (HCTP 0/1
  match / detailed body and 2 gear; 2004 1 body, 2 gear; 2006 1/2 bodies, 4 gear), so the biggest model
  of a character's attire is the body and models under 600 vertices are its gear.
- Each entry is a **PAC** (`"PAC ", u32 n, n x {u16 id, u24 offset, u24 size}`): 2 the YOBJ model, 6 / 7 /
  26 / 27 attached parts (pads), 8 one RTX3, 9 the texture bank (JBI: 1 one TIM2). 2006 / 2007 BPE each part.

## YOBJ models

Little-endian, pointers relative to byte 8. 0x18 mesh count, 0x1C node count, 0x20 texture count, 0x24 ->
meshes (0x40), 0x28 -> nodes (0x50: name[16], t, 1.0, r radians, ?, s32 parent), 0x2C -> texture names,
0x30 -> object name (unreliable: HCTP's Rock is "hogan").

- **Mesh**: +00 batch count, +04 submesh count, +08 -> batches, +0C -> submeshes, +18 -> positions,
  +1C -> weights.
- **Batch** (0x20): u32 vertex count, u32 influences (1-4), -> positions f32x4, -> normals f32x4, s32 x4
  bones (1-based node indices). Vertices run over the batches in order, in model space.
- **Weights**: per run of consecutive multi-bone batches, 16 bytes (12 zero, a VIF UNPACK V4_32 tag: count
  in bits 16-23) then f32x4 per vertex, weight k for the batch's bone k. One-bone batches have none.
- **Submesh** (0xD0): +0x0C specular level (f32, SvR 2004 on), +0x24 flags, +0x28 texture slot; at +0xC0
  the packet table: one triangle strip per packet of `{s, t, q, vertex index}{r, g, b, a}` f32x4 pairs.
  Vertex colours are white in every game checked (nothing baked).
- **Materials** (+0x24): SvR 2004 on: 0x05 solid, 0x115 / 0x117 blended (hair, laces, logos, the blood
  overlay). JBI / SYM / HCTP: 0x07 solid, 0x05 blended - but only in a model that also uses 0x07 (HCTP's Rock
  and Austin are all 0x05 and all solid).
- **Textures**: RTX3 banks (`u32 n, n x {name[16], "txc", u32 size, u32 offset}`; PSMT8 with a CSM1-swizzled
  CT32 CLUT, PSMT4, PSMCT32; alpha 0..0x80) and TIM2 (32 / 24-bit, 8 / 4-bit CLUT; format 1 has a
  128-byte-aligned header).

## Conversion

- **Skeleton**: Yuke's bone names, as 2011's; bones 2011 lacks (`*_d`, `Dummy*`, `*_w0N`, eyebrows, teeth,
  cloth bones) fold into their nearest ancestor 2011 has. Units and axes are 2011's (10 cm, -Y up, -Z front).
- **Pose**: the PS2 bind pose is a T-pose, 2011's an A-pose: every bone is turned (the smallest rotation,
  down the hierarchy) so its segment points as 2011's does; vertices follow their weighted bones and
  `joints` gives the builder the posed proportions.
- **Meshes**: one per PS2 texture (the builder packs them into one sheet; UVs past 0..1 get a wrapped copy).
  The blood overlay (textures `blood*`) is left out - drawn solid it is a black-and-red mask.
- **Kinds and alpha**: blended submeshes, and from SvR 2004 on solid ones whose triangles sit on see-through
  texels (the PS2 alpha-tests every mesh), become kind `hair` with their texture's alpha (2011's hair shader:
  opacity). Other textures' alpha is 2011's specular level: the material's +0x0C (SvR 2004 on) or 48; their
  see-through texels are filled from their neighbours.
- **Gear**: an attire's small models are worn in its `entrance_gear` entry.
- **Budget**: an attire the builder refuses (over 192 vertex pages) loses its gear version, else takes its
  lighter body (`fitted[]` in the report).
- **Renders**: HCTP / 2004 MENU.PAC "MENU" SPFA (128 bust) / SPFB (256 x 128), picture n = the character
  id's hex digits read as decimal; SYM MENU.PAC MNSP by the MNCM/CHAR select roster.

## Checked in game

One character per game (SYM, 2004, 2006, PS2 2007 / 2008 / 2009 / 2010) plus HCTP Rock, Triple H (4
attires), Lita, Victoria, Undertaker and SYM Molly, Jazz, Kane: they play full CPU matches, skinned to
2011's motions, textured, with hair and see-through parts. Known: the PS2 2007 Rock shows a white strip on
top of his head (the head texture really is white there and the head mesh maps it; how the PS2 hides it
is not known).

## WWE Crush Hour (parked)

A different engine (Pacific Coast Power & Light, RenderWare 3.4); notes for whoever picks it up:

- Full-body models are the select-screen ones: `BUNDLES\SHELL\BELT\A-J|K-Z\<NAME>_SH.BND` (e.g.
  `ROCK_SHE.BND`) -> `b_rock.dff`, `anim\rock_01.anm`, `texture.txd`. In-match the drivers are seated torsos
  inside the cars (`BUNDLES\VEHICLES`).
- `.BND`: `u32 count` (counts more than the real entries), entries `{path\0, u32 BE offset, u32 BE size}`
  until the paths stop; offsets from a data base found by matching a RenderWare chunk's size + 12 to its
  entry's size (0x200 in `ROCK_SHE.BND`).
- `b_rock.dff`: Clump -> FrameList (64 frames, HAnim 0x11E: 62 bones with ids, no frame names) and two
  plain geometries (not PS2-native): the belt (72 vertices) and the body (1416 vertices, 2018 triangles, one
  material `rock_256`), each with RpSkin 0x116 (62 bones, u8 x4 indices, f32 x4 weights, then 62 x 68
  bytes of inverse bind matrices).
- Needed: a structural bone matcher (the bones have no names), a PS2-native TXD decoder (GS-swizzled
  rasters) or textures dumped by the Crush Hour port (`PS2X_RASTER_DUMP`), and the inverse bind pose ->
  A-pose. The models are low-detail (about 1400 vertices, one 256 texture).

## Choosing attires, blended flags (21 Oct)

- `superstar_port.py attires --src <PS2 disc | one archive> [--game ps2_*] [--item K] --out D` draws every attire of a
  character (ps2_char `--attire-pictures`: a small software rasteriser, front view, about a second each) to
  `D/attire_<n>.png` and prints `{ok, game, id, attires: [{attire, model_id, picture, meshes, see_through, drawn_solid}]}`.
  `convert ... --attires 9,3,0` then takes up to four of those numbers, in that order (disc and single file alike).
- JBI / SYM / HCTP: a submesh flagged blended whose triangle middles are 90%+ on see-through texels (and has 8+
  triangles) is drawn solid - its texture alpha isn't opacity (the PS2's opaque path ignores it). Stock HCTP: 977 of
  1089 blended submeshes are under 10%; the five at 90-100% have 2-4 triangles. A modded model with the flag on its
  body showed holes, outline boots and no legs. The convert report's `sources[]` now lists per attire `mesh_kinds`,
  `see_through` (with the share) and `drawn_solid`.
