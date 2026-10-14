# HCTP (SLES_520.36) backstage gimmick system: reverse-engineering notes for the SvR2011 port

Tags: **[C]** confirmed from code/data, **[L]** likely, **[G]** guess.
Addresses are EE virtual addresses in SLES_520.36. Helper scripts in this folder (see the end).
Room numbers are **decimal**: GetNowStage() returns 30 (0x1E) for stg_0030 "Arena Back Entrance"
(`ST30CageOn`, `CellCheck_BackOpenSpace` etc. compare against 0x1E) [C]. In the code room 30 is called
"BackOpenSpace" (BOS) [C].

---------------------------------------------------------------------------------------------------
## 0. Summary of the pipeline (answers Q1 in short)

1. `CGmControl::__ct` (0x190b60) → `AllLoadData` (0x1910f0) reads the room's BG.PAC stage package:
   * entry **50100** (0xC3B4) → `SetGimmickLocate` (0x191270): sub-entry 10000 = placement records
     (u32 count, 12 bytes pad, count × 32-byte `GmObjLocate`) copied to CGmControl+0xC/+0x10 [C].
   * entry **50101** (0xC3B5) → `SetModelData` (0x191330): sub-entry 0 = TIM2 bank; sub-entries
     **1000..1499** = YOBJ models → `CSkinModel` at CGmControl+0x818+4*(id-1000); **2000..2499** = HMD
     collision blobs at CGmControl+0x14+4*(id-2000); **3000..3009** = misc blobs (vector at +0x17B8) [C].
   * entry **50102** (0xC3B6) → `SetMotionData` (0x1917a0): whole file → `CEKey` (object/prop motion
     bank) at CGmControl+0x17F8 [C].
   * entry **50103** (0xC3B7) → `SetParticleTbl` (0x191830): u16 count + 0x10 header, count × 0x7C
     records copied to CGmControl+4 (count at +8) [C]; no reader found (see §6) [L unused].
2. `CGmObjectData::AllLoadData` (0x1b1a20) loads the **gimmick parameter table** from GM.PAC **GMMC_GOBJ**
   (`ObjectsLoad` 0x1b1ad0): s = `GetGmPacNo2Stage(arena)` = arena−29 for arenas 30..36, else 0;
   entries 10+s (GMPD head), 20+s (tags), 30+s (floats); particles from entry 101 (GamePart==1, in-game)
   or 100 (`ParticleLoad` 0x1b1c50) [C]. Then it calls `CGmControl::CreateGimmick` [C].
3. `CreateGimmick` (0x191930): for every placement record, `t = (s16)rec+0xC & 0x7FFF`:
   * 100 ≤ t < 1000 → `CreateObject(t, rec)` (gimmick object) [C]
   * otherwise (t < 100 or ≥ 1000) → `CGmBaseParticle::CreateParticle(t, rec, particleMgr)` (only when
     GamePart==1) [C]. **So placement types < 100 are particle emitters, not prop models** (e.g. room 30
     t32/t34 are the two steam jets, t63×3 the GmCharAction particle pool) [C].
4. `CreateObject` (0x1b2090): `SearchObjectParam(t)` finds the GmTag {s16 id, s16 kind, s16 nfloats,
   s16 floatIndex} (binary search by id) and its float array; `kind` selects the C++ class through the
   jump table at 0x4641D0 (kind 3..98; out of range/unused = plain `GmObjectBase`); then
   `SetLocateData` copies pos/rot and the id into the object (obj+0 = id, +0x10 pos, +0x20 rot) [C].
   Every GmObjectBase reads **param[0] = model id** (1000+n → CGmControl model table) and keeps
   `p = &param[1]` at obj+0x44 [C]. All further params below are given as p[0], p[1]… (i.e. after the model).
5. Gimmicks talk to each other only by **id**: `CSetGimmickActiveFlag(0, id, bool)` (0x225d60) sets the
   active flag of every live object with that id; `CGmActionCtrl::GetGimmickIndex(id, n)` (0x198260)
   iterates instances; `CSetParticleBirthFlag(0, particleId, bool)` turns particle emitters on/off [C].
   An id with no placement record has no instance, so messages to it are no-ops (room 30: 200, 204, 205,
   211, 214, 216, 219, 223-226 are never placed) [C].

---------------------------------------------------------------------------------------------------
## 1. Data formats

### 1.1 Placement record `GmObjLocate` (50100/10000.bin, also GMEV 10000) — 32 bytes [C]
```
+00 f32 x, y, z          world position (Y negative = up)
+0C s16 type             low 15 bits used (SetLocateData: lh, <<17>>17); bit15 = flag (GMEV uses 0x8400)
+0E s16 hi               0 in BG.PAC rooms
+10 f32 rx, ry, rz       radians
+1C u32                  0 (unused in rooms)
```
File: u32 count, 12 bytes zero, then records [C].

### 1.2 GMMC_GOBJ (GM.PAC) — gimmick parameter tables [C]
* 000(10+s).bin: `"GMPD"`, u32 nTags, u32 nFloats, u32 0.
* 000(20+s).bin: nTags × {s16 id, s16 kind, s16 nFloats, s16 firstFloat}, sorted by id.
* 000(30+s).bin: nFloats × f32 (ints stored as floats).
* 00100/00101.bin: particle definitions (header u16 ?, u16 count, u32 size…, 0x10 bytes; then count ×
  96-byte `ParticleData`); 00101 (65 entries) is the in-game table [C].
* s: 0 = default/ring (ids 500-581), 1 = room 30, 2 = 31, 3 = 32, 4 = 33, 5 = 34, 6 = 35, 7 = 36 [C].
Full dump of all eight tables with class names: `gobj_dump.txt` (made by `gobj_dump.py`).

### 1.3 50102 / M.PAC MOT banks — `CEKey` motion banks [C]
`CEKey` = up to 8 bank pointers; each bank (`Search` 0x168dc0, `GetData` 0x168f00):
```
+000 0x100 bytes   per-bone/track flags (mostly 0; 50102 has byte 0xC0 = 2)
+100 u32 n
+104 n × 16 B      {u32 key, u32 dataOffset, u32 frameCount, u32 0}   sorted by key
                   key = act<<16 | sub1<<8 | sub2  (top 2 bits masked)
data               offset from end of the table; per-frame byte stream: b<0x60 = skip b frames,
                   b==0x60 = escape (next byte is the length), b>0x60 = (b-0x60) data bytes follow
```
`JumpAct(obj, act, sub1, sub2)` (0x1c08a0) plays key (act,sub1,sub2); `JumpAct(obj, act)` = (act,0,0).
Gimmick models are created by `CGmModel::CreataGimmickModel(model, a, b, useStageKeys)` (0x26e620) with a
`CSD3MotionWeapon` motion link (same player as weapons); if the 4th flag is set the model reads the room
bank 50102 (CGmControl+0x17F8) [C]; otherwise it uses the global banks.

* **50102 (room 30)**: object motions 23..33 (frames): 23 pipes idle (1), 24 pipes fall (41),
  25 shutter open (26), 26 drum stack fall (72), 27 dustbox fall (40), 28 railing fall (32),
  29 crates fall off cart (53), 30 shutter close (31), 31 crates idle (1), 32 cardboard idle (1),
  33 cardboard stack collapse (42) [C ids/frames; meaning C from the param tables below]. `ekey.py` lists it.
* **M.PAC → MOT → GAME** is a PAC of 7 CEKey banks (ids 0,1,2,3,4,6,7) [C]. **Bank 4** holds all backstage
  character + object "act" motions 12000-12801, 15000 (weapon idle), 15440 (door), 15916 [C];
  bank 0 holds generic acts incl. 256/262/263/340/660/664/679 [C]. Sub-key convention: (act,0,0) =
  wrestler, (act,0,1) = partner/object, (act,0,2) = second object; e.g. 12036 (0,0)=27 f wrestler turns
  valve, (0,1)=23 f valve model; 12045 (0,1) = cage wall shake anim; 12034 (0,0)/(0,1)/(0,2) =
  attacker/victim/crate [C keys, L roles]. Other MOT groups: BRAP, TAG, CAGE (steel-cage match acts
  1550-1562, 4460-4477), CHAR/IYMP (no CEKey header), ALL, MARK; WPON PROP/KAGE [C].
Backstage acts used by room 30 (frames from bank 4): 12014 hit-object reaction ×6 variants (32/98/39/64/
73/33), 12016 (39), 12021 (36), 12027 (130/136), 12028 (15), 12030 (1), 12031 (2), 12032 (88/90),
12034 (133/170/171), 12035 press button (24), 12036 valve open (27/23), 12037 valve close (46/33),
12038 (24), 12043 crate climb-out (198/86), 12045 cage-wall grab (36/36), 12046 cage-wall break (35/40),
12047 steam burn reaction (103), 12060 wall release (1), 12089 backstage throw (42/77/42),
12092 climb out of broken cage (39), 12518, 12523 (86), 12572 jump down (90); generic 256/262 climb
(4 variants), 263 climb wall/ladder (54), 340 hit-object (32), 660 fall/drop (8 variants), 664 (32).

### 1.4 HMD collision (00999.hmd and 50101/20xx.hmd) [C]
`"HMD "`, u32 n, n × 96 bytes = 24 floats: rows 0-3 a 4×4 frame (`M[0][3]`,`M[1][3]` hold the x-range),
`M[3][0..2]` = translation, floats 16..21 = the triangle's three (a,b) in-plane coords, **word 23 = u32
attribute** (low byte → CChar+0x17F/0x17B "touched attribute", byte1 = material/second attribute) [C]
(decoder = `tri_corners` in port/tools/svr08_stage.py; same as SvR2011).
Room 30 attributes (world space):
| attr | where | meaning |
|---|---|---|
| 0x200 (byte1 2) | 8 floor tris y=-30.3, x 20..89.5, z 19..59.5; also 2060 roof panels | cage roof [C pos, G material] |
| 0xDF | walls y 0..-30.3 at z=19.1 x 22..38; 2 tris in each cage panel 2063/2066 | **climbable wall → act 263** (climb onto cage roof) [C] |
| 0xFB | wall y 0..-20.5, x -76..-56, z 26..47 | **climb crates (floor → crate top -20.5)**, act 256 sub 1 (walk) / 664 (run) [C] |
| 0xFD | wall y -20.5..-30.3, x -85..-65, z 34..55 | **climb crate top → upper walkway**, act 256/262 sub 2; with weapon 15740/15750 [C] |
| 0xF4 | wall y 0..-13.2 at z=57, x -51..-41 | **ladder**: snaps to (-46,0,55), act 263; `GmLadderCheck` blocks a 2nd wrestler [C] |
| 0x900 (byte1 9) | floor y=0 x -48..18 z -18..48; railing 2050 | floor material [G] |
| 0xDB | 2103 (inner box of the broken roof halves) | "inside cage" wall: push stick into it + attack → act 12092 climb out (§3.4) [C act, L button] |
| 0xFF08FF | drum stack 2069 | material [G] |
Floor levels in 00999: y=0 ground, y=-20.5 crate top (2 tris), y=-30.3 upper walkway/cage top (25 tris) [C].

### 1.5 50000.txt (stage objects, `MakeStgObj` 0x22cf30) [C]
`{id, ?, (x,y,z), (rx,ry,rz deg), act, c}` parsed with sscanf; `c='w'` → `MakeWeapon` with weapon no `id`
(weapon idle act 15000); `c='m'` → stage object made through the weapon/stage-model table (stage
model file = 1000 + id*10) plus `MakeCGate4` → door/gate objects [C code path, L meaning].
Room 30: `w 8` (lid) at (-78,-31,85), `w 9` (trash can) at (-43,0,13); `m 201` (88,0,-4.5) ry 90 [L ramp
exit], `m 202` (74.25,-30,93.49) ry 45 = 03020.yobj door_Training (upper level), `m 204`
(-74.96,-30,106.5) act 15916 = 03040.yobj door_Parking (upper level) [C files, L exits].
### 1.6 50001.bin, 50005.txt [C from the files' own comments]
* 50001: per room-model flags, `id flags ;comment`: n near-clip, w only in move-camera, s/a/k
  translucency kinds, z no Z-write, r/y mirrored copies, e event-only, m/l motion/light motion.
  Room 30: 1 n (body), 2 n w (front wall), 3 n w (ceiling), 4 n (black screen cover), 11 w, 30 k, 31 s,
  40 a z (lights), 41 a z (floor shine), 42 w.
* 50005: camera zones, one per line: area X Y Z, radius R, camera position XYZ, third XYZ
  (look-at [L]). Room 30: 金網前 front of the cage, 中央 centre, 機械左/右 machinery left/right (upper
  level, y=-31), 網の下 under the net.

### 1.7 GM.PAC GMEV_xxxx [C]
`CGmControl::CreateEventGimmick` (0x191b30), called only from `CSd5EventSequence::ActionWaitGimmickReady`
(story/season cut-scenes): 10000 = `CGmLocateData` (same 32-byte records, types e.g. 509/510 = default
table), 10001 = small YOBJ models, 10002 = TIM2 pack. **Not used by backstage brawls** [C]; ignore for the
port. `IsEventOk` (0x1b3160) only disables ids 0x136/0x13E/0x154/0x19F/0x1A9 during specific events.

---------------------------------------------------------------------------------------------------
## 2. Gimmick classes and every room-30 type

CreateObject kind → class (jump table 0x4641D0) [C]; full list in `jt_out.txt`. Room-30 relevant:
36 CGmShutObject, 37 CGmSwitch, 39 CGmSwitchOnOffParticle, 44 CGmShutter, 46 CGmComeOffObject,
47 CGmWireNetting, 49 CGmSwitchLight, 51 CGmThrowObject, 54 CGmCrateAndWinch, 8 CGmDispWeaponParam,
88 CGmCollision, 89 gmAttackObjAndWeapon, 94 GmCharAction, 95 GmWeaponPosRot, 90 GmReflect.

Common bases [C]: `CGmBase` (0x198340; +0 id, +4/+5 active/visible, +0x10 pos, +0x20 rot, +0x30 locate);
`GmObjectBase` (0x1b1ed0; +0x40 tag, +0x44 p, +0x48 model id, +0x4C CSkinModel, +0x50 ymp);
`GmAttackBase` (0x1992b0; +0x60 attack mask, +0x64 wrestler reaction act, +0x6C 60-frame cool-down,
+0x70 triggered, +0x74 attacker). `GmAttackBase::CheckAttack` (0x199370) → `CheckCharaAttackAll(coll,
mask)` (0x199630) → `CheckAttack(CChar,coll)` (0x199910) returns attack bits:
**1** strike (`Dageki`), **2** dash/whip (`Dash`/`WeaponDash`/`MotBkStgThrow` = act 12089 with
CChar+0x24B==1 is the backstage whip-throw) as attacker, **4** being whipped (same with +0x24B==1, or act
0x294) , **8** running strike (`DashDageki`), **0x10** weapon strike [C]. First hit → `EventStart`
(vtable+0x38). Reaction: if p-act == 12014 → wrestler `JumpAct(12014, rand%5+1)` + vibration, else
`JumpAct(act)` (skipped if he is striking/dash-striking) [C].

### 2.1 Room 30 (stage index 1) — every tag, decoded

| id | placed at (records) | class | params (model; p[...]) | in game |
|---|---|---|---|---|
| 200 | – | GmObjectBase | 1234 1236 2055 2061 0 24 1243 | unused leftover [C not placed] |
| **201** | (-15,-14,61) | CGmShutter | model 1236 syattar; p0=25 open act, p1=34, p2=48 (vanish 14 frames ending at 48), p3=1265 syattar2 (closing model), p4=30 close act | garage shutter. Activated by switch 208 → plays 25; `CloseShutter` (called by 220 240 frames after the cart is done) plays 30 on syattar2; at its end re-activates 208, clears switch lights 217, re-arranges 220 and 201 [C] |
| **202** | (35,0,-15) ry 60° | gmAttackObjAndWeapon | 1243 danboru; p0=12 attack mask (whipped 4 + running strike 8); p1=2061 moving collision; p2=33 object act; p3=12014 wrestler reaction; p4=226 weapon-spawn tag; p5,p6=-1 particles; p7=0 infinity flag; p8=32 idle act | **cardboard box stack**: whip/run an opponent into it → stack collapses (33), victim 12014 random variant, **cardboard box weapon 56** created at 226's position (40,0,-10) with act 15000 [C]; fades when a wrestler is in x 20..50, z -20..20 (alpha 0.3, camera occlusion) [C] |
| **203** ×4 | (20,0,49.7) ry90, (20,0,29.7) ry90, (50,0,19.75), (70,0,19.75) | CGmWireNetting | 1242 ami; p0=2063 wall collision (0xDF climb faces), p1=2064 grab zone | **chain-link cage walls** (left side x=20 = instances 0,1; front side z=19.75 = 2,3), see §3 [C] |
| 204 | – | CGmThrowObject | 1244 dustbox; 2059 340 27 58 30 … | dust bin (not placed; target of valve 214, also not placed) [C] |
| 205 | – | CGmSwitchOnOffParticle | valve, particle 30 | not placed [C] |
| **206** ×2 | (-23.6,-30,64.5) ry180, (5.5,-30,64.5) ry180 | CGmThrowObject | 1232 fence; p0=2050 collision; p1=340 reaction act; p2=28 fall act; p3=42/p4=30 vanish; p5/p6=-1 | **upper-walkway railings**: whip into them → railing falls (28), vanishes after 42+30 frames, collision removed (no more barrier) [C] |
| **207** | (-68.9,0,11.8) ry -90 | CGmShutObject | 1233 kizai_box; p0=2053 coll A, p1=0 init act, p2=12031, p3=12030, p4=12032, p5=12034, p6=12043, p7=2054 coll B | **open equipment crate**: Irish-whip (`CheckHammer`/`CheckHammerObjNear`, act 800 frames 50..56) the opponent into it → crate + both wrestlers play 12034 (sync, `JumpActDouki`) or 12032; victim hidden inside; at frame 420 crate+victim play 12043 (climb out); ready again at 901 [C acts/frames, L naming] |
| **208** ×2 | (11.5,0,60) ground, (-53,-31,105.1) upper | CGmSwitch | 1235 switch; p0=2056 zone; p1=12035 press act; p2..4 = 220, 201, -1 | **green button panels** (one per level): stand in zone (r 5), press the action button (CChar+0x1DA bit 0x80) walking/running toward it within ±30° → wrestler 12035 snapped/turned to the panel; activates 220 (cart) and 201 (shutter); lights 217 on; all 208 disabled until the cycle ends [C] |
| **209** | (22,-30,104.5) | CGmThrowObject | 1234 paipu; p0=2051; p1=340; p2=24 fall; p3=60,p4=30 vanish; p5=211; p6=-1; p7=23 idle; **p8=148 weapon, p9=4 bone, p10=60 delay** | **lead pipes** (upper level): whip into them → pipes fall (24); after 60 frames **lead pipe weapon 148** spawns at model bone 4 [C]; also activates 211 (unplaced, no-op) |
| **210** | (-9.6,-31,105) | CGmSwitchOnOffParticle | 1238 barubu; p0=2057 zone; p1=12036 open act; p2=12037 close act; p3..5=32,-1,-1 particles; p6..8=-1 | **valve, upper level** → steam particle 32 (placed t32 at (60.2,-45,98.4) ry -135°). Special case id 210: turning it also triggers **barrel stack 218** (calls its EventStart if not yet fallen) [C] |
| 211 | – | default | 0 148 60 0 0 | unused [C] |
| **212/213** | (40,-30.59,39.74) / (60,-30.59,39.74) | CGmComeOffObject | 1241 nukeami2; p0=2060 roof panel collision (19.5×19.5, attr 0x200); p1=221 / 222 | **breakable cage-roof halves**, see §3.2 [C] |
| 214 | – | valve (particle 33, activates 204) | not placed [C] |
| **215** | (-33.5,-31,105) | CGmSwitchOnOffParticle | as 210, particle 34 | **valve, upper level** → steam 34 (placed t34 at (-54.3,-13.3,55) ry 110°) [C] |
| 216 | – | CGmWireNetting 1245 ami2 2066 2067 | not placed [C] |
| **217** ×2 | same spots as 208 | CGmSwitchLight | 1246 switch_light; p0=0, p1=3001, p2=3000 (CMotionLight slots 0x24/0x25) | button lamp: translucent model drawn with "on" ambient (g_GameLight+0x2C0) when 217+0x68=1 (set by switch), else "off" (+0x2D0) [C] |
| **218** | (45,-30,76) | gmAttackObjAndWeapon | 1252 drumcan; p0=13 (strike 1 + whipped 4 + run 8); p1=2069; p2=26 fall act; p3=12014; p4=224 weapon tag; p8=0 | **barrel stack** (upper level): hit/whip/run into it or turn valve 210 → falls (26); **barrel weapon 57** at 224 (45,30→-30,76); attacker extra `JumpAct(12014, 0 or 4)`; fades when a wrestler is at x 35..53, y -31..-29, z 80..120 [C] |
| 219 | – | CGmDispWeaponParam | 0 0 0 | would spawn weapon p0 after p1 frames when activated (220 activates it) — not placed → no-op [C] |
| **220** | (-15,0,90) | CGmCrateAndWinch | 1264 hako (crates); p0=1262 daisya (cart), p1=1263 goo_shd (shadow), p2=2062 moving collision, p3=29 break act, p4=31 idle act | **cart with crates**: see §2.2 [C] |
| **221/222** | = roof halves | CGmCollision | p0=0?, p1=2103 (attr 0xDB box ±8.2, 16.5 high) | inactive until the matching roof half breaks (activated by 212/213), then always added to stage collision [C] |
| 223 | – | barrel stack (weapon 225) | not placed [C] |
| 224/225/226 | – | GmWeaponPosRot data | 57 / 57 / 56 | weapon-spawn descriptors read by 218/223/202 (`GmWeaponStatus::Set` 0x29a810: no, act, sub1, sub2, pos xyz, rot xyz, int, flag) [C] |
| **230** | (-93.3,-30,98.2) | GmWeaponPosRot | weapon 9, act 15000, pos (-93.29,30,98.15) | **trash can** on the upper level, created on the first frame (flag p11=0) [C] |
| **231** | (88,0,13) | GmWeaponPosRot | weapon 9, pos (88,0,13) | **trash can** on the floor [C] |
| **580** | (0,0,0) | GmCharAction | 0 4752 30 4753 30 4788 190 4 0 0 -5 0 0 0 63 | in every room: when any wrestler is at (act,frame) = (4752,30),(4753,30),(4788,190) spawn particle 63 at bone 4 + (0,0,-5) rotated by his ry [C] (generic move effect, not a backstage prop) |
| 900 | – | GmReflect | | not placed [C] |
| t32, t34 | particles | steam jets for valves 210/215 [C] |
| t63 ×3 | (10/20/30,0,0) | particles | pool of particle 63 for GmCharAction (`ParticleEventStart` picks a free one) [C] |

Note on weapon Y: GmWeaponPosRot stores y = +30 for upper-level items while the matching props are at
-30 and MakeWeaponGame copies y unchanged into CWeapon+0xE4 [C]; use -30 in the port [L].

### 2.2 CGmCrateAndWinch (220) detail [C]
* ctor 0x2a6c20: 3 models (crates/cart/shadow). `GmInit`: move-collision 2062; `InitArrangement`
  (0x2a7190): crates model act p4=31, all 3 models visible, `CObjMove::Set(start = locate (-15,0,90),
  dest = (-15,0,-11), ratio 0.05, scale 4, 20)`, 0xC4 = 240, 0xC0 = 1 (moving).
* Active only after switch 208. Each frame: models + collision follow the CObjMove position (eased
  toward z=-11, i.e. the cart rolls **out of the garage** at z≈90 past the shutter at z=61 into the room).
  While moving the collision blocks; contact check: at destination `CheckCollisionChar_NotMuteki`, else
  `CheckCharaAttackAll(coll, 0xF)`. On contact: hit wrestler `JumpAct(340)` (or partner-sync handling),
  activate 219, models vanish (cart/shadow 60..160, crates 60..130), crates act p3=29 (fall off),
  collision kept 100 more frames.
* After that it waits 240 frames, then `CloseShutter` on every 201 and deactivates itself; the shutter
  close end re-arms everything (`InitArrangement` again).

### 2.3 Other rooms (types 300-711) — class map [C], meaning [L]
| room | ids → class |
|---|---|
| 31 parking | 300 cfg(kind55), 310/318 CGmBrokenCar (car parts 1269-1274/1326-1331, colls 2090-2092/2099-2101), 311 CGmPowerSupply, 312 CGmMoveCar (+313-315 path points kind 74), 317 gmHammerThrowAtk, 319 CGmThrowObject 1239, 320 CVehicleForkLift, 321 CForkliftBoxSupply, 330 GmSkBo (skateboard; 331/332 cfg), 340 CGmThrowObjectHIdden, 341 CGmThrowObjectRepeat |
| 32 training | 400 GmLocker, 405 gmToilet, 410 gmBodySoap, 415 gmSandBag, 420 gmShower, 425 gmTrngMch, 430 gmBrokenWall, 435 gmTVClash, 440/445/467 gmRackWithObj, 460 gmAttackObjFade, 465/466/468 gmAttackObjWeapon, 469 gmWeaponHitEvent, 475 CGmDispWeaponAppointMotion |
| 33 Times Sq. | 512 GmHeli, 515-530 GmCarMany (+GmObjectBase anchors), 531 kind60 path, 540-543 car cfg (kind14), 544 gmWeaponHitEvent, 550 GmShowWindow |
| 34 outside A | 600 GmSkBo, 610 GmSyoukakiControl (fire extinguisher), 630-635 GmSkBoAddSpeed, 640-650 cfg, 660-667 cfg, 670 gmWeaponTrackGas, 671 gmWeaponTrackDrum, 680/681 GmCarMany |
| 35/36 outside B | 700/701 CGmRion, 702/709 CGmFloodObservation, 705/706 CGmFountain, 707 CGmTower, 710/711 GmFountainThrow, 703/704/708 cfg |
(Exact params: `gobj_dump.txt`.)

---------------------------------------------------------------------------------------------------
## 3. The Back Entrance cage (Q2)

### 3.1 What is *not* this cage [C]
`CCage` (0x21a380, `cage_pos`, `yureCage`, min/max x/z), `CMot::Cage` (acts 1550-1562), `CMot::CageUp`,
`CMot2::CageOchi` (needs `GetRule()->cage`), `CHA_CageMoveALL` (4460-4464), `CComCmd_ClimbCage`,
`CComCmd_ChamClimbCage`, `CComCmd_Attack2Cage`, `CComMacro_ClimbCHECage`, `CComMacro_Walk2CageClimbPos`,
`GameVariables::open_cage/open_cage_up` (used in `HicDrop` with `rectHicOpenL/R` = Hell-in-a-Cell roof
holes), `HicInTheCell` → all belong to Steel Cage / Hell in a Cell / Elimination Chamber matches, not room 30.

### 3.2 Room-30 specific code [C]
* Walls = 4 × `CGmWireNetting` (203). `MyAction` 0x2a63f0: wall collision 2063 always solid. If not
  broken/grabbed and a wrestler stands in grab zone 2064 (r 5), no weapon, presses the action button
  (CChar+0x1DA & 0x80), has a stick direction within ±@10335 of the wall's ry and is walking/running →
  wrestler + wall play **12045** (grab & shake), wrestler snapped/turned; then every frame the button
  bit is set the counter +0x70 rises, **≥ 11 → `BreakNet`** (mash ~11 presses [L]); if he stops and
  ≥181 frames passed he lets go (wall act 12060). `BreakNet` (0x2a69d0): wall act **12046**, wrestler
  act 12046, wall vanishes (50..150), wall collision replaced by **2120** (only the top bar, y -25.5..-30.5),
  partner panel broken too (pairs 0↔1, 2↔3). AI: `CComMacro_ST30CageShake` mashes button 0x80 while in
  12045; `CAroundInfo::ST30CageOn` (0x2c2c20) tells the AI a wall can still be shaken.
* Cells: `CGmWireNetting::CheckChar/CheckPos` (instances 0 and 2 only) write per-wrestler and per-weapon
  `GM_BOS_CELL` (`SetCheckCell` → GmGameFlgSD5+0xA8): 0 none, 1 / 2 = inside / outside the cage (from x/z
  vs 20, ignoring y < -20 and x/z < -10); wrestlers in 12045/12046 forced to 1. `GmAtariCheck`
  (0x2a7500, from `CCharAtari::Ateta`) **cancels hits between wrestlers in different non-zero cells**;
  `GetWeapon` uses the weapon cell so you cannot pick up through the fence [C].
* Roof = 2 × `CGmComeOffObject` (212 left, 213 right) at y=-30.59, each a 19.5×19.5 panel (collision 2060,
  attr 0x200) added to the stage collision every frame while intact; the rest of the cage top is static
  floor in 00999 (y=-30.3, attr 0x200) [C].
  **Damage counter** (`MyAction` 0x2a5990): for each wrestler with `GmGameFlg::IsDamage` this frame, who is
  falling (`IsOtiruAct`) or down (`Down`), with y < -3.9 (on a high level) and inside the panel's
  bounding sphere, and the panel not already shaking: count+1; **count < 2 → panel shakes 18 frames
  (`CGmModelShake`)**; **count reaches 2 → `ComeOff`** (0x2a5b20) [C]. So the 2nd knock-down on a half breaks
  the roof. `ComeOff`: activates p1 (221/222 = collision 2103), `CGmModelFall` 50 frames, vanish 20,
  sets `GmGameFlgSD5::SetLeftDrop` (212) / `SetRightDrop` (213) and also breaks the other half (both halves
  always fall together).
* Falling in: `CellCheck_BackOpenSpace` (0x260610; via `HicDrop` → `HicDrop_BackStage` when a wrestler
  is hit) — wrestler not in 0x294/0x29B/0x107/0x311C, not down, y ≤ -20, x ≥ -50, z ≤ 57:
  - walkway edges: x<18 (and x>15 → x-=7, GRAV vx=-0.1; z>55 → z-=7, vz=-0.1) or z<20 (z-=7, vz=-0.1);
  - **LeftDrop** set, y < -28 and 30<x<50, 30<z<50 → nudge ±7 toward the centre (vx/vz ±0.4);
  - **RightDrop** set, 50<x<70, 30<z<50 → same;
  then `MakeDrop` (0x1e6ea0): wrestler `JumpAct(0x294 = 660)` (fall), sync partners released (`EtcDown`/
  `JumpFight`). Gravity takes him to the floor y=0 inside the cage [C].
* Climbing out: `CaseFight` (0x1f35a8): touched wall attribute **0xDB** (collision 2103) + human player
  with stick pushing into the wall (dot < -0.5) → `JumpAct(12092)` + `CMuku` (turn) [C]. CaseFight is the
  strike-button case [L] → matches the FAQ's "Left + Square at the wall". Caveat: 2103 decodes to a
  ±8.2 box from y -30.6 to -47 (above the roof plane) at the roof-half positions [C data]; how a wrestler on
  the cage floor touches it is not resolved — port as "inside-cage walls with attr 0xDB" [G].
* Height constants: roof y = -30.59; "high" tests use y < -3.9 (roof damage), ≤ -20 (walkway/cell),
  < -28 (on roof), ST30_EnemyOnTheHigh: self y > -1 and enemy y < -20 [C].

### 3.3 Climbing onto the roof
Not a special gimmick: walking/running into a **0xDF** wall face (front of the cage / cage panels) →
`CaseDash` (0x1ef44c chain) → `JumpAct(263)` + `SetClimbRot` (face the wall) [C]. Act 263 (54 frames,
bank 0) is the generic climb-up used for ladders too [C]. AI uses `CMovesInfo::CanGoUp` (0x2c4380).

---------------------------------------------------------------------------------------------------
## 4. Two levels (Q3)
* Purely **collision geometry** in 00999.hmd: floors y=0, y=-20.5 (crate top), y=-30.3 (walkway + cage
  top) [C]. No area-cell code for height; `GM_BOS_CELL` only separates inside/outside the cage [C].
* Up: wall attributes in `CaseDash` (0x1ef44c..0x1f03d8) [C]:
  0xFB crate side → act 256 sub 1 (walk) / 664 (run), `ClimbOK` gate; 0xFD crate top → walkway → act 256
  sub 2 / 262 sub 2 (run), carrying a weapon → `JumpActDoukiWithWeapon` 15740/15750; 0xF4 ladder → snap
  to (-46,0,55), act 263; 0xDF cage wall → act 263. (Other attributes handled there for other rooms:
  0xFE/0xFC/0xE5 → 256/262, 0xDD → 12016, 0xDC → 12523, 0xF6 → 1671, 0xF7 → 679/256, 0xF0-0xF3/0xED/0xEE
  → 265-271/677/664, 0xE6/0xE7/0xE8 → 12517/12516, 0xE3 → 12021/12027, 0xE2 → 12028, 0xDE → 12038,
  0xE0 Times-Square ladders 12069/12070, 0xE4 flood.)
* Down: (a) `GapIn_BackOpenSpace` (0x1d63f0, via `CPossible::GapInBkStg` in `CaseFight`): y ≤ -28 and
  near an edge (x<17 & z<70, or x>20 & z≤55 by stick direction) → `JumpAct(12572)` + `SetGapPos` (x=23
  or z=63/23) = jump down [C acts, L meaning]; (b) knocked off the edge → `CellCheck_BackOpenSpace` →
  `MakeDrop` (§3.2); (c) through the broken roof.

---------------------------------------------------------------------------------------------------
## 5. Weapons (Q5)
* Weapon number n ↔ stage file **1000 + n*10** (+variant) in the room package (`CStage::__ct` loop
  0x21f224: entries 1000..3199 → `g_pStage+0x9EC[]` model, +0xA6C[] = n*10+v; 10000+.. collisions) [C].
  `MakeWeapon(model,-1,no,..)` (0x20a250) searches that table; `GetWeaponType` = `CWeaponProp::m_nKind[no]`
  (no < 171) [C].
* Room 30 files: 01080 n08 → **8 trash-can lid**, 01090 n09 → **9 trash can**, 01560 k0056 (danboru tex)
  → **56 cardboard box**, 01570 k0057 (drum tex) → **57 barrel**, 02480 k0148 (long pole) → **148 lead
  pipe**, 02000 kyouki_shd = weapon shadow, 03020/03040 doors (n 202/204) [C files, L names]. No name
  table in the ELF (strings live in menu data) [C].
* Spawns: `MakeWeaponGame(slot,-1 owner, x,y,z, no, rx,ry,rz)` (0x1fa560) — slot -1 = first free of 72
  `g_pWeapon`, sets CWeapon+0xE0 pos/+0xD0 rot, act 15000 sub=kind [C]. Users:
  gmAttackObjAndWeapon::EventStart (weapon at its GmWeaponStatus pos, act ws[1..3], sets 'LIGH' link),
  CGmThrowObject (weapon at model bone p9 after p10 frames), GmWeaponPosRot (at start or on event),
  CGmDispWeaponParam [C]. Initial loose weapons: 50000 `w` lines + 230/231 [C].

---------------------------------------------------------------------------------------------------
## 6. Effects (Q6)
* Placement types <100 create particles by id: `ParticleData = GOBJ 00101[id]` (96 B; s16 type,
  s16 model id, s16…; floats; word13 = hit collision id, word14 = reaction act) [C layout of the read
  fields, L others]. `CreateParticle` type → class [C]: 0 Sprite, 1 Polygon, 3 EmitCircleSprite,
  4 EmitCirclePolygon, 14 NoActiveSpriteReflectGround, 15 NoActiveSprite, 16 NoActivePolygon,
  17 NoActiveEmitCircleSprite, 18 NoActiveEmitCirclePolygon, 23 NoActiveHitSprite, 24 NoActiveHitPolygon,
  25 HitSprite, 26 HitPolygon, 20 FountainSurface, 21 FloodSurface, 22 Spray, 8-13/27 event particles.
  "NoActive" = dormant until `CSetParticleBirthFlag` / `ParticleEventStart` [C].
* **Steam** (zyouki): particles 30-34 are type 24 `CGmNoActiveHitPolygonParticle` drawing model **1237
  zyouki** (5×5 quad) with hit collision 2058/2068 and wrestler reaction act 12047 (32, 34), 12518 (30),
  12041 (31), 678 (33) [C]. Room 30 places 32 and 34 (steam pipes). Valve 210/215 toggles them
  (`SwitchParticle` 0x2ceb00: SE 0xA2 + birth flag on/off); minimum 121 frames between toggles, auto-off
  after 250 frames [C]. A wrestler in the jet is hit (`DoukiJumpAct` reaction) [C].
* Particle 63 (type 16, model 1353 tiny quad) = GmCharAction move effect [C].
* 50103: 290 × 0x7C records, byte-identical in every room (md5 2daa7d25…) [C]; stored at CGmControl+4,
  no reader found [L unused/global sprite table].
* Sparks (hibana) are not a room-30 gimmick; `gmAttackObjBase` particle slots (p5,p6) are -1 here [C].

---------------------------------------------------------------------------------------------------
## 7. Port checklist (room 30)
1. Props/colls/anims: 50101 models 1232-1265, colls 2050-2069/2103/2120, object acts from 50102 and
   M.PAC GAME bank 4 (sub-key 1/2 = object) — convert with the CEKey reader.
2. Gimmick logic: 202, 203×4, 206×2, 207, 208×2+217×2, 209, 210, 212/213+221/222, 215, 218, 220, 201,
   230/231, steam 32/34 — rules above.
3. Wall attributes 0xDF/0xFB/0xFD/0xF4/0xDB → climb/ladder/climb-out acts; drop edges + GapIn.
4. Cage cells: block hits/pick-ups across the fence until a wall pair is broken.
5. Ignore GMEV packages, 200/204/205/211/214/216/219/223, GmReflect 900.

## Helper scripts (this folder)
* `jt.py <table> <n> <base>` – resolve a switch jump table (`jt_out.txt` = CreateObject kinds).
* `gobj_dump.py` → `gobj_dump.txt` – all GMMC_GOBJ tables with class names.
* `cdis.py <regex>` – compact disassembly (drops prologue noise, shows float `lui` consts).
* `dref.py <addr|sym> [span]` – data cross references (lui/lo pairs, cached in dref.pkl).
* `ekey.py <file>` – list a CEKey bank (50102).
* `d_*.txt` – annotated disassembly dumps of the classes discussed.
