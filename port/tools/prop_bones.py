"""Bone rest rotations of a superstar model lined up with SvR 2011's own (found working on B3).

    rep = fix_pac(ch_pac, out=None)                 # out=None: report only
    py prop_bones.py <ch.pac> [<out.pac>]          # (out: write the fixed copy)
    py prop_bones.py --stock <2011 pac folder>     # prop_bones_stock.json from the shipped models

Why: the game poses a model's bones from the stock motions as if its rests were the stock ones, so a
model whose body bones rest rotated unlike the shipped models (an edited or user-made one) is distorted
in motion - seen in game 14 Oct (attire mods on Rey / Miz / Ziggler, joints kept in place): a hand rest
turned 1.4 rad stretched the fingers, koshi / mune / root turned wrecked the whole body; the fixed copy
played like stock. Every shipped 2011 model and every port we make (2007-2010, '13, 2K14-17, PS2) has
stock rests (nothing changes for them). B3 itself (a belt held vertical on a player's SvR 2010 Benoit)
was NOT reproduced: waist belts sat right with te / kata_m / onaka / sakotsu turned; the game names
l_te / r_te, l_kata_m / r_kata_m and onaka (image 0x820028C0 / 0x82001F34, 0x8201264C / 0x82012658,
0x820067D0) - the places it puts held props.

Stock rests vary a little (women's thumbs, one 0.35 hand, the two forearm shapes), so the reference is
every rest the shipped models use per bone: prop_bones_stock.json (made by --stock from the 2011 pac).
Fix: per JBOY of every attire (the body and its parts each have a node table), every body bone whose
rest rotation is further than TOL from all the stock ones gets the nearest stock rotation, parents
first; each node's translation (in its parent's frame) is recomputed so every joint stays where it
was. The mesh is in model space (bind = rest; JBOY stores no inverse binds, the game builds them from
the nodes), so it doesn't move; the bones' frames become stock ones, which is what the stock motions
and the held props expect. Models already like the stock ones are left byte for byte.
Node (80 bytes): name[16], f32 t[3] +16, f32 r[3] +28 (R = Rz Ry Rx), u32 parent +48 (0xFFFFFFFF for
root); big-endian; the table at JBOY +8 + u32 at +0x28, count at +0x20.
"""
import json
import math
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

TOL = 0.2
STOCK = os.path.join(HERE, 'prop_bones_stock.json')
FACE_FROM = 'atama'   # (the face bones under the head are left alone: their rests vary per face)
# the standard body skeleton (ch104's, face left out): only these are lined up - hair (kami_*) and cloth
# (kl_* / kr_*) bones vary per model
BODY = set('''root l_momo l_momo_x l_sune l_ashi l_tsumasaki r_momo r_momo_x r_sune r_ashi r_tsumasaki koshi mune
r_sakotsu r_ninoude r_ninoude_x r_kote r_kote_x r_kote_x2 r_te r_yubi10 r_yubi11 r_yubi12 r_yubi20 r_yubi21 r_yubi22
r_yubi30 r_yubi31 r_yubi32 r_yubi40 r_yubi41 r_yubi42 r_yubi00 r_yubi01 r_yubi02 r_ninoude_m r_kote_m r_kata_m
l_sakotsu l_ninoude l_ninoude_x l_kote l_kote_x l_kote_x2 l_te l_yubi30 l_yubi31 l_yubi32 l_yubi10 l_yubi11 l_yubi12
l_yubi40 l_yubi41 l_yubi42 l_yubi20 l_yubi21 l_yubi22 l_yubi00 l_yubi01 l_yubi02 l_kote_m l_ninoude_m l_kata_m
l_mune_dummy l_sakotsu_m r_mune_dummy r_sakotsu_m kubi kubi_x atama onaka'''.split())


def rotm(r):
    x, y, z = r
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    return np.array([[cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz],
                     [cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz],
                     [-sy, sx * cy, cx * cy]])


def _nodes(d):
    n, ptr = struct.unpack_from('>I', d, 0x20)[0], struct.unpack_from('>I', d, 0x28)[0]
    out = []
    for i in range(n):
        o = 8 + ptr + 80 * i
        out.append(dict(o=o, name=d[o:o + 16].split(b'\0')[0].decode('latin-1'),
                        t=struct.unpack_from('>3f', d, o + 16), r=struct.unpack_from('>3f', d, o + 28),
                        parent=struct.unpack_from('>I', d, o + 48)[0]))
    return out


def _face(nodes):
    """names of the bones under the head (atama's descendants)"""
    kids = {}
    for i, n in enumerate(nodes):
        if n['parent'] < len(nodes) and n['parent'] != i:
            kids.setdefault(n['parent'], []).append(i)
    out, stack = set(), [i for i, n in enumerate(nodes) if n['name'] == FACE_FROM]
    while stack:
        i = stack.pop()
        for k in kids.get(i, ()):
            out.add(nodes[k]['name'])
            stack.append(k)
    return out


def _bodies(pac):
    """[{bone: rest rotation}] of every JBOY of every attire (body bones only)"""
    import ch_tool
    import svrfmt
    _, groups = ch_tool.epk8_read(open(pac, 'rb').read())
    out = []
    for name, blob in groups[0][1]:
        if not name.strip(b'\0'):
            continue
        for i, x in svrfmt.pach_read(ch_tool.unpack(blob)):
            d = ch_tool.unpack(x)
            if d[:4] == b'JBOY':
                out.append({n['name']: n['r'] for n in _nodes(d) if n['name'] in BODY})
    return out


def make_stock(pac11, out=STOCK):
    """every rest rotation the shipped models (pac/ch/ch*.pac) use per body bone -> prop_bones_stock.json"""
    import glob
    seen = {}
    for f in sorted(glob.glob(os.path.join(pac11, 'ch', 'ch[0-9]*.pac'))):
        try:
            bodies = _bodies(f)
        except Exception:   # noqa: BLE001 (not a character model pac)
            continue
        for b in bodies:
            for k, r in b.items():
                vs = seen.setdefault(k, [])
                if all(max(abs(a - c) for a, c in zip(r, v)) > 0.05 for v in vs):
                    vs.append([round(x, 4) for x in r])
    with open(out, 'w', newline='\n') as f:
        json.dump({'about': 'prop_bones.py: the rest rotations (x, y, z rad) the shipped SvR 2011 models use '
                            'per body bone', 'bones': seen}, f, indent=0, sort_keys=True)
    return seen


def reference(ref=None):
    """{bone: [stock rest rotations]}: prop_bones_stock.json, else (ref = a model pac) that model's"""
    if ref is None or ref.endswith('.json'):
        return json.load(open(ref or STOCK))['bones']
    out = {}
    for b in _bodies(ref):
        for k, v in b.items():
            out.setdefault(k, []).append(v)
    return out


def fix_jboy(d, ref):
    """-> (new bytes or None, [(bone, old r, new r)])"""
    nodes = _nodes(d)
    face = _face(nodes)
    changed, new_r = [], []
    for n in nodes:
        opts = ref.get(n['name']) if n['name'] in BODY and n['name'] not in face else None
        if opts:
            want = min(opts, key=lambda v: max(abs(a - b) for a, b in zip(n['r'], v)))
            if max(abs(a - b) for a, b in zip(n['r'], want)) > TOL:
                new_r.append(tuple(want))
                changed.append((n['name'], n['r'], tuple(want)))
                continue
        new_r.append(n['r'])
    if not changed:
        return None, []
    N = len(nodes)

    def worlds(rs, ts):
        R, P = [None] * N, [None] * N

        def go(i):
            if R[i] is not None:
                return
            p = nodes[i]['parent']
            if p >= N or p == i:
                R[i], P[i] = rotm(rs[i]), np.array(ts[i])
            else:
                go(p)
                R[i], P[i] = R[p] @ rotm(rs[i]), P[p] + R[p] @ np.array(ts[i])
        for i in range(N):
            go(i)
        return R, P
    _, P_old = worlds([n['r'] for n in nodes], [n['t'] for n in nodes])
    R_new, _ = worlds(new_r, [n['t'] for n in nodes])
    out = bytearray(d)
    for i, n in enumerate(nodes):
        p = n['parent']
        t = n['t'] if (p >= N or p == i) else tuple(float(v) for v in R_new[p].T @ (P_old[i] - P_old[p]))
        if max(abs(a - b) for a, b in zip(t, n['t'])) > 1e-6 or new_r[i] != n['r']:
            struct.pack_into('>3f', out, n['o'] + 16, *t)
            struct.pack_into('>3f', out, n['o'] + 28, *new_r[i])
    return bytes(out), changed


def fix_pac(src, ref=None, out=None):
    """every attire's JBOYs lined up with the stock rests (ref: a json, or one model pac's rests instead);
    -> {'fixed': bool, 'bones': {bone: (old, new)}}"""
    import ch_tool
    import svrfmt
    import w13_char as W
    stock = reference(ref)
    raw = open(src, 'rb').read()
    _, groups = ch_tool.epk8_read(raw)
    entries, bones, changed = [], {}, False
    for name, blob in groups[0][1]:
        if not name.strip(b'\0'):
            continue
        items = svrfmt.pach_read(ch_tool.unpack(blob))
        new, hit = [], False
        for i, x in items:
            d = ch_tool.unpack(x)
            if d[:4] == b'JBOY':
                nd, ch = fix_jboy(d, stock)
                if nd is not None:
                    x, hit = svrfmt.bpe_encode(nd, compress=True), True
                    for b, o, w in ch:
                        bones.setdefault(b, (tuple(round(v, 2) for v in o), tuple(round(v, 2) for v in w)))
            new.append((i, x))
        if hit:
            changed = True
            blob = svrfmt.pach_write(new)
        entries.append((b'EMD ', name, blob))
    if out and changed:
        W.write_epk8(src, entries, out)
    return {'fixed': changed, 'bones': bones}


def report_line(rep):
    if not rep['fixed']:
        return None
    return ('bone rests put back to 2011\'s (the model\'s own ones distort it in motion): %s' %
            ', '.join('%s %s -> %s' % (b, o, w) for b, (o, w) in sorted(rep['bones'].items())))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--stock':
        print(len(make_stock(sys.argv[2])), 'bones ->', STOCK)
        raise SystemExit(0)
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    r = fix_pac(sys.argv[1], None, sys.argv[2] if len(sys.argv) == 3 else None)
    print(report_line(r) or 'the model already has 2011\'s bone rests')
