"""World poses of a 2011 body -> YMBs motion channels (moves_port.py's taunt path; ymbs.py codec).

The game's YMBs pose (sub_823A4420 / the IK sub_823A4DD8, as the Mod Maker's char_preview.cpp builds it -
checked bone by bone against the game's own pose dump, 9289602):
  - rotation channels replace the bone's local rotation (Euler zyx, R = Rz Ry Rx), except the IK bones
    (ninoude, kote, momo, sune: not driven); sakotsu / te take y and z only, tsumasaki y only; the root
    translation replaces root t; the kote channel's x (roll) goes to the forearm twist bones kote_x /
    kote_x2 (half each);
  - the limbs are solved in their parent's frame (arms the sakotsu's, legs the root's) to the target
    root + IK vector (world): two equal segments, L = 2 x the shoulder / hip to elbow / knee distance,
    d clamped at 0.99 L for arms (legs soft), bend = 2 acos(d / L) (legs negative); upper bone (in the
    parent frame) = Align(+X -> target) . RotX(pole, arms - pi) . RotY(-bend / 2), lower bone RotY(bend).
pose() is that forward model; solve_frame() its inverse: the rotation channels parents first, each against
the pose the game builds from the channels so far, the IK vector from the target hand / foot, the pole from
where the target elbow / knee lies about the shoulder-target line (in the parent frame), the hand's roll
about the forearm into the kote roll. (A first model with world-space poles about a fixed forward axis
played as a frozen arms-out pose in game.)

    sample = w13_sampler(w13_retarget.Retarget(...), yanm block)     # t seconds -> {name: 4x4}
    data, frames = build_motion(bank, nodes11(), sample, frames30)   # a type-0 motion's bytes
"""
import math

import numpy as np

import ymbs

MOT_BONES = ('root koshi mune kubi atama l_sakotsu l_ninoude l_kote l_te r_sakotsu r_ninoude r_kote r_te '
             'l_momo l_sune l_ashi l_tsumasaki r_momo r_sune r_ashi r_tsumasaki').split()
LIMBS = [('l_ninoude', 'l_kote', 'l_te'), ('r_ninoude', 'r_kote', 'r_te'),
         ('l_momo', 'l_sune', 'l_ashi'), ('r_momo', 'r_sune', 'r_ashi')]
ANG = 2 * math.pi / 65536


def euler(r):
    """R = Rz Ry Rx"""
    x, y, z = r
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    return np.array([[cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz],
                     [cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz],
                     [-sy, sx * cy, cx * cy]])


def euler_zyx(R):
    """R = Rz Ry Rx -> [x, y, z]"""
    y = math.asin(max(-1.0, min(1.0, -R[2, 0])))
    if abs(R[2, 0]) < 0.99999:
        return [math.atan2(R[2, 1], R[2, 2]), y, math.atan2(R[1, 0], R[0, 0])]
    return [0.0, y, math.atan2(-R[0, 1], R[1, 1])]


def nodes11():
    """the 2011 body (ch104's node table pruned to the motion chains, w13_skeletons)"""
    from w13_skeletons import NODES11
    return [dict(name=n, t=np.array(t, float), r=np.array(r, float), parent=p) for n, t, r, p in NODES11]


def _mat(R, t):
    M = np.eye(4)
    M[:3, :3] = R
    M[:3, 3] = t
    return M


def world(ns):
    W = []
    for n in ns:
        L = _mat(euler(n['r']), n['t'])
        W.append(L if n['parent'] < 0 else W[n['parent']] @ L)
    return W


def frame_values(seg, f):
    """a decoded segment (ymbs.Bank.decode) at key f -> values (radians / model units)"""
    def g(v, i):
        return v[max(0, min(i, len(v) - 1))]
    out = {}
    bid, sc, xyz = seg['root']
    out['root_t'] = np.array([g(v, 2 * f) * sc * 1e-4 for v in xyz])   # (translation keyed at 2x rate)
    out['ik'] = [np.array([g(v, 2 * f) * sc * 1e-4 for v in xyz]) for bid, sc, xyz in seg['ik']]
    out['twist'] = [g(v, f) * sc * ANG for bid, sc, v in seg['twist']]
    out['rot'] = [[(g(a, f) * sc * ANG) if a else 0.0 for a in axes] for bid, sc, axes in seg['rot']]
    out['rot_has'] = [any(a for a in axes) for bid, sc, axes in seg['rot']]
    return out


IK_BONES = {6, 7, 10, 11, 13, 14, 17, 18}   # (MOT_BONES indices the game doesn't drive)
YZ_ONLY = {5, 8, 9, 12}                    # sakotsu, te
Y_ONLY = {16, 20}                          # tsumasaki
GAME_LIMBS = [('l_ninoude', 'l_kote', 'l_sakotsu', True, 'l_te'), ('r_ninoude', 'r_kote', 'r_sakotsu', True, 'r_te'),
              ('l_momo', 'l_sune', 'root', False, 'l_ashi'), ('r_momo', 'r_sune', 'root', False, 'r_ashi')]


def _rx(a):
    return euler([a, 0.0, 0.0])


def _ry(a):
    return euler([0.0, a, 0.0])


def _align(n):
    """the shortest arc +X -> n"""
    axis = np.array([0.0, -n[2], n[1]])
    al = np.linalg.norm(axis)
    ang = math.acos(max(-1.0, min(1.0, n[0])))
    if al < 1e-6:
        return np.diag([1.0, 1.0, 1.0]) if n[0] >= 0 else np.diag([-1.0, -1.0, 1.0])
    k = axis / al
    c, sn, t = math.cos(ang), math.sin(ang), 1 - math.cos(ang)
    return np.array([[c + k[0] * k[0] * t, k[0] * k[1] * t - k[2] * sn, k[0] * k[2] * t + k[1] * sn],
                     [k[1] * k[0] * t + k[2] * sn, c + k[1] * k[1] * t, k[1] * k[2] * t - k[0] * sn],
                     [k[2] * k[0] * t - k[1] * sn, k[2] * k[1] * t + k[0] * sn, c + k[2] * k[2] * t]])


def _fk(ns, local):
    W = []
    for i, n in enumerate(ns):
        W.append(local[i] if n['parent'] < 0 else W[n['parent']] @ local[i])
    return W


def _constrain(c, e):
    e = list(e)
    if c in YZ_ONLY:
        e[0] = 0.0
    if c in Y_ONLY:
        e[0] = e[2] = 0.0
    return e


def pose(ns, vals, limbs=True):
    """the game's pose from channel values -> world 4x4 per node"""
    idx = {n['name']: i for i, n in enumerate(ns)}
    local = [_mat(euler(n['r']), n['t']) for n in ns]
    for c, name in enumerate(MOT_BONES):
        if name in idx and c not in IK_BONES and vals['rot_has'][c]:
            i = idx[name]
            local[i] = _mat(euler(_constrain(c, vals['rot'][c])), ns[i]['t'])
    local[idx['root']][:3, 3] = vals['root_t']
    for side in ('l', 'r'):   # (the kote's roll on the twist bones, half each when there are two)
        roll = vals['rot'][MOT_BONES.index(side + '_kote')][0]
        two = (side + '_kote_x2') in idx
        for nm in (side + '_kote_x', side + '_kote_x2'):
            if nm in idx:
                local[idx[nm]][:3, :3] = _rx(roll * 0.5 if two else roll)
    W = _fk(ns, local)
    if not limbs:
        return W
    root_p = W[idx['root']][:3, 3]
    for l, (up, lo, par, arm, end) in enumerate(GAME_LIMBS):
        iu, il, ip = idx[up], idx[lo], idx[par]
        su = ns[iu]['parent']
        P = W[ip]
        Pr = P[:3, :3]
        S = Pr.T @ (W[iu][:3, 3] - P[:3, 3])
        v = Pr.T @ (root_p + vals['ik'][l] - P[:3, 3]) - S
        L = 2 * np.linalg.norm(W[il][:3, 3] - W[iu][:3, 3])
        d = np.linalg.norm(v)
        if L <= 1e-4 or d <= 1e-5:
            continue
        lim = (0.99 if arm else 0.99999) * L
        if d >= lim:
            d = lim if arm else lim + (d - lim) * 0.3
        bend = 0.0 if d >= L else 2 * math.acos(min(d / L, 0.99999))
        if not arm:
            bend = -bend
        pole = vals['twist'][l] - (math.pi if arm else 0.0)
        R = _align(v / np.linalg.norm(v)) @ _rx(pole) @ _ry(-bend * 0.5)
        local[iu][:3, :3] = W[su][:3, :3].T @ (Pr @ R)
        local[il][:3, :3] = _ry(bend)
    return _fk(ns, local)


def _yz(R):
    return [0.0, math.asin(max(-1.0, min(1.0, -R[2, 0]))), math.atan2(R[1, 0], R[0, 0])]


def _y(R):
    return [0.0, math.atan2(R[0, 2] - R[2, 0], R[0, 0] + R[2, 2]), 0.0]


def solve_frame(ns, target, rot=True):
    """target: name -> 4x4 world for the MOT bones -> values in frame_values() form. rot=False: the root
    translation and IK vectors only (the 60 Hz channels; the poles come from the 30 Hz solve)"""
    idx = {n['name']: i for i, n in enumerate(ns)}
    vals = {'root_t': target['root'][:3, 3].copy(), 'ik': [], 'twist': [0.0] * 4,
            'rot': [[0.0, 0.0, 0.0] for _ in MOT_BONES], 'rot_has': [True] * len(MOT_BONES)}
    root_p = target['root'][:3, 3]
    for up, lo, par, arm, end in GAME_LIMBS:
        vals['ik'].append(target[end][:3, 3] - root_p)
    if not rot:
        return vals

    def solve_pole(l):
        up, lo, par, arm, end = GAME_LIMBS[l]
        W = pose(ns, vals, limbs=False)
        P = W[idx[par]]
        Pr = P[:3, :3]
        S = Pr.T @ (W[idx[up]][:3, 3] - P[:3, 3])
        v = Pr.T @ (target[end][:3, 3] - P[:3, 3]) - S
        w = Pr.T @ (target[lo][:3, 3] - P[:3, 3]) - S
        nv = np.linalg.norm(v)
        if nv < 1e-6:
            return
        n = v / nv
        q = _align(n).T @ (w - np.dot(w, n) * n)
        if np.hypot(q[1], q[2]) < 1e-6:   # (straight: keep the last pole)
            return
        sgn = 1.0 if arm else -1.0
        vals['twist'][l] = math.atan2(-q[1] * sgn, q[2] * sgn) + (math.pi if arm else 0.0)

    for c, name in enumerate(MOT_BONES):
        if name not in idx:
            continue
        if c in IK_BONES:
            if c in (6, 10, 13, 17):   # (at the upper bone: that limb's pole)
                solve_pole({6: 0, 10: 1, 13: 2, 17: 3}[c])
            continue
        W = pose(ns, vals)
        i = idx[name]
        par = ns[i]['parent']
        Rl = (np.eye(3) if par < 0 else W[par][:3, :3].T) @ target[name][:3, :3]
        vals['rot'][c] = _yz(Rl) if c in YZ_ONLY else _y(Rl) if c in Y_ONLY else euler_zyx(Rl)
        if name.endswith('_te'):   # (the hand's roll about the forearm -> kote x, then the hand again)
            ck = MOT_BONES.index(name[:2] + 'kote')
            for _ in range(4):
                W = pose(ns, vals)
                x = euler_zyx(W[par][:3, :3].T @ target[name][:3, :3])[0]
                if abs(x) < 1e-4:
                    break
                vals['rot'][ck][0] += x
            W = pose(ns, vals)
            vals['rot'][c] = _yz(W[par][:3, :3].T @ target[name][:3, :3])
    return vals


def unwrap(seq):
    """angles in radians, continuous"""
    out = [seq[0]]
    for v in seq[1:]:
        out.append(out[-1] + (v - out[-1] + math.pi) % (2 * math.pi) - math.pi)
    return out


def solve(ns, sample, frames):
    """-> (rotation values at 30 Hz, frames + 1; translation / IK values at 60 Hz, 2 frames + 1)"""
    return ([solve_frame(ns, sample(k / 30.0)) for k in range(frames + 1)],
            [solve_frame(ns, sample(k / 60.0), rot=False) for k in range(2 * frames + 1)])


def build_motion(bank, ns, sample, frames, typ=0, solved=None):
    """sample(t seconds) -> {name: 4x4 world}. -> (data bytes, frames): a motion for `bank`'s channel
    table `typ`, rotation keys at 30 Hz (frames + 1 values), translation / IK at 60 Hz. solved: solve()'s
    result, when one motion goes to several banks"""
    K = frames
    rot_vals, tr_vals = solved or solve(ns, sample, frames)
    chans = {'root': None, 'ik': [], 'twist': [], 'rot': [], 'extra': None}
    ik_i = tw_i = rot_i = 0
    for bid, desc in bank.channels(typ):
        f, b1, scale = desc[0], desc[1], desc[2]
        if f & 0x80 and f & 0x40:
            chans['ik'].append((bid, scale, [[v['ik'][ik_i][a] / (scale * 1e-4) for v in tr_vals] for a in range(3)]))
            ik_i += 1
        elif f & 0x80:
            seq = unwrap([v['twist'][tw_i] for v in rot_vals])
            chans['twist'].append((bid, scale, [x / (scale * ANG) for x in seq]))
            tw_i += 1
        elif f & 0x40:
            chans['root'] = (bid, scale, [[v['root_t'][a] / (scale * 1e-4) for v in tr_vals] for a in range(3)])
        elif f & 0x20:
            chans['extra'] = (bid, scale, [0] * (K + 1))
        else:
            axes = []
            raw = (b1 & 0xF0) == 0x40
            for ax in range(3):
                if f & (1 << ax):
                    seq = unwrap([v['rot'][rot_i][ax] for v in rot_vals])
                    vv = [x / (scale * ANG) for x in seq]
                    axes.append(vv[:K] if raw else vv)
                else:
                    axes.append(None)
            chans['rot'].append((bid, scale, axes))
            rot_i += 1
    return ymbs.encode_motion(bank, typ, K, chans), frames


def w13_sampler(R, m):
    """a w13_retarget.Retarget and a YANM block (60 Hz) -> sample(t seconds) -> {2011 name: 4x4}: the
    same targets the YMKs path solves for (rotation delta from the A-pose binds, 0.1 x position)"""
    names = {j: n['name'] for j, n in enumerate(R.s11.nodes)}
    last = m['frames'] - 1

    def sample(t):
        W13 = R.p13.world(m, min(int(round(t * 60.0)), last))
        return {names[j]: _mat(Rm, pos) for j, (Rm, pos) in R.targets(W13).items()}
    return sample


def check(bank, data, frames, typ, sample, ns, mid=0x7ffe):
    """decode `data` back (one motion in a bank built around it) and compare the game's pose with the
    targets -> the worst joint distance (model units) over a few keys"""
    one = (bank.d[:0x110] + struct_pack_dir(mid, frames, len(data)) + b'\0' * 4 +
           bytes([typ, 0, 0, 0]) + bytes(16) + bytes([0, 0, 0, 0]) + bytes(16) + data)
    seg = ymbs.Bank(one).decode(0)[0]
    idx = {n['name']: i for i, n in enumerate(ns)}
    worst = 0.0
    for f in range(0, frames + 1, max(1, frames // 6)):
        tg = sample(f / 30.0)
        Wp = pose(ns, frame_values(seg, f))
        for nm in ('root', 'l_te', 'r_te', 'l_ashi', 'r_ashi', 'atama', 'l_kote', 'r_kote', 'l_sune', 'r_sune'):
            worst = max(worst, float(np.linalg.norm(Wp[idx[nm]][:3, 3] - tg[nm][:3, 3])))
    return worst


def struct_pack_dir(mid, frames, size):
    import struct
    return (struct.pack('<I', 2) + struct.pack('<BBHIII', 0, 0, mid, 44, frames, 0) +
            struct.pack('<BBHIII', 0, 0, 0x7fff, 44 + size, 0, 0))
