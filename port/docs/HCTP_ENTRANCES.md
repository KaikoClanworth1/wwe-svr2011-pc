# HCTP entrances -> SvR 2011 (tools/hctp_entrance.py)

SmackDown! Here Comes the Pain (PS2, 2003, "HCTP") entrance motions converted into SvR 2011
entrances that a mod carries. First one: **Goldberg** (HCTP 3047 -> 2011 entrance 503).

- **Tool:** `tools/hctp_entrance.py` (+ `tools/hctp_motion.py`, the HCTP motion codec; both
  use `tools/ymks.py` and the EVP writer of `tools/svr08_entrance.py`).
- **Related:** docs/SVR08_ENTRANCES.md (the 2011 EVP/EVT format, the SvR 2008 converter this
  one shares its writer with), docs/MOVE_PACKS.md (`pacentry`), docs/SUPERSTAR_MODS.md
  (`entrance=`).
- **Data:** the HCTP disc extracted to `D:\Xbox Games Ports\HCTP Extract` (PAC\EVTENT.PAC,
  PAC\MOVES.PAC; ELF SLES_520.36 with a full C++ symbol table).

Tags: [C] checked on data or in game, [L] likely, [G] guess.

## 1. Quick use

    python tools/hctp_entrance.py list  "<HCTP>\PAC\EVTENT.PAC" "<HCTP>\PAC\MOVES.PAC"
    python tools/hctp_entrance.py convert "<HCTP>\PAC\EVTENT.PAC" 3047 503 out --name Goldberg --pack

- `list` prints every HCTP entrance: number, owner name (MOVES.PAC), object kinds, bank size.
  3000-3099 are the superstars' own entrances (3047 Goldberg, 3039 John Cena, 3060 Batista...);
  entries with "2 objects char 2" and a 512-byte bank are empty placeholders.
- `convert` writes into `out/`:
  - `503_evp.bin`, `503_evt.bin`: the 2011 script (EVP) and tracks (EVT, a YMKs bank);
  - `parts/10503..14503_evp/evt.bin`: the 5 Create-An-Entrance parts (like every shipped
    entrance has; `svr08_entrance.cae_parts`);
  - `report.txt`: what was converted, moved or dropped;
  - with `--pack`: `pack/` = a move pack (`pack.txt` + `entrance/*.bin`) with the
    `pacentry evt\Nyujyo5.pac EVPE|EVTE <n> <file>` lines.
- Options: `--name` (the EVP's 30-byte name), `--fov <rad>` (camera field of view, section 5),
  `--no-ground` (keep HCTP's raised stage heights, section 6), `--no-parts`.

**Install** (docs/MOVE_PACKS.md): put `pack/` in a superstar mod as `moves/` (with
`entrance=503` in its manifest), or on its own as `Mods/Moves/<name>/` and point any mod's
`entrance=` at the number. The game's own files are never written: the move-pack overlay
builds `Mods/PacOverlay/evt/nyujyo5.pac` with the entries in.

**Numbers:** a 2011 entrance number picks its pac by range (Yuke's layout; `nyujyo_for()`):
100-134 Nyujyo0 (EVP1/EVT1), 135-178 Nyujyo1 (2), 179-271 Nyujyo2 (3), 272-290 Nyujyo3 (4),
291-479 Nyujyo4 (5), 480-511 Nyujyo5 (E), 512-531 Nyujyo6 (F), 532-551 Nyujyo7 (G),
552-599 Nyujyo8 (H), 600-999 Nyujyo9 (I). It must be below 1000 (CAE parts are
10000 + 1000k + n). Free numbers in 2011 (no stock entry): Nyujyo5 488-499, 503 (504 is the
Sandman mod's), Nyujyo7 542, Nyujyo8 553/559/561, Nyujyo9 many (619, 623-627, ...). Goldberg
uses **503**.

## 2. HCTP entrance data [C]

All little-endian (PS2). `PAC\EVTENT.PAC` is a DPAC (like 2011's EPAC): groups from 0x800,
data from 0x4000 in 0x800 sectors (`dpac()` in the tool).

| group / entry | content |
|---|---|
| `EVET/<number>` | one CEKey motion bank per entrance, 3000-3533 (221 entries) |
| `ENTR/SEVP` | the scripts of all 534 entrances in one `POF0`/`VE` chunk |
| `ENTR/SYDT` | per-entrance records (section 7) |

Owner names: `PAC\MOVES.PAC` group `MOVE` entry `LIST` (an inner `PAC `), sub-entry 2 =
`u16 count`, then count x `{u16 number, char name[30]}` (the entrance list of the edit menus).

### 2.1 SEVP (script)

The same family as 2008/2011's EVP, older and smaller. The chunk tags read reversed
(`POF0`, `VE\0\0` = 2011's `0FOP`, `EV\0\0` in little-endian), pointers are relative to offset 8,
a POF0 table follows the chunk.

| off | content |
|---|---|
| 0x10 | u32 count (534) |
| 0x14 | ptr -> table: count x `{u16 number, u16 objects, u32 0, ptr objects}` |
| 0x18 | u32 count (534) |
| 0x1C | ptr -> tails: count x `{u16 number, u16 records, ptr}` |

**Object** (28 bytes, = 2008's layout in little-endian): `{u16 idx, u16 model, u8 type[8],
u32 -1, u32 0, u16 slots (5), u16 0, ptr slots}`. The 8 type bytes are in the same order as
2011's (read them as two big-endian u32): `01 00 00 00 ..` character, `00 01 00 00 ..` camera,
`00 00 00 01 ..` rope (section 8), `00 00 01 00 ..` prop.

**Slot** (40 bytes; 2011's is 212 with 48 params):

| off | content |
|---|---|
| 0 | u16 slot (0-4 = the 5 segments), u8 on, u8 0 |
| 4 | u32 track id (3047: 9950 for every object) |
| 8 | u32 track x (3047: the object's index) |
| 12 | u32 track y (3047: the segment) |
| 16 | f32 x, f32 y, f32 z, f32 yaw, f32 ? (= 2011 params 1..5) |
| 36 | u16 parent object (9999 = none), u16 0 |

**Tail** (one per entrance): records of 16 bytes `{u16 type, u16 0, u16 frame, u16 100,
u16 ?, u16 items, ptr items}`, items `{u8 kind, u24 0, ptr value}` (kind 2 = u32, 3 = u16), every
value 0 in the data (filled from the profile at run time) [L]:

| type | meaning [L] | 3047 |
|---|---|---|
| 1 | entrance music | frame 0 |
| 7 | ? (no items) | frame 0 |
| 2 | ?, 3 u32 (not pyro, section 7) | frame 480 of segment 0 |
| 8 | titantron movie | frame 1 |
| 9 | ? (3000 only: frame 30, 1000) | - |

2011's tail has only types 1 (music 999) and 8 (movie 999); the tool writes that standard tail.

### 2.2 EVET (tracks)

A CEKey bank: `T[256]` (event sizes), `u32 n`, n x `{u32 key, u32 offset, u32 frames, u32 0}`,
data (docs: scratchpad hctp/mot/re_motion.md; `hctp_motion.read_bank`). Key = `id << 16 |
x << 8 | y`, the (id, x, y) a slot names. 3047:

| key | object | frames per segment 0..4 | content |
|---|---|---|---|
| (9950, 0, 0..4) | 0 character | 557 114 154 123 218 | body poses |
| (26334, 1, 0..4) | 1 camera | same | 13-byte camera keys |
| (9950, 2, 4) | 2 rope | 68 | 5-byte scalar keys |
| (9950, 3, 0..4) | 3 prop (title belt) | 1 | one object pose + op 182 |

- **Camera tracks carry the track id | 0x4000** (9950 -> 26334) [C].
- **Entrance banks use HCTP's wide bone table** (`H_TYPES_WIDE`): kubi/te/ashi y,z and
  tsumasaki y are full s16, a full key is 99 bytes (0x60+count, s16 root x3, 92 bytes of bones).
  The game's own move banks (MOT) use the 8-bit form (87-byte keys). [C: every 3047 record
  decodes with no trailing bytes only with the wide table]
- Frame rate, Euler order, angle unit, root unit (0.01), bone list and the motion bind are the
  same as 2011's: **a character track converts with no retargeting** (re_motion.md section 0).

## 3. The 5 segments [C]

Both games play 5 segments in the same coordinates (ring centre at 0, y down, the stage
towards +z around z 300), but **2011's segment 0 is the hidden intro** (the titantron plays,
the entrant is not drawn: 506 keeps him at y +50 for 181 frames). A first test that mapped
HCTP slot k to 2011 slot 5k one to one played everything except the stage part, which the
game hid. HCTP's segment 0 is "hidden intro + stage" in one track, so the tool cuts it:

| 2011 segment (slot) | role | 3047 Goldberg source |
|---|---|---|
| 0 | intro, entrant hidden | HCTP 0 frames 0-161 (crowd/arena cameras; he is below the stage, y +22.4 until f147) |
| 1 (5) | stage | HCTP 0 frames 162-556 (out on the stage z 294, flashes f402-464, walks to z 247) |
| 2 (10) | ramp | HCTP 1 (z 186 -> 139) |
| 3 (15) | ringside, into the ring | HCTP 2 (around to the corner, onto the apron) + HCTP 3 (through the ropes) |
| 4 (20) | in-ring pose | HCTP 4 (the rope push, object 2) |

- The cut point (`intro_end`) is the first camera cut (a key with count 0) at or after the
  entrant's root rises above y +15 (3047: up at f147, cut at f162). No hidden start = no cut;
  the tool then maps k -> 5k.
- A cut inside a track starts with the state there: the character gets a full key of the pose
  reached (`char_state_at`), the camera its last key with count 0.
- Joined parts (2011 segment 3) are appended frame by frame; a one-frame prop pose is held.
- Segments may jump between each other (a camera cut hides it); 2011's own entrances do too.

## 4. Event ops [C]

Researched in both executables (docs/HCTP_ENTRANCE_EVENTS.md; HCTP `CAttribute::SetAttr`
0x1b35c0 for characters and props, `CEventMotionCamera::AnalyzeAttribute` 0x164fa0 for
cameras; 2011 `sub_8239D750` generic, other ops to vtable +92 = `sub_823306B0` for wrestler /
effect objects, nothing for camera and prop objects). Sizes are equal in both games.

| op | bytes | HCTP | 2011 | tool |
|---|---|---|---|---|
| 18 | `12 r g b a t` | screen fade/flash to colour, alpha 0-128, t ticks (x5/6 on PAL) | the same layout, `sub_823B4730` (alpha doubled, same PAL step) | kept on the character |
| 98 | `62` | ring shake level 2 | no-op (96-99) | kept |
| 102/103 | `66` `67` | crowd camera flash bulbs, level op-100 | the same, `sub_823A7D50` | kept |
| 112 | `70 z r` | camera zoom: target z/10, rate r/1000 per frame (0 = snap) | the same scaling, `sub_8239A8A0`; with it the key's fov is not used | kept |
| 113 | `71 c` | camera height follows cast c-1 (`71 01` = the entrant) | `sub_8239C008`; stock entrances write `71 00` | `71 c` -> `71 c-1` [L] |
| 182 | `b6` | owner's motion state 1 | 2011 props have `b6` at frame 0 | kept |
| 16 | `10 slot id16` | face (slots 0/3) or hand (1/2) animation id | no handler | dropped |
| 118 | `76 v` | crowd lighting | no handler | dropped |
| 192 | `c0 id` | sound effect (HCTP SFX number; 3047's flashes play 207) | ignored on purpose | dropped |

2011's op 193 (`c1 lo hi`, `sub_825E8E40`) looks like a sound/voice cue dispatcher [L]; an HCTP
sound would need a 2011 id picked by hand. Ops 16, 112, 118 and 192 never appear in a shipped
2011 entrance.

## 5. Cameras [C]

- HCTP camera key = 2011's camera mode 0: `[count][s16 pos x3, 0.01 units][s16 rot x3]`.
  2011 entrances use mode 1 (29 bytes: `[count][f32 pos x3][f32 rot x3, rad][f32 fov]`); the tool
  writes mode 1.
- **Both games use R = Rz*Ry*Rx and look down the camera's local -x** (checked by aiming every
  key at the character's root: median 7 deg (HCTP) / 11 deg (2011) off, every other convention
  > 19 deg; scratchpad gold/camconv.py). Angles copy unchanged.
- HCTP keys have no field of view; HCTP sets it with op 112 (zoom z): horizontal fov =
  2*atan(0.625/z) (64 deg at zoom 1, PAL 4:3). 2011 handles op 112 the same way
  (`sub_8239C5C0`: fov = 2*atan(0.625/z) x 0.75, or x 0.82 with a global flag, likely
  widescreen [L]) and then ignores the key's fov. Every 3047 camera track starts with op 112
  (zoom 1-4), so the tool keeps the ops and writes `--fov` (default 0.7217) into the keys only
  as a fallback. Key fov for a given zoom, if ever baked: 1 -> 1.016/1.105, 2 -> 0.563/0.614,
  3 -> 0.383/0.419, 4 -> 0.290/0.317.
- 2011 cameras may have a second track y1 (133 of 229 entrances); 96 have y0 only. The tool
  writes y0.

## 6. Stage and ramp heights [C]

HCTP's stage is level with the ring (the character's root y -9.3 = standing on y 0) and a ramp
falls to the arena floor (y +11.8, root +2.5) between z 237 and z 87. 2011's own entrances
stand on a floor-level stage and a flat ramp (506: root y +2.5 from z 300 to the ring), and the
2011 arenas are built that way. Played as is, the wrestler would walk 1.2 m above the 2011
stage. The tool moves every character root on the stage/ramp down by `11.8 - ground(z)`
(`ground` = 0 at z >= 237, 11.8 at z <= 87, linear between); a camera key moves by its
segment's character shift at the same frame (so the framing stays); positioned objects without
a parent likewise. Ring-side segments (z < 87) are unchanged. `--no-ground` turns it off.

## 7. SYDT and pyro [C]

- **SYDT** (count 534, first id 3000) is a compact copy of SEVP (the same objects, slots,
  positions, parents). The executable names only `/ENTR/SEVP` and `/EVET/%04d`, so SYDT is
  likely unused [L]. The tool ignores it.
- **No pyro trigger in HCTP's entrance data or code**: the "pyro" moments of 3047 are its op 18
  white flashes (f402-464 of segment 0, each with sound 207), a warm tint at f162 and the 66/67
  crowd flashes, all carried over. Sparks seen in HCTP come from the arena or titantron video [L].
  The tail record type 2 at frame 480 (section 2.1) is not pyro [L]. Real 2011 pyro would need
  2011's global effect object (docs/SVR08_ENTRANCES.md) [G].

## 8. Objects

| HCTP object | 2011 object |
|---|---|
| character (model 4700 = the entrant) | type `01000000`, model 16102 (2011's preview body; the entrant replaces it) |
| camera (type `00010000`) | the same type, model 0 |
| rope (type byte [3] = `00000001`, model = rope index, 2 = top [L]) | the same type and model: HCTP `CEventRopeAction` -> `CRope::SetYure`, the 5-byte keys = how far the entrant pushes/steps through the rope nearest the slot position; type byte [4] = the turnbuckle variant (`SetCorner`). 2011's 506 has the same objects (12-14, 17); docs/SVR08_ENTRANCES.md calls them lights |
| prop (`00000100`) | HCTP props load from BG.PAC `SOBJ/<id>`. **HCTP 2019 = the WWE (Undisputed) Title folded in the hand** (textures shared with 2001/2010); **2011's 2019 is the World Heavyweight Title - Folded**, so the tool maps 2019 -> **2014 "WWE Title - Folded"** (`PROP_MAP`; 5004 "WWE Title - Folded2" and 3004/3014/4004/4014 are other WWE Title designs). The belt is attached to the entrant (parent 0) |

EVT ids are 25000 + object index (2011's convention), segment slots 0/5/10/15/20, tracks y0.

## 9. What a 2011 entrance has that the conversion lacks

- Face and hand tracks (2011 y1..3 under the character): HCTP has none (its face events are
  op 192 on y0). Without them the face keeps its default and the hands their default pose.
- 2011 pyro is its own system (the global effect object, docs/SVR08_ENTRANCES.md); HCTP has none (section 7).
- Sounds (op 192) and face/hand animations (op 16) use HCTP ids; both are dropped.

## 10. Research notes

- The full op research: docs/HCTP_ENTRANCE_EVENTS.md (addresses in both executables). Disassembly dumps: session scratchpad `gold/re/` (temporary;
  d_setattr.txt, d_camattr.txt, s11_D750.txt, s11_306B0.txt, tex_2019*.png, SOBJ_2019.bin).
- HCTP motion codec research: session 04b01c42 scratchpad `hctp/mot/re_motion.md` (same
  engine generation as 2011, no retarget).
- 2011 entrance numbers in use: scratchpad gold/epk8.py lists the Nyujyo groups.

## 11. Tests [C]

- Test game: `port/runs/opt_gold_game` (its own Mods folder; data linked from Game Files, which
  is never written), saves `port/runs/gold_userdata`. Scripts in the session scratchpad
  (gold/g_setup.ps1, g_session.ps1, g_ent.ps1: muted, behind, screen 2, waits for a free test
  slot).
- Test mod `Mods/Superstars/goldberg_test`: `base=160` (Batista), Batista's `ch.pac`,
  `entrance=503`, `moves/` = the converted pack. `SVR2011_TEST_MATCH="people=GOLDBERG
  TEST,RANDY ORTON arena=1"`, entrances not skipped (`route normal` + `pressuntil A test match:
  people`), `SVR2011_TEST_PRO_LOG=1` shows the slot's entrance number (`CHAR/PRO loaded: id 111
  ... entrance 503`).
- **Result 2026-10-08 (run g5, Goldberg Test vs Randy Orton, RAW arena):** the whole entrance
  plays, then the match starts, no crash: the hidden intro (crowd cameras), Goldberg's stage
  walk-out and crouch-and-roar with HCTP's low cameras and the white flash bursts, the ramp walk,
  around to the corner, through the ropes, the in-ring pose. The first run (HCTP slot k -> 2011
  5k) played everything except the stage part (section 3).
- Test notes: the entrance order (who comes out first) varies between runs - capture about 150 s
  (300 shots at 0.5 s from 50 s in). A copied save keeps the mod slot's own data; check
  `CHAR/PRO loaded: ... entrance 503` with SVR2011_TEST_PRO_LOG=1.
- Not checked yet: the Create-An-Entrance parts (10503-14503) in CAE, the belt for a champion,
  other arenas (raised stages), other HCTP entrances. Hands keep 2011's default pose (HCTP's
  hand animations are op 16 ids, dropped).

## 12. Mod Maker integration (plan)

What a Mod Maker page needs, all of it in `hctp_entrance.py` already:

1. **Source:** the user's HCTP disc folder (ISO extracted; the tool needs `PAC\EVTENT.PAC` and,
   for names, `PAC\MOVES.PAC`). Like the other games pages (docs: Mod Maker "Other games"),
   remember the folder in the settings.
2. **List:** `hctp_entrance.py list` -> a table (number, name, objects). Hide the placeholders
   (512-byte banks).
3. **Number:** propose a free 2011 number (section 1); refuse one a stock entrance or another
   installed mod uses (read the Nyujyo pac's group, and the other packs' `pacentry` lines).
4. **Convert:** `hctp_entrance.py convert <EVTENT> <hctp> <n> <dir> --name <name> --pack`
   (run through `tool_run.cpp` like the other converters; py + numpy not needed by this one).
5. **Output:** either
   - into a superstar project: copy `pack/` to the mod's `moves/` and set `entrance=<n>`;
   - or an entrance-only `.svrmod` of `type=moves` (a `Mods/Moves/<name>/` pack) that other
     mods name with `entrance=`.
6. **Preview:** the Animations page can show the converted character tracks
   (`--anims` takes a bank; the 2011 EVT is a YMKs bank: key 25000, x 0/5/10/15/20, y 0).
7. Show `report.txt` in the Problems list (dropped ops, moved fades, prop ids).
