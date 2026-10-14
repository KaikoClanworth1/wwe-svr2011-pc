# SvR 2011 secondary motion ("Bane") - hair, cloth and body jiggle

How the game moves hair (kami_b chains), body jiggle (the *_m / *_x helper bones) and CAW hair / cloth
(kami_a / hira chains): the data each attire carries (ch.pac model children 50 / 60 / 61), the bones, the
per-frame solver, exact enough to reimplement (section 7: the recipe) - for the Mod Maker viewers and
for converters that want ported hair to move. Checked against the game: section 9.

Read-only research from the recompiled code (`port/generated/default/*.cpp`) and `port/runs/image.bin`.

- [C] means read from the code: the instruction addresses are given, and the behaviour can be checked there.
- [L] means likely: inferred from the code shape, the data or naming, but not proven.

All addresses are guest VAs. The research helper scripts (kept with the research session, not in the
repo) were:

- `asm.py` - asm cache and annotator:
  - `asm.py index` builds `asmcache/` and `funcs.json`.
  - `asm.py fa 0xADDR` prints an annotated listing.
  - `asm.py w|f|d|str 0xADDR` reads data.
  - `all.s` is the concatenated cache, for fast grep.
- `bane_range.s` (0x82167E00-0x82170000), `muscle_range.s` (0x8226C000-0x82271000) and `caw_range.s` (0x82226000-0x8222F000) are annotated dumps.
- `nodes102.py`, `scan60.py` (statistics over all ch pacs), `edscan2.py` and `edjboy.py` (CAW edit pacs).
- `edtxt2/` holds the CAW sway texts as UTF-8.

Conventions used by all three systems:

- [C] Matrices are 4x4 row-major and used with row vectors, so `p' = p * M`. Row 3 holds the translation. The helpers are:
  - `8216B248(v, M)` computes `v*M`, using all four components of `v`.
  - `821605F0(out, A, B)` computes `out = A*B`.
  - `8216A5A0(out, M)` computes the rigid inverse.
  - `82152980` transposes in place.
  - `826A8890(q, a, b)` builds the shortest-arc quaternion that turns `a` onto `b`: the axis is `a x b`, the angle is `acos(a.b)`. It returns identity if `|axb| < 1e-6` or `|angle| < 1e-4`.
  - `826A7FB8(q, M)` converts a quaternion to a row-vector matrix.
- [C] A bone's +X axis points along the bone. A JBOY node's local translation x is the bone length; see the jboy node record, where the parent is at +48.
- [C/L] World **+Y is down**, which explains why a positive gravity value pulls downwards. Evidence:
  - [C] The hair reset `8216A830` hangs the chain from the head along +Y.
  - [C] The floor clamp `8216AFF4` keeps `y <= floorY - r`.
  - [C] The ring mat height is `-8.4`.
- Model arrays on the model object (`char[84]` for the body; sub-models come from the 'SUBM' component):
  - [C] `+176` local matrices, 64 bytes per node.
  - [C] `+180` world matrices.
  - [C] `+184` skin matrices.
  - [L] `+196` inverse-bind matrices. The skin matrix is computed as `skin = M196[i] * world[i]` (`821605F0` calls at `8216CF54` and `8226E1AC`).
- [L] Units are about 0.11 m per unit: a wrestler is about 17-18 units tall and the ring is 64 units wide.
- [C] Character scale is `char+428` (float). It multiplies the collision shapes.

There are **three independent systems**, plus the ring ropes:

| System | Component (4cc) | Code | Data |
|---|---|---|---|
| Superstar hair `kami_bNN` | `gBaH` (0x67426148), 24400-byte object, vtables 0x82001DE0 / +16: 0x82001DC0 | 0x82168000-0x8216F000 | PACH 60 (chains) + 61 (collision capsules) via `desc+76` |
| Body "muscle" jiggle | `gBaM` (0x6742614D), 1600-byte object, vtables 0x820067EC / +32: 0x820067E4 | 0x8226D2E8-0x8226E270 | PACH 50 via `desc+52` |
| CAW hair/cloth `kami_a/kami_k/hira_a/hira_k` | `HAIM` (0x4841494D) 176 bytes → part objects (208 bytes, vtable 0x8200527C) → chain objects (5296 bytes, vtable 0x82005288) | 0x82226000-0x8222F000 | sway text inside the CAW part pacs (pac/edit/edNN, PACH ids 2316-2339) via `desc+16/+20` |
| Ring ropes | object vtable 0x82001DBC (3 ropes, 4 sides × 24 nodes) | 0x82168058-0x8216A268 | - (not analysed) |

The components are created in the character setup functions `821AB7E8`, `821ADC00` and `821AEA10`, in this order:

- `8222E0D0` HAIM, at `821AC460`.
- `8216E1F8` gBaH, at `821AC48C`.
- `8226E1F0` gBaM, at `821AC498`.

`desc` is the 128-byte per-character resource descriptor (`821DC6D0` returns `[[0x82E368CC]] + slot*128`; its constructor is `823C7088`). [C] gBaH is only created if `desc+76 != 0`, and gBaM only if `desc+52 != 0`.

Not found: the code that fills `desc+52` and `desc+76` from the PACH entries. Treat the mapping 50→+52 and 60/61→+76 as [L]. The evidence is strong: the parsers consume exactly the line formats of those entries, the `{size, ptr}` pairs sit at `[0]/[4]` (entry 60) and `[16]/[20]` (entry 61), and `[8]/[12]` is unused.

**Entries 40 and 80 are not read by any of the three systems** [C]: the constructors only touch `desc+16/20/52/76`. Their consumer is unknown.

---

## 1. Loading and parsers

### Text tokenizer (shared) [C]

- `826A8B88(obj, ptr, size, 10|13)` creates a line reader.
- `826A8C98()` advances to the next line and returns the token count, or -1 at the end. Comment lines (`;`) produce no tokens.
- `826A9110(i)` returns token *i* as a string.
- `826A9090(i)` returns token *i* as a float.
- `826A8F18(i)` returns token *i* as an int.

### Entry 60 "バネ系髪の設定" → `8216D3C0`, called from `8216DC60`

`8216DC60` is the gBaH init, called by the constructor `8216E178` [C]. One line per chain:

```
kami_bNN  radius visc grav spring damp A lencorr bend movecorr
```

- [C] Up to 16 chains; the chain count is at `H+752`.
- [C] `8216D280(name)` decides which model the chain belongs to:
  - First it searches every sub-model of the character's 'SUBM' component (the CAW/attire part models). If the name is found there, the result is an index into `H+23300[]` (count at `H+23296`).
  - Otherwise, if the name is found in the main model, the result is `100`.
  - Otherwise the result is `-1` and the line is ignored.
- [C] The root node is the node named by token 0, for example `kami_b03`. The children are then searched by name, `kami_b03_00`, `_01`, `_02` …, until one is missing. The suffix is built at `8216D45C..8216D534`.
- [C] Node 0 is the root and nodes 1..n are the `_MM` bones, so the count is `n+1`. **There is no bound check against 16 nodes.**

Chain record `C = H + 768 + 1408*i` [C]:

| off | meaning |
|---|---|
| C+0 | node count (root + children) |
| C+4 | model selector (100 = body, else sub-model index) |
| C+8 | **parent bone of the root** (file node record +48, read through model vfunc 12 at `8216D540`) - normally `atama` |
| C+16 + 80*j | node *j* (see below) |
| C+1296 + 4*j | rest length of segment j (between nodes j and j+1). **All of them are set to the local X translation of the LAST found node** (`f31` at `8216D4D4`, filled at `8216D5CC`) |
| C+1360 | `lencorr` (長補正) - **never read** [C: no load of +1360/+2128 in the module] |
| C+1364 | `visc` (粘抵) |
| C+1368 | `grav` (重力) |
| C+1372 | `spring` (バネ) |
| C+1376 | `damp` (減衰) |
| C+1380 | `radius` (半径) - collision radius of the chain |
| C+1384 | `bend` (曲難) |
| C+1388 | `movecorr` (移動量補正) |
| C+1392 | `A` (ア, int) - selects the write-back mode; 568 of the 570 lines in all ch pacs have 0 |

Node record `N = C + 16 + 80*j` [C]:

| off | meaning |
|---|---|
| +0 | position (world, w=1) |
| +16 | velocity |
| +32 | force (scratch) |
| +48 | previous position |
| +64 | pinned flag (int) |
| +68 | 0.3 for j≥1 (set in reset, use not found) |
| +72 | bone index |

### Entry 61 → `8216A268`, called from `8216DC60`

[C] One line per collision capsule, at most 14 capsules (count at `H+68`):

```
bone x0 y0 z0 r0 x1 y1 z1 r1 k
```

The capsule record is at `H + 80 + 48*c`:

- `+0` holds `p0`, with w=1.
- `+16` holds `p1`.
- `+32` holds `r0` and `+36` holds `r1`.
- `+40` holds `int(k)`. **k (always 5 in the data) is never read.**
- `+44` holds the bone index (body model only).

`p0` and `p1` are in the bone's local space.

### Entry 50 "バネ系筋肉の設定" → `8226D690`, called from the gBaM constructor `8226D890`

[C] The parser reads the value lines in order, up to 9 rows of 8 floats; the `;name` lines are comments. Each value is stored for row r as follows:

- `visc` at `M+1268+4r`
- `grav` at `M+1304+4r`
- `spring` at `M+1340+4r`
- `damp` at `M+1376+4r`
- `time` at `M+1412+4r`
- `limitX` at `M+1456+16r`
- `limitY` at `M+1460+16r`
- `limitZ` at `M+1464+16r`

The defaults in `8226D2E8` are always overwritten by the data, because gBaM only exists when entry 50 exists.

### CAW sway text (`揺れ物設定ファイル`, in the edit pacs)

This text does **not** use ATARI/KAKUDO/ZOFFSET keywords in any stock file. See section 6.

---

## 2. Which bones are simulated

- **gBaH** [C] simulates only the chains listed in entry 60, found by name: `kami_bNN` plus its `kami_bNN_MM` children. A chain is not discovered automatically from node names alone.
  - Node 0 is the root bone, and it is never rotated.
  - Node 1 (`_00`) is pinned to its animated position every frame.
  - Nodes 2.. are simulated.
  - The bones that get rewritten are nodes 1..count-2. The tip node (count-1) is only a target: its bone matrix is never written, so it should be an end bone with no skin weights.
- **gBaM** [C] uses the 15 fixed bones in the table at `0x82D9EA98`, mapped to parameter rows by the table at `0x820067A8`:

  | bones | parameter row |
  |---|---|
  | `koshi` | 0 |
  | `mune` | 1 |
  | `kubi` | 2 |
  | `l_sakotsu`, `r_sakotsu` | 3 |
  | `l_momo`, `r_momo` | 4 |
  | `l_momo_x`, `r_momo_x` | 5 |
  | `l_sakotsu_m`, `r_sakotsu_m` | 6 |
  | `l_ninoude_m`, `r_ninoude_m` | 7 |
  | `l_kote_m`, `r_kote_m` | 8 |

  These are real skeleton bones plus the `_m` muscle-helper bones. A missing bone gets index -1 and is skipped.
- **CAW** [C] `8222A290` walks every node of a part model:
  - `kami_aNN` gives type 1, `kami_kNN` type 2, `hira_aNN` type 3 and `hira_kNN` type 4. Names whose 9th character is `_` (that is, the children) are skipped.
  - `kamiNN` with no underscore also gives type 1.
  - The chain children are found in `82229688` from the prefix table at `0x82D9E958`: `"kami00","kami_a00","kami_k00","hira_a00","hira_k00"[type]`, with characters 6..7 replaced by NN. The children are `<root>_00`, `_01`, …, or `kamiNN00`, `kamiNN01`, … for the type-0 naming.
  - The names at `0x8206F7DC..0x8206F818` (`kami_a01..06`, `hira_a00_00..hira_a06`) and their tables at `0x82DB4EFC` / `0x82DB50F0` belong to a bone-name list used elsewhere. They are not part of the physics.

---

## 3. gBaH solver (superstar hair) - per frame

### Scheduling

- [C] The kick is `8216D268`, slot 1 of the gBaH vtable. If `char+1096 != 0`, it calls `8216B0E0`.
- [C] `8216B0E0` picks the worker object `[0x82DE0374 + 4*(char[1156]&1)]` and posts the character to it (`8216A3A8`: sets the busy flag and signals the event).
- [C] The worker loop `8216E098` is the thread-table entry for "BaneHairThread0" (`0x82000D78`; entry 0x8216E160, stack 0x8000, hw thread 2). For every pending character slot in `0x82E3CC50[9]` it runs:

```
8216D618(H)   // root follow + pin + collision-sphere build + RK4 step #1 per chain
8216C6F0(H)   // bend stiffness + head-turn kick (all chains), then RK4 step #2 per chain
8216A9E8(H)   // length constraints, collision, floor, prev=pos
8216CAE8(H)   // write bone matrices (local/world/skin)
```

- [C] The join `8216A450` (wait on the per-character done event) is called before the character is drawn (`82196960`, `82225508`, `82258CC0`, `82718EE0`).
- [L] So the hair runs once per character update. It runs after the animation and skeleton have produced world matrices, and before rendering.
- [C] During loading `82323300` kicks every character 30 times, as a settle warm-up.

**Frame-rate dependence** [C]: there is no dt anywhere. One step is two RK4 substeps of h=1. The only frame-rate term is `G = 0x82EDDBE8`:

- `G+60` is 60/fps: 1.0 at 60 fps, 2.0 at 30, 1.2 at 50, 2.4 at 25. It is set in `826E1AE8` / `826E1D28`.
- `G+8` is the fps.
- These are used only in the movement-correction factor (below).
- At the fixed 60 Hz world update of the port, use G60 = 1 and `lowfps = false`.

### Step A - `8216D618`

1. **Collision spheres** [C]. Each capsule c is turned into 5 spheres, stored as `(center.xyz, radius)` at `H+23312 + 80*c + 16*s`. The sphere count `5*ncap` is at `H+24112`.

   ```
   s = char.scale (char+428)
   P0 = (p0*s, 1) * World[bone];  R0 = r0*s
   P1 = (p1*s, 1) * World[bone];  R1 = r1*s
   M  = (P0+P1)/2;                RM = (R0+R1)/2
   S3 = (P1+M)/2;                 R3 = (RM+R1)/2
   S4 = (P0+S3)/2;                R4 = (RM+R0)/2   // note: centre uses S3, not M (original quirk)
   spheres = [ (P0,R0), (P1,R1), (M,RM), (S3,R3), (S4,R4) ]
   ```

2. **Per chain** [C]:

   ```
   rootW = World[N0.bone]; N0.pos = rootW.row3 (w=1)
   d = N0.pos - N0.prev
   if |d| <= 1.0:
       loc = normalize( (N0.prev,1) * rigidInverse(rootW) )        // prev root pos in current root frame
       f = movecorr * G60
       if loc.y < 0: f = (loc.y + 1) * movecorr
       f = max(f, lowfps ? 0.75 : 0.5)     // lowfps = (G.fps==30 || G.fps==25), 8216A6D8
       d *= f
   // |d| > 1.0: teleport, d unchanged (100%)
   for j in 1..count-1: Nj.pos += d; Nj.prev += d; Nj.pos.w = 1
   N1.pos = World[N1.bone].row3; N1.pinned = 1     // N0.pinned = 1 from reset
   RK4Step(C)                                     // 8216B5E8
   ```

   With the stock `movecorr` range of 0-0.45, `f` is effectively 0.5: half of the head translation is carried rigidly into the hair.

### Forces - `8216B3B0(H, nodes, C)` [C]

```
for each node j: F_j = -visc * V_j;  F_j.y += grav
for each segment j = 0..count-2:
    d = P_j - P_{j+1};  len = |d|;  if len < 1e-4: d.y = 1e-4, len = 1e-4
    dv = V_j - V_{j+1}
    s = ( spring*(len - L_j) + damp*(dv.d)/len ) / len
    F_j     -= d * s
    F_{j+1} += d * s
```

Pinned nodes (0 and 1) take part in the forces and keep integrating their velocity. Only their position is frozen.

### RK4Step - `8216B5E8(H, C)`, h = 1 per call [C]

This is RK4 with a quirk: the trial state T accumulates from stage to stage. It is not rebuilt from the start state each time, so the code must be reproduced exactly.

```
k1: F = forces(N);         A.V = V;      A.F = F
    T.V = V + 0.5*F;       T.P = P + 0.5*V      (T.P = P if pinned)
k2: F2 = forces(T); V2 = T.V
    A.F += 2*F2;  A.V += 2*V2
    T.V += 0.5*F2;         T.P += 0.5*V2 (unless pinned)
k3: F3 = forces(T); V3 = T.V
    A.F += 2*F3;  A.V += 2*V3
    T.V += F3;             T.P += V3 (unless pinned)
k4: F4 = forces(T); V4 = T.V
    A.F += F4;   A.V += V4
N.V += A.F/6;  if !pinned: N.P += A.V/6
```

The 4-way unrolled loop (`8216B698`) and the remainder loop (`8216BB34`) do the same work.

### Step B - `8216C6F0` [C]

For each chain:

```
v = (0,0,-1,0) * World[C+8 parent]     // parent (head) -Z axis
turn = |v - H.prevDir[i]| * 0.1  (H+24128+16i);  H.prevDir[i] = v;  turn = min(turn, 0.02)
for j in 2..count-1:                                    // bend stiffness 曲難
    e = 2*P_{j-1} - P_{j-2} - P_j                       // towards straight continuation
    if |e| / L_0 > 0.1:
        k = bend * (j>=4 ? 0.5 : j==3 ? 0.75 : 1.0)
        V_j += e * k
if turn > 0.001:
    z = (0,0,-turn,0) * World[N0.bone]                  // root -Z * turn
    for j in 2..count-1: V_j += z
```

Then, for all chains: `RK4Step(C)`, the second substep.

### Step C - `8216A9E8` [C]

For each chain:

```
repeat 4:                                 // Jacobi "only-stretch" constraint
    for s in 1..count-2:                  // segment s = nodes s,s+1 (root segment 0 excluded)
        if |P_s - P_{s+1}| > L_s:
            m = (P_s+P_{s+1})/2; n = normalize(P_{s+1}-m)
            pair[s] = (m - n*L_s/2, m + n*L_s/2)
        else pair[s] = (P_s, P_{s+1})
    for j in 2..count-2: P_j = (pair[j-1].second + pair[j].first)/2
    P_{count-1} = pair[count-2].second
for s in 1..count-2:                      // final strict pass, top-down
    if |P_{s+1}-P_s| > L_s: P_{s+1} = P_s + normalize(P_{s+1}-P_s)*L_s
```

Collision is skipped for **all** chains if `chain0.N0.pos.y > 5.0` or `sphere0.y > 0`, that is, when the character is below the floor:

```
for j in 2..count-1, for every sphere (c,r):
    R = r + chain.radius
    if |P_j - c| < R: P_j = c + (P_j-c)*R/|P_j-c|;  V_j = 0
floorY = (char+444 byte != 0) ? H+24384 : -12.0      // H+24384 = 0.0, or -8.4 in the ring (82556190/8216A830)
lim = floorY - chain.radius
for j in 2..count-1: if P_j.y > lim:
    if !arenaHasRing() (821871B0==0) || (|x|<32 && |z|<32 using [0x82D9D704]=64):  P_j.y = lim
```

After that, every node of every chain gets `prev = pos`.

### Step D - write-back `8216CAE8` [C]

```
W = World[N0.bone]                         // animated root
len = Local[N1.bone].row3.x                // bone offset of node 1
if A == 0:
  for j in 1..count-2:
    t   = (P_{j+1},1) * rigidInverse(W);  t.x -= len        // target seen from node j origin, in parent frame
    q   = shortestArc((1,0,0), normalize(t))
    Lj  = Local[Nj.bone]
    Lj.rows0..2 = matrix(q)
    if j >= 2: Lj.row3 = (len,0,0,1)        // j==1 keeps animated translation
    newLen = |P_{j+1} - P_j|
    W = Lj * W
    World[Nj.bone] = W;  Skin[Nj.bone] = InvBind[Nj.bone] * W
    len = newLen
  for j in 1..count-2: P_j = World[Nj.bone].row3      // resync
else (A != 0, 2 chains in the stock data):
  same t, but only Local[Nj.bone].row3 = t (j>=2); W = Lj*W; no world/skin write;
  the computed rotation is unused  [C]
```

The bones' twist about X is lost, because the new rotation is the minimal arc from +X.

### Reset `8216A830`

This is slot 4 of the +16 interface; it is also called right after construction.

- [C] Every node is placed at `World[C+8].row3 + (0, ΣL, 0)`.
- [C] `V=F=0`, `prev=pos`, `N0.pinned=1`, and `prevDir=(0,0,0,1)`.
- [C] `H+48 = model[304]` (use not found).
- [C] `H+24384 = 0`, or `-8.4` when the arena is 78 and `82185690()` returns false.

---

## 4. gBaM muscle jiggle (`8226D910` sim, `8226DFD8` write-back)

This is a different solver: a point mass per bone, tied to the bone's world position by a damped spring. The output is a translation of that bone's skin matrix only. Children are not affected [C].

### Scheduling

- [C] The update is vtable slot 6, `8226DFC0`. If `model+736 != 0` it calls `8226D808`, which resets: mass = anim position, v=0, F=0. Otherwise it calls `8226D910`.
- [C] The write-back is slot 1 of the +32 interface (`8226E270` → `8226DFD8`).
- [C] The constructor (`8226D2E8`, then `8226D620`) sets:
  - `bone[i]` and a pointer `P[i] = &World[bone].row3`;
  - `X_i = M_i = P_i + (0,0.1,0)` (sim position at `M+176+16i`, previous anim position at `M+416+16i`);
  - `L0 = 0.1` for all 15 bones (`M+716+4i`);
  - `V = 0`, `F = 0`.

### Simulation [C]

```
for each bone i (row r):
    mv = |P_i - Mprev_i|                          // anim movement this frame
    if | mv - mvPrev_i | > 0.7 or mv > 10.0:  X_i += P_i - Mprev_i     // teleport: drag the mass along
    mvPrev_i = mv;  Mprev_i = P_i
    F_i = -visc[r]*V_i;  F_i.y += grav[r]
for each bone i:
    d = P_i - X_i;  len = |d|;  if len == 0: d.y = 0.001, len = |d|
    F_i += d/len * ( spring[r]*(len - 0.1) + damp[r]*(V_i.d)/len )
for each bone i:
    V_i += F_i * time[r]          // "時間" = time step multiplier
    X_i += V_i
```

### Write-back [C]

This runs after the world matrices exist.

```
off   = X_i - P_i - (0,0.1,0)
local = (off.x·Xaxis, off.y·Yaxis, off.z·Zaxis)  i.e. (off,0) * transpose(World)   // bone-local
local *= (limitX[r], limitY[r], limitZ[r])        // "リミット" is a per-axis GAIN, not a clamp
wOff  = (local,0) * World
tmp = World; tmp.row3 += wOff
Skin[i] = InvBind[i] * tmp            // 821605F0(model+184, model+196, model+180)
World[i].row3 unchanged (restored)
```

In the stock data, gravity is 0 everywhere. The mass therefore sits 0.1 from the bone origin in the direction it last lagged, which gives a small velocity-driven sway.

---

## 5. Switches and frame-rate summary

| what | where | effect |
|---|---|---|
| component exists | `8216E1F8` / `8226E1F0` | hair needs `desc+76`, muscle needs `desc+52` |
| hair kick gate | `8216D268` | `char+1096 != 0` [L: character active/visible] |
| hair threads | `8216B0E0` | needs the worker object `[0x82DE0374+4*(slot&1)]` |
| muscle reset instead of sim | `8226DFC0` | `model+736 != 0` [L: pose reset/teleport flag] |
| hair collision off | `8216AE44` | chain0 root y > 5 or capsule0 sphere0 y > 0 |
| floor height | `8216AFA4` | `char+444` ? `H+24384` (0 / -8.4) : -12.0 |
| floor only inside ring | `8216B000` | arenas <40 and 76/77/79 (`821871B0`) |
| low fps | `8216A6D8` | fps 30/25 → movement-correction minimum 0.75 instead of 0.5 |
| load warm-up | `82323300` | 30 kicks |
| LOD / camera distance / cutscene | - | none found in any of the hair/muscle paths |

- Hair and muscle have no dt and run one step per character update: hair runs 2 RK4 substeps, muscle 1 Euler step.
- At 30 fps on the original hardware they ran half as often. Only the hair's movement correction and the ropes compensate.
- The ropes are BaneHairThread1, job `82198B78`. They use 2 substeps instead of 1 at 25/30 fps, on 3 rope objects at `obj+64/68/72`.
- "BaneRopeThread" (`82167E98`) calls vfunc 80 on characters whose `+2624 == 4`. It was not analysed.

---

## 6. CAW path (`kami_a` / `kami_k` / `hira_a` / `hira_k`) - structure [C], solvers only partly read

### Data

- The data comes from the part's own PACH text in the edit pacs: ids 2332/2338/2339 in ed10 (hair), and 2316/2317/2333-2337 in the cloth pacs. The part entry in `desc+16` is 16 bytes: `{size, ptr, u16 partId}`. `82229358` creates the tokenizer at `part+196`.
- Format (examples in `re/edtxt2/`):

  ```
  kami_a00 ;comment
  {
  atama   v1 v2 v3 v4 v5 v6 [v7]
  kubi    ...
  }
  ```

- The part object `8222AAB8` also scans the same text for `ZOFFSET n`. The int n goes to `submodel+24` as a float. It is a draw/sort offset, not physics [L].

### Chain type and solver

- [C] The default type comes from the chain name (section 2).
- [C] When the text is applied (`8222E1D8` → `82229158`), token 1 of the chain's header line may override the type. The override names are in the table at `0x82D9E940`: `ATARI_SD5`=0, `ATARI_KAMI0`=1, `ATARI_HIRA0`=2, `KAKUDO_KAMI0`=3, `KAKUDO_HIRA0`=4, `KAKUDO_HIRA1`=5.
- [C] No stock file uses an override.
- [L] So kami_a = ATARI_KAMI (collision hair), kami_k = ATARI_HIRA, hira_a = KAKUDO_KAMI ("angle" variant) and hira_k = KAKUDO_HIRA0.
- [C] The type selects the solver (`8222E460`):

  | type | solver |
  |---|---|
  | 0 | `8222BAA0(c,0)` |
  | 1 | `8222BAA0(c,1)` |
  | 2 | `8222ACE8(c,1)` |
  | 3 | `8222BAA0(c,0)` |
  | 4 | `8222ACE8(c,0)` |
  | 5 | `8222CB08` |

- [C] The type also selects the block parser: `822299C8` for types 1 and 3, `82229000` for types 2 and 4, `82229C68` for type 5.

### Block lines (types 1/3, `822299C8`) [C]

- Each `bone v1..v6` line becomes collision body k, at most 6 per chain (count at `chain+56`). The body is 160 bytes at `chain+96+160k`; its bone (default node 2 if not found) is at `chain+60+4k`.
- `body+0 = (v4,v5,v6)*scale` is the center offset in bone space.
- `body+144 = (v1,v2,v3)*scale` holds the shape extents.
- `v7` (0.9 in every file) goes to `chain+3924`. It is a per-chain factor used by the solver at `8222C5EC`.
- Every frame, `822F3B30` builds the body's world matrix (bone world, translation = center) and its inverse. `82229900` copies the body to the global table at `0x82E38BD0` (`8222E4C0`).
- The test against a point is `822F3C28`, not decoded.
- [L] `v1..v3` are ellipsoid radii along the bone axes. Type-5 bodies are generated as `(L/2,0,0)` / `(L/2,0,1)` from each node length.

### Solver `8222BAA0` (kami) [C/L]

It is **not** a mass-spring. It is a procedural angular solver:

- It keeps a per-node velocity/offset history (`chain+2208…`, `+2464…`, a speed ring at `+2080`). It reads the root's world-space movement (`82E37C30` scratch block) and damps it with constant factors: 0.9, 0.92, 0.3, 0.035, 0.36/0.28, and 0.95/0.75/0.35 chosen by speed.
- It adds a 16-bit phase sine wobble (`8269E558`, amplitude 0.08 or 0.15 by the flag, step -0.0078). It adds the HAIM "wind" vector `HAIM+112` × node index × 0.25.
- `HAIM+112` is driven by `8222A058`: it decays by 0.92 per frame and gets impulses from `HAIM+144` while a timer at `HAIM+160` counts down.
- It caps speed at 12 and pushes out of the collision bodies and the floor (`char[100]+2360`).
- It converts each segment direction to atan2 yaw/pitch, clamps pitch to `[0, chain+3624 = 80°]` (`+3628` for the first segment), rebuilds the local matrix from axis-angle (`826A86D0`/`826A7FB8`), and writes the local and world matrices.
- With the reset flag `chain+2160`, it calls `8222A778` and runs 12 extra iterations when `[0x82E3CCF8]==5`.
- `8222ACE8` (hira/flap) and `8222CB08` (`KAKUDO_HIRA1`, uses ±π/2, 2π and 1200) are the same family. They were not decoded.

**Recommendation for the viewer:** approximate CAW `kami_a`/`hira_a` chains with the gBaH solver below. Use stock-like parameters, for example `kami_b`: radius 0.15, visc 0.1, grav 0.034, spring 0, damp 0, bend 0. Use the CAW bodies as spheres at `v4..v6` with radius `max(v1..v3)`. An exact port needs about 3.5k more instructions decoded (`8222BAA0`/`ACE8`/`CB08`).

---

## 7. Reimplementation recipe (gBaH + gBaM, exact to the code above)

```python
# ---------- inputs ----------
# skel: nodes with parent, bind local 4x4 (row-vector), per frame animated Local[] -> World[] (row-vector, +Y down)
# e60 lines -> chains, e61 lines -> capsules, e50 rows -> muscle params; char_scale (1.0 for stock)
G60, LOWFPS = 1.0, False          # 60 Hz

class Node: pos, vel, frc, prev (vec3); pinned; bone
class Chain: nodes[]; parent; L[] ; radius, visc, grav, spring, damp, A, bend, movecorr; prevDir=(0,0,0)

def build_chain(name, params):
    root = find(name); kids = [find(f"{name}_{k:02d}") for k=0.. until missing]
    c = Chain(); c.nodes = [Node(b) for b in [root]+kids]; c.parent = skel.parent[root]
    tipLen = bindLocal[kids[-1]].row3.x
    c.L = [tipLen]*(len(c.nodes)-1)
    reset(c)                                    # 8216A830
def reset(c):
    p = World[c.parent].row3; y = 0
    for j,n in enumerate(c.nodes):
        n.pos = p + (0,y,0); n.prev = n.pos; n.vel = n.frc = 0
        n.pinned = (j == 0)
        if j < len(c.L): y += c.L[j]
    c.prevDir = 0

def forces(c, P, V):
    F = [-c.visc*v + (0,c.grav,0) for v in V]
    for j in range(len(P)-1):
        d = P[j]-P[j+1]; l = |d|
        if l < 1e-4: d.y = 1e-4; l = 1e-4
        s = (c.spring*(l-c.L[j]) + c.damp*dot(V[j]-V[j+1], d)/l) / l
        F[j] -= d*s; F[j+1] += d*s
    return F

def rk4(c):                                   # 8216B5E8 incl. accumulating trial state
    P0=[n.pos for n in c.nodes]; V0=[n.vel for n in c.nodes]; pin=[n.pinned for n in c.nodes]
    F1=forces(c,P0,V0); AV=V0[:]; AF=F1[:]
    TV=[V0[j]+0.5*F1[j] for j]; TP=[P0[j] if pin[j] else P0[j]+0.5*V0[j] for j]
    F2=forces(c,TP,TV); V2=TV[:]
    AF+=2*F2; AV+=2*V2; TV=[TV[j]+0.5*F2[j]]; TP=[TP[j] if pin[j] else TP[j]+0.5*V2[j]]
    F3=forces(c,TP,TV); V3=TV[:]
    AF+=2*F3; AV+=2*V3; TV=[TV[j]+F3[j]];     TP=[TP[j] if pin[j] else TP[j]+V3[j]]
    F4=forces(c,TP,TV); V4=TV[:]
    AF+=F4; AV+=V4
    for j,n in enumerate(c.nodes):
        n.vel += AF[j]/6
        if not pin[j]: n.pos += AV[j]/6

def spheres_from_capsules():                  # per frame
    S=[]
    for cap in caps:
        W=World[cap.bone]; s=char_scale
        P0=(cap.p0*s,1)*W; P1=(cap.p1*s,1)*W; R0=cap.r0*s; R1=cap.r1*s
        M=(P0+P1)/2; RM=(R0+R1)/2; S3=(P1+M)/2; S4=(P0+S3)/2
        S += [(P0,R0),(P1,R1),(M,RM),(S3,(RM+R1)/2),(S4,(RM+R0)/2)]
    return S

def hair_frame(chains, floorY=0.0, ringHalf=32, ring=True):
    S = spheres_from_capsules()
    for c in chains:                                           # 8216D618
        n0=c.nodes[0]; W=World[n0.bone]; n0.pos=W.row3
        d=n0.pos-n0.prev
        if |d| <= 1.0:
            loc=normalize((n0.prev,1)*rigidInverse(W))
            f=c.movecorr*G60
            if loc.y<0: f=(loc.y+1)*c.movecorr
            f=max(f, 0.75 if LOWFPS else 0.5); d=d*f
        for n in c.nodes[1:]: n.pos+=d; n.prev+=d
        c.nodes[1].pos=World[c.nodes[1].bone].row3; c.nodes[1].pinned=True
        rk4(c)
    for c in chains:                                           # 8216C6F0
        v=(0,0,-1,0)*World[c.parent]; turn=min(|v-c.prevDir|*0.1, 0.02); c.prevDir=v
        N=c.nodes
        for j in range(2,len(N)):
            e=2*N[j-1].pos-N[j-2].pos-N[j].pos
            if |e|/c.L[0] > 0.1:
                N[j].vel += e*c.bend*(0.5 if j>=4 else 0.75 if j==3 else 1.0)
        if turn>0.001:
            z=(0,0,-turn,0)*World[N[0].bone]
            for j in range(2,len(N)): N[j].vel+=z
    for c in chains: rk4(c)
    for c in chains:                                           # 8216A9E8
        N=c.nodes; n=len(N)
        for it in range(4):
            pair={}
            for s in range(1,n-1):
                a,b=N[s].pos,N[s+1].pos
                if |a-b|>c.L[s]:
                    m=(a+b)/2; u=normalize(b-m); pair[s]=(m-u*c.L[s]/2, m+u*c.L[s]/2)
                else: pair[s]=(a,b)
            for j in range(2,n-1): N[j].pos=(pair[j-1][1]+pair[j][0])/2
            if n>2: N[n-1].pos=pair[n-2][1]
        for s in range(1,n-1):
            if |N[s+1].pos-N[s].pos|>c.L[s]:
                N[s+1].pos=N[s].pos+normalize(N[s+1].pos-N[s].pos)*c.L[s]
    if not (chains[0].nodes[0].pos.y > 5.0 or (S and S[0][0].y > 0)):
        for c in chains:
            for nd in c.nodes[2:]:
                for (ctr,r) in S:
                    R=r+c.radius; dd=nd.pos-ctr; l=|dd|
                    if l<R: nd.pos=ctr+dd*(R/l); nd.vel=0
            lim=floorY-c.radius
            for nd in c.nodes[2:]:
                if nd.pos.y>lim and (not ring or (abs(nd.pos.x)<ringHalf and abs(nd.pos.z)<ringHalf)):
                    nd.pos.y=lim
    for c in chains:
        for nd in c.nodes: nd.prev=nd.pos
    for c in chains:                                           # 8216CAE8
        N=c.nodes; W=World[N[0].bone]; ln=Local[N[1].bone].row3.x
        for j in range(1,len(N)-1):
            t=(N[j+1].pos,1)*rigidInverse(W); t.x-=ln
            newLen=|N[j+1].pos-N[j].pos|
            Lj=Local[N[j].bone]
            if c.A==0:
                Lj.rot3x3=quatToMat(shortestArc((1,0,0),normalize(t)))
                if j>=2: Lj.row3=(ln,0,0,1)
                W=Lj*W; World[N[j].bone]=W; Skin[N[j].bone]=InvBind[N[j].bone]*W
            else:
                if j>=2: Lj.row3=(t,1)
                W=Lj*W
            ln=newLen
        if c.A==0:
            for j in range(1,len(N)-1): N[j].pos=World[N[j].bone].row3

# ---------- muscle (gBaM) ----------
ROW={'koshi':0,'mune':1,'kubi':2,'l_sakotsu':3,'r_sakotsu':3,'l_momo':4,'r_momo':4,'l_momo_x':5,'r_momo_x':5,
     'l_sakotsu_m':6,'r_sakotsu_m':6,'l_ninoude_m':7,'r_ninoude_m':7,'l_kote_m':8,'r_kote_m':8}
# init: X=Mprev=World[b].row3+(0,0.1,0); V=0; mvPrev=0; L0=0.1
def muscle_frame():
    for m in muscles:
        p=World[m.bone].row3; r=m.row
        mv=|p-m.Mprev|
        if abs(mv-m.mvPrev)>0.7 or mv>10.0: m.X+=p-m.Mprev
        m.mvPrev=mv; m.Mprev=p
        m.F=-visc[r]*m.V; m.F.y+=grav[r]
    for m in muscles:
        p=World[m.bone].row3; d=p-m.X; l=|d|
        if l==0: d.y=0.001; l=|d|
        m.F+=d/l*(spring[r]*(l-0.1)+damp[r]*dot(m.V,d)/l)
    for m in muscles:
        m.V+=m.F*time[r]; m.X+=m.V
    # write-back (after World is final):
    for m in muscles:
        Wm=World[m.bone]; off=m.X-Wm.row3-(0,0.1,0)
        loc=(dot(off,Wm.row0),dot(off,Wm.row1),dot(off,Wm.row2))*(limX[r],limY[r],limZ[r])
        wo=loc.x*Wm.row0+loc.y*Wm.row1+loc.z*Wm.row2
        T=Wm.copy(); T.row3+=wo
        Skin[m.bone]=InvBind[m.bone]*T          # World itself unchanged
```

Order per frame:

1. Animation, then Local, then World, then Skin.
2. `muscle_frame()` simulation.
3. `hair_frame()`, which rewrites Local, World and Skin of the hair bones.
4. The muscle write-back to Skin.
5. Draw.

The relative order of the muscle and hair steps does not matter, because they touch different bones.

## 8. Open points

- The loader that fills `desc+52` / `desc+76`, so the exact PACH-id mapping is still [L]. Also the consumers of entries 40 and 80.
- The exact semantics of `char+1096`, `model+736` and `char+444`.
- The CAW solvers `8222BAA0` / `8222ACE8` / `8222CB08` and the body test `822F3C28`: only outlined.
- Whether anything reads `lencorr` or the capsule `k` through another base. Neither is read inside the Bane module.

## 9. Checked against the game (2026-10-14)

`src/hair_dump.cpp` (test aid, `SVR2011_TEST_HAIR_DUMP=<file>`) logs the gBaH state of the character in
slot 0 around each per-frame step (positions, velocities, previous positions, pins; the collision spheres;
the head directions H+24128; each chain's rest lengths and parameters). The scratchpad `replay.py`
re-runs every step from the game's state before it with the recipe in section 7 and compares with the
game's state after it. Edge vs Chris Jericho (exhibition, arena 1, the test-match route), 1755 frames,
7 chains (3-5 nodes, segments 0.851; visc 0.10-0.15, grav 0.08, spring 0.02, damp 0, radius 0.15-0.20,
bend 0.03-0.05, movecorr 0), 12283 chain-frames:

| step | max error (units, positions and velocities) |
|---|---|
| A: root follow (f = 0.5), pin, forces + RK4 #1 | 0.00003 |
| B: bend + RK4 #2, frames without the head-turn kick (3738) | 0.000025 |
| B: frames with the kick (8540) | 0.018 when the replay leaves the kick out - the kick axis is the hair root's -Z (World[N0.bone]), which the dump doesn't log; with the head's -Z in its place the error is 0.037 |
| C: length passes, sphere collision, floor | 0.00007 |

What the check settled:
- The RK4 with the accumulating trial state is exact as written (section 3).
- The floor is **-12.0** in a normal ring match (`char+444` clear there), and the clamp applies only inside the
  ring's 64 x 64 square (an arena with a ring); collision is skipped while the character is below the floor
  (chain 0's root y > 5 or sphere 0's y > 0) - during the entrance (z ~ 300, y ~ +10) the hair is not clamped.
- Not checked in game: the write-back (step D, bone matrices) and the muscle jiggle (gBaM) - both from code only.
