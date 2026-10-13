"""World poses of a 2011 body -> YMBs motion channels (moves_port.py's taunt path; ymbs.py codec).

The game's YMBs pose, as far as known (checked against stock taunts and with ported ones in game):
rotation channels replace a node's local r (Euler zyx, R = Rz Ry Rx), the root translation replaces
root t, limbs (ninoude-kote-te, momo-sune-ashi) are two-bone IK to root + IK vector, the knee /
elbow side given by a pole angle that turns the forward axis (0, 0, -1, its part along the limb
removed) about the limb. pose() is that forward model, solve_frame() its inverse: IK targets and
poles from the target joints, then the rotation channels parents first, each against the pose the
game will build from the channels so far. YMBs hands store only y / z: their twist about the
forearm goes to the forearm roll (kote x), as w13_retarget does for YMKs.

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


def _aim(x_dir, d, sign):
    x = x_dir / np.linalg.norm(x_dir)
    z = sign * (d - np.dot(d, x) * x)
    z /= np.linalg.norm(z)
    return np.column_stack([x, np.cross(z, x), z])


def pose(ns, vals):
    """the game's pose from channel values -> world 4x4 per node"""
    idx = {n['name']: i for i, n in enumerate(ns)}
    local = [_mat(euler(n['r']), n['t']) for n in ns]
    for c, name in enumerate(MOT_BONES):
        if name in idx and vals['rot_has'][c]:
            i = idx[name]
            local[i] = _mat(euler(vals['rot'][c]), ns[i]['t'])
    local[idx['root']][:3, 3] = vals['root_t']

    def fk():
        W = []
        for i, n in enumerate(ns):
            W.append(local[i] if n['parent'] < 0 else W[n['parent']] @ local[i])
        return W
    W = fk()
    root_p = W[idx['root']][:3, 3]
    fwd = np.array([0, 0, -1.0])
    for ti, (up, lo, end) in enumerate(LIMBS):
        sgn = 1 if ti < 2 else -1
        twist_bone = lo if ti < 2 else None
        iu, il, ie = idx[up], idx[lo], idx[end]
        S = W[iu][:3, 3]
        L1 = np.linalg.norm(W[il][:3, 3] - S)
        L2 = np.linalg.norm(W[ie][:3, 3] - W[il][:3, 3])
        T = root_p + vals['ik'][ti]
        a = T - S
        dist = min(np.linalg.norm(a), (L1 + L2) * 0.9999)
        a = a / np.linalg.norm(a)
        d0 = fwd - np.dot(fwd, a) * a
        d0 /= np.linalg.norm(d0)
        th = vals['twist'][ti]
        d = d0 * math.cos(th) + np.cross(a, d0) * math.sin(th)
        cosA = (L1 * L1 + dist * dist - L2 * L2) / (2 * L1 * dist)
        sinA = math.sqrt(max(0.0, 1 - cosA * cosA))
        J = S + a * (L1 * cosA) + d * (L1 * sinA)
        E = S + a * dist
        local[iu][:3, :3] = W[ns[iu]['parent']][:3, :3].T @ _aim(J - S, d, sgn)
        W = fk()
        Rl = _aim(E - J, d, sgn)
        if twist_bone:   # (forearm roll channel, x, from the motion)
            Rl = Rl @ euler([vals['rot'][MOT_BONES.index(twist_bone)][0], 0, 0])
        local[il][:3, :3] = W[ns[il]['parent']][:3, :3].T @ Rl
        W = fk()
    return W


def solve_frame(ns, target, rot=True):
    """target: name -> 4x4 world for the MOT bones -> values in frame_values() form. rot=False: the
    root translation, IK targets and poles only (the 60 Hz channels)"""
    idx = {n['name']: i for i, n in enumerate(ns)}
    vals = {'root_t': target['root'][:3, 3].copy(), 'ik': [], 'twist': [],
            'rot': [[0.0, 0.0, 0.0] for _ in MOT_BONES], 'rot_has': [True] * len(MOT_BONES)}
    root_p = target['root'][:3, 3]
    fwd = np.array([0, 0, -1.0])
    for up, lo, end in LIMBS:
        S, J, E = target[up][:3, 3], target[lo][:3, 3], target[end][:3, 3]
        vals['ik'].append(E - root_p)
        a = E - S
        a = a / np.linalg.norm(a)
        d0 = fwd - np.dot(fwd, a) * a
        d0 /= np.linalg.norm(d0)
        dj = (J - S) - np.dot(J - S, a) * a
        n = np.linalg.norm(dj)
        vals['twist'].append(0.0 if n < 1e-6 else
                             math.atan2(np.dot(np.cross(d0, dj / n), a), np.dot(d0, dj / n)))
    if not rot:
        return vals
    for c, name in enumerate(MOT_BONES):
        if name not in idx:
            continue
        Wp = pose(ns, vals)
        i = idx[name]
        par = ns[i]['parent']
        Rpar = np.eye(3) if par < 0 else Wp[par][:3, :3]
        vals['rot'][c] = euler_zyx(Rpar.T @ target[name][:3, :3])
    for side in ('l', 'r'):
        ck, ct = MOT_BONES.index(side + '_kote'), MOT_BONES.index(side + '_te')
        par = ns[idx[side + '_te']]['parent']
        for _ in range(4):
            Wp = pose(ns, vals)
            x = euler_zyx(Wp[par][:3, :3].T @ target[side + '_te'][:3, :3])[0]
            if abs(x) < 1e-4:
                break
            vals['rot'][ck][0] += x
        Wp = pose(ns, vals)
        vals['rot'][ct] = euler_zyx(Wp[par][:3, :3].T @ target[side + '_te'][:3, :3])
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
        for nm in ('root', 'l_te', 'r_te', 'l_ashi', 'r_ashi', 'atama'):
            worst = max(worst, float(np.linalg.norm(Wp[idx[nm]][:3, 3] - tg[nm][:3, 3])))
    return worst


def struct_pack_dir(mid, frames, size):
    import struct
    return (struct.pack('<I', 2) + struct.pack('<BBHIII', 0, 0, mid, 44, frames, 0) +
            struct.pack('<BBHIII', 0, 0, 0x7fff, 44 + size, 0, 0))
