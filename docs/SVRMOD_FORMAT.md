# The .svrmod format

A `.svrmod` is a zip (stored or deflated) of one mod folder. The launcher
unpacks it into `<game>\Mods\<Type>\<id>\`; the game reads the folders at
start. The Mod Maker reads and writes every type below; it also adds a
`project.txt` to the mods it saves so they reopen with their settings.

Every mod has a `manifest.txt` of `key=value` lines (UTF-8, one per line):

| key | all types |
|---|---|
| `type=` | `arena`, `backstage`, `superstar`, `moves`, `signs`, `media` (the launcher routes the folder by it) |
| `id=` | the folder name: letters, digits and `_` (a mod with the same id replaces the older one) |
| `name=` | shown in the launcher and, where it matters, in the game |
| `author=`, `version=` | shown in the launcher; a newer version replaces an installed one, an older one is kept |
| `made_with=` | who made the file: `Mod Maker <version>` for the Mod Maker's, `Port tools (...)` for the converters'; the launcher shows it, and "non-Mod Maker" when it is missing |

A file named `disabled` in the folder turns the mod off (the launcher's tick).

## type=arena (`Mods\Arenas\<id>`)

| | |
|---|---|
| `arena.pac` | the arena, an EPAC like `pac\bg\bgNN.pac` (same layout; must stay within the original's file and unpacked sizes) |
| `banner.dds` | the select page banner, 256 x 128 DXT5 (no mips) |
| `load.dds` | optional, the loading screen, 1024 x 512 DXT5 |
| `vs\<name>.dds` | optional, VS screen pictures replacing the base theme's texture `<name>` (same size, DXT5) |
| `base=arena_XX` | the banner name of the arena it plays in place of (`arena_SD`, `arena_RAW`, `arena_WM26`, `arena_slam`, `arena_SS`, `arena_HinC`, `arena_Judg`, `arena_BASH`, `arena_NoC`, `arena_BP`, `arena_RR`, `arena_BR`, `arena_Series`, `arena_TLC`, `arena_ttt`, `arena_EC`, `arena_back`, `arena_EXT`, `arena_ECW`, `arena_drui`) |
| `ring.ropes=1 1 1` | which ropes exist (bottom, middle, top); the game adapts rope moves and breaks |
| `ring.rope_base=`, `ring.rope_gap=` | the low rope's height and the gap, game units (1 = 10 cm; the game's 3.4 / 4.2) |
| `ring.tints=`, `ring.turnbuckles=`, `ring.pads=`, `light.color=`, `light.strength=`, `crowd=0` | the Mod Maker's own notes: already baked into arena.pac, read back when the mod reopens |

Game: `src/arena_mods.cpp` (select pages, redirect, banners, VS / loading
pictures), `src/ring_rules.cpp` (rope gameplay).

## type=backstage (`Mods\Backstage\<id>`)

| | |
|---|---|
| `arena.pac` | the whole `bg78.pac` with one room rebuilt |
| `area=<0-6>` | the room: 0 parking lot, 1 GM's office, 2 locker room A, 3 locker room B, 4 large locker room, 5 interview area, 6 catering |
| `row=<label>` | optional: an area of its own - a row in ONE ON ONE -> BACKSTAGE after the room's; the mod plays only there |
| `box=x,z,hx,hz` | optional (with `row`): the fight box, game units |
| `camera=<units>` | optional: caps the match camera's distance |
| `camera_height=<units>` | optional: the camera's eye at least this high |
| `gimmick=<4 chars>` + `gimmick.pac` | optional (with `row`): a pac holding only `GMGB/<name>` - the area's cars, props and hot spots, played instead of the room's |

One backstage mod plays at a time (the last enabled folder by name). Game:
`src/arena_mods.cpp`, `src/match_types.cpp`, `src/move_packs.cpp` (gimmick).

## type=superstar (`Mods\Superstars\<id>`)

| | |
|---|---|
| `ch.pac` | the model: an EPK8 like `pac\ch\chNNN.pac`; the `EMD` entries get the slot's id |
| `name=` (31 chars), `short=` | the names |
| `base=<100-321>` | the roster superstar underneath: stats, moves, entrance motions, pyro, select render. Must be one of the 91 with an `SSFA` render |
| `style=<text>` | the Mod Maker's style name; its presence makes the name call default to "The Superstar" |
| `ratings=a,b,c,d,e,f,g` | grapple, submission, speed, strikes, hardcore, charisma, durability (1-99) |
| `abilities=a,b,...` | up to 8 ability ids: 1 Dirty Pin, 7 Move Thief, 9 Hammer Throw, 11 Resiliency, 12 Durability, 14 Kip-Up, 19 Outside Dives, 20 Springboard Dives, 22 Leverage Pin, 23 Fired Up, 24 Ring Escape |
| `height=<scale>` | optional: the size against the base superstar's, from the feet, 0.80 - 1.25 with 2 decimals (1.05 = 5% taller; missing = the base's own). The game stretches the skeleton (record +28, 4.12 fixed point, 4096 = 1.0: the base's x scale, kept within 3300 - 4900, so about x0.81 - x1.20 for a 4096 base; at 4139 and up the game counts the wrestler as "big"), so grapples follow; about 0.90 - 1.15 plays best |
| `call=<0-83>` | the Created Superstar nickname the announcer and commentary use |
| `announcer=<NAME>` | letters and digits: a name the announcer's sound banks have clips of (e.g. `JEFFHARDY`); wins over `voice=` |
| `voice=voice.<ext>` | a recording of the name the announcer plays |
| `entrance=<number>` | the entrance motions and pyro: a superstar's id, or 535 (Jeff Hardy's SvR 2010 one) |
| `song=theme.<ext>` | the entrance theme (.mp3 .m4a .aac .wav .flac .wma .ogg); plays at full level, make it about -24 LUFS |
| `movie=movie.bik` | the entrance movie, 320 x 320 Bink |
| `attire2..4=attireN.pac`, `attireN_name=` | more attires: another model pac's first attire each |
| `render.dds`, `render_small.dds` | the select picture: 512 x 512 and the 256 x 256 bust, DXT5 |
| `sign1..4.dds` | the fans' crowd signs, 128 x 64 DXT1 with mips |
| `moves=moves.txt` | lines `0xOFF=<move id>`: the move at byte offset OFF (0..0x1BF, even) of the profile's move block, over the base's |
| `moves\pack.txt`, `moves\motions\*` | a move pack carried inside the mod (below) |

Game: `src/superstar_mods.cpp` (72 slots: `Mods\Superstars\slots.txt` keeps
each mod's id), `src/managers.cpp` (the M tile list).

## type=moves (`Mods\Moves\<id>`)

`pack.txt`, one item per line, and the motion files it names:

```
motion <m.pac|mpsp.pac> <TYPE/NAME/child/...> <id> <x> <y> <frames> <file>
waze <id> <16 bytes hex>        the move's category bits (which move-set slots take it)
wazename <id> <text>            the move's name
wazecopy <id> <from id>         category bits copied from another move
exh <group> <36 bytes hex>
evt <group> <16 bytes hex> <events hex>
mbd <group> <8 bytes hex>
```

A motion's key is (id, x = phase or variant, y = track: 0 attacker, 1
victim, others props and cameras). The game merges every enabled pack into
copies of `m.pac`, `misc.pac` and `mpsp.pac` in `Mods\PacOverlay` when it
starts. Game: `src/move_packs.cpp`; details docs/MOVE_PACKS.md.

## type=signs (`Mods\Signs\<id>`)

Any `*.dds` in the folder: 128 x 64 DXT1 with mips, as the game's own
(`audience.pac` AUDE/BORD). They join the general signs every match draws
from (ids 6001+). Game: `src/crowd_signs.cpp`.

## type=media (`Mods\Media\<id>`)

| key | |
|---|---|
| `video.<id>=<file>` | a superstar's entrance video (320 x 320 Bink) |
| `theme.<id>=<file>` | a superstar's entrance theme |
| `render.<id>=`, `bust.<id>=`, `icon.<id>=` | a superstar's 512 render, 256 bust, 64 face icon (DDS) |
| `arena.<nn>=bgNN.pac` | the arena's file with its screen flip-book (`<prefix>_anim00..09`) replaced |
| `menu_music=<file>` | the menu music |
| `sound.<event>=<file>` | any game sound by event name (without `Play_`) |

Game: `src/media_mods.cpp`.

## The game's containers (what the files above are made of)

- **EPAC** (`pac\*.pac`, `bg\`, `menu\`, `string.pac`): little-endian; header
  +4 = table bytes, +8 = data bytes; group table at 0x800 of 12-byte group
  headers `{type[4], u32 entries*3, u32 0}` followed by 12-byte entries
  `{name[4], u32 sector, u32 size/0x100}`; data at `0x4000 + sector*0x800`.
- **EPK8** (`pac\ch\chNNN.pac`): like EPAC with 16-byte entries `{name[8],
  u32 sector, u32 size/0x100}` and a u16 count.
- **PACH**: `"PACH", u32 n, n x {u32 id, u32 off, u32 size}`, data after the
  table, 4-byte aligned; found by binary search on the id (keep ids sorted).
- **BPE**: `"BPE "` Yuke byte-pair compression in 4000-byte blocks. Entries
  are decoded in place: packed data must be smaller than unpacked.
- **Texture bundle**: `{u32 count, 0x100, 0, 0x10}` then 32-byte records
  `{name[16], "dds\0", u32 size, u32 offset}` and plain DDS data.
- **JBOY**: Yuke's models (big-endian; nodes, meshes with strips, 1 unit =
  10 cm, -Y up).
- **YMBs / YMKs**: motion banks - a 256-byte table at 0x10, u32 count at
  0x110, 16-byte entries `{u8 y, u8 x, u16 id, u32 off, u32 frames, u32
  runtime}` sorted by key. YMBs (menu motions) are ADPCM channels the Mod
  Maker plays; YMKs (match moves) only the game plays.
- **String tables** (`string.pac` SDB, entries `NNLL`: LL language 00 EN, 02
  IT, 03 FR, 04 DE, 05 ES, 06 JA; NN 64 = all 14 tables): `{u32 0, u32
  count}` + 16-byte records `{str_off, str_len, id, 0}` sorted by id, UTF-8.
- **CHAR/DAT** (`chEtc.pac`): 260-byte roster records after a 4-byte header
  (ratings +0..6, id +32, names +34 / +102 / +170, signs +210, selectable
  +221, abilities +230, DLC +257); **CHAR/PRO**: 1056-byte profiles (moves
  +0..0x1BF, entrance block +0x1C0).
- **MOVS/WAZE** (`misc.pac`): 160-byte move records (category bits +0, name
  +0x10, id +0x90); **WAZA/DATA**: EXH / event / MBD tables by move id.
