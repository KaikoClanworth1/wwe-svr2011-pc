# SvR2011 runtime API for HCTP gimmicks in backstage rooms

Read-only research, 2026-10-07. This builds on re_svr2011.md.

Tags:
- **[C]** confirmed from the code;
- **[L]** likely;
- **[G]** guess. Test it before relying on it.

Conventions:
- Addresses are guest addresses. Memory is big-endian (`base+addr`).
- `f` = a fighter (CChar). Fighters are listed at **0x82E3CC50** (8 slots of pointers).
- "obj" = a motion object: a fighter, a gimmick prop or a weapon. All three share the motion base (ctor
  sub_82396DF8), so the same motion and position fields work on all of them.
- Query helper: `rq.py` in this folder. It loads the database once and runs several commands:
  `python rq.py "dis 824B3480" "calls 821d2070" "grep re"`.

## 0. Bug found in the current port code [C]

`hctp_room.cpp` hooks `sub_821E5910` and treats `ctx.r3` as the fighter. It is not.

- The function is `sub_821E5910(collisionComponent, body, outVec)`.
- The only game caller, sub_821E70C0, loops over collision components.
- The fighter is at **`*(r3+32)`**, and the code reads `+455` through it (`lwz r11,32(r27); stb ...,455(r11)`).

So `Climb()` never finds a kChars slot and never runs. Use `f = Rd32(base + ctx.r3 + 32)`.

---

## 1. Gimmick runtime access

**Manager** [C]: `mgr = *(u32*)0x82E3DBCC` (lis -32028, -9268).

| field | contents |
|---|---|
| mgr+16 | element count |
| mgr+20 | `element*[400]` |

- `sub_824B1328(mgr, i)` gives element i, or 0.
- **`sub_824B0DC8(mgr, recordId, nth)`** gives the first element (nth = 0) whose **+72 == recordId**. Record id =
  the locator's `word12 >> 17` = the GMPD record index. Returns 0 if none.
- Element type check: `+68`. Only accept 23 for objects (as sub_824B9AC8 does).

**Element layout** [C]. The ctor is sub_824B92A0, then sub_824B9400, then sub_824B93B8(elem, locator).

| field | contents |
|---|---|
| +0 | vtable |
| +16 | "draw" enable. The manager draw pass calls vt+8 when it is non-zero |
| +20 | "update" enable. The manager update calls vt+4 when it is non-zero |
| +32..+44 | position x, y, z, 1 (from the locator) |
| +48..+60 | rotation x, y, z, 1 (radians [L]; rot y is copied straight into fighter +276) |
| +64 | locator pointer |
| +68 | type |
| +72 | record id |
| +76 | package index |
| +80 | record pointer |
| +84 | &word1 (p0..) as floats |
| +88 | word0 (int) = model id (1000-1255 bank 0, 1500-1755 bank 1) |
| +92 / +96 | draw node of the bank 0 / bank 1 object |
| **+100 / +104** | **the T23's object instances** (bank 0 / bank 1); either may be 0 |

- vt+20 / vt+24 return a pointer to the position / rotation.
- sub_824B9150 rebuilds a node matrix from pos/rot: vt+40(matrix, -1), then vt+44.

**Object instances** [C].
- The room package is `pkg = sub_825052E0(*(u32*)0x82E3DBC8, idx)` = `((u32*)*(0x82E3DBC8))[idx]`.
  - Index **2** is the gimmick (GMGB) package used by T33 and by the stage code.
  - An element uses `elem+76`.
- The objects live at `pkg+2200`: 256 pointers for bank 0 and 256 for bank 1.
- **`sub_824B3C10(pkg+2200, modelId)`** returns the object for model 1000-1255 / 1500-1755, or 0.
  **One object per model id.** Two T23 records with the same model share the instance.

**Object instance layout.** The ctor is sub_824B42C0 (vtable 0x82020D70). The base is the motion object
sub_82396DF8.

| field | contents | tag |
|---|---|---|
| +36 | component holder | [C] |
| +84 | draw node (vt+8 pose count, vt+44 commit) | [C] |
| **+212** | motion key (low 15 bits) | [C] |
| **+216 / +220** | x / y | [C] |
| +228..+236 | previous key / x / y | [C] |
| **+24** | current frame (float) | [C] |
| +28 | previous frame | [C] |
| **+272..+284** | rotation x, y, z, 1. **+276 = yaw, in radians, wrapped to ±π** | [C] |
| **+288..+300** | position x, y, z, 1 | [C] |
| +304 | physics body pointer: fighters and weapons keep their position at body+0/+4/+8 as well | [C] |
| +476 / +780 | the two draw instances: +0 "show" word, +16 visible word | [C] |
| +1088 | component list. The 'S3MW' (0x53334D57) component's +12 = 1 runs the object's motion | [C] |
| **+1120** | object active (update and draw). vt+16 returns it, and sub_824B41A0 only updates when it is set | [C] |
| +1124 | shown flag | [C] |
| +1128 / +1132 / +1136 / +1144 | word0..word5 options set by T23 init | [C] |
| **+1140 / +1164** | if either is non-zero, sub_824B46F8 skips copying the motion pose to the node, so the drawn pose freezes | [L] |

**Read or write the position.** Write obj+288/+292/+296 and obj+272..+280, as T23 init and sub_824B3480 do.
- For a prop, also keep elem+32 / elem+48 in step, because the game snaps fighters to them.
- Then rebuild the node: `sub_824B9150(elem, elem+92 node, elem+32, elem+48)`, and the same with +96.
  Prop nodes are only refreshed by this call and by the motion update [L].

**Hide or show** [C]:
- **show**: `sub_824B4198(obj, 1)` then `sub_824B4150(obj, 1)`;
- **hide**: `sub_824B4198(obj, 0)` then `sub_824B4150(obj, 0)`.
  - sub_824B4198 sets +1124.
  - sub_824B4150 sets +16 of the draw instances at +476 and +780.
- **swap two models**: `sub_824B3C70(pkg+2200, hideModel, showModel)`, exactly as above. The game uses it
  for T33 variants 7 and 13: hide 1023, show 1133.
- **active flag**: `sub_824B8FE0(elem, active, visible)` sets +1120 = active on both objects and calls
  sub_824B4150(visible). It is used by T23 init word3.

---

## 2. Playing a gimmick-bank key on a T23 object

**Bank and lookup** [C]:
- the motion bank is `*(u32*)0x82E3CC40`;
- `sub_82394520(bank, key, x, y)` returns non-zero if (key, x, y) exists. hctp_room already hooks it.

**Recipe** [C, from T23 init and T33 / sub_824B91C0]:

```
// elem = T23 element (from sub_824B0DC8). Plays on both bank objects and enables the motion component.
if (sub_82394520(bank, K, x, y)) sub_824B91C0(elem, K, x, y);
```

- sub_824B91C0 runs `sub_82396E80(obj, K, x, y, f1 = 0.0)`, then sets S3MW+12 = 1, for +100 and +104.
- A whole object list (T33 +112: {n, obj[3]}) is `sub_824B9B68(list, K, x, firstY)`. Each object gets track
  y = firstY, firstY+1, ...
- **Track convention** [C]:
  - T23 idle keys (w5) use (K, 0, 0);
  - T33 grapples use **(K, x = 1, y = 2, 3, 4)** for objects, with the wrestlers on (K, 1, 0) and (K, 1, 1);
  - weapons used as props get (K, 1, 4..7).
  - HCTP prop motions converted into our bank should follow the same layout.
- For a direct call on a single object: `sub_82396E80(obj, K, x, y, f1 = startFrame)`.
  - Use `ctx.f1.f64` for f1.
  - sub_82396E80 is a tail call into `sub_82395E10(obj, K, x, y, r7 ignored, r8 = 1, f1)`.
  - That function stores +212/+216/+220, sets frame +24 = f1 (snapped to the frame step when the global
    rate at 0x82EDDBE8+8 is 25 or 50), clears +244..+256, calls vt+8 (reset), and binds the motion through
    the component 'BldC' (0x426C6443) vt+32.
  - Bit 15 of K sets +240 (a flag).

**Finished?**
- No "motion ended" helper was found for props [G].
- `sub_823960B0(obj, n)` returns 1 while frame +24 is in [n, n + step) [C]. Use it with the frame count of
  our own converted key: we author those keys, so we know N.
- Or watch +24 stop increasing.
- Fighters are different: their state machine replaces +212 when a motion ends. The game itself detects the
  end of a grapple by **`f+212 != key`** (sub_824B2960 does this for T33) [C].

**Hold the last frame:**
- `sub_82396040(obj, frame)` sets +24 and +28 [C]. Call it every frame with N−1.
- Or, cheaper: once at N−1, clear the S3MW component's +12 (stops the motion update), or set **obj+1140 = 1**
  (the pose is no longer re-uploaded) [L].
- Alternatively author the key with a long final hold.

---

## 3. Playing a key on a fighter

| call | what it does | tag |
|---|---|---|
| `sub_82397228(f, K)` | = sub_82395E10(f, K, 0, 0, r8 = 1, f1 = 0.0). The plain "JumpAct". No snapping | [C] |
| `sub_82397240(f, K)` | same, but keeps x = f+216 | [C] |
| `sub_82396E80(f, K, x, y, f1)` | the full form | [C] |
| **`sub_8218F550(a, v, K, x)`** | paired move. Sets a+96 = v, v+96 = a (opponents). For some keys it picks the variant x itself (130+n random, 135/136). Then plays **(K, x, 0) on a and (K, x, 1) on v**. Copies the per-pair tables (+2172.., +2284.., +2319/+2327), links the collision components (sub_823AE788), sets the +476 draw flags, then branches on the move class (sub_8221A628 / 8220AB68 / 8220ACA8 / 82202EC8 / 821819C0) | [C] |
| `sub_82191AA8(a, v, K)` | = sub_8218F550(a, v, K, 0). Used by the lumberjack code | [C] |

- **No position snapping** is done by any of these [C]. Callers snap first.
- **T22 start** (sub_824B67D0) [C]:
  1. JumpAct(f, p1).
  2. f+288 / f+296 = element pos x/z, and the same x/z written to the body: `*(f+304)+0`, `+8`.
  3. f+276 = element rot y (wrapped ±π).
  4. If the element has a model, the position is the object's +288 instead.
- **T33 grapple** (sub_824B3480) [C]:
  - sub_8218F550(a, v = a+96, K, 1);
  - then `sub_824B23F0(elem, v+288, v+272)`: the victim's x/z and yaw come from the T33 spot or its first
    object;
  - then `sub_824B2810(v+288, v+272, 0, f1 = ±dist)` steps him along his facing.
- **Moving a fighter so it sticks:** write f+288/+292/+296 **and** body `*(f+304)` +0/+4/+8. The game's
  placement code (sub_8227CEB8) and T22 both write both. The lumberjack notes report that writing only +288
  doesn't stick.
- **Root motion at the end** [G]:
  - not traced, because the frame advance is inside the BldC component;
  - the wrestler moves during moves, so the root is applied continuously [L];
  - verify after a climb key by logging +288/+292 before and after.
- Turning: `sub_8218CEF0(f, dAngle)` adds to +276 and wraps it [C].

---

## 4. Creating weapons at run time

**Table** [C]: `weapon*[72]` at **0x82DE0220** (lis -32034, +544). Note: **0x821E2220 in the earlier notes is
wrong**.

| field | contents |
|---|---|
| w+80 | model |
| w+1152 | slot |
| w+1156 / +1160 | class from sub_8227F0E8(model) |
| w+444 | place (0 ring / 1 floor) |
| w+2596 | 1 = stowed |
| w+96 | holder |
| f+1828 | slot of the fighter's held weapon |

- Free slot: `sub_821CDBA0()` returns 0..71, or −1.
- The object (2624 bytes) is made by `sub_821CF178(-1, model, 0, 0)`. It returns 0 unless the model is in the
  match's loaded weapon models (list at *0x82E3BE9C, built from the selected set: WPBS backstage).

**One-call spawn, used mid-match by the game itself** [C]:
- `sub_822C7C00` (broken pieces 33/34, motion 15160);
- `sub_82281EA8` (re-spawns a weapon, motion 15000/15005, place 11);
- `sub_8220DEA8(f, model)` (makes a weapon straight in the fighter's hand: slot -> f+1828, pickup motion
  15081 via sub_8218FDD0).

```
w = sub_821D2070(r3 = -1 /*any free slot*/, f1..f3 = x,y,z, f4..f6 = rot x,y,z (as the 45010 table: degrees [L]),
                 r7 = model, STACK: caller_r1+84 = motion (15010 lying, 15000 standing),
                 caller_r1+92 = place (1 floor), caller_r1+100 = flag (1; ->w+2600))
```

- What it does:
  - fills the slot;
  - writes +288/+272/+444/+1152;
  - plays the motion (sub_82396E80(w, motion, class, 0, 0.0));
  - registers the weapon: sub_821CD5A8, sub_821C0698 -> sub_821C3438.
- It returns w, or 0.
- **Stack args from C++:** use the managers.cpp / superstar_mods.cpp pattern.
  1. `saved = ctx; ctx.r1.u64 = saved.r1.u32 - 0x200;`
  2. Write the three u32s at `base + ctx.r1.u32 + 84/92/100`.
  3. Call it, then `ctx = saved`.
- **Stow:** `sub_821CECB0(w)` hides it (the +476 words = 0) and sets +2596 = 1.
- **Re-place a stowed weapon:** `sub_8227CEB8(slot, x,y,z, rx,ry,rz, model, STACK +84 motion, +92 place)`.
  - It requires `table[slot]->model == model` and `+1152 == slot`.
  - It resets velocities and state, sets the body position, shows the weapon and plays the motion.
- **Safest HCTP spawn** [L]:
  1. When the match loads (after `__imp__sub_8227B938` in the existing match_types hook), create the spare
     weapons with `slot = sub_821CDBA0(); w = sub_821CF178(-1, model, 0, 0); table[slot] = w; w+1152 = slot;
     sub_821CECB0(w)`. This is exactly how sub_8227B938 makes its extras.
  2. On a prop break, call sub_8227CEB8 for that slot.
- Simply calling sub_821D2070 mid-match also works (the game does it), but it allocates.

**WPBS model ids** (backstage set; entry keys < 40000):
3, 4, 5, 9, 11, 15, 16, 17, 20, 27, 60, 63, 65, 72, 81, 88, 89, 90, 102, 112, 120, 142, 145, 146, 150.

Names come from WPBS 40000 (Japanese weapon-settings text, `mdl` = model×10 + variant) and from code. The
text is partly stale.

| model | name | tag |
|---|---|---|
| 4 | pipe chair | [C] |
| 5 | long table. sub_8227B938 alternates 5 -> 9 outside backstage | [C] |
| 9 | the text says shovel. Unclear, given the 5 -> 9 table swap | [G] |
| 11 | ladder | [C] |
| 17 | stacked tires, act 15305 | [C] |
| 60 | crutch | [C] |
| **63** | **trash can** (type 33, hp 4) | [C] |
| 65 | fire extinguisher | [C] |
| 89 | guitar | [C] |
| 90 | mop | [L] |
| 102 | HHH sledgehammer | [L] |
| 112 | pool cue (the docs say barbell) | [L] |
| **120** | **tire iron**: the closest pipe-like weapon; 78PL/RL's T24 rack holds three | [L] |
| 150 | belt prop (T33 variant 7) | [C] |

- No lid, cardboard box, barrel or lead pipe model exists in WPBS or WPON [C: id lists]. Map them:
  - lead pipe -> 120 (or 60);
  - box / barrel -> 63 or a new model added to WPBS.
- Adding a model needs a WPBS entry plus 40000/41000 data. Not researched.
- Name dumps: `wpbs_40000.txt`, `wpon_40000.txt` (identical).

---

## 5. Stage collision at run time

**Stage and TRIPs** [C]:
- `stage = *(u32*)0x82E35530`. stage+1152 = arena id (78).
- Per-area TRIP pointers: `stage+22516 + 4k`.
- Cell headers: `stage+22548 + 4k`. They are u32 per z-cell: u16 start at +0, u16 count at +2.
- Cell triangle lists: `stage+22580 + 4k` (u16 triangle indices).
- k is chosen by the live rule byte `*sub_825740C8()`:

| k | rule | entries | area |
|---|---|---|---|
| 0 | default | 996 | outline |
| 1 | 0x1C | 998 | |
| 2 | 0x20 | 1000 | hallway |
| 3 | 0x21 | 1002 | |
| **4** | **0x1B** | **1004** (index 1005) | **parking** |
| 5 | 0x1D | 1006 | locker |
| 6 | 0x1E | 1008 | locker, mirrored |
| 7 | 0x1F | 1010 | GL |

- Getters: `sub_821874C8(stage)` (TRIP), `sub_82187610` (headers), `sub_82187758` (lists).
- The user's "1003/1004" is HMD 1004 + index 1005 [L, from the 996 + 2k pattern].

**TRIP layout** [C]:
- +176 = triangle count, **+180 = pointer to `record*[count]`**.
- Each record is 96 bytes:

| offset | contents |
|---|---|
| +0..+63 | world -> triangle-local 4×4 matrix (sub_8216B248 transforms the body position) |
| **+12 / +28** | the **y range**, used as max and min in the broad phase |
| +64.. | in-plane triangle data (sub_826A9770 / 826AA5D0 / 826AA780) |
| +88 | half-extent in local x/z |
| +92 | near flag, **rewritten on every query** |
| **+93** | class |
| +94 / +95 | byte1 / low attribute byte |

**Query order (sub_826AB2E8)** [C]:
1. The broad phase sub_826A9CD8 runs on every listed triangle and sets +92.
2. **If arg5 != 255, every listed triangle whose +95 == arg5 gets +92 = 0** (disabled).
3. The narrow test sub_826AAFE0 runs.

Classes:
- class 3 collides always;
- class 2 only with bodies whose flag word +36 has 0x80;
- other classes need body flag 0x2 clear (0x100 bodies take a side path).

The only callers are in sub_821E5910: the stage query passes `li r7,255` at 821E6264.

**Ways to disable triangles:**
- **A. Per triangle** [C from code, untested]: set **+12 = +28 = 1.0e6** (both non-zero). The broad phase then
  never marks the triangle near. Save the two floats to restore it. No cache needs refreshing: +92 is rebuilt
  every query, and the cell lists are static.
- **B. Per attribute group** [C, untested]: give breakable rails / cage panels a unique low byte (e.g. 0xE1).
  Then hook `sub_826AB2E8` and, for the stage query (caller lr 0x821E626C), replace r7 = 255 with that byte
  while the group should be gone.
  - Only one byte value per query.
  - An unknown low byte reaching the wall switch falls to its default "stop" [C].
- Avoid changing +93 to 2: which bodies carry 0x80 is unknown [G].

**Moving triangles (the cart)** [L]:
- Translate a triangle by d: subtract `d·R` from the matrix translation (rows 0-2 are the rotation, row 3 /
  +48..+56 the translation, in the game's row-vector convention [L]). For a pure y move, also shift +12/+28.
- **The z-cells are static** (120 bands of 10 by |z|).
  - Build the index so the cart's triangles are listed in **every band along their travel** (data side,
    `index_build`).
  - The broad phase will then find them wherever they are. Nothing else caches positions.
- Note: wall collision is skipped during grapples (sub_821E5910 exits early).

---

## 6. Free per-frame hook points

These are not hooked anywhere in port/src [C, from a grep of REX_HOOK_RAW].

| function | when it runs | tag |
|---|---|---|
| **`sub_824B3D60(pkg+2200)`** | the gimmick object update for one package. Called **directly** from the manager update sub_824B0C50, for packages 0..11, once a frame. **Best place for room logic**: act when `r3 == pkg(2)+2200`, after `__imp__` | [C] |
| `sub_824B0C50(mgr)` | gimmick manager update: element vt+4 for elements with +20 != 0, then sub_824B3D60 for each package. It is reached through a vtable; whether REX_HOOK_RAW catches indirect calls should be checked [L] | [C] |
| `sub_821E70C0` | the collision manager (once a frame, loops over components and calls sub_821E5910). Reached indirectly | [C] |
| `sub_824B41A0(obj)` | per prop object | [C] |
| `sub_824B6168(f)` | the T21/T31 run-into search | [C] |
| `sub_824B31B8` | running into a T33 | [C] |
| `sub_824B3310` | whipped into a T33 (from sub_82233D90) | [C] |
| `sub_824B3480` | the T33 grapple start | [C] |
| `sub_8218F550` | every paired move | [C] |
| `sub_821D2070` / `sub_8227CEB8` | weapon spawn / place | [C] |

Hooks that already exist and can be reused:
- per fighter, once a frame: **`sub_82216C58(control)`** (match_types.cpp). control+32 = fighter, +36 = input object;
- `sub_821E5910` (hctp_room.cpp, see §0);
- `sub_82306B10`, `sub_824B6A70`, `sub_82394520` (hctp_room.cpp);
- `sub_8227B938` / `sub_8227D130` (match_types.cpp).

---

## 7. Fighter state

**Motion** (f+212 = key, +216 = x, +24 = frame) [C]:

| motion | meaning |
|---|---|
| 20500 | backstage idle / walk |
| 1000 | lying down |
| 20 | getting up |
| 50 / 51 | running (sub_821B6A18 also checks +632) |

- Throw victims go to 449 or 6801 -> 50 -> down (lumberjack notes).

**Predicates** [C code; meanings L]:

| function | test |
|---|---|
| sub_821B6A48 | IsRunning (50/51 or +632, and +1286 == 1) |
| sub_821B69D8 | +636, or motion 70-77, or 9900-9999 [G: special or down states] |
| sub_821B6B00 | same, or 1700-1859, or +544 [L: busy] |
| sub_821B9450 | airborne (not supported: CChar+457 bit 0) |

- Being thrown at someone: the attacker and victim are linked via +96 [C].

**Knock-down counting** [L]: count a fighter's transitions into motion 1000 (or out of the victim side of a
paired move with +96 set) while his x/z is on the panel. Log `SVR2011_TEST_MOTION_LOG=1` in the room to
confirm which ids appear.

**Other fields** [C]:
- facing: **+276**, yaw in radians, wrapped to ±π;
- position: +288..+296 (plus body *(f+304));
- floor/ring: +444 (0 ring, 1 outside);
- touched wall attribute: +455;
- supported: +457 bit 0;
- opponent: +96;
- person index: +1156;
- match frame counter: *0x82E3CD0C.

**Buttons:**
- The input object is control+36.
- `sub_8221D320(input)` updates it; it is called from sub_82216C58.
- **+68 = pressed (edge), +72, +76 = held. X = 0x10** [C from the lumberjack notes].
- The other bits are unmapped [G]. Log `+68/+76` from the existing control hook while pressing each button.
- CPU fighters don't use the input object, so AI must be handled separately.

**Damage / health apply:** not found in this pass [G].
- For steam, first play a hit reaction with JumpAct (victim strike reactions 300-307 exist).
- To find the health field, diff a fighter's 0x000-0xA40 block before and after a strike in a test match,
  then find the writer.

---

## Scripts added (this folder)

| script | what it does |
|---|---|
| `rq.py` | batched queries: `dis A [lo hi]`, `calls A`, `grep RE`, `ctx RE\|before\|after\|funcs`, `callees A` |
| `small.py RE maxlen lo hi` | small functions in a range that match RE |
| `s11_wpnames.py` | WPBS model keys and the 40000/40001/41000 dumps |
