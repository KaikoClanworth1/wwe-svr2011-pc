# Entrance pyro: prtcls.pac and the PRTC particle format

Work in progress, for drawing pyro in the Mod Maker's Full preview (checklist: "Full preview pyro").
For now the preview only lists an entrance's set ("Pyro plays in game, not here: 21 emitters: fireworks,
smoke, flashes"). [C] = checked in data, [L] = likely.

## The file

`pac/prtcls.pac` is an EPAC with these groups [C]:

| group | entries | what |
|---|---|---|
| PCE6 / LPE6 | 290 each, `0050` ... `8500` | a stock entrance's set, keyed by entrance number (`%04d`). LPE6 is the second copy (low-poly / other screen mode [L]) |
| PCCH / LPCH | 96, `5001` ... `5105` | created-entrance parts (kind 5, `/PCCH/%04d`) |
| PCGE / LPGE | 6, `GAME RYAL ECW_ INFE FREE JERI` | arena and show sets |
| PCGI | 0 | |

How the game picks a set (code) is in docs/OTHER_GAMES_ENTRANCES.md, "Pyro" (`sub_8275F548`, `sub_8275F490(kind, n)`,
name table at 0x82D60EC0).

## A set (PCE6/`nnnn`)

A PACH, ids [C]:

- **0 - 99**: the entrance's own emitters. 115 of the 290 sets have some (0-39 used, e.g. 104 Jericho 21, 176 28).
- **110 - 133, 210 - 223**: the same 38 light emitters in every set (byte-identical between 0100 and 0139; Light0001,
  Cross01/02, LightW). These are the stage lights [L].
- **16000**: a 1334-byte timing table: u32 count (0x25), then 36-byte records `{u32 2, f32 a, f32 b, 0 ...}`.
  a runs 111, 112, 113... (as floats 0x42de0000 = 111.0, 0x42e00000 = 112.0) and b is 110.0, so this maps the
  light emitters to the first (110) [L].

An emitter is a PACH with up to 2 parts [C]:

- **part 0: `PRTC`**: the particle script. Header `PRTC 00060000 f0170000 04000100 ...` (version 0x600 [L]), then
  named chunks: `rootchk`, effect names (`Light`), and many `cmd_chk` command chunks (343,000 across all sets).
  The emitter's textures are named after `TexGroup` / `texture` (e.g. `Light0001`, `Smoke0001`, `FireW003`).
- **part 1**: the texture group, present only in the first emitter of a group (110 carries 22,048 bytes, while 111-133
  are 2,500 each and share it [L]). It holds `.dds` DXT5 images named like the TexGroup entries.

Texture name -> what it looks like (from the names; used by the preview's note): Hanabi / FireW = fireworks,
Fire0x = flames, Smoke = smoke, Tsubu (grains) = sparks, Tama (balls) = fireballs, Bo0x = jets, Light / Cross /
LG = flashes.

## Firing

In the entrance (EVP), the effect object is the cast of type `00000000 00010000` (Cena 139: object 11). Its
tracks sit in slots 11-18, 60-68 and 160-218 with sub-tracks y0/1/10/50+ [C]. Which track starts which emitter is
open.

## Next

1. Find the PRTC reader in the recompiled code (the loader is `sub_826A31B8`; follow it to the parser of `rootchk` /
   `cmd_chk`).
2. Decode the cmd_chk commands: emit rate, lifetime, velocity, gravity, size/colour/alpha over life, blend.
3. Map the effect object's track events to emitter ids and positions.
4. A small CPU particle system in the Mod Maker (billboards, additive / alpha blend) drawn in the preview's scene.
