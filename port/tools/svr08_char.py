"""SvR 2008 character model -> a ch.pac SvR2011 can load (superstar mods).

    python svr08_char.py <2008 chNN.pac> <out ch.pac> --template <2011 ch104.pac> [--portrait <256 DDS>] [--costume 0x20,...]

2008 models are EPAC/PACH/JBOY like 2011's, on the same skeleton in the same
units (2011's node names plus collision/helper bones k*). Differences: 2008
meshes use yBumpMap / yReflect / yDefault with older vertex formats, one
texture set (child 0xa: <id>color, <id>normal, <id>ao) and costume pieces as
extra JBOYs (0x20.., textures in 0x27). So: the 2011 template's node table,
palettes remapped by name (helpers to their nearest 2011 ancestor), the
template's parameter blocks (yBumpMapSS skin, yReflect eyes, hair, eyelash),
textures re-sorted into 2011's sets (0xa normal / noise / Mk, 0xc color),
Mk from the 2008 AO (bruise mask R = 0, AO in G / B), costume pieces merged
into the body, and the template's 0x28/0x32/0x50/0x64, cube map and blood
overlays.
"""
import argparse, io, math, os, struct, sys
import numpy as np
from PIL import Image

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import ch_tool, svrfmt, jboy  # noqa: E402

B = 8


def unp(b):
    return svrfmt.bpe_decode(b) if b[:4] == b'BPE ' else b


def u32(d, o): return struct.unpack_from('>I', d, o)[0]
def cstr(b): return b.split(b'\0')[0].decode('latin-1')


def read08(d):
    """JBOY (mesh 0xB4) -> dict, true node layout (r +32, parent +48)"""
    nmesh, mptr, nnode, ntex, nptr, tptr, optr = struct.unpack_from('>7I', d, 0x18)
    m = {'name': cstr(d[B + optr:B + optr + 16]),
         'textures': [cstr(d[B + tptr + 16 * i:B + tptr + 16 * i + 16]) for i in range(ntex)], 'nodes': [], 'meshes': []}
    for i in range(nnode):
        o = B + nptr + 80 * i
        m['nodes'].append({'name': cstr(d[o:o + 16]), 'parent': struct.unpack_from('>i', d, o + 48)[0]})
    for i in range(nmesh):
        o = B + mptr + 0xB4 * i
        vc, nstrip, npal = struct.unpack_from('>3I', d, o)
        s = {'palette': list(struct.unpack_from('>20i', d, o + 0x0C))[:npal]}
        nw = max(1, u32(d, o + 0x5C))
        vblk, wptr, uvptr = struct.unpack_from('>3I', d, o + 0x68)
        s['shader'] = cstr(d[o + 0x78:o + 0x88])
        npar, parptr, iptr = struct.unpack_from('>3I', d, o + 0x90)
        vdat = B + u32(d, B + vblk)
        s['verts'] = [struct.unpack_from('>6fI', d, vdat + 28 * k) for k in range(vc)]
        s['nw'] = nw
        s['wraw'] = d[B + wptr:B + wptr + 8 * vc * nw]
        s['uvs'] = [struct.unpack_from('>2f', d, B + uvptr + 8 * k) for k in range(vc)]
        s['params'] = []
        for k in range(npar):
            po = B + u32(d, B + parptr + 4 * k)
            typ, size = struct.unpack_from('>HH', d, po + 16)
            s['params'].append((cstr(d[po:po + 16]), typ, d[po + 20:po + size]))
        s['strips'] = []
        for k in range(nstrip):
            prim, icount, idat = struct.unpack_from('>3I', d, B + iptr + 12 * k)
            s['strips'].append(list(struct.unpack_from('>%dH' % icount, d, B + idat)))
        m['meshes'].append(s)
    return m


def tex_read(b):
    n = struct.unpack_from('<I', b, 0)[0]
    out = []
    for i in range(n):
        r = b[16 + 32 * i:48 + 32 * i]
        sz, off = struct.unpack_from('<II', r, 20)
        out.append((r[:16].split(b'\0')[0].decode('latin-1'), b[off:off + sz]))
    return out


def tex_write(texs, head):
    n = len(texs)
    off = (16 + 32 * n + 15) & ~15
    recs, blobs = b'', b''
    for name, dd in texs:
        recs += name.encode('latin-1')[:16].ljust(16, b'\0') + b'dds\0' + struct.pack('<II', len(dd), off + len(blobs)) + b'\0' * 4
        blobs += dd + b'\0' * (-len(dd) % 16)
    return (struct.pack('<I', n) + head[4:16] + recs).ljust(off, b'\0') + blobs


def dds_encode(img, fourcc):
    w, h = img.size
    mips = int(math.log2(max(w, h))) + 1
    out = None
    for m in range(mips):
        im = img if m == 0 else img.resize((max(1, w >> m), max(1, h >> m)), Image.BOX)
        b = io.BytesIO()
        im.save(b, 'DDS', pixel_format=fourcc)
        v = b.getvalue()
        out = bytearray(v) if out is None else out + v[128:]
    struct.pack_into('<I', out, 8, struct.unpack_from('<I', out, 8)[0] | 0x20000)
    struct.pack_into('<I', out, 28, mips)
    struct.pack_into('<I', out, 108, struct.unpack_from('<I', out, 108)[0] | 0x400008)
    return bytes(out)


def mk_from_ao(ao_dds):
    im = Image.open(io.BytesIO(ao_dds)).convert('RGB')
    g = im.convert('L')
    return dds_encode(Image.merge('RGBA', (Image.new('L', im.size, 0), g, g, Image.new('L', im.size, 255))), 'DXT5')


class Template:
    def __init__(self, path):
        _, groups = ch_tool.epk8_read(open(path, 'rb').read())
        self.t = {i: ch_tool.unpack(x) for i, x in svrfmt.pach_read(ch_tool.unpack(groups[0][1][0][1]))}
        self.body = jboy.read(self.t[0])
        self.hair = jboy.read(self.t[0x2710])
        self.lash = jboy.read(self.t[0x4e20])
        d = self.t[0]
        nnode, nptr = u32(d, 0x20), u32(d, 0x28)
        self.parent = [struct.unpack_from('>i', d, B + nptr + 80 * i + 48)[0] for i in range(nnode)]
        self.index = {n['name']: i for i, n in enumerate(self.body.nodes)}
        ms = self.body.meshes
        self.p_skin = next(s for s in ms if s.shader == 'yBumpMapSS' and s.vfmt == 0)
        self.p_eye = next(s for s in ms if s.shader == 'yReflect')
        self.p_hair = self.hair.meshes[-1]
        self.p_lash = self.lash.meshes[0]
        self.ttex = dict(tex_read(self.t[0xa]))


# 2008 sway chains with no 2011 bone, pinned to a steadier one than their parent: the turban's
# cloth tails (7004 kami_a06 back, a07 / a08 rear right / left - both parented to r_sakotsu in 2008)
PIN = {'kami_a06': 'mune', 'kami_a07': 'mune', 'kami_a08': 'mune'}


def node_map(m8, tp):
    out = {}
    for i, nd in enumerate(m8['nodes']):
        pin = next((v for p, v in PIN.items() if nd['name'].startswith(p)), None)
        if pin in tp.index:
            out[i] = tp.index[pin]
            continue
        k = i
        while k >= 0 and m8['nodes'][k]['name'] not in tp.index:
            k = m8['nodes'][k]['parent']
        out[i] = tp.index[m8['nodes'][k]['name']] if k >= 0 else 0
    return out


def mesh(s8, tmpl, nmap, textures, slots):
    s = jboy.Mesh()
    s.raw = bytes(tmpl.raw)
    s.u04 = 1
    s.u74 = 0
    s.shader = tmpl.shader
    s.vfmt = tmpl.vfmt
    s.palette = [nmap[p - 1] + 1 if p > 0 else p for p in s8['palette']]
    s.verts = [(v[0], v[1], v[2], v[3], v[4], v[5], 0xFFFFFFFF) for v in s8['verts']]
    vc, nw, w = len(s.verts), s8['nw'], s8['wraw']
    s.weights = [[(tuple(w[8 * vc * j + 8 * k:8 * vc * j + 8 * k + 4]), struct.unpack_from('>f', w, 8 * vc * j + 8 * k + 4)[0])
                  for k in range(vc)] for j in range(nw)]
    s.uvs = list(s8['uvs'])
    s.strips = [(6, idx) for idx in s8['strips']]
    s.params = []
    for name, typ, val in tmpl.params:
        if typ == 0x0f:
            want = slots.get(name)
            k = textures.index(want) if want in textures else -1
            val = struct.pack('>i', k) + val[4:]
        s.params.append((name, typ, val))
    P = np.array([v[:3] for v in s.verts])
    c = (P.min(0) + P.max(0)) / 2
    r = float(np.sqrt(((P - c) ** 2).sum(1)).max())
    md = bytearray(s.raw)
    struct.pack_into('>I', md, 0x8C, tmpl.vfmt)
    struct.pack_into('>4f', md, 0xA4, float(c[0]), float(c[1]), float(c[2]), r)
    s.raw = bytes(md)
    return s


def model(tp, tmodel, parts, textures):
    """parts: [(m8, kind(s8) -> (template mesh, slots))]"""
    m = jboy.Model()
    m.header, m.name, m.group = tmodel.header, tmodel.name, tmodel.group
    m.name_tail = getattr(tmodel, 'name_tail', b'')
    m.textures = textures
    m.nodes = tp.body.nodes
    m.meshes = []
    for m8, kind in parts:
        nmap = node_map(m8, tp)
        for s8 in m8['meshes']:
            tmpl, slots = kind(s8)
            ms = mesh(s8, tmpl, nmap, textures, slots)
            ms.u74 = len(m.meshes)
            m.meshes.append(ms)
    return m


def dds_img(d):
    return Image.open(io.BytesIO(d)).convert('RGBA')


def remap_uvs(m8, su, sv, ou, ov):
    for s8 in m8['meshes']:
        s8['uvs'] = [(ou + u * su, ov + v * sv) for u, v in s8['uvs']]
    return m8


def attire(kids, tp, portrait, costume_ids, first_kids):
    """One 2011 attire. Costume pieces (2008 children 0x20.., own textures in 0x27) go in the
    body model on the body's own textures, as 2011 models are made: the costume's colour,
    normal and AO are painted into the body sheets' free box (the hair strip, u 0.8125-1,
    v 0-0.625: an attire with costume pieces has no hair model) and its UVs mapped there.
    Every texture keeps the stock size and full mips. Tested in game (2026-10-08): extra
    texture names (cos_c...), a 2048 sheet and a mip-less sheet all rendered the attire
    black and red; the body mesh with attire 1's sheets rendered fine."""
    base = dict(tex_read(kids[0xa]))
    base0 = dict(tex_read(first_kids[0xa]))   # (attire 2 shares attire 1's color / normal)
    pick = lambda suffix: next((d for n, d in list(base.items()) + list(base0.items()) if n.lower().endswith(suffix)), None)
    color, normal = pick('color'), pick('normal')
    ao = next((d for n, d in base.items() if n.lower().endswith('ao')), None) or pick('ao')
    cos = dict(tex_read(kids[0x27])) if 0x27 in kids else {}
    cpick = lambda suffix: next((d for n, d in cos.items() if n.lower().endswith(suffix)), None)
    pieces = [cid for cid in costume_ids if cid in kids and kids[cid][:4] == b'JBOY'] if cos else []
    textures = ['color', 'normal', 'mk', 'noise', 'cubemap']
    SKIN = {'texDiffuse': 'color', 'texNormal': 'normal', 'texOcclusion': 'mk', 'texTilingMap': 'noise'}
    EYE = {'texDiffuse': 'color', 'texCubeRefction': 'cubemap', 'texRefctionReg': 'color', 'texOcclusion': 'mk'}
    atlas = bool(pieces)
    BOX = (0.8175, 0.0, 0.995, 0.62)   # (u0, v0, u1, v1) inside the free strip, small margins
    parts = [(read08(kids[0]), lambda s: (tp.p_eye, EYE) if 'Reflect' in s['shader'] else (tp.p_skin, SKIN))]
    for cid in pieces:
        parts.append((remap_uvs(read08(kids[cid]), BOX[2] - BOX[0], BOX[3] - BOX[1], BOX[0], BOX[1]), lambda s: (tp.p_skin, SKIN)))
    fit = lambda m8: m8
    out = {0: jboy.write(model(tp, tp.body, parts, textures))}
    if 0x2710 in kids:
        out[0x2710] = jboy.write(model(tp, tp.hair, [(fit(read08(kids[0x2710])), lambda s: (tp.p_hair, SKIN))], textures[:5]))
    if 0x4e20 in kids:
        out[0x4e20] = jboy.write(model(tp, tp.lash, [(fit(read08(kids[0x4e20])), lambda s: (tp.p_lash, {'texDiffuse': 'color'}))], ['color']))
    if atlas:
        def paint(body, piece):
            w, h = body.size
            x0, y0, x1, y1 = int(BOX[0] * w), int(BOX[1] * h), int(round(BOX[2] * w)), int(round(BOX[3] * h))
            im = body.copy()
            im.paste(piece.resize((x1 - x0, y1 - y0), Image.LANCZOS), (x0, y0))
            return im
        color_d = dds_encode(paint(dds_img(color), dds_img(cpick('_c'))), 'DXT5')
        normal_d = dds_encode(paint(dds_img(normal), dds_img(cpick('_n'))), 'DXT5')
        ao_img = paint(dds_img(ao).convert('L'), dds_img(cpick('ao')).convert('L'))
        mk_d = dds_encode(Image.merge('RGBA', (Image.new('L', ao_img.size, 0), ao_img, ao_img, Image.new('L', ao_img.size, 255))), 'DXT5')
    else:
        color_d, normal_d, mk_d = color, normal, mk_from_ao(ao)
    ta = [('normal', normal_d), ('noise', tp.ttex['noise']), ('Mk', mk_d)]
    ta += [(n, d) for n, d in tp.ttex.items() if n.lower().startswith('bloodhair')]
    out[0xa] = tex_write(ta, tp.t[0xa][:16])
    out[0xb] = tp.t[0xb]
    out[0xc] = tex_write([('color', color_d)], tp.t[0xc][:16])
    for k in (0x28, 0x32, 0x50, 0x64, 0x7530, 0x7531, 0x7532):
        if k in tp.t:
            out[k] = tp.t[k]
    for k in (0xc8, 0xc9):
        out[k] = portrait or tp.t[k]
    return out


def write_epk8(tmpl, entries, out):
    t = open(tmpl, 'rb').read()
    header = bytearray(t[:0x800])
    index = bytearray(0x3800)
    struct.pack_into('<4sHH', index, 0, b'EMD ', 4 * len(entries), 0)
    index[6:12] = t[0x806:0x80C]
    body = bytearray()
    p = 12
    for name, blob in entries:
        while len(body) % 0x800:
            body.append(0)
        index[p:p + 8] = name.ljust(8, b'\0')[:8]
        struct.pack_into('<II', index, p + 8, len(body) // 0x800, (len(blob) + 0xFF) // 0x100)
        body += blob
        p += 16
    while len(body) % 0x800:
        body.append(0)
    struct.pack_into('<I', header, 4, 12 + 16 * len(entries))
    struct.pack_into('<I', header, 8, len(body))
    open(out, 'wb').write(bytes(header) + bytes(index) + bytes(body) + bytes(0x800))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('--template', required=True)
    ap.add_argument('--portrait')
    ap.add_argument('--costume', default='0x20')
    ap.add_argument('--id', type=int, default=70)
    a = ap.parse_args()
    tp = Template(a.template)
    portrait = open(a.portrait, 'rb').read() if a.portrait else None
    cos = [int(x, 0) for x in a.costume.split(',') if x]
    h, g, t = svrfmt.epac_read(open(a.src, 'rb').read())
    ents = [(n, {i: unp(x) for i, x in svrfmt.pach_read(unp(b))}) for n, b in g[0][1]]
    first = ents[0][1]
    entries = []
    for k, (n, kids) in enumerate(ents):
        out = attire(kids, tp, portrait, cos, first)
        name = b'%06d%02d' % (a.id, k * 10 + 2)
        entries.append((name, svrfmt.pach_write([(i, svrfmt.bpe_encode(out[i], compress=True)) for i in sorted(out)])))
        print('attire', n.decode(), '->', name.decode(), sorted(hex(i) for i in out), flush=True)
    write_epk8(a.template, entries, a.out)
    print('wrote', a.out, os.path.getsize(a.out))


if __name__ == '__main__':
    main()
