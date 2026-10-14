# Hair physics: implementation plan (viewers, converters)

What to build so hair (and later jiggle / CAW cloth) moves like the game's. The behaviour itself is in
`docs/HAIR_PHYSICS.md` (section 7 is the recipe, section 9 how it was checked); `tools/hair_sim.py` is a
Python reference of the hair solver, `tools/data/hair_ref_edge.txt` 60 frames of the game's own hair state
(Edge) to test a reimplementation against.

## Facts both sides need

- Hair chains are bones named `kami_bNN` (NN 00-15) with children `kami_bNN_00`, `_01`, ... (found by name,
  in order, until one is missing). Parent of `kami_bNN` is normally `atama`; each `_MM` hangs off the
  previous one. The real parent is the node's s32 at +48 (`jboy.py` `parent` since d05be04).
- Roles in a chain of n nodes: node 0 (`kami_bNN`) is a fixed pivot (never rotated); node 1 (`_00`) follows
  the head rigidly (pinned); nodes 2..n-1 are simulated; the last node is only a target (its bone is never
  written - it must be an end bone with no skin weights). So skin hair only on nodes 0..n-2.
- Bones point along their local **+X**: a chain bone's local translation is (L, 0, 0) and its rotation turns
  +X towards its child. Every segment of a chain uses the **same** rest length L = the last node's local x.
- The parameters are text children of the attire's model package (EMD PACH):
  - 0x3C (60) - one line per chain: `kami_bNN radius visc grav spring damp A lencorr bend movecorr`
    (Shift-JIS comment lines start with `;`; A 0, lencorr unused). At most 16 chains.
  - 0x3D (61) - body collision capsules: `bone x0 y0 z0 r0 x1 y1 z1 r1 5` (points in the bone's frame,
    times the character scale; the trailing number unused). At most 14.
  - 0x32 (50) - body jiggle, 9 rows of 8 floats (viscosity, gravity, spring, damping, time, gain x / y / z)
    for koshi, mune, kubi, sakotsu, momo, momo_x, sakotsu_m, ninoude_m, kote_m.
- Stock values (570 chain lines): radius 0.15-0.20, visc 0.10-0.15, grav 0.034-0.08, spring 0-0.02,
  damp 0, bend 0.03-0.05, movecorr 0-0.45 (0 typical); segments ~0.85.
- 60 Hz, one step per frame (no time step in the solver); the floor is -12 in a normal match (ring matches:
  clamp only inside the 64 x 64 square).

## A. Mod Maker viewers (ModMakerUIRework) - `modmaker/char_preview.cpp`

0. (Done by ModMakerUIRework, Main 14 Oct) each model skinned against its own node table: the stretch is
   gone, hair follows the head rigidly.
1. Load: parse the attire's 0x3C / 0x3D (and 0x32) children with the same tokenizer rules as the game
   (lines; `;` comments; whitespace-separated tokens; numbers as floats; a line whose first token isn't a
   chain root / bone name is skipped). Build chains by name from the hair model's node table (the model
   that has the `kami_b` bones - child 0x2710 on stock models).
2. Hair solver: port `HAIR_PHYSICS.md` section 7 (or `tools/hair_sim.py`) to C++ - per chain the node
   state (pos, vel, prev, pinned), steps A (root follow: half of the root's move carried, pin node 1 to its
   animated position, collision spheres from the capsules, RK4), B (bend, head-turn kick, RK4), C (length
   passes, spheres, floor), D (write-back: per chain bone the shortest rotation from +X to the next node,
   local / world / skin rewritten). Copy the RK4 exactly as documented (the trial state accumulates).
3. Timing: the preview draws at the display rate - step the solver at 60 Hz (fixed-step accumulator), and
   run 30 settle steps when a model loads or a motion is (re)started, as the game does at load.
4. Floor: the preview stands on y 0 - pass floorY = the stand height (the game's -12 is relative to its
   world; in the viewer it is just "below the feet").
5. Check: a test mode that feeds `tools/data/hair_ref_edge.txt`'s inputs through the C++ steps and compares
   (as `hair_sim.py` does) - errors ~1e-5 expected (step B ~1e-2 from the reference's kick axis).
6. Then the body jiggle (section 4 of HAIR_PHYSICS.md: one Euler spring point per bone, skin matrix only).
7. CAW hair / cloth (kami_a / kami_k / hira_*, sway texts in the edit pacs): optional; until its solver is
   read in full, drive those chains with the superstar hair solver and the stock parameters.

## B. Converters (Superstar Porting) - svr11_build (FBX / PS2 / 2K path), w13_char, svr07 / svr08_char

Today none of them write 0x3C / 0x3D and they fold source hair / cloth bones into atama / mune / koshi, so
ported hair is rigid. To make it move:

1. Chains: for each source hair strand (or a generated one), append nodes `kami_bNN`, `kami_bNN_00`, ...,
   tip, to the node table (svr11_build gives all three models one table - append there):
   - `kami_bNN`: parent atama, at the strand's root on the scalp;
   - each next node: parent the previous one, **local translation (L, 0, 0) with one L for the whole chain**,
     local rotation turning +X along the strand (so the bind pose points every bone at its child);
   - the tip: one more node, no weights.
   `with_bones()` (extra bones: translation only, no rotation) is not enough as is - it needs the rotation
   and the equal-length rule.
2. Weights: skin the hair mesh to the chain bones (nodes 0..n-2 only, at most 20 bones per mesh palette);
   whatever stays on atama is rigid.
3. Write 0x3C: one line per chain with stock-like values (above), e.g.
   ` kami_b00 0.1500 0.1000 0.0340 0.0000 0.0000 0 0.0000 0.0300 0.0000`; write 0x3D: copy the template's
   when it has one (all templates with hair do), else the stock set from ch102 (`HAIR_PHYSICS.md` /
   any stock attire). Keep 0x32 as now (the template's).
4. Per source:
   - WWE '13 / 2K: same engine family and the same `kami_b` names (w13_char already maps them to atama) -
     check whether the source attires carry 0x3C / 0x3D; if so keep the chains (with the +X / L rules
     checked) and copy their entries; else generate as in 1-3.
   - FBX: chains from the model's hair bones (rig_maps: a hair-chain mapping), or none.
   - PS2 / SvR 2007 / 2008: no usable hair rigs (PS2 has none; 2007 / 2008 cloth tails go to mune) - leave
     rigid unless a strand generator is added (optional: chains from the hair mesh's long cards).
   - SvR 2010 (svr10_char): keeps every child already - its hair moves (if 2010's entries are the same).
5. Check in game: `src/hair_dump.cpp` (branch hair-dump) on the converted superstar in slot 0 - chains found
   (P lines), nodes moving, no stretch; `tools/hair_sim.py` on the dump.
