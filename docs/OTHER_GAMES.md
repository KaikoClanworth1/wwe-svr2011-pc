# Other games: what can come to SvR 2011

One section per kind of asset. Each porting chat keeps its own section up to
date; the converters follow docs/MOD_MAKER_3_PLAN.md section 4
(`tools/<kind>_port.py games / list / convert`).

## Arenas

Converter: `port/tools/arena_port.py` (Arena Porting). It reads the disc
through `tools/discfs.py` (an ISO or an extracted folder). It writes a
normal arena mod: `<out>/<id>/` and `<out>/<id>.svrmod` with `manifest.txt`,
`arena.pac`, `banner.dds`, and where the game has the art, `load.dds` and
`vs/<name>.dds`. The manifest's `base=` decides the tile (on the custom arena
pages) and the VS theme. The game gives the arena file the stage name of the
slot it is played in (`arena_mods.cpp` RedirectArena), so any base slot works.

| Game | Arena files | State | How |
|---|---|---|---|
| SvR 2007 (360) | | research | not extracted yet |
| SvR 2008 (360) | `pac/bg/bgNN.pac`, EPAC | research | different layout (see below); unchanged it crashes the 2011 arena load |
| SvR 2009 (360) | | research | not extracted yet |
| **SvR 2010 (360)** | `pac/bg/bgNN.pac`, EPAC | **ready** | the same format as 2011: the file is used as it is |
| WWE '12 (360) | | research | not extracted yet |
| **WWE '13 (360)** | `pac/bg/bgNN.pac`, `0x0FF512ED` LZX container around an EPK8 | **ready (in the Mod Maker)** | `svrfmt/wwe13.cpp` (`svrmod wwe13`); see below |
| WWE 2K14 (360) | | research | not extracted yet |

### SvR 2010

- **Format:** the same EPAC / PACH / JBOY / texture sets as 2011. The entries
  are 2011's, except that there is no announce table set (0x192, so the table
  shows the game's default pictures) and a smaller crowd pack.
- **Tested:** 2010's RAW (bg01) plays unchanged in 2011 (crowd, ring, props),
  and so does the converted No Mercy (bg06) on the Hell in a Cell slot.
- **Arenas:**

  | bg | Show | Notes |
  |---|---|---|
  | 00 | SmackDown | |
  | 01 | RAW | |
  | 02 | ECW | |
  | 04 | SummerSlam | |
  | 05 | Unforgiven | |
  | 06 | No Mercy | |
  | 07 | Survivor Series | |
  | 08 | Armageddon | |
  | 09 | Royal Rumble | |
  | 10 | No Way Out | |
  | 11 | WrestleMania 25 | |
  | 12 | Backlash | |
  | 13 | Judgment Day | |
  | 14 | Night of Champions | |
  | 15 | The Great American Bash | |
  | 18 | Cyber Sunday | |
  | 24 | Extreme Rules | no banner or VS art on the disc |
  | 16, 20, 22, 25 | | story copies (RAW, No Way Out, Royal Rumble, Road to WrestleMania), left out |
  | 23 | | practice ring, left out |
  | 30 | | not in the arena list |

- **Pictures:**
  - The banner comes from 2010's arena select (MatchHD `MENU/MASI`, the
    `arena_*` textures). 2010's PPV banners are square, so they are shown
    whole over the arena's darkened VS picture.
  - The loading picture is the biggest picture of 2010's VS theme for that
    arena (`MENU/M<nn>I`).
  - That picture is also used for the VS screen when the base slot's 2011
    theme has a single 1024 x 512 background: SmackDown `bg_sd`, RAW
    `RAW01`, Tribute to the Troops `troops_tex05`, SummerSlam `BG`,
    WrestleMania `wm_bg01`. Otherwise the base theme stays.
- **Base slot:** the 2011 arena of the same show (SmackDown 0, RAW 1, ECW 2,
  SummerSlam 4, Survivor Series 7, Royal Rumble 9, Judgment Day 13, Night of
  Champions 14, Bash 15, Extreme Rules 16). Otherwise its own number.
  `--base` changes it.

### SvR 2008

- **Format:** the arena file is an EPAC with the stage group (STG/00NN), but
  laid out differently from 2010 / 2011. It has:
  - no main texture set 0x190;
  - no crowd pack (0x4E20/0x4E2A);
  - no visibility tree (0xC364), no announce table / card entries
    (0x192 / 0xEE48) and no 0x80E8 / 0x7922.

  The models are the same JBOY family: `tools/svr08_stage.py` already moves
  2008 stage models into a 2011 backstage room.
- **Tested:** RAW 2008 (bg01) unchanged crashes the 2011 arena load (in
  `sub_826D73D0`).
- **Next:** where 2008 keeps an arena's textures; then a converter like
  `wwe13.cpp`: the 2008 models and collision, with the missing entries
  (crowd, visibility, light zones) from a 2011 host.

### WWE '13

Done in the Mod Maker (`svrfmt/wwe13.cpp`). Notes:

- The memory notes in the arenas work, and the Mod Maker manual, have the
  details.
- A single model over about 430 KB unpacked crashes the arena load. Use the
  `drop-mesh=` option.
- The TitanTron videos are Bink 2, which can't be converted.
- Arenas without a 0x192 table set get a made one.

## Belts and weapons (D11 research, Arena Porting)

### Where 2011 keeps them

- **Title belts:** models in `pac/evt/evtobj.pac`, group SOBJ, one entry per
  object id. SOBJ/LIST names them (64-byte records: u16 id, name).
  - There are 12 titles: IC, U.S., WWE, Hardcore, Women's, WWE Tag Team,
    Cruiserweight, World Heavyweight, World Tag Team, ECW, Divas and Million
    Dollar.
  - Each comes in several forms: Stretched 2002-2010 / 2305 / 2308 / 2512,
    Folded 2012-2020 / 2306 / 2309 / 2513, Rolled 2022-2030 / 2307 / 2310 /
    2514, and the variants 3xxx / 4xxx / 5xxx.
  - The belt you pick up in a match is a weapon: `bgEtc.pac`
    STG/WPON/0x000E, 14 packs, each a folded belt JBOY (named by its SOBJ id)
    with two textures and a small XML. The textures are `k2002`-`k2009_01/02`,
    `cenab_01/02` (WWE spinner), `2050`, `2306`, `2309` and `mdb_01/02`.
  - The `belts` branch scales them (title_belt_scale, `title_belts.cpp`).
- **Weapons:** `pac/bg/bgEtc.pac`, group STG/WPON (and WPBS, the backstage set).
  - **0x9C40** is the weapon settings text, one line per model number: `{mdl,
    type, se_hit, se_drop, hp, weight 0-3, damage, up to 5 hit spheres (x, y,
    z, r)}`. The model is the WPON pack id x 10 (40 chair -> 0x4, 140 belt ->
    0xE). The type is the behaviour class, e.g. 3 chair, 6 ladder, 11 table,
    14 belt, 21 bat, 30 crutch. There are 58 models: barbed bat, gong,
    chair, tables, announce tables, shovel, ladder, belts, steps, signs,
    crutch, cane, umbrella, trash can, fire extinguisher, speaker, monitor,
    bottles, tube light, tire, TV, Triple H's sledgehammer, bat, cue, tire
    iron, camera, pillow.
  - **0xAFC8-0xAFCF / 0xC350-0xC354** are per-stage placement texts: model,
    position, rotation, start motion, place (ring / outside / crowd).
  - **0x9C41 / 0xA028 (WADD) / 0xAFD2** are binary tables, not decoded yet.
  - **Packs:** each WPON pack is a PACH holding a model JBOY (0x0), a texture
    set (0xA) and optional extras.
  - The Fixes chat's `SVR2011_TEST_ITEM_LOG` (`belts` branch) logs the stock
    under the ring and the CPU's weapon choices.

### Swap or add?

- **Swap: yes.** A belt or weapon is a model + texture set in a pac entry.
  - **Reskin (same model):** works today with **texture packs** (native
    renderer: `Texture Packs\<pack>\..._<hash16>.dds/png`, matched on the
    XXH3 of the top level). A belt has two textures per title.
  - **Model swap:** needs the game to serve changed copies of `evtobj.pac` /
    `bgEtc.pac`. The move-pack overlay (Mods/PacOverlay, `move_packs.cpp`)
    already does that for m.pac / misc.pac / mpsp.pac.
- **Add a title: no.** The 12 title slots are fixed in the game's code and
  saves; 2011 has no Create-a-Championship. Unused slots (Hardcore,
  Cruiserweight, Million Dollar) could be given a new look, but showing them
  as new titles in the menus is game research.
- **Add a weapon: maybe.** The settings are a text table parsed by the game,
  so a new model number with an existing type may work, but the stock under
  the ring and the spawn lists are not mapped yet. Research first.

### From other games

- **SvR 2010:** the same weapon table (the same 58 models and types) and
  `evtobj.pac` belts in the same JBOY format, so its belt designs (the older
  titles) can be model swaps once the game side above exists.
- **WWE '13:** belts and weapons are JBOY with WWE '13's 0xB8 mesh descriptor
  inside its LZX container, which `svrfmt/wwe13.cpp` already reads; units
  are cm (x 0.1).
- **SvR 2008 and the rest:** research.

### Proposed mod formats (to agree with ModMakerUIRework)

- **Phase 1, reskins (no game work):** the Mod Maker writes a texture pack,
  `Texture Packs\<name>\<owner>_<texture>_<hash16>.dds`, from the stock
  belt or weapon textures (`belt_port.py` / `weapon_port.py list` give the
  names, sizes and hash16 of each title's or weapon's textures).
- **Phase 2, model swaps (game side needed):**
  - `type=belt`, `title=<IC|US|WWE|HARDCORE|WOMENS|WWE_TAG|CRUISER|WHC|WORLD_TAG|ECW|DIVAS|MILLION>`,
    with `stretched.jboy`, `folded.jboy`, `rolled.jboy` and `textures.bin`
    (a texture set).
  - `type=weapon`, `model=<number>`, with `model.jboy`, `textures.bin` and
    optional stats keys (`hp=`, `weight=`, `damage=`, `spheres=`).
  - Both go through a pac overlay for `evtobj.pac` / `bgEtc.pac`.
