# CAW parts - research (step 1)

Status (2026-10-26): research only, nothing built. Goal: Create-a-Superstar items (masks, clothing, ...) made from an
OBJ/FBX and added by a mod, next to the game's own. Sources: `Game Files\pac\edit` (read-only), hidden CAW editor runs
with the debug build. The survey scripts are throw-away; the facts are below.

## Where parts live

- `pac\edit\edNN.pac`: one EPAC per part category, NN = the category number. Its PACH entries:
  - `Mnnn`: a part with a model. `Tnnn`: a texture-only part (face paint, tattoos, make-up: ed06-08, 12/13/15,
    37-43, 58-63).
  - Inside each entry (PACH child ids):
    - `10xx`: colour texture (colour-mapped TGA, see Textures). Some parts have two (`1025` + `1026`).
    - `11xx`: normal map (colour-mapped TGA, flat-normal palette). `14xx`: a small extra map on a few parts.
    - `20xx`: the JBOY model, xx = the slot type. `21xx`: the same mesh bound to the other skeleton
      (bone palette 37 instead of 79 on ed44, same vertices). `22xx`: per-vertex table
      (`ffffffff`, u32 count = vertex count, then count x 16 B `0, 0, index, cccccccc`; looks like a weld/seam map).
    - `23xx`: `;CAS` sway text (bone + spring values); `2233` = hair chain data.
- `pac\edit\edit.pac`, EPAC group `EDIT`:
  - `MENU`: PACH child 0 holds 116 children, one per category id: the editor's part lists (below).
  - `EXSS` (12 preset tables), `SSTL` (Superstar Threads, not parts), `SSCP` / `MOT` (editor poses), `IK`
    (15 x 1648 B), `TEX` (shared textures), `FACE`.
- The editor looks `/EDIT/MOT`, `MENU`, `EXSS`, `FACE`, `TEX` up through the resolver sub_826B87B0
  (seen with `SVR2011_TEST_VFS_TRACE`). The edNN pacs are read by index, not through the resolver.

## MENU records

Each category child: a 12-byte header, then `total` records of 60 bytes (little-endian).

Header: `u16 total`, `u16 male`, `u16 female` (or `total, total, 0` when both share it), then flags.

| Offset | Size | Meaning |
|---|---|---|
| +0 | 4 x u8 | **colour band ends** (palette index, ascending, 0 = unused): `ff` = one band 0-255; `7f ff` = 0-127 / 128-255; `3f 7f bf ff` = four bands |
| +4 | 4 x u8 | the same again (all 3082 records have +4 = +0) |
| +8 | 4 x u8 | small flags (layer / draw order; not decoded) |
| +12 | u16 | model index -> `Mnnn` (`ffff` = none) |
| +14 | u16 | texture index -> `Tnnn` |
| +16 | u32 | `ffffffff` |
| +20 | u8 | **colour slots** the editor offers; **0 = fixed colours** (the palette is shown as stored) |
| +21 | u8 | the same again (23 of 3082 records differ) |
| +23 | u8 | 1 / 2 (gender list the record belongs to) |
| +24..+31 | | per-category extras (e.g. `02 02`, `1e 1e`: default shine / pattern values, not decoded) |
| +32 | u16 | style group (records sharing a shape) |
| +40.. | | hair sway values (hair categories only) |

The record index is what a CAW save stores, so new parts must be **appended** (never inserted).

## Textures and tinting

Part textures are colour-mapped TGAs: 18-byte header, 256 x 32-bit BGRA palette, 8-bit indices.

- **Tintable part:** the palette is grey. The index image is split into the record's **bands** (MENU +0): each band
  is one colour region, and its grey ramp is the shading inside that region. The editor colour for a region
  multiplies that band's greys. Checked on ed16 / ed19 / ed31 / ed33: the bands match the used index ranges
  (e.g. ed31 record 4, `3f 7f bf ff`: 19.6 % / 52 % / 10.8 % / 17.5 % of the pixels, each band its own grey ramp).
- **Fixed-colour part:** MENU +20 = 0 and a real colour palette (e.g. all coloured parts in ed27, ed22 rec 8).
- Not the region source (all checked): mesh count, palette alpha, vertex colours (all `ffffffff`), `22xx`, `21xx`.
- At run time the editor builds its own textures (the dumps show unnamed DXT textures, `tex_1024x1024_dxt1_*`),
  so the tint is baked on the CPU, not done in the shader. The shader parameters stay 1.0 (`g_f4MatDifCol`).

What this means for a converter:
- A model's own texture -> quantise to 256 colours, MENU +20 = 0: shows as made, not recolourable.
- A recolourable part -> greyscale, 1-4 bands with band ends in MENU +0, MENU +20 = band count.

## Models

- JBOY, one or more meshes, shader `yBumpMap` / `yBumpMapSS` / `yReflect` / `yDefault`.
- **Parts without their own nodes** (`nodes 0`, e.g. ed05, ed09, ed18, ed25-28, ed31-35, ed44): the mesh bone
  palette points straight into the shared CAW skeleton (ed44: palette `[79]`; pants: `[25,24,1,15,14,13,4,3,2]`),
  so they can still bend with several bones. These are the easy kind to convert.
- **Parts with their own node tree** (35-115 nodes: ed04 faces, ed10 / ed16 / ed20 hair, ed14, ed19, ed24, ed45, ed48):
  they carry a face / hair rig. Converting these is the skinned-clothing step.
- Coordinates: y points down; head about -8.5, boots +5..9.6 (game units).

## Categories (model categories)

`menu` = MENU record count. Kinds from the part names inside, the shape, and the editor (see the tour note).

| ed | menu | nodes | Shape / names | Kind |
|---|---|---|---|---|
| 02 | 15 | 0 | upper body with arms | tops (body-fixed) |
| 04 | 63 | 113 | whole head, `eyelash`, `teeth`, `bloodface` | faces / heads |
| 05 | 80 | 0 | two small pieces at the eyes | eyes |
| 09 | 28 | 0 | mouth | teeth |
| 10 | 204 | 73 | head + neck, `kami_*` (hair), `Undertaker` | hair |
| 11 | 13 | 0 | upper body | tops |
| 14 | 32 | 42 | chin | facial hair |
| 16 | 136 | 37 | top of the head | hair / headwear |
| 18 | 84 | 0 | brow line | eyebrows |
| 19 | 34 | 113 | face front, face rig | (see tour) |
| 20 | 106 | 37 | head | hair |
| 21, 22, 24, 25 | 46-87 | 0 / 115 | torso | shirts / jackets |
| 26, 27, 28 | 30-66 | 0 | arms / wrists | arm accessories |
| 31, 32, 33 | 27-54 | 0 | hips / legs | trunks / pants / knee |
| 34, 35 | 26 / 96 | 0 | feet | boots |
| 44 | 118 | 0 | one small piece by each ear | earrings |
| 45 | 79 | 37 | neck / chest | necklaces / beards |
| 47 | 4 | 0 | small face pieces | piercings |
| 48 | 56 | 35 | hips | belts / tassels |
