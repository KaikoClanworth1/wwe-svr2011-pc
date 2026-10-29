"""EXPERIMENTAL (23 Oct 2026, not used by the converters yet): a walkway into an SvR 2008 arena's crowd area (its crowd collision, stage HMD 999 - docs/OTHER_GAMES.md).

SvR 2008's crowd collision is the match collision (996) plus a closed pocket a few units beyond the arena's wall:
the stands' walls, a raised stand, but no ground floor (2008 got you in there its own way; 2011 has no way, and with
no floor a fighter put there is moved back to the nearest floor). This opens it for 2011's own movement:
- the walls where the two face each other (both ends within GAP of the other side: short pieces - the arena's and
  the pocket's) are taken out, full height;
- the pocket and a margin around that opening get a ground floor (y 0, CELL x CELL cells, two triangles each);
- the pocket's walls get a copy facing in (the shipped ones only stop someone coming in);
- the z-band index (997) is rebuilt for the new list.
Coordinates here are the HMD's (its x is the game's z, its z the game's x).

    py crowd_walkway.py <arena.pac> [<out.pac>]     (in place without <out>)

State (SvR 2008 RAW port, hidden tests, 29 Oct 2026): the walls facing in are fixed (their origin was at a corner, past
the +88 reach, so the game skipped them); with them a player put in the pocket stays in it and walks out through the
opening. People's walls are one-sided: the shipped pocket walls only stop someone coming in. The AI's wall grid covers
the pocket through the manifest's crowd_area= (crowd_area() - the game stretches its ringside grid). Still open: a
player walking IN through the opening is stopped at it (not by this collision), and the space behind the barricade
has no end walls (a CPU walked off along it).
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402
import svr08_stage as s  # noqa: E402
import svrfmt as f  # noqa: E402

MATCH_HMD, CROWD_HMD, CROWD_INDEX = 996, 999, 997
GAP = 15.0     # (walls this close to the other side's: the opening)
CELL = 25.0   # (floor cells: few triangles - the collision list stays near its own size)
MARGIN = 15.0  # (floor this far around the walls taken out: the gap between the arena floor and the pocket)


def corners(r):
    return np.array(s.tri_corners(r))


def is_floor(c):
    n = np.cross(c[1] - c[0], c[2] - c[0])
    return abs(n[1]) / (np.linalg.norm(n) or 1) > 0.7


def floor_record(pts, like):
    """A level triangle at y 0 through three (x, z) points, in the frame layout of floor record `like` (its
    material words kept): identity rotation, the corners about their middle."""
    r = like.copy()
    P = np.array([[x, 0.0, z] for x, z in pts])
    mid = P.mean(0)
    M = np.eye(4)
    M[3, :3] = -mid
    r[:16] = M.flatten()
    loc = P - mid
    r[16:22] = loc[:, [0, 2]].ravel()
    r[3], r[7] = P[:, 0].min(), P[:, 0].max()
    extra = np.frombuffer(np.array(like[22:24], dtype='<f4').tobytes(), dtype='<u4').copy()
    half = float(np.abs(loc[:, [0, 2]]).max())
    extra[0] = np.frombuffer(np.array([half], dtype='<f4').tobytes(), dtype='<u4')[0]
    r[22:24] = np.frombuffer(extra.tobytes(), dtype='<f4')
    # (the frame must give the corners back)
    assert np.allclose(corners(r), P, atol=1e-3), 'floor frame'
    return r


def reversed_wall(r):
    """The wall triangle facing the other way (normal and winding flipped, the same corners): the pocket's walls
    only stop someone coming in - a fighter inside walked out through them."""
    C = corners(r)
    R = r[:16].reshape(4, 4)[:3, :3].copy()
    R[:, 0] *= -1  # (normal and one side axis flipped: the same handedness)
    R[:, 1] *= -1
    t = -C[0] @ R
    L = C @ R + t
    # (the origin in the middle of both plane axes, like the shipped data: +88 is the corners' reach from it - a
    # copy with its origin at a corner reached past +88 and the game skipped it)
    t[0] -= (L[:, 0].min() + L[:, 0].max()) / 2
    t[2] -= (L[:, 2].min() + L[:, 2].max()) / 2
    L = C @ R + t
    M = np.eye(4)
    M[:3, :3] = R
    M[3, :3] = t
    out = r.copy()
    out[:16] = M.flatten()
    out[16:22] = L[[0, 2, 1]][:, [0, 2]].ravel()  # (the other winding)
    out[3], out[7] = C[:, 0].min(), C[:, 0].max()
    extra = np.frombuffer(np.array(r[22:24], dtype='<f4').tobytes(), dtype='<u4').copy()
    extra[0] = np.frombuffer(np.array([float(np.abs(L[:, [0, 2]]).max())], dtype='<f4').tobytes(), dtype='<u4')[0]
    out[22:24] = np.frombuffer(extra.tobytes(), dtype='<f4')
    assert np.allclose(np.sort(corners(out), 0), np.sort(C, 0), atol=1e-3), 'reversed wall'
    return out


def open_walkway(match, crowd):
    """match / crowd: HMD record lists (996, 999) -> (the new 999 list, a report)"""
    walls = [(k, corners(r)) for k, r in enumerate(crowd) if not is_floor(corners(r))]

    def seg(c):  # (a wall's stretch on the floor: its two farthest corners in x / z)
        q = c[:, [0, 2]]
        i, j = max(((i, j) for i in range(3) for j in range(3)), key=lambda ij: np.sum((q[ij[0]] - q[ij[1]]) ** 2))
        return q[i], q[j]

    def seg_dist(a, b):
        def pt(p, s0, s1):
            d = s1 - s0
            t = 0.0 if not d.any() else float(np.clip(np.dot(p - s0, d) / np.dot(d, d), 0, 1))
            return float(np.linalg.norm(p - (s0 + t * d)))
        return min(pt(a[0], *b), pt(a[1], *b), pt(b[0], *a), pt(b[1], *a))

    # The pocket: the walls reaching beyond the match collision's walls (996), and every wall joined to them end to
    # end (its whole outline, low and high parts) - the arena's own wall is a few units off, not joined.
    mw = np.concatenate([corners(r) for r in match if not is_floor(corners(r))])
    bx0, bx1, bz0, bz1 = mw[:, 0].min() - 1, mw[:, 0].max() + 1, mw[:, 2].min() - 1, mw[:, 2].max() + 1
    segs = {k: seg(c) for k, c in walls}
    pocket_set = {k for k, c in walls if (c[:, 0] < bx0).any() or (c[:, 0] > bx1).any() or (c[:, 2] < bz0).any() or
                  (c[:, 2] > bz1).any()}
    if not pocket_set:
        return crowd, 'no crowd pocket (nothing beyond the match walls)'
    grown = True
    while grown:
        grown = False
        ends = [e for k in pocket_set for e in segs[k]]
        for k, c in walls:
            if k not in pocket_set and any(np.linalg.norm(e - q) < 0.6 for e in segs[k] for q in ends):
                pocket_set.add(k)
                grown = True
    P = np.concatenate([c for k, c in walls if k in pocket_set])
    px0, px1, pz0, pz1 = P[:, 0].min(), P[:, 0].max(), P[:, 2].min(), P[:, 2].max()
    psegs = [(k, segs[k]) for k in pocket_set]
    asegs = [(k, segs[k]) for k, c in walls if k not in pocket_set]
    def near_both(sg, others):  # (both ends of a wall within GAP of the other side: a short piece where they face)
        def pt(p, s0, s1):
            d = s1 - s0
            t = 0.0 if not d.any() else float(np.clip(np.dot(p - s0, d) / np.dot(d, d), 0, 1))
            return float(np.linalg.norm(p - (s0 + t * d)))
        return all(min(pt(e, *o) for _, o in others) < GAP for e in sg)
    drop = {k for k, sa in asegs if near_both(sa, psegs)} | {k for k, sp in psegs if near_both(sp, asegs)}
    if not drop:
        return crowd, "the crowd pocket is nowhere near the arena's wall"
    D = np.concatenate([corners(crowd[k]) for k in drop])
    boxes = [(px0, px1, pz0, pz1),
             (D[:, 0].min() - MARGIN, D[:, 0].max() + MARGIN, D[:, 2].min() - MARGIN, D[:, 2].max() + MARGIN)]
    like = next(r for r in crowd if is_floor(corners(r)) and abs(corners(r)[:, 1].mean()) < 1)
    floors, cells = [], set()
    for x0, x1, z0, z1 in boxes:
        for i in range(int(math.floor(x0 / CELL)), int(math.ceil(x1 / CELL))):
            for j in range(int(math.floor(z0 / CELL)), int(math.ceil(z1 / CELL))):
                cells.add((i, j))
    for i, j in sorted(cells):
        a, b = i * CELL, j * CELL
        floors.append(floor_record([(a, b), (a + CELL, b), (a + CELL, b + CELL)], like))
        floors.append(floor_record([(a + CELL, b + CELL), (a, b + CELL), (a, b)], like))
    back = [reversed_wall(crowd[k]) for k in sorted(pocket_set) if k not in drop]
    new = [r for k, r in enumerate(crowd) if k not in drop] + floors + back
    rep = ('walkway: %d wall triangles out, %d floor triangles and %d walls facing in added, %d in all (pocket x %.0f..%.0f z %.0f..%.0f, game x %.0f..%.0f '
           'z %.0f..%.0f)' % (len(drop), len(floors), len(back), len(new), px0, px1, pz0, pz1, pz0, pz1, px0, px1))
    return new, rep


CROWD_AREAS = 50003  # (SvR 2008's stage text: "Fighting in the Crowd" rectangles, start x z / end x z per line)


def crowd_area(epac):
    """an arena pac's (bytes) crowd area for the manifest's crowd_area= (the game's ringside wall grid is stretched
    over it in crowd matches - crowd_fight.cpp): the union of its stage's 50003 rectangles, 'x0,z0,x1,z1' in stage
    x / z, or None"""
    h, g, t = f.epac_read(epac)
    for typ, ents in g:
        for name, blob in ents:
            if typ != b'STG ' or name[:1] != b'0' or s.unpack(blob)[:4] != b'PACH':
                continue
            text = dict(f.pach_read(s.unpack(blob))).get(CROWD_AREAS)
            if text is None:
                return None
            xs, zs = [], []
            for line in s.unpack(text).decode('latin-1').splitlines():
                v = line.split(';')[0].split()
                try:
                    x0, z0, x1, z1 = (float(a) for a in v[:4])
                except ValueError:
                    continue
                xs += [x0, x1]
                zs += [z0, z1]
            return '%g,%g,%g,%g' % (min(xs), min(zs), max(xs), max(zs)) if xs else None
    return None


def patch_pac(path, out=None):
    """the arena pac with its crowd collision opened (999 and its index 997) -> report"""
    h, g, t, (gi, ei), ents = s.stage(path)
    E = dict(ents)
    raw = {eid: s.unpack(b) for eid, b in ents}
    if CROWD_HMD not in raw or MATCH_HMD not in raw or raw[CROWD_HMD][:4] != b'HMD ':
        return 'no crowd collision (999) in this arena'
    new, rep = open_walkway(s.hmd_read(raw[MATCH_HMD]), s.hmd_read(raw[CROWD_HMD]))
    if len(new) == len(s.hmd_read(raw[CROWD_HMD])) and 'walkway' not in rep:
        return rep
    hmd = s.hmd_write(new)
    idx = s.index_build(new)
    packed = lambda old, data: f.bpe_encode(data, compress=True) if old[:4] == b'BPE ' else data
    E[CROWD_HMD] = packed(E[CROWD_HMD], hmd)
    E[CROWD_INDEX] = packed(E[CROWD_INDEX], idx)
    stg = f.pach_write([(eid, E[eid]) for eid, _ in ents])
    typ, gents = g[gi]
    name, blob = gents[ei]
    gents = list(gents)
    gents[ei] = (name, f.bpe_encode(stg, compress=True) if blob[:4] == b'BPE ' else stg)
    g = list(g)
    g[gi] = (typ, gents)
    data = f.epac_write(h, g, t)
    target = out or path
    tmp = target + '.tmp'
    with open(tmp, 'wb') as fh:
        fh.write(data)
    os.replace(tmp, target)
    return rep


if __name__ == '__main__':
    print(patch_pac(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None))
