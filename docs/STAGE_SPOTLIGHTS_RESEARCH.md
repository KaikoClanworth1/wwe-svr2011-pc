# Stage spotlights and floor projections: research notes

Read-only research (15 Oct). Sources: the recompiled code (`port/generated/default`, guest addresses
`sub_XXXXXXXX`), the captured shaders (`port/runs/d3dtrace/xsc`, constant tables;
`port/runs/shaders_native/hlsl`), and the game data (`Game Files/pac`). Nothing was run.

Marks: **[P]** proven by code or data, **[G]** guess. Earlier notes: the session scratch file
`LIGHTS_RE.md` (light bank 0x7530 format, C351 flags).

## 1. Short answer

- The game has **no per-pixel spotlights**. The spot and point light constants exist in the engine
  table, but no game shader uses them. [P for the 467 captured shaders; G for the full library]
- The coloured light pools are a **"spot map"**: every frame the game draws up to **10 soft sprites**
  (one per moving light channel y50-y59 of a light set) into a **512x512 render target seen from
  above**, covering **x and z from -300 to +300**. Arena models then sample this texture with a
  **planar top-down projection** (u = x/600 + 0.5, v = z/600 + 0.5) and **add** it to their colour. [P]
- Because the projection is straight down, vertical surfaces (stand fronts, truss legs) get stretched
  streaks. The C351 flag `x` (`xg`, `xe`) turns the spot map off for a model. [P]
- The pool positions and colours come from **light set 20** (arena 0x7530, matches) or from the
  **entrance's own light tracks** (global effect object, `x = 10*segment + 13`, y50-y59). [P]
- The pool picture is `lightmap00` from the arena (a soft white disc), or a gobo picked by event
  **op 137** from the arena's effect pack in `gm.pac` (`/GMST/<arena>`: star, disc, sunburst, dots). [P]
- `g_fm44TexProjLight` is **not** a gobo projector: it is the wrestler **shadow map** projection
  (with `g_fm44ViewLight`, `g_fm44ViewProjLight`, `g_fm44Trapezoidal`, `sampShadowMap`). [P]
- **Big Show's logo on the stage floor was not found in the spot map path.** The spot map only covers
  +-300 around the ring and its textures are generic gobos. See section 6. [P for the negative; source
  of the logo open]

## 2. Shader side

### 2.1 Constant table (sub_826D9308 = register table init)

`sub_826D9308` / `sub_826E84D0` only fill a name -> register table (`{name, u16 register, u16 count,
type}`), they do not compute values. Registers [P]:

| constant | register |
|---|---|
| g_f4DifLghCol / g_f4DifLghVec | c0-c7 / c8-c15 (4 directional lights) |
| g_f4SpotLghPos / Vec / Col, g_f3SpotLghPrm / Att | c16-18 / c19-21 / c22-24 / c25-27 / c28-30 (3 spots) |
| g_f4PointLghPos / Col, g_f3PointLghAtt | c31-36 / c37-42 / c43-48 (6 points) |
| g_fm44ViewProj / ViewProjLight / Trapezoidal / TexProjLight | c54 / c58 / c62 / c66 |
| g_fShadowRate | c159 |
| g_b4SpotLgh1, g_b4PointLgh1/2 | b4-b7, b8-b11, b12-b15 |

### 2.2 What the captured shaders use [P]

The 467 shaders captured boot -> menus -> ring (408 of them already exist at the title screen, so this
is nearly the whole library) carry constant tables with names (`port/runs/d3dtrace/xsc`).

- **No shader references** `g_f4SpotLgh*`, `g_f3SpotLgh*`, `g_b4SpotLgh1`, `g_f4PointLgh*`.
- `g_fm44TexProjLight` (42 vertex shaders) writes TEXCOORD6; the pixel shaders sample
  `sampShadowMap` (s12) with it -> shadow mapping, together with `g_fm44Trapezoidal` (2 shadow
  caster vertex shaders). Not a coloured projector.
- `sampLightMap` (sampler **s13**, about 70 pixel shader variants of the normal arena material set)
  is the spot map. The vertex shader writes `TEXCOORD0.zw = world.xz * (1/600) + 0.5`
  (constant 0x3ADA740E = 1/600, e.g. VS `456298598036D391`). Pixel shader, e.g. `320FD17144E0CADD`:
  `out.rgb = diffuse.rgb * color0.rgb + spot.rgb * 0.5 * color0.rgb` (some variants: factor 1).
  So the pool is **added**, scaled by the vertex lighting colour, **not** multiplied by the diffuse
  texture.
- `sampLightMap_VS` (vertex texture, 22 shaders) is a different, static map: u = x * 7.58e-4 + 0.5
  (+-660), v = z * 5.81e-4 + 0.5 (+-860), multiplied into the vertex colour. Guess: the arena's baked
  `aud_lightmap` (256x512, arena entry 400; bound by the crowd renderer `sub_8217DCE8`). [G]
- `sampLogo` / `sampLogo2` (s10/s11) use the mesh UV (TEXCOORD0.xy): attire / mat logos, not
  projections.

## 3. The spot map (CPU side)

### 3.1 Object "SPDS" [P]

- Constructor `sub_821E1368` (called from `sub_82189470`), link tag `'SPDS'` (0x53504453),
  vtable 0x82003EC0. Fields: +48 width = 512, +52 height = 512, +56 **extent = 300**,
  +60/+64 render target texture, +72 surface, +84 picture number N (name `lightmap0N`, 0 by default),
  +88 **enabled**.
- Enabled by `sub_82751288(1)` from the entrance setup (section 4.2) when the entrance's crowd light
  set has a y50 track, and by `sub_8243E088` (caller `sub_8243F568`, mode not identified);
  disabled by `sub_8243ECD0`.

### 3.2 Drawing it: sub_821E0B60 (vtable slot of SPDS) [P]

1. Skip when SPDS+88 == 0.
2. Light id: **21** when the game mode global `*(0x82E3CCF8) == 5` (falls back to **20** when the
   arena bank has no 21), else **20**. `sub_8257E6B8(mgr, index)` returns the evaluated light
   object of that id; it honours the entrance override (section 4.2).
3. Default picture: texture `lightmap0N` (N = SPDS+84) from the arena texture list (arena entry 400,
   via stage+15892). RAW: `lightmap00`, 64x64 soft white disc.
4. Render target cleared (black, [G]), 2D state.
5. For k = 0..9 (= channels y50..y59, light set slots 6..15):
   - skip when the slot colour is (0,0,0);
   - mode 5 only: if `mgr+128+4k` holds a cast index (0..8), the position is taken from that entrance
     cast member (`sub_82393778(index)` -> object +304): **the pool follows a wrestler**. Set by a
     cast object event method `sub_821D3EC8` (bytes `[?, k, on]`; vtable 0x82003958, slot 36; its
     YMKs op number was not resolved);
   - picture: the slot's own texture if op 137 set one (`sub_8257E4F0` reads `mgr+248+8k` texture,
     `mgr+168+4k` width, `mgr+208+4k` height), else `lightmap0N`;
   - sprite colour = (R,G,B) * 255 as ints, alpha 255;
   - sprite centre in pixels: `px = x / 300 * 256 + 256`, `py = z / 300 * 256 + 256`;
   - sprite rotation (z) = third component of the slot's rotated axis vector (player
     `(203+slot)*16` = key rotation matrix * (-1,0,0)) [P for the source, G for its meaning];
   - sprite size = op 137 width/height in pixels of the 512 map, or the picture's own size.
6. The map texture is bound to sampler 13 for the arena draw (`sub_8217D790`, link 'SPDS', stage 13).
   `sub_826E4120(renderer, on)` switches the spot-receiving shader variant.

Blend mode of the sprites: not read; additive is the natural guess (overlapping pools add up). [G]

### 3.3 Light set slots -> evaluated values [P]

Light set player (8608 bytes): per slot k, position = `(251+k)*16` (lerp of the 25-byte key positions,
x0.01), direction = `(203+k)*16`, colour in the 80-byte record at `4528+80k` (key RGB / 4096).
`sub_8257F148` copies them into the 928-byte 'LIGX' light object: slot 6+k (y50+k) -> position at
+336+16k, direction at +496+16k, colour at +656+16k. Key format = 0x7530 keys (pos float3 x0.01,
rot s16x3, colour u16x3 / 4096; colours above 1.0 occur).

## 4. Where the per-frame values come from

### 4.1 Matches: arena bank 0x7530, light set 20 [P]

Light set 20's y50-y55 (241 frames, moving coloured lights, the same in every game 2007-2K14) feed
the spot map in matches (when SPDS is enabled). Light ids 20/21 are also the crowd lights
(`sub_8217DCE8`).

### 4.2 Entrances: the global effect object [P]

Each entrance EVP has a "global" object (type `00000000 00010000`, Big Show 125: object 11, EVT id
25011). Its EVT tracks are light sets in the 0x7530 key format:

- **x = 10 * segment + 11 + i** (segment 0/5/10/15/20 -> x 11-19, 61-69, 111-119, 161-169,
  211-219), y = 0, 1, 10-13 (ambient/directional), 50-59 (moving lights).
- `sub_82776980` -> `sub_82776640` binds, per segment, entrance light set i (0..8) via
  `sub_8257EF78(mgr, i, id, x, idForY50, banks)` and points light ids at it
  (`sub_8257E428`, mgr+104 per id):

| i | x (segment 0) | light ids that use it |
|---|---|---|
| 0 | 11 | 10 |
| 1 | 12 | 100-199 and 200-299 (characters), or 100 / 108 |
| 2 | **13** | **20, 21 (crowd + spot map)**; if y50 exists at this x -> SPDS enabled |
| 3 | 14 | 30, 31 (ring object 'RING', `sub_8219D798`) |
| 4-8 | 15-19 | 14, 11, 12, 13, 15 |

- Census of all 239 stock entrances: 222 have the global light tracks; y50-y59 at x13 in 222, at
  x63 in 218, x113 in 22, x163 in 15, x213 in 9. Big Show x13 y52-y59: 8 blue pools
  (0.235, 0.235, 0.627) circling at radius 100-180 around the ring; y50 at (0, 0, 750) with
  colour 0 (segment 0) / grey 0.27 (segment 5) - outside the +-300 map.
- The x10 tracks (y1-y8, 218 entrances) are not read by `sub_82776640`; consumer unknown.

### 4.3 Event op 137 (0x89): the pool picture [P]

Bytes `89 k idx w h` (5 bytes, T[137] = 5). Handled by the light set player (`sub_823C7DD0`, inside
light tracks) and by the entrance object dispatcher (`sub_823306B0`). Calls
`sub_8257F668(mgr, k, texture, w, h)`:

- texture = entry `idx` (< 16) of texture container slot 1 (`*(0x82E3DBC8)` -> `sub_825052E0(.,1)`
  -> `sub_824B7F90`); idx >= 16 -> no texture (back to `lightmap0N`);
- w/h = sprite size in map pixels; 0 = the texture's size.

Container slot 1 is loaded by `sub_824B03F0(1, arena)` from **gm.pac `/GMST/%04d`** (number =
`*(sub_825740C8()+64)`, the arena number [G, from its compares with 3/23/29/66-80]). Every GMST pack
(0000-0022) holds the same 8 textures (PACH #4 -> #0): 0 star flare 256, 1 colour ramp 64 (alpha 0),
2/3/4 flat blue (alpha 0), **5 soft disc 32**, **6 sunburst 128**, **7 dot cluster 256**. The
index -> picture order assumes the container lists the PACH children in order [G].

Stock use (census): `(k=0, idx=5)` on y50 tracks 508x, `(0,1)` on character tracks 3904x,
`(7,5)`, `(6,5)`, entrance 145 `(1/8/9, 7, 40, 40)`, entrance 482 `(2..9, 7, 60-80)`.

Other ops seen on the way (not lighting): op 171 `ab a b c` -> `sub_82178008` and op 172 -> `sub_821780B8`
act on the object at `*(0x82E36898)` (loops 20 audience characters) -> crowd actions [G], not
lights. Op 136 (0x88) sets flags on the object at `*(0x82EC1A88)`; op 159 (0x9F) sets the 4 enable
flags of a light set (+8584..+8596).

## 5. The `x` flag [P]

C351 legend: `x` = model that does not receive spotlight (`xg` only in the game, `xe` only in
events). Parser `sub_82183390`: record +9 |= 0x80 (`xg`), 0x40 (`xe`), 0xC0 (`x`). Record =
stage + 1156 + 12 * model.

Model draw `sub_82186500`: if mode `*(0x82E3CCF8) == 1` (match) receive = !(rec+9 & 0x80), else
receive = !(rec+9 & 0x40); `sub_826E4120(renderer, receive)` selects the shader variant with or
without `sampLightMap`. RAW uses no `x` at all; every RAW model receives the map.

## 6. The entrance floor logo (open)

Not explained by the spot map:

- the map covers only x, z in +-300 (ring and ringside); the stage is further out (Big Show's y50 sits
  at z = 750);
- op 137 textures come only from the shared GMST gobos, nothing per superstar;
- no other code path writes the slot textures (`sub_8257F668` callers: `sub_823306B0`,
  `sub_823C7DD0`, `sub_82776840` only).

Candidates [G]:
1. a stage model showing the entrance titantron video (Big Show's video is red with his logo). The
   movie shaders (`sampMovieY/U/V/A`: PS `1FEDE9146D50D360`, `597EAFED152B279E`,
   `5B681851424CBF50`, `D1B1D8716906A446`) are UV-mapped; RAW stage models 212-214 (`n`, textures
   `light_00`, `light_00b`, `raw_ent0`) and 215/216 (`g`, `raw_titan`, `raw_titan_l`) are the
   places to check;
2. an effect from gm.pac `/GMEV/0125` (Big Show's entrance effect pack, GMPD + YMKs, no textures of
   its own).

Cheapest check: a native frame dump during Big Show's entrance (`SVR2011_NATIVE_DUMP_FRAME`) and
look at which pixel shader / texture draws the floor logo.

## 7. What the Mod Maker needs

Spot map (both cases):
1. Evaluate the light set: matches -> arena 0x7530 id 20, channels y50-y59; entrance preview ->
   global object tracks `x = 10*segment + 13`, y50-y59 (fallback: arena set 20). Same 25-byte key
   decode as today; also parse op 137 events in those tracks (and in character tracks) for
   `k, idx, w, h`.
2. Each frame: a 512x512 target, cleared black; for each slot with non-zero colour, draw the picture
   (op 137 gobo from gm.pac `/GMST/<arena>` PACH #4/#0 child `idx`, else arena texture `lightmap00`)
   at `(x/300*256+256, z/300*256+256)`, size w x h px (or the picture's size), tinted RGB (clamp at
   1), additive [G], rotated by the angle in 3.2 [G].
3. In the arena shader, for models without `x`/`xg` (match) or `x`/`xe` (entrance):
   `color += spot(x/600+0.5, z/600+0.5).rgb * 0.5 * vertexLight` (some game variants use factor 1).
   World x/z are the model's world coordinates in the same units as the light keys.
4. Optional: in entrance mode a slot can follow a cast member (event via `sub_821D3EC8`, not decoded).

Open questions: the YMKs op behind `sub_821D3EC8`; the sprite blend state; the meaning of the sprite
rotation value; who reads global object x10 y1-y8; what draws the stage floor logo (section 6).

## Addendum (15 Oct, Arena Porting): the floor logo is a movie surface

RAW's stage floor, ramp and under-stage screens (models 801-808) name the textures `titantron_movie` and
`arena_movie`, which are in no pac: the game fills them with a movie at run time. The superstar's
`movies\titantron\<id>.bik` is 320 x 320 with the titantron above and a name / logo strip below (Big Show:
`125.bik`) - the logo on the floor in his entrance is that movie on `arena_movie`, not a light. [G for the
half-to-surface mapping; P that the surfaces are movie names] The Mod Maker's 3D view draws them as a screen that
is off (86a4ad6 follow-up); putting the movie on them is the Full preview's titantron path (SetTextureOverride).
