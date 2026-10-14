"""HCTP (Hogan, CH.PAC entry 0) vs SvR2011 (ch104) skeletons: names, parents, rest t/r, world joint
positions, the 21 motion bones' chain lengths, and the name mapping.  python skel_compare.py [ch.pac]"""
import sys, math
import numpy as np
import fk

nH, nameH = fk.hctp_nodes()
n11 = fk.svr11_nodes(sys.argv[1]) if len(sys.argv) > 1 else fk.svr11_nodes()
iH = {n['name']: i for i, n in enumerate(nH)}
i11 = {n['name']: i for i, n in enumerate(n11)}


def world(nodes):
    W = []
    for n in nodes:
        M = np.eye(4); M[:3, :3] = fk.rot(n['r']); M[:3, 3] = n['t']
        W.append(M if n['parent'] < 0 else W[n['parent']] @ M)
    return W


WH, W11 = world(nH), world(n11)
deg = lambda r: '%6.1f %6.1f %6.1f' % tuple(math.degrees(a) for a in r)
print('motion bones (21): name | HCTP parent, rest r (deg), world pos | 2011 parent, rest r, world pos')
for b in fk.MOT_BONES:
    h, s = nH[iH[b]], n11[i11[b]]
    print('%-12s | %-10s %s  %s | %-12s %s  %s' % (b, nH[h['parent']]['name'] if h['parent'] >= 0 else '-', deg(h['r']),
          '%6.2f %6.2f %6.2f' % tuple(WH[iH[b]][:3, 3]), n11[s['parent']]['name'] if s['parent'] >= 0 else '-', deg(s['r']),
          '%6.2f %6.2f %6.2f' % tuple(W11[i11[b]][:3, 3])))
print()
onlyH = [n['name'] for n in nH if n['name'] not in i11]
only11 = [n['name'] for n in n11 if n['name'] not in iH]
print('HCTP-only nodes:', onlyH)
print('2011-only nodes:', only11)
print()
segs = [('koshi', 'mune'), ('mune', 'kubi'), ('kubi', 'atama'), ('l_sakotsu', 'l_ninoude'), ('l_ninoude', 'l_kote'),
        ('l_kote', 'l_te'), ('root', 'l_momo'), ('l_momo', 'l_sune'), ('l_sune', 'l_ashi'), ('l_ashi', 'l_tsumasaki'), ('root', 'koshi')]
print('segment lengths (world, rest): HCTP Hogan / 2011 ch104 / ratio')
for a, b in segs:
    lh = np.linalg.norm(WH[iH[b]][:3, 3] - WH[iH[a]][:3, 3])
    l1 = np.linalg.norm(W11[i11[b]][:3, 3] - W11[i11[a]][:3, 3])
    print('  %-22s %6.2f %6.2f %5.2f' % (a + '-' + b, lh, l1, l1 / lh))
