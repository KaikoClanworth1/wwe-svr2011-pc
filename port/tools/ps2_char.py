"""PS2 superstar models -> SvR 2011 character pacs (superstar mods), through svr11_build.py.

    py ps2_char.py --list <disc iso or extract folder>
    py ps2_char.py <disc> <id> <out dir> --template <2011 chNNN.pac> [--attire N|all] [--no-gear]

--list prints JSON {game, disc_serial, characters: [{id, name, raw_id, source, attires: [{attire, source,
bones, meshes}]}]}. The convert writes into <out dir>: model.json (svr11_build's neutral model), tex_*.png
(the PS2 part textures), ch.pac and report.json (the builder's report + {game, id, sources: [{attire, model_id,
raw_name, bones_folded, height_m}], scale, height_m, bones_folded}). Exit 0 ok, 2 bad input, 3 failed (the builder's codes).

Games: SmackDown! Here Comes the Pain (HCTP), SmackDown! Shut Your Mouth (SYM: HCTP's formats, ids
character * 16 + n - see class SYM). The PS2 data (HCTP):

- PAC\\CH.PAC / CH2.PAC: DPAC (u32 "DPAC", ..., groups from 0x800, data from 0x4000; group "EMD": u16
  n | 0x8000, u16 start sector, n x {u16 id, u16 size / 0x100}, entries 0x800-aligned). Model id XXYZ (hex
  digits): XX the character, Y the attire, Z the part: 0 the match body, 1 the same body in more detail
  (used here when there is one), 2 gear (glasses, a vest, a cape), 3 multi-part bodies.
- each entry a PAC ("PAC ", u32 n, n x {u16 id, u24 offset, u24 size}): 2 the YOBJ model, 6 / 7 attached
  YOBJ parts (pads, own small skeletons), 8 one RTX3 texture, 9 the texture bank.
- YOBJ (little-endian; pointers are offsets from byte 8): 0x18 mesh count, 0x1C node count, 0x20 texture
  count, 0x24 -> meshes (0x40 each), 0x28 -> nodes (0x50: name[16], t f32x3, 1.0, r f32x3 radians, ?,
  s32 parent), 0x2C -> texture names (16 B), 0x30 -> object name.
  mesh: +00 batch count, +04 submesh count, +08 -> batches, +0C -> submeshes, +18 -> positions block,
  +1C -> weights block.
  batch (0x20): u32 vertex count, u32 influences (1-4), -> positions (f32x4), -> normals (f32x4), s32 x4
  bone numbers (1-based node indices, -1 unused). Vertices run over the batches in order (model space).
  weights: per run of consecutive batches with more than one influence, 16 bytes (12 zero, a VIF UNPACK
  V4_32 tag: bits 16-23 the count) then f32x4 per vertex, weight k for the batch's bone k; one-influence
  batches have none (1.0).
  submesh (0xD0): +0x28 texture slot; +0xC0: u16 1, u16 qwords, u32 packets, -> packet table (16 B: 3, 3,
  u32 vertices, -> data), each packet one triangle strip of {s, t, q, vertex index} {r, g, b, a} f32x4 pairs.
- texture bank (entry 9): u32 n, then n x 0x20 {name[16], "txc", u32 size, u32 offset}; RTX3 textures
  (PSMT8 / PSMT4 with a CT32 CSM1 CLUT, or PSMCT32; alpha 0..0x80).

Units and axes match 2011 (10 cm, -Y up, -Z front, +X left, pelvis origin); bone names are Yuke's, as
2011's. The PS2 bind pose is a T-pose (arms straight out, legs straight down), 2011's an A-pose: each bone
is turned so its segment points as 2011's bone does (hierarchically, the smallest turn), the vertices go
with their weighted bones, and "joints" gives svr11_build the posed proportions. Bones 2011 lacks fold into
their nearest ancestor 2011 has.
"""
import argparse
import json
import math
import os
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import discfs  # noqa: E402

SKELETON = json.load(open(os.path.join(HERE, 'svr11_skeleton.json')))
B11 = {b['name']: b for b in SKELETON['bones']}
SPECULAR = 48   # colour alpha = specular level (2011 skin: about 40-60)

# a bone's segment: bone -> the child it points at (2011 names)
SEGMENT = {'koshi': 'mune', 'mune': 'kubi', 'kubi': 'atama'}
for _s in 'lr':
    SEGMENT.update({'%s_momo' % _s: '%s_sune' % _s, '%s_sune' % _s: '%s_ashi' % _s,
                    '%s_ashi' % _s: '%s_tsumasaki' % _s, '%s_sakotsu' % _s: '%s_ninoude' % _s,
                    '%s_ninoude' % _s: '%s_kote' % _s, '%s_kote' % _s: '%s_te' % _s,
                    '%s_te' % _s: '%s_yubi20' % _s})
    for _f in range(5):
        SEGMENT['%s_yubi%d0' % (_s, _f)] = '%s_yubi%d1' % (_s, _f)
        SEGMENT['%s_yubi%d1' % (_s, _f)] = '%s_yubi%d2' % (_s, _f)


class BadInput(Exception):
    pass


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def cstr(b):
    return b.split(b'\0')[0].decode('latin-1')


# ------------------------------------------------------------------ archives

def dpac(d):
    """HCTP DPAC -> [(group, id, offset, size)] (the "EMD" groups' numbered entries)"""
    if d[:4] != b'DPAC':
        raise BadInput('not a DPAC archive')
    out = []
    off = 0x800
    while off < 0x4000 and d[off:off + 4] != bytes(4):
        tag = d[off:off + 4].decode('latin-1').strip()
        n, start = struct.unpack_from('<HH', d, off + 4)
        if n & 0x8000:
            n &= 0x7FFF
            pos = 0x4000 + start * 0x800
            for i in range(n):
                id_, sz = struct.unpack_from('<HH', d, off + 8 + 4 * i)
                out.append((tag, id_, pos, sz * 0x100))
                pos += (sz * 0x100 + 0x7FF) & ~0x7FF
            off += 8 + 4 * n
        else:   # (named groups: {name4, u16 sector, u16 size})
            cnt = 0
            while cnt < n and d[off + 8 + 8 * cnt:off + 12 + 8 * cnt] != bytes(4):
                cnt += 1
            off += 8 + 8 * cnt
    return out


def pac(d):
    """inner PAC -> {id: bytes}"""
    if d[:4] != b'PAC ':
        return {}
    n = u32(d, 4)
    base = 8 + 8 * n
    out = {}
    for i in range(n):
        e = d[8 + 8 * i:16 + 8 * i]
        o = int.from_bytes(e[2:5], 'little')
        s = int.from_bytes(e[5:8], 'little')
        out[struct.unpack_from('<H', e)[0]] = d[base + o:base + o + s]
    return out


# ------------------------------------------------------------------ YOBJ

def euler(r):
    rx, ry, rz = r
    cx, sx, cy, sy, cz, sz = math.cos(rx), math.sin(rx), math.cos(ry), math.sin(ry), math.cos(rz), math.sin(rz)
    X = np.array([[1, 0, 0], [0, cx, sx], [0, -sx, cx]])
    Y = np.array([[cy, 0, -sy], [0, 1, 0], [sy, 0, cy]])
    Z = np.array([[cz, sz, 0], [-sz, cz, 0], [0, 0, 1]])
    return X @ Y @ Z   # (row vectors: v' = v @ R)


def read_yobj(d):
    """-> dict(name, textures, nodes [{name, parent, world (4x4, row vectors)}], meshes)"""
    if d[:4] != b'YOBJ':
        raise BadInput('not a YOBJ model')
    B = 8
    nmesh, nnode, ntex = u32(d, 0x18), u32(d, 0x1C), u32(d, 0x20)
    out = dict(name=cstr(d[u32(d, 0x30) + B:][:16]),
               textures=[cstr(d[u32(d, 0x2C) + B + 16 * i:][:16]) for i in range(ntex)], nodes=[], meshes=[])
    pn = u32(d, 0x28) + B
    for i in range(nnode):
        e = pn + 0x50 * i
        T = np.eye(4)
        T[:3, :3] = euler(struct.unpack_from('<3f', d, e + 32))
        T[3, :3] = struct.unpack_from('<3f', d, e + 16)
        parent = struct.unpack_from('<i', d, e + 48)[0]
        if 0 <= parent < i:
            T = T @ out['nodes'][parent]['world']
        else:
            parent = -1
        out['nodes'].append(dict(name=cstr(d[e:e + 16]), parent=parent, world=T))
    pm = u32(d, 0x24) + B
    for mi in range(nmesh):
        e = pm + 0x40 * mi
        nb, nsub = u32(d, e), u32(d, e + 4)
        pb, ps = u32(d, e + 8) + B, u32(d, e + 0xC) + B
        pos, nrm, infl = [], [], []
        pw = u32(d, e + 0x1C) + B   # weights block
        left = 0
        for bi in range(nb):
            cnt, ninf, pp, pnn = struct.unpack_from('<4I', d, pb + 0x20 * bi)
            bones = struct.unpack_from('<4i', d, pb + 0x20 * bi + 16)
            if not 1 <= ninf <= 4:
                raise BadInput('mesh %d batch %d: %d influences' % (mi, bi, ninf))
            pos.append(np.frombuffer(d, '<f4', cnt * 4, pp + B).reshape(cnt, 4)[:, :3].astype(np.float64))
            nrm.append(np.frombuffer(d, '<f4', cnt * 4, pnn + B).reshape(cnt, 4)[:, :3].astype(np.float64))
            nodes = [b - 1 for b in bones[:ninf]]
            if ninf == 1:
                infl += [[(nodes[0], 1.0)]] * cnt
                left = 0
                continue
            for _ in range(cnt):
                if left == 0:   # (a new run: its VIF tag)
                    tag = u32(d, pw + 12)
                    if tag >> 24 != 0x6C:
                        raise BadInput('mesh %d batch %d: no weights tag (%08x)' % (mi, bi, tag))
                    left = (tag >> 16) & 0xFF
                    pw += 16
                w = struct.unpack_from('<4f', d, pw)
                pw += 16
                left -= 1
                infl.append([(nodes[k], w[k]) for k in range(ninf) if w[k] > 0])
        mesh = dict(pos=np.concatenate(pos), nrm=np.concatenate(nrm), infl=infl, subs=[])
        if len(infl) != len(mesh['pos']):
            raise BadInput('mesh %d: %d weights for %d vertices' % (mi, len(infl), len(mesh['pos'])))
        for si in range(nsub):
            s = ps + 0xD0 * si
            npk, pt = u32(d, s + 0xC4), u32(d, s + 0xC8) + B
            strips = []
            for k in range(npk):
                _, _, nvk, pdata = struct.unpack_from('<4I', d, pt + 16 * k)
                pdata += B
                strip = []
                for j in range(nvk):
                    s_, t_, q_ = struct.unpack_from('<3f', d, pdata + 32 * j)
                    q_ = q_ or 1.0
                    strip.append((u32(d, pdata + 32 * j + 12), s_ / q_, t_ / q_,
                                  struct.unpack_from('<4f', d, pdata + 32 * j + 16)))
                strips.append(strip)
            mesh['subs'].append(dict(tex=u32(d, s + 0x28), strips=strips))
        out['meshes'].append(mesh)
    return out


def rtx3(t):
    """RTX3 -> RGBA uint8 (h, w, 4), alpha 0..255"""
    if t[:4] != b'RTX3':
        raise BadInput('not an RTX3 texture')
    h = struct.unpack_from('<16I', t)
    psm = ((h[2] | (h[3] << 32)) >> 20) & 0x3F
    w, hh = h[8] & 0xFFFF, h[8] >> 16
    po, co = h[10], h[15]
    if psm == 0x13:
        idx = np.frombuffer(t, np.uint8, w * hh, po)
        pal = np.frombuffer(t, np.uint8, 1024, co).reshape(256, 4).copy()
        i = np.arange(256)
        pal = pal[(i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)]   # (CSM1 swizzle)
    elif psm == 0x14:
        b = np.frombuffer(t, np.uint8, w * hh // 2, po)
        idx = np.empty(w * hh, np.uint8)
        idx[0::2], idx[1::2] = b & 15, b >> 4
        pal = np.frombuffer(t, np.uint8, 64, co).reshape(16, 4).copy()
    elif psm == 0x00:
        img = np.frombuffer(t, np.uint8, w * hh * 4, po).reshape(hh, w, 4).copy()
        img[..., 3] = np.minimum(img[..., 3].astype(int) * 2, 255)
        return img
    else:
        raise BadInput('RTX3 pixel format %#x' % psm)
    pal = pal.astype(np.int32)
    pal[:, 3] = np.minimum(pal[:, 3] * 2, 255)
    return pal[idx].reshape(hh, w, 4).astype(np.uint8)


def texbank(d):
    out = {}
    for i in range(u32(d, 0)):
        e = d[0x10 + 0x20 * i:0x30 + 0x20 * i]
        size, off = struct.unpack_from('<II', e, 20)
        try:
            out[cstr(e[:16]).lower()] = rtx3(d[off:off + size + 0x40])
        except BadInput:
            out[cstr(e[:16]).lower()] = None
    return out


# ------------------------------------------------------------------ the games

class HCTP:
    game = 'WWE SmackDown! Here Comes the Pain'
    pacs = ('PAC/CH.PAC', 'PAC/CH2.PAC')

    def __init__(self, disc):
        if type(self) is HCTP and disc.exists('PAC/DJ.PAC'):   # (Shut Your Mouth: class SYM)
            raise BadInput('not Here Comes the Pain')
        self.disc = disc
        self.entries = {}   # id -> (pac path, offset, size)
        for p in self.pacs:
            if not disc.exists(p):
                continue
            head = disc.read(p, 0, 0x4000)
            for _, id_, off, size in dpac(head):
                self.entries[id_] = (p, off, size)
        if not self.entries:
            raise BadInput('no PAC/CH.PAC on this disc')

    def entry(self, id_):
        p, off, size = self.entries[id_]
        return pac(self.disc.read(p, off, size))

    def characters(self):
        """{character: {attire: model id}} - the detailed body (Z=1) when there is one, else Z=0"""
        chars = {}
        for id_ in sorted(self.entries):
            ch, at, part = id_ >> 8, (id_ >> 4) & 0xF, id_ & 0xF
            if part in (0, 1):
                cur = chars.setdefault(ch, {}).get(at)
                if cur is None or part == 1:
                    chars[ch][at] = id_
        return chars

    def model(self, id_):
        """-> (body YOBJ dict, [part YOBJ dicts], textures {name: RGBA})"""
        e = self.entry(id_)
        if 2 not in e:
            raise BadInput('%04x has no model' % id_)
        body = read_yobj(e[2])
        parts = [read_yobj(e[k]) for k in (6, 7) if k in e and e[k][:4] == b'YOBJ']
        tex = texbank(e[9]) if 9 in e else {}
        return body, parts, tex

    def gear(self, ch, at):
        """an attire's entrance gear model ids (part 2: glasses, a vest, a cape)"""
        id_ = (ch << 8) | (at << 4) | 2
        return [id_] if id_ in self.entries else []


class SYM(HCTP):
    """SmackDown! Shut Your Mouth: HCTP's formats; model ids are character * 16 + n: n 0/1, 2/3, 4/5 an
    attire's detailed (even, 96 nodes) and match (odd, 92) body; 6-9 extra bodies and gear (glasses, a
    vest: a few hundred vertices)."""
    game = 'WWE SmackDown! Shut Your Mouth'
    GEAR_VERTS = 600

    def __init__(self, disc):
        if not (disc.exists('PAC/DJ.PAC') and disc.exists('PAC/FINKEL.PAC')):   # (SYM's own pacs)
            raise BadInput('not Shut Your Mouth')
        super().__init__(disc)

    def shape(self, id_):
        """-> (vertex count, the model's texture names) of an entry's body"""
        e = self.entry(id_)
        if 2 not in e or e[2][:4] != b'YOBJ':
            return 0, frozenset()
        y = read_yobj(e[2])
        return sum(len(m['pos']) for m in y['meshes']), frozenset(t.lower() for t in y['textures'])

    def characters(self):
        chars = {}
        for id_ in sorted(self.entries):
            ch, n = id_ >> 4, id_ & 0xF
            ats = chars.setdefault(ch, {})
            if n <= 5:
                at = n // 2
                if at not in ats or n % 2 == 0:
                    ats[at] = id_
        for id_ in sorted(self.entries):   # (6-9: more bodies - not gear, not another copy of an attire's)
            ch, n = id_ >> 4, id_ & 0xF
            if not 6 <= n <= 9:
                continue
            verts, texs = self.shape(id_)
            if verts < self.GEAR_VERTS:
                continue
            if any(self.shape(o)[1] == texs for o in self.entries if o >> 4 == ch and (o & 0xF) <= 5):
                continue
            ats = chars.setdefault(ch, {})
            ats[max(ats, default=-1) + 1] = id_
        return chars

    def gear(self, ch, at):
        """the character's first small 6-9 model (glasses, a vest) for its first attire (SYM doesn't say
        which attire wears it)"""
        if at != min(self.characters().get(ch, {0: 0})):
            return []
        for id_ in sorted(self.entries):
            if id_ >> 4 == ch and 6 <= (id_ & 0xF) <= 9 and 0 < self.shape(id_)[0] < self.GEAR_VERTS:
                return [id_]
        return []


GAMES = [SYM, HCTP]


def open_game(path):
    disc = discfs.open(path)
    for G in GAMES:
        try:
            return G(disc)
        except BadInput:
            continue
    raise BadInput('no supported PS2 game found in %s' % path)


# ------------------------------------------------------------------ to 2011

def map_names(nodes):
    """PS2 node index -> 2011 bone name (itself, or its nearest ancestor 2011 has)"""
    out, folded = [], []
    for i, nd in enumerate(nodes):
        k = i
        while k >= 0 and nodes[k]['name'] not in B11:
            k = nodes[k]['parent']
        out.append(nodes[k]['name'] if k >= 0 else 'root')
        if nodes[i]['name'] not in B11:
            folded.append('%s->%s' % (nd['name'], out[-1]))
    return out, folded


def align(a, b):
    """the smallest rotation (3x3, column vectors) turning unit a onto unit b"""
    v = np.cross(a, b)
    c = float(np.dot(a, b))
    if c > 1 - 1e-9:
        return np.eye(3)
    if c < -1 + 1e-9:
        p = np.array([1.0, 0, 0]) if abs(a[0]) < 0.9 else np.array([0, 1.0, 0])
        v = np.cross(a, p)
        v /= np.linalg.norm(v)
        return 2 * np.outer(v, v) - np.eye(3)
    V = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + V + V @ V / (1 + c)


def pose(nodes):
    """the PS2 skeleton turned into 2011's A-pose: per node (rotation D, posed position) so that a vertex
    bound to node i goes to p'_i + D_i (v - p_i)"""
    P = [nd['world'][3, :3] for nd in nodes]
    idx = {nd['name']: i for i, nd in enumerate(nodes)}
    D, Q = [], []
    for i, nd in enumerate(nodes):
        p = nd['parent']
        Dp = D[p] if p >= 0 else np.eye(3)
        Q.append(P[i] if p < 0 else Q[p] + Dp @ (P[i] - P[p]))
        R = Dp
        c = SEGMENT.get(nd['name'])
        if c and c in idx and nd['name'] in B11 and c in B11:
            src = Dp @ (P[idx[c]] - P[i])
            dst = np.array(B11[c]['world']) - np.array(B11[nd['name']]['world'])
            if np.linalg.norm(src) > 1e-6 and np.linalg.norm(dst) > 1e-6:
                R = align(src / np.linalg.norm(src), dst / np.linalg.norm(dst)) @ Dp
        D.append(R)
    return D, Q


def part_nodes(part, body):
    """a part's node index -> the body's node index (by name; a bone the body lacks: its nearest ancestor
    the body has)"""
    bidx = {nd['name']: i for i, nd in enumerate(body['nodes'])}
    out = []
    for i, nd in enumerate(part['nodes']):
        k = i
        while k >= 0 and part['nodes'][k]['name'] not in bidx:
            k = part['nodes'][k]['parent']
        out.append(bidx[part['nodes'][k]['name']] if k >= 0 else 0)
    return out


def build_model(body, parts, tex, out_dir, template, tag):
    """-> (model.json dict, info)"""
    names11, folded = map_names(body['nodes'])
    D, Q = pose(body['nodes'])
    P = [nd['world'][3, :3] for nd in body['nodes']]
    # textures used -> png
    pngs = {}

    def png(name):
        k = name.lower()
        if k not in pngs:
            img = tex.get(k)
            if img is None:
                return None
            fn = 'tex_%s_%s.png' % (tag, ''.join(c if c.isalnum() else '_' for c in k))
            # (2011 reads the colour alpha as the specular level, not opacity: PS2 skin is matte - a low
            # level, as stock skin, else the builder's derived level made the skin look like shiny plastic)
            rgba = img.copy()
            rgba[..., 3] = SPECULAR
            Image.fromarray(rgba, 'RGBA').save(os.path.join(out_dir, fn))
            pngs[k] = fn
        return pngs[k]

    meshes = []
    for src, nmap in [(body, None)] + [(p, part_nodes(p, body)) for p in parts]:
        for mi, m in enumerate(src['meshes']):
            # vertices to the A-pose, weights to 2011 names
            V = np.zeros_like(m['pos'])
            N = np.zeros_like(m['nrm'])
            infl = []
            for v, inf in enumerate(m['infl']):
                tot = sum(w for _, w in inf) or 1.0
                ws = {}
                for node, w in inf:
                    n = nmap[node] if nmap is not None else node
                    n = min(max(n, 0), len(body['nodes']) - 1)
                    V[v] += (w / tot) * (Q[n] + D[n] @ (m['pos'][v] - P[n]))
                    N[v] += (w / tot) * (D[n] @ m['nrm'][v])
                    ws[names11[n]] = ws.get(names11[n], 0.0) + w / tot
                infl.append(ws)
            # one output mesh per texture
            for si, sub in enumerate(m['subs']):
                tname = src['textures'][sub['tex']] if sub['tex'] < len(src['textures']) else ''
                key = {}
                pos, nrm, uvs, wts, tris, bones = [], [], [], [], [], []
                bidx = {}
                for strip in sub['strips']:
                    ids = []
                    for vi, s_, t_, _col in strip:
                        k = (vi, round(s_, 5), round(t_, 5))
                        if k not in key:
                            key[k] = len(pos)
                            pos.append([float(x) for x in V[vi]])
                            n = N[vi] / (np.linalg.norm(N[vi]) or 1.0)
                            nrm.append([float(x) for x in n])
                            uvs.append([float(s_), float(t_)])
                            row = []
                            for b, w in infl[vi].items():
                                if b not in bidx:
                                    bidx[b] = len(bones)
                                    bones.append(b)
                                row.append([bidx[b], round(w, 5)])
                            wts.append(row)
                        ids.append(key[k])
                    for j in range(2, len(ids)):
                        a, b, c = (ids[j - 1], ids[j - 2], ids[j]) if j % 2 else (ids[j - 2], ids[j - 1], ids[j])
                        if a != b and b != c and a != c:
                            tris.append([a, b, c])
                if not tris:
                    continue
                kind = 'eye' if 'eye' in tname.lower() else 'skin'
                mesh = dict(name='%s_m%d_%d' % (src['name'] or tag, mi, si), kind=kind, positions=pos, normals=nrm,
                            uvs=uvs, bones=bones, weights=wts, triangles=tris)
                p = png(tname) if tname else None
                if p:
                    mesh['textures'] = {'color': p}
                meshes.append(mesh)
    joints = {}
    for i, nd in enumerate(body['nodes']):
        if nd['name'] in B11 and nd['name'] not in joints:
            joints[nd['name']] = [float(x) for x in Q[i]]
    ys = [p[1] for m in meshes for p in m['positions']]
    info = dict(bones_folded=folded, scale=1.0, height_m=round((max(ys) - min(ys)) / 10.0, 2) if ys else None)
    return dict(meshes=meshes, joints=joints), info


def convert(path, id_, out_dir, template, attire='all', gear=True):
    g = open_game(path)
    chars = g.characters()
    if id_ not in chars:
        raise BadInput('no character %x (have %s)' % (id_, ' '.join('%x' % c for c in sorted(chars))))
    ats = sorted(chars[id_]) if attire == 'all' else [int(attire)]
    os.makedirs(out_dir, exist_ok=True)
    model = dict(version=1, template=os.path.abspath(template), attires=[])
    infos = []
    joints = None
    for at in ats:
        if at not in chars[id_]:
            raise BadInput('character %x has no attire %d' % (id_, at))
        mid = chars[id_][at]
        body, parts, tex = g.model(mid)
        a, info = build_model(body, parts, tex, out_dir, template, '%04x' % mid)
        joints = joints or a['joints']
        entry = dict(name=None, meshes=a['meshes'])
        gids = g.gear(id_, at) if gear else []
        if gids:   # (the attire wearing its gear: the game's entrance model)
            gparts, gtex = [], dict(tex)
            for gid in gids:
                gb, gp, gt = g.model(gid)
                gparts += [gb] + gp
                gtex.update(gt)
            ga, _ = build_model(body, parts + gparts, gtex, out_dir, template, '%04x' % mid)
            entry['entrance_gear'] = dict(meshes=ga['meshes'])
        model['attires'].append(entry)
        infos.append(dict(attire=at, model_id='%04x' % mid, raw_name=body['name'],
                          gear=['%04x' % x for x in gids], **info))
    for a in model['attires']:
        a.pop('name')
    model['joints'] = joints
    mj = os.path.join(out_dir, 'model.json')
    json.dump(model, open(mj, 'w'))
    pac_out = os.path.join(out_dir, 'ch.pac')
    r = subprocess.run([sys.executable, os.path.join(HERE, 'svr11_build.py'), mj, pac_out,
                        os.path.join(out_dir, 'build_report.json')], capture_output=True, text=True)
    try:
        rep = json.loads(r.stdout)
    except ValueError:
        rep = dict(ok=False, error=(r.stderr or r.stdout)[-2000:])
    rep.update(game=g.game, id='%x' % id_, sources=infos, scale=1.0,
               height_m=max((i['height_m'] or 0) for i in infos),
               bones_folded=sorted({f for i in infos for f in i['bones_folded']}))
    json.dump(rep, open(os.path.join(out_dir, 'report.json'), 'w'), indent=1)
    print(json.dumps(rep))
    return r.returncode


def list_disc(path):
    g = open_game(path)
    out = dict(game=g.game, disc_serial=(g.disc.info() or {}).get('serial'), characters=[])
    for ch, ats in sorted(g.characters().items()):
        c = dict(id='%x' % ch, attires=[])
        for at, mid in sorted(ats.items()):
            try:
                body, parts, _ = g.model(mid)
            except BadInput as e:
                c['attires'].append(dict(attire=at, raw_id='%04x' % mid, error=str(e)))
                continue
            p = g.entries[mid][0]
            c.setdefault('name', body['name'])
            c['attires'].append(dict(attire=at, raw_id='%04x' % mid, source='%s/EMD/%04x' % (p, mid),
                                     name=body['name'], bones=len(body['nodes']),
                                     meshes=sum(len(m['subs']) for m in body['meshes']), parts=len(parts)))
        out['characters'].append(c)
    print(json.dumps(out, indent=1))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--list', metavar='DISC')
    ap.add_argument('disc', nargs='?')
    ap.add_argument('id', nargs='?')
    ap.add_argument('out', nargs='?')
    ap.add_argument('--template')
    ap.add_argument('--attire', default='all')
    ap.add_argument('--no-gear', action='store_true', help='leave the entrance gear out')
    a = ap.parse_args(argv)
    try:
        if a.list:
            list_disc(a.list)
            return 0
        if not (a.disc and a.id and a.out and a.template):
            ap.error('give <disc> <id> <out dir> --template <2011 ch.pac>, or --list <disc>')
        return convert(a.disc, int(a.id, 16), a.out, a.template, a.attire, not a.no_gear)
    except BadInput as e:
        print(json.dumps(dict(ok=False, error=str(e))))
        return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
