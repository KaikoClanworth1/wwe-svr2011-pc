"""WWE '13 body motion (YANM block) -> SvR 2011 YMKs body keys (21 FK bones), for tools/wwe13_entrance.py.

Both games use the same rig (tools/w13_char.py): Biped names in WWE '13, the 2011 Japanese names in
2011, -Y up, floor origin, WWE '13 in cm and 2011 in model units (10 cm).

1. WWE '13 pose: yanm_decode tracks sampled at a (60 Hz) frame, rotations replace the bind local
   rotation, translations add to the bind t, IK calves solved as the game does (yanm_decode._solve_ik).
2. Target per 2011 motion bone: world rotation R13(anim) * R13(bind)^T * R11(bind) (the binds are both
   A-poses, so a bone keeps its delta from the bind), position 0.1 * the WWE '13 world position.
3. FK solver: parents first, each bone's local rotation against the pose already rebuilt from the
   quantised keys, relative to the motion bind (REST11: HCTP's rest rotations, the bind 2011's own
   motion data assumes), restricted to the bone's YMKs axes (TYPES_M1): xyz free; yz (sakotsu,
   kubi, te) keep the bone direction; y (sune, tsumasaki) and xy (kote) are hinges. The hand's twist
   about the forearm goes to the forearm roll (kote x), as tools' YMBs retarget does.
Root translation: raw = model units / 0.01 (= 10 x cm).
"""
import math
import numpy as np

import ymks
import yanm_decode as Y
from w13_skeletons import NODES13, NODES11, REST11, MAP

ANG = ymks.ANG
Y.add_names([n[0] for n in NODES13])


def rotm(r):
    x, y, z = r
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    return np.array([[cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz],
                     [cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz],
                     [-sy, sx * cy, cx * cy]])


def euler_zyx(R):
    """R = Rz Ry Rx -> (x, y, z)"""
    sy = max(-1.0, min(1.0, -R[2, 0]))
    y = math.asin(sy)
    if abs(sy) < 0.99999:
        return math.atan2(R[2, 1], R[2, 2]), y, math.atan2(R[1, 0], R[0, 0])
    return 0.0, y, math.atan2(-R[0, 1], R[1, 1])


def qmat(q):
    return np.array(Y.q_to_mat(q))


def s16(v):
    v = int(round(v)) & 0xffff
    return v - 0x10000 if v & 0x8000 else v


def _nodes(tab):
    return [dict(name=n, t=np.array(t, float), r=np.array(r, float), parent=p) for n, t, r, p in tab]


# ============================================================================ WWE '13 pose

class Pose13:
    """ik=False: plain FK (WWE '12 bodies have no IK calves; any IK stream is then ignored)"""

    def __init__(self, table=NODES13, ik=True):
        self.ik = ik
        self.nodes = _nodes(table)
        self.idx = {n['name']: i for i, n in enumerate(self.nodes)}
        self.bq = [Y.q_euler(*n['r']) for n in self.nodes]
        W = []
        for n in self.nodes:
            M = np.eye(4)
            M[:3, :3] = rotm(n['r'])
            M[:3, 3] = n['t']
            W.append(M if n['parent'] < 0 else W[n['parent']] @ M)
        self.Wbind = W

    def world(self, m, f):
        """m: yanm_decode.decode(..., sample=False); f: 60 Hz frame -> list of 4x4 (cm)"""
        ns = self.nodes
        ev = {}
        for name, tr in m['tracks'].items():
            if name in self.idx:
                ev[name] = Y.sample_track(tr, f)
        loc_t, loc_R = [], []
        for i, n in enumerate(ns):
            e = ev.get(n['name'])
            t = n['t'].copy()
            q = self.bq[i]
            if e:
                if e['rot'] is not None:
                    q = e['rot']
                if e['pos'] is not None:
                    t = t + np.array(e['pos'])
            loc_t.append(t)
            loc_R.append(qmat(q))
        W = self._fk(loc_t, loc_R)
        for n in (ns if self.ik else ()):
            e = ev.get(n['name'])
            if not e or e['ik'] is None or e['rot'] is not None:
                continue
            mode = m['tracks'][n['name']]['parts']['ik']['mode']
            if mode not in (1, 2):
                continue
            side = n['name'].split()[1]
            th, ca, fo = self.idx.get('Bip %s Thigh' % side), self.idx[n['name']], self.idx.get('Bip %s Foot' % side)
            if th is None or fo is None:
                continue
            self._ik(W, loc_t, loc_R, th, ca, fo, e['ik'], mode)
            W = self._fk(loc_t, loc_R)
        return W

    def _fk(self, loc_t, loc_R):
        W = []
        for i, n in enumerate(self.nodes):
            M = np.eye(4)
            M[:3, :3] = loc_R[i]
            M[:3, 3] = loc_t[i]
            W.append(M if n['parent'] < 0 else W[n['parent']] @ M)
        return W

    def _chain(self, a, b):
        tot, i = 0.0, b
        while i != a and i >= 0:
            tot += float(np.linalg.norm(self.nodes[i]['t']))
            i = self.nodes[i]['parent']
        return tot

    def _ik(self, W, loc_t, loc_R, thigh, calf, foot, target, mode):
        """two-bone IK, yanm_decode._solve_ik (game 0x82384b78)"""
        Wt = W[thigh]
        hip = Wt[:3, 3]
        L1, L2 = self._chain(thigh, calf), self._chain(calf, foot)
        ax = 1 if mode == 1 else 2
        u = Wt[:3, ax]
        tx, ty, tz, w = target
        dv = np.array([tx, ty, tz]) - hip
        dist = float(np.linalg.norm(dv)) or 1e-6
        d = dv / dist
        zc = np.cross(d, u); zc /= (np.linalg.norm(zc) or 1.0)
        yc = np.cross(zc, d); yc /= (np.linalg.norm(yc) or 1.0)
        dw = math.sqrt((L1 + L2 * math.cos(w)) ** 2 + (L2 * math.sin(w)) ** 2)
        k = max(0.0, min(1.0, (w - math.pi / 6) * -5.729578))
        de = dist + (dw - dist) * k
        ck = max(-1.0, min(1.0, (de * de - L1 * L1 - L2 * L2) / (2 * L1 * L2)))
        knee = math.acos(ck)
        a = math.atan2(-L2 * math.sin(knee), L1 + L2 * ck)
        if mode == 1:
            F = np.column_stack([d, yc, zc])
            Rl = qmat(Y.qaxis(1, a))
            calf_R = qmat(Y.qaxis(1, knee))
        else:
            z2 = u - d * float(np.dot(u, d)); z2 /= (np.linalg.norm(z2) or 1.0)
            y2 = np.cross(z2, d); y2 /= (np.linalg.norm(y2) or 1.0)
            F = np.column_stack([d, y2, z2])
            Rl = qmat(Y.qaxis(2, -a))
            calf_R = qmat(Y.qaxis(2, -knee))
        Rw = F @ Rl
        Pr = W[self.nodes[thigh]['parent']][:3, :3]
        loc_R[thigh] = Pr.T @ Rw
        loc_R[calf] = calf_R


# ============================================================================ 2011 side

class Skel11:
    """SvR 2011 body (ch104 node table pruned to the motion chains). bind='rest' (HCTP rest
    rotations for the 21 motion bones, what 2011's own motions assume) or 'own' (node r)."""

    def __init__(self, bind='rest', table=NODES11):
        self.nodes = _nodes(table)
        self.idx = {n['name']: i for i, n in enumerate(self.nodes)}
        self.mot = {self.idx[b]: k for k, b in enumerate(ymks.BONES)}
        self.local = []
        for n in self.nodes:
            M = np.eye(4)
            M[:3, :3] = rotm(n['r'])
            M[:3, 3] = n['t']
            self.local.append(M)
        self.Wbind = []
        for i, n in enumerate(self.nodes):
            self.Wbind.append(self.local[i] if n['parent'] < 0 else self.Wbind[n['parent']] @ self.local[i])
        self.B = {}
        for i, n in enumerate(self.nodes):
            if i in self.mot:
                self.B[i] = rotm(REST11[n['name']]) if bind == 'rest' else rotm(n['r'])

    def pose(self, root, ang):
        """root: raw (0.01 units); ang: [21][3] s16 (x, y, z) -> list of 4x4"""
        W = []
        for i, n in enumerate(self.nodes):
            k = self.mot.get(i)
            if k is None:
                L = self.local[i]
            else:
                L = np.eye(4)
                L[:3, :3] = self.B[i] @ rotm([a * ANG for a in ang[k]])
                L[:3, 3] = np.array(root, float) * ymks.ROOT_UNIT if k == 0 else n['t']
            W.append(L if n['parent'] < 0 else W[n['parent']] @ L)
        return W

    def joints(self, W, names):
        return np.array([W[self.idx[nm]][:3, 3] for nm in names])


# ============================================================================ retarget + solver

class Retarget:
    def __init__(self, bind='rest', twist=True, ik=True, nodes13=NODES13):
        self.p13 = Pose13(nodes13, ik)
        self.s11 = Skel11(bind)
        self.twist = twist
        self.src = {}
        for j, n in enumerate(self.s11.nodes):
            if n['name'] in MAP:
                self.src[j] = self.p13.idx[MAP[n['name']]]

    def targets(self, W13):
        """WWE '13 world matrices -> {2011 node index: (R 3x3, pos)} for the 21 bones"""
        out = {}
        for j, b in self.src.items():
            R = W13[b][:3, :3] @ self.p13.Wbind[b][:3, :3].T @ self.s11.Wbind[j][:3, :3]
            out[j] = (R, 0.1 * W13[b][:3, 3])
        return out

    def _angles(self, Rm, k):
        ax = ymks.axes_of(ymks.TYPES_M1[k])
        if ax == 'xyz':
            x, y, z = euler_zyx(Rm)
        elif ax == 'yz':
            x, y, z = 0.0, math.asin(max(-1.0, min(1.0, -Rm[2, 0]))), math.atan2(Rm[1, 0], Rm[0, 0])
        elif ax == 'xy':
            y = math.atan2(-Rm[2, 0], Rm[0, 0])
            M = rotm((0, y, 0)).T @ Rm
            x, z = math.atan2(M[2, 1] - M[1, 2], M[1, 1] + M[2, 2]), 0.0
        else:                                   # 'y' hinge
            x, y, z = 0.0, math.atan2(Rm[0, 2] - Rm[2, 0], Rm[0, 0] + Rm[2, 2]), 0.0
        return [s16(x / ANG) if 'x' in ax else 0, s16(y / ANG) if 'y' in ax else 0, s16(z / ANG) if 'z' in ax else 0]

    def solve(self, W13):
        """-> (root raw 3 floats, ang [21][3] s16)"""
        S = self.s11
        tg = self.targets(W13)
        ang = [[0, 0, 0] for _ in range(21)]
        root = [0.0, 0.0, 0.0]
        W = [None] * len(S.nodes)

        def local_of(i):
            n = S.nodes[i]
            k = S.mot.get(i)
            if k is None:
                return S.local[i]
            L = np.eye(4)
            L[:3, :3] = S.B[i] @ rotm([a * ANG for a in ang[k]])
            L[:3, 3] = np.array(root) * ymks.ROOT_UNIT if k == 0 else n['t']
            return L

        def fk_from(i):
            p = S.nodes[i]['parent']
            W[i] = local_of(i) if p < 0 else W[p] @ local_of(i)

        for i, n in enumerate(S.nodes):
            k = S.mot.get(i)
            p = n['parent']
            if k is not None:
                R, pos = tg[i]
                Rp = np.eye(3) if p < 0 else W[p][:3, :3]
                Rm = S.B[i].T @ (Rp.T @ R)
                ang[k] = self._angles(Rm, k)
                if k == 0:
                    root = [float(v) / ymks.ROOT_UNIT for v in pos]
                if self.twist and n['name'].endswith('_te'):
                    # the hand's roll about the forearm -> kote x, then the hand again
                    kote = S.idx[n['name'][:2] + 'kote']
                    kk = S.mot[kote]
                    chain = []
                    c = i
                    while c != kote:
                        c = S.nodes[c]['parent']
                        chain.append(c)
                    chain.reverse()
                    for _ in range(3):
                        Rp = W[p][:3, :3]
                        x = euler_zyx(S.B[i].T @ (Rp.T @ R))[0]
                        if abs(x) < 1e-4:
                            break
                        ang[kk][0] = s16(ang[kk][0] + x / ANG)
                        for c in chain:
                            fk_from(c)
                    Rm = S.B[i].T @ (W[p][:3, :3].T @ R)
                    ang[k] = self._angles(Rm, k)
            fk_from(i)
        return root, ang

    def keys(self, m, nframes):
        """YANM block -> [(root, ang)] for 2011 frames 0..nframes-1 (sampled at 60 Hz frame 2f)"""
        out = []
        last = m['frames'] - 1
        for f in range(nframes):
            out.append(self.solve(self.p13.world(m, min(2 * f, last))))
        return out


def body_track(keys, events=None):
    """[(root, ang)] + {frame: [event bytes]} -> YMKs motion data (dense 0x7d keys, count 0)"""
    ev = {}
    for f, lst in (events or {}).items():
        ev.setdefault(min(f, len(keys) - 1), []).extend(lst)
    ks = []
    for f, (root, ang) in enumerate(keys):
        bones = {k: {a: ang[k]['xyz'.index(a)] for a in ymks.axes_of(ymks.TYPES_M1[k])} for k in range(21)}
        ks.append(dict(frame=f, count=0, root=tuple(root), bones=bones, events=ev.get(f, [])))
    return ymks.build_motion(ks, len(keys))
