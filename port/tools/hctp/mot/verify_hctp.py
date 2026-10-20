"""Plausibility of the HCTP decode: FK on Hulk Hogan's skeleton (CH.PAC entry 0) for every
character track of a bank; per frame the lowest joint (max y, y down) should sit on the floor
(y ~ 0) for grounded motions, and joints must move smoothly.  Compares three composition rules."""
import sys, collections, math
import numpy as np
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (its own folder: the embeddable Python doesn't add it)
import hmot, fk

FEET = ['l_ashi', 'r_ashi', 'l_tsumasaki', 'r_tsumasaki', 'l_te', 'r_te', 'atama', 'koshi', 'l_sune', 'r_sune']
ALL = [n for n in fk.MOT_BONES] + ['l_yubi20', 'r_yubi20']


def run(bank, limit=60, modes=('bind_motion', 'motion', 'motion_bind'), ids=None):
    nodes, name = fk.hctp_nodes()
    S = fk.Skel(nodes)
    T, ents = hmot.read_bank(open(bank, 'rb').read())
    res = {m: [] for m in modes}
    jumps = {m: [] for m in modes}
    n = 0
    for e in ents:
        if e['y'] not in (0, 1) or not e['data'] or (ids and e['id'] not in ids):
            continue
        if hmot.track_kind(e['data'], e['y']) != 'char':
            continue
        fr = hmot.sample(e['data'], e['frames'] or None)
        if len(fr) < 2:
            continue
        n += 1
        for m in modes:
            low, prev, jmp = [], None, 0
            for r, A in fr:
                W = S.pose([v * 0.01 for v in r], A, m)
                J = S.joints(W, ALL)
                low.append(J[:, 1].max())
                if prev is not None:
                    jmp = max(jmp, np.abs(J - prev).max())
                prev = J
            res[m].append(np.median(low))
            jumps[m].append(jmp)
        if n >= limit:
            break
    print(bank, 'tracks', n, 'skeleton', name)
    for m in modes:
        a = np.array(res[m])
        print('  %-12s lowest joint y (median per track): median %.2f, |.|<0.6 for %d/%d, max frame-to-frame joint step median %.2f'
              % (m, np.median(a), (np.abs(a) < 0.6).sum(), len(a), np.median(jumps[m])))


if __name__ == '__main__':
    run(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 60)
