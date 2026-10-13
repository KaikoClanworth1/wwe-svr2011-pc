# Entrances from other games -> SvR 2011

One section per game: where its entrances are, how they differ from 2011, what converts, and
the state of the converter. Linked from docs/OTHER_GAMES.md (Superstar Porting's overview).

- **Entry point:** `tools/entrance_port.py` (`games`, `list`, `convert`, `free-numbers`; JSON on
  stdout, `progress <pct> <text>` on stderr, exit 0/2/3). `--src` is an extract folder or a disc
  image (`tools/discfs.py`). The Mod Maker calls it (docs/MOD_MAKER_3_PLAN.md).
- **Output:** a move pack (`pack.txt` with `pacentry evt\NyujyoN.pac EVPx|EVTx <n> <file>` lines,
  docs/MOVE_PACKS.md) holding the entrance and its 5 Create-An-Entrance parts
  (10000 + 1000k + n). Put it in a superstar mod's `moves/` (with `entrance=<n>`, or
  `attire<N>_entrance=<n>`: docs/ENTRANCE_ATTIRES.md) or in `Mods/Moves/<name>/`.
- **The 2011 format** (target of every converter): docs/SVR08_ENTRANCES.md section 1-2, 6
  (EVP script: 32-byte objects, 21 x 212-byte slots, segments at slots 0/5/10/15/20, tail;
  EVT: a YMKs bank, track key (25000 + object, x = slot, y = sub-track); `pac/evt/NyujyoN.pac`
  by number range; 2011's segment 0 is the hidden intro).
- **Themes and titantrons** are not part of an entrance in any of these games: the entrance's
  tail says "music / movie from the profile", so they go into the superstar mod as `song=` /
  `movie=` (docs/SUPERSTAR_MODS.md, docs/MEDIA_MODS.md).

| game | disc id | entrances | converter | status |
|---|---|---|---|---|
| SmackDown! Here Comes the Pain (PS2) | SLES_520.36 | 185 | `hctp_entrance.py` | ready, played in game (Goldberg) |
| SvR 2007 (X360) | 545107E0 | | | research |
| SvR 2008 (X360) | 5451080B | 136 (78 in 2011) | `svr08_entrance.py` | ready, played in game (Sandman) |
| SvR 2009 (X360) | 54510826 | | | research |
| SvR 2010 (X360) | 54510844 | 203 (191 in 2011) | `svr10_entrance.py` | ready, played in game (Mr. Kennedy) |
| WWE '12 (X360) | 54510890 | | | research |
| WWE '13 (X360) | 545108B4 | 317 (150 in 2011) | - | researched: needs the motion retarget and Bink 2 |
| WWE 2K14 (X360) | 545408B2 | | | research |

## SmackDown! Here Comes the Pain (PS2)

Details: docs/HCTP_ENTRANCES.md (+ HCTP_ENTRANCE_EVENTS.md).

- **Where:** `PAC/EVTENT.PAC` (DPAC): `EVET/<n>` motion banks 3000-3533 and `ENTR/SEVP` (all
  scripts); names in `PAC/MOVES.PAC` LIST sub-entry 2. Placeholders (512-byte banks) are skipped.
- **Format:** an older script (40-byte slots, 5 segments, little-endian) and CEKey banks in
  HCTP's wide bone form; same motion engine generation as 2011 (no retarget).
- **Rules:** HCTP's segment 0 (intro + stage) is cut at the first camera cut after the reveal;
  HCTP's raised stage and sloped ramp are brought down to 2011's floor level; camera keys
  13 -> 29 bytes (same rotation convention); events kept / dropped per both executables; ring-rope
  objects kept; prop 2019 (folded WWE Title) -> 2014.
- **Pyro:** none in the data (HCTP's flashes are op 18 events, kept). **Titantron/theme:** from
  the profile.

## SvR 2007 (Xbox 360)

(research running: `pac/evt/evt0-8.pac` EPAC groups EVP1/EVT1.., ids 4000+, evt0 = the entrance
editor; music in `music/` (XACT); titantrons `movies/titantron/*.bik`)

## SvR 2008 (Xbox 360)

Details: docs/SVR08_ENTRANCES.md.

- **Where:** `pac/evt/Nyujyo0-3.pac` (EPAC) groups EVP1-4 / EVT1-4, numbers 4000+.
- **Format:** 28-byte object entries (2011: 32), the same 212-byte slots; the EVT is a 2011 YMKs
  bank without its 16-byte header, with 2008's own event-size table.
- **Rules (validated on 4071 -> 506):** objects to 32 bytes, track ids + 25000, 2011's size
  table, op-193 face events with params 0x30-0x3F dropped from y0 tracks, belt 2029 -> 5009,
  entrant model -> 16102 (preview body), generic 3902 -> 13902, x 10..14 effect tracks copied
  to x + 50.
- **Props:** the shinai, can and microphone exist in 2011 with the same ids. **Pyro:** the global
  effect object's tracks (converted). **Titantron/theme:** from the profile (the superstar mod).

## SvR 2009 (Xbox 360)

(research running: `pac/evt/Nyujyo*.pac` + `Create00-20.pac` EPAC groups EVPO/EVTO.., ids 5002+;
`sound/*.yaf`; 85 titantrons `.bik`)

## SvR 2010 (Xbox 360)

- **Where:** `pac/evt/Nyujyo0-3.pac` - EPK8 like 2011, groups EVP1-4 / EVT1-4; 211 entrances:
  singles 100-303, alternates, champion versions 450-478, CAE styles 480-487, legends 500-563,
  tag teams 700-773, match intros 800-840. CAE parts in separate `Create00..20.pac` (not used:
  the converter makes 2011-style parts). 2010 profile entrance block at +0x178 (attire 1) and
  +0x180 (attire 2): music, movie, entrance, champion.
- **In 2011:** 184 by name (some renumbered: 111 Jeff Hardy -> 535, 114 JBL -> 536,
  127 Maria -> 539, 157 Umaga -> 537, 166 Khali -> 547, 168 Dreamer -> 526, 503 Masters -> 50,
  504 RVD -> 306, champion versions 450-464 -> 600-614; 35 were rebuilt by Yuke's). Not in 2011:
  118 Carlito, 162 Mr. Kennedy, 214 Festus, 542 Layla, 403 JBL 2P, 411 Cena PPV, 412 HBK PPV,
  some champion versions.
- **Format:** the EVP already is the 2011 layout (32-byte objects, 212-byte slots, 2011 tail;
  `parse_evp`/`build_evp` round-trip 203 of 211 exactly); the EVT is a full YMKs bank whose ops
  all have 2011's sizes (op 208 shrank 6 -> 2 but no 2010 entrance uses it).
- **Rules** (`svr10_entrance.py`; 67 of 183 shipped pairs byte-identical, the rest were
  re-authored): track ids + 25000, 2011's size table, belt 2029 -> 5009, the global effect
  object's segment parents 9999 -> 0, object 6's position templates from 2011's own entrances,
  preview body unless 2011 uses the 2010 entrant model.
- **Props:** all exist in 2011 except 2403 (JBL 2P limo extra) and 2405 (Miz) - the converter
  warns (2011's own 703 references 2405 too, so a missing prop is tolerated [L]).
- **Pyro:** `prtcls.pac` PCE6/LPE6 packs keyed by entrance number (`0NNN`); ids 0-23 are the
  entrance's own emitters (byte-identical 2010 -> 2011). Of the 2011-missing entrances only 411
  and 412 have their own pyro. Not installed by the converter yet: `prtcls.pac` is EPAC and the
  move-pack overlay's `pacentry` is EPK8-only.
- **Sounds:** op-193 sound ids 4000-4499 are the same in both games; only Kennedy's 4300-4304
  (and 9012, 9349) are missing in 2011 (silent).
- **Titantron:** Bink 1 320x320 (same as 2011): copy as the mod's `movie=`. **Theme:**
  `music1.yaf` XMA2 records -> mp3 (`svr10_superstar.py` already does it).

## WWE '12 (Xbox 360)

(research running: `pac/evt/Nyujyo0-*.pac` EPK8 groups EVP1/EVP2..; `sound/*.pck` (Wwise);
217 titantrons `.bik` + `movies/season`)

## WWE '13 (Xbox 360)

- **Where:** `pac/evt/Entrance00..08.pac` + `Entrance_dlc01.pac` - xcompress around EPK8 with BPE
  entries; groups `EVP ` (script) and `EVZ ` (tracks); 317 entrances with CAE parts like 2011.
  150 have the same number and superstar in 2011, 39 use a number 2011 gives to someone else
  (50 DDP, 511 Matt Hardy, ...), 128 use numbers free in 2011 (Mankind 246, Cactus Jack 245,
  Attitude Era 310-316, legends 553-569, ...). Profile entrance block at +0x230 (same layout as
  2011's).
- **Script (EVP):** the 2011 format byte for byte (`parse_evp(d, '2011')` reads it); 310 of 317
  have no tail (write 2011's standard tail); generic extras use 19202/10402 (2011: 13902).
- **Tracks (EVZ):** WWE '13 motion records (YANM animation + YAT2 events) per object, named by
  the 2011 track key, at 60 Hz (2011: 30). The character needs the WWE '13 skeleton retarget
  (as for WWE '13 moves) into a 2011 body track; cameras convert exactly (eye/target/fov,
  position x 0.1, a fixed axis swap); events map by a table derived from the 150 shared
  entrances; the lights/effects object is not fully decoded (first version: a donor).
- **Props:** 34 of 58 exist in 2011; 24 need porting or a stand-in (Godfather stick, Lincoln,
  Lowrider, Kane mask, Cena chain/caps...). **Pyro:** `prtcls.pac` same format, by number.
  **Titantron:** Bink 2 (2011 plays Bink 1) - needs RAD's Bink 2 decoder (a download, the user's
  call) before the project's own Bink 1 encoder. **Theme:** Wwise `Music.pck` (vgmstream, as
  for the WWE '13 Mankind port).
- **Converter:** not written yet (the joint-angle solver for 2011 body tracks is the new part);
  validate offline on 101/107/123 against 2011's own, then one in-game run (Mankind 246).

## WWE 2K14 (Xbox 360)

(research running: `pac/evt/Entrance00-*.pac` (xcompress around EPK8, like '13); `sound/*.pck`;
157 titantrons `.bik2`)

## Per-attire entrances and entrance outfits (game side)

docs/ENTRANCE_ATTIRES.md: `attire<N>_entrance=<n>` (Rey Mysterio's "Evil" style), entrance
outfits (EMD kind 4 per attire, already working for mods).
