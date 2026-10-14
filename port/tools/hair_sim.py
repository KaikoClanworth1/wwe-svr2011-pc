"""2011's superstar hair solver (gBaH, docs/HAIR_PHYSICS.md section 7) in Python - the reference for
reimplementations (Mod Maker viewers, converter checks) - and its check against the game's own numbers.

    py tools/hair_sim.py <dump> [floorY]      e.g. py tools/hair_sim.py tools/data/hair_ref_edge.txt -12

<dump>: src/hair_dump.cpp output (SVR2011_TEST_HAIR_DUMP=<file>): the game's hair state around each step.
tools/data/hair_ref_edge.txt is 60 frames of Edge (vs Chris Jericho, arena 1). Every step is replayed from
the game's state before it and compared with the state after it (max error ~0.00003; step B ~0.03 because
the head-turn kick's axis - the hair root's -Z - isn't in the dump and the head's -Z stands in).
floorY: -12 in a normal match (HAIR_PHYSICS.md section 9).
"""
import math
import sys
from collections import defaultdict

import numpy as np


def parse(path):
    P, steps, S, D = {}, defaultdict(dict), {}, defaultdict(dict)
    for line in open(path):
        t = line.split()
        if not t:
            continue
        if t[0] == 'P':
            i, cnt = int(t[1]), int(t[2])
            v = [float(x) for x in t[3:19]]
            lencorr, visc, grav, spring, damp, radius, bend, movecorr = [float(x) for x in t[19:27]]
            P[i] = dict(count=cnt, L=v, visc=visc, grav=grav, spring=spring, damp=damp, radius=radius, bend=bend,
                        movecorr=movecorr, A=int(t[27]))
        elif t[0] == 'S':
            f, n = int(t[1]), int(t[2])
            S[f] = np.array([float(x) for x in t[3:3 + 4 * n]]).reshape(n, 4)
        elif t[0] == 'D':
            D[int(t[1])][int(t[2])] = np.array([float(x) for x in t[3:6]])
        else:
            f, step, i, cnt = int(t[0]), t[1], int(t[2]), int(t[3])
            if len(t) != 4 + 10 * cnt:
                continue   # (the last line, cut when the game stopped)
            a = np.array([float(x) for x in t[4:4 + 10 * cnt]]).reshape(cnt, 10)
            steps[f][(step, i)] = dict(P=a[:, 0:3].copy(), V=a[:, 3:6].copy(), prev=a[:, 6:9].copy(),
                                       pin=a[:, 9].astype(int))
    return P, steps, S, D


def forces(Pp, V, c):
    n = len(Pp)
    F = -c['visc'] * V
    F[:, 1] += c['grav']
    for j in range(n - 1):
        d = Pp[j] - Pp[j + 1]
        ln = np.linalg.norm(d)
        if ln < 1e-4:
            d = d.copy()
            d[1] = 1e-4
            ln = 1e-4
        dv = V[j] - V[j + 1]
        s = (c['spring'] * (ln - c['L'][j]) + c['damp'] * float(dv @ d) / ln) / ln
        F[j] -= d * s
        F[j + 1] += d * s
    return F


def rk4(st, c):
    Pp, V, pin = st['P'], st['V'], st['pin']
    free = pin == 0
    F = forces(Pp, V, c)
    AV, AF = V.copy(), F.copy()
    TV = V + 0.5 * F
    TP = Pp.copy()
    TP[free] += 0.5 * V[free]
    for k, (wa, wt) in enumerate(((2, 0.5), (2, 1.0), (1, None))):
        F2 = forces(TP, TV, c)
        V2 = TV.copy()
        AF += wa * F2
        AV += wa * V2
        if wt is not None:
            TV = TV + wt * F2
            TP[free] += wt * V2[free]
    st['V'] = V + AF / 6
    Pn = Pp.copy()
    Pn[free] += AV[free] / 6
    st['P'] = Pn


def step_a(pre, post, c):
    st = {k: v.copy() for k, v in pre.items()}
    n0 = post['P'][0]
    d = n0 - st['prev'][0]
    if np.linalg.norm(d) <= 1.0:
        d = d * max(c['movecorr'], 0.5)   # (stock movecorr 0-0.45: 0.5)
    st['P'][0] = n0
    st['P'][1:] += d
    st['prev'][1:] += d
    st['P'][1] = post['P'][1]
    st['pin'] = post['pin'].copy()
    rk4(st, c)
    return st


def step_b(pre, c, kick):
    st = {k: v.copy() for k, v in pre.items()}
    n = len(st['P'])
    L0 = c['L'][0]
    for j in range(2, n):
        e = 2 * st['P'][j - 1] - st['P'][j - 2] - st['P'][j]
        if np.linalg.norm(e) / L0 > 0.1:
            k = c['bend'] * (0.5 if j >= 4 else 0.75 if j == 3 else 1.0)
            st['V'][j] += e * k
    if kick is not None:
        st['V'][2:] += kick
    rk4(st, c)
    return st


def step_c(pre, c, spheres, floorY, skip_coll=False, ring=True):
    st = {k: v.copy() for k, v in pre.items()}
    Pp, V = st['P'], st['V']
    n = len(Pp)
    L = c['L']
    for _ in range(4):
        pair = {}
        for s in range(1, n - 1):
            a, b = Pp[s], Pp[s + 1]
            if np.linalg.norm(a - b) > L[s]:
                m = (a + b) / 2
                nn = (b - m) / np.linalg.norm(b - m)
                pair[s] = (m - nn * L[s] / 2, m + nn * L[s] / 2)
            else:
                pair[s] = (a.copy(), b.copy())
        for j in range(2, n - 1):
            Pp[j] = (pair[j - 1][1] + pair[j][0]) / 2
        Pp[n - 1] = pair[n - 2][1]
    for s in range(1, n - 1):
        dd = Pp[s + 1] - Pp[s]
        if np.linalg.norm(dd) > L[s]:
            Pp[s + 1] = Pp[s] + dd / np.linalg.norm(dd) * L[s]
    if spheres is not None and not skip_coll:
        for j in range(2, n):
            for cx, cy, cz, r in spheres:
                ce = np.array([cx, cy, cz])
                R = r + c['radius']
                dist = np.linalg.norm(Pp[j] - ce)
                if 0 < dist < R:
                    Pp[j] = ce + (Pp[j] - ce) * R / dist
                    V[j] = 0
    lim = floorY - c['radius']
    for j in range(2, n):
        # (an arena with a ring: only inside its 64 x 64 square)
        if Pp[j][1] > lim and (not ring or (abs(Pp[j][0]) < 32 and abs(Pp[j][2]) < 32)):
            Pp[j][1] = lim
    st['prev'] = Pp.copy()
    return st


def err(a, b, rows=slice(None)):
    return float(np.max(np.abs(a[rows] - b[rows]))) if len(a) else 0.0


def main():
    path = sys.argv[1]
    floorY = float(sys.argv[2]) if len(sys.argv) > 2 else -8.4
    P, steps, S, D = parse(path)
    frames = sorted(steps)
    worst = defaultdict(float)
    sums = defaultdict(float)
    nobs = defaultdict(int)
    for f in frames:
        for i, c in P.items():
            st = steps[f]
            if ('A0', i) not in st or ('C1', i) not in st:
                continue
            a = step_a(st[('A0', i)], st[('A1', i)], c)
            kick = None
            if f + 1 in D and i in D[f + 1] and f in D and i in D[f]:
                v, pv = D[f + 1][i], D[f][i]
                turn = min(np.linalg.norm(v - pv) * 0.1, 0.02)
                if turn > 0.001:
                    kick = v * turn   # (approximation: the root's -Z taken as the head's)
            b = step_b(st[('A1', i)], c, kick)
            # (collision skipped for every chain when the character is below the floor: chain 0's root
            # y > 5 or sphere 0's centre y > 0)
            skip = (('B1', 0) in st and st[('B1', 0)]['P'][0][1] > 5.0) or (f in S and len(S[f]) and S[f][0][1] > 0)
            cc = step_c(st[('B1', i)], c, S.get(f), floorY, skip)
            for name, mine, game in (('A pos', a['P'], st[('A1', i)]['P']), ('A vel', a['V'], st[('A1', i)]['V']),
                                     ('B pos', b['P'], st[('B1', i)]['P']), ('B vel', b['V'], st[('B1', i)]['V']),
                                     ('C pos', cc['P'], st[('C1', i)]['P'])):
                e = err(mine, game)
                worst[name] = max(worst[name], e)
                sums[name] += e
                nobs[name] += 1
    print('frames', len(frames), 'chains', len(P))
    for k in worst:
        print('%-6s mean err %.5f  worst %.5f  (%d chain-frames)' % (k, sums[k] / nobs[k], worst[k], nobs[k]))


if __name__ == '__main__':
    main()
