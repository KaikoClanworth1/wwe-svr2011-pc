"""PS2 superstar models -> SvR 2011 character pacs (superstar mods), through svr11_build.py.

    py ps2_char.py --list <disc iso or extract folder>
    py ps2_char.py <disc> <id> <out dir> --template <2011 chNNN.pac> [--attire N[,N..]|all] [--no-gear] [--models ID,ID..]

--list prints JSON {game, disc_serial, characters: [{id, name, raw_id, source, attires: [{attire, source,
bones, meshes}]}]}. The convert writes into <out dir>: model.json (svr11_build's neutral model), tex_*.png
(the PS2 part textures), render.dds / render_small.dds (the PS2 select pictures, when the
game has them; left out otherwise), ch.pac and report.json (the builder's report + {game, id, sources: [{attire, model_id,
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
import re
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
CUTOUT_TRIS = 0.15   # an opaque-drawn mesh with more triangle middles on see-through texels is a cut-out too

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
        else:   # (named groups: {name4, u16 sector, u16 size}; SvR 2009 names its models "6601")
            cnt = 0
            while cnt < n and off + 16 + 8 * cnt <= len(d) and d[off + 8 + 8 * cnt:off + 12 + 8 * cnt] != bytes(4):
                p = off + 8 + 8 * cnt
                name = d[p:p + 4].decode('latin-1')
                if tag.startswith('EMD') and all(c in '0123456789abcdefABCDEF' for c in name):
                    sec, sz = struct.unpack_from('<HH', d, p + 4)
                    out.append((tag, int(name, 16), 0x4000 + sec * 0x800, sz * 0x100))
                cnt += 1
            off += 8 + 8 * cnt
    return out


def dpk8(d):
    """DPK8 (the PS2 SvR 2010 / 2011) -> [(group, id, offset, size)]: groups {tag4, u32 n (0x4000: 12-byte
    entries)}, entries {name8, u16 sector, u16 size / 0x100}; the EMD groups name their models in 8 hex
    digits: "000", the 3-digit superstar id, the attire, the part ("00010201": superstar 102, attire 0, part 1)"""
    if d[:4] != b'DPK8':
        raise BadInput('not a DPK8 archive')
    out = []
    off = 0x800
    while off < 0x4000 and d[off:off + 4] != bytes(4):
        tag = d[off:off + 4].decode('latin-1').strip()
        n = u32(d, off + 4)
        size = 12 if n & 0x4000 else 8
        n &= 0x3FFF
        for i in range(n):
            p = off + 8 + size * i
            name = d[p:p + size - 4].decode('latin-1')
            sec, sz = struct.unpack_from('<HH', d, p + size - 4)
            if tag.startswith('EMD') and name.strip() and all(c in '0123456789abcdefABCDEF' for c in name):
                out.append((tag, int(name, 16), 0x4000 + sec * 0x800, sz * 0x100))
        off += 8 + size * n
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
            mesh['subs'].append(dict(tex=u32(d, s + 0x28), strips=strips, mat=d[s:s + 0xC0]))
        out['meshes'].append(mesh)
    return out


def rtx3(t, size=None):
    """RTX3 -> RGBA uint8 (h, w, 4), alpha 0..255. `size`: the texture's block size (the bank entry's): the block is
    0x40 header, pixels, palette last - the header's offsets (+0x28 pixels, +0x3C palette) are 8 bytes short in some
    games' banks (SvR 2006 / 2007: pixels at 0x40 but +0x28 = 0x38), so they're taken from the block's end when it
    says so. (Read as they were, the first two colours of every palette were the bytes before it and every texel
    moved by 16 / 8 places: green and yellow patches on Kane's tights and shins, a strip across the top of a head.)"""
    if t[:4] != b'RTX3':
        raise BadInput('not an RTX3 texture')
    h = struct.unpack_from('<16I', t)
    psm = ((h[2] | (h[3] << 32)) >> 20) & 0x3F
    w, hh = h[8] & 0xFFFF, h[8] >> 16
    po, co = h[10], h[15]
    if size and psm in (0x13, 0x14):
        pal_bytes = 1024 if psm == 0x13 else 64
        pix_bytes = w * hh if psm == 0x13 else w * hh // 2
        delta = (size - pal_bytes) - co
        if delta in (4, 8, 16) and po + delta + pix_bytes == co + delta:
            po, co = po + delta, co + delta
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
            out[cstr(e[:16]).lower()] = rtx3(d[off:off + size + 0x40], size)
        except BadInput:
            out[cstr(e[:16]).lower()] = None
    return out


def tim2(b):
    """TIM2 (its first picture) -> RGBA uint8 (h, w, 4): 32 / 24-bit, or 8 / 4-bit with a CLUT (CT32,
    CT24 or CT16; CSM1 swizzled unless the CLUT type says CSM2); alpha 0..0x80 -> 0..255"""
    if b[:4] != b'TIM2':
        raise BadInput('not a TIM2 picture')
    if b[5] == 1:   # (format 1: the picture header 128-byte aligned - SYM's MNSP 0006 / 0014 / 0042 ...)
        b = b[:16] + b[0x80:]
    _, _, img_size, hdr_size, ncol, _, _, clut_type, img_type, w, h = struct.unpack_from('<IIIHHBBBBHH', b, 16)
    data = 16 + hdr_size

    def c32(a):
        a = a.reshape(-1, 4).astype(np.int32)
        a[:, 3] = np.minimum(a[:, 3] * 2, 255)
        return a
    if img_type == 3:
        return c32(np.frombuffer(b, np.uint8, w * h * 4, data)).reshape(h, w, 4).astype(np.uint8)
    if img_type == 2:
        rgb = np.frombuffer(b, np.uint8, w * h * 3, data).reshape(h, w, 3)
        return np.concatenate([rgb, np.full((h, w, 1), 255, np.uint8)], 2)
    if img_type not in (4, 5):
        raise BadInput('TIM2 picture type %d' % img_type)
    if img_type == 5:
        idx = np.frombuffer(b, np.uint8, w * h, data)
    else:
        p4 = np.frombuffer(b, np.uint8, w * h // 2, data)
        idx = np.empty(w * h, np.uint8)
        idx[0::2], idx[1::2] = p4 & 15, p4 >> 4
    cp = data + img_size
    ct = clut_type & 0x1F
    if ct == 3:
        pal = c32(np.frombuffer(b, np.uint8, ncol * 4, cp))
    elif ct == 2:
        rgb = np.frombuffer(b, np.uint8, ncol * 3, cp).reshape(-1, 3).astype(np.int32)
        pal = np.concatenate([rgb, np.full((ncol, 1), 255, np.int32)], 1)
    else:
        v = np.frombuffer(b, '<u2', ncol, cp).astype(np.int32)
        pal = np.stack([(v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3, np.where(v & 0x8000, 255, 0)], 1)
    if img_type == 5 and not clut_type & 0x80:
        i = np.arange(len(pal))
        pal = pal[(i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)]
    return pal[idx].reshape(h, w, 4).astype(np.uint8)


def named_groups(head):
    """DPAC named groups -> {group: {entry name: (offset, size)}} ({name4, u16 sector, u16 size / 0x100})"""
    out = {}
    off = 0x800
    while off < 0x4000 and head[off:off + 4] != bytes(4):
        tag = head[off:off + 4].decode('latin-1')
        n = u32(head, off + 4)
        if n & 0x8000:
            off += 8 + 4 * (n & 0x7FFF)
            continue
        ents = {}
        for i in range(n):
            p = off + 8 + 8 * i
            if p + 8 > len(head) or head[p:p + 4] == bytes(4):
                break
            sec, sz = struct.unpack_from('<HH', head, p + 4)
            ents[head[p:p + 4].decode('latin-1')] = (0x4000 + sec * 0x800, sz * 0x100)
        out[tag] = ents
        off += 8 + 8 * len(ents)
    return out


# ------------------------------------------------------------------ the games

SERIALS = {   # PS2 disc serial -> game
    'SLES_504.77': 'WWF SmackDown! Just Bring It', 'SLUS_202.69': 'WWF SmackDown! Just Bring It',
    'SLES_512.83': 'WWE SmackDown! Shut Your Mouth', 'SLUS_205.12': 'WWE SmackDown! Shut Your Mouth',
    'SLES_520.36': 'WWE SmackDown! Here Comes the Pain', 'SLUS_207.87': 'WWE SmackDown! Here Comes the Pain',
    'SLES_527.81': 'WWE SmackDown! vs. Raw (2004)', 'SLUS_209.36': 'WWE SmackDown! vs. Raw (2004)',
    'SLES_536.76': 'WWE SmackDown! vs. Raw 2006', 'SLUS_211.34': 'WWE SmackDown! vs. Raw 2006',
    'SLES_544.89': 'WWE SmackDown vs. Raw 2007 (PS2)', 'SLUS_214.27': 'WWE SmackDown vs. Raw 2007 (PS2)',
    'SLES_548.79': 'WWE SmackDown vs. Raw 2008 (PS2)', 'SLES_552.51': 'WWE SmackDown vs. Raw 2009 (PS2)',
    'SLES_555.45': 'WWE SmackDown vs. Raw 2010 (PS2)', 'SLES_556.35': 'WWE SmackDown vs. Raw 2011 (PS2)',
}


class Yukes:
    """Yuke's PS2 SmackDowns with DPAC character archives (HCTP, SvR 2004, 2006, the PS2 2007 - 2009):
    model ids XXYZ - character, attire, part. What a part number means differs by game (HCTP 0 / 1 the
    match / detailed body and 2 gear, SvR 2004 1 the body and 2 gear, SvR 2006 1 / 2 two bodies and 4
    gear), so: per character and attire the biggest model is the body, models under GEAR_VERTS vertices
    are gear."""
    PACS = ('PAC/CH.PAC', 'PAC/CH2.PAC', 'PAC/CH3.PAC', 'PAC/CH/CH.PAC', 'PAC/CH/CH2.PAC', 'PAC/CH/CH3.PAC',
            'CH.PAC', 'CH2.PAC')
    PARTS = (6, 7, 26, 27)   # (attached parts: pads)
    GEAR_VERTS = 600

    def __init__(self, disc):
        self.disc = disc
        serial = (disc.info() or {}).get('serial')
        self.game = SERIALS.get(serial, 'PS2 SmackDown (%s)' % serial)
        # (material flags before SvR 2004: see build_model's cutout)
        self.old_materials = any(k in self.game for k in ('Just Bring It', 'Shut Your Mouth', 'Here Comes the Pain'))
        self.entries = {}   # id -> (pac path, offset, size)
        self._shape = {}
        self.dpk8 = False   # (PS2 SvR 2010 / 2011: 8-digit names "000" + 3-digit superstar id + attire + part)
        for p in self.PACS:
            if not disc.exists(p):
                continue
            head = disc.read(p, 0, 0x4000)
            if head[:4] not in (b'DPAC', b'DPK8'):
                continue
            for _, id_, off, size in (dpac(head) if head[:4] == b'DPAC' else dpk8(head)):
                self.entries[id_] = (p, off, size)
            if head[:4] == b'DPK8':
                self.dpk8 = True
        if not self.entries:
            raise BadInput('no DPAC character archive (CH.PAC) on this disc')

    def entry(self, id_):
        """an entry's PAC -> {id: bytes} (BPE'd parts, as SvR 2006 / 2007 store them, decoded)"""
        p, off, size = self.entries[id_]
        out = pac(self.disc.read(p, off, size))
        for k, v in out.items():
            if v[:4] == b'BPE ':
                import svrfmt
                out[k] = svrfmt.bpe_decode(v)
        return out

    def shape(self, id_):
        """-> (vertex count, the model's texture names) of an entry's model (cached)"""
        if id_ not in self._shape:
            e = self.entry(id_)
            if 2 not in e or e[2][:4] != b'YOBJ':
                self._shape[id_] = (0, frozenset())
            else:
                try:
                    y = read_yobj(e[2])
                    self._shape[id_] = (sum(len(m['pos']) for m in y['meshes']),
                                        frozenset(t.lower() for t in y['textures']))
                except BadInput:
                    self._shape[id_] = (0, frozenset())
        return self._shape[id_]

    def groups(self):
        """{(character, attire): [model ids]}"""
        out = {}
        for id_ in sorted(self.entries):
            if self.dpk8:   # ("00020101": superstar 201 (key 0x201: its decimal digits as hex), attire 0, part 1)
                raw = '%08x' % id_
                key = (int(raw[3:6], 16), int(raw[6], 16))
            else:
                key = (id_ >> 8, (id_ >> 4) & 0xF)
            out.setdefault(key, []).append(id_)
        return out

    def characters(self):
        """{character: {attire: model id}} - each attire's biggest model"""
        if not hasattr(self, '_chars'):
            chars = {}
            for (ch, at), ids in self.groups().items():
                best = max(ids, key=lambda i: self.shape(i)[0])
                if self.shape(best)[0] >= self.GEAR_VERTS:
                    chars.setdefault(ch, {})[at] = best
            self._chars = chars
        return self._chars

    def model(self, id_):
        """-> (body YOBJ dict, [part YOBJ dicts], textures {name: RGBA})"""
        e = self.entry(id_)
        if 2 not in e:
            raise BadInput('%04x has no model' % id_)
        body = read_yobj(e[2])
        parts = [read_yobj(e[k]) for k in self.PARTS if k in e and e[k][:4] == b'YOBJ']
        tex = texbank(e[9]) if 9 in e else {}
        return body, parts, tex

    def portraits(self, ch):
        """{'bust': 128 x 128, 'wide': 256 x 128} RGBA - the select pictures (MENU.PAC "MENU" SPFA / SPFB:
        BPE'd PACs of 90 TIM2s; picture n = the character id's hex digits read as decimal: 0x30 -> 30)"""
        if not hasattr(self, '_spf'):
            self._spf = {}
            if self.disc.exists('PAC/MENU.PAC'):
                import svrfmt
                ents = named_groups(self.disc.read('PAC/MENU.PAC', 0, 0x4000)).get('MENU', {})
                for key, nm in (('bust', 'SPFA'), ('wide', 'SPFB')):
                    if nm in ents:
                        b = pac(self.disc.read('PAC/MENU.PAC', *ents[nm])).get(0, b'')
                        if b[:4] == b'BPE ':
                            b = svrfmt.bpe_decode(b)
                        self._spf[key] = pac(b)
        try:
            n = int('%x' % ch)
        except ValueError:   # (PS2 2009 ids aren't decimal digits)
            return {}
        out = {}
        for key, pics in self._spf.items():
            if n in pics:
                try:
                    out[key] = tim2(pics[n])
                except BadInput:
                    pass
        return out

    def gear(self, ch, at):
        """an attire's entrance gear: its small models (glasses, a vest, a cape)"""
        return [i for i in self.groups().get((ch, at), []) if 0 < self.shape(i)[0] < self.GEAR_VERTS]


class SYM(Yukes):
    """SmackDown! Shut Your Mouth: HCTP's formats; model ids are character * 16 + n: n 0/1, 2/3, 4/5 an
    attire's detailed (even, 96 nodes) and match (odd, 92) body; 6-9 extra bodies and gear (glasses, a
    vest: a few hundred vertices)."""
    def __init__(self, disc):
        if not (disc.exists('PAC/DJ.PAC') and disc.exists('PAC/FINKEL.PAC')):   # (SYM's own pacs)
            raise BadInput('not Shut Your Mouth')
        super().__init__(disc)
        self.game = SERIALS['SLES_512.83']

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

    pictures = None   # ({character: picture number}: sym_pictures, read on first use)

    def portraits(self, ch):
        """{'wide': 256 x 128} - MENU.PAC "MNSP" per-character TIM2s ("0000".."0062", 256 x 128), picture n
        = record n of the select roster (sym_pictures)"""
        if self.pictures is None:
            self.pictures = sym_pictures(self.disc, self.characters())
        if ch not in self.pictures:
            return {}
        ents = sym_groups(self.disc.read('PAC/MENU.PAC', 0, 0x4000)).get('MNSP', {})
        name = '%04d' % self.pictures[ch]
        if name not in ents:
            return {}
        try:
            return {'wide': tim2(self.disc.read('PAC/MENU.PAC', *ents[name]))}
        except BadInput:
            return {}

    def gear(self, ch, at):
        """the character's first small 6-9 model (glasses, a vest) for its first attire (SYM doesn't say
        which attire wears it)"""
        if at != min(self.characters().get(ch, {0: 0})):
            return []
        for id_ in sorted(self.entries):
            if id_ >> 4 == ch and 6 <= (id_ & 0xF) <= 9 and 0 < self.shape(id_)[0] < self.GEAR_VERTS:
                return [id_]
        return []


def sym_groups(head):
    """SYM DPAC named groups -> {group: {entry name: (offset, size)}}: {tag[4], u16 2 x count, u16 sector}
    then the rows {name[4], u16 sector, u16 size / 0x100} (named_groups reads HCTP's single count and
    runs into the next group); numbered groups (bit 15) skipped"""
    out = {}
    off = 0x800
    while off + 8 <= 0x4000 and head[off:off + 4] != bytes(4):
        tag = head[off:off + 4].decode('latin-1').strip()
        n = struct.unpack_from('<H', head, off + 4)[0]
        if n & 0x8000:
            off += 8 + 4 * (n & 0x7FFF)
            continue
        ents = out.setdefault(tag, {})
        for i in range(n // 2):
            p = off + 8 + 8 * i
            sec, sz = struct.unpack_from('<HH', head, p + 4)
            ents[head[p:p + 4].decode('latin-1').strip()] = (0x4000 + sec * 0x800, sz * 0x100)
        off += 8 + 8 * (n // 2)
    return out


def sym_pictures(disc, characters):
    """SYM: {character: MNSP picture number}.  The select pictures follow the select roster, MENU.PAC "MNCM"
    "CHAR" item 0: 116-byte records {+2 u16 CHAR record number, +4 name}, alphabetical (Al Snow, Albert,
    Big Show, The Big Valbowski ... X-Pac: 60); picture n = record n (checked by eye, all 60).  The
    character's record number = its id's hex digits read as decimal (0x48 -> X-Pac's record 48)."""
    if not disc.exists('PAC/MENU.PAC'):
        return {}
    ents = sym_groups(disc.read('PAC/MENU.PAC', 0, 0x4000)).get('MNCM', {})
    if 'CHAR' not in ents:
        return {}
    tab = pac(disc.read('PAC/MENU.PAC', *ents['CHAR'])).get(0, b'')
    by_record = {}
    for n in range(len(tab) // 116):
        r = tab[116 * n:116 * n + 116]
        if not r[4:5].isalpha():
            break
        by_record.setdefault(struct.unpack_from('<H', r, 2)[0], n)
    out = {}
    for ch in characters:
        try:
            rec = int('%x' % ch)
        except ValueError:
            continue
        if rec in by_record:
            out[ch] = by_record[rec]
    return out


class JBI(Yukes):
    """WWF SmackDown! Just Bring It: CH.PAC at the disc root, one model per id (its own character: names come
    from the game's records), each with ONE texture - a 1024 x 128 TIM2 strip (entry 1) its UVs map straight
    into; the model's texture names are labels only. Parts 6 / 7 (pads)."""

    def __init__(self, disc):
        if not disc.exists('CH.PAC') or disc.exists('PAC/CH.PAC'):
            raise BadInput('not Just Bring It')
        super().__init__(disc)

    def characters(self):
        if not hasattr(self, '_chars'):
            self._chars = {i: {0: i} for i in sorted(self.entries) if self.shape(i)[0] >= self.GEAR_VERTS}
        return self._chars

    def groups(self):
        return {(i, 0): [i] for i in self.entries}

    def model(self, id_):
        e = self.entry(id_)
        if 2 not in e or e[2][:4] != b'YOBJ':
            raise BadInput('%04x has no model' % id_)
        body = read_yobj(e[2])
        parts = [read_yobj(e[k]) for k in (6, 7) if k in e and e[k][:4] == b'YOBJ']
        strip = tim2(e[1]) if 1 in e and e[1][:4] == b'TIM2' else None
        names = set(body['textures']) | {t for p in parts for t in p['textures']}
        return body, parts, ({n.lower(): strip for n in names} if strip is not None else {})

    def gear(self, ch, at):
        return []

    def portraits(self, ch):
        return {}


GAMES = [JBI, SYM, Yukes]


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


def build_model(body, parts, tex, out_dir, template, tag, old_materials=False):
    """-> (model.json dict, info)"""
    names11, folded = map_names(body['nodes'])
    D, Q = pose(body['nodes'])
    P = [nd['world'][3, :3] for nd in body['nodes']]
    # textures used -> png
    def see_through(name, uvs, tris):
        """an opaque-drawn mesh that is a cut-out all the same (SvR 2004 on alpha-test every mesh: the 2007 Rock's
        head cap showed white; not JBI / SYM / HCTP, whose alpha means other things): its texture's alpha is see-through-or-solid (not SYM's gloss masks) and many of its
        triangles' middles fall on see-through texels (not corners: island edges touch a sheet's unused parts)"""
        img = tex.get(name.lower())
        if img is None or not tris:
            return False
        al = img[..., 3]
        if float(((al < 16) | (al > 240)).mean()) < 0.9:
            return False
        h, w = al.shape
        mids = [((uvs[i][0] + uvs[j][0] + uvs[k][0]) / 3, (uvs[i][1] + uvs[j][1] + uvs[k][1]) / 3) for i, j, k in tris]
        return sum(1 for u, v in mids if al[int(v * h) % h, int(u * w) % w] < 128) / len(mids) > CUTOUT_TRIS

    def cutout(sub):
        """whether a PS2 submesh is drawn blended (hair cards, hair lines, laces): its material's flags (u32 at
        +0x24) - JBI / SYM / HCTP: 0x07 opaque, 0x05 blended (bit 0x02 clear) in a model that has 0x07; SvR 2004 on: 0x05 opaque,
        0x115 / 0x117 blended (bit 0x10: hair, strings, the blood overlay)"""
        mat = sub.get('mat') or b''
        if len(mat) < 0x28:
            return False
        f = u32(mat, 0x24)
        return (not f & 0x02 and mixed) if old_materials else bool(f & 0x10)

    def spec_of(sub):
        """the PS2 material's specular level (float +0x0C: 0.2 skin, 0.15 boots, 0.1 trunks, 0.05 laces) as 2011's
        colour alpha; HCTP / SYM materials hold 1.0 there (not a level): the default"""
        mat = sub.get('mat') or b''
        if old_materials or len(mat) < 0x10:   # (JBI / SYM / HCTP: not a level - HCTP's 0.3-0.4 looked like plastic)
            return SPECULAR
        f = struct.unpack_from('<f', mat, 0x0C)[0]
        return int(round(f * 255)) if 0.0 < f < 0.5 else SPECULAR

    def png(name, keep_alpha, spec=SPECULAR):
        """-> a PS2 texture's png. 2011's skin shader reads the colour alpha as the specular level, its hair
        shader as opacity: a texture hair meshes use keeps its alpha; the others get a low specular level (PS2
        skin is matte; the builder's derived level looked like shiny plastic)."""
        img = tex.get(name.lower())
        if img is None:
            return None
        fn = 'tex_%s_%s%s.png' % (tag, ''.join(c if c.isalnum() else '_' for c in name.lower()), '_cut' if keep_alpha else '')
        rgba = img.copy()
        if not keep_alpha:
            rgba = bleed(rgba)   # (see-through texels a solid mesh still covers: their neighbours' colour, not white)
            rgba[..., 3] = spec
        Image.fromarray(rgba, 'RGBA').save(os.path.join(out_dir, fn))
        return fn

    meshes = []
    # (JBI / SYM / HCTP: 0x05 means blended only in a model that also has 0x07 - the Rock's and Austin's
    # models are all 0x05, all solid)
    mixed = any(u32(sub['mat'], 0x24) & 0x02 for m in body['meshes'] for sub in m['subs'] if len(sub.get('mat') or b'') >= 0x28)
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
                if tname.lower().startswith('blood'):
                    continue   # (the PS2's blood overlay: shown only while bleeding - 2011 has its own)
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
                kind = 'eye' if 'eye' in tname.lower() else 'hair' if cutout(sub) or (not old_materials and see_through(tname, uvs, tris)) else 'skin'
                mesh = dict(name='%s_m%d_%d' % (src['name'] or tag, mi, si), kind=kind, positions=pos, normals=nrm,
                            uvs=uvs, bones=bones, weights=wts, triangles=tris, _tex=tname, _spec=spec_of(sub))
                meshes.append(mesh)
    # (a texture both hair and solid meshes use: a see-through copy for the hair, a specular one for the rest)
    files = {}
    for m in meshes:
        t = m.pop('_tex')
        sp = m.pop('_spec')
        if not t:
            continue
        key = (t.lower(), m['kind'] == 'hair')
        if key not in files:
            files[key] = png(t, key[1], sp)
        if files[key]:
            m['textures'] = {'color': files[key]}
            if not key[1]:
                # (the png's alpha is the PS2 specular: svr11_build reads a skin-shader mesh's shine from the
                # normal alpha unless told the colour alpha is it - then it moves it there, colour alpha = sweat)
                m['material'] = {'specular_alpha': 'texture'}
    wrap_textures(meshes, out_dir)
    joints = {}
    for i, nd in enumerate(body['nodes']):
        if nd['name'] in B11 and nd['name'] not in joints:
            joints[nd['name']] = [float(x) for x in Q[i]]
    ys = [p[1] for m in meshes for p in m['positions']]
    info = dict(bones_folded=folded, scale=1.0, height_m=round((max(ys) - min(ys)) / 10.0, 2) if ys else None)
    return dict(meshes=meshes, joints=joints), info


def bust_render(game, ch, size=256):
    """a picture of a character from its own model when the game has none (SvR 2006 on: no select pictures found):
    the body and its parts in their bind pose, drawn from the front with their textures (nearest texel, a z-buffer,
    a little light from the normals), cropped to head and shoulders -> RGBA uint8 (size, size, 4) or None"""
    ats = game.characters().get(ch)
    if not ats:
        return None
    body, parts, tex = game.model(ats[min(ats)])
    tris = []   # (3 positions, 3 normals, 3 uvs, texture)
    for src in [body] + parts:
        for m in src['meshes']:
            for sub in m['subs']:
                tname = src['textures'][sub['tex']].lower() if sub['tex'] < len(src['textures']) else ''
                if tname.startswith(('blood', 'bandage')):   # (injury overlays: shown only when hurt)
                    continue
                img = tex.get(tname)
                for strip in sub['strips']:
                    for k in range(len(strip) - 2):
                        a, b, c = strip[k], strip[k + 1], strip[k + 2]
                        idx = [a[0], b[0], c[0]]
                        if len(set(idx)) < 3 or max(idx) >= len(m['pos']):
                            continue
                        flip = np.array([1.0, -1.0, 1.0])   # (PS2 model space: up is -y)
                        tris.append((m['pos'][idx] * flip, m['nrm'][idx] * flip,
                                     np.array([[a[1], a[2]], [b[1], b[2]], [c[1], c[2]]]), img))
    if not tris:
        return None
    P = np.concatenate([t[0] for t in tris])
    top, bottom = P[:, 1].max(), P[:, 1].min()
    span = (top - bottom) * 0.30   # (head and shoulders)
    # facing: away from the side the head's (flipped) normals lean to - the back of the head and neck holds more
    # surface than the face (SvR 2006 / 2007: they look down -z)
    head = np.concatenate([t[1] for t in tris if t[0][:, 1].min() > top - span * 0.5])
    face_z = -1.0 if head[:, 2].mean() >= 0 else 1.0
    cx = P[P[:, 1] > top - span, 0].mean()
    scale = size / span
    color = np.zeros((size, size, 3), np.float32)
    alpha = np.zeros((size, size), np.uint8)
    zbuf = np.full((size, size), -np.inf)
    light = np.array([0.3, 0.4, face_z * 0.86])
    for pos, nrm, uv, img in tris:
        if pos[:, 1].max() < top - span * 1.05:
            continue
        x = (pos[:, 0] - cx) * scale * (-face_z) + size / 2   # (seen from the front: its left on our right)
        y = (top - pos[:, 1]) * scale + size * 0.04
        z = pos[:, 2] * face_z
        x0, x1 = max(int(x.min()), 0), min(int(x.max()) + 1, size)
        y0, y1 = max(int(y.min()), 0), min(int(y.max()) + 1, size)
        if x0 >= x1 or y0 >= y1:
            continue
        den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
        if abs(den) < 1e-9:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
        l0 = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / den
        l1 = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / den
        l2 = 1 - l0 - l1
        inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
        if not inside.any():
            continue
        zz = l0 * z[0] + l1 * z[1] + l2 * z[2]
        sub = zbuf[y0:y1, x0:x1]
        draw = inside & (zz > sub)
        if img is not None:
            h, w = img.shape[:2]
            u = l0 * uv[0, 0] + l1 * uv[1, 0] + l2 * uv[2, 0]
            v = l0 * uv[0, 1] + l1 * uv[1, 1] + l2 * uv[2, 1]
            texel = img[(v * h).astype(int) % h, (u * w).astype(int) % w]
            draw &= texel[..., 3] >= 64   # (see-through texels: hair cards' gaps)
            rgb = texel[..., :3].astype(np.float32)
        else:
            rgb = np.full(gx.shape + (3,), 160.0, np.float32)
        n = nrm.mean(axis=0)
        shade = 0.55 + 0.45 * max(0.0, float(np.dot(n / (np.linalg.norm(n) or 1), light)))
        sub[draw] = zz[draw]
        color[y0:y1, x0:x1][draw] = rgb[draw] * shade
        alpha[y0:y1, x0:x1][draw] = 255
    out = np.zeros((size, size, 4), np.uint8)
    out[..., :3] = np.clip(color, 0, 255).astype(np.uint8)
    out[..., 3] = alpha
    return out if alpha.any() else None


def write_renders(pics, out_dir):
    """the PS2 select pictures -> the mod's renders (DXT5 with mips, as superstar_port writes them): the
    128 x 128 bust -> render_small.dds (256 x 256), the 256 x 128 wide bust -> render.dds (doubled, at the
    bottom of a transparent 512 x 512). PS2 has no full-body picture: no render_full.dds. -> {file: source}"""
    import svr08_char
    out = {}
    if 'bust' in pics:
        im = Image.fromarray(pics['bust'], 'RGBA').resize((256, 256), Image.LANCZOS)
        open(os.path.join(out_dir, 'render_small.dds'), 'wb').write(svr08_char.dds_encode(im, 'DXT5'))
        out['render_small.dds'] = 'bust'
    if 'bust' not in pics and 'wide' in pics:   # (SYM: the wide picture's middle as the bust)
        wide = Image.fromarray(pics['wide'], 'RGBA')
        x = (wide.width - wide.height) // 2
        im = wide.crop((x, 0, x + wide.height, wide.height)).resize((256, 256), Image.LANCZOS)
        open(os.path.join(out_dir, 'render_small.dds'), 'wb').write(svr08_char.dds_encode(im, 'DXT5'))
        out['render_small.dds'] = 'wide (middle)'
    if 'wide' in pics:
        im = Image.fromarray(pics['wide'], 'RGBA')
        im = im.resize((im.width * 2, im.height * 2), Image.LANCZOS)
        canvas = Image.new('RGBA', (512, 512), (0, 0, 0, 0))
        canvas.paste(im, ((512 - im.width) // 2, 512 - im.height))
        open(os.path.join(out_dir, 'render.dds'), 'wb').write(svr08_char.dds_encode(canvas, 'DXT5'))
        out['render.dds'] = 'wide'
    return out


def bleed(img, passes=48):
    """RGBA with its see-through texels (alpha < 128) filled from the nearest solid ones (repeated 4-neighbour
    averages): the PS2 alpha-tests them away, 2011's skin shader draws them - a head cap's white corner"""
    rgb = img[..., :3].astype(np.float32)
    solid = img[..., 3] >= 128
    if solid.all() or not solid.any():
        return img
    for _ in range(passes):
        if solid.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros(solid.shape, np.float32)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            s_ = np.roll(solid, (dy, dx), (0, 1))
            acc += np.roll(rgb, (dy, dx), (0, 1)) * s_[..., None]
            cnt += s_
        grow = ~solid & (cnt > 0)
        rgb[grow] = acc[grow] / cnt[grow][:, None]
        solid = solid | grow
    out = img.copy()
    out[..., :3] = np.clip(rgb, 0, 255).astype(np.uint8)
    return out


def wrap_textures(meshes, out_dir, most=4.0):
    """PS2 UVs wrap (a seam at 1.03, a strap tiled twice); the builder packs every texture into one sheet,
    where they would clamp: a texture used past 0..1 gets a copy covering the range it is used over
    (pixels wrapped) and its meshes' UVs are mapped into it."""
    by = {}
    for m in meshes:
        c = (m.get('textures') or {}).get('color')
        if c:
            by.setdefault(c, []).append(m)
    for c, ms in by.items():
        uv = np.array([q for m in ms for q in m['uvs']])
        lo = np.minimum(np.floor(uv.min(0) * 64) / 64, 0.0)
        hi = np.maximum(np.ceil(uv.max(0) * 64) / 64, 1.0)
        hi = np.minimum(hi, lo + most)
        if (lo >= 0).all() and (hi <= 1).all():
            continue
        img = np.array(Image.open(os.path.join(out_dir, c)))
        h, w = img.shape[:2]
        x0, x1 = int(round(lo[0] * w)), int(round(hi[0] * w))
        y0, y1 = int(round(lo[1] * h)), int(round(hi[1] * h))
        wrapped = img.take(np.arange(y0, y1) % h, 0).take(np.arange(x0, x1) % w, 1)
        fn = c[:-4] + '_w.png'
        Image.fromarray(wrapped).save(os.path.join(out_dir, fn))
        span = hi - lo
        for m in ms:
            m['uvs'] = [[float((u - lo[0]) / span[0]), float((v - lo[1]) / span[1])] for u, v in m['uvs']]
            m['textures']['color'] = fn


def convert(path, id_, out_dir, template, attire='all', gear=True, models=None):
    """models: model ids to build as attires 1, 2, .. in that order instead of the character's own (JBI keeps
    one model per id: a superstar's other models are other ids)"""
    g = open_game(path)
    chars = g.characters()
    if models:
        for mid in models:
            if mid not in g.entries:
                raise BadInput('no model %x' % mid)
        ats = list(range(len(models)))
        body_of = dict(zip(ats, models))
    else:
        if id_ not in chars:
            raise BadInput('no character %x (have %s)' % (id_, ' '.join('%x' % c for c in sorted(chars))))
        ats = sorted(chars[id_]) if str(attire) == 'all' else [int(x) for x in str(attire).split(',') if x.strip()]
        for at in ats:
            if at not in chars[id_]:
                raise BadInput('character %x has no attire %d' % (id_, at))
        body_of = {at: chars[id_][at] for at in ats}
    os.makedirs(out_dir, exist_ok=True)
    # an attire over the game's vertex page budget: its gear version is left out, else its next lighter body
    # (2006 and later keep a match body and a lighter one), until the builder takes it
    no_gear = set() if gear and not models else set(ats)
    fitted = []
    for _ in range(12):
        rep, rc = build_pac(g, id_, ats, body_of, no_gear, out_dir, template)
        m = re.search(r'(\d{8}): (\d+) vertex pages', rep.get('error') or '') if not rep.get('ok') else None
        if not m:
            break
        k, kind = int(m.group(1)[6]), int(m.group(1)[7])   # (entry names: 6-digit id, attire index, kind)
        if not 0 <= k < len(ats):
            break
        at = ats[k]
        if kind == 4 and at not in no_gear:
            no_gear.add(at)
            fitted.append('attire %d: %s pages with its gear - the gear version left out' % (at, m.group(2)))
            continue
        lighter = [i for i in sorted(g.groups().get(group_key(g, body_of[at]), []), key=lambda i: -g.shape(i)[0])
                   if g.shape(i)[0] < g.shape(body_of[at])[0] and g.shape(i)[0] >= g.GEAR_VERTS]
        if not lighter:
            break
        fitted.append('attire %d: %s pages - the lighter body %0*x instead of %0*x' % (
            at, m.group(2), 8 if lighter[0] > 0xFFFF else 4, lighter[0], 8 if body_of[at] > 0xFFFF else 4, body_of[at]))
        body_of[at] = lighter[0]
    rep['fitted'] = fitted
    json.dump(rep, open(os.path.join(out_dir, 'report.json'), 'w'), indent=1)
    print(json.dumps(rep))
    return rc


def group_key(g, id_):
    """the (character, attire) a model id belongs to, as g.groups() keys them"""
    for k, ids in g.groups().items():
        if id_ in ids:
            return k
    return None


def build_pac(g, id_, ats, body_of, no_gear, out_dir, template):
    """model.json for these attires (body_of: attire -> model id; no_gear: attires without the gear version)
    and svr11_build -> (report, exit code)"""
    model = dict(version=1, template=os.path.abspath(template), attires=[])
    infos = []
    joints = None
    for at in ats:
        mid = body_of[at]
        tag = '%0*x' % (8 if mid > 0xFFFF else 4, mid)
        body, parts, tex = g.model(mid)
        a, info = build_model(body, parts, tex, out_dir, template, tag, g.old_materials)
        joints = joints or a['joints']
        entry = dict(meshes=a['meshes'])
        gids = g.gear(id_, at) if at not in no_gear else []
        if gids:   # (the attire wearing its gear: the game's entrance model)
            gparts, gtex = [], dict(tex)
            for gid in gids:
                gb, gp, gt = g.model(gid)
                gparts += [gb] + gp
                gtex.update(gt)
            ga, _ = build_model(body, parts + gparts, gtex, out_dir, template, tag, g.old_materials)
            entry['entrance_gear'] = dict(meshes=ga['meshes'])
        model['attires'].append(entry)
        infos.append(dict(attire=at, model_id=tag, raw_name=body['name'], gear=['%04x' % x for x in gids], **info))
    model['joints'] = joints
    renders = write_renders(g.portraits(id_), out_dir)
    mj = os.path.join(out_dir, 'model.json')
    json.dump(model, open(mj, 'w'))
    r = subprocess.run([sys.executable, os.path.join(HERE, 'svr11_build.py'), mj, os.path.join(out_dir, 'ch.pac'),
                        os.path.join(out_dir, 'build_report.json')], capture_output=True, text=True)
    try:
        rep = json.loads(r.stdout)
    except ValueError:
        rep = dict(ok=False, error=(r.stderr or r.stdout)[-2000:])
    rep.update(game=g.game, id='%x' % id_, sources=infos, scale=1.0, renders=renders,
               height_m=max((i['height_m'] or 0) for i in infos),
               bones_folded=sorted({f for i in infos for f in i['bones_folded']}))
    return rep, r.returncode


def list_disc(path):
    g = open_game(path)
    out = dict(game=g.game, disc_serial=(g.disc.info() or {}).get('serial'), characters=[])
    for ch, ats in sorted(g.characters().items()):
        c = dict(id='%x' % ch, attires=[])
        for at, mid in sorted(ats.items()):
            try:
                body, parts, _ = g.model(mid)
            except BadInput as e:
                c['attires'].append(dict(attire=at, raw_id='%0*x' % (8 if mid > 0xFFFF else 4, mid), error=str(e)))
                continue
            p = g.entries[mid][0]
            c.setdefault('name', body['name'])
            c['attires'].append(dict(attire=at, raw_id='%0*x' % (8 if mid > 0xFFFF else 4, mid), source='%s/EMD/%0*x' % (p, 8 if mid > 0xFFFF else 4, mid),
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
    ap.add_argument('--attire', default='all', help='all, one attire or a list: 0,2,3 (in this order)')
    ap.add_argument('--no-gear', action='store_true', help='leave the entrance gear out')
    ap.add_argument('--models', help='model ids (hex) to build as attires 1, 2, .. in that order: 00,40')
    a = ap.parse_args(argv)
    try:
        if a.list:
            list_disc(a.list)
            return 0
        if not (a.disc and a.id and a.out and a.template):
            ap.error('give <disc> <id> <out dir> --template <2011 ch.pac>, or --list <disc>')
        models = [int(x, 16) for x in a.models.split(',') if x.strip()] if a.models else None
        return convert(a.disc, int(a.id, 16), a.out, a.template, a.attire, not a.no_gear, models)
    except BadInput as e:
        print(json.dumps(dict(ok=False, error=str(e))))
        return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
