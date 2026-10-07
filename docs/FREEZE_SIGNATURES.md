# Freeze signatures (world update stuck)

When the world update stops for 3 s, `frame_rate.cpp`'s watchdog logs this line:

```
frame rate: world update stuck N s in pass P (1x: update x+1, 20: draw) - object O function F; jobs [...]
```

After that line it logs:
- every guest thread's call chain: `frame rate: thread ID '' at lr chain...`;
- the last ~160 hand-offs of the characters' job: `frame rate: job events ...`.

Kinds in the job-events line:
- `F`: a frame, with its tick count.
- `R` / `r`: the job's round run / left for later. `P` / `p`: the paused round run / left. `E` / `e`: the paused round's second half.
- `J`: the job ran, with its tick count.
- `W` / `w`: a job wait in / out. A trailing `x` after the frame number means an extra update.
- `L`: a lost request, asked for again.
- `O`: a round owed, run before the draw.

The signatures below tell the known freezes apart.

## 1. Lost job request ("stuck in pass 20") - fixed in 4e1ec2e

- **Pass:** 20 (the draw) or a later update, waiting in `sub_8216A450` for a job slot that nobody runs.
- **Jobs line:** slot functions `826DFD40`.
- **Threads:** the logic thread waits in `8216A450` / `8215A8C0`. The job worker (`8216E098`) is idle, waiting for work.
- **Cause:** a race in the job system. The worker signals a slot's event and only then sets the request flag back to 1. A request for the same slot made in between is wiped out. Two updates a frame (30 fps, slow PCs, phones) make it more likely.
- **Since 4e1ec2e:** the wait is sliced; a lost request is noticed and asked for again. The log then shows `frame rate: job N was asked for but lost - asked again (K times)` instead of a freeze.
- **Seen on:** Fold 7 (big matches), PC Royal Rumble soak, the svr2011_027 entrance freeze.

## 2. Characters' job stuck in physics ("stuck in pass 10") - not fixed, rare

- **Pass:** 10 (the first update of a frame), function `82167228`. No "asked for but lost" line before it.
- **Logic thread:** `82902B70 8216F548 8216559C 8269D7B4 ...`. It is in the characters' job round (`sub_8216F4C8`), waiting for the job it started last frame to finish.
- **Job thread:** `82A4DAD4 82A456C0 821A86B0 821A89AC 82171A18 821724B8`. It is running: the CHPH job (`sub_82171940` / `82171A18`) is inside the physics world step (`821A86B0` → Havok `82A456C0` / `82A4DAD4`) and doesn't come back.
- **Job events:** normal up to the end. Each frame has `F`, `R`, `J`, then waits that come back (`W` ... `w`), with nothing lost and nothing owed. The last frame's round `R` is followed by no `J`.
- **Seen:** once, 2026-10-07 on PC, 60 fps.
  - Match: triple threat (rule 0x0D) as THREE STAGES OF HELL, started by the test route (`SVR2011_TEST_RULE=0D`), right at the start of the match before any fall.
  - The Three Stages code had not changed anything yet (it only acts on a fall).
  - A rerun of the same match was clean (10 min).
  - The run was stopped 3 s into the freeze, so whether it ever recovers is unknown.
- **What it is not:** the job system's request race (that is #1: the worker would be idle, not inside physics).
  - Most likely a Havok step that runs away. Possible causes are a solver loop on a bad contact (several people overlapping at the start) or a NaN position.
- **If players report it:** ask for the log. The tell is pass 10 plus the job thread inside `82A4DAD4` / `82A456C0`.
  - To chase it: log the characters' positions (`SVR2011_FPS_PROBE=2`) up to the freeze, and check for NaN / huge values before the step.
