# SvR2011: rope gameplay (static analysis)

Sources: generated code `SvR2011 Arenas/port/generated/default`, `image.bin`. Nothing was built or run.
"Confirmed" means read in the code. "Guess" means inferred from the code around it, not proven.

## Common facts

- **Per-fighter control dispatcher:** `sub_822EE2E0` (called from `sub_82216C58`). It walks the 116 handlers at 0x82008790 in order. Each handler is `f(ctx, fighter)`, `ctx+36` is the input object, and the walk stops at the first handler that returns nonzero. Confirmed.
  - Entry 7 = `sub_822E7C58`: if the fighter is running (`sub_821B6A18`: +212 == 50 or 51, or +632 != 0) and +1828 == -1, it calls the **run handler `sub_82306F90(fighter, input)`**.
  - Entry 22 = `sub_822ED230`: the **rebound-in-progress handler**. Its gate is `sub_821B6A80`: motion 60 with frame (+24 float) >= 2.0, or motion 62 with frame >= 12.0.
  - Entry 26 = `sub_822EA370`: for motion 65 or 927, it calls `sub_821F9778`.
- **Motion setters:** `sub_82397228(f, id)` sets a motion at once. `sub_82396E80(f, id, r5, r6, f1 blend)` sets one with a blend. `sub_82191AA8(f, opp, id)` starts a paired motion. `sub_8218E600(f, slot)` returns the motion id in the wrestler's move-set slot.
- **Motion ids (confirmed from the code that uses them):**
  - 50 / 51: running (51 is probably the run start). `sub_8220C808` sets 50 ("keep running").
  - 53: run brake. Set at 0x82307BD0 and 0x82307C6C when the player stops running.
  - 60: rebound for a wrestler running on his own (40 frames).
  - 62: rebound for a whipped wrestler (56 frames).
  - 63: "stop at the ropes". `sub_821F65C8` clears +1290, +1286 and +1287, then sets 63.
  - 64: whipped hard into the ropes (when +1760 > 220).
  - 1510..1537: the "on the ropes / groggy against ropes" family.
- **Fighter fields:**
  - +212 motion id; +24 motion frame (float).
  - +272 / +276 / +280 = ground x / **yaw** / z. `sub_8219E108(vec, q)` rotates x,z by q*90 deg and adds q*90 deg to the yaw.
  - +288..+296 position; +304 pointer to the position object; +112 physics body (+48 / +56 = x / z).
  - +440 rope side / quadrant. Nearest side comes from `sub_8218E400`: 0 = -z, 1 = -x, 2 = +z, 3 = +x.
  - +444 area (0 = in the ring).
  - +1284 corner index (-1 / 255 = none).
  - **+1286 whip state** (0 = running on his own, 1 = Irish-whipped, 2+ = no rebound); +1287 hard-whip flag.
  - +1965 input bits latched this frame; +2224 target opponent.
  - +1969 "touching ropes with rope break on", written every frame (see 2).
- **Ring lines:** global float 0x82E354E0 = 64*0.5 - 3.2 = **28.8**, the inner rope line, set by `sub_82D35F68`.
  - `sub_821BB530(f)`: inside the ropes (|x|, |z| < 28.8).
  - `sub_821B89B0(f)`: physics body past the 28.8 line while not in a rope motion (60-63, 700-799, 1515).
- **Live match rules:** 0x82E3DE00 (`sub_825740C8`). `sub_825740E0(i)` / `sub_825740F0(i, v)` get and set byte i.
  - Bytes 0..63 are filled by `sub_828C48E8`. It copies the rule's 64-byte record (`sub_828B4B70`), then overrides each byte with the user's match-option byte unless the record locks it (bit 7 of its own byte).

## 1. Rope rebound (running or whipped wrestler reaches the ropes)

### Where it is decided (confirmed)

In the run handler **`sub_82306F90`**, block **0x823075A0-0x82307650**:

```
if (motion != 62 && motion != 51 && +216 != 2
    && sub_821B4DD0(f)     // two bones (+176, +192) have y > -17: low / on the mat (guess)
    && sub_821B4E50(f))    // no other fighter in motion 15010/15013 between him and the ropes
{
    v = f->groundPos(+272);
    sub_8219E108(&v, -q);              // q = run-direction quadrant (r29 / r26)
    if (v.z < -19.0)                   // [0x82004AB8]: about 9.8 units from the 28.8 line
        { if (sub_823091A0(f, q)) return; }      // 0x8230761C
    else if (v.z < -12.0 && f[+1286] == 1)       // whipped: starts earlier
        sub_82309398(f, q);                      // 0x82307650
}
```

**`sub_823091A0(f, q)`**: the rebound decision for a wrestler running on his own.

- If +1828 != -1, it returns 0.
- It rotates +272 by -q and writes q to +440.
- Bit 0 of rule byte 59, with the global at 0x82E3CCEC >= 500: random motion 1457/1458 (a slip; guess).
- **+1286 >= 2: `sub_8220C808`, then it returns 1** (keeps running, no rebound).
- **Relative yaw within ±30 deg (0x82004650 / 0x82005330): `stb 0,+1965; sub_82397228(f, 60)` at 0x8230929C-0x823092AC.** This is the rebound.
- Later in the same function:
  - Hard whip (+1286 and +1287), with rule bytes 4 and 6 allowing it: `sub_82308BB8` (whip continuation, picks 62/712/713/81xx).
  - Bit 0 of rule byte 9 with +1286 and +1287: motion 1566 (r5 = 1 when |x| or |z| > 23).
- It returns 0 even after setting 60, so the run handler goes on with that frame.

**`sub_82309398(f, q)`**: whipped wrestler (+1286 == 1). Relative yaw must be within ±10 deg (0x82003814 / 0x82004510), and `sub_821B4E50` must pass. It then picks one of:

- 64 (hard whip with +1760 > 220; 62 when rule byte 6 is set);
- 62 (rule byte 14, or no fighter in motion 7061 in the way);
- 8040 / 8119 / 8120 (rule byte 6 random);
- 1457 / 1458.

It sets the motion at 0x82309620 (`sub_82397228`), or with `sub_82396E80` at 0x823094C4.

**Second path, for a wrestler who starts running right at the ropes:**

- At 0x82307560-0x8230759C: r24 != 0, +453 != 0, and the rotated **position** (+288) has z < -21.0. Then `sub_823054F0(f)` is checked: +432 < 0.97 with `sub_82305470`, or `sub_821B4DD0`; plus `sub_821B4E50`. If it passes, r27 = 1.
- Then, at 0x82307940 onwards:
  - r27 with motion 51: motion 60 at 0x82307978 (blend) or 0x823079C0. When rule byte 4 == 0 it is 717 instead.
  - r27 == 1: `sub_82308BB8` (whipped, frame < 5), or 712 / 560 / 1566, or **60 at 0x82307B70**.

**After the rebound starts:** `sub_822ED230` (entry 22) runs it.

- Each frame it clamps the physics body (+112 -> +48 / +56) to ±29.5 (0x82008784 / 0x82008788).
- Ends at frame 40 (motion 60) or 56 (motion 62).
- Pressing "stop" inside the "ROPEREBOUND" window (`sub_821BA5B8`, tuning name at 0x820036FC) switches to motion 63.
- An attack goes to `sub_822D24F8` (running attack).
- Once local z > -20.8 it switches to motion 50 (`sub_8220C808`).

### Hook proposal (no-ropes ring)

1. **Hook `sub_823091A0`.** When the ring has no ropes:
   - if +1286 != 0, clear bytes +1286, +1287 and +1290;
   - call `sub_82397228(f, 53)` (run brake);
   - set ctx.r3 = 1 and return. A return of 1 makes the run handler exit at 0x82308A60.

   Simpler but untested alternative: `sub_8220C808` plus return 1, which just keeps running. Do not leave the runner in motion 50 at the line: nothing found here stops a runner at 28.8 other than the rebound itself.
2. **Hook `sub_82309398`** (whipped): when there are no ropes, do the same as in step 1 (clear the whip bytes, motion 53 or 63) and do not call the original. This also removes the whip -> 64 ("groggy on the ropes") entry.
3. **Hook `sub_823054F0`**: when there are no ropes, return 0. That turns off the r27 path, so the 60 / 712 / 560 / 1566 / `sub_82308BB8` sites in `sub_82306F90` never run.

Optional: hook `sub_821B6A80` to return 0 when there are no ropes, so the rebound handler never runs. Not needed once 60 and 62 are never set.

Confidence:
- High that `sub_823091A0` and `sub_82309398` are the rebound decision points (direct motion 60/62 sets after a geometric test in the run handler).
- High for `sub_823054F0`.
- Medium that 53 is the best stop motion (it is the game's own run-release brake). 63 is the game's own "stop at the ropes", but its animation probably shows a rope grab.
- Untested in game.

## 2. Rope break (pin / submission)

### The "touching the ropes" test: `sub_821BD388(fighter)` (confirmed)

- If rule id == 0x60 (`sub_828B4DA8`) and +1800 == 0, it returns 0.
- If +444 != 0 (not in the ring), it returns 0.
- **If `rules[3]` (byte 0x82E3DE03) == 0, it returns 0. This is the ROPE BREAK rule.**
- If +108 is set and `sub_823AD338` fails, it returns 0.
- Otherwise it loops over the bone list at 0x82003740 (bytes 4, 1, 4, 15, 19, 12, 8, 11, 7, ending in 0xFF; head, hands and feet; guess). Bone matrix = fighter[(idx+28)*4], translation x/z at +48 / +56.
  - Threshold = 28.8 - 0.5 - 2.5 for the first bone, and 28.3 - 1.0 for the others.
  - When a bone is past the threshold, it transforms a tip offset ((1.5,0,0) for the first bone, (1,0,0) for the others) by the bone matrix.
  - It returns 1 if the tip's |x| or |z| > 28.3.

### Callers

- **Pin, referee: `sub_822446E0`** (reached from `sub_822478F0`, which sits at vtable 0x82005790 of the referee class).
  - It looks up `fighters[ref+112]`. If that fighter's +516 == 2 (pinning; guess), it tests his +96 (the pinned wrestler).
  - If touching: `sub_82243BD0` (stop the count), `sub_82244680(ref, 1)` (rope-break gesture), `sub_82273B68(pinned, 0, ..)` (break the pin), `sub_821EFCA0`, `sub_827ACA10`.
- **Submission: `sub_82219830`**. This is the update method, vtable 0x82004EC8 slot 6. The class is built in `sub_82217F98` via `sub_822197D8`; +40 is the attacker, +44 the defender. The attacker motion checks are 20500-21499, 16001-19999 and 30000-30999.
  - At 0x82219B5C: if +140 == 0, it calls `sub_821BD388(defender)`, then checks rules[3] again, then the referee (*0x82E3BC84), then +446 on both, then +1800 (different teams).
  - Then `sub_82244680(ref, 2)`. If ref+152, it sets +140 = 1 and calls `sub_82273B68(defender)`, `sub_821EFCA0`, `sub_827ACA10(attacker)`.
- **`sub_82220BC8`** (many callers, including `sub_82219830`) at 0x82220DA0: a hold-resolution path. When +2276 != +1156, bit 1 of +2308 is set, and `sub_821BD388(self)` returns 1, it releases (`sub_82192E70`, `sub_82244680(ref, 2)`, `sub_821EFCA0`, `sub_827ACA10`).
- **`sub_82214790`** at 0x82214C68 (per-fighter update from `sub_82216C58`): writes `+1969 = sub_821BD388(f)` every frame. It is read only by `sub_826218C0`.
- Commentary / AI also call it: `sub_8260D7E0`, `sub_8260E150`, `sub_82621700`.

### The rule (confirmed as a user rule; the "ROPE BREAK" label is very likely but not proven)

- `rules[3]` is filled in `sub_828C48E8` at 0x828C4958-0x828C4970: `rules[3] = options[16]` unless the rule record locks that byte.
- Other readers of `rules[3]`:
  - the pause rules screen `sub_82433718` (0x824340E4);
  - AI code: `sub_82825618`, `sub_82828C70`, `sub_828020A0` (also needs ref+152), `sub_82854DF8`, `sub_82312C58` and `sub_82246BF0`, the last two together with `rules[27]`.

### Hook proposal

- **Code hook:** override `sub_821BD388` to return 0 when there are no ropes. That covers the pin, submission, hold, the +1969 flag and commentary.
- **Data alternative (also stops AI rope crawling):** write `0x82E3DE03 = 0` once the match has started, after `sub_828C48E8` / `sub_828BBEF0` ran. Restore it after the match. With no-ropes plus rope break on, the pause screen would then show the rule as off.

Confidence: high.

## 3. Rope-dependent moves (lower priority; partial)

Found:

- **"Opponent groggy on the ropes" moves** are keyed off the opponent's motion id, not geometry.
  - Family 1510-1537, set in `sub_8220FC78`, `sub_8223DDE0`, `sub_8220EBD0`, `sub_82215AD8`, `sub_822E35E8` and `sub_82236D98`.
  - Example predicate: `sub_821B3928` = opponent in motion 1536. `sub_822E35E8` (grapple start) uses it to start paired move 1521.
  - The main way into that family from gameplay is the whip into the ropes (64 in `sub_82309398`, and the 1566 / 81xx paths). Steps 1-3 of section 1 cut that off.
  - Other knockback-into-ropes entries were not traced.
- **Running dive through the ropes** (move slot 172, event 21): `sub_822D24F8` at 0x822D2B48-0x822D2C20.
  - Conditions: self in the ring, target outside (+444 == 1), facing within the yaw window of `sub_822D2478`, and the rotated position z < -14.
  - The rebound handler's own variants: slot 212 (dive) and 167 / 211 (rebound attack) in `sub_822ED230`.
  - These are fine without ropes, so no change is needed.
- **Other geometric "near / facing ropes" helpers worth hooking if needed:**
  - `sub_821BA448(f)`: used with rule byte 14. When running (motion 51) it decides 703 vs 716 in `sub_8228B5A8` and `sub_82306F90` (0x82308310).
  - `sub_821F6948`: (33.87 - |local z|) <= 20; called from control entry `sub_822EA5D8`.
  - `sub_821BB530` (inside the 28.8 line; 14 callers).
  - `sub_821BC868` (motion slot 173-176 or inside the ropes; called from `sub_822309C8` and `sub_8231F3F8`).
- **Not found:** the springboard / 619 / tree-of-woe availability check. Lead: move availability is data-driven per move-set category (`../MVMT/ROPE`, `APRO`, `CETB`, etc., loaded by `sub_82387F78`). The "near ropes" condition for "MOVE TOWARDS ROPE + button" is probably in one of the 116 control handlers; candidates are those calling `sub_8219E108` or `sub_8218E400`.

Confidence: low-medium.

## Pitfall

When `ppc_xref`-style dumps are filtered with `awk '$1==f'`, names like `8218E400` are read as numbers (8218e400 = inf) and match other lines. Compare as strings instead.
