# SvR 2008 entrances -> SvR 2011 (tools/svr08_entrance.py)

Tool: `tools/svr08_entrance.py` (uses tools/ymks.py). First use: The Sandman's crowd entrance
(2008 4132 "Sandman SP ENT" -> 2011 entrance 504, shipped in sandman.svrmod as move-pack
`pacentry` lines, docs/MOVE_PACKS.md). Played in game 2026-10-08: crowd walk, beer can and
Singapore cane in hand, nameplate, cane raised on the turnbuckle; the stock entrance after it
played unchanged.

Tags: [C] checked on data, [L] likely.

## 1. EVP (script), big-endian [C]

Pointers are relative to file offset 8. A POF0 table after the chunk lists every pointer.

| off | content |
|---|---|
| 0x00 | `0FOP`, u32 size |
| 0x08 | `EV\0\0`, u32 size (the same value; data end aligned to 16 from offset 8) |
| 0x10 | u32 0x10000 |
| 0x14 | ptr -> 0x20 |
| 0x18 | u32 0x10000 if a tail exists, else 0 |
| 0x1C | ptr -> tail (0 = none) |
| 0x20 | u16 entrance number, then the name (30 bytes) |
| 0x40 | u32 object count |
| 0x44 | u32: 2008 1/2/0, **2011 0** in all 52 converted pairs |
| 0x48 | ptr -> object table (0x4C) |

**Object table entry**
- 2008: 28 bytes `{u16 idx, u16 model, type[8], u32 -1, u32 0, u16 slots, u16 0, ptr block}`.
- 2011: 32 bytes. idx and model become **u32**; the rest is unchanged.
- That is the "12200 / 0xD8F1" noise: 2008 `00010F3E` is idx 1 + model 3902; 2011 `00000001 00003EE6` is idx 1 + model 16102.

Type bytes:

| type | meaning | model |
|---|---|---|
| `01000000 00000000` | character | a character model |
| `00000100 00000000` | prop | an evtobj.pac id |
| `00010000 00000000` | camera | |
| `00000001 00000000` and `00000000 01000000` | [L] lights / effects | 0/1/2 |
| `00000000 00010000` | "global" effect object | |

The global effect object's EVT tracks sit at x 10..14 (and 60..64, 63/y50+). CAE parts leave it out.

**Block:** `slots` (normally 21) x 212-byte slots. Each slot is:
- u16 slot
- u8 on
- u8 0
- u32 EVT id
- u32 slot (= EVT x)
- 48 u32 params (floats: positions and so on)
- u16 parent object (9999 = none), u16 0

So `0x270F0000` is "parent: none", not a terminator. Slots 0/5/10/15/20 are the 5 entrance segments. They match EVT x = 0/5/10/15/20.

**Tail:** `{u16 number, u16 n, ptr}`, then n records `{u16 type, u16 0, u32 100, u32 m, ptr items}`, then items `{u8 kind, u24 0, ptr value}`.
- Kind 2 is a u32; kinds 1 and 3 are u16.
- The standard tail is type 1 `[999]` (music, from the profile) and type 8 `[999, 0, 0]` (movie).
- 2011 adds two kind-1 u16 0 items to type 1 (all 216 shipped tails). 2011 writes the u16 values before the u32.

**2008 -> 2011 rules:**
- Table entries 28 -> 32 bytes; every pointer moves by 4 x n_objects; POF0 rebuilt.
- Slot EVT id +25000 (0x61A8).
- 0x44 -> 0; tail gets the 2 extra items.
- Models:
  - prop 2029 -> 5009 (56/67 pairs).
  - Object 0 (the entrant) -> 16102 (the 2011 preview body).
  - Generic 3902 -> 13902. Objects 1..6 copy 16102 when object 0 is 16102 (Yuke's pattern; it matches 506).

## 2. EVT (tracks) [C]

The 2008 EVT is a 2011 YMKs bank without the 16-byte header (`YMKs`, u32 0x100, 8 zero bytes). It also has its own event-size table T: 2011 adds ops (63, 77, ...), and op 151 grows from 2 to 3 bytes (not used by any 2008 entrance).

Rules:
- Add the header and use 2011 T (`T11` in the tool, the variant 506 uses).
- id +25000; dir rt = 55438460 (the game overwrites it at runtime).
- **Drop op 193 events with param 0x30-0x3F (`c1 3x 23`) from y=0 tracks.**
  - 2008 writes them on y0..3. 2011 keeps them only on y1..3.
  - Corpus counts on 2011 y0: 9 "lo" vs 3028 "hi" (0x80+ ids, new 2011 events).
- Copy the global object's x 10..14 tracks to x+50 (60..64) when missing. Yuke's did this for about half the pairs, including 506. Sabu 505 has none, so this step is optional.
- The directory stays sorted (`ymks.write_bank`).

## 3. Validation: 4071 -> 506 (`python svr08_entrance.py validate`)

**EVP:** same size (80948 bytes); **54 bytes differ**, all hand edits by Yuke's:
- Object 6 (an unused extra character), slots 0/5/10/15/20: position/yaw floats added (for example 38.0, 300.0, 8.0).
- Parent 9999 -> 0 on object 11 (global effect), slots 10/15/20, and on the props Shinai (15, slot 0) and Can (16, slots 0/10/15/20). These are slots where the prop has no track.

**EVT:** 120/120 keys present. The header, T and directory order are identical. 106 tracks are byte-identical. The 14 that differ are all hand-made:
- (0,0,0) re-keyed: 181 static frames -> 8 keys, plus face events `c1aa23`/`c1ac23` and op 80. Yuke's also added (0,0,1..3) face/hand tracks. 2008 x0 has none, and 21 shipped 2011 entrances also lack them.
- (0,5,0) and (0,15,0): new 2011 face events (193 0xB3..0xBD).
- (0,15,0..3):
  - The new 2011 op 77 `4d 00 01 0f` / `4d 00 00 0f` at f742/f819. [L] Object 15 = Shinai attach/show on/off.
  - Re-exported poses (±4 codes on a few bones).
- (13,15,0) and (14,15,0): light tracks re-authored (404/341 -> 5028 bytes).
- (11,60,4) and (11,60,8): Yuke's x+50 copies set one byte to 1 (a variant flag); a plain copy has 0.

The 4071 → 506 pair is fully explained: the structural rules match exactly, and only the authored content listed above differs.

**Writer self-test (`selftest`, needs corpus_evt.pkl):**
- parse -> build is byte-exact for 1038 of 1054 EVPs in both games. The 16 failures are unusual special/tag scripts. 4071, 506 and 4132 are fine.
- The CAE-part generator rebuilds 646 of 675 shipped 2011 parts exactly (ignoring EVT rt). The 29 failures are the 5 entrances with unusual tails.

## 4. Props [C]

- 4071, 506 and 4132 use the same 4 props.
- 2008 has them in `pac\evt\_EvtObj.pac` (SOBJ); 2011 has them in `pac\evt\evtobj.pac` (SOBJ, names from its LIST entry).

| id | 2011 LIST name | 2008 | 2011 |
|---|---|---|---|
| 2108 | Shinai (Singapore cane, `bamboo_sword_01`) | yes | yes, same size, kept by Yuke's |
| 2115 | Can (beer can) | yes | yes, kept |
| 2038 | Microphone | yes | yes, kept |
| 2029 | World Heavyweight Title - Rolled | yes | yes, but Yuke's maps it to **5009** "Title - Folded2" |

**So the beer can and the cane exist in 2011 and need no porting.** The converter maps 2029 -> 5009 like Yuke's.

## 5. 4132 output

Command:

    svr08_entrance.py convert in/2008_4132_EVP.bin in/2008_4132_EVT.bin 504 sp_ent_evp.bin sp_ent_evt.bin --ref (4071 08 EVP/EVT, 506 11 EVP/EVT) --parts parts_504

- **Number 504.** It is unused in 2011 and sits in Nyujyo5's range (480-511, next to Sabu 505 and Sandman 506).
- **4132 itself is not usable:** CAE parts are 10000+1000k+n, and 14132 already exists (entrance 132's part 4). n must be below 1000.
- `--ref` applies Yuke's 4071->506 edits only where the 2008 data of 4132 equals 4071's:
  - Prop/effect parent fixes: object 11 slots 10/15/20, Shinai slot 0, Can slots 0/10/15/20.
  - Object 6 slot 20.
  - The two x+50 variant tracks.
- Output:
  - sp_ent_evp.bin: 76448 bytes, 17 objects.
  - sp_ent_evt.bin: 729155 bytes, 116 tracks.
  - 14 op-193 events dropped.
- Both files re-parse; every event block walks 2011 T.
- `parts_504/` holds the CAE parts 10504..14504 (EVP and EVT).

## 6. Installing in 2011 (what a mod needs)

**Container:** an EPK8 entry pair, like Nyujyo5.pac:
- `EPK8`, TOC at 0x800 to 0x4000, data from 0x4000 in 0x800 sectors.
- Group header: `{4CC, u16 count*4, ...}` (12 bytes).
- Entries: 16 bytes `{name[8] ASCII number, space-padded, u32 sector, u32 size/0x100}`.
- Entry data is `BPE ` packed (all 216 entries in Nyujyo5); `svrfmt.bpe_encode` can write it.
- The EVT group ends with a sentinel entry `65535` (256 bytes of "9999...").

**Groups:**
- The 4CC letter belongs to the file: Nyujyo0-4 = EVP1..5/EVT1..5, Nyujyo5-9 = EVPE..I/EVTE..I, NyujyoA = EVPK/EVTK.
- For 504, **use groups EVPE/EVTE, names `504`** (plus `10504`..`14504` for CAE), in an overlay copy of Nyujyo5.pac or a pac that shadows it.
- In use: the move-pack overlay builds a copy of `evt\Nyujyo5.pac` with the entries in
  (`pacentry evt\Nyujyo5.pac EVPE 504 <file>`) and the overlay pac list names it; the game
  finds 504 there. The game's own file is never written.

**Profile:** entrance = 504 in the mod manifest. superstar_mods.cpp already writes profile +0x1C0 +0x14/+0x16/+0x18 (the `entrance=` key).
- Music and movie come from the profile (the tail has 999/999), so `song=`/`movie=` work.
- No profile change is needed beyond that.

## 7. Open risks

- Played once in game (above); the face and cane-grip points below were not judged closely.
- Authored 2011 extras are missing for 4132's own segments:
  - Face events (193 hi ids) and op 77 Shinai attach toggles.
  - x0 face/hand tracks.
  - Light re-authoring.
  - Extra-character positions.
  - Expected effect: neutral face; the cane may not be gripped exactly while it follows its own track.
- New-in-2011 ops (63, 77) are never generated.
- The prop parent fix copies 4071's choices only where the slots match. Can/cane slots whose 2008 data differs keep parent 9999 (a floating prop is possible).
- Object 0 model 16102 is only a preview placeholder (the entrant replaces it at runtime) [L].
- Not checked:
  - The 4132 pyro, sound and particle ids inside EVT events (no 2011 sound bank named for 4132; Sandman 506's sounds are used if the events reference shared ids).
  - The CAE owner string list.
- The x+50 copies are plain copies. Yuke's set a variant byte in two of them (only the ones taken from 506 via --ref have it).
