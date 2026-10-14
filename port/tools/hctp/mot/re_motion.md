# HCTP (PS2) motions -> SvR 2011 YMKs: record formats, skeletons, props, converter

Tags: **[C]** read from code or proven on data, **[L]** likely, **[G]** guess.
HCTP addresses are EE addresses in `SLES_520.36` (symbols via `..\eedis.py`). 2011 facts marked
"(dfe83e05)" come from the earlier YMKs work, `...\dfe83e05-...\scratchpad\ymks\YMKS_SPEC.md`, whose
codec `ymks.py` is copied into this folder.

## 0. Short answer

HCTP and SvR2011 use **the same motion engine generation**. They share:
- the bank container and the frame stream;
- the pose sub-record ops;
- the 21-bone list and order (`root koshi mune kubi atama l_sakotsu l_ninoude l_kote l_te r_sakotsu r_ninoude r_kote r_te l_momo l_sune l_ashi l_tsumasaki r_momo r_sune r_ashi r_tsumasaki`);
- the Euler order (R = Rz·Ry·Rx, column vectors);
- the angle unit (s16 × 2π/65536);
- the root unit (0.01 model units, y down, pelvis origin);
- the motion bind (HCTP's rest pose; see §3);
- the event-op numbering.

**A wrestler motion converts with no retargeting.** The values are copied and re-encoded with 2011's
wider bone types. Converted poses are identical to the HCTP poses frame by frame (0 difference), and
they stand on the floor on a 2011 skeleton.

Props convert by renaming two ops.

## 1. HCTP container and stream [C]

**Bank (`CEKey`):** `T[256]` (event sizes), `u32 n`, then n × `{u32 key, u32 off, u32 frames, u32 0}`, then data.
- No `YMKs` header.
- `CEKey::GetData` 0x168f00 returns −1 when `frame >= frames`.
- `off == 0xFFFFF` means no data.

**Key = `id<<16 | x<<8 | y`.** The low byte **y is the track**:
- 0 = wrestler, 1 = partner/object, 2+ = more objects;
- 10-12 and 30-32 = 5-byte scalar tracks;
- 50+ = cameras.

Byte 1 (x) is the variant. This is the same layout as 2011 (`u8 y, u8 x, u16 id` in LE order). **The brief had x and y swapped.**

**Stream:**
- `b < 0x60`: skip b frames.
- `b > 0x60`: a record of `b - 0x60` bytes.
- **`0x60`: a u8 size follows.** 2011 uses a u16 here, and the converter rewrites it.
- One record per frame. Frame = tick >> 1, so 30 fps data at 60 Hz (`CMotionData::SetKey` 0x1b69d0).

**Record:** if the first byte has bit 7 set, an event block of `b & 0x7f` bytes comes first (`DataAnalysis`). The rest is the pose.
- Event-only records are the short records: 2-15 bytes, for example `82 01 08` or `84 52 c8 fe 58`.
- The 5-byte records are y 10-12 / 30-32 scalar keys, `[count][s16][s16]`.

**Per-bank T** lists only the ops that bank uses. **Every op size equals 2011's T** except op 185 (1 byte in HCTP, 2 in 2011), across all 7 MOT_GAME banks and 50102.

## 2. HCTP wrestler pose (`CMotionData::DataAnalysis` 0x1b77d0, `SetBoneData` 0x1b6bd0) [C]

**Sub-records.** The parser loops while `p < size − 1`.

| op | layout |
|---|---|
| 0x60-0x6f | `[op][s16 root ×3][21 bones]`, count = op − 0x60. **This is the 87-byte record** (1 + 6 + 80). |
| 0x7c | `[7c][count][s16 root ×3][21 bones]`. This is the 88-byte record. |
| 0x7d | `[7d][count][f32 root ×3][21 bones]` |
| 0x50-0x5f / 0x7a | `[op]([count] for 7a)[s16 root ×3][u24 mask][bones in mask]` |
| 0x7e / 0x7f | `[op][count][s16 root ×3]` / `[op][count][f32 root ×3]`: root only |
| 0x7b | `[7b][bone][count][s16 ×3]`: translation of that bone = rest + v × 0.01 |
| 0x00-0x14 | `[bone][count][bone data]` |

**Differences from 2011:**
- HCTP's masked forms carry an **s16** root. In 2011, 0x50-0x5f and 0x7a carry f32 and 0x75 carries s16.
- HCTP has no 0x74, 0x75 or 0x76.

**Count:** the pose is reached in `(count+1)·2` ticks, so a key at frame f is reached at f + count + 1 (same as 2011).
- `root × 0.01` (const 0x464568). Mirroring negates x (`Hanten`).
- At runtime the root's x/z are scaled by obj+0x4c and its y by the character's size (`TransChg_Y` 0x1b5910, link+0x164). The data is for the standard body.

**Bone types** (`s_sk_hokan` 0x4124a0; names `tblBoneName` 0x4124c0):
`7 7 7 14 23 6 7 3 14 6 7 3 14 7 2 14 10 7 2 14 10`

| type | axes | storage | bones |
|---|---|---|---|
| 7, 23 | x y z | s16 ×3 | root, koshi, mune, atama (23), ninoude, momo |
| 3 | x y | s16 ×2 | kote |
| 6 | y z | s16 ×2 | sakotsu |
| 2 | y | s16 | sune |
| 14 | y z | **u8 ×2, the high byte of the s16** | kubi, te, **ashi** |
| 10 | y | **u8, the high byte** | tsumasaki |

`BoneCreate` 0x1b5c60 widens 10→2 and 14→6 unless the motion object's mode is 1. The stock MOT banks are mode 1: 87-byte records, and 158 478/158 478 records re-encode byte-identically.

**Full 87-byte record:**

| bytes | field | bytes | field | bytes | field |
|---|---|---|---|---|---|
| 0 | 0x60+count | 33-36 | l_sakotsu y,z | 65-70 | l_momo xyz |
| 1-6 | root t x,y,z | 37-42 | l_ninoude xyz | 71-72 | l_sune y |
| 7-12 | root rot xyz | 43-46 | l_kote x,y | 73-74 | l_ashi y,z (u8) |
| 13-18 | koshi xyz | 47-48 | l_te y,z (u8) | 75 | l_tsumasaki y (u8) |
| 19-24 | mune xyz | 49-52 | r_sakotsu y,z | 76-81 | r_momo xyz |
| 25-26 | kubi y,z (u8) | 53-58 | r_ninoude xyz | 82-83 | r_sune y |
| 27-32 | atama xyz | 59-62 | r_kote x,y | 84-85 | r_ashi y,z (u8) |
| | | 63-64 | r_te y,z (u8) | 86 | r_tsumasaki y (u8) |

**Angle → matrix:** `objMatrix::RotZYX(int*)` 0x1454f0 gives the rows
`(cy·cz, cy·sz, −sy) (sx·sy·cz − cx·sz, sx·sy·sz + cx·cz, sx·cy) (cx·sy·cz + sx·sz, cx·sy·sz − sx·cz, cx·cy)`.
That is the transpose of Rz·Ry·Rx. This is 2011's convention.

**Composition:** `CMotionHokan::ActionLink` 0x1b60b0 computes `bone matrix = Motion × Bind` with VU0 row-vector matrices. In column form, local = R_bind · R_motion. Bind = RotZYX(node r) from `BoneCreate`.

**Interpolation:**
- Type-7 bones are kept as a matrix and re-split (`ToRotXYX`) only to interpolate.
- With the twist flag (ninoude/kote of a character), **40 %** of the twist goes to `*_x` (constants 0.4/0.6).
- This is runtime-only and needs nothing in the data.

**Plausibility.** `verify_hctp.py` runs FK on Hulk Hogan (CH.PAC entry 0, 71 nodes) with Bind·Motion. Per track, the median lowest joint is on the floor:
- bank 0: 71/80 tracks;
- bank 4: 50/60;
- with Motion only: 9/80;
- with Motion·Bind: 34/80.

The standing pelvis is at −940 (0.01 units). In the renders the poses are human.

## 3. SvR2011 110-byte record and object/camera tracks

**110-byte record.** The full spec is in YMKS_SPEC.md (dfe83e05).
- Layout: `[7d][count][f32 root ×3][96 bytes of bones]`.
- Same bone order as HCTP, with m.pac types `7 7 7 6 23 6 7 3 6 6 7 3 6 7 2 23 2 7 2 23 2` (all values s16). **ashi is x y z.**

**Round trips [C]** (`test_roundtrip.py --all`, on 78PK plus the 163 m.pac banks):

| test | result |
|---|---|
| `write_bank(read_bank)` | 164/164 byte-identical |
| streams | 35 713/35 713 |
| 110-byte 0x7d records, decode → encode | **2 098 771/2 098 771 byte-identical** |
| other character forms | 89 864 pass |

The 32 025 failures are object tracks that use other bone tables, already known in dfe83e05.

**The "29-byte defender tracks" are cameras** (y 50/51): `[count][f32 pos][f32 rot][f32 fov]`, camera mode 1.

**Motion bind (new) [C on data, code site not found]:**
- 2011 motions assume **HCTP's rest rotations** as the motion bind: root z90, koshi z180, sakotsu ±z90, and **0 for momo/ninoude/kote**.
- They do *not* assume the ch.pac node r: momo (−1.6, 0.1, ∓14.9°), ninoude ±15°, kote y 20° (A-pose).
- Evidence (`bind_test_2011.py`, `bind_test_12300.png`):

| motions | own node r as bind | HCTP rest as bind |
|---|---|---|
| stock 78PK y0 | 5/41 on the floor | 39/41 |
| BM01_1 | 12/80 | 21/80 |

  The 12300 renders show splayed legs with the own node r and a natural stance with the HCTP rest.
- The earlier note "YMKs angles relative to the bind (node r)" holds only for the single-axis bones.
- **Worth re-checking in the WWE '13 retarget (`w13_motion.py` / `retarget.py`)** if it uses ch.pac node r for momo, ninoude or kote.

**Object tracks [C on data]** (5 679 GMGB object records in 78GM/GL/HW/IS/PK/PL/RL parse this way):

| op | layout |
|---|---|
| 0x75 | `[75][count][s16 root][u24 mask][s16 x,y,z per bone]` |
| 0x7a | the same with an f32 root |
| 0x7e | root |
| 0x7b | bone translation |
| 0x00-0x1f | `[bone][count][s16 x,y,z]` |

- Bones are the nodes `root, bone01…` (the 78PK cars have 5).
- Up to bone 9 is seen.

## 4. Skeletons and mapping (`skel_compare.py`)

**HCTP Hogan:** 71 nodes.
- Body chain `root → koshi → mune → {l,r}_sakotsu → ninoude → ninoude_x → kote → kote_x → te → fingers` (`l_yubi00`-`42`).
- `kubi → atama` with face nodes (kuchi, mayu, mabuta, eye, ha).
- Legs `root → momo → sune → ashi → tsumasaki`.
- Dummies `*_d`, `root_d`.

**2011 ch104:** 102 nodes. Same names, plus:
- `momo_x` (between momo and sune);
- `kubi_x`;
- `kote_x2`;
- `*_m` muscle nodes, `kata_m`, `mune_dummy`;
- a richer face;
- `onaka`.

**Mapping:** the 21 motion bones map **by identical name**. Fingers also share names, but the body record does not drive them.

**Rest frames:** both are pelvis origin, y down, x along the bone, about 10 cm units.
- root, koshi and sakotsu frames are identical.
- 2011 skin binds add A-pose rotations, but motions ignore them (§3).

**Lengths** (Hogan / ch104):

| segment | HCTP | 2011 |
|---|---|---|
| spine | 1.99 | 2.23 |
| upper arm | 2.90 | 2.81 |
| forearm | 2.24 | 2.82 |
| thigh | 4.63 | 4.35 |
| shin | 4.45 | 4.35 |
| foot | 1.45 | 1.45 |

These are character differences that both engines absorb.

**Corrections needed:** none. Use the same angles and an unscaled root.
- The scale test (root × 0.9 / 0.95 / 1.0 / 1.05) is best at **1.0**: the lowest joint is at −0.08 on ch104, against −0.12 for 2011's own data.
- No axis flips are needed.
- Optional [G]: a ±4.8° thigh/knee offset correction (HCTP's knee sits 0.39 off the thigh's x axis). It was not applied, because 2011 data uses the same bind.

## 5. Props

**HCTP props** (`CSD3MotionWeapon::SetData` 0x1c2260, `SetRot` 0x1c1c10) [C]:
- Every bone is x,y,z s16 (RotZYX). Roots and translations are s16 × 0.01.

| op | layout |
|---|---|
| 0x7a | `[7a][count][s16 root][u24 mask][6 bytes per bone]` |
| 0x7d / 0x7e | root (8 bytes) |
| 0x7b | translation |
| 0x00-0x1f | single bone |

- Any other op ends the record.
- Bone k is the node named `tblSD3WEP[k]` = `root, bone01..bone19`, matched by name (ctor 0x1c2990).
- Bind = RotZYX(node r); composition is the same as for wrestlers.

**50102** (room 30): acts 23-33 are y=0 object tracks.

**danboru** (50101/01243): `root` plus `bone01-10`, the boxes on the root, at rows y −2.5, −8, −13.5, −19.
- Act 32 is a 1-frame idle; act 33 is the 42-frame collapse.
- FK of act 33 (`objpose.py`) ends with **every box at y −2.5, on the floor** [C]. This checks the 0x7b offset × 0.01 + rest rule.

**Mapping to 2011:**
- HCTP `0x7a` → 2011 `0x75` (byte-identical body); `0x7d` → `0x7e`; the rest is copied.
- Bones keep their index, because both games bind `root/boneNN` by name.
- `verify_props.py`: 11/11 tracks parse with the 2011 grammar, the held state is identical 11/11, and the converted act 33 ends with all boxes on the floor.

**Where they go in 2011:**
- A pair act with its objects: tracks y=2.. of the grapple key. 78PK 12300-12307 animate the cars on y2-4, and T33 p2..p4 name the T23 records.
- A standalone object motion is (key, 0, 0) [L] (T23 w5 idle key).
- `hctp2ymks.py --attach 50102.bin:33:2` adds a prop act as track y of the converted ids.

**Not handled:**
- `hctp2jboy.py` bakes props into one node. **Animated props need JBOY files that keep the `root/boneNN` nodes and node-attached meshes** (HCTP batches are rigid per node).
- The GMGB T23 records and locators must reference them.

## 6. Converter `hctp2ymks.py` (prototype)

**Usage:**
```
python hctp2ymks.py <hctp bank> <id[=newid],...> [--into <2011 YMKs bank>] [--out f]
       [--events keep|jumps|none] [--objects] [--compact] [--attach bank:act:y] [--jumpmap a=b] [--camera-raw] [--fov r]
```

**Wrestler tracks:**
- Full keys → 0x7d (110 bytes).
- Masked keys → 0x50+count or 0x7a with an f32 root (2011's stock forms).
- 0x7e, 0x7f, 0x7b and single-bone keys → same op.
- 8-bit values become s16 (unchanged values); ashi gains x = 0.

**Other tracks:**
- Objects → §5.
- Cameras (13-byte, = 2011 mode 0) → 29-byte mode 1 with fov 0.7217 rad. This is 2011 78PK's median; **HCTP has no fov in the key [G]**.
- 5-byte scalar tracks are copied.

**Frame stream:** re-emitted with the u16 escape. Trailing skips and the `frames` field are kept.

**Events:**
- `keep` copies every op of equal size, which is all except 185.
- Jumps 49/50/51 are renamed when the target is converted or in `--jumpmap`, and dropped otherwise.

**Checks** (`verify_convert.py`):
- banks 0/2/4, 60-80 ids each: bank rewrite identical;
- every 2011 record decodes with no trail, and every event block parses with 2011's T;
- **max pose difference 0** (angles and root), for 99 + 154 + 139 tracks;
- lowest joint per y0 track: HCTP/Hogan −0.17 / −0.32 / −0.22, converted on ch104 −0.12 / −0.23 / −0.14.

**Renders:** `conv_12034.png` (HCTP 12034, top = Hogan, bottom = converted on ch104) and `conv_bank0.png`.

**Not handled / open:**
1. **Event semantics [L/G].** The op numbering is inherited (sizes equal, overlapping op sets in backstage banks), but arguments such as sound, effect, hit or camera-shake ids may index HCTP-only tables. It is untested in game.
   - Most used HCTP ops: 192, 111, 16, 82, 88, 81, 80, 97.
   - Jumps to generic HCTP ids (443, 1900, 1660…) are dropped unless `--jumpmap` is given.
2. **Ambiguous tracks.** A track whose records parse both as wrestler and as object (only 0x7a / 0x7b / single-bone) becomes an object for y ≥ 1. The bytes differ only for masks containing 8-bit or 2-axis bones.
3. **Not converted.** The output is a raw copy and a report line.
   - MOT_GAME bank 6 (0x79 records, likely `CSD3MotionFace` face/hand data) and bank 7.
   - MOT_BRAP y1 (138-byte records, a different rig, likely costume).
   - WPON_PROP/KAGE, MOT_ALL and MOT_MARK are not CEKey banks (text or other containers).
4. **Untested ops.** A packed MOTP form is not written; GMGB banks use the unpacked form. The `--compact` and `0x75` wrestler forms are legal per sub_82391380, but 0x75 is rare in stock data.
5. **HCTP runtime extras with no data.** IK (`CCharIK`, `IsIKOff`) and the character-size root scaling are runtime-only. 2011 has its own versions.
6. **Untested in game.** Nothing was run in the game (read-only task).
   - Next steps: put the converted keys in a GMGB bank (free range 12500-12599) and trigger them via T33 p1, or JumpAct from a hook.

## 7. Files (this folder)

| file | role |
|---|---|
| `hmot.py` | HCTP bank / stream / wrestler + object decode and encode, track kinds, sampler |
| `ymks.py` | 2011 codec (copied from dfe83e05) |
| `s11mot.py` | 2011 sampler |
| `fk.py` | Euler, FK, and the HCTP / 2011 node loaders |
| `skel11.py` | 2011 node loader |
| `objpose.py` | prop state and FK |
| `hctp2ymks.py` | converter |
| `test_roundtrip.py` | HCTP 159 894/159 894 and 2011 round trips |
| `verify_hctp.py` | HCTP plausibility check |
| `verify_convert.py` | conversion check and renders |
| `verify_props.py` | prop conversion check |
| `bind_test_2011.py` | which bind 2011 motions assume |
| `standheight.py` | standing pelvis height in both games |
| `skel_compare.py` | skeleton comparison |
| `failscan.py` | lists records a bank fails to decode |
| `d_*.txt` | disassembly |
| `78PK_bank.bin` | 2011 template bank |
| `*.png` | renders |
