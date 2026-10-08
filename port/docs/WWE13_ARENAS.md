# WWE '13 arenas (svrmod wwe13)

`svrmod wwe13 <WWE '13 bgNN.pac> <SvR2011 host bgNN.pac> <out.pac> [tex=...] [card=...] [drop-mesh=<id>:<mesh>]`
(modmaker/svrfmt/wwe13.cpp) converts a WWE '13 arena onto a SvR2011 host arena file.
Bundled so far: RAW IS WAR (bg28 on bg01), SmackDown 1999 (bg29 on bg00),
Royal Rumble 1998 (bg35 on bg09), King of the Ring 1998 (bg38 on bg06).

## Barricade corners

WWE '13 models each barricade corner (`ar_fence01_c/f/g/h`) once, as a
stretchable piece between two posts; its code moves the posts onto the gap
between the side run's end and the front / back run's end. SvR2011 does not, so
`BakeBarrierCorners` bakes the stretch and puts the corner into the side run's
model. The corner's far end (B) must lie beyond the side run's end along z:
SmackDown 1999's side run is two panels deep (x 70-73), and its inner posts were
taken as B, which squashed every corner to 3 units (x0.18) and left a gap at
each corner (player report, fixed in SD99 v1.1). The converter prints one
"barrier corner" line per corner; a stretch far from x1.3 means a wrong B.

## Known open items

- SmackDown 1999: two barricade textures (`ar_fence`, `ar_fence_b`) are halved
  to keep the arena within the host file size (the converter's "textures
  halved to fit" line), so the barricade is a little blurrier than in WWE '13.
- SmackDown 1999: the WWE '13 light maps are dropped (111 meshes), and players
  report the arena dark at times.
- SmackDown 1999: no barricade along the ramp, and the front-row floor chairs
  are empty (user report, 2026-10-06).
- A model over about 430 KB unpacked crashes the arena load: drop the heavy mesh
  with `drop-mesh=` (RR98 / KOTR98 stage cables, `drop-mesh=d1:8`).
- An arena must stay within its host file's size, and must sit on its own slot
  (its stage entry is `STG/<slot>`).
