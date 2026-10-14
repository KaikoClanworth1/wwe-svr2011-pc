# SvR2011 side of the HCTP "Arena Back Entrance" port: collision attributes, floors, motions, gimmick types, weapons

Read-only research, 2026-10-07. Tags: **[C]** confirmed from code/data, **[L]** likely, **[G]** guess.
Addresses are guest addresses in the recompiled SvR2011 code (`generated/default/svr2011_recomp.*.cpp`,
asm in the comments). Image dump used for vtables/constants: `WWE Smackdown Vs Raw 2011/port/runs/image.bin`.
Helper scripts (this folder, prefix `s11_`) are listed at the end.

---------------------------------------------------------------------------------------------------
## 0. Short answers

1. 2011 still has the HCTP-style "touched wall attribute -> action" switch: **sub_82306B10** (switch on
   attr-200, 0..54). The per-character touched attribute is the byte **CChar+455** (written by the stage
   collision driver sub_821E5910). But 2011's switch is a different set:
   - **0xFB, 0xF4, 0xDB are not handled** (they fall to the default "stop at the wall") [C].
   - **0xDF does not climb.** It is the trigger wall for backstage gimmicks T21/T31 (running into it) and
     T22 (walking into it) [C].
   - **0xFD** is a weapon-object attribute (tables): running into it gives act 664, or 56 [C].
   - **0xFE/0xFC** still have climb code (acts 256/262), but no stage uses them and 262 has no motion data
     [C code, C data].
2. Collision is **sphere vs triangle in each triangle's own plane**, with no floor/wall type. Horizontal
   triangles at any y push the body up and set the "supported" flag CChar+457 bit 0 [C]. Outside the ring
   the ground height is the wrestler's own y [L], so floors at height work (bg78's rooms stand on −8.4
   triangles).
   - No HCTP-like fall code (MakeDrop / GapIn) was found. Fall motions exist: 660 x0..7, 661, 662, 666 [C data].
   - Limit: only arena 78 loads 8 collision meshes (one per area), every other arena loads 1. Each mesh
     needs its z-cell index (997/999...) [C].
3. Generic climb/fall acts live in m.pac MOT/BM03 (match copy mpsp.pac MOTP/GAME). Present: 256 x2 (25 f),
   664 (25 f), 241, 291, 299, 679, 660 x0-7, 2305 (ladder, MVMT/LADR), 31000/31001 (cell, MVMT/CELL).
   - **262 and 263 are 1-frame stubs** (bank BM03/63 only) [C].
   - **HCTP 12xxx backstage acts are absent**: 12572, 12092, 12045-12047, 15740/15750 [C].
   - 2011's own 12xxx keys are different moves, in the per-room gimmick banks. The common set 12200-12231
     holds the run-into and whipped-into object reactions [C/L].
4. GMPD element types (factory sub_824B8AB8, types 1-34) [C ctors; meanings L]:
   - **T33**: environment hot spot (grapple move at an object, plus run-into and whip-into reactions).
   - **T21/T31**: run into a 0xDF wall near the spot, and the wrestler plus objects play a key.
   - **T22**: walk into a 0xDF wall, and the wrestler plays a key.
   - **T24**: binds placed weapons by model.
   - **T29**: take a weapon from a T24 rack.
   - **T25**: grapple move with a main-bank move id, plus objects.
   - **T34**: fade objects when someone is near.
   - **T30**: chamber pod parts.
   - **T23**: object.
   - **T20/T32**: their consumers exist, but no shipped data uses them.

   Not found in 2011:
   - an object that **spawns** a new weapon when hit;
   - one object that triggers another (the closest are T29 -> T24 and the T33 follow-up keys);
   - moving or breakable collision on gimmick objects. GMGB packages carry no HMD;
   - damaging particles (steam).
5. Weapons at the bell come from bgEtc `STG/WPON` or `STG/WPBS`, **entry 45010**, keyed by **rule id**. The
   backstage areas are rules 0x1B-0x21, in backstage world coordinates [C].
   - The text files 500xx (WPBS 50078, WPON 50000/50054-58) are **not read by code**: no constant for them
     exists [L].
   - The weapon models come from the selected set (WPBS for backstage).

---------------------------------------------------------------------------------------------------
## 1. Collision attributes (Q1)

### 1.1 Stage collision data and per-character storage [C]

**Loading (sub_82187258).** Loads the stage HMDs: entry 996 (+997 index), 998 (+999)... For **arena 78
there are 8 pairs (996..1010)**, one per backstage area; for every other arena there is 1.
- Each HMD becomes a "TRIP" object (sub_826AAA20, vtable 0x82057C98): count at +176, record pointers at +180.
- All 24 words of each record are byte-swapped, so word 23 is big-endian in memory:
  - byte +92 is reused at run time as a per-query "near" flag;
  - byte +93 is the attribute class;
  - bytes +94/+95 are byte1/byte0.
- The odd entries are u16 z-cell index lists: 120 cells of 10 units, chosen by |z| (as `index_build` in
  svr08_stage.py).

**Triangle test (sub_826AAFE0(tri, body, mode)).** Skips a triangle whose near-flag is 0. The attribute
class decides who collides:
- class 2: only bodies with flag 0x80;
- class 3: always.

It returns `word23 & 0xFFFFFF` on a hit. Modes: 0 = face, 1 = vertex, 2 = edge (pushouts in the triangle
plane: sub_826A9770 / sub_826AA5D0 / sub_826AA780).

**Mesh query (sub_826AB2E8(trip, body, cellList, n, disableAttr)).**
- ORs the attributes of all hits, except 0xFFFFFF.
- Class 1 hits (0x01xxxx, the bg00 ramp-side walls 0x10900) only correct x/z.
- A hit with attr low byte not 0 or 0xFF saves the triangle normal at trip+192.
- The last argument can disable every triangle whose low byte equals it. The only caller always passes
  255, so this per-attribute switch-off is unused.

**Character driver (sub_821E5910).** The only caller. It runs two passes.

Pass 1: object/weapon collisions in the list at stage+22512 (iterator sub_823ACBE0).
- **CChar+416** = the touched object.
- **CChar+456** = its slot + 1.
- Class byte >= 128 calls sub_822C7C00 (object event).
- With body flag 0x10, a low byte of 246 (0xF6) is stored at +455.

Pass 2: stage HMD of the current area, only when CChar+444 != 0 (not in the ring). Results are stored in
CChar:

| offset | contents |
|---|---|
| **+455** | **touched attribute (low byte)**. Set once per frame, and only if the top byte is 0 and low ∉ {0, 0xF9, 0xFF} |
| +400..+415 | wall normal |
| +460 | first attribute |
| +459 / +462 | byte1 (material) |
| +461 | byte2 (e.g. 0x0A/0x0B/0x0C floors, 0xF8/F9/FA on 0xEA walls) |
| **+457 bit 0** | "pushed up by collision" (supported). Set when the y correction is < −0.01. Body +116 = landed height |

sub_821E70C0 clears these bytes each frame and shifts the +457 history into +458.

**Who reads +455.** The switch below; the climb-onto-weapon code sub_82306110 (0xFD with a touched
object); grapple code (sub_822C9F48, 823BA890, 822E35E8: 245, 234, 235, 240); AI (sub_8284D4E0/7F8: 243,
sub_828509C0, sub_82829BA8: any).

### 1.2 The switch: sub_82306B10 (equivalent of HCTP CaseDash) [C]

**How it is reached.** Called from the movement code (sub_82306F90 when +455 != 0, and sub_8228F2E0)
when the wrestler pushes into a wall. CChar+1286 = running flag [L]: set by the movement state when
+216 == 3, and read by IsRunning sub_821B6A48 (act 50/51 or +632, and +1286 == 1).

**Before the switch:**
- If running, sub_824B31B8 is tried first: a T33 hot spot in range gives act 12202 or 12212 (see §4).
- 0xD8 and 0xF5 try sub_82306830 first: T20 check, act 563/561/562, otherwise the default.

**Helpers used in the table:**
- **ClimbOK** = sub_821BCB60: wall normal +400 within 45° of facing, and a foot bone (+176/+192, y) below −2.9.
- **JumpAct(c, act)** = sub_82397228 (sub 0, 0).
- **JumpAct(c, act, s1, s2)** = sub_82396E80. Both end in sub_82395E10.

| attr | handler | 2011 behaviour |
|---|---|---|
| 0xC8 (200) | inline | running -> JumpAct(**664**) |
| 0xD6, 0xD8 | sub_82305CA0 | running -> **231** (BM03/9, 22 f); walking -> stop |
| 0xD7 | sub_82306760 | ClimbOK + running + flag -> **241** (26 f) + vibration; else stop. **bg00-02: 6 walls at (−65, 63)** [steel steps, L] |
| 0xD9 | sub_82305D28 | running -> **238** (27 f) |
| **0xDF, 0xE0** | sub_82306930 | running -> **T21/T31 gimmick** (sub_824B6168), else sub_82306830 (T20 -> 563/561/562), else stop. Walking -> **T22 gimmick** (sub_824B6A70), else stop. **No climb** |
| 0xEA, 0xEB, 0xF0 | sub_82305B70 | ClimbOK + running -> byte2 0xF8/F9/FA give 296/297/298, which are **missing** (needs another wrestler in the same act, through object 93); else **299** (29 f). bg24/25/26/80 only |
| 0xED (237) | sub_82305A48 | running -> T21/T31 (sub_824B6168) or, in backstage/story modes (sub_828B4BD8: arena 78 or certain modes), stop; elsewhere **664**. Walking -> stop. **bg66/bg78 room walls** |
| 0xF3 | sub_82305B00 | ClimbOK + running -> **241**. bg00-02: 4 walls at (65, −63) [steel steps, L] |
| 0xF5 | sub_82306630 | running near the ring (sub_821871B0) with a small offset -> stop, else **291** (27 f) [barricade, L]. bg00-02: 13 walls |
| 0xF6 | sub_82305960 | running, outside the ±55.3 square (sub_821B8F38) -> **31000/31001** (MVMT/CELL: Hell in a Cell wall). Comes from object collision |
| 0xFA | sub_823069E0 | running with a carried weapon (+420) for more than 2 frames -> **664**. Walking at a standing ladder (weapon type 11, act 15011) -> **2305** with the ladder (sub_8218FDD0) = ladder climb |
| 0xFC | sub_823058B0 | ClimbOK -> JumpAct(**256** or **262** if running, sub **3**) |
| 0xFD | sub_823064C8 | running -> **664** (or **56** if the carried object is type 11/5/9 with act 15000). Walking on a weapon-object top -> sub_82306110 (tables, act 28500 checks) |
| 0xFE | sub_82305810 | ClimbOK -> JumpAct(**256**, or **262** if running) |
| default, incl. **0xDB, 0xF4, 0xFB** | sub_8220C808 / sub_823055A0 | stop: JumpAct(50, sub 2) |

Notes:
- 0xDF in bg78 (and bg66): entry **1004** (parking), 2 triangles at about (111, −524), y −91.6..8.4.
  These sit at 78PK's trailer, where **T22 #97 [0 9 12100 0]** at (109.4, −523.5) waits.
  - Walking into that wall within radius 9 plays key 12100 from 78PK's bank on the wrestler (trailer door).
  - T33 #98 (key 12312, object 94 = trailer 1061) is the matching grapple spot.
- 0xED: every backstage room wall in bg78 998-1010 (100 tall); the 996 outline is 0xFF.
- 0xF5: bg00 barricade-side walls.
- 0xA0000 / 0xB0000 / 0xC0000: bg00 floor patches at y 0. They only set +461 (10/11/12). +461 == 12 is
  tested in the grapple code (sub_822E2350, 822E35E8) [G: ring-side zones].

**Attribute census of all shipped stages.** s11_attrs.txt: 0xD7, 0xDF, 0xE7-0xE9, 0xED, 0xEE, 0xF0, 0xF3,
0xF5, 0xFF, 0x2ED, 0x900, 0x10900, 0xA/B/C0000, 0xF8/F9/FA00EA, 0xFFFFFF.
- **No stage uses 0xFB/0xFC/0xFD/0xFE/0xF4/0xDB** [C].

**Porting consequence.**
- Climbing like HCTP (0xFB/0xFD/0xF4/0xDF -> 256/262/263/664) is **not** available as-is.
- 0xFE/0xFC are the only stage-usable climb attributes. They call 256 (sub 0 or 3) / 262, but only
  256 sub 2 has data.
- Options:
  - **(a)** a hook in sub_82306B10 for new attribute values, playing our own keys (motions put in our GMGB
    bank and started with JumpAct);
  - **(b)** use 0xDF + T22 (walk) / T21 (run) records next to the climbable faces: the game plays any key
    from the room bank (§4).

  Either way the wrestler's y after the climb must be held by a floor triangle at the top [L, untested].

---------------------------------------------------------------------------------------------------
## 2. Floors at height and falling (Q2)

**Collision itself [C].**
- Pushouts are 3D, in each triangle's own frame (rows 0-3 of the record). A flat triangle therefore
  pushes a sphere up like any wall pushes sideways.
- sub_821E5910 writes the corrected body position and sets CChar+457 bit 0 when the push is upward
  (dy < −0.01). Body+116 = landed y.

**Ground height [L].**
- sub_8218EB88 returns −12 inside the ring (with steps and apron cases), and table values for steps
  (sub_821BB4A0: −9.7, −60.5).
- Elsewhere it returns **the wrestler's own y (CChar+292)**. Outside the ring the height is whatever
  gravity plus collision leaves.
- Evidence:
  - bg78 rooms stand at y −8.4 on 108+ flat −8.4 triangles (996/1000/1002/1010), while the parking area
    (entry 1004, no floor triangles) is at y 0.
  - bg00 has flat triangles at −18.1 (entrance stage, attr 0x900) next to 0x10900 side walls.
- sub_821B9450 ("airborne/high") = not supported (457 bit 0 clear) and all bones above a per-area level.
  The grapple code uses it.

**Falling.**
- No HCTP-like "MakeDrop / GapIn" or edge-check code was found in this pass [G: none].
- Generic fall acts exist:
  - 660 sub 0-7 (9/9/27/8/12/12/12/12 f), 661 (24 f), 662 (59 f), 666 (48 f), in BM03 and MOTP/GAME;
  - ~33 code sites load 660, e.g. sub_8220DF88, 82230670, 822E35E8, 82277308.
- How a wrestler who walks off a −30 ledge is handled (gravity glide or a fall act) is **untested**. Test it
  in game with a raised floor before relying on it.

**Limits [C].**
- 8 HMDs only for arena 78; other arenas 1.
- Cells cover |z| < 600 (120 bands of 10).
- Area selection is by stage helpers sub_82187610/82187758/821874C8.
- New geometry needs a rebuilt cell index.
- Wall collision is skipped during grapples (sub_821E5910 early exits; see MATCH_TYPES_RESEARCH).

---------------------------------------------------------------------------------------------------
## 3. Motions (Q3)

Bank index: s11_motidx.pkl / s11_mq.py. In the tuples, x = sub1/variant, y = track (0 wrestler,
1 partner, 2+ objects, 50+ camera/prop).

| HCTP act | 2011 | where |
|---|---|---|
| 256 climb (walk) | **256 x2** (25 f) only; x0 is a 1-frame stub | m.pac MOT/BM03/9, mpsp MOTP/GAME |
| 262 climb (run) | stub only (BM03/63, 1 f) — **no motion** | |
| 263 climb wall/ladder | stub only — **no motion**; ladder climbing is **2305** (MVMT/LADR, RU*) | |
| 664 | **664** (25 f) | BM03/9 |
| 660 fall | **660 x0..7** | BM03/1, MOTP/GAME, CAF |
| 679 | 679 x0/x1, y0-2 (27/40/53 f) | BM03/9 |
| 340 hit object | none | |
| 12572 jump down, 12092 climb out, 12045/46 cage wall, 12047 steam, 15740/15750 | **none** | |

**Other 2011 environment acts.**
- 231 (22 f), 238 (27 f), 241 (26 f, steps), 291 (27 f, barricade [L]), 299 (29 f) in BM03/9.
- 31000/31001 cell wall (MVMT/CELL).
- 56 (41 f).

**2011's 12xxx keys are per-room gimmick moves**, in each GMGB package's YMKs bank (gm.pac):

| package | keys |
|---|---|
| 78GM | 12001-12013, 12330 |
| 78GL | 12051-12058 |
| 78HW | 12051, 12057-58, 12335-37, 12350-51, 12360, 12401 |
| 78IS | 12057-58, 12332-34, 12338, 12345-46, 12354-56, 12360-64 |
| 78PK | 12100, 12300-12307, 12312 |
| 78PL / 78RL | 12000, 12320-23, 12331, 12340, 12365-66 |
| 78AP | 12451 |

- **Common to every room**: 12200-12207, 12210-12217, 12220, 12225, 12230-12231. These are the
  object-reaction moves code asks for by number [C]:
  - **running into a T33 object**: sub_824B31B8: class p6 = 0 -> 12202, 1 -> 12212;
  - **thrown/whipped into one**: sub_824B3310: class 0 -> 12200/12207, class 1 -> 12210/12217, by the
    attacker/victim flag;
  - others from sub_82233D90 (12203, 12213), sub_822E35E8 (12205/12206/12216/12225), sub_82357A80 (12227),
    sub_823602C0 (12215, 12223-12226).
  - The meanings (class 0 = low object, 1 = tall/wall) are [L].
- Same numbers as HCTP, but **different moves**. The HCTP keys 12000-12801 cannot be carried over by
  number. Converted HCTP motions must be given free keys in our own GMGB package bank (for example
  12500-12599, unused by 2011).
- 2011 keys are (id, x, y) with tracks y0/y1 = the wrestler pair and y2+ = objects. This matches HCTP's
  sub-key convention (act,0,0) wrestler / (act,0,1) partner/object.

**Generic climb motions must be authored or converted.**
- 2011 has no 262/263 data. 256 x2 is a 25-frame climb (onto what is unverified).
- HCTP's are CEKey frame streams, a different pose format: not checked.

---------------------------------------------------------------------------------------------------
## 4. GMPD element types (Q4)

**Construction [C].**
- sub_824B6D78 walks the locators. sub_824BA1F8 finds the record with id = locator index.
- The factory **sub_824B8AB8** switches on type−1 (0..33).
- Base sub_824B92A0 / sub_824B9400 sets:
  - +68 = type, +76;
  - +80 = record, **+84 = &word1 ("p")**, +88 = (int)word0 (object id; −1000 -> model slot, sub_824B90B0).
- Base vtable 0x82020FA0 has 15 slots: +0 dtor, +4 update, +16 init, +20/+24 position/rotation getters,
  +44 "register prompt sphere" (sub_8224F678 with radius p0) [L].
- Locators: 32-byte {pos, idx, rot} in PACH 3/10000. Rotation: x' = x cos a − z sin a.

**Constructors and vtables** (s11_vt.py). Types 1-12 are not backstage (5 'GMFR' and 7 'GMEL' tagged).

| type | ctor | vtable | size |
|---|---|---|---|
| 20 | 824B1488 | 82020C6C | 160 |
| 21 | 824B5400 | 82020DD8 (19 slots) | 128 |
| 22 | 824B6630 | 82020EA0 | 112 |
| 23 | 824B9A40 | 82021048 | 112 |
| 24 | 824B94F0 | 8202100C | 160 |
| 25 | 824BB3E0 | 82021178 | 192 |
| 27 | 824BB190 | 82021138 | 192 |
| 28 | 824BAE38 | 820210FC | 144 |
| 29 | 824B0558 | 82020C20 (19) | 128 |
| 30 | 824BA9F0 | 820210C0 | 144 |
| 31 | 824B5458 | 82020E28 (19, same 0/4/44 as T21) | 144 |
| 32 | 824B4DF8 | 82020D90 | 128 |
| 33 | 824B2280 | 82020CF8 | 176 |
| 34 | 824BA658 | 82021084 | 144 |

Consumers are found by scanning for `lwz rX,68(..)` + `cmplwi rX,<type>`.

**T23 object** [C, earlier notes]: word0 = model id (1000-1255 bank 0, 1500-1755 bank 1); w4 = draw/variant;
**w5 = idle motion key** played on the object; w6 = draw condition.

**T33 hot spot** (78PK/GM/GL/HW/IS/PL/RL), words = [0, p0..p9]:
- **Fields:**
  - p0 radius (8-12);
  - **p1 grapple key** (pair motion; 0 = no grapple);
  - **p2..p4 T23 record ids** animated with it (track y2..);
  - p5 variant: 0 normal; 1/2 heavier-opponent key+1 (sub_82193398); 4 facing-dependent 12011/12012
    (GM); **7 uses placed weapon model 150 as a prop** (init sub_824B2300);
  - **p6 object class** for the generic reactions (0/1, also 2/3/13-15 in HW/IS);
  - p7 / p8 flags (stand side 0/1 [L]);
  - p9, p10 follow-up keys (12300/12303 on car fronts; −1 = none) [G: what plays after].
- **Users:**
  - sub_824B2C90 picks the T33 in range (availability sub_824B2718: p1 != 0, free, radius vs CChar+304).
  - sub_824B31B8 (running into it: 12202/12212).
  - sub_824B3310 (thrown/whipped into it, from sub_82233D90: 12200/12207/12210/12217).
  - **sub_824B3480** (the grapple at the spot, from sub_822E35E8 via sub_824B3A88): picks key = p1
    (+variant), sub_8218F550(attacker, victim, key, 1), snaps both to the spot (sub_824B23F0 /
    sub_824B2810), vibration.
  - Prompt and AI: sub_824B2D28 (from sub_8231C398), sub_82295398.
- **This is the closest match to HCTP's whip-into-object:**
  - every T33 object reacts to whips and runs with the common 122xx pair motions;
  - a grapple plays the room key with the objects animated.
- **What it lacks:** spawning a weapon, removing collision, a state change persisting as a different
  object. The objects only play their tracks.

**T21 / T31 run-into-wall trigger** (GM #13, IS #173, PL/RL #143/#149/#118/#124; chamber glass in GMGA/0020):
- **Fields:**
  - p0 radius 10 (15 in the chamber);
  - **p1 key**;
  - p2 = 0/1/2 required running state vs CChar+1287 (2 = either);
  - p3 = 1/2 use limit (counter +116);
  - p4..p6 T23 objects (e.g. 1094/1095, the locker doors [L]).
- **Users:**
  - sub_824B6168: running into **0xDF or 0xED** walls (handler table above); key must be in the bank
    (sub_82394520). Vtable +72 = busy check, +60 = start(char).
  - sub_824B6410: prompt.
  - T31 = the same with its own +60..+72.
- **Fits HCTP's railing/pipes** "run or whip into it -> object falls".
  - The trigger is the wrestler himself running into the wall, not being whipped. A whip goes through
    T33 instead.

**T22 walk-into-wall trigger** (78PK #97 trailer door) [C]:
- Fields: p0 radius, **p1 key**, p2 flag.
- sub_824B6A70 (walking into 0xDF) -> sub_824B69E0 (key exists, not busy) -> sub_824B67D0: **JumpAct(char,
  p1)** and snap/turn the wrestler to the spot.
- **Usable for climbing**: put 0xDF triangles on the crate/cage side and a T22 with a climb key. The
  climb is then a one-wrestler motion from our bank.

**T24 weapon binder** [C init sub_824B9558]:
- Fields: p0 = mode/flag; **p1 key** played on the bound weapons (e.g. HW 12058, a 3-frame pose);
  **p2..p7 = weapon model numbers**.
- At init it grabs placed weapons of those models from the weapon table (0x821E2220, 72 slots), up to 6,
  and parks them at the locator.
- Examples:
  - PL/RL T24 [0 0 0 120 120 120 ...] takes the three model-120 weapons that rules 0x1D/0x1E place at
    (305, −10, ±290) (§5);
  - HW T24 takes the table (5) placed at (−20, −8.4, −225).

**T29 take-a-weapon spot** [C]:
- Fields: p0 = T24 record id; p1 = mode (1: pickup motion; 2: needs a scene check sub_824B9870(3));
  p2 radius; **p3 pickup key** (12320 PL/RL, 12360 HW/IS).
- The weapon pickup code (sub_821916C8, sub_8230A160 -> sub_824B0B20 -> sub_824B0690/0790) hands the
  next bound weapon to the wrestler:
  - JumpAct(char, key);
  - the weapon plays the key's track 1 (sub_824B91C0).
- This is **2011's only "weapon from an object" interaction**, a weapon that already exists in a rack.
  - Nothing creates a new weapon when an object is hit [L, not found].

**T25** (GM #18, PL/RL #148/#123: [0 14 11560 1 ...]):
- Fields: p0 radius 14; **p1 = main-bank move id 11560** (m.pac MOT/BM02 / MOTP/GAME: 4 tracks y0-y3,
  121/121/126/115 f); p2 = 1; p3..p5 = T23 records (objects 1/2, 127/128, 102/103).
- Init sub_824BB4E0 sets up the objects and a per-move table entry (sub_8229D588, −11500 offset). The
  update (sub_824BB950) checks 11560.
- So T25 = a grapple at furniture using a regular move whose y2/y3 tracks drive the objects [L].

**T34** (GM #16 [0 80 0 0 225 0 0]):
- Fields: p0 = radius (squared, with hysteresis); p3..p5 = T23 records.
- A counter (+128, +3 per frame up to 60) runs while any wrestler is within the radius, and drives
  sub_824BA7D8 (object alpha) [L]. This is HCTP's camera-occlusion fade.
- 78GM names record 225, which does not exist, so the record is inert in the shipped data.

**Other types:**
- **T30** (chamber [0 idx 1503 1504 1505 0]): pod index + 3 parts, paired with T21 glass breaks [L].
- **T20**: consumers sub_824B1BB0 (wall run-in near a T20: result 2 -> act 563), sub_824B1CC0,
  sub_824B2150 (p2 == char act). No shipped 2011 package has T20 (2008 data did).
- **T32**: sub_824B5320 -> sub_8220D270 with p1 (from sub_822DA3F8). Unused in the data.
- **T27/T28**: their constructors exist; no consumer was found; unused in the data.

**Mapping HCTP room-30 gimmicks onto 2011:**

| HCTP | 2011 approach | gap |
|---|---|---|
| gmAttackObjAndWeapon (cardboard 202, barrels 218: whip/run/strike -> collapse + spawn weapon) | T33 (whip/run reactions + grapple key, objects animate) | weapon spawn: hook, or pre-place the weapon hidden in a T24 and release it [G] |
| CGmThrowObject (railings 206, pipes 209: fall, remove collision, pipe weapon after 60 f) | T21 (run into 0xDF/0xED wall) or T33 | collision removal (the disable-by-attribute path exists in sub_826AB2E8 but is fed 255) and weapon spawn need hooks |
| CGmSwitch 208 -> cart 220 + shutter 201, lights 217 | none: no element triggers another; T22/T33 only play keys on their own objects | needs native code (a hook on the T22/T33 start) |
| CGmCrateAndWinch (moving cart with moving collision) | none: GMGB has no HMD, object collision only exists for weapons (TRIP from sub_823AC3B0) | native code, or a weapon-type collision object [G] |
| CGmComeOffObject roof (2 knock-downs -> falls) | none | native |
| CGmWireNetting walls (grab + mash -> break, cells block hits) | T21/T22 can play a grab motion; no mash/break/cell logic | native |
| valves + steam particles with hit reaction | gimmick packages have particles (/1 PRTC) but no damaging-particle type was found | native |
| GmWeaponPosRot trash cans 230/231, 50000 'w' lines | 45010 rule records (§5) | — |
| GmCharAction particle 63 | n/a | — |

---------------------------------------------------------------------------------------------------
## 5. Weapons (Q5) [C unless noted]

**The placement table.**
- `sub_8227D938` loads **entry 45010** (`li r4,-20526`) of the selected bgEtc STG set.
- `STG/WPON` (ring) and `STG/WPBS` (backstage) have **byte-identical** 45010s. The set only changes which
  weapon models are loaded (WPBS: 3, 4, 5, 9, 11, 15, 16, 17, 20, 27, 60, 63 ... 146, 150 + 40000/40001/41000).
- Format: 4-byte header, then chunks:
  - `SubH` {s16 rule, s16 n, s32 link};
  - n × `Data` {u32 36, f32 pos[3], f32 rot[3] (deg), s16 act, u8 place, u8 model}.
- s11_w45010.py decodes it. Backstage rules (world coordinates, y −8.4 / 0):

| rule | area [L] | weapons |
|---|---|---|
| 0x1B | parking | 65 fire extinguisher, 2× 63 trash can, 6× 17 tires stacked (act 15305) at (155-175, 0, −500..−525) |
| 0x1C | ? | 112 and 150 at (100, −8.4, −450/−475) |
| 0x1D | locker | 2× chair 4, 63, 112, **3× model 120 at (305, −10, −290)** -> the T24 rack in 78PL |
| 0x1E | locker (mirror) | same, at z +290 (78RL) |
| 0x1F | GL | chairs, 60 crutch, 63, 65 |
| 0x20 | hallway | table 5 at (−20, −8.4, −225) (T24 in 78HW), chairs, 90, 63, 65 |
| 0x21 | free roam | 12 items along the corridor |

- Placement code: sub_8227B938 (create) and sub_8227D130 (place), as in MATCH_TYPES_RESEARCH.
  - Models must be in the per-match load list *(0x82E3C0E8).

**The 500xx text files.**
- WPBS 50078 and WPON 50000/50054-50058 are Shift-JIS lists of model, pos, rot, ActNo, Place. 50078 has
  8 lines in **ring coordinates**: chairs, 15, 60, 89, 90, 112, 150.
- No code builds 50000+arena or loads 50078: no `li`/`addi`/`ori` constant for them. The `ori 50000` sites
  are unrelated, and sub_821C76B8 uses 50000-50002 as weapon ids.
- So the per-stage text list **is not read for arena 78**, nor for other arenas [L].
- To add room-30 weapons (trash can 63/9, lid, cardboard box, barrel, lead pipe): add `Data` records under
  the backstage rule used for the new room. They can be patched in at run time the way match_types.cpp
  swaps the {count, records} entry.
  - The models must exist in WPBS. HCTP's 56 cardboard, 57 barrel and 148 lead pipe have no 2011
    counterparts by number; map them to 2011 weapon models.

**No gimmick creates a weapon.**
- T24 binds already-placed weapons; T29 hands them out; T33 variant 7 uses weapon 150 as a prop.
- An HCTP-style spawn (cardboard/barrel/pipe) needs a hook that creates a weapon at run time [G: find
  2011's MakeWeapon from sub_8227B938's create path].

---------------------------------------------------------------------------------------------------
## 6. Suggested port route (from the above)

1. **Geometry.**
   - A new bg78 area HMD pair (or replace one area): floors at y 0 / −20.5 / −30.3 as flat triangles.
   - Walls with 0xED for room walls.
   - 0xDF on faces that should trigger something.
   - Rebuild the cell index.
2. **Climbing.**
   - 0xDF faces + T22 (walk) and T21 (run) records next to them, with climb keys converted into our GMGB
     bank (free key range).
   - Or hook sub_82306B10 for 0xFB/0xFD/0xF4/0xDB and call JumpAct with our keys.
   - Verify in game that y stays on the upper floor after the motion [untested].
3. **Whip/run-into props** (cardboard, barrels, railings, pipes, crates): T33 records with p6 class + a
   grapple key, objects as T23 with idle keys (w5).
4. **Weapons at start:** 45010 records for the room's rule.
5. **Native work (hooks)** for:
   - weapon spawn on prop break;
   - button -> cart + shutter;
   - cart moving collision;
   - roof break / falling through;
   - cage wall mash;
   - steam damage;
   - jump-down (12572);
   - climb-out (12092).

---------------------------------------------------------------------------------------------------
## Helper scripts (this folder)

| script | what it does |
|---|---|
| `s11_db.py` | builds `s11_db.pkl` = {func: [(addr, asm)]} from the ReXGlue output |
| `s11_q.py dis <addr> [from to]`, `grep <re>`, `calls <addr>` | disassembly, search, callers |
| `s11_ctx.py <re> [before after] [funcs]` | matches with context |
| `s11_img.py vt/f/s` | vtables, floats, strings from port/runs/image.bin |
| `s11_vt.py` | vtable per element type (resolved from the constructors) |
| `s11_hmd.py <bg.pac> [attr|all]` | HMD triangles with word22/23 |
| `s11_attrs.py` -> `s11_attrs.txt` | attribute census over every bg*.pac |
| `s11_gmgb.py [names]` -> `s11_gmgb_all.txt` | GMPD records with values and locators for all GMGB/GMGA packages |
| `s11_motidx.py` -> `s11_motidx.pkl`, `s11_mq.py <id|lo-hi>` | which banks hold a motion id |
| `s11_wpbs.py`, `s11_w45010.py [v]` | bgEtc weapon sets and the 45010 placement table |

Extra text dumps:
- `s11_82306B10.cpp`: the switch with its case table;
- `s11_d_5910.txt`: the collision driver;
- `s11_d_EB88.txt`: the ground-height function.
