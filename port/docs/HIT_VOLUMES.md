# Hit volumes - research (D4)

How SvR 2011 decides that one fighter touches another. Research only: nothing
in the port uses this yet. Written for the Mod Maker viewer, which can draw
these spheres on its two dummies.

Short version: every hit volume is a **sphere** on a model bone. Each fighter
has 16 attack spheres and 15 body (target) spheres. Bones, radii and offsets
are fixed tables in the game's code, the same for every character; nothing
comes from character data.

## Structures

**Sphere element**

| Offset | Meaning |
|---|---|
| +16 | world centre (x, y, z, w); w == 0 means the sphere is off |
| +32 | radius |
| +36 | flags; bit 0x8 = skip as a target |

**Sphere group**: +0 count, +8 pointer to an array of element pointers,
+16 / +32 a bounding sphere.

**Shape test** `sub_826A9BA0`: box pre-check on the two bounding spheres, then
each pair is a hit when `dist(c1, c2) < r1 + r2`. Returns the first hit pair.

**Fighter collider** (520 bytes, base constructor `sub_82275EA8`):

| Offset | Meaning |
|---|---|
| +32 | attack group (16 spheres) |
| +36 + 4i / +120 + 4i | attack sphere i: bone matrix pointer / local offset pointer |
| +204 | target group (15 spheres) |
| +208 + 4i / +288 + 4i | target sphere i: bone matrix pointer / local offset pointer |
| +440 | this frame's active attack mask |
| +448 | the fighter |

**Hit broker** `sub_823AC758`: tests each collider's attack group (+32) against
every other collider's target group (+204). On a hit, vt[8] `sub_82238F28` and
vt[10] `sub_8222F678` are called.

## Bones

A "part" p is `fighter + 112 + 4p`: a pointer to a model bone's matrix
(`model + 180 + boneIndex * 64`). `sub_823AD4A0` fills them from the bone-name
table at 0x82DA0F80:

| Part | Bone | Part | Bone |
|---|---|---|---|
| 0 | root | 10 | r_ninoude |
| 1 | koshi | 11 | r_kote |
| 2 | mune | 12 | r_te |
| 3 | kubi | 13 | l_momo |
| 4 | atama | 14 | l_sune |
| 5 | l_sakotsu | 15 | l_ashi |
| 6 | l_ninoude | 16 | l_tsumasaki |
| 7 | l_kote | 17 | r_momo |
| 8 | l_te | 18 | r_sune |
| 9 | r_sakotsu | 19 | r_ashi |

World centre = bone matrix x (offset, 1), computed by `sub_8269EED0`. All the
offsets but one are along the bone's local X: probably the bone's length axis,
to be checked in the viewer. Units are model units (a fighter is about 20 tall).

## Target spheres (always on)

Radii at 0x82006B20, part bytes at +60, offsets at +80.

| # | Bone | Radius | Offset (local X) |
|---|---|---|---|
| 0 | atama | 1.7 | 1 |
| 1 | mune | 2.5 | 2 |
| 2 | koshi | 2.5 | 0 |
| 3 / 6 | l_ / r_ninoude | 1.2 | 1 |
| 4 / 7 | l_ / r_kote | 1.2 | 0 |
| 5 / 8 | l_ / r_kote | 1.2 | 2 |
| 9 / 12 | l_ / r_momo | 1.7 | 1.7 |
| 10 / 13 | l_ / r_sune | 1.7 | 0 |
| 11 / 14 | l_ / r_sune | 1.7 | 2.7 |

## Attack spheres

Radii at +320, offsets at +384 (same table base as the target radii), part
bytes at 0x8200283C, a group byte per sphere at 0x82002824.

| # | Bone | Radius | Offset | Group |
|---|---|---|---|---|
| 0 | atama | 1.8 | x 1 | 1 head |
| 1 | mune | 3.2 | x 2 | 2 body |
| 2 | koshi | 3.0 | 0 | 2 body |
| 3, 4, 5 | l_ninoude, l_kote, l_te | 1.5 | x 1.2, 0, 0 | 4 left arm |
| 6, 7, 8 | r_ninoude, r_kote, r_te | 1.5 | x 1.2, 0, 0 | 8 right arm |
| 9, 10, 11 | l_momo, l_sune, l_ashi | 2.0, 2.0, 1.8 | x 2, 0, 0 | 16 left leg |
| 12, 13, 14 | r_momo, r_sune, r_ashi | 2.0, 2.0, 1.8 | x 2, 0, 0 | 32 right leg |
| 15 | l_kote | 3.0 | (0, 0, -3) | 64, special cases only |

The per-frame update `sub_822763E8` also has hard-coded tweaks by motion id
(`fighter + 212`) and frame (`fighter + 24`) for a few moves (220, 53, 56, 234,
7030, 7064, 3373, 932, ...), and doubles the forearm / hand spheres while the
weapon-hand check `sub_821B3B70` is true. A viewer can ignore these.

## Turning attack spheres on

- `fighter + 596` is a group mask. Attack sphere i is live (w = 1) when
  `group[i] & mask`.
- YMKs event **op 40** (`28 mm`, 2 bytes): handler `sub_823C4618` sets the mask
  to `mm`. For a mirrored move (`fighter + 240`) it swaps left and right
  (4 <-> 8, 16 <-> 32).
- YMKs event **op 41** (1 byte): handler `sub_823C2F08` sets the mask to 0.
  (The on handler uses `ctx + 36`, the off handler `ctx + 40`; that both are the
  same fighter is medium confidence.)
- The op -> handler table is at about 0x820100F4.
- Code also sets the mask directly in a few places: 63 (all groups) in
  `sub_822D5958`, `sub_821CE318`, `sub_82235D18`; 2 (body) in `sub_823830D8`.
- YMKs event **op 87** (0x57, 2 bytes: `57 mm`) sets the mask to `mm` and
  **op 94** (0x5E, 1 byte) clears it: this is the pair ordinary strikes use
  (measured, see below). The handlers are not traced, so whether op 87 also
  swaps left / right for mirrored moves is unknown (probably, like op 40).

### What the data uses

**Op 87 / op 94: every strike.** m.pac and mpsp.pac hold 2999 op 87 events in
483 motions: 2182 windows closed by op 94, 817 that run to the motion's end, and
137 op 94 without an op 87. Windows are short: mostly 3-5 key frames. Masks seen
most: 63 (all groups, 1175), 8 (right arm, 317), 32 (right leg, 236), 50, 3, 12,
48, 10, 4, 62.

**Op 40 / op 41: weapons only.** Just 54 windows, all in weapon motions (id 15190
in the WEAP, WEPB, WEAT, RU* banks, plus BM02 741). Masks 11, 12, 13, 15, 29, 43, 63.

### Runtime check (2026-10-13)

`SVR2011_HIT_LOG=1` (branch d4-log, `src/hit_log.cpp`) logged a CPU-vs-CPU match
(John Cena vs Randy Orton): per collider the motion, frame, `fighter + 596` and
`collider + 440` on change, every sphere-pair hit of `sub_826A9BA0` and every
`sub_82238F28` call.

| Motion (BM00/4) | Op 87 at key | Op 94 at key | Logged mask | Logged window (fighter + 24) |
|---|---|---|---|---|
| 3128 | 10, mask 08 | 13 | 8, active 0x1C0 (spheres 6-8) | 21 - 27 |
| 3860 | 10, mask 20 | 15 | 0x20, active 0x7000 (12-14) | 21 - 31 |
| 3893 | 18, mask 3E | 25 | 0x3E, active 0x7FFE | 37 - 51 |

- `fighter + 24` counts at twice the key frame rate: window = key x 2 (+1).
- Each hit was a shape-test hit (attack sphere x target sphere, e.g. 8 x 2 =
  right hand x koshi) from the broker (caller 0x823AC82C), followed by the hit
  callback (caller 0x823AC934).
- Motions 53, 220 and 234 showed active spheres with mask 0 (0x8000 = sphere 15;
  0x1B0): the hard-coded per-motion tweaks in `sub_822763E8`.
- Mask 2 (body, active 0x6) appeared in motions 20500, 59 and 6801: set from code.

## For the viewer

1. The bone world matrices it already computes.
2. The two tables above.
3. Target spheres on both dummies all the time.
4. An attack sphere only while an op 87 -> op 94 window (strikes) or an
   op 40 -> op 41 window (weapons) is open and its group bit is in the mask
   (left / right swapped for mirrored moves). A window without its closing op
   runs to the end of the motion.
