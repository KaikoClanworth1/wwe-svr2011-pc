"""HCTP backstage room -> map viewer (glb + self-contained HTML).

    python room_view.py <stage dir, e.g. ...\_unpacked\bg\stg_0030> <out base>

Shows: the room models (world space), props placed by 50100/10000 (type < 200 ->
model 1200+type), gimmick records (types >= 200) and 50000 weapon spots as
labelled boxes, collision 00999 (red), prop collision at their spots (orange).
"""
import base64
import math
import os
import re
import struct
import sys

import numpy as np

import to_glb
import yobj

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import svr08_stage as s8  # noqa: E402


def box(cx, cy, cz, hx, hy, hz):
    c = np.array([[x, y, z] for x in (-hx, hx) for y in (-hy, hy) for z in (-hz, hz)]) + (cx, cy, cz)
    q = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    out = []
    for a, b, cc, d in q:
        out += [c[[a, b, cc]], c[[a, cc, d]]]
    return out


def rot_y(P, ry):
    c, s = math.cos(ry), math.sin(ry)
    R = np.array([[c, 0, -s], [0, 1, 0], [s, 0, c]])
    return P @ R


def placements(stage):
    p = os.path.join(stage, '50100', '10000.bin')
    if not os.path.exists(p):
        return []
    d = open(p, 'rb').read()
    n = struct.unpack_from('<I', d, 0)[0]
    out = []
    for i in range(n):
        r = d[0x10 + 0x20 * i:0x30 + 0x20 * i]
        x, y, z = struct.unpack_from('<3f', r, 0)
        t = struct.unpack_from('<I', r, 12)[0]
        rx, ry, rz = struct.unpack_from('<3f', r, 16)
        out.append((i, t, (x, y, z), (rx, ry, rz)))
    return out


def weapons(stage):
    p = os.path.join(stage, '50000.txt')
    if not os.path.exists(p):
        return []
    out = []
    for m in re.finditer(r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*\(([^)]*)\)\s*,\s*\(([^)]*)\)\s*,\s*(\d+)\s*,\s*([@\w]+)\s*\}',
                         open(p, 'rb').read().decode('cp932', 'replace')):
        out.append((int(m.group(1)), tuple(float(v) for v in m.group(3).split(',')),
                    tuple(float(v) for v in m.group(4).split(',')), int(m.group(5)), m.group(6)))
    return out


def main(stage, outbase):
    tb = yobj.texbank(open(os.path.join(stage, '00200.bin'), 'rb').read())
    models = []
    for f in sorted(os.listdir(stage)):
        if f.endswith('.yobj') and int(f[:5]) < 1000:
            M = yobj.read(open(os.path.join(stage, f), 'rb').read())
            models.append((f[:5], M, None))
    props = {}
    pdir = os.path.join(stage, '50101')
    if os.path.isdir(pdir):
        for f in os.listdir(pdir):
            if f.endswith('.yobj'):
                props[int(f[:5])] = f
    debug = []
    import hctp_props
    for pr in hctp_props.props(stage):
        R = hctp_props.rot_y(pr['ry'])
        for mid in pr['models']:
            M = yobj.read(open(os.path.join(pdir, f'{mid:05d}.yobj'), 'rb').read())
            for mesh in M.meshes:
                mesh['pos'] = mesh['pos'] @ R + pr['pos']
                mesh['nrm'] = mesh['nrm'] @ R
            models.append((f"g{pr['id']}_{mid}", M, None))
    for i, t, pos, rot in placements(stage):
        if t < 100:
            debug.append((f'particle {i} type {t} @({pos[0]:.0f},{pos[1]:.0f},{pos[2]:.0f})',
                          box(pos[0], pos[1] - 3, pos[2], 2.5, 3, 2.5), (0.1, 0.4, 1.0, 0.85)))
    for wid, pos, rot, extra, fl in weapons(stage):
        debug.append((f'50000 id {wid} {fl} @({pos[0]:.0f},{pos[1]:.0f},{pos[2]:.0f})',
                      box(pos[0], pos[1] - 2, pos[2], 2, 2, 2), (1.0, 0.85, 0.0, 0.9)))
    hp = os.path.join(stage, '00999.hmd')
    if os.path.exists(hp):
        R = s8.hmd_read(open(hp, 'rb').read())
        debug.append(('collision 00999', [np.array(s8.tri_corners(r)) for r in R], (1.0, 0.1, 0.1, 0.35)))
    glb = to_glb.build(models, tb, outbase + '.glb', debug)
    html = to_glb.VIEWER.replace('%GLB%', base64.b64encode(glb).decode()).replace('%TITLE%', os.path.basename(stage))
    open(outbase + '.html', 'w').write(html)
    print(outbase, len(glb), 'bytes', len(models), 'models', len(debug), 'markers')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
