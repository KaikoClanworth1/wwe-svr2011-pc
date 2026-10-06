# Roaming Falls Count Anywhere: plan (old-style, SvR 2006/07)

**Goal:** a Falls Count Anywhere match that moves between separate areas with a transition, as in SvR 2006/07, for example parking lot -> arena ring. It is not the free-roam backstage (all of bg78 at once) and not the modded 2008 parking lot.

**Prototype:** two areas, the PARKING LOT (2011's own backstage room) -> the ring.

## What we already know

- **Arenas are loaded one at a time.** A match loads one stage. bg78 holds every backstage room, and the ring arenas are separate (arena 0-29...).
- **Backstage rooms:** each room is a rule (1v1 0x1B-0x21, the parking lot 0x1B). The 'CFRS' task `sub_825310B8` shows only the room at the camera. `sub_8224EF28` sets each area's fight box (centre and half sizes, also copied to 0x82D9E9D4 for the clamp `sub_821E5408` and the AI).
- **Choosing an arena for a match:** `test_match.cpp` sets the match screen's arena (`*0x82EDEB48 +244`) early, from the getter `sub_825740C8`. Forcing the live record +64 instead lost the cage on PC and froze the Fold at load.
- **Start places:** `sub_822AD738(placer, rule, ?)` places people from a per-rule table (88 bytes per rule; kind, x, y, z, facing in tenths), as used for the lumberjacks.
- **Falls:** the judge `sub_82245618` and the live pinfall byte (+1 = 2: anywhere) work mid-match (Three Stages switches to FCA during a match).
- **Match end flow:** `sub_82322DA0` steps 0-5 (result, fade, highlights, results; see `replays.cpp`). The results menu has REMATCH, which reloads the same match with the same people.

## How to change area

### A. Segment chain (recommended)

Each area is its own match "segment". The match moves on with the game's own loading:
1. In the current area an exit fires (below). The segment ends with no result: no winner, no highlights, no results screen. The port steps the match-end flow straight into a restart, the way REMATCH does.
2. Before the next load, the port saves what carries over: each wrestler's health and damage per body part, momentum, finishers/signatures stored, the time so far and the score (falls so far, if any).
3. The next segment loads with the next area's arena (set as `test_match.cpp` does) and its rule: backstage room rule 0x1B for the parking lot, ring FCA 0x2B with pinfall anywhere. Start places are the area's "entrance" (e.g. the ramp / stage end for the ring), through the placement table.
4. After the load the saved state is put back, and the fight goes on.

This uses only the normal loading path, so it is the safest. The transition is a loading screen, as it was in the old games. Camera, AI mode, fight box and crowd are each area's own.

### B. Swap the stage during a match (not recommended)

This would load another arena's geometry, collision, camera set-up and crowd with fighters alive. Nothing in the engine does that (Road to WrestleMania moves between matches, not inside one). The risk of render, memory and AI breakage is high, especially on Android.

### C. Room to room inside bg78 (an add-on, later)

For backstage-to-backstage moves only. All rooms are already loaded, so a fade to black, moving both fighters to the next room's start place, and the fight box set for that room give separate areas with no load. The ring still needs A.

## Exits (what moves the match on)

- **Each area has exit zones** in world coordinates (a door, a ramp, a garage gate). Each zone leads to the next area.
- **Trigger:** both wrestlers within the zone (or the player there with the opponent near) for 1-2 s, not in a grapple. Optionally the player presses a button there, as in the old games (a prompt shown over the match).
- **CPU vs CPU and when the player never leads:** after N minutes in an area the match moves on by itself (a fallback timer).
- **The CPU follows** its opponent, so it goes along once the player moves.

## Weapons and objects

- Weapons per area come from the placement table *(0x82E3BE98) by rule (Weapons Everywhere uses it). The ring segment can use FCA's weapons and the parking lot its own.
- Parking-lot objects (cars, crates) are the room's own GMGB records.

## What has to be found (research in the prototype)

1. **Ending a segment with no result and restarting at once.** Where REMATCH starts the reload, and whether the results/highlights steps can be skipped. Universe on in the save also brings up its question box; segments must not count as Universe matches.
2. **Where health and damage live** in the character struct (diff a fighter before and after hits), and whether writing them back after a load sticks (HUD meters, damage display).
3. **Start places per area** for a roaming match (a rule id of our own, or a placement table record changed while the segment loads).
4. **The exit zones' coordinates** in the parking lot (doors) and in each arena (the ramp end).

## Risks

- The results flow is fiddly (highlights, the Universe question, saves, achievements). A segment end must not record a match.
- A state carry that misses something (e.g. a body part's damage, or blood) shows as a reset.
- Loading time: about 10-20 s per transition on PC, more on Android.
- Not for online / P2P (both sides would have to load in step); the row is offline only.

## Estimate

- **Research + 2-area prototype (parking lot -> ring):** 3-4 days.
  - Segment end/restart: 1-1.5 days.
  - State carry: 1 day.
  - Exit zone and start places: 0.5-1 day.
  - Tests: 0.5 day.
- **Full feature after that:** more areas (other backstage rooms, the stage / ramp, crowd), per-area weapons, the exit prompt, the CPU fallback timer, menu rows for 1v1 and 2v2, and polish. About 4-6 more days.

## Order

1. Segment restart into another arena (no state), from a test aid: "change area now".
2. State carry.
3. Exit zone in the parking lot -> ring.
4. Menu row ROAMING FALLS COUNT ANYWHERE (ONE ON ONE, beside FALLS COUNT ANYWHERE).
5. Report, then the full feature if the user wants it.
