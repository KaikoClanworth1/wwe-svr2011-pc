# HCTP entrance 3047 (Goldberg) events -> SvR 2011

Tags: **[C]** read from code or checked on data, **[L]** likely, **[G]** guess.
HCTP = EE addresses in SLES_520.36 (eedis.py names). 2011 = guest addresses (s11_q.py / recomp).
Disassembly dumps in this folder: `d_setattr.txt` (HCTP CAttribute::SetAttr), `d_camattr.txt`
(HCTP CEventMotionCamera::AnalyzeAttribute), `d_startscene.txt`, `s11_D750.txt` (2011 generic),
`s11_306B0.txt` (2011 object op handler). Also `tex_2019*.png` (prop 2019 textures) and
`evtobj_LIST.bin` (2011 evtobj.pac name list, decoded).

## 0. Dispatch

**HCTP**
- Character and prop tracks: `CAttribute::SetAttr` **0x1b35c0** (one if-chain on the op). It is called by
  `CMotionData::SetKey` 0x1b69d0 (characters) and `CSD3MotionWeapon::SetKey` 0x1c20f0 (props).
- Camera tracks: `CEventMotionCamera::AnalyzeAttribute` **0x164fa0**. It handles only 0x01, 0x3b, 0x3c,
  0x64-0x67, 0x6f, 0x70, 0x71, 0x74 and 0xc0. Everything else is skipped.
- Cast type bytes (tagEVSChar +4..+8, `CEventSystem::StartScene` 0x168030, `CEventInfo::CreateEventObject`
  0x224500):

| byte | class |
|---|---|
| [0] | character |
| [1] | camera |
| [2] | prop (a weapon from `/SOBJ/%03d`) |
| [3] | `CEventRopeAction` with flag 1 (CRope::SetYure) |
| [4] | `CEventRopeAction` with flag 0 (CRope::SetCorner) |

**2011**
- Generic handler: `sub_8239D750`. It calls vt+96 for every op, which is `blr` on all four event-object
  vtables.
  - It handles 1, 10, 49-51, 90, 111, 112, 113, 116, 123-135.
  - **It skips 192 explicitly** (`cmpwi 192; beq` at 0x8239DA90).
  - Other ops go to vt+92.
- Event-object vtables containing D750:

| vtable | track decoder | vt+92 |
|---|---|---|
| 0x8200AA98 | 8239E9D0 | **sub_823306B0** |
| 0x820322A8 | 8239E9D0 | **sub_823306B0** |
| 0x82012910 (objects) | 8239E9D0 | returns 0 |
| 0x820128A8 (camera) | 8239DB28 | returns 0 |

- `sub_823306B0` is the 2011 op table for the wrestler/effect objects.
- **2011 corpus** (all shipped entrance EVTs, `census.py`): ops **16, 112, 118 and 192 are never used**.
  Ops 18, 98, 102, 103, 113, 173, 182 and 193 are used [C].

## 1-2. Op table (HCTP meaning vs 2011)

| op (bytes) | HCTP (3047 track) | HCTP meaning | 2011 behaviour | Port action |
|---|---|---|---|---|
| **16** `10 s idLo idHi` | char | `CCharMotionPts::Set(slot, u16 id, -1)` 0x1ccf40. Plays face/hand motion *id* from the g_pKeyPts bank through CSD3MotionFace ('3MFA' link). **Slot 0 and 3 = face** (SetData), **slot 1/2 = left/right hand** (SetDataHand). Mirroring swaps 1<->2 and ids ±200. 3047 uses hands 1 / 201 / 210 / 10, face 0x4b2 = 1202 and 0x456 = 1110 [C] | **No handler**: neither D750 nor 823306B0 has 16 (unhandled, returns 0). Never used in 2011 [C] | Drop it, or rebuild as 2011 face/hand tracks (x0 y1..3). No 1:1 op |
| **18** `12 r g b a dur` | char | Screen fade/flash. Link 'EVFA' (event type 2) or 'FADE'; only if fader+0x3c == 1. Target colour r,g,b,**a (PS2 0-128)**, start = current colour, **dur ticks (×5/6 on PAL)**. SetAttr 0x1b4760 [C] | **Same op, same layout.** 823306B0 -> `sub_823B4730(r,g,b, clamp(a*2), dur)` on the 'FADE' link (sub_821D33B0). It does the same ×5/6 when refresh is 50/25 [C] | **Copy bytes unchanged.** 2011 doubles alpha itself |
| 98 `62` (96-99) | char | `RingYure(op-0x60)`: ring shake, level 2 [C] | 823306B0: 96-99 return 1 and **do nothing** [C]. 92 uses in 2011 data | Harmless. Copy or drop |
| **102/103** `66`/`67` (100-103) | char, very frequent | `CGameFlash`: crowd camera flash bulbs. g_FlashLv = op-100, g_FlashCnt = (op-100)*20+60. Skipped in arenas 0x1c/0x27/0x2a [C] | **Same**: if sub_821871B0 (arena), level = op-100 (+offset, clamp 0..5), `sub_823A7D50((op-97)*20)` [C]. 11 688 / 1 425 uses. Op 173 `ad lv` is the same with level as a parameter | **Copy unchanged** |
| **112** `70 z r` | camera | Camera **zoom**: target = z/10 (+0x1c8), rate = r/1000 (+0x1cc). Each frame cur += rate·(target−cur); rate 0 = snap. cur (+0x1c4) -> +0x40 -> `g_pro_z = 512·zoom` (FrameActive 0x164ae0, UpdataCamera 0x164880) [C] | **Same scaling exists**: D750 -> `sub_8239A8A0(b1·0.1, b2·0.001)`. It writes +244 target, +252 rate and sets zoom mode +1320 = 1 (when +1148 == 0). `sub_8239C5C0` then sets **cam+80 (fov°) = 2·atan(0.625/zoom)·k**, with k = 0.75, or 0.82 when [0x82EDDBEC] == 1 (widescreen [L]) [C] | **Keep it**: 2011 reproduces HCTP's zoom model (see §3). Unused by stock 2011 data |
| **113** `71 c` | camera, f0 | Camera height reference: c = cast+1 (1 = Goldberg). Lerp to base + (19.75 − char height) (GetCharHeight 0x1656d0); 0 = none [C] | D750 -> `sub_8239C008(this, c, 0, 0)`: retarget to cast c (table this+1280+4c), same lerp fields. 116 = `(c, frames)` like HCTP 0x74 [C code; index base L] | Copy. 2011 data uses **`71 00`** at f0 (3064×), so map HCTP `71 01` -> `71 00` [L] |
| **118** `76 v` | char f0 | `SetC3DAudienceLight(v)`: crowd lighting state (0x215e20) [C] | **No handler** (823306B0 returns 0); never in 2011 data [C]. Equivalent not identified (op 175 `af v` sets a stage global, [G]) | Drop |
| **182** `b6` | prop, f0 of every segment | Sets the motion state +0x18 = 1 of the track's owner. For props it is copied to +0x1c after SetData. Family: 181=3, 183=4, 185=−1, 186=0xff, 187=8, 188=12, 190=5, 191=9, 184=9 if synced [C]. Meaning of 1 [L]: "prop follows its track" | Not in D750/823306B0 (returns 0) [C]. **2011 props carry the same `b6` at f0** (2037×, e.g. entrance 100 object 7) [C data] | **Copy unchanged** |
| **192** `c0 id` | char + camera | **Sound effect**: `GameSE`/`PlaySE_FromAttribute(obj, id, 0)` 0x3ad790. id = HCTP SFX number (CSD5_SfxFileList::SeNo2BankNo; 300-325 / 0xca / 0xd0-d2 are MIDI cues). 3047 uses 0x31, 0x2f, 0xcc, 0xcf [C] | **Explicitly ignored** in D750 [C]. 2011 sound op = **193 `c1 lo hi`** -> `sub_825E8E40(0, u16 id, 0, 0)` (sound/voice cue, 'SFXE'/'SEFW' links; ids 2000+, 3000-3499, 9000-9499 special) [L] | Re-author as 193 with 2011 ids, or drop. **HCTP ids do not carry over** |

**Notes**
- **193 is not a pure "face" op.** 823306B0 sends it to the sound/cue dispatcher. Stock 2011 uses
  0x2330-0x2333 on the hand/face sub-tracks y1..3 and 0x23AA-0x23BE on y0.
  - Whether those cues also animate the face (voice/lip cues) is [L/G].
  - SVR08_ENTRANCES.md calls them face events.
- **Other 2011 ops in 823306B0** (for reference):
  - 15 `0f a b` (+1360/+1364)
  - 20: an 8-byte fade variant
  - 22: 'BLUR' (`sub_823B4A38`)
  - 58, 59, 71
  - 117 `75 a lo hi`
  - 127, 134 (flash +3), 136, 137, 138
  - 158-169 (jump table at 0x82330BB0)
  - 171-175, 226

## 3. Camera FOV

**HCTP**
- The keys have no fov.
- Projection (`UpdataCamera` 0x164880): `sceVu0ViewScreenMatrix(scrz = 512·zoom, ax = 0.8, ay = 1.0742857,
  cx = 2048, cy = 2072)`.
- The PAL frame is 512 wide (SD4ViewScreenMatrix uses ax = ScreenWidth/640 = 0.8). The base camera
  constant is 64.0176° = 2·atan(0.625).
- So:
  - **hfov = 2·atan(0.625/zoom)**;
  - **vfov ≈ 2·atan(0.469/zoom)** (4:3).
- **Zoom defaults to 1.0** (ctor 0x164a88). **Op 112 sets it.**

3047 camera zooms:

| segment | zoom events |
|---|---|
| y0 | 2.0 snap, 3.0 at f69, 2.0 at f162, then -> 3.0 (rate 0.02), 1.0 at f350 |
| y1 | 1 -> 2 (0.02) |
| y2 | 1 |
| y3 | 2 -> 4 (0.02) |
| y4 | 2, 1 at f88, then -> 2 (0.01) |

**2011**
- With 29-byte mode-1 keys, +244 = 2·atan(0.8·tan(F/2)) rad (F = key fov). sub_8239DB28 at 0x8239DF10:
  0.5, 0.8, 2.0 constants.
- cam+80 is that value in degrees (sub_8239C5C0).
- Stock median F 0.7217 is 33.6° (≈ HCTP zoom 1.5-1.7).
- **Op 112 overrides the key fov** (zoom mode, +1320).

| HCTP zoom | HCTP hfov | 2011 fov via op 112 (k=0.75 / 0.82) | 29-byte key F to match (k=0.75 / 0.82) | key F for true 4:3 vfov |
|---|---|---|---|---|
| 1 | 1.1172 (64.0°) | 0.838 / 0.916 rad | **1.016 / 1.105** | 1.060 |
| 2 | 0.6058 | 0.454 / 0.497 | **0.563 / 0.614** | 0.570 |
| 3 | 0.4108 | 0.308 / 0.337 | **0.383 / 0.419** | 0.386 |
| 4 | 0.3100 | 0.233 / 0.254 | **0.290 / 0.317** | 0.291 |

- Formula: F = 2·atan(tan(t/2)/0.8), with t = k·2·atan(0.625/z).
- **Recommendation:** keep op 112 (and 113) in the camera tracks. 2011 then applies HCTP's own zoom
  curve, and the key fov is ignored.
- If op 112 is dropped, bake F per key from the zoom in force at that frame. **The fixed 0.7217 is wrong
  for 3047, which is mostly zoom 2-4.**

## 4. Prop 2019

- HCTP props are weapons: `MakeWeapon(model)` 0x211eb0 loads **`/SOBJ/%03d`** -> **BG.PAC group SOBJ,
  entry 2019** (52 992 bytes; YOBJ "k2019" + RTX3 bank) [C].
- Belt exchange applies only to models 70-78 and 172-177 (s_HandBeltRootTbl/WaistBeltRootTbl 0x41a020),
  so 2019 is shown as-is [C].

**What it is**
- Nodes `k2019, root, dummy01, bone01, dummy02`. Size about 4×4×3.4, two hinged halves.
- Same textures as 2001 (stretched, 12+ bones, 34 long) and 2010 (hanging, 13-bone chain).
- Texture plate: W logo, eagle, globe, "CHAMPION" = **WWE (Undisputed) Championship, 2002-05 design,
  folded / hand-held variant** [C geometry, L name] (`tex_2019_plate.png`).
- HCTP SOBJ 2001-2009 / 2010-2017 / 2019-2027 are 9 belts × 3 poses.

**2011 evtobj.pac (LIST, 64-byte records `u16 id + name`)**

| ids | variant |
|---|---|
| 2002-2010 | Stretched |
| 2012-2020 | Folded |
| 2022-2030 | Rolled |
| 5002-5010 | Folded2 |

The WWE Title entries are **2014 "WWE Title - Folded"**, **5004 "WWE Title - Folded2"** and 2004 / 2024.
More WWE Title models are 3004/3014/3024 and 4004/4014 (other designs; check which one is the 2002 eagle
belt [G]).

**Trap:** **2011 id 2019 = "World Heavyweight Title - Folded"**. Keeping the number shows the Big Gold belt.
That may even suit Goldberg in 2003 (he was WHC); otherwise use 2014 or 5004.

## 5. "Light" object 2 = ring rope action, not a light

- Type byte [3] -> `CEventRopeAction(key, offset, frame, flag=1, model)` ctor 0x2268b0, `Action` 0x2269d0 [C]:
  - rope = model % 3 = **2 (top rope)** [L top];
  - side = `GetNearRope(slot pos)`. Slot 4 is at (−1.5, −11.8, −28.5): one ring side, rope height;
  - 5-byte keys `[count][s16 a][s16 b]` ×0.01, lerped over (count+1)·2 ticks;
  - `CRope::SetYure(side, rope, along, a, b)` 0x2188a0 pushes the rope. b == 100.0 resets it;
  - Flag 0 (type byte [4]) is `SetCorner`.
- 3047 y4 keys: a 0.05..1.30, b −1.6..+1.7 over 68 frames. **Goldberg presses or steps through the top
  rope.** It is active only in segment 4 (slot 4 on).
- **2011 equivalent: the same type byte**:
  - 506 has objects 12/13/14, `t0 = 0x00000001`, models 0/1/2 = bottom/middle/top rope;
  - object 17, `t1 = 0x01000000`, model 2 = the SetCorner variant;
  - each 506 track (25013/25014, x15) is 5028 bytes for 837 frames: one 5-byte key every frame.
  - [L, strong]: same layout, same model meaning.
- SVR08_ENTRANCES.md's "lights" (13,15,0)/(14,15,0) are these rope tracks.
- **Port:** an object `{model 2, t0 0x00000001}` with the y4 scalar track copied, slot 4 position kept.

## 6. Pyro and SYDT

- **No pyro trigger exists in 3047's data or in the HCTP entrance code paths** [C]. Checked:
  - every SetAttr callee (no particle calls);
  - CEventEffect types: titantron, nameplate, colour filter, stage movie, BGM, fullscreen titantron, fake
    blur, "FrameFalling" (= frame-skip strobe, not falling sparks), fake glare;
  - 3047's 4 casts (char, camera, top-rope action, prop 2019);
  - `ParticleEventStart` callers (only gimmicks and bikes).
- **The "pyro" beats are authored as events** on x0 y0:

| frames | event | meaning |
|---|---|---|
| 402-464 | `12 c8 c8 c8 64 02` | white flash, 78 %, 2 ticks |
| | `12 00 00 00 00 14` | fade out, 20 ticks (×5) |
| | `c0 cf` | SFX 207, a bang [L] |
| f162 | `12 c8c8be5a00`, then `12 0000 001e` at f231 | warm tint, then fade |
| all segments | `66`/`67` | crowd flash bulbs |

- Real sparks, if seen, come from the arena (stage movie per arena, `CEventStageMovie` keyed by
  g_common+0x200) or the titantron video [L].
- **2011:**
  - op 18 and 66/67 copy unchanged;
  - the bang needs a 2011 sound id via op 193;
  - real pyro would need the 2011 global effect object (type `00000000 00010000`, x10..14 / 60..64
    tracks) [G].
- **SYDT** (`16 02 b8 0b` = count 534, first id 3000):
  - It is a **compact duplicate of SEVP**: per entrance `{u16 id, 0, u16 ncast, …}`, per cast
    idx/model/type, and 33-byte slots `{u16 slot, u8 on, u16 EVT id 9950, …, slot, f32 pos ×3, f32 rot,
    u16 parent}`.
  - For 3047 it has the same 4 casts. The rope slot 4 is (−1.5, −11.8, −28.5); the char slot 4 has rot π.
  - The ELF names only `/ENTR/SEVP` and `/EVET/%04d`. There is no "SYDT" string or tag constant, so it is
    **not used at run time** [L]. **No pyro in it.**
- **SEVP cast record** (28 bytes): `u16 idx, u16 model, u8 type[8], i32 -1, i32 0, u16 nslots, u16 0, ptr`.
  Slots are 40 bytes: `u16 slot, u16 on, u16 9950, u16 0, u32 cast, u32 slot, f32[4]…, u16 parent`.
  - Parent 9999 = none; 3047 prop parent = 0 (Goldberg).
